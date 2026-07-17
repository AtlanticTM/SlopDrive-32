#pragma once

// ============================================================================
// UiProtocol — shared op-code definitions for the 0x10 CMD / 0x11 ECHO binary
// WebSocket control plane (Appendix C of SD32-overhaul-plan.md).
//
// Consumed by:
//   - Firmware: src/ui/WebUI.cpp (dispatch) + src/ui/UiSocket.cpp (idempotency)
//   - Web UI:    webui/src/core/wire.js (frame codecs, copy of these constants)
//
// All values are uint8_t.  Ranges 0x01–0x11, gaps reserved for future ops. :3
// ============================================================================

// ---- Frame type markers ---------------------------------------------------
// Device → client telemetry/status frames (binary, LE):
//   0x00 HELLO   — proto_ver + cfg_gen
//   0x01 TELE    — batched position/target/raw samples + current
//   0x02 STATUS  — bus/thermal/wifi/heap/cfg (~2Hz)
//   0x03 CLOCK   — round-trip time-sync reply
//   0x04 INTERP  — v0.4 interpolator debug snapshot (~45Hz), see below
//   0x05 ANOMALY — interpolator anomaly event (event-driven), see below
#define WS_FRAME_INTERP  0x04   // device → client:  interpolator planned-path snapshot
#define WS_FRAME_ANOMALY 0x05   // device → client:  interpolator anomaly event
#define WS_FRAME_CMD   0x10   // client → device:  {type, id:u16, op:u8, payload:JSON}
#define WS_FRAME_ECHO  0x11   // device → client:  {type, id:u16, ok:u8, cfg_gen:u16, payload:JSON}

// ---- 0x04 INTERP frame layout (device → client, ~45Hz) --------------------
// Little-endian. Positions are normalized 0..1 encoded as u16 (val*10000).
// Velocity is signed units/second encoded as i16 (val*1000, clamped ±32.767/s).
//   off  type  field
//   0    u8    0x04
//   1    u8    flags: b0 active, b1 liveMode, b2 gradMode
//   2    u8    style (InterpStyle enum)
//   3    u16   startPos   (norm *10000)
//   5    u16   endPos     (norm *10000)
//   7    u16   curPos     (norm *10000)
//   9    i16   curVel     (units/s *1000, signed, clamped)
//   11   u32   durationUs
//   15   u32   elapsedUs
// Total: 19 bytes.
#define INTERP_FRAME_SIZE 19

// ---- 0x05 ANOMALY frame layout (device → client, event-driven) ------------
// Emitted whenever the interpolator records a suspect event (overshoot on a
// steep G tangent, a dropped v3 point, a starved decel-overrun, or a duration
// fallback). Captures the INPUT that caused it so the funscript point behind a
// stutter/non-move can be pinned exactly. Little-endian.
//   off  type  field
//   0    u8    0x05
//   1    u8    kind        (InterpAnomalyType: 1=Overshoot 2=PointDropped
//                                              3=DecelOverrun 4=DurFallback)
//   2    u16   seq         (rolling event id, wraps)
//   4    u32   tDevUs      (device esp_timer us, low 32 bits)
//   8    u16   targetPos   (norm *10000)
//   10   u16   startPos    (norm *10000)
//   12   f32   startVel    (units/second, signed — raw IEEE754 LE)
//   16   f32   endSlope    (raw MFP G<slope> wire value — raw IEEE754 LE)
//   20   u32   durationUs  (planned segment duration)
//   24   f32   extra       (kind-specific: overshoot frac / gap_us / vEnd)
// Total: 28 bytes.
#define ANOMALY_FRAME_SIZE 28

// ---- 0x10 CMD op codes -----------------------------------------------------
#define WS_OP_SET_WINDOW   0x01   // {min, max, no_persist?}
#define WS_OP_SET_SPEED    0x02   // {mm_s}
#define WS_OP_SET_ACCEL    0x03   // {mm_s2}
#define WS_OP_GEN_CFG      0x04   // {speed, depth, stroke, sensation, pattern, rate_tick}
#define WS_OP_GEN_RUN      0x05   // {run:bool}
#define WS_OP_MODE         0x06   // {transport}  "WS"|"SER"|"BT"|"DONGLE"|"OSSM"
#define WS_OP_BLEND        0x07   // {bm}  1=let-it-land, 2=allow-reversal, 3=hybrid
#define WS_OP_PAUSE        0x08   // {paused:bool}
#define WS_OP_HALT         0x09   // (no payload)  motor hard-stop, stays homed
#define WS_OP_ESTOP        0x0A   // (no payload)  full estop — same path as /api/stop
#define WS_OP_HOME         0x0B   // (no payload)  begin sensorless homing
#define WS_OP_OVERRIDE     0x0C   // {on:bool}  manual override
#define WS_OP_BYPASS       0x0D   // {on:bool}  bypass stroke-window limits on moves
#define WS_OP_CLEAR_FAULT  0x0E   // (no payload)  clear driver fault
#define WS_OP_SAVE         0x0F   // (no payload)  persist config to NVS
#define WS_OP_GET_CFG      0x10   // (no payload)  full config snapshot (same as /api/settings GET)
#define WS_OP_MOVE         0x11   // {position:float, stream:bool, bypass_limits:bool, speed?:float}
#define WS_OP_STREAM_MODE  0x12   // {mode:u8}  0=ceiling-pegged, 1=velocity-matched (stream speed-feed A/B)
#define WS_OP_OVERSHOOT    0x13   // {on:bool}  monotone (Fritsch-Carlson) tangent clamp on v4 gradient cubic

// ---- Idempotency -----------------------------------------------------------
// Per-client ring of last N seen (id, echo) pairs.
// Duplicate id → re-echo last result, do NOT re-apply the command.
#define WS_IDEMPOTENCY_RING 32