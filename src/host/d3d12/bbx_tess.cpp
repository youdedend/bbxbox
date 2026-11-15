// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/host/d3d12/bbx_tess.h"

namespace bbx::d3d12 {

const char* TessPathName(TessPath path) {
    switch (path) {
    case TessPath::kNone: return "none";
    case TessPath::kHardware: return "hardware";
    case TessPath::kCompute: return "compute";
    case TessPath::kSkip: return "skipped";
    }
    return "none";
}

TessDecision ChooseTessPath(const Caps& caps, const TessDraw& draw, const TessPolicy& policy) {
    TessDecision decision;
    if (!draw.has_hull_shader && !draw.has_domain_shader) {
        decision.path = TessPath::kNone;
        return decision;
    }

    // A patch shader without its pair is broken guest state; skipping it is the
    // only safe answer, and the log line says which half is missing.
    if (!draw.has_hull_shader || !draw.has_domain_shader) {
        decision.path = TessPath::kSkip;
        decision.reason = draw.has_hull_shader ? "HS without DS" : "DS without HS";
        return decision;
    }

    if (caps.hardware_tessellator) {
        decision.path = TessPath::kHardware;
        decision.chunks = 1;
        return decision;
    }

    if (!policy.emulation_enabled) {
        decision.path = TessPath::kSkip;
        decision.reason = "tessellation emulation is off";
        return decision;
    }
    if (draw.isoline) {
        decision.path = TessPath::kSkip;
        decision.reason = "isoline patches are not emulated";
        return decision;
    }
    if (!GroupSharedFits(caps, draw.hull_group_shared)) {
        decision.path = TessPath::kSkip;
        decision.reason = "hull shader needs " + std::to_string(draw.hull_group_shared) +
                          " bytes of group shared memory (limit " +
                          std::to_string(caps.max_group_shared_bytes) + ")";
        return decision;
    }

    decision.path = TessPath::kCompute;
    if (draw.patch_count == 0) {
        decision.chunks = 0;  // nothing to draw
        decision.reason = "no patches";
        return decision;
    }
    if (draw.max_patches_per_chunk != 0) {
        decision.chunks = (draw.patch_count + draw.max_patches_per_chunk - 1) /
                          draw.max_patches_per_chunk;
    } else {
        decision.chunks = 1;
    }
    return decision;
}

void TessCounters::Note(const TessDecision& decision) {
    switch (decision.path) {
    case TessPath::kHardware: ++hardware; break;
    case TessPath::kCompute: ++compute; break;
    case TessPath::kSkip:
        ++skipped;
        if (decision.reason.find("isoline") != std::string::npos) ++skipped_isoline;
        else if (decision.reason.find("group shared") != std::string::npos) ++skipped_lds;
        else if (decision.reason.find("emulation is off") != std::string::npos) ++skipped_disabled;
        break;
    case TessPath::kNone: break;
    }
}

std::string DescribeTess(const TessCounters& counters) {
    return "tessellation: " + std::to_string(counters.hardware) + " hardware, " +
           std::to_string(counters.compute) + " compute, " + std::to_string(counters.skipped) +
           " skipped (" + std::to_string(counters.skipped_disabled) + " off, " +
           std::to_string(counters.skipped_isoline) + " isoline, " +
           std::to_string(counters.skipped_lds) + " LDS)";
}

}  // namespace bbx::d3d12
