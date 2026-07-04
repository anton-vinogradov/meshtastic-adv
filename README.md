# meshtastic-adv

Keyboard-first **Meshtastic** client for the **M5Stack Cardputer ADV** with the **Cap LoRa-1262** (SX1262).

A from-scratch UI focused on one thing: making it genuinely comfortable to *message people* on the mesh
from a pocket QWERTY device — while keeping the full feature set of stock Meshtastic.

> Status: **WIP — M1 (read path)**. The custom UI runs on-device: a branded splash, a sorted
> node list (signal · hops · last-heard · role), and a keyboard-driven contact picker. Sending
> messages is next. See the [interface guide](docs/interface.md) for what's on screen and how
> to drive it.

## Idea

The stock on-device Meshtastic UI on the Cardputer is hard to use and ignores most of the Cardputer's
hardware. This project keeps the proven Meshtastic **mesh engine** (LoRa PHY, routing, PKC crypto, node DB,
protobufs) untouched, and replaces **only the UI/input layer** with a clean, keyboard-first chat experience
that actually uses the device: the 56-key keyboard, the RGB LED, the speaker, and the SD card.

## Architecture

The mesh engine and the UI talk over the **same protobuf Client API** (`ToRadio` / `FromRadio`) that the
phone app uses — but in-process, on the same ESP32-S3, via a custom `PhoneAPI` implementation ("InternalAPI").

```
  ┌─────────────────────────────┐        ToRadio / FromRadio        ┌──────────────────────────┐
  │  UI (this project, scratch) │  ───────────────────────────────▶ │  Meshtastic mesh engine  │
  │  keyboard · screen · RGB    │  ◀─────────────  protobuf  ─────── │  (upstream, unmodified)  │
  │  · speaker · SD history     │                                   │  LoRa · routing · crypto │
  └─────────────────────────────┘                                   └──────────────────────────┘
```

Why this boundary:
- **Same feature set as stock** — everything the engine can do is reachable through the same API the phone uses.
- **UI is fully ours** — the stock `Screen` / canned-message / menu modules are disabled.
- **Upgrade-safe** — the protobuf boundary is stable, so pulling newer upstream doesn't break the UI.
- **Free companion mode later** — the exact same UI can talk to a *separate* node over BLE/Serial by only
  swapping the transport.

The upstream `meshtastic/firmware` is brought in as a git submodule (kept pristine and updatable); our UI
code and build overlay live on top. See [HARDWARE.md](HARDWARE.md) for the Cap LoRa-1262 pinout.

## Hardware

| Part | Detail |
|---|---|
| MCU | M5Stack Cardputer ADV — Stamp-S3A (ESP32-S3FN8, 8 MB flash, **no PSRAM**) |
| Radio | Cap LoRa-1262 — Semtech **SX1262**, 868–923 MHz, external RP-SMA antenna |
| GNSS | ATGM336H (on the Cap) — enables position/neighbor-map |
| Display | 1.14" 240×135 |
| Input | 56-key QWERTY |
| Audio | ES8311 codec + 1 W speaker, MEMS mic |
| Extras | microSD, SK6812 RGB LED, RTC, 1750 mAh battery, IR |

> ⚠️ Never power the LoRa cap without the antenna attached — the PA can be permanently damaged.

## Roadmap

- **M0 — bring-up:** build & flash *stock* `cardputer-adv`, set LoRa region, confirm the node exchanges
  messages with a second node. (Baseline gate before touching UI.)
- **M1 — read path:** custom render of node list (name, SNR/RSSI, last-seen, hops) + incoming messages;
  RGB + sound on receive.
- **M2 — write path:** keyboard input → send to channel/DM via `ToRadio`, with ACK status.
- **M3 — messaging UX:** conversation list (channels + DMs), scrollback history on SD, quick replies,
  contact/channel switching.
- **M4 — full Meshtastic:** position/neighbors, telemetry, traceroute, channel management (PSK/QR),
  settings, sleep/battery.
- **M5 — polish:** themes, ringtones, notifications, autotext.

## License

GPL-3.0 (matches the upstream Meshtastic firmware this builds on).

*Meshtastic® is a registered trademark of Meshtastic LLC. This is an unofficial, community project.*
