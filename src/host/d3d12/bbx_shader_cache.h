// SPDX-License-Identifier: GPL-3.0-or-later
// <dump>/bbx_shaders.cache, as the launcher loads it.
//
// The bake (tools/bbx_bake_cache.cpp) lifts the game's OrbShdr containers to
// SPIR-V, prints them as the console's dialect of HLSL (gcn::HlslOptions
// dxil_sm64), compiles each with dxc -T *_6_4, and writes every signed blob into
// one file with a stamp beside it. This side validates that file once at boot
// (ParseCache: magic, version, stamp, CRC, every entry's bounds, no duplicate
// keys) and then answers a look-up per program, without touching the file again.
//
// The stamp is two facts: the file's own (the bake's compiler and signer, and
// the translator's fingerprint) and this build's (dxil-6.4, tables, 11_0).
// ExpectedConsoleStamp pairs them, so a cache built for another target is
// refused by name instead of bound into a pipeline that would draw garbage.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "host/d3d12/bbx_cache.h"

namespace bbx::d3d12 {

// One program's DXIL, compressed by the bake: zlib, `raw_size` when inflated.
struct BakedBlob {
    const std::uint8_t* data = nullptr;
    std::uint32_t size = 0;
    std::uint32_t raw_size = 0;
};

class ShaderCache {
public:
    // `cache` is the file's bytes, `stamp_text` the sidecar the bake wrote
    // beside it (CacheStamp::Serialize). Anything ParseCache refuses leaves
    // entries() at zero: nothing is trusted by halves.
    CacheStatus Load(std::span<const std::uint8_t> cache, std::string_view stamp_text);

    // The interface hash's blob, as the bake compressed it. False when the
    // cache did not validate or has no such program.
    [[nodiscard]] bool Lookup(std::uint64_t key, BakedBlob* out) const;

    [[nodiscard]] std::size_t entries() const { return parsed_.entries.size(); }
    [[nodiscard]] std::size_t bytes() const { return file_.size(); }
    [[nodiscard]] CacheStatus status() const { return parsed_.status; }
    [[nodiscard]] const std::string& detail() const { return parsed_.detail; }

    // "ok: 25711 entries, 41.2 MiB" or "stamp mismatch: ...": the one line the
    // loader logs and the HUD shows.
    [[nodiscard]] std::string Report() const;

private:
    std::vector<std::uint8_t> file_;
    ParseResult parsed_;
    std::unordered_map<std::uint64_t, std::uint32_t> index_;  // interface hash -> entry
};

}  // namespace bbx::d3d12
