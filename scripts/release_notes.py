#!/usr/bin/env python3
"""Render one stable release's canonical GitHub notes from CHANGELOG.md."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from release_tag import parse_tag


HEADING_RE = re.compile(r"(?m)^## \[(?P<version>[^]\r\n]+)\](?: - \d{4}-\d{2}-\d{2})?[ \t]*$")


def changelog_notes(changelog: str, version: str) -> str:
    """Extract exactly one version section and its reference comparison link."""
    headings = list(HEADING_RE.finditer(changelog))
    matches = [match for match in headings if match.group("version") == version]
    if len(matches) != 1:
        raise ValueError(f"CHANGELOG.md must contain exactly one [{version}] section")

    current = matches[0]
    following = next((match for match in headings if match.start() > current.start()), None)
    body = changelog[current.end() : following.start() if following else len(changelog)].strip()
    if not body:
        raise ValueError(f"CHANGELOG.md [{version}] section is empty")

    reference_re = re.compile(rf"(?m)^\[{re.escape(version)}\]:[ \t]+(https://[^\s]+)[ \t]*$")
    references = reference_re.findall(changelog)
    if len(references) != 1:
        raise ValueError(f"CHANGELOG.md must contain exactly one [{version}] comparison link")
    return f"{body}\n\n[Full changelog]({references[0]})\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag", help="vMAJOR.MINOR.PATCH[-PRERELEASE]")
    parser.add_argument("--changelog", type=Path, default=Path("CHANGELOG.md"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)

    try:
        version, prerelease = parse_tag(args.tag)
        if prerelease:
            # Prereleases keep GitHub's generated notes. Stable distribution is
            # the point at which a dated, reviewed changelog entry is mandatory.
            return 0
        notes = changelog_notes(args.changelog.read_text(), version)
        if args.output:
            args.output.write_text(notes)
        else:
            sys.stdout.write(notes)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
