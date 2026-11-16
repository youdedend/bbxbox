// SPDX-License-Identifier: GPL-3.0-or-later
// <dump>/bbx_shaders.paths: the bake's binding assignment, as the console reads it.
//
// The bake translates every program, so it has what the console cannot compute:
// for each binding the baked shader declares, the `ResourcePath` the translator
// found (a user SGPR, the dereferences, an offset in the last block - see
// bbx_guest_path.h) and the binding index the console's HLSL declared for it. That
// pairing is the one thing standing between 5B-2a's null space-1 descriptors and a
// dispatch that reads the game's buffers, and it must not be guessed on the
// console: `t7` in the baked HLSL is whatever the translator made it.
//
// So the bake writes it beside the cache, and this is the reader. The file is
// text, small next to the DXIL, and validated before a single plan is trusted:
//
//   # bbx_shaders.paths v1
//   stamp <32 hex digits: the cache stamp's digest>
//   bind <key:16 hex> <stage:u32> <binding:u32> <srv|uav> <user=..;imm=..;..>
//   ...
//
// A file whose stamp is not the cache's is refused whole - a plan from another
// translator's run describes another shader's bindings - and so is a line this
// format does not define. Refusing is cheap; binding the wrong buffer is not.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "host/d3d12/bbx_cache.h"
#include "host/d3d12/bbx_guest_path.h"

namespace bbx::d3d12 {

enum class PathTableStatus : std::uint8_t {
    kOk = 0,
    kMissing,       // nothing was passed: the bake has not run for this dump
    kNotATable,     // the header is not this file
    kStampMismatch, // written for another cache
    kCorrupt,       // a line this format does not define, or a duplicate binding
};

[[nodiscard]] const char* PathTableStatusName(PathTableStatus status);

// What a binding is: a shader-visible read (t#) or a read-write storage buffer
// (u#). A written binding is a UAV in D3D12 and nothing else is.
enum class BindingKind : std::uint8_t { kSrv = 0, kUav = 1 };

struct BindingPlan {
    std::uint32_t binding = 0;
    BindingKind kind = BindingKind::kSrv;
    PathPlan path;
};

class PathTable {
public:
    // `expected` is the digest of the stamp the *cache* validated: the two files
    // are one artefact, and a mismatch means one of them came from another bake.
    PathTableStatus Load(std::string_view text, const StampDigest& expected);

    // The plan for `key`'s `binding`, or nullptr when the bake recorded none - a
    // binding the console may not bind, never a default descriptor.
    [[nodiscard]] const BindingPlan* Find(std::uint64_t key, std::uint32_t binding) const;

    [[nodiscard]] std::size_t bindings() const { return plans_.size(); }
    [[nodiscard]] std::size_t programs() const { return programs_; }
    [[nodiscard]] PathTableStatus status() const { return status_; }
    [[nodiscard]] const std::string& detail() const { return detail_; }
    // "ok: 857 bindings on 311 programs" or "stamp mismatch: ..."
    [[nodiscard]] std::string Report() const;

    // The line the bake writes for one binding, and back. Exposed so both sides
    // use one spelling.
    [[nodiscard]] static std::string FormatBinding(std::uint64_t key, std::uint32_t stage, const BindingPlan& plan);
    // The two lines that are not records: the header the reader checks first and
    // the stamp it pairs with the cache. Spelled here so the bake and both
    // readers cannot drift.
    [[nodiscard]] static const char* HeaderLine();
    [[nodiscard]] static std::string FormatStamp(const StampDigest& digest);

private:
    // (key, binding) -> plan, with the pair folded into one integer. A duplicate is
    // refused by the loader rather than silently keeping the last one: two plans for
    // one binding is a bake that disagreed with itself.
    std::unordered_map<std::uint64_t, BindingPlan> plans_;
    std::unordered_set<std::uint64_t> program_keys_;
    std::size_t programs_ = 0;
    PathTableStatus status_ = PathTableStatus::kMissing;
    std::string detail_;
};

}  // namespace bbx::d3d12
