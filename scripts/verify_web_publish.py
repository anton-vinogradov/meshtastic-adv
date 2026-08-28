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


SAFE_WEB_ASSET_PART_RE = re.compile(r"^[A-Za-z0-9_.-]+$")


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
    expected_versions_index: bytes | None,
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
        if expected_versions_index is not None:
            actual = _download(
                _public_url(base_url, "versions/index.json", version, attempt), origin, timeout
            )
            if actual != expected_versions_index:
                raise WebPublishError("public versions/index.json bytes differ from the exact site commit")


def verify(
    *,
    base_url: str,
    version: str,
    firmware_path: Path,
    font_path: Path,
    media_paths: dict[str, Path],
    index_path: Path | None = None,
    versions_index_path: Path | None = None,
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
    expected_versions_index = (
        versions_index_path.read_bytes() if versions_index_path is not None else None
    )
    if archive_tag is not None:
        if not re.fullmatch(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", archive_tag):
            raise WebPublishError(f"invalid historical archive tag: {archive_tag!r}")
        base_url = urljoin(base_url.rstrip("/") + "/", f"versions/{archive_tag}/")
        media = {}
    required_media = {
        "img/hero.gif",
        "img/demo.gif",
        "img/hero.png",
        "img/demo-manifest.json",
        "img/m5burner-cover.png",
    }
    required_site_code = {
        "site.js",
        "site.css",
        "installer.js",
        "vendor/esp-web-tools/install-button.js",
    }
    actual_paths = set(media)
    site_code_paths = actual_paths - required_media
    complete_site_code = required_site_code <= site_code_paths and all(
        path in required_site_code or path.startswith("vendor/esp-web-tools/")
        for path in site_code_paths
    )
    if not firmware or not font or (
        archive_tag is None
        and actual_paths != required_media
        and not (required_media < actual_paths and complete_site_code)
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
                expected_versions_index=expected_versions_index,
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


def web_tools_media_paths(directory: Path) -> dict[str, Path]:
    if not directory.is_dir() or directory.is_symlink():
        raise WebPublishError("vendored ESP Web Tools directory is missing or unsafe")
    result: dict[str, Path] = {}
    for path in sorted(directory.rglob("*")):
        if path.is_symlink():
            raise WebPublishError(f"symlink in vendored ESP Web Tools: {path}")
        if path.is_dir():
            continue
        relative = path.relative_to(directory)
        if not path.is_file() or any(
            not SAFE_WEB_ASSET_PART_RE.fullmatch(part) for part in relative.parts
        ):
            raise WebPublishError(f"unsafe vendored ESP Web Tools path: {relative}")
        public_path = "vendor/esp-web-tools/" + relative.as_posix()
        result[public_path] = path
    if "vendor/esp-web-tools/install-button.js" not in result:
        raise WebPublishError("vendored ESP Web Tools entrypoint is missing")
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--archive-tag")
    parser.add_argument("--index", type=Path)
    parser.add_argument("--versions-index", type=Path)
    parser.add_argument("--hero", required=True, type=Path)
    parser.add_argument("--demo", required=True, type=Path)
    parser.add_argument("--poster", required=True, type=Path)
    parser.add_argument("--demo-manifest", required=True, type=Path)
    parser.add_argument("--cover", required=True, type=Path)
    parser.add_argument("--site-script", type=Path)
    parser.add_argument("--stylesheet", type=Path)
    parser.add_argument("--installer", type=Path)
    parser.add_argument("--web-tools-dir", type=Path)
    parser.add_argument("--attempts", type=int, default=20)
    parser.add_argument("--delay", type=float, default=5)
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()
    try:
        site_inputs = (
            args.site_script,
            args.stylesheet,
            args.installer,
            args.web_tools_dir,
        )
        if len({item is None for item in site_inputs}) != 1:
            raise WebPublishError(
                "--site-script, --stylesheet, --installer, and --web-tools-dir "
                "must be provided together"
            )
        media_paths = {
            "img/hero.gif": args.hero,
            "img/demo.gif": args.demo,
            "img/hero.png": args.poster,
            "img/demo-manifest.json": args.demo_manifest,
            "img/m5burner-cover.png": args.cover,
        }
        if all(item is not None for item in site_inputs):
            media_paths["site.js"] = args.site_script
            media_paths["site.css"] = args.stylesheet
            media_paths["installer.js"] = args.installer
            media_paths.update(web_tools_media_paths(args.web_tools_dir))
        verify(
            base_url=args.base_url,
            version=args.version,
            firmware_path=args.firmware,
            font_path=args.font,
            archive_tag=args.archive_tag,
            manifest_path=args.manifest,
            index_path=args.index,
            versions_index_path=args.versions_index,
            media_paths=media_paths,
            attempts=args.attempts,
            delay=args.delay,
            timeout=args.timeout,
        )
    except (OSError, WebPublishError) as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main()
