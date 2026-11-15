// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "host/d3d12/bbx_cache.h"

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
