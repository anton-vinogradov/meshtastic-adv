#!/usr/bin/env python3
"""Convert GNU Unifont .hex builds into the font file the firmware reads.

    python3 scripts/mkunifont.py unifont-16.0.04.hex unifont_upper-16.0.04.hex docs/unifont.bin

Output format ("AUF2"): a 4-byte magic, then fixed 33-byte records — one per
covered codepoint, so a glyph seek is pure arithmetic with no index:

    records 0..65535       the BMP, offset = 4 + cp * 33
    records 65536..68607   U+1F000..U+1FBFF (the emoji blocks from the upper
                           font), offset = 4 + (65536 + cp - 0x1F000) * 33

    byte 0     width code: 0 = no glyph, 1 = halfwidth 8x16, 2 = fullwidth 16x16
    bytes 1-32 the bitmap, 16 rows top to bottom, MSB-first (halfwidth uses one
               byte per row in bytes 1-16; the rest stays zero)

GNU Unifont's .hex lines are exactly those bitmaps already (32 hex digits for
halfwidth, 64 for fullwidth), so this is a repack, not a re-render. The font is
(c) Roman Czyborra and the Unifont maintainers, dual-licensed GPLv2+ with the
font-embedding exception / SIL OFL 1.1 — see unifoundry.com/unifont.
"""

import sys

SMP_BASE, SMP_END = 0x1F000, 0x1FBFF  # the supplementary-plane emoji blocks
SMP_COUNT = SMP_END - SMP_BASE + 1


def record_index(cp):
    if cp <= 0xFFFF:
        return cp
    if SMP_BASE <= cp <= SMP_END:
        return 65536 + cp - SMP_BASE
    return None


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: mkunifont.py <unifont.hex> <unifont_upper.hex> <out.bin>")
    table = bytearray(4 + (65536 + SMP_COUNT) * 33)
    table[0:4] = b"AUF2"
    n_half = n_full = 0
    for path in sys.argv[1:3]:
        for line in open(path):
            line = line.strip()
            if not line or ":" not in line:
                continue
            cp_s, bits_s = line.split(":", 1)
            idx = record_index(int(cp_s, 16))
            if idx is None:
                continue
            bits = bytes.fromhex(bits_s)
            off = 4 + idx * 33
            if len(bits) == 16:  # halfwidth 8x16
                table[off] = 1
                table[off + 1 : off + 17] = bits
                n_half += 1
            elif len(bits) == 32:  # fullwidth 16x16
                table[off] = 2
                table[off + 1 : off + 33] = bits
                n_full += 1
    open(sys.argv[3], "wb").write(table)
    print(f"{sys.argv[3]}: {n_half} halfwidth + {n_full} fullwidth glyphs, {len(table)} bytes")


if __name__ == "__main__":
    main()
