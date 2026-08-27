#!/usr/bin/env python3
"""Validate local links, images and heading fragments in public Markdown."""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[1]
MARKDOWN_LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
HTML_LINK_RE = re.compile(r"\b(?:href|src)=[\"']([^\"']+)[\"']", re.IGNORECASE)
HEADING_RE = re.compile(r"^\s{0,3}#{1,6}\s+(.+?)\s*#*\s*$")
INLINE_LINK_RE = re.compile(r"\[([^\]]+)\]\([^)]+\)")
HTML_TAG_RE = re.compile(r"<[^>]+>")


def public_documents(root: Path) -> list[Path]:
    paths = [root / "README.md", root / "README.ru.md", root / "HARDWARE.md", root / "CHANGELOG.md"]
    paths.extend(sorted((root / "hil").glob("*.md")))
    paths.extend(sorted((root / "docs").glob("*.md")))
    paths.append(root / "docs/index.html")
    return [path for path in paths if path.is_file()]


def github_slug(heading: str) -> str:
    text = INLINE_LINK_RE.sub(r"\1", heading)
    text = HTML_TAG_RE.sub("", text)
    text = text.replace("`", "").strip().lower()
    text = re.sub(r"[^\w\- ]", "", text, flags=re.UNICODE)
    return re.sub(r"\s+", "-", text)


def anchors(path: Path) -> set[str]:
    result: set[str] = set()
    counts: dict[str, int] = {}
    fenced = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        match = HEADING_RE.match(line)
        if not match:
            continue
        base = github_slug(match.group(1))
        count = counts.get(base, 0)
        counts[base] = count + 1
        result.add(base if count == 0 else f"{base}-{count}")
    return result


def references(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    return MARKDOWN_LINK_RE.findall(text) + HTML_LINK_RE.findall(text)


def validate(root: Path) -> dict[str, int]:
    root = root.resolve()
    documents = public_documents(root)
    anchor_cache: dict[Path, set[str]] = {}
    failures: list[str] = []
    checked = 0
    for source in documents:
        for raw in references(source):
            target_text = raw.strip().strip("<>")
            if (
                not target_text
                or target_text.startswith("//")
                or re.match(r"^[a-z][a-z0-9+.-]*:", target_text, re.IGNORECASE)
            ):
                continue
            path_text, separator, fragment = target_text.partition("#")
            target = source if not path_text else (source.parent / unquote(path_text)).resolve()
            checked += 1
            try:
                target.relative_to(root)
            except ValueError:
                failures.append(f"{source.relative_to(root)}: local reference escapes repository: {target_text}")
                continue
            if not target.exists():
                failures.append(f"{source.relative_to(root)}: missing {target_text}")
                continue
            if separator and fragment:
                heading_file = target / "README.md" if target.is_dir() else target
                if heading_file.suffix.lower() != ".md":
                    failures.append(f"{source.relative_to(root)}: fragment on non-Markdown target {target_text}")
                    continue
                available = anchor_cache.setdefault(heading_file, anchors(heading_file))
                wanted = unquote(fragment).lower()
                if wanted not in available:
                    failures.append(f"{source.relative_to(root)}: missing anchor #{fragment} in {heading_file.relative_to(root)}")
    if failures:
        raise ValueError("documentation link errors:\n" + "\n".join(failures))
    return {"documents": len(documents), "references": checked}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", type=Path, default=ROOT)
    result = validate(parser.parse_args().root.resolve())
    print(f"docs: {result['documents']} files, {result['references']} local references OK")


if __name__ == "__main__":
    main()
