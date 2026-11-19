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
    { ls "$root"/src/host/d3d12/*.cpp 2>/dev/null \
          | grep -Ev 'bbx_(format|barriers|budget|cache|quarantine|precompile|descriptors|present|stream|untile|psocache|tess)\.cpp$'
      # The UWP platform TUs that avoid C++/WinRT: the D3D12 session takes an
      # IUnknown* and SDK headers only, precisely so it lands in this check.
      ls "$root"/xbox/platform/d3d12_session.cpp 2>/dev/null
      # The compute and graphics executors: SDK headers and the translator, no C++/WinRT.
      ls "$root"/xbox/platform/guest_compute.cpp 2>/dev/null
      ls "$root"/xbox/platform/guest_graphics.cpp 2>/dev/null
    }
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
    # __uuidof shim instead. The unused-* warnings are off because the stub headers
    # are full of constants and helpers that only some translation units use, and
    # -Werror would turn that into a failure that says nothing about the code under
    # test.
    # The stubs are included with -isystem, so their unused constants and helpers
    # are not this check's business; the code under test is held to -Werror in
    # full, which is how an unused local is caught here rather than on CI.
    #
    # -Wno-ignored-attributes: the SDK (and therefore the stubs) put
    # __declspec(uuid(...)) before `struct`, which plain clang-with-ms-extensions
    # ignores, and -Werror would turn that decoration into a failure. clang-cl
    # accepts it, so the Windows job never sees the warning.
    extra=()
    if "$cxx" --version 2>/dev/null | grep -qi clang; then
        extra+=(-fms-extensions -fdeclspec)
    else
        # -Wformat-truncation is GCC-only; clang rejects it with -Werror,-Wunknown-warning-option.
        extra+=(-Wformat-truncation)
    fi
    output=$("$cxx" -fsyntax-only -std=c++20 -Wall -Wextra -Werror \
        "${extra[@]}" -fno-exceptions \
        -Wno-unused-parameter -Wno-ignored-attributes \
        -D_WIN32 -DBBX_UWP=1 -DUNICODE -D_UNICODE -DWINAPI_FAMILY=WINAPI_FAMILY_APP \
        -isystem "$stubs" -I"$root" -I"$root/src" -I"$root/upstream/bbhost/src" \
        "$file" 2>&1)
    if [ $? -ne 0 ]; then
        echo "syntax_check: FAIL: $(basename "$file")"
        echo "$output" | head -40
        if [ "${GITHUB_ACTIONS:-}" = "true" ]; then
            # The run log is not always reachable after the fact; this is.
            # tr -d '\n': a workflow command ends at a real newline, so the
            # diagnostics must be escaped *and* joined or only the first line
            # (an include stack) reaches the annotation.
            encoded=$(echo "$output" | head -30 | sed -e 's/%/%25/g' -e 's/\r/%0D/g' -e 's/$/%0A/' | tr -d '\n')
            echo "::error title=syntax_check $(basename "$file")::$encoded"
        fi
        status=1
    fi
done

if [ $status -eq 0 ]; then
    echo "syntax_check: PASS (${#files[@]} Windows-only translation unit(s))"
fi
exit $status
