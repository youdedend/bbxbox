// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "src/host/d3d12/bbx_caps.h"

using namespace bbx::d3d12;

TEST(XboxCapsAreShaderModel64AndNothingNewer) {
    const Caps caps = XboxSeriesCaps();
    // 0x64, because that is D3D_SHADER_MODEL_6_4: 100 decimal, not 64.
    CHECK_EQ(caps.shader_model, 0x64u);
    CHECK_EQ(caps.device_shader_model, 0x64u);
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

// The bug this pins: SM 6.4 is 0x64 (one hundred), and the console answered
// exactly that. A port that stored the decimal 64 refused a device that
// D3D12CreateDevice had already created.
TEST(ShaderModelIsHexEncodedNotDecimal) {
    CHECK_EQ(kShaderModel64, 0x64u);
    CHECK_EQ(kShaderModel64, 100u);
    CHECK(kShaderModel64 != 64u);
    CHECK_EQ(ShaderModelMajor(kShaderModel64), 6u);
    CHECK_EQ(ShaderModelMinor(kShaderModel64), 4u);
    CHECK_EQ(ShaderModelText(kShaderModel64), std::string("6.4"));
    CHECK_EQ(ShaderModelText(kShaderModel68), std::string("6.8"));
    // A device that answers exactly what the console answered passes the check.
    Caps actual = XboxSeriesCaps();
    actual.device_shader_model = 0x64;
    CHECK(Validate(XboxSeriesCaps(), actual).ok);
    // And the hex and decimal are both on the one log line.
    const std::string line = FormatShaderModelLine(0x64, kShaderModel64);
    CHECK(line.find("device=SM 6.4 100 (0x64)") != std::string::npos);
    CHECK(line.find("expected=SM 6.4 100 (0x64)") != std::string::npos);
    CHECK(line.find("adequate=1") != std::string::npos);
    CHECK(FormatShaderModelLine(0x63, kShaderModel64).find("adequate=0") != std::string::npos);
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

// A newer device is fine; an older one is a log line, and the message states
// both values in both units so it cannot be misread.
TEST(CapsValidationCatchesOlderThanCompiledFor) {
    Caps actual = XboxSeriesCaps();
    CHECK(Validate(XboxSeriesCaps(), actual).ok);
    actual.device_shader_model = 0x63;
    const CapsCheck check = Validate(XboxSeriesCaps(), actual);
    CHECK(!check.ok);
    CHECK(check.problem.find("SM 6.3") != std::string::npos);
    CHECK(check.problem.find("0x63") != std::string::npos);
    CHECK(check.problem.find("100 (0x64)") != std::string::npos);
    // Newer than the shaders need is not a problem.
    Caps newer = XboxSeriesCaps();
    newer.device_shader_model = 0x66;
    CHECK(Validate(XboxSeriesCaps(), newer).ok);
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
