#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# What has to pass before every push. Two minutes, no Windows SDK:
#
#   1. the portable core and the platform model build and their tests pass;
#   2. the AppContainer API rules hold in the sources (no bare CreateFileW,
#      VirtualAlloc, LoadLibraryW; no PAGE_EXECUTE_READWRITE anywhere);
#   3. the stack fix is still in place (/STACK:8388608,65536; never 16777216);
#   4. no game files, no generated caches, no absolute developer paths are
#      tracked by git;
#   5. the docs still agree with the code on the numbers that matter.
#
# CI runs the same script, so "it passed locally" means something.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
status=0
fail() { echo "precheck: FAIL: $*"; status=1; }
note() { echo "precheck: $*"; }

# --- 1. host build + tests ---------------------------------------------------
note "building the portable core"
# A clean checkout has no build directory; the redirect below would fail first.
mkdir -p "$root/build"
if ! ./tools/build_host.sh "$root/build/bbx_tests" >"$root/build/precheck-tests.log" 2>&1; then
  tail -30 "$root/build/precheck-tests.log"
  fail "host build or tests failed (see build/precheck-tests.log)"
else
  tail -1 "$root/build/precheck-tests.log"
fi

# --- 2. AppContainer API rules ----------------------------------------------
uwp_sources=$(git ls-files 'xbox/*.cpp' 'xbox/*.h' 'xbox/*.inc' 'src/host/uwp/*' 2>/dev/null)
if [ -n "$uwp_sources" ]; then
  for pattern in 'CreateFileW' 'CreateFileA' 'VirtualAlloc(' 'VirtualAlloc2(' 'VirtualProtect(' \
                 'LoadLibraryW(' 'LoadLibraryA(' 'FindFirstFileW(' 'FindFirstFileExW(' \
                 'MapViewOfFile(' 'PAGE_EXECUTE_READWRITE'; do
    # The FromApp variants contain the same prefix; match the call, not the family.
    hits=$(grep -n -- "$pattern" $uwp_sources 2>/dev/null | grep -v 'FromApp' | grep -viE '^\s*[^:]+:[0-9]+:\s*(//|\*)' || true)
    # A mention in a comparison ("!= PAGE_EXECUTE_READWRITE") is a check, not a use.
    hits=$(printf '%s\n' "$hits" | grep -viE '(!=|==)\s*PAGE_EXECUTE_READWRITE' || true)
    hits=$(printf '%s\n' "$hits" | grep -vE '"[^"]*PAGE_EXECUTE_READWRITE[^"]*"' || true)
    if [ -n "${hits// /}" ]; then
      printf '%s\n' "$hits" | head -3
      fail "desktop-only or W^X-breaking API: $pattern"
    fi
  done
fi

# --- 3. the stack fix --------------------------------------------------------
# docs/ explains the anti-pattern; only build files may not use it.
if grep -rn "STACK:16777216" --include=*.cmake --include=*.ps1 --include=CMakeLists.txt xbox/ cmake/ CMakeLists.txt 2>/dev/null | grep -v ':#' >/dev/null; then
  fail "/STACK:16777216 is the 1.28 GiB mistake (lesson #1)"
fi
if ! grep -q "STACK:8388608,65536" xbox/CMakeLists.txt; then
  fail "xbox/CMakeLists.txt must link /STACK:8388608,65536"
fi

# --- 4. nothing that must not be tracked ------------------------------------
banned=$(git ls-files | grep -Ei '\.(pkg|sfo|prx|self|elf|bin|dcx|bnd|mapbnd|shaderbnd|mp4)$|(^|/)(eboot|param)\.|dvdroot|\.sprx$' || true)
if [ -n "$banned" ]; then
  echo "$banned"
  fail "game files or dumps are tracked"
fi
generated=$(git ls-files | grep -E '^(build|build-deps|out)/|\.(obj|pdb|lib|dll|appx|appxupload)$' || true)
if [ -n "$generated" ]; then
  echo "$generated"
  fail "build output is tracked"
fi
if git ls-files -z | xargs -0 grep -lI -E 'C:\\Users\\[A-Za-z]|/home/[a-z]+/' 2>/dev/null | grep -v '^docs/' | grep -v '^tools/' >/dev/null; then
  git ls-files -z | xargs -0 grep -lI -E 'C:\\Users\\[A-Za-z]|/home/[a-z]+/' 2>/dev/null | head -5
  fail "absolute developer paths in tracked files"
fi

# --- 5. the numbers the docs promise ---------------------------------------
for pair in "memory-budget.md:4,480" "memory-budget.md:5,120" "porting-plan.md:8388608" \
            "precompile-cache.md:precompile_inflight.txt" "console-ux.md:FutureAccessList" \
            "d3d12-backend.md:CreateSwapChainForCoreWindow" "d3d12-backend.md:6_4" \
            "ARCHITECTURE_XBOX_D3D12.md:3840" "ARCHITECTURE_XBOX_D3D12.md:pipeline library" \
            "PORT_STATUS.md:146" ; do
  file="${pair%%:*}"; needle="${pair#*:}"
  if ! grep -q -- "$needle" "docs/$file" 2>/dev/null; then
    fail "docs/$file no longer mentions $needle"
  fi
done
if ! grep -q "allocatable" src/host/d3d12/bbx_budget.h; then
  fail "bbx_budget.h lost allocatable()"
fi

# --- 5b. the vendored guest core ---------------------------------------------
if [ ! -f upstream/bbhost/UPSTREAM-PIN ]; then
  fail "upstream/bbhost/UPSTREAM-PIN is missing"
fi
if ! grep -q "a317321c7fea9f34cb002a4796d492f461ff52dd" upstream/bbhost/UPSTREAM-PIN 2>/dev/null; then
  fail "the bbhost pin changed; update UPSTREAM.md and docs/PORT_STATUS.md with it"
fi
vendored=$(find upstream/bbhost/src -name '*.cpp' | wc -l)
if [ "$vendored" -lt 200 ]; then
  fail "only $vendored vendored bbhost translation units; the tree looks truncated"
fi
if [ -d upstream/bbhost/.git ]; then
  fail "upstream/bbhost/.git must not be vendored"
fi

# --- 6. the console's shader and pipeline rules ------------------------------
if grep -rn "enable-16bit-types\|enable_16bit_types *= *true" --include=*.cpp --include=*.h --include=*.cmake src/ xbox/ 2>/dev/null | grep -v "bbx_caps.h" | grep -v "^.*//" >/dev/null; then
  fail "16-bit shader types are enabled somewhere (console has no native 16-bit ALU)"
fi
if grep -rn "CreatePipelineLibrary\|ID3D12PipelineLibrary" --include=*.cpp --include=*.h src/ xbox/ 2>/dev/null | grep -vE ':\s*//' | grep -v "bbx_caps.h" >/dev/null; then
  fail "a pipeline library is used; it removes the device on the console"
fi
if [ ! -f src/host/d3d12/bbx_psocache.cpp ] || [ ! -f src/host/d3d12/bbx_psocache.h ]; then
  fail "the PSO LRU is missing"
fi

if [ "$status" -eq 0 ]; then
  note "PASS"
else
  note "FAILED"
fi
exit "$status"
