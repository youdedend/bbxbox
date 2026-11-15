// SPDX-License-Identifier: GPL-3.0-or-later
// host_fs.h's shared half: the wrappers, the path helpers and the boot-time
// self test. Nothing here knows the platform; the table comes from whichever
// backend the build linked (host_fs_desktop.cpp or host_fs_uwp.cpp).
#include "xbox/platform/host_fs.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace bbx::hostfs {

bool Stat(const std::string& path, FileStat* out) {
    if (Table().stat == nullptr || out == nullptr) return false;
    *out = FileStat{};
    return Table().stat(path.c_str(), out);
}

bool Exists(const std::string& path) {
    FileStat st;
    return Stat(path, &st);
}

bool IsDir(const std::string& path) {
    FileStat st;
    return Stat(path, &st) && st.is_dir;
}

bool MkDir(const std::string& path, bool create_parents) {
    if (Table().mk_dir == nullptr) return false;
    return Table().mk_dir(path.c_str(), create_parents);
}

bool ListDir(const std::string& path, std::vector<DirEntry>* out) {
    if (Table().list_dir == nullptr || out == nullptr) return false;
    out->clear();
    return Table().list_dir(path.c_str(), out);
}

int Open(const std::string& path, int flags, int mode) {
    if (Table().open == nullptr) return -1;
    return Table().open(path.c_str(), flags, mode);
}

std::int64_t Read(int handle, void* buffer, std::uint64_t count) {
    if (Table().read == nullptr) return -1;
    return Table().read(handle, buffer, count);
}

std::int64_t Write(int handle, const void* buffer, std::uint64_t count) {
    if (Table().write == nullptr) return -1;
    return Table().write(handle, buffer, count);
}

std::int64_t Seek(int handle, std::int64_t offset, int whence) {
    if (Table().seek == nullptr) return -1;
    return Table().seek(handle, offset, whence);
}

bool Truncate(int handle, std::int64_t length) {
    if (Table().truncate == nullptr) return false;
    return Table().truncate(handle, length);
}

bool Flush(int handle) {
    if (Table().flush == nullptr) return false;
    return Table().flush(handle);
}

bool Close(int handle) {
    if (Table().close == nullptr) return false;
    return Table().close(handle);
}

bool Unlink(const std::string& path) {
    if (Table().unlink == nullptr) return false;
    return Table().unlink(path.c_str());
}

bool RmDir(const std::string& path) {
    if (Table().rm_dir == nullptr) return false;
    return Table().rm_dir(path.c_str());
}

bool Rename(const std::string& from, const std::string& to, bool replace) {
    if (Table().rename == nullptr) return false;
    return Table().rename(from.c_str(), to.c_str(), replace);
}

bool Canonical(const std::string& path, std::string* out) {
    if (Table().canonical == nullptr || out == nullptr) return false;
    out->clear();
    return Table().canonical(path.c_str(), out);
}

int LastError() {
    if (Table().last_error == nullptr) return EIO;
    return Table().last_error();
}

std::string JoinPath(std::string dir, const std::string& name) {
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    std::string leaf = name;
    while (!leaf.empty() && (leaf.front() == '/' || leaf.front() == '\\')) leaf.erase(leaf.begin());
    while (!leaf.empty() && (leaf.back() == '/' || leaf.back() == '\\')) leaf.pop_back();
    if (leaf.empty()) return dir;
    if (dir.empty()) return leaf;
    return dir + '/' + leaf;
}

std::string Slashify(std::string path) {
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

std::string DirName(const std::string& path) {
    const std::string p = Slashify(path);
    const std::size_t cut = p.find_last_of('/');
    if (cut == std::string::npos) return ".";
    if (cut == 0) return "/";
    return p.substr(0, cut);
}

bool EndsWithSlash(const std::string& path) {
    return !path.empty() && (path.back() == '/' || path.back() == '\\');
}

bool SelfTest(const std::string& dir, std::string* why) {
    auto fail = [why](const char* step) {
        if (why != nullptr) {
            char text[160];
            std::snprintf(text, sizeof(text), "%s (table %s, errno %d)", step, Table().name,
                          LastError());
            *why = text;
        }
        return false;
    };
    if (dir.empty()) return fail("no directory to test in");
    if (!MkDir(dir, true)) return fail("mkdir");

    const std::string probe = JoinPath(dir, "hostfs_probe.bin");
    const std::string moved = JoinPath(dir, "hostfs_probe.tmp");
    static const char kBytes[] = "bbxbox host fs";
    const int handle = Open(probe, kWrite | kCreate | kTruncate);
    if (handle < 0) return fail("open for write");
    const bool wrote = Write(handle, kBytes, sizeof(kBytes) - 1) == std::int64_t(sizeof(kBytes) - 1);
    const bool flushed = Flush(handle);
    const bool closed = Close(handle);
    if (!wrote) return fail("write");
    if (!closed) return fail("close after write");
    if (!flushed) return fail("flush");

    FileStat st;
    if (!Stat(probe, &st)) return fail("stat");
    if (st.is_dir || st.size != sizeof(kBytes) - 1) return fail("stat says the wrong size");

    const int read_handle = Open(probe, kRead);
    if (read_handle < 0) return fail("open for read");
    char back[sizeof(kBytes)]{};
    const std::int64_t got = Read(read_handle, back, sizeof(back));
    const bool closed_read = Close(read_handle);
    if (!closed_read) return fail("close after read");
    if (got != std::int64_t(sizeof(kBytes) - 1) || std::memcmp(back, kBytes, sizeof(kBytes) - 1) != 0) {
        return fail("read back");
    }

    std::vector<DirEntry> entries;
    if (!ListDir(dir, &entries)) return fail("list");
    bool saw = false;
    for (const DirEntry& entry : entries) {
        if (entry.name == "hostfs_probe.bin") saw = true;
    }
    if (!saw) return fail("list does not show the probe");

    if (!Rename(probe, moved, true)) return fail("rename");
    if (!Exists(moved)) return fail("rename left nothing behind");
    if (!Unlink(moved)) return fail("unlink");
    if (Exists(moved)) return fail("unlink left the file behind");
    return true;
}

}  // namespace bbx::hostfs
