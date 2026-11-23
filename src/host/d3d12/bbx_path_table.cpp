// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/host/d3d12/bbx_path_table.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace bbx::d3d12 {
namespace {

constexpr char kHeaderPrefix[] = "# bbx_shaders.paths v1";
constexpr std::uint32_t kMaxBindingsPerProgram = 128;  // t0..t127 / u0..u127

std::string HexOf(const StampDigest& digest) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (std::uint8_t byte : digest) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0f]);
    }
    return out;
}

bool ParseHex64(const std::string& text, std::uint64_t* out) {
    if (text.empty() || text.size() > 16) return false;
    std::uint64_t value = 0;
    for (char c : text) {
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint32_t>(c - 'a' + 10);
        else return false;
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

bool ParseU32(const std::string& text, std::uint32_t* out) {
    if (text.empty()) return false;
    char* stop = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &stop, 10);
    if (stop == nullptr || *stop != '\0' || value > 0xfffffffful) return false;
    *out = static_cast<std::uint32_t>(value);
    return true;
}

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t at = 0;
    while (at <= line.size()) {
        const std::size_t end = line.find(' ', at);
        const std::string field = line.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!field.empty()) fields.push_back(field);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return fields;
}

}  // namespace

const char* PathTableStatusName(PathTableStatus status) {
    switch (status) {
        case PathTableStatus::kOk: return "ok";
        case PathTableStatus::kMissing: return "missing";
        case PathTableStatus::kNotATable: return "not a binding table";
        case PathTableStatus::kStampMismatch: return "stamp mismatch";
        case PathTableStatus::kCorrupt: return "corrupt";
    }
    return "?";
}

const char* PathTable::HeaderLine() { return kHeaderPrefix; }

std::string PathTable::FormatStamp(const StampDigest& digest) { return "stamp " + HexOf(digest); }

std::string PathTable::FormatBinding(std::uint64_t key, std::uint32_t stage, const BindingPlan& plan) {
    char head[96] = {};
    std::snprintf(head, sizeof(head), "bind %016llx %u %u %s ", static_cast<unsigned long long>(key), stage,
                  plan.binding, plan.kind == BindingKind::kUav ? "uav" : "srv");
    return std::string(head) + SerializePath(plan.path);
}

PathTableStatus PathTable::Load(std::string_view text, const StampDigest& expected) {
    plans_.clear();
    program_keys_.clear();
    programs_ = 0;
    detail_.clear();
    if (text.empty()) {
        status_ = PathTableStatus::kMissing;
        detail_ = "the bake wrote no binding table for this dump";
        return status_;
    }
    bool have_header = false;
    bool have_stamp = false;
    std::size_t at = 0;
    std::size_t line_number = 0;
    while (at <= text.size()) {
        const std::size_t end = text.find('\n', at);
        std::string line(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ++line_number;
        if (end == std::string::npos) at = text.size() + 1;
        else at = end + 1;

        if (line.empty()) continue;
        if (!have_header) {
            if (line != kHeaderPrefix) {
                status_ = PathTableStatus::kNotATable;
                char why[128] = {};
                std::snprintf(why, sizeof(why), "line %zu is not the header this reader writes", line_number);
                detail_ = why;
                return status_;
            }
            have_header = true;
            continue;
        }
        if (line[0] == '#') continue;  // a comment the bake added, not a record
        const std::vector<std::string> fields = Split(line);
        if (fields.empty()) continue;
        if (fields[0] == "stamp") {
            if (have_stamp || fields.size() != 2) {
                status_ = PathTableStatus::kCorrupt;
                detail_ = "the stamp line is missing, repeated, or carries more than one field";
                return status_;
            }
            if (fields[1] != HexOf(expected)) {
                status_ = PathTableStatus::kStampMismatch;
                detail_ = "the binding table's stamp is not the cache's: one of the two came from another bake";
                return status_;
            }
            have_stamp = true;
            continue;
        }
        if (fields[0] != "bind") {
            status_ = PathTableStatus::kCorrupt;
            detail_ = "line " + std::to_string(line_number) + ": '" + fields[0] + "' is not a record this reader writes";
            return status_;
        }
        if (fields.size() != 6) {
            status_ = PathTableStatus::kCorrupt;
            detail_ = "line " + std::to_string(line_number) + ": a binding line has six fields";
            return status_;
        }
        BindingPlan plan;
        std::uint64_t key = 0;
        std::uint32_t stage = 0;
        if (!ParseHex64(fields[1], &key)) {
            status_ = PathTableStatus::kCorrupt;
            detail_ = "line " + std::to_string(line_number) + ": the key is not hexadecimal";
            return status_;
        }
        if (!ParseU32(fields[2], &stage) || stage > 3) {
            status_ = PathTableStatus::kCorrupt;
            detail_ = "line " + std::to_string(line_number) + ": the stage is not one this port bakes";
            return status_;
        }
        if (!ParseU32(fields[3], &plan.binding) || plan.binding >= kMaxBindingsPerProgram) {
            status_ = PathTableStatus::kCorrupt;
            detail_ = "line " + std::to_string(line_number) + ": the binding is not a t#/u# this layout serves";
            return status_;
        }
        if (fields[4] == "uav") plan.kind = BindingKind::kUav;
        else if (fields[4] == "srv") plan.kind = BindingKind::kSrv;
        else {
            status_ = PathTableStatus::kCorrupt;
            detail_ = "line " + std::to_string(line_number) + ": '" + fields[4] + "' is not srv or uav";
            return status_;
        }
        if (!ParsePath(fields[5], &plan.path)) {
            status_ = PathTableStatus::kCorrupt;
            detail_ = "line " + std::to_string(line_number) + ": the path is not one this reader writes";
            return status_;
        }
        const PlanSlotKey slot{key, plan.binding};
        if (plans_.find(slot) != plans_.end()) {
            status_ = PathTableStatus::kCorrupt;
            char why[128] = {};
            std::snprintf(why, sizeof(why), "binding %u of key %016llx is written twice", plan.binding,
                          static_cast<unsigned long long>(key));
            detail_ = why;
            return status_;
        }
        plans_.emplace(slot, plan);
        program_keys_.insert(key);
    }
    if (!have_header) {
        status_ = PathTableStatus::kNotATable;
        detail_ = "the file has no header line";
        return status_;
    }
    if (!have_stamp) {
        status_ = PathTableStatus::kCorrupt;
        detail_ = "the file has no stamp line, so it cannot be paired with a cache";
        return status_;
    }
    programs_ = program_keys_.size();
    status_ = PathTableStatus::kOk;
    return status_;
}

const BindingPlan* PathTable::Find(std::uint64_t key, std::uint32_t binding) const {
    if (binding >= kMaxBindingsPerProgram) return nullptr;
    const auto it = plans_.find(PlanSlotKey{key, binding});
    if (it == plans_.end()) return nullptr;
    return &it->second;
}

std::string PathTable::Report() const {
    char line[192] = {};
    if (status_ == PathTableStatus::kOk) {
        std::snprintf(line, sizeof(line), "ok: %zu bindings on %zu programs", plans_.size(), programs_);
        return line;
    }
    std::string out = PathTableStatusName(status_);
    if (!detail_.empty()) out += ": " + detail_;
    return out;
}

}  // namespace bbx::d3d12
