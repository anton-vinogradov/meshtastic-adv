#!/usr/bin/env python3
"""Bind social-card URLs in the site HTML to the exact poster bytes."""

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


SOCIAL_RE = re.compile(
    r'(<meta (?:property="og:image"|name="twitter:image") content=")([^"?]+)(?:\?[^" ]*)?(" />)'
)
INLINE_MEDIA = (
    (re.compile(r'(src="\./img/hero\.png)(?:\?[^" ]*)?(")'), "poster"),
    (re.compile(r'(data-poster="\./img/hero\.png)(?:\?[^" ]*)?(")'), "poster"),
    (re.compile(r'(data-animation="\./img/demo\.gif)(?:\?[^" ]*)?(")'), "demo"),
)
README_DEMO_RE = re.compile(r'(<img src="docs/img/demo\.gif)(?:\?[^" ]*)?(")')
DURATION_RE = re.compile(r'(data-duration-ms=")[0-9]+(")')
SITE_CODE = (
    (re.compile(r'(href="\./site\.css)(?:\?[^" ]*)?(")'), "stylesheet"),
    (
        re.compile(
            r'(src="\./vendor/esp-web-tools/install-button\.js)(?:\?[^" ]*)?(")'
        ),
        "web_tools",
    ),
    (re.compile(r'(src="\./installer\.js)(?:\?[^" ]*)?(")'), "installer"),
    (re.compile(r'(src="\./site\.js)(?:\?[^" ]*)?(")'), "script"),
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


def story_duration_ms(story_path: Path) -> int:
    payload = json.loads(story_path.read_text(encoding="utf-8"))
    frames = payload.get("frames") if isinstance(payload, dict) else None
    if not isinstance(frames, list) or not frames:
        raise RuntimeError("demo story has no frames")
    delays = [frame.get("delay_ms") for frame in frames if isinstance(frame, dict)]
    if len(delays) != len(frames) or any(not isinstance(delay, int) or delay <= 0 for delay in delays):
        raise RuntimeError("demo story contains an invalid frame delay")
    return sum(delays)


def bind_inline_demo(
    html_path: Path,
    demo_path: Path,
    poster_path: Path,
    duration_ms: int | None = None,
) -> tuple[str, str]:
    digests = {
        "demo": hashlib.sha256(demo_path.read_bytes()).hexdigest(),
        "poster": hashlib.sha256(poster_path.read_bytes()).hexdigest(),
    }
    html = html_path.read_text(encoding="utf-8")
    for pattern, media in INLINE_MEDIA:
        html, count = pattern.subn(
            lambda match: f"{match.group(1)}?sha256={digests[media]}{match.group(2)}",
            html,
        )
        if count != 1:
            raise RuntimeError(f"site must contain exactly one inline {media} binding")
    if duration_ms is not None:
        if duration_ms <= 0:
            raise RuntimeError("demo duration must be positive")
        html, count = DURATION_RE.subn(
            lambda match: f"{match.group(1)}{duration_ms}{match.group(2)}",
            html,
        )
        if count != 1:
            raise RuntimeError("site must contain exactly one demo duration binding")
    html_path.write_text(html, encoding="utf-8")
    return digests["demo"], digests["poster"]


def bind_readme_demo(readme_path: Path, demo_path: Path) -> str:
    digest = hashlib.sha256(demo_path.read_bytes()).hexdigest()
    content = readme_path.read_text(encoding="utf-8")
    updated, count = README_DEMO_RE.subn(
        lambda match: f"{match.group(1)}?sha256={digest}{match.group(2)}",
        content,
    )
    if count != 1:
        raise RuntimeError(f"{readme_path} must contain exactly one inline demo binding")
    readme_path.write_text(updated, encoding="utf-8")
    return digest


def bind_site_code(
    html_path: Path,
    script_path: Path,
    stylesheet_path: Path,
    installer_path: Path,
    web_tools_path: Path,
) -> dict[str, str]:
    digests = {
        "script": hashlib.sha256(script_path.read_bytes()).hexdigest(),
        "stylesheet": hashlib.sha256(stylesheet_path.read_bytes()).hexdigest(),
        "installer": hashlib.sha256(installer_path.read_bytes()).hexdigest(),
        "web_tools": hashlib.sha256(web_tools_path.read_bytes()).hexdigest(),
    }
    html = html_path.read_text(encoding="utf-8")
    for pattern, asset in SITE_CODE:
        html, count = pattern.subn(
            lambda match: f"{match.group(1)}?sha256={digests[asset]}{match.group(2)}",
            html,
        )
        if count != 1:
            raise RuntimeError(f"site must contain exactly one {asset} binding")
    html_path.write_text(html, encoding="utf-8")
    return digests


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--html", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--url", required=True)
    parser.add_argument("--demo", type=Path)
    parser.add_argument("--poster", type=Path)
    parser.add_argument("--story", type=Path)
    parser.add_argument("--site-script", type=Path)
    parser.add_argument("--stylesheet", type=Path)
    parser.add_argument("--installer", type=Path)
    parser.add_argument("--web-tools-entry", type=Path)
    parser.add_argument("--readme", action="append", default=[], type=Path)
    args = parser.parse_args()
    try:
        digest = update(args.html, args.image, args.url)
        if len({args.demo is None, args.poster is None, args.story is None}) != 1:
            raise RuntimeError("--demo, --poster, and --story must be provided together")
        site_inputs = (
            args.site_script,
            args.stylesheet,
            args.installer,
            args.web_tools_entry,
        )
        if len({item is None for item in site_inputs}) != 1:
            raise RuntimeError(
                "--site-script, --stylesheet, --installer, and --web-tools-entry "
                "must be provided together"
            )
        if args.demo is not None and args.poster is not None:
            bind_inline_demo(
                args.html,
                args.demo,
                args.poster,
                story_duration_ms(args.story),
            )
            for readme in args.readme:
                bind_readme_demo(readme, args.demo)
        elif args.readme:
            raise RuntimeError("--readme requires --demo, --poster, and --story")
        if all(item is not None for item in site_inputs):
            bind_site_code(
                args.html,
                args.site_script,
                args.stylesheet,
                args.installer,
                args.web_tools_entry,
            )
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        sys.exit(str(error))
    print(digest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
