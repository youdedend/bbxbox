# Validation

Two kinds of proof: what CI can check on a build machine, and what a console run must show. Nothing
in this port is "done" because it compiles.

## 1. Build machine (CI)

| Gate | Command | Passes when |
|---|---|---|
| Portable core builds | `cmake -S . -B build -DBBX_HOST_ONLY=ON && cmake --build build` | no warnings in the port's own sources (`-Wall -Wextra -Werror`) |
| Unit tests | `ctest --test-dir build --output-on-failure` | all of `tests/` green |
| Syntax check of UWP-only sources | `tools/syntax_check.sh` (clang-cl, Windows runners) | zero errors against the GDK headers |
| PE import audit | `xbox/scripts/check-imports.ps1` | every active import resolves to `WindowsApp.lib`, the Store CRT, or a DLL in the package |
| Manifest lint | `xbox/scripts/check-manifest.ps1` | `x64`, `Windows.Universal`, `codeGeneration` + `removableStorage`, **no** `runFullTrust`, **no** `broadFileSystemAccess` |
| Package | `xbox/scripts/package.ps1` | `.appx` + `bbxbox.map` + `ThirdParty/` licences produced; size ≤ 120 MiB |
| Symbolication smoke test | `xbox/scripts/symbolicate.py --self-test` | a known RVA resolves to a file:line |

The unit tests are the port's own logic, not the game's:

| Test | What it pins |
|---|---|
| `budget_test` | the partition sums to 5,120 MiB; caps never over-commit; the four residency tiers trigger at the documented percentages; the pager's start/stop thresholds are consistent |
| `cache_test` | `dxil.bin` header/entry layout, CRC, lazy read; stamp mismatch invalidates the whole cache; truncated file rejected; the PSO key's canonical bytes for a fixed input |
| `quarantine_test` | inflight → quarantine promotion; sorted dedup; blank/truncated lines; pair quarantine writes both hashes |
| `precompile_plan_test` | dedupe by hash, manifest ordering, skips quarantined entries, pauses on memory pressure and resumes at the documented headroom |
| `format_test` | every GCN `dfmt`/`nfmt` pair the game uses maps to the documented DXGI format, with sRGB/u/int/BC handled and unknown pairs rejected |
| `barrier_test` | every (Vulkan stage, access) pair the recorder can emit maps to a D3D12 state set; an unmapped pair is a test failure |
| `descriptor_test` | ring allocation, free-list retirement by fence value, no reuse before the fence |
| `stream_walk_test` | record headers are 8-byte aligned and in bounds; record sizes match the pinned layout (fails on an upstream layout change) |
| `present_policy_test` | tearing only when supported **and** vsync off; triple buffering; vsync latch never presents more than once per refresh |
| `untile_test` | tile address math against a CPU reference for every tiling mode and odd sizes |

## 2. Console runs

Each run is a numbered acceptance test with a log. A run is only useful if the package version, the
dump layout and the numbers are in the report.

### T1 — install and boot (first launch)

* `.appx` installs in Game mode (App type = `Game`).
* `loader.log.txt` contains `Budget: limit 5120 MiB`.
* `UWP memory: contract self-test PASSED (result 0)`, `LocalState\memory-selftest.txt` = `result=0`.
* `AppMemoryUsage` at `presents=0` **≤ 1,100 MiB**.
* `presents > 1,000`, `corrupt=0`.

### T2 — dump layouts

* merged 01.09: `DumpLayout: kind=merged … effective=01.09`, `patches=required`.
* split 01.00 + patch 01.09: both folders logged, `effective=01.09`, the USB folders' SHA-256
  unchanged after the run (nothing written to the dump).
* base only: banner, `patches=skipped` only after the confirmation.
* the choice survives a reboot (`bbxbox.ini` + `FutureAccessList`).
* a removed USB drive produces a clear message, not a crash.

### T3 — input, audio, keyboard

* Every mapped button reaches the game (movement, attack, dodge, transform, item use).
* Touchpad chords: `View + Left` opens Gestures, a `View` tap opens Personal Effects, a `View` hold
  used as a modifier does not also fire a right click.
* Hunter Name entry works with the virtual keyboard and the name persists in the save.
* Audio is continuous for 10 minutes with no underruns; `Audio: XAudio2 mastering voice ready (N
  channels)` names the format; a 7.1 stream is not downmixed silently.

### T4 — precompile and cache

* First `Precompile shaders`: `Cache: N stages, M pipelines …`, ends with
  `precompile: done, 0 quarantined`.
* Second launch: `precompile: 0 compiled, N reused` at the title screen.
* A warm start reaches gameplay with no PSO creation during the first minute (`BB_D3D12_STATS=1`
  shows zero new PSOs).
* USB round-trip: export, wipe `LocalState\bbxbox\cache`, import, second launch reuses the cache;
  a cache built with another stamp is reported and skipped.
* Deliberate quarantine drill: `precompile_quarantine.txt` with one hash boots cleanly, logs
  `skipping 1 previously quarantined shader`, and the rest of the game compiles.

### T5 — rendering correctness

* Title screen, menus, HUD, subtitles: no missing geometry, no upside-down or swizzled textures, no
  Z-fighting that bbhost's Vulkan build does not have.
* Central Yharnam: geometry, shadows, fog, particles, blood effects, cloth.
* YEBIS post-processing: the game's own tone mapping, DOF, motion blur, and the menu's backdrop.
* Scaleform UI text is legible at 1080p; the Hunter's Dream mirror (appearance editor) works.

### T6 — endurance

* `presents > 10,000` in Yharnam, combat, a lamp warp to the Dream and back, one boss attempt.
* **Zero** device-removed events; `corrupt=0`; `window evictions=0`.
* `AppMemoryUsage` ≤ 3,500 MiB in gameplay (levels 0–1), stable across the run (no monotonic rise:
  the last hour's peak is within 64 MiB of the first ten minutes').
* No `UNHANDLED NATIVE FAULT` lines.

### T7 — lifecycle

* Guide → Home → resume: no crash, audio returns, the frame rate recovers within 2 s.
* Suspend during gameplay: `plm-checkpoint.json` written, `Suspending` completes in < 5 s (logged).
* Terminate while suspended, relaunch: the save is intact and the cache is valid.
* Pull the controller: input resumes on reconnect without a restart.

### T8 — the 60 FPS claim

Measured, not asserted. Protocol:

1. Load Central Yharnam at the lamp, stand still 30 s, then run the same 60 s route (lamp → plaza →
   Cleric Beast fog gate) three times, with `BB_D3D12_STATS=1` and the perf summary on.
2. Record: average FPS, 1% low, GPU busy %, `cp`/`record` load %, `AppMemoryUsage`, resolution the
   dynamic controller settled on.
3. Series X at 1080p: **average ≥ 60 FPS, 1% low ≥ 55 FPS** with GPU busy ≤ 90%.
4. Series S at 900p–1080p dynamic: **average ≥ 60 FPS, 1% low ≥ 50 FPS**, resolution ≥ 810p.
5. The same route in the Hunter's Dream must hold 60 FPS (a place where YEBIS dominates).

A run that misses any of T8's numbers is reported as a miss with the numbers, not massaged.

## 3. Reporting a run

Paste, in this order:

```
package:      bbxbox_<version>_x64.appx, bbhost <commit>, build <CI run id>
console:      Series X | Series S, Game mode
dump:         merged | split | base-only, app_ver, TITLE_ID
T1..T8:       pass/fail per item with the log lines quoted
memory:       presents=0 …… MiB, Yharnam peak …… MiB, level …
perf:         avg …… fps, 1% low ……, GPU busy ……%, resolution ……
faults:       none | the UNHANDLED lines verbatim + symbolicate.py output
quarantine:   count + hashes
files:        loader.log.txt, report.txt, d3d12-device-removed.txt (if any), bbxbox.map
```

## 4. Regression list

These have burned a previous Xbox UWP port; each has a test or an acceptance line above:

| Regression | Guard |
|---|---|
| 16 MiB stack commit × 80 threads (1.28 GiB) | link line; T1 memory ceiling |
| guest address above 1 TiB | arena reservation below 1 TiB; memory self-test |
| paging a page the GPU is reading | live-view registry; `corrupt=0`, `window evictions=0` |
| cache adopted from another device/build | stamp validation; T4 USB test |
| DXC crash → boot loop | inflight breadcrumb; T4 quarantine drill |
| tearing flag without support | `present_policy_test`; device-removed check in T6 |
| shader compile on the render thread mid-frame | precompile; T4 zero-new-PSO check |
| audio crackle under compile load | DXC clamp to 2 workers; T3 |
| save torn down while a session uses it | ref-counted mounts (upstream); T7 |
| crash report with no symbol | `/MAP` + symbolicate in CI; T6 fault format |
