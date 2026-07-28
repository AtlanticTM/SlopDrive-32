---
title: CBOR keys
description: Generated table of the SlopSync control-plane CBOR integer key space and every scoped sub-map key space.
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

# CBOR keys

Control-plane payloads are CBOR maps with integer keys. The key space is
**global**: a key means the same thing in every message that uses it.

Keys 1–63 are core. Keys 64–127 are reserved. Keys 128 and above are
experimental and never appear in a tagged release.

A receiver ignores an unknown key. It never NACKs one.

## Global key space

| Key | Name | Type | Notes |
|---|---|---|---|
| `1` | `proto_ver` | `uint` | HELLO/WELCOME: protocol major version |
| `2` | `client_kind` | `tstr` | e.g. webui, c5-remote, mobile, sim, tcode-bridge |
| `3` | `client_name` | `tstr` | human-readable, ≤32 UTF-8 bytes |
| `4` | `instance_id` | `bstr` | 8-byte stable client identity (§6.1) |
| `5` | `token` | `bstr` | 16-byte pairing token (§12.2); absent = viewer |
| `6` | `session_id` | `uint` | u32, hub-assigned (§6.1) |
| `7` | `boot_id` | `uint` | u32 random per hub boot (§7.2) |
| `8` | `catalog_etag` | `bstr` | 8-byte truncated SHA-256 (§8.3) |
| `9` | `cfg_gen` | `uint` | u16 config generation (§4.2) |
| `10` | `subscriptions` | `array` | of {15:channel,12:rate,13:priority} |
| `11` | `publishes` | `array` | of {15:channel,12:rate,42:burst} — HELLO wishes and PUBLISH (0x18) renegotiation (RFC-013) |
| `12` | `rate_hz` | `float` | requested rate; 0 = on-change only |
| `13` | `priority` | `uint` | priority class 0–3 |
| `14` | `granted_rate_hz` | `float` | GRANT: applied rate after clamp (§10.2) |
| `15` | `channel_id` | `uint` | u16 |
| `16` | `code` | `uint` | NACK/GOODBYE reason code (§16.1) |
| `17` | `detail` | `tstr` | optional human-readable diagnostic |
| `18` | `intent_id` | `uint` | u16 idempotency id, session-scoped (§9.3) |
| `19` | `applied` | `map` | ECHO: post-clamp applied values |
| `20` | `value` | `any` | INTENT payload value(s) per catalog schema |
| `21` | `timestamp` | `uint` | hub-ms (control plane events) |
| `22` | `limits` | `map` | WELCOME: hub limits (max_frame, max_subs, max_clients...) |
| `23` | `roles` | `uint` | granted access level (max of session) |
| `24` | `deadman_ms` | `uint` | WELCOME: applied deadman timeout for this session |
| `25` | `deadman_policy` | `uint` | 0=stop(decel) 1=hold 2=none — per active-source rules §11.3 |
| `26` | `probe_result` | `map` | PROBE_REPORT: {bytes, span_ms, loss_pct, rtt_ms} — sub-keys in `probe_result_keys` |
| `27` | `chunks` | `array` | BLOB_REQ repair: missing chunk indices. (Was 'CATALOG_REQ repair' pre-v1.0; the key is REUSED rather than replaced because catalog transfer is now blob namespace 0 — same meaning, generalized carrier.) Present WITH a full request = MALFORMED (RFC-022.6). |
| `28` | `pin_proof` | `bstr` | PAIR_REQ: HMAC-SHA256(PIN, hello-nonce) truncated 16B (§12.2) |
| `29` | `nonce` | `bstr` | WELCOME: 8-byte pairing nonce |
| `30` | `precondition` | `uint` | INTENT: expected cfg_gen (CAS guard, §9.3) |
| `31` | `retry_after_ms` | `uint` | NACK BUSY: earliest reconnect time |
| `32` | `takeover` | `bool` | safety-intent: forcible source takeover flag (§11.4) |
| `33` | `event_kind` | `uint` | EVENT: kind discriminator per catalog entry |
| `34` | `seq_of_state` | `uint` | EVENT: seq of the STATE twin frame it corresponds to (§9.4) |
| `35` | `grants` | `array` | WELCOME: batch grant results — array of {13:priority, 14:granted_rate_hz, 15:channel_id} (§6.3, §10.2) |
| `36` | `granted_publishes` | `array` | WELCOME / PUBLISH result: granted STREAM-ingress publishes — array of {14:granted_rate_hz, 15:channel_id, 42:burst} (§6.3, §10.5). Omitted when empty. |
| `37` | `identity` | `map` | WELCOME: hub identity (RFC-016) — sub-keys in `identity_keys`. Capability discovery is CATALOG introspection, not a list here: a feature exists iff its channels exist. |
| `38` | `blob` | `map` | BLOB_REQ / BLOB_CHUNK / store CRUD intents: which blob, and its item fields (RFC-021) — sub-keys in `blob_keys`. |
| `39` | `trust` | `map` | HELLO + WELCOME + AUTH: identity proof, signature material, token presentation, pairing modes (RFC-029/027) — sub-keys in `trust_keys`. Absent = the potato path: bearer token, no crypto, unchanged v1-draft handshake cost. |
| `40` | `body` | `map` | EVENT: the kind-specific fields. Integer keys come from the CHANNEL'S CATALOG `schema`, exactly as INTENT's `value` (20) does — NOT from this global space. Grammar fix from the feasibility pass: with kind-specific fields at the top level, every device-authored EVENT channel (the motion-anomaly channel!) would have needed a registry PR to name its own fields — the precise coupling the self-describing catalog exists to prevent. |
| `41` | `intent_seq` | `uint` | NACK: seq of the frame being rejected (RFC-001). Hubs SHOULD populate it whenever a specific inbound frame provoked the NACK; clients MUST tolerate its absence. Without it a client with two intents in flight on ONE channel cannot tell which was refused. |
| `42` | `burst` | `float` | publishes / granted_publishes ENTRY maps: token-bucket capacity in samples, decoupled from rate (RFC-013). Default = granted rate (today's behavior). Clamped to rate x max_burst_multiple and echoed like every wish. Exists because §10.5 made rate double as bucket depth, so a 2-4/s segment sender with a 25/s peak had to declare 30 Hz — lying to admission control to buy burst. |
| `43` | `reboot_in_ms` | `uint` | ECHO `applied` (19): this accepted intent commits by rebooting, in about this many ms (RFC-020). The hub then GOODBYEs every session with REBOOTING; `boot_id` change handles the rest. |
| `44` | `deadman_wish_ms` | `uint` | HELLO: requested deadman window (RFC-038). The hub clamps into [deadman_min_ms, deadman_max_ms] (a hub MAY clamp tighter) and echoes the APPLIED value via the existing key 24 — post-clamp echo, zero new response plumbing. Exists because a client that KNOWS its liveness cadence is coarse (a browser whose background-tab timers are throttled, a BLE client on a slow connection interval) could not ask for a window it can actually honor; §11.3's loss policy is untouched — this negotiates WHEN the deadman fires, never WHAT it does. |
| `45` | `curve_family` | `uint` | publishes / granted_publishes ENTRY maps: which `curve_families` smoothness class the segment stream describes (RFC-030). Wish rides HELLO or PUBLISH (0x18) like `burst`, so a sender switching interpolators mid-session renegotiates without a reconnect; the GRANT echo carries the EFFECTIVE family — post `curve_policy` override — so a client can tell 'honored' from 'downgraded'. Absent = unspecified = today's behavior. RFC-049b: `requested_curve_family` (48) rides the SAME entry map echoing the original wish verbatim, so the honored-vs-downgraded read is a direct comparison of two present keys, not an inference from what the client remembers sending. |
| `46` | `ws_port` | `uint` | WELCOME: the hub's own WebSocket listening port (RFC-046 §3). Present on every binding but load-bearing over BLE — it is the in-band endpoint disclosure a BLE-connected client needs to hop to WS (RFC-043's auto-upgrade). Also closes the sim's hub-identity gap for WS-side clients: the same key tells a WS client what the hub believes its own endpoint is. 0 = none (no WS listener right now). |
| `47` | `ipv4` | `uint` | WELCOME: the hub's own IPv4 address (RFC-046 §3), packed big-endian into one u32 (e.g. 192.168.1.229 = 0xC0A801E5) — there is no bstr(4) here because a plain integer makes '0 = none' the same natural sentinel 0.0.0.0 already is. Read alongside `ws_port` for the BLE→WS upgrade hop. 0 = none. |
| `48` | `requested_curve_family` | `uint` | publishes / granted_publishes ENTRY maps (RFC-049b): echoes the client's `curve_family` (45) WISH verbatim, unmodified by `curve_policy`. Exists alongside the existing effective value at key 45 so a downgrade is a visible FACT (both numbers present, compare them) rather than an inference a client has to reconstruct from what it originally sent. Present only when a curve_family wish was made; a hub with no curve-family opinion omits both keys exactly as before this RFC. Implementation: Phase D (RFC-049). |

## Scoped sub-map key spaces

A feature that needs several keys takes **one** global key and numbers
its interior in its own tiny space. Those spaces are listed below. A
number here has no meaning in the global space above.

### `limits` (key 22)

Sizing caps the hub advertises in WELCOME.

| Sub-key | Name | Notes |
|---|---|---|
| `1` | `max_frame` | largest frame this hub accepts, bytes |
| `2` | `max_subscriptions` | per-session subscription cap |
| `3` | `retained_pending` | count of retained STATE pushes that will follow WELCOME |
| `4` | `max_subscriptions_per_frame` | RFC-033.3: most subscription wishes one SUBSCRIBE (or HELLO) frame may carry. Before this was advertised, a client could only find the reference hub's 16-wish decode cap by binary-searching against a live machine — which two clients did, one night each. |

### `probe_result` (key 26)

The client's measurement of the optional post-WELCOME network probe.

| Sub-key | Name | Notes |
|---|---|---|
| `1` | `bytes_received` | bytes received during the probe burst |
| `2` | `span_ms` | wall time of the burst as observed by the client |
| `3` | `loss_pct_x100` | loss percentage x100 (2 decimal fixed-point) |
| `4` | `rtt_ms` | measured round-trip time, ms |

### `identity` (key 37)

Who this hub is. There is exactly one home for hub identity.

| Sub-key | Name | Notes |
|---|---|---|
| `1` | `product` | tstr: product/model identifier, e.g. 'slopdrive-32' (<=32 B) |
| `2` | `fw_version` | tstr: hub firmware version, e.g. '2.1.47' (<=24 B). Retires the mDNS-TXT-only exposure that made the MFP plugin label devices 'boot 0x...'. A change here SHOULD be surfaced to the user (RFC-029.3). |
| `3` | `hub_name` | tstr: operator-assigned machine name (<=32 B). Writable as a str16/str32 setting (RFC-026) where the hub offers one. |
| `4` | `info` | map: OPTIONAL device-defined extras (hardware rev, build date...). Keys are device-defined tstr; the protocol never interprets them. Depth: WELCOME map -> identity map -> info map = 3, one under the §5.3 cap. |
| `5` | `hub_instance_id` | uint (u64): RFC-048, operator veto of an RFC-046 decision. DURABLE hub identity — generated once and NVS-persisted, survives every reboot and firmware update (only a factory reset regenerates it) — as opposed to `boot_id` (cbor_keys 7, §6.1/§7.2), which is a FRESH random value EVERY boot and exists only to fence stale per-boot state. Distinguishes THIS PHYSICAL HUB from any other, across time. Present in WELCOME `identity` (37) for any hub that persists one; absent = the hub has no durable identity yet (a fresh dev build, a non-persisting simulator) and a client MUST tolerate its absence exactly as it tolerates the rest of `identity` (§6.3). Also the value DISCOVER_REPLY (0x1F, §13.8) now carries as `hub_instance_id`, replacing that frame's original `boot_id`-based disambiguator (see the `frame_types` 0x1F note) — a boot-scoped id could not deduplicate 'two hubs sharing a name' across a reboot, which was the field's whole job. |

### `blob` (key 38)

Which blob, and its item fields. Shared by BLOB_REQ, BLOB_CHUNK and store intents.

| Sub-key | Name | Notes |
|---|---|---|
| `1` | `ns` | uint: which blob space — see `blob_namespaces`. (Named `ns`, not `namespace`: the generated C++ constant would otherwise be a keyword. Prose and CDDL may say 'namespace'.) |
| `2` | `store_id` | uint u8: which store within blob_namespaces.store; absent/0 for the catalog namespace. Declared by the store's STORE-class catalog entry. |
| `3` | `slot` | uint u8: item index within the store, 0..capacity-1 |
| `4` | `generation` | uint u16: the roster generation this request/response is consistent with. Lets a client notice its enumeration went stale mid-transfer. |
| `5` | `name` | tstr: item name, <= the store's declared name_max |
| `6` | `kind` | tstr: namespaced payload kind, e.g. 'pattern.frayd'. The hub validates kind + size on import and NACKs INVALID_VALUE; it never inspects the payload itself. |
| `7` | `payload` | bstr: the opaque item document (<= preset_item_max_bytes / the store's per_item_max). Present on save-with-payload (= import); absent on save means 'capture CURRENT live state'. |
| `8` | `chunk_index` | uint: 0-based index of this chunk |
| `9` | `chunk_count` | uint: total chunks in this transfer |
| `10` | `total_bytes` | uint: total encoded byte length being transferred (lets a receiver size/reject before assembling — RFC-028 no-unbounded-allocation) |

### `trust` (key 39)

Identity proof, signature material, token presentation, pairing modes. Every key is optional.

| Sub-key | Name | Notes |
|---|---|---|
| `1` | `client_ver` | tstr: HELLO — client software version (<=24 B). The CHANGE TRIPWIRE: an observed version change drops a paired device to RECOGNIZED-PENDING (admitted at `watch`, granted role suspended, re-approval surfaced to configure sessions). HONESTY CLAUSE, normative: self-reported, therefore a tripwire and NOT attestation — a deliberately malicious update lies and keeps its token. The real bounds are role scoping, instant revocation, roster visibility and the role-exempt safety ops. |
| `2` | `client_nonce` | bstr: HELLO — 8 bytes of CLIENT entropy. The hub signs client_nonce \|\| session_id \|\| boot_id. This was a feasibility-pass BLOCKER: without client entropy the WELCOME signature is replayable from one captured handshake and an evil twin passes verification. |
| `3` | `sig_request` | bool: HELLO — 'please sign my nonce'. Signing is ON REQUEST because the S3 has no ECC accelerator (mbedtls software ECDSA: sign ~30-80 ms, one uninterruptible call, never inline in a WS handler), so potato handshakes must stay instant. Absent/false = no signature, no cost. A request is NOT a promise: a hub with no keypair answers with silence, and silence is a conformant answer — only a client that PINNED a key (which it can only have received from that machine's own PAIR_GRANT) is entitled to read silence as failure, after hub_sig_timeout_ms. |
| `4` | `hub_pubkey` | bstr: PAIR_GRANT — SEC1-compressed P-256 public key (33 B). Delivered AT THE PAIRING CEREMONY, i.e. TOFU anchored at the moment physical presence was proven. P-256 because WebCrypto can verify it — the browser participates. |
| `5` | `welcome_sig` | bstr: WELCOME or HUB_SIG (0x1D) — deterministic ECDSA-P256 (RFC 6979) signature over the 16-byte string client_nonce(8) \|\| session_id(u32 LE) \|\| boot_id(u32 LE). A clone machine copies every identity string and fails this; the client MUST surface 'not your machine' and withhold intents. Potato clients paired by physical ceremony MAY skip verification. TWO DELIVERY POINTS, ONE MEANING: inline in WELCOME where a hub can sign without stalling its own tick, otherwise deferred in HUB_SIG once the hub's own low-priority worker has produced it. Signature material and client handling are identical either way; a client accepts whichever arrives first and ignores a second. |
| `6` | `token_proof` | bstr: AUTH — HMAC-SHA256(token, WELCOME nonce) truncated to 16 B. Costs ONE extra round trip per connect; that is the honest price. The 'previous-session nonce' shortcut was DROPPED by the feasibility pass as replay-unsafe (undefined rotation point, and §6.3 makes a successful replay EVICT the real client). |
| `7` | `presentation_mode` | uint: how the client presents its token — 0 = bearer (raw 16 B in HELLO; legal, the potato floor stays a memcpy), 1 = proof (AUTH, RECOMMENDED for anything with SHA-256, i.e. everyone but coin cells). v1 transports are cleartext, so bearer is sniffable by a passive LAN observer; the roster records which mode a device uses, making security posture visible. |
| `8` | `pairing_modes` | uint: WELCOME — BITMASK of `pairing_modes` this hub currently offers, RE-EVALUATED PER SESSION so a mode that is only transiently available (RFC-027(c)'s push-to-pair window) is advertised only while it is actually open. RFC-027.3 said 'WELCOME limits/identity map'; it lands here instead because it is session-security capability, and `limits` is sized caps while `identity` is who-you-are. Window state is thus observable in-band by any watch session. |

## Blob namespaces

One transfer verb serves the whole protocol. The namespace says what is
being transferred. Values 0–127 are spec-governed; 128–255 are
device-defined.

| Value | Namespace | Notes |
|---|---|---|
| `0` | `catalog` | the hub's channel catalog (§8.4). store_id/slot absent. This is the ONLY namespace with a READY concept (CATALOG_READY 0x19) — you cannot decode STATE without the catalog, but nothing gates on a preset. |
| `1` | `store` | items in a catalog-declared STORE-class channel: store_id picks the store, slot picks the item. Presets, saved positions, limit profiles, recordings, the trust ledger — all the same machinery, for free. Unused is unproblem. |

