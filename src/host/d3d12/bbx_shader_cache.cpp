// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_shader_cache.h"

#include <cstdio>

namespace bbx::d3d12 {

CacheStatus ShaderCache::Load(std::span<const std::uint8_t> cache, std::string_view stamp_text) {
    file_.assign(cache.begin(), cache.end());
    index_.clear();
    // The stamp beside the file says who compiled it; this build says for what.
    // A file without its sidecar has no second half, so it is refused rather
    // than trusted on its own word.
    const CacheStamp recorded = CacheStamp::Parse(stamp_text);
    parsed_ = ParseCache(file_, ExpectedConsoleStamp(recorded));
    if (parsed_.status != CacheStatus::kOk) return parsed_.status;
    index_.reserve(parsed_.entries.size());
    for (std::uint32_t i = 0; i < parsed_.entries.size(); ++i) index_.emplace(parsed_.entries[i].key, i);
    return parsed_.status;
}

bool ShaderCache::Lookup(std::uint64_t key, BakedBlob* out) const {
    if (out != nullptr) *out = BakedBlob{};
    if (parsed_.status != CacheStatus::kOk) return false;
    const auto it = index_.find(key);
    if (it == index_.end()) return false;
    const CacheEntry& entry = parsed_.entries[it->second];
    // ParseCache validated this range; it is re-stated here so the pointer is
    // never formed outside the blob region even if the file were swapped under
    // the process after validation.
    if (static_cast<std::size_t>(entry.offset) + entry.size > parsed_.blobs.size()) return false;
    if (out != nullptr) {
        out->data = parsed_.blobs.data() + entry.offset;
        out->size = entry.size;
        out->raw_size = entry.raw_size;
    }
    return true;
}

std::string ShaderCache::Report() const {
    // The HUD's font is uppercase 3x5 with a small punctuation set, so the line
    // stays short ASCII: a status, a count and a size. The long form of the
    // status, and its detail, are the loader's log line.
    const char* name = "?";
    switch (parsed_.status) {
    case CacheStatus::kOk: name = "ok"; break;
    case CacheStatus::kMissing: name = "missing"; break;
    case CacheStatus::kNotACache: name = "not a cache"; break;
    case CacheStatus::kStampMismatch: name = "stamp mismatch"; break;
    case CacheStatus::kCorrupt: name = "corrupt"; break;
    case CacheStatus::kTooNew: name = "too new"; break;
    }
    char text[128];
    const double mib = static_cast<double>(file_.size()) / (1024.0 * 1024.0);
    if (parsed_.status == CacheStatus::kOk) {
        std::snprintf(text, sizeof(text), "%s: %zu entries, %.1f MiB", name, entries(), mib);
    } else {
        std::snprintf(text, sizeof(text), "%s: %zu bytes", name, file_.size());
    }
    return text;
}

}  // namespace bbx::d3d12
