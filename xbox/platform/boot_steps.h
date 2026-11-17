// SPDX-License-Identifier: GPL-3.0-or-later
// Boot step, 100 ms heartbeat, framework-view callback logging, 5 s telemetry
// + 2-sample stall detector, and shutdown instrumentation, pure C++20 so the
// host tests run the same state machines that wWinMain and LauncherView drive
// on the console. Every emitted line goes through one sink that writes and
// flushes to both loader.log and errors.log.
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "xbox/platform/log.h"

namespace bbx::platform {

inline constexpr std::uint64_t kHeartbeatPeriodMs = 100;
inline constexpr std::uint64_t kTelemetryPeriodSeconds = 5;
inline constexpr std::uint64_t kAcquireGatePeriodMs = 1000;

enum class BootStep : std::uint32_t {
    kBoot = 0,
    kLauncherViewCreated = 1,
    kDisplayAcquired = 2,
    kFirstFramePresented = 3,
    kDumpScanStarted = 4,
    kDumpScanFinished = 5,
    kSelectionAccepted = 6,
};

[[nodiscard]] inline const char* BootStepName(BootStep step) {
    switch (step) {
    case BootStep::kBoot: return "wWinMain";
    case BootStep::kLauncherViewCreated: return "launcher view created";
    case BootStep::kDisplayAcquired: return "display acquired";
    case BootStep::kFirstFramePresented: return "first frame presented";
    case BootStep::kDumpScanStarted: return "dump scan started";
    case BootStep::kDumpScanFinished: return "dump scan finished";
    case BootStep::kSelectionAccepted: return "selection accepted";
    }
    return "unknown";
}

[[nodiscard]] inline const char* AppMemoryLevelName(std::uint32_t level) {
    switch (level) {
    case 0: return "Low";
    case 1: return "Medium";
    case 2: return "High";
    case 3: return "OverLimit";
    default: return "Unknown";
    }
}

struct StepSnapshot {
    std::uint32_t counter = 0;
    BootStep step = BootStep::kBoot;
    std::uint64_t elapsed_ms = 0;
};

struct ThreadDiagnostic {
    std::uint32_t tid = 0;
    std::int32_t base_priority = 0;
    std::string name;
};

class BootStepTracker {
public:
    using SinkFn = std::function<void(Level, const std::string&)>;
    using ClockFn = std::function<std::uint64_t()>;

    BootStepTracker(SinkFn sink, ClockFn clock_ms)
        : sink_(std::move(sink)), clock_ms_(std::move(clock_ms)) {}

    ~BootStepTracker() { StopHeartbeatThread(); }

    BootStepTracker(const BootStepTracker&) = delete;
    BootStepTracker& operator=(const BootStepTracker&) = delete;

    void LauncherViewCreated() {
        Advance(BootStep::kLauncherViewCreated, "launcher view created");
    }

    void DisplayAcquired(const char* backend = nullptr) {
        std::string text = "display acquired";
        if (backend != nullptr && backend[0] != '\0') {
            text += " (";
            text += backend;
            text += ')';
        }
        Advance(BootStep::kDisplayAcquired, text);
    }

    bool FirstFramePresented() {
        std::lock_guard<std::mutex> lock(mu_);
        if (first_frame_logged_) return false;
        first_frame_logged_ = true;
        ++step_counter_;
        current_step_ = BootStep::kFirstFramePresented;
        EmitLocked(Level::kInfo,
                   "step " + std::to_string(step_counter_) + ": first frame presented");
        return true;
    }

    void DumpScanStarted() { Advance(BootStep::kDumpScanStarted, "dump scan started"); }

    void DumpScanFinished(std::size_t found_count) {
        Advance(BootStep::kDumpScanFinished,
                "dump scan finished (found " + std::to_string(found_count) + ")");
    }

    void SelectionAccepted(const char* action = nullptr) {
        std::string text = "selection accepted";
        if (action != nullptr && action[0] != '\0') {
            text += " (";
            text += action;
            text += ')';
        }
        Advance(BootStep::kSelectionAccepted, text);
    }

    // Logs a framework-view callback event serialized through the same dual-log
    // sink as steps, heartbeats, and shutdowns.
    void LogCallback(const std::string& detail) {
        std::lock_guard<std::mutex> lock(mu_);
        EmitLocked(Level::kInfo, "view: " + detail);
    }

    // Logs LauncherView::Uninitialize naming the step counter, step name, and
    // elapsed time reached when the framework view tore down.
    void LogUninitialize() {
        std::lock_guard<std::mutex> lock(mu_);
        const std::uint64_t elapsed = clock_ms_ ? clock_ms_() : 0;
        std::string line = "view: Uninitialize (step=";
        line += std::to_string(step_counter_);
        line += ": ";
        line += BootStepName(current_step_);
        line += ", elapsed=";
        line += std::to_string(elapsed);
        line += "ms)";
        EmitLocked(Level::kInfo, line);
    }

    // Emits one heartbeat line when the tracker has not entered shutdown.
    bool EmitHeartbeatAt(std::uint64_t elapsed_ms) {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopped_) return false;
        EmitHeartbeatLocked(elapsed_ms);
        return true;
    }

    // Deterministic 100 ms cadence driver for host tests and cooperative loops.
    std::size_t TickUpTo(std::uint64_t elapsed_ms) {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopped_) return 0;
        std::size_t emitted = 0;
        while (!stopped_ && next_heartbeat_ms_ <= elapsed_ms) {
            EmitHeartbeatLocked(next_heartbeat_ms_);
            next_heartbeat_ms_ += kHeartbeatPeriodMs;
            ++emitted;
        }
        return emitted;
    }

    void StartHeartbeatThread(
        std::chrono::milliseconds period = std::chrono::milliseconds(kHeartbeatPeriodMs),
        std::function<void()> on_thread_start = {},
        std::function<void()> on_thread_exit = {}) {
        std::lock_guard<std::mutex> join_lock(join_mu_);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stopped_ || heartbeat_thread_.joinable()) return;
        }
        heartbeat_thread_ = std::thread(
            [this, period, start_cb = std::move(on_thread_start),
             exit_cb = std::move(on_thread_exit)]() {
                if (start_cb) start_cb();
                {
                    std::unique_lock<std::mutex> lock(mu_);
                    while (!cv_.wait_for(lock, period, [this]() { return stopped_; })) {
                        const std::uint64_t elapsed = clock_ms_ ? clock_ms_() : 0;
                        EmitHeartbeatLocked(elapsed);
                    }
                }
                if (exit_cb) exit_cb();
            });
    }

    // Records a shutdown path naming the step reached, stops the heartbeat, and
    // joins the background heartbeat thread before returning.
    void Shutdown(const char* path) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopped_ = true;
            const std::uint64_t elapsed = clock_ms_ ? clock_ms_() : 0;
            std::string line = "shutdown: ";
            line += (path != nullptr && path[0] != '\0') ? path : "unknown";
            line += " (step=";
            line += std::to_string(step_counter_);
            line += ": ";
            line += BootStepName(current_step_);
            line += ", elapsed=";
            line += std::to_string(elapsed);
            line += "ms)";
            EmitLocked(Level::kInfo, line);
        }
        cv_.notify_all();
        JoinHeartbeatIfNeeded();
    }

    void StopHeartbeatThread() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopped_ = true;
        }
        cv_.notify_all();
        JoinHeartbeatIfNeeded();
    }

    void RegisterThread(std::uint32_t tid, std::string name) {
        std::lock_guard<std::mutex> lock(threads_mu_);
        for (ThreadDiagnostic& existing : registered_threads_) {
            if (existing.tid == tid) {
                existing.name = std::move(name);
                return;
            }
        }
        ThreadDiagnostic entry;
        entry.tid = tid;
        entry.name = std::move(name);
        registered_threads_.push_back(std::move(entry));
    }

    void UnregisterThread(std::uint32_t tid) {
        std::lock_guard<std::mutex> lock(threads_mu_);
        registered_threads_.erase(
            std::remove_if(registered_threads_.begin(), registered_threads_.end(),
                           [tid](const ThreadDiagnostic& entry) { return entry.tid == tid; }),
            registered_threads_.end());
    }

    [[nodiscard]] std::vector<ThreadDiagnostic> RegisteredThreads() const {
        std::lock_guard<std::mutex> lock(threads_mu_);
        return registered_threads_;
    }

    [[nodiscard]] StepSnapshot SnapshotStep() const {
        std::lock_guard<std::mutex> lock(mu_);
        StepSnapshot snap;
        snap.counter = step_counter_;
        snap.step = current_step_;
        snap.elapsed_ms = clock_ms_ ? clock_ms_() : 0;
        return snap;
    }

    [[nodiscard]] std::uint32_t StepCounter() const {
        std::lock_guard<std::mutex> lock(mu_);
        return step_counter_;
    }

    [[nodiscard]] BootStep CurrentStep() const {
        std::lock_guard<std::mutex> lock(mu_);
        return current_step_;
    }

    [[nodiscard]] bool Stopped() const {
        std::lock_guard<std::mutex> lock(mu_);
        return stopped_;
    }

private:
    void Advance(BootStep step, const std::string& description) {
        std::lock_guard<std::mutex> lock(mu_);
        ++step_counter_;
        current_step_ = step;
        EmitLocked(Level::kInfo, "step " + std::to_string(step_counter_) + ": " + description);
    }

    void EmitHeartbeatLocked(std::uint64_t elapsed_ms) {
        std::string line = "heartbeat: step=";
        line += std::to_string(step_counter_);
        line += " (";
        line += BootStepName(current_step_);
        line += ") elapsed=";
        line += std::to_string(elapsed_ms);
        line += "ms";
        EmitLocked(Level::kInfo, line);
    }

    void EmitLocked(Level level, const std::string& line) {
        if (sink_) sink_(level, line);
    }

    void JoinHeartbeatIfNeeded() {
        std::lock_guard<std::mutex> join_lock(join_mu_);
        if (heartbeat_thread_.joinable() &&
            heartbeat_thread_.get_id() != std::this_thread::get_id()) {
            heartbeat_thread_.join();
        }
    }

    SinkFn sink_;
    ClockFn clock_ms_;
    mutable std::mutex mu_;
    std::mutex join_mu_;
    std::condition_variable cv_;
    std::thread heartbeat_thread_;
    std::uint32_t step_counter_ = 0;
    BootStep current_step_ = BootStep::kBoot;
    bool first_frame_logged_ = false;
    bool stopped_ = false;
    std::uint64_t next_heartbeat_ms_ = kHeartbeatPeriodMs;
    mutable std::mutex threads_mu_;
    std::vector<ThreadDiagnostic> registered_threads_;
};

struct TelemetrySample {
    std::uint64_t used_mib = 0;
    std::uint64_t limit_mib = 0;
    std::uint32_t usage_level = 0;
    std::uint64_t presents = 0;
    bool visible = true;
};

// Evaluates 5 s memory/present telemetry samples and dumps the current step,
// recent log lines, and active thread list once the present count has failed to
// change across two consecutive visible samples (10 s).
class StallWatchdog {
public:
    using RecentLogsFn = std::function<std::vector<std::string>()>;
    using ThreadsFn = std::function<std::vector<ThreadDiagnostic>()>;

    StallWatchdog(const BootStepTracker& tracker, BootStepTracker::SinkFn sink,
                  RecentLogsFn recent_logs_fn, ThreadsFn threads_fn,
                  std::uint64_t initial_presents = 0, unsigned threshold_samples = 2)
        : tracker_(tracker),
          sink_(std::move(sink)),
          recent_logs_fn_(std::move(recent_logs_fn)),
          threads_fn_(std::move(threads_fn)),
          last_presents_(initial_presents),
          threshold_samples_(threshold_samples == 0 ? 1u : threshold_samples) {}

    // Processes one 5 s sample; returns true when a stall report was dumped.
    bool OnSample(const TelemetrySample& sample) {
        Emit(Level::kInfo, FormatTelemetryLine(sample));

        if (!sample.visible) {
            stalled_samples_ = 0;
            last_presents_ = sample.presents;
            return false;
        }

        if (sample.presents != last_presents_) {
            stalled_samples_ = 0;
            stall_dumped_ = false;
            last_presents_ = sample.presents;
            return false;
        }

        ++stalled_samples_;
        if (stalled_samples_ < threshold_samples_ || stall_dumped_) {
            return false;
        }

        stall_dumped_ = true;
        // Capture recent lines BEFORE emitting the stall header so the dump
        // shows what preceded the freeze rather than its own output.
        const std::vector<std::string> recent =
            recent_logs_fn_ ? recent_logs_fn_() : std::vector<std::string>{};
        const std::vector<ThreadDiagnostic> threads =
            threads_fn_ ? threads_fn_() : std::vector<ThreadDiagnostic>{};
        const StepSnapshot snap = tracker_.SnapshotStep();

        std::string header = "STALL DETECTED: presents stuck at ";
        header += std::to_string(sample.presents);
        header += " for ";
        header += std::to_string(static_cast<std::uint64_t>(stalled_samples_) *
                                 kTelemetryPeriodSeconds);
        header += "s while visible (step=";
        header += std::to_string(snap.counter);
        header += ": ";
        header += BootStepName(snap.step);
        header += ", elapsed=";
        header += std::to_string(snap.elapsed_ms);
        header += "ms)";
        Emit(Level::kError, header);

        Emit(Level::kInfo, "stall: last log lines (" + std::to_string(recent.size()) + "):");
        for (std::size_t i = 0; i < recent.size(); ++i) {
            Emit(Level::kInfo, "  log[" + std::to_string(i) + "] " + recent[i]);
        }

        Emit(Level::kInfo, "stall: thread list (" + std::to_string(threads.size()) + "):");
        for (const ThreadDiagnostic& th : threads) {
            std::string line = "  thread tid=";
            line += std::to_string(th.tid);
            line += " pri=";
            line += std::to_string(th.base_priority);
            line += " name=";
            line += th.name.empty() ? "unnamed" : th.name;
            Emit(Level::kInfo, line);
        }
        return true;
    }

    [[nodiscard]] unsigned StalledSamples() const { return stalled_samples_; }
    [[nodiscard]] bool StallDumped() const { return stall_dumped_; }

    [[nodiscard]] static std::string FormatTelemetryLine(const TelemetrySample& sample) {
        std::string line = "telemetry: memory=";
        line += std::to_string(sample.used_mib);
        line += '/';
        line += std::to_string(sample.limit_mib);
        line += " MiB (level=";
        line += std::to_string(sample.usage_level);
        line += '/';
        line += AppMemoryLevelName(sample.usage_level);
        line += ", presents=";
        line += std::to_string(sample.presents);
        line += ", visible=";
        line += sample.visible ? '1' : '0';
        line += ')';
        return line;
    }

private:
    void Emit(Level level, const std::string& line) {
        if (sink_) sink_(level, line);
    }

    const BootStepTracker& tracker_;
    BootStepTracker::SinkFn sink_;
    RecentLogsFn recent_logs_fn_;
    ThreadsFn threads_fn_;
    std::uint64_t last_presents_ = 0;
    unsigned threshold_samples_ = 2;
    unsigned stalled_samples_ = 0;
    bool stall_dumped_ = false;
};

struct AcquireGateSnapshot {
    bool acquire_entered = false;
    bool display_ready = false;
    bool stored_window_present = false;
    bool visible = false;
    std::uint32_t visibility_events = 0;
    std::string ui_blocker;
    // Why the display is not up, in plain English. Printed once a second for as
    // long as it is set: there is no SDL fallback on this target, so a device
    // that cannot be created must not turn into a silent window.
    std::string fault;
};

[[nodiscard]] inline std::string FormatAcquireGateLine(const StepSnapshot& step,
                                                       const AcquireGateSnapshot& gate) {
    std::string line = "acquire-gate: entered=";
    line += gate.acquire_entered ? '1' : '0';
    line += " ready=";
    line += gate.display_ready ? '1' : '0';
    line += " step=";
    line += std::to_string(step.counter);
    line += " (";
    line += BootStepName(step.step);
    line += ") stored_window=";
    line += gate.stored_window_present ? '1' : '0';
    line += " visible=";
    line += gate.visible ? '1' : '0';
    line += " vis_events=";
    line += std::to_string(gate.visibility_events);
    line += " ui_blocker=";
    line += gate.ui_blocker.empty() ? "none" : gate.ui_blocker;
    if (!gate.fault.empty()) {
        line += " fault=";
        line += gate.fault;
    }
    return line;
}

}  // namespace bbx::platform
