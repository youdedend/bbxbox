// SPDX-License-Identifier: GPL-3.0-or-later
// The console side of the guest arena: bbhost's core/memory.cpp contract,
// implemented by xbox/platform/guest_memory.inc over the FromApp APIs.
//
// bbhost calls these from the ELF loader and from every patch installer, so the
// signatures are its own (`guest_alloc`, `guest_protect_rwx`, ...) and the
// CMake build replaces bbhost's memory.cpp with this file. Nothing else in the
// tree needs to know that the console cannot call VirtualAlloc.
#include "xbox/platform/guest_memory.h"

// The arena implementation itself (Alloc/Free/Protect/KeepWritable/
// MarkExecutableRange/CommittedMib) is shared with the host tests, so it lives in
// the header-like .inc; the same ledger runs over VirtualAlloc2FromApp here and
// over malloc there.
#include "xbox/platform/guest_memory.inc"

#include "xbox/platform/log.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace bbx::guest {
// One arena per process. bbhost loads exactly one eboot.
Arena g_arena;
bool g_warned_wx = false;

Arena& GlobalArena() { return g_arena; }

bool AllocGlobal(std::uint64_t slide, std::size_t size) {
    if (g_arena.base != nullptr) FreeGlobal();
    const std::uint64_t mib = (size + (1u << 20) - 1) >> 20;
    if (!Alloc(g_arena, slide, size)) {
        bbx::platform::LogF("guest: VirtualAlloc2FromApp(0x%llx, %zu) refused (limit 1 TiB, %llu MiB)",
                            static_cast<unsigned long long>(slide), size,
                            static_cast<unsigned long long>(mib));
        return false;
    }
    bbx::platform::LogF("guest: arena at 0x%llx, %zu bytes (%llu MiB committed), below the 1 TiB line",
                        static_cast<unsigned long long>(g_arena.slide), g_arena.size,
                        static_cast<unsigned long long>(CommittedMib(g_arena)));
    return true;
}

void FreeGlobal() { Free(g_arena); }

}  // namespace bbx::guest

// --- bbhost's contract ------------------------------------------------------
// `extern "C"` at namespace scope is the one form clang-cl accepts with
// __attribute__((sysv_abi)) callers on the other side (bbhost's thunk table).

extern "C" bool guest_alloc(bbx::guest::Arena* mem, std::uint64_t slide, std::size_t size) {
    if (mem == nullptr) return false;
    *mem = bbx::guest::Arena{};
    return bbx::guest::Alloc(*mem, slide, size);
}

extern "C" void guest_free(bbx::guest::Arena* mem) {
    if (mem != nullptr) bbx::guest::Free(*mem);
}

extern "C" bool guest_protect_rx(bbx::guest::Arena* mem, std::uint64_t va, std::size_t size) {
    return mem != nullptr && bbx::guest::Protect(*mem, va, size, bbx::guest::kR | bbx::guest::kX);
}

extern "C" bool guest_protect_rw(bbx::guest::Arena* mem, std::uint64_t va, std::size_t size) {
    return mem != nullptr &&
           bbx::guest::Protect(*mem, va, size, bbx::guest::kR | bbx::guest::kW);
}

extern "C" bool guest_protect_rwx(bbx::guest::Arena* mem, std::uint64_t va, std::size_t size) {
    if (mem == nullptr) return false;
    // Strict W^X: the page becomes writable now and executable when bbhost asks
    // for RX again (its installers do, immediately after writing).
    return bbx::guest::Protect(*mem, va, size, bbx::guest::kR | bbx::guest::kW);
}

extern "C" void guest_keep_writable(bbx::guest::Arena* mem, std::uint64_t va) {
    if (mem == nullptr) return;
    bbx::guest::KeepWritable(*mem, va);
    if (bbx::guest::WxConflict(*mem, va & ~0xfffull) && !bbx::guest::g_warned_wx) {
        bbx::guest::g_warned_wx = true;
        bbx::platform::LogF(
            "guest: page 0x%llx is both kept-writable and executable; W^X keeps it writable and the "
            "call site must ask for RX before running it",
            static_cast<unsigned long long>(va & ~0xfffull));
    }
}

extern "C" void guest_mark_executable(bbx::guest::Arena* mem, std::uint64_t va, std::size_t size) {
    if (mem != nullptr) bbx::guest::MarkExecutableRange(*mem, va, size);
}
