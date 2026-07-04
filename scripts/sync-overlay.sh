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

# main.cpp: two idempotent injections — include our UI header and create the UI
# once after setupModules() (engine + nodeDB up). AdvUI is an OSThread and then
# schedules itself, so there is no main-loop edit.
MC="$FW/src/main.cpp"
if [ -f "$MC" ] && ! grep -q 'advui-inject' "$MC"; then
  perl -0pi -e 's{#include "main\.h"\n}{#include "main.h"\n#include "advui/AdvUI.h" // advui-inject\n}' "$MC"
  perl -0pi -e 's{(\n[ \t]*setupModules\(\);\n)}{$1    advui::advuiSetup(); // advui-inject\n}' "$MC"
  echo "injected advui hooks into main.cpp"
fi

# CardputerKeyboard.cpp (upstream) uses `inputBroker` but relies on a transitive
# InputBroker.h include that MESHTASTIC_EXCLUDE_SCREEN removes. Inject the include
# directly (idempotent). This is the only working-tree edit to a tracked upstream
# file; run `git -C firmware checkout -- .` before pulling upstream, then re-sync.
KB="$FW/src/input/CardputerKeyboard.cpp"
if [ -f "$KB" ] && ! grep -q 'advui-inject' "$KB"; then
  perl -0pi -e 's{#include "main\.h"\n}{#include "main.h"\n#include "InputBroker.h" // advui-inject: lost with EXCLUDE_SCREEN\n}' "$KB"
  echo "injected InputBroker.h include into CardputerKeyboard.cpp"
fi

echo "overlay synced into $FW"
