#!/usr/bin/env python3
"""Verify GitHub release state, notes, and exact asset bytes before publication."""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


STABLE_TAG_RE = re.compile(
    r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
)


def normalized_notes(value: str) -> str:
    return value.replace("\r\n", "\n").rstrip("\n")


def stable_tuple(tag: str) -> tuple[int, int, int]:
    match = STABLE_TAG_RE.fullmatch(tag)
    if not match:
        raise RuntimeError(f"invalid stable release tag: {tag!r}")
    return tuple(map(int, match.groups()))


def verify_no_newer_public_stable(tag: str, releases: list[dict[str, object]]) -> None:
    target = stable_tuple(tag)
    newer = []
    for item in releases:
        if item.get("draft") is True or item.get("prerelease") is True:
            continue
        candidate = item.get("tag_name")
        match = STABLE_TAG_RE.fullmatch(candidate) if isinstance(candidate, str) else None
        if match and tuple(map(int, match.groups())) > target:
            newer.append(candidate)
    if newer:
        raise RuntimeError(f"newer public stable GitHub release exists: {sorted(newer)!r}")


def public_releases() -> list[dict[str, object]]:
    repository = os.environ.get("GITHUB_REPOSITORY")
    if not repository or not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise RuntimeError("GITHUB_REPOSITORY is missing or unsafe")
    result = subprocess.run(
        [
            "gh", "api", "--paginate", "--slurp",
            f"repos/{repository}/releases?per_page=100",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    pages = json.loads(result.stdout)
    if not isinstance(pages, list) or any(not isinstance(page, list) for page in pages):
        raise RuntimeError("GitHub releases response is malformed")
    return [item for page in pages for item in page if isinstance(item, dict)]


def verify_release(
    tag: str,
    title: str,
    assets: dict[str, Path],
    notes: Path | None,
    state: str,
    prerelease: bool,
    no_newer_stable: bool = False,
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
    if no_newer_stable:
        verify_no_newer_public_stable(tag, public_releases())

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
    if no_newer_stable:
        verify_no_newer_public_stable(tag, public_releases())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True)
    parser.add_argument("--title", required=True)
    parser.add_argument("--asset", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--notes", type=Path)
    parser.add_argument("--state", choices=("draft", "public", "either"), default="either")
    parser.add_argument("--prerelease", choices=("true", "false"), required=True)
    parser.add_argument("--no-newer-stable", action="store_true")
    args = parser.parse_args()

    assets: dict[str, Path] = {}
    try:
        for item in args.asset:
            name, separator, path = item.partition("=")
            if not separator or name in assets:
                raise RuntimeError(f"invalid or duplicate --asset: {item!r}")
            assets[name] = Path(path)
        verify_release(
            args.tag,
            args.title,
            assets,
            args.notes,
            args.state,
            args.prerelease == "true",
            args.no_newer_stable,
        )
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        sys.exit(str(error))
    print(f"GitHub release verified: {args.tag}")
    return 0


if __name__ == "__main__":
    main()
