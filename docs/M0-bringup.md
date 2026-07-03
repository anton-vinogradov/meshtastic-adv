# M0 — bring-up (flash stock, set region, verify mesh)

Goal of M0: prove the hardware and the mesh work **before** touching the UI. We build and flash the
**unmodified** upstream `m5stack-cardputer-adv` env, set the LoRa region, and confirm this node exchanges
messages with a second node.

All tools live in an isolated venv: `~/.pio-core-venv/bin/{pio,meshtastic}`.

## 1. Build (already done)

```
cd firmware
~/.pio-core-venv/bin/pio run -e m5stack-cardputer-adv
```

Artifacts: `firmware/.pio/build/m5stack-cardputer-adv/`
- `firmware-...-factory.bin` — full image, flash at offset `0x0`
- Baseline footprint: Flash app 72.7% (~0.9 MB free), static RAM 41.4% (~192 KB free).

## 2. Flash

⚠️ **Attach the LoRa antenna to the Cap first** — powering the PA without an antenna can destroy it.

Connect the Cardputer ADV over USB-C, then:

```
cd firmware
~/.pio-core-venv/bin/pio run -e m5stack-cardputer-adv -t upload
```

PlatformIO auto-detects the port. If it can't enter the bootloader, hold **G0/BTN0** while plugging in
(or while it resets), then release. If several serial ports exist, add `--upload-port /dev/cu.usbmodemXXXX`.

## 3. Configure region + identity

The stock firmware boots with region `UNSET` and **stays silent on air** until a region is set.
Pick the region that matches your second node (e.g. `EU_868` or `RU`):

```
~/.pio-core-venv/bin/meshtastic --set lora.region EU_868
~/.pio-core-venv/bin/meshtastic --set-owner "Anton" --set-owner-short "ANT"
```

The node reboots after a region change. (You can also do all of this from the Meshtastic phone app over BLE.)

## 4. Verify the mesh

```
# device + radio config, primary channel URL:
~/.pio-core-venv/bin/meshtastic --info

# node DB — the second node should appear here once both are on the same region + channel:
~/.pio-core-venv/bin/meshtastic --nodes

# send a broadcast on the primary channel:
~/.pio-core-venv/bin/meshtastic --sendtext "hello from cardputer"
```

Both nodes must share the **region** and the **primary channel** (defaults: preset `LongFast`, default PSK).
If the second node is on stock defaults and the same region, they will talk out of the box.

**M0 is done** when `--nodes` lists the second node and a text sent from one is received on the other.
Next: M1 — replace the UI (read path).
