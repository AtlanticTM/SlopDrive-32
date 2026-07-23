// ServoMotionExecutor — StreamedSetpointExecutor implementation.
// Build-guarded behind DRIVER_AIM_SERVO && FEATURE_RS485_MODBUS.
//
// JERK-LIMITED TARGET TRACKER (see the header doctrine writeup). Every motion
// source moves `_target` (+ vmax/amax limits) via track(); this file's
// onTick() integrates a third-order-smooth (pos, vel, acc) state toward that
// target every servoBusTask tick (2ms) and streams the result as FC 0x10
// incremental deltas on the send cadence. OSSM-RS parity: their Ruckig
// S-curve feeds a 10ms 0x7B stream; our tracker feeds a 6-10ms delta stream.
// The profile math in MotionProfile.h is no longer sampled here — kept for
// reference/diagnostics. :3
#if defined(DRIVER_AIM_SERVO) && defined(FEATURE_RS485_MODBUS)

#include "ServoMotionExecutor.h"
#include "ServoModbus.h"
#include "config_api.h"
#include <cmath>

StreamedSetpointExecutor::StreamedSetpointExecutor(ServoModbus& bus) : _bus(bus) {}

// Legacy entry point — a couple of call sites may still hand us a planned
// profile; all we need from it is the destination + limits. The tracker does
// the rest (and does it smoother). :3
void StreamedSetpointExecutor::adoptProfile(const TrapezoidProfile& profile) {
    track(profile.target, profile.vmax, profile.accel);
}

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
    // not a move. :3
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

void StreamedSetpointExecutor::onTick(int64_t now_us) {
    // ---- Integration dt — real elapsed time, bounded. A scheduling hiccup
    // must never integrate a giant step (first tick after seed uses 0). :3
    float dt = 0.0f;
    if (_last_tick_us != 0) {
        dt = (float)(now_us - _last_tick_us) * 1.0e-6f;
        if (dt < 0.0f)    dt = 0.0f;
        if (dt > 0.05f)   dt = 0.05f;
    }
    _last_tick_us = now_us;

    portENTER_CRITICAL(&_mux);
    bool    seeded = _seeded;
    bool    frozen = _frozen;
    float   target = _target;
    float   vmax   = _vmax;
    float   amax   = _amax;
    float   jmax   = _jmax;
    float   p      = _cmd_pos;
    float   v      = _cmd_vel;
    float   a      = _trk_acc;
    int32_t offset = _wire_offset;
    int8_t  sign   = _wire_sign;
    portEXIT_CRITICAL(&_mux);

    // Nothing is EVER sent before the driver seeds us with a live encoder
    // reading — the hard safety requirement from plan.md. :3
    if (!seeded) return;

    bool moving = false;
    if (!frozen && dt > 0.0f) {
        // ---- Jerk-limited tracking step (ruckig-lite) ----------------------
        // 1. Desired velocity: braking-curve toward the target, capped at
        //    vmax. The 0.85 accel margin keeps the sqrt curve conservative so
        //    the jerk-limited accel response can always land without ringing.
        // 2. Desired accel: reach v_des within this tick, capped at amax.
        // 3. Jerk limit: slew the actual accel toward desired at jmax.
        // 4. Integrate. Snap when both error and speed are sub-count. :3
        float d    = target - p;
        float dir  = (d >= 0.0f) ? 1.0f : -1.0f;
        float dmag = fabsf(d);

        if (dmag < 2.0f && fabsf(v) < 4.0f) {
            p = target; v = 0.0f; a = 0.0f;      // parked
        } else {
            float v_stop = sqrtf(2.0f * 0.85f * amax * dmag);
            float v_des  = dir * ((v_stop < vmax) ? v_stop : vmax);

            float a_des  = (v_des - v) / dt;
            if (a_des >  amax) a_des =  amax;
            if (a_des < -amax) a_des = -amax;

            float da_max = jmax * dt;
            float da     = a_des - a;
            if (da >  da_max) da =  da_max;
            if (da < -da_max) da = -da_max;
            a += da;

            v += a * dt;
            // vmax is a hard ceiling regardless of what the accel ramp did.
            if (v >  vmax) v =  vmax;
            if (v < -vmax) v = -vmax;
            p += v * dt;
            moving = true;
        }
    }

    portENTER_CRITICAL(&_mux);
    _cmd_pos = p;
    _cmd_vel = v;
    _trk_acc = a;
    _active  = moving;
    portEXIT_CRITICAL(&_mux);

    // ---- DELTA WIRE PROTOCOL (bench-proven fw 2.1.26) ----------------------
    // Incremental FC 0x10 pair writes; _last_sent_wire is the accumulated
    // commanded wire position, advanced only on an ACCEPTED send. First send
    // after seed() zeroes the delta against the seed offset (the "send
    // current position, observe zero motion" invariant). Zero deltas still go
    // out on the keep-alive cadence as liveness probes. Lost-echo-but-
    // executed = model desync until re-home — bounded by the 3-miss freeze +
    // EncoderValidator. :3
    int32_t  wire      = offset + (int32_t)sign * (int32_t)lroundf(p);
    uint32_t now_ms    = (uint32_t)(now_us / 1000);
    if (!_have_sent) _last_sent_wire = offset;
    int32_t  delta     = wire - _last_sent_wire;
    bool     new_value = !_have_sent || (delta != 0);
    uint32_t period_ms = new_value ? _bus.spPeriodMs() : AIM_SP_KEEPALIVE_MS;
    if (now_ms - _last_sent_ms < period_ms) return;

    if (_bus.sendPositionDelta(delta)) {
        _last_sent_ms   = now_ms;
        _last_sent_wire = wire;
        _have_sent      = true;
    }
}

#endif // defined(DRIVER_AIM_SERVO) && defined(FEATURE_RS485_MODBUS)
