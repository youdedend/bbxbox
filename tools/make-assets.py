#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the Bloodborne-inspired package logos and splash screen in xbox/Assets.

The manifest names six images; makeappx fails on a package whose manifest points
at files that are not there. Each asset is generated deterministically in pure
Python with 4x4 supersampled signed-distance geometry:

  * Deep Yharnam midnight obsidian sky (#09060A) with a glowing Paleblood
    Crimson Moon and subtle lunar surface texture;
  * The iconic Bloodborne Hunter's Mark (Caryll Rune of the Hunter) etched in
    molten tarnished gold, pale ivory, and blood-crimson ember glow;
  * Gothic cathedral spire silhouettes and Victorian tarnished-brass framing.

    python3 tools/make-assets.py            # write xbox/Assets/*.png
    python3 tools/make-assets.py --check    # exit 1 if they are missing
"""

from __future__ import annotations

import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "xbox" / "Assets"

# (file name, width, height) exactly as AppxManifest.xml names them.
IMAGES = [
    ("StoreLogo.png", 50, 50),
    ("Square44x44Logo.png", 44, 44),
    ("Square150x150Logo.png", 150, 150),
    ("Wide310x150Logo.png", 310, 150),
    ("LargeTile.png", 310, 310),
    ("SplashScreen.png", 620, 300),
]

SUPERSAMPLE = 3


def clamp(v: float, lo: float = 0.0, hi: float = 1.0) -> float:
    return lo if v < lo else (hi if v > hi else v)


def smoothstep(edge0: float, edge1: float, x: float) -> float:
    if edge0 == edge1:
        return 0.0 if x < edge0 else 1.0
    t = clamp((x - edge0) / (edge1 - edge0))
    return t * t * (3.0 - 2.0 * t)


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def lerp_rgb(
    c0: tuple[float, float, float], c1: tuple[float, float, float], t: float
) -> tuple[float, float, float]:
    t = clamp(t)
    return (
        lerp(c0[0], c1[0], t),
        lerp(c0[1], c1[1], t),
        lerp(c0[2], c1[2], t),
    )


def sd_segment(
    px: float, py: float, ax: float, ay: float, bx: float, by: float
) -> float:
    pax, pay = px - ax, py - ay
    bax, bay = bx - ax, by - ay
    denom = bax * bax + bay * bay
    h = 0.0 if denom <= 1e-9 else clamp((pax * bax + pay * bay) / denom)
    dx = pax - bax * h
    dy = pay - bay * h
    return math.hypot(dx, dy)


def hunters_mark_sdf(px: float, py: float) -> float:
    """Signed distance to the Bloodborne Hunter's Mark (Caryll Rune of the Hunter).

    Coordinates are in [-1, 1] vertical space with (0, 0) at the rune center
    and +y pointing down.
    """
    ax = abs(px)

    # 1. Central vertical hanging shaft + top spire + lower ritual drip
    d_spine = sd_segment(ax, py, 0.0, -0.78, 0.0, 0.45)
    d_drip = sd_segment(ax, py, 0.0, 0.45, 0.0, 0.77)

    # 2. Upper horizontal suspension cross-bar and lower nexus bar
    d_top_bar = sd_segment(ax, py, 0.0, -0.57, 0.135, -0.57)
    d_low_bar = sd_segment(ax, py, 0.0, 0.43, 0.095, 0.43)

    # 3. Outer sweeping diamond / crescent arms of the hanging mark
    d_arm_upper = sd_segment(ax, py, 0.0, -0.43, 0.34, -0.05)
    d_arm_lower = sd_segment(ax, py, 0.34, -0.05, 0.175, 0.35)
    d_arm_close = sd_segment(ax, py, 0.175, 0.35, 0.0, 0.45)

    # 4. The four signature inward-hooked Caryll rune barbs
    d_barb_hi = sd_segment(ax, py, 0.255, -0.155, 0.105, -0.045)
    d_barb_lo = sd_segment(ax, py, 0.245, 0.145, 0.095, 0.055)

    # 5. Lower ritual fork prongs flanking the central drip
    d_fork = sd_segment(ax, py, 0.02, 0.44, 0.085, 0.63)

    d = min(
        d_spine,
        d_drip,
        d_top_bar,
        d_low_bar,
        d_arm_upper,
        d_arm_lower,
        d_arm_close,
        d_barb_hi,
        d_barb_lo,
        d_fork,
    )

    # Subtle hand-carved organic rune edge variation
    etch = (
        0.0035 * math.sin(py * 29.0 + ax * 17.0)
        + 0.0020 * math.cos(py * 53.0 - ax * 23.0)
    )
    return d + etch


def gothic_spires_alpha(px: float, py: float) -> float:
    """Silhouette coverage [0..1] for Yharnam gothic cathedral spires at the horizon."""
    ax = abs(px)
    # Base horizon wall
    horizon = 0.78
    # Repeated gothic pinnacles and pointed arches
    spires = (
        (0.38, 0.38, 0.11),
        (0.58, 0.18, 0.13),
        (0.78, 0.44, 0.09),
        (1.02, 0.08, 0.16),
        (1.26, 0.34, 0.11),
        (1.48, 0.22, 0.13),
        (1.72, 0.42, 0.10),
    )
    h = horizon
    for sx, top_y, half_w in spires:
        dx = abs(ax - sx)
        if dx < half_w:
            # Steep gothic spire profile with a sharp finial needle
            t = dx / half_w
            spire_y = top_y + (horizon - top_y) * (t ** 0.65)
            if spire_y < h:
                h = spire_y
        # Spire needle tip
        if dx < 0.012 and py > top_y - 0.11:
            h = min(h, top_y - 0.11 + (dx / 0.012) * 0.11)
    return smoothstep(h - 0.012, h + 0.012, py)


def sample_art(u: float, v: float, aspect: float) -> tuple[int, int, int]:
    """Evaluate the Bloodborne icon/splash artwork at normalized (u, v) in [0, 1]^2."""
    # Aspect-corrected coordinate system with py in [-1, 1], px in [-aspect, aspect]
    px = (u - 0.5) * 2.0 * aspect
    py = (v - 0.5) * 2.0
    r_center = math.hypot(px, py)

    # 1. Yharnam Midnight Sky & Paleblood Atmosphere
    night_zenith = (9.0, 6.0, 11.0)
    night_horizon = (26.0, 9.0, 14.0)
    sky_t = clamp((py + 1.0) * 0.5)
    rgb = lerp_rgb(night_zenith, night_horizon, sky_t * sky_t)

    # Atmospheric blood-mist clouds
    mist = 0.5 + 0.5 * math.sin(px * 2.8 + math.cos(py * 3.4) * 1.2) * math.cos(
        py * 2.3 - px * 1.4
    )
    rgb = lerp_rgb(rgb, (52.0, 11.0, 16.0), mist * 0.28 * smoothstep(1.6, 0.1, r_center))

    # 2. The Paleblood / Red Moon centered behind the Hunter's Mark
    moon_y = py + 0.03
    moon_r = math.hypot(px, moon_y)
    moon_radius = 0.64

    # Outer crimson moon corona
    corona = math.exp(-max(0.0, moon_r - 0.18) * 2.35)
    rgb = lerp_rgb(rgb, (132.0, 18.0, 24.0), corona * 0.65)

    # Moon disc with lunar maria texture and bright crimson-gold limb
    if moon_r < moon_radius + 0.03:
        disc_mask = 1.0 - smoothstep(moon_radius - 0.02, moon_radius + 0.015, moon_r)
        crater = (
            0.5
            + 0.28 * math.sin(px * 8.5 + py * 5.0)
            + 0.22 * math.cos(px * 15.0 - py * 12.0)
        )
        limb = clamp(moon_r / moon_radius)
        moon_core = lerp_rgb((150.0, 24.0, 26.0), (88.0, 12.0, 16.0), crater * 0.65)
        moon_edge = (225.0, 74.0, 42.0)
        moon_col = lerp_rgb(moon_core, moon_edge, limb ** 2.6)
        rgb = lerp_rgb(rgb, moon_col, disc_mask * 0.78)

    # Thin tarnished-gold lunar astrolabe ring around the Blood Moon
    ring_d = abs(moon_r - 0.72)
    if ring_d < 0.022:
        ring_t = 1.0 - smoothstep(0.004, 0.020, ring_d)
        rgb = lerp_rgb(rgb, (176.0, 134.0, 74.0), ring_t * 0.55)

    # 3. Yharnam Gothic Cathedral Spires along the horizon
    spire_cov = gothic_spires_alpha(px, py)
    if spire_cov > 0.0:
        # Dark obsidian silhouette with faint blood-moon rim light
        spire_col = (10.0, 7.0, 11.0)
        rgb = lerp_rgb(rgb, spire_col, spire_cov * 0.92)

    # 4. Wide-tile / SplashScreen ornamental Victorian filigree wings
    if aspect > 1.35:
        ax = abs(px)
        if 0.80 < ax < aspect - 0.16:
            line_d = abs(py)
            if line_d < 0.03:
                t = 1.0 - smoothstep(0.005, 0.024, line_d)
                rgb = lerp_rgb(rgb, (186.0, 142.0, 78.0), t * 0.65)
            # Diamond finials along the wing rule
            for gem_x in (0.94, 1.32, 1.68):
                if gem_x < aspect - 0.20:
                    gd = abs(ax - gem_x) + abs(py)
                    if gd < 0.065:
                        gt = 1.0 - smoothstep(0.02, 0.062, gd)
                        rgb = lerp_rgb(rgb, (226.0, 178.0, 98.0), gt * 0.85)

    # 5. The Centerpiece: Bloodborne Hunter's Mark (Caryll Rune)
    d_rune = hunters_mark_sdf(px, py)

    # Outer blood-ember aura around the rune
    if d_rune < 0.22:
        aura = math.exp(-max(0.0, d_rune) * 18.0)
        rgb = lerp_rgb(rgb, (212.0, 28.0, 28.0), aura * 0.72)

    # Carved crimson-bronze bevel
    if d_rune < 0.048:
        bevel = 1.0 - smoothstep(0.022, 0.046, d_rune)
        rgb = lerp_rgb(rgb, (228.0, 82.0, 44.0), bevel * 0.92)

    # Molten tarnished-gold & paleblood ivory rune core
    if d_rune < 0.026:
        core = 1.0 - smoothstep(0.007, 0.025, d_rune)
        hot_center = 1.0 - smoothstep(0.0, 0.013, d_rune)
        rune_col = lerp_rgb((228.0, 178.0, 96.0), (252.0, 236.0, 198.0), hot_center)
        rgb = lerp_rgb(rgb, rune_col, core)

    # 6. Ornate Victorian Tarnished-Gold & Blood-Iron Border Frame
    # Convert border distance to normalized [0..0.5] edge distance
    edge_x = min(u, 1.0 - u) * aspect
    edge_y = min(v, 1.0 - v)
    edge = min(edge_x, edge_y)

    # Outer dark vignette
    if edge < 0.09:
        vig = 1.0 - smoothstep(0.0, 0.09, edge)
        rgb = lerp_rgb(rgb, (7.0, 5.0, 8.0), vig * 0.65)

    # Tarnished brass frame line
    frame_d = abs(edge - 0.045)
    if frame_d < 0.014:
        ft = 1.0 - smoothstep(0.003, 0.013, frame_d)
        rgb = lerp_rgb(rgb, (172.0, 128.0, 68.0), ft * 0.80)

    # Corner diamond studs
    corner_d = abs(edge_x - 0.045) + abs(edge_y - 0.045)
    if corner_d < 0.045:
        ct = 1.0 - smoothstep(0.012, 0.042, corner_d)
        rgb = lerp_rgb(rgb, (228.0, 176.0, 96.0), ct * 0.90)

    return (
        int(clamp(rgb[0], 0.0, 255.0) + 0.5),
        int(clamp(rgb[1], 0.0, 255.0) + 0.5),
        int(clamp(rgb[2], 0.0, 255.0) + 0.5),
    )


def render(width: int, height: int) -> bytes:
    """RGBA rows, supersampled, as the raw IDAT payload."""
    rows = bytearray()
    step = 1.0 / SUPERSAMPLE
    aspect = float(width) / float(height)
    n = SUPERSAMPLE * SUPERSAMPLE
    for py in range(height):
        rows.append(0)  # filter: none
        for px in range(width):
            r = g = b = 0
            for sy in range(SUPERSAMPLE):
                v = (py + (sy + 0.5) * step) / height
                for sx in range(SUPERSAMPLE):
                    u = (px + (sx + 0.5) * step) / width
                    cr, cg, cb = sample_art(u, v, aspect)
                    r += cr
                    g += cg
                    b += cb
            rows.extend((r // n, g // n, b // n, 255))
    return bytes(rows)


def chunk(tag: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + tag
        + payload
        + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    )


def png(width: int, height: int) -> bytes:
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(render(width, height), 9))
        + chunk(b"IEND", b"")
    )


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
