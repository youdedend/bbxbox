// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from youdedend/Test-project (xbox/platform/dump_panel.h, GPL-2.0-or-later).
//
// The text the launcher puts in front of the player, as pure string builders:
// main.cpp owns WinRT and cannot be built on a CI machine, so the sentences a
// player sees (and the lines a bug report quotes) live here where the tests can
// pin them.
#pragma once

#include <string>

#include "xbox/platform/dump.h"
#include "xbox/platform/sfo_version.h"

namespace BbPanel {

inline std::string VersionText(const std::string& version) {
    return version.empty() ? std::string("unknown") : version;
}

inline std::string PathText(const std::string& path) {
    return path.empty() ? std::string("-") : path;
}

/// `<path> - app_ver=01.09 (gp) CUSA03173`
inline std::string DescribeDump(const std::string& path, const bbx::platform::SfoInfo& info) {
    std::string text = PathText(path) + " - app_ver=" + VersionText(info.Version());
    if (!info.app_type.empty()) text += " (" + info.app_type + ")";
    if (!info.TitleId().empty()) text += " " + info.TitleId();
    return text;
}

/// The summary above the buttons.
inline std::string PanelText(const bbx::platform::DumpLayout& layout) {
    std::string text =
        "Base dump:  " + DescribeDump(layout.base_path, layout.base) + "\n";
    if (layout.patch_path.empty()) {
        text += "Patch dump: none found\n";
    } else if (layout.use_patch) {
        text += "Patch dump: " + DescribeDump(layout.patch_path, layout.patch) + " (in use)\n";
    } else {
        text += "Patch dump: " + DescribeDump(layout.patch_path, layout.patch) +
                " (NOT USED - \"Use patch\" is off)\n";
    }
    text += "Effective version: " + VersionText(layout.EffectiveVersion());
    if (!layout.CanRunPatched() && layout.kind != bbx::platform::LayoutKind::kMerged) {
        text += " - " + std::string(BbPrepare::kPatchTargetVersion) + " is required";
    }
    return text;
}

/// The warning when the run would not be the build bbhost targets.
inline std::string BaseOnlyBanner(const bbx::platform::DumpLayout& layout) {
    if (layout.kind != bbx::platform::LayoutKind::kBaseOnly) return {};
    return "! No " + std::string(BbPrepare::kPatchTargetVersion) +
           " update was found for this dump.\n"
           "bbhost is measured against 1.09: running " +
           VersionText(layout.base.Version()) +
           " is unsupported and may crash when gameplay starts.\n\n"
           "Choose the update folder - the one holding its own eboot.bin and sce_sys\\param.sfo - "
           "or continue anyway, at your own risk. The dump itself is never modified.";
}

/// `Dump: ...` / `Cache: ...` footer for the launcher.
inline std::string CacheLine(std::size_t stages, std::size_t pipelines, const std::string& stamp,
                             bool current) {
    return "Cache: " + std::to_string(stages) + " stages, " + std::to_string(pipelines) +
           " pipelines, stamp " + (stamp.empty() ? std::string("none") : stamp.substr(0, 8)) +
           (current ? " (this build)" : " (rebuilding)");
}

/// One line in loader.log.txt that says what will run and whether it is the
/// build the port supports.
inline std::string LaunchTargetLine(const bbx::platform::DumpLayout& layout) {
    const char* patches = layout.CanRunPatched() ? "required"
                           : layout.kind == bbx::platform::LayoutKind::kMerged ? "required"
                                                                             : "skipped";
    return "Launch target: base=" + PathText(layout.base_path) +
           "; patch=" + PathText(layout.patch_path) +
           "; effective=" + VersionText(layout.EffectiveVersion()) + "; patches=" + patches;
}

}  // namespace BbPanel
