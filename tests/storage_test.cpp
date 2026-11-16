// SPDX-License-Identifier: GPL-3.0-or-later
// tests/storage_test.cpp -- StorageInit recording, call_once retention, and
// post-LogOpen replay ordering.
//
// wWinMain runs StorageInit before LogOpen, opens loader.log on the resolved
// LocalState path, immediately replays the recorded probe lines, and then
// LauncherView::Initialize calls StorageInit a second time through the same
// once_flag. These cases hold both halves of that contract.
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "harness.h"
#include "xbox/platform/storage.h"

using namespace bbx::platform;

namespace {

constexpr std::int32_t kNotImpl = static_cast<std::int32_t>(0x80004001u);
constexpr std::int32_t kAccessDenied = static_cast<std::int32_t>(0x80070005u);
constexpr std::int32_t kPathNotFound = static_cast<std::int32_t>(0x80070003u);
constexpr std::int32_t kCoNotInit = static_cast<std::int32_t>(0x800401f0u);

struct FakeProber {
    std::vector<std::string>* timeline = nullptr;
    StorageProbeOutcome app{"", 0, true};
    StorageProbeOutcome local{"T:\\Users\\UserMgr0\\AppData\\Local\\Packages\\bbxbox\\LocalState",
                              0, true};
    StorageProbeOutcome temp{"T:\\Users\\UserMgr0\\AppData\\Local\\Packages\\bbxbox\\TempState", 0,
                             true};
    StorageProbeOutcome roaming{"", kNotImpl, false};
    StorageProbeOutcome pkg{"G:\\AppFiles\\bbxbox", 0, true};
    StorageProbeOutcome derived{"", kPathNotFound, false};
    int prepared = 0;

    void Note(const char* step) {
        if (timeline != nullptr) timeline->emplace_back(step);
    }

    StorageProbeOutcome AppData() {
        Note("probe:ApplicationData::Current");
        return app;
    }
    StorageProbeOutcome Local() {
        Note("probe:local");
        return local;
    }
    StorageProbeOutcome Temporary() {
        Note("probe:temporary");
        return temp;
    }
    StorageProbeOutcome Roaming() {
        Note("probe:roaming");
        return roaming;
    }
    StorageProbeOutcome Package() {
        Note("probe:package");
        return pkg;
    }
    StorageProbeOutcome DeriveLocal() {
        Note("probe:derive_local");
        return derived;
    }
    void PrepareDirs(const StoragePaths&) {
        ++prepared;
        Note("prepare_dirs");
    }
};

const char* LevelTag(Level level) {
    switch (level) {
    case Level::kDebug: return "D";
    case Level::kInfo: return "I";
    case Level::kWarn: return "W";
    case Level::kError: return "E";
    }
    return "?";
}

}  // namespace

TEST(StorageInit_RecordsBeforeLogOpenAndReplaysInOrderWithFlushes) {
    std::vector<std::string> timeline;
    std::once_flag once;
    StorageInitResult state;
    int factory_calls = 0;

    // 1. wWinMain calls StorageInit before LogOpen: probes record into `state`,
    //    nothing is emitted yet, and Local + Temporary succeed despite Roaming.
    const StorageInitResult* early = RunStorageInitOnce(once, state, [&]() {
        ++factory_calls;
        FakeProber prober;
        prober.timeline = &timeline;
        return prober;
    });
    CHECK(early == &state);
    CHECK(early->ok);
    CHECK(static_cast<bool>(*early));
    CHECK(std::string(early->failed_folder).empty());
    CHECK(early->roaming_missing);
    CHECK_EQ(early->roaming_hr, kNotImpl);
    CHECK_EQ(early->count, static_cast<std::size_t>(5));
    CHECK_EQ(std::string(early->entries[0].name), std::string("ApplicationData::Current"));
    CHECK(early->entries[0].ok);
    CHECK_EQ(early->entries[0].hr, 0);
    CHECK_EQ(std::string(early->entries[1].name), std::string("local"));
    CHECK(early->entries[1].ok);
    CHECK_EQ(std::string(early->entries[2].name), std::string("temporary"));
    CHECK(early->entries[2].ok);
    CHECK_EQ(std::string(early->entries[3].name), std::string("roaming"));
    CHECK(!early->entries[3].ok);
    CHECK_EQ(early->entries[3].hr, kNotImpl);
    CHECK_EQ(std::string(early->entries[4].name), std::string("package"));
    CHECK(early->entries[4].ok);

    // Zero emit or flush events happened before LogOpen.
    for (const std::string& event : timeline) {
        CHECK(event.rfind("emit:", 0) != 0);
        CHECK(event != "flush");
    }

    // 2. LogOpen succeeds, and wWinMain immediately calls ReplayStorageResults.
    const std::size_t open_index = timeline.size();
    timeline.emplace_back("log_open");
    std::vector<std::pair<Level, std::string>> emitted;
    ReplayStorageInit(
        *early,
        [&](Level level, const std::string& line) {
            emitted.emplace_back(level, line);
            timeline.push_back(std::string("emit:") + LevelTag(level) + ":" + line);
        },
        [&]() { timeline.emplace_back("flush"); });
    timeline.emplace_back("wWinMain:summary");

    // Five recorded lines at Info + one warning for missing Roaming = 6 lines,
    // each immediately followed by a flush, all strictly after log_open and
    // before wWinMain's own summary line.
    CHECK_EQ(emitted.size(), static_cast<std::size_t>(6));
    int warnings = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(emitted[i].first == Level::kInfo);
    }
    CHECK(emitted[0].second.find("ApplicationData::Current ok (0x00000000)") != std::string::npos);
    CHECK(emitted[1].second.find("local ") != std::string::npos);
    CHECK(emitted[1].second.find("(0x00000000)") != std::string::npos);
    CHECK(emitted[2].second.find("temporary ") != std::string::npos);
    CHECK(emitted[3].second.find("roaming unavailable (0x80004001)") != std::string::npos);
    CHECK(emitted[4].second.find("package ") != std::string::npos);
    for (const auto& [level, line] : emitted) {
        if (level == Level::kWarn) {
            ++warnings;
            CHECK(line.find("roaming") != std::string::npos);
            CHECK(line.find("0x80004001") != std::string::npos);
        }
    }
    CHECK_EQ(warnings, 1);

    // Every emit in the timeline is after log_open, before wWinMain:summary,
    // and paired with an immediate flush.
    const std::size_t summary_index = timeline.size() - 1;
    CHECK_EQ(summary_index - open_index - 1, static_cast<std::size_t>(12));
    for (std::size_t k = 0; k < 6; ++k) {
        const std::size_t emit_at = open_index + 1 + k * 2;
        CHECK(timeline[emit_at].rfind("emit:", 0) == 0);
        CHECK_EQ(timeline[emit_at + 1], std::string("flush"));
    }

    // 3. LauncherView::Initialize calls StorageInit again through the same
    //    once_flag: same struct, still ok, no second probe pass.
    const StorageInitResult* later = RunStorageInitOnce(once, state, [&]() {
        ++factory_calls;
        return FakeProber{};
    });
    CHECK(later == early);
    CHECK(later->ok);
    CHECK_EQ(factory_calls, 1);
    CHECK_EQ(later->count, static_cast<std::size_t>(5));
}

TEST(StorageInit_FailureNamesTheFolderThatFailedAcrossCallOnce) {
    // Missing TemporaryFolder fails and names "temporary", even when Roaming is
    // also absent, and the second call reports the same struct.
    {
        std::once_flag once;
        StorageInitResult state;
        const StorageInitResult* first = RunStorageInitOnce(once, state, []() {
            FakeProber prober;
            prober.temp = {"", kAccessDenied, false};
            return prober;
        });
        const StorageInitResult* second =
            RunStorageInitOnce(once, state, []() { return FakeProber{}; });
        CHECK(first == second);
        CHECK(!first->ok);
        CHECK(!second->ok);
        CHECK_EQ(FormatStorageFailureLine(*second), std::string("storage init failed: temporary"));
    }

    // Missing LocalFolder falls back to the derived LocalState path for LogOpen,
    // records the derived entry, keeps ok false, and names "local".
    {
        std::once_flag once;
        StorageInitResult state;
        const StorageInitResult* res = RunStorageInitOnce(once, state, []() {
            FakeProber prober;
            prober.local = {"", kPathNotFound, false};
            prober.derived = {"D:\\Packages\\bbxbox\\LocalState", 0, true};
            return prober;
        });
        CHECK(!res->ok);
        CHECK_EQ(res->paths.local_state, std::string("D:\\Packages\\bbxbox\\LocalState"));
        CHECK_EQ(res->count, static_cast<std::size_t>(6));
        CHECK_EQ(std::string(res->entries[5].name), std::string("local (derived)"));
        CHECK_EQ(FormatStorageFailureLine(*res), std::string("storage init failed: local"));
    }

    // Both LocalFolder and TemporaryFolder missing names both.
    {
        std::once_flag once;
        StorageInitResult state;
        const StorageInitResult* res = RunStorageInitOnce(once, state, []() {
            FakeProber prober;
            prober.local = {"", kPathNotFound, false};
            prober.temp = {"", kAccessDenied, false};
            return prober;
        });
        CHECK(!res->ok);
        CHECK_EQ(FormatStorageFailureLine(*res),
                 std::string("storage init failed: local, temporary"));
    }

    // ApplicationData::Current() itself throwing records its HRESULT as its own
    // entry, skips folder calls on the null object, and names it as the failure.
    {
        std::once_flag once;
        StorageInitResult state;
        std::vector<std::string> timeline;
        const StorageInitResult* res = RunStorageInitOnce(once, state, [&]() {
            FakeProber prober;
            prober.timeline = &timeline;
            prober.app = {"", kCoNotInit, false};
            return prober;
        });
        CHECK(!res->ok);
        CHECK(!res->roaming_missing);
        CHECK_EQ(res->count, static_cast<std::size_t>(2));  // ApplicationData::Current + package
        CHECK_EQ(std::string(res->entries[0].name), std::string("ApplicationData::Current"));
        CHECK_EQ(res->entries[0].hr, kCoNotInit);
        CHECK_EQ(FormatStorageFailureLine(*res),
                 std::string("storage init failed: ApplicationData::Current"));
    }
}

TEST(StorageInit_PresentRoamingEmitsNoWarning) {
    std::once_flag once;
    StorageInitResult state;
    const StorageInitResult* res = RunStorageInitOnce(once, state, []() {
        FakeProber prober;
        prober.roaming = {"C:\\Users\\dev\\RoamingState", 0, true};
        return prober;
    });
    CHECK(res->ok);
    CHECK(!res->roaming_missing);
    int infos = 0;
    int warns = 0;
    int flushes = 0;
    ReplayStorageInit(
        *res,
        [&](Level level, const std::string&) {
            if (level == Level::kInfo) ++infos;
            if (level == Level::kWarn) ++warns;
        },
        [&]() { ++flushes; });
    CHECK_EQ(infos, 5);
    CHECK_EQ(warns, 0);
    CHECK_EQ(flushes, 5);
}
