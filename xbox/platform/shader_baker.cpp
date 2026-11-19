// SPDX-License-Identifier: GPL-3.0-or-later
// On-console first-boot shader baker:
//
//   USB/LocalState dump (via bbx::hostfs)
//     -> .shaderbnd.dcx (DCX + BND4) + unwrapped eboot.bin (fSELF -> ELF)
//     -> gcn::decode -> translate -> lift -> dxil_sm64 HLSL
//     -> packaged dxcompiler.dll (-T *_6_4 -HV 2021 -O3) + dxil.dll signer
//     -> SFI0 console feature gate -> zlib -> atomic write to LocalState\data
//     -> FreeLibrary(dxcompiler.dll) + FreeLibrary(dxil.dll)
//
// Every file operation goes through `bbx::hostfs` (`CreateFile2FromApp` /
// `FindFirstFileExFromApp` on UWP) so USB dumps granted via FutureAccessList
// work identically to LocalState. No `std::filesystem`, no `std::fopen`, and
// no desktop DLL loader calls.
#include "xbox/platform/shader_baker.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zlib.h>

#include "gcn/container.h"
#include "gcn/hlsl.h"
#include "gcn/isa.h"
#include "gcn/lift.h"
#include "gcn/translate.h"
#include "host/shader_patch.h"
#include "src/host/core/bbx_self.h"
#include "src/host/d3d12/bbx_cache.h"
#include "src/host/d3d12/bbx_common.h"
#include "src/host/d3d12/bbx_path_table.h"
#include "src/host/d3d12/bbx_quarantine.h"
#include "src/host/d3d12/bbx_shader_bake.h"
#include "src/host/d3d12/bbx_shader_cache.h"
#include "xbox/platform/host_fs.h"
#include "xbox/platform/hostfs_extra.h"
#include "xbox/platform/log.h"

namespace bbx::platform {

// COM interfaces for dxcompiler.dll and dxil.dll declared at external linkage
// (outside an anonymous namespace so clang does not devirtualize pure virtuals
// into __cxa_pure_virtual).
namespace dxc_com {

struct Guid {
    std::uint32_t a;
    std::uint16_t b;
    std::uint16_t c;
    std::uint8_t d[8];
};

constexpr Guid kClsidCompiler = {
    0x73e22d93u, 0xe6ceu, 0x47f3u, {0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0}};
constexpr Guid kClsidValidator = {
    0x8ca3e215u, 0xf728u, 0x4cf3u, {0x8c, 0xdd, 0x88, 0xaf, 0x91, 0x75, 0x87, 0xa1}};
constexpr Guid kIidCompiler3 = {
    0x228b4687u, 0x5a6au, 0x4730u, {0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54}};
constexpr Guid kIidValidator = {
    0xa6e82bd2u, 0x1fd7u, 0x4826u, {0x98, 0x11, 0x28, 0x57, 0xe7, 0x97, 0xf4, 0x9a}};
constexpr Guid kIidOperationResult = {
    0xcedb484au, 0xd4e9u, 0x445au, {0xb9, 0x91, 0xca, 0x21, 0xca, 0x15, 0x7d, 0xc2}};
constexpr Guid kIidVersionInfo = {
    0xb04f5b50u, 0x2059u, 0x4f12u, {0xa8, 0xff, 0xa1, 0xe0, 0xcd, 0xe1, 0xcc, 0x7e}};

using HResult = std::int32_t;
using WChar = wchar_t;

struct Unknown {
    virtual HResult STDMETHODCALLTYPE QueryInterface(const Guid& iid, void** out) = 0;
    virtual unsigned long STDMETHODCALLTYPE AddRef() = 0;
    virtual unsigned long STDMETHODCALLTYPE Release() = 0;
};

struct Blob : Unknown {
    virtual void* STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual std::size_t STDMETHODCALLTYPE GetBufferSize() = 0;
};

struct BlobEncoding : Blob {
    virtual HResult STDMETHODCALLTYPE GetEncoding(BOOL* known, std::uint32_t* code_page) = 0;
};

struct OperationResult : Unknown {
    virtual HResult STDMETHODCALLTYPE GetStatus(HResult* status) = 0;
    virtual HResult STDMETHODCALLTYPE GetResult(Blob** result) = 0;
    virtual HResult STDMETHODCALLTYPE GetErrorBuffer(BlobEncoding** errors) = 0;
};

struct Buffer {
    const void* ptr;
    std::size_t size;
    std::uint32_t encoding;
};

struct Compiler3 : Unknown {
    virtual HResult STDMETHODCALLTYPE Compile(const Buffer* source, const WChar** args,
                                              std::uint32_t argc, void* include_handler,
                                              const Guid& iid, void** result) = 0;
    virtual HResult STDMETHODCALLTYPE Disassemble(const Buffer* object, const Guid& iid,
                                                  void** result) = 0;
};

struct Validator : Unknown {
    virtual HResult STDMETHODCALLTYPE Validate(Blob* shader, std::uint32_t flags,
                                               OperationResult** result) = 0;
};

struct VersionInfo : Unknown {
    virtual HResult STDMETHODCALLTYPE GetVersion(std::uint32_t* major, std::uint32_t* minor) = 0;
    virtual HResult STDMETHODCALLTYPE GetFlags(std::uint32_t* flags) = 0;
};

using CreateInstanceFn = HResult(STDAPICALLTYPE*)(const Guid& clsid, const Guid& iid, void** out);
constexpr std::uint32_t kCpUtf8 = 65001u;
constexpr std::uint32_t kValidatorInPlaceEdit = 1u;

}  // namespace dxc_com

namespace {

using bbx::d3d12::BakePhase;
using bbx::d3d12::BakeProgressSnapshot;
using bbx::d3d12::BindingKind;
using bbx::d3d12::BindingPlan;
using bbx::d3d12::CacheEntry;
using bbx::d3d12::CacheStamp;
using bbx::d3d12::CacheStatus;
using bbx::d3d12::ConsoleContainerStage;
using bbx::d3d12::ConsoleStageKey;
using bbx::d3d12::ConsoleStageKeyOf;
using bbx::d3d12::ConsoleStageOptions;
using bbx::d3d12::DxilFeatures;
using bbx::d3d12::EmbeddedContainerSpan;
using bbx::d3d12::PathStep;
using bbx::d3d12::PathTable;
using bbx::d3d12::PathTableStatus;
using bbx::d3d12::Quarantine;
using bbx::d3d12::ShaderCache;
using bbx::d3d12::ShaderKind;
using bbx::d3d12::Stage;
using bbx::d3d12::Unit;

template <typename T>
struct ComRef {
    T* p = nullptr;
    ~ComRef() { Reset(); }
    void Reset() {
        if (p != nullptr) {
            p->Release();
            p = nullptr;
        }
    }
    void** out() { return reinterpret_cast<void**>(&p); }
};

std::string ReadTextFile(const std::string& path) {
    std::vector<std::uint8_t> bytes;
    if (!bbx::hostfs::ReadFileAll(path, &bytes)) return {};
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool WriteTextFile(const std::string& path, const std::string& text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    return bbx::hostfs::WriteFileAll(path, bytes);
}

bool WriteBytesAtomic(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    const std::string tmp = path + ".tmp";
    if (!bbx::hostfs::WriteFileAll(tmp, bytes)) return false;
    if (!bbx::hostfs::Rename(tmp, path, true)) {
        (void)bbx::hostfs::Unlink(tmp);
        return false;
    }
    return true;
}

bool WriteTextAtomic(const std::string& path, const std::string& text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    return WriteBytesAtomic(path, bytes);
}

bool DeflateBlob(const std::vector<std::uint8_t>& raw, std::vector<std::uint8_t>& out) {
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    out.resize(static_cast<std::size_t>(size));
    const int z = compress2(out.data(), &size, raw.data(), static_cast<uLong>(raw.size()),
                            Z_BEST_COMPRESSION);
    if (z != Z_OK) {
        out.clear();
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    return true;
}

// Loads packaged dxil.dll FIRST, then dxcompiler.dll via LoadPackagedLibrary.
// Loading dxil.dll first guarantees dxcompiler.dll finds the packaged signer
// without hitting a loader-lock deadlock, and gives us an explicit
// IDxcValidator fallback if dxcompiler returns an unsigned DXBC container.
// Unload() releases every DLL handle so 0 MiB remains resident during gameplay.
class PackagedDxcCompiler {
public:
    ~PackagedDxcCompiler() { Unload(); }

    bool Load(std::string& error) {
        Unload();
        dxil_mod_ = ::LoadPackagedLibrary(L"dxil.dll", 0);
        if (dxil_mod_ == nullptr) {
            error = "LoadPackagedLibrary(dxil.dll) failed (win32=" +
                    std::to_string(static_cast<unsigned long>(::GetLastError())) + ")";
            return false;
        }
        dxc_mod_ = ::LoadPackagedLibrary(L"dxcompiler.dll", 0);
        if (dxc_mod_ == nullptr) {
            error = "LoadPackagedLibrary(dxcompiler.dll) failed (win32=" +
                    std::to_string(static_cast<unsigned long>(::GetLastError())) + ")";
            Unload();
            return false;
        }
        const FARPROC dxc_proc = ::GetProcAddress(dxc_mod_, "DxcCreateInstance");
        if (dxc_proc == nullptr) {
            error = "dxcompiler.dll has no DxcCreateInstance export";
            Unload();
            return false;
        }
        static_assert(sizeof(dxc_create_) == sizeof(dxc_proc));
        std::memcpy(&dxc_create_, &dxc_proc, sizeof(dxc_create_));
        const FARPROC dxil_proc = ::GetProcAddress(dxil_mod_, "DxcCreateInstance");
        if (dxil_proc != nullptr) {
            std::memcpy(&dxil_create_, &dxil_proc, sizeof(dxil_create_));
        }

        ComRef<dxc_com::Compiler3> compiler;
        if (dxc_create_(dxc_com::kClsidCompiler, dxc_com::kIidCompiler3, compiler.out()) != 0 ||
            compiler.p == nullptr) {
            error = "DxcCreateInstance(CLSID_DxcCompiler) failed";
            Unload();
            return false;
        }
        ComRef<dxc_com::VersionInfo> info;
        std::uint32_t major = 0;
        std::uint32_t minor = 0;
        if (compiler.p->QueryInterface(dxc_com::kIidVersionInfo, info.out()) == 0 &&
            info.p != nullptr) {
            (void)info.p->GetVersion(&major, &minor);
        }
        version_ = std::to_string(major) + "." + std::to_string(minor);
        return true;
    }

    void Unload() noexcept {
        dxc_create_ = nullptr;
        dxil_create_ = nullptr;
        if (dxc_mod_ != nullptr) {
            (void)::FreeLibrary(dxc_mod_);
            dxc_mod_ = nullptr;
        }
        if (dxil_mod_ != nullptr) {
            (void)::FreeLibrary(dxil_mod_);
            dxil_mod_ = nullptr;
        }
    }

    [[nodiscard]] const std::string& version() const noexcept { return version_; }

    bool Compile(Stage stage, const std::string& source, std::vector<std::uint8_t>& object,
                 std::string& error) const {
        object.clear();
        error.clear();
        if (dxc_create_ == nullptr) {
            error = "compiler not loaded";
            return false;
        }
        const wchar_t* profile = nullptr;
        switch (stage) {
            case Stage::kVertex: profile = L"vs_6_4"; break;
            case Stage::kPixel: profile = L"ps_6_4"; break;
            case Stage::kCompute: profile = L"cs_6_4"; break;
            default:
                error = "unsupported console shader stage";
                return false;
        }
        const dxc_com::WChar* args[] = {L"-T", profile, L"-E", L"main", L"-HV", L"2021", L"-O3"};
        ComRef<dxc_com::Compiler3> compiler;
        if (dxc_create_(dxc_com::kClsidCompiler, dxc_com::kIidCompiler3, compiler.out()) != 0 ||
            compiler.p == nullptr) {
            error = "no compiler instance";
            return false;
        }
        const dxc_com::Buffer src{source.data(), source.size(), dxc_com::kCpUtf8};
        ComRef<dxc_com::OperationResult> result;
        const dxc_com::HResult hr = compiler.p->Compile(
            &src, args, static_cast<std::uint32_t>(sizeof(args) / sizeof(args[0])), nullptr,
            dxc_com::kIidOperationResult, result.out());
        if (hr != 0 || result.p == nullptr) {
            error = "DXC Compile failed (" + std::to_string(hr) + ")";
            return false;
        }
        dxc_com::HResult status = -1;
        (void)result.p->GetStatus(&status);
        if (status != 0) {
            ComRef<dxc_com::BlobEncoding> errors;
            if (result.p->GetErrorBuffer(
                    reinterpret_cast<dxc_com::BlobEncoding**>(errors.out())) == 0 &&
                errors.p != nullptr && errors.p->GetBufferSize() > 0) {
                error.assign(static_cast<const char*>(errors.p->GetBufferPointer()),
                             errors.p->GetBufferSize());
                while (!error.empty() && (error.back() == '\0' || error.back() == '\n' ||
                                          error.back() == '\r')) {
                    error.pop_back();
                }
            }
            if (error.empty()) error = "DXC status " + std::to_string(status);
            return false;
        }
        ComRef<dxc_com::Blob> blob;
        if (result.p->GetResult(reinterpret_cast<dxc_com::Blob**>(blob.out())) != 0 ||
            blob.p == nullptr || blob.p->GetBufferSize() < 20) {
            error = "DXC returned no object";
            return false;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(blob.p->GetBufferPointer());
        const std::size_t size = blob.p->GetBufferSize();

        // If dxcompiler.dll returned an unsigned container inside AppContainer,
        // sign it explicitly via dxil.dll's IDxcValidator in place.
        std::string sign_why;
        if (!bbx::d3d12::IsSignedDxbc(std::span<const std::uint8_t>(bytes, size), sign_why) &&
            dxil_create_ != nullptr) {
            ComRef<dxc_com::Validator> validator;
            if (dxil_create_(dxc_com::kClsidValidator, dxc_com::kIidValidator, validator.out()) ==
                    0 &&
                validator.p != nullptr) {
                ComRef<dxc_com::OperationResult> val_res;
                const dxc_com::HResult vhr = validator.p->Validate(
                    blob.p, dxc_com::kValidatorInPlaceEdit,
                    reinterpret_cast<dxc_com::OperationResult**>(val_res.out()));
                dxc_com::HResult vstatus = -1;
                if (vhr == 0 && val_res.p != nullptr) {
                    (void)val_res.p->GetStatus(&vstatus);
                }
                if (vhr == 0 && vstatus == 0) {
                    ComRef<dxc_com::Blob> signed_blob;
                    if (val_res.p->GetResult(
                            reinterpret_cast<dxc_com::Blob**>(signed_blob.out())) == 0 &&
                        signed_blob.p != nullptr && signed_blob.p->GetBufferSize() >= 20) {
                        const auto* sb = static_cast<const std::uint8_t*>(
                            signed_blob.p->GetBufferPointer());
                        object.assign(sb, sb + signed_blob.p->GetBufferSize());
                        return true;
                    }
                    // In-place signing mutated blob.p directly.
                    const auto* updated =
                        static_cast<const std::uint8_t*>(blob.p->GetBufferPointer());
                    object.assign(updated, updated + blob.p->GetBufferSize());
                    return true;
                }
            }
        }

        object.assign(bytes, bytes + size);
        return true;
    }

private:
    HMODULE dxil_mod_ = nullptr;
    HMODULE dxc_mod_ = nullptr;
    dxc_com::CreateInstanceFn dxc_create_ = nullptr;
    dxc_com::CreateInstanceFn dxil_create_ = nullptr;
    std::string version_;
};

struct RawContainerItem {
    std::string name;
    std::uint8_t type = 0;
    std::vector<std::uint8_t> container;
    std::string bundle;
    std::string entry;
};

struct AnalysedProgram {
    std::uint64_t key = 0;
    Stage stage = Stage::kPixel;
    std::vector<std::uint32_t> words;
    std::uint64_t program_hash = 0;
    ConsoleStageOptions options;
    std::vector<gcn::VertexElement> vertex_input;
    std::string name;
    std::string bundle;
    std::string entry;
};

Stage StageOfType(std::uint8_t type) {
    switch (type) {
        case 1: return Stage::kVertex;
        case 2: return Stage::kPixel;
        case 4: return Stage::kCompute;
        default: return Stage::kVertex;
    }
}

ShaderKind ShaderKindOfStage(Stage stage) {
    switch (stage) {
        case Stage::kVertex: return ShaderKind::kVertex;
        case Stage::kPixel: return ShaderKind::kPixel;
        case Stage::kCompute: return ShaderKind::kCompute;
        default: return ShaderKind::kVertex;
    }
}

gcn::Stage TranslateStage(Stage stage) {
    return stage == Stage::kVertex   ? gcn::Stage::Vertex
           : stage == Stage::kPixel  ? gcn::Stage::Pixel
                                     : gcn::Stage::Compute;
}

gcn::TranslateOptions MakeTranslateOptions(const AnalysedProgram& program) {
    const ConsoleStageOptions& o = program.options;
    gcn::TranslateOptions t;
    t.stage = TranslateStage(o.stage);
    t.rsrc1 = o.rsrc1;
    t.rsrc2 = o.rsrc2;
    t.ps_input_ena = o.ps_input_ena;
    t.ps_flat_mask = o.ps_flat_mask;
    t.ps_clip_discard = o.ps_clip_discard;
    t.early_fragment_tests = o.early_fragment_tests;
    t.ps_input_map = o.ps_input_map;
    t.vs_out_cntl = o.vs_out_cntl;
    for (int i = 0; i < 3; ++i) t.cs_threads[i] = o.cs_threads[i];
    t.descriptor_set = ConsoleStageOptions::kDescriptorSet;
    t.bindless = ConsoleStageOptions::kBindless;
    t.cb_ssbo = ConsoleStageOptions::kCbSsbo;
    t.cb_no_fallback = ConsoleStageOptions::kNoFallback;
    t.console_memory = (o.stage == Stage::kCompute);
    for (const std::uint32_t d : o.image_dims) {
        t.image_dims.emplace_back(d >> 1, (d & 1u) != 0);
    }
    for (const std::uint8_t m : o.sampler_unnormalized) {
        t.sampler_force_unnormalized.push_back(m != 0);
    }
    if (o.stage == Stage::kVertex) {
        t.vertex_input = program.vertex_input;
        t.vertex_formats_from_params = !program.vertex_input.empty();
    }
    return t;
}

void CollectBundlePaths(const std::string& dir, std::vector<std::string>& out,
                        const BakeProgressState& progress, int depth = 0) {
    if (dir.empty() || depth > 10 || progress.CancelRequested()) return;
    std::vector<bbx::hostfs::DirEntry> entries;
    if (!bbx::hostfs::ListDir(dir, &entries)) return;
    std::sort(entries.begin(), entries.end(),
              [](const bbx::hostfs::DirEntry& a, const bbx::hostfs::DirEntry& b) {
                  return a.name < b.name;
              });
    for (const bbx::hostfs::DirEntry& e : entries) {
        if (progress.CancelRequested()) return;
        const std::string child = bbx::hostfs::JoinPath(dir, e.name);
        if (e.is_dir) {
            CollectBundlePaths(child, out, progress, depth + 1);
        } else if (bbx::d3d12::IsShaderBundleFilename(e.name)) {
            out.push_back(child);
        }
    }
}

std::string BaseName(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void AddBundleContainers(const std::string& path, std::vector<RawContainerItem>& items,
                         std::unordered_map<std::string, bool>& seen) {
    std::vector<std::uint8_t> raw;
    if (!bbx::hostfs::ReadFileAll(path, &raw) || raw.empty()) return;
    std::string why;
    const std::vector<std::uint8_t> plain = gcn::dcx_decompress(raw, &why);
    std::vector<gcn::BundleEntry> entries;
    if (plain.empty() || !gcn::bnd4_entries(plain, entries, &why)) return;
    const std::string bundle_name = BaseName(path);
    for (gcn::BundleEntry& entry : entries) {
        if (entry.data.size() < 0x60) continue;
        const std::span<const std::uint8_t> span(entry.data.data(), entry.data.size());
        const std::string name = bbx::d3d12::ContainerName(span);
        const int type = bbx::d3d12::ContainerType(span);
        if (name.empty() || (type != 1 && type != 2 && type != 4)) continue;
        if (!seen.emplace(name, true).second) continue;
        RawContainerItem item;
        item.name = name;
        item.type = static_cast<std::uint8_t>(type);
        item.container = std::move(entry.data);
        item.bundle = bundle_name;
        item.entry = entry.name.empty() ? name : entry.name;
        items.push_back(std::move(item));
    }
}

void AddEbootContainers(const std::string& eboot_path, std::vector<RawContainerItem>& items,
                        std::unordered_map<std::string, bool>& seen) {
    if (eboot_path.empty()) return;
    std::vector<std::uint8_t> raw;
    if (!bbx::hostfs::ReadFileAll(eboot_path, &raw) || raw.empty()) return;
    std::vector<std::uint8_t> elf;
    bbx::core::FselfResult unwrapped = bbx::core::UnwrapOrbisImage(raw);
    if (unwrapped.ok()) {
        elf = std::move(unwrapped.elf);
    } else {
        elf = std::move(raw);
    }
    const std::vector<EmbeddedContainerSpan> spans = bbx::d3d12::ScanEbootForContainers(elf);
    for (const EmbeddedContainerSpan& s : spans) {
        if (!seen.emplace(s.name, true).second) continue;
        RawContainerItem item;
        item.name = s.name;
        item.type = s.type;
        item.container.assign(elf.begin() + static_cast<std::ptrdiff_t>(s.offset),
                              elf.begin() + static_cast<std::ptrdiff_t>(s.offset + s.size));
        item.bundle = "eboot.bin";
        item.entry = s.name;
        items.push_back(std::move(item));
    }
}

bool AnalyseContainer(const RawContainerItem& item, AnalysedProgram& program) {
    const Stage stage_kind = StageOfType(item.type);
    const ConsoleContainerStage stage =
        bbx::d3d12::ReadConsoleContainerStage(item.container, item.type);
    if (!stage.ok) return false;
    gcn::ShaderCode code;
    if (!gcn::shader_code(item.container, code) || code.type != item.type) return false;

    program = AnalysedProgram{};
    program.stage = stage_kind;
    program.words = std::move(code.words);
    shader_patch_apply(item.name, program.words);
    program.program_hash = bbx::Fnv1a64(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(program.words.data()),
        program.words.size() * sizeof(std::uint32_t)));
    program.name = item.name;
    program.bundle = item.bundle;
    program.entry = item.entry;

    const gcn::Program prog = gcn::decode(program.words.data(), program.words.size());
    if (!prog.errors.empty()) return false;

    gcn::TranslateOptions paths_options;
    paths_options.stage = TranslateStage(stage_kind);
    paths_options.rsrc1 = stage.key.rsrc1;
    paths_options.rsrc2 = stage.key.rsrc2;
    const gcn::TranslateResult paths = gcn::translate(prog, paths_options);
    if (!paths.errors.empty()) return false;

    ConsoleStageKey key = stage.key;
    for (const gcn::PredictedImage& p : gcn::predict_image_dims(prog, paths)) {
        key.dims.push_back((p.dim << 1) | (p.arrayed ? 1u : 0u));
    }
    key.sampler_modes.assign(paths.samplers.size(), 0);
    if (stage_kind == Stage::kPixel) {
        std::string why;
        if (!bbx::d3d12::ConsolePsInputMap(stage, paths.ps_inputs, key.input_map, key.flat_mask,
                                          why)) {
            return false;
        }
        key.early_fragment_tests = false;
    }
    if (stage_kind == Stage::kVertex) {
        for (std::size_t k = 0; k < key.elements.size(); ++k) {
            gcn::VertexElement el;
            el.location = static_cast<std::uint32_t>(k);
            el.vdata = (key.elements[k] >> 8) & 0xffu;
            el.count = (key.elements[k] >> 16) & 0xffu;
            program.vertex_input.push_back(el);
        }
    }
    program.key = ConsoleStageKeyOf(program.words, key);
    program.options.stage = stage_kind;
    program.options.rsrc1 = key.rsrc1;
    program.options.rsrc2 = key.rsrc2;
    program.options.ps_input_ena = stage_kind == Stage::kPixel ? key.mask : 0;
    program.options.vs_out_cntl = stage_kind == Stage::kVertex ? key.mask : 0;
    program.options.ps_flat_mask = key.flat_mask;
    program.options.ps_clip_discard = key.clip_discard;
    program.options.early_fragment_tests = key.early_fragment_tests;
    program.options.ps_input_map = key.input_map;
    program.options.image_dims = key.dims;
    program.options.sampler_unnormalized = key.sampler_modes;
    for (int i = 0; i < 3; ++i) program.options.cs_threads[i] = key.threads[i];
    return true;
}

CacheStamp MakeConsoleBakeStamp(const std::string& dxc_version) {
    CacheStamp recorded;
    recorded.translator = "bbhost-xbox-bake";
    recorded.dxc_version = dxc_version;
    recorded.dxil_version = dxc_version;
    return bbx::d3d12::ExpectedConsoleStamp(recorded);
}

bool ValidateCacheDir(const std::string& dir, std::string* report) {
    if (dir.empty()) return false;
    const std::string cache_path = bbx::hostfs::JoinPath(dir, "bbx_shaders.cache");
    if (!bbx::hostfs::Exists(cache_path)) return false;

    std::string stamp_path = cache_path + ".stamp.txt";
    if (!bbx::hostfs::Exists(stamp_path)) {
        stamp_path = bbx::hostfs::JoinPath(dir, "bbx_shaders.stamp");
        if (!bbx::hostfs::Exists(stamp_path)) return false;
    }
    const std::string paths_path = bbx::hostfs::JoinPath(dir, "bbx_shaders.paths");
    if (!bbx::hostfs::Exists(paths_path)) return false;

    std::vector<std::uint8_t> cache_bytes;
    if (!bbx::hostfs::ReadFileAll(cache_path, &cache_bytes) || cache_bytes.empty()) return false;
    const std::string stamp_text = ReadTextFile(stamp_path);
    const std::string paths_text = ReadTextFile(paths_path);
    if (stamp_text.empty() || paths_text.empty()) return false;

    ShaderCache cache;
    if (cache.Load(cache_bytes, stamp_text) != CacheStatus::kOk) return false;
    PathTable table;
    if (table.Load(paths_text, cache.stamp_digest()) != PathTableStatus::kOk) return false;
    if (report != nullptr) {
        *report = cache.Report() + "; " + table.Report();
    }
    return true;
}

}  // namespace

bool HasValidConsoleCache(const std::vector<std::string>& search_dirs, std::string* found_dir,
                          std::string* report) {
    for (const std::string& dir : search_dirs) {
        std::string local_report;
        if (ValidateCacheDir(dir, &local_report)) {
            if (found_dir != nullptr) *found_dir = dir;
            if (report != nullptr) *report = local_report;
            return true;
        }
    }
    return false;
}

BakeResult BakeConsoleShaders(const BakeRequest& req, BakeProgressState& progress) {
    BakeResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_sec = [&started]() -> std::uint32_t {
        const auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
        return sec > 0 ? static_cast<std::uint32_t>(sec) : 0u;
    };

    BakeProgressSnapshot snap;
    snap.phase = BakePhase::kScanning;
    snap.detail = "CHECKING QUARANTINE AND LOADING DXC";
    progress.Update(snap);

    if (req.out_dir.empty() || req.app0.empty()) {
        result.summary = "shader_baker: missing dump or output directory";
        snap.phase = BakePhase::kFailed;
        snap.detail = result.summary;
        progress.Update(snap);
        return result;
    }
    (void)bbx::hostfs::MkDir(req.out_dir, true);

    // --- 1. Crash quarantine recovery ----------------------------------------
    const std::string inflight_path = bbx::hostfs::JoinPath(req.out_dir, "precompile_inflight.txt");
    const std::string quarantine_path =
        bbx::hostfs::JoinPath(req.out_dir, "precompile_quarantine.txt");
    Quarantine quarantine = Quarantine::Parse(ReadTextFile(quarantine_path));
    if (bbx::hostfs::Exists(inflight_path)) {
        const std::string inflight_text = ReadTextFile(inflight_path);
        const bbx::d3d12::CrashRecovery recovery =
            bbx::d3d12::RecoverFromInflight(inflight_text, quarantine);
        if (recovery.found) {
            LogMessage(Level::kWarn, "shader_baker: %s", recovery.message.c_str());
            if (recovery.promoted) {
                (void)WriteTextAtomic(quarantine_path, quarantine.Serialize());
            }
        }
        (void)bbx::hostfs::Unlink(inflight_path);
    }

    // --- 2. Load packaged dxil.dll + dxcompiler.dll --------------------------
    PackagedDxcCompiler compiler;
    std::string load_error;
    if (!compiler.Load(load_error)) {
        result.summary = "shader_baker: " + load_error;
        LogMessage(Level::kError, "%s", result.summary.c_str());
        snap.phase = BakePhase::kFailed;
        snap.elapsed_seconds = elapsed_sec();
        snap.detail = load_error;
        progress.Update(snap);
        return result;
    }
    LogMessage(Level::kInfo, "shader_baker: loaded packaged dxil.dll + dxcompiler.dll (v%s)",
               compiler.version().c_str());

    // --- 3. Discover shader bundles + eboot containers via hostfs ------------
    std::vector<std::string> bundle_paths;
    if (!req.update_dir.empty() && req.update_dir != req.app0) {
        CollectBundlePaths(req.update_dir, bundle_paths, progress);
    }
    CollectBundlePaths(req.app0, bundle_paths, progress);

    std::vector<RawContainerItem> items;
    std::unordered_map<std::string, bool> seen_containers;
    for (std::size_t i = 0; i < bundle_paths.size(); ++i) {
        if (progress.CancelRequested()) {
            result.cancelled = true;
            compiler.Unload();
            return result;
        }
        snap.phase = BakePhase::kScanning;
        snap.done = static_cast<std::uint32_t>(i + 1);
        snap.total = static_cast<std::uint32_t>(bundle_paths.size() + 1);
        snap.elapsed_seconds = elapsed_sec();
        snap.current_name = BaseName(bundle_paths[i]);
        progress.Update(snap);
        AddBundleContainers(bundle_paths[i], items, seen_containers);
    }
    AddEbootContainers(req.eboot, items, seen_containers);
    LogMessage(Level::kInfo,
               "shader_baker: discovered %zu unique shader container(s) across %zu bundle(s) + eboot",
               items.size(), bundle_paths.size());

    if (items.empty()) {
        compiler.Unload();
        result.summary = "shader_baker: no shader containers found in dump";
        LogMessage(Level::kError, "%s", result.summary.c_str());
        snap.phase = BakePhase::kFailed;
        snap.elapsed_seconds = elapsed_sec();
        snap.detail = result.summary;
        progress.Update(snap);
        return result;
    }

    // --- 4. Analyse & deduplicate by console stage key -----------------------
    std::vector<AnalysedProgram> unique_programs;
    unique_programs.reserve(items.size());
    std::unordered_map<std::uint64_t, bool> seen_keys;
    std::uint32_t analyse_refused = 0;

    for (std::size_t i = 0; i < items.size(); ++i) {
        if (progress.CancelRequested()) {
            result.cancelled = true;
            compiler.Unload();
            return result;
        }
        if ((i & 31u) == 0u || i + 1 == items.size()) {
            snap.phase = BakePhase::kAnalysing;
            snap.done = static_cast<std::uint32_t>(i + 1);
            snap.total = static_cast<std::uint32_t>(items.size());
            snap.refused_count = analyse_refused;
            snap.elapsed_seconds = elapsed_sec();
            snap.current_name = items[i].name;
            progress.Update(snap);
        }
        AnalysedProgram prog;
        if (!AnalyseContainer(items[i], prog)) {
            ++analyse_refused;
            continue;
        }
        if (seen_keys.emplace(prog.key, true).second) {
            unique_programs.push_back(std::move(prog));
        }
    }
    // Free raw container bytes before entering DXC to minimize peak RAM.
    items.clear();
    items.shrink_to_fit();

    LogMessage(Level::kInfo, "shader_baker: %zu unique shader key(s) to compile (%u skipped in analysis)",
               unique_programs.size(), analyse_refused);

    // --- 5. Compile to signed DXIL SM 6.4 (single-threaded for low RAM & exact crash quarantine) ---
    std::vector<CacheEntry> entries;
    std::vector<std::uint8_t> blobs;
    std::string bindings_text;
    entries.reserve(unique_programs.size());

    std::uint32_t signed_count = 0;
    std::uint32_t compile_refused = analyse_refused;
    std::uint32_t quarantined_count = 0;
    std::size_t raw_dxil_bytes = 0;
    std::size_t planned_bindings = 0;

    for (std::size_t i = 0; i < unique_programs.size(); ++i) {
        if (progress.CancelRequested()) {
            (void)bbx::hostfs::Unlink(inflight_path);
            result.cancelled = true;
            compiler.Unload();
            return result;
        }
        const AnalysedProgram& program = unique_programs[i];
        snap.phase = BakePhase::kCompiling;
        snap.done = static_cast<std::uint32_t>(i);
        snap.total = static_cast<std::uint32_t>(unique_programs.size());
        snap.signed_count = signed_count;
        snap.refused_count = compile_refused;
        snap.quarantined_count = quarantined_count;
        snap.elapsed_seconds = elapsed_sec();
        snap.current_name = program.name;
        progress.Update(snap);

        if (quarantine.Contains(program.key)) {
            ++quarantined_count;
            continue;
        }

        // Write crash breadcrumb before lifting + invoking DXC.
        Unit unit;
        unit.hash = program.key;
        unit.kind = ShaderKindOfStage(program.stage);
        unit.bundle = program.bundle;
        unit.entry = program.entry;
        (void)WriteTextFile(
            inflight_path,
            bbx::d3d12::SerializeInflight(unit, static_cast<std::uint32_t>(i + 1),
                                          static_cast<std::uint32_t>(unique_programs.size())));

        const gcn::Program decoded = gcn::decode(program.words.data(), program.words.size());
        if (!decoded.errors.empty()) {
            ++compile_refused;
            continue;
        }
        const gcn::TranslateOptions tr_opts = MakeTranslateOptions(program);
        const gcn::TranslateResult translated = gcn::translate(decoded, tr_opts);
        if (!translated.errors.empty()) {
            ++compile_refused;
            continue;
        }

        std::vector<BindingPlan> bindings;
        std::uint32_t pointer_bindings = 0;
        for (const gcn::BufferBinding& buffer : translated.buffers) {
            if (buffer.pointer) {
                ++pointer_bindings;
                continue;
            }
            BindingPlan plan;
            plan.binding = buffer.binding;
            plan.kind = buffer.written ? BindingKind::kUav : BindingKind::kSrv;
            plan.path.user_sgpr = buffer.path.user_sgpr;
            plan.path.final_offset_dw = buffer.path.final_offset_dw;
            plan.path.final_vsharp = buffer.path.final_vsharp;
            plan.path.immediate = buffer.path.immediate;
            for (const gcn::ResourceStep& step : buffer.path.loads) {
                plan.path.loads.push_back(PathStep{step.offset_dw, step.vsharp});
            }
            bindings.push_back(std::move(plan));
        }

        gcn::LiftResult lifted;
        switch (program.stage) {
            case Stage::kVertex:
                lifted = gcn::lift_vertex_shader(decoded, tr_opts, translated);
                break;
            case Stage::kPixel:
                lifted = gcn::lift_pixel_shader(decoded, tr_opts, translated);
                break;
            default:
                lifted = gcn::lift_compute_shader(decoded, tr_opts, translated);
                break;
        }
        const std::vector<std::uint32_t>& spv = lifted.ok() ? lifted.spirv : translated.spirv;

        gcn::HlslOptions hlsl_opts;
        hlsl_opts.title = program.name;
        hlsl_opts.dxil_sm64 = true;
        const gcn::HlslResult hlsl = gcn::emit_hlsl(spv, hlsl_opts);
        if (!hlsl.ok() || !hlsl.arguments.empty()) {
            ++compile_refused;
            continue;
        }

        std::string why;
        if (bbx::d3d12::Has64BitInteger(hlsl.source, why)) {
            ++compile_refused;
            continue;
        }

        std::vector<std::uint8_t> dxil_obj;
        if (!compiler.Compile(program.stage, hlsl.source, dxil_obj, why)) {
            ++compile_refused;
            continue;
        }
        if (!bbx::d3d12::IsSignedDxbc(dxil_obj, why)) {
            ++compile_refused;
            continue;
        }
        const DxilFeatures features = bbx::d3d12::DxilFeatureFlags(dxil_obj);
        if (bbx::d3d12::HasForbiddenConsoleFeature(features)) {
            ++compile_refused;
            continue;
        }

        std::vector<std::uint8_t> packed;
        if (!DeflateBlob(dxil_obj, packed)) {
            ++compile_refused;
            continue;
        }

        CacheEntry entry;
        entry.key = program.key;
        entry.program_hash = program.program_hash;
        entry.bytecode_hash =
            bbx::Fnv1a64(std::span<const std::uint8_t>(dxil_obj.data(), dxil_obj.size()));
        entry.stage = program.stage;
        entry.fallback = false;
        entry.offset = static_cast<std::uint32_t>(blobs.size());
        entry.size = static_cast<std::uint32_t>(packed.size());
        entry.raw_size = static_cast<std::uint32_t>(dxil_obj.size());
        entries.push_back(entry);
        blobs.insert(blobs.end(), packed.begin(), packed.end());
        raw_dxil_bytes += dxil_obj.size();

        char note[224] = {};
        std::snprintf(
            note, sizeof(note),
            "# key %016llx stage %u: %zu buffer binding(s) recorded, %u not (an address, not a V#), "
            "%zu image(s), %zu sampler(s)\n",
            static_cast<unsigned long long>(program.key), static_cast<unsigned>(program.stage),
            bindings.size(), pointer_bindings, translated.images.size(),
            translated.samplers.size());
        bindings_text += note;
        for (const BindingPlan& plan : bindings) {
            bindings_text += PathTable::FormatBinding(
                program.key, static_cast<std::uint32_t>(program.stage), plan);
            bindings_text += '\n';
        }
        planned_bindings += bindings.size();
        ++signed_count;
    }

    (void)bbx::hostfs::Unlink(inflight_path);

    // --- 6. Unload dxcompiler.dll + dxil.dll BEFORE writing & booting game ---
    const std::string dxc_version = compiler.version();
    compiler.Unload();
    LogMessage(Level::kInfo,
               "shader_baker: unloaded dxcompiler.dll and dxil.dll (0 MiB compiler RAM for gameplay)");

    unique_programs.clear();
    unique_programs.shrink_to_fit();

    if (entries.empty()) {
        result.refused_count = compile_refused;
        result.quarantined_count = quarantined_count;
        result.elapsed_seconds = elapsed_sec();
        result.summary = "shader_baker: no shaders signed (" + std::to_string(compile_refused) +
                         " refused, " + std::to_string(quarantined_count) + " quarantined)";
        LogMessage(Level::kError, "%s", result.summary.c_str());
        snap.phase = BakePhase::kFailed;
        snap.done = snap.total;
        snap.elapsed_seconds = result.elapsed_seconds;
        snap.detail = result.summary;
        progress.Update(snap);
        return result;
    }

    // --- 7. Sort, serialize, write atomically, and verify readback -----------
    snap.phase = BakePhase::kWriting;
    snap.done = snap.total;
    snap.signed_count = signed_count;
    snap.refused_count = compile_refused;
    snap.quarantined_count = quarantined_count;
    snap.elapsed_seconds = elapsed_sec();
    snap.current_name = "bbx_shaders.cache";
    progress.Update(snap);

    std::sort(entries.begin(), entries.end(),
              [](const CacheEntry& a, const CacheEntry& b) { return a.key < b.key; });
    const CacheStamp stamp = MakeConsoleBakeStamp(dxc_version);
    const std::vector<std::uint8_t> cache_file = bbx::d3d12::SerializeCache(stamp, entries, blobs);
    const std::string stamp_text = stamp.Serialize();
    const std::string paths_text = std::string(PathTable::HeaderLine()) + "\n" +
                                   PathTable::FormatStamp(stamp.Digest()) + "\n" + bindings_text;

    const std::string cache_path = bbx::hostfs::JoinPath(req.out_dir, "bbx_shaders.cache");
    const std::string stamp_path = cache_path + ".stamp.txt";
    const std::string paths_path = bbx::hostfs::JoinPath(req.out_dir, "bbx_shaders.paths");

    if (!WriteBytesAtomic(cache_path, cache_file) || !WriteTextAtomic(stamp_path, stamp_text) ||
        !WriteTextAtomic(paths_path, paths_text)) {
        result.elapsed_seconds = elapsed_sec();
        result.summary = "shader_baker: failed to write cache files into " + req.out_dir;
        LogMessage(Level::kError, "%s", result.summary.c_str());
        snap.phase = BakePhase::kFailed;
        snap.elapsed_seconds = result.elapsed_seconds;
        snap.detail = result.summary;
        progress.Update(snap);
        return result;
    }

    // Round-trip validation: prove the exact files on disk load cleanly.
    std::string verify_report;
    if (!ValidateCacheDir(req.out_dir, &verify_report)) {
        result.elapsed_seconds = elapsed_sec();
        result.summary = "shader_baker: written cache failed readback verification";
        LogMessage(Level::kError, "%s", result.summary.c_str());
        snap.phase = BakePhase::kFailed;
        snap.elapsed_seconds = result.elapsed_seconds;
        snap.detail = result.summary;
        progress.Update(snap);
        return result;
    }

    result.ok = true;
    result.entries_written = signed_count;
    result.refused_count = compile_refused;
    result.quarantined_count = quarantined_count;
    result.elapsed_seconds = elapsed_sec();
    char summary_buf[256] = {};
    std::snprintf(summary_buf, sizeof(summary_buf),
                  "baked %u DXIL 6.4 shaders (%.1f MiB -> %.1f MiB, %zu plans) in %us (%u skipped)",
                  signed_count, static_cast<double>(raw_dxil_bytes) / 1048576.0,
                  static_cast<double>(cache_file.size()) / 1048576.0, planned_bindings,
                  result.elapsed_seconds, compile_refused + quarantined_count);
    result.summary = summary_buf;
    LogMessage(Level::kInfo, "shader_baker: %s (%s)", result.summary.c_str(), verify_report.c_str());

    snap.phase = BakePhase::kDone;
    snap.elapsed_seconds = result.elapsed_seconds;
    snap.detail = result.summary;
    progress.Update(snap);
    return result;
}

}  // namespace bbx::platform
