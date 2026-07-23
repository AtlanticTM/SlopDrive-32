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
    CATALOG_REQ = 0x09,  // c2h, control, §8.4
    CATALOG_CHUNK = 0x0A,  // h2c, raw, §8.4
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
};

enum class AccessLevel : uint8_t {
    viewer = 0,
    controller = 1,
    admin = 2,
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
};

namespace channels {
inline constexpr uint16_t catalog = 0x0001;  // STATE: catalog meta: etag, chunk count, entry count
inline constexpr uint16_t session_roster = 0x0002;  // STATE: connected sessions: id, kind, name, roles
inline constexpr uint16_t safety = 0x0003;  // STATE: latched safety word: estop/stop/hold/pause + cause + owner (§11.1)
inline constexpr uint16_t control_owner = 0x0004;  // STATE: active arbiter source + owning session per source (§11.4)
inline constexpr uint16_t safety_intents = 0x0005;  // INTENT: STOP/HOLD/PAUSE/RESUME/ESTOP_CLEAR/TAKEOVER (§11)
inline constexpr uint16_t hub_status = 0x0006;  // STATE: boot_id, fw version, heap, uptime, transport stats
inline constexpr uint16_t session_events = 0x0007;  // EVENT: join/leave/takeover/eviction notifications
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
    publishes = 11,  // array: of {15:channel,12:rate}
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
    probe_result = 26,  // map: PROBE_REPORT: {bytes, span_ms, loss_pct, rtt_ms}
    chunks = 27,  // array: CATALOG_REQ repair: missing chunk indices
    pin_proof = 28,  // bstr: PAIR_REQ: HMAC-SHA256(PIN, hello-nonce) truncated 16B (§12.2)
    nonce = 29,  // bstr: WELCOME: 8-byte pairing nonce
    precondition = 30,  // uint: INTENT: expected cfg_gen (CAS guard, §9.3)
    retry_after_ms = 31,  // uint: NACK BUSY: earliest reconnect time
    takeover = 32,  // bool: safety-intent: forcible source takeover flag (§11.4)
    event_kind = 33,  // uint: EVENT: kind discriminator per catalog entry
    seq_of_state = 34,  // uint: EVENT: seq of the STATE twin frame it corresponds to (§9.4)
    grants = 35,  // array: WELCOME: batch grant results — array of {13:priority, 14:granted_rate_hz, 15:channel_id} (§6.3, §10.2)
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

namespace session_events {
inline constexpr uint8_t takeover = 1;  // control source ownership transferred (§11.4)
inline constexpr uint8_t session_joined = 2;  // a session reached GRANTED
inline constexpr uint8_t session_left = 3;  // a session ended (any reason)
}  // namespace session_events

namespace safety_ops {
inline constexpr uint8_t estop_clear = 1;  // clear the ESTOP latch (§11.2 conditions apply; NACK CLEAR_REFUSED otherwise)
inline constexpr uint8_t stop = 2;  // controlled decel stop (§11.1)
inline constexpr uint8_t hold = 3;  // position hold (§11.1)
inline constexpr uint8_t pause = 4;  // pattern pause (§11.1)
inline constexpr uint8_t resume = 5;  // resume from HOLD/PAUSE (§11.1)
}  // namespace safety_ops

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
    UNKNOWN_CHANNEL = 0x0200,  // channel id not in catalog
    ACCESS_DENIED = 0x0201,  // channel access level above session role
    CLASS_MISMATCH = 0x0202,  // e.g. SUBSCRIBE to an INTENT channel
    SUB_LIMIT = 0x0203,  // per-session subscription cap reached
    CONFLICT = 0x0300,  // precondition (cfg_gen CAS) failed
    RATE_LIMITED = 0x0301,  // ingress intent rate exceeded
    INVALID_VALUE = 0x0302,  // outside schema min/max or wrong type
    UNSUPPORTED_OP = 0x0303,  // intent op not implemented on this hub
    ESTOP_ACTIVE = 0x0400,  // refused while e-stop latched
    NOT_HOMED = 0x0401,  // motion intent before homing
    INTERLOCK = 0x0402,  // hub-specific safety interlock
    SOURCE_CONFLICT = 0x0403,  // another session owns this arbiter source
    TAKEOVER_REQUIRED = 0x0404,  // control exists; retry with takeover flag
    CLEAR_REFUSED = 0x0405,  // e-stop clear conditions not met (§11.2)
    CHUNK_UNAVAILABLE = 0x0500,  // catalog chunk index out of range
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
inline constexpr uint32_t max_subscriptions_per_session = 64;
inline constexpr std::string_view ws_subprotocol = "slopsync.v1";
inline constexpr std::string_view mdns_service = "_slopsync._tcp";
}  // namespace limits

}  // namespace slopsync
