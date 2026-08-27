# Hardware notes — Cardputer ADV + Cap LoRa-1262

## Cap LoRa-1262 → Cardputer ADV (ESP32-S3) pin map

Semtech **SX1262**, 868–923 MHz.

| SX1262 signal | GPIO |
|---|---|
| SPI SCK  | 5  |
| SPI MOSI | 39 |
| SPI MISO | 14 |
| NSS / CS | 40 |
| RST      | 3  |
| BUSY     | 6  |
| DIO1 / IRQ | 4 |

GNSS (ATGM336H on the same Cap):

| GNSS | GPIO |
|---|---|
| GPS RX | 13 |
| GPS TX | 15 |

## Gotchas

- **Antenna switch is NOT a plain GPIO.** The RF antenna switch is driven through the **PI4IOE I2C IO
  expander** (SDA 8 / SCL 9), output **P0 must be set HIGH** during init. The upstream `cardputer-adv`
  variant already handles this — when we replace the UI we must **not** drop this init, or the radio goes
  silent (SPI works, but nothing is transmitted/received).
- **No antenna = possible hardware damage.** Never power the cap without the RP-SMA antenna attached.
- **Region defaults to UNSET.** The radio will not transmit until a region is configured. ADV opens the
  region picker on first boot and keeps it available under **Settings → LoRa → Region**; it never silently
  replaces an intentional profile such as US.
- **No PSRAM.** ESP32-S3FN8 has ~300 KB usable SRAM. The UI therefore uses a fixed 240×135×8-bit canvas
  (32.4 KB), a bounded live-message ring, and a pageable archive in internal LittleFS. The Unicode font
  lives in its own flash partition and can fall back to SD; it is never loaded wholesale into RAM. The
  roughly 8.4 KiB BLE companion mirror/queue block is allocated on demand and is absent in onboard-LoRa mode.
