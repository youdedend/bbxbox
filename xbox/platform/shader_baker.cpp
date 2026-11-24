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
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <zlib.h>

#include "gcn/container.h"
#include "gcn/half.h"
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
        const std::uint32_t opt_level = bbx::d3d12::ChooseDxcOptimizationLevel(source);
        const wchar_t* opt_flag = L"-O3";
        if (opt_level == 0u) {
            opt_flag = L"-O0";
        } else if (opt_level == 1u) {
            opt_flag = L"-O1";
        } else if (opt_level == 2u) {
            opt_flag = L"-O2";
        }
        const dxc_com::WChar* args[] = {L"-T", profile, L"-E", L"main", L"-HV", L"2021", opt_flag};
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
        const std::uint64_t raw_hash = bbx::Fnv1a64(span);
        const std::string dedup_key =
            name + ":" + std::to_string(type) + ":" + std::to_string(raw_hash);
        if (!seen.emplace(dedup_key, true).second) continue;
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
        const std::span<const std::uint8_t> span(elf.data() + s.offset, s.size);
        const std::uint64_t raw_hash = bbx::Fnv1a64(span);
        const std::string dedup_key =
            s.name + ":" + std::to_string(s.type) + ":" + std::to_string(raw_hash);
        if (!seen.emplace(dedup_key, true).second) continue;
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

int StageSlot(Stage stage) noexcept {
    if (stage == Stage::kVertex) return 0;
    if (stage == Stage::kPixel) return 1;
    return 2;
}

const char* StageShortName(int slot) noexcept {
    if (slot == 0) return "VS";
    if (slot == 1) return "PS";
    return "CS";
}

std::string FirstLineTrimmed(const std::string& text) {
    const std::size_t nl = text.find('\n');
    std::string line = (nl == std::string::npos) ? text : text.substr(0, nl);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.size() > 140) line.resize(140);
    return line;
}

bool AnalyseContainer(const RawContainerItem& item, AnalysedProgram& program, std::string& reason) {
    reason.clear();
    const Stage stage_kind = StageOfType(item.type);
    const ConsoleContainerStage stage =
        bbx::d3d12::ReadConsoleContainerStage(item.container, item.type);
    if (!stage.ok) {
        reason = stage.error.empty() ? "analyse: container stage" : ("analyse: " + stage.error);
        return false;
    }
    gcn::ShaderCode code;
    if (!gcn::shader_code(item.container, code) || code.type != item.type) {
        reason = "analyse: shader_code";
        return false;
    }

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
    if (!prog.errors.empty()) {
        reason = "decode: " + prog.errors.front().what;
        return false;
    }

    gcn::TranslateOptions paths_options;
    paths_options.stage = TranslateStage(stage_kind);
    paths_options.rsrc1 = stage.key.rsrc1;
    paths_options.rsrc2 = stage.key.rsrc2;
    paths_options.vs_out_cntl = stage_kind == Stage::kVertex ? stage.key.mask : 0;
    paths_options.ps_input_ena = stage_kind == Stage::kPixel ? stage.key.mask : 0;
    const gcn::TranslateResult paths = gcn::translate(prog, paths_options);
    if (!paths.errors.empty()) {
        reason = "translate: " + paths.errors.front();
        return false;
    }

    ConsoleStageKey key = stage.key;
    for (const gcn::PredictedImage& p : gcn::predict_image_dims(prog, paths)) {
        key.dims.push_back((p.dim << 1) | (p.arrayed ? 1u : 0u));
    }
    key.sampler_modes.assign(paths.samplers.size(), 0);
    if (stage_kind == Stage::kPixel) {
        std::string why;
        if (!bbx::d3d12::ConsolePsInputMap(stage, paths.ps_inputs, key.input_map, key.flat_mask,
                                          why)) {
            reason = why.empty() ? "ps_input_map" : ("ps_input_map: " + why);
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

std::string FormatStampHex(const bbx::d3d12::StampDigest& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const std::uint8_t b : digest) {
        out.push_back(kHex[(b >> 4) & 0xfu]);
        out.push_back(kHex[b & 0xfu]);
    }
    return out;
}

bool ValidateCacheDir(const std::string& dir, std::string* report,
                      std::size_t* out_stages = nullptr,
                      std::string* out_stamp = nullptr) {
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
    if (out_stages != nullptr) *out_stages = cache.entries();
    if (out_stamp != nullptr) *out_stamp = FormatStampHex(cache.stamp_digest());
    return true;
}

struct CompiledProgramOutput {
    bool ok = false;
    bool lifted = false;
    std::string reason;
    CacheEntry entry;
    std::vector<std::uint8_t> packed_blob;
    std::string bindings_fragment;
    std::size_t raw_dxil_bytes = 0;
    std::size_t binding_count = 0;
};

CompiledProgramOutput CompileOneProgram(const PackagedDxcCompiler& compiler,
                                        const AnalysedProgram& program) {
    CompiledProgramOutput out;
    const gcn::Program decoded = gcn::decode(program.words.data(), program.words.size());
    if (!decoded.errors.empty()) {
        out.reason = "decode: " + decoded.errors.front().what;
        return out;
    }

    const gcn::TranslateOptions tr_opts = MakeTranslateOptions(program);
    const gcn::TranslateResult translated = gcn::translate(decoded, tr_opts);
    if (!translated.errors.empty()) {
        out.reason = "translate: " + translated.errors.front();
        return out;
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
    out.lifted = lifted.ok();
    if (!out.lifted) {
        out.reason = "lift: " + (lifted.rejections.empty()
                                     ? std::string("not liftable to HLSL")
                                     : FirstLineTrimmed(lifted.rejections.front()));
        return out;
    }

    gcn::HlslOptions hlsl_opts;
    hlsl_opts.title = program.name;
    hlsl_opts.dxil_sm64 = true;
    const gcn::HlslResult hlsl = gcn::emit_hlsl(lifted.spirv, hlsl_opts);
    if (!hlsl.ok()) {
        out.reason = "hlsl: " + (hlsl.errors.empty() ? std::string("failed") : hlsl.errors.front());
        return out;
    }
    if (!hlsl.arguments.empty()) {
        out.reason = "hlsl: needs " + hlsl.arguments.front();
        return out;
    }

    std::string why;
    if (bbx::d3d12::Has64BitInteger(hlsl.source, why)) {
        out.reason = "hlsl: 64-bit type (" + FirstLineTrimmed(why) + ")";
        return out;
    }

    std::vector<std::uint8_t> dxil_obj;
    if (!compiler.Compile(program.stage, hlsl.source, dxil_obj, why)) {
        out.reason = "dxc: " + FirstLineTrimmed(why);
        return out;
    }
    if (!bbx::d3d12::IsSignedDxbc(dxil_obj, why)) {
        out.reason = "signature: " + FirstLineTrimmed(why);
        return out;
    }
    const DxilFeatures features = bbx::d3d12::DxilFeatureFlags(dxil_obj);
    if (bbx::d3d12::HasForbiddenConsoleFeature(features)) {
        char fbuf[64] = {};
        std::snprintf(fbuf, sizeof(fbuf), "dxil: forbidden SFI0 (present=%u flags=0x%x)",
                      static_cast<unsigned>(features.present), features.flags);
        out.reason = fbuf;
        return out;
    }

    std::vector<std::uint8_t> packed;
    if (!DeflateBlob(dxil_obj, packed)) {
        out.reason = "zlib: deflate failed";
        return out;
    }

    out.entry.key = program.key;
    out.entry.program_hash = program.program_hash;
    out.entry.bytecode_hash =
        bbx::Fnv1a64(std::span<const std::uint8_t>(dxil_obj.data(), dxil_obj.size()));
    out.entry.stage = program.stage;
    out.entry.fallback = false;
    out.entry.offset = 0;
    out.entry.size = static_cast<std::uint32_t>(packed.size());
    out.entry.raw_size = static_cast<std::uint32_t>(dxil_obj.size());
    out.packed_blob = std::move(packed);
    out.raw_dxil_bytes = dxil_obj.size();
    out.binding_count = bindings.size();

    char note[224] = {};
    std::snprintf(
        note, sizeof(note),
        "# key %016llx stage %u: %zu buffer binding(s) recorded, %u not (an address, not a V#), "
        "%zu image(s), %zu sampler(s)\n",
        static_cast<unsigned long long>(program.key), static_cast<unsigned>(program.stage),
        bindings.size(), pointer_bindings, translated.images.size(),
        translated.samplers.size());
    out.bindings_fragment = note;
    for (const BindingPlan& plan : bindings) {
        out.bindings_fragment += PathTable::FormatBinding(
            program.key, static_cast<std::uint32_t>(program.stage), plan);
        out.bindings_fragment += '\n';
    }
    out.ok = true;
    return out;
}

bool CopyFileAtomic(const std::string& src_path, const std::string& dst_path) {
    std::vector<std::uint8_t> bytes;
    if (!bbx::hostfs::ReadFileAll(src_path, &bytes)) return false;
    return WriteBytesAtomic(dst_path, bytes);
}

bool CopyTreeRecursive(const std::string& src_dir, const std::string& dst_dir,
                       std::size_t& copied_files, std::size_t& failed_files, int depth = 0) {
    if (depth > 8 || !bbx::hostfs::Exists(src_dir)) return false;
    (void)bbx::hostfs::MkDir(dst_dir, true);
    std::vector<bbx::hostfs::DirEntry> entries;
    if (!bbx::hostfs::ListDir(src_dir, &entries)) return false;
    for (const bbx::hostfs::DirEntry& e : entries) {
        if (e.name.empty() || e.name == "." || e.name == "..") continue;
        const std::string src_child = bbx::hostfs::JoinPath(src_dir, e.name);
        const std::string dst_child = bbx::hostfs::JoinPath(dst_dir, e.name);
        if (e.is_dir) {
            (void)CopyTreeRecursive(src_child, dst_child, copied_files, failed_files, depth + 1);
        } else {
            if (CopyFileAtomic(src_child, dst_child)) {
                ++copied_files;
            } else {
                ++failed_files;
            }
        }
    }
    return failed_files == 0;
}

}  // namespace

bool HasValidConsoleCache(const std::vector<std::string>& search_dirs, std::string* found_dir,
                          std::string* report, std::size_t* out_stages, std::string* out_stamp) {
    bool found_any = false;
    std::string best_dir;
    std::string best_report;
    std::size_t best_stages = 0;
    std::string best_stamp;
    for (const std::string& dir : search_dirs) {
        std::string local_report;
        std::size_t local_stages = 0;
        std::string local_stamp;
        if (ValidateCacheDir(dir, &local_report, &local_stages, &local_stamp)) {
            if (!found_any || local_stages > best_stages) {
                found_any = true;
                best_dir = dir;
                best_report = std::move(local_report);
                best_stages = local_stages;
                best_stamp = std::move(local_stamp);
            }
        }
    }
    if (!found_any) return false;
    if (found_dir != nullptr) *found_dir = std::move(best_dir);
    if (report != nullptr) *report = std::move(best_report);
    if (out_stages != nullptr) *out_stages = best_stages;
    if (out_stamp != nullptr) *out_stamp = std::move(best_stamp);
    return true;
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
    const std::string checkpoint_path =
        bbx::hostfs::JoinPath(req.out_dir, "bbx_shaders.checkpoint");
    Quarantine quarantine = Quarantine::Parse(ReadTextFile(quarantine_path));
    bool recovered_crash = false;
    if (bbx::hostfs::Exists(inflight_path)) {
        const std::string inflight_text = ReadTextFile(inflight_path);
        const bbx::d3d12::CrashRecovery recovery =
            bbx::d3d12::RecoverFromInflight(inflight_text, quarantine);
        if (recovery.found) {
            LogMessage(Level::kWarn, "shader_baker: %s", recovery.message.c_str());
            if (recovery.promoted) {
                (void)WriteTextAtomic(quarantine_path, quarantine.Serialize());
            }
        } else {
            const std::vector<bbx::d3d12::WorkerInflightSlot> multi_slots =
                bbx::d3d12::ParseMultiWorkerInflight(inflight_text);
            if (!multi_slots.empty()) {
                for (const bbx::d3d12::WorkerInflightSlot& slot : multi_slots) {
                    if (slot.active && slot.hash != 0) {
                        quarantine.Add(slot.hash);
                        LogMessage(Level::kWarn,
                                   "shader_baker: quarantined %016llx (%s/%s, item %u/%u) from interrupted worker %u",
                                   static_cast<unsigned long long>(slot.hash),
                                   slot.bundle.c_str(), slot.entry.c_str(),
                                   slot.index, slot.total, slot.worker_id);
                    }
                }
                (void)WriteTextAtomic(quarantine_path, quarantine.Serialize());
            } else {
                recovered_crash = true;
                LogMessage(Level::kWarn,
                           "shader_baker: previous multi-threaded bake did not finish cleanly; "
                           "retrying in single-thread isolation mode");
            }
        }
        (void)bbx::hostfs::Unlink(inflight_path);
    }
    if (req.force_rebuild && bbx::hostfs::Exists(checkpoint_path)) {
        (void)bbx::hostfs::Unlink(checkpoint_path);
    }

    // Console DXIL SM 6.4 dialect: no native 16-bit half rounding and no runtime
    // sample offsets (mirrors tools/bbx_bake_cache.cpp). Set once before threads.
    gcn::set_native_half_rtz(false);
    gcn::set_runtime_sample_offsets(false);

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
    const std::string dxc_version = compiler.version();
    const CacheStamp bake_stamp = MakeConsoleBakeStamp(dxc_version);
    const std::uint32_t workers = bbx::d3d12::ChooseBakeWorkerCount(
        std::thread::hardware_concurrency(), req.max_workers, recovered_crash);
    snap.workers = workers;
    LogMessage(Level::kInfo,
               "shader_baker: loaded packaged dxil.dll + dxcompiler.dll (v%s, %u worker%s%s)",
               dxc_version.c_str(), workers, workers == 1u ? "" : "s",
               recovered_crash ? ", single-thread isolation after crash" : "");

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
            result.summary = "shader_baker: cancelled while scanning";
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

    // --- 4. Analyse & deduplicate by console stage key (multi-threaded) ------
    struct StageStats {
        std::uint32_t containers = 0;
        std::uint32_t analysed = 0;
        std::uint32_t unique_keys = 0;
        std::uint32_t lifted = 0;
        std::uint32_t signed_ok = 0;
        std::uint32_t refused = 0;
    };
    StageStats stage_stats[3]{};
    std::map<std::string, std::uint32_t> refusal_reasons;

    struct AnalysedSlot {
        bool ok = false;
        Stage stage = Stage::kPixel;
        AnalysedProgram prog;
        std::string reason;
    };
    std::vector<AnalysedSlot> analysed_slots(items.size());
    std::atomic<std::size_t> next_analyse{0};
    std::atomic<std::uint32_t> analyse_done{0};
    std::atomic<std::uint32_t> analyse_refused_atomic{0};
    std::mutex analyse_progress_mu;

    auto analyse_worker = [&]() {
        for (;;) {
            if (progress.CancelRequested()) break;
            const std::size_t i = next_analyse.fetch_add(1, std::memory_order_relaxed);
            if (i >= items.size()) break;
            AnalysedSlot& out_slot = analysed_slots[i];
            out_slot.stage = StageOfType(items[i].type);
            out_slot.ok = AnalyseContainer(items[i], out_slot.prog, out_slot.reason);
            if (!out_slot.ok) {
                analyse_refused_atomic.fetch_add(1, std::memory_order_relaxed);
            }
            const std::uint32_t done_now =
                analyse_done.fetch_add(1, std::memory_order_relaxed) + 1u;
            if ((done_now & 31u) == 0u || done_now == static_cast<std::uint32_t>(items.size())) {
                std::lock_guard<std::mutex> lock(analyse_progress_mu);
                snap.phase = BakePhase::kAnalysing;
                snap.done = done_now;
                snap.total = static_cast<std::uint32_t>(items.size());
                snap.refused_count = analyse_refused_atomic.load(std::memory_order_relaxed);
                snap.workers = workers;
                snap.elapsed_seconds = elapsed_sec();
                snap.current_name = items[i].name;
                progress.Update(snap);
            }
        }
    };

    if (workers <= 1u || items.size() < 64u) {
        analyse_worker();
    } else {
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (std::uint32_t t = 0; t < workers; ++t) {
            pool.emplace_back(analyse_worker);
        }
        for (std::thread& th : pool) {
            if (th.joinable()) th.join();
        }
    }

    if (progress.CancelRequested()) {
        result.cancelled = true;
        result.summary = "shader_baker: cancelled while analysing";
        compiler.Unload();
        return result;
    }

    std::vector<AnalysedProgram> unique_programs;
    unique_programs.reserve(items.size());
    std::unordered_map<std::uint64_t, bool> seen_keys;
    std::uint32_t analyse_refused = 0;
    for (std::size_t i = 0; i < analysed_slots.size(); ++i) {
        AnalysedSlot& s = analysed_slots[i];
        const int st = StageSlot(s.stage);
        ++stage_stats[st].containers;
        if (!s.ok) {
            ++analyse_refused;
            ++stage_stats[st].refused;
            const std::string key = std::string(StageShortName(st)) + " | " +
                                    (s.reason.empty() ? std::string("analyse failed")
                                                      : FirstLineTrimmed(s.reason));
            ++refusal_reasons[key];
            continue;
        }
        ++stage_stats[st].analysed;
        if (seen_keys.emplace(s.prog.key, true).second) {
            ++stage_stats[st].unique_keys;
            unique_programs.push_back(std::move(s.prog));
        }
    }
    analysed_slots.clear();
    analysed_slots.shrink_to_fit();
    // Free raw container bytes before entering DXC to minimize peak RAM.
    items.clear();
    items.shrink_to_fit();

    LogMessage(Level::kInfo,
               "shader_baker: %zu unique shader key(s) to compile (%u skipped in analysis; "
               "VS=%u PS=%u CS=%u)",
               unique_programs.size(), analyse_refused, stage_stats[0].unique_keys,
               stage_stats[1].unique_keys, stage_stats[2].unique_keys);

    // --- 4b. Resume prior checkpoint if present and matching this stamp ------
    std::vector<CacheEntry> entries;
    std::vector<std::uint8_t> blobs;
    std::string bindings_text;
    std::unordered_set<std::uint64_t> resumed_keys;
    entries.reserve(unique_programs.size());

    std::uint32_t signed_count = 0;
    std::uint32_t resumed_count = 0;
    std::uint32_t compile_refused = analyse_refused;
    std::uint32_t quarantined_count = 0;
    std::size_t raw_dxil_bytes = 0;
    std::size_t planned_bindings = 0;

    if (!req.force_rebuild && bbx::hostfs::Exists(checkpoint_path)) {
        std::vector<std::uint8_t> cp_bytes;
        bbx::d3d12::BakeCheckpoint cp;
        if (bbx::hostfs::ReadFileAll(checkpoint_path, &cp_bytes) &&
            bbx::d3d12::ParseBakeCheckpoint(cp_bytes, bake_stamp, &cp) && !cp.entries.empty()) {
            entries = std::move(cp.entries);
            blobs = std::move(cp.blobs);
            bindings_text = std::move(cp.bindings_text);
            resumed_keys.reserve(entries.size());
            for (const CacheEntry& e : entries) {
                resumed_keys.insert(e.key);
                raw_dxil_bytes += e.raw_size;
                ++stage_stats[StageSlot(e.stage)].signed_ok;
            }
            signed_count = static_cast<std::uint32_t>(entries.size());
            resumed_count = signed_count;
            LogMessage(Level::kInfo,
                       "shader_baker: resumed %u compiled shader(s) from checkpoint",
                       resumed_count);
        } else {
            LogMessage(Level::kInfo,
                       "shader_baker: discarding stale or unreadable checkpoint");
            (void)bbx::hostfs::Unlink(checkpoint_path);
        }
    }

    // Build worklist of remaining un-compiled, non-quarantined shaders.
    std::vector<std::size_t> pending_indices;
    pending_indices.reserve(unique_programs.size());
    std::uint32_t completed_total = 0;
    for (std::size_t i = 0; i < unique_programs.size(); ++i) {
        const std::uint64_t key = unique_programs[i].key;
        if (resumed_keys.find(key) != resumed_keys.end()) {
            ++completed_total;
            continue;
        }
        if (quarantine.Contains(key)) {
            ++quarantined_count;
            ++completed_total;
            continue;
        }
        pending_indices.push_back(i);
    }

    // --- 5. Compile to signed DXIL SM 6.4 (multi-threaded with checkpoints) --
    constexpr std::uint32_t kCheckpointInterval = 250u;
    std::mutex merge_mu;
    std::atomic<std::size_t> next_work{0};
    std::uint32_t newly_signed_since_checkpoint = 0;
    std::uint32_t newly_processed_since_checkpoint = 0;
    std::vector<bbx::d3d12::WorkerInflightSlot> worker_slots(std::max<std::uint32_t>(1u, workers));
    for (std::uint32_t w = 0; w < worker_slots.size(); ++w) {
        worker_slots[w].worker_id = w;
    }

    const auto save_checkpoint_locked = [&]() {
        if (entries.empty()) return;
        const std::vector<std::uint8_t> cp_bytes = bbx::d3d12::SerializeBakeCheckpoint(
            bake_stamp, entries, blobs, bindings_text);
        if (WriteBytesAtomic(checkpoint_path, cp_bytes)) {
            LogMessage(Level::kInfo,
                       "shader_baker: checkpoint saved (%zu shaders, %u/%zu done, %.1f MiB)",
                       entries.size(), completed_total, unique_programs.size(),
                       static_cast<double>(cp_bytes.size()) / 1048576.0);
        }
    };

    const auto publish_progress_locked = [&](const std::string& current_name) {
        snap.phase = BakePhase::kCompiling;
        snap.done = completed_total;
        snap.total = static_cast<std::uint32_t>(unique_programs.size());
        snap.signed_count = signed_count;
        snap.refused_count = compile_refused;
        snap.quarantined_count = quarantined_count;
        snap.resumed_count = resumed_count;
        snap.workers = workers;
        snap.elapsed_seconds = elapsed_sec();
        snap.current_name = current_name;
        progress.Update(snap);
    };

    {
        std::lock_guard<std::mutex> lock(merge_mu);
        publish_progress_locked(pending_indices.empty()
                                    ? std::string("RESUMED")
                                    : unique_programs[pending_indices.front()].name);
    }

    auto worker_fn = [&](std::uint32_t worker_id) {
        for (;;) {
            if (progress.CancelRequested()) break;
            const std::size_t slot = next_work.fetch_add(1, std::memory_order_relaxed);
            if (slot >= pending_indices.size()) break;
            const std::size_t prog_idx = pending_indices[slot];
            const AnalysedProgram& program = unique_programs[prog_idx];

            {
                std::lock_guard<std::mutex> lock(merge_mu);
                if (worker_id < worker_slots.size()) {
                    bbx::d3d12::WorkerInflightSlot& ws = worker_slots[worker_id];
                    ws.active = true;
                    ws.hash = program.key;
                    ws.stage = program.stage;
                    ws.index = static_cast<std::uint32_t>(prog_idx + 1);
                    ws.total = static_cast<std::uint32_t>(unique_programs.size());
                    ws.name = program.name;
                    ws.bundle = program.bundle;
                    ws.entry = program.entry;
                }
                if (workers == 1u) {
                    Unit unit;
                    unit.hash = program.key;
                    unit.kind = ShaderKindOfStage(program.stage);
                    unit.bundle = program.bundle;
                    unit.entry = program.entry;
                    (void)WriteTextFile(
                        inflight_path,
                        bbx::d3d12::SerializeInflight(
                            unit, static_cast<std::uint32_t>(prog_idx + 1),
                            static_cast<std::uint32_t>(unique_programs.size())));
                } else {
                    (void)WriteTextFile(inflight_path,
                                        bbx::d3d12::SerializeMultiWorkerInflight(worker_slots));
                }
            }

            const auto t_shader_start = std::chrono::steady_clock::now();
            CompiledProgramOutput compiled = CompileOneProgram(compiler, program);
            const auto shader_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - t_shader_start)
                                       .count();

            std::lock_guard<std::mutex> lock(merge_mu);
            if (worker_id < worker_slots.size()) {
                worker_slots[worker_id].active = false;
            }
            if (shader_ms >= 1500) {
                LogMessage(Level::kInfo,
                           "shader_baker: slow shader %s (%s/%s, key %016llx, %zu words) took %lld ms (%s)",
                           program.name.c_str(), program.bundle.c_str(), program.entry.c_str(),
                           static_cast<unsigned long long>(program.key), program.words.size(),
                           static_cast<long long>(shader_ms),
                           compiled.ok ? "signed" : compiled.reason.c_str());
            }
            ++completed_total;
            ++newly_processed_since_checkpoint;
            const int st = StageSlot(program.stage);
            if (compiled.lifted) ++stage_stats[st].lifted;
            if (compiled.ok) {
                compiled.entry.offset = static_cast<std::uint32_t>(blobs.size());
                entries.push_back(compiled.entry);
                blobs.insert(blobs.end(), compiled.packed_blob.begin(), compiled.packed_blob.end());
                bindings_text += compiled.bindings_fragment;
                raw_dxil_bytes += compiled.raw_dxil_bytes;
                planned_bindings += compiled.binding_count;
                ++signed_count;
                ++stage_stats[st].signed_ok;
                ++newly_signed_since_checkpoint;
            } else {
                ++compile_refused;
                ++stage_stats[st].refused;
                const std::string key = std::string(StageShortName(st)) + " | " +
                                        (compiled.reason.empty()
                                             ? std::string("compile failed")
                                             : FirstLineTrimmed(compiled.reason));
                ++refusal_reasons[key];
            }
            if (newly_signed_since_checkpoint >= kCheckpointInterval ||
                (newly_processed_since_checkpoint >= kCheckpointInterval &&
                 newly_signed_since_checkpoint > 0u)) {
                newly_signed_since_checkpoint = 0;
                newly_processed_since_checkpoint = 0;
                save_checkpoint_locked();
            }
            if ((completed_total & 3u) == 0u || completed_total == unique_programs.size()) {
                publish_progress_locked(program.name);
            }
        }
    };

    if (workers <= 1u || pending_indices.size() < 4u) {
        worker_fn(0u);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (std::uint32_t t = 0; t < workers; ++t) {
            pool.emplace_back(worker_fn, t);
        }
        for (std::thread& th : pool) {
            if (th.joinable()) th.join();
        }
    }

    (void)bbx::hostfs::Unlink(inflight_path);

    if (progress.CancelRequested()) {
        if (!entries.empty()) {
            std::lock_guard<std::mutex> lock(merge_mu);
            save_checkpoint_locked();
        }
        compiler.Unload();
        result.cancelled = true;
        result.entries_written = signed_count;
        result.resumed_count = resumed_count;
        result.refused_count = compile_refused;
        result.quarantined_count = quarantined_count;
        result.elapsed_seconds = elapsed_sec();
        result.summary = "shader_baker: cancelled (" + std::to_string(signed_count) +
                         " shaders saved in checkpoint for resume)";
        LogMessage(Level::kInfo, "%s", result.summary.c_str());
        return result;
    }

    // --- 6. Unload dxcompiler.dll + dxil.dll BEFORE writing & booting game ---
    compiler.Unload();
    LogMessage(Level::kInfo,
               "shader_baker: unloaded dxcompiler.dll and dxil.dll (0 MiB compiler RAM for gameplay)");

    unique_programs.clear();
    unique_programs.shrink_to_fit();

    // Emit per-stage scorecard and top refusal reasons so console logs pinpoint
    // exactly why any shader was skipped or rejected by DXIL SM 6.4 lowering/DXC.
    for (int st = 0; st < 3; ++st) {
        LogMessage(Level::kInfo,
                   "shader_baker: stage %s: containers=%u analysed=%u keys=%u lifted=%u signed=%u refused=%u",
                   StageShortName(st), stage_stats[st].containers, stage_stats[st].analysed,
                   stage_stats[st].unique_keys, stage_stats[st].lifted, stage_stats[st].signed_ok,
                   stage_stats[st].refused);
    }
    if (!refusal_reasons.empty()) {
        std::vector<std::pair<std::uint32_t, std::string>> ranked;
        ranked.reserve(refusal_reasons.size());
        for (const auto& [reason, count] : refusal_reasons) {
            ranked.emplace_back(count, reason);
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) {
                      return a.first != b.first ? (a.first > b.first) : (a.second < b.second);
                  });
        const std::size_t limit = std::min<std::size_t>(ranked.size(), 12u);
        for (std::size_t i = 0; i < limit; ++i) {
            LogMessage(Level::kInfo, "shader_baker: refused [%u] %s",
                       ranked[i].first, ranked[i].second.c_str());
        }
    }

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
    snap.resumed_count = resumed_count;
    snap.elapsed_seconds = elapsed_sec();
    snap.current_name = "bbx_shaders.cache";
    progress.Update(snap);

    std::sort(entries.begin(), entries.end(),
              [](const CacheEntry& a, const CacheEntry& b) { return a.key < b.key; });
    const std::vector<std::uint8_t> cache_file = bbx::d3d12::SerializeCache(bake_stamp, entries, blobs);
    const std::string stamp_text = bake_stamp.Serialize();
    const std::string paths_text = std::string(PathTable::HeaderLine()) + "\n" +
                                   PathTable::FormatStamp(bake_stamp.Digest()) + "\n" + bindings_text;

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

    // Final cache verified: remove temporary checkpoint file.
    (void)bbx::hostfs::Unlink(checkpoint_path);

    result.ok = true;
    result.entries_written = signed_count;
    result.resumed_count = resumed_count;
    result.refused_count = compile_refused;
    result.quarantined_count = quarantined_count;
    result.elapsed_seconds = elapsed_sec();
    char summary_buf[256] = {};
    std::snprintf(summary_buf, sizeof(summary_buf),
                  "baked %u DXIL 6.4 shaders (%uT, %u resumed, %.1f -> %.1f MiB, %zu plans) in %us",
                  signed_count, workers, resumed_count,
                  static_cast<double>(raw_dxil_bytes) / 1048576.0,
                  static_cast<double>(cache_file.size()) / 1048576.0, planned_bindings,
                  result.elapsed_seconds);
    result.summary = summary_buf;
    LogMessage(Level::kInfo, "shader_baker: %s (%s)", result.summary.c_str(), verify_report.c_str());

    snap.phase = BakePhase::kDone;
    snap.elapsed_seconds = result.elapsed_seconds;
    snap.detail = result.summary;
    progress.Update(snap);
    return result;
}

bool SyncShaderCacheFiles(const std::string& src_dir, const std::string& dst_dir,
                          std::string* detail) {
    if (src_dir.empty() || dst_dir.empty()) {
        if (detail) *detail = "missing source or destination directory";
        return false;
    }
    std::string report;
    if (!ValidateCacheDir(src_dir, &report)) {
        if (detail) *detail = "no valid shader cache found in " + src_dir;
        return false;
    }
    (void)bbx::hostfs::MkDir(dst_dir, true);

    const std::string src_cache = bbx::hostfs::JoinPath(src_dir, "bbx_shaders.cache");
    std::string src_stamp = src_cache + ".stamp.txt";
    if (!bbx::hostfs::Exists(src_stamp)) {
        src_stamp = bbx::hostfs::JoinPath(src_dir, "bbx_shaders.stamp");
    }
    const std::string src_paths = bbx::hostfs::JoinPath(src_dir, "bbx_shaders.paths");

    const std::string dst_cache = bbx::hostfs::JoinPath(dst_dir, "bbx_shaders.cache");
    const std::string dst_stamp = dst_cache + ".stamp.txt";
    const std::string dst_paths = bbx::hostfs::JoinPath(dst_dir, "bbx_shaders.paths");

    if (!CopyFileAtomic(src_cache, dst_cache) || !CopyFileAtomic(src_stamp, dst_stamp) ||
        !CopyFileAtomic(src_paths, dst_paths)) {
        if (detail) *detail = "failed writing shader cache files to " + dst_dir;
        return false;
    }
    if (!ValidateCacheDir(dst_dir, &report)) {
        if (detail) *detail = "copied cache failed verification in " + dst_dir;
        return false;
    }
    if (detail) *detail = "synced shader cache (" + report + ")";
    return true;
}

bool BackupSavesAndLogsToUsb(const std::string& local_state_dir, const std::string& data_dir,
                             const std::string& usb_root, std::string* detail) {
    if (usb_root.empty()) {
        if (detail) *detail = "no USB / dump folder selected";
        return false;
    }
    const std::string backup_root = bbx::hostfs::JoinPath(usb_root, "bbxbox_backups");
    (void)bbx::hostfs::MkDir(backup_root, true);

    std::size_t save_files = 0;
    std::size_t failed_files = 0;
    if (!data_dir.empty()) {
        const std::string save_src = bbx::hostfs::JoinPath(data_dir, "savedata");
        if (bbx::hostfs::Exists(save_src)) {
            const std::string save_dst = bbx::hostfs::JoinPath(backup_root, "savedata");
            (void)CopyTreeRecursive(save_src, save_dst, save_files, failed_files);
        }
    }

    std::size_t aux_files = 0;
    if (!local_state_dir.empty()) {
        const std::string logs_dst = bbx::hostfs::JoinPath(backup_root, "logs");
        (void)bbx::hostfs::MkDir(logs_dst, true);
        for (const char* name : {"loader.log", "errors.log", "crash.log", "memory-selftest.txt"}) {
            const std::string src = bbx::hostfs::JoinPath(local_state_dir, name);
            if (bbx::hostfs::Exists(src)) {
                if (CopyFileAtomic(src, bbx::hostfs::JoinPath(logs_dst, name))) {
                    ++aux_files;
                } else {
                    ++failed_files;
                }
            }
        }
        const std::string ini_src = bbx::hostfs::JoinPath(local_state_dir, "bbxbox.ini");
        if (bbx::hostfs::Exists(ini_src)) {
            if (CopyFileAtomic(ini_src, bbx::hostfs::JoinPath(backup_root, "bbxbox.ini"))) {
                ++aux_files;
            }
        }
    }

    if (save_files == 0 && aux_files == 0) {
        if (detail) *detail = "no save data or logs found to back up";
        return false;
    }
    char buf[192] = {};
    std::snprintf(buf, sizeof(buf),
                  "backed up %zu save file(s) and %zu log/config file(s) to bbxbox_backups",
                  save_files, aux_files);
    if (detail) *detail = buf;
    return failed_files == 0;
}

bool RestoreSavesFromUsb(const std::string& usb_root, const std::string& data_dir,
                         std::string* detail) {
    if (usb_root.empty() || data_dir.empty()) {
        if (detail) *detail = "missing USB or LocalState data folder";
        return false;
    }
    const std::string save_src =
        bbx::hostfs::JoinPath(bbx::hostfs::JoinPath(usb_root, "bbxbox_backups"), "savedata");
    if (!bbx::hostfs::Exists(save_src)) {
        if (detail) *detail = "no bbxbox_backups\\savedata folder found on USB";
        return false;
    }
    const std::string save_dst = bbx::hostfs::JoinPath(data_dir, "savedata");
    std::size_t copied = 0;
    std::size_t failed = 0;
    const bool ok = CopyTreeRecursive(save_src, save_dst, copied, failed);
    if (copied == 0) {
        if (detail) *detail = "backup savedata folder was empty";
        return false;
    }
    char buf[160] = {};
    std::snprintf(buf, sizeof(buf), "restored %zu save file(s) into LocalState\\data\\savedata",
                  copied);
    if (detail) *detail = buf;
    return ok;
}

}  // namespace bbx::platform
