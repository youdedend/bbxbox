// SPDX-License-Identifier: GPL-3.0-or-later
// The four things the guest's code kept reaching for `std::filesystem` to do.
//
// `std::filesystem` is the one part of the standard library that cannot be used
// in a packaged title: MSVC's implementation imports `CreateFile2`,
// `FindFirstFileW`/`ExW`, `CreateDirectoryW` and `MoveFileExW` directly, so every
// translation unit that includes the header asks the linker for the desktop
// partition's entry points - and `check-imports.ps1` refuses them, correctly: a
// granted dump arrives through a `FutureAccessList` token, and those calls are
// refused on such a path at run time even where they link.
//
// These are the replacements, written once against `bbx::hostfs` (the seam's
// wrappers, `host_fs.h`), and used by the ten call sites that used to include
// the header. They are plain C++ with no platform code in them, so the desktop
// test suite exercises the same implementation the console runs: on Linux
// through `host_fs_desktop.cpp`, on the console through `host_fs_uwp.cpp`.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xbox/platform/host_fs.h"

namespace bbx::hostfs {

// Every byte of `path`, or false (with the seam's error set) when it cannot be
// opened or read. A directory is a failure, not an empty file.
inline bool ReadFileAll(const std::string& path, std::vector<std::uint8_t>* out) {
    out->clear();
    const int handle = Open(path, kRead);
    if (handle < 0) return false;
    FileStat st{};
    if (Stat(path, &st) && st.is_dir) {
        (void)Close(handle);
        return false;
    }
    std::uint8_t buffer[64 * 1024];
    for (;;) {
        const std::int64_t n = Read(handle, buffer, sizeof(buffer));
        if (n < 0) {
            (void)Close(handle);
            out->clear();
            return false;
        }
        if (n == 0) break;
        out->insert(out->end(), buffer, buffer + n);
    }
    (void)Close(handle);
    return true;
}

// Writes `bytes` to `path`: parents created, the file truncated, every byte
// written, the buffer flushed to the device before the handle closes (a save
// that is flushed is the difference between losing one and corrupting one).
inline bool WriteFileAll(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    const std::string dir = DirName(path);
    if (!dir.empty() && !MkDir(dir, true)) {
        // Already there is not a failure; anything else is.
        if (!IsDir(dir)) return false;
    }
    const int handle = Open(path, kWrite | kCreate | kTruncate);
    if (handle < 0) return false;
    std::uint64_t written = 0;
    while (written < bytes.size()) {
        const std::int64_t n = Write(handle, bytes.data() + written, bytes.size() - written);
        if (n <= 0) {
            (void)Close(handle);
            return false;
        }
        written += static_cast<std::uint64_t>(n);
    }
    const bool flushed = Flush(handle);
    const bool closed = Close(handle);
    return flushed && closed;
}

// `remove_all`: the directory's children first, then the directory itself; a
// file is unlinked. A path that is not there is already removed, which is not a
// failure (the callers use this to clear a staging folder).
inline bool RemoveAll(const std::string& path) {
    FileStat st{};
    if (!Stat(path, &st)) return true;
    if (!st.is_dir) return Unlink(path);
    std::vector<DirEntry> entries;
    if (!ListDir(path, &entries)) return false;
    bool ok = true;
    for (const DirEntry& entry : entries) {
        const std::string child = JoinPath(path, entry.name);
        if (entry.is_dir) {
            if (!RemoveAll(child)) ok = false;
        } else if (!Unlink(child)) {
            ok = false;
        }
    }
    if (!RmDir(path)) ok = false;
    return ok;
}

// `copy(recursive | overwrite_existing)`: the tree at `from` written under `to`,
// existing files replaced. Used by the save system's backup, so it copies bytes
// rather than links or shares them.
inline bool CopyTree(const std::string& from, const std::string& to) {
    FileStat st{};
    if (!Stat(from, &st)) return false;
    if (!st.is_dir) {
        std::vector<std::uint8_t> bytes;
        if (!ReadFileAll(from, &bytes)) return false;
        return WriteFileAll(to, bytes);
    }
    if (!MkDir(to, true) && !IsDir(to)) return false;
    std::vector<DirEntry> entries;
    if (!ListDir(from, &entries)) return false;
    bool ok = true;
    for (const DirEntry& entry : entries) {
        if (!CopyTree(JoinPath(from, entry.name), JoinPath(to, entry.name))) ok = false;
    }
    return ok;
}

// `create_directories`: true when the directory is there afterwards.
inline bool MkDirs(const std::string& path) { return MkDir(path, true) || IsDir(path); }

// --- the `std::filesystem` shape, without the header ------------------------
// Three of the engine's asset writers read as `fs::path(data) / "bbhost" /
// "world-assets"` and `fs::create_directories(out)`; keeping that shape means the
// console branch of those files differs from the desktop one in one line (the
// namespace it points `fs` at) instead of in every call site. `path` here is a
// string that knows how to be joined, and the three functions are the seam's.
namespace shim {

class path {
public:
    path() = default;
    path(const char* value) : value_(value == nullptr ? std::string() : std::string(value)) {}
    path(std::string value) : value_(std::move(value)) {}
    path operator/(const path& rhs) const { return path(JoinPath(value_, rhs.value_)); }
    path operator/(const char* rhs) const { return operator/(path(rhs)); }
    [[nodiscard]] std::string string() const { return value_; }
    [[nodiscard]] const std::string& str() const { return value_; }
    [[nodiscard]] std::string parent_path() const { return DirName(value_); }
    [[nodiscard]] std::string filename() const {
        const std::string dir = DirName(value_);
        if (dir.empty()) return value_;
        return value_.size() > dir.size() + 1 ? value_.substr(dir.size() + 1) : std::string();
    }
    [[nodiscard]] bool empty() const { return value_.empty(); }
    operator std::string() const { return value_; }

private:
    std::string value_;
};

inline path operator/(const char* lhs, const path& rhs) { return path(lhs) / rhs; }

inline bool exists(const path& p) { return Exists(p.str()); }
inline bool create_directories(const path& p) { return MkDirs(p.str()); }
// Replacing, as `std::filesystem::rename` does not: every caller here stages a
// temporary and moves it over the file it replaces.
inline bool rename(const path& from, const path& to) { return Rename(from.str(), to.str(), true); }

}  // namespace shim

}  // namespace bbx::hostfs
