#!/usr/bin/env bash
# Build tools/bbx_bake_cache.cpp: the console shader cache's bake.
#
# Not part of the game: the bake runs on a desktop, over a dump, with a DXC the
# console build neither has nor may link. So this script is deliberately the
# same shape as tools/build_host.sh - whatever C++20 compiler is present, the
# vendored zlib compiled as C, no CMake - and only the four units the bake needs
# from the port.
#
#   tools/build_bake_cache.sh            -> build/bbx_bake_cache
#   tools/build_bake_cache.sh --run ...  -> build and run it with the arguments
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx="${CXX:-g++}"
out="$root/build/bbx_bake_cache"

run=0
if [ "${1:-}" = "--run" ]; then
  run=1
  shift
fi

mkdir -p "$(dirname "$out")"
objdir="$(mktemp -d)"
trap 'rm -rf "$objdir"' EXIT

# The vendored zlib, as build_host.sh compiles it: C, quietly, into an archive.
cc="${CC:-cc}"
zlib="$root/upstream/third_party/zlib"
objs=()
for unit in adler32 crc32 inflate inffast inftrees uncompr zutil compress trees deflate; do
  "$cc" -std=c99 -O2 -w -I "$zlib" -c "$zlib/$unit.c" -o "$objdir/$unit.o"
  objs+=("$objdir/$unit.o")
done
ar rcs "$objdir/libbbx_zlib.a" "${objs[@]}"

includes=(-I "$root/src" -I "$root" -I "$root/upstream/bbhost/src" -I "$zlib")

# The tool and the port's own units are warning-clean under -Werror; bbhost's
# translator is built by upstream's own CMake with its own flags, so it is
# compiled quietly rather than edited here.
objects=()
quiet=(
  "$root/upstream/bbhost/src/gcn/container.cpp" \
  "$root/upstream/bbhost/src/gcn/decode.cpp" \
  "$root/upstream/bbhost/src/gcn/half.cpp" \
  "$root/upstream/bbhost/src/gcn/hlsl.cpp" \
  "$root/upstream/bbhost/src/gcn/image_dims.cpp" \
  "$root/upstream/bbhost/src/gcn/lift.cpp" \
  "$root/upstream/bbhost/src/gcn/names.cpp" \
  "$root/upstream/bbhost/src/gcn/spirv.cpp" \
  "$root/upstream/bbhost/src/gcn/translate.cpp" \
  "$root/upstream/bbhost/src/gcn/wave.cpp" \
  "$root/upstream/bbhost/src/host/shader_patch.cpp"
)
for unit in "${quiet[@]}"; do
  obj="$objdir/$(basename "${unit%.cpp}").o"
  "$cxx" -std=c++20 -O2 -w -pthread "${includes[@]}" -c "$unit" -o "$obj"
  objects+=("$obj")
done
"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror -pthread "${includes[@]}" \
  -o "$out" \
  "$root/tools/bbx_bake_cache.cpp" \
  "$root/src/host/core/bbx_self.cpp" \
  "$root/src/host/d3d12/bbx_cache.cpp" \
  "$root/src/host/d3d12/bbx_shader_cache.cpp" \
  "${objects[@]}" "$objdir/libbbx_zlib.a" -ldl

if [ "$run" = 1 ]; then
  "$out" "$@"
else
  echo "built $out"
fi
