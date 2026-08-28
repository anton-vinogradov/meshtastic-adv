#!/usr/bin/env python3
"""Wait until the public web installer serves the exact release bytes."""

import argparse
import json
import re
import sys
import time
from pathlib import Path
from urllib.error import URLError
from urllib.parse import urlencode, urljoin, urlparse
from urllib.request import Request, urlopen


class WebPublishError(RuntimeError):
    pass


def _public_url(base_url: str, path: str, version: str, attempt: int) -> str:
    url = urljoin(base_url.rstrip("/") + "/", path)
    query = urlencode({"release": version, "attempt": attempt, "nonce": time.time_ns()})
    return f"{url}?{query}"


def _download(url: str, expected_origin: tuple[str, str], timeout: float) -> bytes:
    request = Request(url, headers={"Cache-Control": "no-cache", "Pragma": "no-cache"})
    with urlopen(request, timeout=timeout) as response:
        final = urlparse(response.geturl())
        if (final.scheme, final.netloc) != expected_origin:
            raise WebPublishError("public installer redirected outside its origin")
        return response.read()


def _manifest_parts(payload: bytes, expected_version: str, *, archive: bool = False) -> tuple[str, str]:
    manifest = json.loads(payload)
    if manifest.get("version") != expected_version:
        raise WebPublishError(
            f"public manifest is {manifest.get('version')!r}, expected {expected_version!r}"
        )
    builds = manifest.get("builds")
    if not isinstance(builds, list) or len(builds) != 1:
        raise WebPublishError("public manifest must contain exactly one build")
    build = builds[0]
    if not build or not isinstance(build.get("parts"), list):
        raise WebPublishError("public manifest has no ESP32-S3 parts")
    if build.get("chipFamily") != "ESP32-S3" or len(build["parts"]) != 2:
        raise WebPublishError("public manifest must contain exactly two ESP32-S3 parts")
    parts = {
        item.get("path"): item.get("offset")
        for item in build["parts"]
        if isinstance(item, dict) and isinstance(item.get("path"), str)
    }
    if parts.get("firmware.factory.bin") != 0 or len(parts) != 2:
        raise WebPublishError(f"public manifest parts differ: {parts!r}")
    font_paths = [path for path, offset in parts.items() if offset == 0x340000]
    allowed_fonts = {"unifont.bin", "../../unifont.bin"} if archive else {"unifont.bin"}
    if len(font_paths) != 1 or font_paths[0] not in allowed_fonts:
        raise WebPublishError(f"public manifest font path differs: {parts!r}")
    return "firmware.factory.bin", font_paths[0]


def verify_once(
    *,
    base_url: str,
    version: str,
    firmware: bytes,
    font: bytes,
    media: dict[str, bytes],
    expected_index: bytes | None,
    archive: bool,
    expected_manifest: bytes | None,
    attempt: int,
    timeout: float,
) -> None:
    parsed = urlparse(base_url)
    if parsed.scheme != "https" or not parsed.netloc or parsed.username or parsed.password:
        raise WebPublishError("public installer base URL must be credential-free HTTPS")
    origin = (parsed.scheme, parsed.netloc)
    manifest = _download(_public_url(base_url, "manifest.json", version, attempt), origin, timeout)
    if expected_manifest is not None and manifest != expected_manifest:
        raise WebPublishError("public manifest bytes differ from the exact site commit")
    firmware_name, font_name = _manifest_parts(manifest, version, archive=archive)
    for path, expected in ((firmware_name, firmware), (font_name, font)):
        actual = _download(_public_url(base_url, path, version, attempt), origin, timeout)
        if actual != expected:
            raise WebPublishError(f"public {path} bytes differ from the tagged release")
    for path, expected in media.items():
        actual = _download(_public_url(base_url, path, version, attempt), origin, timeout)
        if actual != expected:
            raise WebPublishError(f"public {path} bytes differ from the tagged release")
    if not archive:
        if expected_index is None:
            raise WebPublishError("exact root index bytes are required")
        actual = _download(_public_url(base_url, "index.html", version, attempt), origin, timeout)
        if actual != expected_index:
            raise WebPublishError("public index.html bytes differ from the exact site commit")


def verify(
    *,
    base_url: str,
    version: str,
    firmware_path: Path,
    font_path: Path,
    media_paths: dict[str, Path],
    index_path: Path | None = None,
    archive_tag: str | None = None,
    manifest_path: Path | None = None,
    attempts: int = 20,
    delay: float = 5,
    timeout: float = 10,
) -> None:
    if attempts < 1 or delay < 0 or timeout <= 0:
        raise WebPublishError("attempts, delay, or timeout is invalid")
    firmware = firmware_path.read_bytes()
    font = font_path.read_bytes()
    media = {name: path.read_bytes() for name, path in media_paths.items()}
    expected_manifest = manifest_path.read_bytes() if manifest_path is not None else None
    expected_index = index_path.read_bytes() if index_path is not None else None
    if archive_tag is not None:
        if not re.fullmatch(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", archive_tag):
            raise WebPublishError(f"invalid historical archive tag: {archive_tag!r}")
        base_url = urljoin(base_url.rstrip("/") + "/", f"versions/{archive_tag}/")
        media = {}
    if not firmware or not font or (
        archive_tag is None
        and set(media)
        != {
            "img/hero.gif",
            "img/demo.gif",
            "img/hero.png",
            "img/demo-manifest.json",
            "img/m5burner-cover.png",
        }
    ):
        raise WebPublishError("tagged installer input is empty")
    if archive_tag is None and not expected_index:
        raise WebPublishError("exact root index input is empty")
    if any(not value for value in media.values()):
        raise WebPublishError("tagged media input is empty")

    last_error: Exception | None = None
    for attempt in range(1, attempts + 1):
        try:
            verify_once(
                base_url=base_url,
                version=version,
                firmware=firmware,
                font=font,
                media=media,
                expected_index=expected_index,
                archive=archive_tag is not None,
                expected_manifest=expected_manifest,
                attempt=attempt,
                timeout=timeout,
            )
            print(f"public web installer verified: {version}")
            return
        except (OSError, URLError, ValueError, json.JSONDecodeError, WebPublishError) as error:
            last_error = error
            if attempt < attempts:
                time.sleep(delay)
    raise WebPublishError(f"public web installer did not converge: {last_error}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--archive-tag")
    parser.add_argument("--index", type=Path)
    parser.add_argument("--hero", required=True, type=Path)
    parser.add_argument("--demo", required=True, type=Path)
    parser.add_argument("--poster", required=True, type=Path)
    parser.add_argument("--demo-manifest", required=True, type=Path)
    parser.add_argument("--cover", required=True, type=Path)
    parser.add_argument("--attempts", type=int, default=20)
    parser.add_argument("--delay", type=float, default=5)
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()
    try:
        verify(
            base_url=args.base_url,
            version=args.version,
            firmware_path=args.firmware,
            font_path=args.font,
            archive_tag=args.archive_tag,
            manifest_path=args.manifest,
            index_path=args.index,
            media_paths={
                "img/hero.gif": args.hero,
                "img/demo.gif": args.demo,
                "img/hero.png": args.poster,
                "img/demo-manifest.json": args.demo_manifest,
                "img/m5burner-cover.png": args.cover,
            },
            attempts=args.attempts,
            delay=args.delay,
            timeout=args.timeout,
        )
    except (OSError, WebPublishError) as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main()
