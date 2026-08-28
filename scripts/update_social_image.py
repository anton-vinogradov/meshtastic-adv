#!/usr/bin/env python3
"""Bind social-card URLs in the site HTML to the exact poster bytes."""

import argparse
import hashlib
import re
import sys
from pathlib import Path


SOCIAL_RE = re.compile(
    r'(<meta (?:property="og:image"|name="twitter:image") content=")([^"?]+)(?:\?[^" ]*)?(" />)'
)


def update(html_path: Path, image_path: Path, public_url: str) -> str:
    if not public_url.startswith("https://") or "?" in public_url or "#" in public_url:
        raise RuntimeError("social image URL must be an unqualified HTTPS URL")
    digest = hashlib.sha256(image_path.read_bytes()).hexdigest()
    html = html_path.read_text(encoding="utf-8")
    matches = SOCIAL_RE.findall(html)
    if len(matches) != 2 or {match[1] for match in matches} != {public_url}:
        raise RuntimeError("site must contain exact matching og:image and twitter:image URLs")
    bound = f"{public_url}?sha256={digest}"
    updated, count = SOCIAL_RE.subn(lambda match: match.group(1) + bound + match.group(3), html)
    if count != 2:
        raise RuntimeError("social image metadata replacement was incomplete")
    html_path.write_text(updated, encoding="utf-8")
    return digest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--html", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--url", required=True)
    args = parser.parse_args()
    try:
        digest = update(args.html, args.image, args.url)
    except (OSError, RuntimeError) as error:
        sys.exit(str(error))
    print(digest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
