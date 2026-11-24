// SPDX-License-Identifier: GPL-3.0-or-later
// Which guest buffer a dispatch's resource table names - and which ones this
// port may not bind.
//
// Slice 5B-2a runs a compute dispatch with its space-1 slots left null: the
// shader's `t0..t63`/`u0..u63`/`b1..b63` all read "nothing bound", which is a
// defined value in D3D12 and the safe answer when the alternative is binding the
// wrong buffer. Slice 5B-2b is the resolver: a V# (the console's buffer
// descriptor) says base, stride and num_records, and that is enough to decide
// three things without a device:
//
//   1. is there anything there at all (`base == 0 && num_records == 0` is GCN's
//      null buffer: reads return zero, and the null descriptor already says so);
//   2. is the range expressible in this port's guest memory? The page table is
//      two dwords in the shader and has no carry between 4 GiB regions
//      (bbx_guest_mem.h), so a range that crosses one is refused here rather
//      than mapped halfway;
//   3. is the range inside the guest's window at all, aligned, and non-empty.
//
// The decode is upstream's: `gcn::translate.h` reads a V#'s length as "stride in
// word 1 [29:16], num_records in word 2, times the stride when that is not 0",
// and `VSharpRangeDwords` below is that same rule, tested against it. What this
// file adds is the *refusal*, because a host that binds a range the guest's page
// table cannot address gets a shader that reads another buffer's bytes and a
// frame that is wrong in a way no log explains.
//
// Portable and device-free on purpose: `tests/guest_vsharp_test.cpp` covers the
// verdicts on the build machine, where a wrong rule costs a second and not a
// console round trip.
#pragma once

#include <cstdint>
#include <string>

#include "src/host/d3d12/bbx_guest_mem.h"

namespace bbx::d3d12 {

// A V#, decoded. `bytes` is what GCN checks a buffer access against: num_records
// bytes when the stride is 0 (a raw byte range), num_records x stride otherwise
// (a record array).
struct VSharpView {
    std::uint64_t base = 0;         // guest VA, byte address (words 0 and 1[15:0])
    std::uint32_t stride = 0;       // word 1 [29:16]: 0 for a flat byte range
    std::uint32_t num_records = 0;  // word 2
    std::uint32_t word3 = 0;        // data format [18:15], number format [14:12], DST_SEL [11:0]
    std::uint64_t bytes = 0;        // the range's length, as GCN computes it
    [[nodiscard]] bool null() const { return base == 0 && num_records == 0 && bytes == 0; }
};

// Four dwords, little-endian as the guest's memory holds them.
[[nodiscard]] VSharpView DecodeVSharp(const std::uint32_t words[4]);

// `gcn::translate.h`'s vsharp_range_dwords, restated here so the port's rule and
// the translator's cannot drift apart silently (the test compares them).
[[nodiscard]] std::uint64_t VSharpRangeDwords(std::uint32_t w1, std::uint32_t w2);

enum class SlotVerdict : std::uint8_t {
    kEmpty = 0,       // a null V#: keep the null descriptor, GCN reads zeroes
    kBindable,        // one linear range, inside one guest region, aligned
    kCrossesRegion,   // the range spans a 4 GiB boundary: the page table has no carry
    kOutsideGuest,    // base or end above the guest's addressable window
    kEmptyRange,      // a base with no records: nothing to make a view of
    kUnaligned,       // a base that is not 4-byte aligned
};

[[nodiscard]] const char* SlotVerdictName(SlotVerdict verdict);

struct SlotDecision {
    SlotVerdict verdict = SlotVerdict::kEmpty;
    VSharpView view;      // the decode, whatever the verdict
    std::uint64_t end = 0;  // base + bytes, saturating
};

// The guest's addressable primary window: direct memory sits at [0x100000000, 0x240000000)
// (up to 5 GiB starting at 4 GiB = 9 GiB ceiling), while the GPU alias window
// [0xff00000000, +4 GiB) is checked alongside it in DecideSlot.
inline constexpr std::uint64_t kGuestWindowBytes = (4ull << 30) + kGuestRamBytes;

// `base_limit` is one past the highest address a slot may name. Passed in rather
// than read from the header so a test can pin a smaller window.
[[nodiscard]] SlotDecision DecideSlot(const std::uint32_t words[4],
                                      std::uint64_t base_limit = kGuestWindowBytes);

// Counts, for the log line a refusal is reported with: one line per dispatch
// key, never per slot.
struct SlotReport {
    std::uint32_t bindable = 0;
    std::uint32_t empty = 0;
    std::uint32_t crosses_region = 0;
    std::uint32_t outside_guest = 0;
    std::uint32_t empty_range = 0;
    std::uint32_t unaligned = 0;
    // A slot the table did not have the words for. Its own counter, because it
    // is a fact about the *read*, not about the descriptor.
    std::uint32_t unread = 0;

    void Count(const SlotDecision& decision);
    [[nodiscard]] std::uint32_t slots() const;
    // "3 bindable, 60 null, 1 refused (crosses a 4 GiB region)" - the shape a
    // reviewer can check against the dump without a device.
    [[nodiscard]] std::string Line() const;
};

// A whole table, decided. `words` is the resource table as the guest's memory
// holds it: `slots` V#s of four dwords each, in slot order. `word_count` is what
// the read actually produced, and it is a parameter rather than an assumption:
// a table the dispatch names but the guest has not written (or a length that
// disagrees with the shader's declared count) leaves slots *unread*, and an
// unread slot is refused, never bound from whatever followed in memory. That is
// the one place where being short has to be loud.
[[nodiscard]] SlotReport DecideTable(const std::uint32_t* words, std::uint32_t word_count,
                                     std::uint32_t slots,
                                     std::uint64_t base_limit = kGuestWindowBytes);

}  // namespace bbx::d3d12
