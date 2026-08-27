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
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PIO="${PIO:-$HOME/.pio-core-venv/bin/pio}"
PY="${PY:-$HOME/.pio-core-venv/bin/python3}"
ENV="m5stack-cardputer-adv-advui"

usage() {
    echo "usage: scripts/flash.sh [--port /dev/cu.usbmodemXXXX]"
    echo "       MESHTASTIC_PORT=/dev/cu.usbmodemXXXX scripts/flash.sh"
}

PORT="${MESHTASTIC_PORT:-}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --port)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            PORT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "!! unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "$PORT" ]; then
    shopt -s nullglob
    ports=(/dev/cu.usbmodem*)
    shopt -u nullglob
    if [ "${#ports[@]}" -gt 1 ]; then
        echo "!! multiple ESP USB serial ports found; refusing to guess:" >&2
        printf '   %s\n' "${ports[@]}" >&2
        echo "!! choose the Cardputer explicitly: scripts/flash.sh --port /dev/cu.usbmodemXXXX" >&2
        exit 2
    fi
    PORT="${ports[0]:-}"
fi

if [ -z "$PORT" ] || [ ! -e "$PORT" ]; then
    echo "!! no /dev/cu.usbmodem* — plug the Cardputer in (data cable, straight into the Mac)"
    exit 1
fi

"$ROOT/scripts/sync-overlay.sh" >/dev/null

echo ">> building $ENV"
if ! ( cd "$ROOT/firmware" && "$PIO" run -e "$ENV" ); then
    echo
    echo "!! build failed; flash was not touched"
    exit 1
fi

# Do not use `pio -t upload` for this hybrid Arduino/ESP-IDF environment. Its
# child PlatformIO process drops --upload-port and silently auto-detects another
# attached ESP32. Invoke esptool directly on the explicit path so port selection
# cannot change underneath us.
ESPTOOL_PY="${ESPTOOL_PY:-$HOME/.platformio/penv/bin/python}"
BUILD_DIR="$ROOT/firmware/.pio/build/$ENV"
shopt -s nullglob
images=("$BUILD_DIR"/firmware-"$ENV"-*.bin)
shopt -u nullglob
APP=""
APP_COUNT=0
for image in "${images[@]}"; do
    if [[ "$image" != *.factory.bin ]]; then
        APP="$image"
        APP_COUNT=$((APP_COUNT + 1))
    fi
done
if [ "$APP_COUNT" -ne 1 ] || [ ! -f "$BUILD_DIR/bootloader.bin" ] || [ ! -f "$BUILD_DIR/partitions.bin" ]; then
    echo "!! complete firmware image set not found in $BUILD_DIR" >&2
    exit 1
fi
BOOT_APP0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
[ -f "$BOOT_APP0" ] || { echo "!! boot_app0.bin not found: $BOOT_APP0" >&2; exit 1; }

echo ">> flashing $ENV on explicit port $PORT (PlatformIO auto-detect disabled)"
if ! "$ESPTOOL_PY" -m esptool --chip esp32s3 --port "$PORT" --baud 921600 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0 "$BUILD_DIR/bootloader.bin" \
    0x8000 "$BUILD_DIR/partitions.bin" \
    0xe000 "$BOOT_APP0" \
    0x10000 "$APP"; then
    echo
    echo "!! upload failed to connect — power-cycle the selected Cardputer or hold G0"
    echo "!! while plugging it in, then re-run with the same explicit --port"
    exit 1
fi

# The port re-enumerates after the flash reset; give it a moment, then kick the
# app. On native USB the reset only takes if we open the port while the chip is
# settled, so we retry and confirm the ROM banner rather than firing once blindly.
sleep 2
"$PY" - "$PORT" <<'PYEOF'
import os, time, serial, sys

target = sys.argv[1]

def find_target():
    return target if os.path.exists(target) else None

p = None
for _ in range(30):
    p = find_target()
    if p:
        break
    time.sleep(0.3)
if not p:
    print(">> flashed, but no serial port to reset — power-cycle to boot the new firmware")
    sys.exit(0)

booted = False
for attempt in range(3):
    port = find_target() or p
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
    if [ -e "$PORT" ]; then
        "$PY" "$ROOT/scripts/settime.py" "$PORT" || true
    else
        echo ">> target port did not return; skipping clock sync"
    fi
fi
