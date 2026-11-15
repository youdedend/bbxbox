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

#include <windows.h>

// The FromApp surface: its own header, and declared only when NTDDI_VERSION is
// at least NTDDI_WIN10_RS3 (xbox/CMakeLists.txt sets it).
#include <fileapifromapp.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <unordered_map>

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

int Fail(int code) {
    t_error = code;
    return -1;
}

int FailWin32() { return Fail(ErrnoFrom(::GetLastError())); }

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
std::mutex g_handle_mu;
std::unordered_map<int, HANDLE> g_handles;
int g_next_handle = 1;  // 0 and negatives are "no handle"

int Adopt(HANDLE file) {
    std::lock_guard<std::mutex> lock(g_handle_mu);
    const int number = g_next_handle++;
    g_handles.emplace(number, file);
    return number;
}

HANDLE Lookup(int number, bool take) {
    std::lock_guard<std::mutex> lock(g_handle_mu);
    auto it = g_handles.find(number);
    if (it == g_handles.end()) return INVALID_HANDLE_VALUE;
    HANDLE file = it->second;
    if (take) g_handles.erase(it);
    return file;
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
        FailWin32();
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

bool MkDirOne(const std::string& path) {
    const std::wstring wide = Widen(path.c_str());
    if (wide.empty()) return false;
    if (::CreateDirectoryFromAppW(wide.c_str(), nullptr)) return true;
    // EXISTS is what "make sure this directory is there" means; anything else
    // fails, and the error is kept for the caller's log line.
    if (::GetLastError() == ERROR_ALREADY_EXISTS) return true;
    FailWin32();
    return false;
}

bool MkDirImpl(const char* path, bool create_parents) {
    std::string full = Slashify(path != nullptr ? path : "");
    while (!full.empty() && full.back() == '/') full.pop_back();
    if (full.empty()) {
        Fail(EINVAL);
        return false;
    }
    if (!create_parents) {
        if (MkDirOne(full)) return true;
        FailWin32();
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
            if (!MkDirOne(sofar)) {
                FailWin32();
                return false;
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return true;
}

bool ListImpl(const char* path, std::vector<DirEntry>* out) {
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
        FailWin32();
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

    DWORD access = 0;
    switch (flags & kAccessMask) {
        case kRead: access = GENERIC_READ; break;
        case kWrite: access = GENERIC_WRITE; break;
        case kRead | kWrite: access = GENERIC_READ | GENERIC_WRITE; break;
        default: access = GENERIC_READ; break;
    }
    if ((flags & kAppend) != 0) access |= FILE_APPEND_DATA;

    DWORD disposition = OPEN_EXISTING;
    if ((flags & kCreate) != 0) {
        disposition = (flags & kTruncate) != 0 ? CREATE_ALWAYS
                      : (flags & kExclusive) != 0 ? CREATE_NEW
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
    if (file == INVALID_HANDLE_VALUE) return FailWin32();
    return Adopt(file);
}

std::int64_t ReadImpl(int handle, void* buffer, std::uint64_t count) {
    HANDLE file = Lookup(handle, false);
    if (file == INVALID_HANDLE_VALUE) {
        Fail(EBADF);
        return -1;
    }
    // ReadFile takes a DWORD; the HLE reads in chunks far below 4 GiB, but the
    // clamp keeps a wild count from wrapping.
    DWORD want = count > 0x40000000ull ? 0x40000000u : static_cast<DWORD>(count);
    DWORD got = 0;
    if (!::ReadFile(file, buffer, want, &got, nullptr)) {
        FailWin32();
        return -1;
    }
    return static_cast<std::int64_t>(got);
}

std::int64_t WriteImpl(int handle, const void* buffer, std::uint64_t count) {
    HANDLE file = Lookup(handle, false);
    if (file == INVALID_HANDLE_VALUE) {
        Fail(EBADF);
        return -1;
    }
    DWORD want = count > 0x40000000ull ? 0x40000000u : static_cast<DWORD>(count);
    DWORD put = 0;
    if (!::WriteFile(file, buffer, want, &put, nullptr)) {
        FailWin32();
        return -1;
    }
    return static_cast<std::int64_t>(put);
}

std::int64_t SeekImpl(int handle, std::int64_t offset, int whence) {
    HANDLE file = Lookup(handle, false);
    if (file == INVALID_HANDLE_VALUE) {
        Fail(EBADF);
        return -1;
    }
    DWORD move = FILE_BEGIN;
    if (whence == kSeekCur) move = FILE_CURRENT;
    else if (whence == kSeekEnd) move = FILE_END;
    LARGE_INTEGER distance{};
    distance.QuadPart = offset;
    LARGE_INTEGER placed{};
    if (!::SetFilePointerEx(file, distance, &placed, move)) {
        FailWin32();
        return -1;
    }
    return placed.QuadPart;
}

bool TruncateImpl(int handle, std::int64_t length) {
    HANDLE file = Lookup(handle, false);
    if (file == INVALID_HANDLE_VALUE) {
        Fail(EBADF);
        return false;
    }
    const std::int64_t at = SeekImpl(handle, 0, kSeekCur);
    if (at < 0) return false;
    if (!SeekImpl(handle, length, kSeekSet)) return false;
    const bool ok = ::SetEndOfFile(file) != FALSE;
    if (!ok) FailWin32();
    if (!SeekImpl(handle, at < length ? at : length, kSeekSet)) return false;
    return ok;
}

bool FlushImpl(int handle) {
    HANDLE file = Lookup(handle, false);
    if (file == INVALID_HANDLE_VALUE) {
        Fail(EBADF);
        return false;
    }
    if (!::FlushFileBuffers(file)) {
        FailWin32();
        return false;
    }
    return true;
}

bool CloseImpl(int handle) {
    HANDLE file = Lookup(handle, true);
    if (file == INVALID_HANDLE_VALUE) {
        Fail(EBADF);
        return false;
    }
    if (!::CloseHandle(file)) {
        FailWin32();
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
        // ReplaceFileFromApp replaces an existing file, and does so atomically.
        // It fails when the destination does not exist, which is the common
        // case for a first save, so the plain move is the fallback - and the
        // plain move is also what "no replace" wants.
        if (::ReplaceFileFromAppW(b.c_str(), a.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
            return true;
        }
        if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
            FailWin32();
            return false;
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
        FailWin32();
        return false;
    }
    while (text.size() > 1 && text.back() == '/') text.pop_back();
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
