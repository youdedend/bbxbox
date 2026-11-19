// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox/platform/storage.h"

#include "xbox/platform/host_fs.h"

#include "xbox/platform/log.h"

#include <windows.h>

// The FromApp file APIs: the APP-partition surface for paths outside the
// package's own folders. They live in their own header, they are not in
// <windows.h>, and the SDK declares them only when NTDDI_VERSION is at least
// NTDDI_WIN10_RS3 (see xbox/CMakeLists.txt).
#include <appmodel.h>
#include <fileapifromapp.h>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <thread>

namespace bbx::platform {
namespace {

StorageInitResult g_storage;
std::once_flag g_once;

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), need);
    return out;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), need,
                          nullptr, nullptr);
    return out;
}

std::string Join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

struct WinrtStorageProber {
    winrt::Windows::Storage::ApplicationData current{nullptr};

    StorageProbeOutcome AppData() {
        try {
            current = winrt::Windows::Storage::ApplicationData::Current();
            return {"", 0, true};
        } catch (const winrt::hresult_error& error) {
            return {"", static_cast<std::int32_t>(error.code().value), false};
        }
    }

    StorageProbeOutcome Local() {
        try {
            return {winrt::to_string(current.LocalFolder().Path()), 0, true};
        } catch (const winrt::hresult_error& error) {
            return {"", static_cast<std::int32_t>(error.code().value), false};
        }
    }

    StorageProbeOutcome Temporary() {
        try {
            return {winrt::to_string(current.TemporaryFolder().Path()), 0, true};
        } catch (const winrt::hresult_error& error) {
            return {"", static_cast<std::int32_t>(error.code().value), false};
        }
    }

    StorageProbeOutcome Roaming() {
        try {
            return {winrt::to_string(current.RoamingFolder().Path()), 0, true};
        } catch (const winrt::hresult_error& error) {
            return {"", static_cast<std::int32_t>(error.code().value), false};
        }
    }

    StorageProbeOutcome Package() {
        try {
            return {
                winrt::to_string(winrt::Windows::ApplicationModel::Package::Current().InstalledPath()),
                0, true};
        } catch (const winrt::hresult_error& error) {
            return {"", static_cast<std::int32_t>(error.code().value), false};
        }
    }

    StorageProbeOutcome DeriveLocal() {
        // A launch that dies with an empty LocalState has no log to read, so
        // the log cannot depend on WinRT activation. The folder is where the
        // platform puts it - %LOCALAPPDATA%\Packages\<family>\LocalState -
        // and both halves are reachable from an AppContainer with kernel32
        // alone. If this path is wrong, LogOpen's open fails and the run is
        // exactly as logless as it was before: nothing is lost by trying.
        wchar_t local_app_data[MAX_PATH * 2] = {};
        wchar_t family[PACKAGE_FAMILY_NAME_MAX_LENGTH] = {};
        UINT32 family_len = PACKAGE_FAMILY_NAME_MAX_LENGTH;
        if (::GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH * 2) > 0) {
            const LONG rc = ::GetCurrentPackageFamilyName(&family_len, family);
            if (rc == ERROR_SUCCESS) {
                std::wstring derived(local_app_data);
                derived += L"\\Packages\\";
                derived += family;
                derived += L"\\LocalState";
                const std::string narrow = Narrow(derived);
                if (!narrow.empty()) return {narrow, 0, true};
            }
            return {"", static_cast<std::int32_t>(HRESULT_FROM_WIN32(static_cast<DWORD>(rc))),
                    false};
        }
        return {"", static_cast<std::int32_t>(HRESULT_FROM_WIN32(::GetLastError())), false};
    }

    void PrepareDirs(const StoragePaths& paths) {
        ::CreateDirectoryFromAppW(Widen(Join(paths.local_state, "shadercache")).c_str(), nullptr);
        ::CreateDirectoryFromAppW(Widen(Join(paths.local_state, "crashes")).c_str(), nullptr);
        ::CreateDirectoryFromAppW(Widen(Join(paths.local_state, "guestswap")).c_str(), nullptr);
    }
};

}  // namespace

const StorageInitResult& StorageInit() {
    // Runs before LogOpen in wWinMain and is asked again by LauncherView::Initialize:
    // the probes record into `g_storage` once and every caller reads that struct,
    // while ReplayStorageResults() writes the recorded lines once loader.log exists.
    return *RunStorageInitOnce(g_once, g_storage, []() { return WinrtStorageProber{}; });
}

void ReplayStorageResults() {
    const StorageInitResult& state = StorageInit();
    ReplayStorageInit(
        state,
        [](Level level, const std::string& line) { LogMessage(level, "%s", line.c_str()); },
        []() { LogFlush(); });
}

const StoragePaths& Paths() { return g_storage.paths; }

std::string Local(const std::string& relative) {
    if (g_storage.paths.local_state.empty()) return relative;
    return Join(g_storage.paths.local_state, relative);
}

bool ReadBinary(const std::string& path, std::vector<std::uint8_t>& out, std::size_t cap) {
    out.clear();
    // CreateFile2FromApp is refused on a folder the app holds only a
    // FutureAccessList token for - the dump on USB - so the read goes through
    // the host_fs seam, whose console backend falls back to WinRT for exactly
    // that case. Attributes on such a path are readable, which is why the dump
    // looked present and only param.sfo "could not be read".
    const int handle = bbx::hostfs::Open(path, bbx::hostfs::kRead);
    if (handle < 0) {
        LogMessage(Level::kWarn, "storage: %s could not be opened (errno=%d)", path.c_str(),
                   bbx::hostfs::LastError());
        return false;
    }
    bbx::hostfs::FileStat info{};
    if (!bbx::hostfs::Stat(path, &info) || info.is_dir || info.size > cap) {
        (void)bbx::hostfs::Close(handle);
        LogMessage(Level::kWarn, "storage: %s is not a readable file under the %zu byte cap",
                   path.c_str(), cap);
        return false;
    }
    out.resize(static_cast<std::size_t>(info.size));
    std::size_t done = 0;
    while (done < out.size()) {
        const std::int64_t got =
            bbx::hostfs::Read(handle, out.data() + done, out.size() - done);
        if (got <= 0) break;
        done += static_cast<std::size_t>(got);
    }
    (void)bbx::hostfs::Close(handle);
    if (done != out.size()) {
        LogMessage(Level::kWarn, "storage: %s read %zu of %zu bytes", path.c_str(), done,
                   out.size());
        out.clear();
        return false;
    }
    return true;
}

bool ReadText(const std::string& path, std::string& out, std::size_t cap) {
    std::vector<std::uint8_t> bytes;
    if (!ReadBinary(path, bytes, cap)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

bool WriteBinary(const std::string& path, std::span<const std::uint8_t> bytes) {
    const std::wstring wide = Widen(path);
    const std::wstring temporary = Widen(path + ".tmp");
    HANDLE file = ::CreateFile2FromAppW(temporary.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size();
    // The breadcrumb has to survive a power cut, not just a crash.
    ok = ok && ::FlushFileBuffers(file);
    ::CloseHandle(file);
    if (!ok) {
        ::DeleteFileFromAppW(temporary.c_str());
        return false;
    }
    ::DeleteFileFromAppW(wide.c_str());
    return ::MoveFileFromAppW(temporary.c_str(), wide.c_str()) != 0;
}

bool WriteText(const std::string& path, const std::string& text) {
    return WriteBinary(path, std::span<const std::uint8_t>(
                                  reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

bool AppendText(const std::string& path, const std::string& text) {
    std::string existing;
    // Append never fails on a missing file: ReadText reports false and `existing`
    // stays empty, which is exactly the file-to-create case.
    (void)ReadText(path, existing, 4u << 20);
    return WriteText(path, existing + text);
}

bool Exists(const std::string& path) { return bbx::hostfs::Exists(path); }

bool IsDir(const std::string& path) { return bbx::hostfs::IsDir(path); }

std::uint64_t FileSize(const std::string& path) {
    bbx::hostfs::FileStat info{};
    if (!bbx::hostfs::Stat(path, &info) || info.is_dir) return 0;
    return info.size;
}

namespace {

std::vector<std::string> ListEntries(const std::string& path, bool want_dirs) {
    std::vector<bbx::hostfs::DirEntry> entries;
    if (!bbx::hostfs::ListDir(path, &entries)) {
        LogMessage(Level::kWarn, "storage: %s could not be listed (errno=%d)", path.c_str(),
                   bbx::hostfs::LastError());
        return {};
    }
    std::vector<std::string> out;
    for (const auto& entry : entries) {
        if (entry.is_dir != want_dirs) continue;
        out.push_back(entry.name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

std::vector<std::string> ListDirs(const std::string& path) { return ListEntries(path, true); }
std::vector<std::string> ListFiles(const std::string& path) { return ListEntries(path, false); }

bool PickFolderGrant(std::string& path_out, std::string& token_out) {
    // The picker is a UI-thread call, and blocking the dispatcher here stopped the
    // only thread that can pump the picker: choosing a dump folder froze the
    // launcher until the ten-minute timeout expired and nothing ever appeared.
    // The folder is therefore asked for with a completion handler - the
    // dispatcher returns immediately and keeps pumping - while the caller waits
    // on a worker thread.
    struct Shared {
        std::mutex mutex;
        std::string path;
        std::string token;
    };
    auto shared = std::make_shared<Shared>();
    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (done == nullptr) return false;

    std::thread([shared, done]() {
        try {
            auto dispatcher = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView()
                                  .CoreWindow()
                                  .Dispatcher();
            dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                                [shared, done]() {
                using winrt::Windows::Storage::Pickers::FolderPicker;
                namespace AccessCache = winrt::Windows::Storage::AccessCache;
                namespace Collections = winrt::Windows::Foundation::Collections;
                namespace Foundation = winrt::Windows::Foundation;
                try {
                    FolderPicker picker;
                    picker.SuggestedStartLocation(
                        winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
                    // A FolderPicker shows nothing until its filter has an entry;
                    // the filter is an IVector<hstring>, so the '*' is a real
                    // hstring.
                    Collections::IVector<winrt::hstring> filters = picker.FileTypeFilter();
                    const winrt::hstring any_file = L"*";
                    filters.Append(any_file);
                    // Keep the picker alive until its operation completes, and
                    // complete on the handler so this callback can return.
                    auto keep = std::make_shared<FolderPicker>(picker);
                    auto operation = picker.PickSingleFolderAsync();
                    operation.Completed([shared, done, keep](
                                            const Foundation::IAsyncOperation<
                                                winrt::Windows::Storage::StorageFolder>& async,
                                            Foundation::AsyncStatus status) {
                        try {
                            if (status == Foundation::AsyncStatus::Completed) {
                                const auto folder = async.GetResults();
                                if (folder) {
                                    std::lock_guard<std::mutex> lock(shared->mutex);
                                    shared->path = winrt::to_string(folder.Path());
                                    auto recent =
                                        AccessCache::StorageApplicationPermissions::FutureAccessList();
                                    const winrt::Windows::Storage::IStorageItem item = folder;
                                    // Add, not AddOrReplace: an extra entry in a
                                    // 1,000-entry list costs nothing and the list
                                    // is pruned on load (see RestoreDump).
                                    shared->token = winrt::to_string(recent.Add(item));
                                }
                            } else {
                                LogMessage(Level::kWarn,
                                           "storage: folder picker finished with status %d",
                                           static_cast<int>(status));
                            }
                        } catch (const winrt::hresult_error& error) {
                            LogMessage(Level::kWarn, "storage: folder picker failed (0x%08x)",
                                       static_cast<unsigned>(error.code().value));
                        }
                        ::SetEvent(done);
                    });
                } catch (const winrt::hresult_error& error) {
                    LogMessage(Level::kWarn, "storage: folder picker could not start (0x%08x)",
                               static_cast<unsigned>(error.code().value));
                    ::SetEvent(done);
                }
            });
        } catch (const winrt::hresult_error& error) {
            LogMessage(Level::kWarn, "storage: no dispatcher for the folder picker (0x%08x)",
                       static_cast<unsigned>(error.code().value));
            ::SetEvent(done);
        }
    }).detach();

    const DWORD waited = ::WaitForSingleObject(done, 10 * 60 * 1000);
    ::CloseHandle(done);
    if (waited != WAIT_OBJECT_0) {
        LogMessage(Level::kWarn, "storage: folder picker timed out after ten minutes");
        return false;
    }
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (shared->path.empty()) return false;
    path_out = shared->path;
    token_out = shared->token;
    return true;
}

bool ResolveGrant(const std::string& token, std::string& path_out) {
    if (token.empty()) return false;
    try {
        using namespace winrt::Windows::Storage::AccessCache;
        if (!StorageApplicationPermissions::FutureAccessList().ContainsItem(winrt::to_hstring(token))) {
            return false;
        }
        const auto item = StorageApplicationPermissions::FutureAccessList().GetFolderAsync(
            winrt::to_hstring(token)).get();
        if (!item) return false;
        path_out = winrt::to_string(item.Path());
        return !path_out.empty();
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

bool ReleaseGrant(const std::string& token) {
    if (token.empty()) return false;
    try {
        winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList()
            .Remove(winrt::to_hstring(token));
        return true;
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

std::vector<std::string> CandidateDumpRoots() {
    std::vector<std::string> roots;
    const auto add_unique = [&roots](std::string path) {
        if (path.empty()) return;
        for (char& ch : path) {
            if (ch == '/') ch = '\\';
        }
        while (path.size() > 3 && path.back() == '\\') path.pop_back();
        auto same = [&path](const std::string& existing) {
            if (existing.size() != path.size()) return false;
            for (std::size_t i = 0; i < existing.size(); ++i) {
                if (std::towlower(static_cast<unsigned char>(existing[i])) !=
                    std::towlower(static_cast<unsigned char>(path[i]))) {
                    return false;
                }
            }
            return true;
        };
        if (std::none_of(roots.begin(), roots.end(), same)) {
            roots.push_back(std::move(path));
        }
    };

    // Xbox Dev Mode internal storage: D:\ root itself is denied to an
    // AppContainer, but D:\DevelopmentFiles is readable without the picker and
    // is where Device Portal / FTP dumps are usually placed.
    {
        WIN32_FIND_DATAW dev_data{};
        HANDLE dev_find = ::FindFirstFileExFromAppW(L"D:\\DevelopmentFiles\\*", FindExInfoBasic,
                                                    &dev_data, FindExSearchNameMatch, nullptr, 0);
        if (dev_find != INVALID_HANDLE_VALUE) {
            ::FindClose(dev_find);
            add_unique("D:\\DevelopmentFiles");
        }
    }

    // Removable USB drives via KnownFolders::RemovableDevices(), queried on an
    // MTA worker thread so calling CandidateDumpRoots() from ui-main never
    // blocks or throws on the CoreWindow dispatcher thread.
    {
        std::vector<std::string> winrt_drives;
        std::thread worker([&winrt_drives]() {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (...) {
            }
            try {
                const auto drives =
                    winrt::Windows::Storage::KnownFolders::RemovableDevices().GetFoldersAsync().get();
                for (const auto& drive : drives) {
                    const std::string p = winrt::to_string(drive.Path());
                    if (!p.empty()) winrt_drives.push_back(p);
                }
            } catch (const winrt::hresult_error& err) {
                LogMessage(Level::kWarn, "storage: RemovableDevices enumeration failed (0x%08x)",
                           static_cast<unsigned>(err.code().value));
            } catch (...) {
            }
        });
        worker.join();
        for (auto& drive : winrt_drives) add_unique(std::move(drive));
    }

    // Fallback drive-letter probe: catches E:\ and any other mapped volume
    // reachable through FromApp even if RemovableDevices returned nothing.
    for (wchar_t letter = L'D'; letter <= L'Z'; ++letter) {
        const std::wstring root{letter, L':', L'\\'};
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileExFromAppW((root + L"*").c_str(), FindExInfoBasic, &data,
                                                FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE) continue;
        ::FindClose(find);
        add_unique(Narrow(root));
    }
    if (!g_storage.paths.local_state.empty()) add_unique(g_storage.paths.local_state);
    return roots;
}

namespace {
std::atomic<bool> g_usb_access_denied{false};
std::atomic<bool> g_usb_hint_logged{false};
}  // namespace

bool SawUsbAccessDenied() { return g_usb_access_denied.load(std::memory_order_relaxed); }

void ResetUsbAccessDenied() { g_usb_access_denied.store(false, std::memory_order_relaxed); }

DumpFs MakeDumpFs() {
    DumpFs fs;
    fs.exists = [](const std::string& path) { return Exists(path); };
    fs.is_dir = [](const std::string& path) { return IsDir(path); };
    fs.read = [](const std::string& path, std::size_t cap) {
        std::vector<std::uint8_t> bytes;
        const bool ok = ReadBinary(path, bytes, cap);
        if (!ok) {
            // "could not be read" folded four different failures into one line:
            // missing file, refused open, short read and an unparsable SFO. Say
            // which one it was.
            const int read_err = bbx::hostfs::LastError();
            bbx::hostfs::FileStat info{};
            const bool stat_ok = bbx::hostfs::Stat(path, &info);
            const int stat_err = stat_ok ? 0 : bbx::hostfs::LastError();
            if (read_err == EACCES || stat_err == EACCES) {
                g_usb_access_denied.store(true, std::memory_order_relaxed);
                if (!g_usb_hint_logged.exchange(true, std::memory_order_relaxed)) {
                    LogMessage(Level::kWarn, "storage: %s", UsbPermissionHint());
                }
            }
            LogMessage(Level::kWarn, "dump: read failed for %s (stat=%d size=%llu errno=%d)",
                       path.c_str(), stat_ok ? 1 : 0,
                       static_cast<unsigned long long>(stat_ok ? info.size : 0),
                       read_err != 0 ? read_err : stat_err);
            return std::vector<std::uint8_t>{};
        }
        std::uint32_t magic = 0;
        if (bytes.size() >= 4) {
            magic = (static_cast<std::uint32_t>(bytes[0]) << 24) |
                    (static_cast<std::uint32_t>(bytes[1]) << 16) |
                    (static_cast<std::uint32_t>(bytes[2]) << 8) |
                    static_cast<std::uint32_t>(bytes[3]);
        }
        LogMessage(Level::kInfo, "dump: read %zu bytes from %s (magic=0x%08X)", bytes.size(),
                   path.c_str(), magic);
        return bytes;
    };
    fs.list_dirs = [](const std::string& path) { return ListDirs(path); };
    return fs;
}

bool OpenUsbCacheTarget(const std::string& token, UsbCacheTarget& out) {
    std::string root;
    if (!ResolveGrant(token, root)) return false;
    if (!IsDir(root)) return false;
    out.root = root;
    out.token = token;
    return true;
}

}  // namespace bbx::platform
