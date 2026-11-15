// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "host/d3d12/bbx_barriers.h"

using namespace bbx::d3d12;

TEST(UsageStates) {
    CHECK_EQ(StateFor({true, Usage::kRenderTarget}), kStateRenderTarget);
    CHECK_EQ(StateFor({true, Usage::kDepthWrite}), kStateDepthWrite);
    CHECK_EQ(StateFor({false, Usage::kVertexBuffer}), kStateVertexAndConstantBuffer);
    CHECK_EQ(StateFor({false, Usage::kIndexBuffer}), kStateIndexBuffer);
    CHECK_EQ(StateFor({true, Usage::kShaderRead}), kStateAllShaderResource);
    CHECK_EQ(StateFor({true, Usage::kShaderWrite}), kStateUnorderedAccess);
    CHECK_EQ(StateFor({true, Usage::kCopyDest}), kStateCopyDest);
    CHECK_EQ(StateFor({true, Usage::kPresent}), kStateCommon);
}

TEST(TransitionsOnlyWhenTheStateChanges) {
    const Transition same = TransitionFor({true, Usage::kRenderTarget}, {true, Usage::kRenderTarget});
    CHECK(!same.needed);
    CHECK_EQ(same.before, kStateRenderTarget);
    CHECK_EQ(same.after, kStateRenderTarget);

    const Transition to_srv = TransitionFor({true, Usage::kRenderTarget}, {true, Usage::kShaderRead});
    CHECK(to_srv.needed);
    CHECK_EQ(to_srv.before, kStateRenderTarget);
    CHECK_EQ(to_srv.after, kStateAllShaderResource);
}

TEST(UavBarrierAfterAWrite) {
    const BarrierPlan write_then_read =
        PlanBarrier({true, Usage::kShaderWrite}, {true, Usage::kShaderRead}, true);
    CHECK(write_then_read.needs_transition || write_then_read.needs_uav_barrier);
    CHECK(write_then_read.needs_uav_barrier);
    CHECK_EQ(write_then_read.before, kStateUnorderedAccess);
    CHECK_EQ(write_then_read.after, kStateAllShaderResource);

    // A buffer needs the barrier too, a texture only is not special.
    const BarrierPlan buffer = PlanBarrier({false, Usage::kShaderWrite}, {false, Usage::kShaderRead},
                                           false);
    CHECK(buffer.needs_transition);
    CHECK(!buffer.needs_uav_barrier);

    // UAV to UAV is no barrier at all.
    const BarrierPlan uav_uav =
        PlanBarrier({true, Usage::kShaderWrite}, {true, Usage::kShaderWrite}, true);
    CHECK(!uav_uav.needs_transition);
    CHECK(!uav_uav.needs_uav_barrier);
}

TEST(VulkanMasksMap) {
    // A render target write from the color attachment stage.
    CHECK_EQ(StateFromVulkanMask(kVkStageColorAttachmentOutput, kVkAccessColorAttachmentWrite, true),
             kStateRenderTarget);
    // A pixel shader sampling.
    CHECK_EQ(StateFromVulkanMask(kVkStageFragmentShader, kVkAccessShaderRead, true),
             kStateAllShaderResource);
    // A storage buffer read in compute.
    CHECK_EQ(StateFromVulkanMask(kVkStageComputeShader, kVkAccessShaderRead, false),
             kStateGenericRead);
    // A UAV write.
    CHECK_EQ(StateFromVulkanMask(kVkStageComputeShader, kVkAccessShaderWrite, false),
             kStateUnorderedAccess);
    // A transfer.
    CHECK_EQ(StateFromVulkanMask(kVkStageTransfer, kVkAccessTransferWrite, true), kStateCopyDest);
    CHECK_EQ(StateFromVulkanMask(kVkStageTransfer, kVkAccessTransferRead, false),
             kStateCopySource);
    // Depth.
    CHECK_EQ(StateFromVulkanMask(kVkStageEarlyFragmentTests, kVkAccessDepthStencilAttachmentWrite,
                                 true),
             kStateDepthWrite);
    // A pure sync barrier changes no state.
    CHECK_EQ(StateFromVulkanMask(kVkStageTopOfPipe, 0, true), kStateCommon);
    // A memory-only barrier (ALL_COMMANDS + MEMORY_WRITE) is a synchronisation
    // point with no resource state, so COMMON is the right answer, not a guess.
    CHECK_EQ(StateFromVulkanMask(kVkStageAllCommands, kVkAccessMemoryWrite, true), kStateCommon);
}

TEST(UnknownUsageIsCommonNotGarbage) {
    const ResourceUsage unknown{true, static_cast<Usage>(200)};
    const Transition t = TransitionFor(unknown, {true, Usage::kShaderRead});
    CHECK(t.before == kStateCommon);
    const BarrierPlan plan = PlanBarrier(unknown, {true, Usage::kShaderRead}, true);
    CHECK(!plan.needs_transition);
    CHECK(!plan.needs_uav_barrier);
}
