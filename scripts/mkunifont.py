#!/usr/bin/env python3
"""Convert a GNU Unifont .hex build into the SD font file the firmware reads.

    python3 scripts/mkunifont.py unifont-16.0.04.hex docs/unifont.bin

Output format ("AUF1"): a 4-byte magic, then 65536 fixed 33-byte records — one
per BMP codepoint, so a glyph seek is just 4 + cp * 33 with no index:

    byte 0     width code: 0 = no glyph, 1 = halfwidth 8x16, 2 = fullwidth 16x16
    bytes 1-32 the bitmap, 16 rows top to bottom, MSB-first (halfwidth uses one
               byte per row in bytes 1-16; the rest stays zero)

GNU Unifont's .hex lines are exactly those bitmaps already (32 hex digits for
halfwidth, 64 for fullwidth), so this is a repack, not a re-render. The font is
(c) Roman Czyborra and the Unifont maintainers, dual-licensed GPLv2+ with the
font-embedding exception / SIL OFL 1.1 — see unifoundry.com/unifont.
"""

import sys


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: mkunifont.py <unifont.hex> <out.bin>")
    table = bytearray(4 + 65536 * 33)
    table[0:4] = b"AUF1"
    n_half = n_full = 0
    for line in open(sys.argv[1]):
        line = line.strip()
        if not line or ":" not in line:
            continue
        cp_s, bits_s = line.split(":", 1)
        cp = int(cp_s, 16)
        if cp > 0xFFFF:
            continue  # BMP only: SMP is emoji/rare, and direct indexing stays tiny
        bits = bytes.fromhex(bits_s)
        off = 4 + cp * 33
        if len(bits) == 16:  # halfwidth 8x16
            table[off] = 1
            table[off + 1 : off + 17] = bits
            n_half += 1
        elif len(bits) == 32:  # fullwidth 16x16
            table[off] = 2
            table[off + 1 : off + 33] = bits
            n_full += 1
    open(sys.argv[2], "wb").write(table)
    print(f"{sys.argv[2]}: {n_half} halfwidth + {n_full} fullwidth glyphs, {len(table)} bytes")


if __name__ == "__main__":
    main()
