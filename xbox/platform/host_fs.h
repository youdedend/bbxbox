// SPDX-License-Identifier: GPL-3.0-or-later
// The host file system, as the guest's syscalls see it.
//
// bbhost's HLE (`hle/fs.cpp`, `hle/fios.cpp`, `hle/save_commit.cpp`) was
// written against the desktop CRT: `_open`, `_read`, `_stat64`, `_mkdir`,
// `FindFirstFileA`, `std::filesystem`, `MoveFileExW`. Every one of those
// resolves to `CreateFileW` inside the CRT, and `CreateFileW` is not callable
// from an AppContainer on a path the app was granted through a
// `FutureAccessList` token - which is exactly how the dump arrives. The call
// compiles, packages, and is refused at run time on the console.
//
// So the HLE gets its file system from here instead: one table of operations,
// two implementations, chosen at link time.
//
//   * `upstream/bbhost/src/core/host_fs_desktop.cpp` — the CRT and POSIX calls
//     bbhost always made. Linux uses it in the host tests; a desktop Windows
//     build uses the same file with its `_WIN32` branches.
//   * `xbox/platform/host_fs_uwp.cpp` — `CreateFile2FromApp`,
//     `GetFileAttributesExFromApp`, `FindFirstFileExFromApp`,
//     `CreateDirectoryFromApp`, `DeleteFileFromApp` and `MoveFileFromApp`.
//
// Nothing above this header knows which one it got, so the HLE's path policy
// (the `/app0` dump, the update folder ahead of it, the mods and plugin
// overlays, `/data`, mount points, the case-insensitive fallback) is one piece
// of code with one set of tests, and the platform difference is a vtable.
//
// The neutral open flags below are the intersection both can honour. They are
// not `_O_*` or `O_*`: `hle/fs.cpp` translates the guest's Orbis flags into
// these, and the two backends translate these into what their platform wants.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bbx::hostfs {

// --- open flags ------------------------------------------------------------
inline constexpr int kAccessMask = 0x3;
inline constexpr int kRead = 0x1;
inline constexpr int kWrite = 0x2;
inline constexpr int kCreate = 0x0100;     // create when missing
inline constexpr int kTruncate = 0x0200;   // length 0 on open
inline constexpr int kExclusive = 0x0400;  // with kCreate: fail when it exists
inline constexpr int kAppend = 0x0800;     // every write goes to the end
inline constexpr int kDirectory = 0x1000;  // must be a directory

// Seek origins, the same numbers the guest uses.
inline constexpr int kSeekSet = 0;
inline constexpr int kSeekCur = 1;
inline constexpr int kSeekEnd = 2;

struct FileStat {
    std::uint64_t size = 0;
    bool is_dir = false;
    std::int64_t mtime = 0;  // seconds since the epoch, 0 when unknown
};

struct DirEntry {
    std::string name;  // the child's name, never a path
    bool is_dir = false;
    std::uint64_t size = 0;
};

// Everything a file system here has to be able to do. Each entry is
// implemented by both backends; a null one is a bug in a backend, and the
// wrappers below treat it as a failure rather than calling through it.
struct Ops {
    const char* name = "none";

    // Fills `out`; false when the path does not exist or cannot be read.
    bool (*stat)(const char* path, FileStat* out) = nullptr;
    // One directory level. `kCreateParents` creates missing parents.
    bool (*mk_dir)(const char* path, bool create_parents) = nullptr;
    // One directory level, names only; false when unreadable or not a
    // directory. Entries are the children, "." and ".." excluded.
    bool (*list_dir)(const char* path, std::vector<DirEntry>* out) = nullptr;

    // A handle, or -1 on failure. Handles are the backend's own numbering;
    // only the wrappers below see them.
    int (*open)(const char* path, int flags, int mode) = nullptr;
    std::int64_t (*read)(int handle, void* buffer, std::uint64_t count) = nullptr;
    std::int64_t (*write)(int handle, const void* buffer, std::uint64_t count) = nullptr;
    std::int64_t (*seek)(int handle, std::int64_t offset, int whence) = nullptr;
    bool (*truncate)(int handle, std::int64_t length) = nullptr;
    // Pushes the file's bytes to the device. A save that is flushed before its
    // rename is the difference between losing a save and corrupting one.
    bool (*flush)(int handle) = nullptr;
    bool (*close)(int handle) = nullptr;

    bool (*unlink)(const char* path) = nullptr;
    bool (*rm_dir)(const char* path) = nullptr;
    // `replace`: an existing destination is replaced, atomically where the
    // platform allows it (a save commit is exactly this call).
    bool (*rename)(const char* from, const char* to, bool replace) = nullptr;

    // Absolute, slash-separated, no trailing slash; false when the path does
    // not resolve. Used to canonicalise the roots once, never per file.
    bool (*canonical)(const char* path, std::string* out) = nullptr;

    // The errno value of the last failure on this thread, for the guest's
    // error codes.
    int (*last_error)() = nullptr;
};

// The backend's table. Defined in exactly one translation unit per build.
[[nodiscard]] const Ops& Table();

// --- wrappers --------------------------------------------------------------
// Thin, so a call site reads like the CRT call it replaced.
[[nodiscard]] bool Stat(const std::string& path, FileStat* out);
[[nodiscard]] bool Exists(const std::string& path);
[[nodiscard]] bool IsDir(const std::string& path);
[[nodiscard]] bool MkDir(const std::string& path, bool create_parents = false);
[[nodiscard]] bool ListDir(const std::string& path, std::vector<DirEntry>* out);
[[nodiscard]] int Open(const std::string& path, int flags, int mode = 0666);
[[nodiscard]] std::int64_t Read(int handle, void* buffer, std::uint64_t count);
[[nodiscard]] std::int64_t Write(int handle, const void* buffer, std::uint64_t count);
[[nodiscard]] std::int64_t Seek(int handle, std::int64_t offset, int whence);
[[nodiscard]] bool Truncate(int handle, std::int64_t length);
[[nodiscard]] bool Flush(int handle);
[[nodiscard]] bool Close(int handle);
[[nodiscard]] bool Unlink(const std::string& path);
[[nodiscard]] bool RmDir(const std::string& path);
[[nodiscard]] bool Rename(const std::string& from, const std::string& to, bool replace);
[[nodiscard]] bool Canonical(const std::string& path, std::string* out);
[[nodiscard]] int LastError();

// --- path habits -----------------------------------------------------------
// The dump's paths arrive as `/app0/...` with forward slashes and the host's
// with backslashes on Windows; both backends accept either, and these keep the
// joining and the canonical form in one place.
[[nodiscard]] std::string JoinPath(std::string dir, const std::string& name);
[[nodiscard]] std::string Slashify(std::string path);
[[nodiscard]] std::string DirName(const std::string& path);
[[nodiscard]] bool EndsWithSlash(const std::string& path);

// --- the seam's own test --------------------------------------------------
// Runs a handful of operations against `dir` - create a directory, write a
// file, read it back, list the directory, rename it, delete it - and returns
// true only when all of them worked. The console calls it once at boot, so a
// file system the AppContainer refuses is one logged line with the operation
// that failed, not a game that hangs at its first save. `why` names the step.
[[nodiscard]] bool SelfTest(const std::string& dir, std::string* why);

}  // namespace bbx::hostfs
