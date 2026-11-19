// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "xbox/platform/guest_swap.inc"

#include <cstring>
#include <vector>

using namespace bbx::guest;

namespace {

struct MemFile {
    std::vector<std::uint8_t> bytes;
    explicit MemFile(std::uint64_t size) : bytes(size, 0) {}

    static bool Write(void* ctx, std::uint64_t offset, const void* src, std::size_t len) {
        auto* file = static_cast<MemFile*>(ctx);
        if (offset + len > file->bytes.size()) return false;
        std::memcpy(file->bytes.data() + offset, src, len);
        return true;
    }
    static bool Read(void* ctx, std::uint64_t offset, void* dst, std::size_t len) {
        auto* file = static_cast<MemFile*>(ctx);
        if (offset + len > file->bytes.size()) return false;
        std::memcpy(dst, file->bytes.data() + offset, len);
        return true;
    }
    SpillIo Io() { return SpillIo{this, &Write, &Read}; }
};

bool FailWrite(void*, std::uint64_t, const void*, std::size_t) { return false; }
bool OkRead(void*, std::uint64_t, void*, std::size_t) { return true; }

}  // namespace

TEST(SpillStoresAndLoadsByKey) {
    Spill spill;
    MemFile file(kSpillSlotBytes * 4);
    CHECK(SpillInit(spill, file.Io(), 4));
    CHECK(spill.Ready());
    CHECK_EQ(spill.FileBytes(), static_cast<std::uint64_t>(kSpillSlotBytes) * 4);

    std::vector<std::uint8_t> payload(1024, 0xAB);
    CHECK(SpillStore(spill, 42, payload.data(), payload.size()));
    CHECK(SpillResident(spill, 42));
    CHECK_EQ(spill.UsedSlots(), 1u);

    std::vector<std::uint8_t> back(1024, 0);
    CHECK(SpillLoad(spill, 42, back.data(), back.size()));
    CHECK(back == payload);
    CHECK_EQ(spill.bytes_written, static_cast<std::uint64_t>(1024));
    CHECK_EQ(spill.bytes_read, static_cast<std::uint64_t>(1024));
}

TEST(SpillOverwritesInPlace) {
    Spill spill;
    MemFile file(kSpillSlotBytes * 2);
    CHECK(SpillInit(spill, file.Io(), 2));
    std::vector<std::uint8_t> first(64, 1), second(64, 2), back(64, 0);
    CHECK(SpillStore(spill, 7, first.data(), first.size()));
    CHECK(SpillStore(spill, 7, second.data(), second.size()));
    CHECK_EQ(spill.UsedSlots(), 1u);
    CHECK(SpillLoad(spill, 7, back.data(), back.size()));
    CHECK(back == second);
}

TEST(SpillEvictsTheLeastRecentlyUsed) {
    Spill spill;
    MemFile file(kSpillSlotBytes * 2);
    CHECK(SpillInit(spill, file.Io(), 2));
    std::uint8_t a[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    std::uint8_t b[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    std::uint8_t c[8] = {3, 3, 3, 3, 3, 3, 3, 3};
    CHECK(SpillStore(spill, 1, a, sizeof(a)));
    CHECK(SpillStore(spill, 2, b, sizeof(b)));
    // Touch 1 so 2 is the oldest.
    CHECK(SpillLoad(spill, 1, a, sizeof(a)));
    CHECK(SpillStore(spill, 3, c, sizeof(c)));
    CHECK(SpillResident(spill, 1));
    CHECK(!SpillResident(spill, 2));
    CHECK(SpillResident(spill, 3));
    CHECK_EQ(spill.evictions, static_cast<std::uint64_t>(1));
    CHECK_EQ(spill.UsedSlots(), 2u);
}

TEST(SpillRejectsOversizedAndUnknownKeys) {
    Spill spill;
    MemFile file(kSpillSlotBytes * 2);
    CHECK(SpillInit(spill, file.Io(), 2));
    std::vector<std::uint8_t> too_big(kSpillSlotBytes + 1, 0);
    CHECK(!SpillStore(spill, 5, too_big.data(), too_big.size()));
    CHECK(!SpillStore(spill, 5, nullptr, 8));
    std::uint8_t out[8];
    CHECK(!SpillLoad(spill, 5, out, sizeof(out)));
    CHECK_EQ(spill.UsedSlots(), 0u);
}

TEST(SpillTrimFreesToTheRequestedSize) {
    Spill spill;
    MemFile file(kSpillSlotBytes * 8);
    CHECK(SpillInit(spill, file.Io(), 8));
    std::uint8_t value[16] = {};
    for (std::uint64_t key = 0; key < 6; ++key) {
        CHECK(SpillStore(spill, key, value, sizeof(value)));
    }
    CHECK_EQ(spill.UsedSlots(), 6u);
    CHECK_EQ(SpillTrim(spill, 2), 4u);
    CHECK_EQ(spill.UsedSlots(), 2u);
    // The survivals are the newest keys.
    CHECK(SpillResident(spill, 5));
    CHECK(SpillResident(spill, 4));
}

TEST(SpillInitClampsAndRefusesNonsense) {
    Spill spill;
    MemFile file(kSpillSlotBytes * 2);
    CHECK(!SpillInit(spill, SpillIo{}, 2));
    SpillIo io = file.Io();
    io.read = nullptr;
    CHECK(!SpillInit(spill, io, 2));
    CHECK(!SpillInit(spill, file.Io(), 0));
    CHECK(SpillInit(spill, file.Io(), kSpillMaxSlots + 99));
    CHECK_EQ(spill.slots, kSpillMaxSlots);
}

TEST(SpillWriteFailureDoesNotLeaveTheKeyResident) {
    Spill spill;
    CHECK(SpillInit(spill, SpillIo{nullptr, &FailWrite, &OkRead}, 1));
    std::uint8_t value[8] = {};
    CHECK(!SpillStore(spill, 9, value, sizeof(value)));
    CHECK(!SpillResident(spill, 9));
    CHECK_EQ(spill.UsedSlots(), 0u);
    // The slot went back, so a later successful store is not starved.
    CHECK_EQ(spill.free_slots.size(), static_cast<std::size_t>(1));
}

TEST(SpillSectorAlignmentIsExact) {
    CHECK_EQ(kSpillSlotBytes % kSpillSectorAlign, 0u);
    CHECK_EQ(SpillAlignUp(1), static_cast<std::size_t>(kSpillSectorAlign));
    CHECK_EQ(SpillAlignUp(kSpillSectorAlign), static_cast<std::size_t>(kSpillSectorAlign));
    CHECK_EQ(SpillAlignUp(kSpillSectorAlign + 1), static_cast<std::size_t>(kSpillSectorAlign * 2u));
    CHECK_EQ(SpillAlignUp(kSpillSlotBytes), static_cast<std::size_t>(kSpillSlotBytes));
}
