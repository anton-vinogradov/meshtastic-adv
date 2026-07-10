#!/usr/bin/env bash
# Build + flash the advui firmware to the Cardputer ADV, then reset it into the app.
#
# Why the reset step: on the Cardputer's native USB there is no auto-reset wiring,
# so esptool's post-flash "Hard resetting via RTS pin" does NOT run the new firmware
# — the chip sits in download mode (screen black) until an external reset. Opening
# the serial port (dtr/rts low) kicks it into the app, so the screen comes up in ~4s
# instead of staying black until you power-cycle.
#
# If the upload FAILS to connect: the app is running and native USB won't enter
# download mode on its own. Power-cycle the USB (unplug/replug, directly, data cable)
# — or hold G0 while plugging in — then re-run.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PIO="${PIO:-$HOME/.pio-core-venv/bin/pio}"
PY="${PY:-$HOME/.pio-core-venv/bin/python3}"
ENV="m5stack-cardputer-adv-advui"

find_port() { ls /dev/cu.* 2>/dev/null | grep usbmodem | head -1; }

PORT="$(find_port)"
if [ -z "$PORT" ]; then
    echo "!! no /dev/cu.usbmodem* — plug the Cardputer in (data cable, straight into the Mac)"
    exit 1
fi

"$ROOT/scripts/sync-overlay.sh" >/dev/null

echo ">> flashing $ENV on $PORT"
if ! ( cd "$ROOT/firmware" && "$PIO" run -e "$ENV" -t upload --upload-port "$PORT" ); then
    echo
    echo "!! upload failed to connect — the app is running and native USB won't enter"
    echo "!! download mode on its own. Power-cycle the USB (unplug/replug) or hold G0"
    echo "!! while plugging in, then re-run: scripts/flash.sh"
    exit 1
fi

# The port re-enumerates after the flash reset; give it a moment, then kick the
# app. On native USB the reset only takes if we open the port while the chip is
# settled, so we retry and confirm the ROM banner rather than firing once blindly.
sleep 2
"$PY" - <<'PYEOF'
import glob, time, serial, sys

def find():
    ps = sorted(glob.glob('/dev/cu.usbmodem*'))
    return ps[0] if ps else None

p = None
for _ in range(30):
    p = find()
    if p:
        break
    time.sleep(0.3)
if not p:
    print(">> flashed, but no serial port to reset — power-cycle to boot the new firmware")
    sys.exit(0)

booted = False
for attempt in range(3):
    port = find() or p
    try:
        s = serial.Serial()
        s.port = port; s.baudrate = 115200
        s.dtr = False; s.rts = False  # run the app, not download mode
        s.timeout = 0.3
        s.open()  # the open itself triggers USB_UART_CHIP_RESET on native USB
        got = b""
        deadline = time.time() + 2.5
        while time.time() < deadline:
            d = s.readline()
            if d:
                got += d
            if any(m in got for m in (b"ESP-ROM", b"entry 0x", b"advui", b"Booted")):
                booted = True
                break
        s.close()
    except Exception as e:
        print(f">> reset attempt {attempt + 1} failed: {e}")
    if booted:
        print(f">> flashed and reset {port} into the app — screen up shortly")
        break
    time.sleep(1.0)

if not booted:
    print(">> flashed, but couldn't confirm the reset — if the screen stays black, unplug/replug once")
PYEOF

# No battery-backed RTC: every flash boots clockless and messages would stamp
# timeless until a phone/NTP/mesh sync. Push the host's time over the serial
# PhoneAPI once the app is up. Best-effort: a failure never fails the flash.
# SETTIME=0 skips it (e.g. to test the clockless path on purpose).
if [ "${SETTIME:-1}" != "0" ]; then
    echo ">> setting the device clock from the host"
    sleep 8 # let the app finish booting before opening an API session
    "$PY" "$ROOT/scripts/settime.py" "$(find_port)" || true
fi
