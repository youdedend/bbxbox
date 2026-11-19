// SPDX-License-Identifier: GPL-3.0-or-later
// Native Xbox D3D12 graphics draw executor (Slice 5B-2b): runs Bloodborne's
// baked vs_6_4 and ps_6_4 DXIL shaders directly on the console's D3D12 device.
//
// Shares the validated shader cache (`bbx_shaders.cache`), space-1 binding
// table (`bbx_shaders.paths`), and GPU SRV snapshot ring owned by GuestCompute
// so the console loads each artifact once per boot.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "host/gpu.h"
#include "src/host/d3d12/bbx_d3d12.h"
#include "src/host/d3d12/bbx_guest_draw.h"
#include "src/host/d3d12/bbx_guest_mem.h"

namespace bbx::d3d12 {
class DescriptorRing;
class Device;
class PipelineSet;
}  // namespace bbx::d3d12

namespace bbx::platform {

class GuestCompute;

class GuestGraphics {
public:
    struct Counts {
        std::uint64_t drawn = 0;            // graphics draws recorded and submitted
        std::uint64_t batches = 0;          // batched graphics command lists submitted
        std::uint64_t indexed = 0;          // of them, indexed draws
        std::uint64_t indirect = 0;         // of them, ExecuteIndirect draws
        std::uint64_t shaders_noted = 0;    // Shdr containers analysed via host_gpu_note_shader_created
        std::uint64_t miss_vs = 0;          // VS key not found in baked cache
        std::uint64_t miss_ps = 0;          // PS key not found in baked cache
        std::uint64_t no_cache = 0;         // baked cache not valid
        std::uint64_t bad_program = 0;      // VS or PS code VA unreadable
        std::uint64_t bad_blob = 0;         // zlib inflate failed for VS or PS DXIL
        std::uint64_t bad_pso = 0;          // CreateGraphicsPipelineState failed
        std::uint64_t bad_topology = 0;     // unsupported VGT_PRIMITIVE_TYPE
        std::uint64_t bad_target = 0;       // render target or depth texture could not be allocated
        std::uint64_t bad_index = 0;        // indexed/indirect buffer could not be resolved
        std::uint64_t created_psos = 0;     // graphics PSOs created
        std::uint64_t evicted_psos = 0;     // graphics PSOs retired by budget LRU
        std::uint64_t targets_created = 0;  // native 2D RTV/DSV textures allocated
        std::uint64_t record_failed = 0;    // command list allocation/close failure
        std::uint64_t bindings_written = 0; // space-1 u#/t# slots written across VS + PS
        std::uint64_t bindings_srv = 0;     // read-only t# slots bound via SRV snapshot ring
        std::uint64_t bindings_refused = 0; // space-1 plans refused (slot stays null)
        std::uint64_t bindings_null = 0;    // space-1 plans with null V#
        std::uint64_t target_clears = 0;    // ClearRenderTargetView / ClearDepthStencilView executed
        std::uint64_t target_copies = 0;    // render-target CopyResource executed
    };

    GuestGraphics();
    ~GuestGraphics();
    GuestGraphics(const GuestGraphics&) = delete;
    GuestGraphics& operator=(const GuestGraphics&) = delete;

    bbx::d3d12::Status Create(bbx::d3d12::Device& device, bbx::d3d12::PipelineSet& pipelines,
                              bbx::d3d12::GuestMemory& memory, GuestCompute& compute,
                              bbx::d3d12::DescriptorRing* descriptor_ring,
                              std::uint32_t arena_base, std::uint32_t sampler_base);
    void Destroy();
    [[nodiscard]] bool Ready() const;

    // host_gpu_note_shader_created hook: parses a Sony Shdr container (stage 0
    // = VS, 1 = PS), applies shader_patch_apply, and computes the exact
    // bake-identical ConsoleStageKeyOf hash before the first draw.
    void NoteShaderCreated(int stage, const std::uint8_t* container, std::size_t size);

    // host_gpu_draw hook: resolves cached vs_6_4 and ps_6_4 DXIL blobs, builds
    // or retrieves the Guard-G4-compliant graphics PSO, binds StageParams,
    // space-1/space-3 descriptor tables, native 2D RTV/DSV targets, viewport
    // and scissor, and records the draw into the active graphics batch.
    bool Draw(const GpuDraw& d, const bbx::d3d12::DrawIdentity& identity);

    // host_gpu_clear_view hook: clears or creates+clears a color RTV or depth DSV
    // on the D3D12 graphics queue in stream order.
    bool ClearView(const bbx::d3d12::DecodedClearView& view, bool apply, bool* out_created = nullptr);

    // host_gpu_clear_target hook: clears any cached RTV/DSV overlapping [va, va+bytes).
    bool ClearTargetAt(std::uint64_t va, std::uint32_t value, std::size_t bytes);

    // host_gpu_copy_guest target-to-target fast path: copies a cached RTV/DSV
    // at `src_va` into a matching RTV/DSV at `dst_va` via D3D12 CopyResource.
    bool CopyTarget(std::uint64_t dst_va, std::uint64_t src_va, std::size_t bytes);

    // host_gpu_forget_region / host_gpu_texture_retire hook: retires any cached
    // RTV/DSV overlapping [lo, hi) with fence-deferred memory reclamation.
    void ForgetRegion(std::uint64_t lo, std::uint64_t hi);

    // Invalidates cached shader program extractions overlapping [lo, hi) when
    // guest memory is rewritten.
    void InvalidatePrograms(std::uint64_t lo, std::uint64_t hi);

    // Flushes any batched graphics commands (draws/clears/copies) to the GPU queue.
    void Flush();
    [[nodiscard]] bool HasPending() const;

    // Zero-copy direct GPU scanout lookup: checks whether `va` corresponds to a
    // GPU-rendered color target of matching dimensions and SRV-compatible format.
    [[nodiscard]] bool HasRenderedTarget(std::uint64_t va, std::uint32_t width, std::uint32_t height) const;
    bool TryAcquirePresentTarget(std::uint64_t va, std::uint32_t width, std::uint32_t height,
                                 bbx::d3d12::ComPtr<ID3D12Resource>* out_resource, DXGI_FORMAT* out_format) const;

    [[nodiscard]] std::string Report() const;
    // Retires evicted PSOs, evicted targets, and completed command allocators.
    void OnFrame(std::uint64_t completed);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bbx::platform
