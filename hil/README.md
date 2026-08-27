# Hardware-in-the-loop tests

The HIL path uses a dedicated `m5stack-cardputer-adv-advui-hil` image. It runs the
production UI and mesh stack with a small USB test protocol compiled in. Because
the suite allocates the complete companion arena, its local NodeDB uses the same
32-slot boot profile as production companion mode; production onboard mode uses
its normal 100/150-slot profile and never allocates that arena. The
published firmware never contains that protocol.

The HIL image forces `tx_enabled=false` in RAM at boot, has an immutable final
guard in `RadioLibInterface::send()`, and makes its ADV receive module ignore
live RF. It cannot emit boot announcements, telemetry, retries or test messages
into the real mesh; all behavioral ingress comes from serialized `FromRadio`
frames over USB. The persisted US region, 26 dBm setting, channels and production
message files are neither replaced nor normalized.
The DUT fixture records `expected_region: "US"` and `expected_tx_power: 26`;
both smoke passes read those persisted values back from the HIL process and fail
before behavioral assertions if the lab profile has drifted.

The engine is pinned to `v2.7.26.54e0d8d`. If the DUT previously booted a 2.8
development build, the 2.7 overlay rejects only the forward-version v25 node
cache instead of decoding it with the incompatible v24 layout. The cache is
relearned from the mesh; identity, channels and radio configuration are kept.
A verified secret-bearing configuration export is still required before the
first physical downgrade.

Device roles are bound to stable identities, never to discovery order. `dut` is
the only USB device the runner may open or flash. Entries in
`protected_devices` are explicit deny-list identities: their ports are never
opened. Network fixtures live in `wifi_peers`; `read-only` is the default policy,
and future RF traffic/configuration suites must additionally require `access:
"test"` for every participating endpoint.

Flashing deliberately does **not** use PlatformIO's `upload` target. The hybrid
Arduino/ESP-IDF builder starts a child PlatformIO process that can drop the
explicit upload port and fall back to auto-detection. The HIL runner instead
builds normally, resolves the DUT by USB identity, confirms the chip MAC with
esptool, writes only the app partition on that exact port, and verifies it. A
MAC mismatch stops before the first flash write.

## First-time setup

```sh
python3 -m pip install -r hil/requirements.txt
python3 scripts/hil.py inventory
python3 scripts/hil.py init \
  --dut E8:F6:0A:00:00:01 \
  --protected-usb 1C:DB:D4:00:00:02 \
  --wifi-peer node-a=192.0.2.10 \
  --wifi-peer node-b=192.0.2.11

# Probe only the configured TCP endpoints; no PhoneAPI payload is sent.
python3 scripts/hil.py wifi-inventory
```

The generated `hil/fixture.local.json` is ignored by Git. Committed automation
uses only `fixture.example.json`; machine-specific identities stay local, and
the generator restricts the local file to its owner (`0600`). Merely
listing a WiFi endpoint does not authorize transmissions or configuration
changes: `init` records every endpoint as `read-only`.

## Run

```sh
# All safe local checks, both firmware images and both size budgets. Hardware is
# never used unless its explicit flag is present. JSON, JUnit and per-stage logs
# are written below hil-artifacts/.
python3 scripts/verify.py

# Full lab pass. Fresh secret-bearing WiFi exports are captured before the RF
# fixtures are changed. n77b's intentional US/26 dBm profile is authoritative;
# there is no automatic RU fallback or region normalization.
python3 scripts/verify.py --rf --usb \
  --capture-rf-backups \
  --rf-profile-from n77b \
  --rf-expect-region US --rf-expect-tx-power 26 \
  --backup-root hil-artifacts/<timestamp>-rf-backup

# Complete fail-safe cycle: build both images, test, and always restore release.
python3 scripts/hil.py run

# Or run individual stages while developing the suite.
# Build and flash only the MAC assigned to devices.dut.
python3 scripts/hil.py build
python3 scripts/hil.py flash

# Fast, non-persistent checks with JSON + JUnit evidence.
python3 scripts/hil.py smoke

# Inject serialized FromRadio protobufs through the production decoder and
# message handler, then verify dedupe, replies, reactions, ACK/NAK and reboot.
# This does not send a mesh packet and uses a HIL-only storage namespace.
python3 scripts/hil.py messages

# Full 29-screen capture. PNG output needs Pillow; raw RGB332 is always saved.
python3 scripts/hil.py visual

# Put the normal serial PhoneAPI firmware back after the session.
python3 scripts/hil.py build-release
python3 scripts/hil.py restore

# State-preserving two-node RF smoke on a random private channel. This requires
# exactly two WiFi peers explicitly marked `access: "test"` and restorable
# backups made before the run. It never transmits on channels 0 or 1.
python3 scripts/rf_hil.py \
  --capture-backups \
  --profile-from n77b \
  --expect-region US --expect-tx-power 26 \
  --backup-root hil-artifacts/<timestamp>-rf-backup

# For an asymmetric/very weak RF path, start with the opposite direction.
python3 scripts/rf_hil.py \
  --backup-root hil-artifacts/<timestamp>-rf-backup \
  --reverse-first

# Cleanup/recovery without sending any mesh packet.
python3 scripts/rf_hil.py \
  --backup-root hil-artifacts/<timestamp>-rf-backup \
  --restore-only
```

Keep backups containing `privateKey` or raw flash images only under the ignored
`hil-artifacts/` tree and restrict them to the current user (`chmod 600`). These
files contain node identities and channel secrets and must never be committed.

`run` is the normal unattended USB entry point. It stages a tested release image
and captures a verified full configuration export before touching hardware,
runs smoke, the FromRadio message-flow suite, forces
the live message ring to spill into the flash archive and verifies it after a
reboot, runs the visual/reboot matrix and smoke again, then restores production
firmware in a `finally` block even when a test fails. Before flashing HIL, the exact recovery
app is hash-verified and copied with owner-only permissions outside the mutable
PlatformIO build tree, so a concurrent clean/rebuild cannot remove the bytes
needed by that `finally` block. The runner then captures the configuration again
and requires the two secret exports to be byte-for-byte equivalent. Both exports
live only in an owner-readable system temporary directory and are deleted before
the runner exits; HIL evidence records only the comparison result and error type.
Use `--skip-build` only during local runner development when both images already
exist. A summary and the four sub-suite reports share one timestamped artifact
directory.

In the tag workflow, `firmware-factory` contains both the publishable factory
image and its app-partition image. The release job first proves that the app bytes
occur at factory offset `0x10000`, then passes that exact app image to HIL via
`--release-image`. The final production restore therefore boots and verifies the
same application bytes users will receive, while preserving the DUT's NVS and
filesystem configuration.

`verify.py` is the top-level reproducible entry point. Its default eight stages
are Python/shell syntax, Python unit tests, sanitizer-backed host tests, HIL and
production builds, and a flash/DRAM/IRAM budget check for each image. `--quick`
runs only the four host stages and `--firmware-only` runs only build/size stages.
RF and USB are opt-in because they mutate lab fixtures. When both are requested,
the deterministic USB suite runs first so weak RF cannot hide its evidence. The
RF and USB runners retain their own guarded cleanup and production-restore
guarantees.

`smoke` never changes radio/WiFi settings or stored messages. It verifies boot,
the physical keyboard controller on I2C, the display framebuffer, serial command
transport, screen-state transitions, search, settings, WiFi/MQTT pages and the
About page. Frame digests catch blank/stale renders without transferring images.

`messages` injects raw serialized `meshtastic_FromRadio` envelopes into the HIL
firmware. The firmware decodes them with its actual nanopb schema and passes the
result to the production `handleFromRadio` path. It covers an incoming Unicode
DM, packet-ID deduplication (including equal IDs from different senders),
replies, reactions, channel broadcasts, a real
key-driven compose/send path stopped at the radio-silent transport guard,
positive routing ACK, `MAX_RETRANSMIT` NAK, malformed UTF-8 and protobuf, plus
negative filters for non-packet envelopes, still-encrypted payloads, unrelated
ports, route-discovery traffic, missing request IDs and malformed routing data.
It also covers rendering, persistence and reboot recovery. It then feeds
48 more ordinary `FromRadio` messages on separate UI ticks, overflows the
32-record live ring into the flash archive, and proves that a delayed routing
ACK still updates the protected pending message. It also proves that reactions
from the same user remain independently scoped to their exact target sender when
two conversations contain the same packet ID, and that deleting one conversation
removes only its reaction while preserving the other live/archived target. It fills the 2048-record seen-node
ledger before admitting a fresh sender by bounded oldest-entry replacement. The keyboard path also changes the
node sort and name style, commits their atomic UI configuration and proves both
values and exact reaction targets survive the same reboot. A synthetic isolated
`AVS4` save also proves old target-less reactions migrate conservatively into
the current format. The 12 KiB heap floor is
reapplied after this reachable storage workload. The suite uses
isolated `/advui_hil_*` files for messages, seen nodes, UI/radio preferences and
the BLE crash guard, and clears them at both boundaries; their production
counterparts are never opened or replaced. A second in-memory ingress replays the stock BLE
companion config stream through `AdvBle` without opening a radio link: `my_info`,
a 66-entry node DB against the bounded 64-slot mirror, advertised keys and
battery, channels, LoRa/device config and `config_complete_id` are asserted from
the resulting state. A second `my_info` on the same logical connection then
proves that a resync replaces stale nodes, channels and configuration and
recounts the new snapshot. A mesh packet then crosses the real companion RX queue into
the production UI handler, with its drop counter and heap floor checked. Fixture cleanup
also initializes the on-demand companion buffer block and reapplies the 12 KiB
runtime heap floor before those scenarios proceed. Before the onboard-storage workload,
HIL explicitly releases that companion-only arena, matching the mandatory reboot between
radio backends; the heap gate therefore never measures an impossible co-resident mode.

Every state response carries a per-boot nonce and the ESP reset reason. The host
pins the first nonce for a session and aborts the dependent scenario immediately
if it changes unexpectedly; a restarted device can no longer turn one crash into
a cascade of misleading state failures. Message-ring persistence waits for three
quiet seconds before the atomic LittleFS rewrite, avoiding overlap with archive
appends during an RX burst. Under a permanently busy stream, archive evictions
remain durable and only the bounded 32-message live tail waits for a quiet gap;
leaving a thread and the explicit persistence path still flush synchronously.

This is deliberately a hybrid fixture model. Behavioral input scenarios live on the host
and are encoded as ordinary `FromRadio` protobufs; the device consumes them in
RAM and, where persistence is under test, writes only its HIL namespace. Direct
`Msg` array seeding is reserved for the 29-screen visual/stress matrix, where the
decoder is not the subject and dense state is useful. Writing production storage
files directly would skip exactly the parsing, deduplication and migration logic
the release gate is meant to verify.

`rf_hil.py` adds the same randomly keyed `ADV-HIL` channel at index 2 on two
policy-authorized WiFi fixtures, aligns only their over-the-air LoRa profile
(region, modem parameters and transmit power while preserving per-device calibration,
FEM/LNA and network policy), and verifies a
hop-limited directed packet plus an explicit routing ACK in each direction. It restores both saved exports in a
`finally` block. Cleanup may erase index 2 only after proving that channels 0
and 1 still exactly match the backup and that index 2 is either disabled or has
the reserved test name. It then reads the disabled slot back to prove the test
key is gone. The final full protobuf fingerprint must equal the backup or the
suite fails. Channel keys, channel URLs and message bodies are excluded from
reports.

If fixture backups use different regions, the runner refuses to guess. For this
lab, select `--profile-from n77b`: its US profile, including 26 dBm transmit
power, is deliberate and required for connectivity. The runner must never
silently convert it to RU/20 dBm. Only the named source profile is copied for the
temporary private exchange, and every node is restored byte-for-byte from its
own freshly captured export afterward. Release automation additionally asserts
`--expect-region US --expect-tx-power 26` before generating a test key or changing
either fixture, so configuration drift fails safely.

`visual` renders controlled in-RAM fixtures, captures every major screen, then
soft-reboots and verifies that the persisted real UI state is available again. Evidence lands under
`hil-artifacts/<timestamp>-*/` as `report.json`, `junit.xml`, raw RGB332 frames,
PNGs when Pillow is installed, and SHA-256 hashes.

## Coverage matrix

| Area | Current automation | Additional fixture needed for physical proof |
|---|---|---|
| Boot, heap, serial protocol | USB HIL state assertions | programmable USB power switch for cold-boot loops |
| UI navigation and settings pages | injected keys + state assertions | key actuator/matrix jig to verify every physical key |
| Display and all major screens | framebuffer digest + 29 real captures | camera for panel/backlight/colour verification |
| Unicode, emoji, replies, reactions, history screens | real serialized FromRadio injection + controlled visual fixtures | extend private RF traffic to every live message type |
| LoRa send/receive | state-preserving private RF exchange in both directions | programmable attenuator for sensitivity/range thresholds |
| Routing ACK/NAK | injected production handler path plus explicit ACK for private directed RF in both directions | programmable attenuator for deterministic failure/retry thresholds |
| PKI DM | policy-bound WiFi fixtures and directed RF foundation | state-preserving PKI-key/message assertions |
| BLE companion state mirroring | serialized `my_info`, 66-node bounded mirror, channel/config and queued mesh-packet replay through production `AdvBle` routing | actual BLE GATT fixture for reconnect/admin and PIN handling |
| Persistence and reboot recovery | native storage tests; HIL-isolated save/reload, selective deletion, saturated seen-ledger replacement and pre/post production-config equality | power-cycle controller and filesystem snapshots |
| WiFi, MQTT and NTP | read-only UI coverage | isolated AP/broker with test credentials |
| Speaker, RGB LED, battery and second display | code/build coverage | microphone, photodiode, programmable supply and panel fixture |

The distinction matters: a framebuffer test can prove the renderer but cannot
prove that the physical LCD is lit; an injected key can prove navigation but not
that every keyboard switch closes. Those last columns are genuine physical HIL,
not problems software-only assertions can honestly claim to cover.

## Release gate

Version tags (`v*`) do not publish immediately. After host and firmware checks,
GitHub Actions schedules `release-hil` on a trusted macOS runner labelled
`self-hosted`, `macOS`, and `meshtastic-adv-hil`. Pull requests and ordinary
branch pushes can never reach USB devices. A release is created
only after the exact tagged source passes USB HIL, the emulated FromRadio suite,
visual checks, reboot recovery and a secret-free pre/post configuration equality
gate; production firmware is restored even on failure. RF routing is provided by
the embedded upstream Meshtastic engine and is
not re-tested as a release condition for the ADV UI layer. The private RF runner
remains available as an explicit, manual integration diagnostic.
The host gate also runs pinned `actionlint` validation over the workflow itself.
The macOS runner must provide Homebrew `python3.12`; the job creates a private
virtual environment under `RUNNER_TEMP`. It deliberately does not use
`actions/setup-python`: downloadable macOS Python builds from that action are
non-relocatable and require privileged access to `/Users/runner/hostedtoolcache`.

Tags must use SemVer syntax. A stable `vX.Y.Z` tag updates the web installer and,
when credentials are configured, M5Burner. A tag such as `vX.Y.Z-rc.1` passes the
same build and physical HIL gates but is published as a GitHub prerelease with
`latest=false`; it cannot replace either stable distribution channel. Malformed
tags fail before publication.

Configure the protected GitHub environment `release-hil` with:

- secret `HIL_FIXTURE_JSON`: a schema-2 fixture binding the Cardputer DUT by USB
  MAC, listing every forbidden USB device (including Leshy) under
  `protected_devices`, and pinning its intentional `expected_region: "US"` and
  `expected_tx_power: 26`. No WiFi peers or RF credentials are required by the
  release job.

The job writes the fixture only below the runner's private temporary directory
with owner-only permissions. Stable USB identities and local serial paths are
redacted from JSON and logs, as is the DUT's stable mesh node ID. It uploads the remaining JSON, JUnit and logs, then
removes the fixture in an `always()` cleanup step.
