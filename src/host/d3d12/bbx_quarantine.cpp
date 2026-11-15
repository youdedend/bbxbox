// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_quarantine.h"

#include "host/d3d12/bbx_common.h"

#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

namespace bbx::d3d12 {
namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string Hex(std::uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return buf;
}

bool ParseHex(std::string_view text, std::uint64_t& out) {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
    if (text.size() != 16) return false;
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    out = value;
    return true;
}

std::vector<std::string_view> SplitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        if (nl == std::string_view::npos) {
            lines.push_back(text);
            break;
        }
        lines.push_back(text.substr(0, nl));
        text.remove_prefix(nl + 1);
    }
    return lines;
}

std::map<std::string, std::string, std::less<>> ParseKeyValues(std::string_view line) {
    std::map<std::string, std::string, std::less<>> out;
    std::size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && line[pos] == ' ') ++pos;
        const std::size_t eq = line.find('=', pos);
        if (eq == std::string_view::npos) break;
        const std::size_t end = line.find(' ', eq + 1);
        const std::string_view key = line.substr(pos, eq - pos);
        const std::string_view value =
            line.substr(eq + 1, end == std::string_view::npos ? std::string_view::npos : end - eq - 1);
        out.emplace(std::string(key), std::string(value));
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    return out;
}

std::uint64_t U64OrDefault(const std::map<std::string, std::string, std::less<>>& kv,
                           std::string_view key, std::uint64_t fallback = 0) {
    const auto it = kv.find(key);
    if (it == kv.end()) return fallback;
    std::uint64_t value = fallback;
    const auto result = std::from_chars(it->second.data(), it->second.data() + it->second.size(),
                                        value, 10);
    return result.ec == std::errc{} ? value : fallback;
}

ShaderKind KindFromName(std::string_view name) {
    if (name == "ps" || name == "pixel") return ShaderKind::kPixel;
    if (name == "cs" || name == "compute") return ShaderKind::kCompute;
    if (name == "gs" || name == "geometry") return ShaderKind::kGeometry;
    if (name == "hs" || name == "hull") return ShaderKind::kHull;
    if (name == "ds" || name == "domain") return ShaderKind::kDomain;
    return ShaderKind::kVertex;
}

const char* ShortKind(ShaderKind kind) {
    switch (kind) {
    case ShaderKind::kVertex: return "vs";
    case ShaderKind::kPixel: return "ps";
    case ShaderKind::kCompute: return "cs";
    case ShaderKind::kGeometry: return "gs";
    case ShaderKind::kHull: return "hs";
    case ShaderKind::kDomain: return "ds";
    }
    return "vs";
}

}  // namespace

const char* ShaderKindName(ShaderKind kind) {
    switch (kind) {
    case ShaderKind::kVertex: return "vertex";
    case ShaderKind::kPixel: return "pixel";
    case ShaderKind::kCompute: return "compute";
    case ShaderKind::kGeometry: return "geometry";
    case ShaderKind::kHull: return "hull";
    case ShaderKind::kDomain: return "domain";
    }
    return "vertex";
}

std::string SerializeInflight(const Unit& unit, std::uint32_t index, std::uint32_t total) {
    std::string out = "1\n";
    out += "kind=" + std::string(ShortKind(unit.kind));
    out += " hash=" + Hex(unit.hash);
    out += " partner=" + Hex(unit.partner);
    out += " index=" + std::to_string(index);
    out += " total=" + std::to_string(total);
    out += " bundle=" + unit.bundle;
    out += " entry=" + unit.entry;
    if (!unit.partner_entry.empty()) out += " partner_entry=" + unit.partner_entry;
    out += "\n";
    return out;
}

Inflight ParseInflight(std::string_view text) {
    Inflight inflight;
    const std::vector<std::string_view> lines = SplitLines(text);
    if (lines.empty()) return inflight;
    const std::string_view version = Trim(lines[0]);
    if (version != "1") return inflight;  // unknown format: ignore, do not quarantine
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string_view line = Trim(lines[i]);
        if (line.empty()) continue;
        const auto kv = ParseKeyValues(line);
        if (kv.empty()) continue;
        std::uint64_t hash = 0;
        const auto hash_it = kv.find("hash");
        if (hash_it == kv.end() || !ParseHex(hash_it->second, hash) || hash == 0) {
            return inflight;  // a record without a hash cannot quarantine anything
        }
        Unit unit;
        unit.hash = hash;
        const auto partner_it = kv.find("partner");
        if (partner_it != kv.end()) ParseHex(partner_it->second, unit.partner);
        const auto kind_it = kv.find("kind");
        if (kind_it != kv.end()) unit.kind = KindFromName(kind_it->second);
        const auto bundle_it = kv.find("bundle");
        if (bundle_it != kv.end()) unit.bundle = bundle_it->second;
        const auto entry_it = kv.find("entry");
        if (entry_it != kv.end()) unit.entry = entry_it->second;
        const auto partner_entry_it = kv.find("partner_entry");
        if (partner_entry_it != kv.end()) unit.partner_entry = partner_entry_it->second;
        inflight.unit = std::move(unit);
        inflight.index = static_cast<std::uint32_t>(U64OrDefault(kv, "index"));
        inflight.total = static_cast<std::uint32_t>(U64OrDefault(kv, "total"));
        inflight.present = true;
        return inflight;  // one record is all the protocol writes
    }
    return inflight;
}

Quarantine Quarantine::Parse(std::string_view text) {
    Quarantine q;
    for (const std::string_view raw : SplitLines(text)) {
        const std::string_view line = Trim(raw);
        if (line.empty()) continue;
        // The first token is the hash; anything after it is a human note that
        // some earlier version wrote, and is ignored rather than fatal.
        std::size_t end = 0;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
        std::uint64_t hash = 0;
        if (ParseHex(line.substr(0, end), hash) && hash != 0) {
            q.hashes_.insert(hash);
        }
    }
    return q;
}

bool Quarantine::Contains(std::uint64_t hash) const {
    return hash != 0 && hashes_.count(hash) != 0;
}

bool Quarantine::Contains(const Unit& unit) const {
    return Contains(unit.hash) || (unit.is_pair() && Contains(unit.partner));
}

void Quarantine::Add(std::uint64_t hash) {
    if (hash != 0) hashes_.insert(hash);
}

void Quarantine::Add(const Unit& unit) {
    Add(unit.hash);
    Add(unit.partner);
}

std::string Quarantine::Serialize() const {
    std::string out;
    for (const std::uint64_t hash : hashes_) {
        out += Hex(hash);
        out += '\n';
    }
    return out;
}

std::string Quarantine::Summary(std::size_t max_entries) const {
    if (hashes_.empty()) return "none";
    std::string out = std::to_string(hashes_.size()) + " entr" + (hashes_.size() == 1 ? "y" : "ies");
    out += " (";
    std::size_t written = 0;
    for (const std::uint64_t hash : hashes_) {
        if (written == max_entries) {
            out += ", …";
            break;
        }
        if (written) out += ", ";
        out += Hex(hash);
        ++written;
    }
    out += ")";
    return out;
}

CrashRecovery RecoverFromInflight(std::string_view inflight_text, Quarantine& quarantine) {
    CrashRecovery recovery;
    const Inflight inflight = ParseInflight(inflight_text);
    if (!inflight.present) return recovery;
    recovery.found = true;
    recovery.unit = inflight.unit;
    quarantine.Add(inflight.unit);
    recovery.promoted = true;

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "Precompile: quarantined %016llx%s (%s/%s) after a crash at item %u/%u",
                  static_cast<unsigned long long>(inflight.unit.hash),
                  inflight.unit.is_pair()
                      ? (" and " + Hex(inflight.unit.partner)).c_str()
                      : "",
                  inflight.unit.bundle.empty() ? "?" : inflight.unit.bundle.c_str(),
                  inflight.unit.entry.empty() ? "?" : inflight.unit.entry.c_str(),
                  inflight.index, inflight.total);
    recovery.message = buf;
    return recovery;
}

std::uint64_t StageHash(std::uint64_t program_hash, ShaderKind kind,
                        std::string_view translator_stamp) {
    std::uint64_t h = Fnv1a64(program_hash);
    const std::array<std::uint8_t, 1> stage{static_cast<std::uint8_t>(kind)};
    h = Fnv1a64(std::span<const std::uint8_t>(stage.data(), stage.size()), h);
    h = Fnv1a64(translator_stamp, h);
    return h;
}

}  // namespace bbx::d3d12
