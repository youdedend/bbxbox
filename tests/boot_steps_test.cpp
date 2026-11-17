// SPDX-License-Identifier: GPL-3.0-or-later
// tests/boot_steps_test.cpp -- BootStepTracker step ordering, 100 ms heartbeat,
// dual-log mirroring with flushes, and clean heartbeat stop on every shutdown
// path.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "harness.h"
#include "xbox/platform/boot_steps.h"
#include "xbox/platform/host_fs.h"

using namespace bbx::platform;

namespace {

namespace fsx = bbx::hostfs;

class TempLogDir {
public:
    explicit TempLogDir(const char* tag) {
        const char* base = std::getenv("TMPDIR");
        if (base == nullptr || base[0] == '\0') base = "/tmp";
        static unsigned counter = 0;
        const unsigned stamp = static_cast<unsigned>(std::time(nullptr));
        char name[80];
        std::snprintf(name, sizeof(name), "bbx_boot_steps_%s_%u_%u", tag, stamp, ++counter);
        dir_ = fsx::JoinPath(base, name);
        (void)fsx::MkDir(dir_, true);
    }

    ~TempLogDir() {
        std::vector<fsx::DirEntry> entries;
        if (fsx::ListDir(dir_, &entries)) {
            for (const fsx::DirEntry& entry : entries) {
                (void)fsx::Unlink(fsx::JoinPath(dir_, entry.name));
            }
        }
        (void)fsx::RmDir(dir_);
    }

    [[nodiscard]] const std::string& Path() const { return dir_; }

private:
    std::string dir_;
};

// Mirrors the exact dual-file + flush contract of LogMirroredMessage into real
// loader.log and errors.log files on disk as well as an in-memory record.
class DualLogRecorder {
public:
    explicit DualLogRecorder(const std::string& dir)
        : loader_path_(fsx::JoinPath(dir, "loader.log")),
          errors_path_(fsx::JoinPath(dir, "errors.log")) {
        loader_fd_ = fsx::Open(loader_path_, fsx::kWrite | fsx::kCreate | fsx::kTruncate);
        errors_fd_ = fsx::Open(errors_path_, fsx::kWrite | fsx::kCreate | fsx::kTruncate);
    }

    ~DualLogRecorder() { Close(); }

    void EmitMirrored(Level level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mu_);
        levels_.push_back(level);
        loader_lines_.push_back(message);
        errors_lines_.push_back(message);

        const std::string formatted = "[I] " + message + "\n";
        if (loader_fd_ >= 0) {
            (void)fsx::Write(loader_fd_, formatted.data(), formatted.size());
            if (fsx::Flush(loader_fd_)) ++loader_flushes_;
        }
        if (errors_fd_ >= 0) {
            (void)fsx::Write(errors_fd_, formatted.data(), formatted.size());
            if (fsx::Flush(errors_fd_)) ++errors_flushes_;
        }
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mu_);
        if (loader_fd_ >= 0) {
            (void)fsx::Close(loader_fd_);
            loader_fd_ = -1;
        }
        if (errors_fd_ >= 0) {
            (void)fsx::Close(errors_fd_);
            errors_fd_ = -1;
        }
    }

    [[nodiscard]] std::vector<std::string> LoaderLines() const {
        std::lock_guard<std::mutex> lock(mu_);
        return loader_lines_;
    }

    [[nodiscard]] std::vector<std::string> ErrorsLines() const {
        std::lock_guard<std::mutex> lock(mu_);
        return errors_lines_;
    }

    [[nodiscard]] std::vector<Level> Levels() const {
        std::lock_guard<std::mutex> lock(mu_);
        return levels_;
    }

    [[nodiscard]] std::size_t LoaderFlushes() const {
        std::lock_guard<std::mutex> lock(mu_);
        return loader_flushes_;
    }

    [[nodiscard]] std::size_t ErrorsFlushes() const {
        std::lock_guard<std::mutex> lock(mu_);
        return errors_flushes_;
    }

    [[nodiscard]] std::string ReadDiskFile(bool errors) const {
        const std::string& path = errors ? errors_path_ : loader_path_;
        const int fd = fsx::Open(path, fsx::kRead);
        if (fd < 0) return {};
        std::string content;
        char buf[512];
        for (;;) {
            const std::int64_t n = fsx::Read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            content.append(buf, static_cast<std::size_t>(n));
        }
        (void)fsx::Close(fd);
        return content;
    }

private:
    std::string loader_path_;
    std::string errors_path_;
    int loader_fd_ = -1;
    int errors_fd_ = -1;
    mutable std::mutex mu_;
    std::vector<Level> levels_;
    std::vector<std::string> loader_lines_;
    std::vector<std::string> errors_lines_;
    std::size_t loader_flushes_ = 0;
    std::size_t errors_flushes_ = 0;
};

}  // namespace

TEST(BootSteps_HealthyBootEmitsOrderedStepsAndMirrorsWithFlushes) {
    TempLogDir temp("healthy");
    DualLogRecorder logs(temp.Path());
    std::uint64_t now_ms = 5;

    BootStepTracker tracker(
        [&logs](Level level, const std::string& line) { logs.EmitMirrored(level, line); },
        [&now_ms]() { return now_ms; });

    CHECK_EQ(tracker.StepCounter(), 0u);
    CHECK_EQ(std::string(BootStepName(tracker.CurrentStep())), std::string("wWinMain"));

    now_ms = 12;
    tracker.LauncherViewCreated();
    now_ms = 28;
    tracker.DisplayAcquired("d3d12");
    now_ms = 34;
    CHECK(tracker.FirstFramePresented());
    // Subsequent frames must not re-emit first frame presented.
    CHECK(!tracker.FirstFramePresented());
    CHECK(!tracker.FirstFramePresented());
    now_ms = 40;
    tracker.DumpScanStarted();
    now_ms = 95;
    tracker.DumpScanFinished(2);
    now_ms = 100;
    CHECK_EQ(tracker.TickUpTo(now_ms), static_cast<std::size_t>(1));
    now_ms = 180;
    tracker.SelectionAccepted("StartGame");
    now_ms = 220;
    tracker.Shutdown("App destructor");
    tracker.Shutdown("return from wWinMain");
    tracker.Shutdown("atexit");

    // Zero heartbeats may appear after shutdown, even if time advances.
    now_ms = 500;
    CHECK_EQ(tracker.TickUpTo(now_ms), static_cast<std::size_t>(0));
    CHECK(!tracker.EmitHeartbeatAt(now_ms));

    const std::vector<std::string> expected = {
        "step 1: launcher view created",
        "step 2: display acquired (d3d12)",
        "step 3: first frame presented",
        "step 4: dump scan started",
        "step 5: dump scan finished (found 2)",
        "heartbeat: step=5 (dump scan finished) elapsed=100ms",
        "step 6: selection accepted (StartGame)",
        "shutdown: App destructor (step=6: selection accepted, elapsed=220ms)",
        "shutdown: return from wWinMain (step=6: selection accepted, elapsed=220ms)",
        "shutdown: atexit (step=6: selection accepted, elapsed=220ms)",
    };

    const std::vector<std::string> loader_lines = logs.LoaderLines();
    const std::vector<std::string> errors_lines = logs.ErrorsLines();
    CHECK_EQ(loader_lines.size(), expected.size());
    CHECK_EQ(errors_lines.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQ(loader_lines[i], expected[i]);
        CHECK_EQ(errors_lines[i], expected[i]);
    }

    for (const Level lvl : logs.Levels()) {
        CHECK_EQ(lvl, Level::kInfo);
    }
    CHECK_EQ(logs.LoaderFlushes(), expected.size());
    CHECK_EQ(logs.ErrorsFlushes(), expected.size());

    logs.Close();
    const std::string disk_loader = logs.ReadDiskFile(/*errors=*/false);
    const std::string disk_errors = logs.ReadDiskFile(/*errors=*/true);
    CHECK(!disk_loader.empty());
    CHECK_EQ(disk_loader, disk_errors);
}

TEST(BootSteps_HeartbeatTracksCurrentStepEvery100MsAndStopsAfterShutdown) {
    TempLogDir temp("cadence");
    DualLogRecorder logs(temp.Path());
    std::uint64_t now_ms = 0;

    BootStepTracker tracker(
        [&logs](Level level, const std::string& line) { logs.EmitMirrored(level, line); },
        [&now_ms]() { return now_ms; });

    tracker.LauncherViewCreated();
    CHECK_EQ(tracker.TickUpTo(99), static_cast<std::size_t>(0));
    CHECK_EQ(tracker.TickUpTo(100), static_cast<std::size_t>(1));

    tracker.DisplayAcquired("d3d12");
    CHECK_EQ(tracker.TickUpTo(350), static_cast<std::size_t>(2));

    now_ms = 380;
    tracker.Shutdown("suspend event");
    CHECK(tracker.Stopped());

    // Advancing well past several 100 ms periods after shutdown emits nothing.
    CHECK_EQ(tracker.TickUpTo(1000), static_cast<std::size_t>(0));
    CHECK(!tracker.EmitHeartbeatAt(400));

    const std::vector<std::string> expected = {
        "step 1: launcher view created",
        "heartbeat: step=1 (launcher view created) elapsed=100ms",
        "step 2: display acquired (d3d12)",
        "heartbeat: step=2 (display acquired) elapsed=200ms",
        "heartbeat: step=2 (display acquired) elapsed=300ms",
        "shutdown: suspend event (step=2: display acquired, elapsed=380ms)",
    };

    const std::vector<std::string> lines = logs.ErrorsLines();
    CHECK_EQ(lines.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQ(lines[i], expected[i]);
    }
}

TEST(BootSteps_AllFiveShutdownPathsNameStepReached) {
    struct PathExpectation {
        const char* path;
        BootStep target_step;
        const char* expected_line;
    };

    const PathExpectation cases[] = {
        {"suspend event", BootStep::kLauncherViewCreated,
         "shutdown: suspend event (step=1: launcher view created, elapsed=31ms)"},
        {"window closed event", BootStep::kFirstFramePresented,
         "shutdown: window closed event (step=3: first frame presented, elapsed=64ms)"},
        {"App destructor", BootStep::kDumpScanStarted,
         "shutdown: App destructor (step=4: dump scan started, elapsed=112ms)"},
        {"return from wWinMain", BootStep::kDumpScanFinished,
         "shutdown: return from wWinMain (step=5: dump scan finished, elapsed=145ms)"},
        {"atexit", BootStep::kSelectionAccepted,
         "shutdown: atexit (step=6: selection accepted, elapsed=210ms)"},
    };

    const std::uint64_t times[] = {31, 64, 112, 145, 210};

    for (std::size_t idx = 0; idx < 5; ++idx) {
        std::vector<std::string> lines;
        const std::uint64_t elapsed = times[idx];
        BootStepTracker tracker(
            [&lines](Level, const std::string& line) { lines.push_back(line); },
            [elapsed]() { return elapsed; });

        const BootStep target = cases[idx].target_step;
        if (target >= BootStep::kLauncherViewCreated) tracker.LauncherViewCreated();
        if (target >= BootStep::kDisplayAcquired) tracker.DisplayAcquired("d3d12");
        if (target >= BootStep::kFirstFramePresented) (void)tracker.FirstFramePresented();
        if (target >= BootStep::kDumpScanStarted) tracker.DumpScanStarted();
        if (target >= BootStep::kDumpScanFinished) tracker.DumpScanFinished(0);
        if (target >= BootStep::kSelectionAccepted) tracker.SelectionAccepted("Quit");

        tracker.Shutdown(cases[idx].path);
        CHECK(!lines.empty());
        CHECK_EQ(lines.back(), std::string(cases[idx].expected_line));
        CHECK_EQ(tracker.TickUpTo(elapsed + 500), static_cast<std::size_t>(0));
    }
}

TEST(BootSteps_BackgroundHeartbeatThreadStopsBeforeShutdownReturns) {
    TempLogDir temp("thread");
    DualLogRecorder logs(temp.Path());
    const auto start = std::chrono::steady_clock::now();

    BootStepTracker tracker(
        [&logs](Level level, const std::string& line) { logs.EmitMirrored(level, line); },
        [start]() {
            const auto delta = std::chrono::steady_clock::now() - start;
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
        });

    tracker.LauncherViewCreated();
    tracker.DisplayAcquired("d3d12");
    (void)tracker.FirstFramePresented();
    tracker.StartHeartbeatThread(std::chrono::milliseconds(10));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::size_t heartbeats = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        heartbeats = 0;
        for (const std::string& line : logs.ErrorsLines()) {
            if (line.rfind("heartbeat: step=3 (first frame presented) elapsed=", 0) == 0) {
                ++heartbeats;
            }
        }
        if (heartbeats >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_MSG(heartbeats >= 2, "expected at least two background heartbeats before shutdown");

    tracker.Shutdown("window closed event");
    const std::size_t count_at_shutdown = logs.ErrorsLines().size();

    // Sleep several periods past Shutdown(): the joined thread must never write again.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const std::vector<std::string> final_loader = logs.LoaderLines();
    const std::vector<std::string> final_errors = logs.ErrorsLines();

    CHECK_EQ(final_errors.size(), count_at_shutdown);
    CHECK_EQ(final_loader.size(), final_errors.size());
    CHECK(!final_errors.empty());
    CHECK_EQ(final_errors[0], std::string("step 1: launcher view created"));
    CHECK_EQ(final_errors[1], std::string("step 2: display acquired (d3d12)"));
    CHECK_EQ(final_errors[2], std::string("step 3: first frame presented"));
    CHECK(final_errors.back().rfind(
              "shutdown: window closed event (step=3: first frame presented, elapsed=", 0) == 0);
}

TEST(BootSteps_FrameworkViewCallbackAndUninitializeSequence) {
    TempLogDir temp("viewseq");
    DualLogRecorder logs(temp.Path());
    std::uint64_t now_ms = 8;

    BootStepTracker tracker(
        [&logs](Level level, const std::string& line) { logs.EmitMirrored(level, line); },
        [&now_ms]() { return now_ms; });

    tracker.LauncherViewCreated();
    now_ms = 10;
    tracker.LogCallback("Initialize (registered Activated, Suspending, Resuming)");
    now_ms = 14;
    tracker.LogCallback(
        "SetWindow (stored CoreWindow + agile_ref; registered Closed, VisibilityChanged, KeyDown, KeyUp)");
    now_ms = 18;
    tracker.LogCallback("Activated (window.Activate() called)");
    tracker.LogCallback("VisibilityChanged (visible=1)");
    now_ms = 22;
    tracker.LogCallback("Run entered");
    now_ms = 36;
    tracker.DisplayAcquired("d3d12");
    now_ms = 37;
    tracker.LogCallback("window.Activate() completed before first present");
    now_ms = 41;
    CHECK(tracker.FirstFramePresented());
    now_ms = 43;
    tracker.DumpScanStarted();
    now_ms = 88;
    tracker.DumpScanFinished(1);
    now_ms = 320;
    tracker.SelectionAccepted("Quit");
    now_ms = 330;
    tracker.LogUninitialize();

    const std::vector<std::string> expected = {
        "step 1: launcher view created",
        "view: Initialize (registered Activated, Suspending, Resuming)",
        "view: SetWindow (stored CoreWindow + agile_ref; registered Closed, VisibilityChanged, KeyDown, KeyUp)",
        "view: Activated (window.Activate() called)",
        "view: VisibilityChanged (visible=1)",
        "view: Run entered",
        "step 2: display acquired (d3d12)",
        "view: window.Activate() completed before first present",
        "step 3: first frame presented",
        "step 4: dump scan started",
        "step 5: dump scan finished (found 1)",
        "step 6: selection accepted (Quit)",
        "view: Uninitialize (step=6: selection accepted, elapsed=330ms)",
    };

    const std::vector<std::string> lines = logs.ErrorsLines();
    CHECK_EQ(lines.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQ(lines[i], expected[i]);
    }
}

TEST(BootSteps_StallWatchdogDumpsStepLogsAndThreadsAfterTwoVisibleStalledSamples) {
    TempLogDir temp("stall");
    DualLogRecorder logs(temp.Path());
    std::uint64_t now_ms = 25;

    BootStepTracker tracker(
        [&logs](Level level, const std::string& line) { logs.EmitMirrored(level, line); },
        [&now_ms]() { return now_ms; });

    tracker.RegisterThread(1001, "ui-main");
    tracker.RegisterThread(1002, "boot-heartbeat");
    tracker.RegisterThread(1003, "telemetry");

    tracker.LauncherViewCreated();
    tracker.DisplayAcquired("d3d12");

    StallWatchdog watchdog(
        tracker,
        [&logs](Level level, const std::string& line) { logs.EmitMirrored(level, line); },
        [&logs]() { return logs.LoaderLines(); },
        [&tracker]() { return tracker.RegisteredThreads(); },
        /*initial_presents=*/0, /*threshold_samples=*/2);

    // Sample 1 at 5 s: presents still 0 while visible -> 1 stalled sample, no dump yet.
    now_ms = 5005;
    CHECK(!watchdog.OnSample(TelemetrySample{412, 5120, 0, 0, true}));
    CHECK_EQ(watchdog.StalledSamples(), 1u);
    CHECK(!watchdog.StallDumped());

    // Sample 2 at 10 s: presents still 0 while visible -> 2nd sample dumps step, logs, threads.
    now_ms = 10012;
    CHECK(watchdog.OnSample(TelemetrySample{414, 5120, 0, 0, true}));
    CHECK_EQ(watchdog.StalledSamples(), 2u);
    CHECK(watchdog.StallDumped());

    const std::vector<std::string> after_dump = logs.ErrorsLines();
    bool saw_header = false;
    bool saw_recent_section = false;
    bool saw_thread_section = false;
    bool saw_ui_thread = false;
    for (const std::string& line : after_dump) {
        if (line ==
            "STALL DETECTED: presents stuck at 0 for 10s while visible (step=2: display acquired, elapsed=10012ms)") {
            saw_header = true;
        }
        if (line.rfind("stall: last log lines (", 0) == 0) saw_recent_section = true;
        if (line.rfind("stall: thread list (3):", 0) == 0) saw_thread_section = true;
        if (line == "  thread tid=1001 pri=0 name=ui-main") saw_ui_thread = true;
        // The recent-lines snapshot must not contain the stall header itself.
        CHECK(line.find("STALL DETECTED") == std::string::npos || line.rfind("  log[", 0) != 0);
    }
    CHECK(saw_header);
    CHECK(saw_recent_section);
    CHECK(saw_thread_section);
    CHECK(saw_ui_thread);

    // Sample 3 at 15 s while still stuck at 0: must not spam-repeat the same dump.
    now_ms = 15020;
    CHECK(!watchdog.OnSample(TelemetrySample{414, 5120, 0, 0, true}));

    // Sample 4 at 20 s: presents advance -> resets stall state and re-arms.
    now_ms = 20025;
    CHECK(!watchdog.OnSample(TelemetrySample{450, 5120, 1, 120, true}));
    CHECK_EQ(watchdog.StalledSamples(), 0u);
    CHECK(!watchdog.StallDumped());

    // Samples 5 & 6 while hidden (visible=false): unchanging presents must NOT trigger stall.
    now_ms = 25030;
    CHECK(!watchdog.OnSample(TelemetrySample{450, 5120, 1, 120, false}));
    now_ms = 30035;
    CHECK(!watchdog.OnSample(TelemetrySample{450, 5120, 1, 120, false}));
    CHECK_EQ(watchdog.StalledSamples(), 0u);
    CHECK(!watchdog.StallDumped());

    // Samples 7 & 8 visible again and stuck at 120: dumps after two samples.
    now_ms = 35040;
    CHECK(!watchdog.OnSample(TelemetrySample{455, 5120, 2, 120, true}));
    now_ms = 40045;
    CHECK(watchdog.OnSample(TelemetrySample{455, 5120, 2, 120, true}));

    // Unregistering a thread removes it from subsequent snapshots.
    tracker.UnregisterThread(1003);
    CHECK_EQ(tracker.RegisteredThreads().size(), static_cast<std::size_t>(2));
}

TEST(BootSteps_AcquireGateLineReportsStepReadyFlagsAndUiBlocker) {
    std::uint64_t now_ms = 1000;
    std::vector<std::string> lines;
    BootStepTracker tracker(
        [&lines](Level, const std::string& line) { lines.push_back(line); },
        [&now_ms]() { return now_ms; });

    tracker.LauncherViewCreated();

    AcquireGateSnapshot blocked;
    blocked.acquire_entered = false;
    blocked.display_ready = false;
    blocked.stored_window_present = true;
    blocked.visible = true;
    blocked.visibility_events = 0;
    blocked.ui_blocker = "winpthreads::SelfTest/pthread_join(cond_worker)";

    const std::string line1 = FormatAcquireGateLine(tracker.SnapshotStep(), blocked);
    CHECK_EQ(line1,
             std::string("acquire-gate: entered=0 ready=0 step=1 (launcher view created) "
                         "stored_window=1 visible=1 vis_events=0 "
                         "ui_blocker=winpthreads::SelfTest/pthread_join(cond_worker)"));

    AcquireGateSnapshot in_d3d12;
    in_d3d12.acquire_entered = true;
    in_d3d12.display_ready = false;
    in_d3d12.stored_window_present = true;
    in_d3d12.visible = true;
    in_d3d12.visibility_events = 1;
    in_d3d12.ui_blocker = "D3d12Session::Create";

    const std::string line2 = FormatAcquireGateLine(tracker.SnapshotStep(), in_d3d12);
    CHECK_EQ(line2,
             std::string("acquire-gate: entered=1 ready=0 step=1 (launcher view created) "
                         "stored_window=1 visible=1 vis_events=1 "
                         "ui_blocker=D3d12Session::Create"));
}


