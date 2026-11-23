// sceFios2: file/dir handles and the few async ops the game uses, over the
// sandboxed host path mapper. Ops complete synchronously.
#include "core/write_watch.h"
#include "host/frame_stats.h"
#include "hle/common.h"
#include "hle/fs.h"
#include "hle/hle.h"
#include "hle/modules.h"
#include "hle/platform.h"

// The file system itself: the CRT on a desktop build, `*FromApp` in the
// AppContainer (xbox/platform/host_fs.h).
#include "xbox/platform/host_fs.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace hostfs = bbx::hostfs;

namespace {

// SCE_FIOS_ERROR_*
constexpr int kFiosOk = 0;
constexpr int kFiosErrBadPath = static_cast<int>(0x80C90007u);
constexpr int kFiosErrBadFh = static_cast<int>(0x80C90011u);
constexpr int kFiosErrBadDh = static_cast<int>(0x80C90012u);
constexpr int kFiosErrBadOp = static_cast<int>(0x80C90013u);
constexpr int kFiosErrAccess = static_cast<int>(0x80C90005u);
constexpr int kFiosErrExists = static_cast<int>(0x80C9000Au);
constexpr int kFiosErrBadArg = static_cast<int>(0x80C90003u);

// SceFiosOpenParams.openFlags
constexpr std::uint32_t kOpenRead = 1;
constexpr std::uint32_t kOpenWrite = 2;
constexpr std::uint32_t kOpenCreate = 0x10;
constexpr std::uint32_t kOpenTruncate = 0x20;
constexpr std::uint32_t kOpenAppend = 0x40;

struct OpenParams {
    std::uint32_t open_flags;
    std::uint32_t op_flags;
    std::uint32_t reserved[2];
    void* buf_ptr;
    std::size_t buf_len;
};

// SceFiosDate is a 64-bit count of microseconds; SceFiosStat is 0x50 bytes.
struct FiosStat {
    std::int64_t file_size;
    std::uint64_t access_date;
    std::uint64_t modification_date;
    std::uint64_t creation_date;
    std::uint32_t stat_flags;  // 1 = directory
    std::uint32_t reserved;
    std::int64_t uid;
    std::int64_t gid;
    std::int64_t dev;
    std::int64_t ino;
    std::int64_t mode;
};
static_assert(sizeof(FiosStat) == 0x50, "SceFiosStat");

struct FiosDirEntry {
    FiosStat stat;
    std::uint16_t offset_to_name;
    std::uint16_t name_length;
    std::uint16_t full_path_length;
    std::uint16_t reserved;
    char full_path[1024];
};

struct File {
    // The seam's handle, not a FILE*: a CRT stream is a CreateFileW stream
    // underneath, which the AppContainer refuses on the dump. -1 is closed.
    int fd = -1;
    std::string path;
};

struct Dir {
    std::string guest_path;
    std::vector<std::string> names;
    std::size_t pos = 0;
};

struct Op {
    int result = 0;
    std::int64_t actual = 0;
    bool done = true;
};

std::mutex g_mu;
int g_next_fh = 1;
int g_next_dh = 1;
int g_next_op = 1;
std::unordered_map<int, File> g_files;
std::unordered_map<int, Dir> g_dirs;
std::unordered_map<int, Op> g_ops;

std::uint64_t to_fios_date(std::int64_t unix_sec) {
    return unix_sec > 0 ? static_cast<std::uint64_t>(unix_sec) * 1000000ull : 0;
}

bool host_stat(const std::string& host, FiosStat* out) {
    hostfs::FileStat st{};
    if (!hostfs::Stat(host, &st)) {
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    out->file_size = st.is_dir ? 0 : static_cast<std::int64_t>(st.size);
    out->access_date = to_fios_date(st.mtime);
    out->modification_date = to_fios_date(st.mtime);
    out->creation_date = to_fios_date(st.mtime);  // the seam knows one time
    out->stat_flags = st.is_dir ? 1u : 0u;
    out->dev = 0;
    out->ino = 0;
    out->mode = st.is_dir ? 0040755 : 0100644;
    return true;
}

int make_op(int result, std::int64_t actual) {
    std::lock_guard<std::mutex> lock(g_mu);
    int id = g_next_op++;
    g_ops[id] = Op{result, actual, true};
    return id;
}

int mode_for(std::uint32_t flags) {
    const bool w = (flags & kOpenWrite) != 0;
    const bool r = (flags & kOpenRead) != 0;
    int out = r && w ? (hostfs::kRead | hostfs::kWrite) : (w ? hostfs::kWrite : hostfs::kRead);
    if ((flags & kOpenAppend) != 0) out |= hostfs::kAppend;
    if (w && ((flags & kOpenTruncate) != 0 || !r)) out |= hostfs::kCreate | hostfs::kTruncate;
    if ((flags & kOpenCreate) != 0) out |= hostfs::kCreate;
    return out;
}

// int sceFiosFHOpenSync(const SceFiosOpAttr*, SceFiosFH* out, const char* path, const SceFiosOpenParams*)
GUEST_ABI int hle_fios_fh_open_sync(const void*, int* out, const char* path, const OpenParams* params) {
    if (!out || !path) {
        return kFiosErrBadArg;
    }
    *out = 0;
    std::string host = hle_fs_map_path(path);
    if (host.empty()) {
        return kFiosErrBadPath;
    }
    const std::uint32_t flags = params ? params->open_flags : kOpenRead;
    const int fd = hostfs::Open(host, mode_for(flags));
    if (fd < 0) {
        static int logs = 0;
        if (logs < 16) {
            ++logs;
            host_log("sceFiosFHOpenSync %s (flags 0x%x) failed", path, flags);
        }
        return kFiosErrBadPath;
    }
    int id;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        id = g_next_fh++;
        g_files[id] = File{fd, host};
    }
    *out = id;
    static int logs = 0;
    if (logs < 16) {
        ++logs;
        host_log("sceFiosFHOpenSync %s -> fh %d", path, id);
    }
    return kFiosOk;
}

GUEST_ABI int hle_fios_fh_close_sync(const void*, int fh) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_files.find(fh);
        if (it == g_files.end()) {
            return kFiosErrBadFh;
        }
        fd = it->second.fd;
        g_files.erase(it);
    }
    if (fd >= 0) (void)hostfs::Close(fd);
    return kFiosOk;
}

int file_of(int fh) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_files.find(fh);
    return it == g_files.end() ? -1 : it->second.fd;
}

// SceFiosSize sceFiosFHReadSync(attr, fh, void* buf, SceFiosSize len)
GUEST_ABI std::int64_t hle_fios_fh_read_sync(const void*, int fh, void* buf, std::int64_t len) {
    const int fd = file_of(fh);
    if (fd < 0) {
        return kFiosErrBadFh;
    }
    if (!buf || len < 0) {
        return kFiosErrBadArg;
    }
    if (len == 0) return 0;
    // The kernel writes this buffer, so a page the texture cache has
    // write-protected fails the read rather than faulting: release first, and
    // again if an upload protected one in the meantime.
    write_watch_release(buf, static_cast<std::size_t>(len));
    auto* dst = static_cast<std::uint8_t*>(buf);
    std::int64_t total = 0;
    while (total < len) {
        const std::uint64_t rem = static_cast<std::uint64_t>(len - total);
        std::int64_t n = hostfs::Read(fd, dst + total, rem);
        if (n < 0) {
            const int err = hostfs::LastError();
            if (err == EFAULT && write_watch_release(dst + total, static_cast<std::size_t>(rem))) {
                n = hostfs::Read(fd, dst + total, rem);
            }
            if (n < 0) return total > 0 ? total : kFiosErrAccess;
        }
        if (n == 0) break;  // EOF
        total += n;
    }
    return total;
}

GUEST_ABI std::int64_t hle_fios_fh_write_sync(const void*, int fh, const void* buf, std::int64_t len) {
    const int fd = file_of(fh);
    if (fd < 0) {
        return kFiosErrBadFh;
    }
    if (!buf || len < 0) {
        return kFiosErrBadArg;
    }
    if (len == 0) return 0;
    const auto* src = static_cast<const std::uint8_t*>(buf);
    std::int64_t total = 0;
    while (total < len) {
        const std::int64_t n = hostfs::Write(fd, src + total, static_cast<std::uint64_t>(len - total));
        if (n < 0) return total > 0 ? total : kFiosErrAccess;
        if (n == 0) break;
        total += n;
    }
    return total;
}

// SceFiosOffset sceFiosFHSeek(fh, offset, whence)  whence: 0 set, 1 cur, 2 end
GUEST_ABI std::int64_t hle_fios_fh_seek(int fh, std::int64_t off, int whence) {
    const int fd = file_of(fh);
    if (fd < 0) {
        return kFiosErrBadFh;
    }
    const int w = whence == 0 ? hostfs::kSeekSet : (whence == 1 ? hostfs::kSeekCur : hostfs::kSeekEnd);
    const std::int64_t at = hostfs::Seek(fd, off, w);
    return at < 0 ? kFiosErrBadArg : at;
}

GUEST_ABI std::int64_t hle_fios_fh_tell(int fh) {
    const int fd = file_of(fh);
    if (fd < 0) {
        return kFiosErrBadFh;
    }
    const std::int64_t at = hostfs::Seek(fd, 0, hostfs::kSeekCur);
    return at < 0 ? kFiosErrBadArg : at;
}

GUEST_ABI int hle_fios_stat_sync(const void*, const char* path, FiosStat* out) {
    if (!path || !out) {
        return kFiosErrBadArg;
    }
    std::string host = hle_fs_map_path(path);
    if (host.empty() || !host_stat(host, out)) {
        return kFiosErrBadPath;
    }
    return kFiosOk;
}

GUEST_ABI int hle_fios_file_delete_sync(const void*, const char* path) {
    if (!path) {
        return kFiosErrBadArg;
    }
    std::string host = hle_fs_map_path(path);
    if (host.empty()) {
        return kFiosErrBadPath;
    }
    return hostfs::Unlink(host) ? kFiosOk : kFiosErrBadPath;
}

// SceFiosOp sceFiosDirectoryCreate(attr, path): async; completes immediately.
GUEST_ABI int hle_fios_directory_create(const void*, const char* path) {
    if (!path) {
        return make_op(kFiosErrBadArg, 0);
    }
    std::string host = hle_fs_map_path(path);
    if (host.empty()) {
        return make_op(kFiosErrBadPath, 0);
    }
    if (hostfs::MkDir(host, false)) {
        return make_op(kFiosOk, 0);
    }
    return make_op(hostfs::LastError() == EEXIST ? kFiosErrExists : kFiosErrAccess, 0);
}

int dh_open_impl(int* out, const char* path) {
    if (!out || !path) {
        return kFiosErrBadArg;
    }
    *out = 0;
    std::string host = hle_fs_map_path(path);
    if (host.empty()) {
        return kFiosErrBadPath;
    }
    Dir d;
    d.guest_path = path;
    std::vector<hostfs::DirEntry> entries;
    if (!hostfs::ListDir(host, &entries)) {
        return kFiosErrBadPath;
    }
    for (const hostfs::DirEntry& entry : entries) {
        d.names.push_back(entry.name);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    int id = g_next_dh++;
    g_dirs[id] = std::move(d);
    *out = id;
    return kFiosOk;
}

// int sceFiosDHOpenSync(attr, SceFiosDH* out, const char* path, SceFiosBuffer buf{ptr,len})
GUEST_ABI int hle_fios_dh_open_sync(const void*, int* out, const char* path, void*, std::size_t) {
    return dh_open_impl(out, path);
}

// SceFiosOp sceFiosDHOpen(attr, out, path, buf): async form.
GUEST_ABI int hle_fios_dh_open(const void*, int* out, const char* path, void*, std::size_t) {
    return make_op(dh_open_impl(out, path), 0);
}

GUEST_ABI int hle_fios_dh_read_sync(const void*, int dh, FiosDirEntry* out) {
    if (!out) {
        return kFiosErrBadArg;
    }
    std::string guest_dir;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_dirs.find(dh);
        if (it == g_dirs.end()) {
            return kFiosErrBadDh;
        }
        Dir& d = it->second;
        if (d.pos >= d.names.size()) {
            return static_cast<int>(0x80C9001Fu);  // SCE_FIOS_ERROR_EOF
        }
        guest_dir = d.guest_path;
        name = d.names[d.pos++];
    }
    std::string full = guest_dir;
    if (full.empty() || full.back() != '/') {
        full += '/';
    }
    full += name;
    std::memset(out, 0, sizeof(*out));
    std::string host = hle_fs_map_path(full.c_str());
    if (!host.empty()) {
        host_stat(host, &out->stat);
    }
    std::strncpy(out->full_path, full.c_str(), sizeof(out->full_path) - 1);
    out->full_path_length = static_cast<std::uint16_t>(full.size());
    out->offset_to_name = static_cast<std::uint16_t>(full.size() - name.size());
    out->name_length = static_cast<std::uint16_t>(name.size());
    return kFiosOk;
}

GUEST_ABI int hle_fios_dh_close_sync(const void*, int dh) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_dirs.erase(dh) ? kFiosOk : kFiosErrBadDh;
}

GUEST_ABI int hle_fios_op_wait(int op) {
    MainThreadWait timed(2);
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_ops.find(op);
    return it == g_ops.end() ? kFiosErrBadOp : it->second.result;
}

GUEST_ABI std::int64_t hle_fios_op_get_actual_count(int op) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_ops.find(op);
    return it == g_ops.end() ? 0 : it->second.actual;
}

GUEST_ABI void hle_fios_op_delete(int op) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_ops.erase(op);
}

}  // namespace

void hle_register_fios() {
#define REG(name, fn) register_hle_fn(name, reinterpret_cast<void*>(fn))
    REG("sceFiosFHOpenSync", hle_fios_fh_open_sync);
    REG("sceFiosFHCloseSync", hle_fios_fh_close_sync);
    REG("sceFiosFHReadSync", hle_fios_fh_read_sync);
    REG("sceFiosFHWriteSync", hle_fios_fh_write_sync);
    REG("sceFiosFHSeek", hle_fios_fh_seek);
    REG("sceFiosFHTell", hle_fios_fh_tell);
    REG("sceFiosStatSync", hle_fios_stat_sync);
    REG("sceFiosFileDeleteSync", hle_fios_file_delete_sync);
    REG("sceFiosDirectoryCreate", hle_fios_directory_create);
    REG("sceFiosDHOpen", hle_fios_dh_open);
    REG("sceFiosDHOpenSync", hle_fios_dh_open_sync);
    REG("sceFiosDHReadSync", hle_fios_dh_read_sync);
    REG("sceFiosDHCloseSync", hle_fios_dh_close_sync);
    REG("sceFiosOpWait", hle_fios_op_wait);
    REG("sceFiosOpGetActualCount", hle_fios_op_get_actual_count);
    REG("sceFiosOpDelete", hle_fios_op_delete);
#undef REG
}
