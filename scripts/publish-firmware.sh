#!/usr/bin/env bash
# Build the current firmware and stage it for the web installer.
#
# The installer serves the image SAME-ORIGIN from docs/firmware.factory.bin, because
# GitHub release assets don't send CORS headers and esp-web-tools fetches the binary
# from the Pages origin. So the image is committed under docs/ and deployed via Pages.
#
# Usage: scripts/publish-firmware.sh <version-label>
#   then review and: git commit && git push   (Pages redeploys the installer)
set -euo pipefail

PIO="${PIO:-$HOME/.pio-core-venv/bin/pio}"
ENV="m5stack-cardputer-adv"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/firmware/.pio/build/$ENV"

VERSION="${1:?usage: publish-firmware.sh <version-label>}"

echo ">> building $ENV"
( cd "$REPO_ROOT/firmware" && "$PIO" run -e "$ENV" )

FACTORY="$(ls -t "$BUILD_DIR"/firmware-*.factory.bin | head -1)"
[ -f "$FACTORY" ] || { echo "!! factory image not found in $BUILD_DIR"; exit 1; }

cp "$FACTORY" "$REPO_ROOT/docs/firmware.factory.bin"

# bump the manifest version label
python3 - "$REPO_ROOT/docs/manifest.json" "$VERSION" <<'PY'
import json, sys
path, ver = sys.argv[1], sys.argv[2]
m = json.load(open(path))
m["version"] = ver
with open(path, "w") as f:
    json.dump(m, f, indent=2)
    f.write("\n")
PY

git -C "$REPO_ROOT" add docs/firmware.factory.bin docs/manifest.json
echo ">> staged docs/firmware.factory.bin ($(du -h "$REPO_ROOT/docs/firmware.factory.bin" | cut -f1)), manifest version=$VERSION"
echo ">> next: git -C $REPO_ROOT commit && git -C $REPO_ROOT push   # deploys the installer via Pages"
