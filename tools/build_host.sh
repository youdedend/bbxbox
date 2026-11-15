#!/usr/bin/env bash
# Build and run the portable half of the port with whatever C++20 compiler is
# present, without CMake. This is what CI and a developer without Visual Studio
# use; it is deliberately the same source list as CMakeLists.txt.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx="${CXX:-g++}"
out="${1:-$root/build/bbx_tests}"

sources=(
  "$root"/src/host/d3d12/bbx_format.cpp
  "$root"/src/host/d3d12/bbx_barriers.cpp
  "$root"/src/host/d3d12/bbx_budget.cpp
  "$root"/src/host/d3d12/bbx_cache.cpp
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
  "$root"/tests/shaders_test.cpp
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
)

mkdir -p "$(dirname "$out")"
"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror \
  -I "$root/src" -I "$root/tests" -I "$root" -I "$root/upstream/bbhost/src" \
  -o "$out" "${sources[@]}"
"$out"
