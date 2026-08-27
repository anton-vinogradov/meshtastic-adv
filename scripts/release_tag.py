#!/usr/bin/env python3
"""Validate a release tag and emit stable GitHub Actions outputs."""

from __future__ import annotations

import re
import sys


CORE = r"(?:0|[1-9][0-9]*)"
IDENTIFIER = r"(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)"
TAG_RE = re.compile(
    rf"^v(?P<version>{CORE}\.{CORE}\.{CORE}(?:-(?P<prerelease>{IDENTIFIER}(?:\.{IDENTIFIER})*))?)$"
)


def parse_tag(tag: str) -> tuple[str, bool]:
    match = TAG_RE.fullmatch(tag)
    if match is None:
        raise ValueError(
            f"invalid release tag {tag!r}; expected vMAJOR.MINOR.PATCH or "
            "vMAJOR.MINOR.PATCH-PRERELEASE"
        )
    return match.group("version"), match.group("prerelease") is not None


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 1:
        print(f"usage: {sys.argv[0]} vMAJOR.MINOR.PATCH[-PRERELEASE]", file=sys.stderr)
        return 2
    try:
        version, prerelease = parse_tag(args[0])
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 2
    print(f"version={version}")
    print(f"prerelease={'true' if prerelease else 'false'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
