// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/host/d3d12/bbx_shader_bake.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "host/d3d12/bbx_common.h"

namespace bbx::d3d12 {

namespace {

constexpr char kCheckpointMagic[4] = {'B', 'B', 'X', 'C'};
constexpr std::uint32_t kCheckpointVersion = 1u;
constexpr std::size_t kCheckpointHeaderBytes = 24u;  // magic(4) + ver(4) + cache_len(4) + paths_len(4) + hash(8)

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
}

void AppendU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    AppendU32(out, static_cast<std::uint32_t>(v & 0xffffffffu));
    AppendU32(out, static_cast<std::uint32_t>(v >> 32));
}

std::uint32_t ReadU32(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

std::uint64_t ReadU64(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint64_t>(ReadU32(data, offset)) |
           (static_cast<std::uint64_t>(ReadU32(data, offset + 4)) << 32);
}

}  // namespace

bool IsSignedDxbc(std::span<const std::uint8_t> object, std::string& why) {
    if (object.size() < 20) {
        why = "shorter than a DXBC header";
        return false;
    }
    if (std::memcmp(object.data(), "DXBC", 4) != 0) {
        why = "not a DXBC container";
        return false;
    }
    std::uint32_t digest = 0;
    for (int i = 0; i < 16; ++i) digest |= object[4 + static_cast<std::size_t>(i)];
    if (digest == 0) {
        why = "unsigned (the 16 bytes at 4..20 are zero: no dxil signer)";
        return false;
    }
    why.clear();
    return true;
}

bool Has64BitInteger(std::string_view hlsl, std::string& what) {
    for (const char* name : {"uint64_t", "int64_t", "uint64_t(", "double"}) {
        const std::size_t at = hlsl.find(name);
        if (at != std::string_view::npos) {
            const std::size_t from = at > 60 ? at - 60 : 0;
            const std::string_view snippet = hlsl.substr(from, 100);
            what = std::string(name) + " in \"" + std::string(snippet) + "\"";
            return true;
        }
    }
    what.clear();
    return false;
}

DxilFeatures DxilFeatureFlags(std::span<const std::uint8_t> object) {
    DxilFeatures out;
    if (object.size() < 32 || std::memcmp(object.data(), "DXBC", 4) != 0) return out;
    const auto u32 = [&object](std::size_t at) {
        std::uint32_t v = 0;
        if (at + 4 <= object.size()) std::memcpy(&v, object.data() + at, 4);
        return v;
    };
    const std::uint32_t parts = u32(28);
    for (std::uint32_t k = 0; k < parts && 32 + 4 * static_cast<std::size_t>(k) + 4 <= object.size() && k < 64; ++k) {
        const std::uint32_t off = u32(32 + 4 * static_cast<std::size_t>(k));
        if (static_cast<std::size_t>(off) + 12 > object.size()) continue;
        if (std::memcmp(object.data() + off, "SFI0", 4) != 0) continue;
        out.present = true;
        out.flags = u32(static_cast<std::size_t>(off) + 8);
        return out;
    }
    return out;
}

std::string ContainerName(std::span<const std::uint8_t> container) {
    const std::size_t size = container.size();
    if (size < 20) return {};
    for (std::size_t i = size - 20;; --i) {
        if (i + 20 <= size && std::memcmp(container.data() + i, "OrbShdr", 7) == 0) {
            std::uint32_t h0 = 0;
            std::memcpy(&h0, container.data() + i + 16, 4);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%08x", h0);
            return buf;
        }
        if (i == 0) break;
    }
    return {};
}

int ContainerType(std::span<const std::uint8_t> container) {
    const std::size_t size = container.size();
    for (std::size_t i = 0; i + 16 <= size && i < 0x100; ++i) {
        if (std::memcmp(container.data() + i, "Shdr", 4) == 0) {
            return static_cast<int>(container[i + 8] & 0xfu);
        }
    }
    return -1;
}

bool IsShaderBundleFilename(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (const char c : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    const auto ends_with = [&lower](std::string_view suffix) {
        return lower.size() >= suffix.size() &&
               lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return ends_with(".shaderbnd.dcx") || ends_with(".shaderbnddebug.dcx");
}

std::vector<EmbeddedContainerSpan> ScanEbootForContainers(std::span<const std::uint8_t> elf_bytes) {
    std::vector<EmbeddedContainerSpan> out;
    const std::size_t size = elf_bytes.size();
    if (size < 0x60) return out;
    const std::uint8_t* const data = elf_bytes.data();
    for (std::size_t at = 0; at + 0x60 <= size; ++at) {
        const auto* p = static_cast<const std::uint8_t*>(std::memchr(data + at, 'S', size - at));
        if (!p) break;
        at = static_cast<std::size_t>(p - data);
        if (at + 0x60 > size || std::memcmp(p, "Shdr", 4) != 0 ||
            (at >= 3 && std::memcmp(p - 3, "Orb", 3) == 0)) {
            continue;
        }
        const int type = static_cast<int>(p[8] & 0xfu);
        if (type != 1 && type != 2 && type != 4) continue;
        const std::size_t limit = std::min(size, at + (1u << 20));
        std::size_t foot = std::string::npos;
        for (std::size_t k = at + 16; k + 28 <= limit; ++k) {
            if (data[k] == 'O' && std::memcmp(data + k, "OrbShdr", 7) == 0) {
                foot = k;
                break;
            }
        }
        if (foot == std::string::npos) continue;
        const std::size_t span_len = (foot + 28) - at;
        const std::string name = ContainerName(elf_bytes.subspan(at, span_len));
        if (name.empty()) continue;
        EmbeddedContainerSpan entry;
        entry.offset = at;
        entry.size = span_len;
        entry.type = static_cast<std::uint8_t>(type);
        entry.name = name;
        out.push_back(std::move(entry));
    }
    return out;
}

std::uint32_t ChooseBakeWorkerCount(std::uint32_t hardware_threads,
                                    std::uint32_t requested_max,
                                    bool force_single_thread) noexcept {
    if (force_single_thread) return 1u;
    std::uint32_t cap = requested_max > 0 ? std::min<std::uint32_t>(requested_max, 4u) : 4u;
    if (hardware_threads <= 1u) return 1u;
    // Use half the logical cores (clamped to [2, cap]) so Xbox Series X|S (8
    // threads) runs 4 DXC workers while keeping the UI thread and OS smooth.
    std::uint32_t chosen = hardware_threads / 2u;
    if (chosen < 2u) chosen = 2u;
    if (chosen > cap) chosen = cap;
    return chosen;
}

std::uint32_t ChooseDxcOptimizationLevel(std::string_view hlsl) noexcept {
    const std::size_t bytes = hlsl.size();
    if (bytes >= 49152u) return 0u;  // >= 48 KiB HLSL: -O0 avoids quadratic DXC SSA/GVN passes

    std::size_t lines = 0;
    std::size_t loops = 0;
    std::size_t branches = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        const char c = hlsl[i];
        if (c == '\n') {
            ++lines;
        } else if (c == 'f' && i + 5 < bytes && std::memcmp(hlsl.data() + i, "for (", 5) == 0) {
            ++loops;
        } else if (c == 'i' && i + 4 < bytes && std::memcmp(hlsl.data() + i, "if (", 4) == 0) {
            ++branches;
        }
    }
    if (lines >= 1200u || loops >= 12u || branches >= 160u) return 0u;
    if (bytes >= 24576u || lines >= 600u || loops >= 6u || branches >= 80u) return 1u;
    return 3u;
}

namespace {

const char* StageToken(Stage s) noexcept {
    switch (s) {
        case Stage::kVertex: return "vs";
        case Stage::kPixel: return "ps";
        case Stage::kCompute: return "cs";
        default: return "ps";
    }
}

Stage StageFromToken(std::string_view tok) noexcept {
    if (tok == "vs") return Stage::kVertex;
    if (tok == "cs") return Stage::kCompute;
    return Stage::kPixel;
}

bool ParseHex64(std::string_view s, std::uint64_t* out) noexcept {
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.remove_prefix(2);
    if (s.empty() || s.size() > 16) return false;
    std::uint64_t v = 0;
    for (const char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint64_t>(c - 'A' + 10);
        else return false;
    }
    *out = v;
    return v != 0;
}

std::uint32_t ParseDec32(std::string_view s) noexcept {
    std::uint32_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') break;
        v = v * 10u + static_cast<std::uint32_t>(c - '0');
    }
    return v;
}

}  // namespace

std::string SerializeMultiWorkerInflight(std::span<const WorkerInflightSlot> slots) {
    std::string out = "1\nmode=multithread\n";
    for (const WorkerInflightSlot& s : slots) {
        if (!s.active || s.hash == 0) continue;
        char line[384] = {};
        std::snprintf(
            line, sizeof(line),
            "worker=%u kind=%s hash=%016llx index=%u total=%u name=%s bundle=%s entry=%s\n",
            s.worker_id, StageToken(s.stage), static_cast<unsigned long long>(s.hash),
            s.index, s.total,
            s.name.empty() ? "?" : s.name.c_str(),
            s.bundle.empty() ? "?" : s.bundle.c_str(),
            s.entry.empty() ? "?" : s.entry.c_str());
        out += line;
    }
    return out;
}

std::vector<WorkerInflightSlot> ParseMultiWorkerInflight(std::string_view text) {
    std::vector<WorkerInflightSlot> out;
    std::size_t pos = 0;
    bool version_ok = false;
    std::size_t line_idx = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos) ? text.substr(pos)
                                                               : text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() : (nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        if (line.empty()) continue;
        if (line_idx == 0) {
            ++line_idx;
            if (line != "1") return out;
            version_ok = true;
            continue;
        }
        ++line_idx;
        if (!version_ok || line == "mode=multithread") continue;

        WorkerInflightSlot slot;
        std::size_t p = 0;
        while (p < line.size()) {
            while (p < line.size() && line[p] == ' ') ++p;
            const std::size_t eq = line.find('=', p);
            if (eq == std::string_view::npos) break;
            const std::size_t sp = line.find(' ', eq + 1);
            const std::string_view k = line.substr(p, eq - p);
            const std::string_view v = (sp == std::string_view::npos)
                                           ? line.substr(eq + 1)
                                           : line.substr(eq + 1, sp - eq - 1);
            if (k == "worker") slot.worker_id = ParseDec32(v);
            else if (k == "kind") slot.stage = StageFromToken(v);
            else if (k == "hash") (void)ParseHex64(v, &slot.hash);
            else if (k == "index") slot.index = ParseDec32(v);
            else if (k == "total") slot.total = ParseDec32(v);
            else if (k == "name") slot.name = std::string(v);
            else if (k == "bundle") slot.bundle = std::string(v);
            else if (k == "entry") slot.entry = std::string(v);
            if (sp == std::string_view::npos) break;
            p = sp + 1;
        }
        if (slot.hash != 0) {
            slot.active = true;
            out.push_back(std::move(slot));
        }
    }
    return out;
}

std::vector<std::uint8_t> SerializeBakeCheckpoint(
    const CacheStamp& stamp,
    const std::vector<CacheEntry>& entries,
    const std::vector<std::uint8_t>& blobs,
    std::string_view bindings_text) {
    // Sort a copy of entries by key so ParseCache's sorted-index invariant holds.
    std::vector<CacheEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const CacheEntry& a, const CacheEntry& b) { return a.key < b.key; });
    const std::vector<std::uint8_t> cache_bytes = SerializeCache(stamp, sorted, blobs);

    std::vector<std::uint8_t> payload(cache_bytes.size() + bindings_text.size());
    if (!cache_bytes.empty()) {
        std::memcpy(payload.data(), cache_bytes.data(), cache_bytes.size());
    }
    if (!bindings_text.empty()) {
        std::memcpy(payload.data() + cache_bytes.size(), bindings_text.data(), bindings_text.size());
    }
    const std::uint64_t hash = bbx::Fnv1a64(std::span<const std::uint8_t>(payload));

    std::vector<std::uint8_t> out;
    out.reserve(kCheckpointHeaderBytes + payload.size());
    for (char c : kCheckpointMagic) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
    AppendU32(out, kCheckpointVersion);
    AppendU32(out, static_cast<std::uint32_t>(cache_bytes.size()));
    AppendU32(out, static_cast<std::uint32_t>(bindings_text.size()));
    AppendU64(out, hash);
    out.resize(kCheckpointHeaderBytes + payload.size());
    if (!payload.empty()) {
        std::memcpy(out.data() + kCheckpointHeaderBytes, payload.data(), payload.size());
    }
    return out;
}

bool ParseBakeCheckpoint(std::span<const std::uint8_t> bytes,
                         const CacheStamp& expected_stamp,
                         BakeCheckpoint* out) {
    if (out == nullptr || bytes.size() < kCheckpointHeaderBytes) return false;
    if (std::memcmp(bytes.data(), kCheckpointMagic, sizeof(kCheckpointMagic)) != 0) return false;
    if (ReadU32(bytes, 4u) != kCheckpointVersion) return false;
    const std::size_t cache_len = ReadU32(bytes, 8u);
    const std::size_t paths_len = ReadU32(bytes, 12u);
    const std::uint64_t expected_hash = ReadU64(bytes, 16u);
    if (cache_len == 0 || kCheckpointHeaderBytes + cache_len + paths_len != bytes.size()) {
        return false;
    }
    const std::span<const std::uint8_t> payload = bytes.subspan(kCheckpointHeaderBytes);
    if (bbx::Fnv1a64(payload) != expected_hash) return false;

    const std::span<const std::uint8_t> cache_span = payload.subspan(0, cache_len);
    ParseResult parsed = ParseCache(cache_span, expected_stamp);
    if (parsed.status != CacheStatus::kOk) return false;

    BakeCheckpoint cp;
    cp.entries = std::move(parsed.entries);
    cp.blobs = std::move(parsed.blobs);
    const std::span<const std::uint8_t> paths_span = payload.subspan(cache_len, paths_len);
    cp.bindings_text.assign(reinterpret_cast<const char*>(paths_span.data()), paths_span.size());
    *out = std::move(cp);
    return true;
}

const char* BakePhaseName(BakePhase phase) noexcept {
    switch (phase) {
        case BakePhase::kIdle: return "IDLE";
        case BakePhase::kScanning: return "SCANNING SHADERS";
        case BakePhase::kAnalysing: return "ANALYSING PROGRAMS";
        case BakePhase::kCompiling: return "COMPILING DXIL 6.4";
        case BakePhase::kWriting: return "WRITING CACHE";
        case BakePhase::kDone: return "COMPLETE";
        case BakePhase::kFailed: return "FAILED";
    }
    return "UNKNOWN";
}

std::string FormatBakeHudLine(const BakeProgressSnapshot& snap) {
    char buf[256] = {};
    if (snap.total > 0) {
        const std::uint32_t pct =
            snap.done >= snap.total ? 100u : static_cast<std::uint32_t>((static_cast<std::uint64_t>(snap.done) * 100ull) / snap.total);
        if (snap.workers > 1u && snap.phase == BakePhase::kCompiling) {
            std::snprintf(buf, sizeof(buf), "%s (%uT): %u / %u (%u%%)  OK %u  SKIP %u  [%us]",
                          BakePhaseName(snap.phase), snap.workers, snap.done, snap.total, pct,
                          snap.signed_count, snap.refused_count + snap.quarantined_count,
                          snap.elapsed_seconds);
        } else {
            std::snprintf(buf, sizeof(buf), "%s: %u / %u (%u%%)  OK %u  SKIP %u  [%us]",
                          BakePhaseName(snap.phase), snap.done, snap.total, pct,
                          snap.signed_count, snap.refused_count + snap.quarantined_count,
                          snap.elapsed_seconds);
        }
    } else {
        std::snprintf(buf, sizeof(buf), "%s...  [%us]",
                      BakePhaseName(snap.phase), snap.elapsed_seconds);
    }
    return buf;
}

}  // namespace bbx::d3d12
