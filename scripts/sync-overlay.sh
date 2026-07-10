#!/usr/bin/env bash
# Copy the additive overlay into the pristine firmware submodule before building.
#
# The firmware submodule stays byte-identical to upstream meshtastic/firmware.
# Our code lives in overlay/ and is copied in as UNTRACKED files, so updating
# upstream is just `git -C firmware pull` with zero merge conflicts.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FW="$ROOT/firmware"

rm -rf "$FW/src/advui"
cp -R "$ROOT/overlay/src/advui" "$FW/src/advui"

rm -rf "$FW/variants/esp32s3/m5stack_cardputer_adv_advui"
cp -R "$ROOT/overlay/variants/esp32s3/m5stack_cardputer_adv_advui" \
      "$FW/variants/esp32s3/m5stack_cardputer_adv_advui"

# main.cpp: three idempotent injections —
#  1. include our UI header,
#  2. create the UI once after setupModules() (engine + nodeDB up); AdvUI is an
#     OSThread and then schedules itself, so there is no main-loop edit,
#  3. make the USB-CDC console non-blocking, so boot logging doesn't stall for
#     seconds when the port is enumerated by a host that isn't draining it
#     (that stall is why USB-powered boots are much slower than battery).
MC="$FW/src/main.cpp"
if [ -f "$MC" ] && ! grep -q 'advui-inject' "$MC"; then
  perl -0pi -e 's{#include "main\.h"\n}{#include "main.h"\n#include "advui/AdvUI.h" // advui-inject\n}' "$MC"
  perl -0pi -e 's{(\n[ \t]*setupModules\(\);\n)}{$1    advui::advuiSetup(); // advui-inject\n}' "$MC"
  perl -0pi -e 's{(\n[ \t]*consoleInit\(\); // Set serial baud rate and init our mesh console\n)}{$1#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT\n    Serial.setTxTimeoutMs(0); // advui-inject: keep boot logging non-blocking when the USB host is not draining the CDC\n#endif\n}' "$MC"
  echo "injected advui hooks into main.cpp"
fi

# SerialConsole.cpp: in the ADVUI_SCREENSHOT dev build the remote-debug driver
# owns serial input (keys/screenshots); the stock console polls-and-drains the
# same port whenever USB is plugged and would eat those bytes. Dead code in
# release builds (the flag is never set there).
SC="$FW/src/SerialConsole.cpp"
if [ -f "$SC" ] && ! grep -q 'advui-inject' "$SC"; then
  perl -0pi -e 's{(int32_t SerialConsole::runOnce\(\)\n\{\n)}{$1#ifdef ADVUI_SCREENSHOT\n    return 250; // advui-inject: the screenshot build reads serial itself\n#endif\n}' "$SC"
  echo "injected advui hook into SerialConsole.cpp"
fi

echo "overlay synced into $FW"
