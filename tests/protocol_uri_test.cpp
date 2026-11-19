// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "xbox/platform/protocol_uri.h"

using namespace bbx::platform;

TEST(ProtocolUriParsesLaunchPassCmdAndReturnUri) {
    const ProtocolLaunchArgs args = ParseProtocolUri(
        "bbxbox:?cmd=bbxbox.exe%20%22E%3A%5CGames%5CBloodborne%201.09%5Ceboot.bin%22%20--fps=60%20--res=900p"
        "&launchOnExit=launchpass:");
    CHECK(args.valid);
    CHECK(args.autoboot);
    CHECK(!args.force_exit);
    CHECK_EQ(args.dump_path, std::string("E:\\Games\\Bloodborne 1.09"));
    CHECK_EQ(args.launch_on_exit, std::string("launchpass:"));
    CHECK_EQ(args.fps_cap, 60);
    CHECK_EQ(args.render_res_mode, 1);
}

TEST(ProtocolUriHandlesDirectPathAndParamSfo) {
    const ProtocolLaunchArgs args = ParseProtocolUri(
        "bbxbox:?path=F%3A%5CCUSA00900%5Csce_sys%5Cparam.sfo&fps=30&res=720p");
    CHECK(args.valid);
    CHECK(args.autoboot);
    CHECK_EQ(args.dump_path, std::string("F:\\CUSA00900"));
    CHECK_EQ(args.fps_cap, 30);
    CHECK_EQ(args.render_res_mode, 2);
}

TEST(ProtocolUriHandlesForceExitAndEmptyInput) {
    const ProtocolLaunchArgs exit_args = ParseProtocolUri("bbxbox:?forceExit=1");
    CHECK(exit_args.valid);
    CHECK(exit_args.force_exit);

    const ProtocolLaunchArgs bare = ParseProtocolUri("bbxbox:");
    CHECK(!bare.valid);
    CHECK(!bare.autoboot);
    CHECK(!bare.force_exit);
}
