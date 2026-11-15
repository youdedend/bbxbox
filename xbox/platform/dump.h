// SPDX-License-Identifier: GPL-3.0-or-later
// Dump discovery and layout: which folder is the game, which is the 1.09
// update, and what version will actually run.
//
// The rules are the ones that cost the prior art a hardware test: a separate
// update folder that is not read boots 01.00 and runs the wrong build. So the
// version is read from each dump's own `sce_sys/param.sfo`, never inferred, and
// a candidate update has to prove itself (same title, strictly newer, carries
// an eboot).
//
// Everything here is pure: the file system arrives as callbacks, so the tests
// run it against an in-memory dump and the console runs it against
// `*FromApp` calls.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "xbox/platform/sfo_version.h"

namespace bbx::platform {

// The fields of `sce_sys/param.sfo` the port cares about.
struct SfoInfo {
    std::string app_ver;    // "01.09"
    std::string app_type;   // "gp", "patch", ...
    std::string title_id;   // "CUSA03173"
    std::string content_id; // "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100"
    std::string title;      // "Bloodborne"

    [[nodiscard]] std::string Version() const {
        return !app_ver.empty() ? app_ver : BbPrepare::UpdateVersionFromContentId(content_id);
    }
    [[nodiscard]] bool IsPatchContainer() const { return BbPrepare::IsPatchAppType(app_type); }
    [[nodiscard]] bool Valid() const { return !TitleId().empty() && !Version().empty(); }
    [[nodiscard]] std::string TitleId() const;
};

// Parses a `param.sfo` image. Returns `nullopt` for anything that is not a
// well-formed SFO; it never guesses an empty version, because an empty version
// that looks valid is worse than no answer.
[[nodiscard]] std::optional<SfoInfo> ParseParamSfo(const std::vector<std::uint8_t>& bytes);

// The file system, as the console provides it.
struct DumpFs {
    std::function<bool(const std::string& path)> exists;
    std::function<bool(const std::string& path)> is_dir;
    // Reads at most `cap` bytes; empty when unreadable.
    std::function<std::vector<std::uint8_t>(const std::string& path, std::size_t cap)> read;
    // Child directory names (not paths) of `path`.
    std::function<std::vector<std::string>(const std::string& path)> list_dirs;
};

enum class LayoutKind : std::uint8_t { kNone = 0, kMerged, kSplit, kBaseOnly };

[[nodiscard]] const char* LayoutKindName(LayoutKind kind);

// What the launcher needs to know.
struct DumpLayout {
    LayoutKind kind = LayoutKind::kNone;
    std::string base_path;   // the folder with eboot.bin
    std::string patch_path;  // empty when there is no separate update
    SfoInfo base;
    SfoInfo patch;
    std::string effective_ver;
    bool use_patch = true;   // `[dump] use_patch`
    bool unpatched_confirmed = false;
    std::string problem;     // why kind == kNone

    // Running an update build: either the update overlay is in use, or the base
    // dump is already the patched build.
    [[nodiscard]] bool Patched() const {
        if (kind == LayoutKind::kMerged) return true;
        return use_patch && !patch_path.empty() && kind == LayoutKind::kSplit;
    }
    [[nodiscard]] std::string EffectiveVersion() const {
        if (layout_ok()) return effective_ver;
        return base.Version();
    }
    [[nodiscard]] bool layout_ok() const { return kind != LayoutKind::kNone; }
    [[nodiscard]] bool CanRunPatched() const { return kind == LayoutKind::kSplit && use_patch; }
};

// True when `path` looks like a dump root: eboot.bin, sce_sys/param.sfo and
// dvdroot_ps4 all present.
[[nodiscard]] bool IsDumpRoot(const DumpFs& fs, const std::string& path);

// Reads and validates one dump folder's metadata.
[[nodiscard]] std::optional<SfoInfo> ReadDumpInfo(const DumpFs& fs, const std::string& path);

// The candidate update folders for `base`: the player's saved/preferred path
// first, then siblings named "<base>-patch" / "<base>-update" / "<base> patch".
[[nodiscard]] std::vector<std::string> PatchCandidates(const DumpFs& fs, const std::string& base,
                                                       const std::string& preferred);

// Decides the layout. `preferred_patch` may be empty. `use_patch` is the
// persisted toggle; `unpatched_confirmed` says the player accepted a base-only
// run this session.
[[nodiscard]] DumpLayout DetectLayout(const DumpFs& fs, const std::string& picked,
                                      const std::string& preferred_patch, bool use_patch,
                                      bool unpatched_confirmed = false);

// The one line the launcher logs for the chosen layout; stable enough that a
// bug report can be matched to it.
[[nodiscard]] std::string LayoutLogLine(const DumpLayout& layout);

// Where the effective version leaves the port: 01.09 is what bbhost targets.
[[nodiscard]] bool VersionSupported(const std::string& version);

}  // namespace bbx::platform
