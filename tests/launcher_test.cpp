// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "xbox/platform/launcher.h"

#include <algorithm>
#include <string>

using namespace bbx::platform;

namespace {

DumpLayout ReadyLayout(LayoutKind kind = LayoutKind::kMerged) {
    DumpLayout layout;
    layout.kind = kind;
    layout.base_path = "E:\\dumps\\Bloodborne";
    layout.base.app_ver = kind == LayoutKind::kBaseOnly ? "01.00" : "01.09";
    layout.base.app_type = "gp";
    layout.base.title_id = "CUSA03173";
    if (kind == LayoutKind::kSplit) {
        layout.patch_path = "E:\\dumps\\Bloodborne-patch";
        layout.patch.app_ver = "01.09";
        layout.patch.app_type = "patch";
        layout.patch.title_id = "CUSA03173";
        layout.effective_ver = "01.09";
    } else {
        layout.effective_ver = layout.base.app_ver;
    }
    return layout;
}

bool Has(const LauncherModel& model, Action action) {
    return std::any_of(model.items.begin(), model.items.end(),
                       [action](const Item& item) { return item.action == action; });
}

}  // namespace

TEST(MenuForAMergedDumpIsStartAndPrecompile) {
    LauncherModel model;
    model.layout = ReadyLayout();
    model.Rebuild();
    CHECK(Has(model, Action::kStartGame));
    CHECK(Has(model, Action::kPrecompile));
    CHECK(Has(model, Action::kPickDump));
    CHECK(Has(model, Action::kPickPatch));  // no update yet: offer to add one
    CHECK(!Has(model, Action::kContinueUnpatched));
    CHECK(model.Current() != nullptr);
    CHECK(model.Current()->enabled);
    CHECK(model.Activate() == Action::kStartGame);
}

TEST(BaseOnlySwapsStartForContinueAnyway) {
    LauncherModel model;
    model.layout = ReadyLayout(LayoutKind::kBaseOnly);
    model.Rebuild();
    CHECK(!Has(model, Action::kStartGame));
    CHECK(!Has(model, Action::kPrecompile));
    CHECK(Has(model, Action::kContinueUnpatched));
    bool dangerous = false;
    for (const Item& item : model.items) {
        if (item.action == Action::kContinueUnpatched) dangerous = item.dangerous;
    }
    CHECK(dangerous);
    CHECK(model.Activate() == Action::kContinueUnpatched);
}

TEST(SplitOffersTheToggle) {
    LauncherModel model;
    model.layout = ReadyLayout(LayoutKind::kSplit);
    model.Rebuild();
    CHECK(Has(model, Action::kTogglePatch));
    CHECK(!Has(model, Action::kPickPatch));
}

TEST(PrecompilingDisablesStartButKeepsQuitUsable) {
    LauncherModel model;
    model.layout = ReadyLayout();
    model.precompiling = true;
    model.Rebuild();
    for (const Item& item : model.items) {
        if (item.action == Action::kStartGame || item.action == Action::kPrecompile) {
            CHECK(!item.enabled);
        }
        if (item.action == Action::kQuit) CHECK(item.enabled);
    }
    // The selection never rests on a disabled item.
    CHECK(model.Current() != nullptr);
    CHECK(model.Current()->enabled);
}

TEST(NavigationWrapsAndSkipsDisabled) {
    LauncherModel model;
    model.layout = ReadyLayout();
    model.Rebuild();
    const std::size_t count = model.items.size();
    CHECK(model.selected == 0);
    model.Move(-1);
    CHECK(model.selected == count - 1);
    model.Move(1);
    CHECK(model.selected == 0);
    CHECK(ActionName(model.Activate()) == std::string("start"));
}

TEST(UsbActionsAppearOnlyWithUsb) {
    LauncherModel model;
    model.layout = ReadyLayout();
    model.has_usb = false;
    model.Rebuild();
    CHECK(!Has(model, Action::kSyncCacheToUsb));
    model.has_usb = true;
    model.Rebuild();
    CHECK(Has(model, Action::kSyncCacheToUsb));
    CHECK(Has(model, Action::kSyncCacheFromUsb));
}

TEST(RebuildKeepsTheSelectionOnTheSameAction) {
    LauncherModel model;
    model.layout = ReadyLayout(LayoutKind::kSplit);
    model.Rebuild();
    while (model.Activate() != Action::kQuit) model.Move(1);
    model.stages = 12;
    model.Rebuild();
    CHECK(model.Activate() == Action::kQuit);
}

TEST(FooterStatesTheEffectiveVersion) {
    LauncherModel model;
    model.layout = ReadyLayout(LayoutKind::kSplit);
    model.cache_current = true;
    model.stages = 3206;
    model.pipelines = 1904;
    model.cache_stamp = "0123456789abcdef";
    model.Rebuild();
    const std::string footer = model.Footer();
    CHECK(footer.find("Effective version: 01.09") != std::string::npos);
    CHECK(footer.find("3206 stages") != std::string::npos);
    CHECK(footer.find("01234567") != std::string::npos);
}

TEST(BaseOnlyFooterWarnsInWords) {
    LauncherModel model;
    model.layout = ReadyLayout(LayoutKind::kBaseOnly);
    model.Rebuild();
    const std::string footer = model.Footer();
    CHECK(footer.find("01.09 is required") != std::string::npos);
    CHECK(footer.find("never modified") != std::string::npos);
}

// --- the <dump>\mods overlay ------------------------------------------------
// The Arabic localisation for CUSA03173 v1.09 is loose files in `msg/`, `font/`
// and `menu/` plus a patched `eboot.bin`: exactly what the two switches are for.

TEST(NoModsFolderNoModsItems) {
    LauncherModel model;
    model.layout = ReadyLayout();
    model.mods_present = false;
    model.Rebuild();
    CHECK(!Has(model, Action::kToggleMods));
    CHECK(!Has(model, Action::kToggleModdedEboot));
}

TEST(ModsFolderAddsTheOverlaySwitchOnByDefault) {
    LauncherModel model;
    model.layout = ReadyLayout();
    model.mods_present = true;
    model.mods_enabled = true;
    model.Rebuild();
    CHECK(Has(model, Action::kToggleMods));
    bool on = false;
    for (const Item& item : model.items) {
        if (item.action == Action::kToggleMods) on = item.label.find("on") != std::string::npos;
    }
    CHECK(on);
    // On by default also means the modded-eboot switch is offered.
    CHECK(Has(model, Action::kToggleModdedEboot));
}

TEST(OverlayOffHidesTheModdedEbootSwitch) {
    LauncherModel model;
    model.layout = ReadyLayout();
    model.mods_present = true;
    model.mods_enabled = false;
    model.Rebuild();
    CHECK(Has(model, Action::kToggleMods));
    // A patched eboot without the files it was patched for is not offered at all.
    CHECK(!Has(model, Action::kToggleModdedEboot));
}

TEST(ModdedEbootSwitchSaysAllowedWhenOn) {
    LauncherModel model;
    model.layout = ReadyLayout();
    model.mods_present = true;
    model.mods_enabled = true;
    model.modded_eboot_ok = true;
    model.Rebuild();
    bool allowed = false;
    for (const Item& item : model.items) {
        if (item.action == Action::kToggleModdedEboot) allowed = item.label.find("on") != std::string::npos;
    }
    CHECK(allowed);
    // Dangerous: it runs a build whose digest the port does not pin.
    for (const Item& item : model.items) {
        if (item.action == Action::kToggleModdedEboot) CHECK(item.dangerous);
    }
}

TEST(VideoTogglesUsbSaveActionsAndStatusBanner) {
    LauncherModel model;
    model.layout = ReadyLayout();
    model.has_usb = true;
    model.fps_cap = 60;
    model.vsync = false;
    model.sharp_upscale = true;
    model.perf_hud = true;
    model.status_banner = "Backed up 2 save file(s) and 3 log(s) to USB";
    model.Rebuild();

    CHECK(Has(model, Action::kToggleFpsCap));
    CHECK(Has(model, Action::kToggleVsync));
    CHECK(Has(model, Action::kToggleSharpUpscale));
    CHECK(Has(model, Action::kTogglePerfHud));
    CHECK(Has(model, Action::kBackupSavesToUsb));
    CHECK(Has(model, Action::kRestoreSavesFromUsb));

    CHECK_EQ(std::string(ActionName(Action::kToggleFpsCap)), std::string("toggle-fps-cap"));
    CHECK_EQ(std::string(ActionName(Action::kBackupSavesToUsb)), std::string("backup-saves-to-usb"));
    CHECK_EQ(std::string(ActionName(Action::kRestoreSavesFromUsb)), std::string("restore-saves-from-usb"));

    const std::string footer = model.Footer();
    CHECK(footer.find("Backed up 2 save file(s)") != std::string::npos);
}

TEST(GameplayEnhancementsDeadzoneAutoBackupAndHunterName) {
    LauncherModel model;
    model.layout = ReadyLayout();
    model.has_usb = true;
    model.skip_logos = true;
    model.enhancements = false;
    model.stick_deadzone_percent = 15;
    model.auto_backup_saves = true;
    model.hunter_name = "Gehrman";
    model.Rebuild();

    CHECK(Has(model, Action::kToggleSkipLogos));
    CHECK(Has(model, Action::kToggleEnhancements));
    CHECK(Has(model, Action::kCycleDeadzone));
    CHECK(Has(model, Action::kToggleAutoBackup));

    CHECK_EQ(std::string(ActionName(Action::kToggleSkipLogos)), std::string("toggle-skip-logos"));
    CHECK_EQ(std::string(ActionName(Action::kToggleEnhancements)), std::string("toggle-enhancements"));
    CHECK_EQ(std::string(ActionName(Action::kCycleDeadzone)), std::string("cycle-deadzone"));
    CHECK_EQ(std::string(ActionName(Action::kToggleAutoBackup)), std::string("toggle-auto-backup"));

    bool found_dz = false;
    bool found_enh_off = false;
    for (const Item& item : model.items) {
        if (item.action == Action::kCycleDeadzone) {
            found_dz = item.label.find("15%") != std::string::npos;
        }
        if (item.action == Action::kToggleEnhancements) {
            found_enh_off = item.label.find("off") != std::string::npos;
        }
    }
    CHECK(found_dz);
    CHECK(found_enh_off);

    const std::string footer = model.Footer();
    CHECK(footer.find("Hunter: Gehrman") != std::string::npos);
}
