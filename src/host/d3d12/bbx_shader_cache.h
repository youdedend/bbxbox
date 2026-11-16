// SPDX-License-Identifier: GPL-3.0-or-later
// <dump>/bbx_shaders.cache, as the launcher loads it.
//
// The bake (tools/bbx_bake_cache.cpp) lifts the game's OrbShdr containers to
// SPIR-V, prints them as the console's dialect of HLSL (gcn::HlslOptions
// dxil_sm64), compiles each with dxc -T *_6_4, and writes every signed blob into
// one file with a stamp beside it. This side validates that file once at boot
// (ParseCache: magic, version, stamp, CRC, every entry's bounds, no duplicate
// keys) and then answers a look-up per program, without touching the file again.
//
// The stamp is two facts: the file's own (the bake's compiler and signer, and
// the translator's fingerprint) and this build's (dxil-6.4, tables, 11_0).
// ExpectedConsoleStamp pairs them, so a cache built for another target is
// refused by name instead of bound into a pipeline that would draw garbage.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "host/d3d12/bbx_cache.h"

namespace bbx::d3d12 {

// One program's DXIL, compressed by the bake: zlib, `raw_size` when inflated.
struct BakedBlob {
    const std::uint8_t* data = nullptr;
    std::uint32_t size = 0;
    std::uint32_t raw_size = 0;
};

class ShaderCache {
public:
    // `cache` is the file's bytes, `stamp_text` the sidecar the bake wrote
    // beside it (CacheStamp::Serialize). Anything ParseCache refuses leaves
    // entries() at zero: nothing is trusted by halves.
    CacheStatus Load(std::span<const std::uint8_t> cache, std::string_view stamp_text);

    // The interface hash's blob, as the bake compressed it. False when the
    // cache did not validate or has no such program.
    [[nodiscard]] bool Lookup(std::uint64_t key, BakedBlob* out) const;

    [[nodiscard]] std::size_t entries() const { return parsed_.entries.size(); }
    [[nodiscard]] std::size_t bytes() const { return file_.size(); }
    [[nodiscard]] CacheStatus status() const { return parsed_.status; }
    [[nodiscard]] const std::string& detail() const { return parsed_.detail; }

    // "ok: 25711 entries, 41.2 MiB" or "stamp mismatch: ...": the one line the
    // loader logs and the HUD shows.
    [[nodiscard]] std::string Report() const;

private:
    std::vector<std::uint8_t> file_;
    ParseResult parsed_;
    std::unordered_map<std::uint64_t, std::uint32_t> index_;  // interface hash -> entry
};

// ---- the console's stage keys ------------------------------------------------
//
// The bake (tools/bbx_bake_cache.cpp) writes each blob under a key and the
// launcher looks it up with that key, so the two must agree byte for byte: a
// key that differs in one field is a shader that was baked and never found,
// and nothing logs it because a miss is a normal thing. Everything that makes
// a key is therefore in this file, and both sides call these functions.
//
// bbhost keys a stage by the program's words, the register block, the draw
// state that changes the module's interface, and the translation's options
// (host/render.cpp: ps_stage_key, vs_stage_key, compute_key). The console has
// no draw state to consult: one module per container has to serve every draw
// the game makes with it, so its key holds what the container itself states
// (ReadConsoleContainerStage), the inputs the program actually reads (which
// only the translation knows: ConsolePsInputMap), the image dims and sampler
// modes predicted from the program, and the console's fixed translation
// choices (ConsoleStageOptions) - nothing from the frame, nothing from a
// device. Two runs of the bake over the same dump produce the same keys, and
// the launcher's first look-up after a bake hits.
//
// The encoding is little-endian bytes, not host words, because the bake may
// run on another machine than the one that plays; and it is versioned, so
// adding a field without bumping kConsoleStageKeyVersion is a test failure
// rather than a silent split between two key spaces.
inline constexpr std::uint32_t kConsoleStageKeyVersion = 1;

// A vertex input element, as the key holds it: the element's word
// (ConsoleVertexElementWord) rather than bbhost's gcn::VertexElement, so the
// key does not drag the translator's headers into the portable core.
[[nodiscard]] std::uint32_t ConsoleVertexElementWord(std::uint32_t location, std::uint32_t vdata, std::uint32_t count,
                                                     bool formats_from_params);

// Every field of a stage's key that is not the program itself. Which fields a
// stage reads is stated per field: the pixel stage's mask, flat mask, early
// tests, clip distances and input map; the vertex stage's out control, its
// elements and whether its formats come from params; the compute stage's
// thread counts. A field a stage does not read must be left at its default,
// because the encoding writes all of them and a stray bit would key the same
// module twice.
struct ConsoleStageKey {
    Stage stage = Stage::kPixel;
    std::uint32_t rsrc1 = 0;  // PS/V: SPI_SHADER_PGM_RSRC1, CS: COMPUTE_PGM_RSRC1
    std::uint32_t rsrc2 = 0;  // the same, RSRC2
    // PS: SPI_PS_INPUT_ENA (which params the pixel stage reads); VS:
    // SPI_VS_OUT_CONFIG / PA_CL_VS_OUT_CNTL as the container's word 8 gives
    // them; CS: 0.
    std::uint32_t mask = 0;
    std::uint32_t flat_mask = 0;  // PS: inputs interpolating flat (FLAT_SHADE)
    bool early_fragment_tests = false;   // PS: the program discards
    std::uint32_t clip_discard = 0;      // PS: TranslateOptions::ps_clip_discard
    std::uint32_t threads[3] = {1, 1, 1};  // CS: COMPUTE_NUM_THREAD_X/Y/Z
    // VS: whether the module takes its vertex formats as constants, which is
    // what lets one module serve every draw (ConsoleVertexElementWord's top
    // bit repeats it per element; the field is what makes an input-less
    // vertex shader's two cases distinguishable).
    bool formats_from_params = false;
    std::vector<std::uint32_t> elements;     // VS: one ConsoleVertexElementWord per input, in table order
    std::vector<std::uint8_t> input_map;     // PS: param register per input, dense to the last one read
    std::vector<std::uint32_t> dims;         // predicted image dims: (dim << 1) | arrayed, in translation order
    std::vector<std::uint8_t> sampler_modes; // predicted sampler modes: non-zero for unnormalized
};

// The 64-bit key over the patched program's words and every field above
// (kConsoleStageKeyVersion first). `program` is the container's code after
// shader_patch_apply, the same words the translation and the lift see.
[[nodiscard]] std::uint64_t ConsoleStageKeyOf(std::span<const std::uint32_t> program, const ConsoleStageKey& key);

// The console's translation and lift choices, as values, so that the core
// states them without including the translator's headers: the bake maps these
// onto gcn::TranslateOptions 1:1, and the launcher will map the same fields.
// No field is left to a default anywhere else.
struct ConsoleStageOptions {
    Stage stage = Stage::kPixel;
    std::uint32_t rsrc1 = 0;
    std::uint32_t rsrc2 = 0;
    std::uint32_t ps_input_ena = 0;      // PS
    std::uint32_t ps_flat_mask = 0;      // PS
    std::uint32_t ps_clip_discard = 0;   // PS
    std::uint32_t vs_out_cntl = 0;       // VS: PA_CL_VS_OUT_CNTL, as the container's word 8 holds it
    std::uint32_t cs_threads[3] = {1, 1, 1};  // CS: COMPUTE_NUM_THREAD_X/Y/Z
    bool early_fragment_tests = false;   // PS
    std::vector<std::uint8_t> ps_input_map;          // PS
    std::vector<std::uint32_t> image_dims;           // (dim << 1) | arrayed
    std::vector<std::uint8_t> sampler_unnormalized;
    // The console's fixed choices, named so that a reader can check them
    // against the device rather than guess:
    static constexpr std::uint32_t kDescriptorSet = 1;
    static constexpr bool kBindless = false;        // Native16Bit/ResourceDescriptorHeap are off on the console
    static constexpr bool kCbSsbo = false;          // constant buffers stay constant buffers
    static constexpr bool kNativeHalfRtz = false;   // no float16_t at SM 6.4 without Native16BitShaderOps
    // A program the translator would have to approximate is refused by the
    // bake and counted, never baked as something the console cannot run.
    static constexpr bool kNoFallback = true;
};

// What a shader container states about its stage, read at the offsets
// host/render.cpp reads for the pixel, vertex and compute precompiles: the
// 'Shdr' block at +0x10 holds sixteen register words, the usage count at +0x3
// and then the input table. `type` is gcn::ShaderCode::type (1 vertex, 2
// pixel, 4 compute; 0 is an optimized relink, which has no stage of its own).
struct ConsoleContainerStage {
    bool ok = false;
    std::string error;  // why not, in words: "no Shdr block in the first 0x100", ...
    Stage stage = Stage::kVertex;
    ConsoleStageKey key;                   // everything the container alone states
    std::uint32_t declared_inputs = 0;     // PS: the input table's length byte at +0x38
    std::vector<std::uint16_t> input_words;  // PS: each declared input's SPI_PS_INPUT_CNTL word
};

// Reads the container's own stage facts. The words are read separately
// (gcn::shader_code, whose slice the caller also patches), because the
// container's code offset is the code parser's to say, not a second copy's.
[[nodiscard]] ConsoleContainerStage ReadConsoleContainerStage(std::span<const std::uint8_t> container, std::uint8_t type);

// The pixel stage's input map and flat mask from the inputs the program reads
// (gcn::TranslateResult::ps_inputs). Only the translation knows which inputs
// are read, so this is a second step: the map is dense to the last input read,
// an unread slot taking its own index, and a read input taking the param
// register its container's semantic predicts (semantic - 0xf, as
// host/render.cpp's creation-time path predicts it). The console never asks
// for GCN's DEFAULT_VAL: a byte semantic has no bit 5 to ask with.
[[nodiscard]] bool ConsolePsInputMap(const ConsoleContainerStage& stage, std::span<const std::uint32_t> read_inputs,
                                     std::vector<std::uint8_t>& map, std::uint32_t& flat_mask, std::string& error);

}  // namespace bbx::d3d12
