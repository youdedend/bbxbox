// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/host/d3d12/bbx_guest_binding.h"

#include <cstdio>

namespace bbx::d3d12 {
namespace {

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

}  // namespace

const char* BindingVerdictName(BindingVerdict verdict) {
    switch (verdict) {
    case BindingVerdict::kWrite: return "a view over one slab";
    case BindingVerdict::kNull: return "a null V# (GCN reads zeroes)";
    case BindingVerdict::kNoPlan: return "no plan from the bake";
    case BindingVerdict::kUnreadable: return "the plan did not resolve from this dispatch's user data";
    case BindingVerdict::kRefusedSlot: return "the V# names a range this port cannot bind";
    case BindingVerdict::kNotContiguous: return "the range crosses a slab boundary";
    }
    return "unknown";
}

BindingWrite DecideBindingWrite(const BindingPlan& plan, const std::uint32_t* user, const GuestPageTable& table,
                                ReadGuest read, void* context, std::uint64_t base_limit) {
    BindingWrite out;
    out.binding = plan.binding;
    out.kind = plan.kind;
    // Step 1: the words, from this dispatch's own user data. Four, because a V#
    // is four dwords whatever the binding is: a store still names the same V#.
    std::uint32_t words[4] = {0, 0, 0, 0};
    const PathResult resolved = ResolvePath(plan.path, user, 4, read, context, words);
    if (resolved.verdict != PathVerdict::kResolved) {
        out.verdict = BindingVerdict::kUnreadable;
        out.why = std::string("step 1 (path ") + plan.path.str() + "): " + PathVerdictName(resolved.verdict);
        return out;
    }
    // Step 2: is there a range the guest's page table can address?
    const SlotDecision decision = DecideSlot(words, base_limit);
    out.view = decision.view;
    if (decision.verdict == SlotVerdict::kEmpty) {
        out.verdict = BindingVerdict::kNull;
        out.why = "step 2: the V# is null (base 0, no records)";
        return out;
    }
    if (decision.verdict != SlotVerdict::kBindable) {
        out.verdict = BindingVerdict::kRefusedSlot;
        out.why = std::string("step 2 (V# " + Hex(decision.view.base) + " + " + Hex(decision.view.bytes) +
                              " bytes): ") +
                  SlotVerdictName(decision.verdict);
        return out;
    }
    // Step 3: one buffer view names one resource, so the range has to be inside a
    // single slab. A range that starts in one slab and ends in the next is not
    // bindable at all - a size the slab cannot hold.
    std::uint32_t slab = 0;
    std::uint64_t offset = 0;
    if (!table.ContiguousSlab(decision.view.base, decision.view.bytes, &slab, &offset)) {
        out.verdict = BindingVerdict::kNotContiguous;
        out.why = "step 3 (V# " + Hex(decision.view.base) + " + " + Hex(decision.view.bytes) +
                  " bytes): not one contiguous mapped range inside a single slab";
        return out;
    }
    out.verdict = BindingVerdict::kWrite;
    out.slab = slab;
    out.offset = offset;
    out.dwords = static_cast<std::uint32_t>((decision.view.bytes + 3) / 4);
    return out;
}

std::string BindingWriteLine(const BindingWrite& write) {
    char name[8];
    std::snprintf(name, sizeof(name), "%c%u", write.kind == BindingKind::kUav ? 'u' : 't', write.binding);
    if (write.verdict == BindingVerdict::kWrite) {
        char text[160];
        std::snprintf(text, sizeof(text), "%s: %u dword(s) at slab %u + 0x%llx (guest 0x%llx)", name, write.dwords,
                      write.slab, static_cast<unsigned long long>(write.offset),
                      static_cast<unsigned long long>(write.view.base));
        return text;
    }
    if (write.verdict == BindingVerdict::kNull) return std::string(name) + ": null V#, the slot stays a null descriptor";
    return std::string(name) + " refused (" + BindingVerdictName(write.verdict) + "): " + write.why;
}

}  // namespace bbx::d3d12
