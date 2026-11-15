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
