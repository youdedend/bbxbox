// SPDX-License-Identifier: GPL-3.0-or-later
// The guest's scanout surface: what sceVideoOutSetBufferAttribute states about
// a display buffer's pixels, and what the present pass must do with them before
// it samples the frame.
//
// The format names are the console's, and they are not the memory order.
// "A8R8G8B8" is a dword read from the most significant channel down: alpha in
// bits 24-31, red in 16-23, green in 8-15, blue in 0-7. Little-endian, those
// bytes are B, G, R, A - DXGI's B8G8R8A8, not R8G8B8A8. Reading the name as the
// byte order swaps red and blue, and the first frame says so: every menu bar
// the game draws red comes out blue, and no amount of shader debugging finds it
// because the shader is right and the image was made in the wrong format.
//
// Tiling is the other half. A buffer registered with TILING_MODE_TILE holds the
// micro-tiled layout the GCN tile unit writes, so the present pass cannot copy
// it straight: it is untiled first (bbx_untile's plan, run by UntileCs) and the
// linear result is what the frame image is made from.
#pragma once

#include <cstdint>
#include <string>

#include "host/d3d12/bbx_format.h"
#include "host/d3d12/bbx_untile.h"

namespace bbx::d3d12 {

// sceVideoOutPixelFormat. The top bit says a display format; the low bits are
// the console's own enumeration, which is stable across titles because it is
// what they were compiled against.
enum class ScanoutFormat : std::uint32_t {
    kA8R8G8B8Srgb = 0x80000000u,
    kB8R8G8A8Srgb = 0x80000001u,
    kA8R8G8B8 = 0x80000002u,
    kB8R8G8A8 = 0x80000003u,
    kR8G8B8A8Srgb = 0x80000004u,
    kR8G8B8A8 = 0x80000005u,
    kA2R10G10B10 = 0x80000006u,
    kA2R10G10B10Srgb = 0x80000007u,
    kA2B10G10R10 = 0x80000008u,
    kA2B10G10R10Srgb = 0x80000009u,
};

// sceVideoOutSetBufferAttribute's tilingMode.
enum class ScanoutTiling : std::uint32_t {
    kLinear = 0,  // rows of pixels, as the display controller scans them out
    kTile = 1,    // GCN micro tiles: the render target's own layout
};

struct ScanoutDesc {
    ScanoutFormat format = ScanoutFormat::kA8R8G8B8;
    ScanoutTiling tiling = ScanoutTiling::kLinear;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;  // bytes per row as the guest states it; 0 is packed
};

// What one display buffer means to the presenter.
struct ScanoutState {
    bool ok = false;
    std::string reason;  // why not, when a buffer cannot be presented
    DxgiFormat format = DxgiFormat::kUnknown;  // the frame image's own format
    bool srgb = false;
    bool tiled = false;
    std::uint32_t row_pitch = 0;      // bytes per row in the guest's buffer
    std::uint32_t dst_row_pitch = 0;  // bytes per row after the detile (256-aligned)
    std::uint64_t bytes = 0;          // how much of the guest's buffer the copy reads
    UntilePlan plan;                  // the detile dispatches, when tiled
};

// The console's name for a format, spelled as its header spells it.
[[nodiscard]] const char* ScanoutFormatName(ScanoutFormat format);
[[nodiscard]] const char* ScanoutTilingName(ScanoutTiling tiling);

// Resolves a display buffer's attribute into the image the present pass makes,
// the row pitch of the guest's own bytes, and - when the guest asked for tiling
// - the detile plan that linearises them. A format with no DXGI layout the
// present pass can sample is refused by name rather than presented as another
// format: a wrong picture is worse than a named one.
[[nodiscard]] ScanoutState DescribeScanout(const ScanoutDesc& desc);

}  // namespace bbx::d3d12
