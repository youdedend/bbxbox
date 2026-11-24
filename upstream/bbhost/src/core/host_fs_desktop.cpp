// SPDX-License-Identifier: GPL-3.0-or-later
// host_fs.h over the desktop file system: the CRT on Windows, POSIX
// everywhere else. This is what bbhost's HLE always did, moved behind the
// table so the console can supply a different one; the Linux host build's
// tests exercise it, and a desktop Windows build compiles it unchanged.
//
// Not part of the package: the UWP build links host_fs_uwp.cpp instead (see
// cmake/bbhost.cmake), so none of these desktop entry points reach
// `bbxbox.exe`.
#include "xbox/platform/host_fs.h"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace bbx::hostfs {
namespace {

thread_local int t_error = 0;

int Fail(int code) {
    t_error = code;
    return -1;
}

std::string ToHost(const char* path) { return path != nullptr ? path : std::string(); }

#if defined(_WIN32)
int ToCrtFlags(int flags) {
    int out = _O_BINARY;
    switch (flags & kAccessMask) {
        case kWrite: out |= _O_WRONLY; break;
        case kRead | kWrite: out |= _O_RDWR; break;
        default: out |= _O_RDONLY; break;
    }
    if ((flags & kCreate) != 0) out |= _O_CREAT;
    if ((flags & kTruncate) != 0) out |= _O_TRUNC;
    if ((flags & kExclusive) != 0) out |= _O_EXCL;
    if ((flags & kAppend) != 0) out |= _O_APPEND;
    return out;
}

bool StatImpl(const char* path, FileStat* out) {
    struct _stat64 st {};
    if (_stat64(path, &st) != 0) {
        Fail(errno);
        return false;
    }
    out->size = static_cast<std::uint64_t>(st.st_size);
    out->is_dir = (st.st_mode & _S_IFDIR) != 0;
    out->mtime = static_cast<std::int64_t>(st.st_mtime);
    return true;
}

bool MkDirOne(const std::string& path) { return _mkdir(path.c_str()) == 0 || errno == EEXIST; }

bool ListImpl(const char* path, std::vector<DirEntry>* out) {
    std::string pattern = path;
    if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/') pattern.push_back('\\');
    pattern += "*";
    WIN32_FIND_DATAA found{};
    HANDLE find = FindFirstFileA(pattern.c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) {
        Fail(ENOENT);
        return false;
    }
    do {
        const char* name = found.cFileName;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) continue;
        DirEntry entry;
        entry.name = name;
        entry.is_dir = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.size = (static_cast<std::uint64_t>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
        out->push_back(std::move(entry));
    } while (FindNextFileA(find, &found));
    FindClose(find);
    return true;
}

bool CanonicalImpl(const char* path, std::string* out) {
    char buffer[4096];
    if (_fullpath(buffer, path, sizeof(buffer)) == nullptr) {
        Fail(ENOENT);
        return false;
    }
    *out = Slashify(buffer);
    while (out->size() > 1 && out->back() == '/') out->pop_back();
    return true;
}
#else
int ToPosixFlags(int flags) {
    int out = 0;
    switch (flags & kAccessMask) {
        case kWrite: out |= O_WRONLY; break;
        case kRead | kWrite: out |= O_RDWR; break;
        default: out |= O_RDONLY; break;
    }
    if ((flags & kCreate) != 0) out |= O_CREAT;
    if ((flags & kTruncate) != 0) out |= O_TRUNC;
    if ((flags & kExclusive) != 0) out |= O_EXCL;
    if ((flags & kAppend) != 0) out |= O_APPEND;
    if ((flags & kDirectory) != 0) out |= O_DIRECTORY;
    return out;
}

bool StatImpl(const char* path, FileStat* out) {
    struct stat st {};
    if (::stat(path, &st) != 0) {
        Fail(errno);
        return false;
    }
    out->size = static_cast<std::uint64_t>(st.st_size);
    out->is_dir = S_ISDIR(st.st_mode);
    out->mtime = static_cast<std::int64_t>(st.st_mtime);
    return true;
}

bool MkDirOne(const std::string& path) { return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST; }

bool ListImpl(const char* path, std::vector<DirEntry>* out) {
    DIR* dir = ::opendir(path);
    if (dir == nullptr) {
        Fail(errno);
        return false;
    }
    while (dirent* entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        DirEntry item;
        item.name = entry->d_name;
#if defined(DT_DIR)
        item.is_dir = entry->d_type == DT_DIR;
#else
        FileStat st;
        item.is_dir = StatImpl(JoinPath(path, item.name).c_str(), &st) && st.is_dir;
#endif
        if (item.is_dir) {
            item.size = 0;
        } else {
            FileStat st;
            if (StatImpl(JoinPath(path, item.name).c_str(), &st)) item.size = st.size;
        }
        out->push_back(std::move(item));
    }
    ::closedir(dir);
    return true;
}

bool CanonicalImpl(const char* path, std::string* out) {
    char buffer[4096];
    if (::realpath(path, buffer) == nullptr) {
        Fail(errno);
        return false;
    }
    *out = buffer;
    while (out->size() > 1 && out->back() == '/') out->pop_back();
    return true;
}
#endif

bool MkDirImpl(const char* path, bool create_parents) {
    std::string full = Slashify(ToHost(path));
    while (!full.empty() && full.back() == '/') full.pop_back();
    if (full.empty()) {
        Fail(EINVAL);
        return false;
    }
    if (!create_parents) {
        if (!MkDirOne(full)) {
            Fail(errno);
            return false;
        }
        return true;
    }
    std::string sofar;
    std::size_t pos = 0;
    if (full.size() >= 2 && full[1] == ':') {  // C:\ or C:/
        sofar = full.substr(0, 2);
        pos = 2;
    }
    if (pos == 0 && !full.empty() && full[0] == '/') {
        sofar = "/";  // an absolute path's first component is not a relative one
        pos = 1;
    }
    while (pos <= full.size()) {
        const std::size_t end = full.find('/', pos);
        const std::string part = full.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!part.empty()) {
            sofar = sofar.empty() ? part : (sofar.back() == '/' ? sofar + part : sofar + '/' + part);
            if (!MkDirOne(sofar)) {
                Fail(errno);
                return false;
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return true;
}

std::int64_t ReadImpl(int handle, void* buffer, std::uint64_t count) {
#if defined(_WIN32)
    const int got = _read(handle, buffer, static_cast<unsigned>(count > 0x7fffffffu ? 0x7fffffffu : count));
#else
    const ssize_t got = ::read(handle, buffer, static_cast<std::size_t>(count));
#endif
    if (got < 0) {
        Fail(errno);
        return -1;
    }
    return static_cast<std::int64_t>(got);
}

std::int64_t WriteImpl(int handle, const void* buffer, std::uint64_t count) {
#if defined(_WIN32)
    const int put = _write(handle, buffer, static_cast<unsigned>(count > 0x7fffffffu ? 0x7fffffffu : count));
#else
    const ssize_t put = ::write(handle, buffer, static_cast<std::size_t>(count));
#endif
    if (put < 0) {
        Fail(errno);
        return -1;
    }
    return static_cast<std::int64_t>(put);
}

std::int64_t SeekImpl(int handle, std::int64_t offset, int whence) {
#if defined(_WIN32)
    const __int64 got = _lseeki64(handle, offset, whence);
#else
    const off_t got = ::lseek(handle, static_cast<off_t>(offset), whence);
#endif
    if (got < 0) {
        Fail(errno);
        return -1;
    }
    return static_cast<std::int64_t>(got);
}

bool TruncateImpl(int handle, std::int64_t length) {
#if defined(_WIN32)
    const bool ok = _chsize_s(handle, length) == 0;
#else
    const bool ok = ::ftruncate(handle, static_cast<off_t>(length)) == 0;
#endif
    if (!ok) Fail(errno);
    return ok;
}

bool FlushImpl(int handle) {
#if defined(_WIN32)
    const bool ok = _commit(handle) == 0;
#else
    const bool ok = ::fsync(handle) == 0;
#endif
    if (!ok) Fail(errno);
    return ok;
}

bool CloseImpl(int handle) {
#if defined(_WIN32)
    const bool ok = _close(handle) == 0;
#else
    const bool ok = ::close(handle) == 0;
#endif
    if (!ok) Fail(errno);
    return ok;
}

bool UnlinkImpl(const char* path) {
#if defined(_WIN32)
    const bool ok = _unlink(path) == 0;
#else
    const bool ok = ::unlink(path) == 0;
#endif
    if (!ok) Fail(errno);
    return ok;
}

bool RmDirImpl(const char* path) {
#if defined(_WIN32)
    const bool ok = _rmdir(path) == 0;
#else
    const bool ok = ::rmdir(path) == 0;
#endif
    if (!ok) Fail(errno);
    return ok;
}

bool RenameImpl(const char* from, const char* to, bool replace) {
#if defined(_WIN32)
    // std::rename does not replace an existing file on Windows; MoveFileExW
    // does, atomically, which is what a save commit needs.
    const DWORD flags = replace ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH : 0;
    const bool ok = MoveFileExA(from, to, flags) != FALSE;
    if (!ok) Fail(static_cast<int>(GetLastError()));
    return ok;
#else
    if (!replace && Exists(to)) {
        Fail(EEXIST);
        return false;
    }
    const bool ok = ::rename(from, to) == 0;
    if (!ok) Fail(errno);
    return ok;
#endif
}

int OpenImpl(const char* path, int flags, int mode) {
    t_error = 0;
#if defined(_WIN32)
    const int handle = _open(path, ToCrtFlags(flags), mode);
#else
    const int handle = ::open(path, ToPosixFlags(flags), static_cast<mode_t>(mode));
#endif
    if (handle < 0) return Fail(errno);
    return handle;
}

int LastErrorImpl() { return t_error; }

const Ops kOps = {
    "desktop",
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

std::FILE* OpenStream(const char* path, const char* mode) {
    if (path == nullptr || mode == nullptr) {
        Fail(EINVAL);
        return nullptr;
    }
    std::FILE* f = std::fopen(path, mode);
    if (f == nullptr) Fail(errno);
    return f;
}

}  // namespace bbx::hostfs
