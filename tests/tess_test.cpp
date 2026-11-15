// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "src/host/d3d12/bbx_tess.h"

using namespace bbx::d3d12;

namespace {

TessDraw Patch(std::uint32_t patches = 100, std::uint32_t lds = 1024) {
    TessDraw draw;
    draw.has_hull_shader = true;
    draw.has_domain_shader = true;
    draw.patch_count = patches;
    draw.hull_group_shared = lds;
    return draw;
}

}  // namespace

TEST(PlainDrawsAreNotTessellated) {
    const TessDecision decision = ChooseTessPath(XboxSeriesCaps(), TessDraw{}, TessPolicy{});
    CHECK(decision.path == TessPath::kNone);
}

TEST(XboxNeverUsesTheHardwareTessellator) {
    const TessDecision decision = ChooseTessPath(XboxSeriesCaps(), Patch(), TessPolicy{});
    CHECK(decision.path == TessPath::kCompute);
    CHECK_EQ(decision.chunks, 1u);
    // Desktop caps do use it.
    CHECK(ChooseTessPath(DesktopCaps(), Patch(), TessPolicy{}).path == TessPath::kHardware);
}

TEST(TessellationOffSkipsInsteadOfHanging) {
    TessPolicy policy;
    policy.emulation_enabled = false;
    const TessDecision decision = ChooseTessPath(XboxSeriesCaps(), Patch(), policy);
    CHECK(decision.path == TessPath::kSkip);
    CHECK(decision.reason.find("off") != std::string::npos);
}

TEST(IsolineAndOversizedLdsAreSkipped) {
    TessDraw isoline = Patch();
    isoline.isoline = true;
    CHECK(ChooseTessPath(XboxSeriesCaps(), isoline, TessPolicy{}).path == TessPath::kSkip);
    TessDraw big = Patch(100, 40u * 1024u);
    const TessDecision decision = ChooseTessPath(XboxSeriesCaps(), big, TessPolicy{});
    CHECK(decision.path == TessPath::kSkip);
    CHECK(decision.reason.find("group shared") != std::string::npos);
}

TEST(PatchesBeyondAChunkAreSplit) {
    TessDraw draw = Patch(1000);
    draw.max_patches_per_chunk = 256;
    const TessDecision decision = ChooseTessPath(XboxSeriesCaps(), draw, TessPolicy{});
    CHECK(decision.path == TessPath::kCompute);
    CHECK_EQ(decision.chunks, 4u);
}

TEST(CountersExplainABlackScreen) {
    TessCounters counters;
    counters.Note(ChooseTessPath(XboxSeriesCaps(), Patch(), TessPolicy{}));
    TessPolicy off;
    off.emulation_enabled = false;
    counters.Note(ChooseTessPath(XboxSeriesCaps(), Patch(), off));
    TessDraw isoline = Patch();
    isoline.isoline = true;
    counters.Note(ChooseTessPath(XboxSeriesCaps(), isoline, TessPolicy{}));
    CHECK_EQ(counters.compute, 1ull);
    CHECK_EQ(counters.skipped, 2ull);
    CHECK_EQ(counters.skipped_disabled, 1ull);
    CHECK_EQ(counters.skipped_isoline, 1ull);
    const std::string text = DescribeTess(counters);
    CHECK(text.find("1 compute") != std::string::npos);
    CHECK(text.find("2 skipped") != std::string::npos);
}
