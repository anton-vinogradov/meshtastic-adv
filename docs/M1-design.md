# M1 — custom UI (design of record)

Goal: replace the stock on-device UI with our own, keeping the upstream mesh engine
untouched. Our UI talks to the engine over the same protobuf Client API the phone app
uses (`ToRadio` / `FromRadio`), but in-process.

## The boundary: `InternalAPI`

`PhoneAPI` (`firmware/src/mesh/PhoneAPI.h`) is the abstract class every transport
subclasses (BLE / serial / TCP / HTTP). We add `advui::InternalAPI : public PhoneAPI`:

- `checkIsConnected()` → `true` (the UI shares the engine's process; the link is always up).
- `begin()` → calls the protected `handleStartConfig()`, which does
  `observe(&service->fromNumChanged)` — so the engine notifies us on new data.
- Poll `available()` / `getFromRadio(buf)` to drain the `FromRadio` stream
  (my-info, config, channels, node DB, then live packets).
- Push outgoing text/commands with `handleToRadio(buf, len)` (M1c).

This is exactly the phone-app model, so we inherit the full feature set for free and stay
decoupled from engine internals. Bonus: the same UI can later run as a BLE *companion* to a
separate node by swapping the transport.

## Build model — additive overlay, no fork

The `firmware` submodule stays **byte-identical to upstream** `meshtastic/firmware`.
Our code lives in `overlay/` and is copied into the firmware tree at build time by
`scripts/sync-overlay.sh`, which also applies three idempotent, marker-guarded (`advui-inject`)
injections. Nothing is committed into the submodule, so updating upstream is
`git -C firmware checkout -- . && git -C firmware pull` then re-sync — no merge conflicts.

- `overlay/src/advui/` → `firmware/src/advui/` (our UI; compiled by the default src filter).
- `overlay/variants/esp32s3/m5stack_cardputer_adv_advui/platformio.ini` → a **new** env
  `m5stack-cardputer-adv-advui` (extends the stock env, adds `-D MESHTASTIC_EXCLUDE_SCREEN`
  + the LovyanGFX dep). A new file, so the `variants/*/*/platformio.ini` glob picks it up.
- Injections: `main.cpp` gets `#include "advui/AdvUI.h"` + a `advui::advuiSetup();` call after
  `setupModules()`; `CardputerKeyboard.cpp` gets the `InputBroker.h` include it loses when the
  stock Screen is excluded.

`AdvUI` is a `concurrency::OSThread`, so once `advuiSetup()` creates it the scheduler drives
`runOnce()` — no main-loop edit. Build with `-e m5stack-cardputer-adv-advui`.

## Hardware (from the variant)

- Display: **ST7789** TFT 240×135 (classic `graphics::Screen`, *not* LVGL `device-ui` —
  the variant doesn't pull the `device-ui` lib). Pins: NSS 37, DC 34, MOSI 35, SCK 36,
  RST 33, backlight 38, SPI2_HOST @ 40 MHz.
- Keyboard: **TCA8418** I2C matrix controller (INT on GPIO 11), `HAS_PHYSICAL_KEYBOARD`.
- NeoPixel: 1× on GPIO 21. I2S audio (ES8311). GPS, BMI270 IMU, battery on GPIO 10.

## Steps

- **M1a — pipeline:** `InternalAPI` drains the `FromRadio` stream in-process. ✅
- **M1b — display:** own ST7789 via LovyanGFX (double-buffered), node list (name / SNR /
  hops), header with node count + battery. Stock screen off via `MESHTASTIC_EXCLUDE_SCREEN`. ✅
- **M1c — input:** ESC-activated contact picker (favourites → recent senders → all),
  type-to-filter, via the stock `InputBroker`; then compose + send via `handleToRadio`.
