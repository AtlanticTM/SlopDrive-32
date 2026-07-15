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
#define WS_FRAME_CMD   0x10   // client → device:  {type, id:u16, op:u8, payload:JSON}
#define WS_FRAME_ECHO  0x11   // device → client:  {type, id:u16, ok:u8, cfg_gen:u16, payload:JSON}

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

// ---- Idempotency -----------------------------------------------------------
// Per-client ring of last N seen (id, echo) pairs.
// Duplicate id → re-echo last result, do NOT re-apply the command.
#define WS_IDEMPOTENCY_RING 32