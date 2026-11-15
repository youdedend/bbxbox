#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the package logos in xbox/Assets.

The manifest names six images; makeappx fails on a package whose manifest points
at files that are not there, and a console install with a placeholder icon looks
like a broken build. So the marks are generated here, deterministically, and
committed: no binary asset arrives in the tree without a script that made it.

The mark is a dark tile with the port's amber "backslash-replay" bar and a cyan
cut, drawn with plain arithmetic rather than a graphics library so the script
runs anywhere Python does. A 4x4 supersample keeps the diagonals clean at 44 px.

    python3 tools/make-assets.py            # write xbox/Assets/*.png
    python3 tools/make-assets.py --check    # exit 1 if they are missing
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "xbox" / "Assets"

# 0x0C0E12 is the manifest's BackgroundColor; the two accents are the port's
# palette (amber for the replay bar, cyan for the guest).
BACKGROUND = (0x0C, 0x0E, 0x12)
AMBER = (0xE8, 0xB0, 0x3A)
CYAN = (0x4E, 0xC8, 0xD8)

# (file name, width, height) exactly as AppxManifest.xml names them.
IMAGES = [
    ("StoreLogo.png", 50, 50),
    ("Square44x44Logo.png", 44, 44),
    ("Square150x150Logo.png", 150, 150),
    ("Wide310x150Logo.png", 310, 150),
    ("LargeTile.png", 310, 310),
    ("SplashScreen.png", 620, 300),
]

SUPERSAMPLE = 4


def mark(x: float, y: float) -> tuple[int, int, int]:
    """The mark, evaluated at one sample point in [0,1]x[0,1] tile coordinates."""
    # Normalise around the centre so every tile shape gets the same mark.
    cx, cy = x - 0.5, y - 0.5
    # A diamond: the guest's four channels.
    if abs(cx) + abs(cy) < 0.30:
        return BACKGROUND
    if abs(cx) + abs(cy) < 0.36:
        return CYAN
    # The replay bar: a diagonal from lower-left to upper-right, clipped to the
    # middle 70% of the tile.
    if 0.14 < x < 0.86 and 0.14 < y < 0.86:
        d = (x - y) * 0.5 + 0.5
        if 0.40 < d < 0.56:
            return AMBER
    return BACKGROUND


def render(width: int, height: int) -> bytes:
    """RGBA rows, supersampled, as the raw IDAT payload."""
    rows = bytearray()
    step = 1.0 / SUPERSAMPLE
    for py in range(height):
        rows.append(0)  # filter: none
        for px in range(width):
            r = g = b = 0
            for sy in range(SUPERSAMPLE):
                for sx in range(SUPERSAMPLE):
                    u = (px + (sx + 0.5) * step) / width
                    v = (py + (sy + 0.5) * step) / height
                    cr, cg, cb = mark(u, v)
                    r += cr
                    g += cg
                    b += cb
            n = SUPERSAMPLE * SUPERSAMPLE
            rows.extend((r // n, g // n, b // n, 255))
    return bytes(rows)


def chunk(tag: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + tag + payload +
            struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def png(width: int, height: int) -> bytes:
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(render(width, height), 9)) +
            chunk(b"IEND", b""))


def main(argv: list[str]) -> int:
    check = "--check" in argv
    missing = []
    for name, width, height in IMAGES:
        path = ASSETS / name
        if check:
            if not path.is_file():
                missing.append(name)
            continue
        ASSETS.mkdir(parents=True, exist_ok=True)
        path.write_bytes(png(width, height))
        print(f"wrote {path.relative_to(ROOT)} ({width}x{height})")
    if check and missing:
        print("missing assets: " + ", ".join(missing), file=sys.stderr)
        print("run: python3 tools/make-assets.py", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
