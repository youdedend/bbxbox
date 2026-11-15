// SPDX-License-Identifier: GPL-3.0-or-later
// The guest arena's ledger, on a host backing: the rules bbhost's patching code
// depends on, without a Win32 allocator.
#define BBX_UNIT_TEST
#include "harness.h"

#include "xbox/platform/guest_memory.inc"

using namespace bbx::guest;

TEST(ArenaStaysBelowTheOneTebLine) {
    Arena arena;
    // The image's slide, rounded to a page.
    CHECK(Alloc(arena, 0x100000000ull, 1u << 20));
    CHECK(RangeIsGuest(arena, 0x100000000ull, 4096));
    CHECK(RangeIsGuest(arena, 0x1000ff000ull, 4096));
    CHECK(!RangeIsGuest(arena, 0x1000ff000ull, 8192));  // runs past the end
    CHECK(!RangeIsGuest(arena, 0x0fffffffull, 4096));   // below the slide
    Free(arena);

    // A slide at the line itself is refused: guest pointers would be 40-bit.
    CHECK(!Alloc(arena, kAddressLimit, 1u << 20));
    // As is a size that crosses it.
    CHECK(!Alloc(arena, kAddressLimit - (1u << 19), 1u << 20));
    // And an unaligned slide.
    CHECK(!Alloc(arena, 0x100000001ull, 1u << 20));
}

TEST(CommittedIsMeasuredForTheBudget) {
    Arena arena;
    CHECK(Alloc(arena, 0x100000000ull, (3u << 20) + 4095));
    CHECK_EQ(CommittedMib(arena), static_cast<std::uint64_t>(4));  // page rounded
    Free(arena);
    CHECK_EQ(CommittedMib(arena), static_cast<std::uint64_t>(0));
}

TEST(KeptWritablePagesStayWritableButNeverExecutable) {
    Arena arena;
    CHECK(Alloc(arena, 0x200000000ull, 1u << 20));
    const std::uint64_t page = 0x200001000ull;
    // bbhost writes a constant here every frame; other writers put the page back
    // to read-only afterwards, and the value must still be writable next frame.
    KeepWritable(arena, page);
    CHECK(KeptWritable(arena, page));
    // Strict W^X: the page is read-write, and a read grant adds read, never execute.
    CHECK_EQ(static_cast<int>(EffectiveAccess(arena, page, kR)), static_cast<int>(kR | kW));
    // A neighbouring page is not affected.
    CHECK(!KeptWritable(arena, page + 0x1000));
    CHECK_EQ(static_cast<int>(EffectiveAccess(arena, page + 0x1000, kR)), static_cast<int>(kR));
    // Asking twice does not grow the ledger.
    KeepWritable(arena, page);
    CHECK_EQ(arena.keep_writable.size(), static_cast<std::size_t>(1));
    Free(arena);
}

TEST(CodePagesAreExecutableAndNotWritableAtOnce) {
    Arena arena;
    CHECK(Alloc(arena, 0x300000000ull, 1u << 20));
    const std::uint64_t lo = 0x300002000ull;
    MarkExecutableRange(arena, lo, 0x4000);
    CHECK(MarkedExecutable(arena, lo));
    CHECK(MarkedExecutable(arena, lo + 0x1000));
    CHECK(MarkedExecutable(arena, lo + 0x3000));   // the range covers 0x4000 bytes
    CHECK(!MarkedExecutable(arena, lo + 0x4000));  // and no more
    // bbhost asks for RWX; under W^X that is a write now and execute later.
    CHECK_EQ(static_cast<int>(EffectiveAccess(arena, lo, kR | kW | kX)), static_cast<int>(kR | kW));
    // A pure read of a code page still executes: the ledger remembers.
    CHECK_EQ(static_cast<int>(EffectiveAccess(arena, lo, kR)), static_cast<int>(kR | kX));
    // Read-only data outside the range stays read-only.
    CHECK_EQ(static_cast<int>(EffectiveAccess(arena, lo + 0x4000, kR)), static_cast<int>(kR));
    Free(arena);
}

TEST(WxConflictIsVisibleToTheCaller) {
    Arena arena;
    CHECK(Alloc(arena, 0x600000000ull, 1u << 20));
    const std::uint64_t page = 0x600001000ull;
    CHECK(!WxConflict(arena, page));
    KeepWritable(arena, page);
    CHECK(!WxConflict(arena, page));
    MarkExecutableRange(arena, page, 0x1000);
    // A page that is both a per-frame write target and entry code cannot be
    // mapped; the backend logs it and treats the request as a write.
    CHECK(WxConflict(arena, page));
    CHECK_EQ(static_cast<int>(EffectiveAccess(arena, page, kR | kW | kX)), static_cast<int>(kR | kW));
    Free(arena);
}

TEST(ExecutableRangesMerge) {
    Arena arena;
    CHECK(Alloc(arena, 0x400000000ull, 1u << 20));
    MarkExecutableRange(arena, 0x400000000ull, 0x2000);
    MarkExecutableRange(arena, 0x400004000ull, 0x2000);
    CHECK_EQ(arena.executable.size(), static_cast<std::size_t>(2));
    MarkExecutableRange(arena, 0x400002000ull, 0x2000);  // bridges them
    CHECK_EQ(arena.executable.size(), static_cast<std::size_t>(1));
    CHECK_EQ(arena.executable[0].first, static_cast<std::uint64_t>(0x400000000ull));
    CHECK_EQ(arena.executable[0].second, static_cast<std::uint64_t>(0x6000));
    Free(arena);
}

TEST(ProtectRefusesRangesOutsideTheArena) {
    Arena arena;
    CHECK(Alloc(arena, 0x500000000ull, 1u << 20));
    CHECK(Protect(arena, 0x500000000ull, 0x1000, kR | kW | kX));
    CHECK(!Protect(arena, 0x5000ff000ull, 0x2000, kR));
    CHECK(!Protect(arena, 0x4ffffff00ull, 0x200, kR));
    CHECK(!Protect(arena, 0x500000000ull, 0, kR));
    Free(arena);
}
