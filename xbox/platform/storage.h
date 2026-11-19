// SPDX-License-Identifier: GPL-3.0-or-later
// Files on the console: LocalState, the package, and the player's dump.
//
// The AppContainer has no file system of its own beyond LocalState. A dump lives
// on a USB drive the app may not read until it holds a `FutureAccessList`
// token for it, and the token is handed out by a picker on the UI thread. Once
// granted, the folder is in the app's ACL and the port reads it with plain
// `CreateFile2FromApp`, which is what makes streaming tens of GB possible at
// all: the storage APIs copy, and copying a 30 GB dump is not a launch.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xbox/platform/dump.h"
#include "xbox/platform/log.h"

namespace bbx::platform {

struct StoragePaths {
    std::string local_state;  // ApplicationData::LocalFolder
    std::string package;      // the read-only install folder
    std::string temp;
    std::string roaming;
};

struct StorageProbeOutcome {
    std::string path;
    std::int32_t hr = 0;
    bool ok = false;
};

// One probe recorded during StorageInit, before loader.log exists:
// ApplicationData::Current() itself, each folder asked of it, Package::Current(),
// and (when LocalFolder failed) the kernel32-derived LocalState path.
struct StorageEntry {
    const char* name;
    std::string path;
    std::int32_t hr;
    bool ok;

    StorageEntry() : name(""), path(), hr(0), ok(false) {}
    StorageEntry(const char* entry_name, std::string entry_path, std::int32_t entry_hr,
                 bool entry_ok)
        : name(entry_name), path(std::move(entry_path)), hr(entry_hr), ok(entry_ok) {}
};

inline constexpr std::size_t kMaxStorageEntries = 8;

// Held once by StorageInit's call_once and returned by reference to every
// caller, so wWinMain (before LogOpen) and LauncherView::Initialize (after)
// read the same struct rather than a stack-local flag.
struct StorageInitResult {
    StoragePaths paths;
    StorageEntry entries[kMaxStorageEntries];
    std::size_t count = 0;
    bool ok = false;
    bool roaming_missing = false;
    std::int32_t roaming_hr = 0;
    const char* failed_folder = "";

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return ok; }

    void Reset() {
        paths = StoragePaths{};
        for (std::size_t i = 0; i < kMaxStorageEntries; ++i) {
            entries[i] = StorageEntry{};
        }
        count = 0;
        ok = false;
        roaming_missing = false;
        roaming_hr = 0;
        failed_folder = "";
    }

    void Record(const char* name, std::string path, std::int32_t hr, bool entry_ok) {
        if (count >= kMaxStorageEntries) return;
        entries[count++] = StorageEntry(name, std::move(path), hr, entry_ok);
    }
};

[[nodiscard]] inline std::string FormatStorageHresult(std::int32_t hr) {
    char hex[16] = {};
    std::snprintf(hex, sizeof(hex), "0x%08x",
                  static_cast<unsigned>(static_cast<std::uint32_t>(hr)));
    return hex;
}

[[nodiscard]] inline std::string FormatStorageEntry(const StorageEntry& entry) {
    std::string line = "storage: ";
    line += entry.name != nullptr ? entry.name : "";
    if (entry.ok) {
        line += ' ';
        line += entry.path.empty() ? "ok" : entry.path;
        line += " (";
        line += FormatStorageHresult(entry.hr);
        line += ')';
    } else {
        line += " unavailable (";
        line += FormatStorageHresult(entry.hr);
        line += ')';
    }
    return line;
}

[[nodiscard]] inline std::string FormatStorageFailureLine(const StorageInitResult& state) {
    const char* which = (state.failed_folder != nullptr && state.failed_folder[0] != '\0')
                            ? state.failed_folder
                            : "unknown";
    return std::string("storage init failed: ") + which;
}

// Populates `state` once through `once` without emitting log lines (the log
// file is opened from `state.paths.local_state` afterwards), and returns the
// address of `state` on every call (a pointer rather than a const reference so
// GCC 13's -Wdangling-reference does not mistake the temporary `make_prober`
// lambda for the referent). Local + Temporary is enough; a missing Roaming
// leaves `ok` true and sets `roaming_missing` for one warning on replay.
template <class MakeProber>
const StorageInitResult* RunStorageInitOnce(std::once_flag& once, StorageInitResult& state,
                                            MakeProber&& make_prober) {
    std::call_once(once, [&state, &make_prober]() {
        state.Reset();
        auto prober = make_prober();

        const StorageProbeOutcome app = prober.AppData();
        state.Record("ApplicationData::Current", app.path, app.hr, app.ok);

        bool have_local = false;
        bool have_temp = false;
        if (app.ok) {
            const StorageProbeOutcome local = prober.Local();
            if (local.ok) state.paths.local_state = local.path;
            state.Record("local", local.path, local.hr, local.ok);
            have_local = local.ok;

            const StorageProbeOutcome temp = prober.Temporary();
            if (temp.ok) state.paths.temp = temp.path;
            state.Record("temporary", temp.path, temp.hr, temp.ok);
            have_temp = temp.ok;

            const StorageProbeOutcome roaming = prober.Roaming();
            if (roaming.ok) {
                state.paths.roaming = roaming.path;
            } else {
                state.roaming_missing = true;
                state.roaming_hr = roaming.hr;
            }
            state.Record("roaming", roaming.path, roaming.hr, roaming.ok);
        }

        const StorageProbeOutcome pkg = prober.Package();
        if (pkg.ok) state.paths.package = pkg.path;
        state.Record("package", pkg.path, pkg.hr, pkg.ok);

        if (!app.ok) {
            state.ok = false;
            state.failed_folder = "ApplicationData::Current";
        } else if (!have_local && !have_temp) {
            state.ok = false;
            state.failed_folder = "local, temporary";
        } else if (!have_local) {
            state.ok = false;
            state.failed_folder = "local";
        } else if (!have_temp) {
            state.ok = false;
            state.failed_folder = "temporary";
        } else {
            state.ok = true;
            state.failed_folder = "";
        }

        if (!have_local) {
            const StorageProbeOutcome derived = prober.DeriveLocal();
            if (derived.ok && !derived.path.empty()) {
                state.paths.local_state = derived.path;
                state.Record("local (derived)", derived.path, derived.hr, true);
            }
        }

        if (!state.paths.local_state.empty()) {
            prober.PrepareDirs(state.paths);
        }
    });
    return &state;
}

template <class EmitFn, class FlushFn>
void ReplayStorageInit(const StorageInitResult& state, EmitFn&& emit, FlushFn&& flush) {
    for (std::size_t i = 0; i < state.count; ++i) {
        emit(Level::kInfo, FormatStorageEntry(state.entries[i]));
        flush();
    }
    if (state.roaming_missing) {
        std::string warn = "storage: roaming unavailable (";
        warn += FormatStorageHresult(state.roaming_hr);
        warn += ") - Xbox has no roaming folder; not needed";
        emit(Level::kWarn, warn);
        flush();
    }
}

// Resolves the paths, creates `LocalState\shadercache` and the crash directory,
// and records each probe result for ReplayStorageResults() once loader.log opens.
const StorageInitResult& StorageInit();
void ReplayStorageResults();
[[nodiscard]] const StoragePaths& Paths();

// LocalState-relative helpers ("loader.log", "shadercache\\stages.bin").
[[nodiscard]] std::string Local(const std::string& relative);

// Whole-file reads with a hard cap: a 30 GB file opened by mistake must fail,
// not allocate.
[[nodiscard]] bool ReadText(const std::string& path, std::string& out, std::size_t cap = 1u << 20);
[[nodiscard]] bool ReadBinary(const std::string& path, std::vector<std::uint8_t>& out,
                              std::size_t cap = 256u << 20);
// Writes through a temporary and a rename, so a crash mid-write leaves the old
// file rather than half a cache.
[[nodiscard]] bool WriteText(const std::string& path, const std::string& text);
[[nodiscard]] bool WriteBinary(const std::string& path, std::span<const std::uint8_t> bytes);
[[nodiscard]] bool AppendText(const std::string& path, const std::string& text);

[[nodiscard]] bool Exists(const std::string& path);
[[nodiscard]] bool IsDir(const std::string& path);
[[nodiscard]] std::uint64_t FileSize(const std::string& path);
[[nodiscard]] std::vector<std::string> ListDirs(const std::string& path);
[[nodiscard]] std::vector<std::string> ListFiles(const std::string& path);

// The permission flow. `PickFolderGrant` shows the folder picker (on the UI
// thread, from a worker) and stores the result in the FutureAccessList; the
// caller persists the token. `ResolveGrant` turns a saved token back into a path.
[[nodiscard]] bool PickFolderGrant(std::string& path_out, std::string& token_out);
[[nodiscard]] bool ResolveGrant(const std::string& token, std::string& path_out);
[[nodiscard]] bool ReleaseGrant(const std::string& token);

// Folders worth probing without a picker: the removable drives the manifest
// grants, and the app's own folders (a dump copied to LocalState by a developer).
[[nodiscard]] std::vector<std::string> CandidateDumpRoots();

// The dump, seen through this layer.
[[nodiscard]] DumpFs MakeDumpFs();

// Set when a USB drive probe or dump read encounters EACCES / ERROR_ACCESS_DENIED,
// allowing the launcher to display actionable NTFS permission setup steps.
[[nodiscard]] bool SawUsbAccessDenied();
void ResetUsbAccessDenied();
[[nodiscard]] inline const char* UsbPermissionHint() {
    return "USB access denied: format NTFS, grant ALL APPLICATION PACKAGES Full Control, set drive as Media on Xbox.";
}

// The cache sync to/from USB: a granted folder, with a manifest so a cache from
// another console is only accepted when the stamps match.
struct UsbCacheTarget {
    std::string root;   // the granted folder path
    std::string token;
};
[[nodiscard]] bool OpenUsbCacheTarget(const std::string& token, UsbCacheTarget& out);

}  // namespace bbx::platform
