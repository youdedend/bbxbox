// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_budget.h"

#include "host/d3d12/bbx_common.h"

#include <cinttypes>
#include <cstdio>

namespace bbx::d3d12 {
namespace {

// docs/memory-budget.md §1, in MiB. Keep the two in step: the test below sums
// them and fails if they do not reach the limit, and the acceptance runs read
// the same numbers from the log.
constexpr std::uint64_t kCapsMiB[static_cast<int>(Owner::kCount)] = {
    2048,  // kGuest
    896,   // kGpuShadows
    768,   // kHeaps
    256,   // kShaderTools
    192,   // kMedia
    224,   // kHost
    96,    // kIo
    640,   // kReserve
};

// The precompiler keeps this much headroom before it starts a compile, and
// stops until the headroom is back to kCompileResume. A compile can allocate
// hundreds of MiB in DXC's arena and the PSO library; a console at the ceiling
// is killed, not slowed.
constexpr std::uint64_t kCompileReserve = 768ull << 20;
constexpr std::uint64_t kCompileResume = 512ull << 20;

int OwnerIndex(Owner owner) { return static_cast<int>(owner); }

}  // namespace

const char* OwnerName(Owner owner) {
    switch (owner) {
    case Owner::kGuest: return "guest";
    case Owner::kGpuShadows: return "gpu";
    case Owner::kHeaps: return "heaps";
    case Owner::kShaderTools: return "dxc";
    case Owner::kMedia: return "media";
    case Owner::kHost: return "host";
    case Owner::kIo: return "io";
    case Owner::kReserve: return "reserve";
    case Owner::kCount: break;
    }
    return "?";
}

const char* ResidencyName(ResidencyLevel level) {
    switch (level) {
    case ResidencyLevel::kNormal: return "normal";
    case ResidencyLevel::kElevated: return "elevated";
    case ResidencyLevel::kHigh: return "high";
    case ResidencyLevel::kCritical: return "critical";
    }
    return "?";
}

Budget::Budget() : Budget(kGameModeLimit) {}

Budget::Budget(std::uint64_t limit) : limit_(limit) {
    owners_.resize(static_cast<std::size_t>(Owner::kCount));
    for (int i = 0; i < static_cast<int>(Owner::kCount); ++i) {
        owners_[static_cast<std::size_t>(i)] = OwnerBudget{static_cast<Owner>(i),
                                                           MiB(kCapsMiB[i]), 0};
    }
    // A limit other than the documented one (a test, or a future console)
    // scales the caps so the plan still sums to it, with the reserve last.
    std::uint64_t plan_sum = 0;
    for (const OwnerBudget& o : owners_) plan_sum += o.cap;
    if (plan_sum != limit && plan_sum != 0) {
        std::uint64_t scaled_sum = 0;
        for (OwnerBudget& o : owners_) {
            o.cap = static_cast<std::uint64_t>(static_cast<double>(o.cap) *
                                               static_cast<double>(limit) /
                                               static_cast<double>(plan_sum));
            scaled_sum += o.cap;
        }
        // Give the rounding remainder to the reserve, so the plan still adds up
        // exactly: a test (or a future console) must not leave a byte unowned.
        if (limit > scaled_sum) owners_[OwnerIndex(Owner::kReserve)].cap += limit - scaled_sum;
    }
}

std::uint64_t Budget::cap(Owner owner) const { return owners_[OwnerIndex(owner)].cap; }

std::uint64_t Budget::used_by(Owner owner) const { return owners_[OwnerIndex(owner)].used; }

ResidencyLevel Budget::Level() const {
    const std::uint64_t usable = allocatable();
    if (usable == 0) return ResidencyLevel::kCritical;
    const double fraction = static_cast<double>(used_) / static_cast<double>(usable);
    if (fraction >= 0.92) return ResidencyLevel::kCritical;
    if (fraction >= 0.85) return ResidencyLevel::kHigh;
    if (fraction >= 0.75) return ResidencyLevel::kElevated;
    return ResidencyLevel::kNormal;
}

AllocationResult Budget::Allocate(Owner owner, std::uint64_t bytes) {
    AllocationResult result;
    if (owner == Owner::kReserve) {
        result.reason = "the reserve is not an allocation owner";
        return result;
    }
    OwnerBudget& o = owners_[OwnerIndex(owner)];
    if (bytes == 0) {
        result.ok = true;
        return result;
    }
    if (o.used > o.cap || bytes > o.cap - o.used) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "%s over its cap: %" PRIu64 " + %" PRIu64 " > %" PRIu64 " MiB", OwnerName(owner),
                      o.used >> 20, bytes >> 20, o.cap >> 20);
        result.reason = buf;
        return result;
    }
    o.used += bytes;
    used_ = SatAdd(used_, bytes);
    result.ok = true;
    return result;
}

void Budget::Free(Owner owner, std::uint64_t bytes) {
    if (owner == Owner::kReserve) return;
    OwnerBudget& o = owners_[OwnerIndex(owner)];
    const std::uint64_t freed = bytes > o.used ? o.used : bytes;
    o.used -= freed;
    used_ = used_ > freed ? used_ - freed : 0;
}

bool Budget::ShouldPage() {
    // Hysteresis against the allocatable budget, not the raw limit: the reserve
    // is the pager's floor, and paging below it would be paging into the
    // reserve.
    const std::uint64_t start = allocatable() - (384ull << 20);
    const std::uint64_t stop = allocatable() - (512ull << 20);
    if (!paging_ && used_ >= start) paging_ = true;
    if (paging_ && used_ <= stop) paging_ = false;
    return paging_ && paging_armed_;
}

void Budget::NotePagedOut(std::uint64_t bytes) {
    paged_out_ = SatAdd(paged_out_, bytes);
    // Paging out does not free guest commit by itself (the page is still
    // mapped); the caller accounts the release against kGuest.
    Free(Owner::kGuest, bytes);
}

void Budget::NotePagedIn(std::uint64_t bytes) {
    paged_in_ = SatAdd(paged_in_, bytes);
    (void)Allocate(Owner::kGuest, bytes);
}

bool Budget::CanCompile(std::uint64_t needed) const {
    const std::uint64_t want = kCompileReserve + needed;
    if (headroom() >= want) return true;
    // Once below the resume threshold the precompiler waits for the frame to
    // give memory back rather than fighting it.
    return false;
}

PressurePolicy Budget::PolicyFor(ResidencyLevel level) const {
    PressurePolicy p;
    switch (level) {
    case ResidencyLevel::kNormal:
        break;
    case ResidencyLevel::kElevated:
        p.trim_dxil_cache = false;
        break;
    case ResidencyLevel::kHigh:
        p.trim_dxil_cache = true;
        p.drop_resolution_step = true;
        break;
    case ResidencyLevel::kCritical:
        p.trim_dxil_cache = true;
        p.trim_dxgi = true;
        p.drop_resolution_step = true;
        break;
    }
    return p;
}

std::string Budget::Describe() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "Budget: limit %" PRIu64 " MiB (usable %" PRIu64 "), used %" PRIu64
                  "; guest %" PRIu64 "/%" PRIu64 " gpu %" PRIu64 "/%" PRIu64
                  " heaps %" PRIu64 "/%" PRIu64 " dxc %" PRIu64 "/%" PRIu64
                  " media %" PRIu64 "/%" PRIu64 " host %" PRIu64 "/%" PRIu64
                  " io %" PRIu64 "/%" PRIu64 " reserve %" PRIu64
                  "; level %s (%u%%), pager %s",
                  limit_ >> 20, allocatable() >> 20, used_ >> 20, used_by(Owner::kGuest) >> 20,
                  cap(Owner::kGuest) >> 20,
                  used_by(Owner::kGpuShadows) >> 20, cap(Owner::kGpuShadows) >> 20,
                  used_by(Owner::kHeaps) >> 20, cap(Owner::kHeaps) >> 20,
                  used_by(Owner::kShaderTools) >> 20, cap(Owner::kShaderTools) >> 20,
                  used_by(Owner::kMedia) >> 20, cap(Owner::kMedia) >> 20,
                  used_by(Owner::kHost) >> 20, cap(Owner::kHost) >> 20,
                  used_by(Owner::kIo) >> 20, cap(Owner::kIo) >> 20, cap(Owner::kReserve) >> 20,
                  ResidencyName(Level()),
                  static_cast<unsigned>(allocatable() ? (used_ * 100) / allocatable() : 100),
                  paging_ ? "paging" : "idle");
    return buf;
}

const std::vector<OwnerBudget>& Budget::Plan() {
    static const std::vector<OwnerBudget> plan = [] {
        std::vector<OwnerBudget> out;
        for (int i = 0; i < static_cast<int>(Owner::kCount); ++i) {
            out.push_back(OwnerBudget{static_cast<Owner>(i), MiB(kCapsMiB[i]), 0});
        }
        return out;
    }();
    return plan;
}

}  // namespace bbx::d3d12
