// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "src/host/d3d12/bbx_psocache.h"

using namespace bbx::d3d12;

namespace {

PsoKey Key(std::uint64_t n) { return PsoKey{n, n * 0x9e3779b97f4a7c15ull}; }

}  // namespace

TEST(PsoCacheHitsAndMisses) {
    PsoCache cache(1u << 20, 8);
    PsoEntryInfo info;
    CHECK(!cache.Acquire(Key(1), 1, &info));
    CHECK_EQ(cache.Stats().misses, 1ull);
    CHECK(cache.Insert(Key(1), 7, 1000, 1).empty());
    CHECK(cache.Acquire(Key(1), 2, &info));
    CHECK_EQ(info.blob, 7ull);
    CHECK_EQ(cache.Stats().hits, 1ull);
    CHECK_EQ(cache.Stats().live, 1u);
    CHECK(cache.Live(Key(1)));
    // A different key with the same low half is not a hit.
    CHECK(!cache.Live(PsoKey{1, 2}));
}

TEST(PsoCacheEvictsLeastRecentlyUsedOverBytes) {
    PsoCache cache(2500, 8);
    CHECK(cache.Insert(Key(1), 1, 1000, 1).empty());
    CHECK(cache.Insert(Key(2), 2, 1000, 2).empty());
    PsoEntryInfo info;
    CHECK(cache.Acquire(Key(1), 3, &info));  // 1 now newer than 2
    const std::vector<PsoKey> evicted = cache.Insert(Key(3), 3, 1000, 4);
    CHECK_EQ(evicted.size(), static_cast<std::size_t>(1));
    CHECK(evicted[0] == Key(2));
    CHECK(cache.Live(Key(1)));
    CHECK(cache.Live(Key(3)));
    CHECK(!cache.Live(Key(2)));
    CHECK_EQ(cache.Stats().evictions, 1ull);
}

TEST(PsoCacheEvictsOverTheObjectCap) {
    PsoCache cache(1u << 20, 3);
    for (std::uint64_t i = 1; i <= 3; ++i) CHECK(cache.Insert(Key(i), i, 10, i).empty());
    const std::vector<PsoKey> evicted = cache.Insert(Key(4), 4, 10, 4);
    CHECK_EQ(evicted.size(), static_cast<std::size_t>(1));
    CHECK(evicted[0] == Key(1));
    CHECK_EQ(cache.Stats().live, 3u);
}

TEST(PsoCacheRejectsAPipelineBiggerThanTheBudget) {
    PsoCache cache(1000, 8);
    CHECK(cache.Insert(Key(1), 1, 1001, 1).empty());
    CHECK(!cache.Live(Key(1)));
    CHECK_EQ(cache.Stats().rejected, 1ull);
    CHECK_EQ(cache.Stats().live_bytes, 0ull);
}

TEST(PsoCacheTrimAndClear) {
    PsoCache cache(1u << 20, 8);
    for (std::uint64_t i = 1; i <= 4; ++i) CHECK(cache.Insert(Key(i), i, 100, i).empty());
    // Trim wants 250 bytes back: entry 1 (oldest) is not enough, so it takes two.
    const std::vector<PsoKey> trimmed = cache.Trim(250);
    CHECK_EQ(trimmed.size(), static_cast<std::size_t>(3));
    CHECK_EQ(cache.Stats().live, 1u);
    const std::vector<PsoKey> all = cache.Clear();
    CHECK_EQ(all.size(), static_cast<std::size_t>(1));
    CHECK_EQ(cache.Stats().live_bytes, 0ull);
}

TEST(PsoCacheReportsPeakBytes) {
    PsoCache cache(1u << 20, 8);
    CHECK(cache.Insert(Key(1), 1, 100, 1).empty());
    CHECK(cache.Insert(Key(2), 2, 250, 2).empty());
    CHECK_EQ(cache.Stats().peak_bytes, 350ull);
    CHECK(cache.Trim(300).size() >= 1);
}
