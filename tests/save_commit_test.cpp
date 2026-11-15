// SPDX-License-Identifier: GPL-3.0-or-later
// The save commit protocol: the promise is that a process killed at any point
// leaves either the old save or the new one, whole. bbhost's own tests broke
// the process at each step; these check the protocol itself over the seam the
// console routes it through (`xbox/platform/host_fs.h`), which is what changed:
// `std::filesystem`, `CreateFileW` and `MoveFileExW` are gone from
// `hle/save_commit.cpp`, so the moves, the listing and the flush are now the
// seam's and need a test of their own.
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "harness.h"

#include "hle/save_commit.h"
#include "xbox/platform/host_fs.h"

namespace {

namespace fsx = bbx::hostfs;

class ScratchDir {
public:
    explicit ScratchDir(const char* tag) {
        const char* base = std::getenv("TMPDIR");
        if (base == nullptr || base[0] == '\0') base = "/tmp";
        static unsigned counter = 0;
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "bbx_save_%s_%u_%u", tag,
                      static_cast<unsigned>(std::time(nullptr)), ++counter);
        root_ = fsx::JoinPath(base, buffer);
        (void)fsx::MkDir(root_, true);
    }
    ~ScratchDir() { RemoveTree(root_); }
    [[nodiscard]] const std::string& path() const { return root_; }

    static void RemoveTree(const std::string& root) {
        std::vector<fsx::DirEntry> entries;
        if (!fsx::ListDir(root, &entries)) return;
        for (const fsx::DirEntry& entry : entries) {
            const std::string child = fsx::JoinPath(root, entry.name);
            if (entry.is_dir) {
                RemoveTree(child);
                (void)fsx::RmDir(child);
            } else {
                (void)fsx::Unlink(child);
            }
        }
    }

private:
    std::string root_;
};

bool Write(const std::string& path, const std::string& text) {
    (void)fsx::MkDir(fsx::DirName(path), true);
    const int fd = fsx::Open(path, fsx::kWrite | fsx::kCreate | fsx::kTruncate);
    if (fd < 0) return false;
    const bool ok = fsx::Write(fd, text.data(), text.size()) == static_cast<std::int64_t>(text.size());
    return fsx::Close(fd) && ok;
}

std::string Read(const std::string& path) {
    std::string out;
    const int fd = fsx::Open(path, fsx::kRead);
    if (fd < 0) return out;
    char buffer[256];
    for (;;) {
        const std::int64_t got = fsx::Read(fd, buffer, sizeof(buffer));
        if (got <= 0) break;
        out.append(buffer, static_cast<std::size_t>(got));
    }
    const bool closed = fsx::Close(fd);
    return closed ? out : std::string();
}

std::vector<std::string> Names(const std::string& dir) {
    std::vector<std::string> names;
    std::vector<fsx::DirEntry> entries;
    if (!fsx::ListDir(dir, &entries)) return names;
    for (const fsx::DirEntry& entry : entries) names.push_back(entry.name);
    for (std::size_t i = 0; i < names.size(); ++i) {  // stable across file systems
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            if (names[j] < names[i]) {
                const std::string tmp = names[i];
                names[i] = names[j];
                names[j] = tmp;
            }
        }
    }
    return names;
}

bool Has(const std::vector<std::string>& names, const char* wanted) {
    for (const std::string& name : names) {
        if (name == wanted) return true;
    }
    return false;
}

void Reset() {
    save_commit::set_test_hook(nullptr);
    save_commit::set_no_exchange(false);
    save_commit::set_exchange_refused(false);
    save_commit::set_old_renames(false);
    save_commit::set_refused_move("");
}

}  // namespace

TEST(SaveCommitNamesTheFilesBesideTheSave) {
    const save_commit::Paths p = save_commit::paths_of("/data/saves/CUSA03173");
    CHECK(p.live == "/data/saves/CUSA03173");
    CHECK(p.stage == "/data/saves/.CUSA03173.stage");
    CHECK(p.old == "/data/saves/.CUSA03173.old");
    CHECK(p.marker == "/data/saves/.CUSA03173.commit");
    CHECK(p.marker_new == "/data/saves/.CUSA03173.commit.new");

    // A Windows-shaped path keeps its separator in the parent and the name out
    // of it, which is what the seam's DirName is for.
    const save_commit::Paths q = save_commit::paths_of("C:/LocalState/data/saves/slot1");
    CHECK(q.stage == "C:/LocalState/data/saves/.slot1.stage");
}

TEST(SaveCommitMakesTheStageTheSave) {
    Reset();
    ScratchDir scratch("commit");
    const std::string live = fsx::JoinPath(scratch.path(), "slot0");
    CHECK(Write(fsx::JoinPath(live, "user.dat"), "old-user"));
    CHECK(Write(fsx::JoinPath(live, "nested/deep.dat"), "old-deep"));

    const std::string stage = save_commit::begin(live);
    CHECK(!stage.empty());
    CHECK(stage == save_commit::paths_of(live).stage);
    CHECK(Read(fsx::JoinPath(stage, "user.dat")) == "old-user");  // the stage starts as a copy

    CHECK(Write(fsx::JoinPath(stage, "user.dat"), "new-user"));
    CHECK(Write(fsx::JoinPath(stage, "added.dat"), "added"));

    // The exchange is a Linux file system's; asking for the file-by-file commit
    // is what the console does, so both are exercised.
    save_commit::set_no_exchange(true);
    std::string why;
    const bool ok = save_commit::commit(live, &why);
    if (!ok) std::printf("    commit said: %s\n", why.c_str());
    CHECK(ok);
    CHECK(Read(fsx::JoinPath(live, "user.dat")) == "new-user");
    CHECK(Read(fsx::JoinPath(live, "added.dat")) == "added");
    CHECK(Read(fsx::JoinPath(live, "nested/deep.dat")) == "old-deep");
    CHECK(!fsx::Exists(stage));

    // The marker is gone too: the next mount must not try to recover anything.
    const save_commit::Paths p = save_commit::paths_of(live);
    CHECK(!fsx::Exists(p.marker));
    CHECK(!fsx::Exists(p.marker_new));
    CHECK(!fsx::Exists(p.old));
}

TEST(SaveCommitRemovesWhatTheSessionDeleted) {
    Reset();
    ScratchDir scratch("deleted");
    const std::string live = fsx::JoinPath(scratch.path(), "slot1");
    CHECK(Write(fsx::JoinPath(live, "keep.dat"), "keep"));
    CHECK(Write(fsx::JoinPath(live, "gone.dat"), "gone"));
    CHECK(Write(fsx::JoinPath(live, "olddir/gone.dat"), "gone-deep"));

    const std::string stage = save_commit::begin(live);
    CHECK(!stage.empty());
    // The game deleted both while the session was open.
    CHECK(fsx::Unlink(fsx::JoinPath(stage, "gone.dat")));
    ScratchDir::RemoveTree(fsx::JoinPath(stage, "olddir"));
    CHECK(fsx::RmDir(fsx::JoinPath(stage, "olddir")));

    save_commit::set_no_exchange(true);
    CHECK(save_commit::commit(live, nullptr));
    const std::vector<std::string> names = Names(live);
    CHECK(Has(names, "keep.dat"));
    CHECK(!Has(names, "gone.dat"));
    CHECK(!Has(names, "olddir"));  // the save is the stage's set exactly
}

TEST(SaveCommitExchangeSwapsTheTwoDirectoriesWhole) {
    Reset();
    ScratchDir scratch("exchange");
    const std::string live = fsx::JoinPath(scratch.path(), "slot2");
    CHECK(Write(fsx::JoinPath(live, "a.dat"), "a1"));

    const std::string stage = save_commit::begin(live);
    CHECK(!stage.empty());
    CHECK(Write(fsx::JoinPath(stage, "a.dat"), "a2"));
    CHECK(Write(fsx::JoinPath(stage, "b.dat"), "b2"));

    // Where the file system has an exchange the two directories are swapped in
    // one step; where it does not (the Windows path) the commit is file by
    // file. Both must land on the same save.
    const bool committed = save_commit::commit(live, nullptr);
    CHECK(committed);
    CHECK(Read(fsx::JoinPath(live, "a.dat")) == "a2");
    CHECK(Read(fsx::JoinPath(live, "b.dat")) == "b2");
    CHECK(!fsx::Exists(stage));
    CHECK(!fsx::Exists(save_commit::paths_of(live).marker));
}

TEST(SaveCommitRecoveryDropsAStageThatNeverCommitted) {
    Reset();
    ScratchDir scratch("recover");
    const std::string live = fsx::JoinPath(scratch.path(), "slot3");
    CHECK(Write(fsx::JoinPath(live, "a.dat"), "live"));

    const std::string stage = save_commit::begin(live);
    CHECK(!stage.empty());
    CHECK(Write(fsx::JoinPath(stage, "uncommitted.dat"), "never"));

    // No marker: nothing was moved under it, so the session is dropped and the
    // save is untouched.
    std::string did;
    const std::string newest = save_commit::recover(live, &did);
    CHECK(newest == live);
    CHECK(did.find("dropped a session") != std::string::npos);
    CHECK(!fsx::Exists(stage));
    CHECK(Read(fsx::JoinPath(live, "a.dat")) == "live");
    CHECK(!fsx::Exists(fsx::JoinPath(live, "uncommitted.dat")));

    // Nothing left to settle the second time.
    std::string again;
    CHECK(save_commit::recover(live, &again) == live);
    CHECK(again.empty());
}

TEST(SaveCommitRefusedMoveKeepsTheStageForTheNextRecovery) {
    Reset();
    ScratchDir scratch("refused");
    const std::string live = fsx::JoinPath(scratch.path(), "slot4");
    CHECK(Write(fsx::JoinPath(live, "a.dat"), "a-live"));
    CHECK(Write(fsx::JoinPath(live, "b.dat"), "b-live"));

    const std::string stage = save_commit::begin(live);
    CHECK(!stage.empty());
    CHECK(Write(fsx::JoinPath(stage, "a.dat"), "a-new"));
    CHECK(Write(fsx::JoinPath(stage, "b.dat"), "b-new"));

    // Another process holds b.dat's target for the whole commit: `a.dat` moves,
    // `b.dat` does not, and the marker stays so a recovery can finish it.
    save_commit::set_no_exchange(true);
    save_commit::set_refused_move("b.dat");
    std::string why;
    CHECK(!save_commit::commit(live, &why));
    CHECK(why.find("b.dat") != std::string::npos);
    const save_commit::Paths p = save_commit::paths_of(live);
    CHECK(fsx::Exists(p.marker));
    CHECK(fsx::Exists(stage));

    // The recovery cannot finish either, so it makes the stage whole again (the
    // moved file is copied back) and the save is read from there: the newest
    // save is the stage.
    std::string did;
    const std::string newest = save_commit::recover(live, &did);
    CHECK(newest == stage);
    CHECK(did.find("the stage holds the newest save") != std::string::npos);
    CHECK(Read(fsx::JoinPath(stage, "a.dat")) == "a-new");
    CHECK(Read(fsx::JoinPath(stage, "b.dat")) == "b-new");

    // Once the file is released, the next recovery finishes the commit and the
    // stage's content is the live save's.
    save_commit::set_refused_move("");
    std::string finished;
    CHECK(save_commit::recover(live, &finished) == live);
    CHECK(Read(fsx::JoinPath(live, "a.dat")) == "a-new");
    CHECK(Read(fsx::JoinPath(live, "b.dat")) == "b-new");
    CHECK(!fsx::Exists(stage));
    CHECK(!fsx::Exists(p.marker));
}

TEST(SaveCommitBeginOnANewSaveMakesAnEmptyStage) {
    Reset();
    ScratchDir scratch("new");
    const std::string live = fsx::JoinPath(scratch.path(), "slot5");
    CHECK(!fsx::Exists(live));

    const std::string stage = save_commit::begin(live);
    CHECK(!stage.empty());
    CHECK(fsx::IsDir(stage));
    CHECK(Names(stage).empty());
    CHECK(Write(fsx::JoinPath(stage, "first.dat"), "first"));

    save_commit::set_no_exchange(true);
    CHECK(save_commit::commit(live, nullptr));
    CHECK(Read(fsx::JoinPath(live, "first.dat")) == "first");
}
