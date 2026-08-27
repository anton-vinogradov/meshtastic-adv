#!/usr/bin/env python3
"""Idempotently upload, publish, and verify one M5Burner release."""

import argparse
import sys
import time

import requests

FID = "78f98d6ec06fe700fa9edced6434a3f0"
API = "https://m5burner-api.m5stack.com"
PUBLIC_API = f"{API}/api/firmware"
LOGIN_API = "https://uiflow2.m5stack.com/api/v1/account/login"


def firmware_by_fid(payload):
    return next((firmware for firmware in payload if firmware.get("fid") == FID), None)


def version_by_name(firmware, version):
    if not firmware:
        return None
    return next((item for item in firmware.get("versions") or [] if item.get("version") == version), None)


def get_admin_firmware(session):
    response = session.get(f"{API}/api/admin/firmware", timeout=30)
    response.raise_for_status()
    firmware = firmware_by_fid(response.json())
    if not firmware:
        raise RuntimeError(f"firmware {FID} not found in the account")
    return firmware


def upload_version(session, firmware, version, binary):
    data = {
        "name": firmware.get("name", ""),
        "description": firmware.get("description", ""),
        "category": firmware.get("category", ""),
        "author": firmware.get("author", ""),
        "version": version,
        "github": firmware.get("github", ""),
        "cover": "null",
    }
    with open(binary, "rb") as source:
        response = session.post(
            f"{API}/api/admin/firmware", data=data, files={"firmware": source}, timeout=300
        )
    response.raise_for_status()


def publish_version(session, file_id):
    response = session.put(f"{API}/api/admin/firmware/{FID}/publish/{file_id}/1", timeout=30)
    response.raise_for_status()
    if response.json().get("status") != 1:
        raise RuntimeError(f"M5Burner publish rejected file {file_id}")


def verify_public_version(session, version, file_id, attempts=6, delay=5):
    for attempt in range(attempts):
        response = session.get(PUBLIC_API, timeout=30)
        response.raise_for_status()
        public = version_by_name(firmware_by_fid(response.json()), version)
        if public and public.get("published") is True and public.get("file") == file_id:
            return
        if attempt + 1 < attempts:
            time.sleep(delay)
    raise RuntimeError(f"M5Burner public catalog does not contain published version {version}")


def publish(session, *, email, password, version, binary):
    response = session.post(LOGIN_API, json={"email": email, "password": password}, timeout=30)
    response.raise_for_status()
    token = session.cookies.get("m5_auth_token")
    if not token:
        raise RuntimeError("login succeeded without m5_auth_token cookie")
    session.headers.update({"m5_auth_token": token})

    firmware = get_admin_firmware(session)
    item = version_by_name(firmware, version)
    if not item:
        upload_version(session, firmware, version, binary)
        firmware = get_admin_firmware(session)
        item = version_by_name(firmware, version)
        if not item:
            raise RuntimeError(f"uploaded version {version} is absent from the account")

    file_id = item.get("file")
    if not file_id:
        raise RuntimeError(f"version {version} has no uploaded file")
    if item.get("published") is not True:
        publish_version(session, file_id)
        firmware = get_admin_firmware(session)
        item = version_by_name(firmware, version)
        if not item or item.get("published") is not True or item.get("file") != file_id:
            raise RuntimeError(f"version {version} did not become published in the account")

    verify_public_version(session, version, file_id)
    return file_id


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--email", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--version", required=True, help="version string without leading v")
    parser.add_argument("--bin", required=True, help="merged image to upload")
    args = parser.parse_args()

    try:
        file_id = publish(
            requests.Session(),
            email=args.email,
            password=args.password,
            version=args.version,
            binary=args.bin,
        )
    except (OSError, RuntimeError, requests.RequestException) as error:
        sys.exit(str(error))
    print(f"published {args.version} to M5Burner as {file_id}")


if __name__ == "__main__":
    main()
