// SPDX-License-Identifier: GPL-3.0-or-later
// The DXIL/PSO cache: file format, stamp validation, and PSO keys.
//
// bbhost already keeps its translations and lifts between runs, stamped with a
// fingerprint over the translator's sources (cmake/translator_fingerprint.cmake).
// The port extends that stamp with the D3D12 identity -- the HLSL target, DXC's
// version, the root signature, the bindless mode, the feature level and the
// pipeline library's format -- because a DXIL blob built by another compiler is
// not invalid, it is simply not this compiler's, and a PSO built for another
// root signature is a crash waiting for the first draw.
//
// The file is deliberately simple: a fixed header, a sorted entry directory,
// zlib'd blobs. It is validated before a single entry is trusted.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bbx::d3d12 {

// The identity that invalidates everything when it changes.
using StampDigest = std::array<std::uint8_t, 16>;
struct CacheStamp {
    std::string translator;      // upstream BBHOST_TRANSLATOR_FINGERPRINT
    std::string dxil_target;     // "dxil-6.6"
    std::string hlsl_emitter;    // the DXIL target's version
    std::string dxc_version;     // "1.9.2609.23"
    std::string dxil_version;    // the signer's
    std::string root_signature;  // version tag
    std::string bindless;        // "direct" | "tables"
    std::string feature_level;   // "12_0"
    std::string pso_library;     // version tag

    [[nodiscard]] std::string Serialize() const;
    [[nodiscard]] static CacheStamp Parse(std::string_view text);
    // A 16-byte identity used as the cache's on-disk stamp and compared
    // exactly; the readable form lives in stamp.txt.
    [[nodiscard]] StampDigest Digest() const;
    [[nodiscard]] bool operator==(const CacheStamp& other) const {
        return Digest() == other.Digest();
    }
};

enum class Stage : std::uint8_t { kVertex = 0, kPixel = 1, kCompute = 2, kGeometry = 3, kHull = 4, kDomain = 5 };

constexpr std::uint32_t kCacheVersion = 1;
inline constexpr char kCacheMagic[8] = {'B', 'B', 'X', 'D', 'X', 'I', 'L', '\0'};

struct CacheEntry {
    std::uint64_t key = 0;           // interface hash (the lookup key)
    std::uint64_t program_hash = 0;  // the GCN container's hash
    std::uint64_t bytecode_hash = 0; // over the validated DXIL
    Stage stage = Stage::kVertex;
    bool fallback = false;           // built through SPIRV-Cross
    std::uint32_t offset = 0;        // into the blob region
    std::uint32_t size = 0;          // compressed size
    std::uint32_t raw_size = 0;
    std::uint64_t last_used = 0;     // monotic counter for LRU trimming
};

enum class CacheStatus : std::uint8_t {
    kOk = 0,
    kMissing,
    kNotACache,       // magic/version
    kStampMismatch,   // built by another translator/compiler/device
    kCorrupt,         // CRC, bounds, or an entry out of range
    kTooNew,
};

[[nodiscard]] const char* CacheStatusName(CacheStatus status);

// The identity this build gives a console shader cache. The bake
// (tools/bbx_bake_cache.cpp) stamps what it writes with ExpectedConsoleStamp,
// and the launcher validates it with the same function, so the two cannot drift:
// the DXIL target, the root signature, the bind mode and the feature level are
// the build's; the compiler and signer versions are whoever ran the bake, which
// the stamp beside the cache supplies.
inline constexpr char kConsoleDxilTarget[] = "dxil-6.4";      // SM 6.4 is the console's ceiling
inline constexpr char kConsoleRootSignature[] = "rs-1.0";     // root signature 1.0, 64 DWORDs
inline constexpr char kConsoleBindless[] = "tables";          // descriptor tables (tier 3)
inline constexpr char kConsoleFeatureLevel[] = "11_0";
inline constexpr char kConsoleHlslEmitter[] = "bbhost-hlsl-dxil64-1";
inline constexpr char kConsolePsoLibrary[] = "none";          // no pipeline library: it removes the device on a console
[[nodiscard]] CacheStamp ExpectedConsoleStamp(const CacheStamp& recorded = {});

struct ParseResult {
    CacheStatus status = CacheStatus::kMissing;
    std::string detail;
    std::vector<CacheEntry> entries;
    std::vector<std::uint8_t> blobs;  // the compressed region
    std::uint32_t crc = 0;
};

// Writes the file image. Entries must be sorted by key; blobs are already
// compressed (the caller owns compression so this stays dependency-free).
[[nodiscard]] std::vector<std::uint8_t> SerializeCache(const CacheStamp& stamp,
                                                       std::span<const CacheEntry> entries,
                                                       std::span<const std::uint8_t> blobs);

// Validates everything before trusting anything: magic, version, stamp,
// directory CRC, every entry's bounds, and no duplicate keys.
[[nodiscard]] ParseResult ParseCache(std::span<const std::uint8_t> bytes,
                                     const CacheStamp& expected);

// The PSO key. The fields are in the order that the canonical test pins; add
// to the end, never in the middle.
struct PsoKeyInput {
    std::uint64_t vs = 0;
    std::uint64_t ps = 0;
    std::uint64_t gs = 0;
    std::uint64_t hs = 0;
    std::uint64_t ds = 0;
    std::uint64_t input_layout = 0;
    std::uint32_t rtv_formats[8] = {};
    std::uint8_t rtv_count = 0;
    std::uint32_t dsv_format = 0;
    std::uint32_t sample_count = 1;
    std::uint32_t blend = 0;        // canonical blend state, already hashed
    std::uint32_t depth_stencil = 0;
    std::uint32_t rasterizer = 0;
    std::uint32_t root_signature = 0;
    std::uint32_t bindless = 0;
    std::uint32_t feature_level = 0;
    std::uint32_t pso_library = 0;
    std::uint32_t topology = 0;
};

// A 128-bit key as two 64-bit halves (a `PsoKey`), because a PSO collision
// draws the wrong pipeline rather than costing a rebuild.
struct PsoKey {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    [[nodiscard]] bool operator==(const PsoKey&) const = default;
};

[[nodiscard]] PsoKey MakePsoKey(const PsoKeyInput& input);
// The canonical byte serialisation, for tests and for logs when a key has to
// be compared across machines. 121 bytes: five 64-bit hashes, one 64-bit input
// layout, one count, eight RTV formats and nine 32-bit state words.
[[nodiscard]] std::vector<std::uint8_t> CanonicalPsoBytes(const PsoKeyInput& input);

// CRC-32 (IEEE), used for the entry directory.
[[nodiscard]] std::uint32_t Crc32(std::span<const std::uint8_t> bytes);

}  // namespace bbx::d3d12
