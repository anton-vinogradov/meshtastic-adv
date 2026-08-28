#!/usr/bin/env python3
"""Verify GitHub release state, notes, and exact asset bytes before publication."""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def normalized_notes(value: str) -> str:
    return value.replace("\r\n", "\n").rstrip("\n")


def verify_release(
    tag: str, title: str, assets: dict[str, Path], notes: Path | None, state: str, prerelease: bool
) -> None:
    if not assets:
        raise RuntimeError("at least one exact release asset is required")
    for name, source in assets.items():
        if Path(name).name != name or not name:
            raise RuntimeError(f"unsafe release asset name: {name!r}")
        if not source.is_file():
            raise RuntimeError(f"local release asset is missing: {source}")

    result = subprocess.run(
        ["gh", "release", "view", tag, "--json", "name,isDraft,isPrerelease,body,assets"],
        check=True,
        capture_output=True,
        text=True,
    )
    payload = json.loads(result.stdout)
    if payload.get("name") != title:
        raise RuntimeError(f"GitHub release {tag} title differs from the exact tag")
    is_draft = payload.get("isDraft") is True
    if state == "draft" and not is_draft:
        raise RuntimeError(f"GitHub release {tag} is already public")
    if state == "public" and is_draft:
        raise RuntimeError(f"GitHub release {tag} is still a draft")
    if payload.get("isPrerelease") is not prerelease:
        raise RuntimeError(f"GitHub release {tag} prerelease state differs")

    remote_names = {item.get("name") for item in payload.get("assets") or []}
    expected_names = set(assets)
    if remote_names != expected_names:
        raise RuntimeError(
            f"GitHub release asset set differs: expected={sorted(expected_names)!r} actual={sorted(remote_names)!r}"
        )
    if notes is not None:
        if not notes.is_file():
            raise RuntimeError(f"local release notes are missing: {notes}")
        if normalized_notes(payload.get("body") or "") != normalized_notes(notes.read_text(encoding="utf-8")):
            raise RuntimeError(f"GitHub release {tag} notes differ from the exact tag")

    with tempfile.TemporaryDirectory(prefix="meshtastic-adv-gh-release-") as directory:
        target = Path(directory)
        for name, source in assets.items():
            subprocess.run(
                ["gh", "release", "download", tag, "--dir", str(target), "--pattern", name],
                check=True,
            )
            downloaded = target / name
            if not downloaded.is_file() or downloaded.is_symlink():
                raise RuntimeError(f"GitHub release asset was not downloaded safely: {name}")
            if downloaded.read_bytes() != source.read_bytes():
                raise RuntimeError(f"GitHub release asset bytes differ: {name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True)
    parser.add_argument("--title", required=True)
    parser.add_argument("--asset", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--notes", type=Path)
    parser.add_argument("--state", choices=("draft", "public", "either"), default="either")
    parser.add_argument("--prerelease", choices=("true", "false"), required=True)
    args = parser.parse_args()

    assets: dict[str, Path] = {}
    try:
        for item in args.asset:
            name, separator, path = item.partition("=")
            if not separator or name in assets:
                raise RuntimeError(f"invalid or duplicate --asset: {item!r}")
            assets[name] = Path(path)
        verify_release(args.tag, args.title, assets, args.notes, args.state, args.prerelease == "true")
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        sys.exit(str(error))
    print(f"GitHub release verified: {args.tag}")
    return 0


if __name__ == "__main__":
    main()
