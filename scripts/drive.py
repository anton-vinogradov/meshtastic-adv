#!/usr/bin/env python3
# Remote-drive the ADVUI_SCREENSHOT build over serial: inject keys, capture live
# screenshots (@@SHOT rgb332 hex dump) into PNGs. One long session per run.
import argparse
import datetime
import glob
import os
import time

parser = argparse.ArgumentParser()
parser.add_argument("out", nargs="?", default=".")
parser.add_argument("script", help="semicolon-separated commands: W:sec K:keys L:name T:text C")
parser.add_argument("--port", default=os.environ.get("MESHTASTIC_PORT"))
args = parser.parse_args()

PORT = args.port
if not PORT:
    ports = glob.glob("/dev/cu.usbmodem*")
    if len(ports) != 1:
        parser.error("choose one serial device with --port or MESHTASTIC_PORT")
    PORT = ports[0]
if not os.path.exists(PORT):
    parser.error(f"serial device does not exist: {PORT}")

OUT = args.out
SCRIPT = args.script

import serial
from PIL import Image

s = serial.Serial(PORT, 115200, timeout=1)

def keys(seq, gap=0.3):
    for ch in seq:
        s.write(b"K" + ch.encode())
        s.flush()
        time.sleep(gap)

def capture(name, timeout=6, attempts=4):
    # the stock serial console races us for input bytes, so 'L' may get eaten —
    # resend until a dump actually starts
    hdr = None
    rows = []
    for att in range(attempts):
        s.reset_input_buffer()
        s.write(b"L")
        s.flush()
        end = time.time() + timeout
        while time.time() < end:
            line = s.readline().decode("ascii", "ignore").strip()
            if line.startswith("@@SHOT"):
                hdr = line.split()
                rows = []
            elif line == "@@END" and hdr:
                break
            elif hdr is not None:
                t = line.strip()
                if len(t) >= 2 and all(c in "0123456789abcdef" for c in t):
                    rows.append(t)
        if hdr:
            break
        print(f"   retry L for {name} ({att + 1})")
    if not hdr:
        print(f"!! {name}: no @@SHOT after {attempts} attempts")
        return False
    w, h = int(hdr[2]), int(hdr[3])
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(min(h, len(rows))):
        r = rows[y]
        for x in range(min(w, len(r) // 2)):
            v = int(r[x * 2 : x * 2 + 2], 16)
            px[x, y] = ((v >> 5) * 255 // 7, ((v >> 2) & 7) * 255 // 7, (v & 3) * 255 // 3)
    img = img.resize((w * 3, h * 3), Image.NEAREST)
    img.save(f"{OUT}/{name}.png")
    print(f"ok {name}.png rows={len(rows)}/{h}")
    return True

for cmd in SCRIPT.split(";"):
    cmd = cmd.strip()
    if not cmd:
        continue
    op, _, arg = cmd.partition(":")
    if op == "W":
        time.sleep(float(arg))
    elif op == "K":
        keys(arg)
    elif op == "T":
        keys(arg, gap=0.15)
    elif op == "L":
        capture(arg)
    elif op == "C":  # type the current Moscow wall-clock as HH:MM
        now = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=3)))
        keys(now.strftime("%H:%M"), gap=0.15)
        print("typed clock:", now.strftime("%H:%M"))
s.close()
print("session done")
