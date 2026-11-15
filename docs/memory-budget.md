# Living in 5,120 MiB

Xbox Game mode gives the process `AppMemoryUsageLimit` = 5,120 MiB. Everything shares it: the
game's guest memory, the GPU copies D3D12 needs, the D3D12 heaps, the shader caches and the
compiler's arena. Going over is not a warning: the process is terminated, usually with a log that
ends mid-frame. The port therefore treats memory as a plan with owners, caps and a pager, and it
proves the plan with a self-test before the game starts.

## 1. The partition

| # | Owner | Cap (MiB) | What it holds | Pressure behaviour |
|---|---|---|---|---|
| 1 | Guest image | 2,048 | the game's RAM, commit-on-touch, `WC_GARLIC` pageable | paged to disk (§4) |
| 2 | GPU shadows | 896 | guest buffer twins, GPU-visible textures (BCn stays compressed) | windowed per frame, LRU evicted |
| 3 | D3D12 heaps | 768 | render targets, depth, staging/upload/readback rings | targets resized by the resolution controller; rings are fixed |
| 4 | Shader compiler and caches | 256 | DXC's arena, `dxil.bin`, PSO library, manifests | precompiler pauses at 768 MiB of headroom |
| 5 | Audio, movies, Scaleform, fonts | 192 | `XAudio2` voices, AV decode buffers, UI atlases | movies drop to 720p decode first |
| 6 | Host code and stacks | 224 | `bbxbox.exe` image, CRT, WinRT, ~90 thread stacks | stacks are 8 MiB VA / 64 KiB commit (§3) |
| 7 | I/O and metadata | 96 | file cache, overlay index, prepared image metadata | file cache is bounded to 32 MiB |
| **8** | **Reserve** | **640** | never handed out; 256 MiB is the pager's hard floor | the pager stops before the floor |

The eight caps sum to the 5,120 MiB Game-mode limit, but only **4,480 MiB is allocatable**: the 640 MiB reserve is subtracted before anything else (`bbx_budget`'s `allocatable()`). Every residency threshold and pager watermark is therefore a fraction of 4,480, not of 5,120 — 75/85/92 % = 3,360 / 3,808 / 4,122 MiB, pager start 4,096, stop 3,968. A budget that measured against the raw limit would page 640 MiB too late.
| | **Total** | **5,120** | | |

The numbers are not guesses: they come from the prior art's Series S runs (`Test-project` reached
3,300–3,500 MiB in Yharnam after the stack fix) scaled to bbhost's larger guest footprint and
D3D12's shadow requirement. `budget.cpp` is the single source of truth, and `loader.log.txt` prints
the table with live values once a minute:

```
Budget: limit 5120 MiB, used 3,412; guest 1,987/2048 gpu 640/896 heaps 402/768 dxc 61/256
        audio 88/192 host 171/224 io 24/96 reserve 640; level elevated (67%), pager idle
```

## 2. The guest arena: below 1 TiB, backed on touch

The PS4's GPU addresses are 40-bit and the guest's own code assumes it. The port therefore:

* reserves the guest's windows with `VirtualAlloc2FromApp(..., MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
  PAGE_NOACCESS, ...)`, all below `0x10000000000` (1 TiB), and links with
  `/HIGHENTROPYVA:NO` so the 64 GiB–1 TiB band stays free;
* splits a placeholder to commit a 64 KiB region when the first page in it is touched
  (`MEM_REPLACE_PLACEHOLDER`), so a 4 KiB protection change inside a 16 KiB guest page does not
  commit the whole page;
* aliases guest-visible pages with `MapViewOfFile3FromApp` when the guest makes a live alias, and
  promotes that page to a small section exactly once;
* keeps all bookkeeping in sorted arrays indexed by address (a binary search per page), never in a
  hash map, because the fault path runs at guest code speed.

The fault handler is the loader's, as upstream: it answers "commit this page", "restore this
page from disk", "pin this page because the CPU touched GPU-visible memory", and otherwise the
guest's own faults propagate.

## 3. The 1.28 GiB thread-stack fix

The prior art's hardware run found the single largest avoidable allocation in the port:

```
/STACK:16777216,16777216   →  16 MiB commit per OS thread × 82 threads ≈ 1,312 MiB
/STACK:8388608,65536       →   64 KiB initial commit, growing to 8 MiB as used ≈ 30 MiB total
```

bbhost creates a thread per guest thread plus its own (`bb-cp`, `bb-record`, `bb-submit`,
`bb-labels`, `bb-hlsl`, 4 precompile workers), and 82 threads is a realistic count in the world. The
link line is in `xbox/CMakeLists.txt` and is **not** configurable: `pthread_attr_setstacksize` cannot
override a PE stack commit, so the link line is the only correct place. The acceptance run checks
`AppMemoryUsage` at `presents=0` and in Yharnam specifically to catch a regression here.

Guest threads keep their own stacks: the guest's requested stack is a guest address range
(< 1 TiB) backed on touch, not a host thread stack.

## 4. The sparse `WC_GARLIC` pager

Only some memory may page out:

| Page kind | Pageable | Why |
|---|---|---|
| guest code (`WC_...`, executable) | **never** | running code cannot fault in a page it is executing |
| guest CPU-touched (`PAGE_READWRITE` and walked by guest code) | pinned on first CPU touch | paging it costs two faults per access |
| GPU-visible, cold, ≥ 4 MiB heap (`WC_GARLIC`) | **yes** | written by the GPU, read next frame; the pager's target |
| host/CRT/D3D12 heaps | not by the pager | owned by their allocators (§1) |

Thresholds, derived from the limit rather than hard-coded:

```
start paging   when used >= limit - 384 MiB   (4,736 MiB)
stop paging    when used <= limit - 512 MiB   (4,608 MiB)
page size      4 KiB, protection preserved across eviction and page-in
integrity      FNV-1a per page, verified on page-in (corrupt=0 is an acceptance criterion)
window         64 MiB of mapped views at most; 128 MiB frame pool; views are registered and never
               recycled while the GPU may still read them
```

Each pass releases 2 MiB and never recycles the shared page-in window, so a pass cannot churn the
pages a frame is reading. The pager's counters are printed with the budget line
(`UWP swap: out=… in=… disk=… corrupt=0`).

## 5. D3D12's own accounting

D3D12 has no host-pointer import, so two costs exist that Vulkan did not have:

* **Buffer twins.** Every guest buffer a draw reads has a GPU copy. The backend uploads only the
  4 KiB pages `write_watch` reports dirty, once per frame, into a 64 MiB upload ring; pages that are
  written every frame by the guest (dynamic vertex/constant buffers) are detected and uploaded
  whole instead of per page.
* **Texture copies stay compressed.** A BCn texture is uploaded as BCn; the backend never
  decompresses to RGBA to make a copy. A 1024×1024 BC3 texture is 1 MiB as the game wrote it and
  4 MiB as RGBA — the difference is the difference between fitting and not.

The planner (`budget.cpp`) answers three questions per allocation: does it fit its owner's cap, if
not what is evictable (texture pool LRU, then shadow windows), and does the frame have enough
headroom to record at all. A refusal is a logged, graceful degradation (skip a shadow, lower the
resolution step), never an `E_OUTOFMEMORY` in the middle of a command list.

### Residency tiers

`AppMemoryUsageLevel` drives four tiers, checked once per frame:

| Tier | Trigger | Action |
|---|---|---|
| Normal | < 75% (3,840 MiB) | retain up to 4,096 cached descriptor tables (age ≤ 600 frames) |
| Elevated | 75–85% | trim tables older than 120 frames down to 2,048; pager armed |
| High | 85–92% | tables down to 512 hot entries (age ≤ 30); drop one resolution step; trim DXIL cache |
| Critical | ≥ 92% (4,710 MiB) | keep 64 tables, flush staging, `IDXGIDevice3::Trim()`, and if still critical, drop another resolution step this frame |

The tiers are the prior art's, unchanged, because they were tuned on the same hardware against the
same limit.

## 6. Thread budget

Seven cores are granted in Game mode; the port plans for them explicitly:

| Pool | Threads | Notes |
|---|---|---|
| guest main + job threads | up to 16 | the game's own; their count is the game's business |
| `bb-cp` | 1 | walks command buffers, takes tokens |
| `bb-record` | 1 | replays the stream onto D3D12 command lists |
| `bb-submit` | 1 | submits, merges when behind |
| `bb-labels` | 1 | waits on the direct fence, writes labels |
| `bb-hlsl` (DXC) | ≤ 2 | clamped `min(max(1, cores/4), 2)`; never during gameplay after precompile |
| precompile workers | 2 (pre-game only) | paused when the game starts |

DXC is the only CPU-hungry background work, which is why it is clamped and why the precompiler is
front-loaded: a compile burst on a 7-core console is audible through `XAudio2` if it runs during
play.

## 7. The memory contract self-test

Before the game starts on the first launch after install (and whenever
`BB_MEMORY_SELFTEST=1`), the app walks the real contract through the loader's own fault handler:

1. lazy zero pages fault in and read as zero;
2. a remapped range preserves its contents;
3. a live alias sees the same physical page from two addresses;
4. a 4 KiB protection change inside one 16 KiB guest page leaves its neighbours alone;
5. a protected host write is refused and reported;
6. nested region queries with a queued writer do not deadlock;
7. concurrent commits from several threads are correct;
8. disk eviction and page-in round-trip a checksummed page;
9. concurrent readers survive an eviction pass;
10. guest file I/O across a paged range works;
11. guest-code pins are honoured and released.

Result goes to `LocalState\memory-selftest.txt` (`result=0`) and the log
(`UWP memory: contract self-test PASSED (result 0)`). A failure stops the launch instead of
starting the game on a half-restored memory manager — the prior art learned that the hard way, and a
console with no debugger has no second chance to find out which invariant broke.

## 8. Measuring

* `loader.log.txt` budget line: once a minute and at every map change.
* `BB_D3D12_STATS=1`: heap use per owner, PSO count, descriptor occupancy, submission latency.
* `presents=0` acceptance: ≤ 1,100 MiB. Yharnam: ≤ 3,500 MiB. Both are in
  `docs/validation.md`; a run that exceeds either is a failure even if the frame rate is fine.
