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

## Build / fork model

A UI replacement inevitably touches `main.cpp`, so the `firmware` submodule tracks our
**`advui` branch** (a fork of `meshtastic/firmware`), kept rebaseable on upstream. Rules:

- All our code lives under `firmware/src/advui/` (isolated, obviously ours).
- Edits to upstream files are minimal and guarded by `-D MESHTASTIC_ADV_UI`
  (set in the `m5stack-cardputer-adv` variant `platformio.ini`).
- `src/advui/` is picked up by the default `build_src_filter` automatically.

### Hook points in `firmware/src/main.cpp`
- include: after `#include "graphics/Screen.h"` (guarded).
- init: after `setupModules()` — engine + `service` exist → `advui::advUI.setup()`.
- pump: top of `loop()` → `advui::advUI.loop()`.

## Hardware (from the variant)

- Display: **ST7789** TFT 240×135 (classic `graphics::Screen`, *not* LVGL `device-ui` —
  the variant doesn't pull the `device-ui` lib). Pins: NSS 37, DC 34, MOSI 35, SCK 36,
  RST 33, backlight 38, SPI2_HOST @ 40 MHz.
- Keyboard: **TCA8418** I2C matrix controller (INT on GPIO 11), `HAS_PHYSICAL_KEYBOARD`.
- NeoPixel: 1× on GPIO 21. I2S audio (ES8311). GPS, BMI270 IMU, battery on GPIO 10.

## Steps

- **M1a — pipeline proof (this step):** `InternalAPI` + minimal `AdvUI` that starts the API
  and drains `FromRadio`, logging throughput. No display changes. Proves our code compiles
  into and runs alongside the engine. *(On-device verify blocked until USB flashing is sorted.)*
- **M1b — display:** force `HAS_SCREEN 0` for our build to compile out the entire stock UI,
  and drive the ST7789 ourselves (LovyanGFX/M5GFX). Decode `FromRadio` → render node list +
  incoming messages. RGB/sound on receive.
- **M1c — input + send:** read the TCA8418 keyboard, compose text, send via `ToRadio`
  (`handleToRadio`), show ACK status.
