// SPDX-License-Identifier: GPL-3.0-or-later
// tests/guest_vsharp_test.cpp -- which guest buffer a V# names (bbx_guest_vsharp).
//
// Slice 5B-2b's first half. The stakes are asymmetric and that is the reason this
// file exists: a slot left null makes a dispatch read zeroes, which is visible in
// one frame and wrong in a way someone can debug; a slot bound to the wrong range
// makes the shader read another buffer's bytes, which is visible in no frame and
// wrong in a way nobody can find later. So every refusal below is a case, and the
// decode is checked against the translator's own length rule
// (`gcn/translate.h`'s vsharp_range_dwords), not against a comment.

#include <cstdint>

#include "harness.h"
#include "host/d3d12/bbx_guest_mem.h"
#include "host/d3d12/bbx_guest_vsharp.h"

using namespace bbx::d3d12;

namespace {

// A V# as the guest's memory holds one: base in words 0/1[15:0], stride in
// word 1[29:16], num_records in word 2, the format in word 3.
void MakeVSharp(std::uint32_t words[4], std::uint64_t base, std::uint32_t stride, std::uint32_t num_records,
                std::uint32_t word3 = 0) {
    words[0] = static_cast<std::uint32_t>(base & 0xffffffffu);
    words[1] = static_cast<std::uint32_t>(((base >> 32) & 0xffffu) | ((stride & 0x3fffu) << 16));
    words[2] = num_records;
    words[3] = word3;
}

}  // namespace

TEST(GuestVSharp_DecodesBaseStrideAndRecords) {
    std::uint32_t words[4] = {};
    MakeVSharp(words, 0x1'2345'6780ull, 16, 64, 0x000a0facu);
    const VSharpView view = DecodeVSharp(words);
    CHECK_EQ(view.base, 0x1'2345'6780ull);
    CHECK_EQ(view.stride, 16u);
    CHECK_EQ(view.num_records, 64u);
    CHECK_EQ(view.word3, 0x000a0facu);
    // 64 records x 16 bytes.
    CHECK_EQ(view.bytes, 1024ull);
    CHECK(!view.null());
}

TEST(GuestVSharp_ZeroStrideIsAFlatByteRange) {
    std::uint32_t words[4] = {};
    MakeVSharp(words, 0x8000'0000ull, 0, 4096);
    const VSharpView view = DecodeVSharp(words);
    CHECK_EQ(view.bytes, 4096ull);  // num_records are bytes when the stride is 0
}

TEST(GuestVSharp_TheBaseIsFortyBitsBecauseWordOneCarriesEightOfThem) {
    // The guest's addresses are 40-bit, and word 1's low byte is base[39:32]:
    // stride lives at [29:16], so bits 8..15 of word 1 belong to neither. Reading
    // them as base bits would put a buffer's flags into its address.
    std::uint32_t words[4] = {0x1234'5678u, 0xab00'0010u, 0x10, 0};
    const VSharpView view = DecodeVSharp(words);
    CHECK_EQ(view.base, 0x10'1234'5678ull);  // word 1's low byte 0x10 is base[39:32]
    CHECK_EQ(view.stride, 0x2b00u);          // word 1[29:16], and 0xab00 & 0x3fff
}

TEST(GuestVSharp_StrideIsTenBitsAndClampedToFourteen) {
    // Word 1[29:16] is stride; the two bits above it belong to something else.
    std::uint32_t words[4] = {0, 0x4000'0000u | (0x3fffu << 16), 2, 0};
    const VSharpView view = DecodeVSharp(words);
    CHECK_EQ(view.stride, 0x3fffu);  // not 0x40003fff, and not shifted into the base
    CHECK_EQ(view.bytes, 2ull * 0x3fffu);
}

TEST(GuestVSharp_RangeRuleMatchesTheTranslator) {
    // gcn/translate.h: vsharp_range_dwords(w1, w2) = ceil(bytes / 4), where bytes
    // is num_records x stride when the stride is set and num_records otherwise.
    std::uint32_t words[4] = {};
    MakeVSharp(words, 0x1000, 8, 100);
    CHECK_EQ(VSharpRangeDwords(words[1], words[2]), 200ull);  // 800 bytes / 4
    MakeVSharp(words, 0x1000, 0, 801);
    CHECK_EQ(VSharpRangeDwords(words[1], words[2]), 201ull);  // rounds up: a partial dword is in range
}

TEST(GuestVSharp_NullIsNotARefusal) {
    std::uint32_t words[4] = {};
    const SlotDecision decision = DecideSlot(words);
    CHECK(decision.verdict == SlotVerdict::kEmpty);
    CHECK(decision.view.null());
}

TEST(GuestVSharp_ABaseWithNoRecordsIsEmpty) {
    std::uint32_t words[4] = {};
    MakeVSharp(words, 0x4000'0000ull, 0, 0);
    const SlotDecision decision = DecideSlot(words);
    CHECK(decision.verdict == SlotVerdict::kEmptyRange);
}

TEST(GuestVSharp_ALinearRangeInsideAWindowBinds) {
    std::uint32_t words[4] = {};
    MakeVSharp(words, 0x4000'0000ull, 4, 256);
    const SlotDecision decision = DecideSlot(words);
    CHECK(decision.verdict == SlotVerdict::kBindable);
    CHECK_EQ(decision.end, 0x4000'0400ull);
}

TEST(GuestVSharp_TheLastByteOfAWindowBinds) {
    // end == base_limit is inside: the range is [base, end).
    std::uint32_t words[4] = {};
    const std::uint64_t base = kGuestWindowBytes - 256;
    MakeVSharp(words, base, 0, 256);
    const SlotDecision decision = DecideSlot(words);
    CHECK(decision.verdict == SlotVerdict::kBindable);
    CHECK_EQ(decision.end, kGuestWindowBytes);
}

TEST(GuestVSharp_OneBytePastTheWindowIsRefused) {
    // Aligned on purpose: an unaligned base is refused for a different reason,
    // and the case being tested is the window's edge.
    std::uint32_t words[4] = {};
    MakeVSharp(words, kGuestWindowBytes - 252, 0, 256);
    const SlotDecision decision = DecideSlot(words);
    CHECK(decision.verdict == SlotVerdict::kOutsideGuest);
}

TEST(GuestVSharp_ARangeThatCrossesFourGiBIsRefused) {
    // The page table's `va_hi` is a region index with no carry: a range that
    // starts in one region and ends in the next cannot be walked, and mapping
    // half of it would read another slab's bytes.
    std::uint32_t words[4] = {};
    MakeVSharp(words, kL1RegionBytes - 128, 0, 256);
    const SlotDecision decision = DecideSlot(words);
    CHECK(decision.verdict == SlotVerdict::kCrossesRegion);
}

TEST(GuestVSharp_ARangeEndingExactlyOnTheBoundaryDoesNotCross) {
    std::uint32_t words[4] = {};
    MakeVSharp(words, kL1RegionBytes - 256, 0, 256);
    const SlotDecision decision = DecideSlot(words);
    CHECK(decision.verdict == SlotVerdict::kBindable);
}

TEST(GuestVSharp_AnUnalignedBaseIsRefused) {
    std::uint32_t words[4] = {};
    MakeVSharp(words, 0x4000'0002ull, 4, 16);
    const SlotDecision decision = DecideSlot(words);
    CHECK(decision.verdict == SlotVerdict::kUnaligned);
}

TEST(GuestVSharp_ABaseAboveTheWindowIsRefusedEvenWhenShort) {
    // 1 TiB is where the guest's own pointers live; a V# in a dispatch names the
    // mapped slabs, which are far below it. The address is legal for the guest
    // and not addressable by the page table, so it is refused, not truncated.
    std::uint32_t words[4] = {};
    // 8 GiB: representable in a V# (40-bit base) and above the 7.75 GiB window.
    MakeVSharp(words, 0x2'0000'0000ull, 0, 16);
    const SlotDecision decision = DecideSlot(words);
    CHECK(decision.verdict == SlotVerdict::kOutsideGuest);
}

TEST(GuestVSharp_AnOverflowingRangeIsRefusedRatherThanWrapped) {
    // num_records is 32 bits and the stride 14: the product fits, but base + bytes
    // must saturate rather than wrap to a small end that would look bindable.
    std::uint32_t words[4] = {};
    MakeVSharp(words, kGuestWindowBytes - 16, 0x3fffu, 0xffffffffu);
    const SlotDecision decision = DecideSlot(words);
    CHECK(decision.verdict == SlotVerdict::kOutsideGuest);
    CHECK(decision.end >= kGuestWindowBytes);
}

TEST(GuestVSharp_ReportCountsEveryVerdictAndSaysItInOneLine) {
    SlotReport report;
    std::uint32_t words[4] = {};
    MakeVSharp(words, 0x4000'0000ull, 4, 16);
    report.Count(DecideSlot(words));
    MakeVSharp(words, 0x4000'0000ull, 4, 16);
    report.Count(DecideSlot(words));
    std::uint32_t empty[4] = {};
    report.Count(DecideSlot(empty));
    MakeVSharp(words, kL1RegionBytes - 8, 0, 64);
    report.Count(DecideSlot(words));
    CHECK_EQ(report.slots(), 4u);
    CHECK_EQ(report.bindable, 2u);
    CHECK_EQ(report.empty, 1u);
    CHECK_EQ(report.crosses_region, 1u);
    const std::string line = report.Line();
    CHECK(line.find("2 bindable") != std::string::npos);
    CHECK(line.find("1 null", 0) != std::string::npos);
    CHECK(line.find("refused") != std::string::npos);
    CHECK(line.find("cross a 4 GiB region") != std::string::npos);
}

TEST(GuestVSharp_ACleanReportSaysNothingWasRefused) {
    SlotReport report;
    const std::uint32_t empty[4] = {};
    report.Count(DecideSlot(empty));
    const std::string line = report.Line();
    CHECK(line.find("none refused") != std::string::npos);
}

TEST(GuestVSharp_TableShorterThanTheDeclaredCountRefusesTheRest) {
    // A table the guest wrote 3 slots into, a shader that declares 5: the two
    // missing slots must be "unread", never whatever the bytes after the table
    // happen to hold.
    std::uint32_t words[3 * 4] = {};
    for (int slot = 0; slot < 3; ++slot) {
        MakeVSharp(words + slot * 4, 0x4000'0000ull + static_cast<std::uint64_t>(slot) * 0x1000, 4, 64);
    }
    const SlotReport report = DecideTable(words, 3 * 4, 5);
    CHECK_EQ(report.bindable, 3u);
    CHECK_EQ(report.unread, 2u);
    CHECK_EQ(report.slots(), 5u);
    CHECK(report.Line().find("2 unread") != std::string::npos);
}

TEST(GuestVSharp_NoTableAtAllIsEverySlotUnread) {
    const SlotReport report = DecideTable(nullptr, 0, 4);
    CHECK_EQ(report.unread, 4u);
    CHECK_EQ(report.bindable, 0u);
    CHECK_EQ(report.empty, 0u);  // unread is not the same as null
}

TEST(GuestVSharp_ATableOfTheRightLengthBindsEverySlot) {
    std::uint32_t words[4 * 4] = {};
    for (int slot = 0; slot < 4; ++slot) {
        MakeVSharp(words + slot * 4, 0x5000'0000ull + static_cast<std::uint64_t>(slot) * 0x400, 0, 0x400);
    }
    const SlotReport report = DecideTable(words, 4 * 4, 4);
    CHECK_EQ(report.bindable, 4u);
    CHECK_EQ(report.unread, 0u);
    CHECK_EQ(report.slots(), 4u);
}

TEST(GuestVSharp_APartialSlotIsUnreadNotHalfDecoded) {
    // Six dwords: one whole V# and half of the next. The half is not a V#.
    std::uint32_t words[6] = {};
    MakeVSharp(words, 0x6000'0000ull, 0, 0x100);
    const SlotReport report = DecideTable(words, 6, 2);
    CHECK_EQ(report.bindable, 1u);
    CHECK_EQ(report.unread, 1u);
}
