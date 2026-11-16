cmake_minimum_required(VERSION 3.24)
project(bbxbox LANGUAGES C CXX)

# bbxbox is an overlay on a pinned bbhost checkout (see UPSTREAM.md). This
# top-level file builds either
#   * the portable half of the port -- the D3D12 backend's policy, cache,
#     memory and stream code -- plus its unit tests, with any C++20 compiler,
#     on any OS (BBX_HOST_ONLY=ON, the default when no bbhost tree is given);
#   * the whole UWP application (xbox/), which needs clang-cl and the Windows
#     SDK, by adding xbox/ as a subdirectory and pulling bbhost's sources in.
#
# Keeping the two modes in one project is deliberate: the tests that pin the
# budget, the cache format, the quarantine protocol and the barrier table are
# the same tests on the build machine and on the console's CI.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BBX_HOST_ONLY "build only the portable core and its tests" OFF)
set(BBX_BBHOST_DIR "" CACHE PATH "a bbhost checkout at the commit in UPSTREAM.md")

if(BBX_BBHOST_DIR AND NOT BBX_HOST_ONLY)
    if(NOT EXISTS "${BBX_BBHOST_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "BBX_BBHOST_DIR does not look like bbhost: ${BBX_BBHOST_DIR}")
    endif()
    enable_language(OBJC)  # not used; fails early on unsupported hosts
endif()

# --- zlib (vendored) ---------------------------------------------------------
# An fSELF segment may be zlib-compressed, and bbhost's container.cpp inflates
# .shaderbnd.dcx. Defined here (not only in the console's bbhost.cmake) so the host
# tests can exercise the compressed path with the same library the console links.
if(NOT TARGET bbx_zlib AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/third_party/zlib/inflate.c")
    set(BBX_ZLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/upstream/third_party/zlib")
    add_library(bbx_zlib STATIC
        "${BBX_ZLIB_ROOT}/adler32.c" "${BBX_ZLIB_ROOT}/compress.c"
        "${BBX_ZLIB_ROOT}/crc32.c" "${BBX_ZLIB_ROOT}/deflate.c"
        "${BBX_ZLIB_ROOT}/gzclose.c" "${BBX_ZLIB_ROOT}/gzlib.c"
        "${BBX_ZLIB_ROOT}/gzread.c" "${BBX_ZLIB_ROOT}/gzwrite.c"
        "${BBX_ZLIB_ROOT}/infback.c" "${BBX_ZLIB_ROOT}/inffast.c"
        "${BBX_ZLIB_ROOT}/inflate.c" "${BBX_ZLIB_ROOT}/inftrees.c"
        "${BBX_ZLIB_ROOT}/trees.c" "${BBX_ZLIB_ROOT}/uncompr.c"
        "${BBX_ZLIB_ROOT}/zutil.c"
    )
    target_include_directories(bbx_zlib PUBLIC "${BBX_ZLIB_ROOT}")
    set_target_properties(bbx_zlib PROPERTIES C_STANDARD 99)
    if(MSVC)
        target_compile_options(bbx_zlib PRIVATE /W0 /D_CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(bbx_zlib PRIVATE -w)
    endif()
endif()

# --- the portable core -------------------------------------------------------
add_library(bbx_d3d12_core STATIC
    src/host/d3d12/bbx_format.cpp
    src/host/d3d12/bbx_barriers.cpp
    src/host/d3d12/bbx_budget.cpp
    src/host/d3d12/bbx_cache.cpp
    src/host/d3d12/bbx_shader_cache.cpp
    src/host/d3d12/bbx_quarantine.cpp
    src/host/d3d12/bbx_precompile.cpp
    src/host/d3d12/bbx_descriptors.cpp
    src/host/d3d12/bbx_present.cpp
    src/host/d3d12/bbx_stream.cpp
    src/host/d3d12/bbx_untile.cpp
    src/host/d3d12/bbx_scanout.cpp
    src/host/d3d12/bbx_guest_mem.cpp
    src/host/d3d12/bbx_guest_vsharp.cpp
    src/host/d3d12/bbx_guest_path.cpp
    src/host/d3d12/bbx_path_table.cpp
    src/host/d3d12/bbx_guest_binding.cpp
    src/host/d3d12/bbx_guest_draw.cpp
    src/host/d3d12/bbx_guest_root.cpp
    src/host/d3d12/bbx_psocache.cpp
    src/host/d3d12/bbx_tess.cpp
    src/host/core/bbx_self.cpp
    src/host/core/bbx_fault_filter.cpp
)
target_include_directories(bbx_d3d12_core PUBLIC src "${CMAKE_CURRENT_SOURCE_DIR}")
if(TARGET bbx_zlib)
    target_link_libraries(bbx_d3d12_core PUBLIC bbx_zlib)
    target_compile_definitions(bbx_d3d12_core PUBLIC BBX_HAVE_ZLIB=1)
endif()
target_compile_features(bbx_d3d12_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(bbx_d3d12_core PRIVATE /W4 /WX)
else()
    target_compile_options(bbx_d3d12_core PRIVATE -Wall -Wextra -Werror)
endif()

# The pure half of the console platform layer: dump discovery, the ini file and
# the launcher model. No WinRT, so CI runs these too.
add_library(bbx_platform_core STATIC
    xbox/platform/dump.cpp
    xbox/platform/ini_file.cpp
    xbox/platform/launcher.cpp
    xbox/platform/ux.cpp
    xbox/platform/display.cpp
    xbox/platform/hud.cpp
)
target_include_directories(bbx_platform_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(bbx_platform_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(bbx_platform_core PRIVATE /W4 /WX)
else()
    target_compile_options(bbx_platform_core PRIVATE -Wall -Wextra -Werror)
endif()

# The console-only backend sources are not part of this library: they need
# d3d12.h, and their correctness is checked by tools/syntax_check.sh on a
# Windows runner. Listing them here keeps them in the tree's build graph.
set(BBX_CONSOLE_SOURCES
    src/host/d3d12/device.cpp
    src/host/d3d12/swapchain.cpp
    src/host/d3d12/descriptors_d3d12.cpp
    src/host/d3d12/rootsig.cpp
    src/host/d3d12/pipeline.cpp
    src/host/d3d12/host_gpu_d3d12.cpp
    src/host/d3d12/replay.cpp
    src/host/d3d12/labels.cpp
    src/host/d3d12/dxil.cpp
    src/host/d3d12/untile_d3d12.cpp
    src/host/d3d12/precompile_runner.cpp
    src/host/d3d12/backend.cpp
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/host/d3d12/device.cpp")
    add_custom_target(bbx_console_sources SOURCES ${BBX_CONSOLE_SOURCES})
endif()

# --- tests -------------------------------------------------------------------
option(BBX_BUILD_TESTS "build the host-side unit tests" ON)
if(BBX_BUILD_TESTS)
    enable_testing()
    add_executable(bbx_tests
        tests/main.cpp
        tests/format_test.cpp
        tests/budget_test.cpp
        tests/cache_test.cpp
        tests/quarantine_test.cpp
        tests/descriptors_test.cpp
        tests/present_test.cpp
        tests/stream_test.cpp
        tests/untile_test.cpp
        tests/scanout_test.cpp
        tests/guest_mem_test.cpp
        tests/guest_vsharp_test.cpp
        tests/guest_path_test.cpp
        tests/path_table_test.cpp
        tests/guest_binding_test.cpp
        tests/guest_draw_test.cpp
        tests/fault_filter_test.cpp
        tests/precompile_test.cpp
        tests/barriers_test.cpp
        tests/sfo_test.cpp
        tests/dump_test.cpp
        tests/ini_test.cpp
        tests/launcher_test.cpp
        tests/storage_test.cpp
        tests/overlay_test.cpp
        tests/swap_test.cpp
        tests/guest_memory_test.cpp
        tests/ux_test.cpp
        tests/caps_test.cpp
        tests/psocache_test.cpp
        tests/tess_test.cpp
        tests/display_test.cpp
        tests/hud_test.cpp
        tests/self_test.cpp
        tests/host_fs_test.cpp
        tests/hostfs_tree_test.cpp
        tests/save_commit_test.cpp
        tests/shaders_test.cpp
        tests/guest_core_test.cpp
        tests/guest_core_stubs.cpp
    )
    # The vendored guest core, as tests/guest_core_test.cpp exercises it: the real
    # SHA-256, NID table, TLS rewrite and paced file, not copies of them.
    # tools/build_host.sh builds the same list for CI; a compiler that cannot build
    # bbhost's core on the host leaves the five cases out rather than failing.
    # The host file-system seam is portable on both: the shared half plus the
    # desktop table (the console links xbox/platform/host_fs_uwp.cpp instead).
    target_sources(bbx_tests PRIVATE
        upstream/bbhost/src/core/host_fs_common.cpp
        upstream/bbhost/src/core/host_fs_desktop.cpp
        upstream/bbhost/src/hle/save_commit.cpp
        # The printer, whose console dialect tests/shaders_test.cpp checks: no
        # [[vk::...]], no 16-bit types, no Shader Model 6.6 - a property of the
        # text, and so checkable without a copy of dxc in the build.
        upstream/bbhost/src/gcn/hlsl.cpp)
    target_include_directories(bbx_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/upstream/bbhost/src")
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        set(BBX_GUEST_CORE_TEST_SOURCES
            upstream/bbhost/src/core/sha256.cpp
            upstream/bbhost/src/core/imports.cpp
            upstream/bbhost/src/core/tls_rewrite.cpp
            upstream/bbhost/src/core/elf.cpp
            upstream/bbhost/src/core/paced_file.cpp
            upstream/bbhost/src/core/portable.cpp
        )
        target_sources(bbx_tests PRIVATE ${BBX_GUEST_CORE_TEST_SOURCES})
        target_include_directories(bbx_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/upstream/bbhost/src")
        target_link_libraries(bbx_tests PRIVATE Threads::Threads)
    endif()
    target_link_libraries(bbx_tests PRIVATE bbx_d3d12_core bbx_platform_core)
    target_include_directories(bbx_tests PRIVATE tests "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME bbx_unit COMMAND bbx_tests)
endif()

if(NOT BBX_HOST_ONLY AND WIN32)
    add_subdirectory(xbox)
endif()

message(STATUS "bbxbox: host-only=${BBX_HOST_ONLY} bbhost=${BBX_BBHOST_DIR}")
