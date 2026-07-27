---
title: Session layer
description: >-
  SlopSync clause 6: identity, HELLO and WELCOME, the readiness gate, the
  network probe, liveness, mid-session subscription management, reconnect and
  teardown.
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

# 6. Session Layer *(normative)* {#s6}

## 6.1 Identity: three numbers, three jobs {#s6-1}

- **`instance_id`** (8 bytes, client-generated once and persisted) — *who this client durably is.* Distinguishes "the same phone reconnecting" from "a second phone". Generated randomly at first run; a client that cannot persist (incognito browser) generates per-load and simply enjoys weaker reconnect and pairing semantics.
- **`session_id`** (u32, hub-assigned, random non-zero, unique within a hub boot) — *this particular association.* Not a secret; authorization lives in tokens ([§12](security.md#s12)).
- **`boot_id`** (u32, hub-generated randomly at every boot) — *which incarnation of the hub.* All hub timestamps, seqs, session ids, and idempotency state are scoped to a `boot_id`; observing a new one invalidates every cached assumption except the catalog etag, pairing tokens, and a pinned hub public key.

## 6.2 HELLO (client → hub) {#s6-2}

CBOR map. Required: `proto_ver` (1), `client_kind` (2), `client_name` (3), `instance_id` (4). Optional: `token` (5), `catalog_etag` (8) — the etag the client has cached — `subscriptions` (10) and `publishes` (11) wish-lists so simple clients complete setup in one round trip, and `trust` (39).

- `subscriptions` entries are `{channel_id, rate_hz, priority}`.
- `publishes` entries are `{channel_id, rate_hz, burst?}`. A c2h STREAM producer has no subscription priority; `burst` is the token-bucket capacity in samples ([§10.5](qos.md#s10-5)).
- `trust` (39) is the optional identity/authenticity sub-map. In HELLO it may carry `client_ver` (1), `client_nonce` (2, 8 bytes of client entropy), and `sig_request` (3). **A client that omits `trust` entirely is on the supported floor**: bearer token, zero crypto, the v1-draft handshake cost unchanged.

**Publication wish validation.** The hub validates each `publishes` wish against the catalog: the channel MUST exist, be class STREAM, be direction c2h, and its effective `access` MUST NOT exceed the session's granted tier. A wish that fails any check is **silently omitted** from the grants — no NACK, because an unwanted publish wish is not an error. A passing wish is granted at `min(wished rate, catalog max_rate_hz)`; a channel whose granted rate resolves to ≤ 0 is not a rate-bearing publish and is omitted. Grants are echoed in WELCOME under `granted_publishes` (36). **A session may send STREAM bundles only on channels granted here or by a later PUBLISH ([§6.7](#s6-7)).**

**Subscription wishes** are answered as grants embedded in WELCOME under `grants` (35), using the same structure GRANT uses ([§10.2](qos.md#s10-2)).

## 6.3 WELCOME (hub → client) {#s6-3}

CBOR map: `proto_ver` (the served version), `session_id`, `boot_id`, `catalog_etag`, `cfg_gen`, `roles` (23 — the granted access tier: `watch` unless a valid token raises it), `limits` (22), `deadman_ms` (24) and `deadman_policy` (25) as applied to this session, `nonce` (29 — 8 bytes, used by a subsequent PAIR_REQ *and* by token-proof presentation), `grants` (35), optionally `granted_publishes` (36), `identity` (37), and optionally `trust` (39).

- `limits` (22) carries at minimum `max_frame` (1), `max_subscriptions` (2), and `retained_pending` (3) — the count of retained STATE pushes that will follow.
- `identity` (37) carries `product` (1), `fw_version` (2), `hub_name` (3), and an optional device-defined `info` map (4) whose keys the protocol never interprets. **This is the only wire home for hub identity.** A hub SHOULD carry it; clients MUST tolerate its absence per [§4.3](foundations.md#s4-3), and MUST NOT make connection or operation conditional on it. Reference-implementation status: [§18-16](limitations.md#s18).
- `trust` (39) in WELCOME may carry `pairing_modes` (8, a bitmask of the association modes this hub offers **right now**, re-evaluated per session so a transient window is advertised only while open) and `welcome_sig` (5) where the hub can sign without stalling ([§12.5](security.md#s12-5)).
- `granted_publishes` is omitted entirely when no publish wish was granted.

WELCOME is the moment grants become truth; anything not granted here needs SUBSCRIBE or PUBLISH.

**Capability discovery is catalog introspection.** There is no capability list in WELCOME and there will not be one. A feature exists **iff its channels exist**: a hub with a current sensor advertises the power channel and a hub without one does not, and that absence *is* the answer. Ceilings and geometry are discovered by `field_roles` ([§8.8](catalog.md#s8-8)), not by a parallel enumeration that can drift.

**Duplicate identity:** if a HELLO arrives bearing the `instance_id` of a live session, the hub MUST evict the old session (GOODBYE `DUPLICATE_INSTANCE` if its transport still functions) and honor the new HELLO. Half-open zombies die here. Because a successful duplicate HELLO **evicts** the incumbent, a second HELLO is never a legal way to change one's own role mid-session — that is what AUTH ([§12.4](security.md#s12-4)) exists for.

**Admission:** a hub at its client limit answers HELLO with NACK `BUSY` carrying `retry_after_ms` (31). A hub's transport-tracking capacity MUST exceed its session capacity by at least one, so that the peer which loses the admission race is still reachable to *receive* its BUSY. Advertised defaults and the conformance floor (≥ `conformance_min_clients`) are in [Appendix G](appendices.md#appendix-g).

## 6.4 Readiness: the dual-plane gate *(CATALOG_READY)* {#s6-4}

A client cannot decode a packed STATE frame without the catalog that describes its layout, and a client MUST NOT act before it has adopted the retained safety latch ([§11.5-2](safety.md#s11-5)). Both problems have one answer.

**The rule.** Every session carries a `ready` flag, initially false. While a session is not ready:

1. the hub emits **no** STATE and **no** STREAM to it — including the retained push;
2. the hub **refuses** inbound INTENTs from it with NACK `NOT_READY`. Refused, not queued: a client acting before adopting the safety latch is exactly the failure [§11.5-2](safety.md#s11-5) forbids.

Nothing is buffered anywhere. Retained values already live once in the hub's channel table; the gate is one flag and costs no RAM, and it never blocks. Frames that are *not* gated: the session and safety planes — PING/PONG, CLOCK, GOODBYE, NACK, PAIR_*, AUTH, BLOB_*, ESTOP, and the safety-intent ops that [§11.2](safety.md#s11-2) makes role-exempt. **You may always stop the machine, ready or not.**

**Becoming ready.**

- **Etag match is proof of possession.** A HELLO whose `catalog_etag` equals the hub's makes the session ready immediately, on the WELCOME. The common reconnect case keeps its zero-latency retained push.
- **Otherwise:** WELCOME advertises the current etag; the client fetches the catalog over BLOB namespace 0 ([§8.4](catalog.md#s8-4)) — which gets the whole pipe, since no telemetry is competing — assembles it, and **verifies the SHA-256 locally**. The hash *is* the acknowledgement; there are no transfer round trips to negotiate. The client then sends **CATALOG_READY** (`0x19`, raw, c2h), payload = the 8-byte etag it now operates against. The hub sets `ready`, the retained push flows, the client reaches LIVE.
- **Loss-proofing:** CATALOG_READY is idempotent. A client re-sends it every `catalog_chunk_gap_timeout_ms` until the first retained STATE arrives. There is no handshake state machine and no hub timer for it.
- **Degraded static clients** ([§8.5](catalog.md#s8-5)) send CATALOG_READY carrying their **stale** etag. Append-only layouts make their prefix-parse safe; the hub serves them and MAY record the session as degraded.

**Timeout.** A session that has not become ready within `catalog_ready_timeout_ms` (15 s) MUST be closed with GOODBYE `READY_TIMEOUT`. This exists because liveness reaping ([§6.6](#s6-6)) never fires on a client that PINGs happily forever: without this rule a half-adopted session would hold a slot indefinitely with both planes gated shut.

*Rationale (informative):* the alternatives were tried and are worse. A hub-side "defer until I have sent the whole catalog" is ambiguous — the hub knows it *sent* chunks, not that they *arrived*, which is true on TCP and false on ESP-NOW. Client-side "discard what I cannot decode" spends airtime shipping frames into a bin. The gate means undecodable state is never transmitted at all.

## 6.5 Network probe (optional, post-READY) {#s6-5}

Grants at WELCOME are deliberately conservative defaults — a controller reconnecting mid-motion must not wait on a bandwidth measurement. A client wanting refinement runs the probe *after* going LIVE:

1. Client sends PROBE (raw, empty payload) → hub replies with a timed burst of PROBE frames (raw payload: `probe_index:u16` + padding) totaling `probe_default_bytes` over at most `probe_max_duration_ms`.
2. Client measures received bytes/span/loss and reports PROBE_REPORT (CBOR: `probe_result` (26), sub-keys `bytes_received`, `span_ms`, `loss_pct_x100`, `rtt_ms`).
3. Hub MAY raise grants accordingly, announced via unsolicited GRANT ([§10.2](qos.md#s10-2)).

The probe measures the hub→client direction. Runtime congestion adaptation ([§10.3](qos.md#s10-3)) continues regardless — the probe sets a better starting point, nothing more.

## 6.6 Liveness, deadman, and idle reaping {#s6-6}

**Any received frame is proof of life.** A dedicated PING (raw, empty; answered by PONG echoing the payload) is sent only when a side has been otherwise silent for its interval: `ping_interval_holding_control_ms` (200 ms) while the session owns an active motion source, `ping_interval_idle_ms` (1 s) otherwise. A 240 Hz streamer therefore never sends PING and never idles out while streaming.

There are **two liveness regimes, deliberately different**:

| Regime | Applies to | Trigger | Consequence |
|---|---|---|---|
| **Deadman** ([§11.3](safety.md#s11-3)) | a session that **owns an active motion source** | silence beyond `deadman_ms` (default 600, clamp 250–5000, negotiated at WELCOME) | the source's **loss policy** fires; safety latches with `cause=deadman`; ownership released |
| **Idle reaping** | every other session | silence beyond `idle_reap_multiplier` × `ping_interval_idle_ms` | the session SHOULD be reaped (GOODBYE if the transport still functions). **No motion consequence** — it owned nothing |

A hub SHOULD implement idle reaping. Without it a watch-tier session that goes dark holds a slot until reboot, and there is no other pressure to release it.

Note the sparse-sender case this design serves on purpose: a client that emits a few timed segments per second ([§9.6](channels.md#s9-6)) holds its session open with [§6.6](#s6-6) PINGs and never needs a protocol change to do it. Pausing playback means the segments stop while the PINGs continue: the session survives, the machine settles, and nothing about the deadman needed special-casing.

## 6.7 Mid-session subscription and publication management {#s6-7}

- **SUBSCRIBE** (`0x06`, c2h): CBOR `subscriptions` array as in HELLO; answered by GRANT per entry, or NACK carrying the offending `channel_id`. Rate or priority changes are a re-SUBSCRIBE of the same channel — the new grant replaces the old one. Subscriptions are capped per session (`max_subscriptions_per_session`, NACK `SUB_LIMIT`).
- **UNSUBSCRIBE** (`0x07`, c2h): array of `channel_id`.
- **PUBLISH** (`0x18`, c2h): CBOR `publishes` array, the c2h counterpart of SUBSCRIBE. Adds, changes or (with rate 0) drops a publication wish mid-session, validated and clamped exactly as in [§6.2](#s6-2) and answered with `granted_publishes` results. Without it, adding one publication required a full reconnect.

This is how a UI opens a 240 Hz scope view for thirty seconds without reconnecting, and how a streaming client switches from dense samples to timed segments without dropping its session.

## 6.8 Reconnect {#s6-8}

On transport restoration a client sends a fresh HELLO (same `instance_id`, same `token`, cached `catalog_etag`, its standing wish-lists). Then:

- **Etag matches** → ready immediately ([§6.4](#s6-4)), no catalog bytes on the wire. **Etag differs or `boot_id` changed** → full SYNCING including catalog transfer.
- **Snapshot adoption is mandatory:** the retained-STATE push *is* the resync; the client MUST discard its shadow and rebuild from it. No client-side state survives a reconnect on its own authority.
- **Idempotency reset:** intent ids are session-scoped ([§9.3](channels.md#s9-3)). Pending unacknowledged intents from the dead session are *gone* — the client MUST NOT blind-retransmit them. It reconciles by comparing its intended value against the adopted snapshot and re-issues only if still wanted and still different. This is why relative intents are forbidden ([§9.3](channels.md#s9-3)): "increment by 5" cannot be reconciled against a snapshot; "set to 405" can.
- **Grant reacquisition is not control reacquisition.** Subscriptions and publications re-grant freely. But if the disconnect triggered the deadman and motion stopped, the returning session does NOT silently resume as active source — it must issue a fresh control-taking intent ([§11.4](safety.md#s11-4)). **Motion never restarts because a socket reopened.**
- **Trust is re-evaluated.** A token presented after a `client_ver` change may be admitted at `watch` with its granted tier suspended ([§12.6](security.md#s12-6)).

## 6.9 Teardown: one path, six doors {#s6-9}

GOODBYE (`0x11`, either direction; CBOR `code` from `nack_codes`, optional `detail`) is a courtesy, not a requirement — transports die rudely and every rule above already tolerates it.

**Normative equivalence rule.** Every way a session can end — voluntary GOODBYE, transport loss detected out of band, slow-consumer eviction ([§10.4](qos.md#s10-4)), administrative eviction ([§12.7](security.md#s12-7)), reuse of a session slot by a duplicate `instance_id` ([§6.3](#s6-3)), idle reaping ([§6.6](#s6-6)), readiness timeout ([§6.4](#s6-4)), and deadman fire ([§11.3](safety.md#s11-3)) — MUST be **behaviorally identical with respect to source ownership and safety latching**. In every case the hub runs the departing session's [§11.3](safety.md#s11-3) loss policy: an initiator-bound source it owned latches STOP and stops; a hub-autonomous source it owned is released and keeps running. Ownership release on session end is **unconditional and independent of how the end was detected**.

The `cause` recorded in the `safety` snapshot distinguishes them: `deadman` (1) means the [§11.3](safety.md#s11-3) silence window actually elapsed; every other teardown path latches `session_loss` (4). A hub MUST NOT report a closed browser tab as a deadman timeout.

*Why this is a numbered rule (informative):* the reference implementation released ownership only from the deadman pump, which requires an occupied slot. GOODBYE, rude detach, both evictions and same-slot re-HELLO all reset the slot first — so a departed streamer's dead `session_id` owned the motion source **forever**, silently conflict-dropping every later client's intents and bundles until reboot. It was invisible to every test that rebooted between runs. Back-to-back sessions with no reboot in between is therefore a mandatory verification pattern for any session-lifecycle change.

Codes of note: `NORMAL_CLOSURE` (clean voluntary teardown, either direction), `SESSION_EVICTED`, `DUPLICATE_INSTANCE`, `DEADMAN_TIMEOUT`, `READY_TIMEOUT`, `REBOOTING` ([§9.3](channels.md#s9-3)).
