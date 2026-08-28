#!/usr/bin/env python3
"""Verify that the published web-installer metadata describes its actual bytes."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path


APP_OFFSET = 0x10000
FONT_OFFSET = 0x340000
FONT_SIZE_AUF2 = 2_264_068
CORE = r"(?:0|[1-9][0-9]*)"
VERSION_RE = re.compile(rf"^({CORE}\.{CORE}\.{CORE}) \(advui · engine ({CORE}\.{CORE}\.{CORE})\)$")
ARCHIVE_RE = re.compile(rf"^v({CORE})\.({CORE})\.({CORE})$")
ARCHIVE_LIMIT = 6


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def archive_key(name: str) -> tuple[int, int, int]:
    """Return the ordering key for one strict stable-release directory."""
    match = ARCHIVE_RE.fullmatch(name)
    if not match:
        raise ValueError(f"unsafe installer archive directory: {name!r}")
    return int(match.group(1)), int(match.group(2)), int(match.group(3))


def validate_manifest(
    manifest_path: Path,
    *,
    factory_path: Path,
    font_path: Path,
    factory_label: str,
    font_label: str,
) -> tuple[dict[str, object], str, str, str]:
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict):
        raise ValueError(f"{manifest_path} is not a JSON object")
    match = VERSION_RE.fullmatch(str(manifest.get("version", "")))
    if not match:
        raise ValueError(f"invalid installer version label in {manifest_path}: {manifest.get('version')!r}")
    release, engine = match.groups()

    builds = manifest.get("builds")
    if not isinstance(builds, list) or len(builds) != 1 or not isinstance(builds[0], dict):
        raise ValueError(f"{manifest_path} must contain exactly one build")
    build = builds[0]
    if build.get("chipFamily") != "ESP32-S3" or not isinstance(build.get("parts"), list):
        raise ValueError(f"{manifest_path} must contain one ESP32-S3 parts list")
    parts = build["parts"]
    expected_parts = {(factory_label, 0), (font_label, FONT_OFFSET)}
    actual_parts = {
        (part.get("path"), part.get("offset"))
        for part in parts
        if isinstance(part, dict)
    }
    if len(parts) != 2 or actual_parts != expected_parts:
        raise ValueError(f"unexpected installer parts in {manifest_path}: {actual_parts!r}")

    image = factory_path.read_bytes()
    if len(image) <= APP_OFFSET or image[APP_OFFSET] != 0xE9:
        raise ValueError(f"{factory_path} does not contain an ESP app at 0x{APP_OFFSET:x}")
    if engine.encode("ascii") not in image[APP_OFFSET:]:
        raise ValueError(f"manifest says engine {engine}, but that version is absent from {factory_path}")

    with font_path.open("rb") as source:
        font_magic = source.read(4)
    if font_path.stat().st_size != FONT_SIZE_AUF2 or font_magic != b"AUF2":
        raise ValueError(f"{font_path} is not the complete {FONT_SIZE_AUF2}-byte AUF2 font")
    return manifest, release, engine, sha256(factory_path)


def archive_directories(versions: Path) -> list[Path]:
    archives: list[Path] = []
    for path in versions.iterdir():
        if path.name == "index.json":
            if path.is_symlink() or not path.is_file():
                raise ValueError("versions/index.json must be a regular file")
            continue
        if path.is_symlink() or not path.is_dir():
            raise ValueError(f"unexpected entry in installer archive: {path.name!r}")
        archive_key(path.name)
        archives.append(path)
    return archives


def prune_archives(docs: Path) -> list[str]:
    """Safely retain the newest installer window and rewrite its exact index."""
    versions = docs.resolve() / "versions"
    archives = sorted(archive_directories(versions), key=lambda path: archive_key(path.name), reverse=True)
    for old in archives[ARCHIVE_LIMIT:]:
        # All candidates were proven to be direct, non-symlink SemVer children
        # before the first deletion. Never feed repository names through a shell.
        if old.parent != versions or not ARCHIVE_RE.fullmatch(old.name):
            raise ValueError(f"refusing unsafe archive removal: {old}")
        shutil.rmtree(old)
    kept = [path.name for path in archives[:ARCHIVE_LIMIT]]
    (versions / "index.json").write_text(json.dumps(kept, indent=2) + "\n")
    return kept


def validate(docs: Path) -> dict[str, str]:
    docs = docs.resolve()
    factory = docs / "firmware.factory.bin"
    font = docs / "unifont.bin"
    manifest, release, engine, current_hash = validate_manifest(
        docs / "manifest.json",
        factory_path=factory,
        font_path=font,
        factory_label="firmware.factory.bin",
        font_label="unifont.bin",
    )

    versions = docs / "versions"
    archive_dirs = archive_directories(versions)
    archive_names = [path.name for path in archive_dirs]
    for name in archive_names:
        archive_key(name)  # reject traversal-ish or otherwise unversioned directories
    expected_index = sorted(archive_names, key=archive_key, reverse=True)
    if not expected_index or len(expected_index) > ARCHIVE_LIMIT:
        raise ValueError(f"installer archive must contain 1..{ARCHIVE_LIMIT} versions")

    index = json.loads((versions / "index.json").read_text())
    if not isinstance(index, list) or any(not isinstance(item, str) for item in index):
        raise ValueError("versions/index.json must be an array of version directory names")
    if len(index) != len(set(index)):
        raise ValueError("versions/index.json contains duplicate versions")
    if index != expected_index:
        raise ValueError(f"versions/index.json is stale or unordered: expected {expected_index!r}")
    if index[0] != f"v{release}":
        raise ValueError(f"current v{release} is not the installer's latest archive")

    for name in index:
        archive = versions / name
        archived_font = archive / "unifont.bin"
        if archived_font.is_file():
            archived_font_label = "unifont.bin"
        else:
            archived_font = font
            archived_font_label = "../../unifont.bin"
        archived_manifest, archived_release, _archived_engine, archived_hash = validate_manifest(
            archive / "manifest.json",
            factory_path=archive / "firmware.factory.bin",
            font_path=archived_font,
            factory_label="firmware.factory.bin",
            font_label=archived_font_label,
        )
        if name != f"v{archived_release}":
            raise ValueError(f"archive {name} contains release {archived_release}")
        if name == f"v{release}":
            if archived_manifest.get("version") != manifest.get("version"):
                raise ValueError("current and archived manifest version labels differ")
            if archived_hash != current_hash:
                raise ValueError("current and archived factory images differ")
    return {"release": release, "engine": engine, "sha256": current_hash}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--prune",
        action="store_true",
        help=f"safely keep the newest {ARCHIVE_LIMIT} archives and regenerate index.json before validation",
    )
    parser.add_argument("docs", nargs="?", type=Path, default=Path("docs"))
    args = parser.parse_args()
    if args.prune:
        prune_archives(args.docs)
    result = validate(args.docs)
    print(f"installer: v{result['release']} engine {result['engine']} sha256={result['sha256']}")


if __name__ == "__main__":
    main()
