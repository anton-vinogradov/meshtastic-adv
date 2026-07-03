#!/usr/bin/env bash
# Build the stock/current firmware and publish it as a GitHub Release asset
# named "firmware.factory.bin", which the web installer references via
# .../releases/latest/download/firmware.factory.bin
#
# Usage: scripts/publish-firmware.sh <tag> [release-notes]
set -euo pipefail

GH="${GH:-/opt/homebrew/bin/gh}"
PIO="${PIO:-$HOME/.pio-core-venv/bin/pio}"
ENV="m5stack-cardputer-adv"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/firmware/.pio/build/$ENV"

TAG="${1:?usage: publish-firmware.sh <tag> [release-notes]}"
NOTES="${2:-Firmware build $TAG}"

echo ">> building $ENV"
( cd "$REPO_ROOT/firmware" && "$PIO" run -e "$ENV" )

FACTORY="$(ls -t "$BUILD_DIR"/firmware-*.factory.bin | head -1)"
[ -f "$FACTORY" ] || { echo "!! factory image not found in $BUILD_DIR"; exit 1; }
echo ">> factory image: $FACTORY"

echo ">> creating release $TAG"
# upload with a stable asset name via the path#name syntax
"$GH" release create "$TAG" "$FACTORY#firmware.factory.bin" \
  --repo anton-vinogradov/meshtastic-adv \
  --title "$TAG" \
  --notes "$NOTES"

echo ">> done — installer will serve it at releases/latest/download/firmware.factory.bin"
