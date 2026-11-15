// SPDX-License-Identifier: GPL-3.0-or-later
// Live pipelines, kept small on purpose.
//
// Two hardware facts decide this file's design:
//
//  * `ID3D12PipelineLibrary` is not an option on the console: `CreatePipelineLibrary`
//    removes the device on SraKmd_arden. So the disk cache stores **signed DXIL
//    blobs**, never a pipeline library, and a pipeline is created from its blob
//    the first time a draw needs it (`docs/precompile-cache.md`).
//  * thousands of live `ID3D12PipelineState` objects do not fit 5,120 MiB. The
//    prior art's ahead-of-time creation ran the game out of memory; the port keeps
//    a bounded LRU instead and re-creates from the cached blob when it evicts.
//
// The cache is portable: it stores opaque blob ids and byte counts, and the
// backend supplies "create this PSO from this blob". That is what lets the
// eviction rules be tested without a GPU.
#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "src/host/d3d12/bbx_cache.h"

namespace bbx::d3d12 {

struct PsoEntryInfo {
    PsoKey key{};
    std::uint64_t blob = 0;        // id of the cached DXIL blob set
    std::uint64_t bytes = 0;       // the driver's estimate for the live object
    std::uint64_t last_used = 0;   // tick of the last draw that asked for it
    std::uint32_t uses = 0;
};

struct PsoStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t creates = 0;
    std::uint64_t evictions = 0;
    std::uint64_t rejected = 0;   // a PSO that alone exceeds the budget
    std::uint64_t live_bytes = 0;
    std::uint64_t peak_bytes = 0;
    std::uint32_t live = 0;
};

class PsoCache {
public:
    // `budget_bytes` comes from the budget's `Owner::kPipelines` (192 MiB by
    // default). `max_live` is the console's object cap; the default is measured:
    // a world frame touches ~900 distinct pipelines, a level's precompile ~1,900.
    explicit PsoCache(std::uint64_t budget_bytes, std::uint32_t max_live = 2048);

    // A draw asks for its pipeline. On a miss the caller compiles/creates it and
    // calls Insert; the returned flag says whether it is already live.
    bool Acquire(const PsoKey& key, std::uint64_t now, PsoEntryInfo* out = nullptr);

    // Registers a newly created pipeline. May evict others to make room; the
    // evicted keys are returned so the caller can release their resources.
    std::vector<PsoKey> Insert(const PsoKey& key, std::uint64_t blob, std::uint64_t bytes,
                               std::uint64_t now);

    // Drops the least recently used entries until `bytes` are free. Used when the
    // pager asks for headroom (level changes, suspends).
    std::vector<PsoKey> Trim(std::uint64_t bytes);

    // Everything, e.g. when the guest session ends.
    std::vector<PsoKey> Clear();

    [[nodiscard]] const PsoStats& Stats() const { return stats_; }
    [[nodiscard]] bool Live(const PsoKey& key) const;
    [[nodiscard]] const PsoEntryInfo* Find(const PsoKey& key) const;

private:
    struct Node {
        PsoKey key;
        PsoEntryInfo info;
        std::list<PsoKey>::iterator lru;
    };
    std::vector<PsoKey> EvictUntil(std::uint64_t bytes);

    std::uint64_t budget_ = 0;
    std::uint32_t max_live_ = 0;
    std::uint64_t bytes_ = 0;
    std::unordered_map<std::uint64_t, Node> by_hash_;
    std::list<PsoKey> lru_;  // front = most recently used
    PsoStats stats_;
};

// The hash the cache keys on: `PsoKey`'s digest (already 128-bit in bbx_cache).
[[nodiscard]] std::uint64_t PsoHash(const PsoKey& key);

}  // namespace bbx::d3d12
