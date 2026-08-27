# Changelog

Notable user-visible and release-engineering changes are recorded here. Earlier
release notes remain available on the [GitHub Releases page](https://github.com/anton-vinogradov/meshtastic-adv/releases).

## [Unreleased]

No user-visible changes are pending after 1.0.3.

## [1.0.3] - 2026-08-28

### Fixed

- Retrying a failed quoted DM now preserves its Meshtastic `reply_id`; rendered
  quote context is resolved only against an earlier message in the current
  conversation instead of an unrelated packet-ID collision.
- Settings now cover every region and device role implemented by the pinned
  2.7.26 engine, plus the active regional/common TX power values. The deliberate
  US/26 dBm profile is preserved, and an unknown value refuses to open rather
  than silently becoming the first picker option.
- Long WiFi/MQTT fields keep their cursor visible by horizontally advancing on
  complete UTF-8 runes.
- Duplicate packets, unchanged reactions/ACKs and unrelated companion envelopes
  no longer trigger a framebuffer redraw, LED flash or favourite alert.
- The manual screenshot driver retries truncated serial frames and never writes
  partial black PNGs or paths outside its selected output directory.

### Changed

- Recurring UI, BLE and keyboard intervals use the engine's rollover-safe
  throttle helper. Dead keyboard multi-tap state was removed, reducing the
  persistent keyboard object and simplifying held-key repeat timing.
- Behavioral HIL now proves failed-reply retransmission on the actual wire field
  and asserts that duplicate or ignored ingress is a no-op for visible UI state.
- The installer validator checks all six archived images/manifests, exact index
  order and uniqueness. Archive pruning validates every direct SemVer child
  before any bounded removal instead of passing repository names to shell `rm`.
- M5Burner publication now downloads the credential-free public CDN artifact and
  verifies its size and SHA-256 against the tagged merged image.
- Release and HIL environments bootstrap an audited, hash-locked `pip`, then
  install the hash-locked PlatformIO CLI without its unused vulnerable Home web
  stack. nanopb's gRPC generator is declared and locked instead of being
  downloaded during a clean build.
- Official GitHub Actions are pinned to their current Node 24 releases instead
  of relying on GitHub's temporary Node 20 compatibility override.

## [1.0.2] - 2026-08-27

The 1.0 release line delivered the following product and release-engineering scope.

### Added

- Radio-selectable onboard LoRa and BLE companion operation with node, channel,
  configuration and message mirroring through the standard Meshtastic Client API.
- Persistent live history plus a 256-message flash archive, reply context,
  favourites and sender-scoped reactions.
- Full-BMP GNU Unifont rendering, inline emoji, Cyrillic transliteration and a
  keyboard-driven settings UI.
- One-command host, firmware and opt-in hardware verification with JSON/JUnit
  evidence and an explicit HIL coverage matrix.
- Tag-only, identity-bound physical release HIL using the exact application image
  embedded in the release factory artifact.

### Changed

- The embedded Meshtastic engine is pinned to `v2.7.26.54e0d8d`; the 2.8
  development line is deliberately not the release base.
- Companion transport and node-mirror buffers are allocated only in companion
  mode, returning about 8.4 KiB to the normal onboard-LoRa heap.
- Release images, the web installer, version archive and M5Burner bundle are now
  derived from the exact tagged inputs and validated byte-for-byte.
- Prerelease tags publish GitHub prereleases but do not replace the stable web
  installer or publish to M5Burner.

### Fixed

- Reaction identity now includes the target message sender, preventing collisions
  when two nodes reuse the same message ID; AVS3/AVS4 data migrates safely to AVS5.
- Message deletion and history eviction preserve or remove only the reactions that
  belong to the exact affected message.
- Companion queue ownership, reconnect handling, bounded node snapshots and packet
  ingress no longer leave static buffers charged to onboard mode or free interior
  queue-storage pointers.
- Daily advisory-date persistence is deferred until receive bursts settle and
  no longer uses an atomic LittleFS transaction for its recoverable four-byte
  hint.
- UTF-8 truncation, malformed input and legacy-history failure paths are bounded
  and fail without replacing recoverable stored data.
- Visual HIL drains and retries one complete deterministic matrix after a native
  USB row loss, while still rejecting incomplete frames and content regressions.
- Synthetic companion config streams now follow production redraw semantics
  instead of forcing a full display push for every mirrored node.
- Per-feature HIL memory gates use direct/current samples and newly observed
  troughs, so an earlier display allocation cannot be misattributed to storage
  or companion ingress; clean-start checks still enforce the lifetime minimum.

### Release safety

- Physical USB HIL is radio-silent, bound to the intended Cardputer identity and
  refuses every protected attached device; engine-owned radio sends are excluded
  from ADV memory checkpoints instead of allocating packets that TX will reject.
- Production firmware and the original configuration are restored and verified
  even after a failed test stage; configuration fingerprints require two
  consecutive complete exports with identical restorable settings while ignoring
  only the live GPS position reported alongside them.
- Local fixtures and configuration backups are owner-only; uploaded evidence
  redacts USB, serial, node and local-network identities.
- Release publication is serialized, malformed tags fail closed, merged images
  cannot reuse stale output, and prereleases cannot enter stable distribution
  channels.

[Unreleased]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v1.0.3...HEAD
[1.0.3]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v1.0.2...v1.0.3
[1.0.2]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v0.5.2...v1.0.2
