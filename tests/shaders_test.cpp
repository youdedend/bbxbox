// SPDX-License-Identifier: GPL-3.0-or-later
// The console's own shaders are compiled at build time, so the only thing a host
// test can check is the contract around them - which is exactly the part that
// breaks silently: an entry point added to the .hlsl and to CMake but not to the
// manifest (so nothing verifies it), a profile newer than the console's shader
// model, or two entry points that would collide in the generated header.
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#include "harness.h"
#include "src/host/d3d12/bbx_caps.h"
#include "src/host/d3d12/bbx_shaders.h"

namespace {

using bbx::d3d12::Caps;
using bbx::d3d12::XboxSeriesCaps;
using bbx::shaders::EntryPoint;
using bbx::shaders::Manifest;

// "ps_6_4" -> 4, or -1 when the profile has no minor number.
int ProfileMinor(const char* profile) {
    const char* underscore = std::strrchr(profile, '_');
    return underscore ? std::atoi(underscore + 1) : -1;
}

// "ps_6_4" -> "ps".
std::string ProfileStage(const char* profile) {
    const std::string text = profile;
    const std::size_t underscore = text.find('_');
    return underscore == std::string::npos ? std::string() : text.substr(0, underscore);
}

// PresentVs -> vs, OverlayPs -> ps: the entry point's own suffix states the
// stage it is written for, so a mismatch is checkable without dxc.
std::string EntryStage(const char* entry) {
    const std::string text = entry;
    if (text.size() < 2) { return {}; }
    std::string suffix = text.substr(text.size() - 2);
    for (char& c : suffix) { c = static_cast<char>(std::tolower(c)); }
    return suffix;
}

}  // namespace

TEST(TheShaderManifestNamesEveryEntryPointTheBuildCompiles) {
    const auto manifest = Manifest();
    CHECK_EQ(manifest.count, 6u);
    CHECK(manifest.items != nullptr);
    // Present is a fullscreen triangle (no vertex buffer, no input layout);
    // overlay is the telemetry/HUD pass. Both are needed for the first frame.
    CHECK(std::strcmp(manifest.items[0].symbol, "PresentVs") == 0);
    CHECK(std::strcmp(manifest.items[1].symbol, "PresentPs") == 0);
    CHECK(std::strcmp(manifest.items[2].symbol, "OverlayVs") == 0);
    CHECK(std::strcmp(manifest.items[3].symbol, "OverlayPs") == 0);
    // The compute passes: the detile and the boot self-test.
    CHECK(std::strcmp(manifest.items[4].symbol, "UntileCs") == 0);
    CHECK(std::strcmp(manifest.items[5].symbol, "SelftestCs") == 0);
    for (std::size_t i = 0; i < manifest.count; ++i) {
        const EntryPoint& point = manifest.items[i];
        CHECK(point.symbol != nullptr);
        CHECK(point.entry != nullptr);
        CHECK(point.profile != nullptr);
        CHECK(point.source != nullptr);
        CHECK(std::strlen(point.entry) > 0);
        CHECK(std::strlen(point.profile) > 0);
        CHECK(std::strlen(point.source) > 0);
        // The generated blob is named after the symbol, so it must be a plain
        // identifier fragment: no dots, no dashes.
        CHECK(std::strspn(point.symbol, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ==
              std::strlen(point.symbol));
    }
}

TEST(NoShaderTargetIsNewerThanTheConsoleShaderModel) {
    const Caps caps = XboxSeriesCaps();
    const auto manifest = Manifest();
    for (std::size_t i = 0; i < manifest.count; ++i) {
        const EntryPoint& point = manifest.items[i];
        // SM 6.4 is the console's ceiling; 6.5/6.6 targets fail PSO creation.
        CHECK(ProfileMinor(point.profile) >= 0);
        CHECK_EQ(static_cast<unsigned>(ProfileMinor(point.profile)), caps.shader_model % 10);
        CHECK_EQ(ProfileStage(point.profile), EntryStage(point.entry));
        // A .hlsl under xbox/shaders, not a path: the build globs that directory.
        CHECK(std::strchr(point.source, '/') == nullptr);
        CHECK(std::strchr(point.source, '\\') == nullptr);
        CHECK(std::strstr(point.source, ".hlsl") != nullptr);
    }
}

TEST(ShaderEntryPointsDoNotCollide) {
    const auto manifest = Manifest();
    std::set<std::string> keys;
    for (std::size_t i = 0; i < manifest.count; ++i) {
        // Two entry points with the same name and profile would generate two
        // arrays with the same identifier in bbx_shaders_generated.h.
        keys.insert(std::string(manifest.items[i].entry) + ":" + manifest.items[i].profile);
        keys.insert(std::string("symbol:") + manifest.items[i].symbol);
    }
    CHECK_EQ(keys.size(), manifest.count * 2);
}
