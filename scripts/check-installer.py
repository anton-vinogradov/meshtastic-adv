#!/usr/bin/env python3
"""Verify that the published web-installer metadata describes its actual bytes."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


APP_OFFSET = 0x10000
FONT_OFFSET = 0x340000
FONT_SIZE_AUF2 = 2_264_068
VERSION_RE = re.compile(
    r"^(\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?) \(advui · engine (\d+\.\d+\.\d+)\)$"
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(docs: Path) -> dict[str, str]:
    manifest_path = docs / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    match = VERSION_RE.fullmatch(str(manifest.get("version", "")))
    if not match:
        raise ValueError(f"invalid installer version label: {manifest.get('version')!r}")
    release, engine = match.groups()

    parts = [part for build in manifest.get("builds", []) for part in build.get("parts", [])]
    app_parts = [part for part in parts if part.get("offset") == 0]
    if len(app_parts) != 1:
        raise ValueError(f"expected one offset-0 factory part, found {len(app_parts)}")
    factory = docs / str(app_parts[0].get("path", ""))
    image = factory.read_bytes()
    if len(image) <= APP_OFFSET or image[APP_OFFSET] != 0xE9:
        raise ValueError(f"{factory} does not contain an ESP app at 0x{APP_OFFSET:x}")
    if engine.encode("ascii") not in image[APP_OFFSET:]:
        raise ValueError(f"manifest says engine {engine}, but that version is absent from {factory}")

    font_parts = [part for part in parts if part.get("offset") == FONT_OFFSET]
    if len(font_parts) != 1:
        raise ValueError(f"expected one font part at 0x{FONT_OFFSET:x}, found {len(font_parts)}")
    font = docs / str(font_parts[0].get("path", ""))
    with font.open("rb") as source:
        font_magic = source.read(4)
    if font.stat().st_size != FONT_SIZE_AUF2 or font_magic != b"AUF2":
        raise ValueError(f"{font} is not the complete {FONT_SIZE_AUF2}-byte AUF2 font")

    archive = docs / "versions" / f"v{release}"
    archived_factory = archive / "firmware.factory.bin"
    archived_manifest = json.loads((archive / "manifest.json").read_text())
    if archived_manifest.get("version") != manifest.get("version"):
        raise ValueError("current and archived manifest version labels differ")
    current_hash = sha256(factory)
    if sha256(archived_factory) != current_hash:
        raise ValueError("current and archived factory images differ")
    archived_parts = [
        part for build in archived_manifest.get("builds", []) for part in build.get("parts", [])
    ]
    archived_fonts = [part for part in archived_parts if part.get("offset") == FONT_OFFSET]
    if len(archived_fonts) != 1:
        raise ValueError("archived manifest does not contain exactly one font part")
    archived_font = (archive / str(archived_fonts[0].get("path", ""))).resolve()
    if archived_font != font.resolve():
        raise ValueError("archived manifest does not reuse the site-root Unicode font")

    index = json.loads((docs / "versions" / "index.json").read_text())
    if f"v{release}" not in index:
        raise ValueError(f"v{release} is missing from versions/index.json")
    return {"release": release, "engine": engine, "sha256": current_hash}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("docs", nargs="?", type=Path, default=Path("docs"))
    result = validate(parser.parse_args().docs)
    print(f"installer: v{result['release']} engine {result['engine']} sha256={result['sha256']}")


if __name__ == "__main__":
    main()
