// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/d3d12/bbx_shader_cache.h"

#include "host/d3d12/bbx_common.h"

#include <cstdio>
#include <cstring>

namespace bbx::d3d12 {

CacheStatus ShaderCache::Load(std::span<const std::uint8_t> cache, std::string_view stamp_text) {
    file_.assign(cache.begin(), cache.end());
    index_.clear();
    // The stamp beside the file says who compiled it; this build says for what.
    // A file without its sidecar has no second half, so it is refused rather
    // than trusted on its own word.
    const CacheStamp recorded = CacheStamp::Parse(stamp_text);
    parsed_ = ParseCache(file_, ExpectedConsoleStamp(recorded));
    if (parsed_.status != CacheStatus::kOk) return parsed_.status;
    index_.reserve(parsed_.entries.size());
    for (std::uint32_t i = 0; i < parsed_.entries.size(); ++i) index_.emplace(parsed_.entries[i].key, i);
    return parsed_.status;
}

bool ShaderCache::Lookup(std::uint64_t key, BakedBlob* out) const {
    if (out != nullptr) *out = BakedBlob{};
    if (parsed_.status != CacheStatus::kOk) return false;
    const auto it = index_.find(key);
    if (it == index_.end()) return false;
    const CacheEntry& entry = parsed_.entries[it->second];
    // ParseCache validated this range; it is re-stated here so the pointer is
    // never formed outside the blob region even if the file were swapped under
    // the process after validation.
    if (static_cast<std::size_t>(entry.offset) + entry.size > parsed_.blobs.size()) return false;
    if (out != nullptr) {
        out->data = parsed_.blobs.data() + entry.offset;
        out->size = entry.size;
        out->raw_size = entry.raw_size;
    }
    return true;
}

std::string ShaderCache::Report() const {
    // The HUD's font is uppercase 3x5 with a small punctuation set, so the line
    // stays short ASCII: a status, a count and a size. The long form of the
    // status, and its detail, are the loader's log line.
    const char* name = "?";
    switch (parsed_.status) {
    case CacheStatus::kOk: name = "ok"; break;
    case CacheStatus::kMissing: name = "missing"; break;
    case CacheStatus::kNotACache: name = "not a cache"; break;
    case CacheStatus::kStampMismatch: name = "stamp mismatch"; break;
    case CacheStatus::kCorrupt: name = "corrupt"; break;
    case CacheStatus::kTooNew: name = "too new"; break;
    }
    char text[128];
    const double mib = static_cast<double>(file_.size()) / (1024.0 * 1024.0);
    if (parsed_.status == CacheStatus::kOk) {
        std::snprintf(text, sizeof(text), "%s: %zu entries, %.1f MiB", name, entries(), mib);
    } else {
        std::snprintf(text, sizeof(text), "%s: %zu bytes", name, file_.size());
    }
    return text;
}

// ---- the console's stage keys ------------------------------------------------

std::uint32_t ConsoleVertexElementWord(std::uint32_t location, std::uint32_t vdata, std::uint32_t count,
                                       bool formats_from_params) {
    // 8 bits each, which is what the container's table holds; the top byte says
    // whether the module read them as constants. A translation that takes an
    // element by fetching it registers, rather than by constants, reads its
    // dwords apart, so the flag belongs in the word and not in a separate bit
    // somewhere else: two elements that differ only in it are two modules.
    return (location & 0xffu) | ((vdata & 0xffu) << 8) | ((count & 0xffu) << 16) | (formats_from_params ? 1u << 24 : 0u);
}

namespace {

// The key's canonical bytes: little-endian, length-prefixed where a field is a
// vector, version first. Written by hand rather than memcpy'd from the struct
// so that padding, field order and the host's endianness cannot leak in.
class KeyBytes {
public:
    void U8(std::uint8_t v) { bytes_.push_back(v); }
    void U32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
    }
    void Raw(std::span<const std::uint8_t> bytes) { bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }
    void U32s(std::span<const std::uint32_t> values) {
        U32(static_cast<std::uint32_t>(values.size()));
        for (const std::uint32_t v : values) U32(v);
    }
    void Bytes(std::span<const std::uint8_t> values) {
        U32(static_cast<std::uint32_t>(values.size()));
        Raw(values);
    }
    [[nodiscard]] const std::vector<std::uint8_t>& get() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

std::uint16_t Load16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

}  // namespace

std::uint64_t ConsoleStageKeyOf(std::span<const std::uint32_t> program, const ConsoleStageKey& key) {
    KeyBytes out;
    out.U32(kConsoleStageKeyVersion);
    out.U8(static_cast<std::uint8_t>(key.stage));
    out.U32(key.rsrc1);
    out.U32(key.rsrc2);
    out.U32(key.mask);
    out.U32(key.flat_mask);
    out.U8(key.early_fragment_tests ? 1u : 0u);
    out.U32(key.clip_discard);
    out.U32(key.threads[0]);
    out.U32(key.threads[1]);
    out.U32(key.threads[2]);
    out.U8(key.formats_from_params ? 1u : 0u);
    out.U32s(key.elements);
    // The map is dense to the last input read, so its length is information too:
    // a program reading one more input is another module.
    out.Bytes(key.input_map);
    out.U32s(key.dims);
    out.Bytes(key.sampler_modes);
    // The program last: it is the one field that is always large, and the
    // fields above are what a reader compares when two keys differ.
    out.U32s(program);
    return Mix64(Fnv1a64(out.get()));
}

ConsoleContainerStage ReadConsoleContainerStage(std::span<const std::uint8_t> container, std::uint8_t type) {
    ConsoleContainerStage stage;
    // The 'Shdr' block, found the way host/render.cpp and gpu.cpp find it: in
    // the first 0x100 bytes, because the container's file header is 0x24 bytes
    // and some containers carry a short prefix.
    std::size_t shdr = 0;
    while (shdr + 0x60 <= container.size() && shdr < 0x100 && std::memcmp(container.data() + shdr, "Shdr", 4) != 0) ++shdr;
    if (shdr + 0x60 > container.size() || shdr >= 0x100) {
        stage.error = "no Shdr block in the first 0x100 bytes";
        return stage;
    }
    switch (type) {
    case 1: stage.stage = Stage::kVertex; break;
    case 2: stage.stage = Stage::kPixel; break;
    case 4: stage.stage = Stage::kCompute; break;
    case 0: stage.error = "an optimized relink has no stage of its own"; return stage;
    case 3: stage.error = "a geometry container: the game has no geometry stage on the console"; return stage;
    default: stage.error = "container type " + std::to_string(type) + " is not a stage the console translates"; return stage;
    }
    // The Shdr block at `shdr`: sixteen register words at +0x10, the usage
    // count at +0x13 and the input table after them.
    const std::size_t base = shdr + 16;
    if (base + 0x40 > container.size()) {
        stage.error = "the Shdr register block runs past the container";
        return stage;
    }
    std::uint32_t regs[16];
    std::memcpy(regs, container.data() + base, sizeof(regs));
    const std::size_t usage = container[base + 3];
    stage.key.stage = stage.stage;
    if (stage.stage == Stage::kPixel) {
        // The pixel stage's own words: RSRC1/RSRC2 for the program's resource
        // needs and SPI_PS_INPUT_ENA for the params it reads.
        stage.key.rsrc1 = regs[4];
        stage.key.rsrc2 = regs[5];
        stage.key.mask = regs[8];
        stage.declared_inputs = container[base + 0x38];
        const std::size_t table = base + 0x3c + usage * 4;
        if (table + 2 * static_cast<std::size_t>(stage.declared_inputs) > container.size()) {
            stage.error = "a pixel input table past the container";
            return stage;
        }
        stage.input_words.resize(stage.declared_inputs);
        for (std::uint32_t k = 0; k < stage.declared_inputs; ++k) {
            stage.input_words[k] = Load16(container.data() + table + 2 * k);
        }
    } else if (stage.stage == Stage::kVertex) {
        // sccGnmSetVsShader adds VGPR_COMP_CNT (RSRC1 bits 24-25): 3, as the
        // game's own setter does, so the key agrees with the draw's register.
        stage.key.rsrc1 = (regs[4] & ~(3u << 24)) | (3u << 24);
        stage.key.rsrc2 = regs[5];
        stage.key.mask = regs[8];
        const std::uint32_t inputs = container[base + 0x24];
        const std::size_t table = base + 0x28 + usage * 4;
        if (inputs > 16) {
            stage.error = "a vertex input count past 16";
            return stage;
        }
        if (table + 4 * static_cast<std::size_t>(inputs) > container.size()) {
            stage.error = "a vertex input table past the container";
            return stage;
        }
        // One module per vertex shader: its elements' formats come from the
        // module's own params (the console has no pipeline library to rebuild
        // the interface per draw), which is why an element's word carries the
        // flag and why a shader with no inputs still says which of the two it is.
        stage.key.formats_from_params = inputs != 0;
        stage.key.elements.reserve(inputs);
        for (std::uint32_t k = 0; k < inputs; ++k) {
            const std::uint8_t* entry = container.data() + table + 4 * k;
            const std::uint32_t count = entry[2];
            if (count == 0 || count > 4) {
                stage.error = "a vertex input with no components or more than four";
                return stage;
            }
            stage.key.elements.push_back(ConsoleVertexElementWord(k, entry[1], count, stage.key.formats_from_params));
        }
    } else {
        // CsStageRegs: pgm_lo, pgm_hi, rsrc1, rsrc2, num_thread_x/y/z.
        stage.key.rsrc1 = regs[4];
        stage.key.rsrc2 = regs[5];
        for (int i = 0; i < 3; ++i) stage.key.threads[i] = regs[6 + i];
        if (stage.key.threads[0] == 0 || stage.key.threads[1] == 0 || stage.key.threads[2] == 0) {
            stage.error = "a compute shader with a zero thread count";
            return stage;
        }
    }
    stage.ok = true;
    return stage;
}

bool ConsolePsInputMap(const ConsoleContainerStage& stage, std::span<const std::uint32_t> read_inputs,
                       std::vector<std::uint8_t>& map, std::uint32_t& flat_mask, std::string& error) {
    map.clear();
    flat_mask = 0;
    if (stage.stage != Stage::kPixel) {
        error = "only a pixel stage has an input map";
        return false;
    }
    if (!stage.ok) {
        error = stage.error;
        return false;
    }
    const std::uint32_t read = read_inputs.empty() ? 0u : read_inputs[0];
    for (std::uint32_t k = 0; k < 32; ++k) {
        if (((read >> k) & 1u) == 0) continue;
        if (k >= stage.declared_inputs || k >= stage.input_words.size()) {
            error = "an input outside the container's table (input " + std::to_string(k) + " of " +
                    std::to_string(stage.declared_inputs) + ")";
            return false;
        }
        const std::uint16_t word = stage.input_words[k];
        if (((word >> 12) | (word >> 10)) & 1) flat_mask |= 1u << k;  // FLAT_SHADE, as sub_14873a0 builds it
        const std::uint32_t semantic = word & 0xffu;
        // The vertex-shader output with that semantic, which every container
        // the game ships puts at semantic - 0xf.
        if (semantic < 0xf || semantic - 0xf >= 32) {
            error = "input " + std::to_string(k) + " has a semantic outside the param registers";
            return false;
        }
        while (map.size() < k) map.push_back(static_cast<std::uint8_t>(map.size()));
        map.push_back(static_cast<std::uint8_t>(semantic - 0xf));
    }
    return true;
}

}  // namespace bbx::d3d12
