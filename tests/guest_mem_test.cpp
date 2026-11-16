// SPDX-License-Identifier: GPL-3.0-or-later
// tests/guest_mem_test.cpp -- the guest's page table and slab table (bbx_guest_mem).
//
// What a wrong answer here costs: a shader that reads a page it has no business
// in is supposed to hit the guard page, and if the table is off by one it
// instead reads the title's memory and the corruption shows up minutes later in
// a level nobody connected to it. The tests therefore check the tables twice:
// once through the builder's own accessors, and once by repeating the *shader's*
// arithmetic (hlsl.cpp console_memory_prelude: l1[min(hi, 4095)] -> an 8-byte
// entry index, (l2_base + ((lo >> 16) & 0xffff)) * 8) over the serialized bytes.
// A builder that agrees with itself but not with the shader is the bug this file
// exists to catch.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "harness.h"
#include "host/d3d12/bbx_guest_mem.h"
#include "host/d3d12/bbx_guest_root.h"

using namespace bbx::d3d12;

namespace {

std::uint32_t U32At(const std::vector<std::uint8_t>& bytes, std::uint64_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, bytes.data() + at, 4);
    return v;
}

// The shader's walk, over the page table's bytes, exactly as HLSL spells it:
//   l2_base = g_page_table.Load(min(va_hi, kL1Entries - 1) * 4)
//   slot    = g_page_table.Load2((l2_base + ((va_lo >> 16) & 0xffff)) * 8)
GuestSlot ShaderWalk(const std::vector<std::uint8_t>& table, std::uint64_t va) {
    const std::uint32_t lo = static_cast<std::uint32_t>(va);
    const std::uint32_t hi = static_cast<std::uint32_t>(va >> 32);
    const std::uint32_t l1_at = (hi < kPageL1Entries ? hi : kPageL1Entries - 1) * 4;
    const std::uint32_t l2_base = U32At(table, l1_at);
    const std::uint32_t entry = (l2_base + ((lo >> kPageShift) & (kPageL2Entries - 1))) * 8;
    return GuestSlot{U32At(table, entry), U32At(table, entry + 4)};
}

}  // namespace

// An empty table reads the guard page everywhere: slab 0, offset 0, for every
// address a shader could name - including one past the last L1 entry, which the
// shader clamps to the last region rather than reading out of the buffer.
TEST(GuestMem_EmptyTableIsTheGuardPage) {
    const GuestPageTable table;
    const std::uint64_t probes[] = {0, 0x1000, 0x40000000ull, 0x100000000ull, 0xff00000000ull, 0xffffffffffffffffull};
    for (const std::uint64_t va : probes) {
        CHECK(table.SlotAt(va).guard());
        CHECK(!table.IsMapped(va));
    }
    CHECK_EQ(table.L2Tables(), 0u);
    CHECK_EQ(table.MappedBytes(), 0ull);

    const std::vector<std::uint8_t> bytes = table.Bytes();
    CHECK_EQ(static_cast<std::uint64_t>(bytes.size()), kPageL1Bytes + kPageL2Bytes);
    for (const std::uint64_t va : probes) CHECK(ShaderWalk(bytes, va).guard());
    // Every L1 dword points at the zero table, and every zero entry is the guard.
    for (std::uint32_t r = 0; r < kPageL1Entries; ++r) CHECK_EQ(U32At(bytes, std::uint64_t{r} * 4), static_cast<std::uint32_t>(kPageL1Bytes / 8));
    CHECK(ShaderWalk(bytes, 0xff00000000ull).guard());
}

// A mapped page reads back where it was put, and the page next to it does not:
// the guard is what an unmapped page means.
TEST(GuestMem_MapOnePage) {
    GuestPageTable table;
    CHECK(table.MapRange(kPageBytes, kPageBytes, 3, 4 * kPageBytes));
    CHECK_EQ(table.SlotAt(kPageBytes).slab, 3u);
    CHECK_EQ(table.SlotAt(kPageBytes).offset, 4 * kPageBytes);
    CHECK(table.IsMapped(kPageBytes));
    CHECK(table.SlotAt(0x0000).guard());
    CHECK(table.SlotAt(2 * kPageBytes).guard());
    CHECK_EQ(table.MappedBytes(), kPageBytes);

    const std::vector<std::uint8_t> bytes = table.Bytes();
    CHECK(ShaderWalk(bytes, kPageBytes) == (GuestSlot{3, 4 * kPageBytes}));
    CHECK(ShaderWalk(bytes, 0x0000).guard());
    // The L1's dword for the region names the table's first entry: a dword index
    // that the shader turns into a byte offset by multiplying by 8.
    CHECK_EQ(U32At(bytes, 0), static_cast<std::uint32_t>(kPageL1Bytes / 8 + kPageL2Bytes / 8));
}

// A range has to stay inside one 4 GiB region: the shader's VA is two dwords,
// so a range that crosses means the second half would be read through the first
// region's table.
TEST(GuestMem_RangeMayNotCrossFourGiB) {
    GuestPageTable table;
    CHECK(!table.MapRange(0xffff0000ull, 0x20000, 1, 0));
    CHECK(table.MapRange(0xffff0000ull, 0x10000, 1, 0));
    CHECK(table.SlotAt(0xffff0000ull).slab == 1u);
    CHECK_EQ(table.SlotAt(0xffff0000ull).offset, 0u);
}

// The slab is bounded, slab 0 is the guard's and never data, and a page maps
// once: each of those is a refusal, not a clamp.
TEST(GuestMem_Refusals) {
    GuestPageTable table;
    CHECK(!table.MapRange(0, kGuestSlabBytes + kPageBytes, 1, 0));  // past the slab
    CHECK(!table.MapRange(0, kPageBytes, 1, kGuestSlabBytes));      // offset at the end
    CHECK(!table.MapRange(0, kPageBytes, kGuestSlabCount, 0));      // no such slab
    CHECK(!table.MapRange(0, kPageBytes, kGuestGuardSlab, 0));      // the guard page's own slab
    CHECK(!table.MapRange(1, kPageBytes, 1, 0));                    // not page aligned
    CHECK(!table.MapRange(0, 4, 1, 0));                             // a partial page
    CHECK(table.MapRange(0, 2 * kPageBytes, 1, 0));                 // 0x0 and 0x10000
    CHECK(!table.MapRange(0x10000, kPageBytes, 2, 0));              // already mapped
    CHECK(table.SlotAt(0x20000).guard());
}

// The alias is the same memory, not a copy: both addresses read the same slab and
// offset, so a write through either is seen through the other.
TEST(GuestMem_GpuAliasMirrorsTheSamePages) {
    GuestPageTable table;
    CHECK(table.MapRange(0, 4 * kPageBytes, 2, 0x10000));
    CHECK(table.MapAlias(kGuestGpuAliasBase, 0, 4 * kPageBytes));
    for (std::uint64_t at = 0; at < 4 * kPageBytes; at += kPageBytes) {
        CHECK(table.SlotAt(kGuestGpuAliasBase + at) == table.SlotAt(at));
        CHECK(!table.SlotAt(kGuestGpuAliasBase + at).guard());
    }
    // ... and a range whose source is unmapped has nothing to mirror.
    CHECK(!table.MapAlias(kGuestGpuAliasBase + 0x100000, 0x100000, kPageBytes));
    // ... and the alias may not cross 4 GiB either.
    CHECK(!table.MapAlias(kL1RegionBytes - kPageBytes, 0, 2 * kPageBytes));
}

// The boot's own plan: 5 GiB of RAM in slabs 1..21, the alias over its first
// 4 GiB, the guard page reserved, and every probe -- including the last page of
// RAM and the last page of the alias -- reading a real slot through the shader's
// arithmetic.
TEST(GuestMem_PlanFiBvGiB) {
    const GuestPlan plan = PlanGuestMemory();
    CHECK(plan.ok());
    CHECK_MSG(plan.refused.empty(), plan.refused);
    CHECK_EQ(plan.slabs_resident, 21u);
    CHECK_EQ(plan.windows.size(), 3u);  // RAM below 4 GiB, RAM above, the alias
    CHECK_EQ(plan.table.MappedBytes(), 5ull * (1ull << 30) + 4ull * (1ull << 30));

    const std::vector<std::uint8_t> bytes = plan.table.Bytes();
    // The first page of RAM is slab 1, offset 0; 256 MiB in is slab 2's start;
    // the page at 4 GiB is the second region's first; 5 GiB - 64 KiB is the last
    // page of the 20th slab.
    CHECK(ShaderWalk(bytes, 0) == (GuestSlot{1, 0}));
    CHECK(ShaderWalk(bytes, 256ull << 20) == (GuestSlot{2, 0}));
    CHECK(ShaderWalk(bytes, kL1RegionBytes) == (GuestSlot{17, 0}));  // page 65536: 1 + 65536/4096
    CHECK(ShaderWalk(bytes, (5ull << 30) - kPageBytes) == (GuestSlot{20, kGuestSlabBytes - kPageBytes}));
    // The alias reads the very same slots as RAM below 4 GiB.
    for (std::uint64_t at = 0; at < 4ull << 30; at += (64ull << 20)) {
        CHECK(ShaderWalk(bytes, kGuestGpuAliasBase + at) == ShaderWalk(bytes, at));
    }
    // Nothing is mapped past 5 GiB, and nothing between 5 GiB and the alias.
    CHECK(ShaderWalk(bytes, 5ull << 30).guard());
    CHECK(ShaderWalk(bytes, 0x1000000000ull).guard());
}

// The plan is bounded by the guest's own 5 GiB (hle/kernel.cpp's dmem size) and
// refuses a plan with nothing in it rather than map an empty table.
TEST(GuestMem_PlanStaysInsideTheSlabTable) {
    const GuestPlan capped = PlanGuestMemory(kGuestRamBytes * 2, kGuestGpuAliasBytes * 2);
    CHECK(capped.ok());
    CHECK_EQ(capped.table.MappedBytes(), 5ull * (1ull << 30) + 4ull * (1ull << 30));  // clamped, not refused
    CHECK(capped.slabs_resident <= kGuestSlabCount);
    const GuestPlan none = PlanGuestMemory(0, 0);
    CHECK(!none.ok());
    CHECK(none.refused.find("RAM") != std::string::npos);
}

// host_gpu_mem_write's input: the range cut where it moves to another slab, with
// byte offsets inside the buffer's own coordinates. An unmapped page is not a
// write target, and neither is a run that would run off the end of a slab.
TEST(GuestMem_SlabRuns) {
    GuestPageTable table;
    CHECK(table.MapLinear(0, 2 * kGuestSlabBytes, 0));  // a whole slab, then the next
    std::vector<GuestRun> runs;
    CHECK(SlabRuns(table, 0, 16, &runs));
    CHECK_EQ(runs.size(), 1u);
    CHECK_EQ(runs[0].slab, kGuestFirstDataSlab);
    CHECK_EQ(runs[0].offset, 0u);
    CHECK_EQ(runs[0].bytes, 16u);

    // Across the 256 MiB line: two runs, the second in slab 2 at offset 0.
    CHECK(SlabRuns(table, kGuestSlabBytes - 8, 16, &runs));
    CHECK_EQ(runs.size(), 2u);
    CHECK_EQ(runs[0].slab, kGuestFirstDataSlab);
    CHECK_EQ(runs[0].offset, kGuestSlabBytes - 8);
    CHECK_EQ(runs[0].bytes, 8u);
    CHECK_EQ(runs[1].slab, kGuestFirstDataSlab + 1);
    CHECK_EQ(runs[1].offset, 0u);

    // A page boundary inside one slab is not a new run: the offsets are contiguous.
    CHECK(SlabRuns(table, kPageBytes - 4, 8, &runs));
    CHECK_EQ(runs.size(), 1u);
    CHECK_EQ(runs[0].offset, kPageBytes - 4);

    // Unmapped: refused, so nothing writes the guard page "successfully".
    CHECK(!SlabRuns(table, 2 * kGuestSlabBytes, 4, &runs));
    CHECK(!SlabRuns(table, 0, 0, &runs));
}

// The tables are a function of what is mapped, not of the order it was mapped in:
// two plans built in different orders serialize to the same bytes, or a boot's
// layout would move with its mapping history.
TEST(GuestMem_TableLayoutIsDeterministic) {
    GuestPageTable a;
    CHECK(a.MapRange(0, kPageBytes, 1, 0));
    CHECK(a.MapRange(kL1RegionBytes, kPageBytes, 2, 0));
    GuestPageTable b;
    CHECK(b.MapRange(kL1RegionBytes, kPageBytes, 2, 0));
    CHECK(b.MapRange(0, kPageBytes, 1, 0));
    CHECK(a.Bytes() == b.Bytes());
    CHECK_EQ(a.BufferBytes(), b.BufferBytes());
    CHECK_EQ(a.BufferBytes(), kPageL1Bytes + 3 * kPageL2Bytes);  // L1, the zero table, two regions
    CHECK_EQ(a.TableOffset(0), kPageL1Bytes + kPageL2Bytes);
    CHECK_EQ(a.TableOffset(1), kPageL1Bytes + 2 * kPageL2Bytes);
    CHECK_EQ(a.TableOffset(2), kPageL1Bytes);  // unmapped: the zero table
}

// ---- the guest root signature (bbx_guest_root) ------------------------------------

// The signature fits the console's 64 DWORDs, and it names the registers the
// baked shaders name: b0 space1 for StageParams, t0 and u0..u31 in space3.
TEST(GuestRoot_FitsAndNamesTheShadersRegisters) {
    const GuestRootLayout layout = GuestRoot();
    CHECK(layout.Valid());
    CHECK(layout.Dwords() <= GuestRootLayout::kMaxDwords);
    CHECK_EQ(layout.Dwords(), 4u);  // two root descriptors' worth: a CBV (2) and two tables (2)

    bool params_seen = false;
    for (const GuestRootParam& p : layout.params) {
        if (p.kind == GuestRootParamKind::kParamsCbv) params_seen = true;
    }
    CHECK(params_seen);

    GuestBindingSlot slot{};
    CHECK(GuestBinding(layout, GuestRangeKind::kSrv, kGuestPageTableRegister, kGuestSpace, &slot));
    CHECK(slot.param == GuestRootParamKind::kGuestMemory);
    CHECK_EQ(slot.offset, 0u);  // the page table is the table's first range
    CHECK(GuestBinding(layout, GuestRangeKind::kUav, kGuestSlabRegister + kGuestSlabCount - 1, kGuestSpace, &slot));
    CHECK(slot.param == GuestRootParamKind::kGuestMemory);
    CHECK_EQ(slot.offset, 1u + (kGuestSlabCount - 1));  // after the page table's one SRV

    // A guest shader's own resources live in the console's descriptor set, and
    // the page table is not reachable there: the two spaces must not overlap.
    CHECK(GuestBinding(layout, GuestRangeKind::kSrv, 0, kGuestParamsSpace, &slot));
    CHECK(slot.param == GuestRootParamKind::kStageResources);
    CHECK_EQ(slot.offset, 0u);
    CHECK(GuestBinding(layout, GuestRangeKind::kUav, 0, kGuestParamsSpace, &slot));
    CHECK_EQ(slot.offset, kGuestSrvCount);
    CHECK(GuestBinding(layout, GuestRangeKind::kSampler, 3, kGuestParamsSpace, &slot));
    CHECK_EQ(slot.offset, kGuestSrvCount + kGuestUavCount + 3);
}

// A binding outside every range is refused, not clamped: a stale descriptor read
// as a texture is a picture that is wrong in a way no log explains.
TEST(GuestRoot_RefusesABindingItDoesNotCarry) {
    const GuestRootLayout layout = GuestRoot();
    GuestBindingSlot slot{};
    CHECK(!GuestBinding(layout, GuestRangeKind::kSrv, kGuestSrvCount, kGuestParamsSpace, &slot));
    CHECK(!GuestBinding(layout, GuestRangeKind::kUav, kGuestUavCount, kGuestParamsSpace, &slot));
    CHECK(!GuestBinding(layout, GuestRangeKind::kSampler, kGuestSamplerCount, kGuestParamsSpace, &slot));
    CHECK(!GuestBinding(layout, GuestRangeKind::kUav, kGuestSlabRegister, kGuestSpace + 1, &slot));
    CHECK(layout.Describe().find("space3") != std::string::npos ||
          layout.Describe().find("space 3") != std::string::npos);
}
