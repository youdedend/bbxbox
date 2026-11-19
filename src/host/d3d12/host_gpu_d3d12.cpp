// SPDX-License-Identifier: GPL-3.0-or-later
// The guest's GPU, on the console: `host/gpu.h` over the port's D3D12 device.
//
// `host/gpu_stub.cpp` is upstream's no-Vulkan build of this surface: every call
// answers "no GPU", so a boot runs, draws nothing and never waits on a fence.
// This file replaces it for the console. It is a *seam* over the device owner
// (the port's D3d12Session) rather than a second device: the session installs a
// binding at Create and clears it at Destroy, and everything here either forwards
// to it or counts why it could not.
//
// What is real (Slice 5B-1):
//
//   * the timeline fence - host_gpu_available, host_gpu_submit,
//     host_gpu_flush, host_gpu_submissions_completed, host_gpu_work_needs and
//     host_gpu_wait_completed all answer from the device's own fence, so
//     gnm_exec's EOP/label waits and video.cpp's g_flip_after_gpu track real
//     submissions instead of believing nothing was ever submitted;
//   * host_gpu_mem_write / _fill / _copy / host_gpu_read_guest, which go to the
//     guest's slabs through bbx_guest_mem (bbx::d3d12::GuestMemory), so a CPU
//     write lands where the shader's page table says it should;
//   * host_gpu_dispatch's shader identity: the program is read from guest
//     memory, its stage key computed exactly as the bake computes it
//     (ConsoleStageKeyOf), and the interface hash looked up in the baked cache.
//     A miss, or a shader the cache will not vouch for, is counted once by key
//     and refused - never dispatched as something the console cannot run.
//
// What is not (Slice 5B-2): the pipeline. A dispatch that *does* find its blob
// hands the resolved key, DXIL and GpuDispatch to the binding's `dispatch` hook,
// which is where the compute PSO (PsoCache), the StageParams CBV and the space-3
// tables are bound and `Dispatch`/`ExecuteIndirect` issued. Until that hook is
// installed the dispatch is counted as `kNoPipeline` and refused. `host_gpu_draw`
// decodes each draw's identity from the guest's own registers (5B-2b's graphics
// half, first step), logs it once per unique draw, and refuses a draw it cannot
// build for by name; recording the pipeline itself is the step that follows. The
// render-target surface (`host_gpu_clear_view`, the copy/blit paths) stays the
// stub's, counted, until then.
//
// Everything else in this file is `gpu_stub.cpp`'s answer, unchanged: the console
// has one device and it is the D3D12 session's, so the Vulkan-only reporting,
// captures and plugin surfaces have nothing to say.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "host/gpu.h"
#include "host/memory_policy.h"
#include "host/shader_overrides.h"
#include "src/host/d3d12/bbx_guest_draw.h"
#include "src/host/d3d12/bbx_guest_mem.h"
#include "src/host/d3d12/host_gpu_d3d12.h"

namespace bbx::d3d12 {
namespace {

std::mutex g_mu;
const GuestGpuBinding* g_bound = nullptr;

struct Counters {
    std::uint64_t dispatches = 0;
    std::uint64_t dispatched = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t no_pipeline = 0;
    std::uint64_t no_device = 0;
    std::uint64_t bad_program = 0;
    std::uint64_t draws = 0;
    std::uint64_t draws_identified = 0;       // a draw whose identity this port decoded
    std::uint64_t draws_executed = 0;         // ... and which the graphics pipeline submitted
    std::uint64_t draw_refusals = 0;          // ... and which it refused, by name
    std::uint64_t draw_pipeline_missing = 0;  // identified, but no graphics pipeline recorded it
    // Which draw identities have already put a line in the log (5B-2b): a level
    // makes the same draw thousands of times and the answer is one line.
    std::set<std::uint64_t> logged_draws;
    std::set<std::uint64_t> logged_draw_refusals;
    std::uint64_t clears = 0;
    std::uint64_t target_copies = 0;
    std::uint64_t texture_writes = 0;
    std::uint64_t mem_writes = 0, mem_fills = 0, mem_copies = 0, mem_reads = 0;
    std::uint64_t mem_refused = 0;
    GuestGpuCounts published;  // the snapshot host_gpu_d3d12.h hands out
    // Which keys have already put a line in the log, so a shader that misses is
    // reported once and a level that misses a thousand times does not flood it.
    std::map<std::uint64_t, GuestDispatchRefusal> refused;
};

std::mutex& mutex() { return g_mu; }
Counters& counters() {
    static Counters c;
    return c;
}

const char* RefusalName(GuestDispatchRefusal why) {
    switch (why) {
    case GuestDispatchRefusal::kNone: return "none";
    case GuestDispatchRefusal::kNoDevice: return "no-device";
    case GuestDispatchRefusal::kBadProgram: return "bad-program";
    case GuestDispatchRefusal::kCacheMiss: return "cache-miss";
    case GuestDispatchRefusal::kNoPipeline: return "no-pipeline";
    }
    return "?";
}

void Publish() {
    Counters& c = counters();
    c.published.dispatches = c.dispatches;
    c.published.dispatched = c.dispatched;
    c.published.cache_misses = c.cache_misses;
    c.published.no_pipeline = c.no_pipeline;
    c.published.no_device = c.no_device;
    c.published.bad_program = c.bad_program;
    c.published.draws = c.draws;
    c.published.draws_executed = c.draws_executed;
    c.published.draw_refusals = c.draw_refusals;
    c.published.draw_pipeline_missing = c.draw_pipeline_missing;
    c.published.clears = c.clears;
    c.published.target_copies = c.target_copies;
    c.published.texture_writes = c.texture_writes;
    c.published.mem_writes = c.mem_writes;
    c.published.mem_fills = c.mem_fills;
    c.published.mem_copies = c.mem_copies;
    c.published.mem_reads = c.mem_reads;
    c.published.mem_refused = c.mem_refused;
    c.published.refused_keys = static_cast<std::uint64_t>(c.refused.size());
}

// One line per key, at the first refusal: the boot's log carries why a shader
// never ran, and the count of how often it was asked for.
void Refuse(std::uint64_t key, GuestDispatchRefusal why, const std::string& detail) {
    Counters& c = counters();
    switch (why) {
    case GuestDispatchRefusal::kNoDevice: ++c.no_device; break;
    case GuestDispatchRefusal::kBadProgram: ++c.bad_program; break;
    case GuestDispatchRefusal::kCacheMiss: ++c.cache_misses; break;
    case GuestDispatchRefusal::kNoPipeline: ++c.no_pipeline; break;
    case GuestDispatchRefusal::kNone: break;
    }
    auto& entry = c.refused[key];
    if (entry == GuestDispatchRefusal::kNone) {
        entry = why;
        std::fprintf(stderr, "host_gpu: dispatch key %016llx refused: %s%s%s\n", static_cast<unsigned long long>(key),
                     RefusalName(why), detail.empty() ? "" : " (", detail.empty() ? "" : detail.c_str());
        if (!detail.empty()) std::fputs(")\n", stderr);
    }
    Publish();
}

ClearViewInput ToClearViewInput(const GpuClearView& c) {
    ClearViewInput in{};
    in.depth = c.depth;
    in.extent = c.extent;
    for (int i = 0; i < 5; ++i) in.color[i] = c.color[i];
    for (int i = 0; i < 4; ++i) in.rgba[i] = c.rgba[i];
    in.z_info = c.z_info;
    in.stencil_info = c.stencil_info;
    in.z_read_base = c.z_read_base;
    in.depth_size = c.depth_size;
    in.flags = c.flags;
    in.depth_value = c.depth_value;
    in.stencil = c.stencil;
    return in;
}

}  // namespace

void SetGuestGpuBinding(const GuestGpuBinding* binding) {
    std::lock_guard<std::mutex> lock(mutex());
    g_bound = (binding && binding->ready) ? binding : nullptr;
    Publish();
}

const GuestGpuBinding* GuestGpu() {
    std::lock_guard<std::mutex> lock(mutex());
    return g_bound;
}

GuestGpuCounts GuestGpuStats() {
    std::lock_guard<std::mutex> lock(mutex());
    return counters().published;
}

namespace {

std::string FormatGuestGpuReportLocked(const Counters& c) {
    char line[512];
    std::snprintf(line, sizeof(line),
                  "guest gpu: %llu dispatch(es) (%llu dispatched, %llu cache miss, %llu no pipeline, %llu no device, "
                  "%llu bad program), %llu draw(s) (%llu drawn, %llu refused, %llu no pipeline), "
                  "%llu clear(s), %llu copy(s), %llu upload(s) (%llu refused)",
                  static_cast<unsigned long long>(c.dispatches), static_cast<unsigned long long>(c.dispatched),
                  static_cast<unsigned long long>(c.cache_misses), static_cast<unsigned long long>(c.no_pipeline),
                  static_cast<unsigned long long>(c.no_device), static_cast<unsigned long long>(c.bad_program),
                  static_cast<unsigned long long>(c.draws), static_cast<unsigned long long>(c.draws_executed),
                  static_cast<unsigned long long>(c.draw_refusals),
                  static_cast<unsigned long long>(c.draw_pipeline_missing),
                  static_cast<unsigned long long>(c.clears),
                  static_cast<unsigned long long>(c.target_copies),
                  static_cast<unsigned long long>(c.mem_writes + c.mem_fills + c.mem_copies),
                  static_cast<unsigned long long>(c.mem_refused));
    return line;
}

}  // namespace

std::string GuestGpuReport() {
    std::lock_guard<std::mutex> lock(mutex());
    return FormatGuestGpuReportLocked(counters());
}

std::string GuestDispatchReport() {
    std::lock_guard<std::mutex> lock(mutex());
    std::string out = FormatGuestGpuReportLocked(counters());
    for (const auto& [key, why] : counters().refused) {
        char line[128];
        std::snprintf(line, sizeof(line), "\n  key %016llx: %s", static_cast<unsigned long long>(key), RefusalName(why));
        out += line;
    }
    return out;
}

}  // namespace bbx::d3d12

using namespace bbx::d3d12;

bool host_gpu_init(const char* const*, std::uint32_t, bool) {
    // The device is the session's; a guest-visible GPU exists when the session
    // installed its binding, and not before (a boot that never creates a device
    // still runs, as it does on upstream's stub).
    return host_gpu_available();
}

bool host_gpu_available() { return GuestGpu() != nullptr; }
// Xbox Dev Mode enforces a 5,120 MiB unified memory cap; reporting tight memory
// keeps live_resolution's display buffers and GFX_GraphicsPrivate heap capped at
// 1920x1080 (avoiding 5120x2160 buffers, +1,088 MiB heap bloat, and 5K clears).
bool host_gpu_memory_tight() { return true; }
bool host_gpu_small_vram() { return false; }
bool host_gpu_integrated() {
    GpuMemoryGlance g{};
    return host_gpu_memory_glance(g) ? g.integrated : false;
}
bool host_gpu_integrated_with_room() { return false; }
const memory_policy::Policy& host_gpu_memory_policy() {
    static const memory_policy::Policy p;
    return p;
}

// ---- the timeline fence ------------------------------------------------------------
// gnm_exec's EOP waits and video.cpp's flip-after-GPU ask these three questions,
// and the answers are the device's own fence: what was signalled, what the GPU
// has finished, and how much is in flight.
std::uint64_t host_gpu_work_needs() {
    const GuestGpuBinding* gpu = GuestGpu();
    if (!gpu || !gpu->fence_submitted || !gpu->fence_completed) return 0;
    const std::uint64_t submitted = gpu->fence_submitted(gpu->user);
    const std::uint64_t completed = gpu->fence_completed(gpu->user);
    return submitted > completed ? submitted - completed : 0;
}

std::uint64_t host_gpu_submissions_completed() {
    const GuestGpuBinding* gpu = GuestGpu();
    if (!gpu || !gpu->fence_completed) return 0;  // nothing has ever been submitted
    return gpu->fence_completed(gpu->user);
}

bool host_gpu_wait_completed(std::uint64_t value, std::uint64_t timeout_ms) {
    const GuestGpuBinding* gpu = GuestGpu();
    if (!gpu || !gpu->fence_wait) return false;
    if (gpu->fence_completed && gpu->fence_completed(gpu->user) >= value) return true;
    return gpu->fence_wait(gpu->user, value, timeout_ms);
}

void host_gpu_submit() {
    const GuestGpuBinding* gpu = GuestGpu();
    if (gpu && gpu->fence_signal) gpu->fence_signal(gpu->user);
}

void host_gpu_flush() {
    // Flush is submit: the port has one queue and one fence, and a guest flush
    // is a promise that the work is on the GPU, which a signal is.
    host_gpu_submit();
}

// ---- guest memory ----------------------------------------------------------------
bool host_gpu_mem_write(std::uint64_t va, const void* data, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex());
    Counters& c = counters();
    const GuestGpuBinding* gpu = g_bound;
    if (!gpu || !gpu->mem_write || !data || bytes == 0) {
        ++c.mem_refused;
        Publish();
        return false;
    }
    const bool ok = gpu->mem_write(gpu->user, va, data, bytes);
    if (ok) {
        ++c.mem_writes;
    } else {
        ++c.mem_refused;
    }
    Publish();
    return ok;
}

bool host_gpu_mem_write_serial(std::uint64_t va, const void* data, std::size_t bytes, std::uint64_t* serial) {
    if (!host_gpu_mem_write(va, data, bytes)) return false;
    if (serial) {
        const GuestGpuBinding* gpu = GuestGpu();
        if (gpu && gpu->fence_signal) {
            *serial = gpu->fence_signal(gpu->user);
        } else {
            *serial = host_gpu_submissions_completed();
        }
    }
    return true;
}

bool host_gpu_mem_write_ordered(std::uint64_t va, const void* data, std::size_t bytes, std::uint64_t* serial) {
    return host_gpu_mem_write_serial(va, data, bytes, serial);
}

bool host_gpu_mem_fill(std::uint64_t va, std::uint32_t value, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex());
    Counters& c = counters();
    const GuestGpuBinding* gpu = g_bound;
    if (!gpu || !gpu->mem_fill || bytes == 0) {
        ++c.mem_refused;
        Publish();
        return false;
    }
    const bool ok = gpu->mem_fill(gpu->user, va, value, bytes);
    ok ? ++c.mem_fills : ++c.mem_refused;
    Publish();
    return ok;
}

bool host_gpu_mem_copy(std::uint64_t dst_va, std::uint64_t src_va, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex());
    Counters& c = counters();
    const GuestGpuBinding* gpu = g_bound;
    if (!gpu || !gpu->mem_copy || bytes == 0) {
        ++c.mem_refused;
        Publish();
        return false;
    }
    const bool ok = gpu->mem_copy(gpu->user, dst_va, src_va, bytes);
    ok ? ++c.mem_copies : ++c.mem_refused;
    Publish();
    return ok;
}

void host_gpu_read_guest(std::uint64_t va, std::size_t bytes, void* out) {
    std::lock_guard<std::mutex> lock(mutex());
    Counters& c = counters();
    const GuestGpuBinding* gpu = g_bound;
    if (!gpu || !gpu->read_guest || !out || bytes == 0) {
        if (out && bytes != 0) std::memset(out, 0, bytes);  // a read of nothing is zeroes, not garbage
        ++c.mem_refused;
        Publish();
        return;
    }
    if (gpu->read_guest(gpu->user, va, out, bytes)) {
        ++c.mem_reads;
    } else {
        std::memset(out, 0, bytes);
        ++c.mem_refused;
    }
    Publish();
}

// ---- dispatch ---------------------------------------------------------------------
bool host_gpu_dispatch(const GpuDispatch& d) {
    std::lock_guard<std::mutex> lock(mutex());
    Counters& c = counters();
    ++c.dispatches;
    const GuestGpuBinding* gpu = g_bound;
    if (!gpu) {
        Refuse(0, GuestDispatchRefusal::kNoDevice, "no device binding installed");
        return false;
    }
    // The program's identity, exactly as the bake computes it: the words the
    // guest's COMPUTE_PGM_LO/HI point at, and the stage facts the dispatch's
    // registers state (bbx_shader_cache.h ConsoleStageKeyOf).
    std::uint64_t key = 0;
    if (gpu->dispatch_key) {
        key = gpu->dispatch_key(gpu->user, d);
    }
    if (key == 0) {
        Refuse(d.code_va, GuestDispatchRefusal::kBadProgram, "the dispatch names no readable program");
        return false;
    }
    if (gpu->dispatch) {
        // The pipeline (5B-2): the binding resolves the cached DXIL into a
        // compute PSO, binds the StageParams CBV and the space-3 tables, and
        // issues the dispatch. Its answer is the truth about what happened.
        if (gpu->dispatch(gpu->user, key, d)) {
            ++c.dispatched;
            Publish();
            return true;
        }
        Refuse(key, GuestDispatchRefusal::kNoPipeline, "the binding could not build the pipeline");
        return false;
    }
    Refuse(key, GuestDispatchRefusal::kCacheMiss, "no dispatch hook: the baked cache answered nothing");
    return false;
}

GpuDispatchClass host_gpu_last_dispatch_class(const char** program) {
    if (program) *program = "-";  // the classifications are the Vulkan path's census
    return kDcNone;
}

std::uint32_t host_gpu_dispatch_user_reads() { return 0; }

bool host_gpu_draw(const GpuDraw& d) {
    std::lock_guard<std::mutex> lock(mutex());
    Counters& c = counters();
    ++c.draws;
    // Step 1 of the graphics half (5B-2b): what this draw is, from the guest's own
    // registers and from nothing else - the two stage programs, the targets and
    // their formats, the depth format, the scissor. No field is guessed, and an
    // identity this port cannot build a pipeline for is refused by name here
    // rather than dropped by a counter.
    // The stage list is the session's answer (`GpuDrawInputs::stages`, i.e.
    // VGT_SHADER_STAGES_EN); with no GX inputs there is nothing to say it, so 0.
    const std::uint32_t stages = d.gx != nullptr ? d.gx->stages : 0;
    const DrawIdentity identity = DecodeDraw(d.sh, d.ctx, d.uconfig, stages);
    const std::uint64_t fingerprint = DrawFingerprint(identity);
    if (identity.verdict != DrawVerdict::kOk) {
        ++c.draw_refusals;
        if (c.logged_draw_refusals.insert(fingerprint).second) {
            std::fprintf(stderr, "host_gpu: draw %llu refused: %s\n", static_cast<unsigned long long>(c.draws),
                         DrawIdentityLine(identity).c_str());
        }
        Publish();
        return false;
    }
    ++c.draws_identified;
    if (c.logged_draws.insert(fingerprint).second) {
        std::fprintf(stderr, "host_gpu: draw %llu: %s\n", static_cast<unsigned long long>(c.draws),
                     DrawIdentityLine(identity).c_str());
    }
    const GuestGpuBinding* gpu = g_bound;
    if (gpu && gpu->draw) {
        if (gpu->draw(gpu->user, d, identity)) {
            ++c.draws_executed;
            Publish();
            return true;
        }
    }
    ++c.draw_pipeline_missing;
    if (c.logged_draws.size() <= 8 || c.draw_pipeline_missing == 1) {
        std::fprintf(stderr,
                     "host_gpu: draw %llu identified but not recorded (%s)\n",
                     static_cast<unsigned long long>(c.draws),
                     (gpu && gpu->draw) ? "graphics pipeline refused or cache miss" : "no graphics hook installed");
    }
    Publish();
    return false;
}

void host_gpu_note_shader_created(int stage, const std::uint8_t* container, std::size_t size, std::uint64_t) {
    std::lock_guard<std::mutex> lock(mutex());
    const GuestGpuBinding* gpu = g_bound;
    if (gpu && gpu->note_shader_created && container != nullptr && size != 0) {
        gpu->note_shader_created(gpu->user, stage, container, size);
    }
}

int host_gpu_run_commit(const GpuDraw*, const std::uint64_t*, std::uint32_t, std::uint32_t& failed) {
    failed = 0;
    return 0;
}

void host_gpu_report() {
    const std::string line = GuestGpuReport();
    std::printf("%s\n", line.c_str());
}

GpuStats host_gpu_stats() {
    GpuStats stats{};
    const GuestGpuCounts counts = GuestGpuStats();
    stats.dispatches = counts.dispatches;
    stats.draws = counts.draws;
    stats.draw_calls = counts.draws;
    stats.transfers = counts.mem_writes + counts.mem_fills + counts.mem_copies;
    return stats;
}

// ---- render targets, clears, copies, texture updates, and memory queries ----------
bool host_gpu_clear_view_resolvable(const GpuClearView& c, const char** why) {
    const DecodedClearView decoded = DecodeClearView(ToClearViewInput(c));
    if (!decoded.ok) {
        if (why) *why = decoded.why;
        return false;
    }
    const GuestGpuBinding* gpu = GuestGpu();
    if (!gpu || !gpu->clear_view) {
        if (why) *why = "no GPU";
        return false;
    }
    if (why) *why = nullptr;
    return true;
}

bool host_gpu_clear_view(const GpuClearView& c, bool apply, GpuClearViewResult* out) {
    const DecodedClearView decoded = DecodeClearView(ToClearViewInput(c));
    if (!decoded.ok) return false;
    std::lock_guard<std::mutex> lock(mutex());
    Counters& cnt = counters();
    const GuestGpuBinding* gpu = g_bound;
    if (!gpu || !gpu->clear_view) return false;
    bool created = false;
    if (!gpu->clear_view(gpu->user, decoded, apply, &created)) return false;
    if (apply) {
        ++cnt.clears;
        Publish();
    }
    if (out != nullptr) {
        out->image = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(decoded.base));
        out->first_layer = c.first_slice;
        out->layers = c.slices != 0 ? c.slices : 1u;
        out->created = created;
    }
    return true;
}

bool host_gpu_clear_target(std::uint64_t va, std::uint32_t value, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex());
    Counters& cnt = counters();
    const GuestGpuBinding* gpu = g_bound;
    if (!gpu || !gpu->clear_target || va == 0 || bytes == 0) return false;
    if (!gpu->clear_target(gpu->user, va, value, bytes)) return false;
    ++cnt.clears;
    Publish();
    return true;
}

bool host_gpu_copy_guest(std::uint64_t dst, std::uint64_t src, std::size_t bytes, std::uint64_t, std::uint64_t,
                         std::uint32_t, std::uint32_t) {
    if (dst == 0 || src == 0 || bytes == 0 || dst == src) return false;
    {
        std::lock_guard<std::mutex> lock(mutex());
        Counters& cnt = counters();
        const GuestGpuBinding* gpu = g_bound;
        if (gpu && gpu->copy_target && gpu->copy_target(gpu->user, dst, src, bytes)) {
            ++cnt.target_copies;
            if (gpu->mem_copy && gpu->mem_copy(gpu->user, dst, src, bytes)) {
                ++cnt.mem_copies;
            }
            Publish();
            return true;
        }
    }
    return host_gpu_mem_copy(dst, src, bytes);
}

bool host_gpu_copy_back(std::uint64_t dst, std::uint64_t src, std::size_t bytes) {
    return host_gpu_copy_guest(dst, src, bytes);
}

std::size_t host_gpu_copy_backs(const std::uint64_t* dst, const std::uint64_t* src, const std::uint64_t* bytes,
                                std::size_t n) {
    if (dst == nullptr || src == nullptr || bytes == nullptr) return 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (bytes[i] == 0) continue;
        if (!host_gpu_copy_guest(dst[i], src[i], static_cast<std::size_t>(bytes[i]))) return i;
    }
    return n;
}

bool host_gpu_draw_window(std::uint64_t dst, const void* data, std::uint32_t bytes) {
    return host_gpu_mem_write(dst, data, bytes);
}

std::uint64_t host_gpu_guest_copy_targets() { return GuestGpuStats().target_copies; }

void host_gpu_forget_region(std::uint64_t lo, std::uint64_t hi) {
    std::lock_guard<std::mutex> lock(mutex());
    const GuestGpuBinding* gpu = g_bound;
    if (gpu && gpu->forget_region && lo < hi) {
        gpu->forget_region(gpu->user, lo, hi);
    }
}

void host_gpu_targets_rebuild(bool on) {
    if (!on) {
        host_gpu_forget_region(0, ~0ull);
    }
}

void host_gpu_texture_written(std::uint64_t base, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex());
    Counters& cnt = counters();
    const GuestGpuBinding* gpu = g_bound;
    if (gpu && gpu->texture_written && base != 0 && bytes != 0) {
        gpu->texture_written(gpu->user, base, bytes);
        ++cnt.texture_writes;
        Publish();
    }
}

bool host_gpu_yebis_texture_update(std::uint64_t memory, std::uint64_t, std::uint32_t mip, std::uint32_t slice,
                                   std::uint32_t x, std::uint32_t y, std::uint32_t, std::uint32_t h, const void* rows,
                                   std::uint32_t pitch, std::uint64_t) {
    if (mip == 0 && slice == 0 && x == 0 && y == 0 && memory != 0 && rows != nullptr && pitch != 0 && h != 0) {
        (void)host_gpu_mem_write(memory, rows, static_cast<std::size_t>(pitch) * h);
    }
    // Return false so gx_resources also calls host_gpu_texture_written after the
    // guest UpdateSubresource finishes writing its tiled/linear memory.
    return false;
}

bool host_gpu_memory_glance(GpuMemoryGlance& out) {
    std::lock_guard<std::mutex> lock(mutex());
    const GuestGpuBinding* gpu = g_bound;
    if (!gpu || !gpu->memory_glance) return false;
    return gpu->memory_glance(gpu->user, &out);
}

std::string host_gpu_memory_budget_report() {
    std::lock_guard<std::mutex> lock(mutex());
    const GuestGpuBinding* gpu = g_bound;
    if (!gpu || !gpu->memory_budget_report) return {};
    return gpu->memory_budget_report(gpu->user);
}

std::string host_gpu_video_memory_report(bool) { return host_gpu_memory_budget_report(); }

std::string host_gpu_device_identity() {
    std::lock_guard<std::mutex> lock(mutex());
    const GuestGpuBinding* gpu = g_bound;
    if (!gpu || !gpu->device_identity) return {};
    return gpu->device_identity(gpu->user);
}

bool host_gpu_fsr_upscale(void*, void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                          std::uint32_t, std::uint32_t, void*, std::int32_t, std::int32_t, std::uint32_t,
                          std::uint32_t) {
    // D3d12Session::Present applies native SM 6.4 RCAS contrast-adaptive
    // sharpening directly in present.hlsl when sharp_upscale is enabled.
    return false;
}

void host_gpu_save_pipeline_cache() {}
void host_gpu_save_pipeline_cache_at_exit() {}
std::string host_gpu_compile_report() { return {}; }
std::string host_gpu_profile_report() { return {}; }

// ---- the stub's own answers, unchanged -------------------------------------------
// Everything below is gpu_stub.cpp's: with no Vulkan device there is nothing to
// report, capture, plugin or borrow, and a boot that asks gets the same answer it
// always did. Kept verbatim so a later slice can replace them one at a time and
// the diff says which.
#include "src/host/d3d12/host_gpu_d3d12_stub.inc"
