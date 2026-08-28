#!/usr/bin/env python3
"""Classify a stable site update without allowing historical rerun rollback."""

import argparse
import json
import re
import sys
from pathlib import Path


CORE_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
LABEL_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:\s|$)")
ARCHIVE_RE = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
RETENTION_LIMIT = 6


def target_tuple(value: str) -> tuple[int, int, int]:
    match = CORE_RE.fullmatch(value)
    if not match:
        raise ValueError(f"invalid target stable version: {value!r}")
    return tuple(map(int, match.groups()))


def current_tuple(manifest: Path) -> tuple[int, int, int]:
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("installer manifest is not an object")
    match = LABEL_RE.match(str(payload.get("version", "")))
    if not match:
        raise ValueError(f"invalid current installer version: {payload.get('version')!r}")
    return tuple(map(int, match.groups()))


def published_tuples(
    manifest: Path, archives: Path
) -> tuple[tuple[int, int, int], set[tuple[int, int, int]]]:
    if archives.is_symlink() or not archives.is_dir():
        raise ValueError("installer archive directory is missing or unsafe")
    root_version = current_tuple(manifest)
    seen: set[tuple[int, int, int]] = set()
    for child in sorted(archives.iterdir()):
        if child.name == "index.json":
            if child.is_symlink() or not child.is_file():
                raise ValueError("installer archive index is missing or unsafe")
            continue
        if child.is_symlink():
            raise ValueError(f"installer archive entry is a symlink: {child.name}")
        if not child.is_dir():
            raise ValueError(f"unexpected installer archive entry: {child.name!r}")
        match = ARCHIVE_RE.fullmatch(child.name)
        if not match:
            raise ValueError(f"invalid installer archive directory: {child.name!r}")
        folder_version = tuple(map(int, match.groups()))
        if folder_version in seen:
            raise ValueError(f"duplicate installer archive version: {child.name}")
        archive_manifest = child / "manifest.json"
        if archive_manifest.is_symlink() or not archive_manifest.is_file():
            raise ValueError(f"installer archive manifest is missing or unsafe: {child.name}")
        manifest_version = current_tuple(archive_manifest)
        if manifest_version != folder_version:
            raise ValueError(
                f"installer archive folder/manifest version mismatch: {child.name}"
            )
        seen.add(folder_version)
    return root_version, seen


def highest_published_tuple(manifest: Path, archives: Path) -> tuple[int, int, int]:
    root_version, archived = published_tuples(manifest, archives)
    return max({root_version, *archived})


def release_mode(
    manifest: Path,
    archives: Path,
    version: str,
    retention_limit: int = RETENTION_LIMIT,
) -> str:
    if retention_limit < 1:
        raise ValueError("installer archive retention limit must be positive")
    root_version, archived = published_tuples(manifest, archives)
    current = max({root_version, *archived})
    target = target_tuple(version)
    if current > target:
        if target in archived:
            return "historical"
        # A missing old target is proven pruned only when the retained window is
        # full. Before that, absence is corruption and must fail closed.
        if len(archived) == retention_limit and target < min(archived):
            return "retired"
        raise ValueError("historical installer archive is missing inside the retained window")
    if current == target:
        return "repair"
    return "promote"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--archives", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--retention-limit", type=int, default=RETENTION_LIMIT)
    parser.add_argument("--output", choices=("mode", "latest-tag"), default="mode")
    args = parser.parse_args()
    try:
        mode = release_mode(
            args.manifest, args.archives, args.version, args.retention_limit
        )
        if args.output == "latest-tag":
            latest = max(
                highest_published_tuple(args.manifest, args.archives),
                target_tuple(args.version),
            )
            print("v" + ".".join(map(str, latest)))
        else:
            print(mode)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
