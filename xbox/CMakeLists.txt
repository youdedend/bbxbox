# SPDX-License-Identifier: GPL-3.0-or-later
# The console application: x64, WindowsStore, clang-cl, Game mode.
#
# Two rules shape this file.
#
# 1. The toolchain is clang-cl, because bbhost calls guest code through
#    __attribute__((sysv_abi)) function pointers and MSVC has no equivalent. The
#    SDL3-uwp dependency is the one thing built with MSVC (/ZW), once, by
#    xbox/scripts/build-deps.ps1: its C API is the boundary, so the two
#    toolchains meet at a C ABI, which they can.
# 2. /STACK:8388608,65536. Test-project committed 16 MiB per OS thread
#    (/STACK:16777216,16777216) and 82 threads cost ~1.28 GiB of the 5,120 MiB
#    before the game ran. 8 MiB with a 64 KiB commit granularity is the lesson.

cmake_minimum_required(VERSION 3.28)
project(bbxbox LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    message(FATAL_ERROR "xbox/ must be configured with -DCMAKE_SYSTEM_NAME=WindowsStore")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "bbxbox is x64 only")
endif()

set(BBX_SDL3_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../build-deps/prefix/sdl3" CACHE PATH
    "SDL3-uwp prefix produced by xbox/scripts/build-deps.ps1")
set(BBX_LIBUWP_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../build-deps/prefix/libuwp" CACHE PATH
    "libuwp prefix produced by xbox/scripts/build-deps.ps1")

if(NOT EXISTS "${BBX_SDL3_PREFIX}/include/SDL3/SDL.h")
    message(FATAL_ERROR
        "SDL3-uwp is not in ${BBX_SDL3_PREFIX}.\n"
        "Run: pwsh xbox/scripts/build-deps.ps1 (it builds SDL3-uwp once, into build-deps/prefix).")
endif()

# bbhost: vendored at upstream/bbhost, pinned, and compiled here (see
# cmake/bbhost.cmake). The guest core and the host stream hooks join as P1 lands.
option(BBX_BBHOST "compile the vendored bbhost sources" ON)
if(BBX_BBHOST)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bbhost.cmake")
endif()

set(BBX_APP_SOURCES
    platform/main.cpp
    platform/log.cpp
    platform/storage.cpp
    platform/guest_memory.cpp
    platform/memory_selftest.cpp
    platform/dump.cpp
    platform/ini_file.cpp
    platform/launcher.cpp
    platform/ux.cpp
)

add_executable(bbxbox WIN32 ${BBX_APP_SOURCES})
set_target_properties(bbxbox PROPERTIES OUTPUT_NAME "bbxbox")

target_include_directories(bbxbox PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../upstream/bbhost/src"
    "${BBX_SDL3_PREFIX}/include"
)

# The portable core and the platform model are compiled into the app; the D3D12
# backend objects join from P2 on.
target_sources(bbxbox PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/host/d3d12/bbx_format.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/host/d3d12/bbx_barriers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/host/d3d12/bbx_budget.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/host/d3d12/bbx_cache.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/host/d3d12/bbx_quarantine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/host/d3d12/bbx_precompile.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/host/d3d12/bbx_descriptors.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/host/d3d12/bbx_present.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/host/d3d12/bbx_stream.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/host/d3d12/bbx_untile.cpp"
)

target_compile_features(bbxbox PRIVATE cxx_std_20)
target_compile_definitions(bbxbox PRIVATE
    BBX_UWP=1
    WINAPI_FAMILY=WINAPI_FAMILY_APP
    _WIN32_WINNT=0x10
    SDL_MAIN_HANDLED=1
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # System V ABI for guest calls, the lesson that forces clang-cl at all.
    # /WX only on the port's own TUs: upstream's code is built with /W3 and its
    # warnings are not this port's to fix.
    target_compile_options(bbxbox PRIVATE /EHsc /W4 /WX /permissive- /Zc:__cplusplus)
    target_link_options(bbxbox PRIVATE
        /STACK:8388608,65536
        /APPCONTAINER
        /HIGHENTROPYVA:NO
        /OPT:REF /OPT:ICF)
else()
    message(WARNING "Building the console app with ${CMAKE_CXX_COMPILER_ID}: guest thunks need clang-cl")
    target_link_options(bbxbox PRIVATE /STACK:8388608,65536)
endif()

if(BBX_BBHOST)
    target_link_libraries(bbxbox PRIVATE bbhost_gcn bbx_atrac9 bbx_monocypher)
endif()

target_link_libraries(bbxbox PRIVATE
    "${BBX_SDL3_PREFIX}/lib/SDL3.lib"
    WindowsApp
    d3d12
    dxgi
    dxguid
    xaudio2_9
)

# Debugging: the .map is what xbox/scripts/symbolicate.py resolves a console
# crash's module+RVA against.
target_link_options(bbxbox PRIVATE /MAP:"${CMAKE_CURRENT_BINARY_DIR}/bbxbox.map")
if(MSVC)
    target_link_options(bbxbox PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# The manifest, assets and DLLs are laid out by the packaging script; this keeps
# a plain build runnable for `make install`-style local checks.
install(TARGETS bbxbox RUNTIME DESTINATION .)
