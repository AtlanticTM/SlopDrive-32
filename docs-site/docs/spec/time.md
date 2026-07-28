---
title: Time and sequencing
description: >-
  SlopSync clause 7: hub time as the timebase, the CLOCK exchange, timestamp
  formats and wraparound, sequence numbers, and time through relays.
register: IEEE
generated: true
---

<!-- ==========================================================
     GENERATED FILE — DO NOT EDIT.
     Source of truth: docs/slopsync/SPEC.md
     Generator:       docs-site/tools/gen_spec_pages.py
     Regenerate:      python docs-site/tools/gen_spec_pages.py
     CI gate:         python docs-site/tools/gen_spec_pages.py --check
     Normative text is copied verbatim. Hand edits are overwritten
     and fail the docs build. Edit the specification instead.
     ========================================================== -->

# 7. Time and Sequencing *(normative)* {#s7}

## 7.1 Clock: the hub is the timebase {#s7-1}

All protocol timestamps are **hub time**: microseconds (streams) or milliseconds (state/events) since hub boot. Clients never send their own clock in data frames; they *convert* using an offset learned from CLOCK exchanges.

CLOCK (`0x05`, raw, 13 bytes, unchanged from the port-81 ancestor): the client sends `0x05` + `t0:u32` (client µs); the hub **MUST** reply `0x05` + `t0:u32` (echo) + `t1:u32` (hub µs at receipt) + `t2:u32` (hub µs at send). The client computes `offset = ((t1 − t0) + (t2 − t3))/2` and `RTT = (t3 − t0) − (t2 − t1)`, with `t3` = client µs at reply receipt.

Answering CLOCK is a hub obligation, not an option: a hub that ignores it leaves every streaming client's timestamps uncorrected while the wire carries no signal that anything is wrong.

Clients holding stream subscriptions or publications SHOULD resync every `clock_resync_interval_s` (10 s) and on every RTT spike > 2× median; drift between resyncs is assumed linear and ignored (µs-class drift over 10 s is below sample-offset resolution).

CLOCK exchanges MUST NOT traverse buffering relays unless the relay performs timestamp correction ([§14.3](transports.md#s14-3)); a relay that cannot correct MUST drop CLOCK frames, forcing clients behind it to rely on WELCOME's coarse bootstrap (informative accuracy: ±bundle-interval).

## 7.2 Timestamp formats and wraparound {#s7-2}

- **STREAM:** `t_base` u32 hub-µs (wraps every ~71.6 min) + per-sample u16 µs offsets. Wraparound rule: samples are always near-now; a receiver interprets `t_base` in the ±35.8 min window around its current hub-time estimate. Ancient or far-future values indicate a missed resync, not time travel — resync, don't extrapolate.
- **STATE/EVENT:** u32 hub-ms (wraps ~49.7 days) with the same nearest-window rule.
- `boot_id` ([§6.1](session.md#s6-1)) fences all of it: a new `boot_id` voids all prior timestamps, seqs, and offsets.

**Consequence, stated because it surfaces in the trust ledger ([§12.6](security.md#s12-6)):** the protocol's own clock is boot-relative and wrapping. A hub can populate a wall-clock field (a "first paired at" timestamp) only if the *application* has a real time source and supplies it. **A hub with a wall-clock source SHOULD populate wall-clock fields it declares** (RFC-049d); zero is the honest default where no such source exists, and will be common. The protocol never invents one — it is not audit-grade, and a client MUST NOT present it as such. A client displaying a wall-clock field SHOULD visually distinguish a populated value from zero (e.g. "unknown" rather than a literal epoch date), so an operator can tell "this hub has no clock" from "this event genuinely happened at boot."

## 7.3 Sequence numbers {#s7-3}

`seq` is u16, **per channel per direction**, incrementing by 1, wrapping mod 2¹⁶, compared by serial arithmetic: `a` is newer than `b` iff `0 < (a − b) mod 2¹⁶ < 2¹⁵`. Class-specific rules:

- **STATE:** newest-wins by seq — a frame older than the shadow's seq is silently dropped. This, not arrival order, is what defeats reordering on datagram bindings. Gaps are meaningless (conflation is legal and expected).
- **STREAM:** bundles carry seq; consumers drop any bundle not newer than the last accepted, and MAY drop individual samples older than the newest rendered timestamp. Gap tolerance is the consumer's business — timestamps, not seqs, drive interpolation and scheduling.
- **INTENT/ECHO:** the header `seq` is used for frame-level correlation only ([§16.1](errors.md#s16-1)); intent ordering and idempotency are carried by `intent_id`.
- **EVENT:** seq present; used for duplicate suppression on at-least-once delivery paths, and referenced by `seq_of_state` (34) to name the STATE frame an edge corresponds to.

## 7.4 Time through relays {#s7-4}

See [§14.3](transports.md#s14-3). Summary: a relay MUST either correct timestamps for its buffering delay, or be transparent to CLOCK (zero added asymmetry), or drop CLOCK frames entirely. Exactly one of the three.
