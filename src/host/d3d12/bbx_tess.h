// SPDX-License-Identifier: GPL-3.0-or-later
// Tessellation on a console whose tessellator emits no domain points.
//
// Verified on Series X (KytyPS5 commit `988ceea`): a hardware-tessellated draw
// produces **zero** domain points, i.e. the draw silently disappears (or worse,
// the driver hangs waiting for them). Bloodborne's hull/domain draws - Forbidden
// Woods, cloth, the particles that are not already spliced - must therefore run
// through compute passes or not at all.
//
// bbhost already splices the GPU particle export+geometry stage into a vertex
// shader (`src/gcn/gs_vertex.h`), so the geometry side is not an issue. What
// remains is the real HS/DS pair, and this header is the policy for it:
//
//   kCompute  capture pass (LS as VS) -> HS as one workgroup per patch ->
//             scan + tessellate kernels (the D3D11 reference tessellator, MIT,
//             as ported in `988ceea`: `gpu_tess_scan.comp` and
//             `gpu_tess_tessellate.comp`) -> TES as the VS of an indirect draw.
//   kSkip     the level's tessellation toggle is off, or the patch cannot fit.
//
// A skipped draw is counted and named in the log once per pipeline, so a black
// screen is attributable instead of mysterious.
#pragma once

#include <cstdint>
#include <string>

#include "src/host/d3d12/bbx_caps.h"

namespace bbx::d3d12 {

enum class TessPath : std::uint8_t {
    kNone = 0,      // the draw does not tessellate
    kHardware,      // desktop: the real tessellator
    kCompute,       // console: the four-pass emulation
    kSkip,          // console without emulation, or a patch that cannot fit
};

[[nodiscard]] const char* TessPathName(TessPath path);

struct TessDraw {
    bool has_hull_shader = false;
    bool has_domain_shader = false;
    bool isoline = false;           // isoline patches are not emulated
    std::uint32_t patch_count = 0;
    std::uint32_t hull_group_shared = 0;
    // The emulation's scratch buffers hold a fixed chunk of patches; a bigger
    // draw is split. Zero means "no limit known yet", which is allowed.
    std::uint32_t max_patches_per_chunk = 0;
};

struct TessPolicy {
    bool emulation_enabled = true;  // the level's Testing toggle, default on here
    bool log_once = true;
};

struct TessDecision {
    TessPath path = TessPath::kNone;
    std::uint32_t chunks = 0;  // how many indirect draws the patch set becomes
    std::string reason;        // logged once per pipeline
};

// The decision for one draw. Never returns kHardware when
// `caps.hardware_tessellator` is false.
[[nodiscard]] TessDecision ChooseTessPath(const Caps& caps, const TessDraw& draw,
                                          const TessPolicy& policy);

// Running totals for the exit report: how many tessellated draws ran emulated and
// how many were skipped, per pipeline. A nonzero skip count with a black screen
// is the first thing to check in a bug report.
struct TessCounters {
    std::uint64_t hardware = 0;
    std::uint64_t compute = 0;
    std::uint64_t skipped = 0;
    std::uint64_t skipped_isoline = 0;
    std::uint64_t skipped_lds = 0;
    std::uint64_t skipped_disabled = 0;

    void Note(const TessDecision& decision);
};

[[nodiscard]] std::string DescribeTess(const TessCounters& counters);

}  // namespace bbx::d3d12
