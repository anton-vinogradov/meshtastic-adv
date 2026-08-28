#!/usr/bin/env python3
"""Fail unless a remote release tag still peels to the exact workflow commit."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys


TAG_RE = re.compile(r"^v[0-9A-Za-z.-]+$")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")


class RefError(ValueError):
    pass


def parse_remote_refs(output: str, tag: str) -> str:
    direct_name = f"refs/tags/{tag}"
    peeled_name = f"{direct_name}^{{}}"
    refs: dict[str, str] = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) != 2 or fields[1] not in {direct_name, peeled_name}:
            raise RefError(f"unexpected ls-remote result for {tag}: {line!r}")
        sha, name = fields
        if not SHA_RE.fullmatch(sha) or name in refs:
            raise RefError(f"malformed or duplicate remote ref for {tag}")
        refs[name] = sha
    if direct_name not in refs:
        raise RefError(f"remote tag is missing: {tag}")
    return refs.get(peeled_name, refs[direct_name])


def verify(remote: str, tag: str, expected_commit: str) -> str:
    if not TAG_RE.fullmatch(tag) or not SHA_RE.fullmatch(expected_commit):
        raise RefError("unsafe tag or expected commit")
    result = subprocess.run(
        ["git", "ls-remote", remote, f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}"],
        check=True,
        capture_output=True,
        text=True,
    )
    actual = parse_remote_refs(result.stdout, tag)
    if actual != expected_commit:
        raise RefError(f"remote tag {tag} moved: expected {expected_commit}, got {actual}")
    return actual


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remote", default="origin")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    try:
        commit = verify(args.remote, args.tag, args.commit)
    except (OSError, subprocess.CalledProcessError, RefError) as exc:
        print(f"release ref verification failed: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc
    print(f"release ref verified: {args.tag} -> {commit}")


if __name__ == "__main__":
    main()
