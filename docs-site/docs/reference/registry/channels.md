---
title: Channels
description: Generated tables of channel classes, stream kinds, access levels, priority classes, channel id ranges and the spec-core channels.
register: IEEE
generated: true
---

<!-- ==========================================================
     GENERATED FILE. DO NOT EDIT.
     Source of truth: docs/slopsync/registry/registry.yaml
     Generator:       docs-site/tools/gen_docs_tables.py
     Regenerate:      python docs-site/tools/gen_docs_tables.py
     CI gate:         python docs-site/tools/gen_docs_tables.py --check
     Hand edits are overwritten and fail the docs build.
     ========================================================== -->

# Channels

A channel is a named, numbered, typed data flow declared in the catalog.
Its class decides the frames it uses and the delivery rules it obeys.

## Channel classes

| Value | Class | Notes |
|---|---|---|
| `0` | `STATE` | §9.1 |
| `1` | `STREAM` | §9.2 |
| `2` | `INTENT` | §9.3 |
| `3` | `EVENT` | §9.4 |
| `4` | `STORE` | §8.7 |

The word "STREAM" is a channel class. Transports are described as
*stream-oriented* or *datagram-oriented*, never as stream transports.

## Stream kinds

A STREAM channel declares what one sample **is**. The congestion rules
read this property. They never guess it from a unit string.

| Value | Kind | Meaning |
|---|---|---|
| `0` | `samples` | dense points reporting a value AT AN INSTANT (§9.2); a dropped sample is recoverable by interpolation from its neighbors. Decimable under congestion. The default — absent on the wire means this. |
| `1` | `segments` | each sample COMMANDS A TIME EXTENT — it carries its own duration, so it is not a point on a continuous curve. A dropped segment is a permanently lost COMMAND, not a recoverable interpolation gap (RFC-014/023). NOT decimable: §10.4's shedding table sheds whole-source or not at all for these. |

## Access levels

One session holds one access level. A channel declares the level a
session needs to subscribe to it, or to send on it.

| Value | Level |
|---|---|
| `0` | `watch` |
| `1` | `control` |
| `2` | `configure` |

The safety operations `estop` and `stop` are exempt from these levels.
Any session may stop the machine.

## Priority classes

A subscription carries a priority class. The lower number sheds first.

| Value | Class | Notes |
|---|---|---|
| `0` | `background` |  |
| `1` | `normal` |  |
| `2` | `elevated` |  |
| `3` | `critical` |  |

## Channel id ranges

| Range | Name | Notes |
|---|---|---|
| `0x0000` | `SESSION` | session-scoped frames; never subscribable |
| `0x0001-0x007F` | `spec-core` | allocated below; spec-governed |
| `0x0080-0x7FFF` | `device-defined` | hub firmware allocates; described by catalog. RFC-047: the RECOMMENDED allocation shape is the 0xCDSS grid (class nibble 1=STATE/2=STREAM/3=INTENT/4=EVENT/5=STORE, domain nibble device-chosen subsystem, slot byte) — see docs/slopsync/CHANNEL-MAP.md for the worked grid and SlopDrive-32's own renumber. Within this range, 0x7000-0x7FFF is reserved EXPERIMENTAL/VENDOR play space and MUST NEVER appear in a shipped catalog. |
| `0x8000-0xFFFF` | `reserved` |  |

## Spec-core channels

These channel ids mean the same thing on every hub. A hub still declares
each one it implements in its catalog. A channel absent from the catalog
does not exist on that hub.

| Id | Name | Class | Notes |
|---|---|---|---|
| `0x0001` | `catalog` | `STATE` | catalog meta: etag, chunk count, entry count |
| `0x0002` | `session-roster` | `STATE` | RFC-047 §3: allocated and specified (RFC-018), NOT implemented — no reference catalog builder declares it (see RFC-QUEUE.md deferred ledger, SPEC.md §18). `status: reserved` is the machine-checkable fact the registry never carried before: a channel can be allocated and described on paper without any conformant hub being able to claim it is live. Specified layout: generation u16 + count u8 + flags u8, then 8 packed slots {session_id u32, role u8, flags u8, name str16} = 8x22 + 4 = 180 B, inside the 242 B floor at default_max_clients_ws 8. The str16 name (feasibility pass) is intended to FIX the never-replayed-join-events blocker: a late joiner would learn existing sessions' names from the roster snapshot instead of missed 0x0007 events. Names over 16 B would truncate here; the full name rides 0x0007 while the session lives. |
| `0x0003` | `safety` | `STATE` | latched safety word: estop/stop/hold/pause + cause + owner (§11.1); RFC-025 appends manual_override + bypass_limits to the same snapshot (append-only is legal) |
| `0x0004` | `control-owner` | `STATE` | active arbiter source + owning session per source (§11.4) |
| `0x0005` | `safety-intents` | `INTENT` | STOP/HOLD/PAUSE/RESUME/ESTOP_CLEAR/ESTOP/TAKEOVER + override/bypass (§11, safety_intent_ops) |
| `0x0006` | `hub-status` | `STATE` | boot_id, heap, uptime, transport stats. NO fw version: RFC-016 puts identity in WELCOME `identity` — one home, no drift. |
| `0x0007` | `session-events` | `EVENT` | join/leave/takeover/eviction notifications |
| `0x0008` | `log` | `EVENT` | RFC-017: device log in-band — {level u8, tag, hub-ms, message <=128 B} via the `body` sub-map. Bounded drop-oldest with the §9.4 visible drop counter, `background` priority, `watch` access. Declares a replay depth (log_replay_depth_default) so the hub MAY replay its ring tail on grant — the named exception to §9.4's no-replay rule. Retires /api/log; the serial-quiet handoff re-binds from 'first HTTP GET' to 'first log grant'. |
| `0x0009` | `session-admin` | `INTENT` | RFC-018: {op: evict, session_id} -> hub GOODBYEs the target with SESSION_EVICTED. `configure` access — and note RFC-027 makes configure reachable BY CEREMONY now (§12.2's 'admin only via the hub's own UI' sentence is struck), so this power is deliberately pairing-reachable. |
| `0x000A` | `pending-pairing` | `STATE` | RFC-027(a) knock-and-approve: the bounded pending list (pairing_pending_max) as protocol state — {generation u16, count u8, flags u8} + slots {instance_id u64, kind u8, expires_s u16, name str16}. Any `configure` session approves/denies via 0x0009 (session_admin_ops pair_approve/pair_deny); the joiner needs one button and no display. TWO FIELD CLARIFICATIONS made when M4b implemented this: `kind` is the `pairing_modes` BIT that produced the knock (1 knock_approve / 2 pin_proof / 4 push_to_pair), so an approving UI can say HOW the device asked; and the countdown is `expires_s`, NOT ms — pairing_window_default_s is 120, i.e. 120000 ms, which does not fit the u16 the slot layout allocates. Seconds is also the only resolution an approval UI can use. `flags` bit0 = a pairing association window is currently OPEN (RFC-027(c) push-to-pair), which is how window state stays observable in-band for a session that connected before the window opened. |
| `0x000B` | `pairing-events` | `EVENT` | RFC-027: knock arrived / approved / denied / expired, plus pairing-window open/close. EVENT twin of 0x000A. |
| `0x000C` | `paired-devices` | `STORE` | trust-ledger store descriptor: {store_id, kind 'trust.ledger', capacity paired_devices_max, per_item_max, name_max}. Items are read/revoked by any `configure` session via BLOB_REQ on blob_namespaces.store + the 0x0009 admin surface. Whole encoded ledger stays under trust_ledger_max_bytes (one NVS page). |
| `0x000D` | `paired-devices-roster` | `STATE` | the 0x000C store's roster: {generation u16, count u8, capacity u8}. On-change, tiny; a generation bump means 're-enumerate' (fetch again over BLOB_REQ). `configure` access — the paired-device list is not open reading. |
| `0x000E` | `safety-events` | `EVENT` | RFC/§9.4 duality: the EVENT TWIN of the `safety` STATE channel (0x0003). Kinds in `safety_event_kinds`; fields ride the scoped `body` (40) sub-map keyed by this channel's own catalog schema (word, cause, owner_session, estop_seq, level). `critical` priority and `watch` access, matching its STATE twin exactly — an edge nobody is allowed to be denied and nobody is allowed to shed. Carries `seq_of_state` (34) naming the 0x0003 frame it corresponds to, which is what lets a client that missed the edge reconcile against the latch it DID receive. Emitted on TRANSITIONS ONLY: a repeated ESTOP frame re-broadcasts the STATE (that is §11.2's loss recovery) but does NOT re-emit the edge, because an edge that did not happen is a lie. |

