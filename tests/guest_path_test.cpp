// SPDX-License-Identifier: GPL-3.0-or-later
// tests/guest_path_test.cpp -- how a binding finds its bytes (bbx_guest_path).
//
// Two halves, and the second is the one that has to be proved rather than
// assumed: which user-data slot carries a dispatch's resource table pointer.
// The answer is not a convention this port gets to choose - it is what the
// program does, so every case below is built out of real GCN instruction words
// (gcn/decode.cpp's SMRD encoding) and the decode reads them back.
//
// The instruction words are spelled out here rather than imported from the
// translator, on purpose: a test that shares the decoder with the code under
// test proves nothing. The bit layout is the one gcn/decode.cpp documents -
// 0b11000 in bits 31:27, op in 26:22, sdst in 21:15, sbase in 14:9 as an SGPR
// pair index, the immediate flag in bit 8, the byte offset in 7:0.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "harness.h"
#include "host/d3d12/bbx_guest_path.h"
#include "host/d3d12/bbx_guest_vsharp.h"

using namespace bbx::d3d12;

namespace {

// gcn/decode.cpp: SMRD = (word >> 27) == 0b11000.
std::uint32_t Smrd(std::uint32_t op, std::uint32_t sdst, std::uint32_t sbase_reg, std::uint32_t offset_bytes) {
    return (0b11000u << 27) | ((op & 0x1f) << 22) | ((sdst & 0x7f) << 15) | (((sbase_reg / 2) & 0x3f) << 9) |
           (1u << 8) | (offset_bytes & 0xffu);
}
constexpr std::uint32_t kSEndpgm = 0xBF810000u;

// A V# as the guest's memory holds one (tests/guest_vsharp_test.cpp's helper,
// repeated rather than shared: this file's cases must not depend on the file
// whose behaviour they are checking).
void MakeVSharpForTest(std::uint32_t words[4], std::uint64_t base, std::uint32_t stride,
                       std::uint32_t num_records) {
    words[0] = static_cast<std::uint32_t>(base & 0xffffffffu);
    words[1] = static_cast<std::uint32_t>(((base >> 32) & 0xffu) | ((stride & 0x3fffu) << 16));
    words[2] = num_records;
    words[3] = 0;
}

// A guest memory the resolver can read: a flat 64 KiB window.
struct FlatMemory {
    std::uint64_t base = 0x1'0000'0000ull;
    std::vector<std::uint32_t> dwords;
};

bool ReadFlat(void* context, std::uint64_t va, void* out, std::size_t bytes) {
    auto* memory = static_cast<FlatMemory*>(context);
    if (va < memory->base) return false;
    const std::uint64_t offset = va - memory->base;
    if (offset + bytes > memory->dwords.size() * sizeof(std::uint32_t)) return false;
    std::memcpy(out, reinterpret_cast<const std::uint8_t*>(memory->dwords.data()) + offset, bytes);
    return true;
}

void WriteDwords(FlatMemory* memory, std::uint64_t at, const std::uint32_t* words, std::size_t count) {
    const std::size_t index = static_cast<std::size_t>(at / 4);
    if (memory->dwords.size() < index + count) memory->dwords.resize(index + count, 0);
    std::memcpy(memory->dwords.data() + index, words, count * sizeof(std::uint32_t));
}

}  // namespace

// ---- the walk ---------------------------------------------------------------

TEST(GuestPath_InterpBaseMasksTheVSharpBaseToFortyBits) {
    std::uint32_t words[4] = {0x4000'0000u, 0xff00'0010u, 0, 0};
    CHECK_EQ(InterpBase(words, true), 0x10'4000'0000ull);
    // A pointer takes all 32 bits of word 1: a table's address is a byte address,
    // not a V#'s base-plus-flags pair.
    CHECK_EQ(InterpBase(words, false), 0xff00'0010'4000'0000ull);
}

TEST(GuestPath_ImmediateWordsComeStraightOutOfUserData) {
    PathPlan plan;
    plan.user_sgpr = 4;
    plan.immediate = true;
    const std::uint32_t user[16] = {0, 0, 0, 0, 0x11223344u, 0x55667788u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::uint32_t out[4] = {};
    const PathResult result = ResolvePath(plan, user, 2, nullptr, nullptr, out);
    CHECK(result.verdict == PathVerdict::kResolved);
    CHECK_EQ(out[0], 0x11223344u);
    CHECK_EQ(out[1], 0x55667788u);
    CHECK_EQ(result.at, 0ull);  // nothing was dereferenced
}

TEST(GuestPath_ImmediateWordsPastUserSgpr15AreRefused) {
    PathPlan plan;
    plan.user_sgpr = 15;
    plan.immediate = true;
    const std::uint32_t user[16] = {};
    std::uint32_t out[4] = {};
    // 15 is the last user SGPR: a pair starting there is already out of range,
    // and four dwords from it is certainly not "immediate".
    CHECK(ResolvePath(plan, user, 4, nullptr, nullptr, out).verdict == PathVerdict::kBadSlot);
    plan.user_sgpr = 14;
    CHECK(ResolvePath(plan, user, 4, nullptr, nullptr, out).verdict == PathVerdict::kShortImmediate);
    CHECK(ResolvePath(plan, user, 2, nullptr, nullptr, out).verdict == PathVerdict::kResolved);
}

TEST(GuestPath_ANegativeSlotIsRefused) {
    PathPlan plan;  // user_sgpr stays -1
    const std::uint32_t user[16] = {};
    std::uint32_t out[4] = {};
    CHECK(ResolvePath(plan, user, 4, nullptr, nullptr, out).verdict == PathVerdict::kBadSlot);
}

TEST(GuestPath_OneDereferenceReadsTheTableEntry) {
    // The program's user data 0/1 holds the address of the resource table; slot
    // 7's V# is at table + 7 * 16, and the bytes it describes are at its base.
    FlatMemory memory;
    memory.base = 0x4000'0000ull;  // the whole window, table and payload
    std::uint32_t vsharp[4] = {};
    MakeVSharpForTest(vsharp, memory.base + 0x800, 4, 256);
    WriteDwords(&memory, 7 * 16, vsharp, 4);
    const std::uint32_t payload[4] = {0xcafe'0001u, 0xcafe'0002u, 0xcafe'0003u, 0xcafe'0004u};
    WriteDwords(&memory, 0x800, payload, 4);
    const std::uint32_t user[16] = {static_cast<std::uint32_t>(memory.base), static_cast<std::uint32_t>(memory.base >> 32),
                                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    PathPlan plan;
    plan.user_sgpr = 0;
    plan.loads.push_back(PathStep{7u * 4u, false});
    // The entry read is a V#, so the bytes are addressed through the V#'s base,
    // not through the entry's own address.
    plan.final_vsharp = true;
    std::uint32_t out[4] = {};
    const PathResult result = ResolvePath(plan, user, 4, ReadFlat, &memory, out);
    CHECK(result.verdict == PathVerdict::kResolved);
    CHECK_EQ(result.at, memory.base + 0x800);
    CHECK_EQ(out[0], 0xcafe'0001u);
    CHECK_EQ(out[1], 0xcafe'0002u);
}

TEST(GuestPath_AReadTheCallerCannotSupplyIsRefusedRatherThanZeroFilled) {
    FlatMemory memory;  // empty: nothing is mapped
    const std::uint32_t user[16] = {static_cast<std::uint32_t>(memory.base),
                                    static_cast<std::uint32_t>(memory.base >> 32), 0, 0,
                                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    PathPlan plan;
    plan.user_sgpr = 0;
    plan.loads.push_back(PathStep{0, false});
    std::uint32_t out[4] = {};
    CHECK(ResolvePath(plan, user, 4, ReadFlat, &memory, out).verdict == PathVerdict::kUnmapped);
    PathPlan flat;  // no load at all: the final read is the one that fails
    flat.user_sgpr = 0;
    CHECK(ResolvePath(flat, user, 4, ReadFlat, &memory, out).verdict == PathVerdict::kUnmapped);
}

TEST(GuestPath_AVSharpStepReadsThroughTheBaseNotThePointer) {
    // A step marked vsharp takes its address from the V#'s base - eight bits of
    // word 1 - so a pointer whose word 1 has high bits set does not read a
    // million pages away.
    FlatMemory memory;
    memory.base = 0;  // so the V# base is an offset into the flat window
    std::uint32_t inner[4] = {0xdead'beefu, 0xfeed'faceu, 0, 0};
    const std::uint64_t at = 0x100;
    WriteDwords(&memory, at, inner, 4);
    const std::uint32_t user[16] = {static_cast<std::uint32_t>(at), 0xff00'0000u, 0, 0,
                                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    PathPlan plan;
    plan.user_sgpr = 0;
    plan.final_vsharp = true;  // the final address comes from the V# base
    std::uint32_t out[4] = {};
    CHECK(ResolvePath(plan, user, 4, ReadFlat, &memory, out).verdict == PathVerdict::kResolved);
    CHECK_EQ(out[0], 0xdead'beefu);
}

// ---- where the table's pointer lives ---------------------------------------
//
// The evidence. A compute dispatch's user SGPRs come from the guest's command
// stream (COMPUTE_USER_DATA_0..15 = registers 0x240..0x24F, hle/gnm_exec.cpp's
// run_dispatch), and the program decides which of them is the table: it loads
// the table's address with an s_load_dwordx2 out of an SGPR pair and then reads
// a V# through what it loaded.

TEST(GuestPath_TheTablePointerIsUserDataZeroBecauseTheProgramLoadsItWithSLoadDwordx2) {
    // s_load_dwordx2 s[4:5], s[0:1], 0x0        ; the table's address, from user 0/1
    // s_buffer_load_dwordx4 s[8:11], s[4:7], 0x10 ; slot 1's V#, through it
    // s_endpgm
    const std::uint32_t program[] = {Smrd(1, 4, 0, 0), Smrd(10, 8, 4, 0x10), kSEndpgm};
    std::string evidence;
    CHECK_EQ(DecodeTableSlot(program, 3, &evidence), 0);
    CHECK(evidence.find("s_load_dwordx2 from s[0:1]+0x0 at dword 0") != std::string::npos);
    CHECK(evidence.find("dereferenced by s_buffer_load_dwordx4 from s[4:7]+0x10 at dword 1") !=
          std::string::npos);
}

TEST(GuestPath_TheTablePointerIsUserDataEightWhenTheProgramSaysSo) {
    // The same program with the pointer in user data 8/9: the answer follows the
    // instruction, which is the whole point of decoding it.
    const std::uint32_t program[] = {Smrd(1, 12, 8, 0), Smrd(10, 16, 12, 0x20), kSEndpgm};
    std::string evidence;
    CHECK_EQ(DecodeTableSlot(program, 3, &evidence), 8);
    CHECK(evidence.find("s_load_dwordx2 from s[8:9]") != std::string::npos);
}

TEST(GuestPath_ALoadedValueThatIsNeverDereferencedIsNotTheTable) {
    // A 64-bit load that nothing reads through is a constant, not a table.
    const std::uint32_t program[] = {Smrd(1, 4, 0, 0), kSEndpgm};
    CHECK_EQ(DecodeTableSlot(program, 2, nullptr), -1);
}

TEST(GuestPath_AProgramThatLoadsNoPointerHasNoTableSlot) {
    // Only a V# load out of user data: no 64-bit pointer anywhere.
    const std::uint32_t program[] = {Smrd(10, 8, 0, 0x30), kSEndpgm};
    CHECK_EQ(DecodeTableSlot(program, 2, nullptr), -1);
}

TEST(GuestPath_ADwordLoadIsNotAPointerLoad) {
    // s_load_dword (op 0) loads one dword: not an address, so not the table.
    const std::uint32_t program[] = {Smrd(0, 4, 0, 0), Smrd(10, 8, 4, 0), kSEndpgm};
    CHECK_EQ(DecodeTableSlot(program, 3, nullptr), -1);
}

TEST(GuestPath_TheDecodeStopsAtSEndpgm) {
    // Instructions after s_endpgm are not the program: data in its tail can hold
    // anything, including something that looks like a table load.
    const std::uint32_t program[] = {kSEndpgm, Smrd(1, 4, 0, 0), Smrd(10, 8, 4, 0)};
    CHECK_EQ(DecodeTableSlot(program, 4, nullptr), -1);
    CHECK(DecodeUserDataLoads(program, 4).empty());
}

TEST(GuestPath_LoadsOutOfTheStageSgprsBeyondUserDataAreNotTheTablePointer) {
    // s[20:21] is not user data: the sixteen user SGPRs are s0..s15. The loads
    // are still decoded - one may be the dereference - but neither can be the
    // pointer, because the pointer has to come from the guest's user data.
    const std::uint32_t program[] = {Smrd(1, 24, 20, 0), Smrd(10, 28, 24, 0), kSEndpgm};
    const std::vector<UserDataLoad> loads = DecodeUserDataLoads(program, 3);
    CHECK_EQ(loads.size(), 2u);
    CHECK(!loads[0].from_user_data);
    CHECK(!loads[1].from_user_data);
    CHECK_EQ(DecodeTableSlot(program, 3, nullptr), -1);
}

TEST(GuestPath_ALoadOfTheWholeUserFileIsDecodedWithItsWidth) {
    // s_load_dwordx16 s[0:15], s[0:0], 0x0 reads the whole sixteen-dword file.
    const std::uint32_t whole[] = {Smrd(4, 0, 0, 0), kSEndpgm};
    const std::vector<UserDataLoad> loads = DecodeUserDataLoads(whole, 2);
    CHECK_EQ(loads.size(), 1u);
    CHECK_EQ(loads[0].dwords, 16u);
    CHECK_EQ(loads[0].sbase, 0u);
    CHECK(!loads[0].buffer);
}

TEST(GuestPath_ALoadThatWouldRunPastUserSgpr15IsNotAUserDataLoad) {
    // x16 starting at s[2:3] would need s[2:17]: the file ends at s15, so this is
    // a register-file read and not a table load.
    const std::uint32_t past[] = {Smrd(4, 0, 2, 0), kSEndpgm};
    const std::vector<UserDataLoad> loads = DecodeUserDataLoads(past, 2);
    CHECK_EQ(loads.size(), 1u);
    CHECK(!loads[0].from_user_data);
}

TEST(GuestPath_ABufferLoadIsMarkedAsGoingThroughAVSharp) {
    const std::uint32_t program[] = {Smrd(11, 8, 0, 0x40), kSEndpgm};
    const std::vector<UserDataLoad> loads = DecodeUserDataLoads(program, 2);
    CHECK_EQ(loads.size(), 1u);
    CHECK(loads[0].buffer);
    CHECK_EQ(loads[0].offset_dw, 0x40u);
}

TEST(GuestPath_APlanSaysWhatItWalks) {
    PathPlan plan;
    plan.user_sgpr = 0;
    plan.loads.push_back(PathStep{4, false});
    plan.final_offset_dw = 2;
    CHECK(plan.str() == "s0 -> [ptr+0x10] -> [ptr+0x8]");
    PathPlan immediate;
    immediate.user_sgpr = 6;
    immediate.immediate = true;
    CHECK(immediate.str() == "immediate s6");
}

// ---- the plan on one line ---------------------------------------------------
//
// The bake has the translator's assignment and the console does not, so the plan
// crosses between them as text. A format either side misunderstands binds the
// wrong buffer, so both directions and every refusal are tested.

TEST(GuestPath_APlanRoundTripsThroughItsText) {
    PathPlan plan;
    plan.user_sgpr = 3;
    plan.loads.push_back(PathStep{2, false});
    plan.loads.push_back(PathStep{0x11, true});
    plan.final_offset_dw = 7;
    plan.final_vsharp = true;
    const std::string text = SerializePath(plan);
    CHECK(text == "user=3;imm=0;fin=7;finv=1;loads=2:0,17:1");
    PathPlan back;
    CHECK(ParsePath(text, &back));
    CHECK_EQ(back.user_sgpr, 3);
    CHECK(!back.immediate);
    CHECK_EQ(back.final_offset_dw, 7);
    CHECK(back.final_vsharp);
    CHECK_EQ(back.loads.size(), 2u);
    CHECK(back.loads[0] == (PathStep{2, false}));
    CHECK(back.loads[1] == (PathStep{0x11, true}));
    CHECK(SerializePath(back) == text);
}

TEST(GuestPath_AnImmediatePlanRoundTripsWithNoLoads) {
    PathPlan plan;
    plan.user_sgpr = 6;
    plan.immediate = true;
    const std::string text = SerializePath(plan);
    CHECK(text == "user=6;imm=1;fin=0;finv=0;loads=");
    PathPlan back;
    CHECK(ParsePath(text, &back));
    CHECK(back.immediate);
    CHECK(back.loads.empty());
}

TEST(GuestPath_TextThisFormatDidNotWriteIsRefused) {
    PathPlan plan;
    // A missing key.
    CHECK(!ParsePath("user=0;imm=0;fin=0;loads=", &plan));
    // A key it does not define: a future bake's field must not be ignored.
    CHECK(!ParsePath("user=0;imm=0;fin=0;finv=0;loads=;stride=16", &plan));
    // A slot outside the sixteen user SGPRs.
    CHECK(!ParsePath("user=16;imm=0;fin=0;finv=0;loads=", &plan));
    CHECK(!ParsePath("user=-2;imm=0;fin=0;finv=0;loads=", &plan));
    // A load step without its vsharp flag, and a bad flag.
    CHECK(!ParsePath("user=0;imm=0;fin=0;finv=0;loads=4", &plan));
    CHECK(!ParsePath("user=0;imm=0;fin=0;finv=0;loads=4:2", &plan));
    // A flag that is not 0 or 1.
    CHECK(!ParsePath("user=0;imm=2;fin=0;finv=0;loads=", &plan));
    // Text that is not this format at all.
    CHECK(!ParsePath("", &plan));
    CHECK(!ParsePath("s0 -> [ptr+0x10]", &plan));
}

TEST(GuestPath_AParsedPlanResolvesExactlyLikeTheOneThatWasWritten) {
    // The round trip is only worth anything if it resolves: same words, same
    // address, same refusal.
    FlatMemory memory;
    memory.base = 0x5000'0000ull;
    const std::uint32_t payload[4] = {0xdead'0001u, 0xdead'0002u, 0xdead'0003u, 0xdead'0004u};
    WriteDwords(&memory, 0, payload, 4);
    const std::uint32_t user[16] = {static_cast<std::uint32_t>(memory.base),
                                    static_cast<std::uint32_t>(memory.base >> 32), 0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 0, 0, 0};
    PathPlan plan;
    plan.user_sgpr = 0;  // the words are at the pointer itself
    PathPlan back;
    CHECK(ParsePath(SerializePath(plan), &back));
    std::uint32_t first[4] = {};
    std::uint32_t second[4] = {};
    const PathResult a = ResolvePath(plan, user, 4, ReadFlat, &memory, first);
    const PathResult b = ResolvePath(back, user, 4, ReadFlat, &memory, second);
    CHECK(a.verdict == PathVerdict::kResolved);
    CHECK(b.verdict == PathVerdict::kResolved);
    CHECK_EQ(a.at, b.at);
    CHECK_EQ(first[0], second[0]);
}
