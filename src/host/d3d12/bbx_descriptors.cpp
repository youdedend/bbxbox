// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_descriptors.h"

#include <algorithm>

namespace bbx::d3d12 {

DescriptorRing::DescriptorRing(std::uint32_t capacity) : capacity_(capacity), free_(capacity) {
    free_list_.push_back(Range{0, capacity});
}

std::optional<DescriptorSlice> DescriptorRing::Allocate(std::uint32_t count,
                                                        std::uint32_t alignment) {
    if (count == 0 || count > capacity_ || !CanHold(count)) return std::nullopt;
    alignment = alignment == 0 ? 1 : alignment;
    for (std::size_t i = 0; i < free_list_.size(); ++i) {
        Range& range = free_list_[i];
        const std::uint32_t aligned = static_cast<std::uint32_t>(
            (range.offset + alignment - 1) / alignment * alignment);
        const std::uint32_t padding = aligned - range.offset;
        if (range.count < padding + count) continue;
        const DescriptorSlice slice{aligned, count};
        const std::uint32_t tail = range.count - padding - count;
        const std::uint32_t front = padding;
        if (front == 0) {
            if (tail > 0) {
                range.offset = aligned + count;
                range.count = tail;
            } else {
                free_list_.erase(free_list_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        } else {
            // `free_list_` stays sorted: front sits at `range.offset` and tail
            // (if any) immediately follows it at `aligned + count`.
            range.count = front;
            if (tail > 0) {
                free_list_.insert(free_list_.begin() + static_cast<std::ptrdiff_t>(i + 1),
                                  Range{aligned + count, tail});
            }
        }
        free_ -= count;
        high_water_ = std::max(high_water_, used());
        return slice;
    }
    return std::nullopt;
}

void DescriptorRing::InsertFree(Range range) {
    if (range.count == 0) return;
    auto it = std::lower_bound(free_list_.begin(), free_list_.end(), range,
                               [](const Range& a, const Range& b) { return a.offset < b.offset; });
    it = free_list_.insert(it, range);
    // Coalesce with the next, then with the previous.
    auto next = it + 1;
    if (next != free_list_.end() && it->offset + it->count == next->offset) {
        it->count += next->count;
        free_list_.erase(next);
    }
    if (it != free_list_.begin()) {
        auto prev = it - 1;
        if (prev->offset + prev->count == it->offset) {
            prev->count += it->count;
            free_list_.erase(it);
        }
    }
}

void DescriptorRing::Release(DescriptorSlice slice, std::uint64_t fence_value) {
    if (!slice.valid()) return;
    pending_.push_back(Pending{slice, fence_value});
}

std::uint32_t DescriptorRing::Collect(std::uint64_t fence_value) {
    if (pending_.empty()) return 0;
    std::uint32_t collected = 0;
    std::size_t write = 0;
    for (std::size_t read = 0; read < pending_.size(); ++read) {
        const Pending p = pending_[read];
        if (p.fence <= fence_value) {
            InsertFree(Range{p.slice.offset, p.slice.count});
            free_ += p.slice.count;
            ++collected;
        } else {
            if (write != read) pending_[write] = p;
            ++write;
        }
    }
    pending_.resize(write);
    return collected;
}

std::uint64_t DescriptorRing::OldestPendingFence() const {
    if (pending_.empty()) return 0;
    std::uint64_t oldest = pending_[0].fence;
    for (std::size_t i = 1; i < pending_.size(); ++i) {
        if (pending_[i].fence < oldest) oldest = pending_[i].fence;
    }
    return oldest;
}

std::optional<DescriptorSlice> DescriptorRing::ReservePermanent(std::uint32_t count) {
    return Allocate(count, 1);
}

void DescriptorRing::Reset() {
    free_list_.clear();
    pending_.clear();
    free_list_.push_back(Range{0, capacity_});
    free_ = capacity_;
    high_water_ = 0;
}

}  // namespace bbx::d3d12
