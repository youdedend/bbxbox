// SPDX-License-Identifier: GPL-3.0-or-later
// tools/bbx_bake_cache.cpp: bakes <dump>/bbx_shaders.cache for the console.
//
// The console cannot compile at run time (no DXC, no SPIR-V, and a shader that
// compiles while the game waits is a hitch in every level), so every DXIL blob
// the UWP build will ever bind is made here, on a desktop, once per dump:
//
//   container  ->  gcn::decode  ->  translate  ->  lift  ->  HLSL  ->  DXC
//               ->  "DXBC" + a signed digest  ->  zlib  ->  bbx_shaders.cache
//
// What it reads: every *.shaderbnd.dcx / *.shaderbnddebug.dcx under the dump
// root (gcn::dcx_decompress, then gcn::bnd4_entries) and the dump's eboot.bin
// (unwrapped from its fake SELF first: the containers live in its segments).
// Both walks are the same ones host/shader_bundles.cpp makes when the game
// runs, which cannot be called from here because it reads through the file
// system's overlays.
//
// Keys: ConsoleStageKeyOf (host/d3d12/bbx_shader_cache.h) over the patched
// program and the stage facts the container itself states, so the launcher
// reaches the same key from the same container without a device, a draw or a
// frame. Containers that reach the same key are one module: the bake compiles
// it once, which is what makes the cache smaller than the sum of its shaders.
//
// It refuses rather than approximates. A program the translator cannot express
// without a fallback (the vk::RawBuffer* compute path, which the console's root
// constants do not bind yet), a container whose input table does not check out,
// and any blob without the "DXBC" magic and a non-zero digest at bytes 4..20 -
// an unsigned DXIL blob is not what the console driver accepts - are counted,
// by stage and reason, and kept out of the cache. There is no option to bake
// them anyway.
//
// Usage (from the repository root, over a dump root that holds app0/ and
// eboot.bin):
//
//   bbx_bake_cache --root /path/to/dump --eboot /path/to/dump/eboot.bin
//                  --dxc /path/to/dxc/lib/libdxcompiler.so --out /path/to/dump
//
// It writes bbx_shaders.cache and bbx_shaders.cache.stamp.txt (the sidecar the
// launcher reads beside it: host/d3d12/bbhost_bridge.cpp's LoadShaderCache),
// then loads what it wrote through ShaderCache::Load and looks every entry up
// again, so a cache that is not kOk is never reported as baked.
//
// --self-test runs the cache write, stamp, load and look-up over synthetic
// blobs in a temporary directory: it proves the file's plumbing without a dump,
// a compiler or any game data, and it is the only mode whose blobs are not
// real DXIL (they are never written outside its own temporary directory).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <zlib.h>

#include "host/settings.h"

#include "gcn/container.h"
#include "gcn/half.h"
#include "gcn/hlsl.h"
#include "gcn/isa.h"
#include "gcn/lift.h"
#include "gcn/translate.h"
#include "host/shader_patch.h"
#include "host/d3d12/bbx_cache.h"
#include "host/d3d12/bbx_common.h"
#include "host/d3d12/bbx_shader_cache.h"
#include "src/host/core/bbx_self.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// The COM interfaces live at file scope, outside the anonymous namespace:
// in it the compiler sees every class that could implement them - none -
// and devirtualizes the calls into __cxa_pure_virtual (the trap
// host/dxc.cpp documents).
// The few COM interfaces of dxcapi.h a DXIL compile needs, by their vtable
// order, as host/dxc.cpp declares them for the SPIR-V path: DXC's own headers
// are not part of the tree. External linkage, because in an anonymous namespace
// the compiler would see every class that could implement them - none - and
// devirtualize the calls into __cxa_pure_virtual.
namespace dxc_com {
struct Guid {
    std::uint32_t a;
    std::uint16_t b, c;
    std::uint8_t d[8];
};
constexpr Guid kClsidCompiler = {0x73e22d93, 0xe6ce, 0x47f3, {0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0}};
constexpr Guid kIidCompiler3 = {0x228b4687, 0x5a6a, 0x4730, {0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54}};
constexpr Guid kIidOperationResult = {0xcedb484a, 0xd4e9, 0x445a, {0xb9, 0x91, 0xca, 0x21, 0xca, 0x15, 0x7d, 0xc2}};
constexpr Guid kIidVersionInfo = {0xb04f5b50, 0x2059, 0x4f12, {0xa8, 0xff, 0xa1, 0xe0, 0xcd, 0xe1, 0xcc, 0x7e}};

using HResult = std::int32_t;
using WChar = wchar_t;

struct Unknown {
    virtual HResult QueryInterface(const Guid& iid, void** out) = 0;
    virtual unsigned long AddRef() = 0;
    virtual unsigned long Release() = 0;
};
struct Blob : Unknown {
    virtual void* GetBufferPointer() = 0;
    virtual std::size_t GetBufferSize() = 0;
};
struct BlobEncoding : Blob {
    virtual HResult GetEncoding(bool* known, std::uint32_t* code_page) = 0;
};
struct OperationResult : Unknown {
    virtual HResult GetStatus(HResult* status) = 0;
    virtual HResult GetResult(Blob** result) = 0;
    virtual HResult GetErrorBuffer(BlobEncoding** errors) = 0;
};
struct Buffer {
    const void* ptr;
    std::size_t size;
    std::uint32_t encoding;
};
struct Compiler3 : Unknown {
    virtual HResult Compile(const Buffer* source, const WChar** args, std::uint32_t argc, void* include_handler, const Guid& iid,
                            void** result) = 0;
    virtual HResult Disassemble(const Buffer* object, const Guid& iid, void** result) = 0;
};
struct VersionInfo : Unknown {
    virtual HResult GetVersion(std::uint32_t* major, std::uint32_t* minor) = 0;
    virtual HResult GetFlags(std::uint32_t* flags) = 0;
};
using CreateInstance = HResult (*)(const Guid& clsid, const Guid& iid, void** out);
constexpr std::uint32_t kCpUtf8 = 65001;
}  // namespace dxc_com

namespace {

namespace fs = std::filesystem;
using bbx::d3d12::CacheEntry;
using bbx::d3d12::CacheStamp;
using bbx::d3d12::ConsoleContainerStage;
using bbx::d3d12::ConsoleStageKey;
using bbx::d3d12::ConsoleStageKeyOf;
using bbx::d3d12::ConsoleStageOptions;
using bbx::d3d12::ShaderCache;
using bbx::d3d12::Stage;


// ---- the compiler -----------------------------------------------------------


template <typename T>
struct Ref {
    T* p = nullptr;
    ~Ref() {
        if (p) p->Release();
    }
    void** out() { return reinterpret_cast<void**>(&p); }
};

// libdxcompiler.so, loaded once and used from every thread: one compiler
// instance per compile (each Dxc may compile from several threads at once).
class Compiler {
public:
    ~Compiler() {
#ifdef _WIN32
        if (lib_) FreeLibrary(static_cast<HMODULE>(lib_));
#else
        if (lib_) dlclose(lib_);
#endif
    }

    bool Load(const std::string& library, const std::string& signer, std::string& error) {
        // The signer goes first: dxcompiler finds it beside itself, and a copy
        // loaded by name here is the one its own look-up resolves.
        if (!signer.empty()) {
            void* h = nullptr;
#ifdef _WIN32
            h = LoadLibraryW(fs::path(signer).wstring().c_str());
#else
            h = dlopen(signer.c_str(), RTLD_NOW | RTLD_GLOBAL);
#endif
            if (!h) {
                error = "cannot load the signer " + signer + SignerWhy();
                return false;
            }
            signer_ = h;
        }
#ifdef _WIN32
        lib_ = LoadLibraryW(fs::path(library).wstring().c_str());
#else
        lib_ = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (!lib_) {
            error = "cannot load " + library + SignerWhy();
            return false;
        }
#ifdef _WIN32
        create_ = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib_), "DxcCreateInstance"));
#else
        create_ = dlsym(lib_, "DxcCreateInstance");
#endif
        if (!create_) {
            error = library + ": no DxcCreateInstance";
            return false;
        }
        // Its version, for the stamp: a cache built by another compiler is not
        // this compiler's.
        const auto create = reinterpret_cast<dxc_com::CreateInstance>(create_);
        Ref<dxc_com::Compiler3> compiler;
        if (create(dxc_com::kClsidCompiler, dxc_com::kIidCompiler3, compiler.out()) != 0 || !compiler.p) {
            error = library + ": no compiler instance";
            return false;
        }
        Ref<dxc_com::VersionInfo> info;
        std::uint32_t major = 0, minor = 0;
        if (compiler.p->QueryInterface(dxc_com::kIidVersionInfo, info.out()) == 0 && info.p) info.p->GetVersion(&major, &minor);
        version_ = std::to_string(major) + "." + std::to_string(minor);
        return true;
    }

    const std::string& version() const { return version_; }

    // Compiles `source` to a DXBC container holding DXIL, with the console's
    // profile for `stage` and Shader Model 6.4's ceiling. False with `error`
    // holding DXC's own messages.
    bool Compile(Stage stage, const std::string& source, std::vector<std::uint8_t>& object, std::string& error) const {
        object.clear();
        error.clear();
        const std::string profile = stage == Stage::kVertex      ? "vs_6_4"
                                    : stage == Stage::kPixel     ? "ps_6_4"
                                    : stage == Stage::kCompute   ? "cs_6_4"
                                                                 : "";
        if (profile.empty()) {
            error = "the console has no profile for this stage";
            return false;
        }
        // The console's ceiling: -HV 2021 (SM 6.4's language), -O3, and no
        // -spirv, so the output is the driver's own container.
        std::vector<std::string> args = {"-T", profile, "-E", "main", "-HV", "2021", "-O3"};
        std::vector<std::wstring> wide;
        wide.reserve(args.size());
        for (const std::string& a : args) wide.push_back(std::wstring(a.begin(), a.end()));
        std::vector<const dxc_com::WChar*> argv;
        argv.reserve(wide.size());
        for (const std::wstring& a : wide) argv.push_back(a.c_str());
        const auto create = reinterpret_cast<dxc_com::CreateInstance>(create_);
        Ref<dxc_com::Compiler3> compiler;
        if (create(dxc_com::kClsidCompiler, dxc_com::kIidCompiler3, compiler.out()) != 0 || !compiler.p) {
            error = "no compiler instance";
            return false;
        }
        const dxc_com::Buffer src{source.data(), source.size(), dxc_com::kCpUtf8};
        Ref<dxc_com::OperationResult> result;
        const dxc_com::HResult hr =
            compiler.p->Compile(&src, argv.data(), static_cast<std::uint32_t>(argv.size()), nullptr, dxc_com::kIidOperationResult,
                                result.out());
        if (hr != 0 || !result.p) {
            error = "DXC failed to run (" + std::to_string(hr) + ")";
            return false;
        }
        dxc_com::HResult status = -1;
        result.p->GetStatus(&status);
        if (status != 0) {
            Ref<dxc_com::BlobEncoding> errors;
            if (result.p->GetErrorBuffer(reinterpret_cast<dxc_com::BlobEncoding**>(errors.out())) == 0 && errors.p && errors.p->GetBufferSize()) {
                error.assign(static_cast<const char*>(errors.p->GetBufferPointer()), errors.p->GetBufferSize());
                while (!error.empty() && (error.back() == '\0' || error.back() == '\n')) error.pop_back();
            }
            if (error.empty()) error = "DXC failed (" + std::to_string(status) + ")";
            return false;
        }
        Ref<dxc_com::Blob> blob;
        if (result.p->GetResult(reinterpret_cast<dxc_com::Blob**>(blob.out())) != 0 || !blob.p || blob.p->GetBufferSize() < 20) {
            error = "DXC returned no object";
            return false;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(blob.p->GetBufferPointer());
        object.assign(bytes, bytes + blob.p->GetBufferSize());
        return true;
    }

private:
    static std::string SignerWhy() {
#ifdef _WIN32
        return ": error " + std::to_string(GetLastError());
#else
        const char* e = dlerror();
        return e ? std::string(": ") + e : std::string();
#endif
    }

    void* lib_ = nullptr;
    void* signer_ = nullptr;
    void* create_ = nullptr;
    std::string version_;
};

// Whether a blob is what the console driver takes: the DXBC container's magic
// and a signed digest right after it. A DXIL blob DXC emitted without a signer
// has zeros there, which is why this is checked on every blob and not once.
bool IsSignedDxbc(const std::vector<std::uint8_t>& object, std::string& why) {
    if (object.size() < 20) {
        why = "shorter than a DXBC header";
        return false;
    }
    if (std::memcmp(object.data(), "DXBC", 4) != 0) {
        why = "not a DXBC container";
        return false;
    }
    std::uint32_t digest = 0;
    for (int i = 0; i < 16; ++i) digest |= object[4 + i];
    if (digest == 0) {
        why = "unsigned (the 16 bytes at 4..20 are zero: no dxil signer)";
        return false;
    }
    return true;
}

// ---- zlib ------------------------------------------------------------------

bool DeflateBlob(const std::vector<std::uint8_t>& raw, std::vector<std::uint8_t>& out) {
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    out.resize(size);
    const int z = compress2(out.data(), &size, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION);
    if (z != Z_OK) return false;
    out.resize(size);
    return true;
}

}  // namespace

namespace {

// ---- reading the dump ------------------------------------------------------

// The name host/shader_bundles.cpp gives a container: the hash in its OrbShdr
// footer, which is what GX's creation hook calls the shader and what
// shader_patch_apply matches against.
std::string ContainerName(const std::uint8_t* container, std::size_t size) {
    for (std::size_t i = size >= 20 ? size - 20 : 0;; --i) {  // the 28-byte footer sits at the end
        if (i + 20 <= size && std::memcmp(container + i, "OrbShdr", 7) == 0) {
            std::uint32_t h0 = 0;
            std::memcpy(&h0, container + i + 16, 4);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%08x", h0);
            return buf;
        }
        if (i == 0) break;
    }
    return {};
}

// Its Shdr type byte (1 vertex, 2 pixel, 4 compute), or -1: the same rule
// gcn::ShaderCode::type reports, read before the code is parsed.
int ContainerType(const std::uint8_t* container, std::size_t size) {
    for (std::size_t i = 0; i + 16 <= size && i < 0x100; ++i) {
        if (std::memcmp(container + i, "Shdr", 4) == 0) return container[i + 8] & 0xf;
    }
    return -1;
}

struct Item {
    std::string name;  // "%08x"
    std::uint8_t type = 0;
    std::vector<std::uint8_t> container;
    std::string source;  // the bundle and entry it came from, or "eboot.bin"
    bool debug = false;
};

// Every container of a .shaderbnd.dcx, as host/shader_bundles.cpp's
// read_bundles finds them: inflate the DCX, walk the BND4 archive's entries,
// and name each by its footer.
void AddBundle(const fs::path& path, std::vector<Item>& items, std::unordered_map<std::string, bool>& seen, std::string& error) {
    std::vector<std::uint8_t> raw;
    if (!gcn::read_file(path.string(), raw)) {
        error = path.string() + ": cannot be read";
        return;
    }
    std::string why;
    const std::vector<std::uint8_t> plain = gcn::dcx_decompress(raw, &why);
    std::vector<gcn::BundleEntry> entries;
    if (plain.empty() || !gcn::bnd4_entries(plain, entries, &why)) {
        error = path.string() + ": " + why;
        return;
    }
    const bool debug = path.string().find("debug") != std::string::npos;
    for (gcn::BundleEntry& entry : entries) {
        if (entry.data.size() < 0x60) continue;
        const std::string name = ContainerName(entry.data.data(), entry.data.size());
        const int type = ContainerType(entry.data.data(), entry.data.size());
        if (name.empty() || (type != 1 && type != 2 && type != 4)) continue;
        if (!seen.emplace(name, true).second) continue;  // a later bundle's copy of a shader already read
        Item item;
        item.name = name;
        item.type = static_cast<std::uint8_t>(type);
        item.container = std::move(entry.data);
        item.source = path.filename().string() + "\\" + entry.name;
        item.debug = debug;
        items.push_back(std::move(item));
    }
}

// The containers the eboot carries itself: host/shader_bundles.cpp's
// eboot_shaders walk, over the unwrapped ELF (a fake SELF's segments are where
// they sit; scanning the wrapper finds nothing when a segment is compressed).
void AddEboot(const std::string& path, std::vector<Item>& items, std::unordered_map<std::string, bool>& seen, std::string& error) {
    std::vector<std::uint8_t> d;
    bbx::core::FselfResult unwrapped = bbx::core::UnwrapOrbisFile(path.c_str());
    if (unwrapped.ok()) {
        d = std::move(unwrapped.elf);
    } else {
        std::vector<std::uint8_t> raw;
        if (gcn::read_file(path, raw)) d = std::move(raw);
        if (d.empty()) {
            error = path + ": " + unwrapped.why;
            return;
        }
    }
    std::size_t found = 0;
    for (std::size_t at = 0; at + 0x60 <= d.size(); ++at) {
        const auto* p = static_cast<const std::uint8_t*>(std::memchr(d.data() + at, 'S', d.size() - at));
        if (!p) break;
        at = static_cast<std::size_t>(p - d.data());
        // A ShaderFileHeader ('Shdr', not the tail of a footer's 'OrbShdr'), its
        // code, then the ShaderBinaryInfo footer 28 bytes after the code.
        if (at + 0x60 > d.size() || std::memcmp(p, "Shdr", 4) != 0 || (at >= 3 && std::memcmp(p - 3, "Orb", 3) == 0)) continue;
        const int type = p[8] & 0xf;
        if (type != 1 && type != 2 && type != 4) continue;
        const std::size_t limit = std::min(d.size(), at + (1u << 20));
        std::size_t foot = std::string::npos;
        for (std::size_t k = at + 16; k + 28 <= limit; ++k) {
            if (d[k] == 'O' && std::memcmp(d.data() + k, "OrbShdr", 7) == 0) {
                foot = k;
                break;
            }
        }
        if (foot == std::string::npos) continue;
        Item item;
        item.type = static_cast<std::uint8_t>(type);
        item.container.assign(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(foot + 28));
        item.name = ContainerName(item.container.data(), item.container.size());
        if (item.name.empty() || !seen.emplace(item.name, true).second) continue;
        item.source = "eboot.bin";
        items.push_back(std::move(item));
        ++found;
    }
    if (found == 0) error = path + ": no shader container in it";
}

// ---- phase one: the container's own facts and its key ----------------------

struct Program {
    std::uint64_t key = 0;
    Stage stage = Stage::kPixel;
    std::vector<std::uint32_t> words;  // after shader_patch_apply
    std::uint64_t program_hash = 0;
    ConsoleStageOptions options;
    std::vector<gcn::VertexElement> vertex_input;  // VS
    std::string name;
    std::string source;
};

struct Analysed {
    bool ok = false;
    Stage stage = Stage::kPixel;
    std::string reason;
    Program program;
};

Stage StageOf(int type) {
    switch (type) {
    case 1: return Stage::kVertex;
    case 2: return Stage::kPixel;
    case 4: return Stage::kCompute;
    default: return Stage::kVertex;
    }
}

gcn::Stage TranslateStage(Stage stage) {
    return stage == Stage::kVertex ? gcn::Stage::Vertex : stage == Stage::kPixel ? gcn::Stage::Pixel : gcn::Stage::Compute;
}

// The console's translation options, field for field from the POD: nothing a
// field decides is left to the translator's default, because a default that
// changes between two builds of the port would move every key.
gcn::TranslateOptions MakeOptions(const ConsoleStageOptions& o) {
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
    for (const std::uint32_t d : o.image_dims) t.image_dims.emplace_back(d >> 1, (d & 1) != 0);
    for (const std::uint8_t m : o.sampler_unnormalized) t.sampler_force_unnormalized.push_back(m != 0);
    return t;
}

// One container: its stage facts, the patched program, the paths translation
// and the key over all of it. The half before DXC, which is what the launcher
// has to be able to repeat.
Analysed Analyse(const Item& item) {
    Analysed out;
    out.stage = StageOf(item.type);
    const ConsoleContainerStage stage = bbx::d3d12::ReadConsoleContainerStage(item.container, item.type);
    if (!stage.ok) {
        out.reason = "container: " + stage.error;
        return out;
    }
    gcn::ShaderCode code;
    if (!gcn::shader_code(item.container, code) || code.type != item.type) {
        out.reason = "container: the code slice does not parse";
        return out;
    }
    Program program;
    program.stage = out.stage;
    program.words = code.words;
    // The same patch the host applies where it reads a program's words, so the
    // module the bake writes is the program the draw asks for.
    shader_patch_apply(item.name, program.words);
    program.program_hash = bbx::Fnv1a64(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(program.words.data()), program.words.size() * sizeof(std::uint32_t)));
    program.name = item.name;
    program.source = item.source;
    const gcn::Program prog = gcn::decode(program.words.data(), program.words.size());
    if (!prog.errors.empty()) {
        out.reason = "decode: " + prog.errors.front().what;
        return out;
    }
    // The paths translation: what the program reads, which decides the image
    // dims, the sampler modes and the pixel stage's input map - the same first
    // step host/render.cpp's precompiles take.
    gcn::TranslateOptions paths_options;
    paths_options.stage = TranslateStage(out.stage);
    paths_options.rsrc1 = stage.key.rsrc1;
    paths_options.rsrc2 = stage.key.rsrc2;
    const gcn::TranslateResult paths = gcn::translate(prog, paths_options);
    if (!paths.errors.empty()) {
        out.reason = "translation: " + paths.errors.front();
        return out;
    }
    ConsoleStageKey key = stage.key;
    for (const gcn::PredictedImage& p : gcn::predict_image_dims(prog, paths)) key.dims.push_back((p.dim << 1) | (p.arrayed ? 1u : 0u));
    key.sampler_modes.assign(paths.samplers.size(), 0);
    if (out.stage == Stage::kPixel) {
        std::string why;
        if (!bbx::d3d12::ConsolePsInputMap(stage, paths.ps_inputs, key.input_map, key.flat_mask, why)) {
            out.reason = "input map: " + why;
            return out;
        }
        // One module per program serves every draw, so it is the variant
        // without early fragment tests: the safe one for a pipeline that writes
        // depth, and the same pixels for one that does not.
        key.early_fragment_tests = false;
    }
    if (out.stage == Stage::kVertex) {
        for (std::size_t k = 0; k < key.elements.size(); ++k) {
            gcn::VertexElement el;
            el.location = static_cast<std::uint32_t>(k);
            el.vdata = (key.elements[k] >> 8) & 0xffu;
            el.count = (key.elements[k] >> 16) & 0xffu;
            program.vertex_input.push_back(el);
        }
    }
    program.key = ConsoleStageKeyOf(program.words, key);
    program.options.stage = out.stage;
    program.options.rsrc1 = key.rsrc1;
    program.options.rsrc2 = key.rsrc2;
    program.options.ps_input_ena = out.stage == Stage::kPixel ? key.mask : 0;
    program.options.vs_out_cntl = out.stage == Stage::kVertex ? key.mask : 0;
    program.options.ps_flat_mask = key.flat_mask;
    program.options.ps_clip_discard = key.clip_discard;
    program.options.early_fragment_tests = key.early_fragment_tests;
    program.options.ps_input_map = key.input_map;
    program.options.image_dims = key.dims;
    program.options.sampler_unnormalized = key.sampler_modes;
    for (int i = 0; i < 3; ++i) program.options.cs_threads[i] = key.threads[i];
    out.ok = true;
    out.program = std::move(program);
    return out;
}

// ---- phase two: the module, the HLSL, and the compiler ---------------------

struct Compiled {
    std::uint64_t key = 0;
    Stage stage = Stage::kPixel;
    std::uint64_t program_hash = 0;
    std::vector<std::uint8_t> object;  // the signed DXBC container
    std::string reason;                // why not, when object is empty
    bool lifted = false;
};

// One key: the full translation, the lift, the console's HLSL, and DXC. The
// module is the lift's when the lift takes the program and the translator's
// SPIR-V otherwise - the same choice host/render.cpp makes.
Compiled Build(const Program& program, const Compiler& compiler) {
    Compiled out;
    out.key = program.key;
    out.stage = program.stage;
    out.program_hash = program.program_hash;
    const gcn::Program prog = gcn::decode(program.words.data(), program.words.size());
    if (!prog.errors.empty()) {
        out.reason = "decode: " + prog.errors.front().what;
        return out;
    }
    const gcn::TranslateOptions options = MakeOptions(program.options);
    const gcn::TranslateResult translated = gcn::translate(prog, options);
    if (!translated.errors.empty()) {
        out.reason = "translation: " + translated.errors.front();
        return out;
    }
    gcn::LiftResult lifted;
    switch (program.stage) {
    case Stage::kVertex: lifted = gcn::lift_vertex_shader(prog, options, translated); break;
    case Stage::kPixel: lifted = gcn::lift_pixel_shader(prog, options, translated); break;
    default: lifted = gcn::lift_compute_shader(prog, options, translated); break;
    }
    out.lifted = lifted.ok();
    const std::vector<std::uint32_t>& module = out.lifted ? lifted.spirv : translated.spirv;
    gcn::HlslOptions hlsl_options;
    hlsl_options.title = program.name;
    hlsl_options.dxil_sm64 = true;  // the console's dialect: no [[vk]], no inline SPIR-V, no spec constants
    const gcn::HlslResult hlsl = gcn::emit_hlsl(module, hlsl_options);
    if (!hlsl.ok()) {
        out.reason = "hlsl: " + hlsl.errors.front();
        return out;
    }
    if (!hlsl.arguments.empty()) {
        // The console's dialect is a fixed set of DXC arguments: a module that
        // needs another one (-enable-16bit-types) is a module the device cannot
        // be handed.
        out.reason = "hlsl: needs " + hlsl.arguments.front();
        return out;
    }
    std::vector<std::uint8_t> object;
    std::string why;
    if (!compiler.Compile(program.stage, hlsl.source, object, why)) {
        out.reason = "dxc: " + why.substr(0, why.find('\n'));
        return out;
    }
    if (!IsSignedDxbc(object, why)) {
        out.reason = "signature: " + why;
        return out;
    }
    out.object = std::move(object);
    return out;
}

// ---- the run ---------------------------------------------------------------

struct Tally {
    std::mutex mu;
    std::map<std::string, std::map<std::string, std::uint64_t>> refused;  // stage -> reason -> count
    std::uint64_t containers[3] = {};  // by stage: VS, PS, CS
    std::uint64_t analysed[3] = {};
    std::uint64_t keys[3] = {};
    std::uint64_t signed_blobs[3] = {};
    std::uint64_t lifted_blobs[3] = {};
    std::uint64_t duplicate_containers = 0;

    static int Slot(Stage s) { return s == Stage::kVertex ? 0 : s == Stage::kPixel ? 1 : 2; }
    static const char* Name(int slot) { return slot == 0 ? "VS" : slot == 1 ? "PS" : "CS"; }
};

std::string FirstLine(const std::string& text) { return text.substr(0, text.find('\n')); }

}  // namespace

namespace {

struct Options {
    std::string root;
    std::string eboot;
    std::string dxc;
    std::string dxil;
    std::string out = ".";
    unsigned jobs = 0;
    std::size_t limit = 0;
    bool self_test = false;
    bool check_compiler = false;
};

// Runs body(i) for every i in [0, count) on `jobs` threads: one pool for both
// halves of the bake, since each half is a long list of independent programs.
void Parallel(unsigned jobs, std::size_t count, const std::function<void(std::size_t)>& body) {
    std::atomic<std::size_t> next{0};
    const auto worker = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= count) return;
            body(i);
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 1; t < jobs; ++t) threads.emplace_back(worker);
    worker();
    for (std::thread& t : threads) t.join();
}

// The cache's own stamp: the build's identity (ExpectedConsoleStamp) plus who
// compiled what - the DXC that made these blobs, which the launcher does not
// share and must not assume.
CacheStamp MakeStamp(const std::string& dxc_version) {
    CacheStamp recorded;
    recorded.translator = "bbhost-prebake";
    recorded.dxc_version = dxc_version;
    recorded.dxil_version = dxc_version;
    return bbx::d3d12::ExpectedConsoleStamp(recorded);
}

// The compressed entries, the file, the sidecar, and the proof that the file
// loads: a bake that cannot read back what it wrote is not a bake.
int Finish(const std::vector<Compiled>& compiled, const Options& options, const std::string& dxc_version,
           const std::map<std::string, std::map<std::string, std::uint64_t>>& refused) {
    // What was refused comes first: a bake that signed nothing needs its
    // reasons in front of the reader, not after a note that there is no cache.
    if (!refused.empty()) {
        std::uint64_t total = 0;
        for (const auto& [stage, reasons] : refused) {
            for (const auto& [reason, count] : reasons) total += count;
        }
        std::printf("\n  refused %llu key(s):\n", static_cast<unsigned long long>(total));
        for (const auto& [stage, reasons] : refused) {
            for (const auto& [reason, count] : reasons) {
                std::printf("    %-3s %6llu  %s\n", stage.c_str(), static_cast<unsigned long long>(count), reason.c_str());
            }
        }
    }
    std::vector<CacheEntry> entries;
    std::vector<std::uint8_t> blobs;
    entries.reserve(compiled.size());
    std::size_t raw_bytes = 0, repeated_keys = 0;
    std::unordered_map<std::uint64_t, bool> once;
    for (const Compiled& c : compiled) {
        if (c.object.empty()) continue;
        // Two containers that reach one key are one module: the directory holds
        // it once (ParseCache refuses a duplicate key, and rightly - it would be
        // two blobs for one look-up).
        if (!once.emplace(c.key, true).second) {
            ++repeated_keys;
            continue;
        }
        std::vector<std::uint8_t> packed;
        if (!DeflateBlob(c.object, packed)) {
            std::fprintf(stderr, "bbx_bake_cache: %016llx: zlib could not compress it\n",
                         static_cast<unsigned long long>(c.key));
            continue;
        }
        CacheEntry entry;
        entry.key = c.key;
        entry.program_hash = c.program_hash;
        entry.bytecode_hash = bbx::Fnv1a64(std::span<const std::uint8_t>(c.object.data(), c.object.size()));
        entry.stage = c.stage;
        entry.fallback = false;
        entry.offset = static_cast<std::uint32_t>(blobs.size());
        entry.size = static_cast<std::uint32_t>(packed.size());
        entry.raw_size = static_cast<std::uint32_t>(c.object.size());
        entries.push_back(entry);
        blobs.insert(blobs.end(), packed.begin(), packed.end());
        raw_bytes += c.object.size();
    }
    if (entries.empty()) {
        std::fprintf(stderr, "bbx_bake_cache: nothing was signed, so there is no cache to write\n");
        return 1;
    }
    // The file is a directory sorted by key: a look-up is a binary search's
    // worth of work, and ParseCache refuses an unsorted one.
    std::sort(entries.begin(), entries.end(), [](const CacheEntry& a, const CacheEntry& b) { return a.key < b.key; });
    const CacheStamp stamp = MakeStamp(dxc_version);
    const std::vector<std::uint8_t> file = bbx::d3d12::SerializeCache(stamp, entries, blobs);
    const std::string cache_path = (fs::path(options.out) / "bbx_shaders.cache").string();
    const std::string stamp_path = cache_path + ".stamp.txt";
    {
        std::FILE* f = std::fopen(cache_path.c_str(), "wb");
        if (!f || std::fwrite(file.data(), 1, file.size(), f) != file.size()) {
            if (f) std::fclose(f);
            std::fprintf(stderr, "bbx_bake_cache: cannot write %s\n", cache_path.c_str());
            return 1;
        }
        std::fclose(f);
        const std::string text = stamp.Serialize();
        std::FILE* s = std::fopen(stamp_path.c_str(), "wb");
        if (!s || std::fwrite(text.data(), 1, text.size(), s) != text.size()) {
            if (s) std::fclose(s);
            std::fprintf(stderr, "bbx_bake_cache: cannot write %s\n", stamp_path.c_str());
            return 1;
        }
        std::fclose(s);
    }
    // Read back what was written, the way the launcher will: the file's own
    // bytes and its sidecar through ShaderCache::Load, then every key looked up
    // and inflated to the size and digest the directory claims.
    ShaderCache cache;
    const bbx::d3d12::CacheStatus status = cache.Load(std::span<const std::uint8_t>(file), stamp.Serialize());
    std::size_t round_trip = 0, bad_round_trip = 0;
    for (const CacheEntry& entry : entries) {
        bbx::d3d12::BakedBlob blob;
        if (!cache.Lookup(entry.key, &blob) || blob.raw_size != entry.raw_size) {
            ++bad_round_trip;
            continue;
        }
        std::vector<std::uint8_t> inflated(blob.raw_size);
        uLongf size = blob.raw_size;
        if (uncompress(inflated.data(), &size, blob.data, blob.size) != Z_OK || size != blob.raw_size ||
            std::memcmp(inflated.data(), "DXBC", 4) != 0) {
            ++bad_round_trip;
            continue;
        }
        ++round_trip;
    }

    // The scorecard: what was read, what it became, and what was refused.
    std::printf("\nbbx_bake_cache: %zu entries, %zu key(s) offered (%zu already present), %.2f MiB of DXIL in %.2f MiB of cache\n",
                entries.size(), compiled.size(), repeated_keys, static_cast<double>(raw_bytes) / 1048576.0,
                static_cast<double>(file.size()) / 1048576.0);
    std::printf("  %s: load %s (%s); look-up %zu/%zu\n", cache_path.c_str(), bbx::d3d12::CacheStatusName(status),
                cache.Report().c_str(), round_trip, entries.size());
    std::printf("  %s: %s\n", stamp_path.c_str(), FirstLine(stamp.Serialize()).c_str());
    if (status != bbx::d3d12::CacheStatus::kOk || bad_round_trip != 0 || round_trip != entries.size()) {
        std::fprintf(stderr, "bbx_bake_cache: the cache did not read back: %zu of %zu entries, %zu bad (%s)\n", round_trip,
                     entries.size(), bad_round_trip, bbx::d3d12::CacheStatusName(status));
        return 1;
    }
    return 0;
}

// A blob that looks exactly like what DXC returns with a signer: the magic and
// a non-zero digest. Only --self-test uses it, and only in its own temporary
// directory - it is not DXIL and no console would run it.
void SyntheticObject(const std::string& source, std::vector<std::uint8_t>& out) {
    out.assign(96, 0);
    std::memcpy(out.data(), "DXBC", 4);
    const std::uint64_t a = bbx::Fnv1a64(source);
    const std::uint64_t b = bbx::Mix64(a);
    std::memcpy(out.data() + 4, &a, 8);
    std::memcpy(out.data() + 12, &b, 8);
    std::memcpy(out.data() + 24, "DXIL", 4);
}

int SelfTest() {
    // The file's plumbing without a dump, a compiler or game data: keys, the
    // entries, the zlib blobs, the stamp beside them, and the read-back.
    std::vector<Compiled> compiled;
    for (int k = 0; k < 12; ++k) {
        ConsoleStageKey key;
        key.stage = (k % 3) == 0 ? Stage::kVertex : (k % 3) == 1 ? Stage::kPixel : Stage::kCompute;
        key.rsrc1 = static_cast<std::uint32_t>(k);
        const std::vector<std::uint32_t> words = {0xbf810000u, static_cast<std::uint32_t>(k)};
        Compiled c;
        c.key = ConsoleStageKeyOf(words, key);
        c.stage = key.stage;
        c.program_hash = bbx::Fnv1a64(static_cast<std::uint32_t>(k));
        SyntheticObject("synthetic " + std::to_string(k), c.object);
        compiled.push_back(std::move(c));
    }
    // One duplicate key on purpose: the file must hold it once.
    compiled.push_back(compiled.front());
    Options options;
    options.out = (fs::temp_directory_path() / "bbx_bake_cache_selftest").string();
    std::error_code ec;
    fs::create_directories(options.out, ec);
    const int result = Finish(compiled, options, "self-test", {});
    fs::remove_all(options.out, ec);
    std::printf("bbx_bake_cache: self-test %s (synthetic blobs, temporary directory, removed)\n", result == 0 ? "PASS" : "FAIL");
    return result;
}

// Whether this DXC signs: one trivial pixel shader through the same door the
// bake uses, and what came back. A DXC without its signer (a Linux build without
// libdxil beside it) returns a DXBC container whose digest is zeros, and every
// blob the bake made would be refused at the end of an hour's work.
int CheckCompiler(const Options& options) {
    Compiler compiler;
    std::string error;
    if (!compiler.Load(options.dxc, options.dxil, error)) {
        std::fprintf(stderr, "bbx_bake_cache: %s\n", error.c_str());
        return 1;
    }
    const char* source = "float4 main(float4 p : SV_Position) : SV_Target { return p; }\n";
    std::vector<std::uint8_t> object;
    if (!compiler.Compile(Stage::kPixel, source, object, error)) {
        std::fprintf(stderr, "bbx_bake_cache: DXC %s refused a trivial ps_6_4: %s\n", compiler.version().c_str(),
                     FirstLine(error).c_str());
        return 1;
    }
    std::string why;
    const bool signed_blob = IsSignedDxbc(object, why);
    std::printf("bbx_bake_cache: DXC %s: ps_6_4 -> %zu bytes, magic %.4s, digest %02x%02x%02x%02x..., %s%s\n",
                compiler.version().c_str(), object.size(), reinterpret_cast<const char*>(object.data()), object[4], object[5],
                object[6], object[7], signed_blob ? "signed" : "UNSIGNED: ", signed_blob ? "" : why.c_str());
    return signed_blob ? 0 : 1;
}

void Usage() {
    std::printf(
        "bbx_bake_cache: bake <dump>/bbx_shaders.cache for the console\n"
        "\n"
        "  --root <dir>     walk it for *.shaderbnd.dcx and *.shaderbnddebug.dcx\n"
        "  --eboot <path>   the dump's eboot.bin (a fake SELF is unwrapped first)\n"
        "  --dxc <path>     libdxcompiler.so (dxcompiler.dll on Windows); required\n"
        "  --dxil <path>    libdxil.so (dxil.dll), loaded before DXC so its look-up\n"
        "                   finds it; also tried beside --dxc\n"
        "  --out <dir>      where bbx_shaders.cache and its .stamp.txt go (default: .)\n"
        "  --jobs <n>       threads (default: the machine's)\n"
        "  --limit <n>      stop after n containers (to try one bundle first)\n"
        "  --check-compiler compile one trivial ps_6_4 and report whether it signed\n"
        "  --self-test      the cache write/read-back over synthetic blobs; no dump\n"
        "  --help           this text\n");
}

}  // namespace

// ---- the settings the patches read -----------------------------------------
//
// The bake is not the game: no ini, no menu, no live settings. But the patches
// decide the words it keys a program by (host/shader_patch.h), so the tool
// answers the two symbols host/shader_patch.cpp asks for the way a fresh
// install does - the settings as they are on the first run - instead of leaving
// the mask at zero, which would bake programs the running game never asks for.
void host_settings_listen(HostSettingsListener fn) {
    if (fn) fn(HostSettings{});
}

int host_opt_motion_rebuilt_bits() { return 0; }

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "bbx_bake_cache: %s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--root") options.root = value("--root");
        else if (arg == "--eboot") options.eboot = value("--eboot");
        else if (arg == "--dxc") options.dxc = value("--dxc");
        else if (arg == "--dxil") options.dxil = value("--dxil");
        else if (arg == "--out") options.out = value("--out");
        else if (arg == "--jobs") options.jobs = static_cast<unsigned>(std::atoi(value("--jobs").c_str()));
        else if (arg == "--limit") options.limit = static_cast<std::size_t>(std::atoll(value("--limit").c_str()));
        else if (arg == "--self-test") options.self_test = true;
        else if (arg == "--check-compiler") options.check_compiler = true;
        else if (arg == "--help" || arg == "-h") {
            Usage();
            return 0;
        } else {
            std::fprintf(stderr, "bbx_bake_cache: unknown argument %s (--help)\n", arg.c_str());
            return 2;
        }
    }
    if (options.self_test) return SelfTest();
    if (options.check_compiler) {
        if (options.dxc.empty()) {
            std::fprintf(stderr, "bbx_bake_cache: --check-compiler needs --dxc\n");
            return 2;
        }
        return CheckCompiler(options);
    }
    if (options.dxc.empty() || (options.root.empty() && options.eboot.empty())) {
        Usage();
        return 2;
    }
    if (options.jobs == 0) {
        const unsigned hardware = std::thread::hardware_concurrency();
        options.jobs = hardware == 0 ? 4 : hardware;
    }

    // The console rounds halves itself nowhere: no float16_t, no DenormPreserve
    // or RoundingModeRTZ to declare (gcn/half.h). Set once, before any thread.
    gcn::set_native_half_rtz(false);

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Item> items;
    std::unordered_map<std::string, bool> seen;
    if (!options.root.empty()) {
        std::error_code ec;
        std::vector<fs::path> bundles;
        for (fs::recursive_directory_iterator it(options.root, ec), end; it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file()) continue;
            const std::string name = it->path().filename().string();
            if (name.size() > 12 && (name.compare(name.size() - 12, 12, "shaderbnd.dcx") == 0 ||
                                     name.compare(name.size() - 17, 17, "shaderbnddebug.dcx") == 0)) {
                bundles.push_back(it->path());
            }
        }
        std::sort(bundles.begin(), bundles.end());  // a stable order: the same dump bakes the same cache
        for (const fs::path& path : bundles) {
            std::string error;
            AddBundle(path, items, seen, error);
            std::printf("bbx_bake_cache: %s: %zu container(s) so far%s%s\n", path.filename().string().c_str(), items.size(),
                        error.empty() ? "" : "; ", error.c_str());
        }
    }
    if (!options.eboot.empty()) {
        std::string error;
        const std::size_t before = items.size();
        AddEboot(options.eboot, items, seen, error);
        std::printf("bbx_bake_cache: eboot.bin: %zu container(s) of its own%s%s\n", items.size() - before, error.empty() ? "" : "; ",
                    error.c_str());
    }
    if (options.limit && items.size() > options.limit) items.resize(options.limit);
    if (items.empty()) {
        std::fprintf(stderr, "bbx_bake_cache: no shader container found under %s%s%s\n", options.root.c_str(),
                     options.eboot.empty() ? "" : " or in ", options.eboot.c_str());
        return 1;
    }
    std::printf("bbx_bake_cache: %zu container(s), %u thread(s)\n", items.size(), options.jobs);

    // Phase one: every container's stage facts and key, in parallel.
    std::vector<Analysed> analysed(items.size());
    std::atomic<std::size_t> done{0};
    Parallel(options.jobs, items.size(), [&](std::size_t i) {
        analysed[i] = Analyse(items[i]);
        const std::size_t n = done.fetch_add(1) + 1;
        if (n % 500 == 0) std::fprintf(stderr, "  analysed %zu/%zu\r", n, items.size());
    });
    Tally tally;
    std::vector<Program> unique;
    std::unordered_map<std::uint64_t, std::size_t> by_key;
    for (const Analysed& a : analysed) {
        const int slot = Tally::Slot(a.stage);
        tally.containers[slot] += 1;
        if (!a.ok) {
            ++tally.refused[Tally::Name(slot)][a.reason];
            continue;
        }
        ++tally.analysed[slot];
        const auto [it, inserted] = by_key.emplace(a.program.key, unique.size());
        if (!inserted) {
            ++tally.duplicate_containers;
            continue;
        }
        unique.push_back(a.program);
        ++tally.keys[slot];
    }
    std::printf("bbx_bake_cache: %zu unique key(s) from %zu container(s) (%llu duplicate container(s))\n", unique.size(), items.size(),
                static_cast<unsigned long long>(tally.duplicate_containers));

    // Phase two: the module, the HLSL and the compiler, in parallel over keys.
    Compiler compiler;
    std::string error;
    const std::string dxil = options.dxil;
    if (!compiler.Load(options.dxc, dxil, error)) {
        std::fprintf(stderr, "bbx_bake_cache: %s\n", error.c_str());
        return 1;
    }
    std::printf("bbx_bake_cache: DXC %s\n", compiler.version().c_str());
    std::vector<Compiled> compiled(unique.size());
    done = 0;
    Parallel(options.jobs, unique.size(), [&](std::size_t i) {
        compiled[i] = Build(unique[i], compiler);
        const std::size_t n = done.fetch_add(1) + 1;
        if (n % 100 == 0) std::fprintf(stderr, "  compiled %zu/%zu\r", n, unique.size());
    });
    std::vector<Compiled> signed_blobs;
    for (Compiled& c : compiled) {
        const int slot = Tally::Slot(c.stage);
        if (c.object.empty()) {
            ++tally.refused[Tally::Name(slot)][FirstLine(c.reason)];
            continue;
        }
        if (c.lifted) ++tally.lifted_blobs[slot];
        ++tally.signed_blobs[slot];
        if (options.limit == 0) signed_blobs.push_back(std::move(c));
        else signed_blobs.push_back(std::move(c));
    }

    std::printf("\n  stage  containers  keys  signed  lifted  refused\n");
    for (int slot = 0; slot < 3; ++slot) {
        std::uint64_t refused = 0;
        for (const auto& [reason, count] : tally.refused[Tally::Name(slot)]) refused += count;
        std::printf("  %-5s %11llu %5llu %7llu %7llu %8llu\n", Tally::Name(slot),
                    static_cast<unsigned long long>(tally.containers[slot]), static_cast<unsigned long long>(tally.keys[slot]),
                    static_cast<unsigned long long>(tally.signed_blobs[slot]), static_cast<unsigned long long>(tally.lifted_blobs[slot]),
                    static_cast<unsigned long long>(refused));
    }
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  %lld ms\n", ms);

    const int result = Finish(signed_blobs, options, compiler.version(), tally.refused);
    if (result == 0) std::printf("bbx_bake_cache: done\n");
    return result;
}
