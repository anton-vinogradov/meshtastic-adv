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
- **Region defaults to UNSET.** Stock firmware will not transmit until the LoRa region is configured. For
  M0 baseline, set it via the phone app or the `meshtastic` Python CLI; our own UI exposes it later (M4).
- **No PSRAM.** ESP32-S3FN8 has ~300 KB usable SRAM. The mesh engine already fits (stock firmware runs),
  but the UI must be frugal: keep message history on **SD**, not in RAM. A 240×135×16-bit framebuffer is
  ~64 KB — acceptable, but budget it deliberately.
