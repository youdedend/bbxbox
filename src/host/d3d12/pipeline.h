// SPDX-License-Identifier: GPL-3.0-or-later
// The port's own pipelines: a root signature and a PSO per pass, created once.
//
// Two rules from the hardware notes shape this file:
//
//  * Root signatures are explicit, not `D3D12SerializeVersionedRootSignature` with
//    a versioned desc: the console's root signature is 1.0, and a descriptor table
//    per kind in its own space is what `bbx_descriptors` hands out.
//  * No pipeline library. `CreatePipelineLibrary` removes the device on a console,
//    so a PSO is created with `CreateGraphicsPipelineState`/`CreateComputePipelineState`
//    and its cost is amortised by `bbx_psocache` (LRU + object cap + byte budget),
//    never by serialising a library. An evicted PSO is released only after the
//    fence value that was current when it was last used has passed.
//
// The DXIL comes from `bbx_shaders.h`, i.e. from the build machine
// (`dxc -T *_6_4 -WX`). Nothing here compiles a shader.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "src/host/d3d12/bbx_common.h"
#include "src/host/d3d12/bbx_d3d12.h"
#include "src/host/d3d12/bbx_psocache.h"
#include "src/host/d3d12/bbx_shaders.h"

namespace bbx::d3d12 {

class Device;

// The port's own passes. `kCount` last; the guest's draws (P3) go through the
// cache with keys of their own, not through this enum.
enum class PassKind : std::uint8_t {
    kPresent = 0,   // fullscreen triangle sampling the frame's final image
    kOverlay,       // glyph atlas and solid quads: telemetry, launcher, boot report
    kUntile,        // compute: GCN micro tiles -> linear
    kSelftest,      // compute: the boot GPU self-test
    kCount,
};

[[nodiscard]] const char* PassName(PassKind kind);

class PipelineSet {
public:
    PipelineSet() = default;
    ~PipelineSet();
    PipelineSet(const PipelineSet&) = delete;
    PipelineSet& operator=(const PipelineSet&) = delete;

    // Root signatures only; PSOs are created on first use (`Ensure`), so a boot
    // that never presents does not pay for a PSO it will not draw with.
    Status Create(Device& device);
    void Destroy();

    // The PSO for `kind`, created if this is the first use. Null on failure; the
    // reason is logged once and the pass is skipped rather than crashing.
    [[nodiscard]] ID3D12PipelineState* Ensure(PassKind kind);
    [[nodiscard]] ID3D12RootSignature* RootSignature(PassKind kind) const {
        return kind < PassKind::kCount ? roots_[static_cast<std::size_t>(kind)].Get() : nullptr;
    }
    [[nodiscard]] bool Ready(PassKind kind) const {
        return kind < PassKind::kCount && psos_[static_cast<std::size_t>(kind)] != nullptr;
    }

    // PT_SRVs/Samplers per pass, for the caller that fills the descriptor tables.
    [[nodiscard]] static std::uint32_t RootConstants(PassKind kind);

    [[nodiscard]] const PsoStats& Stats() const { return cache_->Stats(); }
    [[nodiscard]] std::string Describe() const;

    // Retires evicted PSOs whose last use is older than `completed`. Called once
    // per frame with the fence's completed value.
    void Collect(std::uint64_t completed);

private:
    Status CreateRootSignature(PassKind kind);
    Status CreateGraphicsPso(PassKind kind, const shaders::Blob& vs, const shaders::Blob& ps);
    Status CreateComputePso(PassKind kind, const shaders::Blob& cs);

    Device* device_ = nullptr;
    std::array<ComPtr<ID3D12RootSignature>, static_cast<std::size_t>(PassKind::kCount)> roots_{};
    std::array<ComPtr<ID3D12PipelineState>, static_cast<std::size_t>(PassKind::kCount)> psos_{};
    std::unique_ptr<PsoCache> cache_;
    std::uint64_t clock_ = 0;
};

}  // namespace bbx::d3d12
