// SPDX-License-Identifier: GPL-3.0-or-later
// Implementation of bbhost's core/memory.cpp contract for UWP.
//
// This file is `#include`d by xbox/platform/guest_memory.cpp (which supplies the
// Windows bits) and by tests/guest_memory_test.cpp (which supplies a malloc
// backing), so the ledger and its rules are the same code in both.
#pragma once

#include "xbox/platform/guest_memory.h"

namespace bbx::guest {

namespace detail {

inline std::uint64_t PageOf(std::uint64_t va) { return va & ~0xfffull; }

// Adds `[lo, hi)` to the executable set, merging neighbours so the list stays
// short: bbhost marks every write site it installs, and Yharnam installs
// thousands over a session.
inline void MarkExecutable(Arena& arena, std::uint64_t lo, std::uint64_t hi) {
    if (hi <= lo) return;
    for (auto& range : arena.executable) {
        if (lo >= range.first && hi <= range.first + range.second) return;
    }
    arena.executable.emplace_back(lo, hi - lo);
    // Merge overlapping/adjacent ranges (the list is small: hundreds at most).
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < arena.executable.size(); ++i) {
            for (std::size_t j = i + 1; j < arena.executable.size(); ++j) {
                const std::uint64_t a_lo = arena.executable[i].first;
                const std::uint64_t a_hi = a_lo + arena.executable[i].second;
                const std::uint64_t b_lo = arena.executable[j].first;
                const std::uint64_t b_hi = b_lo + arena.executable[j].second;
                if (b_lo <= a_hi && a_lo <= b_hi) {
                    arena.executable[i].first = a_lo < b_lo ? a_lo : b_lo;
                    arena.executable[i].second = (a_hi > b_hi ? a_hi : b_hi) - arena.executable[i].first;
                    arena.executable.erase(arena.executable.begin() + static_cast<std::ptrdiff_t>(j));
                    changed = true;
                    break;
                }
            }
            if (changed) break;
        }
    }
}

}  // namespace detail

#ifdef BBX_UNIT_TEST

// --- host backing -----------------------------------------------------------
// The tests get a plain allocation; the ledger is what they exercise.

inline bool Alloc(Arena& arena, std::uint64_t slide, std::size_t size) {
    if (slide == 0 || size == 0) return false;
    if (slide >= kAddressLimit || size > kAddressLimit - slide) return false;
    if ((slide & 0xfffull) != 0) return false;  // same rule as the console path
    arena.base = std::calloc(1, size);
    if (arena.base == nullptr) return false;
    arena.slide = slide;
    arena.size = size;
    arena.committed = (size + 0xfff) & ~static_cast<std::size_t>(0xfff);
    return true;
}

inline void Free(Arena& arena) {
    std::free(arena.base);
    arena.base = nullptr;
    arena.size = 0;
    arena.slide = 0;
    arena.committed = 0;
    arena.keep_writable.clear();
    arena.executable.clear();
}

inline bool Protect(Arena& arena, std::uint64_t va, std::size_t size, std::uint8_t access) {
    if (!RangeIsGuest(arena, va, size)) return false;
    // The ledger's rules are all a host run has to agree with: the range must be
    // inside the arena, and an execute grant over any of it sticks (a write does
    // not un-learn that the bytes are code).
    if ((access & kX) != 0) {
        detail::MarkExecutable(arena, detail::PageOf(va), detail::PageOf(va + size - 1) + 0x1000);
    }
    return true;
}

#else  // !BBX_UNIT_TEST

// --- console backing --------------------------------------------------------

namespace detail {

inline DWORD ToWin32(std::uint8_t access) {
    // No PAGE_EXECUTE_READWRITE anywhere: the AppContainer enforces W^X.
    switch (access & (kR | kW | kX)) {
    case kR | kX: return PAGE_EXECUTE_READ;
    case kR | kW:
    case kW: return PAGE_READWRITE;
    case kX: return PAGE_EXECUTE;
    default: return PAGE_READONLY;
    }
}

}  // namespace detail

inline bool Alloc(Arena& arena, std::uint64_t slide, std::size_t size) {
    if (slide == 0 || size == 0) return false;
    // Lesson #2: guest code must stay below the 1 TiB line.
    if (slide >= kAddressLimit || size > kAddressLimit - slide) return false;
    if ((slide & 0xfffull) != 0) return false;  // VirtualAlloc2 needs page alignment

    // Reserve exactly the slide: anything else and every guest pointer in the
    // image (and every precomputed patch site) is off by the delta.
    void* wanted = reinterpret_cast<void*>(static_cast<std::uintptr_t>(slide));
    // FromApp: the desktop VirtualAlloc2 is not callable from an AppContainer.
    // The address is exact: anything else and every precomputed patch site is
    // off by the delta.
    void* base = VirtualAlloc2FromApp(GetCurrentProcess(), wanted, size,
                                      MEM_RESERVE | MEM_COMMIT | MEM_RESERVE_PLACEHOLDER,
                                      PAGE_READWRITE, nullptr, 0);
    if (base == nullptr) {
        base = VirtualAlloc2FromApp(GetCurrentProcess(), wanted, size, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE, nullptr, 0);
    }
    if (base != nullptr && base != wanted) {
        VirtualFree(base, 0, MEM_RELEASE);
        base = nullptr;
    }
    if (base == nullptr) return false;
    if (reinterpret_cast<std::uintptr_t>(base) != static_cast<std::uintptr_t>(slide)) {
        VirtualFree(base, 0, MEM_RELEASE);  // never run a slid image
        return false;
    }
    arena.base = base;
    arena.slide = slide;
    arena.size = size;
    arena.committed = (size + 0xfff) & ~static_cast<std::size_t>(0xfff);
    return true;
}

inline void Free(Arena& arena) {
    if (arena.base != nullptr) VirtualFree(arena.base, 0, MEM_RELEASE);
    arena.base = nullptr;
    arena.size = 0;
    arena.slide = 0;
    arena.committed = 0;
    arena.keep_writable.clear();
    arena.executable.clear();
}

inline bool Protect(Arena& arena, std::uint64_t va, std::size_t size, std::uint8_t access) {
    if (!RangeIsGuest(arena, va, size)) return false;
    const std::uint64_t lo = detail::PageOf(va);
    const std::uint64_t hi = detail::PageOf(va + size - 1) + 0x1000;
    if ((access & kX) != 0) detail::MarkExecutable(arena, lo, hi);
    bool any_execute = false;
    for (std::uint64_t page = lo; page < hi; page += 0x1000) {
        const std::uint8_t effective = EffectiveAccess(arena, page, access);
        DWORD old = 0;
        if (!VirtualProtectFromApp(Ptr(arena, page), 0x1000, detail::ToWin32(effective), &old)) {
            return false;
        }
        if ((effective & kX) != 0) any_execute = true;
    }
    // Strict W^X: a write that re-arms a code page leaves stale instructions in
    // the I-cache until the RX grant flushes it.
    if (any_execute) FlushInstructionCache(GetCurrentProcess(), Ptr(arena, lo), hi - lo);
    return true;
}

#endif  // BBX_UNIT_TEST

inline void KeepWritable(Arena& arena, std::uint64_t va) {
    const std::uint64_t page = detail::PageOf(va);
    for (const std::uint64_t kept : arena.keep_writable) {
        if (kept == page) return;
    }
    arena.keep_writable.push_back(page);
    Protect(arena, page, 0x1000, kR | kW);  // writable, never executable under W^X
}

inline void MarkExecutableRange(Arena& arena, std::uint64_t va, std::size_t size) {
    if (size == 0) return;
    detail::MarkExecutable(arena, detail::PageOf(va), detail::PageOf(va + size - 1) + 0x1000);
}

// The MiB the budget's guest owner accounts for. A page-granular count of what
// is committed, not the reservation.
[[nodiscard]] inline std::uint64_t CommittedMib(const Arena& arena) {
    return (arena.committed + (1u << 20) - 1) >> 20;
}

}  // namespace bbx::guest
