#!/usr/bin/env python3
"""Publish a release's merged image as a new M5Burner firmware version.

    m5burner_publish.py --email ... --password ... --version 0.4.14 --bin <merged.bin>

Slimmed from bmorcelli's Launcher support script (thanks!): login to the
M5Stack account, upload the binary as a new version of our existing USER
CUSTOM firmware entry, publish it. M5Burner is the source both Burner users
and the M5Launcher catalog install from, so this keeps every channel current.
CI calls it on tag releases with credentials from repository secrets;
failures must not fail the release (the step guards for that).
"""

import argparse
import sys

import requests

FID = "78f98d6ec06fe700fa9edced6434a3f0"  # our "Meshtastic ADV" entry
API = "http://m5burner-api.m5stack.com"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--email", required=True)
    ap.add_argument("--password", required=True)
    ap.add_argument("--version", required=True, help="version string, no leading v")
    ap.add_argument("--bin", required=True, help="merged image to upload")
    ap.add_argument("--description", help="file whose contents replace the listing text; "
                                          "omit to keep whatever the account already has")
    # Do not pass this without testing it first. The upload POST carries no fid, so the
    # backend appears to locate the entry by name: sending a different one for v0.4.34
    # returned 200 and did nothing at all — no version added, no rename, no new entry —
    # and the release silently failed to reach M5Burner. Every earlier publish worked
    # only because the name was copied forward untouched.
    ap.add_argument("--name", help="listing title; omit to keep the account's own")
    args = ap.parse_args()

    s = requests.Session()

    r = s.post("https://uiflow2.m5stack.com/api/v1/account/login",
               json={"email": args.email, "password": args.password}, timeout=30)
    r.raise_for_status()
    token = s.cookies.get("m5_auth_token")
    if not token:
        sys.exit("login ok but no m5_auth_token cookie")
    s.headers.update({"m5_auth_token": token})
    print("login ok")

    r = s.get(f"{API}/api/admin/firmware", timeout=30)
    r.raise_for_status()
    fw = next((f for f in r.json() if f.get("fid") == FID), None)
    if not fw:
        sys.exit(f"firmware {FID} not found in the account")
    if any(v.get("version") == args.version for v in fw.get("versions") or []):
        print(f"version {args.version} already uploaded — nothing to do")
        return
    print(f"found: {fw.get('name')}")

    # The listing text is the only thing a browsing user reads before deciding, and
    # until now it lived solely in the M5Burner account — edited through their web UI,
    # reviewed by nobody, and silently copied forward by this script on every release.
    # Keeping it in the repo makes it reviewable like the rest of the product.
    description = fw.get("description", "")
    if args.description:
        with open(args.description, encoding="utf-8") as f:
            description = f.read().strip()

    data = {
        "name": args.name or fw.get("name", ""),
        "description": description,
        "category": fw.get("category", ""),
        "author": fw.get("author", ""),
        "version": args.version,
        "github": fw.get("github", ""),
        "cover": "null",
    }
    with open(args.bin, "rb") as f:
        r = s.post(f"{API}/api/admin/firmware", data=data, files={"firmware": f}, timeout=300)
    r.raise_for_status()
    print("upload ok")

    r = s.get(f"{API}/api/admin/firmware", timeout=30)
    r.raise_for_status()
    fw = next((f for f in r.json() if f.get("fid") == FID), None)
    file_id = next((v.get("file") for v in (fw.get("versions") or []) if v.get("version") == args.version), None)
    if not file_id:
        sys.exit(f"uploaded version {args.version} not found for publishing")

    r = s.put(f"{API}/api/admin/firmware/{FID}/publish/{file_id}/1", timeout=30)
    r.raise_for_status()
    if r.json().get("status") != 1:
        sys.exit(f"publish failed: {r.text}")
    print(f"published {args.version} to M5Burner")


if __name__ == "__main__":
    main()
