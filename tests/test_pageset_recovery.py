#!/usr/bin/env python3
"""Source-contract regression for Resilient TX-page recovery.

This is intentionally a fast source invariant, not a replacement for compiling
against the production kernel or exercising the hardware fault path.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if not match:
        raise AssertionError(f"missing function: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {name}")


def guarded_block(body: str, condition: str) -> str:
    start = body.find(condition)
    if start < 0:
        raise AssertionError(f"missing rejection condition: {condition}")
    brace = body.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing block for: {condition}")
    depth = 0
    for index in range(brace, len(body)):
        if body[index] == "{":
            depth += 1
        elif body[index] == "}":
            depth -= 1
            if depth == 0:
                return body[brace : index + 1]
    raise AssertionError(f"unterminated block for: {condition}")


pageset = (ROOT / "pageset.c").read_text(encoding="utf-8")
morse_h = (ROOT / "morse.h").read_text(encoding="utf-8")
debug_c = (ROOT / "debug.c").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
dot11ah_makefile = (ROOT / "dot11ah" / "Makefile").read_text(encoding="utf-8")

get_page = function_body(pageset, "tx_page_get_for_channel")
restore_page = function_body(pageset, "tx_page_restore")
write_page = function_body(pageset, "morse_pageset_write")

assert "*pool = TX_PAGE_POOL_RESERVED" in get_page
assert "*pool = TX_PAGE_POOL_CACHED" in get_page
assert "kfifo_put(&pageset->reserved_pages" in restore_page
assert "kfifo_put(&pageset->cached_pages" in restore_page
assert "tx_page_restore_fail++" in restore_page

for condition in (
    "if (write_len > page.size_bytes)",
    "if (write_len > (skb->len + skb_tailroom(skb)))",
):
    block = guarded_block(write_page, condition)
    assert block.index("tx_page_restore(pageset, &page, pool)") < block.index(
        "return -ENOSPC"
    )

write_call = write_page.index("morse_pager_hw_page_write")
write_failure = guarded_block(write_page[write_call:], "if (ret)")
assert "tx_page_restore(pageset, &page, pool)" in write_failure
assert "kfifo_put(&pageset->cached_pages, page)" not in write_failure

for counter in (
    "tx_oversize_rejected",
    "tx_tailroom_rejected",
    "tx_page_restored",
    "tx_page_restore_fail",
):
    assert re.search(rf"unsigned int\s+{counter};", morse_h)

for label in (
    "TX oversize rejected",
    "TX tailroom rejected",
    "TX page restored",
    "TX page restore fail",
):
    assert label in debug_c

assert "skb_shinfo(skb)->gso_type" in write_page
assert "%pM" not in write_page

downstream_version = "0-rel_mm6108_2_0_1_resilient_r1_2026_Sep_01"
assert downstream_version in makefile
assert downstream_version in dot11ah_makefile

print("Resilient Morse TX-page recovery source contract passed")
