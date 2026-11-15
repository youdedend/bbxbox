#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Vendors (or re-verifies) droogie/bbhost at the pinned commit.
#
# The tree lives in upstream/bbhost and is committed, so CI never needs network
# access to build the guest core. This script is how it got there and how it is
# refreshed: it clones the pin, copies the same subset, and prints the local
# edits the port has made (every one marked BBX_UWP).
#
#   tools/fetch_upstream.sh            verify the vendored tree matches the pin
#   tools/fetch_upstream.sh --refresh  re-copy from a fresh clone at the pin
#   tools/fetch_upstream.sh --diff     show the port's local edits
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
pin_file="$root/upstream/bbhost/UPSTREAM-PIN"
pin_url=$(sed -n 's/^repository *= *//p' "$pin_file")
pin_sha=$(sed -n 's/^commit *= *//p' "$pin_file")
dest="$root/upstream/bbhost"

if [ -z "$pin_url" ] || [ -z "$pin_sha" ]; then
  echo "fetch_upstream: cannot read the pin from $pin_file" >&2
  exit 1
fi

if [ "${1:-}" = "--diff" ]; then
  grep -rn "BBX_UWP" "$dest/src" | sed "s|$dest/||" | head -200
  exit 0
fi

work="${BBHOST_CHECKOUT:-/tmp/bbhost-vendor}"
if [ ! -d "$work/.git" ]; then
  git clone --filter=blob:none --no-checkout "$pin_url" "$work"
fi
git -C "$work" fetch -q origin "$pin_sha"
git -C "$work" checkout -q "$pin_sha"
actual=$(git -C "$work" rev-parse HEAD)
if [ "$actual" != "$pin_sha" ]; then
  echo "fetch_upstream: checkout is $actual, pin is $pin_sha" >&2
  exit 1
fi

if [ "${1:-}" = "--refresh" ]; then
  # Keep the port's local edits: capture the marked files, re-copy, re-apply the
  # guard markers is not automatic. Refreshing is a deliberate, reviewed action:
  # the script refuses if any local edit exists outside memory.{h,cpp}.
  edits=$(grep -rl "BBX_UWP" "$dest/src" | sed "s|$dest/src/||" || true)
  allowed="core/memory.h
core/memory.cpp"
  unexpected=$(comm -23 <(printf '%s\n' "$edits" | sort) <(printf '%s\n' "$allowed" | sort))
  if [ -n "$unexpected" ]; then
    echo "fetch_upstream: local edits outside memory.{h,cpp}:" >&2
    echo "$unexpected" >&2
    echo "refusing to overwrite them; move them into xbox/ or cmake/ first" >&2
    exit 1
  fi
  rm -rf "$dest/src" "$dest/third_party/libatrac9" "$dest/third_party/monocypher"
  cp -r "$work/src" "$dest/src"
  mkdir -p "$dest/third_party"
  cp -r "$work/third_party/libatrac9" "$work/third_party/monocypher" "$dest/third_party/"
  cp "$work/LICENSE" "$work/LICENSE-EXCEPTION" "$dest/"
  echo "fetch_upstream: refreshed $dest from $pin_sha; re-apply the BBX_UWP guards in core/memory.*"
  exit 0
fi

# Verify: every vendored file that exists upstream must be identical, except the
# two files the port edits.
fail=0
while IFS= read -r -d '' file; do
  rel="${file#$work/}"
  have="$dest/$rel"
  [ -f "$have" ] || continue
  if ! cmp -s "$file" "$have"; then
    case "$rel" in
      src/core/memory.h|src/core/memory.cpp) echo "edited (expected): $rel" ;;
      *) echo "DIFFERS: $rel"; fail=1 ;;
    esac
  fi
done < <(find "$work/src" "$work/third_party/libatrac9" "$work/third_party/monocypher" -type f -print0)
if [ "$fail" -ne 0 ]; then
  echo "fetch_upstream: vendored tree drifted from the pin" >&2
  exit 1
fi
echo "fetch_upstream: vendored tree matches $pin_sha (memory.{h,cpp} edited as documented)"
