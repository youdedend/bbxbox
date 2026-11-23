// SPDX-License-Identifier: GPL-2.0-or-later
// Version and content-id rules (adapted from Test-project's sfo_version_test).
#include "harness.h"

#include "xbox/platform/sfo_version.h"

#include <string>

using namespace BbPrepare;

TEST(VersionOrdering) {
    CHECK(CompareVersions("01.09", "01.00") > 0);
    CHECK(CompareVersions("01.00", "01.09") < 0);
    CHECK(CompareVersions("01.09", "1.09") == 0);
    CHECK(CompareVersions("02.00", "01.99") > 0);
    CHECK(CompareVersions("", "01.00") < 0);
    CHECK(CompareVersions("garbage", "00.00") == 0);
}

TEST(PatchTargetIsExactlyOneNine) {
    CHECK(IsPatchTargetVersion("01.09"));
    CHECK(!IsPatchTargetVersion("01.090"));
    CHECK(!IsPatchTargetVersion("01.00"));
    CHECK(ReachesPatchTargetVersion("01.09"));
    CHECK(ReachesPatchTargetVersion("01.10"));
    CHECK(!ReachesPatchTargetVersion("01.08"));
    CHECK(!ReachesPatchTargetVersion(""));
}

TEST(ContentIdYieldsTheUpdateVersion) {
    CHECK(UpdateVersionFromContentId(
              "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100") == "01.09");
    CHECK(UpdateVersionFromContentId(
              "EP9000-CUSA00207_00-BLOODBORNE0000000-A0100-V0100") == "01.00");
    // The packaging group is never the app version.
    CHECK(UpdateVersionFromContentId("UP9000-CUSA03173_00-NAME-V0100").empty());
    CHECK(UpdateVersionFromContentId("").empty());
    CHECK(UpdateVersionFromContentId("not a content id").empty());
}

TEST(AppTypeClassification) {
    CHECK(IsPatchAppType("patch"));
    CHECK(IsPatchAppType("PATCH"));
    CHECK(IsPatchAppType("gp"));
    CHECK(IsPatchAppType("GP"));
    CHECK(!IsPatchAppType("gd"));
    CHECK(!IsPatchAppType("gde"));
    CHECK(!IsPatchAppType("gdp"));
    // Anything unknown is a base game, never a patch: an unknown dump must not
    // silently count as an update.
    CHECK(!IsPatchAppType(""));
    CHECK(!IsPatchAppType("something"));
}

TEST(ToLowerHandlesTheMetadataAlphabet) {
    CHECK(ToLower("Patch") == "patch");
    CHECK(ToLower("CUSA03173") == "cusa03173");
    CHECK(ToLower("") == "");
}
