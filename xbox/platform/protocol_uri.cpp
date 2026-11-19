// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox/platform/protocol_uri.h"

#include <cctype>
#include <vector>

namespace bbx::platform {
namespace {

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::string ToLowerAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

bool EndsWithIcase(std::string_view value, std::string_view suffix) {
    if (value.size() < suffix.size()) return false;
    return ToLowerAscii(value.substr(value.size() - suffix.size())) == ToLowerAscii(suffix);
}

// Splits a `cmd=` value on whitespace while preserving quoted Windows paths
// (including backslashes, which std::quoted strips by default unless escaped).
std::vector<std::string> SplitCommandTokens(std::string_view cmd) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < cmd.size()) {
        while (i < cmd.size() && std::isspace(static_cast<unsigned char>(cmd[i]))) ++i;
        if (i >= cmd.size()) break;
        std::string token;
        if (cmd[i] == '"') {
            ++i;
            while (i < cmd.size() && cmd[i] != '"') {
                token.push_back(cmd[i++]);
            }
            if (i < cmd.size() && cmd[i] == '"') ++i;
        } else {
            while (i < cmd.size() && !std::isspace(static_cast<unsigned char>(cmd[i]))) {
                token.push_back(cmd[i++]);
            }
        }
        if (!token.empty()) tokens.push_back(std::move(token));
    }
    return tokens;
}

void TrimPathTrailingSlashes(std::string& path) {
    for (char& ch : path) {
        if (ch == '/') ch = '\\';
    }
    while (path.size() > 3 && path.back() == '\\') {
        path.pop_back();
    }
}

void ApplyCmdToken(const std::string& token, ProtocolLaunchArgs& out) {
    const std::string lower = ToLowerAscii(token);
    if (lower == "--autoboot" || lower == "-autoboot") {
        out.autoboot = true;
        return;
    }
    if (lower == "--fps=60" || lower == "--fps60") {
        out.fps_cap = 60;
        return;
    }
    if (lower == "--fps=30" || lower == "--fps30") {
        out.fps_cap = 30;
        return;
    }
    if (lower == "--res=1080p" || lower == "--1080p") {
        out.render_res_mode = 0;
        return;
    }
    if (lower == "--res=900p" || lower == "--900p") {
        out.render_res_mode = 1;
        return;
    }
    if (lower == "--res=720p" || lower == "--720p") {
        out.render_res_mode = 2;
        return;
    }
    if (lower == "--res=540p" || lower == "--540p") {
        out.render_res_mode = 3;
        return;
    }
    if (!token.empty() && token[0] == '-') {
        return;
    }
    // Strip leading executable token (e.g. `bbxbox.exe` or `bbxbox`) injected by LaunchPass.
    if (out.dump_path.empty() && (lower == "bbxbox" || EndsWithIcase(lower, "bbxbox.exe"))) {
        return;
    }
    if (out.dump_path.empty()) {
        std::string path = token;
        TrimPathTrailingSlashes(path);
        // If a frontend pointed directly at eboot.bin or param.sfo, step up to the dump root.
        const std::string norm_lower = ToLowerAscii(path);
        if (EndsWithIcase(norm_lower, "\\eboot.bin")) {
            path.resize(path.size() - 10);
            TrimPathTrailingSlashes(path);
        } else if (EndsWithIcase(norm_lower, "\\sce_sys\\param.sfo")) {
            path.resize(path.size() - 18);
            TrimPathTrailingSlashes(path);
        }
        out.dump_path = std::move(path);
        out.autoboot = true;
    }
}

bool ParseTruthy(std::string_view v) {
    const std::string lower = ToLowerAscii(v);
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

}  // namespace

std::string PercentDecodeQuery(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            const int hi = HexValue(input[i + 1]);
            const int lo = HexValue(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (input[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

ProtocolLaunchArgs ParseProtocolUri(std::string_view raw_uri) {
    ProtocolLaunchArgs out;
    if (raw_uri.empty()) return out;

    const std::size_t qpos = raw_uri.find('?');
    std::string_view query;
    if (qpos != std::string_view::npos) {
        query = raw_uri.substr(qpos + 1);
    } else if (raw_uri.find('=') != std::string_view::npos) {
        query = raw_uri;
    } else {
        return out;
    }

    const std::size_t hash = query.find('#');
    if (hash != std::string_view::npos) query = query.substr(0, hash);

    std::size_t pos = 0;
    while (pos <= query.size()) {
        const std::size_t amp = query.find('&', pos);
        const std::string_view pair =
            query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        if (!pair.empty()) {
            const std::size_t eq = pair.find('=');
            const std::string key = ToLowerAscii(PercentDecodeQuery(
                eq == std::string_view::npos ? pair : pair.substr(0, eq)));
            const std::string val =
                eq == std::string_view::npos ? std::string{} : PercentDecodeQuery(pair.substr(eq + 1));

            if (key == "forceexit") {
                out.force_exit = val.empty() || ParseTruthy(val);
                out.valid = true;
            } else if (key == "launchonexit") {
                if (!val.empty()) {
                    out.launch_on_exit = val;
                    out.valid = true;
                }
            } else if (key == "autoboot") {
                out.autoboot = val.empty() || ParseTruthy(val);
                out.valid = true;
            } else if (key == "path" || key == "dump") {
                if (!val.empty()) {
                    ApplyCmdToken(val, out);
                    out.valid = true;
                }
            } else if (key == "fps") {
                if (val == "30") {
                    out.fps_cap = 30;
                    out.valid = true;
                } else if (val == "60") {
                    out.fps_cap = 60;
                    out.valid = true;
                }
            } else if (key == "res") {
                const std::string r = ToLowerAscii(val);
                if (r == "1080p" || r == "0") {
                    out.render_res_mode = 0;
                    out.valid = true;
                } else if (r == "900p" || r == "1") {
                    out.render_res_mode = 1;
                    out.valid = true;
                } else if (r == "720p" || r == "2") {
                    out.render_res_mode = 2;
                    out.valid = true;
                } else if (r == "540p" || r == "3") {
                    out.render_res_mode = 3;
                    out.valid = true;
                }
            } else if (key == "cmd") {
                const std::vector<std::string> tokens = SplitCommandTokens(val);
                for (const std::string& tok : tokens) {
                    ApplyCmdToken(tok, out);
                }
                if (!tokens.empty()) out.valid = true;
            }
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }

    return out;
}

}  // namespace bbx::platform
