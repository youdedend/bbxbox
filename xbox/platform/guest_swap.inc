// SPDX-License-Identifier: GPL-3.0-or-later
// The spill file, carried over from the prior art's guest swap (lesson #2) and
// pointed at the thing bbhost actually runs out of: GPU-visible memory.
//
// Prior art: shadPS4 maps the whole dump into guest address space, so its swap
// file is sparse guest pages. bbhost does not map the dump - it allocates typed
// resources as the engine asks - so the pressure lands in the D3D12 backend's
// pools instead: readback buffers for the dump panel, staging for the precompile
// cache, and uploaded pipeline-library blobs. Those are the things the residency
// pager (`bbx_budget`) can afford to move out of the 5,120 MiB, and this is
// where they go: a fixed-size, pre-allocated file in `LocalState`, never grown
// at run time, so a spill can never itself fail for lack of space.
//
// Everything except the file I/O is here, and the I/O is a two-function vtable,
// so tests run the real slot allocator against a vector.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#if defined(_WIN32) && defined(BBX_UWP)
#include <windows.h>
#include <fileapifromapp.h>
#endif

namespace bbx::guest {

inline constexpr std::uint32_t kSpillSlotBytes = 64u * 1024u;
inline constexpr std::uint32_t kSpillMaxSlots = 8192u;  // 512 MiB

struct SpillIo {
    void* ctx = nullptr;
    bool (*write)(void* ctx, std::uint64_t offset, const void* bytes, std::size_t len) = nullptr;
    bool (*read)(void* ctx, std::uint64_t offset, void* bytes, std::size_t len) = nullptr;
};

struct Spill {
    SpillIo io{};
    std::uint32_t slots = 0;
    std::vector<std::uint32_t> free_slots;
    std::unordered_map<std::uint64_t, std::uint32_t> slot_of;
    std::unordered_map<std::uint64_t, std::uint64_t> stamp_of;
    std::uint64_t tick = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t evictions = 0;

    [[nodiscard]] bool Ready() const {
        return slots != 0 && io.write != nullptr && io.read != nullptr;
    }
    [[nodiscard]] std::uint32_t UsedSlots() const {
        return static_cast<std::uint32_t>(slot_of.size());
    }
    // The file's size: what a caller must pre-allocate.
    [[nodiscard]] std::uint64_t FileBytes() const {
        return static_cast<std::uint64_t>(slots) * kSpillSlotBytes;
    }
};

// `slots` is clamped to kSpillMaxSlots and must be non-zero.
inline bool SpillInit(Spill& spill, SpillIo io, std::uint32_t slots) {
    spill = Spill{};
    if (io.write == nullptr || io.read == nullptr || slots == 0) return false;
    if (slots > kSpillMaxSlots) slots = kSpillMaxSlots;
    spill.io = io;
    spill.slots = slots;
    spill.free_slots.reserve(slots);
    // Push in reverse so slot 0 is handed out first, which keeps the file's
    // early slots hot and its tail untouched when the cache is small.
    for (std::uint32_t i = slots; i > 0; --i) spill.free_slots.push_back(i - 1);
    return true;
}

// Drops the least recently used entry. Returns false when nothing is resident.
inline bool SpillEvictOne(Spill& spill) {
    if (spill.slot_of.empty()) return false;
    auto oldest = spill.slot_of.begin();
    for (auto it = spill.slot_of.begin(); it != spill.slot_of.end(); ++it) {
        if (spill.stamp_of[it->first] < spill.stamp_of[oldest->first]) oldest = it;
    }
    const std::uint64_t key = oldest->first;
    spill.free_slots.push_back(oldest->second);
    spill.slot_of.erase(oldest);
    spill.stamp_of.erase(key);
    ++spill.evictions;
    return true;
}

// Writes up to one slot. Larger payloads are the caller's problem: the D3D12
// residency manager stores a resource in slot-sized pieces with a key per piece,
// because a 64 MiB buffer is not a unit the pager wants to move atomically.
inline bool SpillStore(Spill& spill, std::uint64_t key, const void* bytes, std::size_t len) {
    if (!spill.Ready() || bytes == nullptr || len == 0 || len > kSpillSlotBytes) return false;
    auto found = spill.slot_of.find(key);
    if (found == spill.slot_of.end()) {
        while (spill.free_slots.empty()) {
            if (!SpillEvictOne(spill)) return false;
        }
        const std::uint32_t slot = spill.free_slots.back();
        spill.free_slots.pop_back();
        found = spill.slot_of.emplace(key, slot).first;
    }
    const std::uint64_t offset =
        static_cast<std::uint64_t>(found->second) * kSpillSlotBytes;
    if (!spill.io.write(spill.io.ctx, offset, bytes, len)) {
        // A failed write must not make the key look resident.
        spill.free_slots.push_back(found->second);
        spill.slot_of.erase(found);
        spill.stamp_of.erase(key);
        return false;
    }
    spill.stamp_of[key] = ++spill.tick;
    spill.bytes_written += len;
    return true;
}

inline bool SpillLoad(Spill& spill, std::uint64_t key, void* out, std::size_t len) {
    if (!spill.Ready() || out == nullptr || len == 0 || len > kSpillSlotBytes) return false;
    const auto found = spill.slot_of.find(key);
    if (found == spill.slot_of.end()) return false;
    const std::uint64_t offset = static_cast<std::uint64_t>(found->second) * kSpillSlotBytes;
    if (!spill.io.read(spill.io.ctx, offset, out, len)) return false;
    spill.stamp_of[key] = ++spill.tick;
    spill.bytes_read += len;
    return true;
}

[[nodiscard]] inline bool SpillResident(const Spill& spill, std::uint64_t key) {
    return spill.slot_of.count(key) != 0;
}

inline constexpr std::uint32_t kSpillSectorAlign = 4096u;

[[nodiscard]] constexpr std::size_t SpillAlignUp(std::size_t bytes) noexcept {
    return (bytes + kSpillSectorAlign - 1u) & ~static_cast<std::size_t>(kSpillSectorAlign - 1u);
}

// Evicts down to `keep` resident slots; used when the pager asks for headroom
// back. Returns how many were dropped.
inline std::uint32_t SpillTrim(Spill& spill, std::uint32_t keep) {
    std::uint32_t dropped = 0;
    while (spill.UsedSlots() > keep && SpillEvictOne(spill)) ++dropped;
    return dropped;
}

#if defined(_WIN32) && defined(BBX_UWP)
// Unbuffered pre-allocated UWP spill file (inspired by XboxEmulationHub/xbox-swap's
// FILE_FLAG_NO_BUFFERING + SetEndOfFile technique, paired with 64 KiB sector-aligned
// slots so spilled cold data never inflates Xbox's 5,120 MiB OS file-cache memory).
struct UnbufferedSpillFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::uint64_t capacity_bytes = 0;
    alignas(kSpillSectorAlign) std::uint8_t sector_buf[kSpillSlotBytes]{};

    ~UnbufferedSpillFile() { Close(); }

    void Close() {
        if (handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
        capacity_bytes = 0;
    }

    bool Open(const wchar_t* path, std::uint32_t slots) {
        Close();
        if (path == nullptr || slots == 0) return false;
        if (slots > kSpillMaxSlots) slots = kSpillMaxSlots;
        const std::uint64_t total = static_cast<std::uint64_t>(slots) * kSpillSlotBytes;
        CREATEFILE2_EXTENDED_PARAMETERS params{};
        params.dwSize = sizeof(params);
        params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        params.dwFileFlags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS;
        HANDLE h = ::CreateFile2FromAppW(path, GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS,
                                         &params);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(total);
        if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) || !::SetEndOfFile(h)) {
            ::CloseHandle(h);
            return false;
        }
        handle = h;
        capacity_bytes = total;
        return true;
    }

    static bool WriteSlot(void* ctx, std::uint64_t offset, const void* bytes, std::size_t len) {
        auto* self = static_cast<UnbufferedSpillFile*>(ctx);
        if (self == nullptr || self->handle == INVALID_HANDLE_VALUE || bytes == nullptr ||
            len == 0 || len > kSpillSlotBytes || (offset % kSpillSectorAlign) != 0) {
            return false;
        }
        const std::size_t aligned = SpillAlignUp(len);
        if (offset + aligned > self->capacity_bytes) return false;
        std::memcpy(self->sector_buf, bytes, len);
        if (aligned > len) std::memset(self->sector_buf + len, 0, aligned - len);
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xffffffffull);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD written = 0;
        return ::WriteFile(self->handle, self->sector_buf, static_cast<DWORD>(aligned), &written,
                           &ov) != FALSE &&
               written == static_cast<DWORD>(aligned);
    }

    static bool ReadSlot(void* ctx, std::uint64_t offset, void* out, std::size_t len) {
        auto* self = static_cast<UnbufferedSpillFile*>(ctx);
        if (self == nullptr || self->handle == INVALID_HANDLE_VALUE || out == nullptr ||
            len == 0 || len > kSpillSlotBytes || (offset % kSpillSectorAlign) != 0) {
            return false;
        }
        const std::size_t aligned = SpillAlignUp(len);
        if (offset + aligned > self->capacity_bytes) return false;
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xffffffffull);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (!::ReadFile(self->handle, self->sector_buf, static_cast<DWORD>(aligned), &got, &ov) ||
            got != static_cast<DWORD>(aligned)) {
            return false;
        }
        std::memcpy(out, self->sector_buf, len);
        return true;
    }

    [[nodiscard]] SpillIo Io() { return SpillIo{this, &WriteSlot, &ReadSlot}; }
};
#endif

}  // namespace bbx::guest
