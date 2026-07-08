#!/usr/bin/env bash
# Build a single-file firmware image for M5Stack's M5Burner.
#
# The web installer flashes two parts (see docs/manifest.json): the factory
# image at 0x0 and the Unicode font partition at 0x340000. M5Burner flashes one
# blob at 0x0, so we merge both into a single image with esptool. Without this,
# an M5Burner install would boot fine but every non-embedded glyph (CJK, Arabic,
# …) falls back to tofu, since the font partition would be empty.
#
# Output goes to dist/ (gitignored) — it is a pure derivative of the two bins in
# docs/, regenerate any time. Usage: scripts/mkm5burner.sh [version]
set -euo pipefail
cd "$(dirname "$0")/.."

VER="${1:-$(sed -n 's/.*"version": *"\([0-9.]*\).*/\1/p' docs/manifest.json)}"
FONT_OFFSET=0x340000   # keep in sync with docs/manifest.json + partitions-advui.csv
FACTORY=docs/firmware.factory.bin
FONT=docs/unifont.bin
OUT="dist/meshtastic-adv-v${VER}-m5burner.bin"

for f in "$FACTORY" "$FONT"; do
    [ -f "$f" ] || { echo "missing $f — build the firmware / run mkunifont.py first" >&2; exit 1; }
done

ESPTOOL="$(command -v esptool || command -v esptool.py || echo "$HOME/.platformio/penv/bin/esptool.py")"
[ -x "$ESPTOOL" ] || { echo "esptool not found (looked in PATH and PlatformIO penv)" >&2; exit 1; }

mkdir -p dist
"$ESPTOOL" --chip esp32s3 merge-bin -o "$OUT" \
    --flash-mode keep --flash-size keep \
    0x0 "$FACTORY" "$FONT_OFFSET" "$FONT" 2>&1 | grep -vi deprecated || true

# sanity: bootloader magic at 0x0, font magic at the partition offset
python3 - "$OUT" "$FONT_OFFSET" <<'PY'
import sys
out, off = sys.argv[1], int(sys.argv[2], 0)
b = open(out, 'rb').read()
assert b[0] == 0xE9, "bad image: no 0xE9 boot magic at 0x0"
assert b[off:off+4] == b'AUF1', "bad image: no AUF1 font blob at font offset"
print(f"ok: {out} ({len(b):,} bytes) — boot magic + AUF1 verified")
PY
echo "flash at offset 0x0 in M5Burner."
