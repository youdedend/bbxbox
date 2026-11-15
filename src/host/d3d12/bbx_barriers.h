// SPDX-License-Identifier: GPL-3.0-or-later
// Vulkan stage/access masks (as the recorder writes them) to D3D12 states.
//
// bbhost records barriers as (srcStageMask, dstStageMask, srcAccessMask,
// dstAccessMask) on a resource. D3D12 wants a before-state and an after-state
// per resource, and getting one wrong is invisible until a frame tears. So the
// table is explicit, total over what the recorder emits, and unit-tested: an
// unmapped pair does not silently become COMMON, it is reported.
#pragma once

#include <cstdint>
#include <optional>

namespace bbx::d3d12 {

// D3D12_RESOURCE_STATES values, repeated so this compiles without the SDK.
using D3d12States = std::uint32_t;
inline constexpr D3d12States kStateCommon = 0x0;
inline constexpr D3d12States kStateVertexAndConstantBuffer = 0x1;
inline constexpr D3d12States kStateIndexBuffer = 0x2;
inline constexpr D3d12States kStateRenderTarget = 0x4;
inline constexpr D3d12States kStateUnorderedAccess = 0x8;
inline constexpr D3d12States kStateDepthWrite = 0x10;
inline constexpr D3d12States kStateDepthRead = 0x20;
inline constexpr D3d12States kStateNonPixelShaderResource = 0x40;
inline constexpr D3d12States kStatePixelShaderResource = 0x80;
inline constexpr D3d12States kStateStreamOut = 0x100;
inline constexpr D3d12States kStateIndirectArgument = 0x200;
inline constexpr D3d12States kStateCopyDest = 0x400;
inline constexpr D3d12States kStateCopySource = 0x800;
inline constexpr D3d12States kStateResolveDest = 0x1000;
inline constexpr D3d12States kStateResolveSource = 0x2000;
inline constexpr D3d12States kStateGenericRead =
    kStateVertexAndConstantBuffer | kStateIndexBuffer | kStateNonPixelShaderResource |
    kStatePixelShaderResource | kStateIndirectArgument | kStateCopySource;
inline constexpr D3d12States kStateAllShaderResource =
    kStateNonPixelShaderResource | kStatePixelShaderResource;

// Vulkan pipeline stage bits used by the stream (values are Vulkan's, so the
// recorder's masks can be fed in directly).
enum : std::uint32_t {
    kVkStageTopOfPipe = 0x1,
    kVkStageDrawIndirect = 0x2,
    kVkStageVertexInput = 0x4,
    kVkStageEarlyFragmentTests = 0x8,
    kVkStageLateFragmentTests = 0x10,
    kVkStageColorAttachmentOutput = 0x20,
    kVkStageComputeShader = 0x40,
    kVkStageTransfer = 0x80,
    kVkStageFragmentShader = 0x100,
    kVkStageAllGraphics = 0x200,
    kVkStageAllCommands = 0x400,
};

enum : std::uint32_t {
    kVkAccessIndirectCommandRead = 0x1,
    kVkAccessIndexRead = 0x2,
    kVkAccessVertexAttributeRead = 0x4,
    kVkAccessUniformRead = 0x8,
    kVkAccessShaderRead = 0x10,
    kVkAccessShaderWrite = 0x20,
    kVkAccessColorAttachmentRead = 0x40,
    kVkAccessColorAttachmentWrite = 0x80,
    kVkAccessDepthStencilAttachmentRead = 0x100,
    kVkAccessDepthStencilAttachmentWrite = 0x200,
    kVkAccessTransferRead = 0x400,
    kVkAccessTransferWrite = 0x800,
    kVkAccessHostRead = 0x1000,
    kVkAccessHostWrite = 0x2000,
    kVkAccessMemoryRead = 0x4000,
    kVkAccessMemoryWrite = 0x8000,
};

// A resource's role, which is what the barrier really means.
enum class Usage : std::uint8_t {
    kCommon,
    kVertexBuffer,
    kIndexBuffer,
    kConstantBuffer,
    kIndirectArgs,
    kShaderRead,     // sampled / SRV
    kShaderWrite,    // UAV
    kRenderTarget,
    kDepthWrite,
    kDepthRead,
    kCopySource,
    kCopyDest,
    kResolveSource,
    kResolveDest,
    kPresent,
};

struct ResourceUsage {
    bool is_texture = false;
    Usage usage = Usage::kCommon;
};

[[nodiscard]] D3d12States StateFor(ResourceUsage usage);

// A D3D12 transition, or nothing when the states already agree (a barrier
// that changes nothing still costs; the replay elides it, and the count is in
// the stats).
struct Transition {
    D3d12States before = kStateCommon;
    D3d12States after = kStateCommon;
    bool needed = false;
};

[[nodiscard]] Transition TransitionFor(ResourceUsage from, ResourceUsage to);

// The state the recorder's masks imply, for a resource with no explicit usage
// (a barrier record carries masks, not roles). `nullopt` when the mask pair is
// not something the port knows how to express; the replay logs it once and
// treats the resource as GENERIC_READ rather than guessing a write state.
[[nodiscard]] std::optional<D3d12States> StateFromVulkanMask(std::uint32_t stage_mask,
                                                            std::uint32_t access_mask,
                                                            bool is_texture);

// The D3D12 state a barrier must be split through when the destination is a
// texture that was written as a UAV and is now read as an SRV in the same
// submission (the classic "decay" case). D3D12 requires the UAV barrier as
// well as the state change; the replay emits both.
struct BarrierPlan {
    D3d12States before = kStateCommon;
    D3d12States after = kStateCommon;
    bool needs_uav_barrier = false;
    bool needs_transition = false;
};

[[nodiscard]] BarrierPlan PlanBarrier(ResourceUsage from, ResourceUsage to, bool is_texture);

}  // namespace bbx::d3d12
