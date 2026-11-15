// SPDX-License-Identifier: GPL-3.0-or-later
// The 5,120 MiB plan: owners, caps, residency tiers and the pager's
// thresholds.
//
// This is the one place that decides whether the port fits. Every allocation
// asks it first; the answer is "take it", "take it after evicting these", or
// "no, and here is what to degrade". Nothing in the backend allocates outside
// an owner, which is what makes the budget line in loader.log.txt add up.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bbx::d3d12 {

// Xbox Series X|S Game mode.
inline constexpr std::uint64_t kGameModeLimit = 5'120ull << 20;

enum class Owner : std::uint8_t {
    kGuest = 0,      // the game's memory, commit-on-touch, pageable
    kGpuShadows,     // buffer twins, GPU-visible textures
    kHeaps,          // render targets, depth, staging/upload/readback
    kShaderTools,    // DXC arena, DXIL cache, PSO library, manifests
    kMedia,          // audio voices, movie decode, Scaleform, fonts
    kHost,           // image, CRT, WinRT, thread stacks
    kIo,             // file cache, overlay index, metadata
    kReserve,        // never handed out
    kCount
};

[[nodiscard]] const char* OwnerName(Owner owner);

struct OwnerBudget {
    Owner owner;
    std::uint64_t cap;
    std::uint64_t used;
};

enum class ResidencyLevel : std::uint8_t { kNormal = 0, kElevated, kHigh, kCritical };

[[nodiscard]] const char* ResidencyName(ResidencyLevel level);

// The pager's hysteresis, derived from the *allocatable* budget (the limit
// minus the reserve) rather than hard-coded, so a future limit keeps working.
struct PagerPolicy {
    std::uint64_t start = kGameModeLimit - (640ull << 20) - (384ull << 20);
    std::uint64_t stop = kGameModeLimit - (640ull << 20) - (512ull << 20);
};

// What the four residency tiers do to the descriptor-table cache and the
// resolution controller. These numbers are the ones the prior art measured on
// Series S against the same ceiling.
struct PressurePolicy {
    std::uint32_t table_keep_normal = 4096;
    std::uint32_t table_keep_elevated = 2048;
    std::uint32_t table_keep_high = 512;
    std::uint32_t table_keep_critical = 64;
    std::uint32_t table_max_age_normal = 600;
    std::uint32_t table_max_age_elevated = 120;
    std::uint32_t table_max_age_high = 30;
    bool trim_dxil_cache = false;   // High and above
    bool trim_dxgi = false;         // Critical
    bool drop_resolution_step = false;  // High and above
};

struct AllocationResult {
    bool ok = false;
    bool evicted = false;
    std::string reason;  // filled on failure
};

// The plan and its live accounting. Single-threaded by design: the backend
// owns one and calls it from the frame thread, so no atomics and no surprises.
class Budget {
public:
    Budget();
    explicit Budget(std::uint64_t limit);

    [[nodiscard]] std::uint64_t limit() const { return limit_; }
    [[nodiscard]] std::uint64_t used() const { return used_; }
    [[nodiscard]] std::uint64_t cap(Owner owner) const;
    [[nodiscard]] std::uint64_t used_by(Owner owner) const;
    // What the owners may actually use: the limit minus the reserve, which is
    // never handed out.
    [[nodiscard]] std::uint64_t allocatable() const { return limit_ - cap(Owner::kReserve); }
    [[nodiscard]] std::uint64_t headroom() const {
        return allocatable() > used_ ? allocatable() - used_ : 0;
    }
    [[nodiscard]] ResidencyLevel Level() const;

    // Reserves `bytes` against `owner`. When the cap would be crossed the call
    // fails with a reason; it never takes from another owner, because a
    // texture allocation eating the guest's memory is how a port dies.
    AllocationResult Allocate(Owner owner, std::uint64_t bytes);
    void Free(Owner owner, std::uint64_t bytes);

    // The pager's decision for this frame, with hysteresis: once paging starts
    // it continues until `stop`.
    // Mutates the hysteresis state, hence not const.
    [[nodiscard]] bool ShouldPage();
    void NotePagedOut(std::uint64_t bytes);
    void NotePagedIn(std::uint64_t bytes);

    // The precompiler asks this before each compile: never push the process
    // into the ceiling for a shader.
    [[nodiscard]] bool CanCompile(std::uint64_t needed = 0) const;

    [[nodiscard]] PressurePolicy PolicyFor(ResidencyLevel level) const;

    // The one-line log form the acceptance runs read.
    [[nodiscard]] std::string Describe() const;

    // The compiled-in plan, for tests and for the documentation's table.
    [[nodiscard]] static const std::vector<OwnerBudget>& Plan();

private:
    std::uint64_t limit_ = kGameModeLimit;
    std::vector<OwnerBudget> owners_;
    std::uint64_t used_ = 0;
    bool paging_ = false;
    bool paging_armed_ = true;
    std::uint64_t paged_out_ = 0;
    std::uint64_t paged_in_ = 0;
};

}  // namespace bbx::d3d12
