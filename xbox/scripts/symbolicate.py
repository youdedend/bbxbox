#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Resolve a console crash report's `bbxbox.exe+0xNNNNNN` against the build's .map.

`uwp_last_chance_logger` prints the faulting address, the module base and the RVA.
The .map that /MAP produced next to bbxbox.exe is enough to turn that RVA into a
function name and source file, which is the difference between "it crashed" and
"it crashed in bbx::d3d12::Replay::Draw at replay.cpp+0x1A2".

Usage:
  symbolicate.py bbxbox.map 0x2A1F30
  symbolicate.py bbxbox.map --log loader.log
"""
import argparse
import re
import sys

PUBLIC_RE = re.compile(r"^\s*([0-9A-Fa-f]{8,16}):([0-9A-Fa-f]{8,16})\s+(\S+)\s+([0-9A-Fa-f]{8,16})\s+(\S+)\s+(.*)$")
SECTION_RE = re.compile(r"^ (?:SECTION HEADER|Start|Address|Publics by Value)", re.IGNORECASE)
RVA_RE = re.compile(r"RVA 0x([0-9A-Fa-f]+)")


def parse_map(path):
    symbols = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = PUBLIC_RE.match(line)
            if not m:
                continue
            section, offset = int(m.group(1), 16), int(m.group(2), 16)
            name = m.group(3)
            # The address column is the RVA of the symbol inside the image.
            try:
                rva = int(m.group(4), 16)
            except ValueError:
                continue
            symbols.append((rva, name, m.group(6).strip()))
    symbols.sort(key=lambda s: s[0])
    return symbols


def resolve(symbols, rva):
    best = None
    for base, name, lib in symbols:
        if base <= rva:
            best = (base, name, lib)
        else:
            break
    if best is None:
        return None
    base, name, lib = best
    return name, rva - base, lib


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("map", help="bbxbox.map produced by /MAP")
    parser.add_argument("rva", nargs="?", help="RVA in hex, e.g. 0x2A1F30")
    parser.add_argument("--log", help="scan a loader.log for RVA lines instead")
    args = parser.parse_args()

    symbols = parse_map(args.map)
    if not symbols:
        sys.exit(f"{args.map}: no public symbols found")

    rvas = []
    if args.log:
        with open(args.log, "r", encoding="utf-8", errors="replace") as f:
            rvas = [int(m, 16) for m in RVA_RE.findall(f.read())]
        if not rvas:
            sys.exit(f"{args.log}: no 'RVA 0x...' lines")
    elif args.rva:
        rvas = [int(args.rva, 16)]
    else:
        sys.exit("give an RVA or --log")

    for rva in rvas:
        hit = resolve(symbols, rva)
        if hit is None:
            print(f"0x{rva:X}: below the first symbol (stub or CRT)")
        else:
            name, delta, lib = hit
            print(f"0x{rva:X}: {name}+0x{delta:X} ({lib})")


if __name__ == "__main__":
    main()
