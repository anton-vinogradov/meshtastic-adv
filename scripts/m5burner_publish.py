#!/usr/bin/env python3
"""Idempotently upload, publish, and verify one M5Burner release."""

import argparse
from contextlib import ExitStack
import hashlib
import io
import os
import re
import struct
import sys
import time
from pathlib import Path
from urllib.parse import urlparse

import requests

FID = "78f98d6ec06fe700fa9edced6434a3f0"
API = "https://m5burner-api.m5stack.com"
PUBLIC_API = f"{API}/api/firmware"
LOGIN_API = "https://uiflow2.m5stack.com/api/v1/account/login"
CDN = "https://m5burner-cdn.m5stack.com/firmware"
COVER_CDN = "https://m5burner-cdn.m5stack.com/cover"
FILE_ID_RE = re.compile(r"^[0-9A-Za-z._-]+\.bin$")
COVER_ID_RE = re.compile(r"^[0-9A-Za-z._-]+\.png$")
STABLE_VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
EXPECTED_LISTING_FIELDS = {
    "category": "cardputer",
    "author": "randoom",
    "github": "https://github.com/anton-vinogradov/meshtastic-adv",
}


def firmware_by_fid(payload):
    return next((firmware for firmware in payload if firmware.get("fid") == FID), None)


def require_response(response, operation):
    status = getattr(response, "status_code", 200)
    if 300 <= status < 400:
        raise RuntimeError(f"M5Burner {operation} attempted an HTTP redirect")
    response.raise_for_status()
    return response


def version_by_name(firmware, version):
    if not firmware:
        return None
    return next((item for item in firmware.get("versions") or [] if item.get("version") == version), None)


def exact_version_by_name(firmware, version):
    matches = [item for item in (firmware or {}).get("versions") or [] if item.get("version") == version]
    if len(matches) > 1:
        raise RuntimeError(f"M5Burner contains duplicate version entries for {version}")
    return matches[0] if matches else None


def stable_version_tuple(value):
    match = STABLE_VERSION_RE.fullmatch(str(value))
    return tuple(map(int, match.groups())) if match else None


def get_admin_firmware(session):
    response = session.get(f"{API}/api/admin/firmware", timeout=30, allow_redirects=False)
    require_response(response, "admin read")
    firmware = firmware_by_fid(response.json())
    if not firmware:
        raise RuntimeError(f"firmware {FID} not found in the account")
    return firmware


def get_public_firmware():
    # Never reuse the authenticated admin session for a credential-free read.
    response = requests.get(PUBLIC_API, timeout=30, allow_redirects=False)
    require_response(response, "public catalog read")
    firmware = firmware_by_fid(response.json())
    if not firmware:
        raise RuntimeError(f"public firmware {FID} not found")
    return firmware


def parse_listing_copy(path):
    text = path.read_text(encoding="utf-8")
    if "\r" in text or text.count("\n---\n") != 1:
        raise RuntimeError("M5Burner listing copy must contain exactly one LF-only --- separator")
    preface, body = text.split("\n---\n")
    first = preface.splitlines()[0] if preface else ""
    if not first.startswith("Title: ") or not first[7:]:
        raise RuntimeError("M5Burner listing copy must start with a non-empty Title: line")
    body = body.rstrip("\n")
    if not body:
        raise RuntimeError("M5Burner listing body is empty")
    return first[7:], body


def verify_listing_metadata(firmware, title, description):
    expected = {"fid": FID, "name": title, "description": description, **EXPECTED_LISTING_FIELDS}
    drift = [key for key, value in expected.items() if firmware.get(key) != value]
    if drift:
        raise RuntimeError(
            "M5Burner listing metadata differs from reviewed copy "
            f"({', '.join(drift)}); paste docs/m5burner-card.txt in the account and rerun"
        )


def version_form(firmware, version, *, include_cover):
    data = {
        "name": firmware.get("name", ""),
        "description": firmware.get("description", ""),
        "category": firmware.get("category", ""),
        "author": firmware.get("author", ""),
        "version": version,
        "github": firmware.get("github", ""),
    }
    if include_cover:
        data["cover"] = "null"
    return data


def validate_cover(path):
    if path.stat().st_size > 2_000_000:
        raise RuntimeError("M5Burner cover exceeds 2 MB")
    header = path.read_bytes()[:24]
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise RuntimeError("M5Burner cover is not a PNG")
    if struct.unpack(">II", header[16:24]) != (1000, 1000):
        raise RuntimeError("M5Burner cover must be exactly 1000x1000")


def version_files(stack, binary, cover):
    files = {"firmware": stack.enter_context(open(binary, "rb"))}
    if cover is not None:
        files["cover"] = stack.enter_context(open(cover, "rb"))
    return files


def upload_version(session, firmware, version, binary, cover=None):
    data = version_form(firmware, version, include_cover=cover is None)
    with ExitStack() as stack:
        response = session.post(
            f"{API}/api/admin/firmware", data=data, files=version_files(stack, binary, cover), timeout=300,
            allow_redirects=False,
        )
    require_response(response, "upload")


def update_version(session, firmware, file_id, version, binary, cover=None):
    """Replace a private pending version; never call this for a public item."""
    if not isinstance(file_id, str) or not FILE_ID_RE.fullmatch(file_id):
        raise RuntimeError(f"unsafe M5Burner file id: {file_id!r}")
    data = version_form(firmware, version, include_cover=False)
    with ExitStack() as stack:
        response = session.put(
            f"{API}/api/admin/firmware/{FID}/version/{file_id}",
            data=data,
            files=version_files(stack, binary, cover),
            timeout=300,
            allow_redirects=False,
        )
    require_response(response, "update")
    if response.json().get("status") != 1:
        raise RuntimeError(f"M5Burner update rejected pending version {version}")


def publish_version(session, file_id):
    response = session.put(
        f"{API}/api/admin/firmware/{FID}/publish/{file_id}/1",
        timeout=30,
        allow_redirects=False,
    )
    require_response(response, "publish")
    if response.json().get("status") != 1:
        raise RuntimeError(f"M5Burner publish rejected file {file_id}")


def verify_public_version(version, file_id, attempts=6, delay=5):
    for attempt in range(attempts):
        public = exact_version_by_name(get_public_firmware(), version)
        if public and public.get("published") is True and public.get("file") == file_id:
            return
        if attempt + 1 < attempts:
            time.sleep(delay)
    raise RuntimeError(f"M5Burner public catalog does not contain published version {version}")


def file_sha256(path):
    digest = hashlib.sha256()
    size = 0
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            size += len(chunk)
            digest.update(chunk)
    return size, digest.hexdigest()


def verify_public_asset(asset_id, source, *, pattern, cdn, label, attempts=6, delay=5):
    """Prove that the credential-free CDN serves the exact uploaded bytes."""
    if not isinstance(asset_id, str) or not pattern.fullmatch(asset_id):
        raise RuntimeError(f"unsafe M5Burner {label} id: {asset_id!r}")
    expected_size, expected_hash = file_sha256(source)
    # Avoid a stale edge response if an admin update preserved the file id.
    url = f"{cdn}/{asset_id}?sha256={expected_hash}"
    expected_origin = ("https", "m5burner-cdn.m5stack.com")
    last_error = None
    for attempt in range(attempts):
        try:
            # Deliberately do not use the authenticated admin session here: its
            # m5_auth_token header must never be disclosed to the public CDN.
            with requests.get(
                url,
                headers={"Cache-Control": "no-cache", "Pragma": "no-cache"},
                timeout=30,
                stream=True,
            ) as response:
                response.raise_for_status()
                final = urlparse(response.url)
                if (final.scheme, final.netloc) != expected_origin:
                    raise RuntimeError(f"M5Burner {label} redirected outside the public CDN origin")
                digest = hashlib.sha256()
                size = 0
                for chunk in response.iter_content(chunk_size=1024 * 1024):
                    if chunk:
                        size += len(chunk)
                        digest.update(chunk)
            if size == expected_size and digest.hexdigest() == expected_hash:
                return
            last_error = RuntimeError(
                f"public M5Burner {label} bytes differ: size={size}, sha256={digest.hexdigest()}"
            )
        except (RuntimeError, requests.RequestException) as error:
            last_error = error
        if attempt + 1 < attempts:
            time.sleep(delay)
    raise RuntimeError(f"M5Burner public {label} did not converge: {last_error}")


def verify_public_binary(file_id, binary, attempts=6, delay=5):
    verify_public_asset(
        file_id, binary, pattern=FILE_ID_RE, cdn=CDN, label="binary", attempts=attempts, delay=delay
    )


def verify_public_cover(cover_id, cover, attempts=6, delay=5):
    """Prove the public cover is visually exact, allowing lossless PNG rewrites."""
    if not isinstance(cover_id, str) or not COVER_ID_RE.fullmatch(cover_id):
        raise RuntimeError(f"unsafe M5Burner cover id: {cover_id!r}")
    expected = cover.read_bytes()
    expected_size, expected_hash = file_sha256(cover)
    url = f"{COVER_CDN}/{cover_id}?sha256={expected_hash}"
    expected_origin = ("https", "m5burner-cdn.m5stack.com")
    last_error = None
    for attempt in range(attempts):
        try:
            with requests.get(
                url,
                headers={"Cache-Control": "no-cache", "Pragma": "no-cache"},
                timeout=30,
                stream=True,
            ) as response:
                response.raise_for_status()
                final = urlparse(response.url)
                if (final.scheme, final.netloc) != expected_origin:
                    raise RuntimeError("M5Burner cover redirected outside the public CDN origin")
                public = bytearray()
                for chunk in response.iter_content(chunk_size=1024 * 1024):
                    if chunk:
                        public.extend(chunk)
                        if len(public) > 2_000_000:
                            raise RuntimeError("public M5Burner cover exceeds 2 MB")
            public = bytes(public)
            if public == expected:
                return
            expected_pixels = normalized_cover_pixels(expected, "uploaded")
            public_pixels = normalized_cover_pixels(public, "public")
            if public_pixels == expected_pixels:
                return
            last_error = RuntimeError(
                "public M5Burner cover pixels differ: "
                f"size={len(public)}, sha256={hashlib.sha256(public).hexdigest()}, "
                f"pixel_sha256={public_pixels}; expected_size={expected_size}, "
                f"expected_sha256={expected_hash}, expected_pixel_sha256={expected_pixels}"
            )
        except (OSError, RuntimeError, requests.RequestException) as error:
            last_error = error
        if attempt + 1 < attempts:
            time.sleep(delay)
    raise RuntimeError(f"M5Burner public cover did not converge: {last_error}")


def normalized_cover_pixels(payload, label):
    """Return a lossless digest of dimensions plus normalized RGBA pixels."""
    try:
        from PIL import Image, UnidentifiedImageError
    except ImportError as error:
        raise RuntimeError(
            "Pillow is required to verify a byte-different public M5Burner cover"
        ) from error
    try:
        with Image.open(io.BytesIO(payload)) as image:
            if image.format != "PNG" or getattr(image, "n_frames", 1) != 1:
                raise RuntimeError(f"{label} M5Burner cover is not a single-frame PNG")
            if image.size != (1000, 1000):
                raise RuntimeError(f"{label} M5Burner cover is not 1000x1000")
            rgba = image.convert("RGBA")
            pixels = rgba.tobytes()
    except (OSError, UnidentifiedImageError) as error:
        raise RuntimeError(f"{label} M5Burner cover cannot be decoded") from error
    digest = hashlib.sha256(struct.pack(">II", 1000, 1000))
    digest.update(pixels)
    return digest.hexdigest()


def publish(session, *, email, password, version, binary, cover, listing_title, listing_description):
    if cover is not None:
        validate_cover(cover)
    target_version = stable_version_tuple(version)
    if target_version is None:
        raise RuntimeError(f"invalid stable M5Burner version: {version!r}")

    # Decide against the credential-free catalog before the session contains a
    # login cookie/header. Global listing metadata and cover describe the latest
    # release, so an already-public historical rerun deliberately ignores them.
    public_firmware = get_public_firmware()
    historical = any(
        entry.get("published") is True
        and (parsed := stable_version_tuple(entry.get("version"))) is not None
        and parsed > target_version
        for entry in public_firmware.get("versions") or []
    )
    if not historical:
        if cover is None:
            raise RuntimeError("latest M5Burner release requires a reviewed cover")
        verify_listing_metadata(public_firmware, listing_title, listing_description)

    response = session.post(
        LOGIN_API,
        json={"email": email, "password": password},
        timeout=30,
        allow_redirects=False,
    )
    require_response(response, "login")
    token = session.cookies.get("m5_auth_token")
    if not token:
        raise RuntimeError("login succeeded without m5_auth_token cookie")
    session.headers.update({"m5_auth_token": token})

    firmware = get_admin_firmware(session)
    item = exact_version_by_name(firmware, version)
    newer_published = any(
        entry.get("published") is True
        and (parsed := stable_version_tuple(entry.get("version"))) is not None
        and parsed > target_version
        for entry in firmware.get("versions") or []
    )
    if newer_published != historical:
        raise RuntimeError("M5Burner catalog changed while establishing release order")
    if not historical:
        verify_listing_metadata(firmware, listing_title, listing_description)
    if newer_published and (not item or item.get("published") is not True):
        raise RuntimeError(
            f"refusing to publish historical M5Burner version {version} after a newer stable release"
        )
    if not item:
        upload_version(session, firmware, version, binary, cover)
        firmware = get_admin_firmware(session)
        item = exact_version_by_name(firmware, version)
        if not item:
            raise RuntimeError(f"uploaded version {version} is absent from the account")
    elif item.get("published") is not True:
        old_file_id = item.get("file")
        if not old_file_id:
            raise RuntimeError(f"pending version {version} has no uploaded file")
        # A retry can encounter a stale half-upload bearing the right version.
        # Replace it while it is still private, then re-fetch because M5Burner
        # may assign a new immutable file id to the replacement.
        update_version(session, firmware, old_file_id, version, binary, cover)
        firmware = get_admin_firmware(session)
        item = exact_version_by_name(firmware, version)
        if not item or item.get("published") is True:
            raise RuntimeError(f"pending version {version} changed state during update")

    file_id = item.get("file")
    if not file_id:
        raise RuntimeError(f"version {version} has no uploaded file")
    if item.get("published") is not True:
        # Prove the private CDN object before making it visible. This is also a
        # post-upload integrity gate for brand-new versions.
        verify_public_binary(file_id, binary)
        publish_version(session, file_id)
        firmware = get_admin_firmware(session)
        item = exact_version_by_name(firmware, version)
        if not item or item.get("published") is not True or item.get("file") != file_id:
            raise RuntimeError(f"version {version} did not become published in the account")

    verify_public_version(version, file_id)
    public_firmware = get_public_firmware()
    if not historical:
        verify_listing_metadata(public_firmware, listing_title, listing_description)
        # M5Burner stages a pending version's cover privately and only promotes
        # it when the version is published. Fetch the post-publish id instead of
        # comparing the still-public previous cover before publication.
        cover_id = public_firmware.get("cover")
        if not isinstance(cover_id, str) or not COVER_ID_RE.fullmatch(cover_id):
            raise RuntimeError(f"unsafe M5Burner cover id: {cover_id!r}")
        verify_public_cover(cover_id, cover)
        if any(
            entry.get("published") is True
            and (parsed := stable_version_tuple(entry.get("version"))) is not None
            and parsed > target_version
            for entry in public_firmware.get("versions") or []
        ):
            raise RuntimeError("a newer M5Burner stable appeared during publication")
    verify_public_binary(file_id, binary)
    return file_id


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--email", default=os.environ.get("M5B_EMAIL"))
    parser.add_argument("--password", default=os.environ.get("M5B_PWD"))
    parser.add_argument("--version", help="version string without leading v")
    parser.add_argument("--bin", type=Path, help="merged image to upload")
    parser.add_argument("--cover", type=Path, help="1000x1000 PNG listing cover")
    parser.add_argument("--listing", required=True, type=Path, help="reviewed M5Burner title/body copy")
    parser.add_argument("--check-listing-only", action="store_true")
    args = parser.parse_args()

    try:
        listing_title, listing_description = parse_listing_copy(args.listing)
        if args.check_listing_only:
            verify_listing_metadata(get_public_firmware(), listing_title, listing_description)
            print("M5Burner public listing matches the reviewed copy")
            return
        if not args.email or not args.password:
            raise RuntimeError("M5Burner credentials are required via environment or CLI")
        if args.bin is None or args.cover is None or not args.version:
            raise RuntimeError("M5Burner publish requires --version, --bin, and --cover")
        file_id = publish(
            requests.Session(),
            email=args.email,
            password=args.password,
            version=args.version,
            binary=args.bin,
            cover=args.cover,
            listing_title=listing_title,
            listing_description=listing_description,
        )
    except (OSError, RuntimeError, requests.RequestException) as error:
        sys.exit(str(error))
    print(f"published {args.version} to M5Burner as {file_id}")


if __name__ == "__main__":
    main()
