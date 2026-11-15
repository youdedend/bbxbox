# Console UX: launcher, storage, input, audio, lifecycle

Xbox Dev Mode is a 10-foot environment without a mouse, without a picker that behaves, and without a
debugger. Everything the player touches is a gamepad, everything the app reads is a folder the
player chose once, and everything that goes wrong must say so in a file they can send.

## 1. Boot flow

```
CoreApplication::OnLaunched
  → init_apartment(MTA)
  → ApplicationViewBoundsMode::UseCoreWindow      (no TV overscan crop)
  → RequiresPointerMode::WhenRequested            (no phantom cursor)
  → AddVectoredExceptionHandler(uwp_last_chance_logger)
  → AppMemoryUsageLimit: must be > 2 GiB, else "App mode" dialog + exit
  → redeem FutureAccessList tokens ("game", "patch")
  → scan known roots: D:\DevelopmentFiles, removable drives, LocalState\game
  → parse param.sfo of each candidate, detect merged / split / base-only
  → launcher panel (gamepad navigable)
      [Prepare & Launch] [Precompile shaders] [Pick dump folder] [Sync cache to USB]
  → bbhost starts on a thread of its own; window is the CoreWindow
```

The first launch after install also runs the memory self-test (`docs/memory-budget.md` §7); it adds
a few seconds and writes its result so it never repeats.

## 2. Dump discovery and the split layout

Bloodborne's 1.09 update is often a **separate PS4 folder** next to the base game. A port that reads
only the base folder boots `APP_VER=01.00`, skips every version-gated patch and runs the wrong
build — the prior art's Test #11 crashed exactly this way. The port therefore always resolves a
*layout*:

| Layout | On disk | What the port does |
|---|---|---|
| merged | one folder, `param.sfo` `APP_VER=01.09` | uses it directly |
| split | `Bloodborne\` (01.00) + `Bloodborne-patch\` (01.09, own `eboot.bin`, `sce_module\`, `sce_sys\`, `dvdroot_ps4\`) | finds the sibling, or `[dump] patch_path` in `bbxbox.ini`, or the folder the player picks; overlays it |
| base only | 01.00 and no update anywhere | warning banner; starts unpatched only after "Continue anyway" |

Version rules (`sfo_version.h`, ported from the prior art and covered by 92 tests there):

* `APP_VER` is read from the dump's own `sce_sys/param.sfo`; it is **never** guessed, and an
  unreadable SFO logs its header bytes and reports `unknown`.
* `CONTENT_ID` `…-A0109-V0100` yields `01.09`; the `V0100` packaging group is not the app version.
* `APP_TYPE` `gp` is a base game; `patch`/`gd`/`gde`/`gdp` is an update.
* A candidate update must share the base's `TITLE_ID`, be strictly newer, and carry `eboot.bin`;
  otherwise it is rejected with the reason.

### The overlay: no copying, ever

A retail dump is tens of gigabytes. The port materialises only the *boot container* into
`LocalState\Prepared\overlay` and writes a list of the update's relative paths
(`overlay\files.txt`). The storage layer then redirects every `<dump>\<rel>` the update provides to
`<update>\<rel>`, and merges directory listings so update-only files appear. Neither source folder
is ever written.

The path rules live in `overlay_index.h` as pure C and are unit-tested: case- and
separator-insensitive prefix match, a boundary check so a dump named `Bloodborne` cannot swallow a
sibling `Bloodborne-patch`, and a bounded sorted lookup.

When the overlay's digest changes (a different update, split↔merged), the prepared image and the
shader cache are invalidated **together**, because both describe the same guest build:

```
Prepare: overlay changed (digest 3f9a… -> 71c2…); invalidating Prepared and cache
```

## 3. Storage: `FutureAccessList` and what the sandbox allows

| Area | Access | Notes |
|---|---|---|
| `LocalState` | full | saves, caches, logs, prepared overlay; survives updates |
| `TemporaryFolder` | full | DXIL scratch, untiling scratch; PLM may drop it |
| Package folder | read-only | `dxcompiler.dll`, `dxil.dll`, assets, patches |
| `D:\DevelopmentFiles\...` | read | Dev Mode's developer folder, the fastest path to a dump |
| removable USB | read | via `removableStorage` + `FutureAccessList` |
| anywhere else | not at all | no `broadFileSystemAccess`, no `runFullTrust` |

The player picks the dump folder once with the system folder picker; the app stores the folder's
token in `StorageApplicationPermissions::FutureAccessList` under `game` (and under `patch` for a
separate update) and **redeems it before probing**, holding it for the process lifetime so
RuntimeBroker keeps the access alive for the whole session. If the token cannot be redeemed (drive
letter changed, drive removed), the launcher re-scans and says which folder is missing.

`bbxbox.ini` (in `LocalState`) keeps the human-readable choices so a log reader can see them:

```ini
[dump]
path=D:\DevelopmentFiles\Bloodborne
patch_path=E:\dumps\Bloodborne-patch
use_patch=1

[video]
resolution=1920x1080
dynamic_scale=1
vsync=1

[shaders]
precompile=1
```

## 4. Input

`Windows.Gaming.Input.Gamepad` is polled once per frame (and hot-plug is handled by the `GamepadAdded`
event, so a controller reconnect never wedges the app). Vibration maps GCN's large/small motors to
`GamepadVibration::LeftMotor`/`RightMotor`.

| PS4 | Xbox | Notes |
|---|---|---|
| Cross / Circle / Square / Triangle | A / B / X / Y | |
| L1 / R1 | LB / RB | |
| L2 / R2 | LT / RT | analog triggers threshold 30/255 for the digital bit, full value on the axis |
| Options | Menu | |
| D-Pad | D-Pad | |
| L3 / R3 | L3 / R3 | |
| Touchpad left click | **View + D-Pad Left** | Gestures; suppresses D-Pad Left while held |
| Touchpad right click | **View** tap, or **View + D-Pad Right** | Personal Effects; suppresses D-Pad Right while held |
| (settings overlay) | **L3 + R3** | bbhost's in-game overlay |
| Hunter Name text | system virtual keyboard | `CoreWindow` text input; the gamepad-only path is the keyboard |

The touchpad chords exist because the Xbox pad has no touchpad and the game uses both clicks for
real actions. The chord logic is a small state machine (`input.cpp`) with the "tap View alone =
right click, 150 ms pulse" rule the prior art verified: it must not fire when View was used as a
modifier.

## 5. Audio

`XAudio2` with a mastering voice and one source voice per game stream:

* 48 kHz, float32 or int16 as the game's `sceAudioOut` asks; 2 channels for stereo, 7.1
  (`WAVE_FORMAT_EXTENSIBLE`, channel mask `0x63f`) when the game opens 8.
* 64 rotating buffer slots; a submit is dropped when 63 are queued (the game's own backpressure),
  and `queued_bytes` is exposed so bbhost's audio thread can pace itself.
* ATRAC9 decoding stays upstream's (`LibAtrac9`, `sceAjm`); movies decode through FFmpeg to PCM and
  are submitted through the same voices.
* Two DXC workers plus one audio thread on 7 cores is the configuration the prior art measured as
  crackle-free; the clamp is in `docs/memory-budget.md` §6.

## 6. Crashing usefully

`uwp_last_chance_logger` is a vectored exception handler installed first, before any WinRT or bbhost
code runs. For a first-chance fault it prints, to `loader.log.txt` and to the debug console:

```
UNHANDLED NATIVE FAULT: code=0xc0000005 module=bbxbox.exe+0x2a1f30 rip=0x7ff6... rsp=...
                        addr=0x... thread=bb-record
UNHANDLED PRECOMPILE FAULT: code=... module=dxcompiler.dll+0x... item=418/3209 stage=ps hash=...
```

and, while the precompiler is active, appends the current shader hash to
`LocalState\precompile_quarantine.txt` (§`docs/precompile-cache.md`) so the next boot is not a loop.
The module is resolved through `VirtualQuery(...)->AllocationBase` and compared against the loaded
modules (`bbxbox.exe`, `dxcompiler.dll`, `dxil.dll`, `xaudio2_9.dll`), so the RVA in the log belongs
to a named binary.

CI builds with `/MAP` and uploads `bbxbox.map` with every package; `xbox/scripts/symbolicate.py`
turns `bbxbox.exe+0xNNNNNN` into `file.cpp:line`, which is the difference between "it crashed" and
"it crashed here".

Death also writes a marker file (`LocalState\last-run.txt` with the phase and frame number); a
launch that finds a marker from a run that did not exit cleanly starts with the last-chance logger's
`crashed=previous` note, so the launcher can offer the safe mode.

## 7. Lifecycle (PLM)

Suspending (5 s to comply):

1. take the `SuspendingDeferral`;
2. stop the frame loop, drain the direct queue to idle;
3. flush the DXIL/PSO cache and the save file (write temp, flush, rename);
4. write `LocalState\plm-checkpoint.json` (frame, map, resolution, cache stamp);
5. `IDXGIDevice3::Trim()`, close the swapchain's extra buffers;
6. complete the deferral.

Resuming rebuilds the swapchain if the buffers were lost, restores the resolution and returns to the
loop. Terminating (no deferral) is unchanged from a crash: the write-ahead breadcrumbs are the only
protection, which is why they exist.

## 8. Save data

Guest saves live in `LocalState\savedata\SPRJ0005` (the title's save slot) and are mounted by bbhost's
`sceSaveData` implementation. Mounts are reference counted (a second mount returns the same
`/savedata0` with `ALREADY_MOUNTED`), and a save is never torn down while a session uses it — the
prior art's Test #11 saw three mounts of `SPRJ0005` and a torn-down save; the fix is upstream of
this port and kept.
