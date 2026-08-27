# Changelog

Notable user-visible and release-engineering changes are recorded here. Earlier
release notes remain available on the [GitHub Releases page](https://github.com/anton-vinogradov/meshtastic-adv/releases).

## [Unreleased]

This is the planned scope of the Meshtastic ADV 1.0 release candidate.

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

[Unreleased]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v0.5.2...HEAD
