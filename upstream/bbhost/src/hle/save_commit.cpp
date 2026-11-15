// A save written whole or not at all: see hle/save_commit.h.
#include "hle/save_commit.h"

// The file system itself: the CRT on a desktop build, `*FromApp` in the
// AppContainer (xbox/platform/host_fs.h). `std::filesystem` is not used: it
// reaches `CreateFileW` underneath, which an AppContainer is refused on a path
// it reached through a `FutureAccessList` token.
#include "xbox/platform/host_fs.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace save_commit {
namespace {

namespace hostfs = bbx::hostfs;

std::atomic<Hook> g_hook{nullptr};
std::atomic<bool> g_no_exchange{false};
std::atomic<bool> g_exchange_refused{false};
std::atomic<bool> g_old_renames{false};
std::mutex g_refused_mu;
std::string g_refused_move;

// The file-by-file marker: this first line, then one line per entry of the
// stage - a file's path in the save, or a folder's with a '/' after it - and
// last "end <entries> <FNV-1a of all above>": a marker that does not end so
// is not one. The first line reads to the recovery of v0.2.32-v0.2.34 as an
// exchange's marker naming neither directory: it keeps the save and moves the
// stage aside, where any other marker would make the stage - by then holding
// only the files not yet moved - the save, and remove the rest.
constexpr char kFilesHead[] = "exchange 0 files\n";
// A file a recovery copies back into the stage, until it is renamed into place.
constexpr char kCopyTail[] = ".bbhost-copy";

void step(const char* name) {
    if (const Hook h = g_hook.load(std::memory_order_relaxed)) h(name);
}

bool exists(const std::string& p) { return hostfs::Exists(p); }

std::string parent_of(const std::string& p) { return hostfs::DirName(p); }

// A directory tree, walked depth first: a directory before its entries.
struct TreeEntry {
    std::string path;  // the host path
    std::string rel;   // relative to the walk's root, '/'-separated
    bool is_dir = false;
    bool is_regular = false;
};

// Calls `visit` for every entry under `root`; the visitor may set `recurse` to
// false to skip a directory's children. False when a directory could not be
// listed (the walk is then incomplete, and every caller treats that as the
// operation failing halfway).
bool walk_tree(const std::string& root, const std::function<bool(const TreeEntry&, bool*)>& visit) {
    std::function<bool(const std::string&, const std::string&)> descend =
        [&](const std::string& dir, const std::string& rel) -> bool {
        std::vector<hostfs::DirEntry> entries;
        if (!hostfs::ListDir(dir, &entries)) return false;
        for (const hostfs::DirEntry& entry : entries) {
            TreeEntry item;
            item.path = hostfs::JoinPath(dir, entry.name);
            item.rel = rel.empty() ? entry.name : rel + "/" + entry.name;
            item.is_dir = entry.is_dir;
            item.is_regular = !entry.is_dir;
            bool recurse = true;
            if (!visit(item, &recurse)) return false;
            if (item.is_dir && recurse && !descend(item.path, item.rel)) return false;
        }
        return true;
    };
    return descend(root, std::string());
}

bool make_dirs(const std::string& p) { return hostfs::MkDir(p, true); }

// A file copied whole into a new one, flushed before it is closed: the caller
// renames it into place afterwards.
bool copy_file(const std::string& from, const std::string& to) {
    const int src = hostfs::Open(from, hostfs::kRead);
    if (src < 0) return false;
    const int dst = hostfs::Open(to, hostfs::kWrite | hostfs::kCreate | hostfs::kTruncate);
    if (dst < 0) {
        (void)hostfs::Close(src);
        return false;
    }
    std::vector<char> buffer(1u << 20);
    bool ok = true;
    for (;;) {
        const std::int64_t got = hostfs::Read(src, buffer.data(), buffer.size());
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0) break;
        std::int64_t put = 0;
        while (put < got) {
            const std::int64_t n = hostfs::Write(dst, buffer.data() + put, static_cast<std::uint64_t>(got - put));
            if (n <= 0) {
                ok = false;
                break;
            }
            put += n;
        }
        if (!ok) break;
    }
    ok = hostfs::Flush(dst) && ok;
    ok = hostfs::Close(dst) && ok;
    ok = hostfs::Close(src) && ok;
    return ok;
}

bool refused_in_test(const std::string& rel) {
    std::lock_guard<std::mutex> lock(g_refused_mu);
    return !g_refused_move.empty() && g_refused_move == rel;
}

// A directory's entries on the disk (Linux: its own fsync; Windows writes a
// rename through with MOVEFILE_WRITE_THROUGH, and there is no handle to flush).
void sync_dir(const std::string& dir) {
#if !defined(_WIN32)
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)dir;
#endif
}

// Another process holding a file for a moment refuses a move or a delete: the
// seam reports it as EACCES/EBUSY and this tries again for ~5 s. The console
// has the same problem (a save scanned while it is replaced).
template <class Op>
bool retried(Op op) {
    for (int i = 0;; ++i) {
        if (op()) return true;
        const int e = hostfs::LastError();
        if (i >= 50 || (e != EACCES && e != EBUSY && e != EPERM)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool flush_file(const std::string& p) {
    const int fd = hostfs::Open(p, hostfs::kRead);
    if (fd < 0) return false;
    const bool ok = hostfs::Flush(fd);
    return hostfs::Close(fd) && ok;
}

// Every file under `dir` and every directory's entries on the disk.
bool flush_tree(const std::string& dir) {
    bool ok = true;
    const bool walked = walk_tree(dir, [&](const TreeEntry& entry, bool*) {
        if (entry.is_regular) {
            ok = flush_file(entry.path) && ok;
        } else if (entry.is_dir) {
            sync_dir(entry.path);
        }
        return true;
    });
    sync_dir(dir);
    return ok && walked;
}

// One file moved over another in one step (the target replaced, never
// missing in between). Windows: written through, a read-only target made
// writable, and tried again while another process holds either file.
bool move_file(const std::string& from, const std::string& to) {
    // One step, the target replaced: the seam's rename is MoveFileFromApp on
    // the console (and ReplaceFileFromApp where the target exists), which
    // carries the write-through with it.
    return retried([&] { return hostfs::Rename(from, to, true); });
}

void remove_file(const std::string& path) {
    retried([&] { return hostfs::Unlink(path); });
    sync_dir(parent_of(path));
}

void remove_tree(const std::string& path) {
    if (!hostfs::IsDir(path)) {
        if (exists(path)) remove_file(path);
        return;
    }
    // Every child first, then the directory itself, then once more for the
    // parent's entries on the disk.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<TreeEntry> entries;
        if (!walk_tree(path, [&](const TreeEntry& entry, bool*) {
                entries.push_back(entry);
                return true;
            })) {
            break;
        }
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->is_dir) {
                (void)hostfs::RmDir(it->path);
            } else {
                (void)hostfs::Unlink(it->path);
            }
        }
        if (hostfs::RmDir(path) || !exists(path)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // a file in it held a moment
    }
    sync_dir(parent_of(path));
}

// The marker written whole under a name of its own and flushed, then renamed
// into place: the marker in place is the old one or the new one, never a part.
bool write_marker(const Paths& p, const std::string& text) {
    int fd = -1;
    retried([&] {
        fd = hostfs::Open(p.marker_new, hostfs::kWrite | hostfs::kCreate | hostfs::kTruncate);
        return fd >= 0;
    });
    if (fd < 0) return false;
    bool ok = true;
    for (std::size_t at = 0; ok && at < text.size();) {
        const std::int64_t n = hostfs::Write(fd, text.data() + at, text.size() - at);
        if (n <= 0) ok = false;
        else at += static_cast<std::size_t>(n);
    }
    ok = ok && hostfs::Flush(fd);
    ok = hostfs::Close(fd) && ok;
    if (!ok) {
        remove_file(p.marker_new);
        return false;
    }
    step("marker-written");
    if (!move_file(p.marker_new, p.marker)) return false;
    sync_dir(parent_of(p.marker));
    return true;
}

std::string read_marker(const std::string& path) {
    std::string text;
    const int fd = hostfs::Open(path, hostfs::kRead);
    if (fd < 0) return text;
    char buf[4096];
    for (;;) {
        const std::int64_t n = hostfs::Read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        text.append(buf, static_cast<std::size_t>(n));
        if (text.size() >= (4u << 20)) break;
    }
    (void)hostfs::Close(fd);
    return text;
}

// Two directories renamed to a name nothing has (the recovery of
// v0.2.32-v0.2.34's commits). Windows: written through, and tried again for
// ~5 s while another process holds a file in it - Windows refuses a rename
// over an open file's directory.
bool rename_dir(const std::string& from, const std::string& to) {
    if (!retried([&] { return hostfs::Rename(from, to, false); })) return false;
    sync_dir(parent_of(to));
    return true;
}

// The two directories swapped in one step (renameat2 RENAME_EXCHANGE): 0, or
// the errno - EINVAL/ENOTSUP/ENOSYS where the file system or the kernel has
// no exchange (exFAT, vfat, NFS), and always on Windows.
int exchange_dirs(const std::string& a, const std::string& b) {
#if defined(_WIN32) || !defined(SYS_renameat2)
    (void)a;
    (void)b;
    return EINVAL;
#else
    if (g_no_exchange.load(std::memory_order_relaxed) || g_exchange_refused.load(std::memory_order_relaxed)) return EINVAL;
    constexpr unsigned kRenameExchange = 1u << 1;  // RENAME_EXCHANGE (linux/fs.h)
    if (::syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), kRenameExchange) != 0) return errno;
    sync_dir(parent_of(b));
    return 0;
#endif
}

std::uint64_t inode_of(const std::string& p) {
#if defined(_WIN32)
    (void)p;
    return 0;
#else
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_ino) : 0;
#endif
}

// A directory tree copied file by file (a test may end the process between two).
bool copy_tree(const std::string& from, const std::string& to) {
    if (!hostfs::MkDir(to, true)) return false;
    bool ok = true;
    const bool walked = walk_tree(from, [&](const TreeEntry& entry, bool*) {
        const std::string dst = hostfs::JoinPath(to, entry.rel);
        if (entry.is_dir) {
            ok = hostfs::MkDir(dst, true) && ok;
        } else if (entry.is_regular) {
            ok = copy_file(entry.path, dst) && ok;
            step("copy");
        }
        return ok;
    });
    return ok && walked;
}

// ---- File by file.

bool is_dir_entry(const std::string& e) { return !e.empty() && e.back() == '/'; }

bool ends_with(const std::string& s, const char* tail) {
    const std::size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// An entry's place under `dir`.
std::string in(const std::string& dir, const std::string& entry) {
    return hostfs::JoinPath(dir, is_dir_entry(entry) ? entry.substr(0, entry.size() - 1) : entry);
}

std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// A path the marker can hold, in the save: relative, never above it, no line
// break, no drive or stream (':').
bool entry_ok(const std::string& e) {
    return !e.empty() && e[0] != '/' && e != ".." && e.rfind("../", 0) != 0 && e.find("/../") == std::string::npos &&
           !ends_with(e, "/..") && e.find_first_of(":\r\n") == std::string::npos;
}

// The stage's files and folders as the marker lists them, sorted; false when
// one cannot be listed.
bool list_stage(const std::string& stage, std::vector<std::string>* out) {
    out->clear();
    bool ok = true;
    const bool walked = walk_tree(stage, [&](const TreeEntry& entry, bool*) {
        if (!entry_ok(entry.rel)) {
            ok = false;
            return false;
        }
        if (entry.is_dir) out->push_back(entry.rel + "/");
        else if (entry.is_regular && !ends_with(entry.rel, kCopyTail)) out->push_back(entry.rel);
        return true;
    });
    if (!ok || !walked) return false;
    std::sort(out->begin(), out->end());
    return true;
}

std::string files_marker(const std::vector<std::string>& entries) {
    std::string text = kFilesHead;
    for (const std::string& e : entries) text += e + "\n";
    char end[64];
    std::snprintf(end, sizeof(end), "end %zu %016llx\n", entries.size(), static_cast<unsigned long long>(fnv1a(text)));
    return text + end;
}

// A file-by-file marker's entries; false when `text` is not one, whole.
bool parse_files(const std::string& text, std::vector<std::string>* entries) {
    const std::size_t head = sizeof(kFilesHead) - 1;
    if (text.size() < head + 2 || text.compare(0, head, kFilesHead) != 0 || text.back() != '\n') return false;
    const std::size_t cut = text.rfind('\n', text.size() - 2) + 1;  // where the end line starts
    if (cut < head) return false;
    const std::string body = text.substr(0, cut);
    unsigned long long n = 0, sum = 0;
    char extra = 0;
    if (std::sscanf(text.c_str() + cut, "end %llu %llx%c", &n, &sum, &extra) != 3 || extra != '\n' || sum != fnv1a(body))
        return false;
    entries->clear();
    for (std::size_t at = head; at < body.size();) {
        const std::size_t nl = body.find('\n', at);
        const std::string e = body.substr(at, nl - at);
        if (!entry_ok(e)) return false;
        entries->push_back(e);
        at = nl + 1;
    }
    return entries->size() == n;
}

// What the save holds that the list lacks - the files and folders the
// session removed - removed: the save is the stage's set exactly.
void remove_extras(const std::string& live, const std::vector<std::string>& entries) {
    const std::set<std::string> listed(entries.begin(), entries.end());
    std::vector<std::string> files, dirs;
    (void)walk_tree(live, [&](const TreeEntry& entry, bool* recurse) {
        if (entry.is_dir) {
            if (!listed.count(entry.rel + "/")) {
                dirs.push_back(entry.path);
                *recurse = false;  // the whole subtree goes with it
            }
        } else if (!listed.count(entry.rel)) {
            files.push_back(entry.path);
        }
        return true;
    });
    for (const std::string& f : files) remove_file(f);
    for (const std::string& d : dirs) remove_tree(d);
}

// The listed files moved from the stage over the save's, each in one step;
// the save's directory is never renamed or removed. A file the stage no
// longer has was moved by an earlier pass. Then what the save has that the
// list lacks is removed, then the marker and the stage. Shared by a commit
// and a recovery; false (a move refused) leaves the marker, the stage and the
// files not yet moved for the next recovery.
bool finish_files(const Paths& p, const std::vector<std::string>& entries, std::string* why) {
    if (!exists(p.live)) {
        (void)make_dirs(p.live);
        sync_dir(parent_of(p.live));
    }
    std::set<std::string> touched;  // the directories whose entries changed
    std::string refused;
    for (const std::string& e : entries) {
        const std::string dst = in(p.live, e);
        if (is_dir_entry(e)) {
            if (!exists(dst)) {
                (void)make_dirs(dst);
                touched.insert(parent_of(dst));
            }
            continue;
        }
        const std::string src = in(p.stage, e);
        if (!exists(src)) continue;
        if (!exists(parent_of(dst))) (void)make_dirs(parent_of(dst));
        if (refused_in_test(e) || !move_file(src, dst)) {
            if (refused.empty()) refused = e;
            continue;
        }
        touched.insert(parent_of(src));
        touched.insert(parent_of(dst));
        step("moved");
    }
    for (const std::string& d : touched) sync_dir(d);
    if (!refused.empty()) {
        if (why) *why = "moving " + refused + " into the save";
        return false;
    }
    remove_extras(p.live, entries);
    step("extras-removed");
    remove_file(p.marker);
    step("marker-removed");
    remove_tree(p.stage);
    step("stage-removed");
    return true;
}

// The stage made whole again while a commit cannot be finished, so the save
// is read from there: the listed files an earlier pass moved are copied back
// from the save, each under a name of its own and renamed into place - a copy
// cut short is never taken for the file. False when one cannot be.
bool copy_back(const Paths& p, const std::vector<std::string>& entries) {
    std::set<std::string> touched;
    for (const std::string& e : entries) {
        const std::string s = in(p.stage, e);
        if (is_dir_entry(e)) {
            if (!make_dirs(s)) return false;
            continue;
        }
        const std::string l = in(p.live, e);
        if (exists(s) || !exists(l)) continue;  // still there; or in neither, nothing to copy
        if (!make_dirs(parent_of(s))) return false;
        const std::string tmp = s + kCopyTail;
        if (!copy_file(l, tmp) || !flush_file(tmp)) return false;
        step("copied-back");
        if (!move_file(tmp, s)) return false;
        touched.insert(parent_of(s));
    }
    for (const std::string& d : touched) sync_dir(d);
    return true;
}

bool has_files(const std::string& dir) {
    bool any = false;
    (void)walk_tree(dir, [&](const TreeEntry& entry, bool*) {
        if (!entry.is_dir) {
            any = true;
            return false;
        }
        return true;
    });
    return any;
}

// ---- v0.2.32-v0.2.34's renames, for the recovery of what they left.

// The two-rename swap's state, from the marker and which directories exist:
// the stage made the save, the old copy and the marker removed. False leaves
// what is there for the next recovery.
bool finish_by_renames(const Paths& p, std::string* why) {
    if (exists(p.stage)) {
        if (exists(p.live)) {
            if (exists(p.old)) remove_tree(p.old);  // a finished commit's leftover
            if (!rename_dir(p.live, p.old)) {
                if (why) *why = "the save -> .old";
                return false;
            }
            step("live-moved");
        }
        if (!rename_dir(p.stage, p.live)) {
            if (why) *why = "the stage -> the save";
            return false;
        }
        step("stage-moved");
    } else if (!exists(p.live) && exists(p.old)) {
        if (!rename_dir(p.old, p.live)) {  // cannot happen in this protocol: the old save back
            if (why) *why = ".old -> the save";
            return false;
        }
    }
    remove_file(p.marker);
    step("marker-removed");
    if (exists(p.old)) remove_tree(p.old);
    step("old-removed");
    return true;
}

}  // namespace

Paths paths_of(const std::string& live) {
    const std::string dir = parent_of(live);
    const std::size_t cut = live.find_last_of("/\\");
    const std::string name = cut == std::string::npos ? live : live.substr(cut + 1);
    return Paths{live, hostfs::JoinPath(dir, "." + name + ".stage"), hostfs::JoinPath(dir, "." + name + ".old"),
                 hostfs::JoinPath(dir, "." + name + ".commit"), hostfs::JoinPath(dir, "." + name + ".commit.new")};
}

std::string recover(const std::string& live, std::string* did) {
    const Paths p = paths_of(live);
    std::string note;
    if (exists(p.marker_new)) remove_file(p.marker_new);  // never put in place: nothing was moved under it
    if (exists(p.marker)) {
        const std::string m = read_marker(p.marker);
        std::vector<std::string> entries;
        if (parse_files(m, &entries)) {
            std::string why;
            if (finish_files(p, entries, &why)) {
                if (did) *did = "finished a commit";
                return live;
            }
            if (copy_back(p, entries)) {
                if (did) *did = "a commit could not be finished (" + why + "): the stage holds the newest save";
                return p.stage;
            }
            if (did) *did = "a commit could not be finished (" + why + ") and the stage could not be made whole again";
            return {};
        }
        if (m.rfind("exchange ", 0) == 0) {
            const std::uint64_t ino = std::strtoull(m.c_str() + 9, nullptr, 10);
            if (!ino) {
                // A file-by-file marker that is not whole (an exchange's names
                // an inode): nothing was moved under it - a move comes only
                // after the marker is in place, whole and flushed.
                if (exists(p.stage)) remove_tree(p.stage);
                remove_file(p.marker);
                if (did) *did = "a commit's marker was not whole: dropped the session";
                return live;
            }
            if (exists(p.live) && inode_of(p.live) == ino) {
                // Swapped: the stage's name holds the old save now (the
                // exchange), or .old does (the renames, where it had none).
                if (exists(p.stage)) remove_tree(p.stage);
                if (exists(p.old)) remove_tree(p.old);
                remove_file(p.marker);
                if (did) *did = "finished a commit (the old save removed)";
                return live;
            }
            if (!(ino && exists(p.stage) && inode_of(p.stage) == ino)) {
                // Neither side is the stage that was written: nothing is
                // dropped - the save stays, the stage is kept aside.
                if (exists(p.stage)) rename_dir(p.stage, p.stage + ".unsure");
                remove_file(p.marker);
                if (did) *did = "a commit's marker matched neither directory: the save kept, the stage moved aside";
                return live;
            }
            // Not swapped yet: the renames finish it.
        }
        std::string why;
        if (!finish_by_renames(p, &why)) {
            if (did) *did = "a commit could not be finished (" + why + "): the stage holds the newest save";
            return exists(p.stage) ? p.stage : live;
        }
        note = "finished a commit";
    } else {
        if (exists(p.stage)) {
            // A stage with no file left is a finished commit's (its marker
            // removed); one with files never committed.
            const bool files = has_files(p.stage);
            remove_tree(p.stage);
            note = files ? "dropped a session that never committed" : "removed an empty stage";
        }
        if (exists(p.old)) {
            if (exists(p.live)) {
                remove_tree(p.old);
            } else if (!rename_dir(p.old, p.live)) {
                if (did) *did = "the old save could not be put back";
                return p.old;
            }
            note += note.empty() ? "cleared an old copy" : ", cleared an old copy";
        }
    }
    if (did) *did = note;
    return live;
}

std::string begin(const std::string& live) {
    const Paths p = paths_of(live);
    if (exists(p.marker)) return {};  // a commit the recovery could not finish: its stage is the save
    if (exists(p.stage)) remove_tree(p.stage);
    const bool ok = exists(p.live) ? copy_tree(p.live, p.stage) : make_dirs(p.stage);
    if (!ok) {
        remove_tree(p.stage);
        return {};
    }
    return p.stage;
}

bool commit(const std::string& live, std::string* why) {
    const Paths p = paths_of(live);
    if (!exists(p.stage)) {
        if (why) *why = "no stage";
        return false;
    }
    if (!flush_tree(p.stage)) {
        if (why) *why = "flushing the stage";
        return false;
    }
    step("flushed");
    const bool old = g_old_renames.load(std::memory_order_relaxed);
    if (exists(p.live)) {
        const std::uint64_t ino = inode_of(p.stage);
        if (ino && !g_no_exchange.load(std::memory_order_relaxed)) {
            if (!write_marker(p, "exchange " + std::to_string(ino) + "\n")) {
                if (why) *why = "the marker";
                return false;
            }
            step("marker");
            const int e = exchange_dirs(p.stage, p.live);
            if (e == 0) {
                step("exchanged");
                remove_tree(p.stage);  // the old save, under the stage's name now
                step("old-removed");
                remove_file(p.marker);
                step("marker-removed");
                return true;
            }
            // No exchange here (exFAT, vfat, NFS): file by file below, its
            // marker put over this one (v0.2.32-v0.2.34: the renames under it).
            if (old) return finish_by_renames(p, why);
        }
    }
    if (old) {
        if (!write_marker(p, "rename\n")) {
            if (why) *why = "the marker";
            return false;
        }
        step("marker");
        return finish_by_renames(p, why);
    }
    std::vector<std::string> entries;
    if (!list_stage(p.stage, &entries)) {
        if (why) *why = "listing the stage";
        return false;
    }
    if (!write_marker(p, files_marker(entries))) {
        if (why) *why = "the marker";
        return false;
    }
    step("marker");
    return finish_files(p, entries, why);
}

void set_test_hook(Hook hook) { g_hook.store(hook); }
void set_no_exchange(bool on) { g_no_exchange.store(on); }
void set_exchange_refused(bool on) { g_exchange_refused.store(on); }
void set_old_renames(bool on) { g_old_renames.store(on); }
void set_refused_move(const std::string& rel) {
    std::lock_guard<std::mutex> lock(g_refused_mu);
    g_refused_move = rel;
}

}  // namespace save_commit
