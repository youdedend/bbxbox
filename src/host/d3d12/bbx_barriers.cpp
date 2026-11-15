// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_barriers.h"

namespace bbx::d3d12 {
namespace {

constexpr Usage kAll[] = {Usage::kCommon,     Usage::kVertexBuffer, Usage::kIndexBuffer,
                          Usage::kConstantBuffer, Usage::kIndirectArgs, Usage::kShaderRead,
                          Usage::kShaderWrite, Usage::kRenderTarget, Usage::kDepthWrite,
                          Usage::kDepthRead, Usage::kCopySource,    Usage::kCopyDest,
                          Usage::kResolveSource, Usage::kResolveDest, Usage::kPresent};

bool KnownUsage(Usage u) {
    for (const Usage k : kAll) {
        if (k == u) return true;
    }
    return false;
}

}  // namespace

D3d12States StateFor(ResourceUsage usage) {
    switch (usage.usage) {
    case Usage::kCommon:
        return kStateCommon;
    case Usage::kVertexBuffer:
        return kStateVertexAndConstantBuffer;
    case Usage::kIndexBuffer:
        return kStateIndexBuffer;
    case Usage::kConstantBuffer:
        return kStateVertexAndConstantBuffer;
    case Usage::kIndirectArgs:
        return kStateIndirectArgument;
    case Usage::kShaderRead:
        return kStateAllShaderResource;
    case Usage::kShaderWrite:
        return kStateUnorderedAccess;
    case Usage::kRenderTarget:
        return kStateRenderTarget;
    case Usage::kDepthWrite:
        return kStateDepthWrite;
    case Usage::kDepthRead:
        return kStateDepthRead;
    case Usage::kCopySource:
        return kStateCopySource;
    case Usage::kCopyDest:
        return kStateCopyDest;
    case Usage::kResolveSource:
        return kStateResolveSource;
    case Usage::kResolveDest:
        return kStateResolveDest;
    case Usage::kPresent:
        return kStateCommon;  // PRESENT is 0 in D3D12; the swapchain owns it
    }
    return kStateCommon;
}

Transition TransitionFor(ResourceUsage from, ResourceUsage to) {
    Transition t;
    t.before = StateFor(from);
    t.after = StateFor(to);
    // Two states can differ yet be compatible (GENERIC_READ vs ALL_SHADER_READ
    // is not compatible, but RENDER_TARGET -> RENDER_TARGET is). Only a real
    // change needs a barrier.
    t.needed = t.before != t.after;
    return t;
}

std::optional<D3d12States> StateFromVulkanMask(std::uint32_t stage_mask, std::uint32_t access_mask,
                                               bool is_texture) {
    using namespace bbx::d3d12;
    const bool read = (access_mask & (kVkAccessShaderRead | kVkAccessUniformRead |
                                      kVkAccessVertexAttributeRead | kVkAccessIndexRead |
                                      kVkAccessIndirectCommandRead | kVkAccessDepthStencilAttachmentRead |
                                      kVkAccessColorAttachmentRead | kVkAccessTransferRead)) != 0;
    const bool write = (access_mask & (kVkAccessShaderWrite | kVkAccessColorAttachmentWrite |
                                       kVkAccessDepthStencilAttachmentWrite | kVkAccessTransferWrite)) != 0;
    const bool uav = (access_mask & kVkAccessShaderWrite) != 0;
    const bool depth = (stage_mask & (kVkStageEarlyFragmentTests | kVkStageLateFragmentTests)) != 0 ||
                       (access_mask & (kVkAccessDepthStencilAttachmentRead |
                                       kVkAccessDepthStencilAttachmentWrite)) != 0;
    const bool color = (access_mask & (kVkAccessColorAttachmentRead | kVkAccessColorAttachmentWrite)) != 0;

    if (uav) return kStateUnorderedAccess;
    if (color) return kStateRenderTarget;
    if (depth) {
        if (write) return kStateDepthWrite;
        if (read) return kStateDepthRead;
        return std::nullopt;
    }
    if ((access_mask & (kVkAccessTransferRead | kVkAccessHostRead)) != 0) return kStateCopySource;
    if ((access_mask & (kVkAccessTransferWrite | kVkAccessHostWrite)) != 0) return kStateCopyDest;
    if ((access_mask & kVkAccessVertexAttributeRead) != 0) return kStateVertexAndConstantBuffer;
    if ((access_mask & kVkAccessIndexRead) != 0) return kStateIndexBuffer;
    if ((access_mask & kVkAccessIndirectCommandRead) != 0) return kStateIndirectArgument;
    if ((access_mask & (kVkAccessUniformRead | kVkAccessShaderRead)) != 0) {
        return is_texture ? kStateAllShaderResource : kStateGenericRead;
    }
    if (!read && !write) {
        // A pure synchronisation barrier (TOP_OF_PIPE -> ALL_COMMANDS) changes
        // no state; COMMON is correct and free.
        return kStateCommon;
    }
    return std::nullopt;
}

BarrierPlan PlanBarrier(ResourceUsage from, ResourceUsage to, bool is_texture) {
    BarrierPlan plan;
    if (!KnownUsage(from.usage) || !KnownUsage(to.usage)) return plan;
    const Transition t = TransitionFor(from, to);
    plan.before = t.before;
    plan.after = t.after;
    plan.needs_transition = t.needed;
    // A texture (or buffer) that was written through a UAV and is now read
    // needs the UAV barrier even though the state change alone would also be
    // issued; AMD's driver is strict about this and Xbox's is AMD's.
    plan.needs_uav_barrier = (from.usage == Usage::kShaderWrite) &&
                             (to.usage != Usage::kShaderWrite) && is_texture;
    return plan;
}

}  // namespace bbx::d3d12
