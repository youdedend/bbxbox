// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/host/d3d12/bbx_shader_bake.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace bbx::d3d12 {

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
        std::snprintf(buf, sizeof(buf), "%s: %u / %u (%u%%)  OK %u  SKIP %u  [%us]",
                      BakePhaseName(snap.phase), snap.done, snap.total, pct,
                      snap.signed_count, snap.refused_count + snap.quarantined_count,
                      snap.elapsed_seconds);
    } else {
        std::snprintf(buf, sizeof(buf), "%s...  [%us]",
                      BakePhaseName(snap.phase), snap.elapsed_seconds);
    }
    return buf;
}

}  // namespace bbx::d3d12
