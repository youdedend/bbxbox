#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Type-check the Windows-only translation units without Windows.
#
# `src/host/d3d12/*.cpp` and the UWP platform code cannot be compiled on the
# machine this port is developed on, and CI takes three minutes per answer. This
# compiles them against `tools/stubs/win_d3d12/` - a hand-written, declarations-only
# stand-in for <windows.h>, <d3d12.h>, <dxgi1_6.h> and <wrl/client.h> - with
# `-fsyntax-only`, so a wrong parameter list, a missing include or a typo in a COM
# method name fails in about a second.
#
# It proves types and signatures, not semantics or the real SDK: `UWP build +
# import audit` still compiles every one of these files with clang-cl against the
# installed SDK, links them with the Store CRT, and audits the imports. Keep the
# stubs honest - when a file needs a declaration that is missing, add it here.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx="${CXX:-}"
if [ -z "$cxx" ]; then
    if command -v clang++ >/dev/null 2>&1; then cxx=clang++; else cxx=g++; fi
fi
stubs="$root/tools/stubs/win_d3d12"

if ! command -v "$cxx" >/dev/null 2>&1; then
    echo "syntax_check: $cxx not found; skipping (it is part of the precheck when present)"
    exit 0
fi

files=()
while IFS= read -r file; do files+=("$file"); done < <(
    ls "$root"/src/host/d3d12/*.cpp 2>/dev/null | grep -Ev 'bbx_(format|barriers|budget|cache|quarantine|precompile|descriptors|present|stream|untile|psocache|tess)\.cpp$'
)

if [ ${#files[@]} -eq 0 ]; then
    echo "syntax_check: nothing Windows-only to check yet"
    exit 0
fi

status=0
for file in "${files[@]}"; do
    # -fms-extensions brings __declspec(uuid()) and __uuidof, which is what the
    # COM code uses through IID_PPV_ARGS; -fno-exceptions matches the console build.
    # -fms-extensions/-fdeclspec only exist on clang; GCC reads the stubs' own
    # __uuidof shim instead.
    extra=()
    if "$cxx" --version 2>/dev/null | grep -qi clang; then extra+=(-fms-extensions -fdeclspec); fi
    output=$("$cxx" -fsyntax-only -std=c++20 -Wall -Wextra -Werror \
        "${extra[@]}" -fno-exceptions -Wno-unused-parameter \
        -D_WIN32 -DBBX_UWP=1 -DUNICODE -D_UNICODE -DWINAPI_FAMILY=WINAPI_FAMILY_APP \
        -I"$stubs" -I"$root" -I"$root/src" -I"$root/upstream/bbhost/src" \
        "$file" 2>&1)
    if [ $? -ne 0 ]; then
        echo "syntax_check: FAIL: $(basename "$file")"
        echo "$output" | head -40
        status=1
    fi
done

if [ $status -eq 0 ]; then
    echo "syntax_check: PASS (${#files[@]} Windows-only translation unit(s))"
fi
exit $status
