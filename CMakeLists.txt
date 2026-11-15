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

# --- the portable core -------------------------------------------------------
add_library(bbx_d3d12_core STATIC
    src/host/d3d12/bbx_format.cpp
    src/host/d3d12/bbx_barriers.cpp
    src/host/d3d12/bbx_budget.cpp
    src/host/d3d12/bbx_cache.cpp
    src/host/d3d12/bbx_quarantine.cpp
    src/host/d3d12/bbx_precompile.cpp
    src/host/d3d12/bbx_descriptors.cpp
    src/host/d3d12/bbx_present.cpp
    src/host/d3d12/bbx_stream.cpp
    src/host/d3d12/bbx_untile.cpp
)
target_include_directories(bbx_d3d12_core PUBLIC src)
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
        tests/precompile_test.cpp
        tests/barriers_test.cpp
        tests/sfo_test.cpp
        tests/dump_test.cpp
        tests/ini_test.cpp
        tests/launcher_test.cpp
        tests/overlay_test.cpp
        tests/swap_test.cpp
        tests/guest_memory_test.cpp
        tests/ux_test.cpp
    )
    target_link_libraries(bbx_tests PRIVATE bbx_d3d12_core bbx_platform_core)
    target_include_directories(bbx_tests PRIVATE tests "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME bbx_unit COMMAND bbx_tests)
endif()

if(NOT BBX_HOST_ONLY AND WIN32)
    add_subdirectory(xbox)
endif()

message(STATUS "bbxbox: host-only=${BBX_HOST_ONLY} bbhost=${BBX_BBHOST_DIR}")
