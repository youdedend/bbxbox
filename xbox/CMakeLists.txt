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

set(BBX_DXC_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../build-deps/prefix/dxc" CACHE PATH
    "DXC from xbox/scripts/build-deps.ps1; used at build time only, never shipped")

# --- shaders: DXIL at build time, zero compiler at run time -------------------
# The console app cannot carry a shader compiler: dxcompiler.dll imports
# CreateFileW / LoadLibraryA / MapViewOfFile, which the AppContainer refuses, so
# LoadPackagedLibrary on it is a coin flip. Every fixed pass is therefore compiled
# here - at the shader model the console supports, with -WX - and embedded as
# bytes. A shader a console cannot run cannot be built into the package, and a
# broken shader fails CI instead of failing on the TV.
set(BBX_DXC "${BBX_DXC_PREFIX}/dxc.exe")
if(NOT EXISTS "${BBX_DXC}")
    message(FATAL_ERROR
        "dxc.exe is not in ${BBX_DXC_PREFIX}.\n"
        "Run: pwsh xbox/scripts/build-deps.ps1 (it downloads DXC v1.9.2609 into build-deps/prefix/dxc).")
endif()

set(BBX_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${BBX_SHADER_DIR}")
find_program(BBX_PWSH NAMES pwsh powershell)
if(NOT BBX_PWSH)
    message(FATAL_ERROR "pwsh or powershell is required to run dxc (see xbox/scripts/dxc-run.ps1)")
endif()
set_property(GLOBAL PROPERTY BBX_SHADER_OUTPUTS "")
set_property(GLOBAL PROPERTY BBX_SHADER_PAIRS "")

function(bbx_shader symbol entry profile source)
    set(out "${BBX_SHADER_DIR}/${symbol}.dxil")
    add_custom_command(OUTPUT "${out}"
        COMMAND "${BBX_PWSH}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/dxc-run.ps1"
                -Dxc "${BBX_DXC}" -Out "${out}"
                -Profile "${profile}" -Entry "${entry}"
                -Source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/dxc-run.ps1"
        COMMENT "dxc: ${source} -E ${entry} -T ${profile} -WX"
        VERBATIM)
    # No -enable-16bit-types: the console has no native 16-bit ALU (guard: SM 6.2
    # 16-bit types are lowered, and DXC's own lowering is what adds the helper
    # lane work). No ResourceDescriptorHeap: SM 6.6 is not there.
    get_property(outputs GLOBAL PROPERTY BBX_SHADER_OUTPUTS)
    set_property(GLOBAL PROPERTY BBX_SHADER_OUTPUTS ${outputs} "${out}")
    get_property(pairs GLOBAL PROPERTY BBX_SHADER_PAIRS)
    set_property(GLOBAL PROPERTY BBX_SHADER_PAIRS "${pairs}" "${symbol}=${out}")
endfunction()

# Keep this list in step with bbx_shaders.h's Manifest(); tools/local_precheck.sh
# compares the two.
bbx_shader(PresentVs PresentVs vs_6_4 present.hlsl)
bbx_shader(PresentPs PresentPs ps_6_4 present.hlsl)
bbx_shader(OverlayVs OverlayVs vs_6_4 overlay.hlsl)
bbx_shader(OverlayPs OverlayPs ps_6_4 overlay.hlsl)
bbx_shader(UntileCs UntileCs cs_6_4 untile.hlsl)
bbx_shader(SelftestCs SelftestCs cs_6_4 stream_selftest.hlsl)

get_property(BBX_SHADER_OUTPUTS GLOBAL PROPERTY BBX_SHADER_OUTPUTS)
get_property(BBX_SHADER_PAIRS GLOBAL PROPERTY BBX_SHADER_PAIRS)
set(BBX_SHADER_HEADER "${BBX_SHADER_DIR}/bbx_shaders_generated.h")
# The symbol/file pairs go through a file, not -DPAIRS=a;b: a semicolon-separated
# value in a -D argument is a CMake list, and a list of Windows paths that a
# generator may or may not quote is a bug waiting for a runner. A file has one
# meaning everywhere.
set(BBX_SHADER_PAIRS_FILE "${BBX_SHADER_DIR}/pairs.txt")
file(WRITE "${BBX_SHADER_PAIRS_FILE}" "")
foreach(pair IN LISTS BBX_SHADER_PAIRS)
    if(pair STREQUAL "")
        continue()
    endif()
    file(APPEND "${BBX_SHADER_PAIRS_FILE}" "${pair}\n")
endforeach()
add_custom_command(OUTPUT "${BBX_SHADER_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DOUT=${BBX_SHADER_HEADER}"
            "-DPAIRS_FILE=${BBX_SHADER_PAIRS_FILE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/embed_shaders.cmake"
    DEPENDS ${BBX_SHADER_OUTPUTS}
            "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/embed_shaders.cmake"
    COMMENT "embedding DXIL into bbx_shaders_generated.h"
    VERBATIM)
add_custom_target(bbx_shaders ALL DEPENDS "${BBX_SHADER_HEADER}")

# bbhost: vendored at upstream/bbhost, pinned, and compiled here (see
# cmake/bbhost.cmake). The guest core and the host stream hooks join as P1 lands.
option(BBX_BBHOST "compile the vendored bbhost sources" ON)
if(BBX_BBHOST)
    # The guest runtime, its host layer and the game's own code are compiled and
    # ready to run - but the link is behind this switch until the import audit
    # passes on the result.
    #
    # The archives are linked (that is what `BBX_GUEST_LINK=ON` does) and
    # `main.cpp` then boots the guest (platform/bbhost_bridge.cpp). What still
    # fails is the audit: the guest's own translation units use
    # `std::filesystem`, and MSVC's implementation of it imports CreateFile2,
    # FindFirstFileW/ExW, CreateDirectoryW and MoveFileExW directly - the app's
    # `bbx::hostfs` seam is bypassed inside the STL. Until those call sites are
    # routed through the seam, the switch stays off: a green branch says more
    # than a linked one.
    #
    # `core/portable.cpp` and `hle/kernel.cpp` map the guest's memory files with
    # `MapViewOfFile` (the desktop name); the AppContainer's
    # `MapViewOfFileFromApp` is the same call, and both files' BBX_UWP branches
    # use it.
    #
    # lld-link resolves archives regardless of order, so these may name each
    # other's symbols freely - and it dead-strips an archive member no one
    # references, so `platform/bbhost_bridge.cpp` is what pulls the runtime in.
    option(BBX_GUEST_LINK "link the guest runtime into the app and boot it" OFF)
    if(BBX_GUEST_LINK)
        target_link_libraries(bbxbox PRIVATE bbhost_gcn bbhost_guest bbhost_host bbhost_engine
            bbx_atrac9 bbx_monocypher)
        target_compile_definitions(bbxbox PRIVATE BBX_GUEST_BOOT=1)
    else()
        target_link_libraries(bbxbox PRIVATE bbhost_gcn bbx_atrac9 bbx_monocypher)
    endif()
endif()

# The Store CRT. A console has vcruntime140_app.dll / msvcp140_app.dll and their
# siblings, not the desktop vcruntime140.dll: the *_app import libraries have to
# be named explicitly, and the desktop vcruntime140_1.lib (which supplies
# __CxxFrameHandler4) has to be refused, or the link picks it and the package
# fails import validation on the console.
set(BBX_STORE_CRT_DIR "" CACHE PATH "the MSVC lib/x64/store directory (the Store CRT)")
if(BBX_STORE_CRT_DIR AND EXISTS "${BBX_STORE_CRT_DIR}")
    # The directory goes first on the search path, so the store CRT wins over the
    # desktop one for every library name the CRT itself requests.
    target_link_directories(bbxbox BEFORE PRIVATE "${BBX_STORE_CRT_DIR}")
    target_compile_definitions(bbxbox PRIVATE _CRT_STDIO_ARBITRARY_WIDE_SPECIFIERS)
    # Toolchains differ: some name the store import libraries *_app.lib, others
    # keep the plain names and only the DLL they point at changes. Link the *_app
    # names when they exist, and refuse the desktop vcruntime140_1.lib, which
    # supplies __CxxFrameHandler4 but imports VCRUNTIME140_1.dll.
    foreach(name vcruntime140_app msvcp140_app vcruntime140_1_app)
        find_library(BBX_${name} ${name} PATHS "${BBX_STORE_CRT_DIR}" NO_DEFAULT_PATH)
    endforeach()
    if(BBX_vcruntime140_app AND BBX_msvcp140_app AND BBX_vcruntime140_1_app)
        target_link_libraries(bbxbox PRIVATE vcruntime140_app msvcp140_app vcruntime140_1_app)
        target_link_options(bbxbox PRIVATE /NODEFAULTLIB:vcruntime140_1.lib)
        message(STATUS "store CRT: linking the *_app import libraries")
    else()
        message(STATUS "store CRT: only the plain import-library names exist")
    endif()
else()
    message(WARNING "BBX_STORE_CRT_DIR is not set; the link will import the desktop CRT")
endif()

target_link_libraries(bbxbox PRIVATE
    "${BBX_SDL3_PREFIX}/lib/SDL3.lib"
    WindowsApp
    d3d12
    dxgi
    dxguid
    # No xaudio2: audio is SDL3's (which links XAudio2 itself). Importing it here
    # also asks for xaudio2_9.lib, which the SDK ships only for desktop targets.
)

# Debugging: the .map is what xbox/scripts/symbolicate.py resolves a console
# crash's module+RVA against.
# The EntryPoint export: see xbox/bbxbox.def and WINRT_GetActivationFactory.
if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_link_options(bbxbox PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/bbxbox.def")
endif()

# Bare /MAP: lld-link rejects the quoted CMake /MAP:"path" form ("cannot open
# ...: invalid argument"). Every driver here is clang-cl -> lld-link, and lld
# writes <image>.map beside the image, i.e. build/xbox/bbxbox.map, which is what
# package.ps1 and symbolicate.py expect.
target_link_options(bbxbox PRIVATE /MAP)
# Every unresolved symbol in one run, not the first twenty: the CI annotation is
# the port's only view of the link census (the job log is not reachable from a
# sandbox), and a link that names one symbol at a time costs a run each.
target_link_options(bbxbox PRIVATE /ERRORLIMIT:0)
if(MSVC)
    target_link_options(bbxbox PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# The manifest, assets and DLLs are laid out by the packaging script; this keeps
# a plain build runnable for `make install`-style local checks.
install(TARGETS bbxbox RUNTIME DESTINATION .)
