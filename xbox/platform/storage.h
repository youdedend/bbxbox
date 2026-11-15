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
#include <span>
#include <string>
#include <vector>

#include "xbox/platform/dump.h"

namespace bbx::platform {

struct StoragePaths {
    std::string local_state;  // ApplicationData::LocalFolder
    std::string package;      // the read-only install folder
    std::string temp;
    std::string roaming;
};

// Resolves the paths, creates `LocalState\shadercache` and the crash directory.
bool StorageInit();
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

// The cache sync to/from USB: a granted folder, with a manifest so a cache from
// another console is only accepted when the stamps match.
struct UsbCacheTarget {
    std::string root;   // the granted folder path
    std::string token;
};
[[nodiscard]] bool OpenUsbCacheTarget(const std::string& token, UsbCacheTarget& out);

}  // namespace bbx::platform
