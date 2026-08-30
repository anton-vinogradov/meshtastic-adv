# Meshtastic ADV

**English** | [Русский](README.ru.md)

[![Build](https://github.com/anton-vinogradov/meshtastic-adv/actions/workflows/build.yml/badge.svg)](https://github.com/anton-vinogradov/meshtastic-adv/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/anton-vinogradov/meshtastic-adv)](https://github.com/anton-vinogradov/meshtastic-adv/releases/latest)
[![License](https://img.shields.io/github/license/anton-vinogradov/meshtastic-adv)](LICENSE)

**A keyboard-first [Meshtastic](https://meshtastic.org) client for the [M5Stack Cardputer ADV](https://shop.m5stack.com/products/m5stack-cardputer-adv-version-esp32-s3) — with its own Cap LoRa-1262 (SX1262), or driving any BLE-capable stock Meshtastic node.**

A from-scratch on-device UI focused on one thing: making it genuinely comfortable to **message people over the mesh** from a pocket QWERTY device — no phone required — while keeping the proven Meshtastic radio stack underneath. No LoRa cap? [Companion mode](#companion-mode-drive-another-node-over-ble) turns the Cardputer into a terminal for a Heltec / T-Beam / RAK you already own.

<p align="center">
  <img src="docs/img/demo.gif?sha256=26ee131a27ec1cbf4493ad4dcddf4df02b4b8a8adce2a5975e34390c60be8ccc" width="72%" alt="Meshtastic ADV: opening an unread chat, typing with emoji, receiving an acknowledgement and reply, reacting, then sending a quoted Cyrillic reply"/>
  <br/><sub>16-second demo · real firmware UI with a synthetic conversation</sub>
</p>

> **[▶ Install in your browser](https://anton-vinogradov.github.io/meshtastic-adv/)** — one click, no toolchain (desktop Chrome/Edge).

## Why

The Cardputer has a real keyboard, a colour screen, a speaker and an SD card — but the stock on-device Meshtastic UI barely uses any of it, and typing a message on it is painful. This project keeps the upstream Meshtastic radio, routing, PKI and protobuf stack underneath, and builds a clean chat experience around the keyboard.

You get a device that boots straight into a usable messenger: pick a contact, type, hit enter.

## What it does

- **🗨️ Recent chats home** — boots into your conversations (DMs + channels), newest first, each with a last-message preview, time and unread badge. Opening one jumps straight to the first unread message.
- **💬 Direct messages** — open a node, type, send. PKI-encrypted DMs like the phone app.
- **📢 Channels** — read and broadcast to any channel, right alongside your DMs.
- **✅ Delivery status** — every sent message shows *sending* (dot) → *delivered* (green check, from the routing ACK) → *failed* (red ✗ with the reason). For channel broadcasts, the check is an implicit mesh acknowledgement: a neighbour was heard relaying the packet.
- **⌨️ Cyrillic input + 😀 emoji** — type Russian on the Latin keyboard via a live transliteration layer (**Fn+L**); receive/render non-Latin text and inline emoji bitmaps; a **Tab** palette inserts emoji.
- **🈶 Broad Unicode glyph coverage** — CJK, Greek, Hebrew, Arabic and the rest of the Basic Multilingual Plane, plus emoji blocks, via a GNU Unifont partition the installer flashes automatically. The renderer is intentionally simple: complex-script shaping and bidirectional layout are not implemented, so Arabic/Hebrew display in code-point order (Latin/Cyrillic stay on the fast embedded font).
- **📇 Node list** — press **Tab** for the nodes currently known on-device, with a signal-bar meter (from SNR), hop count, last-heard age and role, in fixed columns.
- **🔎 Contact search** — start typing to find a known node and start a new chat.
- **⭐ Favourites** — flag contacts and channels; they get priority alerts.
- **🔔 Sound + light** — a single beep **and** a green LED flash from a favourite; a blue flash for everyone else (no buzzing on every packet).
- **🕘 Timestamps** — compact local `HH:MM` on every message, with a UTC-offset (city) picker and a manual clock setting for meshes with no time source.
- **📡 WiFi + MQTT, on-device** — join WiFi and bridge the mesh to the internet over MQTT (default public broker or your own), configured right on the device — no phone needed. WiFi also sets the clock via NTP.
- **💾 History that survives reboots — and eviction** — the live conversation window is persisted to flash, and older messages move to a 256-deep flash archive you can page back into right in the thread.
- **🛟 Automatic SD settings profile** — identity and keys, channels, radio/WiFi/MQTT configuration, companion choice, UI preferences and every favourite are synchronized in the background and restored automatically after a clean ADV reinstall. Messages stay private to internal flash and are not copied.
- **⚙️ On-device settings** — name, region, modem preset, frequency, channel, role, hop limit, TX power, rebroadcast mode, UTC, WiFi and MQTT, all editable on the device (long-press **ESC**).
- **🔋 Screen auto-off** — the display powers down after 15 s…5 min idle (your pick); any key wakes it, alerts still beep and flash the LED while it's dark.
- **📱 Phone app still works** — the stock Bluetooth API stays on: pair the official Meshtastic app (the Cardputer shows the pairing PIN) and use it alongside the on-device UI whenever Bluetooth is free.
- **🔗 Companion mode** — no LoRa cap? Pair the Cardputer with **any BLE-capable stock Meshtastic node** (Heltec, T-Beam, RAK…) and use its radio: the whole chat UI above, through the other node. [Details below.](#companion-mode-drive-another-node-over-ble)

<p align="center"><b><a href="docs/interface.md#screens-at-a-glance">▶ See every screen with captions →</a></b></p>

## Tested before release

Every `v*` tag is held until the exact source passes the hardware-free tests,
both firmware builds and size budgets, then a real Cardputer ADV on a trusted
runner. The physical gate is radio-silent and identity-bound; it replays ordinary
serialized `FromRadio` traffic through the production decoder, drives the real UI,
overflows and reloads persistent history, checks 35 framebuffer captures, reboots,
and finally restores and verifies the exact application bytes users will receive.
That restored production image must then survive repeated complete, read-only WiFi
config/NodeDB downloads for at least two minutes without a reconnect or reboot.
See the honest [HIL coverage matrix](hil/README.md#coverage-matrix), including what
still needs a camera, key jig or controlled RF lab for physical proof.
Release hardening changes and migration notes are kept in the [changelog](CHANGELOG.md).

## Install

The easiest way is the **[web installer](https://anton-vinogradov.github.io/meshtastic-adv/)** (ESP Web Tools):

1. Attach the **LoRa antenna** to the Cap first — *never power the PA without it.*
2. Export your Meshtastic configuration from the official app before any full erase.
3. Open the page in desktop **Chrome** or **Edge**, plug the Cardputer in with a **data** USB-C cable.
4. Click **Install**. If the device isn't listed, hold **G0/BOOT** while connecting, then release.
5. After flashing, long-press **ESC** → set your **Region** and **UTC**.

**No computer?** Two on-device paths:

- **M5Launcher catalog** — in the launcher's online-install (OTA) menu, find **Meshtastic ADV** and install from there: the catalog entry is a complete merged image and the launcher places everything correctly. Same firmware is in **M5Burner** (Cardputer category).
- **M5Launcher from SD card** — use `meshtastic-adv-cardputer-adv-merged.bin` from the [latest release](https://github.com/anton-vinogradov/meshtastic-adv/releases) and flash it as a **full firmware** (written to address `0x0`, replacing the launcher) — *not* as an app/OTA install. An app-slot install runs this firmware on the launcher's partition layout, which ends in restarts and a dead keyboard. Don't use `factory.bin` here: it lacks the Unicode font.

Prefer the CLI? Grab `meshtastic-adv-vX.Y.Z.factory.bin` from the [latest release](https://github.com/anton-vinogradov/meshtastic-adv/releases) and flash it at offset `0x0` with esptool; add `unifont.bin` at `0x340000` for broad Unicode glyph coverage (or drop it on the SD card root instead) — or just take `meshtastic-adv-cardputer-adv-merged.bin`, which contains both in one file.

### Automatic settings restore from SD

Leave a microSD card inserted and ADV maintains three checked generations under
`/meshtastic-adv/<device-id>/`. The first profile appears after this version has
booted with the card and remained storage/API-idle for about ten seconds; later
settings changes are synchronized after the same short quiet period. A phone or
WiFi configuration dump safely postpones SD work until its heap-heavy stream
finishes. On a clean ADV
installation, the newest valid generation is restored before the UI starts and
the device reboots once — there is no Restore button to press. A missing card,
read error or corrupt profile never causes defaults to overwrite the backup;
the firmware keeps the internal settings and retries later.

The profile includes Meshtastic identity/private keys, owner, channels, LoRa,
Bluetooth, WiFi and MQTT settings, plus ADV radio mode, UI preferences and up to
the full 150-node favourite set. It deliberately excludes messages, history,
the learned node cache and transient crash guards. One card can hold profiles
for several Cardputers because every directory is bound to the hardware ID.

> **Security:** the profile contains network, channel and identity secrets and is
> not encrypted. Treat the SD card like a private-key backup; do not share its
> `meshtastic-adv` directory or attach it to bug reports.

## How to drive it

Everything is keyboard-driven. The footer of each screen shows the live hints.

| Where | Key | Action |
|---|---|---|
| Chats (home) | **↑ / ↓** · **Enter** | move · open the conversation |
| Chats (home) | **← / →** | favourite / un-favourite |
| Chats (home) | *type* | search all nodes to start a new chat |
| Chats (home) | **Tab** | switch to the full node list (and back) |
| Conversation | *type* · **Enter** | write a reply · send |
| Conversation | **Fn+L** | toggle Cyrillic (translit) input |
| Conversation | **Tab** | emoji palette |
| Conversation | **↑ / ↓** | scroll through history |
| Conversation | **ESC** | back |
| Anywhere | **long-press ESC** | open Settings |

In Settings, **↑/↓** move, **Enter** edits (toggles for on/off items), **ESC** goes back. Changing anything under LoRa, WiFi/MQTT or Radio mode reboots to apply. Enabling WiFi turns Bluetooth off (Meshtastic behaviour).

## Companion mode: drive another node over BLE

The Cardputer can act as a **keyboard + screen terminal for a separate Meshtastic node** instead of (or without) its own LoRa cap. It connects as a BLE client to the node's standard Bluetooth API — the same one the phone app uses — so **the other node stays on stock firmware, zero changes**.

1. Long-press **ESC** → **Settings** → **Radio** → **Companion via BLE** → the device reboots into a scan.
2. Pick your node from the list, type the **PIN** shown on the node's screen.
3. Done — it downloads the node's contacts and channels and drops you into Chats. Everything works as usual (PKI DMs, delivery checkmarks, reactions, replies), just through the other node's radio.

A Bluetooth rune in the Chats header shows the link state; the link auto-reconnects after drops. **Settings → Radio** doubles as a status page (link signal, the node's battery, traffic; **R** reconnect, **F** forget the node). Switch back with **Radio → Onboard (Cap LoRa)**.

You can also **configure the linked node from the Cardputer**, like the phone app does: Settings shows the *node's* values, **Name / Short / Channel** apply instantly over the link, and **Region / Preset / Frequency** make the node save and reboot itself while the link recovers on its own.

> 📱 The node has a single Bluetooth slot — close the Meshtastic phone app while the Cardputer is linked, or they'll fight over it.

## Architecture

The UI and the mesh engine talk over the **same protobuf Client API** (`ToRadio` / `FromRadio`) the phone app uses — but in-process, on the same ESP32-S3, via a custom `PhoneAPI` implementation.

```
  ┌─────────────────────────────┐        ToRadio / FromRadio        ┌──────────────────────────┐
  │  UI (this project, scratch) │  ───────────────────────────────▶ │  Meshtastic mesh engine  │
  │  keyboard · screen · sound  │  ◀─────────────  protobuf  ─────── │     (upstream core)      │
  └─────────────────────────────┘                                   │  LoRa · routing · crypto │
                                                                    └──────────────────────────┘
```

- **Same Client API** — the UI uses the stock wire protocol, so the official phone app remains compatible.
- **UI is fully ours** — the stock `Screen` / canned-message / input modules are disabled.
- **Memory follows the selected radio** — the fixed BLE companion mirror and transport queues are allocated only in companion mode, returning about 8.4 KiB to the normal onboard-LoRa build.
- **No long-lived firmware fork** — upstream `meshtastic/firmware` is a pinned git submodule; our code lives in `overlay/` and the build applies a small, deterministic integration patch set for startup, USB reliability and low-memory operation.

See [docs/interface.md](docs/interface.md) for the on-screen details and [HARDWARE.md](HARDWARE.md) for the Cap LoRa-1262 pinout.

## Hardware

| Part | Detail |
|---|---|
| MCU | M5Stack Cardputer ADV — Stamp-S3A (ESP32-S3FN8, 8 MB flash, **no PSRAM**) |
| Radio | Cap LoRa-1262 — Semtech **SX1262**, 868–923 MHz, external antenna — *or* any BLE-capable stock Meshtastic node (companion mode) |
| Display | 1.14" 240×135 IPS |
| Input | 56-key QWERTY |
| Audio | ES8311 codec + speaker |
| Extras | microSD, RGB LED, RTC, battery |

> ⚠️ Never power the LoRa cap without the antenna attached — the PA can be permanently damaged.

## Build from source

```sh
git clone --recursive https://github.com/anton-vinogradov/meshtastic-adv
cd meshtastic-adv
scripts/flash.sh        # syncs the overlay, builds, uploads, resets
```

Requires PlatformIO. The embedded engine is pinned to upstream beta `v2.7.26.54e0d8d`; the 2.8 development line is deliberately not the release base yet. The build env is `m5stack-cardputer-adv-advui`. `scripts/sync-overlay.sh` copies `overlay/` into the submodule and applies the integration patches; `scripts/flash.sh` also handles the native-USB reset dance. With more than one ESP device attached it deliberately refuses to guess — use `scripts/flash.sh --port /dev/cu.usbmodemXXXX`.

Run syntax and unit tests, sanitizers, both firmware builds and their flash/DRAM/IRAM budgets with:

```sh
python3 scripts/verify.py
```

Hardware remains an explicit opt-in; commands, fixture safeguards and the coverage matrix live in [hil/](hil/README.md). The private RF runner is a separate integration diagnostic, not a re-test of the embedded Meshtastic routing engine. When moving back from a 2.8 development build, ADV discards only its incompatible, rebuildable node cache; node identity, channels and radio settings remain untouched. Take a verified configuration backup before any physical flash anyway.

## Status

The read **and** write paths are done and running on real hardware: node list, DMs, channels, delivery status, Cyrillic, emoji, reactions, replies, favourites, sound, timestamps, persisted history, on-device settings and BLE companion mode all work today. Companion mode has been manually exercised end-to-end over BLE (encrypted DM through the linked node and routing ACK back); the actual BLE GATT transport is not yet part of the automated release fixture.

The stable 1.0 line is maintained with the same hardware release gate; ongoing work
focuses on field reliability, memory headroom and small usability improvements.

## License

GPL-3.0 (matches the upstream Meshtastic firmware this builds on). The bundled Unicode font is [GNU Unifont](https://unifoundry.com/unifont/) (SIL OFL 1.1 / GPLv2+ with the font-embedding exception).

*Meshtastic® is a registered trademark of Meshtastic LLC. This is an unofficial, community project, not affiliated with or endorsed by Meshtastic LLC.*
