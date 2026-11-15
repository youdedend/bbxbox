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
#include <unordered_map>
#include <vector>

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

// Evicts down to `keep` resident slots; used when the pager asks for headroom
// back. Returns how many were dropped.
inline std::uint32_t SpillTrim(Spill& spill, std::uint32_t keep) {
    std::uint32_t dropped = 0;
    while (spill.UsedSlots() > keep && SpillEvictOne(spill)) ++dropped;
    return dropped;
}

}  // namespace bbx::guest
