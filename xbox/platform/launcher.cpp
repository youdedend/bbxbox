// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox/platform/launcher.h"

#include "xbox/platform/dump_panel.h"

namespace bbx::platform {

const char* ActionName(Action action) {
    switch (action) {
    case Action::kNone: return "none";
    case Action::kStartGame: return "start";
    case Action::kPrecompile: return "precompile";
    case Action::kPickDump: return "pick-dump";
    case Action::kPickPatch: return "pick-patch";
    case Action::kTogglePatch: return "toggle-patch";
    case Action::kContinueUnpatched: return "continue-unpatched";
    case Action::kSyncCacheToUsb: return "sync-cache-to-usb";
    case Action::kSyncCacheFromUsb: return "sync-cache-from-usb";
    case Action::kMemorySelfTest: return "memory-self-test";
    case Action::kQuit: return "quit";
    }
    return "none";
}

void LauncherModel::Rebuild() {
    const Action previous = Current() ? Current()->action : Action::kNone;
    items.clear();

    const bool ready = layout.layout_ok() && !layout.base_path.empty();
    const bool supported = ready && VersionSupported(layout.EffectiveVersion());

    if (ready && supported) {
        items.push_back(Item{Action::kStartGame, "Start game",
                             "bbhost starts; the title's own menu follows", !precompiling, false});
        items.push_back(Item{Action::kPrecompile, precompiling ? "Precompiling…" : "Precompile shaders",
                             "Build the DXIL and pipeline caches on the title screen",
                             !precompiling, false});
    }
    if (ready && !supported) {
        items.push_back(Item{Action::kContinueUnpatched,
                             "Continue anyway (" + layout.EffectiveVersion() + ", unsupported)",
                             "Runs the unverified build; keep a save backup", !precompiling, true});
    }
    items.push_back(Item{Action::kPickDump, "Choose dump folder",
                         "Pick the folder holding eboot.bin, sce_module, sce_sys and dvdroot_ps4",
                         !precompiling, false});
    if (!layout.patch_path.empty()) {
        items.push_back(Item{Action::kTogglePatch,
                             layout.use_patch ? "Use patch: on" : "Use patch: off",
                             "Switch between the base dump and the 1.09 update", true, false});
    } else {
        items.push_back(Item{Action::kPickPatch, "Add 1.09 update folder",
                             "Choose the separate update folder, if your dump is split", true, false});
    }
    if (has_usb && !syncing) {
        items.push_back(Item{Action::kSyncCacheToUsb, "Copy shader cache to USB",
                             "Take this console's precompiled cache to another one", !precompiling,
                             false});
        items.push_back(Item{Action::kSyncCacheFromUsb, "Copy shader cache from USB",
                             "Import a cache built by the same build and device", !precompiling,
                             false});
    }
    items.push_back(Item{Action::kMemorySelfTest, "Run memory self-test",
                         "Walks the guest memory contract; writes LocalState\\memory-selftest.txt",
                         !precompiling, false});
    items.push_back(Item{Action::kQuit, "Quit", "Close bbxbox", true, false});

    selected = 0;
    if (previous != Action::kNone) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].action == previous && items[i].enabled) {
                selected = i;
                break;
            }
        }
    }
    if (selected < items.size() && !items[selected].enabled) Move(1);
}

void LauncherModel::Move(int delta) {
    if (items.empty()) return;
    const std::size_t count = items.size();
    std::size_t at = selected;
    for (std::size_t step = 0; step < count; ++step) {
        at = delta >= 0 ? (at + 1) % count : (at + count - 1) % count;
        if (items[at].enabled) {
            selected = at;
            return;
        }
    }
}

const Item* LauncherModel::Current() const {
    return selected < items.size() ? &items[selected] : nullptr;
}

Action LauncherModel::Activate() const {
    const Item* item = Current();
    if (!item || !item->enabled) return Action::kNone;
    return item->action;
}

std::string LauncherModel::Title() const {
    return "bbxbox - Bloodborne on Xbox Series X|S";
}

std::string LauncherModel::Footer() const {
    std::string text = BbPanel::PanelText(layout);
    const std::string banner = BbPanel::BaseOnlyBanner(layout);
    if (!banner.empty()) text += "\n\n" + banner;
    text += "\n" + BbPanel::CacheLine(stages, pipelines, cache_stamp, cache_current);
    const Item* item = Current();
    if (item && !item->hint.empty()) text += "\n\n" + item->label + ": " + item->hint;
    return text;
}

}  // namespace bbx::platform
