// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/host/d3d12/bbx_guest_vsharp.h"

#include <cstdio>

#include "src/host/d3d12/bbx_guest_mem.h"

namespace bbx::d3d12 {

VSharpView DecodeVSharp(const std::uint32_t words[4]) {
    VSharpView view;
    view.base = static_cast<std::uint64_t>(words[0]) | (static_cast<std::uint64_t>(words[1] & 0xffffu) << 32);
    view.stride = (words[1] >> 16) & 0x3fffu;
    view.num_records = words[2];
    view.word3 = words[3];
    view.bytes = view.stride != 0 ? static_cast<std::uint64_t>(view.num_records) * view.stride
                                  : static_cast<std::uint64_t>(view.num_records);
    return view;
}

std::uint64_t VSharpRangeDwords(std::uint32_t w1, std::uint32_t w2) {
    const std::uint32_t stride = (w1 >> 16) & 0x3fffu;
    const std::uint64_t bytes = stride ? std::uint64_t{w2} * stride : std::uint64_t{w2};
    return (bytes + 3) / 4;
}

const char* SlotVerdictName(SlotVerdict verdict) {
    switch (verdict) {
        case SlotVerdict::kBindable: return "bindable";
        case SlotVerdict::kEmpty: return "null";
        case SlotVerdict::kCrossesRegion: return "crosses a 4 GiB region";
        case SlotVerdict::kOutsideGuest: return "outside the guest window";
        case SlotVerdict::kEmptyRange: return "no records";
        case SlotVerdict::kUnaligned: return "unaligned base";
    }
    return "?";
}

SlotDecision DecideSlot(const std::uint32_t words[4], std::uint64_t base_limit) {
    SlotDecision decision;
    decision.view = DecodeVSharp(words);
    const VSharpView& view = decision.view;
    const std::uint64_t bytes = view.bytes;
    decision.end = view.base + bytes < view.base ? ~std::uint64_t{0} : view.base + bytes;

    // A null V# is not a refusal: it is what the slot already contains, and GCN
    // reads zeroes from it.
    if (view.null()) {
        decision.verdict = SlotVerdict::kEmpty;
        return decision;
    }
    if (bytes == 0) {
        decision.verdict = SlotVerdict::kEmptyRange;
        return decision;
    }
    // 4-byte aligned: the slabs and every view this port makes are dword
    // addressed, and a byte-misaligned base cannot be expressed as a
    // ByteAddressBuffer offset (bbx_guest_mem.h's contract).
    if ((view.base & 3u) != 0) {
        decision.verdict = SlotVerdict::kUnaligned;
        return decision;
    }
    // The guest's window, first: an address above it is not a range question.
    if (view.base >= base_limit || decision.end > base_limit) {
        decision.verdict = SlotVerdict::kOutsideGuest;
        return decision;
    }
    // ... then the region rule. `va_hi` is a dword index in the shader with no
    // carry, so a range that starts in region N and ends in N+1 cannot be walked.
    const std::uint64_t first_region = view.base / kL1RegionBytes;
    const std::uint64_t last_region = (decision.end - 1) / kL1RegionBytes;
    if (first_region != last_region) {
        decision.verdict = SlotVerdict::kCrossesRegion;
        return decision;
    }
    decision.verdict = SlotVerdict::kBindable;
    return decision;
}

SlotReport DecideTable(const std::uint32_t* words, std::uint32_t word_count, std::uint32_t slots,
                       std::uint64_t base_limit) {
    SlotReport report;
    const std::uint32_t readable = words == nullptr ? 0 : word_count / 4;
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        if (slot >= readable) {
            // Not a null V#: nothing was read, so nothing may be bound, and the
            // difference has to survive into the log.
            ++report.unread;
            continue;
        }
        report.Count(DecideSlot(words + static_cast<std::size_t>(slot) * 4, base_limit));
    }
    return report;
}

void SlotReport::Count(const SlotDecision& decision) {
    switch (decision.verdict) {
        case SlotVerdict::kBindable: ++bindable; break;
        case SlotVerdict::kEmpty: ++empty; break;
        case SlotVerdict::kCrossesRegion: ++crosses_region; break;
        case SlotVerdict::kOutsideGuest: ++outside_guest; break;
        case SlotVerdict::kEmptyRange: ++empty_range; break;
        case SlotVerdict::kUnaligned: ++unaligned; break;
    }
}

std::uint32_t SlotReport::slots() const {
    return bindable + empty + crosses_region + outside_guest + empty_range + unaligned + unread;
}

std::string SlotReport::Line() const {
    const std::uint32_t refused = crosses_region + outside_guest + empty_range + unaligned;
    char line[192] = {};
    if (unread != 0) {
        std::snprintf(line, sizeof(line), "%u bindable, %u null, %u refused, %u unread (the table is shorter than the shader's count)",
                      bindable, empty, refused, unread);
        return line;
    }
    if (refused == 0) {
        std::snprintf(line, sizeof(line), "%u bindable, %u null, none refused", bindable, empty);
    } else {
        std::snprintf(line, sizeof(line), "%u bindable, %u null, %u refused (%u cross a 4 GiB region, %u outside, %u empty, %u unaligned)",
                      bindable, empty, refused, crosses_region, outside_guest, empty_range, unaligned);
    }
    return line;
}

}  // namespace bbx::d3d12
