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
# docs/, regenerate any time. CI can override all paths with MESHTASTIC_FACTORY,
# MESHTASTIC_FONT and MESHTASTIC_MERGED_OUT. Usage: scripts/mkm5burner.sh [version]
set -euo pipefail
REPO_ROOT="${MESHTASTIC_REPO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$REPO_ROOT"

VER="${1:-$(sed -n 's/.*"version": *"\([0-9.]*\).*/\1/p' docs/manifest.json)}"
FONT_OFFSET=0x340000   # keep in sync with docs/manifest.json + partitions-advui.csv
FACTORY="${MESHTASTIC_FACTORY:-docs/firmware.factory.bin}"
FONT="${MESHTASTIC_FONT:-docs/unifont.bin}"
OUT="${MESHTASTIC_MERGED_OUT:-dist/meshtastic-adv-v${VER}-m5burner.bin}"

for f in "$FACTORY" "$FONT"; do
    [ -f "$f" ] || { echo "missing $f — build the firmware / run mkunifont.py first" >&2; exit 1; }
done

ESPTOOL="${ESPTOOL:-$(command -v esptool || command -v esptool.py || echo "$HOME/.platformio/penv/bin/esptool.py")}"
[ -x "$ESPTOOL" ] || { echo "esptool not found (looked in PATH and PlatformIO penv)" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
rm -f -- "$OUT"
"$ESPTOOL" --chip esp32s3 merge-bin -o "$OUT" \
    --flash-mode keep --flash-size keep \
    0x0 "$FACTORY" "$FONT_OFFSET" "$FONT"

# Prove that this newly-created output contains both exact source artifacts.
# A magic-only check could accept a stale or truncated previous release. Remove
# a newly-created but invalid blob as well: no failed run leaves a publishable-
# looking output behind for a later manual step.
if ! python3 - "$OUT" "$FACTORY" "$FONT" "$FONT_OFFSET" <<'PY'
import sys
out, factory_path, font_path, off = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 0)
b = open(out, 'rb').read()
factory = open(factory_path, 'rb').read()
font = open(font_path, 'rb').read()

def require(condition, message):
    if not condition:
        raise SystemExit(message)

require(bool(b) and b[0] == 0xE9, "bad image: no 0xE9 boot magic at 0x0")
magic = b[off:off+4]
require(magic in (b'AUF1', b'AUF2'), "bad image: no AUF font blob at font offset")
require(len(b) >= off + len(font), "bad image: merged output is truncated")
require(b[:len(factory)] == factory, "bad image: factory bytes differ at offset 0x0")
require(b[off:off+len(font)] == font, "bad image: font bytes differ at its partition offset")
print(f"ok: {out} ({len(b):,} bytes) — exact factory + {magic.decode()} font verified")
PY
then
    rm -f -- "$OUT"
    exit 1
fi
echo "flash at offset 0x0 in M5Burner."
