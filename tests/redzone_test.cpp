// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "src/host/core/bbx_redzone.h"

#include <cstdint>
#include <cstring>
#include <vector>

using bbx::core::ApplyBloodborne109RedZoneTrampolines;
using bbx::core::kRedZoneTrampolineCapacity;
using bbx::core::RedZonePatchResult;

namespace {

// Site 8 from Bloodborne v1.09 red-zone data:
// offset = 0x1438f, 14 bytes (bytes_off = 54, lens = [5, -5, 4]):
//   46 89 14 a9        (4 bytes? wait: 46 89 14 a9 48 -> no: 54..68 in kRedZoneBytes)
// Let's test Site 0 at offset = 0x140c0 (7 bytes: 66 42 c7 04 69 28 00, lens = [-7])
// and Site 8 at offset = 0x1438f (14 bytes: 46 89 14 a9 48 8b 4c 24 d0 42 c6 04 29 00, lens = [4, -5, 5]).
constexpr std::size_t kSite0Offset = 0x140c0;
constexpr std::uint8_t kSite0Bytes[7] = {0x66, 0x42, 0xc7, 0x04, 0x69, 0x28, 0x00};

constexpr std::size_t kSite8Offset = 0x1438f;
constexpr std::uint8_t kSite8Bytes[14] = {
    0x46, 0x89, 0x14, 0xa9, 0x48, 0x8b, 0x4c, 0x24, 0xd0, 0x42, 0xc6, 0x04, 0x29, 0x00};

}  // namespace

TEST(RedZone_PatchesMatchingSiteAndWrapsRedZoneInstruction) {
    constexpr std::uint64_t kSlide = 0x300000000ull;
    std::vector<std::uint8_t> image(kSite8Offset + 0x100, 0x90);
    std::memcpy(image.data() + kSite0Offset, kSite0Bytes, sizeof(kSite0Bytes));
    std::memcpy(image.data() + kSite8Offset, kSite8Bytes, sizeof(kSite8Bytes));

    std::vector<std::uint8_t> tramp(kRedZoneTrampolineCapacity, 0);
    const std::uint64_t tramp_va = kSlide + 0x1000000ull;

    const RedZonePatchResult res = ApplyBloodborne109RedZoneTrampolines(
        image.data(), image.size(), kSlide, tramp.data(), tramp_va, tramp.size(),
        [&image, kSlide](std::uint64_t va, const void* bytes, std::size_t len) {
            std::memcpy(image.data() + (va - kSlide), bytes, len);
        });

    CHECK_EQ(res.total_sites, 607u);
    CHECK_EQ(res.applied, 2u);
    CHECK(res.trampoline_bytes >= 32u);

    // Site 0 now starts with E9 rel32 pointing to tramp_va, padded with 0xCC.
    CHECK_EQ(image[kSite0Offset], 0xe9u);
    std::int32_t rel = 0;
    std::memcpy(&rel, image.data() + kSite0Offset + 1, sizeof(rel));
    const std::uint64_t target_va =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(kSlide + kSite0Offset + 5) + rel);
    CHECK_EQ(target_va, tramp_va);
    CHECK_EQ(image[kSite0Offset + 5], 0xccu);
    CHECK_EQ(image[kSite0Offset + sizeof(kSite0Bytes) - 1], 0xccu);

    // The trampoline contains `lea rsp, [rsp-0x80]` (48 8d 64 24 80) before
    // Site 0's red-zone instruction (66 42 c7 04 69 28 00) and
    // `lea rsp, [rsp+0x80]` (48 8d a4 24 80 00 00 00) immediately after it.
    static constexpr std::uint8_t kExpectedWrapped[] = {
        0x48, 0x8d, 0x64, 0x24, 0x80,                         // lea rsp, [rsp-0x80]
        0x66, 0x42, 0xc7, 0x04, 0x69, 0x28, 0x00,             // mov word ptr [rcx+r13*2], 0x28
        0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00        // lea rsp, [rsp+0x80]
    };
    bool found_wrapped = false;
    for (std::size_t i = 0; i + sizeof(kExpectedWrapped) <= res.trampoline_bytes; ++i) {
        if (std::memcmp(tramp.data() + i, kExpectedWrapped, sizeof(kExpectedWrapped)) == 0) {
            found_wrapped = true;
            break;
        }
    }
    CHECK(found_wrapped);
}

TEST(RedZone_SkipsNonMatchingOrOutOfRangeSites) {
    constexpr std::uint64_t kSlide = 0x300000000ull;
    // Size covers only site 0 (0x140c0 + 7), while site 1 starts at 0x140d7.
    std::vector<std::uint8_t> image(kSite0Offset + sizeof(kSite0Bytes), 0x90);
    std::vector<std::uint8_t> tramp(kRedZoneTrampolineCapacity, 0);

    // Non-matching bytes for site 0 -> 1 skipped_mismatch, 606 skipped_range.
    const RedZonePatchResult res_mismatch = ApplyBloodborne109RedZoneTrampolines(
        image.data(), image.size(), kSlide, tramp.data(), kSlide + 0x1000000ull, tramp.size(),
        [](std::uint64_t, const void*, std::size_t) {});
    CHECK_EQ(res_mismatch.applied, 0u);
    CHECK_EQ(res_mismatch.skipped_mismatch, 1u);
    CHECK_EQ(res_mismatch.skipped_range, 606u);

    // Matching bytes for site 0, but trampoline >2 GiB away -> all 607 skipped_range.
    std::memcpy(image.data() + kSite0Offset, kSite0Bytes, sizeof(kSite0Bytes));
    const RedZonePatchResult res_far = ApplyBloodborne109RedZoneTrampolines(
        image.data(), image.size(), kSlide, tramp.data(), kSlide + 0x100000000ull, tramp.size(),
        [](std::uint64_t, const void*, std::size_t) {});
    CHECK_EQ(res_far.applied, 0u);
    CHECK_EQ(res_far.skipped_range, 607u);
}
