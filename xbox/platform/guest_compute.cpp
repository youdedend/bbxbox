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
// What is still missing on purpose (Slice 5B-2b), and counted and logged rather
// than guessed: resolving a space-1 descriptor from the dispatch's user data
// (which V#/T# a baked binding names is the translator's `ResourcePath`, and
// binding a wrong buffer is worse than binding none), so the space-1 slots are
// null descriptors today; the graphics half (`host_gpu_draw`); and the sampled
// resources. Every dispatch this file cannot run says so once by key with the
// reason, in host_gpu_d3d12.cpp's log and report.
#include "xbox/platform/guest_compute.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "gcn/isa.h"
#include "gcn/translate.h"
#include "hle/fs.h"
#include "hle/modules.h"
#include "host/shader_patch.h"
#include "src/host/d3d12/bbx_cache.h"
#include "src/host/d3d12/bbx_guest_root.h"
#include "src/host/d3d12/bbx_psocache.h"
#include "src/host/d3d12/bbx_shader_cache.h"
#include "src/host/d3d12/device.h"
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
    for (std::size_t i = 0; i + 32 <= limit; i += 4) {
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

}  // namespace

struct GuestCompute::Impl {
    d3d12::Device* device = nullptr;
    d3d12::PipelineSet* pipelines = nullptr;
    d3d12::GuestMemory* memory = nullptr;
    ID3D12RootSignature* root = nullptr;  // the compute signature (bbx_guest_root.h)
    std::uint32_t arena_base = 0;         // the guest arena's first descriptor (space 3)
    std::uint32_t sampler_base = 0;
    std::uint32_t cbv_stride = 0, sampler_stride = 0;

    ShaderCache cache;
    bool cache_tried = false;
    std::string cache_line = "shader cache: not loaded";

    std::unordered_map<std::uint64_t, Program> programs;              // code VA -> program
    std::map<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>,
             std::uint64_t> keys;                                      // (va, rsrc1, rsrc2, x, y, z) -> key
    std::map<std::uint64_t, std::uint64_t> key_refusals;              // key -> how often it was asked for

    // The compute PSOs: the portable cache decides the budget and the eviction,
    // the device owns the objects.
    std::unique_ptr<d3d12::PsoCache> psos;
    std::unordered_map<std::uint64_t, ID3D12PipelineState*> pso_objects;
    std::uint64_t clock = 0;
    std::uint64_t completed = 0;  // the fence's last completed value (OnFrame)

    // The StageParams block: a ring of upload slots, one per dispatch, so a
    // dispatch recorded now never rewrites the constants of one still in flight.
    ComPtr<ID3D12Resource> params;
    void* params_mapped = nullptr;
    static constexpr std::uint32_t kParamSlots = 256;
    std::uint32_t params_next = 0;

    // ExecuteIndirect's signature: one DISPATCH argument, read from the guest's
    // own slab (the args are the guest's memory, not a copy).
    ComPtr<ID3D12CommandSignature> indirect;

    Counts counts;

    // --- cache -------------------------------------------------------------------
    void LoadCache() {
        if (cache_tried) return;
        cache_tried = true;
        const char* root = hle_fs_data_root();
        const std::string dir = root != nullptr ? root : "";
        if (dir.empty()) {
            cache_line = "shader cache: no data root yet; every guest dispatch is refused until the dump is mounted";
            LogMessage(Level::kWarn, "guest compute: %s", cache_line.c_str());
            return;
        }
        const std::string path = Join(dir, "bbx_shaders.cache");
        if (!bbx::hostfs::Exists(path)) {
            cache_line = "shader cache: " + path + " is missing; no guest dispatch can be built (run the bake)";
            LogMessage(Level::kWarn, "guest compute: %s", cache_line.c_str());
            return;
        }
        bbx::hostfs::FileStat stat{};
        if (!bbx::hostfs::Stat(path, &stat) || stat.size == 0 || stat.size > (256u << 20)) {
            cache_line = "shader cache: " + path + " is empty or implausible; refused";
            LogMessage(Level::kError, "guest compute: %s", cache_line.c_str());
            return;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.size));
        const int handle = bbx::hostfs::Open(path, bbx::hostfs::kRead);
        if (handle < 0) {
            cache_line = "shader cache: " + path + " would not open";
            LogMessage(Level::kError, "guest compute: %s", cache_line.c_str());
            return;
        }
        std::size_t read = 0;
        while (read < bytes.size()) {
            const std::int64_t got = bbx::hostfs::Read(handle, bytes.data() + read, bytes.size() - read);
            if (got <= 0) break;
            read += static_cast<std::size_t>(got);
        }
        (void)bbx::hostfs::Close(handle);
        if (read != bytes.size()) {
            cache_line = "shader cache: " + path + " read short";
            LogMessage(Level::kError, "guest compute: %s", cache_line.c_str());
            return;
        }
        // The stamp beside it: the bake's compiler and signer, which this build
        // pairs with its own target (dxil-6.4, tables, 11_0) and compares.
        std::string stamp;
        const std::string stamp_path = Join(dir, "bbx_shaders.stamp");
        if (bbx::hostfs::Exists(stamp_path)) {
            bbx::hostfs::FileStat stamp_stat{};
            if (bbx::hostfs::Stat(stamp_path, &stamp_stat) && stamp_stat.size < (1u << 16)) {
                stamp.resize(static_cast<std::size_t>(stamp_stat.size));
                const int sh = bbx::hostfs::Open(stamp_path, bbx::hostfs::kRead);
                if (sh >= 0) {
                    std::size_t got_total = 0;
                    while (got_total < stamp.size()) {
                        const std::int64_t got = bbx::hostfs::Read(sh, stamp.data() + got_total, stamp.size() - got_total);
                        if (got <= 0) break;
                        got_total += static_cast<std::size_t>(got);
                    }
                    (void)bbx::hostfs::Close(sh);
                    stamp.resize(got_total);
                }
            }
        }
        const d3d12::CacheStatus status = cache.Load(bytes, stamp);
        cache_line = "shader cache: " + path + ", " + cache.Report();
        if (status == d3d12::CacheStatus::kOk) {
            LogMessage(Level::kInfo, "guest compute: %s", cache_line.c_str());
        } else {
            LogMessage(Level::kError, "guest compute: %s (%s)", cache_line.c_str(),
                       d3d12::CacheStatusName(status));
        }
    }
};

GuestCompute::GuestCompute() : impl_(std::make_unique<Impl>()) {}
GuestCompute::~GuestCompute() { Destroy(); }

d3d12::Status GuestCompute::Create(d3d12::Device& device, d3d12::PipelineSet& pipelines,
                                   d3d12::GuestMemory& memory, std::uint32_t arena_base, std::uint32_t sampler_base) {
    if (impl_ == nullptr) return d3d12::Status::kUnavailable;
    Impl& impl = *impl_;
    impl.device = &device;
    impl.pipelines = &pipelines;
    impl.memory = &memory;
    impl.root = pipelines.GuestRootSignature(GuestStage::kCompute);
    impl.arena_base = arena_base;
    impl.sampler_base = sampler_base;
    impl.cbv_stride = device.CbvSrvUavStride();
    impl.sampler_stride = device.SamplerStride();
    if (impl.root == nullptr) {
        LogMessage(Level::kWarn, "guest compute: no compute root signature; dispatches will be refused");
        return d3d12::Status::kUnavailable;
    }

    // The space-1 regions start as null descriptors: a baked shader that reads
    // t7 before 5B-2b binds it must read "nothing bound", which is a defined
    // value in D3D12, rather than whatever descriptor was there before.
    // bbx_guest_root.h's rule 4: these are written straight into the device's one
    // shader-visible heap, because a view created there is legal and a copy from
    // a shader-visible heap is not.
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv{};
    null_srv.Format = DXGI_FORMAT_R32_UINT;
    null_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null_srv.Buffer.NumElements = 0;  // D3D12 reads a null resource as null when the format is given
    D3D12_UNORDERED_ACCESS_VIEW_DESC null_uav{};
    null_uav.Format = DXGI_FORMAT_R32_UINT;
    null_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    null_uav.Buffer.NumElements = 0;
    for (std::uint32_t stage = 0; stage < GuestHeapLayout::kStageRegions; ++stage) {
        const std::uint32_t base = impl.arena_base + GuestHeapLayout::StageRegion(stage);
        for (std::uint32_t i = 0; i < GuestHeapLayout::kSpace1Count; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = device.CbvSrvUavHeap()->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(base + i) * impl.cbv_stride;
            if (i >= GuestHeapLayout::kUavOffset) {
                device.Get()->CreateUnorderedAccessView(nullptr, nullptr, &null_uav, handle);
            } else if (i >= GuestHeapLayout::kCbvOffset && i < GuestHeapLayout::kSrvOffset) {
                // b1..b63 are constant buffers: a null CBV is a view with no
                // address, which the shader reads as zeroes.
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
                cbv.BufferLocation = 0;
                cbv.SizeInBytes = 0;
                device.Get()->CreateConstantBufferView(&cbv, handle);
            } else {
                device.Get()->CreateShaderResourceView(nullptr, &null_srv, handle);
            }
        }
    }
    // s0..s63 in the sampler heap: one linear-clamp sampler repeated. A shader
    // that samples before 5B-2b binds its S# gets a defined filter rather than a
    // descriptor that was never written.
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

    // The StageParams ring: kParamSlots * 512 B of upload memory, written by the
    // CPU per dispatch and read by the root CBV.
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

    // ExecuteIndirect's command signature: one DISPATCH argument (three
    // workgroup counts, 4-byte stride), so an indirect dispatch reads the guest's
    // own {x,y,z} where the title wrote it.
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = 3 * sizeof(std::uint32_t);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    if (FAILED(device.Get()->CreateCommandSignature(&signature, impl.root,
                                                    IID_PPV_ARGS(impl.indirect.GetAddressOf())))) {
        LogMessage(Level::kWarn, "guest compute: no indirect command signature; indirect dispatches are refused");
        impl.indirect.Reset();
    }

    impl.psos = std::make_unique<d3d12::PsoCache>(device.Memory().cap(d3d12::Owner::kShaderTools));
    LogMessage(Level::kInfo, "guest compute: ready (%s)", d3d12::GuestRootDescription(GuestStage::kCompute).c_str());
    return d3d12::Status::kOk;
}

void GuestCompute::Destroy() {
    if (impl_ == nullptr) return;
    Impl& impl = *impl_;
    impl.pso_objects.clear();
    impl.psos.reset();
    if (impl.params != nullptr && impl.params_mapped != nullptr) {
        impl.params->Unmap(0, nullptr);
    }
    impl.params_mapped = nullptr;
    impl.params.Reset();
    impl.indirect.Reset();
    impl.programs.clear();
    impl.keys.clear();
    impl.device = nullptr;
    impl.pipelines = nullptr;
    impl.memory = nullptr;
    impl.root = nullptr;
}

bool GuestCompute::Ready() const { return impl_ != nullptr && impl_->root != nullptr; }

std::uint64_t GuestCompute::KeyFor(const GpuDispatch& d) {
    if (impl_ == nullptr || !Ready()) return 0;
    Impl& impl = *impl_;
    impl.LoadCache();
    const auto id = std::make_tuple(d.code_va, d.rsrc1, d.rsrc2, d.threads[0], d.threads[1], d.threads[2]);
    if (const auto it = impl.keys.find(id); it != impl.keys.end()) return it->second;

    auto program = impl.programs.find(d.code_va);
    if (program == impl.programs.end()) {
        Program read;
        if (!ExtractProgram(d.code_va, &read)) {
            ++impl.counts.bad_program;
            return 0;
        }
        program = impl.programs.emplace(d.code_va, std::move(read)).first;
    }
    ConsoleStageKey key;
    key.stage = Stage::kCompute;
    key.rsrc1 = d.rsrc1;
    key.rsrc2 = d.rsrc2;
    for (int i = 0; i < 3; ++i) key.threads[i] = d.threads[i];
    std::string why;
    if (!PredictPaths(program->second, d.rsrc1, d.rsrc2, &key, &why)) {
        // A program the translator cannot read has no key in the bake either: it
        // was counted there and refused here, by name.
        ++impl.counts.bad_program;
        if (++impl.counts.bad_program <= 4) {
            LogMessage(Level::kWarn, "guest compute: dispatch at 0x%llx has no key (%s)",
                       static_cast<unsigned long long>(d.code_va), why.c_str());
        }
        return 0;
    }
    const std::uint64_t built = d3d12::ConsoleStageKeyOf(program->second.words, key);
    impl.keys.emplace(id, built);
    return built;
}

bool GuestCompute::Run(std::uint64_t key, const GpuDispatch& d) {
    if (impl_ == nullptr || !Ready() || impl_->device == nullptr) return false;
    Impl& impl = *impl_;
    impl.LoadCache();
    ++impl.clock;
    if (impl.cache.status() != d3d12::CacheStatus::kOk) {
        ++impl.counts.no_cache;
        return false;
    }
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

    // The PSO: created once per key, and the cache decides when one is evicted.
    ID3D12PipelineState* pso = nullptr;
    if (const auto it = impl.pso_objects.find(key); it != impl.pso_objects.end()) {
        pso = it->second;
        impl.psos->Acquire(PsoKey{key, 0}, impl.clock);
    } else {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = impl.root;
        desc.CS.pShaderBytecode = dxil.data();
        desc.CS.BytecodeLength = dxil.size();
        ComPtr<ID3D12PipelineState> created;
        const HRESULT hr = impl.device->Get()->CreateComputePipelineState(
            &desc, IID_PPV_ARGS(created.GetAddressOf()));
        if (FAILED(hr)) {
            ++impl.counts.bad_pso;
            LogMessage(Level::kError, "guest compute: CreateComputePipelineState for key %016llx failed 0x%08lX",
                       static_cast<unsigned long long>(key), static_cast<unsigned long>(hr));
            return false;
        }
        pso = created.Get();
        impl.pso_objects.emplace(key, pso);
        (void)created.Detach();  // the map owns it; Destroy releases them all
        for (const PsoKey& evicted : impl.psos->Insert(PsoKey{key, 0}, key, dxil.size(), impl.clock)) {
            if (const auto it = impl.pso_objects.find(evicted.lo); it != impl.pso_objects.end()) {
                it->second->Release();
                impl.pso_objects.erase(it);
                ++impl.counts.evicted;
            }
        }
        ++impl.counts.created;
    }

    // Which workgroup counts: the packet's own, or the guest's memory through
    // ExecuteIndirect.
    const bool indirect = d.indirect_va != 0;
    if (indirect && impl.indirect == nullptr) {
        ++impl.counts.no_indirect;
        return false;
    }

    // StageParams: user_sgpr only (see this file's header for why that is the
    // whole contract on the console), from a slot of the ring.
    const std::uint32_t slot = impl.params_next++ % Impl::kParamSlots;
    auto* params = static_cast<std::uint8_t*>(impl.params_mapped) + static_cast<std::size_t>(slot) * d3d12::kStageParamsBytes;
    std::memset(params, 0, d3d12::kStageParamsBytes);
    // StageParams::user_sgpr is at offset 16, after two 64-bit members
    // (gcn/translate.h's static_assert).
    std::memcpy(params + 16, d.user_data, sizeof(d.user_data));
    // GDS: the counters a dispatch's ds_append/ds_consume count in. Its words are
    // the guest's, so the addresses are the guest's; with no GDS base the field
    // stays zero, which the shader reads as "no counter".
    if (d.gds_base != 0) {
        std::memcpy(params + 88, &d.gds_base, sizeof(d.gds_base));
    }

    // Record it.
    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(impl.device->Get()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(allocator.GetAddressOf())))) {
        ++impl.counts.record_failed;
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(impl.device->Get()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                     IID_PPV_ARGS(list.GetAddressOf())))) {
        ++impl.counts.record_failed;
        return false;
    }
    // One CBV/SRV/UAV heap and one sampler heap, set once: the guest arena's
    // descriptors for both are in these two (bbx_guest_root.h's rule 4).
    ID3D12DescriptorHeap* heaps[2] = {impl.device->CbvSrvUavHeap(), impl.device->SamplerHeap()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetComputeRootSignature(impl.root);
    list->SetPipelineState(pso);
    const std::uint32_t compute_region = impl.arena_base + GuestHeapLayout::StageRegion(2);
    const std::uint32_t compute_samplers = impl.sampler_base + 2 * GuestHeapLayout::kSamplerCount;
    D3D12_GPU_DESCRIPTOR_HANDLE resources = impl.device->CbvSrvUavHeap()->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE samplers = impl.device->SamplerHeap()->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_VIRTUAL_ADDRESS where = impl.params->GetGPUVirtualAddress() +
                                      static_cast<UINT64>(slot) * d3d12::kStageParamsBytes;
    // The parameter order is the layout's: 0 the root CBV, 1 the resources table,
    // 2 the sampler table, 3 the guest memory (bbx_guest_root.cpp builds it).
    list->SetComputeRootConstantBufferView(0, where);
    resources.ptr += static_cast<UINT64>(compute_region) * impl.cbv_stride;
    list->SetComputeRootDescriptorTable(1, resources);
    samplers.ptr += static_cast<UINT64>(compute_samplers) * impl.sampler_stride;
    list->SetComputeRootDescriptorTable(2, samplers);
    list->SetComputeRootDescriptorTable(3, impl.memory->TableHandle());

    if (indirect) {
        // The args are the guest's own three dwords, in the slab the page table
        // puts them in: a D3D12 buffer is the slab, so the resource is the slab
        // and the offset is the page offset - no copy, and no second address
        // space for the title's memory.
        std::vector<d3d12::GuestRun> runs;
        if (!d3d12::SlabRuns(impl.memory->Table(), d.indirect_va, 3 * sizeof(std::uint32_t), &runs) ||
            runs.empty()) {
            ++impl.counts.no_indirect;
            return false;
        }
        const d3d12::GuestRun run = runs.front();
        if (impl.memory->EnsureSlab(run.slab) != d3d12::Status::kOk) {
            ++impl.counts.no_indirect;
            return false;
        }
        ID3D12Resource* slab = impl.memory->SlabBuffer(run.slab);
        if (slab == nullptr) {
            ++impl.counts.no_indirect;
            return false;
        }
        list->ExecuteIndirect(impl.indirect.Get(), 1, slab, run.offset, nullptr, 0);
        ++impl.counts.indirect;
    } else {
        list->Dispatch(d.dim[0], d.dim[1], d.dim[2]);
    }
    // A UAV barrier: the next dispatch reads what this one wrote. Compute passes
    // in a chain are the common case (GX's copy and clear passes), and a missing
    // barrier there is a frame that is wrong on some GPUs and right on others.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = nullptr;  // every UAV
    list->ResourceBarrier(1, &barrier);
    ++impl.counts.uav_barriers;
    if (FAILED(list->Close())) {
        ++impl.counts.record_failed;
        return false;
    }
    ID3D12CommandList* lists[1] = {list.Get()};
    impl.device->GraphicsQueue()->ExecuteCommandLists(1, lists);
    // No wait: the fence is the submission's, and gnm_exec's EOP/label waits and
    // the flip's are what ask about it (host_gpu_d3d12.cpp). The allocator and
    // the list are released when the last reference goes, which is after the
    // driver has taken the recording.
    ++impl.counts.ran;
    return true;
}

std::string GuestCompute::Report() const {
    if (impl_ == nullptr) return "guest compute: none";
    const Impl& impl = *impl_;
    char line[512];
    std::snprintf(line, sizeof(line),
                  "guest compute: %llu dispatch(es) run, %llu indirect, %llu cache miss, %llu no cache, "
                  "%llu bad program, %llu bad blob, %llu bad PSO, %llu evicted, %llu record failure(s)",
                  static_cast<unsigned long long>(impl.counts.ran),
                  static_cast<unsigned long long>(impl.counts.indirect),
                  static_cast<unsigned long long>(impl.counts.miss),
                  static_cast<unsigned long long>(impl.counts.no_cache),
                  static_cast<unsigned long long>(impl.counts.bad_program),
                  static_cast<unsigned long long>(impl.counts.bad_blob),
                  static_cast<unsigned long long>(impl.counts.bad_pso),
                  static_cast<unsigned long long>(impl.counts.evicted),
                  static_cast<unsigned long long>(impl.counts.record_failed));
    return impl.cache_line + "\n" + line;
}

void GuestCompute::OnFrame(std::uint64_t completed) {
    if (impl_ == nullptr || impl_->psos == nullptr) return;
    // The cache hands back the keys it evicted; their objects are this side's to
    // release (PsoCache stores ids, not COM pointers - bbx_psocache.h).
    for (const PsoKey& evicted : impl_->psos->Trim(0)) {
        if (const auto it = impl_->pso_objects.find(evicted.lo); it != impl_->pso_objects.end()) {
            it->second->Release();
            impl_->pso_objects.erase(it);
        }
    }
    impl_->completed = completed;
}

}  // namespace bbx::platform
