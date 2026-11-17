// SPDX-License-Identifier: GPL-3.0-or-later
// Logging that survives a crash: everything is unbuffered, timestamped, and
// also reachable from the last-chance logger.
//
// Two pairs of files in LocalState, rotated at startup: `loader.log` /
// `loader.prev.log` (the full run) and `errors.log` / `errors.prev.log` (errors,
// faults, boot steps, the 100 ms heartbeat, and shutdown paths). A crash report
// that says "bbxbox.exe+0x2A1F30, last line: compile shader 0x9A3C..." is
// actionable; one that says "the app closed" is not.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace bbx::platform {

enum class Level : std::uint8_t { kDebug = 0, kInfo, kWarn, kError };

// Opens the logs. `local_state` is the absolute LocalState path (UTF-8).
bool LogOpen(const std::string& local_state);
void LogClose();
void LogSetLevel(Level level);

void LogMessage(Level level, const char* format, ...);
void LogVF(Level level, const char* format, std::va_list args);

// Writes one line at `level` to both `loader.log` and `errors.log` (plus their
// USB mirrors when active) and flushes immediately.
void LogMirroredMessage(Level level, const char* format, ...);

// Elapsed milliseconds since LogOpen, matching the `[seconds.ms]` prefix.
[[nodiscard]] std::uint64_t LogElapsedMs();

// Shorthands: the log is written from guest threads too, so these are
// internally locked and never throw.
template <class... Args>
void LogF(const char* format, Args... args) {
    LogMessage(Level::kInfo, format, args...);
}

// The address of the faulting instruction and the module base, as
// `uwp_last_chance_logger` prints them; CI resolves the RVA with
// xbox/scripts/symbolicate.py against the build's .map.
void LogLastChance(const char* what, std::uintptr_t address, std::uintptr_t module_base);

// Installs the vectored exception handler and the terminate/unhandled hooks.
// Called once, first thing in main.
void InstallLastChanceLogger();

// The last line logged, for the crash handler (no allocation on that path).
const char* LastLine();

// Snapshot of the most recent formatted log lines (chronological, up to 16),
// read by the stall detector when present count freezes while visible.
[[nodiscard]] std::vector<std::string> LogRecentLines();

// --- the black box on the player's stick -----------------------------------
//
// `LogMirrorToUsb` is called as soon as a dump folder is granted (the same
// folder the picker handed out): it creates `<dump>\bbxbox_logs\`, mirrors
// every line that follows there unbuffered, and keeps `crash.log` open beside
// it for the fault report. False (and a logged reason) when the folder cannot
// be written. Safe to call twice; the folder does not change mid-run.
bool LogMirrorToUsb(const std::string& dump_root);
bool LogMirrorActive();

// The backend's breadcrumb: the last command replayed, the PSO or shader that
// was being built. Written into the crash report, so a device hang says what
// the GPU was asked to do rather than "it crashed". No lock, no allocation.
void LogCrumb(const char* what);
// A removed device and its GetDeviceRemovedReason(); part of the same report.
void LogDeviceRemoved(long reason);
long LogDeviceReason();

// Flushes both the log and the crash breadcrumb. Called on suspend and on
// present failures.
void LogFlush();

}  // namespace bbx::platform
