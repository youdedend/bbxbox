# SPDX-License-Identifier: GPL-3.0-or-later
# droogie/bbhost, compiled for the console.
#
# The tree is vendored (upstream/bbhost, pinned in UPSTREAM-PIN) so every UWP
# change is tracked in git and compiled on every CI run: no fetch at build time,
# no stand-in implementation of the guest core.
#
# What is here now: zlib (bbhost's only external C dependency for the shader
# path), LibAtrac9 and monocypher exactly as upstream builds them, and the GCN
# library (decode, container, translate, lift, HLSL emitter) which is what the
# D3D12 backend consumes. `bbhost_guest` (src/core, src/hle) and the host stream
# hooks join the same target as P1 wires them; the source lists below are the
# upstream CMakeLists' own, minus Vulkan.
set(BBX_BBHOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../upstream/bbhost" CACHE PATH
    "vendored bbhost checkout (tools/fetch_upstream.sh)")
if(NOT EXISTS "${BBX_BBHOST_ROOT}/src/gcn/lift.cpp")
    message(FATAL_ERROR "bbhost is not vendored at ${BBX_BBHOST_ROOT}; run tools/fetch_upstream.sh")
endif()

# --- zlib (vendored; bbhost's container.cpp inflates .shaderbnd.dcx) ---------
# The top-level CMakeLists defines the same target for the host build; define it
# only once, whichever file is reached first.
set(BBX_ZLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../upstream/third_party/zlib")
if(NOT TARGET bbx_zlib)
add_library(bbx_zlib STATIC
    "${BBX_ZLIB_ROOT}/adler32.c"
    "${BBX_ZLIB_ROOT}/compress.c"
    "${BBX_ZLIB_ROOT}/crc32.c"
    "${BBX_ZLIB_ROOT}/deflate.c"
    "${BBX_ZLIB_ROOT}/gzclose.c"
    "${BBX_ZLIB_ROOT}/gzlib.c"
    "${BBX_ZLIB_ROOT}/gzread.c"
    "${BBX_ZLIB_ROOT}/gzwrite.c"
    "${BBX_ZLIB_ROOT}/infback.c"
    "${BBX_ZLIB_ROOT}/inffast.c"
    "${BBX_ZLIB_ROOT}/inflate.c"
    "${BBX_ZLIB_ROOT}/inftrees.c"
    "${BBX_ZLIB_ROOT}/trees.c"
    "${BBX_ZLIB_ROOT}/uncompr.c"
    "${BBX_ZLIB_ROOT}/zutil.c"
)
target_include_directories(bbx_zlib PUBLIC "${BBX_ZLIB_ROOT}")
set_target_properties(bbx_zlib PROPERTIES C_STANDARD 99)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(bbx_zlib PRIVATE /W0 /D_CRT_SECURE_NO_WARNINGS)
endif()
endif()

# --- LibAtrac9 (upstream's own source glob) ---------------------------------
file(GLOB BBX_ATRAC9_SOURCES "${BBX_BBHOST_ROOT}/third_party/libatrac9/C/src/*.c")
add_library(bbx_atrac9 STATIC ${BBX_ATRAC9_SOURCES})
target_include_directories(bbx_atrac9 PUBLIC "${BBX_BBHOST_ROOT}/third_party/libatrac9/C/src")
set_target_properties(bbx_atrac9 PROPERTIES C_STANDARD 99)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(bbx_atrac9 PRIVATE /W0)
endif()

# --- monocypher (BSD-2/CC0) --------------------------------------------------
add_library(bbx_monocypher STATIC
    "${BBX_BBHOST_ROOT}/third_party/monocypher/monocypher.c"
    "${BBX_BBHOST_ROOT}/third_party/monocypher/monocypher-ed25519.c")
target_include_directories(bbx_monocypher PUBLIC "${BBX_BBHOST_ROOT}/third_party/monocypher")
set_target_properties(bbx_monocypher PROPERTIES C_STANDARD 99)

# --- the GCN shader library (upstream's `gcn` target) ------------------------
add_library(bbhost_gcn STATIC
    "${BBX_BBHOST_ROOT}/src/gcn/decode.cpp"
    "${BBX_BBHOST_ROOT}/src/gcn/container.cpp"
    "${BBX_BBHOST_ROOT}/src/gcn/spirv.cpp"
    "${BBX_BBHOST_ROOT}/src/gcn/half.cpp"
    "${BBX_BBHOST_ROOT}/src/gcn/translate.cpp"
    "${BBX_BBHOST_ROOT}/src/gcn/lift.cpp"
    "${BBX_BBHOST_ROOT}/src/gcn/image_dims.cpp"
    "${BBX_BBHOST_ROOT}/src/gcn/wave.cpp"
    "${BBX_BBHOST_ROOT}/src/gcn/names.cpp"
    "${BBX_BBHOST_ROOT}/src/gcn/hlsl.cpp"
    "${BBX_BBHOST_ROOT}/src/gcn/gs_vertex.cpp"
)
target_include_directories(bbhost_gcn PUBLIC "${BBX_BBHOST_ROOT}/src")
target_compile_features(bbhost_gcn PUBLIC cxx_std_20)
target_compile_definitions(bbhost_gcn PUBLIC BBX_UWP=1)
# Upstream's code is not /W4-clean on clang-cl and its warnings are not this
# port's to fix; the port's own TUs keep /W4 /WX.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(bbhost_gcn PRIVATE /W3 /EHsc /permissive-)
endif()
target_link_libraries(bbhost_gcn PUBLIC bbx_zlib)

# --- the guest core, for the console -----------------------------------------
# bbhost's guest is 33,000 lines of `src/hle` over `src/core`, and it reaches
# into its host (`src/host`) for the renderer, the audio device, the engine
# patches and the decomp hooks. The port takes it a translation unit at a time:
# this library is the list that is already clean against the Store SDK and the
# AppContainer, compiled on every CI run so the list only grows on evidence.
#
# It is a *static library*, deliberately not linked into `bbxbox` yet: the HLE
# still names host symbols the port has not provided (measure them with
# `nm -u` over the objects and subtract what the tree defines), and a link that
# fails says nothing about the TUs above it. Each entry here is compile-verified
# by the Windows job; the link arrives when the last of them is in.
add_library(bbhost_guest STATIC
    # src/core: the pieces the boot path stands on, all of them portable except
    # the virtual-memory layer (whose AppContainer branch is the point).
    "${BBX_BBHOST_ROOT}/src/core/sha256.cpp"
    "${BBX_BBHOST_ROOT}/src/core/imports.cpp"
    "${BBX_BBHOST_ROOT}/src/core/tls_rewrite.cpp"
    "${BBX_BBHOST_ROOT}/src/core/elf.cpp"
    "${BBX_BBHOST_ROOT}/src/core/paced_file.cpp"
    "${BBX_BBHOST_ROOT}/src/core/host_fs_common.cpp"
    "${BBX_BBHOST_ROOT}/src/core/win_vm.cpp"
    "${BBX_BBHOST_ROOT}/src/core/portable.cpp"
    "${BBX_BBHOST_ROOT}/src/core/thunk.cpp"
    "${BBX_BBHOST_ROOT}/src/core/memory.cpp"
    "${BBX_BBHOST_ROOT}/src/core/futex.cpp"
    "${BBX_BBHOST_ROOT}/src/core/host_clock.cpp"
    "${BBX_BBHOST_ROOT}/src/core/image_file.cpp"
    "${BBX_BBHOST_ROOT}/src/core/sfo.cpp"
    "${BBX_BBHOST_ROOT}/src/core/config.cpp"
    "${BBX_BBHOST_ROOT}/src/core/config_move.cpp"
    "${BBX_BBHOST_ROOT}/src/core/legacy_cpu.cpp"
    "${BBX_BBHOST_ROOT}/src/core/legacy_cpu_code.cpp"
    "${BBX_BBHOST_ROOT}/src/core/write_watch.cpp"
    "${BBX_BBHOST_ROOT}/src/core/win_watch.cpp"
    # The winpthreads surface the Store SDK does not have (xbox/winpthreads_shim).
    "${CMAKE_CURRENT_SOURCE_DIR}/../xbox/winpthreads_shim/winpthreads_shim.cpp"
    # src/hle: the file system, over the platform seam.
    "${BBX_BBHOST_ROOT}/src/hle/fs.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/fios.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/save_commit.cpp"
    # src/hle: the CPU-side runtime. The renderer-facing modules (gnm, gnm_exec,
    # gx_trace, video, system, libc) reach `host/gpu.h` and `host/gxvk*.h`, which
    # carry Vulkan types not yet sprayed out of them under BBX_UWP; they join
    # this list once those headers are Vulkan-free for the console.
    "${BBX_BBHOST_ROOT}/src/hle/kernel.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/pthread.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/sync.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/sysv_va.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/guest_console.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/runtime.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/audio.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/ajm.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/avplayer.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/dialog.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/net.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/http.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/np_matching2.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/np_signaling.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/libc.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/gnm.cpp"
    # The renderer-facing HLE. These reach `host/gpu.h` and the `gxvk` headers,
    # which carry no Vulkan types; the one that did - `host/ingame_menu.h` - now
    # declares opaque handles under BBX_UWP.
    "${BBX_BBHOST_ROOT}/src/hle/gnm_exec.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/gx_trace.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/video.cpp"
    "${BBX_BBHOST_ROOT}/src/hle/system.cpp"
)
target_include_directories(bbhost_guest PUBLIC
    # winpthreads for a SDK that has no <pthread.h>; ahead of everything, and
    # only for this target (nothing else here includes a POSIX thread header).
    "${CMAKE_CURRENT_SOURCE_DIR}/../xbox/winpthreads_shim"
    "${BBX_BBHOST_ROOT}/src"
    # bbhost's `core/memory.h` resolves to the port's arena adapter by that
    # path, so the repository root is on the include path too.
    "${BBX_BBHOST_ROOT}/../.."
    # The one header upstream's build generates (`bbhost_version.h`), stamped by
    # the port; see the file.
    "${CMAKE_CURRENT_SOURCE_DIR}/../xbox/bbhost_generated")
target_compile_features(bbhost_guest PUBLIC cxx_std_20)
target_compile_definitions(bbhost_guest PUBLIC
    BBX_UWP=1
    BBHOST_HAVE_SDL3=1
    # <windows.h> defines `min`/`max` as macros unless this is set, and
    # upstream's C++ uses std::min/std::max in the same translation unit as
    # windows.h (core/elf.cpp: `expected unqualified-id`). The port's own TUs
    # are written without the macros too.
    NOMINMAX)
# LibAtrac9 for `hle/ajm.cpp` (the guest's audio decode), monocypher for the
# network modules' keys; both are upstream's own third-party targets.
target_link_libraries(bbhost_guest PUBLIC bbx_zlib bbx_atrac9 bbx_monocypher)
# `BBHOST_HAVE_SDL3=1` is set above, so `core/portable.cpp` and the audio TUs
# include SDL3's headers. Only the headers: the app links the DLL.
if(EXISTS "${BBX_SDL3_PREFIX}/include/SDL3/SDL.h")
    target_include_directories(bbhost_guest PUBLIC "${BBX_SDL3_PREFIX}/include")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Upstream's code is not /W4-clean and its warnings are not this port's to
    # fix; the port's own TUs keep /W4 /WX.
    # `/arch:AVX2`: `core/thunk.cpp`'s save/restore of the vector register file
    # uses `vmovdqu`, and every Xbox Series X|S CPU (Zen 2) has AVX2, FMA and
    # BMI2 natively - the console is not a lowest-common-denominator CPU.
    target_compile_options(bbhost_guest PRIVATE /W3 /EHsc /permissive- /arch:AVX2 -fdelayed-template-parsing
        -Wno-delayed-template-parsing-in-cxx20)
endif()

# --- the portable host layer, for the console --------------------------------
# `src/hle` reaches into its host (`src/host`) for the GPU executor, the audio
# device, the bindings, the stats and the list plumbing. Most of that layer is
# Vulkan-free already, and `host/gpu_stub.cpp` is upstream's own
# no-Vulkan-at-build-time implementation of the whole `host_gpu_*` surface - the
# single largest piece of the link. What is *not* here is desktop furniture the
# console does not want: the window/launcher (SDL3 window shell + ImGui), the
# plugin loader (`dlopen`/`LoadLibrary`), the shader compiler host, the updater
# and the Windows crash reporter.
add_library(bbhost_host STATIC
    "${BBX_BBHOST_ROOT}/src/host/gpu_stub.cpp"
    "${BBX_BBHOST_ROOT}/src/host/audio.cpp"
    "${BBX_BBHOST_ROOT}/src/host/audio_crush.cpp"
    "${BBX_BBHOST_ROOT}/src/host/bindings.cpp"
    "${BBX_BBHOST_ROOT}/src/host/cache_rw.cpp"
    "${BBX_BBHOST_ROOT}/src/host/cb_capture.cpp"
    "${BBX_BBHOST_ROOT}/src/host/cb_proof.cpp"
    "${BBX_BBHOST_ROOT}/src/host/draw_digest.cpp"
    "${BBX_BBHOST_ROOT}/src/host/foreign_hooks.cpp"
    "${BBX_BBHOST_ROOT}/src/host/frame_stats.cpp"
    "${BBX_BBHOST_ROOT}/src/host/gpu_mutex.cpp"
    "${BBX_BBHOST_ROOT}/src/host/gs_emit_gate.cpp"
    "${BBX_BBHOST_ROOT}/src/host/gxvk_arena.cpp"
    "${BBX_BBHOST_ROOT}/src/host/gxvk_buffers.cpp"
    "${BBX_BBHOST_ROOT}/src/host/gxvk_census.cpp"
    "${BBX_BBHOST_ROOT}/src/host/gxvk_heapsnap.cpp"
    "${BBX_BBHOST_ROOT}/src/host/gxvk_lists.cpp"
    "${BBX_BBHOST_ROOT}/src/host/gxvk_lists_leaf.cpp"
    # (gxvk_lists_pool.cpp is out: it reaches host/stream_ops.h, which is a
    # Vulkan type surface. Nothing on the boot path calls it.)
    "${BBX_BBHOST_ROOT}/src/host/host_lists.cpp"
    "${BBX_BBHOST_ROOT}/src/host/label_landing.cpp"
    "${BBX_BBHOST_ROOT}/src/host/list_resolve.cpp"
    "${BBX_BBHOST_ROOT}/src/host/main_wait.cpp"
    "${BBX_BBHOST_ROOT}/src/host/perf_meter.cpp"
    "${BBX_BBHOST_ROOT}/src/host/pixel_cutaway.cpp"
    # (render_hooks.cpp is out: it includes `bbhost_plugin.h`, which is the
    # public plugin SDK header and not part of the vendored source.)
    "${BBX_BBHOST_ROOT}/src/host/shader_bundles.cpp"
    # (shader_liveness.cpp is out: it includes host/gpu_internal.h, which is the
    # Vulkan device's own header.)
    "${BBX_BBHOST_ROOT}/src/host/shader_overrides.cpp"
    "${BBX_BBHOST_ROOT}/src/host/shader_patch.cpp"
    "${BBX_BBHOST_ROOT}/src/host/slow_calls.cpp"
    "${BBX_BBHOST_ROOT}/src/host/tess_lds.cpp"
    # (translation_cache.cpp is out: it includes `translator_fingerprint.h`,
    # which upstream's build generates from the translator's own hash.)
    "${BBX_BBHOST_ROOT}/src/host/vertex_cutaway.cpp"
    "${BBX_BBHOST_ROOT}/src/host/vertex_snap.cpp"
    # `hle/http.cpp` parses a replay's JSON.
    "${BBX_BBHOST_ROOT}/src/replay/json.cpp"
)
target_include_directories(bbhost_host PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../xbox/winpthreads_shim"
    "${BBX_BBHOST_ROOT}/src"
    # `host/shader_bundles.cpp` includes `bbx_self.h` by bare name.
    "${BBX_BBHOST_ROOT}/src/host/core"
    "${BBX_BBHOST_ROOT}/../.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../xbox/bbhost_generated")
if(EXISTS "${BBX_SDL3_PREFIX}/include/SDL3/SDL.h")
    target_include_directories(bbhost_host PUBLIC "${BBX_SDL3_PREFIX}/include")
endif()
target_compile_features(bbhost_host PUBLIC cxx_std_20)
target_compile_definitions(bbhost_host PUBLIC BBX_UWP=1 BBHOST_HAVE_SDL3=1 NOMINMAX)
target_link_libraries(bbhost_host PUBLIC bbx_zlib bbx_atrac9 bbx_monocypher)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(bbhost_host PRIVATE /W3 /EHsc /permissive- /arch:AVX2 -fdelayed-template-parsing
        -Wno-delayed-template-parsing-in-cxx20)
endif()

# --- the guest's own code: decomp, engine patches, the network stack ----------
# `engine` is what makes the title run at 60 fps and what the HLE's `runtime`
# calls into; `decomp` is the reimplemented game code the patches stand on;
# `net` is the P2P stack (offline: nothing here opens a socket until the guest
# asks, and the console's manifest grants no `internetClient`). Eight engine
# files are not here - four reach `host/framegen.h`/`host/upscaler.h`/
# `host/ingame_menu.h` (frame generation and the two upscalers are Vulkan
# passes) and four include `.inc` tables upstream's build generates.
add_library(bbhost_engine STATIC
    "${BBX_BBHOST_ROOT}/src/decomp/camera/follow_camera.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/chalice/ritual.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/chr/chr_sync.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/chr/replay_manipulator.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/events/flag_store.cpp"
    # (decomp/events/interpreter.cpp is out: it includes <bbhost/engine/fd4.hpp>,
    # which lives in upstream's `include/` tree and is not vendored here.)
    "${BBX_BBHOST_ROOT}/src/decomp/gnmx/cue.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/blink.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/block_reclaim.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/command_list.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/constant_engine.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/dead_packets.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/dma.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/drain_token.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/draw.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/flush_wait.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/job_barrier.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/labels.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/movie_draw.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/present.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/query_token.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/rename_wait.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/render_queue.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/resources.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/upload_heap.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/gx/water_gate.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/items/item_lots.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/memory/ez_copy.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/memory/game_memcpy.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/physics/ragdoll_drive.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/player/player_data.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/sfx/node_emit.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/sfx/node_update.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/sfx/ribbons.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/sfx/step_gate.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/sync/adaptive_mutex.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/sync/ez_work.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/talk/evaluator.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/yebis/clear.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/yebis/context.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/yebis/gnm.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/yebis/ppfx.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/yebis/ppfx_setters.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/areas.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/decomp.cpp"
    "${BBX_BBHOST_ROOT}/src/decomp/insn.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/auto_replenish.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/camera.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/change_appearance.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/chara_id.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/debug_menu.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/dream_mirror_layout.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/esd.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/event_flags.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/five_players.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/fmod_probe.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/fmod_ring.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/font_handle_set.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/frame_pool.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/frame_tick.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/guest.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/gx_resources.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/gx_state.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/image_text.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/key_prompts.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/live_resolution.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/loading.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/lua_events.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/maiden_events.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/matching_region.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/menu_close.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/menu_memory.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/menu_steps.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/mouse_camera.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/mouse_camera_stubs.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/np_test.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/paramdef.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/params.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/patch_manifest_file.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/phantom_hp.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/player_data.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/playlog.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/plugin_dialog.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/rebirth.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/rebirth_script.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/sf_heap_probe.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/sf_text_layout.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/sos_signs.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/sound_wake.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/summon_invite.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/texture_origin.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/world_chr.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/world_pause.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/yebis.cpp"
    "${BBX_BBHOST_ROOT}/src/engine/yebis_bind.cpp"
    "${BBX_BBHOST_ROOT}/src/net/account.cpp"
    "${BBX_BBHOST_ROOT}/src/net/http.cpp"
    "${BBX_BBHOST_ROOT}/src/net/nat_job.cpp"
    "${BBX_BBHOST_ROOT}/src/net/nat_probe.cpp"
    "${BBX_BBHOST_ROOT}/src/net/session.cpp"
    "${BBX_BBHOST_ROOT}/src/net/stun.cpp"
)
target_include_directories(bbhost_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../xbox/winpthreads_shim"
    "${BBX_BBHOST_ROOT}/src"
    "${BBX_BBHOST_ROOT}/../.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../xbox/bbhost_generated")
if(EXISTS "${BBX_SDL3_PREFIX}/include/SDL3/SDL.h")
    target_include_directories(bbhost_engine PUBLIC "${BBX_SDL3_PREFIX}/include")
endif()
target_compile_features(bbhost_engine PUBLIC cxx_std_20)
target_compile_definitions(bbhost_engine PUBLIC BBX_UWP=1 BBHOST_HAVE_SDL3=1 NOMINMAX)
target_link_libraries(bbhost_engine PUBLIC bbx_zlib bbx_atrac9 bbx_monocypher)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(bbhost_engine PRIVATE /W3 /EHsc /permissive- /arch:AVX2 -fdelayed-template-parsing
        -Wno-delayed-template-parsing-in-cxx20)
endif()
