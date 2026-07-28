#pragma once

// ============================================================================
// UiProtocol — WS_OP_* op codes for WebUI::handleCommand(), the single
// machine-control entry point. Reached via the SlopSync INTENT delegate
// (src/comms/SlopSyncHubService.cpp). All values uint8_t; gaps reserved.
// Released numbers are never reused (CANON / registry discipline).
// ============================================================================

#define WS_OP_SET_WINDOW   0x01   // {min, max, no_persist?}
#define WS_OP_SET_SPEED    0x02   // {mm_s}
#define WS_OP_SET_ACCEL    0x03   // {mm_s2}
#define WS_OP_GEN_CFG      0x04   // {speed, depth, stroke, sensation, pattern, rate_tick}
                                  // + Advanced mode (fray-d port), all optional/additive:
                                  //   {ap_mode:bool, ap_speed, ap_min_depth, ap_max_depth,
                                  //    ap_in_speed, ap_out_speed, ap_in_accel, ap_out_accel,
                                  //    ap_mod:{ctrl:0..5, amplitude, in_step, in_wait,
                                  //            out_step, out_wait, offset},
                                  //    ap_reset:bool (fray-d baseline; applied FIRST),
                                  //    ap_mods:[<ap_mod objects>] (preset apply)}
#define WS_OP_GEN_RUN      0x05   // {run:bool}
// 0x06 = RETIRED op (transport selector) — number reserved, never reuse
#define WS_OP_BLEND        0x07   // {bm}  1=let-it-land, 2=allow-reversal, 3=hybrid
#define WS_OP_PAUSE        0x08   // {paused:bool}
#define WS_OP_HALT         0x09   // (no payload)  motor hard-stop, stays homed
#define WS_OP_ESTOP        0x0A   // (no payload)  full estop — same path as SlopSync 0x0005 op=stop
#define WS_OP_HOME         0x0B   // (no payload)  begin sensorless homing
#define WS_OP_OVERRIDE     0x0C   // {on:bool}  manual override
#define WS_OP_BYPASS       0x0D   // {on:bool}  bypass stroke-window limits on moves
#define WS_OP_CLEAR_FAULT  0x0E   // (no payload)  clear driver fault
#define WS_OP_SAVE         0x0F   // (no payload)  persist config to NVS
#define WS_OP_GET_CFG      0x10   // (no payload)  full config snapshot (same as /api/settings GET)
#define WS_OP_MOVE         0x11   // {position:float, stream:bool, bypass_limits:bool, speed?:float}
#define WS_OP_STREAM_MODE  0x12   // {mode:u8}  0=ceiling-pegged, 1=velocity-matched (stream speed-feed A/B)
#define WS_OP_OVERSHOOT    0x13   // {on:bool}  monotone (Fritsch-Carlson) tangent clamp on segment shaping
#define WS_OP_HOME_OVERRIDE 0x14  // {on:bool, stroke?:float}  TEST/bench: fake-home without a motor.
                                  // on:true  → force homed, report stroke (default 250mm) so the UI populates.
                                  // on:false → clear the override (back to real motor homing).
