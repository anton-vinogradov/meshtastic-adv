# Changelog

Notable user-visible and release-engineering changes are recorded here. Earlier
release notes remain available on the [GitHub Releases page](https://github.com/anton-vinogradov/meshtastic-adv/releases).

## [Unreleased]

No user-visible changes are pending after 1.0.11.

## [1.0.11] - 2026-08-29

### Fixed

- Rolls forward the unpublished 1.0.9 and 1.0.10 candidates. Web UI server
  startup and request processing now fail softly on fragmented-heap `std::bad_alloc`
  instead of rebooting the whole device; the HTTP listener also uses the
  existing two-client resource bound. WiFi PhoneAPI listener initialization
  releases a fully constructed temporary object if its `init()` allocation
  fails.
- Release HIL now checkpoints the complete production LittleFS partition in an
  owner-only recovery vault and restores and verifies it together with the exact
  production app before boot. The durable recovery marker binds the app,
  factory partition layout, configuration export and filesystem snapshot. This
  preserves message history, learned nodes and every other persistent file even
  after an interrupted job.
- The post-restore NodeDB soak no longer compares the persisted cache with a
  larger volatile RF-learned pre-HIL count. It still enforces the fixture floor,
  repeated complete PhoneAPI dumps and a stable reboot counter; the raw
  verified byte-for-byte filesystem restoration now provides the persistence
  guarantee.

## [1.0.9] - 2026-08-29

### Fixed

- Rolls forward the unpublished 1.0.8 candidate after its release gate reported
  a possible loss of the learned node cache: HIL now uses a private NodeDB filename as
  well as disabling saves, so its reduced in-memory database cannot even open
  production `nodes.proto`. The physical suite also proves that the USB DUT and
  production WiFi endpoint expose the same mesh node identity.
- Incoming WiFi PhoneAPI connections and listener startup now fail softly when
  fragmented heap cannot allocate their thread/stream state, instead of letting
  `std::bad_alloc` terminate and reboot the firmware. The network settings page
  formats its IP address directly into a fixed buffer without a temporary
  Arduino `String` allocation.

## [1.0.8] - 2026-08-29

### Fixed

- Rolls forward all runtime and release-safety improvements from the unpublished
  1.0.6 and 1.0.7 candidates: bounded large-NodeDB streams, preservation of the
  production node cache across HIL, lower UI stack demand, and fail-soft BLE and
  audio paths under memory pressure.
- The release-image output block now satisfies the pinned `actionlint`/ShellCheck
  gate, allowing the already verified firmware and private HIL artifacts to
  advance to physical testing and publication.

## [1.0.7] - 2026-08-29

### Changed

- Channel ordering is now stable and allocation-free, and the 24-hour seen-node
  reconciliation workspace is capped to the board's real 150-node limit. This
  removes roughly 1 KiB from the UI task's peak stack demand.
- Release HIL reuses the exact private HIL image already built and size-checked
  by CI instead of compiling it again on the physical runner. The production
  WiFi gate records the live NodeDB count before flashing and requires the
  restored firmware to retain at least that exact baseline throughout its soak.

### Fixed

- HIL firmware can no longer persist its intentionally reduced 32-node in-memory
  database over the user's production `nodes.proto` cache. Configuration,
  identity, channels and the intentional US/26 dBm profile remain unchanged.
- Optional BLE companion connections and audio alerts fail softly under memory
  pressure instead of terminating the firmware. BLE address conversion no
  longer allocates a temporary string, and completed/failed NimBLE sessions
  release their discovered GATT graph before another worker starts.
- Self-hosted release checkouts recover safely from either partially applied
  state of the retained-stream patch. Interrupted-run recovery now runs only
  after HIL actually crossed its verified flash boundary.

## [1.0.6] - 2026-08-29

### Added

- Stable release HIL now soaks the restored exact production image through at
  least eight complete, read-only WiFi PhoneAPI config/NodeDB dumps over two
  minutes. Identity, build environment, WiFi capability, node count and reboot
  counter must remain exact; reconnects, writes and post-baseline retries are
  forbidden.

### Changed

- PhoneAPI node prefetch now reuses one fixed record instead of growing a deque,
  and NodeDB reserves its exact 150-record capacity before decode. File manifests
  are built only after the node stream finishes, while per-port rate limiting
  uses six fixed timestamp slots instead of an attacker-expandable hash map.
- Full USB/TCP config streams are emitted in bounded cooperative slices. ESP32
  WiFi uses zero-wait socket writes, and incomplete required frames are retained
  until the transport accepts the exact remaining bytes.

### Fixed

- Large real-world NodeDB dumps no longer exhaust fragmented heap in
  `PhoneAPI::prefetchNodeInfos()` or starve the loop watchdog under TCP
  backpressure.
- Native USB CDC short writes, temporary internal disconnect state and physical
  session teardown can no longer silently truncate, duplicate or splice framed
  Client API traffic. Partial input/output state is discarded only when the old
  physical USB session has definitively ended. Dead-host logging uses the
  framework's minimum safe timeout instead of triggering its zero-timeout retry
  counter underflow when a terminal disconnects.
- Corrupt, failed or forward-version NodeDB cache decodes now fail closed and
  reuse the already allocated buffer instead of briefly doubling large node
  storage. Persistent identity, channels and the intentional US/26 dBm radio
  profile remain untouched.

## [1.0.5] - 2026-08-28

### Added

- Every tagged release now records a deterministic 19-frame conversation on the
  real firmware: unread navigation, typing, emoji, send status, routing ACK,
  incoming reply, reaction and a quoted Cyrillic reply. The release publishes a
  device-composited hero GIF and a screen-focused GIF for the website, GitHub and
  community posts.
- Public HIL evidence is staged through a fail-closed privacy boundary. Raw
  framebuffer captures stay on the trusted runner; uploaded text is redacted for
  fixture identities, serial paths, local IPv4/IPv6 addresses, node IDs and
  user-scoped home/temp paths, while generated media is hash-bound to the exact
  tag, source frames, timing, mockup and device image. Text evidence also has
  per-file and total byte budgets.

### Changed

- The project website is now a responsive, keyboard-accessible product page with
  a clearer standalone/companion explanation, safety warnings before flashing,
  user-controlled animation, release-confidence details and concise
  troubleshooting. Legacy screenshots containing lab identities and a stale
  RU/20 example are no longer published or embedded.
- The release HIL visual matrix grows from 29 to 35 real framebuffer captures.
  Interface documentation now describes the narrow Meshtastic integration
  patches and channel acknowledgement semantics precisely, and the M5Burner card
  avoids a time-sensitive release-cadence claim.
- A public GitHub Release is now the final distribution marker: assets remain in
  a draft until M5Burner and the exact-byte web installer/media postconditions
  pass. Every external publication rechecks that the remote tag still peels to
  the exact workflow commit, making a moved tag fail closed.

### Fixed

- Interrupted physical HIL recovery now keeps the verified node configuration in
  an owner-only per-run vault outside checkout/temp cleanup, binds a durable
  flash marker to that backup's filename and SHA-256, and refuses recovery
  without the exact tagged app image. Export and restore use the same pinned
  Meshtastic CLI and re-resolve the Cardputer identity around every export;
  transient private samples are removed immediately.
- The self-hosted fixture is attempt-scoped and created with exclusive,
  no-symlink semantics. Successful HIL/recovery removes only the current attempt;
  failed recovery retains its private vault with a documented repair runbook.

## [1.0.4] - 2026-08-28

### Changed

- Stable GitHub Releases now publish the reviewed, version-specific section from
  `CHANGELOG.md` instead of an empty generated-notes page containing only a
  comparison link. The notes are staged from the exact tag before the installer
  checkout moves to `main`, and a safe release rerun repairs both assets and
  notes. A stable tag with missing or ambiguous changelog notes fails before it
  can unlock the physical HIL job.

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

[Unreleased]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v1.0.11...HEAD
[1.0.11]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v1.0.9...v1.0.11
[1.0.9]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v1.0.8...v1.0.9
[1.0.8]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v1.0.7...v1.0.8
[1.0.7]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v1.0.6...v1.0.7
[1.0.6]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v1.0.5...v1.0.6
[1.0.5]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v1.0.4...v1.0.5
[1.0.4]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v1.0.3...v1.0.4
[1.0.3]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v1.0.2...v1.0.3
[1.0.2]: https://github.com/anton-vinogradov/meshtastic-adv/compare/v0.5.2...v1.0.2
