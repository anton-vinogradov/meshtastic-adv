#!/usr/bin/env bash
# Build the current firmware and stage it for the web installer.
#
# The installer serves the image SAME-ORIGIN from docs/firmware.factory.bin, because
# GitHub release assets don't send CORS headers and esp-web-tools fetches the binary
# from the Pages origin. So the image is committed under docs/ and deployed via Pages.
#
# Usage: scripts/publish-firmware.sh <version>
#   then review and: git commit && git push   (Pages redeploys the installer)
set -euo pipefail

PIO="${PIO:-$HOME/.pio-core-venv/bin/pio}"
ENV="m5stack-cardputer-adv-advui"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/firmware/.pio/build/$ENV"

VERSION="${1:?usage: publish-firmware.sh <version>}"
VERSION="${VERSION#v}"

echo ">> syncing overlay into the pinned upstream firmware tree"
"$REPO_ROOT/scripts/sync-overlay.sh"

echo ">> building $ENV"
( cd "$REPO_ROOT/firmware" && "$PIO" run -e "$ENV" )

FACTORY=""
shopt -s nullglob
factory_images=("$BUILD_DIR"/firmware-*.factory.bin)
shopt -u nullglob
for image in "${factory_images[@]}"; do
    if [ -z "$FACTORY" ] || [ "$image" -nt "$FACTORY" ]; then
        FACTORY="$image"
    fi
done
[ -n "$FACTORY" ] && [ -f "$FACTORY" ] || { echo "!! factory image not found in $BUILD_DIR"; exit 1; }

cp "$FACTORY" "$REPO_ROOT/docs/firmware.factory.bin"

# Update the current installer and same-origin archive as one transaction. The
# label is derived from the pinned submodule; callers cannot accidentally claim
# a different Meshtastic engine than the image they just built.
python3 - "$REPO_ROOT" "$VERSION" <<'PY'
import configparser
import copy
import json
import pathlib
import re
import shutil
import sys

root = pathlib.Path(sys.argv[1])
version = sys.argv[2]
core = r"(?:0|[1-9][0-9]*)"
if not re.fullmatch(rf"{core}\.{core}\.{core}", version):
    raise SystemExit(f"invalid stable release version: {version!r}")

config = configparser.ConfigParser()
config.read(root / "firmware/version.properties")
engine_parts = config["VERSION"]
engine = ".".join(engine_parts[name] for name in ("major", "minor", "build"))

docs = root / "docs"
manifest_path = docs / "manifest.json"
manifest = json.loads(manifest_path.read_text())
manifest["version"] = f"{version} (advui · engine {engine})"
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

archive = docs / "versions" / f"v{version}"
archive.mkdir(parents=True, exist_ok=True)
shutil.copy2(docs / "firmware.factory.bin", archive / "firmware.factory.bin")
archived_manifest = copy.deepcopy(manifest)
for build in archived_manifest["builds"]:
    for part in build["parts"]:
        if part.get("path") == "unifont.bin":
            part["path"] = "../../unifont.bin"
(archive / "manifest.json").write_text(json.dumps(archived_manifest, indent=2) + "\n")
PY

python3 "$REPO_ROOT/scripts/check-installer.py" --prune "$REPO_ROOT/docs"

git -C "$REPO_ROOT" add docs/firmware.factory.bin docs/manifest.json docs/versions
echo ">> staged docs/firmware.factory.bin ($(du -h "$REPO_ROOT/docs/firmware.factory.bin" | cut -f1)), release=$VERSION"
echo ">> next: git -C $REPO_ROOT commit && git -C $REPO_ROOT push   # deploys the installer via Pages"
