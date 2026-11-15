// SPDX-License-Identifier: GPL-3.0-or-later
// The host file-system seam: the desktop/POSIX table, the wrappers over it, and
// the boot-time self test. The AppContainer table (`host_fs_uwp.cpp`) is the
// other half of the same contract and is compiled by the console job; what can
// be checked here is that the contract is complete, that the desktop
// implementation keeps the semantics the HLE relies on (a rename that replaces,
// a size that is the size, a directory listing that is one level), and that
// `SelfTest` fails loudly instead of quietly when the file system refuses.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "harness.h"

#include "xbox/platform/host_fs.h"

namespace {

namespace fsx = bbx::hostfs;

// A directory of its own under the system's temp folder, removed afterwards.
class ScratchDir {
public:
    explicit ScratchDir(const char* tag) {
        const char* base = std::getenv("TMPDIR");
        if (base == nullptr || base[0] == '\0') base = "/tmp";
        // Unique per call, without a process id: the tests run in one process
        // and two scratch directories must never be the same directory.
        static unsigned counter = 0;
        const unsigned stamp = static_cast<unsigned>(std::time(nullptr));
        char buffer[80];
        std::snprintf(buffer, sizeof(buffer), "bbx_hostfs_%s_%u_%u", tag, stamp, ++counter);
        path_ = fsx::JoinPath(base, buffer);
        (void)fsx::MkDir(path_, true);
    }
    ~ScratchDir() {
        // Bottom up: the tests make subdirectories.
        std::vector<fsx::DirEntry> entries;
        if (fsx::ListDir(path_, &entries)) {
            for (const fsx::DirEntry& entry : entries) {
                const std::string child = fsx::JoinPath(path_, entry.name);
                if (entry.is_dir) {
                    std::vector<fsx::DirEntry> inner;
                    if (fsx::ListDir(child, &inner)) {
                        for (const fsx::DirEntry& i : inner) (void)fsx::Unlink(fsx::JoinPath(child, i.name));
                    }
                    (void)fsx::RmDir(child);
                } else {
                    (void)fsx::Unlink(child);
                }
            }
        }
        (void)fsx::RmDir(path_);
    }
    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] std::string child(const char* name) const { return fsx::JoinPath(path_, name); }

private:
    std::string path_;
};

bool WriteAll(const std::string& path, const std::string& text) {
    const int handle = fsx::Open(path, fsx::kWrite | fsx::kCreate | fsx::kTruncate);
    if (handle < 0) return false;
    const bool ok = fsx::Write(handle, text.data(), text.size()) == static_cast<std::int64_t>(text.size());
    return fsx::Close(handle) && ok;
}

std::string ReadAll(const std::string& path) {
    std::string out;
    const int handle = fsx::Open(path, fsx::kRead);
    if (handle < 0) return out;
    char buffer[512];
    for (;;) {
        const std::int64_t got = fsx::Read(handle, buffer, sizeof(buffer));
        if (got <= 0) break;
        out.append(buffer, static_cast<std::size_t>(got));
    }
    const bool closed = fsx::Close(handle);
    return closed ? out : std::string();
}

}  // namespace

TEST(HostFsPathHelpersKeepTheGuestAndHostFormsApart) {
    CHECK(fsx::JoinPath("/app0", "sce_sys/param.sfo") == "/app0/sce_sys/param.sfo");
    CHECK(fsx::JoinPath("/app0/", "/movie/") == "/app0/movie");
    CHECK(fsx::JoinPath("", "eboot.bin") == "eboot.bin");
    CHECK(fsx::JoinPath("/app0", "") == "/app0");

    CHECK(fsx::Slashify("C:\\Users\\x\\dump") == "C:/Users/x/dump");
    CHECK(fsx::Slashify("/app0\\sce_sys") == "/app0/sce_sys");

    CHECK(fsx::DirName("/app0/sce_sys/param.sfo") == "/app0/sce_sys");
    CHECK(fsx::DirName("/eboot.bin") == "/");
    CHECK(fsx::DirName("eboot.bin") == ".");

    CHECK(fsx::EndsWithSlash("/app0/"));
    CHECK(!fsx::EndsWithSlash("/app0"));
}

TEST(HostFsSeamSelfTestPassesOnAWorkingFileSystem) {
    ScratchDir scratch("selftest");
    std::string why;
    const bool ok = fsx::SelfTest(scratch.child("probe"), &why);
    if (!ok) std::printf("    self test said: %s\n", why.c_str());
    CHECK(ok);
    CHECK(why.empty());
    // The test cleans up after itself: a leftover probe is a bug in the seam,
    // and would silently grow LocalState on every boot.
    CHECK(!fsx::Exists(scratch.child("probe").append("/hostfs_probe.bin")));
    CHECK(!fsx::Exists(scratch.child("probe").append("/hostfs_probe.tmp")));
}

TEST(HostFsSeamSelfTestSaysWhatFailed) {
    // A path under a *file* cannot be a directory, so the first step fails and
    // the reason names it.
    ScratchDir scratch("refused");
    const std::string file = scratch.child("not-a-dir");
    CHECK(WriteAll(file, "x"));
    std::string why;
    CHECK(!fsx::SelfTest(fsx::JoinPath(file, "probe"), &why));
    CHECK(why.find("mkdir") != std::string::npos);

    std::string empty;
    CHECK(!fsx::SelfTest("", &empty));
    CHECK(empty.find("no directory") != std::string::npos);
}

TEST(HostFsSeamRoundTripsAFileAndReportsItsSize) {
    ScratchDir scratch("roundtrip");
    const std::string path = scratch.child("save.dat");
    CHECK(WriteAll(path, "0123456789"));

    fsx::FileStat st;
    CHECK(fsx::Stat(path, &st));
    CHECK(!st.is_dir);
    CHECK(st.size == 10);

    CHECK(ReadAll(path) == "0123456789");

    // Re-open for write without truncation: the old bytes are still there, and
    // an append lands after them.
    const int handle = fsx::Open(path, fsx::kWrite | fsx::kAppend);
    CHECK(handle >= 0);
    CHECK(fsx::Write(handle, "ab", 2) == 2);
    CHECK(fsx::Flush(handle));
    CHECK(fsx::Close(handle));
    CHECK(ReadAll(path) == "0123456789ab");
}

TEST(HostFsSeamMissingPathIsAFailureWithAnError) {
    ScratchDir scratch("missing");
    fsx::FileStat st;
    CHECK(!fsx::Stat(scratch.child("nothing-here"), &st));
    CHECK(fsx::LastError() != 0);
    CHECK(!fsx::Exists(scratch.child("nothing-here")));
    CHECK(!fsx::IsDir(scratch.child("nothing-here")));
    CHECK(fsx::Open(scratch.child("nothing-here"), fsx::kRead) < 0);

    // Closing twice is a refusal, not a second close of whatever reused the
    // number: the HLE's file table can drop a handle and still reach a stale one.
    const std::string file = scratch.child("closed-twice.bin");
    CHECK(WriteAll(file, "x"));
    const int handle = fsx::Open(file, fsx::kRead);
    CHECK(handle >= 0);
    CHECK(fsx::Close(handle));
    CHECK(!fsx::Close(handle));
    CHECK(fsx::LastError() != 0);
}

TEST(HostFsSeamListsOneDirectoryLevel) {
    ScratchDir scratch("list");
    CHECK(WriteAll(scratch.child("a.bin"), "abc"));
    CHECK(WriteAll(scratch.child("b.bin"), "0123456"));
    CHECK(fsx::MkDir(scratch.child("nested"), false));
    CHECK(WriteAll(fsx::JoinPath(scratch.child("nested"), "deep.bin"), "x"));

    std::vector<fsx::DirEntry> entries;
    CHECK(fsx::ListDir(scratch.path(), &entries));
    CHECK(entries.size() == 3);  // not the file inside nested
    bool saw_dir = false, saw_a = false, saw_b = false;
    for (const fsx::DirEntry& entry : entries) {
        CHECK(entry.name != "." && entry.name != "..");
        CHECK(entry.name.find('/') == std::string::npos);
        if (entry.name == "nested") {
            saw_dir = entry.is_dir;
        } else if (entry.name == "a.bin") {
            saw_a = !entry.is_dir && entry.size == 3;
        } else if (entry.name == "b.bin") {
            saw_b = !entry.is_dir && entry.size == 7;
        }
    }
    CHECK(saw_dir);
    CHECK(saw_a);
    CHECK(saw_b);

    std::vector<fsx::DirEntry> none;
    CHECK(!fsx::ListDir(scratch.child("nested/deep.bin"), &none));  // a file is not a directory
}

TEST(HostFsSeamSeekTruncateAndReplaceRename) {
    ScratchDir scratch("seeks");
    const std::string path = scratch.child("log.bin");
    CHECK(WriteAll(path, "abcdefghij"));

    const int handle = fsx::Open(path, fsx::kRead | fsx::kWrite);
    CHECK(handle >= 0);
    CHECK(fsx::Seek(handle, 4, fsx::kSeekSet) == 4);
    CHECK(fsx::Write(handle, "XY", 2) == 2);
    CHECK(fsx::Seek(handle, -2, fsx::kSeekEnd) == 8);
    CHECK(fsx::Truncate(handle, 8));
    CHECK(fsx::Close(handle));
    CHECK(ReadAll(path) == "abcdXYgh");

    // Rename with replace, which is the save commit: the destination exists and
    // is replaced whole, never merged.
    const std::string target = scratch.child("save0.sav");
    CHECK(WriteAll(target, "old"));
    CHECK(fsx::Rename(path, target, true));
    CHECK(!fsx::Exists(path));
    CHECK(ReadAll(target) == "abcdXYgh");

    // Without replace, an existing destination is refused.
    CHECK(WriteAll(path, "new"));
    CHECK(!fsx::Rename(path, target, false));
    CHECK(ReadAll(target) == "abcdXYgh");
    CHECK(fsx::Exists(path));
}

TEST(HostFsSeamMakesDirectoriesThatAreAlreadyThere) {
    ScratchDir scratch("mkdir");
    const std::string deep = fsx::JoinPath(scratch.path(), "a/b/c");
    CHECK(fsx::MkDir(deep, true));
    CHECK(fsx::IsDir(deep));
    CHECK(fsx::MkDir(deep, true));  // idempotent: set_roots does this every boot
    CHECK(fsx::MkDir(deep, false));

    CHECK(fsx::RmDir(deep));
    CHECK(!fsx::Exists(deep));
    CHECK(fsx::IsDir(fsx::JoinPath(scratch.path(), "a/b")));
}

TEST(HostFsSeamCanonicalisesAnExistingPath) {
    ScratchDir scratch("canon");
    std::string out;
    CHECK(fsx::Canonical(scratch.path(), &out));
    CHECK(!out.empty());
    CHECK(out.back() != '/');
    CHECK(out.find('\\') == std::string::npos);

    std::string missing;
    CHECK(!fsx::Canonical(scratch.child("nowhere"), &missing));
    CHECK(missing.empty());
}

TEST(HostFsSeamNamesItsBackend) {
    // The console's log line and the crash report both name the table, so a
    // build that linked the wrong one is visible instead of subtle.
    CHECK(std::strcmp(fsx::Table().name, "none") != 0);
    CHECK(fsx::Table().stat != nullptr);
    CHECK(fsx::Table().open != nullptr);
    CHECK(fsx::Table().list_dir != nullptr);
    CHECK(fsx::Table().rename != nullptr);
    CHECK(fsx::Table().canonical != nullptr);
    CHECK(fsx::Table().last_error != nullptr);
}
