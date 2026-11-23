// SPDX-License-Identifier: GPL-3.0-or-later
// The guest's compute dispatches, on the console's device (Slice 5B-2a).
//
// `host_gpu_d3d12.cpp` answers host/gpu.h's fence and memory questions from the
// device, and hands a dispatch here once it has an identity: this file is the
// pipeline half. For a dispatch it:
//
//   1. finds the program at COMPUTE_PGM_LO/HI in the guest's own memory (the
//      OrbShdr footer gives the length; a program without one is decoded to its
//      s_endpgm), patches it exactly as the bake does, and translates it to learn
//      which image dimensions and sampler modes its key holds;
//   2. builds the key the bake wrote it under (ConsoleStageKeyOf over the same
//      words and the same translation choices), so a look-up hits the blob the
//      bake made rather than a shader this build invented;
//   3. looks it up in `<dump>/bbx_shaders.cache` (validated once against its
//      stamp), inflates the cached cs_6_4 blob, and creates the compute PSO on
//      first use with the guest compute root signature (`PsoCache` owns the
//      budget and the eviction);
//   4. records the dispatch: one shader-visible CBV/SRV/UAV heap, the 512-byte
//      StageParams block as the root CBV at `b0, space1`, the space-1 tables
//      (null descriptors: see below), the space-3 page table and slabs, then
//      `Dispatch` or `ExecuteIndirect` and a UAV barrier.
//
// What the StageParams block holds, and why that is the whole of it: the console
// bakes with `kCbSsbo = false`, `kBindless = false` and the guest-memory lowering
// (bbx_guest_mem.h), so a compute shader reads its constants from `b0`'s
// `user_sgpr[16]` and its resources from the space-1 and space-3 tables - the
// `cb_valid`/`cb_bias_dw`/`cb_stride` fields exist for the Vulkan path, where a
// buffer binding is a page-table walk. Filling `user_sgpr` from the dispatch's
// own user data and leaving those zero is therefore the shader's actual contract,
// not a placeholder.
//
// The space-1 slots (Slice 5B-2b): a baked module's `t#`/`u#` are whatever the
// translator made them, which this side cannot work out for itself, so the bake
// writes the assignment beside the cache (`bbx_shaders.paths`) and step 4 binds
// from it: for each plan, the path resolved against this dispatch's own user
// data, the V# decided (`bbx_guest_binding.h`), and one view written into the
// compute stage's slice of the guest arena. A binding with no plan stays a null
// descriptor, which D3D12 defines as "nothing bound" - never a default view. Each
// refusal (a path that does not resolve, a range the page table cannot address,
// a range that spans two slabs) is one plain-English line naming the step, and
// the slot it refused is left null.
//
// What is still missing on purpose, and counted and logged rather than guessed:
// a read-only (`t#`) view over guest memory, which would need the slabs in a read
// state and a transition this pass does not record yet; the graphics half
// (`host_gpu_draw`); and the sampled resources. Every dispatch this file cannot
// run says so once by key with the reason, in host_gpu_d3d12.cpp's log and report.
#include "xbox/platform/guest_compute.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gcn/isa.h"
#include "gcn/translate.h"
#include "hle/fs.h"
#include "hle/modules.h"
#include "host/shader_patch.h"
#include "src/host/d3d12/bbx_cache.h"
#include "src/host/d3d12/bbx_descriptors.h"
#include "src/host/d3d12/bbx_guest_root.h"
#include "src/host/d3d12/bbx_psocache.h"
#include "src/host/d3d12/bbx_shader_bake.h"
#include "src/host/d3d12/bbx_shader_cache.h"
#include "src/host/d3d12/device.h"
#include "src/host/d3d12/bbx_guest_binding.h"
#include "src/host/d3d12/bbx_guest_path.h"
#include "src/host/d3d12/bbx_path_table.h"
#include "src/host/d3d12/pipeline.h"
#include "xbox/platform/host_fs.h"
#include "xbox/platform/log.h"

#if defined(BBX_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace bbx::platform {
namespace {

using bbx::d3d12::ComPtr;
using bbx::d3d12::ConsoleStageKey;
using bbx::d3d12::GuestHeapLayout;
using bbx::d3d12::GuestStage;
using bbx::d3d12::PsoKey;
using bbx::d3d12::ShaderCache;
using bbx::d3d12::Stage;

// The program's words, and what identifies it: the OrbShdr footer's name and the
// hash over the patched words, which is what the bake keys its blobs by
// (program_hash → ConsoleStageKeyOf).
struct Program {
    std::vector<std::uint32_t> words;
    std::string name;
    std::uint64_t hash = 0;
    // Where this program keeps its resource table, decoded from its own
    // instructions (`s_load_dwordx2` out of a user SGPR pair, then a load through
    // what it loaded): -1 when it keeps none, and the evidence is the instruction
    // pair the answer came from, not a comment.
    int table_slot = -1;
    std::string table_evidence;
    // The table offsets the program reads through that pointer, in the order it
    // reads them. These are the bindings' bytes; matching them to the baked
    // shader's `tN`/`uN` needs the translator's assignment, which the bake has and
    // this side does not yet - so they are logged, not bound.
    std::vector<std::uint32_t> table_reads_dw;
    void DecodeTable() {
        table_slot = d3d12::DecodeTableSlot(words.data(), words.size(), &table_evidence);
        table_reads_dw.clear();
        if (table_slot < 0) return;
        const std::vector<d3d12::UserDataLoad> loads = d3d12::DecodeUserDataLoads(words.data(), words.size());
        // The pair the pointer was loaded into is the source of the reads; find it
        // again from the instruction that produced it.
        std::uint32_t pair = 0;
        for (const d3d12::UserDataLoad& load : loads) {
            if (!load.buffer && load.op >= 1 && load.op <= 2 && load.from_user_data &&
                static_cast<int>(load.sbase) == table_slot) {
                pair = load.sdst;
                break;
            }
        }
        for (const d3d12::UserDataLoad& load : loads) {
            if (load.sbase != pair) continue;
            if (table_reads_dw.size() < 64) table_reads_dw.push_back(load.offset_dw);
        }
    }
};

// Same reading as host/gpu.cpp's extract_program: the ShaderBinaryInfo footer
// that follows the code in memory gives its length; without one, decode to the
// last s_endpgm. `hle_kernel_va_mapped` is what makes the read safe: a patch
// address that is not the guest's memory is refused, not dereferenced.
bool ExtractProgram(std::uint64_t code_va, Program* out) {
    if (!hle_kernel_va_mapped(code_va, 4)) return false;
    const auto* p = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(code_va));
    std::size_t limit = 0;
    while (limit < (1u << 20) && hle_kernel_va_mapped(code_va + limit, 4096)) limit += 4096;
    if (limit == 0) return false;
    for (std::size_t i = 0; i + 28 <= limit; i += 4) {
        if (std::memcmp(p + i, "OrbShdr", 7) != 0) continue;
        std::uint32_t lenfield = 0;
        std::memcpy(&lenfield, p + i + 8, 4);
        const std::uint32_t length = lenfield >> 8;
        if (length == 0 || length > i || (length & 3) != 0) return false;
        out->words.resize(length / 4);
        std::memcpy(out->words.data(), p, length);
        std::uint32_t h0 = 0;
        std::memcpy(&h0, p + i + 16, 4);
        char name[32];
        std::snprintf(name, sizeof(name), "%08x", h0);
        out->name = name;
        shader_patch_apply(out->name, out->words);
        out->hash = bbx::Fnv1a64(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(out->words.data()), out->words.size() * sizeof(std::uint32_t)));
        out->DecodeTable();
        return true;
    }
    // No footer: the program is what decodes up to its last s_endpgm.
    const gcn::Program prog = gcn::decode(reinterpret_cast<const std::uint32_t*>(p), limit / 4);
    std::uint32_t end = 0;
    for (const gcn::Inst& in : prog.insts) {
        if (in.enc == gcn::Enc::SOPP && in.op == 1) end = in.offset + 4;  // s_endpgm
        if (!prog.errors.empty() && in.offset >= prog.errors[0].offset) break;
    }
    if (end == 0 || end > limit) return false;
    out->words.resize(end / 4);
    std::memcpy(out->words.data(), p, end);
    out->name = "no-footer";
    out->hash = bbx::Fnv1a64(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(out->words.data()), out->words.size() * sizeof(std::uint32_t)));
    out->DecodeTable();
    return true;
}

// The translation's contribution to a key: the image dimensions and sampler
// modes the program predicts, exactly as the bake asks for them
// (tools/bbx_bake_cache.cpp: gcn::predict_image_dims over the paths translation).
bool PredictPaths(const Program& program, std::uint32_t rsrc1, std::uint32_t rsrc2, ConsoleStageKey* key,
                  std::string* why) {
    const gcn::Program prog = gcn::decode(program.words.data(), program.words.size());
    if (!prog.errors.empty()) {
        *why = "decode: " + prog.errors.front().what;
        return false;
    }
    gcn::TranslateOptions options;
    options.stage = gcn::Stage::Compute;
    options.rsrc1 = rsrc1;
    options.rsrc2 = rsrc2;
    const gcn::TranslateResult paths = gcn::translate(prog, options);
    if (!paths.errors.empty()) {
        *why = "translation: " + paths.errors.front();
        return false;
    }
    key->dims.clear();
    for (const gcn::PredictedImage& image : gcn::predict_image_dims(prog, paths)) {
        key->dims.push_back((image.dim << 1) | (image.arrayed ? 1u : 0u));
    }
    key->sampler_modes.assign(paths.samplers.size(), 0);  // unnormalized: none predicted, as the bake
    return true;
}

// The bake stores each blob zlib-compressed with its inflated size (bbx_cache.h
// BakedBlob): inflate refuses a stream that does not produce exactly that.
bool Inflate(const bbx::d3d12::BakedBlob& blob, std::vector<std::uint8_t>* out) {
#if defined(BBX_HAVE_ZLIB)
    uLongf inflated = blob.raw_size;
    return ::uncompress(out->data(), &inflated, blob.data, blob.size) == Z_OK && inflated == blob.raw_size;
#else
    (void)blob;
    (void)out;
    return false;
#endif
}

std::string Join(const std::string& dir, const char* name) {
    return bbx::hostfs::JoinPath(dir, name);
}

// The resolver's guest-memory seam (bbx_guest_path.h's ReadGuest): the port's
// guest VAs are its own pointers (hle/kernel.cpp maps direct memory there), and a
// range the guest has not mapped is a refusal, not a zero fill.
bool ReadGuestMemory(void*, std::uint64_t va, void* out, std::size_t bytes) {
    if (!hle_kernel_va_mapped(va, bytes)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va)), bytes);
    return true;
}

struct DispatchId {
    std::uint64_t code_va = 0;
    std::uint32_t rsrc1 = 0;
    std::uint32_t rsrc2 = 0;
    std::uint32_t tx = 0;
    std::uint32_t ty = 0;
    std::uint32_t tz = 0;
    bool operator==(const DispatchId&) const = default;
};

struct DispatchIdHash {
    std::size_t operator()(const DispatchId& k) const noexcept {
        std::uint64_t h = k.code_va ^ 0x9e3779b97f4a7c15ull;
        const auto mix = [&h](std::uint64_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix((static_cast<std::uint64_t>(k.rsrc1) << 32) | k.rsrc2);
        mix((static_cast<std::uint64_t>(k.tx) << 32) | k.ty);
        mix(k.tz);
        return static_cast<std::size_t>(h);
    }
};

}  // namespace

struct GuestCompute::Impl {
    d3d12::Device* device = nullptr;
    d3d12::PipelineSet* pipelines = nullptr;
    d3d12::GuestMemory* memory = nullptr;
    d3d12::DescriptorRing* descriptor_ring = nullptr;
    ID3D12RootSignature* root = nullptr;  // the compute signature (bbx_guest_root.h)
    std::uint32_t arena_base = 0;         // the guest arena's first descriptor (space 3)
    std::uint32_t sampler_base = 0;
    std::uint32_t cbv_stride = 0, sampler_stride = 0;

    // Non-shader-visible CPU heap pre-populated with 191 null space-1
    // descriptors so freshly allocated ring slices initialize in a single
    // CopyDescriptorsSimple call without 191 per-slot Create*View calls.
    ComPtr<ID3D12DescriptorHeap> null_template_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE null_template_cpu{};

    ShaderCache cache;
    bool cache_tried = false;
    std::string cache_dir;
    std::string cache_line = "shader cache: not loaded";

    // The bake's binding assignment (bbx_shaders.paths), paired with the cache's
    // stamp: it says which guest memory each of a module's t#/u# is.
    d3d12::PathTable paths;
    bool paths_tried = false;
    std::string paths_line = "binding table: not loaded";

    struct SrvCopy {
        std::uint32_t slab = 0;
        std::uint64_t src_offset = 0;
        std::uint64_t dst_offset = 0;
        std::uint64_t bytes = 0;
    };
    std::uint64_t bound_key = 0;
    std::array<std::uint32_t, 16> bound_user_data{};
    std::array<std::uint32_t, 16> bound_cb_stride{};
    std::array<std::uint32_t, 16> bound_cb_w3{};
    d3d12::DescriptorSlice bound_slice{};
    std::uint64_t bound_srv_ring_gen = 0;
    bool bound_valid = false;
    std::vector<SrvCopy> bound_srv_copies;
    std::unordered_set<std::uint64_t> logged_binding_keys;

    std::unordered_map<std::uint64_t, Program> programs;                   // code VA -> program
    std::unordered_map<DispatchId, std::uint64_t, DispatchIdHash> keys;    // dispatch tuple -> key

    // The compute PSOs: the portable cache decides the budget and the eviction,
    // the device owns the objects. Evicted PSOs stay alive until the GPU fence
    // passes their retirement value. Guarded by pso_mutex so background compute
    // PSO pre-warming workers can safely populate the cache concurrently.
    struct RetiredPso {
        ID3D12PipelineState* pso = nullptr;
        std::uint64_t fence = 0;
    };
    std::mutex pso_mutex;
    std::unique_ptr<d3d12::PsoCache> psos;
    std::uint64_t pso_budget_bytes = 0;
    std::unordered_map<std::uint64_t, ID3D12PipelineState*> pso_objects;
    std::unordered_set<std::uint64_t> failed_psos;
    std::vector<RetiredPso> retired_psos;
    std::vector<ID3D12PipelineState*> batch_retired_psos;
    std::uint64_t clock = 0;
    std::uint64_t completed = 0;  // the fence's last completed value (OnFrame)

    // Multi-threaded compute PSO pre-warmer: inflates Stage::kCompute blobs from
    // bbx_shaders.cache across worker threads using free-threaded
    // CreateComputePipelineState (never ID3D12PipelineLibrary, which removes the
    // device on Xbox).
    bool prewarm_started = false;
    std::atomic<bool> prewarm_stop{false};
    std::atomic<std::size_t> prewarm_next{0};
    std::atomic<std::uint64_t> prewarm_created{0};
    std::atomic<std::uint64_t> prewarm_failed{0};
    std::vector<std::uint64_t> prewarm_keys;
    std::vector<std::thread> prewarm_threads;

    // Fence-tracked command allocator/list pool and open batch state.
    struct CommandSlot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        std::uint64_t fence = 0;
    };
    std::vector<CommandSlot> free_lists;
    std::vector<CommandSlot> in_flight;
    CommandSlot active_cmd;
    bool batch_open = false;
    std::uint32_t batch_dispatches = 0;
    static constexpr std::uint32_t kMaxBatchDispatches = 128;
    bool batch_heaps_bound = false;
    bool batch_root_bound = false;
    bool batch_samplers_bound = false;
    bool batch_space3_bound = false;
    ID3D12PipelineState* batch_pso = nullptr;
    std::uint32_t batch_space1_offset = UINT32_MAX;
    std::vector<d3d12::DescriptorSlice> batch_retired_slices;
    std::vector<d3d12::GuestRun> scratch_runs;

    // The StageParams ring: 4 slices x 512 slots = 2048 slots (1 MiB) of
    // persistently-mapped upload memory, fence-tracked per slice so writes
    // never overwrite in-flight dispatch constants.
    ComPtr<ID3D12Resource> params;
    void* params_mapped = nullptr;
    static constexpr std::uint32_t kParamSlices = 4;
    static constexpr std::uint32_t kParamSlotsPerSlice = 512;
    static constexpr std::uint32_t kParamSlots = kParamSlices * kParamSlotsPerSlice;
    std::uint32_t param_slice = 0;
    std::uint32_t param_slot_in_slice = 0;
    std::array<std::uint64_t, kParamSlices> param_slice_fence{};
    std::uint32_t param_slices_pending_mask = 0;

    // GPU-side SRV snapshot ring (DEFAULT heap, resting state COPY_DEST): 4 x 4 MiB
    // = 16 MiB fence-tracked slices. Space-1 read-only t# buffer bindings copy
    // their slice out of guest memory slabs into this ring before the shader runs,
    // avoiding UAV/SRV state aliasing on space3 slabs.
    static constexpr std::uint32_t kSrvRingSlices = 4;
    static constexpr std::uint64_t kSrvRingSliceBytes = 4ull * 1024ull * 1024ull;
    static constexpr std::uint64_t kSrvRingBytes = kSrvRingSlices * kSrvRingSliceBytes;
    ComPtr<ID3D12Resource> srv_ring;
    std::uint32_t srv_slice = 0;
    std::uint64_t srv_slice_cursor = 0;
    std::array<std::uint64_t, kSrvRingSlices> srv_slice_fence{};
    std::uint32_t srv_slices_pending_mask = 0;
    std::uint64_t srv_ring_gen = 1;
    bool srv_ring_budgeted = false;

    // DEFAULT-heap staging ring (resting state COPY_DEST) for ExecuteIndirect
    // dispatch arguments: 4 x 16 KiB = 64 KiB, fence-tracked per slice.
    static constexpr std::uint32_t kIndirectSlices = 4;
    static constexpr std::uint64_t kIndirectSliceBytes = 16384ull;
    static constexpr std::uint64_t kIndirectStagingBytes = kIndirectSlices * kIndirectSliceBytes;
    ComPtr<ID3D12Resource> indirect_staging;
    std::uint32_t indirect_slice = 0;
    std::uint64_t indirect_slice_cursor = 0;
    std::array<std::uint64_t, kIndirectSlices> indirect_slice_fence{};
    std::uint32_t indirect_slices_pending_mask = 0;

    // ExecuteIndirect's signature: one DISPATCH argument.
    ComPtr<ID3D12CommandSignature> indirect;

    Counts counts;

    void ReclaimCompleted(std::uint64_t done) {
        for (std::size_t i = 0; i < in_flight.size();) {
            if (in_flight[i].fence <= done) {
                CommandSlot slot = std::move(in_flight[i]);
                slot.fence = 0;
                in_flight.erase(in_flight.begin() + static_cast<std::ptrdiff_t>(i));
                free_lists.push_back(std::move(slot));
            } else {
                ++i;
            }
        }
        std::lock_guard<std::mutex> lk(pso_mutex);
        for (std::size_t i = 0; i < retired_psos.size();) {
            if (retired_psos[i].fence <= done) {
                if (retired_psos[i].pso != nullptr) retired_psos[i].pso->Release();
                retired_psos.erase(retired_psos.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    bool AcquireCommandList(CommandSlot* out) {
        ReclaimCompleted(device->CompletedValue());
        if (!free_lists.empty()) {
            *out = std::move(free_lists.back());
            free_lists.pop_back();
            if (FAILED(out->allocator->Reset()) || FAILED(out->list->Reset(out->allocator.Get(), nullptr))) {
                return false;
            }
            return true;
        }
        CommandSlot fresh;
        if (FAILED(device->Get()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(fresh.allocator.GetAddressOf())))) {
            return false;
        }
        if (FAILED(device->Get()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, fresh.allocator.Get(), nullptr,
                                                    IID_PPV_ARGS(fresh.list.GetAddressOf())))) {
            return false;
        }
        *out = std::move(fresh);
        return true;
    }

    bool BeginBatch() {
        if (batch_open) return true;
        if (!AcquireCommandList(&active_cmd)) return false;
        batch_open = true;
        batch_dispatches = 0;
        batch_heaps_bound = false;
        batch_root_bound = false;
        batch_samplers_bound = false;
        batch_space3_bound = false;
        batch_pso = nullptr;
        batch_space1_offset = UINT32_MAX;
        return true;
    }

    void MarkSrvSnapshotsSubmitted(std::uint64_t fence) {
        if (srv_slices_pending_mask == 0) return;
        for (std::uint32_t s = 0; s < kSrvRingSlices; ++s) {
            if ((srv_slices_pending_mask & (1u << s)) != 0) {
                if (fence > srv_slice_fence[s]) srv_slice_fence[s] = fence;
            }
        }
        srv_slices_pending_mask = 0;
    }

    void TouchSrvOffset(std::uint64_t offset) {
        const std::uint32_t slice = static_cast<std::uint32_t>(offset / kSrvRingSliceBytes);
        if (slice < kSrvRingSlices) {
            srv_slices_pending_mask |= (1u << slice);
        }
    }

    void FlushBatch() {
        if (!batch_open) return;
        ID3D12GraphicsCommandList* list = active_cmd.list.Get();
        const HRESULT close_hr = list->Close();
        batch_open = false;
        batch_dispatches = 0;
        batch_heaps_bound = false;
        batch_root_bound = false;
        batch_samplers_bound = false;
        batch_space3_bound = false;
        batch_pso = nullptr;
        batch_space1_offset = UINT32_MAX;

        std::uint64_t fence = device->SubmittedValue();
        if (SUCCEEDED(close_hr)) {
            ID3D12CommandList* lists[1] = {list};
            fence = device->ExecuteAndSignal(lists, 1);
            active_cmd.fence = fence;
            in_flight.push_back(std::move(active_cmd));
            ++counts.batches;
        } else {
            ++counts.record_failed;
            active_cmd = CommandSlot{};
        }

        for (std::uint32_t s = 0; s < kParamSlices; ++s) {
            if ((param_slices_pending_mask & (1u << s)) != 0) {
                if (fence > param_slice_fence[s]) param_slice_fence[s] = fence;
            }
        }
        param_slices_pending_mask = 0;

        for (std::uint32_t s = 0; s < kIndirectSlices; ++s) {
            if ((indirect_slices_pending_mask & (1u << s)) != 0) {
                if (fence > indirect_slice_fence[s]) indirect_slice_fence[s] = fence;
            }
        }
        indirect_slices_pending_mask = 0;

        MarkSrvSnapshotsSubmitted(fence);

        if (descriptor_ring != nullptr && !batch_retired_slices.empty()) {
            for (const d3d12::DescriptorSlice& slice : batch_retired_slices) {
                descriptor_ring->Release(slice, fence);
            }
        }
        batch_retired_slices.clear();

        if (!batch_retired_psos.empty()) {
            std::lock_guard<std::mutex> lk(pso_mutex);
            for (ID3D12PipelineState* pso : batch_retired_psos) {
                retired_psos.push_back({pso, fence});
            }
            batch_retired_psos.clear();
        }
    }

    std::uint32_t ReserveParamSlot() {
        if (param_slot_in_slice >= kParamSlotsPerSlice) {
            const std::uint32_t next = (param_slice + 1) % kParamSlices;
            if ((param_slices_pending_mask & (1u << next)) != 0) {
                FlushBatch();
            }
            const std::uint64_t wait_fence = param_slice_fence[next];
            if (wait_fence > device->CompletedValue()) {
                (void)device->WaitFor(wait_fence);
                ReclaimCompleted(device->CompletedValue());
            }
            param_slice = next;
            param_slot_in_slice = 0;
        }
        const std::uint32_t slot = param_slice * kParamSlotsPerSlice + param_slot_in_slice++;
        param_slices_pending_mask |= (1u << param_slice);
        return slot;
    }

    std::uint64_t ReserveIndirectOffset() {
        if (indirect_slice_cursor + 16ull > kIndirectSliceBytes) {
            const std::uint32_t next = (indirect_slice + 1) % kIndirectSlices;
            if ((indirect_slices_pending_mask & (1u << next)) != 0) {
                FlushBatch();
            }
            const std::uint64_t wait_fence = indirect_slice_fence[next];
            if (wait_fence > device->CompletedValue()) {
                (void)device->WaitFor(wait_fence);
                ReclaimCompleted(device->CompletedValue());
            }
            indirect_slice = next;
            indirect_slice_cursor = 0;
        }
        const std::uint64_t offset =
            static_cast<std::uint64_t>(indirect_slice) * kIndirectSliceBytes + indirect_slice_cursor;
        indirect_slice_cursor += 16ull;
        indirect_slices_pending_mask |= (1u << indirect_slice);
        return offset;
    }

    bool ReserveSrvSlice(std::uint64_t bytes, std::uint64_t* out_offset,
                         GuestCompute::FlushCallback flush_cb, void* flush_user) {
        if (srv_ring == nullptr || bytes == 0 || bytes > kSrvRingBytes) return false;
        const std::uint64_t aligned = (bytes + 15ull) & ~15ull;
        if (aligned > kSrvRingSliceBytes) {
            // Oversized t# buffer (> 4 MiB): flush open work and wait for the
            // highest in-flight slice fence before placing at offset 0.
            FlushBatch();
            if (flush_cb != nullptr) flush_cb(flush_user);
            std::uint64_t max_fence = 0;
            for (const std::uint64_t f : srv_slice_fence) {
                if (f > max_fence) max_fence = f;
            }
            if (max_fence > device->CompletedValue()) {
                (void)device->WaitFor(max_fence);
                ReclaimCompleted(device->CompletedValue());
            }
            *out_offset = 0;
            const std::uint32_t covered =
                static_cast<std::uint32_t>((aligned + kSrvRingSliceBytes - 1ull) / kSrvRingSliceBytes);
            for (std::uint32_t s = 0; s < covered && s < kSrvRingSlices; ++s) {
                srv_slices_pending_mask |= (1u << s);
            }
            srv_slice = std::min<std::uint32_t>(covered > 0 ? covered - 1 : 0, kSrvRingSlices - 1);
            srv_slice_cursor = kSrvRingSliceBytes;  // force slice advance on next reserve
            ++srv_ring_gen;
            return true;
        }
        if (srv_slice_cursor + aligned > kSrvRingSliceBytes) {
            const std::uint32_t next = (srv_slice + 1) % kSrvRingSlices;
            if ((srv_slices_pending_mask & (1u << next)) != 0) {
                FlushBatch();
                if (flush_cb != nullptr) flush_cb(flush_user);
            }
            const std::uint64_t wait_fence = srv_slice_fence[next];
            if (wait_fence > device->CompletedValue()) {
                (void)device->WaitFor(wait_fence);
                ReclaimCompleted(device->CompletedValue());
            }
            srv_slice = next;
            srv_slice_cursor = 0;
            ++srv_ring_gen;
        }
        *out_offset = static_cast<std::uint64_t>(srv_slice) * kSrvRingSliceBytes + srv_slice_cursor;
        srv_slice_cursor += aligned;
        srv_slices_pending_mask |= (1u << srv_slice);
        return true;
    }

    // --- the space-1 slots (ring-buffered per dispatch/state change) --------------
    D3D12_CPU_DESCRIPTOR_HANDLE SlotHandleAt(std::uint32_t absolute) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = device->CbvSrvUavHeap()->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(absolute) * cbv_stride;
        return handle;
    }

    void WriteNullSlotToHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle, std::uint32_t slot) {
        if (slot >= GuestHeapLayout::kUavOffset) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = 0;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            device->Get()->CreateUnorderedAccessView(nullptr, nullptr, &uav, handle);
        } else if (slot >= GuestHeapLayout::kCbvOffset && slot < GuestHeapLayout::kSrvOffset) {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
            cbv.BufferLocation = 0;
            cbv.SizeInBytes = 0;
            device->Get()->CreateConstantBufferView(&cbv, handle);
        } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R32_TYPELESS;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.NumElements = 0;
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            device->Get()->CreateShaderResourceView(nullptr, &srv, handle);
        }
    }

    void RetireSlice(d3d12::DescriptorSlice slice) {
        if (!slice.valid() || descriptor_ring == nullptr) return;
        if (batch_open) {
            batch_retired_slices.push_back(slice);
        } else {
            descriptor_ring->Release(slice, device->SubmittedValue());
        }
    }

    d3d12::DescriptorSlice AllocateSpace1Slice() {
        if (descriptor_ring == nullptr || null_template_heap == nullptr) return {};
        descriptor_ring->Collect(device->CompletedValue());
        std::optional<d3d12::DescriptorSlice> slice =
            descriptor_ring->Allocate(GuestHeapLayout::kSpace1Count);
        if (!slice.has_value() && batch_open) {
            FlushBatch();
            descriptor_ring->Collect(device->CompletedValue());
            slice = descriptor_ring->Allocate(GuestHeapLayout::kSpace1Count);
        }
        if (!slice.has_value()) {
            const std::uint64_t oldest = descriptor_ring->OldestPendingFence();
            if (oldest != 0) {
                (void)device->WaitFor(oldest);
                const std::uint64_t done = device->CompletedValue();
                ReclaimCompleted(done);
                descriptor_ring->Collect(done);
                slice = descriptor_ring->Allocate(GuestHeapLayout::kSpace1Count);
            }
        }
        if (!slice.has_value()) {
            (void)device->WaitIdle();
            const std::uint64_t done = device->CompletedValue();
            ReclaimCompleted(done);
            descriptor_ring->Collect(done);
            slice = descriptor_ring->Allocate(GuestHeapLayout::kSpace1Count);
        }
        if (!slice.has_value()) return {};
        device->Get()->CopyDescriptorsSimple(GuestHeapLayout::kSpace1Count, SlotHandleAt(slice->offset),
                                             null_template_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return *slice;
    }

    // One buffer view over the slab a plan resolved to: the resource is the slab
    // itself (never a copy, so a store lands in the guest's own memory), and the
    // view names the range inside it - offset in dwords, length in dwords.
    bool WriteBufferUav(std::uint32_t table_offset, std::uint32_t slot, const d3d12::BindingWrite& write) {
        if (memory->EnsureSlab(write.slab) != d3d12::Status::kOk) return false;
        ID3D12Resource* slab = memory->SlabBuffer(write.slab);
        if (slab == nullptr) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = write.offset / 4;
        uav.Buffer.NumElements = write.dwords;
        uav.Buffer.StructureByteStride = 0;
        uav.Buffer.CounterOffsetInBytes = 0;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->Get()->CreateUnorderedAccessView(slab, nullptr, &uav, SlotHandleAt(table_offset + slot));
        return true;
    }

    // One read-only t# buffer view over the GPU-side SRV snapshot ring. The
    // dispatch command list copies the guest slab slice into this offset right
    // before Dispatch(), avoiding UAV/SRV state aliasing on space3 slabs.
    bool WriteBufferSrv(std::uint32_t table_offset, std::uint32_t slot, const d3d12::BindingWrite& write,
                        SrvCopy* out_copy) {
        if (memory->EnsureSlab(write.slab) != d3d12::Status::kOk) return false;
        if (memory->SlabBuffer(write.slab) == nullptr) return false;
        const std::uint64_t bytes = static_cast<std::uint64_t>(write.dwords) * 4ull;
        std::uint64_t ring_offset = 0;
        if (!ReserveSrvSlice(bytes, &ring_offset, nullptr, nullptr)) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = static_cast<UINT>(ring_offset / 4ull);
        srv.Buffer.NumElements = write.dwords;
        srv.Buffer.StructureByteStride = 0;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->Get()->CreateShaderResourceView(srv_ring.Get(), &srv, SlotHandleAt(table_offset + slot));
        out_copy->slab = write.slab;
        out_copy->src_offset = write.offset;
        out_copy->dst_offset = ring_offset;
        out_copy->bytes = bytes;
        return true;
    }

    // Ring-buffered space-1 resource binding: reuses the active slice when
    // `(key, user_data)` and `srv_ring_gen` match; otherwise allocates a fresh
    // 191-slot slice from `descriptor_ring` and retires the previous slice with
    // the batch fence, eliminating every GPU idle stall on shader/resource change.
    std::uint32_t BindResources(std::uint64_t key, const GpuDispatch& d) {
        const std::uint32_t static_null_offset = arena_base + GuestHeapLayout::StageRegion(2);
        if (paths.status() != d3d12::PathTableStatus::kOk) return static_null_offset;
        const bool same_user_data =
            std::memcmp(bound_user_data.data(), d.user_data, sizeof(bound_user_data)) == 0;
        const bool srv_valid = bound_srv_copies.empty() || bound_srv_ring_gen == srv_ring_gen;
        if (bound_valid && bound_key == key && same_user_data && srv_valid) {
            for (const SrvCopy& copy : bound_srv_copies) {
                TouchSrvOffset(copy.dst_offset);
            }
            return bound_slice.valid() ? bound_slice.offset : static_null_offset;
        }

        // Collect plans for this key onto the stack first so a key with 0 plans
        // binds the permanent static null region without touching the ring.
        const d3d12::BindingPlan* active_plans[GuestHeapLayout::kSpace1Count];
        std::uint32_t active_bindings[GuestHeapLayout::kSpace1Count];
        std::uint32_t planned = 0;
        for (std::uint32_t binding = 0; binding < GuestHeapLayout::kSpace1Count; ++binding) {
            const d3d12::BindingPlan* plan = paths.Find(key, binding);
            if (plan == nullptr) continue;
            active_plans[planned] = plan;
            active_bindings[planned] = binding;
            ++planned;
        }

        if (bound_slice.valid()) {
            RetireSlice(bound_slice);
            bound_slice = {};
        }
        bound_srv_copies.clear();
        bound_key = key;
        std::memcpy(bound_user_data.data(), d.user_data, sizeof(bound_user_data));

        if (planned == 0) {
            bound_srv_ring_gen = srv_ring_gen;
            bound_valid = true;
            return static_null_offset;
        }

        const d3d12::DescriptorSlice new_slice = AllocateSpace1Slice();
        if (!new_slice.valid()) {
            bound_valid = false;
            return static_null_offset;
        }

        const bool should_log = logged_binding_keys.find(key) == logged_binding_keys.end();
        std::uint32_t written = 0, srvs = 0, refused = 0, nulls = 0;
        std::string line;
        for (std::uint32_t i = 0; i < planned; ++i) {
            const d3d12::BindingPlan* plan = active_plans[i];
            const std::uint32_t binding = active_bindings[i];
            d3d12::GuestHeapSlot slot{};
            const d3d12::GuestRangeKind kind =
                plan->kind == d3d12::BindingKind::kUav ? d3d12::GuestRangeKind::kUav : d3d12::GuestRangeKind::kSrv;
            if (!d3d12::GuestHeapSlotFor(GuestStage::kCompute, kind, binding, d3d12::kGuestParamsSpace, &slot)) {
                ++refused;
                if (should_log) {
                    line += " " + std::to_string(binding) + ": the root signature has no space-1 slot for it";
                }
                continue;
            }
            const d3d12::BindingWrite write =
                d3d12::DecideBindingWrite(*plan, d.user_data, memory->Table(), ReadGuestMemory, nullptr);
            switch (write.verdict) {
            case d3d12::BindingVerdict::kNull:
                ++nulls;
                break;
            case d3d12::BindingVerdict::kWrite:
                if (write.kind == d3d12::BindingKind::kUav) {
                    if (!WriteBufferUav(new_slice.offset, slot.index, write)) {
                        ++refused;
                        if (should_log) {
                            line += " " + d3d12::BindingWriteLine(write) + " refused (the slab is not resident)";
                        }
                        break;
                    }
                    ++written;
                    if (should_log) line += " " + d3d12::BindingWriteLine(write);
                } else {
                    SrvCopy copy{};
                    if (!WriteBufferSrv(new_slice.offset, slot.index, write, &copy)) {
                        ++refused;
                        if (should_log) {
                            line += " " + d3d12::BindingWriteLine(write) +
                                    " refused (SRV snapshot ring full or slab not resident)";
                        }
                        break;
                    }
                    bound_srv_copies.push_back(copy);
                    ++written;
                    ++srvs;
                    if (should_log) line += " " + d3d12::BindingWriteLine(write);
                }
                break;
            default:
                ++refused;
                if (should_log) line += " " + d3d12::BindingWriteLine(write);
                break;
            }
        }
        counts.bindings_written += written;
        counts.bindings_srv += srvs;
        counts.bindings_refused += refused;
        counts.bindings_null += nulls;
        bound_slice = new_slice;
        bound_srv_ring_gen = srv_ring_gen;
        bound_valid = true;

        if (should_log) {
            logged_binding_keys.insert(key);
            std::string text = "guest compute: key ";
            char head[192];
            std::snprintf(head, sizeof(head), "%016llx: %u plan(s), %u slot(s) written (%u srv), %u null, %u refused",
                          static_cast<unsigned long long>(key), planned, written, srvs, nulls, refused);
            text += head;
            text += line;
            LogMessage(refused != 0 ? Level::kWarn : Level::kInfo, "%s", text.c_str());
        }
        return bound_slice.offset;
    }

    // --- multi-threaded compute PSO pre-warming ----------------------------------
    void StartPrewarm() {
        if (prewarm_started || device == nullptr || root == nullptr || psos == nullptr) return;
        if (cache.status() != d3d12::CacheStatus::kOk) return;
        prewarm_started = true;
        prewarm_stop.store(false, std::memory_order_relaxed);
        prewarm_next.store(0, std::memory_order_relaxed);
        prewarm_created.store(0, std::memory_order_relaxed);
        prewarm_failed.store(0, std::memory_order_relaxed);

        prewarm_keys.clear();
        const auto& entries = cache.Entries();
        prewarm_keys.reserve(std::min<std::size_t>(entries.size(), 1024));
        for (const d3d12::CacheEntry& entry : entries) {
            if (entry.stage == Stage::kCompute && entry.raw_size != 0) {
                prewarm_keys.push_back(entry.key);
                if (prewarm_keys.size() >= 1024) break;
            }
        }
        if (prewarm_keys.empty()) return;

        const unsigned hw = std::thread::hardware_concurrency();
        const std::size_t max_workers = hw > 2 ? std::min<std::size_t>(4, hw / 2) : 1;
        const std::size_t workers = std::min<std::size_t>(prewarm_keys.size(), std::max<std::size_t>(1, max_workers));
        const std::uint64_t byte_cap = (pso_budget_bytes * 3ull) / 4ull;

        LogMessage(Level::kInfo, "guest compute: pre-warming %zu compute PSO(s) across %zu worker thread(s)",
                   prewarm_keys.size(), workers);

        prewarm_threads.reserve(workers);
        for (std::size_t w = 0; w < workers; ++w) {
            prewarm_threads.emplace_back([this, byte_cap]() {
                std::vector<std::uint8_t> dxil;
                while (!prewarm_stop.load(std::memory_order_relaxed)) {
                    const std::size_t idx = prewarm_next.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= prewarm_keys.size()) break;
                    const std::uint64_t key = prewarm_keys[idx];
                    {
                        std::lock_guard<std::mutex> lk(pso_mutex);
                        if (pso_objects.find(key) != pso_objects.end()) continue;
                        if (psos->Stats().live >= 1024 ||
                            (byte_cap != 0 && psos->Stats().live_bytes >= byte_cap)) {
                            break;
                        }
                    }
                    d3d12::BakedBlob blob{};
                    if (!cache.Lookup(key, &blob) || blob.data == nullptr || blob.raw_size == 0) continue;
                    dxil.resize(blob.raw_size);
                    if (!Inflate(blob, &dxil)) {
                        prewarm_failed.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    if (prewarm_stop.load(std::memory_order_relaxed)) break;

                    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
                    desc.pRootSignature = root;
                    desc.CS.pShaderBytecode = dxil.data();
                    desc.CS.BytecodeLength = dxil.size();
                    ComPtr<ID3D12PipelineState> created;
                    if (FAILED(device->Get()->CreateComputePipelineState(&desc,
                                                                         IID_PPV_ARGS(created.GetAddressOf())))) {
                        prewarm_failed.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }

                    std::lock_guard<std::mutex> lk(pso_mutex);
                    if (prewarm_stop.load(std::memory_order_relaxed)) break;
                    if (pso_objects.find(key) != pso_objects.end()) continue;
                    if (psos->Stats().live >= 768 ||
                        (byte_cap != 0 && psos->Stats().live_bytes + dxil.size() > byte_cap)) {
                        break;
                    }
                    ID3D12PipelineState* raw = created.Detach();
                    pso_objects.emplace(key, raw);
                    const std::vector<PsoKey> evicted = psos->Insert(PsoKey{key, 0}, key, dxil.size(), 1);
                    if (!evicted.empty()) {
                        const std::uint64_t retire_fence = device->SubmittedValue() + 1;
                        for (const PsoKey& ev : evicted) {
                            if (const auto it = pso_objects.find(ev.lo); it != pso_objects.end()) {
                                if (batch_open) {
                                    batch_retired_psos.push_back(it->second);
                                } else {
                                    retired_psos.push_back({it->second, retire_fence});
                                }
                                pso_objects.erase(it);
                            }
                        }
                    }
                    prewarm_created.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }

    void StopPrewarm() {
        prewarm_stop.store(true, std::memory_order_relaxed);
        for (std::thread& t : prewarm_threads) {
            if (t.joinable()) t.join();
        }
        prewarm_threads.clear();
        prewarm_keys.clear();
        prewarm_started = false;
    }

    // --- cache -------------------------------------------------------------------
    void LoadCache() {
        if (cache_tried) return;
        std::vector<std::string> dirs;
        const auto add_dir = [&dirs](const char* raw) {
            if (raw == nullptr || raw[0] == '\0') return;
            const std::string d(raw);
            if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
        };
        // Prefer LocalState\data on ties so a fresh on-console bake wins over an
        // older copy on USB, while still checking all roots to pick the cache
        // with the most valid entries.
        add_dir(hle_fs_data_root());
        add_dir(hle_fs_update_root());
        add_dir(hle_fs_app0_root());
        if (dirs.empty()) {
            cache_line = "shader cache: no dump or data root yet; every guest dispatch is refused until the dump is mounted";
            LogMessage(Level::kWarn, "guest compute: %s", cache_line.c_str());
            return;
        }
        cache_tried = true;
        bool saw_any = false;
        bool found_best = false;
        d3d12::ShaderCache best_cache;
        std::string best_dir;
        std::string best_line;
        for (const std::string& candidate : dirs) {
            const std::string path = Join(candidate, "bbx_shaders.cache");
            if (!bbx::hostfs::Exists(path)) continue;
            saw_any = true;
            bbx::hostfs::FileStat stat{};
            if (!bbx::hostfs::Stat(path, &stat) || stat.size == 0 || stat.size > (256u << 20)) {
                const std::string msg = "shader cache: " + path + " is empty or implausible; skipping";
                if (!found_best) cache_line = msg;
                LogMessage(Level::kWarn, "guest compute: %s", msg.c_str());
                continue;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.size));
            const int handle = bbx::hostfs::Open(path, bbx::hostfs::kRead);
            if (handle < 0) {
                const std::string msg = "shader cache: " + path + " would not open";
                if (!found_best) cache_line = msg;
                LogMessage(Level::kWarn, "guest compute: %s", msg.c_str());
                continue;
            }
            std::size_t read = 0;
            while (read < bytes.size()) {
                const std::int64_t got = bbx::hostfs::Read(handle, bytes.data() + read, bytes.size() - read);
                if (got <= 0) break;
                read += static_cast<std::size_t>(got);
            }
            (void)bbx::hostfs::Close(handle);
            if (read != bytes.size()) {
                const std::string msg = "shader cache: " + path + " read short";
                if (!found_best) cache_line = msg;
                LogMessage(Level::kWarn, "guest compute: %s", msg.c_str());
                continue;
            }
            // The stamp beside it: `<cache>.stamp.txt`, with legacy fallback `bbx_shaders.stamp`.
            std::string stamp;
            std::string stamp_path = path + ".stamp.txt";
            if (!bbx::hostfs::Exists(stamp_path)) {
                stamp_path = Join(candidate, "bbx_shaders.stamp");
            }
            if (bbx::hostfs::Exists(stamp_path)) {
                bbx::hostfs::FileStat stamp_stat{};
                if (bbx::hostfs::Stat(stamp_path, &stamp_stat) && stamp_stat.size < (1u << 16)) {
                    stamp.resize(static_cast<std::size_t>(stamp_stat.size));
                    const int sh = bbx::hostfs::Open(stamp_path, bbx::hostfs::kRead);
                    if (sh >= 0) {
                        std::size_t got_total = 0;
                        while (got_total < stamp.size()) {
                            const std::int64_t got =
                                bbx::hostfs::Read(sh, stamp.data() + got_total, stamp.size() - got_total);
                            if (got <= 0) break;
                            got_total += static_cast<std::size_t>(got);
                        }
                        (void)bbx::hostfs::Close(sh);
                        stamp.resize(got_total);
                    }
                }
            }
            d3d12::ShaderCache probe_cache;
            const d3d12::CacheStatus status = probe_cache.Load(bytes, stamp);
            const std::string probe_line = "shader cache: " + path + ", " + probe_cache.Report();
            if (status == d3d12::CacheStatus::kOk) {
                if (!found_best || probe_cache.entries() > best_cache.entries()) {
                    best_cache = std::move(probe_cache);
                    best_dir = candidate;
                    best_line = probe_line;
                    found_best = true;
                }
                continue;
            }
            if (!found_best) cache_line = probe_line;
            LogMessage(Level::kWarn, "guest compute: %s (%s); checking next location",
                       probe_line.c_str(), d3d12::CacheStatusName(status));
        }
        if (found_best) {
            cache = std::move(best_cache);
            cache_dir = std::move(best_dir);
            cache_line = std::move(best_line);
            LogMessage(Level::kInfo, "guest compute: %s", cache_line.c_str());
            StartPrewarm();
            return;
        }
        if (!saw_any) {
            cache_line = "shader cache: bbx_shaders.cache is missing under " + dirs.front() +
                         " and data root; no guest dispatch can be built";
            LogMessage(Level::kWarn, "guest compute: %s", cache_line.c_str());
        }
    }

    // The bake's binding assignment, from the file beside the cache and paired
    // with it: a table whose stamp is not this cache's is refused whole. Said
    // once, at load, because it is a property of the dump and not of a dispatch.
    void LoadPaths() {
        if (paths_tried) return;
        if (!cache_tried) return;
        paths_tried = true;
        if (cache.status() != d3d12::CacheStatus::kOk) {
            paths_line = "binding table: not read (the shader cache did not validate)";
            return;
        }
        const std::string dir = !cache_dir.empty() ? cache_dir : (hle_fs_data_root() != nullptr ? hle_fs_data_root() : "");
        if (dir.empty()) {
            paths_line = "binding table: no cache directory yet; no space-1 descriptor is bound until the dump is mounted";
            return;
        }
        const std::string path = Join(dir, "bbx_shaders.paths");
        bbx::hostfs::FileStat stat{};
        if (!bbx::hostfs::Exists(path) || !bbx::hostfs::Stat(path, &stat) || stat.size == 0 ||
            stat.size > (64u << 20)) {
            paths_line = "binding table: " + path +
                         " is missing or implausible; every space-1 slot stays a null descriptor (rerun the bake)";
            LogMessage(Level::kWarn, "guest compute: %s", paths_line.c_str());
            return;
        }
        std::string text(static_cast<std::size_t>(stat.size), '\0');
        const int handle = bbx::hostfs::Open(path, bbx::hostfs::kRead);
        if (handle < 0) {
            paths_line = "binding table: " + path + " would not open";
            LogMessage(Level::kError, "guest compute: %s", paths_line.c_str());
            return;
        }
        std::size_t read = 0;
        while (read < text.size()) {
            const std::int64_t got = bbx::hostfs::Read(handle, text.data() + read, text.size() - read);
            if (got <= 0) break;
            read += static_cast<std::size_t>(got);
        }
        (void)bbx::hostfs::Close(handle);
        text.resize(read);
        const d3d12::PathTableStatus status = paths.Load(text, cache.stamp_digest());
        paths_line = "binding table: " + path + ", " + paths.Report();
        if (status == d3d12::PathTableStatus::kOk) {
            LogMessage(Level::kInfo, "guest compute: %s", paths_line.c_str());
        } else {
            LogMessage(Level::kError, "guest compute: %s (%s): no space-1 descriptor is bound from it",
                       paths_line.c_str(), d3d12::PathTableStatusName(status));
        }
    }
};

GuestCompute::GuestCompute() : impl_(std::make_unique<Impl>()) {}
GuestCompute::~GuestCompute() { Destroy(); }

d3d12::Status GuestCompute::Create(d3d12::Device& device, d3d12::PipelineSet& pipelines,
                                   d3d12::GuestMemory& memory, d3d12::DescriptorRing* descriptor_ring,
                                   std::uint32_t arena_base, std::uint32_t sampler_base) {
    if (impl_ == nullptr) return d3d12::Status::kUnavailable;
    Impl& impl = *impl_;
    impl.device = &device;
    impl.pipelines = &pipelines;
    impl.memory = &memory;
    impl.descriptor_ring = descriptor_ring;
    impl.root = pipelines.GuestRootSignature(GuestStage::kCompute);
    impl.arena_base = arena_base;
    impl.sampler_base = sampler_base;
    impl.cbv_stride = device.CbvSrvUavStride();
    impl.sampler_stride = device.SamplerStride();
    if (impl.root == nullptr) {
        LogMessage(Level::kWarn, "guest compute: no compute root signature; dispatches will be refused");
        return d3d12::Status::kUnavailable;
    }

    // Create a 191-slot non-shader-visible CPU heap pre-populated with null
    // space-1 descriptors so both GuestCompute and GuestGraphics initialize each
    // ring slice in a single CopyDescriptorsSimple call.
    D3D12_DESCRIPTOR_HEAP_DESC tmpl_desc{};
    tmpl_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    tmpl_desc.NumDescriptors = GuestHeapLayout::kSpace1Count;
    tmpl_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device.Get()->CreateDescriptorHeap(&tmpl_desc, IID_PPV_ARGS(impl.null_template_heap.GetAddressOf())))) {
        LogMessage(Level::kError, "guest compute: failed to create null descriptor template heap");
        return d3d12::Status::kOutOfMemory;
    }
    impl.null_template_cpu = impl.null_template_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t i = 0; i < GuestHeapLayout::kSpace1Count; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = impl.null_template_cpu;
        h.ptr += static_cast<SIZE_T>(i) * impl.cbv_stride;
        impl.WriteNullSlotToHandle(h, i);
    }

    // Populate the three permanent static null space-1 regions (fallback when a
    // shader has zero space-1 plans) directly from the CPU null template heap.
    for (std::uint32_t stage = 0; stage < GuestHeapLayout::kStageRegions; ++stage) {
        const std::uint32_t base = impl.arena_base + GuestHeapLayout::StageRegion(stage);
        device.Get()->CopyDescriptorsSimple(GuestHeapLayout::kSpace1Count, impl.SlotHandleAt(base),
                                            impl.null_template_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // s0..s63 in the sampler heap: one linear-clamp sampler repeated.
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    for (std::uint32_t stage = 0; stage < GuestHeapLayout::kStageRegions; ++stage) {
        for (std::uint32_t i = 0; i < GuestHeapLayout::kSamplerCount; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = device.SamplerHeap()->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(impl.sampler_base +
                                              stage * GuestHeapLayout::kSamplerCount + i) * impl.sampler_stride;
            device.Get()->CreateSampler(&sampler, handle);
        }
    }

    // The StageParams ring: kParamSlots * 512 B (1 MiB) of persistently-mapped
    // upload memory, fence-tracked in 4 slices.
    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(Impl::kParamSlots) * d3d12::kStageParamsBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device.Get()->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(impl.params.GetAddressOf())))) {
        LogMessage(Level::kError, "guest compute: the StageParams ring was not created");
        return d3d12::Status::kOutOfMemory;
    }
    if (FAILED(impl.params->Map(0, nullptr, &impl.params_mapped))) return d3d12::Status::kOutOfMemory;

    // GPU-side SRV snapshot ring (DEFAULT heap, resting state COPY_DEST) for
    // read-only space-1 t# buffer bindings without UAV/SRV state aliasing.
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC srv_desc{};
    srv_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    srv_desc.Width = Impl::kSrvRingBytes;
    srv_desc.Height = 1;
    srv_desc.DepthOrArraySize = 1;
    srv_desc.MipLevels = 1;
    srv_desc.SampleDesc.Count = 1;
    srv_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (device.Memory().Allocate(d3d12::Owner::kHeaps, Impl::kSrvRingBytes).ok) {
        if (SUCCEEDED(device.Get()->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &srv_desc,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(impl.srv_ring.GetAddressOf())))) {
            impl.srv_ring_budgeted = true;
        } else {
            device.Memory().Free(d3d12::Owner::kHeaps, Impl::kSrvRingBytes);
            LogMessage(Level::kWarn, "guest compute: SRV snapshot ring was not created; t# buffer plans will be refused");
        }
    } else {
        LogMessage(Level::kWarn, "guest compute: heaps budget full for 16 MiB SRV snapshot ring");
    }

    // DEFAULT-heap staging ring for ExecuteIndirect arguments (4 x 16 KiB).
    D3D12_RESOURCE_DESC ind_desc = srv_desc;
    ind_desc.Width = Impl::kIndirectStagingBytes;
    if (FAILED(device.Get()->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &ind_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(impl.indirect_staging.GetAddressOf())))) {
        LogMessage(Level::kWarn, "guest compute: indirect staging buffer was not created; indirect dispatches refused");
        impl.indirect_staging.Reset();
    }

    // ExecuteIndirect's command signature: one DISPATCH argument.
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = 3 * sizeof(std::uint32_t);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    if (FAILED(device.Get()->CreateCommandSignature(&signature, nullptr,
                                                    IID_PPV_ARGS(impl.indirect.GetAddressOf())))) {
        LogMessage(Level::kWarn, "guest compute: no indirect command signature; indirect dispatches are refused");
        impl.indirect.Reset();
    }

    impl.pso_budget_bytes = device.Memory().cap(d3d12::Owner::kShaderTools);
    impl.psos = std::make_unique<d3d12::PsoCache>(impl.pso_budget_bytes);
    LogMessage(Level::kInfo, "guest compute: ready (%s)", d3d12::GuestRootDescription(GuestStage::kCompute).c_str());
    return d3d12::Status::kOk;
}

void GuestCompute::Destroy() {
    if (impl_ == nullptr) return;
    Impl& impl = *impl_;
    // Stop and join background pre-warming threads before touching device or PSO maps.
    impl.StopPrewarm();
    impl.batch_open = false;
    impl.batch_dispatches = 0;
    impl.active_cmd = Impl::CommandSlot{};
    if (impl.device != nullptr) {
        (void)impl.device->WaitIdle();
    }
    for (ID3D12PipelineState* pso : impl.batch_retired_psos) {
        if (pso != nullptr) pso->Release();
    }
    impl.batch_retired_psos.clear();
    for (Impl::RetiredPso& retired : impl.retired_psos) {
        if (retired.pso != nullptr) retired.pso->Release();
    }
    impl.retired_psos.clear();
    impl.in_flight.clear();
    impl.free_lists.clear();
    for (auto& [key, pso] : impl.pso_objects) {
        (void)key;
        if (pso != nullptr) pso->Release();
    }
    impl.pso_objects.clear();
    impl.failed_psos.clear();
    impl.psos.reset();
    impl.pso_budget_bytes = 0;
    if (impl.srv_ring_budgeted && impl.device != nullptr) {
        impl.device->Memory().Free(d3d12::Owner::kHeaps, Impl::kSrvRingBytes);
        impl.srv_ring_budgeted = false;
    }
    impl.srv_ring.Reset();
    impl.srv_slice = 0;
    impl.srv_slice_cursor = 0;
    impl.srv_slice_fence.fill(0);
    impl.srv_slices_pending_mask = 0;
    impl.srv_ring_gen = 1;
    impl.indirect_staging.Reset();
    impl.indirect_slice = 0;
    impl.indirect_slice_cursor = 0;
    impl.indirect_slice_fence.fill(0);
    impl.indirect_slices_pending_mask = 0;
    if (impl.params != nullptr && impl.params_mapped != nullptr) {
        impl.params->Unmap(0, nullptr);
    }
    impl.params_mapped = nullptr;
    impl.params.Reset();
    impl.param_slice = 0;
    impl.param_slot_in_slice = 0;
    impl.param_slice_fence.fill(0);
    impl.param_slices_pending_mask = 0;
    impl.indirect.Reset();
    impl.null_template_heap.Reset();
    impl.null_template_cpu = {};
    impl.programs.clear();
    impl.keys.clear();
    impl.logged_binding_keys.clear();
    impl.batch_retired_slices.clear();
    impl.scratch_runs.clear();
    impl.paths = d3d12::PathTable{};
    impl.paths_tried = false;
    impl.cache_tried = false;
    impl.cache_dir.clear();
    impl.bound_key = 0;
    impl.bound_user_data.fill(0);
    impl.bound_slice = {};
    impl.bound_srv_ring_gen = 0;
    impl.bound_srv_copies.clear();
    impl.bound_valid = false;
    impl.device = nullptr;
    impl.pipelines = nullptr;
    impl.memory = nullptr;
    impl.descriptor_ring = nullptr;
    impl.root = nullptr;
}

bool GuestCompute::Ready() const { return impl_ != nullptr && impl_->root != nullptr; }

std::uint64_t GuestCompute::KeyFor(const GpuDispatch& d) {
    if (impl_ == nullptr || !Ready()) return 0;
    Impl& impl = *impl_;
    impl.LoadCache();
    impl.LoadPaths();
    const DispatchId id{d.code_va, d.rsrc1, d.rsrc2, d.threads[0], d.threads[1], d.threads[2]};
    if (const auto it = impl.keys.find(id); it != impl.keys.end()) return it->second;

    auto program = impl.programs.find(d.code_va);
    if (program == impl.programs.end()) {
        Program read;
        if (!ExtractProgram(d.code_va, &read)) {
            ++impl.counts.bad_program;
            return 0;
        }
        program = impl.programs.emplace(d.code_va, std::move(read)).first;
        const Program& decoded = program->second;
        if (decoded.table_slot >= 0) {
            std::string offsets;
            char one[16] = {};
            for (const std::uint32_t dw : decoded.table_reads_dw) {
                std::snprintf(one, sizeof(one), " 0x%x", dw * 4u);
                offsets += one;
            }
            LogMessage(Level::kInfo,
                       "guest compute: %s (0x%llx) resource table in user SGPR %d: %s; %zu read(s):%s",
                       decoded.name.c_str(), static_cast<unsigned long long>(d.code_va), decoded.table_slot,
                       decoded.table_evidence.c_str(), decoded.table_reads_dw.size(),
                       offsets.empty() ? " none" : offsets.c_str());
        } else {
            LogMessage(Level::kWarn, "guest compute: %s (0x%llx) has no resource table pointer in its user data",
                       decoded.name.c_str(), static_cast<unsigned long long>(d.code_va));
        }
    }
    ConsoleStageKey key;
    key.stage = Stage::kCompute;
    key.rsrc1 = d.rsrc1;
    key.rsrc2 = d.rsrc2;
    for (int i = 0; i < 3; ++i) key.threads[i] = d.threads[i];
    std::string why;
    if (!PredictPaths(program->second, d.rsrc1, d.rsrc2, &key, &why)) {
        if (++impl.counts.bad_program <= 4) {
            LogMessage(Level::kWarn, "guest compute: dispatch at 0x%llx has no key (%s)",
                       static_cast<unsigned long long>(d.code_va), why.c_str());
        }
        return 0;
    }
    const std::uint64_t built = d3d12::ConsoleStageKeyOf(program->second.words, key);
    impl.keys.emplace(id, built);
    std::uint32_t plans = 0;
    for (std::uint32_t binding = 0; binding < GuestHeapLayout::kSpace1Count; ++binding) {
        if (impl.paths.Find(built, binding) != nullptr) ++plans;
    }
    const Program& decoded = program->second;
    if (impl.paths.status() == d3d12::PathTableStatus::kOk) {
        if (plans == 0 && !decoded.table_reads_dw.empty()) {
            ++impl.counts.keys_without_plans;
            LogMessage(Level::kWarn,
                       "guest compute: key %016llx (%s): the program reads %zu dword(s) through its resource table "
                       "and the bake recorded no space-1 binding for it, so every t#/u# stays a null descriptor",
                       static_cast<unsigned long long>(built), decoded.name.c_str(), decoded.table_reads_dw.size());
        } else {
            LogMessage(Level::kInfo, "guest compute: key %016llx (%s): %u space-1 binding plan(s) from the bake",
                       static_cast<unsigned long long>(built), decoded.name.c_str(), plans);
        }
    }
    return built;
}

bool GuestCompute::Run(std::uint64_t key, const GpuDispatch& d) {
    if (impl_ == nullptr || !Ready() || impl_->device == nullptr) return false;
    Impl& impl = *impl_;
    impl.LoadCache();
    impl.LoadPaths();
    ++impl.clock;
    if (impl.cache.status() != d3d12::CacheStatus::kOk) {
        ++impl.counts.no_cache;
        return false;
    }

    // Check the live PSO table FIRST so cached dispatches skip both cache lookup
    // and zlib decompression completely.
    ID3D12PipelineState* pso = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl.pso_mutex);
        if (const auto it = impl.pso_objects.find(key); it != impl.pso_objects.end()) {
            pso = it->second;
            impl.psos->Acquire(PsoKey{key, 0}, impl.clock);
        } else if (impl.failed_psos.find(key) != impl.failed_psos.end()) {
            ++impl.counts.bad_pso;
            return false;
        }
    }
    if (pso == nullptr) {
        d3d12::BakedBlob blob{};
        if (!impl.cache.Lookup(key, &blob) || blob.data == nullptr || blob.raw_size == 0) {
            ++impl.counts.miss;
            return false;
        }
        std::vector<std::uint8_t> dxil(blob.raw_size);
        if (!Inflate(blob, &dxil)) {
            ++impl.counts.bad_blob;
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = impl.root;
        desc.CS.pShaderBytecode = dxil.data();
        desc.CS.BytecodeLength = dxil.size();
        ComPtr<ID3D12PipelineState> created;
        const HRESULT hr = impl.device->Get()->CreateComputePipelineState(
            &desc, IID_PPV_ARGS(created.GetAddressOf()));
        if (FAILED(hr)) {
            ++impl.counts.bad_pso;
            bool log_first = false;
            {
                std::lock_guard<std::mutex> lk(impl.pso_mutex);
                log_first = impl.failed_psos.insert(key).second;
            }
            if (log_first) {
                std::string sign_why;
                const bool signed_ok = d3d12::IsSignedDxbc(dxil, sign_why);
                const d3d12::DxilFeatures sfi0 = d3d12::DxilFeatureFlags(dxil);
                LogMessage(Level::kError,
                           "guest compute: CreateComputePipelineState for key %016llx failed 0x%08lX "
                           "(bytes=%zu, signed=%u%s%s, sfi0=%u:0x%x)",
                           static_cast<unsigned long long>(key), static_cast<unsigned long>(hr),
                           dxil.size(), signed_ok ? 1u : 0u, signed_ok ? "" : ":",
                           signed_ok ? "" : sign_why.c_str(), sfi0.present ? 1u : 0u, sfi0.flags);
            }
            return false;
        }
        std::lock_guard<std::mutex> lk(impl.pso_mutex);
        if (const auto it = impl.pso_objects.find(key); it != impl.pso_objects.end()) {
            // A pre-warming worker finished this same PSO concurrently.
            pso = it->second;
            impl.psos->Acquire(PsoKey{key, 0}, impl.clock);
        } else {
            pso = created.Detach();
            impl.pso_objects.emplace(key, pso);
            const std::uint64_t retire_fence = impl.device->SubmittedValue();
            for (const PsoKey& evicted : impl.psos->Insert(PsoKey{key, 0}, key, dxil.size(), impl.clock)) {
                if (const auto it = impl.pso_objects.find(evicted.lo); it != impl.pso_objects.end()) {
                    if (impl.batch_open) {
                        impl.batch_retired_psos.push_back(it->second);
                    } else {
                        impl.retired_psos.push_back({it->second, retire_fence});
                    }
                    impl.pso_objects.erase(it);
                    ++impl.counts.evicted;
                }
            }
            ++impl.counts.created;
        }
    }

    // Which workgroup counts: the packet's own, or the guest's memory through
    // ExecuteIndirect.
    const bool indirect = d.indirect_va != 0;
    if (indirect && (impl.indirect == nullptr || impl.indirect_staging == nullptr)) {
        ++impl.counts.no_indirect;
        return false;
    }

    // Resolve indirect args before touching ring cursors so a refusal leaves no
    // half-allocated staging slice.
    ID3D12Resource* indirect_slab = nullptr;
    std::uint64_t indirect_src_offset = 0;
    std::uint64_t indirect_dst_offset = 0;
    if (indirect) {
        impl.scratch_runs.clear();
        if (!d3d12::SlabRuns(impl.memory->Table(), d.indirect_va, 3 * sizeof(std::uint32_t), &impl.scratch_runs) ||
            impl.scratch_runs.size() != 1 || impl.scratch_runs.front().bytes < 3 * sizeof(std::uint32_t)) {
            ++impl.counts.no_indirect;
            return false;
        }
        const d3d12::GuestRun run = impl.scratch_runs.front();
        if (impl.memory->EnsureSlab(run.slab) != d3d12::Status::kOk) {
            ++impl.counts.no_indirect;
            return false;
        }
        indirect_slab = impl.memory->SlabBuffer(run.slab);
        if (indirect_slab == nullptr) {
            ++impl.counts.no_indirect;
            return false;
        }
        indirect_src_offset = run.offset;
        indirect_dst_offset = impl.ReserveIndirectOffset();
    }

    // StageParams: user_sgpr + gds_base from the 4-slice upload ring.
    const std::uint32_t slot = impl.ReserveParamSlot();
    auto* params = static_cast<std::uint8_t*>(impl.params_mapped) +
                   static_cast<std::size_t>(slot) * d3d12::kStageParamsBytes;
    std::memset(params, 0, d3d12::kStageParamsBytes);
    std::memcpy(params + 16, d.user_data, sizeof(d.user_data));
    if (d.gds_base != 0) {
        std::memcpy(params + 88, &d.gds_base, sizeof(d.gds_base));
    }

    // Ring-buffered space-1 slots for this dispatch (may flush if a ring wraps).
    const std::uint32_t space1_offset = impl.BindResources(key, d);
    std::memcpy(params + 160, impl.bound_cb_stride.data(), 64);
    std::memcpy(params + 224, impl.bound_cb_w3.data(), 64);

    // Ensure a batch command list is open, and re-mark this dispatch's ring
    // slices pending in case BindResources flushed the previous batch.
    if (!impl.BeginBatch()) {
        ++impl.counts.record_failed;
        return false;
    }
    impl.param_slices_pending_mask |=
        (1u << ((slot / Impl::kParamSlotsPerSlice) % Impl::kParamSlices));
    if (indirect) {
        impl.indirect_slices_pending_mask |=
            (1u << ((indirect_dst_offset / Impl::kIndirectSliceBytes) % Impl::kIndirectSlices));
    }
    for (const Impl::SrvCopy& copy : impl.bound_srv_copies) {
        impl.TouchSrvOffset(copy.dst_offset);
    }
    ID3D12GraphicsCommandList* list = impl.active_cmd.list.Get();

    const auto transition = [](ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    };

    // Snapshot any space-1 read-only t# slices from guest slabs into srv_ring,
    // and stage indirect dispatch arguments into indirect_staging, using stack
    // arrays (zero heap allocations per dispatch).
    ID3D12Resource* copy_slabs[33];
    std::uint32_t copy_slab_count = 0;
    const auto add_copy_slab = [&copy_slabs, &copy_slab_count](ID3D12Resource* res) {
        if (res == nullptr) return;
        for (std::uint32_t i = 0; i < copy_slab_count; ++i) {
            if (copy_slabs[i] == res) return;
        }
        if (copy_slab_count < 33) copy_slabs[copy_slab_count++] = res;
    };
    const bool has_srv_copies = !impl.bound_srv_copies.empty() && impl.srv_ring != nullptr;
    if (has_srv_copies) {
        for (const Impl::SrvCopy& copy : impl.bound_srv_copies) {
            add_copy_slab(impl.memory->SlabBuffer(copy.slab));
        }
    }
    if (indirect) add_copy_slab(indirect_slab);

    bool srv_ring_transitioned = false;
    if (copy_slab_count != 0) {
        D3D12_RESOURCE_BARRIER pre[33];
        for (std::uint32_t i = 0; i < copy_slab_count; ++i) {
            pre[i] = transition(copy_slabs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        list->ResourceBarrier(copy_slab_count, pre);

        if (has_srv_copies) {
            for (const Impl::SrvCopy& copy : impl.bound_srv_copies) {
                if (ID3D12Resource* slab = impl.memory->SlabBuffer(copy.slab)) {
                    list->CopyBufferRegion(impl.srv_ring.Get(), copy.dst_offset, slab, copy.src_offset, copy.bytes);
                }
            }
        }
        if (indirect) {
            list->CopyBufferRegion(impl.indirect_staging.Get(), indirect_dst_offset, indirect_slab, indirect_src_offset,
                                   3 * sizeof(std::uint32_t));
        }

        D3D12_RESOURCE_BARRIER post[35];
        std::uint32_t post_count = 0;
        for (std::uint32_t i = 0; i < copy_slab_count; ++i) {
            post[post_count++] =
                transition(copy_slabs[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (has_srv_copies) {
            post[post_count++] = transition(impl.srv_ring.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            srv_ring_transitioned = true;
        }
        if (indirect) {
            post[post_count++] = transition(impl.indirect_staging.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        }
        list->ResourceBarrier(post_count, post);
    }

    // Bind descriptor heaps, root signature, samplers, space3 table, PSO, and
    // space1 table only when they changed within this batch.
    if (!impl.batch_heaps_bound) {
        ID3D12DescriptorHeap* heaps[2] = {impl.device->CbvSrvUavHeap(), impl.device->SamplerHeap()};
        list->SetDescriptorHeaps(2, heaps);
        impl.batch_heaps_bound = true;
    }
    if (!impl.batch_root_bound) {
        list->SetComputeRootSignature(impl.root);
        impl.batch_root_bound = true;
    }
    if (impl.batch_pso != pso) {
        list->SetPipelineState(pso);
        impl.batch_pso = pso;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS where = impl.params->GetGPUVirtualAddress() +
                                            static_cast<UINT64>(slot) * d3d12::kStageParamsBytes;
    list->SetComputeRootConstantBufferView(0, where);
    if (impl.batch_space1_offset != space1_offset) {
        D3D12_GPU_DESCRIPTOR_HANDLE resources = impl.device->CbvSrvUavHeap()->GetGPUDescriptorHandleForHeapStart();
        resources.ptr += static_cast<UINT64>(space1_offset) * impl.cbv_stride;
        list->SetComputeRootDescriptorTable(1, resources);
        impl.batch_space1_offset = space1_offset;
    }
    if (!impl.batch_samplers_bound) {
        const std::uint32_t compute_samplers = impl.sampler_base + 2 * GuestHeapLayout::kSamplerCount;
        D3D12_GPU_DESCRIPTOR_HANDLE samplers = impl.device->SamplerHeap()->GetGPUDescriptorHandleForHeapStart();
        samplers.ptr += static_cast<UINT64>(compute_samplers) * impl.sampler_stride;
        list->SetComputeRootDescriptorTable(2, samplers);
        impl.batch_samplers_bound = true;
    }
    if (!impl.batch_space3_bound) {
        list->SetComputeRootDescriptorTable(3, impl.memory->TableHandle());
        impl.batch_space3_bound = true;
    }

    if (indirect) {
        list->ExecuteIndirect(impl.indirect.Get(), 1, impl.indirect_staging.Get(), indirect_dst_offset, nullptr, 0);
        ++impl.counts.indirect;
    } else {
        list->Dispatch(d.dim[0], d.dim[1], d.dim[2]);
    }

    // Restore staging buffers to COPY_DEST and issue a global UAV barrier so the
    // next dispatch/draw sees everything this dispatch wrote.
    D3D12_RESOURCE_BARRIER epilogue[3];
    std::uint32_t epilogue_count = 0;
    if (srv_ring_transitioned) {
        epilogue[epilogue_count++] = transition(impl.srv_ring.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (indirect) {
        epilogue[epilogue_count++] = transition(impl.indirect_staging.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                                D3D12_RESOURCE_STATE_COPY_DEST);
    }
    D3D12_RESOURCE_BARRIER uav_barrier{};
    uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    uav_barrier.UAV.pResource = nullptr;
    epilogue[epilogue_count++] = uav_barrier;
    list->ResourceBarrier(epilogue_count, epilogue);
    ++impl.counts.uav_barriers;

    ++impl.counts.ran;
    ++impl.batch_dispatches;
    if (impl.batch_dispatches >= Impl::kMaxBatchDispatches) {
        impl.FlushBatch();
    }
    return true;
}

void GuestCompute::Flush() {
    if (impl_ == nullptr) return;
    impl_->FlushBatch();
}

bool GuestCompute::HasPending() const {
    return impl_ != nullptr && impl_->batch_open;
}

void GuestCompute::InvalidatePrograms(std::uint64_t lo, std::uint64_t hi) {
    if (impl_ == nullptr || lo >= hi) return;
    Impl& impl = *impl_;
    bool any = false;
    for (auto it = impl.programs.begin(); it != impl.programs.end();) {
        const std::uint64_t prog_lo = it->first;
        const std::uint64_t prog_hi =
            prog_lo + static_cast<std::uint64_t>(it->second.words.size()) * sizeof(std::uint32_t);
        if (prog_lo < hi && prog_hi > lo) {
            it = impl.programs.erase(it);
            any = true;
        } else {
            ++it;
        }
    }
    if (any) {
        for (auto it = impl.keys.begin(); it != impl.keys.end();) {
            if (impl.programs.find(it->first.code_va) == impl.programs.end()) {
                it = impl.keys.erase(it);
            } else {
                ++it;
            }
        }
        impl.bound_valid = false;
    }
}

void GuestCompute::EnsureCacheLoaded() {
    if (impl_ == nullptr) return;
    impl_->LoadCache();
    impl_->LoadPaths();
}

const d3d12::ShaderCache& GuestCompute::Cache() const { return impl_->cache; }

const d3d12::PathTable& GuestCompute::Paths() const { return impl_->paths; }

bool GuestCompute::CacheReady() const {
    return impl_ != nullptr && impl_->cache.status() == d3d12::CacheStatus::kOk;
}

D3D12_CPU_DESCRIPTOR_HANDLE GuestCompute::NullTemplateCpu() const {
    return impl_ != nullptr ? impl_->null_template_cpu : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

ID3D12Resource* GuestCompute::SrvSnapshotRing() const {
    return impl_ != nullptr ? impl_->srv_ring.Get() : nullptr;
}

std::uint64_t GuestCompute::SrvRingGeneration() const {
    return impl_ != nullptr ? impl_->srv_ring_gen : 0;
}

bool GuestCompute::ReserveSrvSnapshot(std::uint64_t bytes, std::uint64_t* out_offset,
                                      FlushCallback flush_cb, void* flush_user) {
    if (impl_ == nullptr || out_offset == nullptr) return false;
    return impl_->ReserveSrvSlice(bytes, out_offset, flush_cb, flush_user);
}

void GuestCompute::TouchSrvSnapshotOffset(std::uint64_t offset) {
    if (impl_ == nullptr) return;
    impl_->TouchSrvOffset(offset);
}

void GuestCompute::MarkSrvSnapshotsSubmitted(std::uint64_t fence) {
    if (impl_ == nullptr) return;
    impl_->MarkSrvSnapshotsSubmitted(fence);
}

std::string GuestCompute::Report() const {
    if (impl_ == nullptr) return "guest compute: none";
    const Impl& impl = *impl_;
    char line[576];
    std::snprintf(line, sizeof(line),
                  "guest compute: %llu dispatch(es) in %llu batch(es), %llu indirect, %llu prewarmed, "
                  "%llu cache miss, %llu no cache, %llu bad program, %llu bad blob, %llu bad PSO, %llu evicted, "
                  "%llu record failure(s), %llu space-1 slot(s) written (%llu srv), %llu refused, %llu null, "
                  "%llu key(s) without plans",
                  static_cast<unsigned long long>(impl.counts.ran),
                  static_cast<unsigned long long>(impl.counts.batches),
                  static_cast<unsigned long long>(impl.counts.indirect),
                  static_cast<unsigned long long>(impl.prewarm_created.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(impl.counts.miss),
                  static_cast<unsigned long long>(impl.counts.no_cache),
                  static_cast<unsigned long long>(impl.counts.bad_program),
                  static_cast<unsigned long long>(impl.counts.bad_blob),
                  static_cast<unsigned long long>(impl.counts.bad_pso),
                  static_cast<unsigned long long>(impl.counts.evicted),
                  static_cast<unsigned long long>(impl.counts.record_failed),
                  static_cast<unsigned long long>(impl.counts.bindings_written),
                  static_cast<unsigned long long>(impl.counts.bindings_srv),
                  static_cast<unsigned long long>(impl.counts.bindings_refused),
                  static_cast<unsigned long long>(impl.counts.bindings_null),
                  static_cast<unsigned long long>(impl.counts.keys_without_plans));
    return impl.cache_line + "\n" + impl.paths_line + "\n" + line;
}

void GuestCompute::OnFrame(std::uint64_t completed) {
    if (impl_ == nullptr) return;
    impl_->ReclaimCompleted(completed);
    if (impl_->psos != nullptr) {
        std::lock_guard<std::mutex> lk(impl_->pso_mutex);
        const std::uint64_t retire_fence = impl_->device != nullptr ? impl_->device->SubmittedValue() : completed;
        for (const PsoKey& evicted : impl_->psos->Trim(0)) {
            if (const auto it = impl_->pso_objects.find(evicted.lo); it != impl_->pso_objects.end()) {
                if (impl_->batch_open) {
                    impl_->batch_retired_psos.push_back(it->second);
                } else {
                    impl_->retired_psos.push_back({it->second, retire_fence});
                }
                impl_->pso_objects.erase(it);
            }
        }
    }
    impl_->completed = completed;
}

}  // namespace bbx::platform
