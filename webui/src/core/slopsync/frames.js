/**
 * frames.js — SlopSync 8-byte frame header + all wire-number constants.
 *
 * EVERY number here is transcribed from
 * lib/slopsync/include/slopsync/generated/registry_constants.hpp
 * (source of truth: docs/slopsync/registry/registry.yaml). The generated
 * constant's name is in a comment beside each value. NEVER invent a wire
 * number — if you need one the registry lacks, fix the registry first
 * (CLAUDE.md §8 spec-gap ritual).
 *
 * Frame header (SPEC §5.1), 8 bytes little-endian:
 *   [type:u8][flags:u8][channel:u16][seq:u16][len:u16]
 */

export const PROTO_VER = 1; // kProtocolVersion
export const HEADER_BYTES = 8; // kHeaderBytes / limits.header_bytes
export const WS_SUBPROTOCOL = 'slopsync.v1'; // limits.ws_subprotocol
export const MDNS_SERVICE = '_slopsync._tcp'; // limits.mdns_service

// ---- Frame types (registry FrameType) --------------------------------------
// v1.0 RETIRED 0x09 CATALOG_REQ / 0x0A CATALOG_CHUNK: catalog transfer is now
// the generalized BLOB_REQ (0x1A) / BLOB_CHUNK (0x1B) pair with the catalog as
// blob namespace 0 (RFC-021). Those two numbers are RETIRED, never reused.
export const FRAME = {
  HELLO: 0x00,
  WELCOME: 0x01,
  PING: 0x03,
  PONG: 0x04,
  CLOCK: 0x05,
  SUBSCRIBE: 0x06,
  UNSUBSCRIBE: 0x07,
  GRANT: 0x08,
  STATE: 0x0b,
  STREAM: 0x0c,
  INTENT: 0x0d,
  ECHO: 0x0e,
  EVENT: 0x0f,
  NACK: 0x10,
  GOODBYE: 0x11,
  PROBE: 0x12,
  PROBE_REPORT: 0x13,
  PAIR_REQ: 0x14,
  PAIR_GRANT: 0x15,
  ACKMASK: 0x16,
  BEACON: 0x17,
  PUBLISH: 0x18, // c2h control: mid-session publish renegotiation (RFC-013)
  CATALOG_READY: 0x19, // c2h raw: the 8-byte etag this client operates against (RFC-015)
  BLOB_REQ: 0x1a, // c2h control: namespaced transfer request (RFC-021)
  BLOB_CHUNK: 0x1b, // h2c raw: 14-byte identity header + <=192 payload bytes
  AUTH: 0x1c, // c2h control: token proof presentation (RFC-029)
  HUB_SIG: 0x1d, // h2c control: deferred hub signature (RFC-029)
  ESTOP: 0xe5,
};
/** reverse lookup: type byte -> name */
export const FRAME_NAME = Object.fromEntries(Object.entries(FRAME).map(([k, v]) => [v, k]));

// ---- Frame flags (registry flags::) ----------------------------------------
export const FLAG_FRAG_START = 1 << 0; // flags::FRAG_START
export const FLAG_FRAG_MORE = 1 << 1; // flags::FRAG_MORE

// ---- CBOR map integer keys (registry CborKey — the global key space) --------
export const K = {
  proto_ver: 1,
  client_kind: 2,
  client_name: 3,
  instance_id: 4,
  token: 5,
  session_id: 6,
  boot_id: 7,
  catalog_etag: 8,
  cfg_gen: 9,
  subscriptions: 10,
  publishes: 11,
  rate_hz: 12,
  priority: 13,
  granted_rate_hz: 14,
  channel_id: 15,
  code: 16,
  detail: 17,
  intent_id: 18,
  applied: 19,
  value: 20,
  timestamp: 21,
  limits: 22,
  roles: 23,
  deadman_ms: 24,
  deadman_policy: 25,
  probe_result: 26,
  chunks: 27,
  pin_proof: 28,
  nonce: 29,
  precondition: 30,
  retry_after_ms: 31,
  takeover: 32,
  event_kind: 33,
  seq_of_state: 34,
  grants: 35,
  granted_publishes: 36,
  identity: 37, // WELCOME: hub identity sub-map (RFC-016), keys in IDENTITY_K
  blob: 38, // BLOB_REQ/CHUNK/store CRUD: which blob (RFC-021), keys in BLOB_K
  trust: 39, // HELLO/WELCOME/AUTH: trust sub-map (RFC-027/029), keys in TRUST_K
  body: 40, // EVENT: kind-specific fields, keyed by the CHANNEL'S catalog schema
  intent_seq: 41, // NACK: header seq of the frame being refused (RFC-001)
  burst: 42, // publishes entry: token-bucket capacity (RFC-013)
  reboot_in_ms: 43, // ECHO applied: this intent commits by rebooting (RFC-020)
  deadman_wish_ms: 44, // HELLO: requested per-session deadman window (RFC-038); hub clamps into [deadman_min_ms,deadman_max_ms] and echoes the APPLIED value via the EXISTING key 24 — never a new response key
};

// WELCOME's `identity` (key 37) sub-map (registry identity_keys) — RFC-016 put
// product/fw_version in band so a client stops labeling devices "boot 0x…".
export const IDENTITY_K = { product: 1, fw_version: 2, hub_name: 3, info: 4 };

// `blob` (key 38) sub-map (registry blob_keys). The SAME vocabulary names the
// fields of BLOB_CHUNK's fixed binary header — one table, never two.
export const BLOB_K = {
  ns: 1, store_id: 2, slot: 3, generation: 4, name: 5, kind: 6, payload: 7,
  chunk_index: 8, chunk_count: 9, total_bytes: 10,
};

// Blob namespaces (registry blob_namespaces). The catalog is namespace 0 — the
// only namespace with a READY concept, because nothing else gates STATE decode.
export const BLOB_NS = { catalog: 0, store: 1 };

// `trust` (key 39) sub-map (registry trust_keys). Declared for completeness;
// this client is bearer-token + unverified for now (see session.js).
export const TRUST_K = {
  client_ver: 1, client_nonce: 2, sig_request: 3, hub_pubkey: 4,
  welcome_sig: 5, token_proof: 6, presentation_mode: 7, pairing_modes: 8,
};
export const PRESENTATION_MODE = { bearer: 0, proof: 1 };

// WELCOME's `limits` (key 22) sub-map has its OWN tiny key space (registry
// welcome_limits::) — NOT the global CborKey space above.
export const WELCOME_LIMITS_K = {
  max_frame: 1, // welcome_limits::max_frame
  max_subscriptions: 2, // welcome_limits::max_subscriptions
  retained_pending: 3, // welcome_limits::retained_pending
  // RFC-033.3: most wishes ONE SUBSCRIBE/HELLO frame may carry (16 on the
  // reference hub). Before this was advertised the cap was discoverable only by
  // binary-searching a live machine, and overflowing it dropped the frame in
  // silence — a healthy-looking LIVE session with zero STATE. Missing this
  // mapping is not harmless: the client falls back to a conservative guess and
  // never uses the real value.
  max_subscriptions_per_frame: 4, // welcome_limits::max_subscriptions_per_frame
};

// ---- Access levels (registry AccessLevel) ----------------------------------
// RFC-027 renamed the tiers (wire values UNCHANGED): watch(0) / control(1) /
// configure(2). `control` includes STREAM publishing — a motion producer is a
// controller.
export const ACCESS = { watch: 0, control: 1, configure: 2 };
export const ACCESS_NAME = ['watch', 'control', 'configure'];

// ---- Priorities (registry Priority) ----------------------------------------
export const PRIORITY = { background: 0, normal: 1, elevated: 2, critical: 3 };

// ---- Channel classes (registry ChannelClass) -------------------------------
export const CHANNEL_CLASS = { STATE: 0, STREAM: 1, INTENT: 2, EVENT: 3, STORE: 4 };
export const CHANNEL_CLASS_NAME = ['STATE', 'STREAM', 'INTENT', 'EVENT', 'STORE'];

// ---- Direction (registry Direction: h2c=0, c2h=1) --------------------------
export const DIRECTION_NAME = ['h2c', 'c2h'];

// ---- Packed field types (registry PackedFieldType) -------------------------
// RFC-026 added the fixed-width string types (8/9/10): zero-padded UTF-8,
// register-map style, so packed offsets stay static and append-only evolution
// is preserved.
export const PACKED = {
  u8: 0, i8: 1, u16: 2, i16: 3, u32: 4, i32: 5, f32: 6, bitfield8: 7,
  str16: 8, str32: 9, str64: 10,
};
export const PACKED_NAME = [
  'u8', 'i8', 'u16', 'i16', 'u32', 'i32', 'f32', 'bitfield8', 'str16', 'str32', 'str64',
];
/** wire size in bytes for each PackedFieldType */
export const PACKED_SIZE = [1, 1, 2, 2, 4, 4, 4, 1, 16, 32, 64];

// ---- Setting categories (registry setting_categories, RFC-009) -------------
export const SETTING_CATEGORY = { device: 0, user: 1, limits: 2, tuning: 3, diagnostics: 4 };
export const SETTING_CATEGORY_NAME = ['device', 'user', 'limits', 'tuning', 'diagnostics'];

// ---- Setting flags (registry setting_flags, RFC-009) -----------------------
export const SETTING_FLAG = { advanced: 1 << 0, restart_required: 1 << 1, secret: 1 << 2 };

// ---- STREAM kinds (registry stream_kinds, RFC-014/023) ---------------------
export const STREAM_KIND = { samples: 0, segments: 1 };

// ---- CBOR schema field types (registry CborFieldType) ----------------------
// uint_t=0, int_t=1, f32_t=2, bool_t=3, tstr_t=4, bstr_t=5 (per catalog.hpp)
export const CBOR_FIELD = { uint_t: 0, int_t: 1, f32_t: 2, bool_t: 3, tstr_t: 4, bstr_t: 5 };
export const CBOR_FIELD_NAME = ['uint_t', 'int_t', 'f32_t', 'bool_t', 'tstr_t', 'bstr_t'];

// ---- Reserved registry channel ids (registry channels::) -------------------
export const CH_CATALOG = 0x0001; // channels::catalog
export const CH_SESSION_ROSTER = 0x0002; // channels::session_roster
export const CH_SAFETY = 0x0003; // channels::safety
export const CH_CONTROL_OWNER = 0x0004; // channels::control_owner
export const CH_SAFETY_INTENTS = 0x0005; // channels::safety_intents
export const CH_HUB_STATUS = 0x0006; // channels::hub_status
export const CH_SESSION_EVENTS = 0x0007; // channels::session_events
export const CH_LOG = 0x0008; // channels::log (EVENT, RFC-017)
export const CH_SESSION_ADMIN = 0x0009; // channels::session_admin (INTENT, RFC-018)
export const CH_PENDING_PAIRING = 0x000a; // channels::pending_pairing (STATE, RFC-027)
export const CH_PAIRING_EVENTS = 0x000b; // channels::pairing_events (EVENT, RFC-027)
export const CH_PAIRED_DEVICES = 0x000c; // channels::paired_devices (STORE, RFC-029)
export const CH_PAIRED_DEVICES_ROSTER = 0x000d; // channels::paired_devices_roster (STATE)
export const CH_SAFETY_EVENTS = 0x000e; // channels::safety_events (EVENT twin of 0x0003)

// ---- Device channel ids (include/comms/SlopSyncCatalog.h ch::) -------------
// RFC-047 "Phase C2" renumbered these onto the new grid (Jul 2026). Values
// below are current; see the RFC for the old->new table if you need history.
export const CH_MOTION = 0x1100; // ch::motion (STATE)
export const CH_MACHINE_CONFIG = 0x1000; // ch::machine_config (STATE)
export const CH_PATTERN_STATE = 0x1200; // ch::pattern_state (STATE)
export const CH_ODOMETER = 0x1020; // ch::odometer (STATE)
export const CH_MOTION_INPUT = 0x2100; // ch::motion_input (STREAM c2h)
export const CH_MOTION_SEGMENT = 0x2101; // ch::motion_segment (STREAM c2h)
export const CH_PLAN_STRIP = 0x1110; // ch::plan_strip (STATE, 45 Hz diagnostics)
export const CH_POWER = 0x1010; // ch::power (STATE, only when the hardware exists)
export const CH_MOTION_DIAG = 0x1111; // ch::motion_diag (STATE, slopmotion counters)
export const CH_MOTION_ANOMALY = 0x4100; // ch::motion_anomaly (EVENT)
// M5b: the four MODE settings the legacy :81/HTTP plane owned. A separate
// category from 0x1000 because that channel's RFC-009 enabled_mask is a
// bitfield8 with seven of eight bits already spoken for.
export const CH_MACHINE_MODES = 0x1030; // ch::machine_modes (STATE)
export const CH_MOVE = 0x3100; // ch::move (INTENT)
export const CH_CONFIG_SET = 0x3000; // ch::config_set (INTENT)
export const CH_PATTERN_CMD = 0x3200; // ch::pattern_cmd (INTENT)
export const CH_HOME = 0x3101; // ch::home (INTENT)
export const CH_MODES_SET = 0x3030; // ch::modes_set (INTENT)

// ---- Safety intent ops (registry safety_ops::) -----------------------------
// estop(6) is RFC-010's client-assertable e-stop: the hub treats it exactly as
// a valid 0xE5 frame (latch, cause=user, publish 0x0003, EVENT twin). It and
// `stop` are ROLE-EXEMPT — anyone connected may stop this machine (RFC-025b).
// override/bypass (7..10) write the 0x0003 snapshot's appended `modes` byte.
export const SAFETY_OP = {
  estop_clear: 1, stop: 2, hold: 3, pause: 4, resume: 5, estop: 6,
  override_on: 7, override_off: 8, bypass_on: 9, bypass_off: 10,
};
/** ops any session may send regardless of role (RFC-025b) — safety outranks authorization. */
export const SAFETY_OP_ROLE_EXEMPT = new Set([SAFETY_OP.stop, SAFETY_OP.estop]);

// ---- Home intent ops (registry home ops; see SlopSyncCatalog.h 0x3101) -----
export const HOME_OP = { home: 1, force_home: 2, clear_override: 3 };

// ---- Safety causes (registry safety_causes::) ------------------------------
export const SAFETY_CAUSE = { user: 0, deadman: 1, fault: 2, relay: 3, session_loss: 4 };
export const SAFETY_CAUSE_NAME = ['user', 'deadman', 'fault', 'relay', 'session_loss'];

// ---- EVENT kind discriminators (key 33) ------------------------------------
export const SESSION_EVENT_KIND = { takeover: 1, session_joined: 2, session_left: 3 };
export const SAFETY_EVENT_KIND = {
  estop_latched: 1, estop_cleared: 2, stop_latched: 3, stop_cleared: 4,
};
export const LOG_EVENT_KIND = { entry: 1 };
export const LOG_LEVEL_NAME = ['trace', 'debug', 'info', 'warn', 'error', 'fatal'];

// ---- Pairing modes (registry `pairing_modes` bitmask, RFC-027) -------------
// The SAME three bits everywhere they appear: WELCOME `trust.pairing_modes`
// (TRUST_K.pairing_modes, key 8 — "which ceremonies this hub offers RIGHT
// NOW, re-evaluated per session"), the pending-pairing (0x000A) slot's `kind`
// field, and the paired-devices roster's `pairing_mode` — "the single bit a
// device paired through". ROLE IS AN ATTRIBUTE OF THE GRANT, NEVER OF THE
// CEREMONY: all three modes end in PAIR_GRANT {token, role}.
export const PAIRING_MODE = { knock_approve: 1, pin_proof: 2, push_to_pair: 4 };
/** bit value -> short registry name, for generic rendering. */
export const PAIRING_MODE_NAME = {
  [PAIRING_MODE.knock_approve]: 'knock_approve',
  [PAIRING_MODE.pin_proof]: 'pin_proof',
  [PAIRING_MODE.push_to_pair]: 'push_to_pair',
};

// ---- pairing-events (0x000B) kind discriminators (registry pairing_event_kinds) --
export const PAIRING_EVENT_KIND = {
  knocked: 1, granted: 2, denied: 3, expired: 4,
  window_opened: 5, window_closed: 6, revoked: 7, recognized_pending: 8,
};

// ---- NACK codes (registry NackCode) ----------------------------------------
export const NACK = {
  MALFORMED: 0x0000,
  UNSUPPORTED_VERSION: 0x0001,
  FRAME_TOO_LARGE: 0x0002,
  PROFILE_VIOLATION: 0x0003,
  BUSY: 0x0100,
  UNAUTHORIZED: 0x0101,
  NOT_CONTROLLER: 0x0102,
  PAIRING_REQUIRED: 0x0103,
  PAIRING_DENIED: 0x0104,
  SESSION_EVICTED: 0x0105,
  DUPLICATE_INSTANCE: 0x0106,
  NORMAL_CLOSURE: 0x0107,
  DEADMAN_TIMEOUT: 0x0108,
  REBOOTING: 0x0109, // hub closing every session to commit a change (RFC-020)
  READY_TIMEOUT: 0x010a, // never sent CATALOG_READY in catalog_ready_timeout_ms
  NOT_READY: 0x010b, // refused: this session has not sent CATALOG_READY (RFC-015)
  IDLE_REAPED: 0x010c, // RFC-039.4: hub reaped a non-owning session that fell silent past the idle threshold — GOODBYE code, deliberately distinct from DEADMAN_TIMEOUT (housekeeping, not a motion-safety event)
  UNKNOWN_CHANNEL: 0x0200,
  ACCESS_DENIED: 0x0201,
  CLASS_MISMATCH: 0x0202,
  SUB_LIMIT: 0x0203,
  CONFLICT: 0x0300,
  RATE_LIMITED: 0x0301,
  INVALID_VALUE: 0x0302,
  UNSUPPORTED_OP: 0x0303,
  ESTOP_ACTIVE: 0x0400,
  NOT_HOMED: 0x0401,
  INTERLOCK: 0x0402,
  SOURCE_CONFLICT: 0x0403,
  TAKEOVER_REQUIRED: 0x0404,
  CLEAR_REFUSED: 0x0405,
  CHUNK_UNAVAILABLE: 0x0500,
  REASSEMBLY_TIMEOUT: 0x0501,
  ETAG_MISMATCH: 0x0502,
  BLOB_REFUSED: 0x0503, // RFC-039.2: a RECEIVER (this client) refusing a declared blob whose total_bytes exceeds its reassembly cap — sent as a GOODBYE code instead of idling in a half-session (the "LIVE WITH NO CATALOG" outage BlobReassembler's own comment used to describe)
};
export const NACK_NAME = Object.fromEntries(Object.entries(NACK).map(([k, v]) => [v, k]));

/**
 * GOODBYE reason codes. RFC-022.2: GOODBYE has NO separate code space — its
 * codes are DRAWN FROM `nack_codes`, which is why these are aliases and not
 * fresh numbers. Two clients had independently hand-written the 0x0107 literal
 * before this existed; naming it is the fix.
 */
export const GOODBYE_CODE = {
  NORMAL_CLOSURE: NACK.NORMAL_CLOSURE, // 0x0107 — clean voluntary teardown
  SESSION_EVICTED: NACK.SESSION_EVICTED,
  DEADMAN_TIMEOUT: NACK.DEADMAN_TIMEOUT,
  REBOOTING: NACK.REBOOTING,
  READY_TIMEOUT: NACK.READY_TIMEOUT,
  IDLE_REAPED: NACK.IDLE_REAPED, // RFC-039.4: housekeeping reap of a dark viewer, never a safety event
  BLOB_REFUSED: NACK.BLOB_REFUSED, // RFC-039.2: this client refusing an over-cap blob transfer
};

/**
 * §4.3: an unknown NACK code MUST be treated as the generic code of its range
 * (the high byte) — never an error, never a reason to disconnect.
 * @param {number} code
 * @returns {string}
 */
export function nackName(code) {
  if (code == null) return '?';
  if (code in NACK_NAME) return NACK_NAME[code];
  const range = (code >> 8) & 0xff;
  return 'UNKNOWN(0x' + code.toString(16).padStart(4, '0') + ', range 0x' + range.toString(16).padStart(2, '0') + '__)';
}

// ---- Registry numeric limits (registry limits::) ---------------------------
export const LIMITS = {
  min_transport_payload: 242, // limits::min_transport_payload
  catalog_chunk_payload: 192, // limits::catalog_chunk_payload
  deadman_default_ms: 600, // limits::deadman_default_ms
  deadman_min_ms: 250, // limits::deadman_min_ms
  deadman_max_ms: 5000, // limits::deadman_max_ms
  ping_interval_holding_control_ms: 200, // limits::ping_interval_holding_control_ms
  ping_interval_idle_ms: 1000, // limits::ping_interval_idle_ms
  clock_resync_interval_s: 10, // limits::clock_resync_interval_s
  instance_id_bytes: 8, // limits::instance_id_bytes
  etag_bytes: 8, // limits::etag_bytes
  catalog_chunk_gap_timeout_ms: 500, // limits::catalog_chunk_gap_timeout_ms
  frag_reassembly_timeout_ms: 5000, // limits::frag_reassembly_timeout_ms
  catalog_ready_timeout_ms: 15000, // limits::catalog_ready_timeout_ms
  nack_detail_max_bytes: 48, // limits::nack_detail_max_bytes
  max_frame_ws: 512, // limits::max_frame_ws
  token_bytes: 16, // limits::token_bytes
};

// ============================================================================
// Frame header build / parse — 8-byte little-endian
// ============================================================================

/**
 * Build a full frame (header + payload).
 * @param {number} type frame type byte (FRAME.*)
 * @param {number} channel target channel id (0 = session-scoped control)
 * @param {Uint8Array} [payload]
 * @param {number} [seq]
 * @param {number} [flags]
 * @returns {Uint8Array}
 */
export function encodeFrame(type, channel, payload = new Uint8Array(0), seq = 0, flags = 0) {
  const out = new Uint8Array(HEADER_BYTES + payload.length);
  const dv = new DataView(out.buffer);
  dv.setUint8(0, type);
  dv.setUint8(1, flags);
  dv.setUint16(2, channel, true);
  dv.setUint16(4, seq, true);
  dv.setUint16(6, payload.length, true);
  out.set(payload, HEADER_BYTES);
  return out;
}

// ---- The ESTOP frame (§5.5) — 12 bytes, OUTSIDE the header discipline ------
// E5 E5 E5 E5 | cause:u8 origin:u8 seq:u16 | crc32:u32   (all LE)
// crc32 is CRC-32/IEEE over the first 8 bytes. Deliberately recognizable
// WITHOUT deframing so a serial ISR or relay hot path can act on it — and, for
// this client, the reason a raw e-stop still works while the session is
// pre-READY: the hub matches the magic BEFORE header decode and before any
// readiness/occupancy gate (hub_impl.hpp::pumpSlot).
export const ESTOP_FRAME_BYTES = 12;

const CRC32_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    t[n] = c >>> 0;
  }
  return t;
})();

/** CRC-32/IEEE (wire/crc32.hpp). @param {Uint8Array} bytes @returns {number} u32 */
export function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC32_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/**
 * Build the 12-byte raw ESTOP frame.
 * @param {number} cause a SAFETY_CAUSE value (user for an operator press)
 * @param {number} origin the initiator's AccessLevel
 * @param {number} seq increments per INITIATION event (repeats share it, §5.5)
 * @returns {Uint8Array}
 */
export function encodeEstopFrame(cause, origin, seq) {
  const out = new Uint8Array(ESTOP_FRAME_BYTES);
  out[0] = 0xe5; out[1] = 0xe5; out[2] = 0xe5; out[3] = 0xe5;
  const dv = new DataView(out.buffer);
  dv.setUint8(4, cause & 0xff);
  dv.setUint8(5, origin & 0xff);
  dv.setUint16(6, seq & 0xffff, true);
  dv.setUint32(8, crc32(out.subarray(0, 8)), true);
  return out;
}

/**
 * Decode one 8-byte header from `buf` at `off`.
 * @param {Uint8Array} buf
 * @param {number} [off]
 * @returns {{type:number, flags:number, channel:number, seq:number, len:number}|null}
 */
export function decodeFrameHeader(buf, off = 0) {
  if (buf.length - off < HEADER_BYTES) return null;
  const dv = new DataView(buf.buffer, buf.byteOffset + off, HEADER_BYTES);
  return {
    type: dv.getUint8(0),
    flags: dv.getUint8(1),
    channel: dv.getUint16(2, true),
    seq: dv.getUint16(4, true),
    len: dv.getUint16(6, true),
  };
}

/**
 * Split one binary WS message into its frames. Usually one frame per message,
 * but the hub MAY pack multiple BLOB_CHUNK frames back-to-back (§8.4), so a
 * receiver must walk the buffer. Returns [{header, payload}, ...].
 * @param {Uint8Array} buf
 * @returns {Array<{header:{type:number,flags:number,channel:number,seq:number,len:number}, payload:Uint8Array}>}
 */
export function parseFrames(buf) {
  const out = [];
  let off = 0;
  while (off + HEADER_BYTES <= buf.length) {
    const header = decodeFrameHeader(buf, off);
    if (!header) break;
    const start = off + HEADER_BYTES;
    const end = start + header.len;
    if (end > buf.length) break; // truncated / not our framing — stop
    out.push({ header, payload: buf.subarray(start, end) });
    off = end;
  }
  return out;
}
