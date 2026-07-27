---
title: QoS and congestion
description: >-
  SlopSync clause 10: priorities and the never-shed set, the grant model,
  per-binding congestion signals, the normative shedding table, and ingress
  limits.
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

# 10. QoS, Flow Control, Congestion *(normative)* {#s10}

## 10.1 Priorities and the never-shed set {#s10-1}

Subscriptions carry a priority class (`priority_classes`): `background(0)` sheds first, then `normal(1)`, then `elevated(2)`. Class `critical(3)` is the **never-shed set**: INTENT, ECHO, ESTOP, NACK, GRANT, GOODBYE, and any channel the catalog marks critical (at minimum `safety`, `safety-events` and `control-owner`). Never-shed traffic is tiny by design; [§10.4](#s10-4) defines what happens when even that cannot drain.

## 10.2 The grant model {#s10-2}

A grant is `{channel_id, granted_rate_hz (14), priority (13)}` for subscriptions and `{channel_id, granted_rate_hz, burst (42)}` for publications — the hub's **applied** answer, communicated in WELCOME (batch, keys 35 / 36) or in GRANT frames. Rules:

- The hub MUST echo **granted** values, and MUST NOT silently deliver less than it granted for longer than a congestion transient — that is what re-granting is for. Wishes are clamped by catalog `max_rate_hz`, session tier, hub capacity, and link estimate.
- **Unsolicited GRANT** (same frame, hub-initiated) re-states current grants whenever the hub changes them: a new high-priority client joined and the pie re-split; the probe justified a raise; sustained congestion forced a cut. Clients MUST comply immediately and SHOULD reflect grant changes in their UI — a scope view showing 60 Hz when granted 20 is lying, and [§1.2-1](foundations.md#s1-2) applies to meta-state too.
- A PUBLISH ([§6.7](session.md#s6-7)) is answered with a GRANT carrying `granted_publishes` **even when nothing was granted**; an empty result is the answer, not silence.
- Grant changes never apply to the never-shed set; its rate is intrinsic.

## 10.3 Congestion signals are per-binding {#s10-3}

The hub detects congestion with the signal native to each binding (declared in the [§13.1](transports.md#s13-1) matrix): TCP-backed bindings use **per-client egress queue watermarks**; ESP-NOW uses **ACK-bitmask loss rate** ([§13.3](transports.md#s13-3)); BLE uses notification-queue depth. Thresholds: sustained > 50 % watermark or > 10 % loss over 1 s ⇒ congested; < 20 % / < 2 % for 5 s ⇒ recovered. On congestion: shed per [§10.4](#s10-4); if sustained > 5 s, re-grant downward ([§10.2](#s10-2)) so that the advertised truth matches the throughput.

Congestion is expressed to the shedding table as a per-subscriber **congestion level**: `0` = clear, `1` = congested, `≥ 2` = severe.

## 10.4 The shedding table *(normative)* {#s10-4}

Two conforming hubs under identical load must shed identically, or a client can predict neither. The table below is the reference behavior and is normative. It is evaluated per (subscriber, channel), in the order the rows are written — the **first** matching row wins.

| # | Condition | Decision |
|---|---|---|
| 1 | congestion level 0 | **Send** |
| 2 | priority `critical` | **Send** (all levels, all classes) |
| 3 | first push since this grant | **Send** ([§9.1](channels.md#s9-1) — never strand a session mid-adoption) |
| 4 | `segments`-kind STREAM, level 1 | **Send** |
| 5 | `segments`-kind STREAM, level ≥ 2, priority `elevated` | **Send** |
| 6 | `segments`-kind STREAM, level ≥ 2, priority `background` or `normal` | **Drop whole source** |
| 7 | level 1, `background`, STREAM | Decimate 4× |
| 8 | level 1, `background`, STATE | Conflate hard |
| 9 | level 1, `background`, other classes | **Send** |
| 10 | level 1, `normal`, STREAM | Decimate 2× |
| 11 | level 1, `normal`, other classes | **Send** |
| 12 | level 1, `elevated` | **Send** |
| 13 | level ≥ 2, `background` | Drop |
| 14 | level ≥ 2, `normal` | Decimate 4× |
| 15 | level ≥ 2, `elevated` | Decimate 2× |

Decision meanings: **Decimate** thins a sample stream, always **newest-biased** — preserve the most recent samples, drop the older ones. **Conflate hard** stretches a periodic STATE channel toward on-change-only; depth-1 queues already conflate, this makes it aggressive. **Drop** discards. **Bounded EVENT queues** drop *oldest* with the visible counter ([§9.4](channels.md#s9-4)) independently of this table.

A hub MUST NOT **delay-and-burst**. A stale motion sample is worse than a missing one: timestamps make dropped samples recoverable by interpolation, whereas stale delivery is a lie.

**The segment exception (rows 4–6) is the one place that rationale does not hold.** "Dropped samples are recoverable by interpolation" is true for dense position samples and **false** for timed segments — a shed segment is a permanently lost command. Segment-class channels therefore shed **whole-source or not at all**; they are never decimated. A hub determines segment class from the catalog's `stream_kind` ([§9.2](channels.md#s9-2)), never from a heuristic.

**Slow-consumer eviction.** If the *never-shed* queue itself cannot drain for `never_shed_stall_eviction_ms` (2 s), the subscriber is broken: the hub sends GOODBYE `SESSION_EVICTED` and closes, running the [§6.9](session.md#s6-9) teardown. One incurable client MUST NOT be allowed to consume hub RAM or airtime indefinitely.

**ESTOP is exempt from even that queue.** It is written ahead of every queue at the binding layer ([§11.2](safety.md#s11-2)) and is 12 bytes — a link that cannot carry 12 bytes is a dead link, and evicting a dead link is not a safety event, because the latch ([§11.2](safety.md#s11-2)) does not depend on any one subscriber observing it.

## 10.5 Ingress rate limiting {#s10-5}

The hub bounds client→hub traffic. Intents are limited per [§9.3](channels.md#s9-3). STREAM input is limited per grant.

**STREAM-ingress enforcement is on samples per second, not bundles per second** (a bundle batches up to 32 samples). The hub meters each granted publication with a per-session **token bucket**: refill rate = the granted sample rate, capacity = `burst` if the wish asked for one, otherwise the granted rate (one second of headroom, the same shape as the intent limiter). Each accepted bundle consumes `n` tokens. A bundle that would overdraw is dropped **whole**, and the session is sent NACK `RATE_LIMITED` carrying the offending `channel_id` — throttled to `stream_ingress_overage_nack_per_s` (5) per session, because it is back-pressure feedback, not a per-drop echo. Later legal-rate bundles on the same channel continue to be delivered. Persistent overage escalates to slow-consumer eviction through the same never-shed stall path as any other session.

**`burst` is clamped and echoed like every other wish**, into `[granted_rate, granted_rate × max_burst_multiple]` with `max_burst_multiple` = 4. It exists because making the granted rate double as bucket depth forced a genuinely sparse-but-bursty sender — a few segments per second with a 25/s peak — to declare a rate it did not want, misrepresenting itself to admission control just to buy headroom. An **unbounded** client-declared burst would reintroduce the very flood the bucket exists to stop, hence the clamp.

A misbehaving client cannot starve the machine's real-time core by flooding its comms core.

## 10.6 Broadcast media {#s10-6}

On broadcast bindings, one transmission serves all peers, so per-subscriber rate limiting is physically meaningless downstream of the radio. Rule: the effective channel rate on a broadcast segment is the **highest grant among its subscribers**. Per-subscriber grants remain meaningful hub-side — they still drive what the hub *offers* the segment — and on unicast bindings. Relays MAY further decimate per [§14.1](transports.md#s14-1).
