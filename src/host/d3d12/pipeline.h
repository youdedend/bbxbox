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
// The guest's memory and the signature its shaders name: portable headers, so
// the build machine tests the numbers this file turns into D3D12 descriptors
// (bbx_guest_mem.h, bbx_guest_root.h).
#include "src/host/d3d12/bbx_guest_root.h"

namespace bbx::d3d12 {

class Device;

// What a guest shader's signature costs, for the boot report: "guest root
// (compute): 4 parameter(s), 5/64 DWORDs, ..." (bbx_guest_root.cpp's Describe).
[[nodiscard]] std::string GuestRootDescription(GuestStage stage = GuestStage::kCompute);

// Builds a D3D12 signature out of a GuestRootLayout: the root CBV, the stage's
// space-1 tables (one per stage for graphics: two stages' t0 are two resources)
// and the space-3 page table plus slabs. A free function so a test (or the
// report) can ask a device for one without a PipelineSet; PipelineSet::Create
// calls it and keeps the result.
//
// The signature is console 1.0 and states its own rules by construction: no
// table mixes a sampler with a CBV/SRV/UAV range (D3D12 refuses to serialize
// that), no static samplers (a guest sampler carries an aniso/border combination
// per binding), and no pipeline library.
Status CreateGuestRootSignature(Device& device, GuestStage stage, ID3D12RootSignature** out);

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
    // The guest shaders' signatures (bbx_guest_root.h): created with the port's
    // own, once each. A compute dispatch uses GuestRootSignature(kCompute), a
    // vertex/pixel draw kGraphics (which carries both stages' tables, because a
    // root parameter has one visibility). Null when the device refused one,
    // which the boot report says rather than failing a dispatch later.
    [[nodiscard]] ID3D12RootSignature* GuestRootSignature(GuestStage stage) const {
        return guest_roots_[static_cast<std::size_t>(stage)].Get();
    }
    [[nodiscard]] bool GuestRootReady(GuestStage stage) const {
        return guest_roots_[static_cast<std::size_t>(stage)] != nullptr;
    }

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
    // The guest's: one signature per stage kind for every baked shader
    // (bbx_guest_root.h). kCompute and kGraphics.
    std::array<ComPtr<ID3D12RootSignature>, 2> guest_roots_{};
    std::array<ComPtr<ID3D12PipelineState>, static_cast<std::size_t>(PassKind::kCount)> psos_{};
    std::unique_ptr<PsoCache> cache_;
    std::uint64_t clock_ = 0;
};

}  // namespace bbx::d3d12
