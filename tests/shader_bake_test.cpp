// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for portable DXIL validation, PS4 shader container inspection,
// eboot scanning, and TV HUD progress formatting used by the on-Xbox baker.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "harness.h"
#include "host/d3d12/bbx_shader_bake.h"

namespace {

using namespace bbx::d3d12;

std::vector<std::uint8_t> MakeDxbcWithSfi0(bool signed_digest, bool include_sfi0,
                                           std::uint32_t sfi0_flags) {
    const std::uint32_t part_count = include_sfi0 ? 1u : 0u;
    const std::size_t sfi0_off = 32 + 4 * static_cast<std::size_t>(part_count);
    std::vector<std::uint8_t> blob(sfi0_off + (include_sfi0 ? 16u : 0u), 0);
    std::memcpy(blob.data(), "DXBC", 4);
    if (signed_digest) {
        blob[4] = 0xAA;
        blob[19] = 0x55;
    }
    std::memcpy(blob.data() + 28, &part_count, 4);
    if (include_sfi0) {
        const std::uint32_t off32 = static_cast<std::uint32_t>(sfi0_off);
        std::memcpy(blob.data() + 32, &off32, 4);
        std::memcpy(blob.data() + sfi0_off, "SFI0", 4);
        const std::uint32_t chunk_size = 8u;
        std::memcpy(blob.data() + sfi0_off + 4, &chunk_size, 4);
        std::memcpy(blob.data() + sfi0_off + 8, &sfi0_flags, 4);
    }
    return blob;
}

std::vector<std::uint8_t> MakeFakePs4Container(std::uint8_t type, std::uint32_t hash) {
    std::vector<std::uint8_t> c(0x80, 0);
    std::memcpy(c.data(), "Shdr", 4);
    c[8] = type;
    const std::size_t footer = c.size() - 28;
    std::memcpy(c.data() + footer, "OrbShdr", 7);
    std::memcpy(c.data() + footer + 16, &hash, 4);
    return c;
}

TEST(shader_bake_signed_dxbc_gate) {
    std::string why;
    const std::vector<std::uint8_t> signed_blob = MakeDxbcWithSfi0(true, true, 0u);
    CHECK(IsSignedDxbc(signed_blob, why));
    CHECK(why.empty());

    const std::vector<std::uint8_t> unsigned_blob = MakeDxbcWithSfi0(false, true, 0u);
    CHECK(!IsSignedDxbc(unsigned_blob, why));
    CHECK(why.find("unsigned") != std::string::npos);

    const std::vector<std::uint8_t> tiny = {'D', 'X', 'B', 'C'};
    CHECK(!IsSignedDxbc(tiny, why));
}

TEST(shader_bake_sfi0_feature_gate) {
    const std::vector<std::uint8_t> clean = MakeDxbcWithSfi0(true, true, 0x10u);
    const DxilFeatures f_clean = DxilFeatureFlags(clean);
    CHECK(f_clean.present);
    CHECK_EQ(f_clean.flags, 0x10u);
    CHECK(!HasForbiddenConsoleFeature(f_clean));

    const std::vector<std::uint8_t> int64 = MakeDxbcWithSfi0(true, true, kDxilFeatureInt64);
    CHECK(HasForbiddenConsoleFeature(DxilFeatureFlags(int64)));

    const std::vector<std::uint8_t> fp16 = MakeDxbcWithSfi0(true, true, kDxilFeatureNative16Bit);
    CHECK(HasForbiddenConsoleFeature(DxilFeatureFlags(fp16)));

    const std::vector<std::uint8_t> no_sfi0 = MakeDxbcWithSfi0(true, false, 0u);
    CHECK(HasForbiddenConsoleFeature(DxilFeatureFlags(no_sfi0)));
}

TEST(shader_bake_64bit_hlsl_gate) {
    std::string what;
    CHECK(!Has64BitInteger("float4 main(uint id : SV_VertexID) : SV_Position { return 0; }", what));
    CHECK(Has64BitInteger("void main() { uint64_t x = 1; }", what));
    CHECK(what.find("uint64_t") != std::string::npos);
    CHECK(Has64BitInteger("void main() { double d = 1.0; }", what));
}

TEST(shader_bake_bundle_filename_filter) {
    CHECK(IsShaderBundleFilename("GXShader.shaderbnd.dcx"));
    CHECK(IsShaderBundleFilename("GXShader.SHADERBND.DCX"));
    CHECK(IsShaderBundleFilename("test.shaderbnddebug.dcx"));
    CHECK(!IsShaderBundleFilename("eboot.bin"));
    CHECK(!IsShaderBundleFilename("shader.dcx"));
    CHECK(!IsShaderBundleFilename("map.mapbnd.dcx"));
}

TEST(shader_bake_container_inspection_and_eboot_scan) {
    const std::vector<std::uint8_t> vs = MakeFakePs4Container(1, 0x1234abcdU);
    CHECK_EQ(ContainerType(vs), 1);
    CHECK_EQ(ContainerName(vs), std::string("1234abcd"));

    const std::vector<std::uint8_t> cs = MakeFakePs4Container(4, 0xdeadbeefU);
    CHECK_EQ(ContainerType(cs), 4);
    CHECK_EQ(ContainerName(cs), std::string("deadbeef"));

    // Embed two containers in a synthetic ELF buffer with padding between them.
    std::vector<std::uint8_t> elf(0x40, 0xCC);
    elf.insert(elf.end(), vs.begin(), vs.end());
    elf.insert(elf.end(), 0x20, 0x00);
    elf.insert(elf.end(), cs.begin(), cs.end());
    const std::vector<EmbeddedContainerSpan> found = ScanEbootForContainers(elf);
    CHECK_EQ(found.size(), 2u);
    CHECK_EQ(found[0].type, 1u);
    CHECK_EQ(found[0].name, std::string("1234abcd"));
    CHECK_EQ(found[1].type, 4u);
    CHECK_EQ(found[1].name, std::string("deadbeef"));
}

TEST(shader_bake_hud_progress_line) {
    BakeProgressSnapshot snap;
    snap.phase = BakePhase::kCompiling;
    snap.done = 500;
    snap.total = 2000;
    snap.signed_count = 490;
    snap.refused_count = 8;
    snap.quarantined_count = 2;
    snap.workers = 4;
    snap.elapsed_seconds = 42;
    const std::string line = FormatBakeHudLine(snap);
    CHECK(line.find("COMPILING DXIL 6.4 (4T)") != std::string::npos);
    CHECK(line.find("500 / 2000 (25%)") != std::string::npos);
    CHECK(line.find("OK 490") != std::string::npos);
    CHECK(line.find("SKIP 10") != std::string::npos);
    CHECK(line.find("[42s]") != std::string::npos);
}

TEST(shader_bake_worker_count_policy) {
    CHECK_EQ(ChooseBakeWorkerCount(8, 4, true), 1u);
    CHECK_EQ(ChooseBakeWorkerCount(1, 4, false), 1u);
    CHECK_EQ(ChooseBakeWorkerCount(2, 4, false), 2u);
    CHECK_EQ(ChooseBakeWorkerCount(4, 4, false), 2u);
    CHECK_EQ(ChooseBakeWorkerCount(8, 4, false), 4u);
    CHECK_EQ(ChooseBakeWorkerCount(16, 6, false), 4u);
}

TEST(shader_bake_checkpoint_roundtrip_and_integrity) {
    CacheStamp stamp;
    stamp.translator = "0123456789abcdef";
    stamp.dxil_target = "dxil-6.4";
    stamp.dxc_version = "1.8.2403";

    std::vector<CacheEntry> entries;
    CacheEntry e0{};
    e0.key = 0x1111222233334444ull;
    e0.program_hash = 0xaaaaull;
    e0.bytecode_hash = 0xbbbbull;
    e0.stage = Stage::kCompute;
    e0.offset = 0;
    e0.size = 4;
    e0.raw_size = 64;
    entries.push_back(e0);

    CacheEntry e1{};
    e1.key = 0x5555666677778888ull;
    e1.program_hash = 0xccccull;
    e1.bytecode_hash = 0xddddull;
    e1.stage = Stage::kPixel;
    e1.offset = 4;
    e1.size = 4;
    e1.raw_size = 128;
    entries.push_back(e1);

    const std::vector<std::uint8_t> blobs = {1, 2, 3, 4, 9, 10, 11, 12};
    const std::string bindings = "1111222233334444 u0 direct 0\n5555666677778888 t1 table 2 4\n";

    const std::vector<std::uint8_t> bytes = SerializeBakeCheckpoint(stamp, entries, blobs, bindings);
    CHECK(!bytes.empty());

    BakeCheckpoint out;
    CHECK(ParseBakeCheckpoint(bytes, stamp, &out));
    CHECK_EQ(out.entries.size(), 2u);
    CHECK_EQ(out.entries[0].key, 0x1111222233334444ull);
    CHECK_EQ(out.entries[0].raw_size, 64u);
    CHECK_EQ(out.entries[1].key, 0x5555666677778888ull);
    CHECK_EQ(out.blobs.size(), 8u);
    CHECK_EQ(out.bindings_text, bindings);

    // Stamp mismatch must reject the checkpoint.
    CacheStamp wrong_stamp = stamp;
    wrong_stamp.translator = "fedcba9876543210";
    CHECK(!ParseBakeCheckpoint(bytes, wrong_stamp, &out));

    // Corrupted payload byte must fail FNV-1a integrity verification.
    std::vector<std::uint8_t> corrupted = bytes;
    corrupted[corrupted.size() / 2] ^= 0xFFu;
    CHECK(!ParseBakeCheckpoint(corrupted, stamp, &out));
}

TEST(shader_bake_adaptive_dxc_optimization_level) {
    CHECK_EQ(ChooseDxcOptimizationLevel("float4 main() : SV_Target { return 0; }\n"), 3u);

    std::string medium_hlsl;
    medium_hlsl.reserve(28000);
    for (int i = 0; i < 650; ++i) {
        medium_hlsl += "    v = v * 1.5f + 0.25f;\n";
    }
    CHECK_EQ(ChooseDxcOptimizationLevel(medium_hlsl), 1u);

    std::string huge_hlsl(50000, 'x');
    CHECK_EQ(ChooseDxcOptimizationLevel(huge_hlsl), 0u);

    std::string loop_heavy;
    for (int i = 0; i < 14; ++i) {
        loop_heavy += "for (uint k = 0; k < 4; ++k) { x += k; }\n";
    }
    CHECK_EQ(ChooseDxcOptimizationLevel(loop_heavy), 0u);
}

TEST(shader_bake_multi_worker_inflight_roundtrip) {
    std::vector<WorkerInflightSlot> slots(3);
    slots[0].worker_id = 0;
    slots[0].active = true;
    slots[0].hash = 0x1122334455667788ull;
    slots[0].stage = Stage::kPixel;
    slots[0].index = 2501;
    slots[0].total = 8590;
    slots[0].name = "deadbeef";
    slots[0].bundle = "GXShader.shaderbnd.dcx";
    slots[0].entry = "GXFlver.ppo";

    slots[1].worker_id = 1;
    slots[1].active = false;
    slots[1].hash = 0x9999ull;

    slots[2].worker_id = 2;
    slots[2].active = true;
    slots[2].hash = 0xaabbccddeeff0011ull;
    slots[2].stage = Stage::kVertex;
    slots[2].index = 2502;
    slots[2].total = 8590;
    slots[2].name = "cafebabe";
    slots[2].bundle = "eboot.bin";
    slots[2].entry = "cafebabe";

    const std::string text = SerializeMultiWorkerInflight(slots);
    const std::vector<WorkerInflightSlot> parsed = ParseMultiWorkerInflight(text);
    CHECK_EQ(parsed.size(), 2u);
    CHECK_EQ(parsed[0].worker_id, 0u);
    CHECK_EQ(parsed[0].hash, 0x1122334455667788ull);
    CHECK(parsed[0].stage == Stage::kPixel);
    CHECK_EQ(parsed[0].index, 2501u);
    CHECK_EQ(parsed[0].total, 8590u);
    CHECK_EQ(parsed[0].name, std::string("deadbeef"));
    CHECK_EQ(parsed[0].bundle, std::string("GXShader.shaderbnd.dcx"));
    CHECK_EQ(parsed[1].worker_id, 2u);
    CHECK_EQ(parsed[1].hash, 0xaabbccddeeff0011ull);
    CHECK(parsed[1].stage == Stage::kVertex);

    // Header-only legacy multithread file parses as empty.
    CHECK(ParseMultiWorkerInflight("1\nmode=multithread\n").empty());
}

}  // namespace

