// SPDX-License-Identifier: GPL-3.0-or-later
// LaunchPass / RetroPass / Xbox frontend URI protocol parser (`bbxbox:`).
//
// Follows the XboxEmulationHub convention used by XBSX2 and RetroArch:
//   bbxbox:?cmd="E:\Bloodborne"&launchOnExit=launchpass:
//   bbxbox:?cmd=bbxbox.exe "E:\Games\Bloodborne" --fps=60 --res=900p&autoboot=1&launchOnExit=retropass:
//   bbxbox:?forceExit=1
//
// Pure and host-tested (no WinRT) so escaping, quoting, Windows backslash
// preservation, and frontend options are verified by unit tests on Linux.
#pragma once

#include <string>
#include <string_view>

namespace bbx::platform {

struct ProtocolLaunchArgs {
    bool valid = false;
    bool autoboot = false;
    bool force_exit = false;
    std::string dump_path;
    std::string launch_on_exit;
    int fps_cap = 0;           // 0 = unchanged, 30 or 60
    int render_res_mode = -1;  // -1 = unchanged, 0=1080p, 1=900p, 2=720p, 3=540p
};

// Decodes a URI query component (`%XX` hex escapes and `+` -> space).
[[nodiscard]] std::string PercentDecodeQuery(std::string_view input);

// Parses a full `bbxbox:...` protocol activation URI or bare query string.
[[nodiscard]] ProtocolLaunchArgs ParseProtocolUri(std::string_view raw_uri);

}  // namespace bbx::platform
