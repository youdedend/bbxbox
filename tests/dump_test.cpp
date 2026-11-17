// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "xbox/platform/dump.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

using namespace bbx::platform;

namespace {

void PutBe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void PutBe16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

// A minimal param.sfo with the fields the port reads.
std::vector<std::uint8_t> MakeSfo(const std::string& app_ver, const std::string& app_type,
                                  const std::string& title_id, const std::string& content_id) {
    struct Field {
        std::string key;
        std::string value;
    };
    std::vector<Field> fields = {{"APP_VER", app_ver},
                                 {"APP_TYPE", app_type},
                                 {"TITLE_ID", title_id},
                                 {"CONTENT_ID", content_id},
                                 {"TITLE", "Bloodborne"}};
    std::vector<std::uint8_t> keys;
    std::vector<std::uint8_t> data;
    std::vector<std::pair<std::uint16_t, std::uint32_t>> index;
    for (const Field& f : fields) {
        const std::uint16_t key_at = static_cast<std::uint16_t>(keys.size());
        keys.insert(keys.end(), f.key.begin(), f.key.end());
        keys.push_back(0);
        const std::uint32_t data_at = static_cast<std::uint32_t>(data.size());
        data.insert(data.end(), f.value.begin(), f.value.end());
        data.push_back(0);
        index.emplace_back(key_at, data_at);
    }
    std::vector<std::uint8_t> out;
    // The four bytes a real param.sfo starts with: 00 50 53 46 ("\0PSF").
    PutBe32(out, 0x00505346u);
    PutBe32(out, 0x101);
    const std::uint32_t key_table =
        20 + static_cast<std::uint32_t>(fields.size()) * 16;
    const std::uint32_t data_table = key_table + static_cast<std::uint32_t>(keys.size());
    PutBe32(out, key_table);
    PutBe32(out, data_table);
    PutBe32(out, static_cast<std::uint32_t>(fields.size()));
    for (std::size_t i = 0; i < fields.size(); ++i) {
        PutBe16(out, index[i].first);
        PutBe16(out, 0x0204);
        PutBe32(out, static_cast<std::uint32_t>(fields[i].value.size() + 1));
        PutBe32(out, static_cast<std::uint32_t>(fields[i].value.size() + 1));
        PutBe32(out, index[i].second);
    }
    out.insert(out.end(), keys.begin(), keys.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

struct FakeDump {
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::map<std::string, std::vector<std::string>> dirs;

    void AddDump(const std::string& root, const std::string& ver, const std::string& type) {
        files[root + "\\eboot.bin"] = {0x7f, 'E', 'L', 'F'};
        files[root + "\\sce_sys\\param.sfo"] =
            MakeSfo(ver, type, "CUSA03173", "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100");
        dirs[root] = {"sce_sys", "sce_module", "dvdroot_ps4"};
        dirs[root + "\\dvdroot_ps4"] = {};
        dirs[root + "\\sce_sys"] = {};
        dirs[root + "\\sce_module"] = {};
        dirs[ParentOf(root)].push_back(BaseOf(root));
    }

    static std::string ParentOf(const std::string& path) {
        const std::size_t slash = path.find_last_of('\\');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }
    static std::string BaseOf(const std::string& path) {
        const std::size_t slash = path.find_last_of('\\');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    DumpFs Fs() {
        DumpFs fs;
        fs.exists = [this](const std::string& p) {
            return files.count(p) != 0 || dirs.count(p) != 0;
        };
        fs.is_dir = [this](const std::string& p) { return dirs.count(p) != 0; };
        fs.read = [this](const std::string& p, std::size_t cap) {
            const auto it = files.find(p);
            if (it == files.end()) return std::vector<std::uint8_t>{};
            std::vector<std::uint8_t> out = it->second;
            if (out.size() > cap) out.resize(cap);
            return out;
        };
        fs.list_dirs = [this](const std::string& p) {
            const auto it = dirs.find(p);
            return it == dirs.end() ? std::vector<std::string>{} : it->second;
        };
        return fs;
    }
};

}  // namespace

TEST(SfoParsesItsFields) {
    const std::vector<std::uint8_t> bytes =
        MakeSfo("01.09", "gp", "CUSA03173", "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100");
    const std::optional<SfoInfo> info = ParseParamSfo(bytes);
    CHECK(info.has_value());
    CHECK(info->app_ver == "01.09");
    CHECK(info->app_type == "gp");
    CHECK(info->TitleId() == "CUSA03173");
    CHECK(info->Version() == "01.09");
    CHECK(!info->IsPatchContainer());
    CHECK(info->Valid());
    CHECK(info->title == "Bloodborne");
}

TEST(SfoAcceptsABaseDumpWhoseContentIdHasNoVersionGroup) {
    // The disc dump's CONTENT_ID is UP9000-CUSA03173_00-BLOODBORNE0000000: there
    // is no -A0100-V0100 group, so a version parsed only from CONTENT_ID is
    // empty and the whole dump used to be refused.
    const auto bytes = MakeSfo("", "gp", "CUSA03173", "UP9000-CUSA03173_00-BLOODBORNE0000000");
    const auto info = ParseParamSfo(bytes);
    CHECK(info.has_value());
    if (info) {
        CHECK(info->TitleId() == "CUSA03173");
        CHECK(info->Version() == "01.00");
        CHECK(info->Valid());
        CHECK(!info->IsPatchContainer());
    }
}

TEST(PatchSfoStillNeedsItsOwnVersion) {
    const auto bytes = MakeSfo("", "patch", "CUSA03173", "UP0001-CUSA03173_00-BLOODBORNE0000000");
    const auto info = ParseParamSfo(bytes);
    CHECK(info.has_value());
    if (info) CHECK(!info->Valid());  // a patch with no version group is not usable
}

TEST(SfoRejectsGarbageRatherThanGuessing) {
    CHECK(!ParseParamSfo({}).has_value());
    CHECK(!ParseParamSfo({0, 0, 0, 0, 1, 2, 3, 4}).has_value());
    std::vector<std::uint8_t> truncated = MakeSfo("01.09", "gp", "CUSA03173", "");
    truncated.resize(24);
    CHECK(!ParseParamSfo(truncated).has_value());
    // Valid header, no APP_VER and no CONTENT_ID: no version, no answer.
    std::vector<std::uint8_t> no_version = MakeSfo("", "gp", "CUSA03173", "");
    CHECK(!ParseParamSfo(no_version).has_value());
}

TEST(MergedLayoutRunsDirectly) {
    FakeDump dump;
    dump.AddDump("D:\\dumps\\Bloodborne", "01.09", "gp");
    const DumpLayout layout =
        DetectLayout(dump.Fs(), "D:\\dumps\\Bloodborne", "", true, false);
    CHECK(layout.kind == LayoutKind::kMerged);
    CHECK(layout.effective_ver == "01.09");
    CHECK(layout.Patched());
    CHECK(VersionSupported(layout.EffectiveVersion()));
    CHECK(layout.problem.empty());
    CHECK(LayoutLogLine(layout).find("kind=merged") != std::string::npos);
}

TEST(SplitLayoutFindsTheUpdate) {
    FakeDump dump;
    dump.AddDump("E:\\dumps\\Bloodborne", "01.00", "gp");
    dump.AddDump("E:\\dumps\\Bloodborne-patch", "01.09", "patch");
    const DumpLayout layout =
        DetectLayout(dump.Fs(), "E:\\dumps\\Bloodborne", "", true, false);
    CHECK(layout.kind == LayoutKind::kSplit);
    CHECK(layout.patch_path == "E:\\dumps\\Bloodborne-patch");
    CHECK(layout.effective_ver == "01.09");
    CHECK(layout.CanRunPatched());
    CHECK(VersionSupported(layout.EffectiveVersion()));
}

TEST(SplitCanBeTurnedOff) {
    FakeDump dump;
    dump.AddDump("E:\\dumps\\Bloodborne", "01.00", "gp");
    dump.AddDump("E:\\dumps\\Bloodborne-patch", "01.09", "patch");
    const DumpLayout layout =
        DetectLayout(dump.Fs(), "E:\\dumps\\Bloodborne", "", false, false);
    CHECK(layout.kind == LayoutKind::kSplit);
    CHECK(!layout.CanRunPatched());
    CHECK(layout.effective_ver == "01.00");
    CHECK(!VersionSupported(layout.EffectiveVersion()));
}

TEST(PickingTheUpdateFolderFindsTheBase) {
    FakeDump dump;
    dump.AddDump("E:\\dumps\\Bloodborne", "01.00", "gp");
    dump.AddDump("E:\\dumps\\Bloodborne-patch", "01.09", "patch");
    const DumpLayout layout =
        DetectLayout(dump.Fs(), "E:\\dumps\\Bloodborne-patch", "", true, false);
    CHECK(layout.kind == LayoutKind::kSplit);
    CHECK(layout.base_path == "E:\\dumps\\Bloodborne");
    CHECK(layout.patch_path == "E:\\dumps\\Bloodborne-patch");
}

TEST(BaseOnlyIsFlagged) {
    FakeDump dump;
    dump.AddDump("E:\\dumps\\Bloodborne", "01.00", "gp");
    const DumpLayout layout =
        DetectLayout(dump.Fs(), "E:\\dumps\\Bloodborne", "", true, false);
    CHECK(layout.kind == LayoutKind::kBaseOnly);
    CHECK(!VersionSupported(layout.EffectiveVersion()));
    CHECK(layout.problem.empty());
}

TEST(CandidatesAreRejectedWithAReason) {
    FakeDump dump;
    dump.AddDump("E:\\dumps\\Bloodborne", "01.00", "gp");
    // A "patch" that is the same version is not an update.
    dump.AddDump("E:\\dumps\\Bloodborne-patch", "01.00", "patch");
    DumpLayout layout = DetectLayout(dump.Fs(), "E:\\dumps\\Bloodborne", "", true, false);
    CHECK(layout.kind == LayoutKind::kBaseOnly);
    CHECK(layout.patch_path.empty());
    CHECK(layout.problem.find("not newer") != std::string::npos);

    // A patch for another title is rejected too.
    FakeDump other;
    other.AddDump("E:\\dumps\\Bloodborne", "01.00", "gp");
    other.files["E:\\dumps\\Bloodborne-patch\\eboot.bin"] = {0x7f};
    other.files["E:\\dumps\\Bloodborne-patch\\sce_sys\\param.sfo"] =
        MakeSfo("01.09", "patch", "CUSA00900", "");
    other.dirs["E:\\dumps\\Bloodborne-patch"] = {"sce_sys", "dvdroot_ps4"};
    other.dirs["E:\\dumps\\Bloodborne-patch\\dvdroot_ps4"] = {};
    other.dirs["E:\\dumps\\Bloodborne-patch\\sce_sys"] = {};
    layout = DetectLayout(other.Fs(), "E:\\dumps\\Bloodborne", "", true, false);
    CHECK(layout.kind == LayoutKind::kBaseOnly);
    CHECK(layout.problem.find("another title") != std::string::npos);
}

TEST(NotADumpIsRefused) {
    FakeDump dump;
    dump.dirs["E:\\random"] = {};
    const DumpLayout layout = DetectLayout(dump.Fs(), "E:\\random", "", true, false);
    CHECK(layout.kind == LayoutKind::kNone);
    CHECK(!layout.problem.empty());
    CHECK(!layout.layout_ok());
}

TEST(PreferredPatchIsHonoured) {
    FakeDump dump;
    dump.AddDump("E:\\dumps\\Bloodborne", "01.00", "gp");
    dump.AddDump("F:\\updates\\BB109", "01.09", "patch");
    const DumpLayout layout =
        DetectLayout(dump.Fs(), "E:\\dumps\\Bloodborne", "F:\\updates\\BB109", true, false);
    CHECK(layout.kind == LayoutKind::kSplit);
    CHECK(layout.patch_path == "F:\\updates\\BB109");
}
