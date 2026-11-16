// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/host/d3d12/bbx_guest_path.h"

#include <cstdio>
#include <cstring>

namespace bbx::d3d12 {
namespace {

// gcn/decode.cpp's SMRD fields, and its identification of the encoding.
constexpr std::uint32_t kSmrdPattern = 0b11000u;  // bits 31:27
constexpr std::uint32_t kEndpgm = 0xBF810000u;    // SOPP s_endpgm, imm16 = 0

bool IsSmrd(std::uint32_t w) { return (w >> 27) == kSmrdPattern; }

// s_load_dword = 0, x2 = 1, x4 = 2, x8 = 3, x16 = 4;
// s_buffer_load_dword = 8, x2 = 9, x4 = 10, x8 = 11, x16 = 12.
std::uint32_t WidthOf(std::uint32_t op) {
    if (op >= 8) op -= 8;
    switch (op) {
        case 0: return 1;
        case 1: return 2;
        case 2: return 4;
        case 3: return 8;
        case 4: return 16;
        default: return 0;
    }
}

}  // namespace

std::string PathPlan::str() const {
    char head[64] = {};
    if (immediate) {
        std::snprintf(head, sizeof(head), "immediate s%u", static_cast<unsigned>(user_sgpr));
    } else {
        std::snprintf(head, sizeof(head), "s%u", static_cast<unsigned>(user_sgpr));
    }
    std::string out = head;
    for (const PathStep& step : loads) {
        char one[48] = {};
        std::snprintf(one, sizeof(one), " -> [%s+0x%x]", step.vsharp ? "V#" : "ptr",
                      static_cast<unsigned>(step.offset_dw) * 4u);
        out += one;
    }
    if (!immediate) {
        char tail[48] = {};
        std::snprintf(tail, sizeof(tail), " -> [%s+0x%x]", final_vsharp ? "V#" : "ptr",
                      static_cast<unsigned>(final_offset_dw) * 4u);
        out += tail;
    }
    return out;
}

std::uint64_t InterpBase(const std::uint32_t* words, bool vsharp) {
    // Eight bits of word 1 for a V# base (40-bit guest addresses), all 32 for a
    // pointer. bbhost's render.cpp and gpu.cpp read both the same way.
    return vsharp ? (static_cast<std::uint64_t>(words[0]) | (static_cast<std::uint64_t>(words[1] & 0xffu) << 32))
                  : (static_cast<std::uint64_t>(words[0]) | (static_cast<std::uint64_t>(words[1]) << 32));
}

const char* PathVerdictName(PathVerdict verdict) {
    switch (verdict) {
        case PathVerdict::kResolved: return "resolved";
        case PathVerdict::kBadSlot: return "user SGPR out of range";
        case PathVerdict::kShortImmediate: return "immediate words past user SGPR 15";
        case PathVerdict::kUnmapped: return "guest read not supplied";
    }
    return "?";
}

PathResult ResolvePath(const PathPlan& plan, const std::uint32_t* user, int ndw, ReadGuest read, void* context,
                       std::uint32_t* out) {
    PathResult result;
    // The hardware gives a stage sixteen user SGPRs; a pair that runs past them
    // is a translator bug or a corrupt path, and reading past `user` is not a
    // thing this port does.
    if (plan.user_sgpr < 0 || plan.user_sgpr + 1 >= 16 || user == nullptr || out == nullptr || ndw <= 0) {
        result.verdict = PathVerdict::kBadSlot;
        return result;
    }
    if (plan.immediate) {
        if (plan.user_sgpr + ndw > 16) {
            result.verdict = PathVerdict::kShortImmediate;
            return result;
        }
        std::memcpy(out, user + plan.user_sgpr, static_cast<std::size_t>(ndw) * sizeof(std::uint32_t));
        return result;
    }
    // The pointer's four dwords: the pair always, words 2 and 3 when the pair is
    // not at the very top of the file (resolve_resource_impl's rule).
    std::uint32_t cur[4] = {user[plan.user_sgpr], user[plan.user_sgpr + 1], 0, 0};
    if (plan.user_sgpr + 3 < 16) {
        cur[2] = user[plan.user_sgpr + 2];
        cur[3] = user[plan.user_sgpr + 3];
    }
    for (const PathStep& step : plan.loads) {
        const std::uint64_t addr = InterpBase(cur, step.vsharp) + static_cast<std::uint64_t>(step.offset_dw) * 4;
        if (read == nullptr || !read(context, addr, cur, sizeof(cur))) {
            result.verdict = PathVerdict::kUnmapped;
            return result;
        }
    }
    const std::uint64_t addr = InterpBase(cur, plan.final_vsharp) + static_cast<std::uint64_t>(plan.final_offset_dw) * 4;
    if (read == nullptr || !read(context, addr, out, static_cast<std::size_t>(ndw) * sizeof(std::uint32_t))) {
        result.verdict = PathVerdict::kUnmapped;
        return result;
    }
    result.at = addr;
    return result;
}

std::vector<UserDataLoad> DecodeUserDataLoads(const std::uint32_t* words, std::size_t count) {
    std::vector<UserDataLoad> loads;
    if (words == nullptr) return loads;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t w = words[i];
        if ((w & 0xffff0000u) == kEndpgm) break;
        if (!IsSmrd(w)) continue;
        UserDataLoad load;
        load.at_dword = static_cast<std::uint32_t>(i);
        load.op = (w >> 22) & 0x1fu;
        load.sdst = (w >> 15) & 0x7fu;
        load.sbase = ((w >> 9) & 0x3fu) * 2;  // the field is an SGPR pair index
        load.offset_dw = w & 0xffu;
        load.dwords = WidthOf(load.op);
        load.buffer = load.op >= 8;
        if (load.dwords == 0) continue;
        load.from_user_data = load.sbase + load.dwords <= 16;
        loads.push_back(load);
    }
    return loads;
}

int DecodeTableSlot(const std::uint32_t* words, std::size_t count, std::string* evidence) {
    const std::vector<UserDataLoad> loads = DecodeUserDataLoads(words, count);
    for (std::size_t i = 0; i < loads.size(); ++i) {
        const UserDataLoad& first = loads[i];
        // The table's address is loaded as a 64-bit value (x2 or x4) out of a
        // user SGPR pair, from a raw pointer rather than through a V#.
        if (first.buffer || first.op < 1 || first.op > 2 || !first.from_user_data) continue;
        // ... and then dereferenced: a later load reads through the SGPRs it
        // wrote, whose pair starts at first.sdst.
        for (std::size_t j = i + 1; j < loads.size(); ++j) {
            if (loads[j].sbase != first.sdst) continue;
            if (evidence != nullptr) {
                char text[192] = {};
                std::snprintf(text, sizeof(text),
                              "s_load_dwordx%u from s[%u:%u]+0x%x at dword %u, dereferenced by s_%sload_dwordx%u "
                              "from s[%u:%u]+0x%x at dword %u",
                              first.op == 1 ? 2u : 4u, first.sbase, first.sbase + 1,
                              first.offset_dw, first.at_dword, loads[j].buffer ? "buffer_" : "",
                              loads[j].dwords, loads[j].sbase, loads[j].sbase + loads[j].dwords - 1,
                              loads[j].offset_dw, loads[j].at_dword);
                *evidence = text;
            }
            return static_cast<int>(first.sbase);
        }
    }
    return -1;
}

}  // namespace bbx::d3d12
