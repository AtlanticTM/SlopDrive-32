/**
 * wire.js — binary frame codecs matching include/ui/UiProtocol.h Appendix C.
 *
 * All multi-byte values little-endian. All functions are pure, unit-testable,
 * zero dependencies. DataView-based parsing and Uint8Array-based building.
 */

// ---- Frame type markers ---------------------------------------------------
export const FRAME_HELLO     = 0x00;
export const FRAME_TELEMETRY = 0x01;
export const FRAME_STATUS    = 0x02;
export const FRAME_CLOCK     = 0x03;
export const FRAME_INTERP    = 0x04;
export const FRAME_ANOMALY   = 0x05;
export const FRAME_STATS     = 0x06;
export const FRAME_CMD       = 0x10;
export const FRAME_ECHO      = 0x11;

// ---- 0x10 CMD op codes (mirrors UiProtocol.h) -----------------------------
export const OP_SET_WINDOW  = 0x01;
export const OP_SET_SPEED   = 0x02;
export const OP_SET_ACCEL   = 0x03;
export const OP_GEN_CFG     = 0x04;
export const OP_GEN_RUN     = 0x05;
export const OP_MODE        = 0x06;
export const OP_BLEND       = 0x07;
export const OP_PAUSE       = 0x08;
export const OP_HALT        = 0x09;
export const OP_ESTOP       = 0x0A;
export const OP_HOME        = 0x0B;
export const OP_OVERRIDE    = 0x0C;
export const OP_BYPASS      = 0x0D;
export const OP_CLEAR_FAULT = 0x0E;
export const OP_SAVE        = 0x0F;
export const OP_GET_CFG     = 0x10;
export const OP_MOVE        = 0x11;
export const OP_STREAM_MODE = 0x12;
// Monotone (Fritsch–Carlson) tangent clamp on the v4 gradient cubic. Kills the
// invented overshoot-then-return micromotion. Firmware op WS_OP_OVERSHOOT. :3
export const OP_OVERSHOOT    = 0x13;
// TEST/bench: fake-home without a motor so the UI populates. {on:bool, stroke?:float}.
// Firmware op WS_OP_HOME_OVERRIDE. on:true forces homed + reports stroke (default 250mm). :3
export const OP_HOME_OVERRIDE = 0x14;

// ---- Stream speed-feed modes (mirrors SystemState::StreamSpeedMode) --------
export const SPEED_CEILING_PEGGED   = 0;
export const SPEED_VELOCITY_MATCHED = 1;

// ---- Interp style enum (mirrors InterpStyle in MotionInterpolator.h) -------
export const INTERP_STYLE_NAMES = ['Ramped', 'EaseIn', 'EaseOut', 'EaseBoth', 'Gradient'];

// ---- Anomaly kind enum (mirrors InterpAnomalyType in MotionInterpolator.h)
// index 0 = None (never sent). Names double as UI labels + tooltip keys.
export const ANOMALY_KIND_NAMES = ['None', 'Overshoot', 'PointDropped', 'DecelOverrun', 'DurFallback'];
// Human-readable one-liners for the WebUI anomaly log — what each event MEANS
// for the motion path so the operator can act on it, not just see a code.
export const ANOMALY_KIND_DESC = {
  Overshoot:    'Steep G tangent bulged the cubic past the [start,end] envelope (invented overshoot-then-return micromotion).',
  PointDropped: 'A bare v3 point arrived after a gap and was dropped — the "fails to move at slow speed" case.',
  DecelOverrun: 'A live/gradient segment starved (no successor packet) → decel-to-stop was invented.',
  DurFallback:  'MFP sent G<slope> without a usable I<ms> — segment duration was invented from the rolling mean interval.'
};

// ---- Flag bit positions for 0x04 INTERP -----------------------------------
export const INTERP_FLAG_ACTIVE = 0x01;
export const INTERP_FLAG_LIVE   = 0x02;
export const INTERP_FLAG_GRAD   = 0x04;

// ---- Flag bit positions for 0x01 ------------------------------------------
// Mirrors UiSocket::sendTelemetry's flags byte exactly. The old table here
// called bit1 "FAULT" and bit2 "GEN_RUNNING" — wrong: firmware sets bit1 =
// pattern running (PB-004) and bit2 = gen_active (compat mirror). :3
export const FLAG_HOMED           = 0x01;
export const FLAG_GEN_RUNNING     = 0x02;   // PatternEngine running (PB-004)
export const FLAG_GEN_ACTIVE      = 0x04;   // generator emitting (compat mirror)
export const FLAG_ESTOP           = 0x08;   // e-stopped (latched until re-home)
export const FLAG_PAUSED          = 0x10;
export const FLAG_OVERRIDE        = 0x20;
export const FLAG_INTIFACE_ACTIVE = 0x40;

// ---- Transport enum (mirrors firmware) ------------------------------------
export const TRANSPORT_NAMES = ['WS', 'SER', 'BT', 'DONGLE', 'OSSM'];

// ============================================================================
// Parsing helpers
// ============================================================================

/**
 * Parse a 0x00 HELLO frame (device→client on connect).
 * u8 type=0x00, u8 proto_ver, u16 cfg_gen
 * @param {DataView} dv — positioned at the type byte
 * @returns {{proto_ver:number, cfg_gen:number}}
 */
export function parseHello(dv) {
  return {
    proto_ver: dv.getUint8(1),
    cfg_gen:   dv.getUint16(2, true)   // LE u16
  };
}

/**
 * Parse a 0x01 TELEMETRY frame (device→client, 40–50Hz).
 * u8 type, u8 flags, u16 seq, u32 t_dev_us, u8 n, u8 dt_100us,
 * n × {u16 pos_10um, u16 tgt_10um, u16 raw_10um},
 * u16 i_bus_mA
 *
 * @param {DataView} dv — positioned at the type byte
 * @param {number} byteLength — total frame length in bytes
 * @returns {{flags:number, seq:number, t_dev_us:number, n:number, dt_100us:number, samples:Array<{pos_10um:number, tgt_10um:number, raw_10um:number}>, i_bus_mA:number}|null}
 */
export function parseTelemetry(dv, byteLength) {
  // Header: 1+1+2+4+1+1 = 10 bytes → type[0] flags[1] seq[2..3]
  // t_dev_us[4..7] n[8] dt_100us[9], samples start at byte 10.
  if (byteLength < 10) return null;
  var n = dv.getUint8(8);
  var dt = dv.getUint8(9);
  // Each sample: 6 bytes (3×u16)
  var samplesSize = n * 6;
  // Trailing i_bus_mA: 2 bytes
  if (byteLength < 10 + samplesSize + 2) return null;

  var samples = [];
  var off = 10; // start of sample data
  for (var i = 0; i < n; i++) {
    samples.push({
      pos_10um: dv.getUint16(off,      true),
      tgt_10um: dv.getUint16(off + 2,  true),
      raw_10um: dv.getUint16(off + 4,  true)
    });
    off += 6;
  }

  return {
    flags:     dv.getUint8(1),
    seq:       dv.getUint16(2, true),
    t_dev_us:  dv.getUint32(4, true),
    n:         n,
    dt_100us:  dt,
    samples:   samples,
    i_bus_mA:  dv.getUint16(off, true)
  };
}

/**
 * Parse a 0x02 STATUS frame (device→client, 2Hz).
 * u8 type, u16 bus_mV, i16 die_c10, u16 peak_mA, i8 rssi, u8 wifi_ch,
 * u16 reconnects, u16 tx_drops, u32 heap_free, u16 cfg_gen, u8 transport,
 * u32 measured_stroke_10um, u8 ip[4]  (28 bytes total)
 *
 * @param {DataView} dv — positioned at the type byte
 * @returns {{bus_mV:number, die_c10:number, peak_mA:number, rssi:number, wifi_ch:number, reconnects:number, tx_drops:number, heap_free:number, cfg_gen:number, transport:number, measured_stroke_10um:number, ip:number[]}|null}
 */
export function parseStatus(dv) {
  return {
    bus_mV:              dv.getUint16(1, true),
    die_c10:             dv.getInt16(3, true),
    peak_mA:             dv.getUint16(5, true),
    rssi:                dv.getInt8(7),
    wifi_ch:             dv.getUint8(8),
    reconnects:          dv.getUint16(9, true),
    tx_drops:            dv.getUint16(11, true),
    heap_free:           dv.getUint32(13, true),
    cfg_gen:             dv.getUint16(17, true),
    transport:           dv.getUint8(19),
    measured_stroke_10um: dv.getUint32(20, true),
    ip: [
      dv.getUint8(24),
      dv.getUint8(25),
      dv.getUint8(26),
      dv.getUint8(27)
    ]
  };
}

/**
 * Parse a 0x06 STATS frame (device→client, ~2Hz). Session odometer totals.
 * Layout: u8 type, u16 max_speed_mm_s, u32 distance_mm, u32 energy_mwh,
 * u32 strokes, u32 session_ms  (19 bytes total). See UiProtocol.h.
 * @param {DataView} dv — positioned at the type byte
 * @returns {{max_speed_mm_s:number, distance_mm:number, energy_wh:number, strokes:number, session_ms:number}}
 */
export function parseStats(dv) {
  return {
    max_speed_mm_s: dv.getUint16(1, true),
    distance_mm:    dv.getUint32(3, true),
    energy_wh:      dv.getUint32(7, true) / 1000,   // milli-Wh → Wh
    strokes:        dv.getUint32(11, true),
    session_ms:     dv.getUint32(15, true)
  };
}

/**
 * Parse a 0x11 ECHO frame (device→client).
 * u8 type, u16 id, u8 ok, u16 cfg_gen, payload = JSON (UTF-8)
 *
 * @param {DataView} dv — positioned at the type byte
 * @param {number} byteLength
 * @returns {{id:number, ok:number, cfg_gen:number, payload:object}|null}
 */
export function parseEcho(dv, byteLength) {
  // Header: 1+2+1+2 = 6 bytes
  if (byteLength < 6) return null;
  var jsonLen = byteLength - 6;
  var payload = null;
  if (jsonLen > 0) {
    var jsonStr = '';
    for (var i = 6; i < byteLength; i++) {
      jsonStr += String.fromCharCode(dv.getUint8(i));
    }
    try { payload = JSON.parse(jsonStr); } catch(e) { payload = null; }
  }
  return {
    id:       dv.getUint16(1, true),
    ok:       dv.getUint8(3),
    cfg_gen:  dv.getUint16(4, true),
    payload:  payload
  };
}

/**
 * Parse a 0x04 INTERP frame (device→client, ~45Hz) — interpolator debug.
 * Layout (LE, 19 bytes) mirrors UiProtocol.h INTERP_FRAME_SIZE:
 *   u8 type, u8 flags, u8 style,
 *   u16 startPos_1e4, u16 endPos_1e4, u16 curPos_1e4,
 *   i16 curVel_1e3, u32 durationUs, u32 elapsedUs
 * Positions returned normalized 0..1; velocity in units/second (signed).
 *
 * @param {DataView} dv — positioned at the type byte
 * @param {number} byteLength
 * @returns {{active:boolean, liveMode:boolean, gradMode:boolean, style:number, styleName:string, startPos:number, endPos:number, curPos:number, curVel:number, durationUs:number, elapsedUs:number}|null}
 */
export function parseInterp(dv, byteLength) {
  if (byteLength < 19) return null;
  var flags = dv.getUint8(1);
  var style = dv.getUint8(2);
  return {
    active:     (flags & INTERP_FLAG_ACTIVE) !== 0,
    liveMode:   (flags & INTERP_FLAG_LIVE)   !== 0,
    gradMode:   (flags & INTERP_FLAG_GRAD)   !== 0,
    style:      style,
    styleName:  INTERP_STYLE_NAMES[style] || ('?' + style),
    startPos:   dv.getUint16(3, true)  / 10000,
    endPos:     dv.getUint16(5, true)  / 10000,
    curPos:     dv.getUint16(7, true)  / 10000,
    curVel:     dv.getInt16(9, true)   / 1000,
    durationUs: dv.getUint32(11, true),
    elapsedUs:  dv.getUint32(15, true)
  };
}

/**
 * Parse a 0x05 ANOMALY frame (device→client, event-driven) — interpolator
 * anomaly event. Layout (LE, 28 bytes) mirrors UiProtocol.h ANOMALY_FRAME_SIZE:
 *   u8 type, u8 kind, u16 seq, u32 tDevUs,
 *   u16 targetPos_1e4, u16 startPos_1e4,
 *   f32 startVel, f32 endSlope, u32 durationUs, f32 extra
 * Positions returned normalized 0..1. startVel/endSlope/extra are raw floats;
 * `extra` is kind-specific (overshoot fraction / gap_us / vEnd) — see
 * ANOMALY_KIND_DESC for what each means.
 *
 * @param {DataView} dv — positioned at the type byte
 * @param {number} byteLength
 * @returns {{kind:number, kindName:string, desc:string, seq:number, tDevUs:number, targetPos:number, startPos:number, startVel:number, endSlope:number, durationUs:number, extra:number}|null}
 */
export function parseAnomaly(dv, byteLength) {
  if (byteLength < 28) return null;
  var kind = dv.getUint8(1);
  var kindName = ANOMALY_KIND_NAMES[kind] || ('?' + kind);
  return {
    kind:       kind,
    kindName:   kindName,
    desc:       ANOMALY_KIND_DESC[kindName] || '',
    seq:        dv.getUint16(2, true),
    tDevUs:     dv.getUint32(4, true),
    targetPos:  dv.getUint16(8,  true) / 10000,
    startPos:   dv.getUint16(10, true) / 10000,
    startVel:   dv.getFloat32(12, true),
    endSlope:   dv.getFloat32(16, true),
    durationUs: dv.getUint32(20, true),
    extra:      dv.getFloat32(24, true)
  };
}

/**
 * Parse a 0x03 CLOCK reply (device→client).
 * u8 type, u32 t0_echo, u32 t1_dev_us, u32 t2_dev_us
 *
 * @param {DataView} dv — positioned at the type byte
 * @returns {{t0_echo:number, t1_dev_us:number, t2_dev_us:number}}
 */
export function parseClockReply(dv) {
  return {
    t0_echo:   dv.getUint32(1, true),
    t1_dev_us: dv.getUint32(5, true),
    t2_dev_us: dv.getUint32(9, true)
  };
}

// ============================================================================
// Building helpers
// ============================================================================

/**
 * Build a 0x03 CLOCK request frame (client→device).
 * u8 type=0x03, u32 t0_client_us
 *
 * @param {number} t0_us — client timestamp in µs from performance.now() * 1000
 * @returns {Uint8Array}
 */
export function buildClock(t0_us) {
  // Convert to u32 (wrap-safe)
  var t0 = (t0_us >>> 0);
  var buf = new Uint8Array(5);
  var dv = new DataView(buf.buffer);
  dv.setUint8(0, FRAME_CLOCK);
  dv.setUint32(1, t0, true);   // LE u32
  return buf;
}

/**
 * Build a 0x10 CMD frame (client→device).
 * u8 type=0x10, u16 id, u8 op, payload = JSON (UTF-8)
 *
 * @param {number} id — monotonic command id
 * @param {number} op — op code (OP_SET_WINDOW, etc.)
 * @param {string} jsonStr — JSON payload string (e.g. '{"min":0,"max":260}')
 * @returns {Uint8Array}
 */
export function buildCmd(id, op, jsonStr) {
  var jsonBytes = (typeof TextEncoder !== 'undefined')
    ? new TextEncoder().encode(jsonStr)
    : (function(s) { var b = new Uint8Array(s.length); for (var i = 0; i < s.length; i++) b[i] = s.charCodeAt(i); return b; })(jsonStr);

  var total = 4 + jsonBytes.length;  // 1 type + 2 id + 1 op = 4 header
  var buf = new Uint8Array(total);
  var dv = new DataView(buf.buffer);
  dv.setUint8(0, FRAME_CMD);
  dv.setUint16(1, id, true);   // LE u16
  dv.setUint8(3, op);
  buf.set(jsonBytes, 4);
  return buf;
}

/**
 * Identify the frame type from the first byte.
 * @param {DataView} dv
 * @returns {number} frame type byte, or -1 if unknown
 */
export function frameType(dv) {
  if (dv.byteLength < 1) return -1;
  var t = dv.getUint8(0);
  switch (t) {
    case FRAME_HELLO:     return FRAME_HELLO;
    case FRAME_TELEMETRY: return FRAME_TELEMETRY;
    case FRAME_STATUS:    return FRAME_STATUS;
    case FRAME_CLOCK:     return FRAME_CLOCK;
    case FRAME_INTERP:    return FRAME_INTERP;
    case FRAME_ANOMALY:   return FRAME_ANOMALY;
    // 0x06 STATS was missing from this switch, so link.js's FRAME_STATS
    // dispatch was unreachable and the SESSION card froze while live on WS —
    // it only ever updated via the HTTP fallback poll. :3
    case FRAME_STATS:     return FRAME_STATS;
    case FRAME_ECHO:      return FRAME_ECHO;
    default:              return -1;
  }
}

// ============================================================================
// Clock sync math (wrap-safe u32 deltas)
// ============================================================================

/**
 * Signed 32-bit delta between two u32 values (wrap-safe).
 * Works correctly when the counter wraps around 2^32.
 *   i32Delta(t1, t0) = signed interpretation of (t1 - t0) in 32-bit math
 *
 * @param {number} a — u32 value
 * @param {number} b — u32 value
 * @returns {number} signed delta (can be negative)
 */
export function i32Delta(a, b) {
  // |0 cast to signed 32-bit integer gives wrap-safe subtraction
  return ((a >>> 0) - (b >>> 0)) | 0;
}

/**
 * Compute clock offset and RTT from a clock-sync exchange.
 *
 *   t0 = client send time (µs, u32 wrap-safe)
 *   t1 = device receive time (µs, u32)
 *   t2 = device send time (µs, u32)
 *   t3 = client receive time (µs, u32)
 *
 *   offset_us = ((t1−t0) + (t2−t3)) / 2    (signed, wrap-safe u32 deltas)
 *   rtt_us    = (t3−t0) − (t2−t1)          (signed, wrap-safe u32 deltas)
 *
 * @param {number} t0_us
 * @param {number} t1_us
 * @param {number} t2_us
 * @param {number} t3_us
 * @returns {{offset_us:number, rtt_us:number}}
 */
export function clockCalc(t0_us, t1_us, t2_us, t3_us) {
  var d10 = i32Delta(t1_us, t0_us);
  var d32 = i32Delta(t2_us, t3_us);
  var offset_us = (d10 + d32) / 2;  // float division — offset can be fractional µs

  var d30 = i32Delta(t3_us, t0_us);
  var d21 = i32Delta(t2_us, t1_us);
  var rtt_us = d30 - d21;

  return { offset_us: offset_us, rtt_us: rtt_us };
}