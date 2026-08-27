#!/usr/bin/env python3
"""Wait until the public web installer serves the exact release bytes."""

import argparse
import json
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


def _manifest_parts(payload: bytes, expected_version: str) -> dict[str, int]:
    manifest = json.loads(payload)
    if manifest.get("version") != expected_version:
        raise WebPublishError(
            f"public manifest is {manifest.get('version')!r}, expected {expected_version!r}"
        )
    builds = manifest.get("builds")
    if not isinstance(builds, list):
        raise WebPublishError("public manifest has no builds")
    build = next(
        (item for item in builds if isinstance(item, dict) and item.get("chipFamily") == "ESP32-S3"),
        None,
    )
    if not build or not isinstance(build.get("parts"), list):
        raise WebPublishError("public manifest has no ESP32-S3 parts")
    parts = {
        item.get("path"): item.get("offset")
        for item in build["parts"]
        if isinstance(item, dict) and isinstance(item.get("path"), str)
    }
    expected = {"firmware.factory.bin": 0, "unifont.bin": 0x340000}
    if parts != expected:
        raise WebPublishError(f"public manifest parts differ: {parts!r}")
    return expected


def verify_once(
    *,
    base_url: str,
    version: str,
    firmware: bytes,
    font: bytes,
    attempt: int,
    timeout: float,
) -> None:
    parsed = urlparse(base_url)
    if parsed.scheme != "https" or not parsed.netloc or parsed.username or parsed.password:
        raise WebPublishError("public installer base URL must be credential-free HTTPS")
    origin = (parsed.scheme, parsed.netloc)
    manifest = _download(_public_url(base_url, "manifest.json", version, attempt), origin, timeout)
    _manifest_parts(manifest, version)
    for path, expected in (("firmware.factory.bin", firmware), ("unifont.bin", font)):
        actual = _download(_public_url(base_url, path, version, attempt), origin, timeout)
        if actual != expected:
            raise WebPublishError(f"public {path} bytes differ from the tagged release")


def verify(
    *,
    base_url: str,
    version: str,
    firmware_path: Path,
    font_path: Path,
    attempts: int = 30,
    delay: float = 10,
    timeout: float = 30,
) -> None:
    if attempts < 1 or delay < 0 or timeout <= 0:
        raise WebPublishError("attempts, delay, or timeout is invalid")
    firmware = firmware_path.read_bytes()
    font = font_path.read_bytes()
    if not firmware or not font:
        raise WebPublishError("tagged installer input is empty")

    last_error: Exception | None = None
    for attempt in range(1, attempts + 1):
        try:
            verify_once(
                base_url=base_url,
                version=version,
                firmware=firmware,
                font=font,
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
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--attempts", type=int, default=30)
    parser.add_argument("--delay", type=float, default=10)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    try:
        verify(
            base_url=args.base_url,
            version=args.version,
            firmware_path=args.firmware,
            font_path=args.font,
            attempts=args.attempts,
            delay=args.delay,
            timeout=args.timeout,
        )
    except (OSError, WebPublishError) as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main()
