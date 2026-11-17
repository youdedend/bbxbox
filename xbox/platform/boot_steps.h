// SPDX-License-Identifier: GPL-3.0-or-later
// Boot step, 100 ms heartbeat, and shutdown instrumentation, pure C++20 so the
// host tests run the same tracker that wWinMain and LauncherView drive on the
// console. Every emitted line goes through one sink that writes and flushes to
// both loader.log and errors.log.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "xbox/platform/log.h"

namespace bbx::platform {

inline constexpr std::uint64_t kHeartbeatPeriodMs = 100;

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
        std::chrono::milliseconds period = std::chrono::milliseconds(kHeartbeatPeriodMs)) {
        std::lock_guard<std::mutex> join_lock(join_mu_);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stopped_ || heartbeat_thread_.joinable()) return;
        }
        heartbeat_thread_ = std::thread([this, period]() {
            std::unique_lock<std::mutex> lock(mu_);
            while (!cv_.wait_for(lock, period, [this]() { return stopped_; })) {
                const std::uint64_t elapsed = clock_ms_ ? clock_ms_() : 0;
                EmitHeartbeatLocked(elapsed);
            }
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
};

}  // namespace bbx::platform
