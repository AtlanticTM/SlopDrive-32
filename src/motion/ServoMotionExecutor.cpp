// ServoMotionExecutor — StreamedSetpointExecutor implementation
//
// Constraints:
//   Build-guarded behind DRIVER_AIM_SERVO && FEATURE_RS485_MODBUS. Every
//   motion source moves `_target` (+ vmax/amax limits) via track(); onTick()
//   integrates a third-order-smooth (pos, vel, acc) state toward that target
//   every servoBusTask tick (2ms) and streams the result as FC 0x7B ABSOLUTE
//   setpoints on the send cadence. Absolute is load-bearing, not a detail:
//   see docs/reversal-drift.md.
//
// See:
//   ServoMotionExecutor.h — the tracker design doctrine
#if defined(DRIVER_AIM_SERVO) && defined(FEATURE_RS485_MODBUS)

#include "ServoMotionExecutor.h"
#include "ServoModbus.h"
#include "config_api.h"
#include <cmath>

StreamedSetpointExecutor::StreamedSetpointExecutor(ServoModbus& bus) : _bus(bus) {}

// The concrete class's own public entry point — ModbusServoDriver calls it
// directly (not via the IServoExecutor vtable, which never declared it) on
// every streamToSteps() dispatch.
void StreamedSetpointExecutor::track(float target_counts,
                                     float vmax_counts_s,
                                     float amax_counts_s2) {
    if (vmax_counts_s  < 1.0f) vmax_counts_s  = 1.0f;
    if (amax_counts_s2 < 1.0f) amax_counts_s2 = 1.0f;
    portENTER_CRITICAL(&_mux);
    _target = target_counts;
    _vmax   = vmax_counts_s;
    _amax   = amax_counts_s2;
    _frozen = false;
    portEXIT_CRITICAL(&_mux);
}

void StreamedSetpointExecutor::setJerkLimit(float jmax_counts_s3) {
    if (jmax_counts_s3 < 1.0f) jmax_counts_s3 = 1.0f;
    portENTER_CRITICAL(&_mux);
    _jmax = jmax_counts_s3;
    portEXIT_CRITICAL(&_mux);
}

void StreamedSetpointExecutor::freeze() {
    portENTER_CRITICAL(&_mux);
    // Hold RIGHT HERE: target snaps to the current tracker position and the
    // kinematic state zeroes. Deliberately jerk-UNLIMITED — freeze is a stop,
    // not a move.
    _target  = _cmd_pos;
    _cmd_vel = 0.0f;
    _trk_acc = 0.0f;
    _frozen  = true;
    _active  = false;
    portEXIT_CRITICAL(&_mux);
}

void StreamedSetpointExecutor::seed(float cmd_pos) {
    portENTER_CRITICAL(&_mux);
    _cmd_pos = cmd_pos;
    _cmd_vel = 0.0f;
    _trk_acc = 0.0f;
    _target  = cmd_pos;
    _frozen  = false;
    _active  = false;
    _seeded  = true;
    portEXIT_CRITICAL(&_mux);
    _have_sent    = false;
    _last_tick_us = 0;
}

void StreamedSetpointExecutor::unseed() {
    portENTER_CRITICAL(&_mux);
    _seeded = false;
    _active = false;
    _cmd_vel = 0.0f;
    _trk_acc = 0.0f;
    portEXIT_CRITICAL(&_mux);
    _have_sent = false;
}

void StreamedSetpointExecutor::setWireMap(int32_t offset, int8_t sign) {
    portENTER_CRITICAL(&_mux);
    _wire_offset = offset;
    _wire_sign   = sign;
    portEXIT_CRITICAL(&_mux);
}

float StreamedSetpointExecutor::commandedPos() const {
    portENTER_CRITICAL(&_mux);
    float v = _cmd_pos;
    portEXIT_CRITICAL(&_mux);
    return v;
}

float StreamedSetpointExecutor::commandedVel() const {
    portENTER_CRITICAL(&_mux);
    float v = _cmd_vel;
    portEXIT_CRITICAL(&_mux);
    return v;
}

float StreamedSetpointExecutor::liveAccel() const {
    portENTER_CRITICAL(&_mux);
    float a = _amax;
    portEXIT_CRITICAL(&_mux);
    return a;
}

bool StreamedSetpointExecutor::active() const {
    portENTER_CRITICAL(&_mux);
    bool a = _active;
    portEXIT_CRITICAL(&_mux);
    return a;
}

// The drive is ALREADY a closed-loop servo: it has the encoder, the position
// loop, and its own profiling to any absolute setpoint. Running a second
// controller in front of it cascades two loops chasing each other, which is
// what produced visible oscillation (90 direction reversals in one move) and a
// standing offset from the commanded target. slopmotion has already produced a
// jerk-limited trajectory upstream; this stage just maps it to wire counts and
// paces it. Smoothing belongs in the stream, never here. See sd-s37.
void StreamedSetpointExecutor::onTick(int64_t now_us) {
    _last_tick_us = now_us;

    portENTER_CRITICAL(&_mux);
    bool    seeded = _seeded;
    bool    frozen = _frozen;
    float   target = _target;
    float   prev   = _cmd_pos;
    int32_t offset = _wire_offset;
    int8_t  sign   = _wire_sign;
    portEXIT_CRITICAL(&_mux);

    // Nothing is EVER sent before the driver seeds us with a live encoder
    // reading: never command motion before a live encoder seed exists.
    if (!seeded) return;

    // freeze() holds the last sample by pinning the target to it, so a frozen
    // executor simply re-states that position on the keep-alive cadence.
    float p = frozen ? prev : target;

    // "Moving" is now a property of the STREAM, not of an internal model:
    // the commanded position changed since the last tick.
    bool moving = fabsf(p - prev) > 0.5f;

    portENTER_CRITICAL(&_mux);
    _cmd_pos = p;
    _active  = moving;
    portEXIT_CRITICAL(&_mux);

    // ---- Absolute wire protocol ---------------------------------------------
    // Every frame states where the shaft SHOULD BE, in the drive's own encoder
    // frame. This is the whole point: a dropped frame, or a move the shaft did
    // not finish, is corrected by the next frame instead of banked forever the
    // way an incremental command banks it. There is no accumulator here to
    // desync, so a lost frame costs latency and never position.
    int32_t  wire      = offset + (int32_t)sign * (int32_t)lroundf(p);
    uint32_t now_ms    = (uint32_t)(now_us / 1000);
    bool     new_value = !_have_sent || (wire != _last_sent_wire);
    uint32_t period_ms = new_value ? _bus.spPeriodMs() : AIM_SP_KEEPALIVE_MS;
    if (now_ms - _last_sent_ms < period_ms) return;

    if (_bus.sendSetpoint(wire)) {
        _last_sent_ms   = now_ms;
        _last_sent_wire = wire;
        _have_sent      = true;
    }
}

#endif // defined(DRIVER_AIM_SERVO) && defined(FEATURE_RS485_MODBUS)
