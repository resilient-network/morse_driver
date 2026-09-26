# Resilient MM6108 performance qualification

This r2 work is a build-and-lab candidate. It is not approved for deployment.
It must not replace the r1 module on a Coordinator or Edge until exact-kernel
CI, bench qualification, rollback validation, and an explicit deployment
approval have completed.

## Why the first work is observability

The field baseline on the strong Edge 13 to C001 link was about 4.5 Mbit/s in
either direction while the radio reported roughly -37 to -41 dBm and a 7.52
Mbit/s expected PHY rate. The nodes used a 2 MHz operating channel and a 12 MHz
SPI clock. C001's WAN download rate was hundreds of Mbit/s while Edge 13's
Internet rate was about 3.4 Mbit/s. That evidence makes the host-to-radio bus,
channel width, aggregation, and framing overhead higher-priority hypotheses
than transmit power or the WAN.

RSSI alone cannot identify the limiting layer. r2 therefore adds a stable,
read-only debugfs record named `resilient_performance` below the driver's
existing `morse` debugfs directory. It contains only aggregate counters and
histograms. It does not contain MAC addresses, peer IDs, packet payloads,
credentials, or application content, and reading it never resets a counter.

The record exposes:

- configured S1G frequency and operating/primary bandwidth;
- SPI clock, wire and useful-payload bytes, transfer duration, error count,
  size histogram, and controller DMA eligibility;
- top-half-to-thread IRQ latency and threaded service time;
- maximum TX queue depth/bytes and queue-residence histograms;
- active page size, current cached/reserved pages, queue stop/wake thresholds,
  per-channel page-starvation duration, page-restore latency, and structural
  drop reasons;
- driver TX/RX payload rates, A-MPDU length histogram, and per-MCS/per-bandwidth
  attempts, successes, failures, aggregation observations, and RSSI sums.

The legacy `skbq_mon` reader is also interval-safe in r2. Reading it no longer
erases the identities and current depth of frames that are still awaiting TX
status. Completions for frames that predate monitor activation are reported as
an aggregate `Untracked completions` count instead of false kernel error logs.

`spi_dma_eligible_xfers` means the controller said the transfer could use DMA;
it does not claim the controller actually completed it with DMA. Wire rate and
driver payload rate are separate so framing/padding overhead is visible.

## Transport-error fallback

The new SPI fallback is disabled by default. Both
`spi_error_fallback_threshold` and `spi_fallback_clock_speed` must be set before
module load. It triggers only after consecutive `spi_sync_locked()` errors,
runs outside the transfer path, can only lower the configured clock, and can
apply once per probe. RF signal, throughput, Internet reachability, overlay
state, and IP probes cannot trigger it. Recovery to a higher clock requires a
controlled reload/reboot under an approved lab or update plan.

The CMD53 read/write paths now propagate a failed SPI transaction immediately
instead of parsing a stale transfer buffer. This is a correctness fix as well
as an evidence improvement.

Objects larger than the active firmware TX page are rejected before entering
the off-chip queue; the page writer keeps the same final check. Data queues now
stop when no cached TX page remains and do not wake until two are available.
The existing SKB high/low watermark also includes frames waiting for firmware
TX status, preventing an apparently short host queue from hiding firmware-side
backlog.

## Controlled matrix

Change one independent variable at a time and retain the complete counter
snapshot before and after each run:

1. Establish r1 control runs at 12 MHz SPI and 2 MHz S1G bandwidth.
2. On r2, repeat 12 MHz without enabling fallback to detect instrumentation
   regressions.
3. Sweep SPI at 18, 24, 30, 40, then 50 MHz. Stop on the first new transfer,
   CRC/checksum, watchdog, firmware-liveness, or page-restoration failure.
4. Select the highest error-free SPI setting, return to a clean boot, then
   sweep S1G operating bandwidth from 2 to 4 and 8 MHz where region, firmware,
   BCF, and peer capability allow it.
5. At the selected bus/channel pair, compare mesh against a direct AP/STA lab
   pair and measure single-flow, four-flow, reverse, bidirectional, and
   application-sized datagrams.
6. Repeat with aggregation settings changed one at a time. Use the new MCS,
   retries, A-MPDU, queue, page, IRQ, and SPI evidence to explain each result.

Each case records exact driver/kernel/firmware/BCF hashes, module parameters,
channel plan, antenna/cable, node role, CPU temperature/throttle state, supply
voltage/current, loss, latency percentiles, TCP/UDP goodput, wind, dust, and
enclosure/antenna movement. A result is invalid if bus and RF variables change
in the same case.

## 2026-09-26 live r1 control

Edge 13 and C001 were retested before any r2 deployment. Both nodes used the
September 11 r1 module, a 20 MHz SPI clock, and a 4 MHz S1G channel. RSSI was
-45 to -48 dBm and the driver expected 13.2 to 14.6 Mbit/s. A bounded 15-second
single-flow TCP test delivered 8.98 Mbit/s Edge-to-Coordinator and 7.81 Mbit/s
Coordinator-to-Edge at the receivers. TCP reported one retransmission in the
forward run and none in reverse. MAC retries increased by roughly 5 to 6
percent of transmitted packets during the two runs, while page-write failures,
page starvation, aged TX frames, and queue-stop counts did not increase.

Activating the legacy `skbq_mon` reader immediately before the test produced
false `Unexpected ctr` messages as completions arrived for frames that existed
before the reader was initialized. The r2 interval-safe accounting above was
added from this observation. The live nodes remain on r1; these results do not
qualify r2 for field deployment.

## Hardware and higher-layer work

- Qualify SDIO or USB as an alternate radio transport; a 50 MHz standard SPI
  link remains a hard architectural ceiling even when the RF link is strong.
- Validate shorter RF cable, connector seating/torque marks, antenna clearance,
  mast rigidity, weather sealing, strain relief, cooling, and stable power.
- Give control, DNS, captive onboarding, and telemetry traffic priority over
  bulk OTA traffic. Bound OTA concurrency per relay branch and use resumable,
  delta artifacts. These policies belong above this driver and are not silently
  changed by r2.
- Expose the aggregate r2 fields through the Coordinator telemetry contract
  only after their units and reset semantics are versioned. Cloud management
  must remain observational and must not directly mutate an Edge radio.

## Promotion gates

An r2 artifact may advance only when both modules compile against the exact
RPi4/RPi5 production ABI, source-contract tests pass, the debugfs schema is
complete, r1 rollback artifacts are verified, no payload or identity data is
disclosed, and bench evidence shows no regression in formation, latency,
loss, page ownership, firmware liveness, or reboot recovery. Field deployment
requires a separate explicit approval.
