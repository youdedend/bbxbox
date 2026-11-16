#!/usr/bin/env bash
# Build and run the portable half of the port with whatever C++20 compiler is
# present, without CMake. This is what CI and a developer without Visual Studio
# use; it is deliberately the same source list as CMakeLists.txt.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx="${CXX:-g++}"
# One invocation compiles and links every test unit, so caching only pays on a
# run that rebuilt something; when ccache is installed it costs nothing to ask.
cxx_cmd=("$cxx")
if [ -z "${CXX:-}" ] && command -v ccache >/dev/null 2>&1; then
  cxx_cmd=(ccache g++)
fi
out="${1:-$root/build/bbx_tests}"

sources=(
  "$root"/src/host/d3d12/bbx_format.cpp
  "$root"/src/host/d3d12/bbx_barriers.cpp
  "$root"/src/host/d3d12/bbx_budget.cpp
  "$root"/src/host/d3d12/bbx_cache.cpp
  "$root"/src/host/d3d12/bbx_shader_cache.cpp
  "$root"/src/host/d3d12/bbx_quarantine.cpp
  "$root"/src/host/d3d12/bbx_precompile.cpp
  "$root"/src/host/d3d12/bbx_descriptors.cpp
  "$root"/src/host/d3d12/bbx_present.cpp
  "$root"/src/host/d3d12/bbx_stream.cpp
  "$root"/src/host/d3d12/bbx_untile.cpp
  "$root"/xbox/platform/dump.cpp
  "$root"/xbox/platform/ini_file.cpp
  "$root"/xbox/platform/launcher.cpp
  "$root"/xbox/platform/ux.cpp
  "$root"/xbox/platform/display.cpp
  "$root"/xbox/platform/hud.cpp
  "$root"/src/host/d3d12/bbx_psocache.cpp
  "$root"/src/host/d3d12/bbx_tess.cpp
  "$root"/src/host/core/bbx_self.cpp
  "$root"/tests/main.cpp
  "$root"/tests/format_test.cpp
  "$root"/tests/budget_test.cpp
  "$root"/tests/cache_test.cpp
  "$root"/tests/quarantine_test.cpp
  "$root"/tests/descriptors_test.cpp
  "$root"/tests/present_test.cpp
  "$root"/tests/stream_test.cpp
  "$root"/tests/untile_test.cpp
  "$root"/tests/precompile_test.cpp
  "$root"/tests/barriers_test.cpp
  "$root"/tests/sfo_test.cpp
  "$root"/tests/dump_test.cpp
  "$root"/tests/ini_test.cpp
  "$root"/tests/launcher_test.cpp
  "$root"/tests/overlay_test.cpp
  "$root"/tests/swap_test.cpp
  "$root"/tests/guest_memory_test.cpp
  "$root"/tests/ux_test.cpp
  "$root"/tests/caps_test.cpp
  "$root"/tests/psocache_test.cpp
  "$root"/tests/tess_test.cpp
  "$root"/tests/display_test.cpp
  "$root"/tests/hud_test.cpp
  "$root"/tests/self_test.cpp
  "$root"/tests/host_fs_test.cpp
  "$root"/tests/hostfs_tree_test.cpp
  "$root"/tests/save_commit_test.cpp
  "$root"/tests/shaders_test.cpp
  # The printer, which shaders_test.cpp checks the console's dialect of.
  "$root"/upstream/bbhost/src/gcn/hlsl.cpp
  "$root"/tests/guest_core_test.cpp
  "$root"/tests/guest_core_stubs.cpp
  # The vendored guest core, compiled as the console compiles it (minus the UWP
  # memory adapter): the tests in guest_core_test.cpp run the real loader pieces,
  # not copies of them.
  "$root"/upstream/bbhost/src/core/sha256.cpp
  "$root"/upstream/bbhost/src/core/imports.cpp
  "$root"/upstream/bbhost/src/core/tls_rewrite.cpp
  "$root"/upstream/bbhost/src/core/elf.cpp
  "$root"/upstream/bbhost/src/core/paced_file.cpp
  "$root"/upstream/bbhost/src/core/portable.cpp
  "$root"/upstream/bbhost/src/core/memory.cpp
  # The host file-system seam the HLE gets its file system from: the shared half
  # and the desktop table (the console links host_fs_uwp.cpp instead).
  "$root"/upstream/bbhost/src/core/host_fs_common.cpp
  "$root"/upstream/bbhost/src/core/host_fs_desktop.cpp
  # The save commit protocol, over that seam: the moves, the listing and the
  # flush are the seam's now, so its test runs here.
  "$root"/upstream/bbhost/src/hle/save_commit.cpp
)

mkdir -p "$(dirname "$out")"
objdir="$(mktemp -d)"
trap 'rm -rf "$objdir"' EXIT

# The vendored zlib is C, and this script builds the port's own code with -Werror:
# compiling zlib as C++ (which g++ would do with a .c argument) is neither what the
# console does nor warning-clean, so it is compiled as C, quietly, into an archive
# the link picks up. Only what inflating a .shaderbnd.dcx or an fSELF segment needs.
cc="${CC:-cc}"
zlib="$root/upstream/third_party/zlib"
zlib_objs=()
# compress.c/trees.c/deflate.c are only needed by the tests, which build their own
# compressed fixtures; the app only ever inflates.
for unit in adler32 crc32 inflate inffast inftrees uncompr zutil compress trees deflate; do
  "$cc" -std=c99 -O2 -w -I "$zlib" -c "$zlib/$unit.c" -o "$objdir/$unit.o"
  zlib_objs+=("$objdir/$unit.o")
done
ar rcs "$objdir/libbbx_zlib.a" "${zlib_objs[@]}"

"${cxx_cmd[@]}" -std=c++20 -O2 -Wall -Wextra -Werror \
  -I "$root/src" -I "$root/tests" -I "$root" -I "$root/upstream/bbhost/src" \
  -I "$zlib" -DBBX_HAVE_ZLIB=1 \
  -o "$out" "${sources[@]}" "$objdir/libbbx_zlib.a"
"$out"
