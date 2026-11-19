// SPDX-License-Identifier: GPL-3.0-or-later
// Descriptor allocation into a shader-visible bindless heap.
//
// A draw binds resources by index into one big SRV/UAV heap, the way GCN binds
// T#/S# slots. The allocator is a first-fit free list with fence-retired
// release: a table released in frame N is not handed out again until the fence
// value recorded with it has passed, because the GPU may still be reading it.
// Reusing a descriptor a frame too early is a picture corruption that only
// shows up under load, so the rule is enforced here rather than by convention.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace bbx::d3d12 {

struct DescriptorSlice {
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
    [[nodiscard]] bool valid() const { return count != 0; }
};

class DescriptorRing {
public:
    explicit DescriptorRing(std::uint32_t capacity);

    [[nodiscard]] std::uint32_t capacity() const { return capacity_; }
    [[nodiscard]] std::uint32_t used() const { return capacity_ - free_; }
    [[nodiscard]] std::uint32_t free() const { return free_; }
    [[nodiscard]] std::uint32_t high_water() const { return high_water_; }

    // 0 means "no room". The caller may call `Collect(fence)` and retry.
    [[nodiscard]] std::optional<DescriptorSlice> Allocate(std::uint32_t count,
                                                          std::uint32_t alignment = 1);
    // The table may be reused once the fence has passed `fence_value`.
    void Release(DescriptorSlice slice, std::uint64_t fence_value);
    // Hands back every released table whose fence is at or before `fence_value`.
    // Returns how many were returned.
    std::uint32_t Collect(std::uint64_t fence_value);
    // The smallest fence value among released-but-not-yet-collected slices, or 0
    // when nothing is pending. Lets callers wait only for the oldest slice rather
    // than draining the entire GPU queue.
    [[nodiscard]] std::uint64_t OldestPendingFence() const;

    // Tables that are permanently allocated (samplers, the white texture) are
    // carved out first and never released.
    [[nodiscard]] std::optional<DescriptorSlice> ReservePermanent(std::uint32_t count);

    void Reset();

private:
    struct Range {
        std::uint32_t offset;
        std::uint32_t count;
    };
    struct Pending {
        DescriptorSlice slice;
        std::uint64_t fence;
    };

    void InsertFree(Range range);
    [[nodiscard]] bool CanHold(std::uint32_t count) const { return free_ >= count; }

    std::uint32_t capacity_ = 0;
    std::uint32_t free_ = 0;
    std::uint32_t high_water_ = 0;
    std::vector<Range> free_list_;
    std::vector<Pending> pending_;
};

}  // namespace bbx::d3d12
