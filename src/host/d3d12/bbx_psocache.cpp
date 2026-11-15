// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/host/d3d12/bbx_psocache.h"

#include <algorithm>

namespace bbx::d3d12 {

std::uint64_t PsoHash(const PsoKey& key) {
    // The two halves are already independent 64-bit digests; mixing them costs
    // one multiply and avoids the list/hashtable ordering problems a plain
    // `lo` would bring on tables keyed by a single field (dxil sets).
    std::uint64_t hash = key.lo ^ (key.hi + 0x9e3779b97f4a7c15ull + (key.lo << 6) + (key.lo >> 2));
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    return hash;
}

PsoCache::PsoCache(std::uint64_t budget_bytes, std::uint32_t max_live)
    : budget_(budget_bytes), max_live_(max_live == 0 ? 1u : max_live) {}

bool PsoCache::Live(const PsoKey& key) const { return by_hash_.count(PsoHash(key)) != 0; }

const PsoEntryInfo* PsoCache::Find(const PsoKey& key) const {
    const auto it = by_hash_.find(PsoHash(key));
    if (it == by_hash_.end() || !(it->second.key == key)) return nullptr;
    return &it->second.info;
}

bool PsoCache::Acquire(const PsoKey& key, std::uint64_t now, PsoEntryInfo* out) {
    const auto it = by_hash_.find(PsoHash(key));
    if (it == by_hash_.end() || !(it->second.key == key)) {
        ++stats_.misses;
        return false;
    }
    Node& node = it->second;
    ++stats_.hits;
    node.info.last_used = now;
    node.info.uses += 1;
    lru_.erase(node.lru);
    lru_.push_front(key);
    node.lru = lru_.begin();
    if (out != nullptr) *out = node.info;
    return true;
}

std::vector<PsoKey> PsoCache::Insert(const PsoKey& key, std::uint64_t blob, std::uint64_t bytes,
                                     std::uint64_t now) {
    std::vector<PsoKey> evicted;
    if (bytes > budget_) {
        // A single pipeline larger than the whole allowance cannot be kept alive;
        // it is created per draw instead of thrashing the cache.
        ++stats_.rejected;
        return evicted;
    }
    const std::uint64_t hash = PsoHash(key);
    const auto existing = by_hash_.find(hash);
    if (existing != by_hash_.end() && existing->second.key == key) {
        // Same key, new object (a re-create after eviction): replace in place.
        stats_.live_bytes -= existing->second.info.bytes;
        bytes_ -= existing->second.info.bytes;
        existing->second.info.blob = blob;
        existing->second.info.bytes = bytes;
        existing->second.info.last_used = now;
        existing->second.info.uses += 1;
        stats_.live_bytes += bytes;
        bytes_ += bytes;
        lru_.erase(existing->second.lru);
        lru_.push_front(key);
        existing->second.lru = lru_.begin();
        stats_.peak_bytes = std::max(stats_.peak_bytes, stats_.live_bytes);
        return evicted;
    }

    // Make room: byte budget first, then the object cap. Eviction is by least
    // recent use, and the caller releases what comes back.
    const std::uint64_t need = bytes > budget_ - std::min(budget_, bytes_) ? 0 : 0;
    (void)need;
    while ((bytes_ + bytes > budget_ || by_hash_.size() + 1 > max_live_) && !lru_.empty()) {
        const PsoKey victim = lru_.back();
        lru_.pop_back();
        const auto node = by_hash_.find(PsoHash(victim));
        if (node == by_hash_.end()) continue;
        bytes_ -= node->second.info.bytes;
        stats_.live_bytes -= node->second.info.bytes;
        by_hash_.erase(node);
        ++stats_.evictions;
        evicted.push_back(victim);
    }

    Node node;
    node.key = key;
    node.info.key = key;
    node.info.blob = blob;
    node.info.bytes = bytes;
    node.info.last_used = now;
    node.info.uses = 1;
    lru_.push_front(key);
    node.lru = lru_.begin();
    by_hash_.emplace(hash, node);
    bytes_ += bytes;
    stats_.live_bytes += bytes;
    stats_.live = static_cast<std::uint32_t>(by_hash_.size());
    stats_.creates += 1;
    stats_.peak_bytes = std::max(stats_.peak_bytes, stats_.live_bytes);
    return evicted;
}

std::vector<PsoKey> PsoCache::EvictUntil(std::uint64_t bytes) {
    std::vector<PsoKey> evicted;
    while (bytes_ > bytes && !lru_.empty()) {
        const PsoKey victim = lru_.back();
        lru_.pop_back();
        const auto node = by_hash_.find(PsoHash(victim));
        if (node == by_hash_.end()) continue;
        bytes_ -= node->second.info.bytes;
        stats_.live_bytes -= node->second.info.bytes;
        by_hash_.erase(node);
        ++stats_.evictions;
        evicted.push_back(victim);
    }
    stats_.live = static_cast<std::uint32_t>(by_hash_.size());
    return evicted;
}

std::vector<PsoKey> PsoCache::Trim(std::uint64_t bytes) {
    const std::uint64_t target = bytes >= bytes_ ? 0 : bytes_ - bytes;
    return EvictUntil(target);
}

std::vector<PsoKey> PsoCache::Clear() {
    std::vector<PsoKey> keys;
    keys.reserve(by_hash_.size());
    for (const auto& [hash, node] : by_hash_) {
        (void)hash;
        keys.push_back(node.key);
    }
    by_hash_.clear();
    lru_.clear();
    bytes_ = 0;
    stats_.live_bytes = 0;
    stats_.live = 0;
    return keys;
}

}  // namespace bbx::d3d12
