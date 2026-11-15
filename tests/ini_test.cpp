// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "xbox/platform/ini_file.h"

#include <string>

using namespace bbx::platform;

TEST(IniReadsSectionsAndKeys) {
    const IniMap ini = ParseIni(DefaultIni());
    CHECK(IniGet(ini, "video.resolution") == "1920x1080");
    CHECK(IniFlag(ini, "video.dynamic_scale", false));
    CHECK(IniFlag(ini, "shaders.precompile", false));
    CHECK(!IniFlag(ini, "video.vsync", false) ? false : true);
    CHECK(IniGet(ini, "missing.key", "fallback") == "fallback");
    // An empty value means "not set", so the fallback applies.
    CHECK(IniGet(ini, "dump.path", "unset") == "unset");
}

TEST(IniSetCreatesAndUpdatesInPlace) {
    std::string text = "[video]\nresolution=1920x1080\nvsync=1\n\n[debug]\nlog_level=info\n";
    CHECK(IniSet(text, "video", "resolution", "2560x1440"));
    CHECK(text.find("resolution=2560x1440") != std::string::npos);
    // Everything else survives.
    CHECK(text.find("[debug]\nlog_level=info") != std::string::npos);

    // Unchanged value: no write, so the launcher does not touch the file for nothing.
    CHECK(!IniSet(text, "video", "resolution", "2560x1440"));

    // New key lands inside its own section, not at the end of the file.
    CHECK(IniSet(text, "video", "fps_cap", "60"));
    const std::size_t video = text.find("[video]");
    const std::size_t debug = text.find("[debug]");
    const std::size_t fps = text.find("fps_cap=60");
    CHECK(fps > video);
    CHECK(fps < debug);
}

TEST(IniSetAppendsMissingSection) {
    std::string text = "[video]\nvsync=1\n";
    CHECK(IniSet(text, "dump", "path", "E:\\dumps\\Bloodborne"));
    CHECK(text.find("[dump]\npath=E:\\dumps\\Bloodborne") != std::string::npos);
    CHECK(IniSet(text, "dump", "use_patch", "0"));
    CHECK(ParseIni(text).at("dump.use_patch") == "0");
}

TEST(IniIgnoresCommentsAndCase) {
    const IniMap ini = ParseIni("; comment\n# another\n[Video]\nVSync = 1\n");
    CHECK(IniFlag(ini, "video.vsync", false));
}
