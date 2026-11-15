// SPDX-License-Identifier: GPL-3.0-or-later
// Overrides bbhost's `src/core/memory.h` (see docs/integration.md): the port's
// arena carries the same three fields bbhost's loader and patch installers use,
// plus the W^X ledger. bbhost's memory.cpp is replaced by guest_memory.cpp, so
// the declarations below are the whole contract.
#pragma once

#include <cstdint>
#include <cstddef>
#include "xbox/platform/guest_memory.h"

using GuestMemory = bbx::guest::Arena;

// extern "C" at namespace scope, as bbhost's own memory.h declares them; the
// definitions live in guest_memory.cpp.
extern "C" {
bool guest_alloc(GuestMemory* mem, std::uint64_t slide, std::size_t size);
void guest_free(GuestMemory* mem);
bool guest_protect_rx(GuestMemory* mem, std::uint64_t va, std::size_t size);
bool guest_protect_rw(GuestMemory* mem, std::uint64_t va, std::size_t size);
bool guest_protect_rwx(GuestMemory* mem, std::uint64_t va, std::size_t size);
void guest_keep_writable(GuestMemory* mem, std::uint64_t va);
void guest_mark_executable(GuestMemory* mem, std::uint64_t va, std::size_t size);
}

inline void* guest_ptr(const GuestMemory& mem, std::uint64_t va) {
    return static_cast<std::uint8_t*>(mem.base) + (va - mem.slide);
}
