#!/usr/bin/env python3
"""Fail CI when the ADV firmware silently consumes its safety headroom."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


LIMITS = {
    "app_flash": 3_000_000,
    # Companion-only tables/queues are allocated on demand, leaving production
    # at 178,492 bytes and HIL at 179,444. Keep an 8+ KiB regression margin,
    # but reject accidentally putting that ~8.4 KiB block back in .bss.
    "dram_static": 188_416,
    # The pinned 2.7.26 Arduino/IDF toolchain puts 91,162 bytes of the stock
    # Cardputer ADV image in IRAM. ADV currently adds about 2.7 KiB; keep an
    # 11 KiB regression margin without rejecting upstream itself.
    "iram": 105_000,
}


def one(items: list[Path], label: str) -> Path:
    if len(items) != 1:
        raise SystemExit(f"expected one {label}, found {len(items)}: {items}")
    return items[0]


def map_sections(path: Path) -> dict[str, int]:
    wanted = {".dram0.data", ".dram0.bss", ".noinit", ".iram0.vectors", ".iram0.text"}
    found: dict[str, int] = {}
    pattern = re.compile(r"^\s*(\.[A-Za-z0-9_.]+)\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\b")
    for line in path.read_text(errors="replace").splitlines():
        match = pattern.match(line)
        if match and match.group(1) in wanted and match.group(1) not in found:
            found[match.group(1)] = int(match.group(2), 16)
    missing = wanted - found.keys()
    if missing:
        raise SystemExit(f"missing map sections: {', '.join(sorted(missing))}")
    return found


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=Path)
    args = parser.parse_args()
    build = args.build_dir

    app = one(
        [p for p in build.glob("firmware-*.bin") if not p.name.endswith(".factory.bin")],
        "application binary",
    )
    map_file = one(list(build.glob("firmware-*.map")), "linker map")
    sections = map_sections(map_file)
    sizes = {
        "app_flash": app.stat().st_size,
        "dram_static": sections[".dram0.data"] + sections[".dram0.bss"] + sections[".noinit"],
        "iram": sections[".iram0.vectors"] + sections[".iram0.text"],
    }

    failed = False
    for name, used in sizes.items():
        limit = LIMITS[name]
        print(f"{name}: {used:,} / {limit:,} bytes ({used / limit:.1%} of budget)")
        failed |= used > limit
    if failed:
        raise SystemExit("firmware size budget exceeded")


if __name__ == "__main__":
    main()
