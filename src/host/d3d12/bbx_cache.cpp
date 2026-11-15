// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_cache.h"

#include "host/d3d12/bbx_common.h"

#include <algorithm>
#include <cstring>

namespace bbx::d3d12 {
namespace {

constexpr std::size_t kHeaderSize = 64;
constexpr std::size_t kEntrySize = 48;

void PutU8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void PutU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint8_t GetU8(std::span<const std::uint8_t> bytes, std::size_t at) {
    return at < bytes.size() ? bytes[at] : 0;
}

std::uint32_t GetU32(std::span<const std::uint8_t> bytes, std::size_t at) {
    if (at + 4 > bytes.size()) return 0;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(bytes[at + i]) << (8 * i);
    return v;
}

std::uint64_t GetU64(std::span<const std::uint8_t> bytes, std::size_t at) {
    if (at + 8 > bytes.size()) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(bytes[at + i]) << (8 * i);
    return v;
}

void PutStamp(std::vector<std::uint8_t>& out, const StampDigest& digest) {
    out.insert(out.end(), digest.begin(), digest.end());
}

}  // namespace

std::string CacheStamp::Serialize() const {
    std::string out;
    const auto add = [&out](std::string_view key, const std::string& value) {
        out += key;
        out += '=';
        out += value;
        out += '\n';
    };
    add("translator", translator);
    add("dxil_target", dxil_target);
    add("hlsl_emitter", hlsl_emitter);
    add("dxc", dxc_version);
    add("dxil", dxil_version);
    add("root_signature", root_signature);
    add("bindless", bindless);
    add("feature_level", feature_level);
    add("pso_library", pso_library);
    return out;
}

CacheStamp CacheStamp::Parse(std::string_view text) {
    CacheStamp stamp;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);
        if (key == "translator") stamp.translator = value;
        else if (key == "dxil_target") stamp.dxil_target = value;
        else if (key == "hlsl_emitter") stamp.hlsl_emitter = value;
        else if (key == "dxc") stamp.dxc_version = value;
        else if (key == "dxil") stamp.dxil_version = value;
        else if (key == "root_signature") stamp.root_signature = value;
        else if (key == "bindless") stamp.bindless = value;
        else if (key == "feature_level") stamp.feature_level = value;
        else if (key == "pso_library") stamp.pso_library = value;
    }
    return stamp;
}

StampDigest CacheStamp::Digest() const {
    const std::string text = Serialize();
    const std::uint64_t a = Fnv1a64(text);
    const std::uint64_t b = Mix64(a ^ 0x9e3779b97f4a7c15ull);
    StampDigest digest{};
    std::memcpy(digest.data(), &a, 8);
    std::memcpy(digest.data() + 8, &b, 8);
    return digest;
}

const char* CacheStatusName(CacheStatus status) {
    switch (status) {
    case CacheStatus::kOk: return "ok";
    case CacheStatus::kMissing: return "missing";
    case CacheStatus::kNotACache: return "not a cache file";
    case CacheStatus::kStampMismatch: return "built by another translator/compiler/device";
    case CacheStatus::kCorrupt: return "corrupt";
    case CacheStatus::kTooNew: return "newer format";
    }
    return "?";
}

std::vector<std::uint8_t> SerializeCache(const CacheStamp& stamp,
                                         std::span<const CacheEntry> entries,
                                         std::span<const std::uint8_t> blobs) {
    std::vector<std::uint8_t> dir;
    dir.reserve(entries.size() * kEntrySize);
    for (const CacheEntry& e : entries) {
        PutU64(dir, e.key);
        PutU64(dir, e.program_hash);
        PutU64(dir, e.bytecode_hash);
        PutU32(dir, e.offset);
        PutU32(dir, e.size);
        PutU32(dir, e.raw_size);
        PutU8(dir, static_cast<std::uint8_t>(e.stage));
        PutU8(dir, e.fallback ? 1u : 0u);
        PutU16(dir, 0);
        PutU64(dir, e.last_used);
    }
    const std::uint32_t crc = Crc32(dir);

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + dir.size() + blobs.size());
    // memcpy rather than insert: GCC 12's -Wstringop-overflow mis-reads the
    // char -> uint8_t widening insert as an 8-byte write into a 7-byte object.
    const std::size_t magic_at = out.size();
    out.resize(magic_at + sizeof(kCacheMagic));
    std::memcpy(out.data() + magic_at, kCacheMagic, sizeof(kCacheMagic));
    PutU32(out, kCacheVersion);
    PutU32(out, static_cast<std::uint32_t>(kEntrySize));
    PutU64(out, static_cast<std::uint64_t>(entries.size()));
    PutStamp(out, stamp.Digest());
    PutU32(out, crc);
    PutU32(out, 0);  // flags
    PutU64(out, static_cast<std::uint64_t>(blobs.size()));
    PutU64(out, 0);  // reserved
    out.insert(out.end(), dir.begin(), dir.end());
    out.insert(out.end(), blobs.begin(), blobs.end());
    return out;
}

ParseResult ParseCache(std::span<const std::uint8_t> bytes, const CacheStamp& expected) {
    ParseResult result;
    if (bytes.empty()) {
        result.status = CacheStatus::kMissing;
        return result;
    }
    if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), kCacheMagic, 8) != 0) {
        result.status = CacheStatus::kNotACache;
        result.detail = "header too small or magic differs";
        return result;
    }
    const std::uint32_t version = GetU32(bytes, 8);
    if (version > kCacheVersion) {
        result.status = CacheStatus::kTooNew;
        result.detail = "version " + std::to_string(version);
        return result;
    }
    const std::uint32_t entry_size = GetU32(bytes, 12);
    if (version != kCacheVersion || entry_size != kEntrySize) {
        result.status = CacheStatus::kNotACache;
        result.detail = "entry size " + std::to_string(entry_size);
        return result;
    }
    const std::uint64_t count = GetU64(bytes, 16);
    StampDigest digest{};
    std::memcpy(digest.data(), bytes.data() + 24, digest.size());
    if (digest != expected.Digest()) {
        result.status = CacheStatus::kStampMismatch;
        result.detail = "stamp differs from this build's";
        return result;
    }
    const std::uint32_t stored_crc = GetU32(bytes, 40);
    const std::uint64_t blob_size = GetU64(bytes, 48);

    // Count * entry size, checked against the file rather than trusted.
    if (count > (bytes.size() - kHeaderSize) / kEntrySize) {
        result.status = CacheStatus::kCorrupt;
        result.detail = "entry count out of bounds";
        return result;
    }
    const std::size_t dir_bytes = static_cast<std::size_t>(count) * kEntrySize;
    if (kHeaderSize + dir_bytes + blob_size != bytes.size()) {
        result.status = CacheStatus::kCorrupt;
        result.detail = "size does not match the directory and blob region";
        return result;
    }
    const std::span<const std::uint8_t> dir = bytes.subspan(kHeaderSize, dir_bytes);
    if (Crc32(dir) != stored_crc) {
        result.status = CacheStatus::kCorrupt;
        result.detail = "directory CRC mismatch";
        return result;
    }
    result.crc = stored_crc;

    const std::span<const std::uint8_t> blob_region =
        bytes.subspan(kHeaderSize + dir_bytes, static_cast<std::size_t>(blob_size));
    result.entries.reserve(static_cast<std::size_t>(count));
    std::uint64_t previous_key = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::size_t at = static_cast<std::size_t>(i) * kEntrySize;
        CacheEntry e;
        e.key = GetU64(dir, at + 0);
        e.program_hash = GetU64(dir, at + 8);
        e.bytecode_hash = GetU64(dir, at + 16);
        e.offset = GetU32(dir, at + 24);
        e.size = GetU32(dir, at + 28);
        e.raw_size = GetU32(dir, at + 32);
        e.stage = static_cast<Stage>(GetU8(dir, at + 36));
        e.fallback = GetU8(dir, at + 37) != 0;
        e.last_used = GetU64(dir, at + 40);
        if (static_cast<std::uint32_t>(e.stage) > static_cast<std::uint32_t>(Stage::kDomain)) {
            result.status = CacheStatus::kCorrupt;
            result.detail = "entry " + std::to_string(i) + " has an unknown stage";
            return result;
        }
        if (e.size == 0 || static_cast<std::uint64_t>(e.offset) + e.size > blob_size) {
            result.status = CacheStatus::kCorrupt;
            result.detail = "entry " + std::to_string(i) + " has an out-of-range blob";
            return result;
        }
        if (i > 0 && e.key <= previous_key) {
            result.status = CacheStatus::kCorrupt;
            result.detail = "entries are not sorted and unique";
            return result;
        }
        previous_key = e.key;
        result.entries.push_back(e);
    }
    result.blobs.assign(blob_region.begin(), blob_region.end());
    result.status = CacheStatus::kOk;
    return result;
}

std::vector<std::uint8_t> CanonicalPsoBytes(const PsoKeyInput& input) {
    std::vector<std::uint8_t> out;
    out.reserve(128);
    PutU64(out, input.vs);
    PutU64(out, input.ps);
    PutU64(out, input.gs);
    PutU64(out, input.hs);
    PutU64(out, input.ds);
    PutU64(out, input.input_layout);
    PutU8(out, input.rtv_count);
    for (std::uint32_t i = 0; i < 8; ++i) PutU32(out, input.rtv_formats[i]);
    PutU32(out, input.dsv_format);
    PutU32(out, input.sample_count);
    PutU32(out, input.blend);
    PutU32(out, input.depth_stencil);
    PutU32(out, input.rasterizer);
    PutU32(out, input.root_signature);
    PutU32(out, input.bindless);
    PutU32(out, input.feature_level);
    PutU32(out, input.pso_library);
    PutU32(out, input.topology);
    return out;
}

PsoKey MakePsoKey(const PsoKeyInput& input) {
    const std::vector<std::uint8_t> bytes = CanonicalPsoBytes(input);
    const std::span<const std::uint8_t> span(bytes.data(), bytes.size());
    PsoKey key;
    key.lo = Fnv1a64(span);
    key.hi = Mix64(key.lo ^ Fnv1a64(span, 0x84222325cbf29ce4ull));
    return key;
}

std::uint32_t Crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (const std::uint8_t b : bytes) {
        crc ^= b;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

}  // namespace bbx::d3d12
