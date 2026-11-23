// SPDX-License-Identifier: GPL-3.0-or-later
// The guest's compute dispatches on the console's device: the pipeline half of
// host_gpu_d3d12.h (Slice 5B-2a). Owned by D3d12Session, which hands this
// object's two entry points to the guest as the binding's `dispatch_key` and
// `dispatch` hooks.
//
// Split from d3d12_session.cpp because it is the only part of the port that
// reads the guest's program memory (`hle_kernel_va_mapped`) and runs the
// translator to build a bake-identical key, and neither belongs in the file that
// also owns the swapchain.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "host/gpu.h"
#include "src/host/d3d12/bbx_guest_mem.h"
#include "src/host/d3d12/bbx_d3d12.h"

namespace bbx::d3d12 {
class DescriptorRing;
class Device;
class PathTable;
class PipelineSet;
class ShaderCache;
}  // namespace bbx::d3d12

namespace bbx::platform {

class GuestCompute {
public:
    // What this object did, and why it did not: the report's numbers.
    struct Counts {
        std::uint64_t ran = 0;            // dispatches recorded and submitted
        std::uint64_t batches = 0;        // batched command lists submitted
        std::uint64_t indirect = 0;       // of them, ExecuteIndirect
        std::uint64_t miss = 0;           // no blob in the baked cache for the key
        std::uint64_t no_cache = 0;       // the cache did not validate: nothing can run
        std::uint64_t bad_program = 0;    // the program at COMPUTE_PGM_LO cannot be read
        std::uint64_t bad_blob = 0;       // the cached blob would not inflate
        std::uint64_t bad_pso = 0;        // the device refused the compute PSO
        std::uint64_t evicted = 0;        // PSOs released by the cache's budget
        std::uint64_t created = 0;        // PSOs created on demand
        std::uint64_t no_indirect = 0;    // an indirect dispatch with args that cannot be reached
        std::uint64_t record_failed = 0;  // a command list that would not record
        std::uint64_t uav_barriers = 0;   // one after every dispatch
        std::uint64_t bindings_written = 0;   // space-1 slots written from the bake's plans
        std::uint64_t bindings_srv = 0;       // read-only t# slots bound via SRV snapshot ring
        std::uint64_t bindings_refused = 0;   // plans this pass would not bind (the slot stays null)
        std::uint64_t bindings_null = 0;      // plans that named a null V#: the null descriptor is right
        std::uint64_t keys_without_plans = 0; // keys that read a table the bake assigned nothing to
    };

    GuestCompute();
    ~GuestCompute();
    GuestCompute(const GuestCompute&) = delete;
    GuestCompute& operator=(const GuestCompute&) = delete;

    // `arena_base` is the guest arena's first CBV/SRV/UAV descriptor in the
    // device's shader-visible heap (bbx_guest_root.h's GuestHeapLayout), and
    // `sampler_base` the sampler heap's. `descriptor_ring` owns the remaining
    // dynamic region of the shader-visible CBV/SRV/UAV heap so per-dispatch and
    // per-draw space-1 descriptor tables never stall the GPU.
    bbx::d3d12::Status Create(bbx::d3d12::Device& device, bbx::d3d12::PipelineSet& pipelines,
                              bbx::d3d12::GuestMemory& memory, bbx::d3d12::DescriptorRing* descriptor_ring,
                              std::uint32_t arena_base, std::uint32_t sampler_base);
    void Destroy();
    [[nodiscard]] bool Ready() const;

    // host_gpu_d3d12.h's `dispatch_key`: the key this dispatch's program was
    // baked under, or 0 when it cannot be read (the caller counts and refuses).
    std::uint64_t KeyFor(const GpuDispatch& d);
    // host_gpu_d3d12.h's `dispatch`: build or find the compute PSO for `key`,
    // bind the root CBV and the tables, and record into the active batch.
    bool Run(std::uint64_t key, const GpuDispatch& d);

    // Flushes any batched compute dispatches to the GPU queue.
    void Flush();
    [[nodiscard]] bool HasPending() const;
    void InvalidatePrograms(std::uint64_t lo, std::uint64_t hi);

    // Shared baked cache, binding table, null descriptor template, and GPU-side
    // SRV snapshot ring used by GuestGraphics so the console loads
    // bbx_shaders.cache and bbx_shaders.paths once instead of twice.
    void EnsureCacheLoaded();
    [[nodiscard]] const bbx::d3d12::ShaderCache& Cache() const;
    [[nodiscard]] const bbx::d3d12::PathTable& Paths() const;
    [[nodiscard]] bool CacheReady() const;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE NullTemplateCpu() const;
    [[nodiscard]] ID3D12Resource* SrvSnapshotRing() const;
    [[nodiscard]] std::uint64_t SrvRingGeneration() const;
    using FlushCallback = void (*)(void*);
    bool ReserveSrvSnapshot(std::uint64_t bytes, std::uint64_t* out_offset,
                            FlushCallback flush_cb = nullptr, void* flush_user = nullptr);
    void TouchSrvSnapshotOffset(std::uint64_t offset);
    void MarkSrvSnapshotsSubmitted(std::uint64_t fence);

    // The cache's one line and the counts, for the boot/exit report.
    [[nodiscard]] std::string Report() const;
    // Retires PSOs and in-flight command allocators whose fence has completed.
    void OnFrame(std::uint64_t completed);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bbx::platform
