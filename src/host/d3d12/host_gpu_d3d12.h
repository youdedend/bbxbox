// SPDX-License-Identifier: GPL-3.0-or-later
// The seam between the guest's GPU (host/gpu.h) and the console's device.
//
// `host_gpu_d3d12.cpp` implements host/gpu.h over the port's D3d12Session, and
// this header is the whole of what it needs from it: a timeline fence, the
// guest's memory, and - from Slice 5B-2 - the dispatch. The session installs a
// binding at Create and clears it at Destroy (`SetGuestGpuBinding(nullptr)`), so
// a boot without a device still runs: every call answers as upstream's stub does.
//
// Why a binding rather than a link: host/gpu.h is called from the guest's HLE
// (hle/gnm_exec.cpp, hle/video.cpp), which is inside `bbhost_guest`, and the
// device is the app's. A function-pointer record the app fills is the smallest
// thing that crosses that line without either side including the other's headers,
// and it keeps the two libraries switchable in the boot report.
//
// Slice 5B-1 (this slice) provides `fence_*`, `mem_*` and `read_guest`, plus
// `dispatch_key` - the shader identity the prebake's cache is keyed by. Slice
// 5B-2 adds `dispatch`, which is where the compute PSO, the StageParams CBV and
// the space-3 tables are bound; until it exists a dispatch that finds its blob is
// counted as `no-pipeline` and refused, with one line in the log per key.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "host/gpu.h"

namespace bbx::d3d12 {

struct DecodedClearView;
struct DrawIdentity;

// Why a dispatch did not run. One per key, in the log, and counted.
enum class GuestDispatchRefusal : std::uint8_t {
    kNone = 0,
    kNoDevice,     // no binding installed: a boot that never created a device
    kBadProgram,   // the dispatch's COMPUTE_PGM_LO/HI do not read as a program
    kCacheMiss,    // no blob in the baked cache for that key (or no dispatch hook)
    kNoPipeline,   // the binding could not build the compute pipeline
};

struct GuestGpuCounts {
    std::uint64_t dispatches = 0;
    std::uint64_t dispatched = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t no_pipeline = 0;
    std::uint64_t no_device = 0;
    std::uint64_t bad_program = 0;
    std::uint64_t draws = 0;
    std::uint64_t draws_executed = 0;
    std::uint64_t draw_refusals = 0;
    std::uint64_t draw_pipeline_missing = 0;
    std::uint64_t clears = 0;
    std::uint64_t target_copies = 0;
    std::uint64_t texture_writes = 0;
    std::uint64_t mem_writes = 0, mem_fills = 0, mem_copies = 0, mem_reads = 0, mem_refused = 0;
    std::uint64_t refused_keys = 0;
};

// Everything the guest's GPU asks of the console's device. `user` is the owner's
// own pointer (the session's Impl); every hook may be null, and a null hook is
// the stub's answer rather than a crash.
struct GuestGpuBinding {
    bool ready = false;
    void* user = nullptr;

    // The timeline fence (Device::Signal / Device::SubmittedValue /
    // Device::CompletedValue / Device::WaitFor). `fence_signal` submits and
    // returns the value; `fence_wait` blocks until `value` is complete or the
    // timeout passes.
    std::uint64_t (*fence_submitted)(void* user) = nullptr;
    std::uint64_t (*fence_completed)(void* user) = nullptr;
    std::uint64_t (*fence_signal)(void* user) = nullptr;
    bool (*fence_wait)(void* user, std::uint64_t value, std::uint64_t timeout_ms) = nullptr;

    // The guest's memory (bbx_guest_mem.h's GuestMemory, over the plan's slabs).
    bool (*mem_write)(void* user, std::uint64_t va, const void* data, std::size_t bytes) = nullptr;
    bool (*mem_fill)(void* user, std::uint64_t va, std::uint32_t value, std::size_t bytes) = nullptr;
    bool (*mem_copy)(void* user, std::uint64_t dst_va, std::uint64_t src_va, std::size_t bytes) = nullptr;
    bool (*read_guest)(void* user, std::uint64_t va, void* out, std::size_t bytes) = nullptr;

    // The program's key in the baked cache, from the dispatch's own registers
    // (the words at COMPUTE_PGM_LO/HI and rsrc1/rsrc2/threads, as the bake's
    // ConsoleStageKeyOf takes them). 0: the program could not be read.
    std::uint64_t (*dispatch_key)(void* user, const GpuDispatch& d) = nullptr;
    // Slice 5B-2: build or find the compute PSO for `key`, bind the StageParams
    // CBV and the space-3 tables, and issue the dispatch (Direct or Indirect).
    // False: counted as `no-pipeline` and refused.
    bool (*dispatch)(void* user, std::uint64_t key, const GpuDispatch& d) = nullptr;

    // Called when the guest creates a shader from a Sony Shdr container (stage 0
    // = VS, 1 = PS), allowing the graphics pipeline to compute and cache the
    // exact bake-identical ConsoleStageKeyOf hash before the first draw.
    void (*note_shader_created)(void* user, int stage, const std::uint8_t* container, std::size_t size) = nullptr;
    // Slice 5B-2b: resolve cached vs_6_4/ps_6_4 DXIL blobs, create or look up the
    // graphics PSO for (vs_key, ps_key, fixed state), bind StageParams/space-1/
    // space-3 tables and render/depth targets, and record the draw.
    bool (*draw)(void* user, const GpuDraw& d, const DrawIdentity& identity) = nullptr;

    // Render/depth target clear, target-to-target copy, and memory invalidation hooks.
    bool (*clear_view)(void* user, const DecodedClearView& view, bool apply, bool* out_created) = nullptr;
    bool (*clear_target)(void* user, std::uint64_t va, std::uint32_t value, std::size_t bytes) = nullptr;
    bool (*copy_target)(void* user, std::uint64_t dst_va, std::uint64_t src_va, std::size_t bytes) = nullptr;
    void (*forget_region)(void* user, std::uint64_t lo, std::uint64_t hi) = nullptr;
    void (*texture_written)(void* user, std::uint64_t va, std::size_t bytes) = nullptr;

    // Device memory & identity reporting hooks.
    bool (*memory_glance)(void* user, GpuMemoryGlance* out) = nullptr;
    std::string (*memory_budget_report)(void* user) = nullptr;
    std::string (*device_identity)(void* user) = nullptr;
};

// Installed by the session: `binding` must outlive the boot (the session's own
// storage), and nullptr clears it.
void SetGuestGpuBinding(const GuestGpuBinding* binding);
// The binding in force, or nullptr.
[[nodiscard]] const GuestGpuBinding* GuestGpu();
// What the guest has asked of the GPU since the boot.
[[nodiscard]] GuestGpuCounts GuestGpuStats();
// "guest gpu: 42 dispatch(es) (12 dispatched, 30 cache miss ...)".
[[nodiscard]] std::string GuestGpuReport();
// The same, with one line per key that was refused and why.
[[nodiscard]] std::string GuestDispatchReport();

}  // namespace bbx::d3d12
