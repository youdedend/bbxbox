#include "core/paced_file.h"

#include <algorithm>
#include <utility>

#if defined(BBX_UWP)
// The commit is a rename onto the dump's own folder, and a granted path is not
// the CRT's to rename: the seam's `Rename` is `MoveFileFromApp` there.
#include "xbox/platform/host_fs.h"
#include "xbox/platform/hostfs_extra.h"
#else
#include <filesystem>
#include <system_error>
#endif

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

HostPacedFile::HostPacedFile(std::string path, std::string tmp, std::size_t chunk)
    : path_(std::move(path)), tmp_(std::move(tmp)), chunk_(std::max<std::size_t>(chunk, 1)) {
    f_ = std::fopen(tmp_.c_str(), "wb");
    ok_ = f_ != nullptr;
}

HostPacedFile::~HostPacedFile() {
    if (f_) std::fclose(f_);
#if defined(BBX_UWP)
    if (!committed_) (void)bbx::hostfs::Unlink(tmp_);  // a half-written staging file
#else
    if (!committed_) {
        std::error_code ec;
        std::filesystem::remove(tmp_, ec);
    }
#endif
}

bool HostPacedFile::sync() {
    if (std::fflush(f_) != 0) return false;
#if defined(_WIN32)
    return _commit(_fileno(f_)) == 0;
#else
    return ::fdatasync(fileno(f_)) == 0;
#endif
}

bool HostPacedFile::write(const void* data, std::size_t size) {
    const char* p = static_cast<const char*>(data);
    while (ok_ && size) {
        const std::size_t n = std::min(size, chunk_ - unsynced_);
        ok_ = std::fwrite(p, 1, n, f_) == n;
        p += n;
        size -= n;
        unsynced_ += n;
        if (ok_ && unsynced_ >= chunk_) {
            ok_ = sync();
            unsynced_ = 0;
        }
    }
    return ok_;
}

bool HostPacedFile::commit() {
    if (ok_ && unsynced_) ok_ = sync();
    if (f_) ok_ = std::fclose(f_) == 0 && ok_;
    f_ = nullptr;
#if defined(BBX_UWP)
    // Replace, because the staging file's whole purpose is to take the place of
    // the save that is already there.
    committed_ = ok_ && bbx::hostfs::Rename(tmp_, path_, true);
#else
    std::error_code ec;
    if (ok_) std::filesystem::rename(tmp_, path_, ec);
    committed_ = ok_ && !ec;
#endif
    return committed_;
}
