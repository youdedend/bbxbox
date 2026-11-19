// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox/platform/ini_file.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace bbx::platform {
namespace {

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

std::string Lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string> Lines(std::string_view text) {
    std::vector<std::string> lines;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        lines.emplace_back(text.substr(0, nl));
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
    return lines;
}

std::string Join(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

// True when `line` opens `section`.
bool IsSection(std::string_view line, std::string_view section) {
    line = Trim(line);
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') return false;
    return Lower(line.substr(1, line.size() - 2)) == Lower(section);
}

bool IsAnySection(std::string_view line) {
    line = Trim(line);
    return line.size() >= 2 && line.front() == '[' && line.back() == ']';
}

}  // namespace

IniMap ParseIni(std::string_view text) {
    IniMap map;
    std::string section;
    for (const std::string& raw : Lines(text)) {
        const std::string_view line = Trim(raw);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (IsAnySection(line)) {
            section = Lower(line.substr(1, line.size() - 2));
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string key =
            section.empty() ? Lower(Trim(line.substr(0, eq)))
                            : section + "." + Lower(Trim(line.substr(0, eq)));
        map[key] = std::string(Trim(line.substr(eq + 1)));
    }
    return map;
}

std::string IniGet(const IniMap& ini, std::string_view key, std::string_view fallback) {
    const auto it = ini.find(std::string(key));
    if (it == ini.end() || it->second.empty()) return std::string(fallback);
    return it->second;
}

bool IniFlag(const IniMap& ini, std::string_view key, bool fallback) {
    const std::string value = Lower(IniGet(ini, key));
    if (value.empty()) return fallback;
    if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "off") return false;
    return fallback;
}

bool IniSet(std::string& text, std::string_view section, std::string_view key,
            std::string_view value) {
    const std::string wanted = std::string(key);
    std::vector<std::string> lines = Lines(text);
    if (!lines.empty() && lines.back().empty()) lines.pop_back();

    // Locate the section and the key inside it.
    std::size_t section_at = std::string::npos;
    std::size_t section_end = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (IsSection(lines[i], section)) {
            section_at = i;
            for (std::size_t j = i + 1; j < lines.size(); ++j) {
                if (IsAnySection(lines[j])) {
                    section_end = j;
                    break;
                }
            }
            break;
        }
    }
    if (section_at != std::string::npos) {
        for (std::size_t i = section_at + 1; i < section_end; ++i) {
            const std::string_view line = Trim(lines[i]);
            const std::size_t eq = line.find('=');
            if (eq == std::string_view::npos) continue;
            if (Lower(Trim(line.substr(0, eq))) != Lower(wanted)) continue;
            const std::string replacement =
                std::string(Trim(line.substr(0, eq))) + "=" + std::string(value);
            if (lines[i] == replacement) return false;  // unchanged: no write
            lines[i] = replacement;
            text = Join(lines);
            return true;
        }
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(section_end),
                     wanted + "=" + std::string(value));
        text = Join(lines);
        return true;
    }

    if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
    lines.push_back("[" + std::string(section) + "]");
    lines.push_back(wanted + "=" + std::string(value));
    text = Join(lines);
    return true;
}

std::string DefaultIni() {
    return
        "; bbxbox settings. Everything here has a working default; edit only what you need.\n"
        "[dump]\n"
        "path=\n"
        "; A separate 1.09 update folder, if your dump is split. Leave empty to auto-detect.\n"
        "patch_path=\n"
        "use_patch=1\n"
        "\n"
        "[video]\n"
        "resolution=1920x1080\n"
        "dynamic_scale=1\n"
        "vsync=1\n"
        "allow_tearing=1\n"
        "fps_cap=60\n"
        "sharp_upscale=1\n"
        "perf_hud=0\n"
        "\n"
        "[gameplay]\n"
        "; Built-in quality-of-life patches applied at guest boot.\n"
        "skip_logos=1\n"
        "enhancements=1\n"
        "; Automatically mirror LocalState saves to USB when exiting the game.\n"
        "auto_backup_saves=1\n"
        "\n"
        "[graphics]\n"
        "; Live post-processing & camera controls (also editable in L3+R3 Quick Menu).\n"
        "chromatic_aberration=1\n"
        "motion_blur=1\n"
        "depth_of_field=1\n"
        "vignette=1\n"
        "fov_percent=100\n"
        "camera_distance_percent=100\n"
        "\n"
        "[controls]\n"
        "; Inner radial deadzone percentage (0..20) for both analog sticks.\n"
        "stick_deadzone_percent=10\n"
        "\n"
        "[shaders]\n"
        "; Run the ahead-of-gameplay precompiler on the title screen.\n"
        "precompile=1\n"
        "\n"
        "[debug]\n"
        "log_level=info\n"
        "d3d12_markers=0\n"
        "d3d12_validation=0\n";
}

}  // namespace bbx::platform
