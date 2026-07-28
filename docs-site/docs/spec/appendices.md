---
title: Appendices A-G
description: >-
  SlopSync appendices A-G: frame types, CBOR keys, the catalog schema, a
  worked catalog sketch, the trace and golden-vector indexes, and limits and
  defaults.
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

# Appendices A-G

## Appendix A — Frame type table *(normative, generated view of `registry.yaml` `frame_types`)* {#appendix-a}

!!! info "Live registry view"

    This appendix is the view of the registry frozen at the v1.0 tag.
    The always-current generated table is [Frame types](../reference/registry/frames.md).
    On any conflict the registry wins ([§5.7](wire-format.md#s5-7)).

| Type | Name | Dir | Plane | Defined in |
|---|---|---|---|---|
| 0x00 | HELLO | c→h | control | [§6.2](session.md#s6-2) |
| 0x01 | WELCOME | h→c | control | [§6.3](session.md#s6-3) |
| 0x03 | PING | any | raw | [§6.6](session.md#s6-6) |
| 0x04 | PONG | any | raw | [§6.6](session.md#s6-6) |
| 0x05 | CLOCK | any | raw | [§7.1](time.md#s7-1) |
| 0x06 | SUBSCRIBE | c→h | control | [§6.7](session.md#s6-7) |
| 0x07 | UNSUBSCRIBE | c→h | control | [§6.7](session.md#s6-7) |
| 0x08 | GRANT | h→c | control | [§10.2](qos.md#s10-2) |
| 0x0B | STATE | h→c | data | [§9.1](channels.md#s9-1) |
| 0x0C | STREAM | any | data | [§9.2](channels.md#s9-2) |
| 0x0D | INTENT | c→h | control | [§9.3](channels.md#s9-3) |
| 0x0E | ECHO | h→c | control | [§9.3](channels.md#s9-3) |
| 0x0F | EVENT | h→c | control | [§9.4](channels.md#s9-4) |
| 0x10 | NACK | h→c | control | [§16.1](errors.md#s16-1) |
| 0x11 | GOODBYE | any | control | [§6.9](session.md#s6-9) |
| 0x12 | PROBE | any | raw | [§6.5](session.md#s6-5) |
| 0x13 | PROBE_REPORT | c→h | control | [§6.5](session.md#s6-5) |
| 0x14 | PAIR_REQ | c→h | control | [§12.3](security.md#s12-3) |
| 0x15 | PAIR_GRANT | h→c | control | [§12.3](security.md#s12-3) |
| 0x16 | ACKMASK | any | raw | [§13.3](transports.md#s13-3) |
| 0x17 | BEACON | h→c | raw | [§13.7](transports.md#s13-7) |
| 0x18 | PUBLISH | c→h | control | [§6.7](session.md#s6-7) |
| 0x19 | CATALOG_READY | c→h | raw | [§6.4](session.md#s6-4) |
| 0x1A | BLOB_REQ | c→h | control | [§8.4](catalog.md#s8-4), [§8.7](catalog.md#s8-7) |
| 0x1B | BLOB_CHUNK | h→c | raw | [§8.4](catalog.md#s8-4), [§8.7](catalog.md#s8-7) |
| 0x1C | AUTH | c→h | control | [§12.4](security.md#s12-4) |
| 0x1D | HUB_SIG | h→c | control | [§12.5](security.md#s12-5) |
| 0x1E | DISCOVER_PROBE | c→h | raw | [§13.8](transports.md#s13-8) |
| 0x1F | DISCOVER_REPLY | h→c | raw | [§13.8](transports.md#s13-8) |
| 0x20 | BLOB_DONE | any | raw | [§8.4](catalog.md#s8-4) |
| 0xE5 | ESTOP | any | raw | [§5.5](wire-format.md#s5-5), [§11.2](safety.md#s11-2) |

**Burned, never to be reallocated:** `0x09` (was CATALOG_REQ) and `0x0A` (was CATALOG_CHUNK), superseded by BLOB_REQ/BLOB_CHUNK when chunked transfer was generalized into a namespaced verb ([§8.4](catalog.md#s8-4)). They stay burned so that a stale v1-draft peer meets an *unknown* type and is ignored per [§4.3](foundations.md#s4-3), rather than silently misreading a blob frame.

**Reserved:** `0x02` and `0x21–0x3F` spec/core (31 slots free); `0x40–0x7F` future spec; `0x80–0xDF` experimental (never in tagged releases); `0xE0–0xFF` reserved except `0xE5`.

**Header flags:** bit0 `FRAG_START`, bit1 `FRAG_MORE` ([§5.6](wire-format.md#s5-6)). Other bits are zero on send and ignored on receive.

*Note on the "Defined in" column:* it cites **this document's** v1.0 section numbers. The registry's own `ref:` fields still carry the v1-draft numbering for several entries and are one subsection out in [§6](session.md#s6) and [§12](security.md#s12) ([§5.7](wire-format.md#s5-7), [§18-19](limitations.md#s18)). The frame numbers themselves — the only thing that is normative here — are identical in both.

## Appendix B — CBOR key registry *(normative, generated view of `registry.yaml`)* {#appendix-b}

!!! info "Live registry view"

    This appendix is the view of the registry frozen at the v1.0 tag.
    The always-current generated table is [CBOR keys](../reference/registry/cbor-keys.md).
    On any conflict the registry wins ([§5.7](wire-format.md#s5-7)).

**Global keys.** Range 1–63 core, 64–127 reserved, 128+ experimental. A key means the same thing in every message that uses it.

| # | Name | Type | # | Name | Type |
|---|---|---|---|---|---|
| 1 | `proto_ver` | uint | 23 | `roles` | uint |
| 2 | `client_kind` | tstr | 24 | `deadman_ms` | uint |
| 3 | `client_name` | tstr | 25 | `deadman_policy` | uint |
| 4 | `instance_id` | bstr | 26 | `probe_result` | map |
| 5 | `token` | bstr | 27 | `chunks` | array |
| 6 | `session_id` | uint | 28 | `pin_proof` | bstr |
| 7 | `boot_id` | uint | 29 | `nonce` | bstr |
| 8 | `catalog_etag` | bstr | 30 | `precondition` | uint |
| 9 | `cfg_gen` | uint | 31 | `retry_after_ms` | uint |
| 10 | `subscriptions` | array | 32 | `takeover` | bool |
| 11 | `publishes` | array | 33 | `event_kind` | uint |
| 12 | `rate_hz` | float | 34 | `seq_of_state` | uint |
| 13 | `priority` | uint | 35 | `grants` | array |
| 14 | `granted_rate_hz` | float | 36 | `granted_publishes` | array |
| 15 | `channel_id` | uint | 37 | `identity` | map |
| 16 | `code` | uint | 38 | `blob` | map |
| 17 | `detail` | tstr | 39 | `trust` | map |
| 18 | `intent_id` | uint | 40 | `body` | map |
| 19 | `applied` | map | 41 | `intent_seq` | uint |
| 20 | `value` | any | 42 | `burst` | float |
| 21 | `timestamp` | uint | 43 | `reboot_in_ms` | uint |
| 22 | `limits` | map | 44 | `deadman_wish_ms` | uint |
| | | | 45 | `curve_family` | uint |
| | | | 46 | `ws_port` | uint |
| | | | 47 | `ipv4` | uint |
| | | | 48 | `requested_curve_family` | uint |
| | | | | *49–63 free* | |

**Scoped sub-map key spaces ([§5.3](wire-format.md#s5-3)).** Each is local to its own map: key 1 of `blob` and key 1 of `trust` are unrelated, and neither is `proto_ver`.

| Parent | Sub-keys |
|---|---|
| `limits` (22) | 1 `max_frame`, 2 `max_subscriptions`, 3 `retained_pending`, 4 `max_subscriptions_per_frame` |
| `probe_result` (26) | 1 `bytes_received`, 2 `span_ms`, 3 `loss_pct_x100`, 4 `rtt_ms` |
| `identity` (37) | 1 `product`, 2 `fw_version`, 3 `hub_name`, 4 `info` (device-defined map) |
| `blob` (38) | 1 `ns`, 2 `store_id`, 3 `slot`, 4 `generation`, 5 `name`, 6 `kind`, 7 `payload`, 8 `chunk_index`, 9 `chunk_count`, 10 `total_bytes` |
| `trust` (39) | 1 `client_ver`, 2 `client_nonce`, 3 `sig_request`, 4 `hub_pubkey`, 5 `welcome_sig`, 6 `token_proof`, 7 `presentation_mode`, 8 `pairing_modes` |
| `body` (40) | **the channel's own catalog `schema` keys** — not a registry space ([§9.4](channels.md#s9-4)) |
| store item payloads | **opaque** — the protocol never decodes them ([§8.7](catalog.md#s8-7)), except the trust ledger's registered `trust_ledger_keys` grammar ([§12.6](security.md#s12-6)) |

## Appendix C — Catalog schema *(normative)* {#appendix-c}

The catalog's CDDL definition lives in [`schema/catalog.cddl`](schema.md). It is the normative encoding of [§8.1](catalog.md#s8-1) and [§8.8](catalog.md#s8-8), and it wins on any disagreement with the prose there. The etag ([§8.3](catalog.md#s8-3)) is computed over a catalog valid against it.

## Appendix D — Worked catalog sketch *(informative)* {#appendix-d}

> ### ⚠ EXAMPLE ONLY — NEVER ALLOCATE THESE IDS
>
> Every channel id below is drawn from the **reserved** range `0x8000–0xFFFF` ([§4.4](foundations.md#s4-4)), which no conforming hub may allocate. They exist to make the shape of a device catalog legible and **cannot** be mistaken for, or collide with, any real allocation.
>
> This is deliberate. An earlier draft sketched device channels using ids inside the real device-defined range; a hub had already spent one of those ids on something else, and the sketch — despite carrying a disclaimer — read like an assignment and misled an implementation once. **The shipped hub's catalog is self-describing and authoritative. It is the only source of a channel id.**

A plausible motion machine, sketched to show how the classes and annotations compose:

| Example id | Name | Class | Dir | Access | Notes |
|---|---|---|---|---|---|
| 0xEE00 | position | STREAM (`samples`) | h→c | watch | sample `{planned, asked, achieved}` as scaled u16; the "asked vs did" triplet in one frame, one seq, one timestamp |
| 0xEE01 | motion-input | STREAM (`samples`) | c→h | control | dense target points; wire velocity feeds the planner's feedforward |
| 0xEE02 | motion-segment | STREAM (`segments`) | c→h | control | timed `{target, duration, end_velocity}`; **non-decimable** ([§10.4](qos.md#s10-4)); `t_base+t_off` is a schedule ([§5.4](wire-format.md#s5-4)) |
| 0xEE03 | machine-config | STATE | h→c | watch | window min/max and the user/input ceilings, `setting_key`-annotated against 0xEE04, role-tagged `limit.*`/`window.*`, plus a `meta.enabled_mask` |
| 0xEE04 | config-set | INTENT | c→h | control | the paired writer named by 0xEE03's `setting_channel` |
| 0xEE05 | motion-status | STATE | h→c | watch | homed/homing/running/paused bits — no `setting_key`, so read-only by construction |
| 0xEE06 | plan-strip | STATE | h→c | watch | the planner's current segment; `elevated` priority, high rate |
| 0xEE07 | pattern-config | STATE | h→c | watch | a `u8 + options` single-select plus its parameters; the mask genuinely drops when unhomed |
| 0xEE08 | pattern-control | INTENT | c→h | control | select/configure/run/stop — activates a hub-autonomous source ([§11.3](safety.md#s11-3)) |
| 0xEE09 | move | INTENT | c→h | control | manual point move — activates an initiator-bound source |
| 0xEE0A | home | INTENT | c→h | control | `action.home`, plus bench ops under safety review |
| 0xEE0B | motion-anomaly | EVENT | h→c | watch | device-authored kinds; fields ride `body` with **no registry change** ([§9.4](channels.md#s9-4)) |
| 0xEE0C | motion-diag | STATE | h→c | watch | per-kind counters and a `meta.reset_gen` ([§9.3](channels.md#s9-3)) |
| 0xEE0D | power | STATE | h→c | watch | bus voltage/current/temperature; **absent entirely** on a machine without the sensor — that absence *is* the capability answer ([§6.3](session.md#s6-3)) |
| 0xEE0E | link-status | STATE | h→c | watch | signal, addresses as `str16`/`str32` ([§5.4](wire-format.md#s5-4)) |
| 0xEE0F | presets | STORE | — | control | `kind: "example.pattern"`, with a companion roster STATE channel ([§8.7](catalog.md#s8-7)) |

Plus the spec-core channels, which **are** real allocations and are listed in `registry.yaml` `core_channels`: `catalog`, `session-roster`, `safety`, `control-owner`, `safety-intents`, `hub-status`, `session-events`, `log`, `session-admin`, `pending-pairing`, `pairing-events`, `paired-devices`, `paired-devices-roster`, `safety-events`.

Every STATE layout above fits 242 bytes by inspection; conformance tooling re-checks mechanically ([§9.1](channels.md#s9-1)).

## Appendix E — Worked traces *(informative)* {#appendix-e}

Annotated end-to-end session traces live in [`examples/session-traces.md`](traces.md), and use the same reserved example ids as [Appendix D](#appendix-d):

- **E1** — cold connect (browser, dynamic catalog, readiness gate, retained push);
- **E2** — reconnect mid-motion (etag skip, reconcile-don't-retransmit, no silent control resume);
- **E3** — controller takeover (two remotes, one machine);
- **E4** — ESTOP over a lossy relay (repeat-until-latch, fast path);
- **E5** — constrained client joins (static profile, etag mismatch, degraded mode).

Per [§17.3](conformance.md#s17-3) these are executable narratives: every step cites the normative rule it exercises, and a step with no rule to cite is a spec bug.

## Appendix F — Golden vector index *(normative as to coverage)* {#appendix-f}

The vector manifest and generation plan live in `vectors/manifest.yaml`; the frozen fixture catalog is `vectors/fixtures/mini-catalog.yaml`, mirroring the normative code fixture. Byte-exact vector files are generated with the injected clock, RNG and crypto delegate mandated by [§17.2](conformance.md#s17-2) and land beside the manifest. The manifest is normative as to *what* is covered; the generated bytes are normative once tagged. Fixture pins at v1.0: **775 bytes**, etag **`F4 A2 8F BB 58 CE D1 6A`** ([§17.2](conformance.md#s17-2)).

The fixture's coverage gaps at v1.0 are stated in [§18-7](limitations.md#s18) rather than implied by silence.

## Appendix G — Limits and defaults *(normative, generated view of `registry.yaml` `limits`)* {#appendix-g}

!!! info "Live registry view"

    This appendix is the view of the registry frozen at the v1.0 tag.
    The always-current generated table is [Limits and defaults](../reference/registry/limits.md).
    On any conflict the registry wins ([§5.7](wire-format.md#s5-7)).

| Identifier | Value | Where used |
|---|---|---|
| `header_bytes` | 8 | [§5.1](wire-format.md#s5-1) |
| `min_transport_payload` | 242 | [§9.1](channels.md#s9-1), [§13.1](transports.md#s13-1) |
| `catalog_chunk_payload` | 192 | [§8.4](catalog.md#s8-4) |
| `blob_chunks_in_flight` | 4 | [§8.4](catalog.md#s8-4) |
| `bundle_max_samples` | 32 | [§5.4](wire-format.md#s5-4) |
| `bundle_max_span_ms` | 20 | [§5.4](wire-format.md#s5-4) |
| `seq_width_bits` / `seq_newer_window` | 16 / 32768 | [§7.3](time.md#s7-3) |
| `frag_reassembly_timeout_ms` | 5000 | [§5.6](wire-format.md#s5-6), [§8.4](catalog.md#s8-4) |
| `frag_max_concurrent_per_session` | 2 | [§5.6](wire-format.md#s5-6) |
| `idempotency_ring_depth` | 32 | [§9.3](channels.md#s9-3) |
| `intent_ingress_default_per_s` | 50 | [§9.3](channels.md#s9-3), [§10.5](qos.md#s10-5) |
| `stream_ingress_overage_nack_per_s` | 5 | [§10.5](qos.md#s10-5) |
| `event_queue_depth_per_subscriber` | 16 | [§9.4](channels.md#s9-4) |
| `never_shed_stall_eviction_ms` | 2000 | [§10.4](qos.md#s10-4) |
| `catalog_chunk_gap_timeout_ms` | 500 (SHOULD) | [§6.4](session.md#s6-4), [§8.4](catalog.md#s8-4) |
| `busy_retry_after_default_ms` | 2000 | [§6.3](session.md#s6-3) |
| `ping_interval_holding_control_ms` / `ping_interval_idle_ms` | 200 / 1000 | [§6.6](session.md#s6-6) |
| `deadman_default_ms` / `deadman_min_ms` / `deadman_max_ms` | 600 / 250 / 5000 | [§11.3](safety.md#s11-3) |
| `idle_reap_multiplier` | 3 | [§6.6](session.md#s6-6) |
| `catalog_ready_timeout_ms` | 15000 | [§6.4](session.md#s6-4) |
| `max_future_schedule_ms` | 250 | [§5.4](wire-format.md#s5-4) |
| `max_burst_multiple` | 4 | [§10.5](qos.md#s10-5) |
| `segment_handoff_k` | 1.5 | [§9.6](channels.md#s9-6) |
| `pairing_window_default_s` | 120 | [§12.3](security.md#s12-3) |
| `pairing_pin_digits` | 4 | [§12.3](security.md#s12-3) |
| `pairing_gesture_boot_count` | 3 | [§12.3](security.md#s12-3) |
| `pairing_gesture_max_uptime_ms` | 10000 | [§12.3](security.md#s12-3) |
| `pairing_pending_max` | 4 | [§12.3](security.md#s12-3) |
| `token_bytes` | 16 | [§12.3](security.md#s12-3) |
| `auth_attempts_max` | 3 | [§12.4](security.md#s12-4) |
| `hub_sig_timeout_ms` | 3000 | [§12.5](security.md#s12-5) |
| `paired_devices_max` | 8 | [§12.6](security.md#s12-6) |
| `trust_ledger_max_bytes` | 1900 | [§12.6](security.md#s12-6) |
| `client_ver_max_bytes` | 24 | [§12.6](security.md#s12-6) |
| `trust_ledger_name_max_bytes` / `trust_ledger_kind_max_bytes` | 16 / 16 | [§12.6](security.md#s12-6) |
| `instance_id_bytes` / `etag_bytes` | 8 / 8 | [§6.1](session.md#s6-1), [§8.3](catalog.md#s8-3) |
| `estop_repeat_interval_ms` / `estop_repeat_max` | 50 / 20 | [§11.2](safety.md#s11-2) |
| `clock_resync_interval_s` | 10 | [§7.1](time.md#s7-1) |
| `probe_default_bytes` / `probe_max_duration_ms` | 8192 / 1500 | [§6.5](session.md#s6-5) |
| `catalog_max_entries` | 256 | [§8.1](catalog.md#s8-1) |
| `catalog_max_entry_bytes` | 4096 | [§8.1](catalog.md#s8-1) |
| `max_subscriptions_per_session` | 64 | [§6.7](session.md#s6-7) |
| `max_subscriptions_per_frame` | 16 | [§6.3](session.md#s6-3), [§6.7](session.md#s6-7) |
| `desc_max_bytes` | 128 | [§8.8](catalog.md#s8-8) |
| `option_label_max_bytes` | 24 | [§8.8](catalog.md#s8-8) |
| `nack_detail_max_bytes` | 48 | [§16.1](errors.md#s16-1) |
| `preset_capacity_min` / `preset_item_max_bytes` | 32 / 4096 | [§8.7](catalog.md#s8-7) |
| `log_replay_depth_default` | 32 | [§16.2](errors.md#s16-2) |
| `max_frame_ws` / `max_frame_espnow` / `max_frame_ble` / `max_frame_serial` | 512 / 250 / 244 / 512 | [§5.1](wire-format.md#s5-1), [§13.1](transports.md#s13-1) |
| `conformance_min_clients` | 4 | [§6.3](session.md#s6-3), [§17.1](conformance.md#s17-1) |
| `default_max_clients_ws` / `_espnow` / `_ble` / `_serial` | 8 / 4 / 1 / 1 | [§6.3](session.md#s6-3) |
| `ws_subprotocol` | `slopsync.v1` | [§13.2](transports.md#s13-2) |
| `mdns_service` | `_slopsync._tcp` | [§13.7](transports.md#s13-7) |
