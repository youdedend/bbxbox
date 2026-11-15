# Ahead-of-gameplay precompile, the PSO/DXIL cache, and the quarantine

The console has no profiler on the player's side and no way to "just recompile": a crash during
shader or pipeline creation is a boot loop, and a boot loop on a console is a support ticket. The
port therefore treats precompilation as a **durable, restartable, attributable** job.

## 1. What is precompiled

| Unit | Source | Output |
|---|---|---|
| Stage | every shader container in every `.shaderbnd.dcx` bundle, plus the eboot's own programs, minus quarantined | validated DXIL blob + interface hash |
| Pipeline | `pipeline-manifest.bin` (bbhost's remembered pipeline list) and `stage-manifest.bin` | PSO key + `D3D12PipelineLibrary` entry |
| Layout | the input layouts those pipelines used | input-layout hash (part of the PSO key) |

The precompiler runs at the title screen (and from the launcher's **Precompile shaders** button),
never mid-game, and it is incremental: what is already in the cache with the current fingerprint is
skipped without opening the bundle.

## 2. Fingerprints: what invalidates what

Upstream's translator fingerprint is a SHA-256 over `src/gcn/*` and `translation_cache.*`. The port
extends the stamp written next to each cache:

```
stamp = H( translator_fingerprint,          # upstream: translator + cache sources
           dxil.target,                     # "dxil-6.6" / "dxil-6.0"
           hlsl_emitter_version,            # the DXIL target's spellings
           dxc.version, dxil.version,       # the compiler and the signer
           root_signature_version,          # the root signature's shape
           bindless_mode,                   # direct-heap-indexing or tables
           feature_level,                   # 12_0
           pso_library_version )            # the pipeline library's format
```

Rules, enforced in `cache.cpp`:

* a **different stamp** ⇒ the whole DXIL/PSO cache is ignored and rebuilt (never partially reused);
* a **truncated or corrupt** file ⇒ rejected with the reason in the log, and one generation is kept
  as `*.rejected` for a bug report;
* the same stamp and a valid file ⇒ `ID3D12Device1::CreatePipelineLibrary` on the blob, which is
  how a warm start avoids creating 3,000 PSOs;
* a cache is **never** adopted by file size or timestamp (that is how ports ship caches from another
  device and crash).

## 3. The breadcrumb protocol

The failure mode this exists for: DXC or the driver crashes on a malformed shader. `__fastfail`
inside static UWP CRT `abort()` bypasses the vectored exception handler, so a last-chance logger
alone cannot quarantine it. The protocol is therefore write-ahead:

```
1. write LocalState\precompile_inflight.txt:
      1
      <kind> <stage-hash> <partner-hash> <bundle> <entry>
   flush it (FlushFileBuffers) — the file must be on disk before the dangerous call
2. compile the shader / create the PSO
3. on success: clear the file (truncate to "0") and continue
4. on crash: nobody runs; the file survives
```

On the next start, **before anything is compiled**:

```
precompile_inflight.txt exists and says 1
  → parse the entry, append its hash(es) to LocalState\precompile_quarantine.txt
  → log  "Precompile: quarantined <hash> (<bundle>/<entry>) after a crash at <stage>"
  → clear the inflight file
  → the shader is skipped by both the precompiler and the runtime pipeline cache
```

Entry format (text, one line, fixed order, ASCII, `\n`):

```
1
stage 9f2c0a11b7d34e55 stage 0 GXShader.shaderbnd/GXFlver_Gbuf_[A].vso
pair  9f2c0a11b7d34e55 3ac0d1e2f4b55667 pair 0 allmaterialbnd.shaderbnd/GXFlver_Gbuf_[A].vso allmaterialbnd.shaderbnd/GXFlver_Gbuf_[A].ppo
```

(`kind`, hashes, pair-or-stage, then the human-readable locations.) The quarantine file is a sorted,
deduplicated list of hashes, one per line — `tests/quarantine_test.cpp` pins parsing, merging and the
sort, including the "same hash twice", "blank line" and "truncated last line" cases.

**Why a pair is quarantined as a pair:** a VS/PS link that crashes DXC is not attributable to one
side. Quarantining only the VS would re-crash on the PS link at the next boot. Both hashes are
written, exactly as the prior art's `diag_partner` does.

## 4. Cache file layout

`LocalState\bbxbox\cache\<serial>\`:

```
stamp.txt                  # the fingerprint above, one line
dxil.bin                   # header + zlib-compressed entries
pso-library.bin            # ID3D12PipelineLibrary::Serialize blob
pipeline-manifest.bin      # upstream's list, copied here for the precompiler
stage-manifest.bin
report.txt                 # last run: counts, reasons, per-shader fallbacks
```

`dxil.bin`:

```c
struct Header {                 // 64 bytes
  char magic[8];                // "BBXDXIL\0"
  uint32_t version;             // 1
  uint32_t entry_size;          // 40
  uint64_t entry_count;
  uint8_t  stamp[32];           // the fingerprint above
  uint32_t crc32;               // over the entry directory
  uint32_t flags;               // bit0: any entry unsigned (diagnostic only)
};
struct Entry {                  // 40 bytes, sorted by key
  uint64_t key;                 // interface hash
  uint64_t program_hash;
  uint32_t offset, size;        // into the blob region, zlib
  uint32_t stage        : 3;
  uint32_t fallback     : 1;
  uint32_t reserved     : 28;
  uint64_t bytecode_hash;
};
```

* Entries are read lazily (offset → inflate → validate), so opening the cache costs a `read` of the
  header, not a 200 MiB load.
* The file is rewritten to `dxil.bin.tmp`, flushed, then renamed, at most once a minute and at
  shutdown; a power loss during the rewrite leaves the old file intact.
* A 512 MiB cap is enforced; beyond it, the least-recently-used entries are dropped first and the
  report says so.

## 5. Portable sync (USB)

`LocalState\bbxbox\cache\<serial>\` can be exported to and imported from a USB drive:

```
<usb>\bbxbox\cache\<serial>\{stamp.txt,dxil.bin,pso-library.bin}
```

* **Import** validates the stamp first; a mismatch is reported and *skipped*, never merged. Then the
  header CRC, then a **sample validation**: 8 random entries are inflated and their bytecode hash
  checked, so a half-copied file is rejected before it can poison a run.
* **Export** copies only after a clean shutdown with the GPU idle; it never runs during gameplay.
* The portability rule is the point: a player's cache built on one console must be usable on
  another Series S/X *only if* the CPU/GPU/driver identity matches, which the stamp covers through
  `feature_level` and the PSO library version. Otherwise the cache is rebuilt (which is correct,
  just slower).

## 6. What the player sees

Launcher (`xbox/platform/main.cpp`):

```
Cache: 1,284 stages, 3,041 pipelines, built by this build (stamp 4f1c…), 172.4 MiB
       [Precompile shaders]   [Sync to USB]   [Sync from USB]
```

`report.txt` and `loader.log.txt` carry the same numbers plus the reasons: `lift 1281, fallback 3,
quarantined 0, dxc-errors 0`. A quarantined entry is named, never silent.

## 7. Failure behaviour, by case

| Case | Behaviour |
|---|---|
| DXC returns an error | entry is not cached; the shader falls back to SPIRV-Cross; the count is in the report |
| DXIL validation fails | same, and the blob is not written |
| `CreateGraphicsPipelineState` fails | PSO key is quarantined by hash; the draw is skipped (logged once) rather than retried every frame |
| Process dies during compile | inflight file is consumed at next boot → quarantine → replay continues past it |
| Cache stamp mismatch | whole cache ignored, one line in the log, rebuilt in the background |
| Cache file truncated | rejected + `.rejected` copy kept, rebuilt |
| USB cache stamp mismatch | import skipped, local cache used |
| Memory pressure during precompile | precompiler pauses at `used + 768 MiB >= limit` and resumes at `limit - 512 MiB` (see `budget.cpp`); it never pushes the process into the ceiling |
