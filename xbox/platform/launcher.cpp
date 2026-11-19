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
    case Action::kToggleMods: return "toggle-mods";
    case Action::kToggleModdedEboot: return "toggle-modded-eboot";
    case Action::kContinueUnpatched: return "continue-unpatched";
    case Action::kToggleFpsCap: return "toggle-fps-cap";
    case Action::kToggleVsync: return "toggle-vsync";
    case Action::kToggleSharpUpscale: return "toggle-sharp-upscale";
    case Action::kTogglePerfHud: return "toggle-perf-hud";
    case Action::kSyncCacheToUsb: return "sync-cache-to-usb";
    case Action::kSyncCacheFromUsb: return "sync-cache-from-usb";
    case Action::kBackupSavesToUsb: return "backup-saves-to-usb";
    case Action::kRestoreSavesFromUsb: return "restore-saves-from-usb";
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
        items.push_back(Item{Action::kStartGame, "Awaken Above Ground (Start Game)",
                             "Enter Yharnam; Bloodborne v1.09 awakens on Xbox Series X|S", !precompiling, false});
        items.push_back(Item{Action::kPrecompile,
                             precompiling ? "Communing with Caryll Runes..." : (cache_current ? "Rebuild shader cache" : "Precompile shaders"),
                             "Etch PS4 GCN shaders natively to Xbox DXIL SM 6.4 and unload DXC",
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
    if (mods_present) {
        items.push_back(Item{Action::kToggleMods, mods_enabled ? "Mods overlay: on" : "Mods overlay: off",
                             "Dump\\mods resolves ahead of dvdroot_ps4; the dump itself is not modified",
                             true, false});
        // Only offered with the overlay on: a patched eboot without the files
        // it was patched for is a build the port cannot vouch for at all.
        if (mods_enabled) {
            items.push_back(Item{Action::kToggleModdedEboot,
                                 modded_eboot_ok ? "Allow modded eboot: on" : "Allow modded eboot: off",
                                 "Load a v1.09 eboot whose SHA-256 is not the pinned one; the digest is logged",
                                 true, true});
        }
    }
    items.push_back(Item{Action::kToggleFpsCap,
                         fps_cap >= 60 ? "Frame rate cap: 60 FPS" : "Frame rate cap: 30 FPS",
                         "Toggle between native 30 FPS pacing and unlocked 60 FPS (also in L3+R3 menu)",
                         !precompiling, false});
    items.push_back(Item{Action::kToggleVsync,
                         vsync ? "VSync: on" : "VSync: off",
                         "Lock presentation to display refresh to eliminate tearing",
                         !precompiling, false});
    items.push_back(Item{Action::kToggleSharpUpscale,
                         sharp_upscale ? "Upscaling: FSR RCAS" : "Upscaling: Bilinear",
                         "HLSL SM 6.4 contrast-adaptive sharpening for 1440p/4K TVs",
                         !precompiling, false});
    items.push_back(Item{Action::kTogglePerfHud,
                         perf_hud ? "Performance HUD: on" : "Performance HUD: off",
                         "Show live FPS, frame time, RAM usage and flips in-game (L3+R3 toggles)",
                         !precompiling, false});
    if (has_usb && !syncing) {
        items.push_back(Item{Action::kSyncCacheToUsb, "Copy shader cache to USB",
                             "Export LocalState DXIL cache to your dump folder for instant reuse",
                             !precompiling, false});
        items.push_back(Item{Action::kSyncCacheFromUsb, "Copy shader cache from USB",
                             "Import a verified DXIL cache from your dump folder into LocalState",
                             !precompiling, false});
        items.push_back(Item{Action::kBackupSavesToUsb, "Backup saves & logs to USB",
                             "Copy LocalState savedata, bbxbox.ini and logs to bbxbox_backups",
                             !precompiling, false});
        items.push_back(Item{Action::kRestoreSavesFromUsb, "Restore saves from USB",
                             "Restore bbxbox_backups\\savedata into LocalState\\data\\savedata",
                             !precompiling, false});
    }
    items.push_back(Item{Action::kMemorySelfTest, "Run memory self-test",
                         "Walks the guest memory contract; writes LocalState\\memory-selftest.txt",
                         !precompiling, false});
    items.push_back(Item{Action::kQuit, "Return to Waking World (Quit)", "Close Bloodborne", true, false});

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
    return "BLOODBORNE  |  THE HUNTER'S DREAM";
}

std::string LauncherModel::Footer() const {
    std::string text = BbPanel::PanelText(layout);
    const std::string banner = BbPanel::BaseOnlyBanner(layout);
    if (!banner.empty()) text += "\n\n" + banner;
    text += "\n" + BbPanel::CacheLine(stages, pipelines, cache_stamp, cache_current);
    if (!status_banner.empty()) text += "\nStatus: " + status_banner;
    const Item* item = Current();
    if (item && !item->hint.empty()) text += "\n\n" + item->label + ": " + item->hint;
    return text;
}

}  // namespace bbx::platform
