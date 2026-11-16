// SPDX-License-Identifier: GPL-3.0-or-later
// The console's own shaders are compiled at build time, so the only thing a host
// test can check is the contract around them - which is exactly the part that
// breaks silently: an entry point added to the .hlsl and to CMake but not to the
// manifest (so nothing verifies it), a profile newer than the console's shader
// model, or two entry points that would collide in the generated header.
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "harness.h"
#include "src/host/d3d12/bbx_caps.h"
#include "gcn/hlsl.h"
#include "src/host/d3d12/bbx_shaders.h"

namespace {

using bbx::d3d12::Caps;
using bbx::d3d12::XboxSeriesCaps;
using bbx::shaders::EntryPoint;
using bbx::shaders::Manifest;

// "ps_6_4" -> 4, or -1 when the profile has no minor number.
int ProfileMinor(const char* profile) {
    const char* underscore = std::strrchr(profile, '_');
    return underscore ? std::atoi(underscore + 1) : -1;
}

// "ps_6_4" -> "ps".
std::string ProfileStage(const char* profile) {
    const std::string text = profile;
    const std::size_t underscore = text.find('_');
    return underscore == std::string::npos ? std::string() : text.substr(0, underscore);
}

// PresentVs -> vs, OverlayPs -> ps: the entry point's own suffix states the
// stage it is written for, so a mismatch is checkable without dxc.
std::string EntryStage(const char* entry) {
    const std::string text = entry;
    if (text.size() < 2) { return {}; }
    std::string suffix = text.substr(text.size() - 2);
    for (char& c : suffix) { c = static_cast<char>(std::tolower(c)); }
    return suffix;
}

}  // namespace

TEST(TheShaderManifestNamesEveryEntryPointTheBuildCompiles) {
    const auto manifest = Manifest();
    CHECK_EQ(manifest.count, 6u);
    CHECK(manifest.items != nullptr);
    // Present is a fullscreen triangle (no vertex buffer, no input layout);
    // overlay is the telemetry/HUD pass. Both are needed for the first frame.
    CHECK(std::strcmp(manifest.items[0].symbol, "PresentVs") == 0);
    CHECK(std::strcmp(manifest.items[1].symbol, "PresentPs") == 0);
    CHECK(std::strcmp(manifest.items[2].symbol, "OverlayVs") == 0);
    CHECK(std::strcmp(manifest.items[3].symbol, "OverlayPs") == 0);
    // The compute passes: the detile and the boot self-test.
    CHECK(std::strcmp(manifest.items[4].symbol, "UntileCs") == 0);
    CHECK(std::strcmp(manifest.items[5].symbol, "SelftestCs") == 0);
    for (std::size_t i = 0; i < manifest.count; ++i) {
        const EntryPoint& point = manifest.items[i];
        CHECK(point.symbol != nullptr);
        CHECK(point.entry != nullptr);
        CHECK(point.profile != nullptr);
        CHECK(point.source != nullptr);
        CHECK(std::strlen(point.entry) > 0);
        CHECK(std::strlen(point.profile) > 0);
        CHECK(std::strlen(point.source) > 0);
        // The generated blob is named after the symbol, so it must be a plain
        // identifier fragment: no dots, no dashes.
        CHECK(std::strspn(point.symbol, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ==
              std::strlen(point.symbol));
    }
}

TEST(NoShaderTargetIsNewerThanTheConsoleShaderModel) {
    const Caps caps = XboxSeriesCaps();
    const auto manifest = Manifest();
    for (std::size_t i = 0; i < manifest.count; ++i) {
        const EntryPoint& point = manifest.items[i];
        // SM 6.4 is the console's ceiling; 6.5/6.6 targets fail PSO creation.
        CHECK(ProfileMinor(point.profile) >= 0);
        CHECK_EQ(static_cast<unsigned>(ProfileMinor(point.profile)), caps.shader_model % 10);
        CHECK_EQ(ProfileStage(point.profile), EntryStage(point.entry));
        // A .hlsl under xbox/shaders, not a path: the build globs that directory.
        CHECK(std::strchr(point.source, '/') == nullptr);
        CHECK(std::strchr(point.source, '\\') == nullptr);
        CHECK(std::strstr(point.source, ".hlsl") != nullptr);
    }
}

TEST(ShaderEntryPointsDoNotCollide) {
    const auto manifest = Manifest();
    std::set<std::string> keys;
    for (std::size_t i = 0; i < manifest.count; ++i) {
        // Two entry points with the same name and profile would generate two
        // arrays with the same identifier in bbx_shaders_generated.h.
        keys.insert(std::string(manifest.items[i].entry) + ":" + manifest.items[i].profile);
        keys.insert(std::string("symbol:") + manifest.items[i].symbol);
    }
    CHECK_EQ(keys.size(), manifest.count * 2);
}

// --- the console's dialect ---------------------------------------------------
//
// The printer writes one source: Vulkan's, with descriptor sets as
// [[vk::binding]], block offsets as [[vk::offset]] and every operation whose
// HLSL spelling DXC lowers differently as inline SPIR-V. The console's compiler
// is given -T *_6_4 and takes none of that, and three of its features are
// absent rather than restricted (16-bit native ops, ResourceDescriptorHeap,
// IsHelperLane), so "does the console dialect still carry one of them" is a
// property of the printed text, checkable without a copy of dxc.

namespace {

// A pixel shader over one texture, one sampler and one block: loads the
// texture coordinate, samples, reads a block member, adds, takes FMin (which
// the printer writes as inline SPIR-V) and stores to the target. `block_offsets`
// is the offset of each float4 member of the block, in order.
std::vector<std::uint32_t> FragmentFixture(const std::vector<std::uint32_t>& block_offsets) {
    std::vector<std::uint32_t> m = {0x07230203u, 0x00010600u, 0u, 400u, 0u};
    const auto op = [&m](std::uint32_t code, const std::vector<std::uint32_t>& ops) {
        m.push_back((static_cast<std::uint32_t>(ops.size() + 1) << 16) | code);
        for (const std::uint32_t o : ops) m.push_back(o);
    };
    const std::uint32_t kVoid = 1, kFunc = 2, kFloat = 3, kVec4 = 4, kImage = 5, kSampler = 6, kSampled = 7;
    const std::uint32_t kPtrUCImage = 8, kPtrUCSampler = 9, kPtrUCSampled = 10, kBlock = 11, kPtrBlock = 12;
    const std::uint32_t kPtrOut = 13, kPtrIn = 14, kPtrFloat = 15, kUint = 16;
    const std::uint32_t kZero = 17, kGlsl = 18;
    const std::uint32_t vTex = 19, vSmp = 20, vBlock = 21, vOut = 22, vIn = 23, fMain = 24, lbl = 25;
    const std::uint32_t tIn = 30, tImage = 31, tSampler = 32, tSampled = 33, tBlock = 34, tPtr = 35, tAdd = 36, tMin = 37,
                       tSample = 38;

    op(17, {1});                                                     // OpCapability Shader
    op(11, {kGlsl, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0u});      // OpExtInstImport "GLSL.std.450"
    op(14, {0, 1});                                                  // OpMemoryModel Logical GLSL450
    op(15, {4, fMain, 0x6e69616du, 0u, vTex, vSmp, vBlock, vOut, vIn});  // OpEntryPoint Fragment "main"
    op(16, {fMain, 7});                                              // OpExecutionMode OriginUpperLeft
    // Descriptor set 0, and the bindings the console's root signature numbers.
    op(71, {vTex, 34, 0});
    op(71, {vTex, 33, 0});
    op(71, {vSmp, 34, 0});
    op(71, {vSmp, 33, 1});
    op(71, {vBlock, 34, 0});
    op(71, {vBlock, 33, 2});
    op(71, {vIn, 30, 0});   // Location 0 in
    op(71, {vOut, 30, 0});  // Location 0 out
    op(71, {kBlock, 2});    // Block
    for (std::uint32_t i = 0; i < block_offsets.size(); ++i) op(72, {kBlock, i, 35, block_offsets[i]});
    op(19, {kVoid});
    op(33, {kFunc, kVoid});
    op(22, {kFloat, 32});
    op(23, {kVec4, kFloat, 4});
    op(25, {kImage, kFloat, 1, 0, 0, 0, 1, 0});  // OpTypeImage 2D, sampled, Unknown format
    op(26, {kSampler});
    op(27, {kSampled, kImage});
    std::vector<std::uint32_t> members = {kBlock};
    members.resize(1 + block_offsets.size(), kVec4);
    op(30, members);
    op(32, {kPtrUCImage, 0, kImage});
    op(32, {kPtrUCSampler, 0, kSampler});
    op(32, {kPtrUCSampled, 0, kSampled});
    op(32, {kPtrBlock, 2, kBlock});
    op(32, {kPtrOut, 3, kVec4});
    op(32, {kPtrIn, 1, kVec4});
    op(32, {kPtrFloat, 2, kFloat});
    op(21, {kUint, 32, 0});
    op(43, {kUint, kZero, 0u});
    op(59, {kPtrUCImage, vTex, 0});
    op(59, {kPtrUCSampler, vSmp, 0});
    op(59, {kPtrBlock, vBlock, 2});
    op(59, {kPtrOut, vOut, 3});
    op(59, {kPtrIn, vIn, 1});
    op(54, {kVoid, fMain, 0, kFunc});
    op(248, {lbl});
    op(61, {kVec4, tIn, vIn});
    op(61, {kImage, tImage, vTex});
    op(61, {kSampler, tSampler, vSmp});
    op(86, {kSampled, tSampled, tImage, tSampler});  // OpSampledImage
    op(87, {kVec4, tSample, tSampled, tIn});         // OpImageSampleImplicitLod
    op(65, {kPtrFloat, tPtr, vBlock, kZero});        // OpAccessChain into member 0
    op(61, {kVec4, tBlock, tPtr});
    op(129, {kVec4, tAdd, tSample, tBlock});
    op(12, {kVec4, tMin, kGlsl, 37, tAdd, tBlock});  // GLSL FMin
    op(62, {vOut, tMin});
    op(253, {});
    op(56, {});
    return m;
}

// A pixel shader whose block starts with a 64-bit member - every stage's
// StageParams does, as u64 l1_table - and which either reads that member or does
// not. `read_wide`: the program loads it and uses it, which is what the console's
// page-table walk does (Slice 5); otherwise nothing touches it.
std::vector<std::uint32_t> WideBlockFixture(bool read_wide) {
    std::vector<std::uint32_t> m = {0x07230203u, 0x00010600u, 0u, 400u, 0u};
    const auto op = [&m](std::uint32_t code, const std::vector<std::uint32_t>& ops) {
        m.push_back((static_cast<std::uint32_t>(ops.size() + 1) << 16) | code);
        for (const std::uint32_t o : ops) m.push_back(o);
    };
    const std::uint32_t kVoid = 1, kFunc = 2, kFloat = 3, kVec4 = 4, kBlock = 11, kPtrBlock = 12, kUint = 16, kZero = 17,
                       kU64 = 20, kPtrU64 = 21, kOut = 30, kPtrOut = 31, kPtrVec = 32;
    const std::uint32_t vBlock = 22, vOut = 23, fMain = 24, lbl = 25, kOne = 26;
    const std::uint32_t tBlock = 34, tPtr = 35, tWide = 36, tU32 = 37, tF32 = 38;

    op(17, {1});                                        // OpCapability Shader
    op(17, {11});                                       // OpCapability Int64
    op(14, {0, 1});                                     // OpMemoryModel Logical GLSL450
    op(15, {4, fMain, 0x6e69616du, 0u, vBlock, vOut});   // OpEntryPoint Fragment "main"
    op(16, {fMain, 7});                                 // OpExecutionMode OriginUpperLeft
    op(71, {vBlock, 34, 0});                            // DescriptorSet 0
    op(71, {vBlock, 33, 2});                            // Binding 2
    op(71, {vOut, kOut, 0});                            // Location 0 out
    op(71, {kBlock, 2});                                // Block
    op(72, {kBlock, 0, 35, 0});                         // member 0 at offset 0
    op(72, {kBlock, 1, 35, 16});                        // member 1 at offset 16
    op(19, {kVoid});
    op(33, {kFunc, kVoid});
    op(22, {kFloat, 32});
    op(23, {kVec4, kFloat, 4});
    op(21, {kUint, 32, 0});
    op(21, {kU64, 64, 0});
    op(43, {kUint, kZero, 0u});
    op(43, {kUint, kOne, 1u});
    op(30, {kBlock, kU64, kVec4});                      // the block: a u64, then a float4
    op(32, {kPtrBlock, 2, kBlock});
    op(32, {kPtrU64, 2, kU64});
    op(32, {kPtrVec, 2, kVec4});
    op(32, {kPtrOut, 3, kVec4});
    op(59, {kPtrBlock, vBlock, 2});
    op(59, {kPtrOut, vOut, 3});
    op(54, {kVoid, fMain, 0, kFunc});
    op(248, {lbl});
    if (read_wide) {
        op(65, {kPtrU64, tPtr, vBlock, kZero});          // OpAccessChain member 0
        op(61, {kU64, tWide, tPtr});                     // OpLoad
        op(110, {kUint, tU32, tWide});                   // OpUConvert
        op(109, {kFloat, tF32, tU32});                   // OpConvertUToF
        op(81, {kVec4, tBlock, tF32});                   // OpCompositeConstruct
    } else {
        op(65, {kPtrVec, tPtr, vBlock, kOne});           // OpAccessChain member 1
        op(61, {kVec4, tBlock, tPtr});                   // OpLoad
    }
    op(62, {vOut, tBlock});
    op(253, {});
    op(56, {});
    return m;
}

}  // namespace

TEST(ConsoleDialectWritesA64BitBlockMemberTheShaderDoesNotReadAsTwoDwords) {
    // Every stage's StageParams starts with u64 l1_table; u64 reserved, and a
    // console VS or PS reads neither. DXC's "64-Bit integer" requirement is a
    // capability the console's driver does not have, so an unread member is
    // declared as the two dwords it is: the same eight bytes, at the same
    // offset, inside one register.
    gcn::HlslOptions options;
    options.dxil_sm64 = true;
    const gcn::HlslResult unread = gcn::emit_hlsl(WideBlockFixture(false), options);
    CHECK(unread.ok());
    CHECK(unread.source.find("uint64_t") == std::string::npos);
    CHECK(unread.source.find("uint2 l1_table;") != std::string::npos);
    // The Vulkan dialect keeps the 64-bit member: there the device has Int64.
    const gcn::HlslResult vulkan = gcn::emit_hlsl(WideBlockFixture(false), {});
    CHECK(vulkan.ok());
    CHECK(vulkan.source.find("uint64_t") != std::string::npos);
    // A member the shader does read is stopped by name: as two dwords it would
    // compute something else, and the console has no 64-bit path for it yet.
    const gcn::HlslResult read = gcn::emit_hlsl(WideBlockFixture(true), options);
    CHECK(!read.ok());
    CHECK(!read.errors.empty());
    CHECK(read.errors.front().find("64-bit block member") != std::string::npos);
}

TEST(ConsoleDialectDropsThePageTableWalkerAComputeShaderNeverCalls) {
    // A compute lift declares xlate() whatever the program does (gcn/lift.cpp),
    // written with the block's 64-bit head and PhysicalStorageBuffer loads:
    // neither of which the console's dialect has. One the shader never calls is
    // dead code and the printer drops it; the real lift's own output is what
    // the drop is really tested on, in tools/bbx_bake_cache --self-test.
    gcn::HlslOptions options;
    options.dxil_sm64 = true;
    const gcn::HlslResult s = gcn::emit_hlsl(FragmentFixture({0}), options);
    CHECK(s.ok());
    CHECK(s.source.find("xlate") == std::string::npos);
}

TEST(ConsoleDialectHasNoVulkanAttributeAndNoShaderModel66) {
    const std::vector<std::uint32_t> module = FragmentFixture({0});
    gcn::HlslOptions options;
    const gcn::HlslResult vulkan = gcn::emit_hlsl(module, options);
    CHECK(vulkan.ok());
    CHECK_EQ(vulkan.profile, std::string("ps_6_6"));
    CHECK(vulkan.source.find("[[vk::binding(0, 0)]]") != std::string::npos);
    CHECK(vulkan.source.find("[[vk::offset(0)]]") != std::string::npos);

    options.dxil_sm64 = true;
    const gcn::HlslResult console = gcn::emit_hlsl(module, options);
    CHECK(console.ok());
    // Shader Model 6.4 and language 2021: the console's driver takes nothing newer.
    CHECK_EQ(console.profile, std::string("ps_6_4"));
    CHECK_EQ(console.language, std::string("2021"));
    // No 16-bit types: Native16BitShaderOpsSupported is FALSE on that driver,
    // which makes -enable-16bit-types E_INVALIDARG rather than slow.
    CHECK(console.arguments.empty());
    for (const char* forbidden : {"vk::", "float16_t", "_6_6", "_6_7", "202x", "[[", "IsHelperLane", "ResourceDescriptorHeap",
                                  "SampleCmpLevel", "dxil.dll"}) {
        CHECK(console.source.find(forbidden) == std::string::npos);
    }
    // The same three resources, in the register files a root signature 1.0 names.
    CHECK(console.source.find(": register(t0, space0);") != std::string::npos);
    CHECK(console.source.find(": register(s1, space0);") != std::string::npos);
    CHECK(console.source.find(": register(b2, space0);") != std::string::npos);
    // The inline SPIR-V FMin is a function of the same signature now.
    CHECK(console.source.find("spv_fmin(") != std::string::npos);
    CHECK(console.source.find("float4 spv_fmin(float4 a0, float4 a1)") != std::string::npos);
    // Not one attribute is left: what a caller may hand to dxc -T ps_6_4.
    CHECK(console.source.find("[[") == std::string::npos);
}

TEST(ConsoleDialectPadsABlockToTheLiftsOffsets) {
    // A member at 32: DXC's layout reaches it only across one empty register,
    // which the console dialect fills with a float4 rather than binding the
    // lift's second member to the wrong one.
    const std::vector<std::uint32_t> module = FragmentFixture({0, 32});
    gcn::HlslOptions options;
    options.dxil_sm64 = true;
    const gcn::HlslResult r = gcn::emit_hlsl(module, options);
    CHECK(r.ok());
    CHECK(r.source.find("float4 _dxil_pad1[1];") != std::string::npos);
    CHECK(r.source.find("register(b2, space0)") != std::string::npos);
    // A member whose offset DXC's layout cannot reach stops the shader by name:
    // bound to the wrong register it would compile and read the wrong value.
    const gcn::HlslResult bad = gcn::emit_hlsl(FragmentFixture({0, 8}), options);
    CHECK(!bad.ok());
    CHECK(!bad.errors.empty());
    CHECK(bad.errors.front().find("dxil 6.4:") != std::string::npos);
}
