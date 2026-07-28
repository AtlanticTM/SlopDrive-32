---
title: Limits and defaults
description: Generated table of SlopSync well-known limits, timeouts, caps and defaults.
register: IEEE
generated: true
---

<!-- ==========================================================
     GENERATED FILE — DO NOT EDIT.
     Source of truth: docs/slopsync/registry/registry.yaml
     Generator:       docs-site/tools/gen_docs_tables.py
     Regenerate:      python docs-site/tools/gen_docs_tables.py
     CI gate:         python docs-site/tools/gen_docs_tables.py --check
     Hand edits are overwritten and fail the docs build.
     ========================================================== -->

# Limits and defaults

Some of these are hard protocol constants. Others are recommended
defaults a hub may tune and then advertise. The Notes column carries the
registry's own rationale where it records one.

## Core protocol constants

| Name | Value | Notes |
|---|---|---|
| `header_bytes` | `8` |  |
| `min_transport_payload` | `242` | ESP-NOW 250 − 8 header; STATE frames must fit this (§9.1) |
| `catalog_chunk_payload` | `192` |  |
| `blob_chunks_in_flight` | `4` | RFC-050: advertised SENDER pacing budget — the max unacknowledged-by-application-progress BLOB_CHUNKs a sender may have outstanding for one transfer before it MUST hold emission (§8.4's backpressure table). Concrete answer to the panel's "what IS the signal" finding: a hub MAY advertise a smaller value, MUST NOT advertise more. |
| `bundle_max_samples` | `32` |  |
| `bundle_max_span_ms` | `20` |  |
| `seq_width_bits` | `16` |  |
| `seq_newer_window` | `32768` | serial arithmetic half-window |
| `frag_reassembly_timeout_ms` | `5000` |  |
| `frag_max_concurrent_per_session` | `2` |  |
| `idempotency_ring_depth` | `32` |  |
| `intent_ingress_default_per_s` | `50` |  |
| `stream_ingress_overage_nack_per_s` | `5` | §10.5: per-session cap on RATE_LIMITED NACKs emitted for STREAM-ingress overage (throttle — the NACK is back-pressure feedback, not a per-sample echo; unthrottled it would mirror the very flood it reports) |
| `event_queue_depth_per_subscriber` | `16` |  |
| `never_shed_stall_eviction_ms` | `2000` |  |
| `catalog_chunk_gap_timeout_ms` | `500` | recommended (SHOULD) |
| `busy_retry_after_default_ms` | `2000` |  |
| `ping_interval_holding_control_ms` | `200` |  |
| `ping_interval_idle_ms` | `1000` |  |
| `deadman_default_ms` | `600` |  |
| `deadman_min_ms` | `250` |  |
| `deadman_max_ms` | `5000` |  |
| `pairing_window_default_s` | `120` |  |
| `pairing_pin_digits` | `4` |  |
| `pairing_gesture_boot_count` | `3` | RFC-049g: N in §12.3(c)'s power-cycle gesture — this many CONSECUTIVE short boots arm the push-to-pair window on the next boot. Was prose-only ("N (default 3)"); the panel's own complaint pattern (registry doctrine says numbers are never left as hedges) applies to this one too. |
| `pairing_gesture_max_uptime_ms` | `10000` | RFC-049g: the per-boot uptime ceiling that counts as "short" for the gesture above. Was prose-only ("~10 s") — the tilde was hedge language in a normative section; pinned here matching the value SPEC §12.3(c) already carried in prose. Reference-firmware conformance to this exact value is unverified by this pass — implementation is Phase D. |
| `token_bytes` | `16` |  |
| `instance_id_bytes` | `8` |  |
| `etag_bytes` | `8` |  |
| `conformance_min_clients` | `4` |  |
| `default_max_clients_ws` | `8` |  |
| `default_max_clients_espnow` | `4` |  |
| `default_max_clients_ble` | `1` |  |
| `default_max_clients_serial` | `1` |  |
| `estop_repeat_interval_ms` | `50` |  |
| `estop_repeat_max` | `20` |  |
| `clock_resync_interval_s` | `10` |  |
| `probe_default_bytes` | `8192` |  |
| `probe_max_duration_ms` | `1500` |  |
| `catalog_max_entries` | `256` |  |
| `catalog_max_entry_bytes` | `4096` | feasibility pass: a 50-field FULLY annotated entry (defaults + options + groups + descs) encodes to ~8–10 KB, which violates RFC-028's no-unbounded-allocation rule for a per-entry decode buffer. Oversize is a catalog-AUTHORING error caught by conformance tooling, not a runtime surprise: the entry splits across channels or trims its descs. |
| `max_subscriptions_per_session` | `64` |  |
| `max_subscriptions_per_frame` | `16` | RFC-033.3: wishes one SUBSCRIBE/HELLO frame may carry (= the reference decoder's kSubscribeMaxWishes, which was previously discoverable only by binary-searching a live hub). Advertised in WELCOME limits key 4; a hub MAY advertise less, never more than it decodes. Overflow answers SUBSCRIBE_REJECTED, never silence. |
| `ws_subprotocol` | `slopsync.v1` |  |
| `mdns_service` | `_slopsync._tcp` |  |

## Per-binding max_frame defaults

| Name | Value | Notes |
|---|---|---|
| `max_frame_ws` | `512` | = the reference hub's FrameBuffer capacity. Nothing NEEDS to be bigger: data-plane frames fit the 242 B floor by rule, blob chunks are catalog_chunk_payload + header, and oversized control frames fragment (§5.6). A fixed capacity keeps every queue slot heap-free on an ESP32. |
| `max_frame_espnow` | `250` | the hard ESP-NOW MTU. Payload 250−8 = 242 = min_transport_payload; that subtraction is where the 242 B STATE floor came from in the first place. |
| `max_frame_ble` | `244` | ATT_MTU 247 − 3 B notification header. Payload 236. A client stuck at the legacy 23 B MTU cannot carry a full STATE frame at all, which is why §13 SHOULD-mandates MTU exchange + data length extension before catalog transfer. |
| `max_frame_serial` | `512` | byte-stream binding: no MTU of its own, so it matches the WS/buffer figure rather than inventing a third number. |

## Session lifecycle (RFC-015, RFC-024)

| Name | Value | Notes |
|---|---|---|
| `catalog_ready_timeout_ms` | `15000` | RFC-015: a session that PINGs happily but never sends CATALOG_READY is GOODBYE'd READY_TIMEOUT. Liveness reaping alone NEVER fires on a pinging client, so without this a half-adopted session holds a slot forever with both planes gated shut. (READY itself is re-sent by the client at catalog_chunk_gap_timeout_ms until retained STATE arrives — idempotent, no handshake state machine.) |
| `idle_reap_multiplier` | `3` | RFC-024: reap a non-owning session after this multiple of ping_interval_idle_ms of silence. §6.5 said "MAY"; it was never implemented, so a viewer that went dark held a slot forever. Two liveness regimes, deliberately different: source OWNERS get the deadman window + §11.3 loss policy; everyone else gets idle reaping with no motion consequence. |

## Streaming (RFC-013, RFC-014, RFC-049c)

| Name | Value | Notes |
|---|---|---|
| `max_future_schedule_ms` | `250` | RFC-014: for segment-class STREAM channels, t_base + t_off[i] IS the intended execution start of sample i; the hub clamps scheduling this far ahead. It was already the shipped fw 2.1.45 behavior but registered NOWHERE — the MFP plugin carried a private SegLookaheadMs=120 against it. Interop by folklore, now by number. Recommended client lookahead <= half of this. |
| `max_burst_multiple` | `4` | RFC-013: cap on `burst` relative to granted rate. An unbounded client-declared burst would reintroduce the exact flood the token bucket exists to stop. |
| `segment_handoff_k` | `1.5` | RFC-049c: the H11 machine-side handoff-sanity bound (§9.6) — a hub SHOULD reject/bound an accepted end-velocity exceeding k * min(\|chord_in\|, \|chord_out\|). Was reference-implementation-only (the MFP plugin's own Fritsch-Carlson limiter and the firmware's boundHandoffVelocity both hardcoded 1.5 independently) — the panel quoted this registry's OWN doctrine ("a competing implementation has no authoritative source for the clamping constant") back at us. Pinned here so a second implementation matches shape without reverse-engineering the reference. Hub-side per-source scheduling-depth backstop (widening H11's lookahead-bounded coverage) is Phase D implementation, not a registry number. |

## String caps (RFC-009, RFC-022.5, RFC-028.2)

| Name | Value | Notes |
|---|---|---|
| `desc_max_bytes` | `128` | RFC-009: per-field user-facing description. Flash-resident on the hub, travels once, etag-cached forever. |
| `nack_detail_max_bytes` | `48` | RFC-022.5 |
| `option_label_max_bytes` | `24` | RFC-009.1: one label in a single-select `options` array. Matches the 24 B field-name cap so a label is never the thing that overflows an entry. |

## Stores & trust (RFC-021, RFC-027, RFC-029)

| Name | Value | Notes |
|---|---|---|
| `preset_capacity_min` | `32` | RFC-021.6: conformance FLOOR for a store's declared capacity, not a cap. 32 fray-d presets ~ 1.5 KB NVS; the mechanism does not blink at 256. Small hubs declare less, the catalog says so, clients render accordingly. |
| `preset_item_max_bytes` | `4096` | RFC-021.6 default per_item_max. The payload is opaque: the protocol never decodes it, so this is purely a transfer/storage budget. |
| `paired_devices_max` | `8` | trust-ledger capacity. 8, matching the library's kMaxPaired TODAY (the RFC text said 16; the feasibility pass corrected it against the source). Also equals default_max_clients_ws, which is a coincidence worth not reading meaning into. |
| `trust_ledger_max_bytes` | `1900` | feasibility pass: the whole encoded ledger is ONE blob in the existing `slopsync` NVS namespace and must stay inside a single ~2 KB NVS page. Written only on change, and gated on ota_active exactly like savePairing() — flash-cache writes during an OTA reset the chip. |
| `pairing_pending_max` | `4` | RFC-027.2a: bounded knock list. Bounded because it is an unauthenticated queue — the one surface a stranger can fill. |
| `client_ver_max_bytes` | `24` | HELLO `trust`.client_ver. Registered as a NUMBER (the trust_keys note only stated it in prose) because RFC-028.2 makes registry string caps a PARSE-TIME obligation: a receiver rejects an over-cap string in a structural payload rather than truncating and continuing, and it cannot enforce a cap that exists only in English. Same value bounds the trust-ledger `version` field, so the tripwire never compares a truncated value against a full one. |
| `trust_ledger_name_max_bytes` | `16` | trust-ledger `name`. 16 to match the 0x0002 roster's str16 rather than HELLO's 32: a roster label is a label, and the authoritative full name rides HELLO/0x0007 while the session lives. |
| `trust_ledger_kind_max_bytes` | `16` | trust-ledger `kind`. Equals the HELLO client_kind cap, so a kind is recorded whole and never appears to change between sessions. |
| `hub_sig_timeout_ms` | `3000` | RFC-029 item 1: how long a client that REQUESTED a hub signature AND holds a pinned key waits before calling the absence a failure. Generous on purpose — one software ECDSA is ~30-80 ms, but it is queued behind whatever the hub's low-priority worker is already doing, and a hub is allowed to be busy. A client with NO pinned key never applies this timeout: it has nothing to verify against, and silence from a hub that simply has no keypair is conformant, not suspicious. |
| `auth_attempts_max` | `3` | RFC-029 item 6: failed AUTH (0x1C) proofs a session may present before the hub stops answering and GOODBYEs it. Mirrors §12.2's "three failures close the PIN window" verbatim rather than inventing a second number. The proof is 16 bytes, so this is not what makes guessing infeasible — it is what stops an unauthenticated peer spending the hub's HMAC budget in a loop. |

## Logging (RFC-017)

| Name | Value | Notes |
|---|---|---|
| `log_replay_depth_default` | `32` | entries the hub MAY replay from its ring tail when a session is granted the log channel. THE named exception to §9.4's no-replay rule ("except where a channel's catalog entry declares a replay depth"); the actual depth is declared per-entry, this is the default. 32 lines is roughly "what went wrong just before I connected" without making every grant a burst. |

