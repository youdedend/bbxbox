// SPDX-License-Identifier: GPL-3.0-or-later
// From a baked binding's plan to the descriptor the console writes (Slice 5B-2b).
//
// `bbx_path_table.h` carries the bake's answer to "which guest memory is `t7`?",
// as the translator's `ResourcePath`: a user SGPR, the dereferences, and the
// dword offset of the resource words in the last block. That path names *words*,
// not a buffer. Turning the words into a descriptor is three separate questions,
// and the port already answers the last two elsewhere:
//
//   1. the path has to resolve against the dispatch's own user data, which needs
//      guest memory (`ResolvePath`, bxx_guest_path.h) - a path that does not
//      resolve refuses, because reading four dwords from wherever the arithmetic
//      ended up is how a shader is handed another buffer's bytes;
//   2. the four words it lands on are a V#, and `DecideSlot`
//      (bbx_guest_vsharp.h) says whether that V# is a range this port can bind at
//      all: null, unaligned, past the guest's window, or crossing a 4 GiB region
//      the page table has no carry for;
//   3. a D3D12 buffer view names *one* resource, so the range has to sit inside
//      one slab (`GuestPageTable::ContiguousSlab`). A range that spans two slabs
//      is refused rather than bound at its start, which would silently drop
//      everything past the first 256 MiB boundary.
//
// Device-free on purpose: `tests/guest_binding_test.cpp` pins every verdict, and
// the console side (xbox/platform/guest_compute.cpp) does nothing but call
// D3D12 with what this returns. A refusal here is a null descriptor and one line
// of plain English naming the step, never a default view.
#pragma once

#include <cstdint>
#include <string>

#include "src/host/d3d12/bbx_guest_mem.h"
#include "src/host/d3d12/bbx_guest_path.h"
#include "src/host/d3d12/bbx_guest_vsharp.h"
#include "src/host/d3d12/bbx_path_table.h"

namespace bbx::d3d12 {

enum class BindingVerdict : std::uint8_t {
    kWrite = 0,         // one view over a mapped range inside one slab
    kNull = 1,          // a null V#: GCN reads zeroes, and the null descriptor says so
    kNoPlan = 2,        // the bake assigned no binding here (see PathTable::Find)
    kUnreadable = 3,    // the plan did not resolve from this dispatch's user data
    kRefusedSlot = 4,   // the V# names a range this port cannot bind (DecideSlot)
    kNotContiguous = 5, // the range crosses a slab boundary: no view can name it
};

[[nodiscard]] const char* BindingVerdictName(BindingVerdict verdict);

struct BindingWrite {
    BindingVerdict verdict = BindingVerdict::kNoPlan;
    std::uint32_t binding = 0;
    BindingKind kind = BindingKind::kSrv;
    VSharpView view{};      // the V# the path landed on, whatever the verdict
    std::uint32_t slab = 0; // the slab the view is made in, when kWrite
    std::uint64_t offset = 0; // the view's byte offset inside that slab
    std::uint32_t dwords = 0; // the view's length, in dwords
    std::string why;          // the refusal, in words: which step, and what it saw
};

// `user` is the dispatch's sixteen user SGPRs. `read`/`context` are the caller's
// guest-memory seam (the console's pager, the tests' array); `table` decides the
// slab. `base_limit` is one past the highest address a V# may name, passed in so
// a test can pin a smaller window (as `DecideSlot` takes it).
[[nodiscard]] BindingWrite DecideBindingWrite(const BindingPlan& plan, const std::uint32_t* user,
                                             const GuestPageTable& table, ReadGuest read, void* context,
                                             std::uint64_t base_limit = kGuestWindowBytes);

// "t4: 10 dwords at slab 3 + 0x2100" or "u0 refused (the V# crosses a 4 GiB
// region)": one line per binding for the console's log, and the same line in the
// tests, so what a reviewer reads is what was decided.
[[nodiscard]] std::string BindingWriteLine(const BindingWrite& write);

}  // namespace bbx::d3d12
