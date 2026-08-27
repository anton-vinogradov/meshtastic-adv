#!/usr/bin/env python3
"""Remote-drive an ADVUI_SCREENSHOT build and save complete RGB332 frames."""

import argparse
import datetime
import glob
import os
import re
import time
from pathlib import Path


SAFE_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")


def decode_rows(width: int, height: int, rows: list[str]) -> bytes:
    """Validate one complete screenshot before any partial PNG can be written."""
    if not (1 <= width <= 1024 and 1 <= height <= 1024 and width * height <= 1024 * 1024):
        raise ValueError(f"unsafe screenshot geometry {width}x{height}")
    if len(rows) != height:
        raise ValueError(f"received {len(rows)}/{height} rows")
    row_chars = width * 2
    if any(len(row) != row_chars or re.fullmatch(r"[0-9a-f]+", row) is None for row in rows):
        raise ValueError(f"a screenshot row is not exactly {row_chars} lowercase hex characters")
    pixels = b"".join(bytes.fromhex(row) for row in rows)
    if len(pixels) != width * height:
        raise ValueError(f"decoded {len(pixels)}/{width * height} pixels")
    return pixels


def keys(serial_port, sequence: str, gap: float = 0.3) -> None:
    for char in sequence:
        serial_port.write(b"K" + char.encode())
        serial_port.flush()
        time.sleep(gap)


def capture(serial_port, output: Path, name: str, timeout: float = 6, attempts: int = 4) -> bool:
    """Request a frame, retry truncation, and publish only a complete PNG."""
    if not SAFE_NAME_RE.fullmatch(name) or name in {".", ".."}:
        print(f"!! unsafe screenshot name: {name!r}")
        return False

    complete: tuple[int, int, bytes] | None = None
    last_error = "no @@SHOT response"
    for attempt in range(attempts):
        header: tuple[int, int] | None = None
        rows: list[str] = []
        serial_port.reset_input_buffer()
        serial_port.write(b"L")
        serial_port.flush()
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = serial_port.readline().decode("ascii", "ignore").strip()
            if line.startswith("@@SHOT"):
                parts = line.split()
                try:
                    if len(parts) != 4:
                        raise ValueError("malformed screenshot header")
                    width, height = int(parts[2]), int(parts[3])
                    if not (1 <= width <= 1024 and 1 <= height <= 1024 and width * height <= 1024 * 1024):
                        raise ValueError(f"unsafe screenshot geometry {width}x{height}")
                    header = (width, height)
                    rows = []
                except ValueError as exc:
                    header = None
                    rows = []
                    last_error = str(exc)
            elif line == "@@END" and header is not None:
                try:
                    pixels = decode_rows(header[0], header[1], rows)
                    complete = (header[0], header[1], pixels)
                except ValueError as exc:
                    last_error = str(exc)
                break
            elif header is not None and re.fullmatch(r"[0-9a-f]+", line):
                rows.append(line)
        if complete is not None:
            break
        print(f"   retry L for {name} ({attempt + 1}/{attempts}): {last_error}")

    if complete is None:
        print(f"!! {name}: incomplete screenshot after {attempts} attempts: {last_error}")
        return False

    from PIL import Image

    width, height, pixels = complete
    image = Image.new("RGB", (width, height))
    target = image.load()
    for index, value in enumerate(pixels):
        x, y = index % width, index // width
        target[x, y] = (
            (value >> 5) * 255 // 7,
            ((value >> 2) & 7) * 255 // 7,
            (value & 3) * 255 // 3,
        )
    image = image.resize((width * 3, height * 3), Image.Resampling.NEAREST)
    output.mkdir(parents=True, exist_ok=True)
    destination = output / f"{name}.png"
    image.save(destination)
    print(f"ok {destination.name} rows={height}/{height}")
    return True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", nargs="?", default=".")
    parser.add_argument("script", help="semicolon-separated commands: W:sec K:keys L:name T:text C")
    parser.add_argument("--port", default=os.environ.get("MESHTASTIC_PORT"))
    args = parser.parse_args()
    if not args.port:
        ports = glob.glob("/dev/cu.usbmodem*")
        if len(ports) != 1:
            parser.error("choose one serial device with --port or MESHTASTIC_PORT")
        args.port = ports[0]
    if not os.path.exists(args.port):
        parser.error(f"serial device does not exist: {args.port}")
    return args


def main() -> int:
    args = parse_args()
    import serial

    failed = False
    with serial.Serial(args.port, 115200, timeout=1) as serial_port:
        for command in args.script.split(";"):
            command = command.strip()
            if not command:
                continue
            operation, _, argument = command.partition(":")
            if operation == "W":
                time.sleep(float(argument))
            elif operation == "K":
                keys(serial_port, argument)
            elif operation == "T":
                keys(serial_port, argument, gap=0.15)
            elif operation == "L":
                failed = not capture(serial_port, Path(args.out), argument) or failed
            elif operation == "C":  # type the current Moscow wall-clock as HH:MM
                now = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=3)))
                keys(serial_port, now.strftime("%H:%M"), gap=0.15)
                print("typed clock:", now.strftime("%H:%M"))
            else:
                raise ValueError(f"unknown drive command: {operation!r}")
    print("session done")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
