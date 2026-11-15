// SPDX-License-Identifier: GPL-2.0-or-later
// The update-overlay path rules (adapted from Test-project's overlay_index tests).
// The header is C with file-local helpers, so silence "unused function" for the
// entry points this suite does not call.
#include "harness.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "xbox/platform/overlay_index.h"
#pragma GCC diagnostic pop

#include <string>
#include <vector>

namespace {

std::string Maps(const BbOverlayIndex& index, const std::string& path) {
    char out[BB_OVERLAY_MAX_ROOT + BB_OVERLAY_MAX_REL];
    if (!bb_overlay_maps_path(&index, path.c_str(), out, sizeof(out))) return {};
    return out;
}

std::string MapsDir(const BbOverlayIndex& index, const std::string& path) {
    char out[BB_OVERLAY_MAX_ROOT + BB_OVERLAY_MAX_REL];
    if (!bb_overlay_maps_dir(&index, path.c_str(), out, sizeof(out))) return {};
    return out;
}

}  // namespace

TEST(OverlayMapsOnlyTheFilesTheUpdateCarries) {
    const std::string list =
        "dvdroot_ps4\\eboot.bin\n"
        "dvdroot_ps4\\param.sfo\n"
        "dvdroot_ps4\\shader\\shaderbnd.dcx\n";
    BbOverlayIndex index{};
    const int rc = bb_overlay_parse(&index, "E:\\dumps\\BB", "F:\\update", list.data(), list.size());
    CHECK_EQ(rc, 0);
    CHECK_EQ(static_cast<int>(index.count), 3);

    CHECK(Maps(index, "E:\\dumps\\BB\\dvdroot_ps4\\eboot.bin") ==
          "F:\\update\\dvdroot_ps4\\eboot.bin");
    // The console's file system is case-insensitive and guest code mixes separators.
    CHECK(Maps(index, "E:/dumps/bb/DVDROOT_PS4/EBOOT.BIN") ==
          "F:\\update\\dvdroot_ps4\\eboot.bin");
    // Not carried by the update: the base dump is served.
    CHECK(Maps(index, "E:\\dumps\\BB\\dvdroot_ps4\\map\\m0100.mapbnd.dcx").empty());
    // Prefixes are not matches.
    CHECK(Maps(index, "E:\\dumps\\BB\\dvdroot_ps4\\eboot.bin.bak").empty());
    bb_overlay_index_free(&index);
}

TEST(OverlayMapsDirectoriesWithoutAnEntryList) {
    const std::string list = "dvdroot_ps4\\shader\\shaderbnd.dcx\n";
    BbOverlayIndex index{};
    CHECK_EQ(bb_overlay_parse(&index, "E:\\dumps\\BB", "F:\\update\\", list.data(), list.size()), 0);
    // Trailing separators are normalized away on both roots.
    CHECK(index.patch == std::string("F:\\update"));
    CHECK(MapsDir(index, "E:\\dumps\\BB\\dvdroot_ps4\\shader") ==
          "F:\\update\\dvdroot_ps4\\shader");
    // A path outside the dump root is never redirected.
    CHECK(MapsDir(index, "F:\\update\\dvdroot_ps4\\shader").empty());
    CHECK(MapsDir(index, "E:\\dumps\\BB").empty());
    bb_overlay_index_free(&index);
}

TEST(OverlayRefusesNonsenseAndStaysEmpty) {
    BbOverlayIndex index{};
    CHECK_EQ(bb_overlay_parse(&index, "", "F:\\u", "x\n", 2), -1);
    CHECK_EQ(index.count, 0u);
    CHECK_EQ(bb_overlay_parse(&index, "E:\\d", "", "x\n", 2), -1);
    CHECK_EQ(bb_overlay_parse(&index, "E:\\d", "F:\\u", "", 0), -1);
    // Only blank lines: no entries, the caller runs without an overlay.
    const std::string blanks = "\r\n\n\r\n";
    CHECK_EQ(bb_overlay_parse(&index, "E:\\d", "F:\\u", blanks.data(), blanks.size()), -1);
    CHECK_EQ(index.count, 0u);
}

TEST(OverlayListIsSortedForBinarySearch) {
    const std::string list =
        "dvdroot_ps4\\z.bin\n"
        "dvdroot_ps4\\A.bin\n"
        "dvdroot_ps4\\m\\b.bin\n";
    BbOverlayIndex index{};
    CHECK_EQ(bb_overlay_parse(&index, "E:\\d", "F:\\u", list.data(), list.size()), 0);
    for (size_t i = 1; i < index.count; ++i) {
        const char* prev = index.files + (i - 1) * BB_OVERLAY_MAX_REL;
        const char* here = index.files + i * BB_OVERLAY_MAX_REL;
        CHECK(std::string(here) > std::string(prev));
    }
    CHECK(Maps(index, "E:\\d\\dvdroot_ps4\\A.bin") == "F:\\u\\dvdroot_ps4\\a.bin");
    bb_overlay_index_free(&index);
}
