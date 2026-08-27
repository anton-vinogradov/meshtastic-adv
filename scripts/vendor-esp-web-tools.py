#!/usr/bin/env python3
"""Vendor the exact hash-verified esp-web-tools browser bundle."""

import argparse
import base64
import hashlib
import os
import tempfile
import urllib.request
from pathlib import Path


VERSION = "10.4.0"
BASE = f"https://unpkg.com/esp-web-tools@{VERSION}"
FILES = {
    "esp32-DNPRK0Ay.js": "7C3U/KW0apZVsVMxLS0Rg7Kn+67EBzlVg5pKBDLfBNA=",
    "esp32c2-CQ3ns5Nm.js": "RtqlYzoQiONbEYXwRuH6BhLxYzkfK+n08rpR0k7h564=",
    "esp32c3-2tchr35W.js": "gUr6E2yQ0mRekJTQpKt43QDMvWIuXmNcI42GAjgY0As=",
    "esp32c5-CeCSizBL.js": "eWoaQQuL//76YGiy94/dVHVC2bjUXRM4v7ESW7gE45U=",
    "esp32c6-DolpfL0e.js": "rI24brQsIj/kfT505dA0IuuaRcxU0x3xiluy84woBI4=",
    "esp32c61-C8HktcOt.js": "wQkNzi7QrGkJlF7Bzfc+4ZmCG1kunHSPKMCwKbrxzGA=",
    "esp32h2-L0n5WuSo.js": "58O+HAcZL6Z4pG7qbUgOjFpzCYwczqpAl+88RAUulZQ=",
    "esp32p4-MR0ikcda.js": "dkipPY7c+ziYqtB5nIEDyPhV8Ii3yy09wUFaFDkvPSk=",
    "esp32s2-w9SUkb7o.js": "WwxcpDGBCWwETOYoD8nPYW4WHlZjkLoQmXxabMl9U8Q=",
    "esp32s3-DQt_R3ZE.js": "aodNmOd9ulYCEAZgwcDPHTs+bAvHsHzjlZ13xhBs72Y=",
    "index-Dt9w-IG1.js": "GWfw73bUksmCKEwVxI9cUCKjcTd4a8FqHi0BLPaaIWY=",
    "install-button.js": "RhKOAwrfRs44dqwoyiED7oQsxQUm5JQJ6EMOO0x5X3k=",
    "install-dialog-im156JnI.js": "bc/DD7S78Y4ZoUHF65ppTtr8XUSAtFdiwiEXP0fv/bU=",
    "stub_flasher_32-BLbsWvxO.js": "V1gbBBgTFlmVlKy1ALejYxbphh8kRQj7PJ7+DMrZ8n4=",
    "stub_flasher_32c2-wLQhZItC.js": "2uILULxLXVSYIgT9hveqzkLI4qCVOTjg/jUW/FRKTNI=",
    "stub_flasher_32c3-DmSvHQKL.js": "Pzf1AZQlcv3sWMjMNVCtVYyTGpRps+ZfuIybAWwaOac=",
    "stub_flasher_32c5-D1WK4DyB.js": "Ih+l/7FQlMQ57k3LUxV7P3zR3sc4MpemGFsdG1zWCp8=",
    "stub_flasher_32c6-ZuxjUVr4.js": "jJNnZ6r7guXh5hzJZazjEfiRgrEzVzfxzTvyrHVz8BQ=",
    "stub_flasher_32c61-DeKkw9vN.js": "dyDYuD5pZSD6gORoKbDlqm6ohZbe5tRIpbHY2P71VxU=",
    "stub_flasher_32h2-CZ4EIL3w.js": "pnJPyTHgu5FTybEEz+Eotmqw5x5AXBDVKZUd67MTh9g=",
    "stub_flasher_32p4-CpHBYEwI.js": "/PTLIxdSX9lCIQ34voQGS2Mf/iCPEa7IG1PIzLbjWeI=",
    "stub_flasher_32p4rc1-DyGqUAeZ.js": "0DLYK0GIhn3SlRsfAr1bz4p2aeFhhLvah7Z7dX1+w1I=",
    "stub_flasher_32s2-CrsP1231.js": "Yb9cBpMxhABwULufpsk9BXfkIgi1ISzafoBqxhSw4Uo=",
    "stub_flasher_32s3-CiJyd6Fk.js": "fAXLHcddqOvKUZnlPeR04Df4LOW/GXgzCjU1TkDodJQ=",
    "stub_flasher_8266-CQFcqJ_a.js": "fKIUvpCKrP83/ZNxiPVQQsmurorzDacUHWNO9od4/Hk=",
    "styles-sT2V1cOw.js": "rTK0GthQeu1PckEXjZpHJFc1Xs8CgvqqZV3j6jeAnJo=",
}
LICENSE_HASH = "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4"


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "meshtastic-adv-vendor/1"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def write_verified(output, name, data, expected):
    actual = base64.b64encode(hashlib.sha256(data).digest()).decode()
    if actual != expected:
        raise RuntimeError(f"hash mismatch for {name}: {actual}")
    output.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{name}.", dir=output)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
        os.replace(temporary, output / name)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def vendor(output):
    for name, expected in FILES.items():
        write_verified(output, name, fetch(f"{BASE}/dist/web/{name}"), expected)
    license_data = fetch(f"{BASE}/LICENSE")
    if hashlib.sha256(license_data).hexdigest() != LICENSE_HASH:
        raise RuntimeError("hash mismatch for esp-web-tools LICENSE")
    (output / "LICENSE").write_bytes(license_data)
    (output / "VERSION").write_text(f"esp-web-tools {VERSION}\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("docs/vendor/esp-web-tools"))
    args = parser.parse_args()
    vendor(args.output)


if __name__ == "__main__":
    main()
