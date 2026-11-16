// SPDX-License-Identifier: GPL-3.0-or-later
// The guest shaders' root signatures, as data: what the console's baked DXIL
// names, in the registers, spaces and tables a console 1.0 signature can hand
// out.
//
// The shaders are the contract. With `dxil_sm64` the prebake prints every
// binding as a register and a space (gcn/hlsl.cpp rewrites Vulkan's
// `vk::binding(n, s)` into `register(xn, spaceS)`), from three sources:
//
//   * the translator's stage params - `ConstantBuffer<params_t> params :
//     register(b0, space1)` - because the console's fixed descriptor set is 1
//     (ConsoleStageOptions::kDescriptorSet in bbx_shader_cache.h);
//   * the stage's own Vulkan bindings, flattened into space 1: a constant
//     buffer becomes `register(bN, space1)` (`kCbSsbo = false`, so a constant
//     buffer stays a constant buffer), an image or a buffer `register(tN,
//     space1)` / `register(uN, space1)`, a sampler `register(sN, space1)`. N is
//     the Vulkan *binding number*, which is why the ranges below start at 0 and
//     are as wide as a guest shader can reach rather than as wide as one shader
//     uses: two programs' t3 are different resources in the same register;
//   * the guest memory's two tables - `t0, space3` and `u0..u31, space3` - from
//     the console's guest-memory lowering (bbx_guest_mem.h, ARCHITECTURE §3.3).
//
// ---------------------------------------------------------------------------
// The four rules this file exists to satisfy (they are hardware rules, not
// taste: D3D12 refuses a signature that breaks the first, and a driver that
// accepts the second corrupts the frame)
//
//  1. **A descriptor table is all samplers or no samplers.**
//     D3D12SerializeRootSignature rejects a table that mixes a SAMPLER range
//     with CBV/SRV/UAV ranges, so `s0..s63` has a table of its own in every
//     signature. Sampler descriptors are not in the CBV/SRV/UAV heap at all:
//     they have their own heap, and finding them in the same table is a
//     validation error at PSO creation rather than at draw.
//
//  2. **CBV, SRV and UAV may share a table; they may not share a register
//     file.** `b1..b63`, `t0..t63` and `u0..u63` are three ranges because a
//     guest shader's flat Vulkan binding numbers collide across classes - its
//     binding 3 may be a constant buffer in one program and a sampled image in
//     the next - and a signature that carried only one class would refuse the
//     other by name. `b0` is absent here on purpose: it is the root CBV above,
//     and a table that also declared b0 would be a second view of one register.
//
//  3. **A vertex/pixel draw needs both stages' space-1 tables, and one root
//     parameter has one visibility.** So the graphics signature carries the
//     vertex stage's and the pixel stage's tables separately (VISIBILITY_VERTEX
//     and VISIBILITY_PIXEL): sharing one table would bind the VS's t0 and the
//     PS's t0 to the same descriptor, which is a wrong texture under load
//     rather than an error. The compute signature has one stage, so its tables
//     are VISIBILITY_ALL. Both signatures are built from the same layout and
//     the same baked DXIL: only the visibility differs.
//
//  4. **`SetDescriptorHeaps` takes one CBV/SRV/UAV heap, and
//     `CopyDescriptorsSimple` refuses a SHADER_VISIBLE source.** The guest
//     arena this file describes is therefore an *index space*, not a heap: the
//     views are made once into a CPU-visible (FLAG_NONE) heap and copied into
//     the device's single shader-visible heap, and the dispatcher's tables are
//     GPU handles at those offsets. bbx_guest_mem.cpp owns the copy;
//     `GuestHeapLayout` below is the arithmetic both sides use.
// ---------------------------------------------------------------------------
//
// This file is portable on purpose: the layout is checked on the build machine
// (tests/guest_mem_test.cpp) and `pipeline.cpp` turns exactly these numbers into
// D3D12 descriptors, so a table that does not match the shaders fails a test
// rather than a dispatch.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/host/d3d12/bbx_guest_mem.h"

namespace bbx::d3d12 {

enum class GuestStage : std::uint8_t { kCompute = 0, kGraphics };
[[nodiscard]] const char* GuestStageName(GuestStage stage);

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
inline constexpr std::uint32_t kGuestCbvCount = 63;    // b1..b63 (b0 is the root CBV)
inline constexpr std::uint32_t kGuestSrvCount = 64;    // t0..t63
inline constexpr std::uint32_t kGuestUavCount = 64;    // u0..u63
inline constexpr std::uint32_t kGuestSamplerCount = 64;  // s0..s63

// What the four rules above make a table's shape: the samplers alone, and the
// other three classes together.
enum class GuestRangeKind : std::uint8_t { kCbv = 0, kSrv, kUav, kSampler };
[[nodiscard]] const char* GuestRangeName(GuestRangeKind kind);
// Whether a kind lives in the sampler heap (rule 1) or the CBV/SRV/UAV heap.
[[nodiscard]] bool GuestRangeIsSampler(GuestRangeKind kind);

// Which stages a root parameter is visible to. D3D12's own enum, minus the
// values a console signature has no use for (no geometry, mesh, amplification or
// hull/domain stage is baked).
enum class GuestVisibility : std::uint8_t { kAll = 0, kVertex, kPixel };
[[nodiscard]] const char* GuestVisibilityName(GuestVisibility v);

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
    kParamsCbv = 0,   // the root CBV: StageParams at b0, space1
    kStageResources,  // space 1: b1..b63, t0..t63, u0..u63 (rule 2)
    kStageSamplers,   // space 1: s0..s63, alone (rule 1)
    kGuestMemory,     // space 3: the page table and the 32 slabs
};

struct GuestRootParam {
    GuestRootParamKind kind = GuestRootParamKind::kParamsCbv;
    GuestVisibility visibility = GuestVisibility::kAll;
    // The ranges this parameter's table carries; empty for the root CBV.
    std::vector<GuestRange> ranges;
    // What D3D12 charges for it: a root descriptor is 2 DWORDs, a table is 1.
    [[nodiscard]] std::uint32_t dwords() const { return kind == GuestRootParamKind::kParamsCbv ? 2u : 1u; }
};

// Where a binding lands in the guest arena (rule 4). Every offset is relative to
// the region's base, which the dispatcher reserves in the device's shader-visible
// heap; the same numbers are relative offsets into the CPU-visible heap the views
// are made in, so a copy moves a slot to the slot of the same name.
struct GuestHeapLayout {
    // space 3: the page table followed by the 32 slabs (bbx_guest_mem.h).
    static constexpr std::uint32_t kSpace3Base = 0;
    static constexpr std::uint32_t kSpace3Count = 1 + kGuestSlabCount;  // 33
    // space 1: one region per stage, because two stages' t0 are two resources
    // (rule 3). Inside a region the three classes are contiguous and in the
    // order the table's ranges are written: CBV, then SRV, then UAV.
    static constexpr std::uint32_t kCbvOffset = 0;
    static constexpr std::uint32_t kSrvOffset = kCbvOffset + kGuestCbvCount;   // 63
    static constexpr std::uint32_t kUavOffset = kSrvOffset + kGuestSrvCount;   // 127
    static constexpr std::uint32_t kSpace1Count = kUavOffset + kGuestUavCount; // 191
    // The sampler heap's region: s0..s63, one region per stage for the same
    // reason, and it is a heap of its own (rule 1).
    static constexpr std::uint32_t kSamplerCount = kGuestSamplerCount;
    // How many CBV/SRV/UAV descriptors a guest arena that serves every stage
    // needs, space 3 first: the session reserves that many at boot.
    static constexpr std::uint32_t kStageRegions = 3;  // vertex, pixel, compute
    static constexpr std::uint32_t kCbvSrvUavDescriptors = kSpace3Count + kStageRegions * kSpace1Count;
    [[nodiscard]] static constexpr std::uint32_t StageRegion(std::uint32_t stage) {
        return kSpace3Base + kSpace3Count + stage * kSpace1Count;
    }
};

// A binding's descriptor, resolved: which heap it is in and its index relative
// to that heap's guest region. `sampler` says which heap (rule 1).
struct GuestHeapSlot {
    bool sampler = false;
    std::uint32_t index = 0;  // relative to the stage's region (space 1) or the space-3 region
};

struct GuestRootLayout {
    GuestStage stage = GuestStage::kCompute;
    std::vector<GuestRootParam> params;
    // The console's ceiling: a root signature is at most 64 DWORDs, and 1.1's
    // larger limit is not on the console.
    static constexpr std::uint32_t kMaxDwords = 64;
    [[nodiscard]] std::uint32_t Dwords() const {
        std::uint32_t total = 0;
        for (const GuestRootParam& p : params) total += p.dwords();
        return total;
    }
    [[nodiscard]] bool Valid() const;
    // The range that would serve `register` in `space`, or nullptr.
    [[nodiscard]] const GuestRange* Find(GuestRangeKind kind, std::uint32_t reg, std::uint32_t space,
                                         GuestVisibility visible_to = GuestVisibility::kAll) const;
    [[nodiscard]] std::string Describe() const;
};

// The two signatures the console's guest shaders are baked against. Same ranges,
// different visibility (rule 3): the compute one is 5 DWORDs, the graphics one 9.
[[nodiscard]] GuestRootLayout GuestRoot(GuestStage stage = GuestStage::kCompute);

// Where a baked shader's binding lands in the arena: false when the layout has
// no range for it (the shader names a register the signature does not carry, and
// the dispatch must refuse rather than read a stale descriptor).
[[nodiscard]] bool GuestHeapSlotFor(GuestStage stage, GuestRangeKind kind, std::uint32_t reg, std::uint32_t space,
                                    GuestHeapSlot* out);

// The table a shader's binding is served by, and the descriptor's index within
// that table (what `D3D12_GPU_DESCRIPTOR_HANDLE` arithmetic needs): the root
// parameter's index in `layout.params`, and the offset from the table's start.
struct GuestBindingSlot {
    GuestRootParamKind param = GuestRootParamKind::kGuestMemory;
    std::uint32_t param_index = 0;
    std::uint32_t range = 0;   // index into that parameter's ranges
    std::uint32_t offset = 0;  // descriptors from the table's start
};
[[nodiscard]] bool GuestBinding(const GuestRootLayout& layout, GuestRangeKind kind, std::uint32_t reg,
                                std::uint32_t space, GuestBindingSlot* out,
                                GuestVisibility visible_to = GuestVisibility::kAll);

}  // namespace bbx::d3d12
