// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "src/host/d3d12/bbx_caps.h"

using namespace bbx::d3d12;

TEST(XboxCapsAreShaderModel64AndNothingNewer) {
    const Caps caps = XboxSeriesCaps();
    CHECK_EQ(caps.shader_model, 64u);
    CHECK_EQ(caps.feature_level_major, 11u);
    CHECK(!caps.descriptor_heap_indexing);  // SM 6.6 ResourceDescriptorHeap
    CHECK(!caps.mesh_shaders);              // SM 6.5
    CHECK(!caps.barycentrics);
    CHECK(!caps.pipeline_library);          // CreatePipelineLibrary removes the device
    CHECK(!caps.hardware_tessellator);      // emits zero domain points
    CHECK(!caps.native_16_bit);
    CHECK_EQ(caps.binding_tier, 3u);
    CHECK_EQ(caps.wave_lanes, 64u);
}

TEST(ShaderTargetsAre64OnXboxAnd68OnDesktop) {
    CHECK(ShaderTarget("ps", XboxSeriesCaps()) == "ps_6_4");
    CHECK(ShaderTarget("cs", XboxSeriesCaps()) == "cs_6_4");
    CHECK(ShaderTarget("vs", XboxSeriesCaps()) == "vs_6_4");
    CHECK(ShaderTarget("ps", DesktopCaps()) == "ps_6_8");
}

TEST(CompileFlagsNeverEnable16BitOrHeapIndexingOnXbox) {
    const CompileFlags flags = FlagsFor(XboxSeriesCaps());
    CHECK(!flags.enable_16bit_types);
    CHECK(flags.lower_16bit_ops);
    CHECK(!flags.descriptor_heap_indexing);
    CHECK(!flags.mesh_shaders);
    CHECK_EQ(flags.max_group_shared_bytes, 32u * 1024u);
    const CompileFlags desktop = FlagsFor(DesktopCaps());
    CHECK(desktop.enable_16bit_types);
    CHECK(!desktop.lower_16bit_ops);
    CHECK(desktop.descriptor_heap_indexing);
}

TEST(GroupSharedIsClampedTo32Kib) {
    const Caps caps = XboxSeriesCaps();
    CHECK(GroupSharedFits(caps, 32u * 1024u));
    CHECK(!GroupSharedFits(caps, 32u * 1024u + 4));
    CHECK(!GroupSharedFits(caps, 64u * 1024u));
}

TEST(CapsValidationCatchesNewerThanCompiledFor) {
    Caps actual = XboxSeriesCaps();
    CHECK(Validate(XboxSeriesCaps(), actual).ok);
    actual.shader_model = 66;
    const CapsCheck check = Validate(XboxSeriesCaps(), actual);
    CHECK(!check.ok);
    CHECK(check.problem.find("SM 66") != std::string::npos);
    Caps library = XboxSeriesCaps();
    library.pipeline_library = true;
    CHECK(!Validate(XboxSeriesCaps(), library).ok);
}

TEST(DescribeCapsNamesTheLimits) {
    const std::string text = DescribeCaps(XboxSeriesCaps());
    CHECK(text.find("SM 6.4") != std::string::npos);
    CHECK(text.find("no 16-bit ALU") != std::string::npos);
    CHECK(text.find("no pipeline library") != std::string::npos);
    CHECK(text.find("tessellator emulated") != std::string::npos);
}
