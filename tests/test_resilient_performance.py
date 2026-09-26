#!/usr/bin/env python3
"""Fast source contract for downstream performance telemetry and safeguards."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if not match:
        raise AssertionError(f"missing function: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {name}")


stats_h = read("resilient_stats.h")
stats_c = read("resilient_stats.c")
spi_c = read("spi.c")
pageset_c = read("pageset.c")
pageset_h = read("pageset.h")
skbq_c = read("skbq.c")
rc_c = read("rc.c")
mac_c = read("mac.c")
makefile = read("Makefile")

assert "resilient_stats.o" in makefile
assert "schema=resilient-morse-performance-v1" in stats_c
for family in (
    "spi_wire_bytes",
    "spi_payload_read_bytes",
    "spi_duration_max_ns",
    "spi_dma_eligible_xfers",
    "irq_latency_max_ns",
    "queue_residence_hist",
    "page_starvation_events",
    "page_restore_max_ns",
    "tx_rate",
    "rx_rate",
    "ampdu_len_hist",
):
    assert family in stats_h

# Aggregate telemetry must never expose peer addresses, payloads, or key material.
stats_output = function_body(stats_c, "read_resilient_performance")
for forbidden in ("%pM", "peer_id", "payload=", "password", "private_key"):
    assert forbidden not in stats_output

xfer = function_body(spi_c, "morse_spi_xfer")
assert "spi_sync_locked" in xfer
assert "morse_resilient_spi_xfer" in xfer
assert "if (ret)" in xfer
assert "schedule_work(&mspi->clock_fallback_work)" in xfer
assert "spi_error_fallback_threshold" in xfer
assert "spi_fallback_clock_speed" in xfer

fallback = function_body(spi_c, "morse_spi_clock_fallback_work")
assert "spi_fallback_clock_speed >= from_hz" in fallback
assert "mspi->clock_fallback_applied" in fallback
assert "morse_spi_setup" in fallback
assert "morse_resilient_spi_fallback" in fallback

# Defaults are fail-safe: no fallback occurs unless an operator sets both parameters.
assert re.search(r"static uint spi_error_fallback_threshold\s*;", spi_c)
assert re.search(r"static uint spi_fallback_clock_speed\s*;", spi_c)
assert "IP" not in fallback and "RSSI" not in fallback

for name in ("morse_spi_cmd53_read", "morse_spi_cmd53_write"):
    body = function_body(spi_c, name)
    assert "ret = morse_spi_xfer" in body
    assert re.search(r"if \(ret\)\s*return ret;", body)

assert "morse_spi_irq_wake" in spi_c
assert "morse_resilient_irq" in function_body(spi_c, "morse_spi_irq_handler")
assert "morse_resilient_page_starvation_begin" in pageset_c
assert "morse_resilient_page_starvation_end" in pageset_c
assert "morse_resilient_page_restore" in pageset_c
assert "morse_skbq_record_residence" in pageset_c
assert "morse_resilient_queue_enqueue" in skbq_c
assert "static DEFINE_SPINLOCK(morse_skbq_mon_lock)" in skbq_c
assert "Untracked completions" in skbq_c
assert "morse_skbq_mon_reset_interval" in skbq_c
monitor_adjust = function_body(skbq_c, "morse_skbq_mon_adjust")
assert monitor_adjust.count("tbl->untracked_completions++") == 2
assert "Unexpected ctr" not in monitor_adjust
enqueue = function_body(skbq_c, "morse_skbq_skb_tx")
assert enqueue.index("morse_pageset_tx_page_size") < enqueue.index("morse_skbq_tx(mq")
assert "return -EMSGSIZE" in enqueue
assert "mq->pending.qlen >= max_txq_len" in skbq_c
assert "morse_pageset_tx_should_stop" in skbq_c
assert "morse_pageset_tx_can_wake" in skbq_c
assert "PAGESET_TX_STOP_PAGES 0" in pageset_h
assert "PAGESET_TX_WAKE_PAGES 2" in pageset_h
for page_counter in ("cached_page_count", "reserved_page_count"):
    assert f"atomic_inc(&pageset->{page_counter})" in pageset_c
    assert f"atomic_dec(&pageset->{page_counter})" in pageset_c
assert "morse_resilient_tx_rate" in rc_c
assert "morse_resilient_rx_rate" in mac_c

version = "0-rel_mm6108_2_0_1_resilient_r2_2026_Sep_21"
assert version in makefile
assert version in read("dot11ah/Makefile")

print("Resilient Morse performance source contract passed")
