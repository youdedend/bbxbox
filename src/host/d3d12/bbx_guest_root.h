// SPDX-License-Identifier: GPL-3.0-or-later
// The guest shaders' root signature, as data: what the console's baked DXIL
// names, in the registers and spaces a console 1.0 signature can hand out.
//
// The shaders are the contract. With `dxil_sm64` the prebake prints every
// binding as a register and a space (gcn/hlsl.cpp rewrites Vulkan's
// `vk::binding(n, s)` into `register(xn, spaceS)`), from two sources:
//
//   * the translator's stage params - `ConstantBuffer<params_t> params :
//     register(b0, space1)` - because the console's fixed descriptor set is 1
//     (ConsoleStageOptions::kDescriptorSet in bbx_shader_cache.h);
//   * the guest memory's two tables - `t0, space3` and `u0..u31, space3` - from
//     the console's guest-memory lowering (bbx_guest_mem.h, ARCHITECTURE §3.3).
//
// So the signature is small: a root CBV for the parameters, one table for the
// stage's own resources in space 1 (SRV, UAV and sampler ranges, each from
// register 0, since a guest shader's registers start at 0 and its highest one is
// bounded here rather than in the shader), and one table in space 3 carrying the
// page table and the 32 slabs. Four DWORDs against the console's ceiling of 64.
//
// This file is portable on purpose: the layout is checked on the build machine
// (tests/guest_mem_test.cpp) and `pipeline.cpp` turns exactly these numbers into
// D3D12 descriptors, so a table that does not match the shaders fails a test
// rather than a dispatch.
#pragma once

#include <cstdint>
#include <string>

#include "host/d3d12/bbx_guest_mem.h"

namespace bbx::d3d12 {

// StageParams: one constant buffer, `b0` in the console's descriptor set.
inline constexpr std::uint32_t kGuestParamsSpace = 1;
inline constexpr std::uint32_t kGuestParamsRegister = 0;
// gcn/translate.h StageParams: 448 bytes of members (through cb_w3), padded to
// the 512 a constant buffer rounds to. The shader reads it as `params`, so the
// root CBV's size has to be at least this or DXC's layout and the root's differ.
inline constexpr std::uint32_t kStageParamsBytes = 512;

// The stage's own resources, in the same space: the bounds a guest shader's
// register numbers are held to. A shader that names t64 stops the bake by name
// (it would read a descriptor the table does not carry), which is the right
// failure: a bigger table would be a guess about Bloodborne's largest PS.
inline constexpr std::uint32_t kGuestSrvCount = 64;
inline constexpr std::uint32_t kGuestUavCount = 8;
inline constexpr std::uint32_t kGuestSamplerCount = 16;

// A descriptor range in the terms D3D12 uses, without D3D12's enum so the build
// machine can hold it.
enum class GuestRangeKind : std::uint8_t { kSrv = 0, kUav, kSampler };
[[nodiscard]] const char* GuestRangeName(GuestRangeKind kind);

struct GuestRange {
    GuestRangeKind kind = GuestRangeKind::kSrv;
    std::uint32_t base_register = 0;
    std::uint32_t count = 1;
    std::uint32_t space = 0;
    [[nodiscard]] std::uint32_t LastRegister() const { return base_register + count - 1; }
    [[nodiscard]] bool Covers(std::uint32_t reg, std::uint32_t in_space) const {
        return space == in_space && reg >= base_register && reg <= LastRegister();
    }
};

enum class GuestRootParamKind : std::uint8_t {
    kParamsCbv = 0,    // the root CBV: StageParams at b0
    kStageResources,   // space 1: the ranges below
    kGuestMemory,      // space 3: the page table and the slabs
};

struct GuestRootParam {
    GuestRootParamKind kind = GuestRootParamKind::kParamsCbv;
    // The ranges this parameter's table carries; empty for the root CBV.
    std::vector<GuestRange> ranges;
    // What D3D12 charges for it: a root descriptor is 2 DWORDs, a table is 1.
    [[nodiscard]] std::uint32_t dwords() const { return kind == GuestRootParamKind::kParamsCbv ? 2u : 1u; }
};

struct GuestRootLayout {
    std::vector<GuestRootParam> params;
    // The console's ceiling: a root signature is at most 64 DWORDs, and 1.1's
    // larger limit is not on the console.
    static constexpr std::uint32_t kMaxDwords = 64;
    [[nodiscard]] std::uint32_t Dwords() const {
        std::uint32_t total = 0;
        for (const GuestRootParam& p : params) total += p.dwords();
        return total;
    }
    [[nodiscard]] bool Valid() const { return !params.empty() && Dwords() <= kMaxDwords; }
    // The range that would serve `register` in `space`, or nullptr.
    [[nodiscard]] const GuestRange* Find(GuestRangeKind kind, std::uint32_t reg, std::uint32_t space) const;
    [[nodiscard]] std::string Describe() const;
};

// The one layout the console's guest shaders are baked against.
[[nodiscard]] GuestRootLayout GuestRoot();

// Where a baked shader's binding lands, for the binder: false when the layout has
// no range for it (the shader names a register the signature does not carry, and
// the dispatch must refuse rather than read a stale descriptor).
struct GuestBindingSlot {
    GuestRootParamKind param = GuestRootParamKind::kGuestMemory;
    std::uint32_t range = 0;       // index into that parameter's ranges
    std::uint32_t offset = 0;      // descriptors from the table's start
};
bool GuestBinding(const GuestRootLayout& layout, GuestRangeKind kind, std::uint32_t reg, std::uint32_t space,
                  GuestBindingSlot* out);

}  // namespace bbx::d3d12
