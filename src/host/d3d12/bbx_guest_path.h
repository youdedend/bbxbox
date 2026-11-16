// SPDX-License-Identifier: GPL-3.0-or-later
// How a binding finds its bytes: the guest's own instructions, decoded.
//
// Upstream resolves a binding by walking what the translator called a
// `ResourcePath` (gcn/translate.h): a user-data slot, a chain of dereferences,
// and an offset in the last block. `resolve_resource_impl` in bbhost's
// render.cpp is that walk, and it is the authority this file follows - the port
// cannot call it (it lives in the Vulkan host, which does not build for the
// console), so the walk is restated here, in portable code, with the same two
// refusals:
//
//   * a slot whose pair would run past the sixteen the hardware gives a stage;
//   * a read of guest memory the caller cannot supply.
//
// The second half of the file is the part that makes the *slot* a fact rather
// than a convention. A compute dispatch's user SGPRs arrive from the guest's
// command stream (COMPUTE_USER_DATA_0..15 are registers 0x240..0x24F, which is
// where hle/gnm_exec.cpp's run_dispatch copies them from), and which of them
// holds the resource table's pointer is decided by the *program*: it loads the
// pointer with an `s_load_dwordx2` from an SGPR pair and then dereferences it.
// `DecodeTableSlot` reads that instruction out of the program's own dwords, so
// the answer is the program's, not this port's guess.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "src/host/d3d12/bbx_guest_vsharp.h"

namespace bbx::d3d12 {

// gcn/translate.h's ResourceStep: one dereference on the way to the resource.
struct PathStep {
    std::uint32_t offset_dw = 0;  // dword offset added to the pointer
    bool vsharp = false;          // the pointer is a V#'s base, not a raw address
    [[nodiscard]] bool operator==(const PathStep& other) const {
        return offset_dw == other.offset_dw && vsharp == other.vsharp;
    }
};

// gcn/translate.h's ResourcePath, restated without the translator's includes.
struct PathPlan {
    int user_sgpr = -1;              // first user SGPR of the pointer
    std::vector<PathStep> loads;     // successive dereferences
    int final_offset_dw = 0;         // dword offset of the resource words
    bool final_vsharp = false;       // the last dereference goes through a V# base
    bool immediate = false;          // the words live in the user SGPRs themselves
    [[nodiscard]] std::string str() const;
};

// render.cpp's interp_base: a pointer, or a V#'s base address out of the same
// two dwords. The V# form takes eight bits of word 1 - the guest's addresses are
// 40-bit - which is the same rule DecodeVSharp follows.
[[nodiscard]] std::uint64_t InterpBase(const std::uint32_t* words, bool vsharp);

// Reads `bytes` of guest memory at `va` into `out`. The console passes its
// pager/mapping seam; the tests pass an array. False when the range is not
// mapped, which is a refusal rather than a zero fill.
using ReadGuest = bool (*)(void* context, std::uint64_t va, void* out, std::size_t bytes);

enum class PathVerdict : std::uint8_t {
    kResolved = 0,
    kBadSlot,        // user_sgpr < 0 or its pair runs past user SGPR 15
    kShortImmediate, // the words wanted are not all in the sixteen user SGPRs
    kUnmapped,       // a dereference read a range the caller could not supply
};

[[nodiscard]] const char* PathVerdictName(PathVerdict verdict);

struct PathResult {
    PathVerdict verdict = PathVerdict::kResolved;
    std::uint64_t at = 0;  // the address the words were read from (0 when immediate)
};

// Fills `out` with the resource's `ndw` dwords, exactly as resolve_resource_impl
// does: the first dereference's 16 bytes come from the four user SGPRs starting
// at `user_sgpr` (words 2 and 3 only when they exist), every later one from guest
// memory.
[[nodiscard]] PathResult ResolvePath(const PathPlan& plan, const std::uint32_t* user, int ndw,
                                     ReadGuest read, void* context, std::uint32_t* out);

// A plan as one line of text, and back. The bake has the translator's assignment
// (`gcn::TranslateResult`'s buffers, each with a `ResourcePath`) and the console
// does not - so the plan has to be written beside the cache and read back there,
// and a format both sides agree on is the whole contract between them. Strict on
// the way in: a line with a token this function did not write is refused, because
// a path half-understood binds the wrong buffer.
[[nodiscard]] std::string SerializePath(const PathPlan& plan);
[[nodiscard]] bool ParsePath(const std::string& text, PathPlan* out);

// ---- the program's own account of where the table is ----------------------

// One SMRD load the program makes out of its user data.
struct UserDataLoad {
    std::uint32_t at_dword = 0;   // the instruction's index in the program
    std::uint32_t op = 0;         // the SMRD op (1 = s_load_dwordx2, 2 = x4, ...)
    std::uint32_t sdst = 0;       // destination SGPR
    std::uint32_t sbase = 0;      // source SGPR
    std::uint32_t offset_dw = 0;  // the byte offset, in dwords
    std::uint32_t dwords = 0;     // how many it loads
    bool buffer = false;          // s_buffer_load_* (through a V# base)
    // True when the source pair is inside the stage's sixteen user SGPRs. A load
    // out of s[40:41] is a register-file value the program computed, so it can be
    // a *dereference* of the table pointer but it can never be the pointer itself.
    bool from_user_data = false;
};

// Every SMRD load in the program, in program order, with `from_user_data` set
// when its source pair is one of the sixteen user SGPRs. Stops at s_endpgm. `words`/`count` are the program's dwords as the
// guest's memory holds them (gcn/decode.cpp's SMRD encoding: 0b11000 in bits
// 31:27, op in 26:22, sdst in 21:15, sbase in 14:9 as an SGPR pair index, the
// immediate flag in bit 8, the byte offset in 7:0).
[[nodiscard]] std::vector<UserDataLoad> DecodeUserDataLoads(const std::uint32_t* words, std::size_t count);

// Which user-data slot holds the resource table's pointer, or -1 when the
// program never loads one. A slot qualifies when the program loads a 64-bit
// value out of it with an `s_load_dwordx2`/`x4` (the table's address) and then
// dereferences the result - the V# it loads from that address is a binding's
// resource. `evidence` receives the instruction the answer came from, so a
// caller can log the reason instead of the number.
[[nodiscard]] int DecodeTableSlot(const std::uint32_t* words, std::size_t count, std::string* evidence);

}  // namespace bbx::d3d12
