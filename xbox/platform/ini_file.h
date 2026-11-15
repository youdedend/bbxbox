// SPDX-License-Identifier: GPL-3.0-or-later
// `bbxbox.ini`: the player's choices, in a file a log reader can understand.
//
// Pure (no Win32): the launcher reads and rewrites it, the tests pin that
// comments, order and unrelated keys survive a write, that a missing key lands
// inside its section, and that an unchanged value does not touch the file.
#pragma once

#include <map>
#include <string>
#include <string_view>

namespace bbx::platform {

// Flattened "section.key" -> value.
using IniMap = std::map<std::string, std::string>;

[[nodiscard]] IniMap ParseIni(std::string_view text);

// Returns `fallback` when the key is absent or empty.
[[nodiscard]] std::string IniGet(const IniMap& ini, std::string_view key,
                                 std::string_view fallback = {});
[[nodiscard]] bool IniFlag(const IniMap& ini, std::string_view key, bool fallback);

// Sets "section.key" in `text`, preserving everything else. Returns true when
// the text changed. A missing section is appended; a missing key is appended to
// its section's last line.
[[nodiscard]] bool IniSet(std::string& text, std::string_view section, std::string_view key,
                          std::string_view value);

// The default file, written on first launch. Every value is a working setting:
// a player should never have to edit this file to play.
[[nodiscard]] std::string DefaultIni();

}  // namespace bbx::platform
