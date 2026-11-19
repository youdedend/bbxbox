// SPDX-License-Identifier: GPL-3.0-or-later
// On-console first-boot shader baker (Xbox Series X|S Dev Mode UWP).
//
// Compiles Bloodborne's PS4 GCN shaders directly on the Xbox into signed DXIL
// SM 6.4 (`bbx_shaders.cache`, `bbx_shaders.cache.stamp.txt`, `bbx_shaders.paths`)
// using the packaged x64 `dxil.dll` + `dxcompiler.dll` loaded via
// `LoadPackagedLibrary`, then unloads both DLLs via `FreeLibrary` before the
// guest allocates its ~3.8 GiB arena. Result: zero PC bake step, full DXC -O3
// shader optimization, and 0 MiB compiler RAM overhead during gameplay.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "src/host/d3d12/bbx_shader_bake.h"

namespace bbx::platform {

struct BakeRequest {
    std::string app0;        // base dump root (contains shader bundles / eboot)
    std::string update_dir;  // optional v1.09 update overlay root (scanned first)
    std::string eboot;       // full path to eboot.bin (fake SELF or flat ELF)
    std::string out_dir;     // writable output dir (LocalState\data)
};

struct BakeResult {
    bool ok = false;
    bool cancelled = false;
    std::uint32_t entries_written = 0;
    std::uint32_t refused_count = 0;
    std::uint32_t quarantined_count = 0;
    std::uint32_t elapsed_seconds = 0;
    std::string summary;
};

// Thread-safe progress channel read at 60 Hz by the launcher HUD while the
// baker runs on a background thread.
class BakeProgressState {
public:
    void Reset() {
        std::lock_guard<std::mutex> lock(mu_);
        snap_ = bbx::d3d12::BakeProgressSnapshot{};
        cancel_.store(false, std::memory_order_relaxed);
    }

    void RequestCancel() noexcept { cancel_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool CancelRequested() const noexcept {
        return cancel_.load(std::memory_order_relaxed);
    }

    void Update(const bbx::d3d12::BakeProgressSnapshot& snap) {
        std::lock_guard<std::mutex> lock(mu_);
        snap_ = snap;
    }

    [[nodiscard]] bbx::d3d12::BakeProgressSnapshot Snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return snap_;
    }

private:
    mutable std::mutex mu_;
    bbx::d3d12::BakeProgressSnapshot snap_;
    std::atomic<bool> cancel_{false};
};

// Checks `search_dirs` in order for a valid `bbx_shaders.cache` + `.stamp.txt`
// + `bbx_shaders.paths` triple matching this build's console stamp. When true,
// no first-boot baking is needed and `dxcompiler.dll` is never loaded.
[[nodiscard]] bool HasValidConsoleCache(const std::vector<std::string>& search_dirs,
                                        std::string* found_dir = nullptr,
                                        std::string* report = nullptr);

// Runs the complete first-boot bake on the calling thread (expected to be a
// background thread so the UI thread keeps pumping CoreWindow events and
// presenting the HUD progress bar). Unloads `dxcompiler.dll` and `dxil.dll`
// before returning regardless of outcome.
[[nodiscard]] BakeResult BakeConsoleShaders(const BakeRequest& req, BakeProgressState& progress);

}  // namespace bbx::platform
