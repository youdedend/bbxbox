// SPDX-License-Identifier: GPL-3.0-or-later
// tests/guest_binding_test.cpp -- from a baked plan to the descriptor to write.
//
// Slice 5B-2b, item 1's decision half. `guest_compute` records a dispatch with
// the space-1 slots null; the bake now says which guest memory each binding is
// (`bbx_shaders.paths`), and this is the three questions that stand between that
// sentence and a D3D12 view: does the path resolve against *this dispatch's* user
// data, is the V# it lands on a range the page table can address, and does the
// range fit inside one slab (one view names one resource).
//
// Every refusal is a case here because the alternative is worse than no
// descriptor: a null slot reads zeroes and shows up in one frame, while a slot
// bound from arithmetic that landed somewhere else reads another buffer's bytes
// and shows up in none.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "harness.h"
#include "host/d3d12/bbx_guest_binding.h"
#include "host/d3d12/bbx_guest_mem.h"

using namespace bbx::d3d12;

namespace {

// A guest memory the resolver can read: a flat window, as tests/guest_path_test's.
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
    const std::size_t index = static_cast<std::size_t>((at - memory->base) / 4);
    if (memory->dwords.size() < index + count) memory->dwords.resize(index + count, 0);
    std::memcpy(memory->dwords.data() + index, words, count * sizeof(std::uint32_t));
}

// base in words 0/1[7:0] (the guest is 40-bit), stride in word 1[29:16],
// num_records in word 2.
void MakeVSharp(std::uint32_t words[4], std::uint64_t base, std::uint32_t stride, std::uint32_t num_records) {
    words[0] = static_cast<std::uint32_t>(base & 0xffffffffu);
    words[1] = static_cast<std::uint32_t>(((base >> 32) & 0xffu) | ((stride & 0x3fffu) << 16));
    words[2] = num_records;
    words[3] = 0;
}

// A plan whose pointer is the user SGPR pair holding `where`, with the resource
// words at that address: the shape a resource table has (the program loads the
// table's address out of user data and the V# is in it).
BindingPlan TablePlan(std::uint32_t binding, bool uav) {
    BindingPlan plan;
    plan.binding = binding;
    plan.kind = uav ? BindingKind::kUav : BindingKind::kSrv;
    plan.path.user_sgpr = 0;
    plan.path.final_offset_dw = 0;
    return plan;
}

// The dispatch's own user SGPRs: the pointer the plan starts from is the pair in
// user slot 0, which is where a resource table's address comes from (the
// program's `s_load_dwordx2` out of `s[0:1]`).
void SetUserPointer(std::uint32_t* user, std::uint64_t address) {
    user[0] = static_cast<std::uint32_t>(address & 0xffffffffu);
    user[1] = static_cast<std::uint32_t>(address >> 32);
    user[2] = 0;
    user[3] = 0;
}

}  // namespace

TEST(GuestBinding_APlanThatResolvesToAMappedRangeBecomesOneView) {
    FlatMemory memory;
    GuestPageTable table;
    CHECK(table.MapRange(0x1'0001'0000ull, 0x1'0000, 3, 0x2'0000));  // 64 KiB in slab 3
    std::uint32_t words[4] = {};
    MakeVSharp(words, 0x1'0001'1000ull, 0, 4096);  // 4096 bytes inside that page
    WriteDwords(&memory, 0x1'0001'0000ull, words, 4);
    std::uint32_t user[16] = {};
    SetUserPointer(user, 0x1'0001'0000ull);
    const BindingWrite write = DecideBindingWrite(TablePlan(7, false), user, table, ReadFlat, &memory);
    CHECK(write.verdict == BindingVerdict::kWrite);
    CHECK_EQ(write.binding, 7u);
    CHECK(write.kind == BindingKind::kSrv);
    CHECK_EQ(write.slab, 3u);
    CHECK_EQ(write.offset, 0x2'1000ull);  // the page's offset in the slab, plus 0x1000
    CHECK_EQ(write.dwords, 1024u);        // 4096 bytes
    CHECK(BindingWriteLine(write) == "t7: 1024 dword(s) at slab 3 + 0x21000 (guest 0x100011000)");
}

TEST(GuestBinding_AWrittenBindingIsAUavAndTheLineSaysSo) {
    FlatMemory memory;
    GuestPageTable table;
    CHECK(table.MapRange(0x1'0002'0000ull, 0x1'0000, 5, 0));
    std::uint32_t words[4] = {};
    MakeVSharp(words, 0x1'0002'0000ull, 4, 16);  // 16 records x 4 bytes
    WriteDwords(&memory, 0x1'0002'0000ull, words, 4);
    std::uint32_t user[16] = {};
    SetUserPointer(user, 0x1'0002'0000ull);
    const BindingWrite write = DecideBindingWrite(TablePlan(0, true), user, table, ReadFlat, &memory);
    CHECK(write.verdict == BindingVerdict::kWrite);
    CHECK(write.kind == BindingKind::kUav);
    CHECK_EQ(write.dwords, 16u);
    CHECK(BindingWriteLine(write).rfind("u0: 16 dword(s)", 0) == 0);
}

TEST(GuestBinding_APathThatDoesNotResolveIsRefusedAtStepOne) {
    FlatMemory memory;
    GuestPageTable table;
    const std::uint32_t user[16] = {};
    BindingPlan plan;
    plan.binding = 4;
    // No user SGPR at all: the bake would never write this, and a plan that
    // arrived with it must not read four dwords from wherever it pointed.
    const BindingWrite write = DecideBindingWrite(plan, user, table, ReadFlat, &memory);
    CHECK(write.verdict == BindingVerdict::kUnreadable);
    CHECK(write.why.find("step 1") != std::string::npos);
    CHECK(BindingWriteLine(write).find("t4 refused") == 0);
}

TEST(GuestBinding_AnUnmappedDereferenceIsRefusedNotZeroFilled) {
    FlatMemory memory;  // empty: nothing is readable at the address it names
    GuestPageTable table;
    std::uint32_t user[16] = {};
    SetUserPointer(user, 0x1'0009'0000ull);  // outside the flat window
    const BindingWrite write = DecideBindingWrite(TablePlan(1, false), user, table, ReadFlat, &memory);
    CHECK(write.verdict == BindingVerdict::kUnreadable);
    CHECK(write.why.find("guest read not supplied") != std::string::npos);
}

TEST(GuestBinding_ANullVSharpKeepsTheNullDescriptor) {
    FlatMemory memory;
    GuestPageTable table;
    CHECK(table.MapRange(0x1'0003'0000ull, 0x1'0000, 2, 0));
    std::uint32_t words[4] = {};  // a V# of zeroes: GCN's null buffer
    WriteDwords(&memory, 0x1'0003'0000ull, words, 4);
    std::uint32_t user[16] = {};
    SetUserPointer(user, 0x1'0003'0000ull);
    const BindingWrite write = DecideBindingWrite(TablePlan(9, false), user, table, ReadFlat, &memory);
    CHECK(write.verdict == BindingVerdict::kNull);
    CHECK(BindingWriteLine(write) == "t9: null V#, the slot stays a null descriptor");
}

TEST(GuestBinding_ARangeThatCrossesAFourGiBRegionIsRefusedAtStepTwo) {
    FlatMemory memory;
    GuestPageTable table;
    CHECK(table.MapRange(0xffff'0000ull, 0x1'0000, 2, 0));
    std::uint32_t words[4] = {};
    MakeVSharp(words, 0xffff'0000ull, 0, 0x2'0000);  // 128 KiB from 4 GiB - 64 KiB
    WriteDwords(&memory, memory.base, words, 4);
    std::uint32_t user[16] = {};
    SetUserPointer(user, memory.base);
    const BindingWrite write = DecideBindingWrite(TablePlan(2, false), user, table, ReadFlat, &memory);
    CHECK(write.verdict == BindingVerdict::kRefusedSlot);
    CHECK(write.why.find("step 2") != std::string::npos);
    CHECK(write.why.find("4 GiB") != std::string::npos);
    CHECK_EQ(write.slab, 0u);
    CHECK_EQ(write.dwords, 0u);  // nothing to write
}

TEST(GuestBinding_AnUnalignedVSharpIsRefused) {
    FlatMemory memory;
    GuestPageTable table;
    CHECK(table.MapRange(0x1'0004'0000ull, 0x1'0000, 2, 0));
    std::uint32_t words[4] = {};
    MakeVSharp(words, 0x1'0004'0002ull, 0, 64);  // 2-byte aligned: not a dword range
    WriteDwords(&memory, 0x1'0004'0000ull, words, 4);
    std::uint32_t user[16] = {};
    SetUserPointer(user, 0x1'0004'0000ull);
    const BindingWrite write = DecideBindingWrite(TablePlan(3, false), user, table, ReadFlat, &memory);
    CHECK(write.verdict == BindingVerdict::kRefusedSlot);
    CHECK(write.why.find("unaligned base") != std::string::npos);
}

TEST(GuestBinding_ARangeThatSpansTwoSlabsIsRefusedAtStepThree) {
    // Two adjacent guest pages in different slabs: the range is addressable page
    // by page, but no single D3D12 buffer names it, and binding the first page's
    // slab with the whole length would read past it into the next slab's bytes.
    FlatMemory memory;
    GuestPageTable table;
    CHECK(table.MapRange(0x1'0005'0000ull, kPageBytes, 1, 0x4'0000));
    CHECK(table.MapRange(0x1'0005'0000ull + kPageBytes, kPageBytes, 2, 0x4'0000));
    std::uint32_t words[4] = {};
    MakeVSharp(words, 0x1'0005'0000ull, 0, static_cast<std::uint32_t>(3 * kPageBytes / 2));  // a page and a half
    WriteDwords(&memory, memory.base, words, 4);
    std::uint32_t user[16] = {};
    SetUserPointer(user, memory.base);
    const BindingWrite write = DecideBindingWrite(TablePlan(5, false), user, table, ReadFlat, &memory);
    CHECK(write.verdict == BindingVerdict::kNotContiguous);
    CHECK(write.why.find("step 3") != std::string::npos);
    CHECK(BindingWriteLine(write).find("slab boundary") != std::string::npos);
}

TEST(GuestBinding_APlanTheBakeNeverWroteIsNoPlanNotADefault) {
    // The default state of a decision nobody asked for is a refusal, so a caller
    // that forgets to check cannot accidentally write slot 0.
    const BindingWrite none;
    CHECK(none.verdict == BindingVerdict::kNoPlan);
    CHECK(BindingWriteLine(none) == "t0 refused (no plan from the bake): ");
    CHECK(std::string(BindingVerdictName(BindingVerdict::kNoPlan)) == "no plan from the bake");
}
