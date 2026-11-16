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
fail() {
  echo "precheck: FAIL: $*"
  # Run logs are not always reachable after the fact; the annotation is.
  if [ "${GITHUB_ACTIONS:-}" = "true" ]; then
    echo "::error title=precheck::$(echo "$*" | sed -e 's/%/%25/g' -e 's/\r/%0D/g' -e 's/$/%0A/' | tr -d '\n')"
  fi
  status=1
}
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
# A dump that is not committed but sitting in the tree is the same problem one
# `git add -A` later: the bake tool reads it from a path outside the repo, and this
# check refuses to let a copy take root inside it.
untracked_dumps=$(git ls-files --others --exclude-standard 2>/dev/null \
  | grep -Ei '\.(shaderbnd[^/]*|dcx|bin|self|pkg|sfo)$|(^|/)eboot\.bin$|(^|/)Bb\.zip$|(^|/)bbx_shaders\.cache$' || true)
if [ -n "$untracked_dumps" ]; then
  echo "$untracked_dumps" | head -5
  fail "game dump or bake output is present in the work tree (keep it outside the repo)"
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
            "PORT_STATUS.md:296 host tests" ; do
  file="${pair%%:*}"; needle="${pair#*:}"
  if ! grep -q -- "$needle" "docs/$file" 2>/dev/null; then
    fail "docs/$file no longer mentions $needle"
  fi
done
if ! grep -q "allocatable" src/host/d3d12/bbx_budget.h; then
  fail "bbx_budget.h lost allocatable()"
fi

# --- 5a. the console build's own source list --------------------------------
# The portable core is one list and the console's build is another, so a new
# core file can compile here, pass every host test, and still be missing from
# `xbox/CMakeLists.txt` - which is a link error three minutes into CI (PathTable,
# 5B-2b). A translation unit under `xbox/platform/` that includes a core header
# whose `.cpp` exists and is built here must have that `.cpp` in the console's
# list too.
# What the console's build actually compiles: its own list, plus the SDK-only
# files the root CMakeLists gathers in BBX_CONSOLE_SOURCES for it.
console_list="$(mktemp)"
cat xbox/CMakeLists.txt > "$console_list"
sed -n '/set(BBX_CONSOLE_SOURCES/,/^)/p' CMakeLists.txt >> "$console_list"
missing=()
for tu in xbox/platform/*.cpp; do
    [ -f "$tu" ] || continue
    while IFS= read -r header; do
        case "$header" in
            src/*) impl="${header%.h}.cpp" ;;
            *) impl="src/${header%.h}.cpp" ;;
        esac
        [ -f "$impl" ] || continue
        # The console's list spells these `../src/host/d3d12/<name>.cpp`, so the
        # basename is the part that has to be there.
        if ! grep -q "$(basename "$impl")" "$console_list"; then
            missing+=("$tu includes $header, and $(basename "$impl") is not in xbox/CMakeLists.txt")
        fi
    done < <(grep -oE '"(src/)?host/d3d12/[a-z0-9_]+\.h"' "$tu" | tr -d '"' | sed -e 's|^src/||' | sort -u)
done
rm -f "$console_list"
if [ ${#missing[@]} -gt 0 ]; then
    printf '%s\n' "${missing[@]}"
    fail "a console translation unit includes a core header whose .cpp the console build does not compile"
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

# --- 5c. package assets -------------------------------------------------------
# makeappx fails when the manifest names an image that is not in the layout, and
# the failure only shows up in the packaging step of the Windows job.
if [ ! -f tools/make-assets.py ]; then
  fail "tools/make-assets.py is missing; the package assets have no source"
fi
for asset in $(grep -o 'Assets[\\/][A-Za-z0-9]*\.png' xbox/AppxManifest.xml |
    sed 's#.*[\\/]##' | sort -u); do
  if [ ! -f "xbox/Assets/$asset" ]; then
    fail "xbox/Assets/$asset is named by AppxManifest.xml but missing (run tools/make-assets.py)"
  fi
done

# --- 5d. sideload readiness ---------------------------------------------------
# A console will not install an unsigned package, and Xbox Device Portal refuses
# one that does not carry the Store CRT framework package. Both facts live in the
# manifest and the packaging script, so check them here rather than on a console.
if ! grep -q "Microsoft.VCLibs.140.00" xbox/AppxManifest.xml; then
  fail "the manifest lost the Microsoft.VCLibs.140.00 PackageDependency (sideloading needs it)"
fi
if ! grep -q "CN=Microsoft Corporation" xbox/AppxManifest.xml; then
  fail "the VCLibs PackageDependency lost Microsoft's publisher string"
fi
if ! grep -q "Microsoft.VCLibs.x64.14.00.appx" xbox/scripts/package.ps1; then
  fail "package.ps1 no longer stages the VCLibs framework package next to the .appx"
fi
if ! grep -q "signtool sign" xbox/scripts/package.ps1; then
  fail "package.ps1 must sign the package; an unsigned .appx will not install on a console"
fi
if ! grep -q "bbxbox.cer" xbox/scripts/package.ps1; then
  fail "package.ps1 must export the signing certificate as bbxbox.cer"
fi

# --- 5e. the shader toolchain -------------------------------------------------
# The console app carries no shader compiler: every fixed pass is DXIL compiled on
# the build machine. These rules keep that true, because the failure mode (a
# packaged desktop dxcompiler.dll that LoadPackagedLibrary refuses on the console)
# only shows up on the TV.
if ! ls xbox/shaders/*.hlsl >/dev/null 2>&1; then
  fail "xbox/shaders has no HLSL: the present and overlay passes are compiled at build time"
fi
# Comments explain these rules, so they must not trip them: strip // to end of line
# and # to end of line before looking for forbidden tokens.
hlsl_code() { sed -e 's://.*::' "$1"; }
cmake_code() { sed -e 's:#.*::' "$1"; }
for hlsl in xbox/shaders/*.hlsl; do
  if hlsl_code "$hlsl" | grep -q '\[\[vk::'; then
    fail "$hlsl uses [[vk::*]]: that is SPIR-V flavoured HLSL, rejected at -T *_6_4"
  fi
  if hlsl_code "$hlsl" | grep -Eq 'min16(float|int|uint)|\bhalf\b|ResourceDescriptorHeap'; then
    fail "$hlsl uses 16-bit types or heap indexing: the console lowers 16-bit and has no SM 6.6"
  fi
done
if cmake_code xbox/CMakeLists.txt | grep -q -- "enable-16bit-types"; then
  fail "xbox/CMakeLists.txt enables 16-bit types; the console has no native 16-bit ALU"
fi
for symbol in PresentVs PresentPs OverlayVs OverlayPs UntileCs SelftestCs; do
  if ! grep -q "bbx_shader($symbol " xbox/CMakeLists.txt; then
    fail "bbx_shader($symbol ...) is missing from xbox/CMakeLists.txt"
  fi
  if ! grep -q "k$symbol{" src/host/d3d12/bbx_shaders.h; then
    fail "k$symbol is missing from bbx_shaders.h's manifest"
  fi
done
if ! grep -q -- "-WX" xbox/scripts/dxc-run.ps1; then
  fail "dxc-run.ps1 must compile with -WX: a shader warning is a PSO failure on the TV"
fi
if ! grep -q -- "-T" xbox/scripts/dxc-run.ps1; then
  fail "dxc-run.ps1 must pass an explicit -T profile"
fi
if grep -Eq 'Copy-Item.*(dxcompiler|dxil)' xbox/scripts/package.ps1; then
  fail "package.ps1 still copies a shader compiler into the package; DXIL is build-time only"
fi
if ! grep -q "spirv_to_dxil" docs/ARCHITECTURE_XBOX_D3D12.md; then
  fail "docs/ARCHITECTURE_XBOX_D3D12.md must state the SPIR-V -> DXIL plan for guest shaders"
fi

# --- 5f. the guest core is compiled and tested, not just vendored -------------
# The loader pieces are the ones whose bugs are silent (a wrong NID becomes the
# wrong HLE function, a bad TLS slot reads past the TEB), so the test binary must
# keep compiling them from upstream/bbhost rather than from a copy.
if ! grep -q "guest_core_test.cpp" tests/../CMakeLists.txt; then
  fail "tests/guest_core_test.cpp is not in CMakeLists.txt"
fi
if ! grep -q "tests/guest_core_test.cpp" tools/build_host.sh; then
  fail "tests/guest_core_test.cpp is not built by tools/build_host.sh"
fi
for core in sha256 imports tls_rewrite elf paced_file portable; do
  if ! grep -q "core/$core.cpp" tools/build_host.sh; then
    fail "tools/build_host.sh no longer compiles upstream/bbhost/src/core/$core.cpp; the guest-core tests would run against nothing"
  fi
done

# --- 5g. the Windows-only code, type-checked without Windows ------------------
# tools/syntax_check.sh compiles src/host/d3d12/*.cpp against the declaration-only
# stubs in tools/stubs/win_d3d12/, so a wrong D3D12 signature fails here in about a
# second instead of on the Windows runner three minutes later.
if ! ./tools/syntax_check.sh; then
  fail "the Windows-only translation units do not type-check (tools/syntax_check.sh)"
fi
# Rules the console enforces that a compiler cannot: see ARCHITECTURE §2 and §5.
if grep -rn "CreatePipelineLibrary" src/host/d3d12/*.cpp 2>/dev/null | grep -v '//' >/dev/null; then
  fail "CreatePipelineLibrary is called somewhere; it removes the device on a console"
fi
if ! grep -q "DXGI_SWAP_EFFECT_FLIP_DISCARD" src/host/d3d12/swapchain.cpp; then
  fail "swapchain.cpp must use DXGI_SWAP_EFFECT_FLIP_DISCARD"
fi
if ! grep -q "SetMatrixTransform" src/host/d3d12/swapchain.cpp; then
  fail "swapchain.cpp must apply the inverse scale with IDXGISwapChain2::SetMatrixTransform (guard G8)"
fi
if ! grep -q "D3D_FEATURE_LEVEL_11_0" src/host/d3d12/device.cpp; then
  fail "device.cpp must create the device at D3D_FEATURE_LEVEL_11_0"
fi
if ! grep -q "XboxSeriesCaps()" src/host/d3d12/device.cpp; then
  fail "device.cpp must validate against bbx_caps::XboxSeriesCaps()"
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
