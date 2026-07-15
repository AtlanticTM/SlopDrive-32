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

// ---- Flag bit positions for 0x01 ------------------------------------------
export const FLAG_HOMED           = 0x01;
export const FLAG_FAULT           = 0x02;
export const FLAG_GEN_RUNNING     = 0x04;
export const FLAG_ESTOP           = 0x08;
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