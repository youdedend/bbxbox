// SPDX-License-Identifier: GPL-3.0-or-later
// The guest arena, for UWP: bbhost's core/memory.cpp contract, implemented on
// VirtualAlloc2 inside an address space we choose.
//
// Why this file exists rather than bbhost's own Windows path:
//
//  * Lesson #2 of the prior art. Guest pointers are PS4 pointers, and bbhost
//    patches guest code in place (`decomp`, `graphics_patch`, `frame_pool`), so
//    every guest address has to sit in the low 47 bits of the process's address
//    space, below the 1 TiB line. Xbox reserves the upper region for the
//    system; an image that lands above it faults on the first call through a
//    patched stub. `guest_alloc` enforces the limit here instead of trusting
//    the loader's slide.
//  * Committed memory is budget. Game Mode gives the title 5,120 MiB and no
//    more; the arena's commitment is what `bbx_budget`'s guest owner accounts
//    for, so the commit is measured and reported, not assumed.
//
// The AppContainer enforces W^X: no page is ever mapped PAGE_EXECUTE_READWRITE.
// bbhost's contract asks for RWX in places, so the port splits it: a write goes
// to RW, execute intent is remembered, and the RX grant is taken when bbhost
// next asks for it (its patch installers always do, right after writing) with
// FlushInstructionCache so the CPU stops running the old bytes. The allocation
// itself uses the FromApp variants because the desktop entry points are not
// callable from an AppContainer.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#ifndef BBX_UNIT_TEST
#include <windows.h>
#endif

namespace bbx::guest {

// The line guest pointers may not cross. PS4 pointers are 47-bit; the console's
// loader places the image well below this, and the port keeps it there.
inline constexpr std::uint64_t kAddressLimit = 1ull << 40;  // 1 TiB

// Matches bbhost's GuestMemory so core/memory.cpp can be replaced wholesale.
struct Arena {
    void* base = nullptr;
    std::size_t size = 0;
    std::uint64_t slide = 0;
    std::uint64_t committed = 0;  // bytes actually committed (page rounded)
    // Pages kept writable for good, and ranges known to be executable: bbhost's
    // guest_keep_writable/guest_mark_executable semantics.
    std::vector<std::uint64_t> keep_writable;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> executable;
};

inline std::uint8_t* Ptr(const Arena& arena, std::uint64_t va) {
    return static_cast<std::uint8_t*>(arena.base) + (va - arena.slide);
}

[[nodiscard]] inline bool RangeIsGuest(const Arena& arena, std::uint64_t va, std::size_t size) {
    if (size == 0) return false;
    if (va < arena.slide) return false;
    const std::uint64_t end = va + size;
    if (end < va) return false;
    return end <= arena.slide + arena.size && end <= kAddressLimit;
}

// True when `va` is one of the pages bbhost asked to keep writable.
[[nodiscard]] inline bool KeptWritable(const Arena& arena, std::uint64_t page) {
    for (const std::uint64_t kept : arena.keep_writable) {
        if (kept == page) return true;
    }
    return false;
}

// True when `[page, page+4KiB)` lies inside an executable range.
[[nodiscard]] inline bool MarkedExecutable(const Arena& arena, std::uint64_t page) {
    for (const auto& [lo, len] : arena.executable) {
        if (page >= lo && page + 0x1000u <= lo + len) return true;
    }
    return false;
}

// The protection a page currently has, as the port models it. `requested` is
// WT_RX / WT_RW / WT_RWX.
enum class Access : std::uint8_t { kRead = 0, kWrite = 1, kExecute = 2 };
inline constexpr std::uint8_t kR = 1u << 0;
inline constexpr std::uint8_t kW = 1u << 1;
inline constexpr std::uint8_t kX = 1u << 2;

// The protection actually mapped for `requested` on `page`, under strict W^X:
// a write request maps RW and keeps the execute right in the ledger (the next
// RX request regains it), and a page already known to hold code is never
// mapped writable-and-executable at once.
[[nodiscard]] inline std::uint8_t EffectiveAccess(const Arena& arena, std::uint64_t page,
                                                  std::uint8_t requested) {
    std::uint8_t access = requested;
    if ((access & kW) != 0) access &= ~static_cast<std::uint8_t>(kX);  // W and X never together
    if ((access & kW) == 0 && MarkedExecutable(arena, page)) access |= kX;  // code stays code
    // A page bbhost keeps writable for the rest of the session stays writable,
    // and never gains execute: W^X.
    if (KeptWritable(arena, page)) access = static_cast<std::uint8_t>((access | kR | kW) & ~kX);
    return access;
}

// True when a page would need both rights at once, which the port must not map.
// The backend logs it once per page and treats bbhost's request as a write.
[[nodiscard]] inline bool WxConflict(const Arena& arena, std::uint64_t page) {
    return KeptWritable(arena, page) && MarkedExecutable(arena, page);
}

#ifdef BBX_UNIT_TEST
// Host tests model the arena as one malloc over which the ledger runs.
struct HostBacking {
    void* raw = nullptr;
    std::size_t size = 0;
};
#endif

// The one arena bbhost's loader uses, created through the console path above.
Arena& GlobalArena();
bool AllocGlobal(std::uint64_t slide, std::size_t size);
void FreeGlobal();

}  // namespace bbx::guest
