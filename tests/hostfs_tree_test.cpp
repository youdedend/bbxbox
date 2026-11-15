// SPDX-License-Identifier: GPL-3.0-or-later
// The tree helpers `hle/system.cpp` uses for save data now that its
// `std::filesystem` calls are gone (`xbox/platform/hostfs_extra.h`): a recursive
// copy for the periodic save backup, and a recursive delete for the backups that
// age out and for what a delete leaves beside a save.
//
// The same header is compiled on the console, where the table under it is
// `host_fs_uwp.cpp` (`*FromApp`); here it is the desktop table, so the copy and
// remove semantics - nesting, overwrite, an empty directory, a missing path -
// are checked on a machine with a file system the tests can inspect.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "harness.h"

#include "xbox/platform/host_fs.h"
#include "xbox/platform/hostfs_extra.h"

namespace {

namespace fsx = bbx::hostfs;

// A directory of its own under the system's temp folder, removed bottom up.
class Scratch {
public:
    explicit Scratch(const char* tag) {
        const char* base = std::getenv("TMPDIR");
        if (base == nullptr || base[0] == '\0') base = "/tmp";
        static unsigned counter = 0;
        const unsigned stamp = static_cast<unsigned>(std::time(nullptr));
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "bbx_hostfs_tree_%s_%u_%u", tag, stamp, ++counter);
        path_ = fsx::JoinPath(base, buffer);
        (void)fsx::MkDir(path_, true);
    }
    ~Scratch() { (void)fsx::RemoveAll(path_); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] std::string child(const char* name) const { return fsx::JoinPath(path_, name); }

private:
    std::string path_;
};

std::string Text(const std::string& path) {
    std::vector<std::uint8_t> bytes;
    if (!fsx::ReadFileAll(path, &bytes)) return "<unreadable>";
    return std::string(bytes.begin(), bytes.end());
}

bool Put(const std::string& path, const char* text) {
    const std::vector<std::uint8_t> bytes(text, text + std::strlen(text));
    return fsx::WriteFileAll(path, bytes);
}

// A tree two levels deep, one empty directory in it and one file per level.
bool BuildTree(const std::string& root) {
    return Put(fsx::JoinPath(root, "sce_param.bin"), "param") &&
           Put(fsx::JoinPath(root, "memory.dat"), "memory") &&
           fsx::MkDirs(fsx::JoinPath(root, "user")) &&
           Put(fsx::JoinPath(fsx::JoinPath(root, "user"), "profile.bin"), "profile") &&
           fsx::MkDirs(fsx::JoinPath(fsx::JoinPath(root, "user"), "empty")) &&
           fsx::MkDirs(fsx::JoinPath(fsx::JoinPath(fsx::JoinPath(root, "user"), "empty"), "deeper")) &&
           Put(fsx::JoinPath(fsx::JoinPath(fsx::JoinPath(root, "user"), "empty"), "note.txt"), "note");
}

}  // namespace

// The save backup's copy: every file, every level, an empty directory kept as a
// directory, and a file that is already there replaced by the copy.
TEST(HostFsCopyTreeCopiesEveryLevelAndReplaces) {
    Scratch scratch("copy");
    const std::string from = scratch.child("save");
    CHECK(fsx::MkDirs(from));
    CHECK(BuildTree(from));

    // A stale file in the destination, of the wrong length: the copy replaces it
    // rather than appending or leaving it.
    const std::string to = scratch.child("backup");
    CHECK(fsx::MkDirs(fsx::JoinPath(to, "user")));
    CHECK(Put(fsx::JoinPath(to, "memory.dat"), "stale-and-longer-than-the-original"));

    CHECK(fsx::CopyTree(from, to));

    CHECK(Text(fsx::JoinPath(to, "sce_param.bin")) == "param");
    CHECK(Text(fsx::JoinPath(to, "memory.dat")) == "memory");
    CHECK(Text(fsx::JoinPath(fsx::JoinPath(to, "user"), "profile.bin")) == "profile");
    CHECK(Text(fsx::JoinPath(fsx::JoinPath(fsx::JoinPath(to, "user"), "empty"), "note.txt")) == "note");
    CHECK(fsx::IsDir(fsx::JoinPath(fsx::JoinPath(to, "user"), "empty")));
    CHECK(fsx::IsDir(fsx::JoinPath(fsx::JoinPath(fsx::JoinPath(to, "user"), "empty"), "deeper")));

    fsx::FileStat st{};
    CHECK(fsx::Stat(fsx::JoinPath(to, "memory.dat"), &st));
    CHECK(!st.is_dir);
    CHECK(st.size == 6);  // "memory"
}

// `remove_all`: children before the directory, however deep, and a path that is
// already gone is not a failure (the backup pruning relies on that).
TEST(HostFsRemoveAllEmptiesNestedTreesAndForgivesMissingPaths) {
    Scratch scratch("remove");
    const std::string tree = scratch.child("tree");
    CHECK(fsx::MkDirs(tree));
    CHECK(BuildTree(tree));

    CHECK(fsx::RemoveAll(tree));
    CHECK(!fsx::Exists(tree));

    // Gone is gone: a second call is a success, not an error.
    CHECK(fsx::RemoveAll(tree));

    // A single file, by the same entry point.
    const std::string file = scratch.child("single.bin");
    CHECK(Put(file, "x"));
    CHECK(fsx::RemoveAll(file));
    CHECK(!fsx::Exists(file));
}

// `WriteFileAll` creates the parents, and `ReadFileAll` gives back exactly the
// bytes written - the two halves the engine's asset writers and the GX
// container reader depend on.
TEST(HostFsFileAllRoundTripsThroughMissingParents) {
    Scratch scratch("all");
    const std::string path = fsx::JoinPath(scratch.path(), "bbhost");
    const std::string file = fsx::JoinPath(path, "world-assets");
    const std::string layout = fsx::JoinPath(file, "m21_00_00_00.msb.dcx");

    // Neither folder exists yet: WriteFileAll makes them.
    CHECK(Put(layout, "layout-bytes"));
    CHECK(fsx::IsDir(path));
    CHECK(fsx::IsDir(file));
    CHECK(Text(layout) == "layout-bytes");

    // A read of a directory is a failure, not an empty file.
    std::vector<std::uint8_t> into;
    CHECK(!fsx::ReadFileAll(file, &into));

    // And a write replaces, not appends.
    CHECK(Put(layout, "second"));
    CHECK(Text(layout) == "second");
}
