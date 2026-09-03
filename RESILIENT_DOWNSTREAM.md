# Resilient MM6108 downstream

This repository is the Resilient Networks downstream of the Morse Micro
driver. The first downstream line is based on the immutable upstream release:

- driver tag: `mm6108-2.0.1`
- driver commit: `98e1936c04ef9a62212c1c64b970218ecf08d15d`
- MMRC submodule commit: `da1425580bd5caa1a8fc926596f366bdd8d841d2`
- downstream line: `resilient/mm6108-2.0.1-r1`

Firmware and HaLow userspace are not forked by this change. Production builds
must continue to use the matching upstream `mm6108-2.0.1` firmware and hostap
release until an independently qualified compatibility change is approved.
The fork uses an absolute official Morse Micro URL for the MMRC submodule so a
GitHub fork cannot silently reinterpret the relative upstream URL as a
Resilient-owned dependency.

## Why this downstream exists

During the 2026 Burning Man deployment, an RPi4 Edge on a strong direct link to
the RPi5 Coordinator logged five host-to-chip rejects of a 2,353-byte SKB for a
1,632-byte firmware page. The Coordinator logged none. Both nodes used
byte-identical driver, firmware, board configuration, module options, and a
healthy 12 MHz SPI transport. Neither node showed a Morse firmware crash,
CMD53/checksum fault, or Raspberry Pi throttle event during the sampled boot.

`morse_pageset_write()` removes a page from a host FIFO before validating the
SKB length and tailroom. Upstream releases from the first public history through
current `main` return directly on either validation error without restoring that
page. Each reject can therefore permanently reduce the host's usable TX-page
cache until the driver is reinitialized. The 2.0.1 refactor also stopped
preserving the reserved-vs-cached origin on a host page-write failure.

The r1 change restores the exact FIFO origin on every failure that occurs before
the page is handed to firmware. It also adds bounded diagnostics and counters:

- `TX oversize rejected`
- `TX tailroom rejected`
- `TX page restored`
- `TX page restore fail`

The diagnostic records only structural metadata: channel, payload/SKB/write
lengths, page size, offset, and GSO type/size/segment count. It never records
frame payload, addresses, keys, or application content.

Both loadable modules identify this downstream unambiguously as
`0-rel_mm6108_2_0_1_resilient_r1_2026_Sep_01`; they must not be published under
the unchanged upstream module version.

## What is known and unknown

Known:

- The page-accounting defect is in the host driver and is independent of RF.
- A 2,353-byte object reached the page writer after the Morse host-interface
  header and alignment were applied.
- The selected firmware page was 1,632 bytes.
- Page restoration is safe under the existing pageset mutex and should always
  have room because the same call just removed exactly one FIFO entry.

Not yet known:

- Which producer created the oversized object.
- Whether it was unsegmented GSO, unexpected mac80211 A-MSDU construction,
  conversion growth, a nonstandard MTU, or another upper-layer invariant
  violation.
- Whether the object is repeatable under a controlled packet-size sweep.

The r1 fix deliberately drops the rejected SKB as upstream already does. It
does not segment, truncate, or send a partial frame. The new metadata must be
collected before changing mac80211 aggregation, MTU, or segmentation behavior.

## Engineering and release policy

Every downstream release must:

1. Record the upstream tag and exact commit.
2. Keep the downstream patch series small and independently revertible.
3. Build both `morse.ko` and `dot11ah.ko` against the exact production kernel
   ABI for RPi4 and RPi5.
4. Record kernel release, module vermagic, source commit, submodule commit, and
   module SHA-256 values in the signed runtime manifest.
5. Preserve the matching official firmware and BCF hashes unless a separate
   firmware qualification explicitly changes them.
6. Pass source-contract checks, an exact-kernel compile, and page-accounting
   fault tests before hardware qualification.
7. Qualify in order: simulated/fault test, powered bench pair, isolated field
   pair, then a bounded fleet canary.
8. Retain an exact rollback artifact containing the prior module pair.

Never overwrite the active modules of a live field node without a signed,
transactional runtime or A/B OS update and a tested rollback.

## Resilience roadmap

### R1: page accounting and evidence

- Restore rejected pages to their exact FIFO.
- Count oversize, tailroom, restoration, and restoration failure events.
- Capture rate-limited non-payload metadata needed to identify the producer.
- Assert that every host-owned TX page is in exactly one state: reserved FIFO,
  cached FIFO, selected under lock, submitted to firmware, or recovery path.

### R2: reject before consuming scarce resources

- Derive the effective maximum host-interface object size from the active page
  table rather than a duplicated constant.
- Reject or correctly segment invalid objects before they enter the off-chip
  SKB queue.
- Add per-channel drop reasons and queue residence-time histograms.
- Add explicit high/low watermarks and hysteresis so mac80211 backpressure is
  driven by usable pages and pending TX status, not only SKB queue length.

### R3: mesh formation and recovery observability

- Timestamp module ready, interface created, mesh joined, first peer, first
  forwarding path, first overlay session, and stable-peer milestones.
- Export path-change reasons, route expiry, retry/failure deltas, and queue-stop
  duration rather than only cumulative counts.
- Distinguish RF loss, firmware liveness, SPI transport faults, route churn,
  and overlay handshake failure. Never reset the radio solely because an IP
  probe failed.

### R4: rate, airtime, and transport adaptation

- Validate MMRC decisions against real S1G rates; standard `iw` VHT aliases are
  not authoritative HaLow throughput.
- Instrument retry-chain selection, airtime per peer, aggregation decisions,
  and head-of-line blocking.
- A/B queue disciplines, QUIC datagram size, aggregation, and rate tries under
  repeatable attenuation and load. Do not tune from RSSI alone.
- Prioritize control, DNS/captive, and telemetry traffic over OTA bulk transfer;
  coordinate one bounded OTA stream per relay branch.

### R5: hardware-condition correlation

- Correlate driver counters with voltage/current, Pi throttle flags,
  temperature, wind, enclosure temperature, antenna movement, and dust events.
- Preserve azimuth, mast height, cable/connector type, torque mark, and node
  topology with each field snapshot.
- Treat SPI frequency changes as bus experiments and antenna/relay changes as
  RF experiments; do not mix both variables in one qualification case.

## Qualification gates for r1

The first hardware canary must show:

- no decrease in total host-owned page accounting after an induced oversize
  rejection;
- `TX oversize rejected` and `TX page restored` increase together;
- `TX page restore fail` remains zero;
- commands and beacons retain their two reserved pages;
- no new CMD53, checksum, firmware crash, watchdog-reset, or interface-loss
  event;
- normal direct-link traffic resumes without a module reload;
- a reboot into the upstream module pair remains available as rollback.
