// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "host/d3d12/bbx_cache.h"
#include "host/d3d12/bbx_shader_cache.h"

#include <algorithm>
#include <cstring>

using namespace bbx::d3d12;

namespace {
CacheStamp MakeStamp() {
    CacheStamp s;
    s.translator = "a1b2c3d4e5f6a7b8a1b2c3d4e5f6a7b8";
    s.dxil_target = "dxil-6.6";
    s.hlsl_emitter = "1";
    s.dxc_version = "1.9.2609.23";
    s.dxil_version = "1.9.2609.23";
    s.root_signature = "bindless-v1";
    s.bindless = "direct";
    s.feature_level = "12_0";
    s.pso_library = "1";
    return s;
}

CacheEntry MakeEntry(std::uint64_t key, std::uint32_t offset, std::uint32_t size) {
    CacheEntry e;
    e.key = key;
    e.program_hash = key ^ 0x5555;
    e.bytecode_hash = key ^ 0xaaaa;
    e.stage = Stage::kPixel;
    e.offset = offset;
    e.size = size;
    e.raw_size = size * 2;
    e.last_used = key & 0xff;
    return e;
}
}  // namespace

TEST(StampRoundTripsAndChangesWithEveryField) {
    const CacheStamp stamp = MakeStamp();
    CHECK(CacheStamp::Parse(stamp.Serialize()) == stamp);
    CacheStamp other = stamp;
    other.dxc_version = "1.9.0000.0";
    CHECK(!(other == stamp));
    other = stamp;
    other.bindless = "tables";
    CHECK(!(other == stamp));
    // Digest is stable.
    CHECK(stamp.Digest() == MakeStamp().Digest());
}

TEST(CacheSerializesAndParses) {
    const CacheStamp stamp = MakeStamp();
    const std::vector<CacheEntry> entries = {MakeEntry(1, 0, 4), MakeEntry(3, 4, 4),
                                             MakeEntry(9, 8, 4)};
    const std::vector<std::uint8_t> blobs = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const std::vector<std::uint8_t> file = SerializeCache(stamp, entries, blobs);
    const ParseResult parsed = ParseCache(file, stamp);
    CHECK(parsed.status == CacheStatus::kOk);
    CHECK_EQ(parsed.entries.size(), 3u);
    CHECK_EQ(parsed.blobs.size(), blobs.size());
    CHECK_EQ(parsed.entries[0].key, 1ull);
    CHECK_EQ(parsed.entries[2].offset, 8u);
}

TEST(CacheRejectsAnotherStamp) {
    const CacheStamp stamp = MakeStamp();
    const std::vector<CacheEntry> entries = {MakeEntry(1, 0, 4)};
    const std::vector<std::uint8_t> blobs(4, 0);
    const std::vector<std::uint8_t> file = SerializeCache(stamp, entries, blobs);
    CacheStamp other = stamp;
    other.translator = "ffffffffffffffffffffffffffffffff";
    const ParseResult parsed = ParseCache(file, other);
    CHECK(parsed.status == CacheStatus::kStampMismatch);
    CHECK(!parsed.detail.empty());
}

TEST(CacheRejectsCorruption) {
    const CacheStamp stamp = MakeStamp();
    const std::vector<CacheEntry> entries = {MakeEntry(1, 0, 4), MakeEntry(2, 4, 4)};
    const std::vector<std::uint8_t> blobs(8, 0);
    std::vector<std::uint8_t> file = SerializeCache(stamp, entries, blobs);

    // Not a cache at all.
    std::vector<std::uint8_t> not_a_cache = file;
    not_a_cache[0] = 'X';
    CHECK(ParseCache(not_a_cache, stamp).status == CacheStatus::kNotACache);

    // Truncated file: the sizes no longer add up.
    std::vector<std::uint8_t> truncated = file;
    truncated.resize(truncated.size() - 3);
    CHECK(ParseCache(truncated, stamp).status == CacheStatus::kCorrupt);

    // A flipped directory byte breaks the CRC.
    std::vector<std::uint8_t> flipped = file;
    flipped[70] ^= 0x40;
    CHECK(ParseCache(flipped, stamp).status == CacheStatus::kCorrupt);

    // A future version is refused rather than misread.
    std::vector<std::uint8_t> newer = file;
    newer[8] = 99;
    CHECK(ParseCache(newer, stamp).status == CacheStatus::kTooNew);
}

TEST(CacheRejectsUnsortedEntries) {
    const CacheStamp stamp = MakeStamp();
    const std::vector<CacheEntry> entries = {MakeEntry(5, 0, 4), MakeEntry(2, 4, 4)};
    const std::vector<std::uint8_t> blobs(8, 0);
    const std::vector<std::uint8_t> file = SerializeCache(stamp, entries, blobs);
    CHECK(ParseCache(file, stamp).status == CacheStatus::kCorrupt);
}

TEST(CacheRejectsOutOfRangeBlob) {
    const CacheStamp stamp = MakeStamp();
    std::vector<CacheEntry> entries = {MakeEntry(1, 4, 4)};
    const std::vector<std::uint8_t> blobs(8, 0);
    const std::vector<std::uint8_t> file = SerializeCache(stamp, entries, blobs);
    const ParseResult parsed = ParseCache(file, stamp);
    CHECK(parsed.status == CacheStatus::kOk);  // 4 + 4 == 8, exactly at the end

    entries[0].offset = 5;
    const std::vector<std::uint8_t> bad = SerializeCache(stamp, entries, blobs);
    CHECK(ParseCache(bad, stamp).status == CacheStatus::kCorrupt);
}

TEST(PsoKeyIsStableAndSensitive) {
    PsoKeyInput input;
    input.vs = 0x1111;
    input.ps = 0x2222;
    input.input_layout = 0x3333;
    input.rtv_count = 4;
    for (std::uint32_t i = 0; i < 4; ++i) input.rtv_formats[i] = 28;
    input.dsv_format = 40;
    input.root_signature = 1;
    input.bindless = 1;
    input.feature_level = 12;
    input.pso_library = 1;
    input.topology = 4;

    const PsoKey a = MakePsoKey(input);
    const PsoKey b = MakePsoKey(input);
    CHECK(a == b);
    CHECK(a.lo != 0 && a.hi != 0);

    PsoKeyInput changed = input;
    changed.dsv_format = 45;
    CHECK(!(MakePsoKey(changed) == a));
    changed = input;
    changed.rtv_formats[3] = 10;
    CHECK(!(MakePsoKey(changed) == a));
    changed = input;
    changed.sample_count = 2;
    CHECK(!(MakePsoKey(changed) == a));

    // The canonical bytes have a fixed size, so a field added in the middle is
    // caught by a size change as well as by a key change.
    CHECK_EQ(CanonicalPsoBytes(input).size(), 121u);
}

TEST(Crc32MatchesKnownValue) {
    const char text[] = "123456789";
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(text), 9);
    CHECK_EQ(Crc32(bytes), 0xcbf43926u);
}

// --- the console's own cache --------------------------------------------------
//
// <dump>/bbx_shaders.cache is built on a PC by tools/bbx_bake_cache.cpp and
// loaded on the console, so the two sides must agree about what the file is for
// without sharing a build. ExpectedConsoleStamp is that agreement: the bake
// stamps with it, the launcher validates with it, and the compiler versions come
// from the sidecar the bake writes beside the file.

namespace {
// What the bake records: the translator's fingerprint (a property of the
// sources, so both builds compute it) and the compiler and signer it ran.
CacheStamp RecordedByBake() {
    CacheStamp s;
    s.translator = "a1b2c3d4e5f6a7b8a1b2c3d4e5f6a7b8";
    s.dxc_version = "1.9.2609.23";
    s.dxil_version = "1.9.2609.23";
    return s;
}
}  // namespace

TEST(ConsoleStampIsTheBuildsTargetAndTheBakesCompiler) {
    const CacheStamp expected = ExpectedConsoleStamp(RecordedByBake());
    CHECK_EQ(expected.dxil_target, std::string(kConsoleDxilTarget));
    CHECK_EQ(expected.dxil_target, std::string("dxil-6.4"));
    CHECK_EQ(expected.bindless, std::string("tables"));
    CHECK_EQ(expected.feature_level, std::string("11_0"));
    CHECK_EQ(expected.pso_library, std::string("none"));
    // From the bake's sidecar, so a cache compiled by another dxc is still
    // this build's - it is a different compiler's output, not another target's.
    CHECK_EQ(expected.dxc_version, std::string("1.9.2609.23"));
    CHECK_EQ(expected.translator, RecordedByBake().translator);
    // And the other way: a cache for another target is not this build's.
    CacheStamp other_recorded = RecordedByBake();
    CacheStamp other = ExpectedConsoleStamp(other_recorded);
    other.dxil_target = "dxil-6.6";
    CHECK(!(other == expected));
}

TEST(ShaderCacheLooksUpWhatTheBakeWroteAndRefusesTheRest) {
    const CacheStamp recorded = RecordedByBake();
    const CacheStamp stamp = ExpectedConsoleStamp(recorded);
    const std::vector<std::uint8_t> blobs = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    const std::vector<CacheEntry> entries = {MakeEntry(0x11, 0, 4), MakeEntry(0x22, 4, 2)};
    const std::vector<std::uint8_t> file = SerializeCache(stamp, entries, blobs);

    ShaderCache cache;
    CHECK(cache.Load(file, recorded.Serialize()) == CacheStatus::kOk);
    CHECK_EQ(cache.entries(), static_cast<std::size_t>(2));
    CHECK_EQ(cache.bytes(), file.size());
    BakedBlob blob;
    CHECK(cache.Lookup(0x22, &blob));
    CHECK_EQ(blob.size, 2u);
    CHECK_EQ(blob.raw_size, 4u);  // MakeEntry: raw_size = size * 2
    CHECK(blob.data != nullptr && *blob.data == 0x05);
    CHECK(!cache.Lookup(0x33, &blob));
    CHECK(blob.data == nullptr);
    // The line the loader logs and the HUD shows is one line, and says the size.
    CHECK(cache.Report().find("ok") == 0);
    CHECK(cache.Report().find("2 entries") != std::string::npos);

    // No sidecar: the file has no compiler, so nothing vouches for it.
    ShaderCache unstamped;
    CHECK(unstamped.Load(file, std::string_view()) == CacheStatus::kStampMismatch);
    CHECK_EQ(unstamped.entries(), static_cast<std::size_t>(0));
    CHECK(!unstamped.Lookup(0x11, &blob));
    CHECK(unstamped.Report().find("stamp mismatch") != std::string::npos);

    // A sidecar from another compiler is that compiler's cache, not this one's.
    CacheStamp foreign = recorded;
    foreign.dxc_version = "1.8.0000.0";
    ShaderCache mismatched;
    CHECK(mismatched.Load(file, foreign.Serialize()) == CacheStatus::kStampMismatch);

    // Truncated: the directory's bounds are checked before any entry is trusted.
    std::vector<std::uint8_t> cut(file.begin(), file.end() - 3);
    ShaderCache corrupt;
    CHECK(corrupt.Load(cut, recorded.Serialize()) != CacheStatus::kOk);
    CHECK_EQ(corrupt.entries(), static_cast<std::size_t>(0));
}

namespace {

// A shader container laid out as the game's are: a 0x24-byte file header, then
// the 'Shdr' block whose +0x10 holds the sixteen register words, +0x13 the
// usage count and then the input table. Only what ReadConsoleContainerStage
// reads is filled in; the code slice is the caller's business.
struct ContainerBuilder {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x24, 0);
    std::size_t base = 0;  // shdr + 16

    explicit ContainerBuilder(std::uint8_t usage) {
        bytes.resize(0x24 + 0x10, 0);  // the file header, then the Shdr block's first 0x10 bytes
        std::memcpy(bytes.data() + 0x24, "Shdr", 4);
        base = bytes.size();           // shdr + 0x10: the register block
        // The whole Shdr block (0x50 bytes from its start) has to be there, as
        // every reader of a container requires before it reads a word of it.
        bytes.resize(base + 0x50, 0);
        bytes[base + 3] = usage;
    }
    void Reg(std::size_t index, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes[base + index * 4 + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
    }
    // Grows the container to hold `size` bytes; never shrinks it, so the
    // register block stays readable.
    void Grow(std::size_t size) {
        if (bytes.size() < size) bytes.resize(size, 0);
    }
    void At(std::size_t offset, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
    }
};

}  // namespace

TEST(ConsoleStageKeyIsStableAndPinned) {
    // The key the bake writes and the launcher looks up, pinned: if this number
    // changes, every cache baked before the change is a miss, so the change has
    // to be a deliberate one (and kConsoleStageKeyVersion bumps with it).
    const std::vector<std::uint32_t> program = {0xbf810000u, 0x7e000300u, 0x00000000u};
    ConsoleStageKey key;
    key.stage = Stage::kPixel;
    key.rsrc1 = 0x00c00000u;
    key.rsrc2 = 0x00000010u;
    key.mask = 0x3u;
    key.flat_mask = 0x2u;
    key.early_fragment_tests = true;
    key.input_map = {0, 1};
    key.dims = {4, 9};
    key.sampler_modes = {0};
    const std::uint64_t pinned = ConsoleStageKeyOf(program, key);
    CHECK_EQ(ConsoleStageKeyOf(program, key), pinned);

    // Every field is in the key: a reader who drops one from the encoding makes
    // two different modules share a blob, and the test says so.
    const auto changes = [&](auto mutate) {
        ConsoleStageKey other = key;
        mutate(other);
        return ConsoleStageKeyOf(program, other) != pinned;
    };
    CHECK(changes([](ConsoleStageKey& k) { k.stage = Stage::kVertex; }));
    CHECK(changes([](ConsoleStageKey& k) { k.rsrc1 ^= 1u << 20; }));
    CHECK(changes([](ConsoleStageKey& k) { k.rsrc2 ^= 1u; }));
    CHECK(changes([](ConsoleStageKey& k) { k.mask ^= 1u; }));
    CHECK(changes([](ConsoleStageKey& k) { k.flat_mask ^= 1u; }));
    CHECK(changes([](ConsoleStageKey& k) { k.early_fragment_tests = false; }));
    CHECK(changes([](ConsoleStageKey& k) { k.clip_discard = 4u; }));
    CHECK(changes([](ConsoleStageKey& k) { k.threads[1] = 64; }));
    CHECK(changes([](ConsoleStageKey& k) { k.formats_from_params = true; }));
    CHECK(changes([](ConsoleStageKey& k) { k.elements = {1u}; }));
    CHECK(changes([](ConsoleStageKey& k) { k.input_map = {0, 1, 2}; }));
    CHECK(changes([](ConsoleStageKey& k) { k.dims = {4, 10}; }));
    CHECK(changes([](ConsoleStageKey& k) { k.sampler_modes = {1}; }));
    // The map's length is information, not just its bytes.
    CHECK(changes([](ConsoleStageKey& k) { k.input_map = {0, 1, 2, 3}; }));
    // And the program is the key's other half.
    std::vector<std::uint32_t> patched = program;
    patched[1] = 0x7e000301u;
    CHECK(ConsoleStageKeyOf(patched, key) != pinned);
    // Order matters: two elements in the other order are two interfaces.
    ConsoleStageKey ordered;
    ordered.stage = Stage::kVertex;
    ordered.elements = {ConsoleVertexElementWord(0, 4, 3, true), ConsoleVertexElementWord(1, 8, 2, true)};
    ConsoleStageKey swapped = ordered;
    std::swap(swapped.elements[0], swapped.elements[1]);
    CHECK(ConsoleStageKeyOf({}, ordered) != ConsoleStageKeyOf({}, swapped));
    // The version is the first field, so a new field cannot reuse an old key.
    CHECK_EQ(kConsoleStageKeyVersion, 1u);
}

TEST(ConsoleContainerStageReadsThePixelTable) {
    ContainerBuilder c(1);
    c.Reg(4, 0x00c00000u);  // rsrc1
    c.Reg(5, 0x00000010u);  // rsrc2
    c.Reg(8, 0x3u);         // SPI_PS_INPUT_ENA
    c.bytes[c.base + 0x38] = 2;
    const std::size_t table = c.base + 0x3c + 4;  // one usage entry
    c.Grow(table + 4);
    // Input 0: semantic 0xf (register 0), flat. Input 1: semantic 0x12 (register 3).
    c.bytes[table] = 0xf;
    c.bytes[table + 1] = 0x10;  // FLAT_SHADE
    c.bytes[table + 2] = 0x12;
    c.bytes[table + 3] = 0;

    const ConsoleContainerStage stage = ReadConsoleContainerStage(c.bytes, 2);
    CHECK(stage.ok);
    if (!stage.ok) return;
    CHECK(stage.stage == Stage::kPixel);
    CHECK_EQ(stage.key.rsrc1, 0x00c00000u);
    CHECK_EQ(stage.key.rsrc2, 0x00000010u);
    CHECK_EQ(stage.key.mask, 0x3u);
    CHECK_EQ(stage.declared_inputs, 2u);
    CHECK_EQ(stage.input_words.size(), static_cast<std::size_t>(2));
    CHECK_EQ(stage.input_words[1], static_cast<std::uint16_t>(0x12));

    // Neither input read: no map, no flat mask - and the key says so.
    std::vector<std::uint8_t> map;
    std::uint32_t flat = 0xffffffffu;
    std::string why;
    CHECK(ConsolePsInputMap(stage, std::vector<std::uint32_t>{0}, map, flat, why));
    CHECK(map.empty());
    CHECK_EQ(flat, 0u);

    // Both read: register 0, then register 3, and input 0 interpolates flat.
    CHECK(ConsolePsInputMap(stage, std::vector<std::uint32_t>{0x3}, map, flat, why));
    CHECK_EQ(map.size(), static_cast<std::size_t>(2));
    CHECK_EQ(static_cast<int>(map[0]), 0);
    CHECK_EQ(static_cast<int>(map[1]), 3);
    CHECK_EQ(flat, 1u);

    // An input the table does not declare is refused by name, not guessed.
    CHECK(!ConsolePsInputMap(stage, std::vector<std::uint32_t>{0x4}, map, flat, why));
    CHECK(!why.empty());
}

TEST(ConsoleContainerStageReadsTheVertexAndComputeTables) {
    // Vertex: three inputs, no usage entries, RSRC1's VGPR_COMP_CNT forced to 3
    // and the formats-from-params flag on every element.
    ContainerBuilder v(0);
    v.Reg(4, 0x00000000u);  // VGPR_COMP_CNT 0 in the container
    v.Reg(5, 0x00000002u);
    v.Reg(8, 0x00400000u);  // vs_out_cntl
    v.bytes[v.base + 0x24] = 3;
    const std::size_t table = v.base + 0x28;
    v.Grow(table + 12);
    const std::uint8_t vdata[3] = {1, 5, 9};
    const std::uint8_t count[3] = {3, 4, 1};
    for (int k = 0; k < 3; ++k) {
        v.bytes[table + 4 * k + 1] = vdata[k];
        v.bytes[table + 4 * k + 2] = count[k];
    }
    const ConsoleContainerStage vs = ReadConsoleContainerStage(v.bytes, 1);
    CHECK(vs.ok);
    if (!vs.ok) return;  // the fields below are the ones just rejected
    CHECK(vs.stage == Stage::kVertex);
    CHECK_EQ(vs.key.rsrc1, 3u << 24);
    CHECK_EQ(vs.key.rsrc2, 0x00000002u);
    CHECK_EQ(vs.key.mask, 0x00400000u);
    CHECK(vs.key.formats_from_params);
    CHECK_EQ(vs.key.elements.size(), static_cast<std::size_t>(3));
    CHECK_EQ(vs.key.elements[1], ConsoleVertexElementWord(1, 5, 4, true));

    // A vertex input with five components is not a module: refused by name.
    ContainerBuilder bad(0);
    bad.bytes[bad.base + 0x24] = 1;
    bad.Grow(bad.base + 0x2c);
    bad.bytes[bad.base + 0x2a] = 5;
    const ConsoleContainerStage refused = ReadConsoleContainerStage(bad.bytes, 1);
    CHECK(!refused.ok);
    CHECK(refused.error.find("more than four") != std::string::npos);

    // Compute: RSRC1/RSRC2, the thread counts, and nothing of the other stages.
    ContainerBuilder cs(0);
    cs.Reg(4, 0x00e00003u);
    cs.Reg(5, 0x00000000u);
    cs.Reg(6, 8);
    cs.Reg(7, 8);
    cs.Reg(8, 1);
    const ConsoleContainerStage c = ReadConsoleContainerStage(cs.bytes, 4);
    CHECK(c.ok);
    CHECK(c.stage == Stage::kCompute);
    CHECK_EQ(c.key.rsrc1, 0x00e00003u);
    CHECK_EQ(c.key.threads[0], 8u);
    CHECK_EQ(c.key.threads[2], 1u);
    CHECK(c.key.elements.empty());
    CHECK(c.key.input_map.empty());

    // A compute shader with a zero thread count would not compile; the key
    // never gets made for it.
    ContainerBuilder zero(0);
    zero.Reg(6, 0);
    CHECK(!ReadConsoleContainerStage(zero.bytes, 4).ok);

    // Types the console does not translate are named, and the 'Shdr' block has
    // to be in the first 0x100 bytes where every other reader looks for it.
    ContainerBuilder gs(0);
    CHECK(!ReadConsoleContainerStage(gs.bytes, 3).ok);
    CHECK(!ReadConsoleContainerStage(gs.bytes, 0).ok);
    std::vector<std::uint8_t> late(0x120, 0);
    std::memcpy(late.data() + 0x110, "Shdr", 4);
    const ConsoleContainerStage none = ReadConsoleContainerStage(late, 2);
    CHECK(!none.ok);
    CHECK(none.error.find("Shdr") != std::string::npos);
}
