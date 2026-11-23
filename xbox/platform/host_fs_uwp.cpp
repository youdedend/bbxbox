// SPDX-License-Identifier: GPL-3.0-or-later
// host_fs.h inside the AppContainer.
//
// Every entry point here is the `*FromApp` variant of the call the CRT would
// have made. That is the whole point of the file: `_open`, `_stat64`,
// `FindFirstFileA` and `MoveFileExW` reach `CreateFileW` and friends, which an
// AppContainer is allowed to import and then refused at run time on a path it
// reached through a `FutureAccessList` token - the dump, on a USB drive. The
// `*FromApp` calls take the same arguments and are the ones the token covers.
//
// The handle numbers the HLE sees are ours: a small table maps them to the
// `HANDLE`s `CreateFile2FromAppW` returned, so nothing above this file ever
// sees a 64-bit handle in an `int`, and closing a stale number is a `false`
// rather than a close of whatever reused the value.
#include "xbox/platform/host_fs.h"

#include "xbox/platform/log.h"
#include "xbox/platform/uwp_handle_access.h"

#include <windows.h>

// A granted folder - the dump on a USB drive - is reachable through WinRT's
// StorageFile/StorageFolder and *not* through the FromApp calls: those are
// refused (ERROR_ACCESS_DENIED) on a path the app only holds a
// `FutureAccessList` token for. That is what `sce_sys\param.sfo could not be
// read` was. Everything below keeps the FromApp path as the fast path - it
// works for LocalState, TemporaryFolder, RoamingState and the package - and
// falls back to WinRT with its own handle table when it is refused.
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>

// The FromApp surface: its own header, and declared only when NTDDI_VERSION is
// at least NTDDI_WIN10_RS3 (xbox/CMakeLists.txt sets it).
#include <fileapifromapp.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bbx::hostfs {
namespace {

thread_local int t_error = 0;

// The guest's error codes are `errno` values (the HLE turns them into Orbis
// codes), so a Win32 error is translated here rather than passed through:
// ERROR_FILE_NOT_FOUND is not ENOENT and would leave the game with a code it
// has no branch for.
int ErrnoFrom(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
            return ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_WRITE_PROTECT:
            return EACCES;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            return EEXIST;
        case ERROR_DIR_NOT_EMPTY:
            return ENOTEMPTY;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return EBUSY;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ENOSPC;
        case ERROR_INVALID_HANDLE:
            return EBADF;
        case ERROR_INVALID_PARAMETER:
            return EINVAL;
        case ERROR_DIRECTORY:
            return ENOTDIR;
        case ERROR_NOACCESS:  // ReadFile into a write-protected page
            return EFAULT;
        case ERROR_FILENAME_EXCED_RANGE:
            return ENAMETOOLONG;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return ENOMEM;
        default:
            return EIO;
    }
}

int ErrnoFromHresult(HRESULT hr) {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        return ErrnoFrom(static_cast<DWORD>(HRESULT_CODE(hr)));
    }
    switch (hr) {
        case E_ACCESSDENIED:
            return EACCES;
        case E_INVALIDARG:
            return EINVAL;
        case E_OUTOFMEMORY:
            return ENOMEM;
        default:
            return EIO;
    }
}

int Fail(int code) {
    t_error = code;
    return -1;
}

int FailWin32() { return Fail(ErrnoFrom(::GetLastError())); }

// WinRT StorageFile/StorageFolder APIs reject forward slashes with E_INVALIDARG
// (0x80070057) and dislike trailing slashes on non-root paths. Convert any
// incoming HLE/Slashify path into canonical Win32 backslash form before calling
// GetFileFromPathAsync / GetFolderFromPathAsync.
std::string WinPath(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        const char ch = path[i] == '/' ? '\\' : path[i];
        // Collapse duplicate separators (preserve leading \\ for UNC paths).
        if (ch == '\\' && !out.empty() && out.back() == '\\' && out.size() > 1) continue;
        out.push_back(ch);
    }
    while (out.size() > 1 && out.back() == '\\') {
        if (out.size() == 3 && out[1] == ':') break;  // keep drive root "E:\"
        out.pop_back();
    }
    return out;
}

std::wstring Widen(const char* path) {
    if (path == nullptr || path[0] == '\0') return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (need <= 1) return {};
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, path, -1, out.data(), need);
    out.resize(static_cast<std::size_t>(need - 1));  // drop the terminator
    return out;
}

std::string Narrow(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(need - 1));
    return out;
}

// --- handles ---------------------------------------------------------------
struct WinFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool append = false;
    std::mutex mu;
    ~WinFile() {
        if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
    }
};

std::mutex g_handle_mu;
std::unordered_map<int, std::shared_ptr<WinFile>> g_handles;
std::atomic<int> g_next_handle{1};  // 0 and negatives are "no handle"

int Adopt(HANDLE file, bool append = false) {
    auto entry = std::make_shared<WinFile>();
    entry->handle = file;
    entry->append = append;
    const int number = g_next_handle.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_handle_mu);
    g_handles.emplace(number, std::move(entry));
    return number;
}

std::shared_ptr<WinFile> Lookup(int number, bool take) {
    std::lock_guard<std::mutex> lock(g_handle_mu);
    auto it = g_handles.find(number);
    if (it == g_handles.end()) return nullptr;
    std::shared_ptr<WinFile> file = it->second;
    if (take) g_handles.erase(it);
    return file;
}

// --- the WinRT path --------------------------------------------------------
//
// Blocking on a WinRT operation is only safe off the thread that pumps the
// CoreWindow dispatcher: `ui-main` opens dump files while it scans for the
// layout, and the guest issues thousands of reads during boot. Run them on a
// persistent MTA worker instead of spawning and joining a thread per call.
class StorageWorker {
public:
    StorageWorker() : thread_([this]() { Run(); }) { thread_.detach(); }

    int RunSync(const std::function<void()>& body) {
        Job job;
        job.body = &body;
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.push_back(&job);
        }
        cv_.notify_one();
        std::unique_lock<std::mutex> done_lock(job.mu);
        job.cv.wait(done_lock, [&job]() { return job.done; });
        return job.err;
    }

private:
    struct Job {
        const std::function<void()>* body = nullptr;
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        int err = 0;
    };

    void Run() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (...) {
        }
        for (;;) {
            Job* job = nullptr;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this]() { return !queue_.empty(); });
                job = queue_.front();
                queue_.pop_front();
            }
            int err = 0;
            try {
                (*job->body)();
            } catch (const winrt::hresult_error& error) {
                err = ErrnoFromHresult(static_cast<HRESULT>(error.code().value));
            } catch (...) {
                err = EIO;
            }
            {
                std::lock_guard<std::mutex> done_lock(job->mu);
                job->err = err;
                job->done = true;
            }
            job->cv.notify_one();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job*> queue_;
    std::thread thread_;
};

StorageWorker& Worker() {
    // Intentionally leaked so UWP process exit never stalls joining a worker
    // while COM/WinRT is tearing down.
    static auto* worker = new StorageWorker();
    return *worker;
}

template <typename Body>
int OnStorageThread(Body&& body) {
    const int err = Worker().RunSync(std::function<void()>(std::forward<Body>(body)));
    if (err != 0) t_error = err;
    return err;
}

struct RtFile {
    std::mutex mu;
    winrt::Windows::Storage::Streams::IRandomAccessStream stream{nullptr};
    std::uint64_t size = 0;
};

std::mutex g_rt_mu;
std::unordered_map<int, std::shared_ptr<RtFile>> g_rt_files;

int AdoptRt(std::shared_ptr<RtFile> file) {
    const int number = g_next_handle.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_rt_mu);
    g_rt_files.emplace(number, std::move(file));
    return number;
}

std::shared_ptr<RtFile> LookupRt(int number, bool take) {
    std::lock_guard<std::mutex> lock(g_rt_mu);
    auto it = g_rt_files.find(number);
    if (it == g_rt_files.end()) return nullptr;
    auto file = it->second;
    if (take) g_rt_files.erase(it);
    return file;
}

bool IsRtHandle(int number) {
    std::lock_guard<std::mutex> lock(g_rt_mu);
    return g_rt_files.find(number) != g_rt_files.end();
}

std::string HstringToUtf8(const winrt::hstring& text) { return winrt::to_string(text); }

std::atomic<bool> g_logged_interop_handle{false};

bool OpenRt(const std::string& path, std::uint64_t* size, int* handle, int* rt_err = nullptr) {
    const std::string win_path = WinPath(path);
    if (win_path.empty()) {
        if (rt_err != nullptr) *rt_err = ENOENT;
        return false;
    }
    HANDLE direct_handle = INVALID_HANDLE_VALUE;
    std::uint64_t direct_size = 0;
    std::shared_ptr<RtFile> file;
    const int err = OnStorageThread([&win_path, &direct_handle, &direct_size, &file]() {
        const auto item = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(
                              winrt::to_hstring(win_path))
                              .get();
        // Fast path (from XboxEmulationHub/RetroArch): pull a native Win32 HANDLE
        // from the FutureAccessList StorageFile via IStorageItemHandleAccess so
        // every subsequent Read/Seek runs directly on ::ReadFile / ::SetFilePointerEx
        // instead of bouncing through WinRT IRandomAccessStream::ReadAsync.
        if (auto* unk = static_cast<IUnknown*>(winrt::get_abi(item))) {
            IStorageItemHandleAccess* handle_access = nullptr;
            if (SUCCEEDED(unk->QueryInterface(__uuidof(IStorageItemHandleAccess),
                                              reinterpret_cast<void**>(&handle_access))) &&
                handle_access != nullptr) {
                HANDLE raw = INVALID_HANDLE_VALUE;
                const HRESULT hr = handle_access->Create(
                    HAO_READ,
                    static_cast<HANDLE_SHARING_OPTIONS>(HSO_SHARE_READ | HSO_SHARE_WRITE |
                                                        HSO_SHARE_DELETE),
                    HO_RANDOM_ACCESS, nullptr, &raw);
                handle_access->Release();
                if (SUCCEEDED(hr) && raw != INVALID_HANDLE_VALUE && raw != nullptr) {
                    LARGE_INTEGER file_size{};
                    if (::GetFileSizeEx(raw, &file_size) && file_size.QuadPart >= 0) {
                        direct_size = static_cast<std::uint64_t>(file_size.QuadPart);
                    }
                    direct_handle = raw;
                    if (!g_logged_interop_handle.exchange(true, std::memory_order_relaxed)) {
                        bbx::platform::LogF(
                            "hostfs: IStorageItemHandleAccess active for FutureAccessList USB reads (native Win32 HANDLE)");
                    }
                    return;
                }
            }
        }
        auto created = std::make_shared<RtFile>();
        created->stream = item.OpenAsync(winrt::Windows::Storage::FileAccessMode::Read).get();
        created->size = created->stream.Size();
        file = created;
    });
    if (rt_err != nullptr) *rt_err = err;
    if (direct_handle != INVALID_HANDLE_VALUE) {
        if (size != nullptr) *size = direct_size;
        if (handle != nullptr) {
            *handle = Adopt(direct_handle);
        } else {
            ::CloseHandle(direct_handle);
        }
        return true;
    }
    if (file == nullptr) return false;
    if (size != nullptr) *size = file->size;
    if (handle != nullptr) *handle = AdoptRt(std::move(file));
    return true;
}

bool StatRt(const std::string& path, FileStat* out, int* rt_err = nullptr) {
    const std::string win_path = WinPath(path);
    if (win_path.empty()) {
        if (rt_err != nullptr) *rt_err = ENOENT;
        return false;
    }
    bool found = false;
    const int file_err = OnStorageThread([&win_path, out, &found]() {
        const auto item = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(
                              winrt::to_hstring(win_path))
                              .get();
        const winrt::Windows::Storage::FileProperties::BasicProperties properties =
            item.GetBasicPropertiesAsync().get();
        out->size = properties.Size();
        out->is_dir = false;
        out->mtime = 0;
        found = true;
    });
    if (found) return true;
    const int dir_err = OnStorageThread([&win_path, out, &found]() {
        const auto folder = winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(
                                winrt::to_hstring(win_path))
                                .get();
        (void)folder;
        out->size = 0;
        out->is_dir = true;
        out->mtime = 0;
        found = true;
    });
    if (rt_err != nullptr) {
        *rt_err = dir_err == ENOENT || file_err == ENOENT ? ENOENT : (dir_err != 0 ? dir_err : file_err);
    }
    return found;
}

bool ListRt(const std::string& path, std::vector<DirEntry>* out, int* rt_err = nullptr) {
    const std::string win_path = WinPath(path);
    if (win_path.empty()) {
        if (rt_err != nullptr) *rt_err = ENOENT;
        return false;
    }
    std::vector<DirEntry> entries;
    bool ok = false;
    const int err = OnStorageThread([&win_path, &entries, &ok]() {
        const auto folder = winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(
                                winrt::to_hstring(win_path))
                                .get();
        const winrt::Windows::Foundation::Collections::IVectorView<
            winrt::Windows::Storage::IStorageItem>
            children = folder.GetItemsAsync().get();
        const std::uint32_t count = children.Size();
        entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const winrt::Windows::Storage::IStorageItem child = children.GetAt(i);
            DirEntry entry;
            entry.name = HstringToUtf8(child.Name());
            entry.is_dir = child.IsOfType(winrt::Windows::Storage::StorageItemTypes::Folder);
            entries.push_back(std::move(entry));
        }
        ok = true;
    });
    if (rt_err != nullptr) *rt_err = err;
    if (!ok) return false;
    *out = std::move(entries);
    return true;
}

// --- operations ------------------------------------------------------------
bool StatImpl(const char* path, FileStat* out) {
    const std::wstring wide = Widen(path);
    if (wide.empty()) {
        Fail(ENOENT);
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExFromAppW(wide.c_str(), GetFileExInfoStandard, &data)) {
        const DWORD code = ::GetLastError();
        // A granted folder is not on the FromApp list; ask WinRT before failing.
        int rt_err = 0;
        if (StatRt(path, out, &rt_err)) {
            t_error = 0;
            return true;
        }
        const int err = (code == ERROR_ACCESS_DENIED && rt_err != 0) ? rt_err : ErrnoFrom(code);
        bbx::platform::LogF("hostfs: stat refused %s (win32=%lu, errno=%d)", path,
                            static_cast<unsigned long>(code), err);
        Fail(err);
        return false;
    }
    out->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    // FILETIME is 100 ns since 1601; the guest wants seconds since 1970.
    const std::uint64_t ticks = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                data.ftLastWriteTime.dwLowDateTime;
    out->mtime = ticks > 116444736000000000ull
                     ? static_cast<std::int64_t>((ticks - 116444736000000000ull) / 10000000ull)
                     : 0;
    return true;
}

bool MkDirOne(const std::string& path, bool allow_existing) {
    const std::wstring wide = Widen(path.c_str());
    if (wide.empty()) return false;
    if (::CreateDirectoryFromAppW(wide.c_str(), nullptr)) return true;
    const DWORD err = ::GetLastError();
    if (allow_existing) {
        if (err == ERROR_ALREADY_EXISTS) {
            FileStat st{};
            if (StatImpl(path.c_str(), &st) && st.is_dir) return true;
            Fail(EEXIST);
            return false;
        }
        if (err == ERROR_ACCESS_DENIED) {
            FileStat st{};
            if (StatImpl(path.c_str(), &st) && st.is_dir) return true;
        }
    }
    Fail(ErrnoFrom(err));
    return false;
}

bool MkDirImpl(const char* path, bool create_parents) {
    std::string full = Slashify(path != nullptr ? path : "");
    while (full.size() > 1 && full.back() == '/') full.pop_back();
    if (full.empty()) {
        Fail(EINVAL);
        return false;
    }
    if (!create_parents) {
        return MkDirOne(full, /*allow_existing=*/false);
    }
    FileStat existing{};
    if (StatImpl(full.c_str(), &existing)) {
        if (existing.is_dir) return true;
        Fail(EEXIST);
        return false;
    }
    std::string sofar;
    std::size_t pos = 0;
    if (full.size() >= 2 && full[1] == ':') {
        sofar = full.substr(0, 2);
        pos = 2;
    }
    while (pos <= full.size()) {
        const std::size_t end = full.find('/', pos);
        const std::string part = full.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!part.empty()) {
            sofar = sofar.empty() ? part : (sofar.back() == '/' ? sofar + part : sofar + '/' + part);
            if (!MkDirOne(sofar, /*allow_existing=*/true)) {
                return false;
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return true;
}

bool ListImpl(const char* path, std::vector<DirEntry>* out) {
    out->clear();
    std::string pattern = Slashify(path != nullptr ? path : "");
    while (!pattern.empty() && pattern.back() == '/') pattern.pop_back();
    pattern += "/*";
    const std::wstring wide = Widen(pattern.c_str());
    if (wide.empty()) {
        Fail(ENOENT);
        return false;
    }
    WIN32_FIND_DATAW found{};
    HANDLE find = ::FindFirstFileExFromAppW(wide.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch,
                                            nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        int rt_err = 0;
        if (ListRt(path != nullptr ? path : "", out, &rt_err)) {
            t_error = 0;
            return true;
        }
        const int err = (code == ERROR_ACCESS_DENIED && rt_err != 0) ? rt_err : ErrnoFrom(code);
        bbx::platform::LogF("hostfs: list refused %s (win32=%lu, errno=%d)", path,
                            static_cast<unsigned long>(code), err);
        Fail(err);
        return false;
    }
    do {
        const wchar_t* name = found.cFileName;
        if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) continue;
        DirEntry entry;
        entry.name = Narrow(name);
        entry.is_dir = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.size = (static_cast<std::uint64_t>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
        out->push_back(std::move(entry));
    } while (::FindNextFileW(find, &found));
    // A find that ends any way other than "no more files" started fine; the
    // entries it produced are still the truth the caller asked for.
    ::FindClose(find);
    return true;
}

int OpenImpl(const char* path, int flags, int mode) {
    (void)mode;  // CreateFile2FromAppW takes no mode; AppContainer files are user-owned.
    t_error = 0;
    const std::wstring wide = Widen(path);
    if (wide.empty()) return Fail(ENOENT);

    const bool is_append = (flags & kAppend) != 0;
    DWORD access = 0;
    switch (flags & kAccessMask) {
        case kRead: access = GENERIC_READ; break;
        case kWrite: access = is_append ? (FILE_APPEND_DATA | SYNCHRONIZE) : GENERIC_WRITE; break;
        case kRead | kWrite: access = GENERIC_READ | GENERIC_WRITE; break;
        default: access = GENERIC_READ; break;
    }

    DWORD disposition = OPEN_EXISTING;
    if ((flags & kCreate) != 0) {
        disposition = (flags & kExclusive) != 0 ? CREATE_NEW
                      : (flags & kTruncate) != 0 ? CREATE_ALWAYS
                                                 : OPEN_ALWAYS;
    } else if ((flags & kTruncate) != 0) {
        disposition = TRUNCATE_EXISTING;
    }
    if ((flags & kDirectory) != 0) {
        access |= FILE_LIST_DIRECTORY;
        disposition = OPEN_EXISTING;
    }

    // Share everything: the guest opens the same save twice (a writer and a
    // reader of its own), and the console's own file system allows it.
    HANDLE file = ::CreateFile2FromAppW(wide.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        disposition, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        int rt_err = 0;
        // Read-only opens of a granted tree go through WinRT. A write to such a
        // path is still refused, and says so: the dump is read-only by design.
        const bool wants_write = (flags & (kWrite | kCreate | kTruncate | kAppend)) != 0;
        if ((flags & kDirectory) == 0 && !wants_write) {
            int rt_handle = -1;
            if (OpenRt(path, nullptr, &rt_handle, &rt_err)) {
                t_error = 0;
                return rt_handle;
            }
        }
        const int err = (code == ERROR_ACCESS_DENIED && rt_err != 0) ? rt_err : ErrnoFrom(code);
        bbx::platform::LogF("hostfs: open refused %s (win32=%lu, errno=%d, flags=0x%x)", path,
                            static_cast<unsigned long>(code), err, flags);
        return Fail(err);
    }
    if (is_append && (flags & kAccessMask) == (kRead | kWrite)) {
        LARGE_INTEGER zero{};
        (void)::SetFilePointerEx(file, zero, nullptr, FILE_END);
    }
    return Adopt(file, is_append);
}

std::int64_t ReadImpl(int handle, void* buffer, std::uint64_t count) {
    if (const auto rt = LookupRt(handle, false)) {
        if (count == 0) return 0;
        std::lock_guard<std::mutex> stream_lock(rt->mu);
        std::int64_t got = -1;
        const int err = OnStorageThread([&rt, buffer, count, &got]() {
            auto stream = rt->stream;
            const std::uint64_t pos = stream.Position();
            if (pos >= rt->size) {
                got = 0;
                return;
            }
            constexpr std::uint64_t kMaxRtRead = 4ull << 20;
            const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>({count, rt->size - pos, kMaxRtRead}));
            const winrt::Windows::Storage::Streams::IBuffer bytes =
                winrt::Windows::Storage::Streams::Buffer(want);
            const winrt::Windows::Storage::Streams::IBuffer read =
                stream.ReadAsync(bytes, want,
                                 winrt::Windows::Storage::Streams::InputStreamOptions::Partial)
                    .get();
            const auto length = read.Length();
            if (length > 0) std::memcpy(buffer, read.data(), length);
            got = static_cast<std::int64_t>(length);
        });
        if (err != 0 || got < 0) return Fail(err != 0 ? err : EIO);
        return got;
    }
    const auto file = Lookup(handle, false);
    if (!file) {
        Fail(EBADF);
        return -1;
    }
    // ReadFile takes a DWORD; the HLE reads in chunks far below 4 GiB, but the
    // clamp keeps a wild count from wrapping.
    DWORD want = count > 0x40000000ull ? 0x40000000u : static_cast<DWORD>(count);
    DWORD got = 0;
    if (!::ReadFile(file->handle, buffer, want, &got, nullptr)) {
        FailWin32();
        return -1;
    }
    return static_cast<std::int64_t>(got);
}

std::int64_t WriteImpl(int handle, const void* buffer, std::uint64_t count) {
    if (IsRtHandle(handle)) return Fail(EACCES);  // the dump is read-only
    const auto file = Lookup(handle, false);
    if (!file) {
        Fail(EBADF);
        return -1;
    }
    DWORD want = count > 0x40000000ull ? 0x40000000u : static_cast<DWORD>(count);
    DWORD put = 0;
    if (file->append) {
        std::lock_guard<std::mutex> lock(file->mu);
        LARGE_INTEGER zero{};
        (void)::SetFilePointerEx(file->handle, zero, nullptr, FILE_END);
        if (!::WriteFile(file->handle, buffer, want, &put, nullptr)) {
            FailWin32();
            return -1;
        }
        return static_cast<std::int64_t>(put);
    }
    if (!::WriteFile(file->handle, buffer, want, &put, nullptr)) {
        FailWin32();
        return -1;
    }
    return static_cast<std::int64_t>(put);
}

std::int64_t SeekImpl(int handle, std::int64_t offset, int whence) {
    if (const auto rt = LookupRt(handle, false)) {
        std::lock_guard<std::mutex> stream_lock(rt->mu);
        std::int64_t target = -1;
        bool invalid = false;
        const int err = OnStorageThread([&rt, offset, whence, &target, &invalid]() {
            std::uint64_t base = 0;
            if (whence == kSeekCur) {
                base = rt->stream.Position();
            } else if (whence == kSeekEnd) {
                base = rt->size;
            }
            const std::int64_t next = static_cast<std::int64_t>(base) + offset;
            if (next < 0) {
                invalid = true;
                return;
            }
            rt->stream.Seek(static_cast<std::uint64_t>(next));
            target = next;
        });
        if (invalid) return Fail(EINVAL);
        if (err != 0 || target < 0) return Fail(err != 0 ? err : EIO);
        return target;
    }
    const auto file = Lookup(handle, false);
    if (!file) {
        Fail(EBADF);
        return -1;
    }
    DWORD move = FILE_BEGIN;
    if (whence == kSeekCur) move = FILE_CURRENT;
    else if (whence == kSeekEnd) move = FILE_END;
    LARGE_INTEGER distance{};
    distance.QuadPart = offset;
    LARGE_INTEGER placed{};
    if (!::SetFilePointerEx(file->handle, distance, &placed, move)) {
        FailWin32();
        return -1;
    }
    return placed.QuadPart;
}

bool TruncateImpl(int handle, std::int64_t length) {
    if (IsRtHandle(handle)) {
        Fail(EACCES);
        return false;
    }
    const auto file = Lookup(handle, false);
    if (!file) {
        Fail(EBADF);
        return false;
    }
    const std::int64_t at = SeekImpl(handle, 0, kSeekCur);
    if (at < 0) return false;
    if (SeekImpl(handle, length, kSeekSet) < 0) return false;
    const bool ok = ::SetEndOfFile(file->handle) != FALSE;
    if (!ok) FailWin32();
    if (SeekImpl(handle, at < length ? at : length, kSeekSet) < 0) return false;
    return ok;
}

bool FlushImpl(int handle) {
    if (IsRtHandle(handle)) return true;  // nothing buffered on the WinRT side
    const auto file = Lookup(handle, false);
    if (!file) {
        Fail(EBADF);
        return false;
    }
    if (!::FlushFileBuffers(file->handle)) {
        FailWin32();
        return false;
    }
    return true;
}

bool CloseImpl(int handle) {
    if (const auto rt = LookupRt(handle, true)) {
        (void)rt;
        return true;
    }
    const auto file = Lookup(handle, true);
    if (!file) {
        Fail(EBADF);
        return false;
    }
    return true;
}

bool UnlinkImpl(const char* path) {
    const std::wstring wide = Widen(path);
    if (wide.empty()) {
        Fail(ENOENT);
        return false;
    }
    if (!::DeleteFileFromAppW(wide.c_str())) {
        FailWin32();
        return false;
    }
    return true;
}

bool RmDirImpl(const char* path) {
    const std::wstring wide = Widen(path);
    if (wide.empty()) {
        Fail(ENOENT);
        return false;
    }
    if (!::RemoveDirectoryFromAppW(wide.c_str())) {
        FailWin32();
        return false;
    }
    return true;
}

bool RenameImpl(const char* from, const char* to, bool replace) {
    const std::wstring a = Widen(from);
    const std::wstring b = Widen(to);
    if (a.empty() || b.empty()) {
        Fail(EINVAL);
        return false;
    }
    if (replace) {
        FileStat src_stat{};
        const bool is_dir = StatImpl(from, &src_stat) && src_stat.is_dir;
        if (!is_dir) {
            if (::ReplaceFileFromAppW(b.c_str(), a.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
                return true;
            }
            if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
                FailWin32();
                return false;
            }
        } else {
            (void)::RemoveDirectoryFromAppW(b.c_str());
        }
    }
    if (!::MoveFileFromAppW(a.c_str(), b.c_str())) {
        FailWin32();
        return false;
    }
    return true;
}

bool CanonicalImpl(const char* path, std::string* out) {
    // Nothing to resolve: every path the AppContainer hands out is already
    // absolute, and GetFullPathName is a desktop call. Normalise the separators
    // and the trailing slash, and require that it exists.
    if (path == nullptr || path[0] == '\0') {
        Fail(ENOENT);
        return false;
    }
    std::string text = Slashify(path);
    const std::wstring wide = Widen(text.c_str());
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExFromAppW(wide.c_str(), GetFileExInfoStandard, &data)) {
        const DWORD code = ::GetLastError();
        FileStat probed{};
        int rt_err = 0;
        if (!StatRt(path, &probed, &rt_err)) {
            const int err = (code == ERROR_ACCESS_DENIED && rt_err != 0) ? rt_err : ErrnoFrom(code);
            bbx::platform::LogF("hostfs: canonicalise refused %s (win32=%lu, errno=%d)", path,
                                static_cast<unsigned long>(code), err);
            Fail(err);
            return false;
        }
        t_error = 0;
    }
    while (text.size() > 1 && text.back() == '/') {
        if (text.size() == 3 && text[1] == ':') break;
        text.pop_back();
    }
    *out = text;
    return true;
}

int LastErrorImpl() { return t_error; }

const Ops kOps = {
    "appcontainer",
    &StatImpl,
    &MkDirImpl,
    &ListImpl,
    &OpenImpl,
    &ReadImpl,
    &WriteImpl,
    &SeekImpl,
    &TruncateImpl,
    &FlushImpl,
    &CloseImpl,
    &UnlinkImpl,
    &RmDirImpl,
    &RenameImpl,
    &CanonicalImpl,
    &LastErrorImpl,
};

}  // namespace

const Ops& Table() { return kOps; }

}  // namespace bbx::hostfs
