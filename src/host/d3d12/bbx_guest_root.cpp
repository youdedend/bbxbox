// SPDX-License-Identifier: GPL-3.0-or-later
// The guest shaders' root layout: see bbx_guest_root.h. Portable, and the only
// place the numbers live, so pipeline.cpp and the tests cannot disagree.
#include "host/d3d12/bbx_guest_root.h"

#include <sstream>

namespace bbx::d3d12 {

const char* GuestRangeName(GuestRangeKind kind) {
    switch (kind) {
    case GuestRangeKind::kSrv: return "srv";
    case GuestRangeKind::kUav: return "uav";
    case GuestRangeKind::kSampler: return "sampler";
    }
    return "?";
}

const GuestRange* GuestRootLayout::Find(GuestRangeKind kind, std::uint32_t reg, std::uint32_t space) const {
    for (const GuestRootParam& p : params) {
        for (const GuestRange& r : p.ranges) {
            if (r.kind == kind && r.Covers(reg, space)) return &r;
        }
    }
    return nullptr;
}

std::string GuestRootLayout::Describe() const {
    std::ostringstream out;
    out << "guest root: " << params.size() << " parameter(s), " << Dwords() << "/" << kMaxDwords << " DWORDs";
    for (const GuestRootParam& p : params) {
        if (p.kind == GuestRootParamKind::kParamsCbv) {
            out << "\n  b" << kGuestParamsRegister << ", space" << kGuestParamsSpace << ": StageParams, "
                << kStageParamsBytes << " B (root CBV, 2 DWORDs)";
            continue;
        }
        out << "\n  table in space " << (p.kind == GuestRootParamKind::kGuestMemory ? kGuestSpace : kGuestParamsSpace) << ":";
        for (const GuestRange& r : p.ranges) {
            out << " " << GuestRangeName(r.kind) << r.base_register << ".." << r.LastRegister() << " (" << r.count << ")";
        }
    }
    return out.str();
}

GuestRootLayout GuestRoot() {
    GuestRootLayout layout;
    // b0 space1: StageParams. A root CBV, so the dispatch costs one pointer
    // write and no descriptor.
    GuestRootParam params;
    params.kind = GuestRootParamKind::kParamsCbv;
    layout.params.push_back(std::move(params));

    // space1: the stage's own resources. One table, three ranges - a range's
    // class and register number are independent, as a guest's binding numbers
    // are (its t0 and u0 are different registers, not different spaces).
    GuestRootParam stage;
    stage.kind = GuestRootParamKind::kStageResources;
    stage.ranges.push_back(GuestRange{GuestRangeKind::kSrv, 0, kGuestSrvCount, kGuestParamsSpace});
    stage.ranges.push_back(GuestRange{GuestRangeKind::kUav, 0, kGuestUavCount, kGuestParamsSpace});
    stage.ranges.push_back(GuestRange{GuestRangeKind::kSampler, 0, kGuestSamplerCount, kGuestParamsSpace});
    layout.params.push_back(std::move(stage));

    // space3: the console's guest memory (ARCHITECTURE §3.3), at the registers
    // the baked shaders name: t0 the page table, u0..u31 the slabs.
    GuestRootParam memory;
    memory.kind = GuestRootParamKind::kGuestMemory;
    memory.ranges.push_back(GuestRange{GuestRangeKind::kSrv, kGuestPageTableRegister, 1, kGuestSpace});
    memory.ranges.push_back(GuestRange{GuestRangeKind::kUav, kGuestSlabRegister, kGuestSlabCount, kGuestSpace});
    layout.params.push_back(std::move(memory));
    return layout;
}

bool GuestBinding(const GuestRootLayout& layout, GuestRangeKind kind, std::uint32_t reg, std::uint32_t space,
                  GuestBindingSlot* out) {
    for (std::size_t p = 0; p < layout.params.size(); ++p) {
        const GuestRootParam& param = layout.params[p];
        for (std::size_t r = 0; r < param.ranges.size(); ++r) {
            const GuestRange& range = param.ranges[r];
            if (range.kind != kind || !range.Covers(reg, space)) continue;
            out->param = param.kind;
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
