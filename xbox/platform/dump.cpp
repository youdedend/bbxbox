// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox/platform/dump.h"

#include <algorithm>
#include <cstring>

namespace bbx::platform {
namespace {

// A real param.sfo starts with the four bytes 00 50 53 46 - "\0PSF" - read
// big-endian, which is 0x00505346. The constant here used to be 0x46535000,
// the same four bytes in the wrong order, so every genuine SFO was rejected
// and the launcher said "sce_sys\\param.sfo could not be read" for a file it
// had just read 1,760 bytes of, magic and all.
constexpr std::uint32_t kSfoMagic = 0x00505346u;  // "\0PSF" as ReadBe32 sees it
constexpr std::uint32_t kMaxSfoBytes = 1u << 20;
constexpr std::uint16_t kFormatUtf8 = 0x0204;

std::uint32_t ReadBe32(const std::vector<std::uint8_t>& b, std::size_t at) {
    if (at + 4 > b.size()) return 0;
    return (static_cast<std::uint32_t>(b[at]) << 24) | (static_cast<std::uint32_t>(b[at + 1]) << 16) |
           (static_cast<std::uint32_t>(b[at + 2]) << 8) | static_cast<std::uint32_t>(b[at + 3]);
}

std::uint16_t ReadBe16(const std::vector<std::uint8_t>& b, std::size_t at) {
    if (at + 2 > b.size()) return 0;
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(b[at]) << 8) | b[at + 1]);
}

std::string Join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char last = a.back();
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

std::string BaseName(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string ParentPath(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string TrimNulls(std::string value) {
    const std::size_t nul = value.find('\0');
    if (nul != std::string::npos) value.resize(nul);
    return value;
}

}  // namespace

std::string SfoInfo::TitleId() const {
    if (!title_id.empty()) return title_id;
    // CONTENT_ID's second group is the title id: UP9000-CUSA03173_00-...
    const std::size_t first = content_id.find('-');
    if (first == std::string::npos) return {};
    const std::size_t second = content_id.find('-', first + 1);
    if (second == std::string::npos || second <= first + 1) return {};
    std::string id = content_id.substr(first + 1, second - first - 1);
    const std::size_t underscore = id.find('_');
    if (underscore != std::string::npos) id.resize(underscore);
    return id;
}

std::optional<SfoInfo> ParseParamSfo(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 20 || bytes.size() > kMaxSfoBytes) return std::nullopt;
    if (ReadBe32(bytes, 0) != kSfoMagic) return std::nullopt;
    const std::uint32_t key_table = ReadBe32(bytes, 8);
    const std::uint32_t data_table = ReadBe32(bytes, 12);
    const std::uint32_t count = ReadBe32(bytes, 16);
    if (key_table >= bytes.size() || data_table >= bytes.size() || count > 512) return std::nullopt;
    if (20ull + static_cast<unsigned long long>(count) * 16 > bytes.size()) return std::nullopt;

    SfoInfo info;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = 20 + static_cast<std::size_t>(i) * 16;
        const std::uint16_t key_offset = ReadBe16(bytes, entry);
        const std::uint16_t format = ReadBe16(bytes, entry + 2);
        const std::uint32_t length = ReadBe32(bytes, entry + 4);
        const std::uint32_t data_offset = ReadBe32(bytes, entry + 12);
        if (format != kFormatUtf8) continue;
        const std::size_t key_at = key_table + key_offset;
        const std::size_t data_at = data_table + data_offset;
        if (key_at >= bytes.size() || data_at >= bytes.size()) continue;
        if (data_at + length > bytes.size() || length > 4096) continue;

        std::string key;
        for (std::size_t at = key_at; at < bytes.size() && bytes[at] != 0; ++at) {
            key.push_back(static_cast<char>(bytes[at]));
        }
        std::string value(reinterpret_cast<const char*>(bytes.data() + data_at), length);
        value = TrimNulls(value);

        if (key == "APP_VER") info.app_ver = value;
        else if (key == "APP_TYPE") info.app_type = value;
        else if (key == "TITLE_ID") info.title_id = value;
        else if (key == "CONTENT_ID") info.content_id = value;
        else if (key == "TITLE") info.title = value;
    }
    if (info.app_ver.empty() && info.content_id.empty()) return std::nullopt;
    return info;
}

bool IsDumpRoot(const DumpFs& fs, const std::string& path) {
    if (!fs.exists || !fs.is_dir) return false;
    if (!fs.is_dir(path)) return false;
    return fs.exists(Join(path, "eboot.bin")) && fs.exists(Join(path, "sce_sys\\param.sfo")) &&
           fs.is_dir(Join(path, "dvdroot_ps4"));
}

std::optional<SfoInfo> ReadDumpInfo(const DumpFs& fs, const std::string& path) {
    if (!fs.read) return std::nullopt;
    const std::vector<std::uint8_t> bytes = fs.read(Join(path, "sce_sys\\param.sfo"), kMaxSfoBytes);
    if (bytes.empty()) return std::nullopt;
    const std::optional<SfoInfo> info = ParseParamSfo(bytes);
    if (!info || !info->Valid()) return std::nullopt;
    return info;
}

std::vector<std::string> PatchCandidates(const DumpFs& fs, const std::string& base,
                                         const std::string& preferred) {
    std::vector<std::string> out;
    if (!preferred.empty()) out.push_back(preferred);
    const std::string parent = ParentPath(base);
    const std::string name = BaseName(base);
    if (!parent.empty() && !name.empty()) {
        for (const char* suffix : {"-patch", "-update", " patch", "_patch", "-p"}) {
            out.push_back(Join(parent, name + suffix));
            // And the inverse: the player picked the update folder, so the base
            // is its sibling with the suffix removed.
            const std::string tail{suffix};
            if (name.size() > tail.size() &&
                name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
                out.push_back(Join(parent, name.substr(0, name.size() - tail.size())));
            }
        }
        if (fs.list_dirs) {
            for (const std::string& child : fs.list_dirs(parent)) {
                if (child == name) continue;
                // A sibling that starts with the base's name is worth looking
                // at; the version/title checks reject it if it is not an update.
                if (child.rfind(name, 0) == 0) out.push_back(Join(parent, child));
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

DumpLayout DetectLayout(const DumpFs& fs, const std::string& picked,
                        const std::string& preferred_patch, bool use_patch,
                        bool unpatched_confirmed) {
    DumpLayout layout;
    layout.use_patch = use_patch;
    layout.unpatched_confirmed = unpatched_confirmed;
    if (picked.empty()) {
        layout.problem = "no dump folder was chosen";
        return layout;
    }

    // The player may have picked the update folder itself: then the sibling
    // base supplies what the update does not carry.
    std::string base_path = picked;
    std::optional<SfoInfo> picked_info = IsDumpRoot(fs, picked) ? ReadDumpInfo(fs, picked)
                                                                 : std::nullopt;
    std::string picked_patch;
    if (picked_info && picked_info->IsPatchContainer()) {
        picked_patch = picked;
        base_path.clear();
        for (const std::string& candidate : PatchCandidates(fs, picked, preferred_patch)) {
            const std::optional<SfoInfo> info = ReadDumpInfo(fs, candidate);
            if (info && info->TitleId() == picked_info->TitleId() &&
                BbPrepare::CompareVersions(info->Version(), picked_info->Version()) < 0 &&
                IsDumpRoot(fs, candidate)) {
                base_path = candidate;
                break;
            }
        }
        if (base_path.empty()) {
            layout.problem = "the chosen folder is an update, and no base dump was found beside it";
            return layout;
        }
    }
    if (!IsDumpRoot(fs, base_path)) {
        layout.problem =
            "that folder is not a Bloodborne dump: it needs eboot.bin, sce_sys\\param.sfo and "
            "dvdroot_ps4";
        return layout;
    }
    const std::optional<SfoInfo> base_info = ReadDumpInfo(fs, base_path);
    if (!base_info) {
        layout.problem = "sce_sys\\param.sfo could not be read";
        return layout;
    }
    layout.base_path = base_path;
    layout.base = *base_info;

    // The update: the picked one, else the saved one, else siblings.
    if (!picked_patch.empty()) {
        layout.patch_path = picked_patch;
    } else if (!preferred_patch.empty()) {
        const std::optional<SfoInfo> info = ReadDumpInfo(fs, preferred_patch);
        if (info && IsDumpRoot(fs, preferred_patch)) layout.patch_path = preferred_patch;
    }
    if (layout.patch_path.empty()) {
        for (const std::string& candidate : PatchCandidates(fs, base_path, {})) {
            if (candidate == base_path) continue;
            const std::optional<SfoInfo> info = ReadDumpInfo(fs, candidate);
            if (!info || !IsDumpRoot(fs, candidate)) continue;
            if (info->TitleId() != layout.base.TitleId()) {
                // Worth saying out loud: a folder that looks like an update but
                // is another title is a common mistake when copying dumps.
                layout.problem = "ignored " + candidate + ": another title (" +
                                 info->TitleId() + " vs " + layout.base.TitleId() + ")";
                continue;
            }
            if (BbPrepare::CompareVersions(info->Version(), layout.base.Version()) <= 0) {
                layout.problem = "ignored " + candidate + ": not newer than " +
                                 layout.base.Version();
                continue;
            }
            layout.patch_path = candidate;
            layout.problem.clear();
            break;
        }
    }

    if (!layout.patch_path.empty()) {
        const std::optional<SfoInfo> info = ReadDumpInfo(fs, layout.patch_path);
        if (!info) {
            layout.problem = "the update folder's param.sfo could not be read";
            layout.patch_path.clear();
        } else if (info->TitleId() != layout.base.TitleId()) {
            layout.problem = "the update folder is for another title (" + info->TitleId() + " vs " +
                             layout.base.TitleId() + ")";
            layout.patch_path.clear();
        } else if (BbPrepare::CompareVersions(info->Version(), layout.base.Version()) <= 0) {
            layout.problem = "the update folder is not newer than the base dump";
            layout.patch_path.clear();
        } else {
            layout.patch = *info;
        }
    }

    if (!layout.patch_path.empty() && use_patch) {
        layout.kind = LayoutKind::kSplit;
        layout.effective_ver = layout.patch.Version();
    } else if (!layout.patch_path.empty()) {
        layout.kind = LayoutKind::kSplit;
        layout.effective_ver = layout.base.Version();
    } else if (BbPrepare::IsPatchTargetVersion(layout.base.Version())) {
        layout.kind = LayoutKind::kMerged;
        layout.effective_ver = layout.base.Version();
    } else {
        layout.kind = LayoutKind::kBaseOnly;
        layout.effective_ver = layout.base.Version();
    }
    return layout;
}

const char* LayoutKindName(LayoutKind kind) {
    switch (kind) {
    case LayoutKind::kNone: return "none";
    case LayoutKind::kMerged: return "merged";
    case LayoutKind::kSplit: return "split";
    case LayoutKind::kBaseOnly: return "base-only";
    }
    return "none";
}

std::string LayoutLogLine(const DumpLayout& layout) {
    const auto text = [](const std::string& value) {
        return value.empty() ? std::string("-") : value;
    };
    return "DumpLayout: kind=" + std::string(LayoutKindName(layout.kind)) +
           "; base_ver=" + text(layout.base.Version()) +
           "; patch_ver=" + text(layout.patch.Version()) +
           "; effective=" + text(layout.EffectiveVersion()) + "; base=" + text(layout.base_path) +
           "; patch=" + text(layout.patch_path) +
           (layout.problem.empty() ? "" : "; problem=" + layout.problem);
}

bool VersionSupported(const std::string& version) {
    return BbPrepare::IsPatchTargetVersion(version);
}

}  // namespace bbx::platform
