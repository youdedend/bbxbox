// SPDX-License-Identifier: GPL-3.0-or-later
// Logging that survives a crash: everything is unbuffered, timestamped, and
// also reachable from the last-chance logger.
//
// Two files in LocalState, rotated at startup: `loader.log` (the run) and
// `loader.prev.log` (the run that crashed). A crash report that says
// "bbxbox.exe+0x2A1F30, last line: compile shader 0x9A3C..." is actionable; one
// that says "the app closed" is not.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>

namespace bbx::platform {

enum class Level : std::uint8_t { kDebug = 0, kInfo, kWarn, kError };

// Opens the logs. `local_state` is the absolute LocalState path (UTF-8).
bool LogOpen(const std::string& local_state);
void LogClose();
void LogSetLevel(Level level);

void LogMessage(Level level, const char* format, ...);
void LogVF(Level level, const char* format, std::va_list args);

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

// Flushes both the log and the crash breadcrumb. Called on suspend and on
// present failures.
void LogFlush();

}  // namespace bbx::platform
