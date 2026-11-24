// SPDX-License-Identifier: GPL-3.0-or-later
// Windows x64 red-zone leaf function trampoline patcher for Bloodborne v1.09.
//
// PS4 System V AMD64 leaf functions legally store locals in [rsp-128..rsp-1].
// On Windows x64, every hardware exception/VEH dispatch (including every
// SEC_RESERVE demand-commit fault) writes EXCEPTION_RECORD/CONTEXT directly
// below RSP, silently corrupting leaf locals unless those stores move RSP
// down by 0x80 bytes around the red-zone instruction.
//
// This module embeds the verified 607 Bloodborne v1.09 leaf sites (identical
// RVAs across CUSA00900 and CUSA03173 v1.09) and emits W^X-safe trampolines
// into a caller-supplied executable buffer within +-2 GiB of the guest image.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace bbx::core {

struct RedZonePatchResult {
    std::size_t total_sites = 0;
    std::size_t applied = 0;
    std::size_t skipped_mismatch = 0;
    std::size_t skipped_range = 0;
    std::size_t trampoline_bytes = 0;
};

// Recommended buffer capacity for the near-code trampoline block (128 KiB holds
// all 607 trampolines with comfortable headroom).
inline constexpr std::size_t kRedZoneTrampolineCapacity = 128u * 1024u;

// Applies all matching Bloodborne v1.09 red-zone trampolines.
//   image_base/image_size: writable (or page-unprotectable via `unlock_page`)
//       guest ELF image mapped at `image_slide`.
//   trampoline_buf/trampoline_va/trampoline_cap: writable memory block whose
//       runtime VA `trampoline_va` is within +-2 GiB of [image_slide, image_slide + image_size).
//   write_code: callback `(uint64_t va, const void* bytes, size_t len)` that
//       writes patched jump bytes into the guest image (handling W^X page flips).
[[nodiscard]] RedZonePatchResult ApplyBloodborne109RedZoneTrampolines(
    const void* image_base, std::size_t image_size, std::uint64_t image_slide,
    void* trampoline_buf, std::uint64_t trampoline_va, std::size_t trampoline_cap,
    const std::function<void(std::uint64_t, const void*, std::size_t)>& write_code);

}  // namespace bbx::core
