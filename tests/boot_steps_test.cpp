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
