// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/host/core/bbx_redzone.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace bbx::core {
namespace {

#include "src/host/core/bbx_redzone_data.inc"

bool FitsRel32(std::int64_t delta) {
    return delta >= std::numeric_limits<std::int32_t>::min() &&
           delta <= std::numeric_limits<std::int32_t>::max();
}

}  // namespace

RedZonePatchResult ApplyBloodborne109RedZoneTrampolines(
    const void* image_base, std::size_t image_size, std::uint64_t image_slide,
    void* trampoline_buf, std::uint64_t trampoline_va, std::size_t trampoline_cap,
    const std::function<void(std::uint64_t, const void*, std::size_t)>& write_code) {
    RedZonePatchResult res{};
    constexpr std::size_t kSiteCount = sizeof(kRedZoneSites) / sizeof(kRedZoneSites[0]);
    res.total_sites = kSiteCount;

    if (image_base == nullptr || image_size == 0 || trampoline_buf == nullptr ||
        trampoline_cap < 64 || !write_code) {
        return res;
    }

    const auto* img = static_cast<const std::uint8_t*>(image_base);
    auto* tramp = static_cast<std::uint8_t*>(trampoline_buf);
    std::size_t used = 0;

    static constexpr std::uint8_t kLeaDown[] = {0x48, 0x8d, 0x64, 0x24, 0x80};                   // lea rsp, [rsp-0x80]
    static constexpr std::uint8_t kLeaUp[] = {0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00};   // lea rsp, [rsp+0x80]

    std::vector<std::uint8_t> code;
    code.reserve(256);
    std::vector<std::uint8_t> patch;
    patch.reserve(64);

    for (std::size_t i = 0; i < kSiteCount; ++i) {
        const RedZoneSiteEntry& site = kRedZoneSites[i];
        const std::size_t off = site.offset;
        const std::size_t len = site.bytes_len;
        if (len < 5 || off + len > image_size) {
            ++res.skipped_range;
            continue;
        }
        const std::uint8_t* expected = kRedZoneBytes + site.bytes_off;
        if (std::memcmp(img + off, expected, len) != 0) {
            // Already patched by bbhost or non-matching binary; never overwrite.
            ++res.skipped_mismatch;
            continue;
        }

        // Build the trampoline: wrap each red-zone-touching instruction (len < 0)
        // with lea rsp, [rsp-0x80] / insn / lea rsp, [rsp+0x80], then jump back.
        code.clear();
        std::size_t pos = 0;
        bool lengths_ok = true;
        for (std::uint16_t k = 0; k < site.lens_count; ++k) {
            const int signed_len = kRedZoneLens[site.lens_off + k];
            const std::size_t n = static_cast<std::size_t>(signed_len < 0 ? -signed_len : signed_len);
            if (n == 0 || pos + n > len) {
                lengths_ok = false;
                break;
            }
            if (signed_len < 0) code.insert(code.end(), std::begin(kLeaDown), std::end(kLeaDown));
            code.insert(code.end(), img + off + pos, img + off + pos + n);
            if (signed_len < 0) code.insert(code.end(), std::begin(kLeaUp), std::end(kLeaUp));
            pos += n;
        }
        if (!lengths_ok || pos != len) {
            ++res.skipped_range;
            continue;
        }

        const std::uint64_t here_va = trampoline_va + used;
        const std::uint64_t site_va = image_slide + off;
        const std::uint64_t resume_va = site_va + len;
        const std::int64_t fwd_delta =
            static_cast<std::int64_t>(here_va) - static_cast<std::int64_t>(site_va + 5);
        const std::int64_t back_delta =
            static_cast<std::int64_t>(resume_va) - static_cast<std::int64_t>(here_va + code.size() + 5);
        if (!FitsRel32(fwd_delta) || !FitsRel32(back_delta)) {
            ++res.skipped_range;
            continue;
        }

        code.push_back(0xe9);
        const std::int32_t back_rel = static_cast<std::int32_t>(back_delta);
        const auto* back_bytes = reinterpret_cast<const std::uint8_t*>(&back_rel);
        code.insert(code.end(), back_bytes, back_bytes + 4);
        const std::size_t pad = (16u - (code.size() % 16u)) % 16u;
        code.insert(code.end(), pad, 0xcc);

        if (used + code.size() > trampoline_cap) {
            ++res.skipped_range;
            continue;
        }
        std::memcpy(tramp + used, code.data(), code.size());
        used += code.size();

        patch.assign(len, 0xcc);
        patch[0] = 0xe9;
        const std::int32_t fwd_rel = static_cast<std::int32_t>(fwd_delta);
        std::memcpy(patch.data() + 1, &fwd_rel, sizeof(fwd_rel));
        write_code(site_va, patch.data(), patch.size());
        ++res.applied;
    }

    res.trampoline_bytes = used;
    return res;
}

}  // namespace bbx::core
