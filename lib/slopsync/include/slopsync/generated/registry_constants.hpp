// ============================================================================
// GENERATED FILE — DO NOT EDIT.
// Source of truth: docs/slopsync/registry/registry.yaml
// Regenerate:      python tools/gen_registry_header.py   (--check in CI)
// ============================================================================
#pragma once

#include <cstdint>
#include <string_view>

namespace slopsync {

inline constexpr uint8_t  kProtocolVersion = 1;
inline constexpr uint8_t  kHeaderBytes     = 8;

enum class FrameType : uint8_t {
    HELLO = 0x00,  // c2h, control, §6.2
    WELCOME = 0x01,  // h2c, control, §6.3
    PING = 0x03,  // any, raw, §6.5
    PONG = 0x04,  // any, raw, §6.5
    CLOCK = 0x05,  // any, raw, §7.1
    SUBSCRIBE = 0x06,  // c2h, control, §6.6
    UNSUBSCRIBE = 0x07,  // c2h, control, §6.6
    GRANT = 0x08,  // h2c, control, §10.2
    STATE = 0x0B,  // h2c, data, §9.1
    STREAM = 0x0C,  // any, data, §9.2
    INTENT = 0x0D,  // c2h, control, §9.3
    ECHO = 0x0E,  // h2c, control, §9.3
    EVENT = 0x0F,  // h2c, control, §9.4
    NACK = 0x10,  // h2c, control, §16.1
    GOODBYE = 0x11,  // any, control, §6.8
    PROBE = 0x12,  // any, raw, §6.4
    PROBE_REPORT = 0x13,  // c2h, control, §6.4
    PAIR_REQ = 0x14,  // c2h, control, §12.2
    PAIR_GRANT = 0x15,  // h2c, control, §12.2
    ACKMASK = 0x16,  // any, raw, §13.3
    BEACON = 0x17,  // h2c, raw, §13.7
    PUBLISH = 0x18,  // c2h, control, §6.6
    CATALOG_READY = 0x19,  // c2h, raw, §8.4
    BLOB_REQ = 0x1A,  // c2h, control, §8.4
    BLOB_CHUNK = 0x1B,  // h2c, raw, §8.4
    AUTH = 0x1C,  // c2h, control, §12.2
    HUB_SIG = 0x1D,  // h2c, control, §12.2
    ESTOP = 0xE5,  // any, raw, §5.5, §11.2
};

namespace flags {
inline constexpr uint8_t FRAG_START = 1u << 0;  // §5.6
inline constexpr uint8_t FRAG_MORE = 1u << 1;  // §5.6
}  // namespace flags

enum class ChannelClass : uint8_t {
    STATE = 0,
    STREAM = 1,
    INTENT = 2,
    EVENT = 3,
    STORE = 4,
};

enum class AccessLevel : uint8_t {
    watch = 0,
    control = 1,
    configure = 2,
};

enum class Priority : uint8_t {
    background = 0,
    normal = 1,
    elevated = 2,
    critical = 3,
};

enum class PackedFieldType : uint8_t {
    u8 = 0,
    i8 = 1,
    u16 = 2,
    i16 = 3,
    u32 = 4,
    i32 = 5,
    f32 = 6,
    bitfield8 = 7,
    str16 = 8,
    str32 = 9,
    str64 = 10,
};

namespace channels {
inline constexpr uint16_t catalog = 0x0001;  // STATE: catalog meta: etag, chunk count, entry count
inline constexpr uint16_t session_roster = 0x0002;  // STATE: IMPLEMENTED at v1.0 (RFC-018): generation u16 + count u8 + flags u8, then 8 packed slots {session_id u32, role u8, flags u8, name str16} = 8x22 + 4 = 180 B, inside the 242 B floor at default_max_clients_ws 8. The str16 name (feasibility pass) also FIXES the never-replayed-join-events blocker: a late joiner learns existing sessions' names from the roster snapshot, not from missed 0x0007 events. Names over 16 B truncate here; the full name rides 0x0007 while the session lives.
inline constexpr uint16_t safety = 0x0003;  // STATE: latched safety word: estop/stop/hold/pause + cause + owner (§11.1); RFC-025 appends manual_override + bypass_limits to the same snapshot (append-only is legal)
inline constexpr uint16_t control_owner = 0x0004;  // STATE: active arbiter source + owning session per source (§11.4)
inline constexpr uint16_t safety_intents = 0x0005;  // INTENT: STOP/HOLD/PAUSE/RESUME/ESTOP_CLEAR/ESTOP/TAKEOVER + override/bypass (§11, safety_intent_ops)
inline constexpr uint16_t hub_status = 0x0006;  // STATE: boot_id, heap, uptime, transport stats. NO fw version: RFC-016 puts identity in WELCOME `identity` — one home, no drift.
inline constexpr uint16_t session_events = 0x0007;  // EVENT: join/leave/takeover/eviction notifications
inline constexpr uint16_t log = 0x0008;  // EVENT: RFC-017: device log in-band — {level u8, tag, hub-ms, message <=128 B} via the `body` sub-map. Bounded drop-oldest with the §9.4 visible drop counter, `background` priority, `watch` access. Declares a replay depth (log_replay_depth_default) so the hub MAY replay its ring tail on grant — the named exception to §9.4's no-replay rule. Retires /api/log; the serial-quiet handoff re-binds from 'first HTTP GET' to 'first log grant'.
inline constexpr uint16_t session_admin = 0x0009;  // INTENT: RFC-018: {op: evict, session_id} -> hub GOODBYEs the target with SESSION_EVICTED. `configure` access — and note RFC-027 makes configure reachable BY CEREMONY now (§12.2's 'admin only via the hub's own UI' sentence is struck), so this power is deliberately pairing-reachable.
inline constexpr uint16_t pending_pairing = 0x000A;  // STATE: RFC-027(a) knock-and-approve: the bounded pending list (pairing_pending_max) as protocol state — {generation u16, count u8, flags u8} + slots {instance_id u64, kind u8, expires_s u16, name str16}. Any `configure` session approves/denies via 0x0009 (session_admin_ops pair_approve/pair_deny); the joiner needs one button and no display. TWO FIELD CLARIFICATIONS made when M4b implemented this: `kind` is the `pairing_modes` BIT that produced the knock (1 knock_approve / 2 pin_proof / 4 push_to_pair), so an approving UI can say HOW the device asked; and the countdown is `expires_s`, NOT ms — pairing_window_default_s is 120, i.e. 120000 ms, which does not fit the u16 the slot layout allocates. Seconds is also the only resolution an approval UI can use. `flags` bit0 = a pairing association window is currently OPEN (RFC-027(c) push-to-pair), which is how window state stays observable in-band for a session that connected before the window opened.
inline constexpr uint16_t pairing_events = 0x000B;  // EVENT: RFC-027: knock arrived / approved / denied / expired, plus pairing-window open/close. EVENT twin of 0x000A.
inline constexpr uint16_t paired_devices = 0x000C;  // STORE: trust-ledger store descriptor: {store_id, kind 'trust.ledger', capacity paired_devices_max, per_item_max, name_max}. Items are read/revoked by any `configure` session via BLOB_REQ on blob_namespaces.store + the 0x0009 admin surface. Whole encoded ledger stays under trust_ledger_max_bytes (one NVS page).
inline constexpr uint16_t paired_devices_roster = 0x000D;  // STATE: the 0x000C store's roster: {generation u16, count u8, capacity u8}. On-change, tiny; a generation bump means 're-enumerate' (fetch again over BLOB_REQ). `configure` access — the paired-device list is not open reading.
inline constexpr uint16_t safety_events = 0x000E;  // EVENT: RFC/§9.4 duality: the EVENT TWIN of the `safety` STATE channel (0x0003). Kinds in `safety_event_kinds`; fields ride the scoped `body` (40) sub-map keyed by this channel's own catalog schema (word, cause, owner_session, estop_seq, level). `critical` priority and `watch` access, matching its STATE twin exactly — an edge nobody is allowed to be denied and nobody is allowed to shed. Carries `seq_of_state` (34) naming the 0x0003 frame it corresponds to, which is what lets a client that missed the edge reconcile against the latch it DID receive. Emitted on TRANSITIONS ONLY: a repeated ESTOP frame re-broadcasts the STATE (that is §11.2's loss recovery) but does NOT re-emit the edge, because an edge that did not happen is a lie.
}  // namespace channels

enum class CborKey : uint8_t {
    proto_ver = 1,  // uint: HELLO/WELCOME: protocol major version
    client_kind = 2,  // tstr: e.g. webui, c5-remote, mobile, sim, tcode-bridge
    client_name = 3,  // tstr: human-readable, ≤32 UTF-8 bytes
    instance_id = 4,  // bstr: 8-byte stable client identity (§6.1)
    token = 5,  // bstr: 16-byte pairing token (§12.2); absent = viewer
    session_id = 6,  // uint: u32, hub-assigned (§6.1)
    boot_id = 7,  // uint: u32 random per hub boot (§7.2)
    catalog_etag = 8,  // bstr: 8-byte truncated SHA-256 (§8.3)
    cfg_gen = 9,  // uint: u16 config generation (§4.2)
    subscriptions = 10,  // array: of {15:channel,12:rate,13:priority}
    publishes = 11,  // array: of {15:channel,12:rate,42:burst} — HELLO wishes and PUBLISH (0x18) renegotiation (RFC-013)
    rate_hz = 12,  // float: requested rate; 0 = on-change only
    priority = 13,  // uint: priority class 0–3
    granted_rate_hz = 14,  // float: GRANT: applied rate after clamp (§10.2)
    channel_id = 15,  // uint: u16
    code = 16,  // uint: NACK/GOODBYE reason code (§16.1)
    detail = 17,  // tstr: optional human-readable diagnostic
    intent_id = 18,  // uint: u16 idempotency id, session-scoped (§9.3)
    applied = 19,  // map: ECHO: post-clamp applied values
    value = 20,  // any: INTENT payload value(s) per catalog schema
    timestamp = 21,  // uint: hub-ms (control plane events)
    limits = 22,  // map: WELCOME: hub limits (max_frame, max_subs, max_clients...)
    roles = 23,  // uint: granted access level (max of session)
    deadman_ms = 24,  // uint: WELCOME: applied deadman timeout for this session
    deadman_policy = 25,  // uint: 0=stop(decel) 1=hold 2=none — per active-source rules §11.3
    probe_result = 26,  // map: PROBE_REPORT: {bytes, span_ms, loss_pct, rtt_ms} — sub-keys in `probe_result_keys`
    chunks = 27,  // array: BLOB_REQ repair: missing chunk indices. (Was 'CATALOG_REQ repair' pre-v1.0; the key is REUSED rather than replaced because catalog transfer is now blob namespace 0 — same meaning, generalized carrier.) Present WITH a full request = MALFORMED (RFC-022.6).
    pin_proof = 28,  // bstr: PAIR_REQ: HMAC-SHA256(PIN, hello-nonce) truncated 16B (§12.2)
    nonce = 29,  // bstr: WELCOME: 8-byte pairing nonce
    precondition = 30,  // uint: INTENT: expected cfg_gen (CAS guard, §9.3)
    retry_after_ms = 31,  // uint: NACK BUSY: earliest reconnect time
    takeover = 32,  // bool: safety-intent: forcible source takeover flag (§11.4)
    event_kind = 33,  // uint: EVENT: kind discriminator per catalog entry
    seq_of_state = 34,  // uint: EVENT: seq of the STATE twin frame it corresponds to (§9.4)
    grants = 35,  // array: WELCOME: batch grant results — array of {13:priority, 14:granted_rate_hz, 15:channel_id} (§6.3, §10.2)
    granted_publishes = 36,  // array: WELCOME / PUBLISH result: granted STREAM-ingress publishes — array of {14:granted_rate_hz, 15:channel_id, 42:burst} (§6.3, §10.5). Omitted when empty.
    identity = 37,  // map: WELCOME: hub identity (RFC-016) — sub-keys in `identity_keys`. Capability discovery is CATALOG introspection, not a list here: a feature exists iff its channels exist.
    blob = 38,  // map: BLOB_REQ / BLOB_CHUNK / store CRUD intents: which blob, and its item fields (RFC-021) — sub-keys in `blob_keys`.
    trust = 39,  // map: HELLO + WELCOME + AUTH: identity proof, signature material, token presentation, pairing modes (RFC-029/027) — sub-keys in `trust_keys`. Absent = the potato path: bearer token, no crypto, unchanged v1-draft handshake cost.
    body = 40,  // map: EVENT: the kind-specific fields. Integer keys come from the CHANNEL'S CATALOG `schema`, exactly as INTENT's `value` (20) does — NOT from this global space. Grammar fix from the feasibility pass: with kind-specific fields at the top level, every device-authored EVENT channel (the motion-anomaly channel!) would have needed a registry PR to name its own fields — the precise coupling the self-describing catalog exists to prevent.
    intent_seq = 41,  // uint: NACK: seq of the frame being rejected (RFC-001). Hubs SHOULD populate it whenever a specific inbound frame provoked the NACK; clients MUST tolerate its absence. Without it a client with two intents in flight on ONE channel cannot tell which was refused.
    burst = 42,  // float: publishes / granted_publishes ENTRY maps: token-bucket capacity in samples, decoupled from rate (RFC-013). Default = granted rate (today's behavior). Clamped to rate x max_burst_multiple and echoed like every wish. Exists because §10.5 made rate double as bucket depth, so a 2-4/s segment sender with a 25/s peak had to declare 30 Hz — lying to admission control to buy burst.
    reboot_in_ms = 43,  // uint: ECHO `applied` (19): this accepted intent commits by rebooting, in about this many ms (RFC-020). The hub then GOODBYEs every session with REBOOTING; `boot_id` change handles the rest.
};

namespace welcome_limits {
inline constexpr uint8_t max_frame = 1;  // largest frame this hub accepts, bytes
inline constexpr uint8_t max_subscriptions = 2;  // per-session subscription cap
inline constexpr uint8_t retained_pending = 3;  // count of retained STATE pushes that will follow WELCOME
}  // namespace welcome_limits

namespace probe_result {
inline constexpr uint8_t bytes_received = 1;  // bytes received during the probe burst
inline constexpr uint8_t span_ms = 2;  // wall time of the burst as observed by the client
inline constexpr uint8_t loss_pct_x100 = 3;  // loss percentage x100 (2 decimal fixed-point)
inline constexpr uint8_t rtt_ms = 4;  // measured round-trip time, ms
}  // namespace probe_result

namespace identity {
inline constexpr uint8_t product = 1;  // tstr: product/model identifier, e.g. 'slopdrive-32' (<=32 B)
inline constexpr uint8_t fw_version = 2;  // tstr: hub firmware version, e.g. '2.1.47' (<=24 B). Retires the mDNS-TXT-only exposure that made the MFP plugin label devices 'boot 0x...'. A change here SHOULD be surfaced to the user (RFC-029.3).
inline constexpr uint8_t hub_name = 3;  // tstr: operator-assigned machine name (<=32 B). Writable as a str16/str32 setting (RFC-026) where the hub offers one.
inline constexpr uint8_t info = 4;  // map: OPTIONAL device-defined extras (hardware rev, build date...). Keys are device-defined tstr; the protocol never interprets them. Depth: WELCOME map -> identity map -> info map = 3, one under the §5.3 cap.
}  // namespace identity

namespace blob {
inline constexpr uint8_t ns = 1;  // uint: which blob space — see `blob_namespaces`. (Named `ns`, not `namespace`: the generated C++ constant would otherwise be a keyword. Prose and CDDL may say 'namespace'.)
inline constexpr uint8_t store_id = 2;  // uint u8: which store within blob_namespaces.store; absent/0 for the catalog namespace. Declared by the store's STORE-class catalog entry.
inline constexpr uint8_t slot = 3;  // uint u8: item index within the store, 0..capacity-1
inline constexpr uint8_t generation = 4;  // uint u16: the roster generation this request/response is consistent with. Lets a client notice its enumeration went stale mid-transfer.
inline constexpr uint8_t name = 5;  // tstr: item name, <= the store's declared name_max
inline constexpr uint8_t kind = 6;  // tstr: namespaced payload kind, e.g. 'pattern.frayd'. The hub validates kind + size on import and NACKs INVALID_VALUE; it never inspects the payload itself.
inline constexpr uint8_t payload = 7;  // bstr: the opaque item document (<= preset_item_max_bytes / the store's per_item_max). Present on save-with-payload (= import); absent on save means 'capture CURRENT live state'.
inline constexpr uint8_t chunk_index = 8;  // uint: 0-based index of this chunk
inline constexpr uint8_t chunk_count = 9;  // uint: total chunks in this transfer
inline constexpr uint8_t total_bytes = 10;  // uint: total encoded byte length being transferred (lets a receiver size/reject before assembling — RFC-028 no-unbounded-allocation)
}  // namespace blob

namespace trust {
inline constexpr uint8_t client_ver = 1;  // tstr: HELLO — client software version (<=24 B). The CHANGE TRIPWIRE: an observed version change drops a paired device to RECOGNIZED-PENDING (admitted at `watch`, granted role suspended, re-approval surfaced to configure sessions). HONESTY CLAUSE, normative: self-reported, therefore a tripwire and NOT attestation — a deliberately malicious update lies and keeps its token. The real bounds are role scoping, instant revocation, roster visibility and the role-exempt safety ops.
inline constexpr uint8_t client_nonce = 2;  // bstr: HELLO — 8 bytes of CLIENT entropy. The hub signs client_nonce || session_id || boot_id. This was a feasibility-pass BLOCKER: without client entropy the WELCOME signature is replayable from one captured handshake and an evil twin passes verification.
inline constexpr uint8_t sig_request = 3;  // bool: HELLO — 'please sign my nonce'. Signing is ON REQUEST because the S3 has no ECC accelerator (mbedtls software ECDSA: sign ~30-80 ms, one uninterruptible call, never inline in a WS handler), so potato handshakes must stay instant. Absent/false = no signature, no cost. A request is NOT a promise: a hub with no keypair answers with silence, and silence is a conformant answer — only a client that PINNED a key (which it can only have received from that machine's own PAIR_GRANT) is entitled to read silence as failure, after hub_sig_timeout_ms.
inline constexpr uint8_t hub_pubkey = 4;  // bstr: PAIR_GRANT — SEC1-compressed P-256 public key (33 B). Delivered AT THE PAIRING CEREMONY, i.e. TOFU anchored at the moment physical presence was proven. P-256 because WebCrypto can verify it — the browser participates.
inline constexpr uint8_t welcome_sig = 5;  // bstr: WELCOME or HUB_SIG (0x1D) — deterministic ECDSA-P256 (RFC 6979) signature over the 16-byte string client_nonce(8) || session_id(u32 LE) || boot_id(u32 LE). A clone machine copies every identity string and fails this; the client MUST surface 'not your machine' and withhold intents. Potato clients paired by physical ceremony MAY skip verification. TWO DELIVERY POINTS, ONE MEANING: inline in WELCOME where a hub can sign without stalling its own tick, otherwise deferred in HUB_SIG once the hub's own low-priority worker has produced it. Signature material and client handling are identical either way; a client accepts whichever arrives first and ignores a second.
inline constexpr uint8_t token_proof = 6;  // bstr: AUTH — HMAC-SHA256(token, WELCOME nonce) truncated to 16 B. Costs ONE extra round trip per connect; that is the honest price. The 'previous-session nonce' shortcut was DROPPED by the feasibility pass as replay-unsafe (undefined rotation point, and §6.3 makes a successful replay EVICT the real client).
inline constexpr uint8_t presentation_mode = 7;  // uint: how the client presents its token — 0 = bearer (raw 16 B in HELLO; legal, the potato floor stays a memcpy), 1 = proof (AUTH, RECOMMENDED for anything with SHA-256, i.e. everyone but coin cells). v1 transports are cleartext, so bearer is sniffable by a passive LAN observer; the roster records which mode a device uses, making security posture visible.
inline constexpr uint8_t pairing_modes = 8;  // uint: WELCOME — BITMASK of `pairing_modes` this hub currently offers, RE-EVALUATED PER SESSION so a mode that is only transiently available (RFC-027(c)'s push-to-pair window) is advertised only while it is actually open. RFC-027.3 said 'WELCOME limits/identity map'; it lands here instead because it is session-security capability, and `limits` is sized caps while `identity` is who-you-are. Window state is thus observable in-band by any watch session.
}  // namespace trust

namespace trust_ledger {
inline constexpr uint8_t instance_id = 1;  // bstr: the device's 8-byte stable identity (§6.1) — the ledger's primary key, and the value a `session_admin_ops` revoke/pair_approve names.
inline constexpr uint8_t kind = 2;  // tstr: the `client_kind` this device presented (<=16 B, HELLO key 2). Recorded so a roster can say 'mfp-plugin' rather than a hex blob.
inline constexpr uint8_t name = 3;  // tstr: the `client_name` this device presented (<=16 B here; HELLO allows 32 and the ledger truncates, same rule as the 0x0002 roster's str16).
inline constexpr uint8_t version = 4;  // tstr: the `trust`.client_ver observed when the role was last approved (<=24 B). The value the RFC-029 item-2 tripwire compares each HELLO against. Absent/empty means the device never reported one, and a device that reports NO version can never trip the wire — stated plainly because it is a real gap, not an oversight.
inline constexpr uint8_t first_seen = 5;  // uint: UNIX epoch seconds when this device was first paired, or 0 for unknown. ZERO IS THE HONEST DEFAULT AND WILL BE COMMON: the protocol's only clock is hub-boot-relative and wraps in ~71 minutes (§7.2), so a hub can fill this in only if the APPLICATION has a wall clock (SNTP) and pushes it in. The library never invents one.
inline constexpr uint8_t last_seen = 6;  // uint: UNIX epoch seconds of the most recent HELLO from this device, or 0 for unknown. Same wall-clock caveat as first_seen.
inline constexpr uint8_t role = 7;  // uint: the granted `access_levels` value. While `state` is recognized_pending this is the SUSPENDED role — what the device gets back if a configure session re-approves it, not what it currently has.
inline constexpr uint8_t state = 8;  // uint: a `trust_states` value.
inline constexpr uint8_t presentation_mode = 9;  // uint: the `trust`.presentation_mode this device last used (0 bearer / 1 proof). RFC-029.6 requires the roster to record it: a device pairing over cleartext with a raw bearer token has a different security posture than one presenting an HMAC proof, and posture the operator cannot SEE is posture the operator cannot fix.
inline constexpr uint8_t pairing_mode = 10;  // uint: the single `pairing_modes` BIT this device paired through (1 knock_approve / 2 pin_proof / 4 push_to_pair). The audit trail RFC-027 leans on instead of a hard tier ceiling: a configure grant issued through a push-to-pair window is visible as exactly that.
}  // namespace trust_ledger

namespace trust_states {
inline constexpr uint8_t trusted = 0;  // paired, and the version observed at the last HELLO matches the version recorded when the role was approved. The granted role applies in full.
inline constexpr uint8_t recognized_pending = 1;  // RFC-029 item 2's tripwire fired: this device presented a DIFFERENT `client_ver` than the ledger recorded. The session is admitted at `watch`, the granted role is SUSPENDED (not revoked), and a re-approval is surfaced to configure sessions via 0x000A/0x000B. HONESTY CLAUSE, normative: the version is SELF-REPORTED, so this catches an honest update and nothing else — a deliberately malicious update lies about its version and keeps its token. The real bounds on a hostile client are role scoping, instant revocation, roster visibility and the role-exempt safety ops. Do not let a UI imply this is attestation.
}  // namespace trust_states

namespace presentation_modes {
inline constexpr uint8_t bearer = 0;  // raw 16-byte token in HELLO. LEGAL, DEFAULT, and the potato floor — a coin-cell client does exactly this and nothing more. v1 transports are cleartext, so a passive LAN observer who captures one HELLO owns the credential until it is revoked; §12.1 excludes that attacker, and this mode accepts that ceiling knowingly.
inline constexpr uint8_t proof = 1;  // HMAC-SHA256(key = token, message = the WELCOME nonce) truncated to 16 B, presented in an AUTH (0x1C) frame after WELCOME. The token itself NEVER crosses the wire, so a sniffer captures a one-time proof and not the credential. Costs exactly one extra round trip per connect (HELLO -> WELCOME -> AUTH -> GRANT); the client is at `watch` in between, which is the correct posture for a client that has not yet proved anything. The 'reuse the previous session's nonce to skip the round trip' shortcut was DROPPED as replay-unsafe — see trust_keys.token_proof.
}  // namespace presentation_modes

namespace blob_ns {
inline constexpr uint8_t catalog = 0;  // the hub's channel catalog (§8.4). store_id/slot absent. This is the ONLY namespace with a READY concept (CATALOG_READY 0x19) — you cannot decode STATE without the catalog, but nothing gates on a preset.
inline constexpr uint8_t store = 1;  // items in a catalog-declared STORE-class channel: store_id picks the store, slot picks the item. Presets, saved positions, limit profiles, recordings, the trust ledger — all the same machinery, for free. Unused is unproblem.
}  // namespace blob_ns

namespace session_events {
inline constexpr uint8_t takeover = 1;  // control source ownership transferred (§11.4)
inline constexpr uint8_t session_joined = 2;  // a session reached GRANTED
inline constexpr uint8_t session_left = 3;  // a session ended (any reason)
}  // namespace session_events

namespace log_events {
inline constexpr uint8_t entry = 1;  // a log line was published (fields ride `body`: level, tag, hub-ms, message)
}  // namespace log_events

namespace pairing_events {
inline constexpr uint8_t knocked = 1;  // a PAIR_REQ joined the pending list (0x000A) — knock-and-approve or PIN mode (RFC-027.2)
inline constexpr uint8_t granted = 2;  // a pending knock (or an existing device's re-approval) was granted a role, via PAIR_GRANT or the 0x0009 admin surface
inline constexpr uint8_t denied = 3;  // a pending knock was denied by a `configure` session
inline constexpr uint8_t expired = 4;  // a pending knock's window elapsed unanswered (pairing_window_default_s)
inline constexpr uint8_t window_opened = 5;  // a pairing association window opened (push-to-pair boot gesture, or a mode newly advertised in `trust`.pairing_modes)
inline constexpr uint8_t window_closed = 6;  // the pairing association window closed
inline constexpr uint8_t revoked = 7;  // a paired device's token was revoked from the trust ledger (RFC-018 admin surface, store 0x000C)
inline constexpr uint8_t recognized_pending = 8;  // RFC-029 item 2: a paired device's observed `client_ver` changed; state dropped trusted -> RECOGNIZED-PENDING (admitted at watch, granted role suspended pending re-approval)
}  // namespace pairing_events

namespace safety_events {
inline constexpr uint8_t estop_latched = 1;  // the ESTOP bit went 0 -> 1 (§5.5). `body` carries word/cause/owner_session/estop_seq. Cause is a `safety_causes` value; `estop_seq` is the §5.5 per-INITIATION sequence, so repeats of one initiation share it.
inline constexpr uint8_t estop_cleared = 2;  // the ESTOP bit went 1 -> 0 via §11.2's guarded clear (`safety_ops::estop_clear` + the hub's and delegate's preconditions). Clearing never restarts motion; this edge says the latch is gone, never that the machine moved.
inline constexpr uint8_t stop_latched = 3;  // one or more of STOP / HOLD / PAUSE went 0 -> 1. `body.level` is the bitmask of the bits that NEWLY set (safety word bits 1/2/3), so one edge reports one operator action even when it sets several. Cause distinguishes an operator `stop` (user) from a §11.3 deadman (deadman) from a teardown loss policy (session_loss) — which is the whole reason this edge is worth having: all three look identical in the snapshot.
inline constexpr uint8_t stop_cleared = 4;  // one or more of STOP / HOLD / PAUSE went 1 -> 0 (`resume`, or a STOP cleared by an accepted new motion intent per §11.1). `body.level` is the bitmask of the bits that NEWLY cleared.
}  // namespace safety_events

namespace log_levels {
inline constexpr uint8_t trace = 0;  // sloplog::Level::Trace — SLOGT
inline constexpr uint8_t debug = 1;  // sloplog::Level::Debug — SLOGD
inline constexpr uint8_t info = 2;  // sloplog::Level::Info — SLOGI
inline constexpr uint8_t warn = 3;  // sloplog::Level::Warn — SLOGW
inline constexpr uint8_t error = 4;  // sloplog::Level::Error — SLOGE
inline constexpr uint8_t fatal = 5;  // sloplog::Level::Fatal — SLOGF
}  // namespace log_levels

namespace safety_ops {
inline constexpr uint8_t estop_clear = 1;  // clear the ESTOP latch (§11.2 conditions apply; NACK CLEAR_REFUSED otherwise). Requires `control`.
inline constexpr uint8_t stop = 2;  // controlled decel stop (§11.1). ROLE-EXEMPT.
inline constexpr uint8_t hold = 3;  // position hold (§11.1). Requires `control`. The HUB latches all four levels in 0x0003 — delegate acceptance is what triggers the latch; a hub whose delegate does not implement this NACKs UNSUPPORTED_OP, which is discoverable and honest (RFC-025a).
inline constexpr uint8_t pause = 4;  // pattern pause (§11.1). Requires `control`.
inline constexpr uint8_t resume = 5;  // resume from HOLD/PAUSE (§11.1). Requires `control`.
inline constexpr uint8_t estop = 6;  // ASSERT e-stop (RFC-010). ROLE-EXEMPT. The hub treats it exactly as a valid 0xE5 frame: latch, cause=user, publish 0x0003, EVENT twin. The raw 0xE5 frame stays as the deframed-path/relay guarantee; this op is the trivially-implementable client path — without it the red button silently degrades to a decel-stop, which is why this gated port-81 deletion.
inline constexpr uint8_t override_on = 7;  // engage manual override (RFC-025c). Requires `control`. Override/bypass are SAFETY-domain state, not rail-UI state: they render near the rail but other surfaces need them, so they live in the 0x0003 snapshot (appended byte) and are written here.
inline constexpr uint8_t override_off = 8;  // release manual override. Requires `control`.
inline constexpr uint8_t bypass_on = 9;  // engage limit bypass (RFC-025c). Requires `control`. The per-move `bypass` key on a motion INTENT is unaffected and stays as-is.
inline constexpr uint8_t bypass_off = 10;  // release limit bypass. Requires `control`.
}  // namespace safety_ops

namespace session_admin_ops {
inline constexpr uint8_t evict = 1;  // RFC-018: GOODBYE the session named by `session_id` with SESSION_EVICTED. Runs the full §6.8/RFC-005 teardown — the evicted session's source ownership is released under its §11.3 loss policy exactly as if it had crashed, because 'no unmonitored path to motion' does not get an exception for admin actions. Evicting your OWN session is legal and is just a rude GOODBYE to yourself.
inline constexpr uint8_t pair_approve = 2;  // RFC-027(a): approve the pending knock named by `instance_id` at `role`, issuing PAIR_GRANT {token, role} to the knocker. ALSO the RFC-029 item-2 RE-APPROVAL verb: applied to a device in `recognized_pending` it re-records the observed version and restores the suspended role — approving a knock and re-trusting a changed device are the same decision ('this identity may do this'), so they are the same op rather than two that could drift.
inline constexpr uint8_t pair_deny = 3;  // RFC-027(a): drop the pending knock named by `instance_id` without issuing a token; emits pairing_events `denied`. On a `recognized_pending` device this is a REVOKE (op 4) in effect — deny means 'no', and leaving a suspended entry in the ledger after an operator said no would be a lie the roster tells forever.
inline constexpr uint8_t revoke = 4;  // RFC-027(4)/029: delete `instance_id` from the trust ledger. Revocation is PROTOCOL, not a WebUI feature — that is the entire point of putting it here. Takes effect at the next HELLO (an already-live session keeps the role it was admitted with until it reconnects or is evicted; use evict to end it now). Emits pairing_events `revoked`.
}  // namespace session_admin_ops

namespace safety_causes {
inline constexpr uint8_t user = 0;  // operator-initiated (physical button, UI, safety-intents `estop`/`stop`) — §5.5
inline constexpr uint8_t deadman = 1;  // §11.3 deadman window actually elapsed (silence timeout, not some other way the session ended — see session_loss)
inline constexpr uint8_t fault = 2;  // hub/driver-detected fault
inline constexpr uint8_t relay = 3;  // relay-originated (segment-local safety event) — §5.5
inline constexpr uint8_t session_loss = 4;  // RFC-022.3: the owning session ended by ANY non-deadman teardown path (GOODBYE, rude detach, either eviction, slot reuse) — §6.8 / RFC-005's teardownSession() loss policy. Was misreported as cause=deadman before this value existed.
}  // namespace safety_causes

namespace setting_categories {
inline constexpr uint8_t device = 0;  // identity, network, storage, firmware — what the machine IS
inline constexpr uint8_t user = 1;  // everyday operating preferences
inline constexpr uint8_t limits = 2;  // safety envelope: windows, ceilings, e-stop behaviour
inline constexpr uint8_t tuning = 3;  // motion/planner internals; typically `advanced`-flagged
inline constexpr uint8_t diagnostics = 4;  // counters, telemetry, resets — mostly read-only fields
}  // namespace setting_categories

namespace stream_kinds {
inline constexpr uint8_t samples = 0;  // dense points reporting a value AT AN INSTANT (§9.2); a dropped sample is recoverable by interpolation from its neighbours. Decimable under congestion. The default — absent on the wire means this.
inline constexpr uint8_t segments = 1;  // each sample COMMANDS A TIME EXTENT — it carries its own duration, so it is not a point on a continuous curve. A dropped segment is a permanently lost COMMAND, not a recoverable interpolation gap (RFC-014/023). NOT decimable: §10.4's shedding table sheds whole-source or not at all for these.
}  // namespace stream_kinds

namespace procedure_phases {
inline constexpr uint8_t idle = 0;  // not running; the reconnect-safe resting value
inline constexpr uint8_t running = 1;  // started and in progress; `progress` 0–100 is advisory
inline constexpr uint8_t succeeded = 2;  // terminal, ok. Also EVENTed (RFC-020).
inline constexpr uint8_t failed = 3;  // terminal, error — `result` u16 carries a nack_codes value or a device code
inline constexpr uint8_t aborted = 4;  // terminal, cancelled or superseded
}  // namespace procedure_phases

namespace setting_flags {
inline constexpr uint8_t advanced = 1u << 0;  // hide behind an 'advanced' affordance by default; NEVER remove from the surface
inline constexpr uint8_t restart_required = 1u << 1;  // the applied value takes effect on the next boot (distinct from RFC-020's reboot_in_ms, which is the hub rebooting ITSELF to commit)
inline constexpr uint8_t secret = 1u << 2;  // NORMATIVE (RFC-009.5): the value NEVER appears in STATE. The snapshot carries only a set/unset presence bit. Writes ride the paired INTENT normally and ECHO confirms application WITHOUT echoing the value. A WiFi password must never ride a retained snapshot that open-access `watch` sessions receive.
}  // namespace setting_flags

namespace pairing_modes {
inline constexpr uint8_t knock_approve = 1u << 0;  // PRIMARY and capability-agnostic: bare PAIR_REQ with no proof -> bounded pending list (pairing_pending_max) exposed as protocol state (0x000A/0x000B) -> ANY `configure` session approves {instance_id, role}. The joiner needs one button and no display; the trusted surface is any configure client, which kills the circular 'the WebUI is trusted because it's the WebUI' dependency. RECOMMENDED partly because its approval surface shows the knocker's identity on hardware the attacker does not control.
inline constexpr uint8_t pin_proof = 1u << 1;  // the HMAC-PIN flow, for keyboard-bearing joiners when no configure session exists. HONESTY CLAUSE (normative): 4 digits = 10^4 offline HMACs, so a passive observer of the exchange can brute-force it. Acceptable for the v1 threat model (casual/drive-by prevention) and MUST be stated plainly. SPAKE2 is the reserved v2 upgrade — not v1, because WebCrypto has no PAKE and mandating it would exile the browser client.
inline constexpr uint8_t push_to_pair = 1u << 2;  // PHYSICAL-PRESENCE proof opens a short SINGLE-GRANT window. The spec requires the PROOF, not a GPIO — minimum hardware is NONE, because the power cord is the button: factory-fresh (zero configure tokens) boots claimable and the first knock gets `configure` (possession is root); later, N=3 consecutive boots with uptime <10 s opens the window (NVS counter only; cannot collide with a session, since any power loss already stops motion and forces re-home). A hub with a real button MAY bind it — UX upgrade, never required. FACTORY RESET MUST BE A HARDER GESTURE than opening pairing.
}  // namespace pairing_modes

namespace field_roles {
inline constexpr std::string_view limit_user_speed = "limit.user.speed";  // speed ceiling of the USER (manual) limit set. CEILING, never a target.
inline constexpr std::string_view limit_user_accel = "limit.user.accel";  // accel ceiling of the user limit set
inline constexpr std::string_view limit_input_speed = "limit.input.speed";  // speed ceiling of the INPUT (machine-driven: patterns, streams, TCode) limit set
inline constexpr std::string_view limit_input_accel = "limit.input.accel";  // accel ceiling of the input limit set
inline constexpr std::string_view limit_input_jerk = "limit.input.jerk";  // jerk ceiling of the input limit set
inline constexpr std::string_view window_min = "window.min";  // stroke window lower bound. Limits normalized against the window are window-relative and therefore MOVE when it does — which is exactly why this is a STATE field and not a one-shot WELCOME value.
inline constexpr std::string_view window_max = "window.max";  // stroke window upper bound
inline constexpr std::string_view telemetry_position = "telemetry.position";  // live actuator position
inline constexpr std::string_view telemetry_velocity = "telemetry.velocity";  // live actuator velocity
inline constexpr std::string_view telemetry_current = "telemetry.current";  // motor/drive current
inline constexpr std::string_view telemetry_power_bus = "telemetry.power.bus";  // DC bus voltage or power
inline constexpr std::string_view telemetry_temp = "telemetry.temp";  // a temperature reading; the field's own name/unit says which
inline constexpr std::string_view telemetry_uptime = "telemetry.uptime";  // hub uptime
inline constexpr std::string_view identity_name = "identity.name";  // the writable machine-name setting (RFC-026 tier 2, str16/str32). Its READ-ONLY twin is WELCOME identity.hub_name.
inline constexpr std::string_view meta_enabled_mask = "meta.enabled_mask";  // RFC-009.4: a bitfield8 field whose bit i gates the i-th setting-annotated field of the SAME layout. On-change, retained, conflated — every client greys from one ground truth. Disabled means GREY, never hide.
inline constexpr std::string_view meta_reset_gen = "meta.reset_gen";  // RFC-019: increments on every applied reset in this counter group, so ALL subscribers observe the reset, not just the sender who asked for it.
}  // namespace field_roles

enum class NackCode : uint16_t {
    MALFORMED = 0x0000,  // undecodable frame/CBOR
    UNSUPPORTED_VERSION = 0x0001,  // HELLO proto_ver not servable
    FRAME_TOO_LARGE = 0x0002,  // exceeds negotiated max_frame
    PROFILE_VIOLATION = 0x0003,  // CBOR not in deterministic profile
    BUSY = 0x0100,  // client limit reached; carries retry_after_ms
    UNAUTHORIZED = 0x0101,  // token invalid/revoked
    NOT_CONTROLLER = 0x0102,  // control op without controller role
    PAIRING_REQUIRED = 0x0103,  // controller requested, no token, pairing window closed
    PAIRING_DENIED = 0x0104,  // bad pin_proof or pairing window closed
    SESSION_EVICTED = 0x0105,  // slow-consumer or admin kick (GOODBYE code)
    DUPLICATE_INSTANCE = 0x0106,  // instance_id already in live session; old session evicted instead — see §6.8
    NORMAL_CLOSURE = 0x0107,  // clean voluntary teardown (GOODBYE code, either direction) — not an error
    DEADMAN_TIMEOUT = 0x0108,  // hub-initiated session teardown: silence exceeded the deadman window (§11.3, GOODBYE code)
    REBOOTING = 0x0109,  // hub is committing a change by rebooting and is closing every session first (RFC-020/022.2, GOODBYE code). Preceded by an ECHO carrying reboot_in_ms; on return the changed boot_id tells clients what happened.
    READY_TIMEOUT = 0x010A,  // session never sent CATALOG_READY within catalog_ready_timeout_ms (RFC-015, GOODBYE code). Needed because liveness reaping NEVER fires on a client that PINGs happily but never finishes adopting the catalog — it would hold a slot forever with both planes gated shut.
    NOT_READY = 0x010B,  // frame refused because the session has not sent CATALOG_READY yet (RFC-015). READY gates BOTH planes: pre-READY INTENTs are NACK'd, not queued, because a client acting before it has adopted the retained safety latch breaks §11.5(2).
    UNKNOWN_CHANNEL = 0x0200,  // channel id not in catalog
    ACCESS_DENIED = 0x0201,  // channel access level above session role
    CLASS_MISMATCH = 0x0202,  // e.g. SUBSCRIBE to an INTENT channel
    SUB_LIMIT = 0x0203,  // per-session subscription cap reached
    CONFLICT = 0x0300,  // precondition (cfg_gen CAS) failed
    RATE_LIMITED = 0x0301,  // ingress intent rate exceeded
    INVALID_VALUE = 0x0302,  // outside schema min/max or wrong type; also a store import whose kind or size the hub refuses (RFC-021.5)
    UNSUPPORTED_OP = 0x0303,  // intent op not implemented on this hub
    ESTOP_ACTIVE = 0x0400,  // refused while e-stop latched
    NOT_HOMED = 0x0401,  // motion intent before homing
    INTERLOCK = 0x0402,  // hub-specific safety interlock
    SOURCE_CONFLICT = 0x0403,  // another session owns this arbiter source
    TAKEOVER_REQUIRED = 0x0404,  // control exists; retry with takeover flag
    CLEAR_REFUSED = 0x0405,  // e-stop clear conditions not met (§11.2)
    CHUNK_UNAVAILABLE = 0x0500,  // blob chunk index out of range, or the requested namespace/store/slot does not exist (generalized from 'catalog chunk' by RFC-021 — the catalog is now namespace 0)
    REASSEMBLY_TIMEOUT = 0x0501,  // fragment reassembly abandoned (5 s)
    ETAG_MISMATCH = 0x0502,  // static-profile client etag != hub catalog etag
};

namespace limits {
inline constexpr uint32_t header_bytes = 8;
inline constexpr uint32_t min_transport_payload = 242;
inline constexpr uint32_t catalog_chunk_payload = 192;
inline constexpr uint32_t bundle_max_samples = 32;
inline constexpr uint32_t bundle_max_span_ms = 20;
inline constexpr uint32_t seq_width_bits = 16;
inline constexpr uint32_t seq_newer_window = 32768;
inline constexpr uint32_t frag_reassembly_timeout_ms = 5000;
inline constexpr uint32_t frag_max_concurrent_per_session = 2;
inline constexpr uint32_t idempotency_ring_depth = 32;
inline constexpr uint32_t intent_ingress_default_per_s = 50;
inline constexpr uint32_t stream_ingress_overage_nack_per_s = 5;
inline constexpr uint32_t event_queue_depth_per_subscriber = 16;
inline constexpr uint32_t never_shed_stall_eviction_ms = 2000;
inline constexpr uint32_t catalog_chunk_gap_timeout_ms = 500;
inline constexpr uint32_t busy_retry_after_default_ms = 2000;
inline constexpr uint32_t ping_interval_holding_control_ms = 200;
inline constexpr uint32_t ping_interval_idle_ms = 1000;
inline constexpr uint32_t deadman_default_ms = 600;
inline constexpr uint32_t deadman_min_ms = 250;
inline constexpr uint32_t deadman_max_ms = 5000;
inline constexpr uint32_t pairing_window_default_s = 120;
inline constexpr uint32_t pairing_pin_digits = 4;
inline constexpr uint32_t token_bytes = 16;
inline constexpr uint32_t instance_id_bytes = 8;
inline constexpr uint32_t etag_bytes = 8;
inline constexpr uint32_t conformance_min_clients = 4;
inline constexpr uint32_t default_max_clients_ws = 8;
inline constexpr uint32_t default_max_clients_espnow = 4;
inline constexpr uint32_t default_max_clients_ble = 1;
inline constexpr uint32_t default_max_clients_serial = 1;
inline constexpr uint32_t estop_repeat_interval_ms = 50;
inline constexpr uint32_t estop_repeat_max = 20;
inline constexpr uint32_t clock_resync_interval_s = 10;
inline constexpr uint32_t probe_default_bytes = 8192;
inline constexpr uint32_t probe_max_duration_ms = 1500;
inline constexpr uint32_t catalog_max_entries = 256;
inline constexpr uint32_t catalog_max_entry_bytes = 4096;
inline constexpr uint32_t max_subscriptions_per_session = 64;
inline constexpr uint32_t max_frame_ws = 512;
inline constexpr uint32_t max_frame_espnow = 250;
inline constexpr uint32_t max_frame_ble = 244;
inline constexpr uint32_t max_frame_serial = 512;
inline constexpr uint32_t catalog_ready_timeout_ms = 15000;
inline constexpr uint32_t idle_reap_multiplier = 3;
inline constexpr uint32_t max_future_schedule_ms = 250;
inline constexpr uint32_t max_burst_multiple = 4;
inline constexpr uint32_t desc_max_bytes = 128;
inline constexpr uint32_t nack_detail_max_bytes = 48;
inline constexpr uint32_t option_label_max_bytes = 24;
inline constexpr uint32_t preset_capacity_min = 32;
inline constexpr uint32_t preset_item_max_bytes = 4096;
inline constexpr uint32_t paired_devices_max = 8;
inline constexpr uint32_t trust_ledger_max_bytes = 1900;
inline constexpr uint32_t pairing_pending_max = 4;
inline constexpr uint32_t client_ver_max_bytes = 24;
inline constexpr uint32_t trust_ledger_name_max_bytes = 16;
inline constexpr uint32_t trust_ledger_kind_max_bytes = 16;
inline constexpr uint32_t hub_sig_timeout_ms = 3000;
inline constexpr uint32_t auth_attempts_max = 3;
inline constexpr uint32_t log_replay_depth_default = 32;
inline constexpr std::string_view ws_subprotocol = "slopsync.v1";
inline constexpr std::string_view mdns_service = "_slopsync._tcp";
}  // namespace limits

}  // namespace slopsync
