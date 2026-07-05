# Meshtastic ADV

**A keyboard-first [Meshtastic](https://meshtastic.org) client for the [M5Stack Cardputer ADV](https://shop.m5stack.com/products/m5stack-cardputer-adv) + Cap LoRa-1262 (SX1262).**

A from-scratch on-device UI focused on one thing: making it genuinely comfortable to **message people over the mesh** from a pocket QWERTY device — no phone required — while keeping the proven Meshtastic radio stack underneath.

<p align="center">
  <img src="docs/img/chats.png" width="49%" alt="Recent conversations home with previews and unread badges"/>
  <img src="docs/img/chat.png" width="49%" alt="Conversation with Cyrillic text, inline emoji and delivery status"/>
</p>

> **[▶ Install in your browser](https://anton-vinogradov.github.io/meshtastic-adv/)** — one click, no toolchain (desktop Chrome/Edge).

## Why

The Cardputer has a real keyboard, a colour screen, a speaker and an SD card — but the stock on-device Meshtastic UI barely uses any of it, and typing a message on it is painful. This project **keeps the Meshtastic mesh engine 100% intact** (LoRa PHY, routing, PKI crypto, node DB, protobufs) and replaces **only the UI/input layer** with a clean chat experience built for the keyboard.

You get a device that boots straight into a usable messenger: pick a contact, type, hit enter.

## What it does

- **🗨️ Recent chats home** — boots into your conversations (DMs + channels), newest first, each with a last-message preview, time and unread badge. Opening one jumps straight to the first unread message.
- **💬 Direct messages** — open a node, type, send. PKI-encrypted DMs like the phone app.
- **📢 Channels** — read and broadcast to any channel, right alongside your DMs.
- **✅ Delivery status** — every sent message shows *sending* (dot) → *delivered* (green check, from the routing ACK) → *failed* (red ✗ with the reason). Channel broadcasts get a "sent" check.
- **⌨️ Cyrillic input + 😀 emoji** — type Russian on the Latin keyboard via a live transliteration layer (**Fn+L**); receive/render non-Latin text and inline emoji bitmaps; a **Tab** palette inserts emoji.
- **📇 Node list** — press **Tab** for everyone on the mesh, with a signal-bar meter (from SNR), hop count, last-heard age and role, in fixed columns.
- **🔎 Contact search** — start typing to find any node and start a new chat.
- **⭐ Favourites** — flag contacts and channels; they get priority alerts.
- **🔔 Sound + light** — a single beep **and** a green LED flash from a favourite; a blue flash for everyone else (no buzzing on every packet).
- **🕘 Timestamps** — compact local `HH:MM` on every message, with a UTC-offset (city) picker.
- **💾 History that survives reboots** — the conversation ring is persisted to flash.
- **⚙️ On-device settings** — name, region, modem preset, frequency, channel and UTC, all editable on the device (long-press **ESC**).

<p align="center">
  <img src="docs/img/emoji.png" width="32%" alt="Emoji palette"/>
  <img src="docs/img/settings.png" width="32%" alt="On-device settings"/>
  <img src="docs/img/utc.png" width="32%" alt="UTC offset picker with cities"/>
</p>

## Install

The easiest way is the **[web installer](https://anton-vinogradov.github.io/meshtastic-adv/)** (ESP Web Tools):

1. Attach the **LoRa antenna** to the Cap first — *never power the PA without it.*
2. Open the page in desktop **Chrome** or **Edge**, plug the Cardputer in with a **data** USB-C cable.
3. Click **Install**. If the device isn't listed, hold **G0/BOOT** while connecting, then release.
4. After flashing, long-press **ESC** → set your **Region** and **UTC**.

Prefer the CLI? Grab `firmware.factory.bin` from the [latest release](https://github.com/anton-vinogradov/meshtastic-adv/releases) and flash at offset `0x0` with esptool.

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

In Settings, **↑/↓** move, **Enter** edits, **ESC** goes back. Changing Region/Preset/Frequency/Channel reboots the radio to apply.

## Architecture

The UI and the mesh engine talk over the **same protobuf Client API** (`ToRadio` / `FromRadio`) the phone app uses — but in-process, on the same ESP32-S3, via a custom `PhoneAPI` implementation.

```
  ┌─────────────────────────────┐        ToRadio / FromRadio        ┌──────────────────────────┐
  │  UI (this project, scratch) │  ───────────────────────────────▶ │  Meshtastic mesh engine  │
  │  keyboard · screen · sound  │  ◀─────────────  protobuf  ─────── │  (upstream, unmodified)  │
  └─────────────────────────────┘                                   │  LoRa · routing · crypto │
                                                                    └──────────────────────────┘
```

- **Same feature set as stock** — anything the engine can do is reachable through the API the phone uses.
- **UI is fully ours** — the stock `Screen` / canned-message / input modules are disabled.
- **No fork** — upstream `meshtastic/firmware` is a pristine git submodule; our code lives in an `overlay/` that is copied in at build time with two tiny `main.cpp` injections. Pulling newer upstream doesn't break the UI.

See [docs/interface.md](docs/interface.md) for the on-screen details and [HARDWARE.md](HARDWARE.md) for the Cap LoRa-1262 pinout.

## Hardware

| Part | Detail |
|---|---|
| MCU | M5Stack Cardputer ADV — Stamp-S3A (ESP32-S3FN8, 8 MB flash, **no PSRAM**) |
| Radio | Cap LoRa-1262 — Semtech **SX1262**, 868–923 MHz, external antenna |
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

Requires PlatformIO. The build env is `m5stack-cardputer-adv-advui`. `scripts/sync-overlay.sh` copies `overlay/` into the submodule and applies the injections; `scripts/flash.sh` also handles the native-USB reset dance.

## Status

The read **and** write paths are done and running on real hardware: node list, DMs, channels, delivery status, Cyrillic, emoji, favourites, sound, timestamps, persisted history and on-device settings all work today.

Next up: full-Unicode font from SD (CJK), and quality-of-life polish.

## License

GPL-3.0 (matches the upstream Meshtastic firmware this builds on).

*Meshtastic® is a registered trademark of Meshtastic LLC. This is an unofficial, community project, not affiliated with or endorsed by Meshtastic LLC.*
