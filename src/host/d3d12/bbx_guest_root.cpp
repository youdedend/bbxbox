// SPDX-License-Identifier: GPL-3.0-or-later
// The guest shaders' root layouts: see bbx_guest_root.h. Portable, and the only
// place the numbers live, so pipeline.cpp and the tests cannot disagree.
#include "src/host/d3d12/bbx_guest_root.h"

#include <sstream>

namespace bbx::d3d12 {
namespace {

// The space-1 ranges every stage's resources table carries, in the order the
// heap region lays them out (kCbvOffset, kSrvOffset, kUavOffset).
void PushSpace1Ranges(GuestRootParam& param) {
    param.ranges.push_back(GuestRange{GuestRangeKind::kCbv, 1, kGuestCbvCount, kGuestParamsSpace});
    param.ranges.push_back(GuestRange{GuestRangeKind::kSrv, 0, kGuestSrvCount, kGuestParamsSpace});
    param.ranges.push_back(GuestRange{GuestRangeKind::kUav, 0, kGuestUavCount, kGuestParamsSpace});
}

void PushSamplerRanges(GuestRootParam& param) {
    param.ranges.push_back(GuestRange{GuestRangeKind::kSampler, 0, kGuestSamplerCount, kGuestParamsSpace});
}

GuestRootParam MakeParams(GuestVisibility visibility) {
    GuestRootParam p;
    p.kind = GuestRootParamKind::kParamsCbv;
    p.visibility = visibility;
    return p;
}

// space3: the console's guest memory (ARCHITECTURE §3.3), at the registers the
// baked shaders name: t0 the page table, u0..u31 the slabs.
GuestRootParam MakeGuestMemory() {
    GuestRootParam p;
    p.kind = GuestRootParamKind::kGuestMemory;
    p.visibility = GuestVisibility::kAll;
    p.ranges.push_back(GuestRange{GuestRangeKind::kSrv, kGuestPageTableRegister, 1, kGuestSpace});
    p.ranges.push_back(GuestRange{GuestRangeKind::kUav, kGuestSlabRegister, kGuestSlabCount, kGuestSpace});
    return p;
}

}  // namespace

const char* GuestStageName(GuestStage stage) {
    switch (stage) {
    case GuestStage::kCompute: return "compute";
    case GuestStage::kGraphics: return "graphics";
    }
    return "?";
}

const char* GuestRangeName(GuestRangeKind kind) {
    switch (kind) {
    case GuestRangeKind::kCbv: return "cbv";
    case GuestRangeKind::kSrv: return "srv";
    case GuestRangeKind::kUav: return "uav";
    case GuestRangeKind::kSampler: return "sampler";
    }
    return "?";
}

bool GuestRangeIsSampler(GuestRangeKind kind) { return kind == GuestRangeKind::kSampler; }

const char* GuestVisibilityName(GuestVisibility v) {
    switch (v) {
    case GuestVisibility::kAll: return "all";
    case GuestVisibility::kVertex: return "vertex";
    case GuestVisibility::kPixel: return "pixel";
    }
    return "?";
}

bool GuestRootLayout::Valid() const {
    if (params.empty()) return false;  // a signature with nothing in it serves no shader
    if (Dwords() > kMaxDwords) return false;  // the console's ceiling
    for (const GuestRootParam& p : params) {
        // Rule 1: a table is all samplers or no samplers.
        bool sampler = false, other = false;
        for (const GuestRange& r : p.ranges) {
            if (r.count == 0) return false;
            if (GuestRangeIsSampler(r.kind)) {
                sampler = true;
            } else {
                other = true;
            }
            // b0 is the root CBV; a range that covered it would be a second
            // view of one register.
            if (r.kind == GuestRangeKind::kCbv && r.space == kGuestParamsSpace && r.base_register == 0) return false;
        }
        if (sampler && other) return false;
        if (p.kind == GuestRootParamKind::kStageSamplers && !sampler) return false;
        if (p.kind != GuestRootParamKind::kStageSamplers && sampler) return false;
        if (p.kind == GuestRootParamKind::kParamsCbv && !p.ranges.empty()) return false;
        if (p.kind != GuestRootParamKind::kParamsCbv && p.ranges.empty()) return false;
        // Two ranges of one class may not overlap inside one table: the second
        // would be a descriptor nothing can name.
        for (std::size_t a = 0; a < p.ranges.size(); ++a) {
            for (std::size_t b = a + 1; b < p.ranges.size(); ++b) {
                const GuestRange& x = p.ranges[a];
                const GuestRange& y = p.ranges[b];
                if (x.kind != y.kind || x.space != y.space) continue;
                if (x.base_register <= y.LastRegister() && y.base_register <= x.LastRegister()) return false;
            }
        }
    }
    // One root CBV per visibility, and no parameter kind twice for one
    // visibility: two of either is a register bound twice.
    for (std::size_t a = 0; a < params.size(); ++a) {
        for (std::size_t b = a + 1; b < params.size(); ++b) {
            if (params[a].visibility != params[b].visibility) continue;
            if (params[a].kind == params[b].kind && params[a].kind != GuestRootParamKind::kGuestMemory) return false;
        }
    }
    return true;
}

const GuestRange* GuestRootLayout::Find(GuestRangeKind kind, std::uint32_t reg, std::uint32_t space,
                                        GuestVisibility visible_to) const {
    for (const GuestRootParam& p : params) {
        if (p.visibility != GuestVisibility::kAll && p.visibility != visible_to) continue;
        for (const GuestRange& r : p.ranges) {
            if (r.kind == kind && r.Covers(reg, space)) return &r;
        }
    }
    return nullptr;
}

std::string GuestRootLayout::Describe() const {
    std::ostringstream out;
    out << "guest root (" << GuestStageName(stage) << "): " << params.size() << " parameter(s), " << Dwords() << "/"
        << kMaxDwords << " DWORDs";
    for (const GuestRootParam& p : params) {
        out << "\n  [" << GuestVisibilityName(p.visibility) << "] ";
        if (p.kind == GuestRootParamKind::kParamsCbv) {
            out << "b" << kGuestParamsRegister << ", space" << kGuestParamsSpace << ": StageParams, "
                << kStageParamsBytes << " B (root CBV, 2 DWORDs)";
            continue;
        }
        out << "table in space " << (p.kind == GuestRootParamKind::kGuestMemory ? kGuestSpace : kGuestParamsSpace) << ":";
        for (const GuestRange& r : p.ranges) {
            out << " " << GuestRangeName(r.kind) << r.base_register << ".." << r.LastRegister() << " (" << r.count << ")";
        }
    }
    return out.str();
}

GuestRootLayout GuestRoot(GuestStage stage) {
    GuestRootLayout layout;
    layout.stage = stage;

    if (stage == GuestStage::kCompute) {
        // 2 (root CBV) + 1 (b1..b63, t0..t63, u0..u63) + 1 (s0..s63) + 1
        // (space 3) = 5 DWORDs, one visibility: a compute dispatch is one stage.
        layout.params.push_back(MakeParams(GuestVisibility::kAll));
        GuestRootParam stage1;
        stage1.kind = GuestRootParamKind::kStageResources;
        PushSpace1Ranges(stage1);
        layout.params.push_back(std::move(stage1));
        GuestRootParam samplers;
        samplers.kind = GuestRootParamKind::kStageSamplers;
        PushSamplerRanges(samplers);
        layout.params.push_back(std::move(samplers));
        layout.params.push_back(MakeGuestMemory());
        return layout;
    }

    // A vertex/pixel draw: the same tables twice, once per stage (rule 3), plus
    // the one guest-memory table both stages share. 4 + 4 + 1 = 9 DWORDs.
    layout.params.push_back(MakeParams(GuestVisibility::kVertex));
    GuestRootParam vs;
    vs.kind = GuestRootParamKind::kStageResources;
    vs.visibility = GuestVisibility::kVertex;
    PushSpace1Ranges(vs);
    layout.params.push_back(std::move(vs));
    GuestRootParam vs_samplers;
    vs_samplers.kind = GuestRootParamKind::kStageSamplers;
    vs_samplers.visibility = GuestVisibility::kVertex;
    PushSamplerRanges(vs_samplers);
    layout.params.push_back(std::move(vs_samplers));

    layout.params.push_back(MakeParams(GuestVisibility::kPixel));
    GuestRootParam ps;
    ps.kind = GuestRootParamKind::kStageResources;
    ps.visibility = GuestVisibility::kPixel;
    PushSpace1Ranges(ps);
    layout.params.push_back(std::move(ps));
    GuestRootParam ps_samplers;
    ps_samplers.kind = GuestRootParamKind::kStageSamplers;
    ps_samplers.visibility = GuestVisibility::kPixel;
    PushSamplerRanges(ps_samplers);
    layout.params.push_back(std::move(ps_samplers));

    layout.params.push_back(MakeGuestMemory());
    return layout;
}

bool GuestHeapSlotFor(GuestStage, GuestRangeKind kind, std::uint32_t reg, std::uint32_t space, GuestHeapSlot* out) {
    if (!out) return false;
    // space 3: the page table, then the slabs, in the order bbx_guest_mem
    // created their views.
    if (space == kGuestSpace) {
        if (kind == GuestRangeKind::kSrv && reg == kGuestPageTableRegister) {
            *out = GuestHeapSlot{false, GuestHeapLayout::kSpace3Base + 0};
            return true;
        }
        if (kind == GuestRangeKind::kUav && reg >= kGuestSlabRegister && reg < kGuestSlabRegister + kGuestSlabCount) {
            *out = GuestHeapSlot{false, GuestHeapLayout::kSpace3Base + 1 + (reg - kGuestSlabRegister)};
            return true;
        }
        return false;
    }
    if (space != kGuestParamsSpace) return false;
    switch (kind) {
        case GuestRangeKind::kCbv:
            if (reg < 1 || reg > kGuestCbvCount) return false;  // b0 is the root CBV
            *out = GuestHeapSlot{false, GuestHeapLayout::kCbvOffset + (reg - 1)};
            return true;
        case GuestRangeKind::kSrv:
            if (reg >= kGuestSrvCount) return false;
            *out = GuestHeapSlot{false, GuestHeapLayout::kSrvOffset + reg};
            return true;
        case GuestRangeKind::kUav:
            if (reg >= kGuestUavCount) return false;
            *out = GuestHeapSlot{false, GuestHeapLayout::kUavOffset + reg};
            return true;
        case GuestRangeKind::kSampler:
            if (reg >= kGuestSamplerCount) return false;
            *out = GuestHeapSlot{true, reg};  // the sampler heap's own region
            return true;
    }
    return false;
}

bool GuestBinding(const GuestRootLayout& layout, GuestRangeKind kind, std::uint32_t reg, std::uint32_t space,
                  GuestBindingSlot* out, GuestVisibility visible_to) {
    if (!out) return false;
    for (std::size_t p = 0; p < layout.params.size(); ++p) {
        const GuestRootParam& param = layout.params[p];
        if (param.visibility != GuestVisibility::kAll && param.visibility != visible_to) continue;
        for (std::size_t r = 0; r < param.ranges.size(); ++r) {
            const GuestRange& range = param.ranges[r];
            if (range.kind != kind || !range.Covers(reg, space)) continue;
            out->param = param.kind;
            out->param_index = static_cast<std::uint32_t>(p);
            out->range = static_cast<std::uint32_t>(r);
            // The table's slots are the ranges appended in order: a binding's
            // offset is what the ranges before it already took.
            std::uint32_t offset = 0;
            for (std::size_t before = 0; before < r; ++before) offset += param.ranges[before].count;
            out->offset = offset + (reg - range.base_register);
            return true;
        }
    }
    return false;
}

}  // namespace bbx::d3d12
