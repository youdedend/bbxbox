// SPDX-License-Identifier: GPL-3.0-or-later
// GCN surface formats as DXGI formats.
//
// The game describes a texture with a GCN "T#" resource, whose DFMT (data
// format) and NFMT (numeric format) name the layout and the interpretation.
// bbhost's Vulkan host maps the same pair to a VkFormat (src/host/textures.cpp
// image_format); this is the D3D12 half of that table and it must agree with
// it value for value, because a texture that is detiled as one format and read
// as another is a picture that is subtly wrong rather than an error.
//
// The numeric DXGI values are the SDK's, repeated here so this file compiles
// on a build machine with no GDK (the tests run there).
#pragma once

#include <cstdint>

namespace bbx::d3d12 {

enum class DxgiFormat : std::uint32_t {
    kUnknown = 0,
    kR32G32B32A32Float = 2,
    kR32G32B32A32Uint = 3,
    kR32G32B32A32Sint = 4,
    kR32G32B32Float = 6,
    kR32G32B32Uint = 7,
    kR32G32B32Sint = 8,
    kR16G16B16A16Float = 10,
    kR16G16B16A16Unorm = 11,
    kR16G16B16A16Uint = 12,
    kR16G16B16A16Snorm = 13,
    kR16G16B16A16Sint = 14,
    kR32G32Float = 16,
    kR32G32Uint = 17,
    kR32G32Sint = 18,
    kR32G32B32A32Typeless = 1,
    kR10G10B10A2Unorm = 24,
    kR10G10B10A2Uint = 25,
    kR11G11B10Float = 26,
    kR8G8B8A8Unorm = 28,
    kR8G8B8A8UnormSrgb = 29,
    kR8G8B8A8Uint = 30,
    kR8G8B8A8Snorm = 31,
    kR8G8B8A8Sint = 32,
    kR16G16Float = 34,
    kR16G16Unorm = 35,
    kR16G16Uint = 36,
    kR16G16Snorm = 37,
    kR16G16Sint = 38,
    kR32Float = 41,
    kR32Uint = 42,
    kR32Sint = 43,
    kR8G8Unorm = 49,
    kR8G8Uint = 50,
    kR8G8Snorm = 51,
    kR8G8Sint = 52,
    kR16Float = 54,
    kR16Unorm = 56,
    kR16Uint = 57,
    kR16Snorm = 58,
    kR16Sint = 59,
    kR8Unorm = 61,
    kR8Uint = 62,
    kR8Snorm = 63,
    kR8Sint = 64,
    kR9G9B9E5SharedExp = 67,
    kD16Unorm = 55,
    kD32Float = 40,
    kD24UnormS8Uint = 45,
    kD32FloatS8X24Uint = 20,
    kBc1Unorm = 71,
    kBc1UnormSrgb = 72,
    kBc2Unorm = 74,
    kBc2UnormSrgb = 75,
    kBc3Unorm = 77,
    kBc3UnormSrgb = 78,
    kBc4Unorm = 80,
    kBc4Snorm = 81,
    kBc5Unorm = 83,
    kBc5Snorm = 84,
    kB5G6R5Unorm = 85,
    kB5G5R5A1Unorm = 86,
    kB8G8R8A8Unorm = 87,
    kB8G8R8A8UnormSrgb = 91,
    kB8G8R8X8Unorm = 88,
    kBc6hUf16 = 95,
    kBc6hSf16 = 96,
    kBc7Unorm = 98,
    kBc7UnormSrgb = 99,
};

// How a format is read: what the texture's descriptor becomes, and what the
// detile shader does with it.
enum class FormatClass : std::uint8_t {
    kUnknown = 0,
    kUnorm,
    kSnorm,
    kUint,
    kSint,
    kFloat,
    kSharedExp,  // R11G11B10 / R9G9B9E5: float semantics, no sign bit tricks
    kDepth,
    kDepthStencil,
};

struct FormatInfo {
    DxgiFormat format = DxgiFormat::kUnknown;
    // GCN's T# may swap R and B relative to the DXGI layout (comp_swap): the
    // port asks the shader to swizzle instead of copying the texture, because a
    // copy of a 4K texture is not in the budget.
    bool rb_swap = false;
    FormatClass cls = FormatClass::kUnknown;
    std::uint8_t bytes_per_block = 0;  // a block is one texel unless compressed
    std::uint8_t block_width = 1;      // 4 for BCn
    std::uint8_t block_height = 1;     // 4 for BCn
    bool srgb = false;
    bool compressed = false;

    [[nodiscard]] constexpr bool valid() const { return format != DxgiFormat::kUnknown; }
    [[nodiscard]] constexpr bool depth() const {
        return cls == FormatClass::kDepth || cls == FormatClass::kDepthStencil;
    }
};

// A GCN T# format pair. `dfmt` and `nfmt` are the T#'s fields (bits 20:26 and
// 26:29 in word 1), which bbhost names the same way.
[[nodiscard]] FormatInfo FromGcnFormat(std::uint32_t dfmt, std::uint32_t nfmt,
                                       std::uint32_t comp_swap = 0);

// GCN's DB_Z_INFO FORMAT field (regs_depth.h) as a DXGI depth format.
// z_info's Z_FORMAT field is two bits (bbhost src/host/render.cpp db_format):
//   0 -> no depth; with stencil, D32_FLOAT_S8X24 (the stencil plane is the
//        depth target's, so there is nothing else to attach)
//   1 -> Z_16: D16_UNORM, or D24_UNORM_S8_UINT with stencil
//   2, 3 -> Z_32: D32_FLOAT, or D32_FLOAT_S8X24 with stencil
// D24S8 is what bbhost uses for a Z_16 target with stencil where the device
// can render to it; Xbox can, so the port does not need the D32S8 fallback.
[[nodiscard]] FormatInfo FromDepthFormat(std::uint32_t z_format, bool stencil_enabled);

// Bytes a texture of this format and extent occupies, respecting block
// rounding. `slices` covers arrays, cubes and mips' array layers.
[[nodiscard]] std::uint64_t TextureBytes(FormatInfo info, std::uint32_t width,
                                         std::uint32_t height, std::uint32_t slices,
                                         std::uint32_t mips);

}  // namespace bbx::d3d12
