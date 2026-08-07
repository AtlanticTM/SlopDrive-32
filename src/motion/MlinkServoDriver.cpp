// MlinkServoDriver -- MotorDriver over the RP2350 SPI motion link.
// Constraints:
// - motorTask (Core 1) only. update() is the single send point; commands set
//   flags that the next tick ships. The 10 ms tick is the command latency.
// - Frame trust = CRC both directions; a dropped retarget heals via the
//   100 ms idempotent refresh, estop/clear by repetition until the echoed
//   state confirms.
// - >=60 us between transactions: the slave block-resets its SPI per frame.
// See: include/motion/MlinkServoDriver.h, dev board sd-dxy.

#include "motion/MlinkServoDriver.h"

#include <Arduino.h>
#include <SPI.h>
#include <cstring>

#include "system/config_api.h"
#include "system/MotionPassthrough.h"
#include "sloplog/sloplog.h"

using namespace motionlink;

// Drop-in pinout (docs/rp2350-wiring.md): the GPIO matrix remaps roles.
namespace {
constexpr int8_t kSck = 7, kMiso = 10, kMosi = 38, kCs = 48, kIrq = 4;
SPIClass s_spi(FSPI);
constexpr uint32_t kTickMs = 10;
constexpr uint32_t kRefreshMs = 100;
constexpr uint32_t kStreamGapMs = 100;   // stream silence before re-anchoring
}  // namespace

void MlinkServoDriver::xfer(uint8_t (&out)[kFrameBytes],
                            uint8_t (&in)[kFrameBytes]) {
    static_assert(kSpiMode == 1, "PL022 slave needs CPHA=1");
    static uint32_t s_lastEndUs = 0;
    const uint32_t sinceUs = micros() - s_lastEndUs;
    if (sinceUs < 60) delayMicroseconds(60 - sinceUs);
    crcStamp(out);
    s_spi.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE1));
    digitalWrite(kCs, LOW);
    s_spi.transferBytes(out, in, kFrameBytes);
    digitalWrite(kCs, HIGH);
    s_spi.endTransaction();
    s_lastEndUs = micros();
}

void MlinkServoDriver::sendOp(uint8_t op) {
    uint8_t out[kFrameBytes] = {op, ++_seq};
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
}

void MlinkServoDriver::sendRetarget() {
    uint8_t out[kFrameBytes] = {kOpRetarget, ++_seq};
    memcpy(&out[2], &_rt_target, 4);
    memcpy(&out[6], &_rt_v, 4);
    memcpy(&out[10], &_rt_a, 4);
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    _rt_dirty = false;
    _last_cmd_ms = millis();
}

void MlinkServoDriver::sendSegment() {
    // Hermite chunk covering [chain, sample]: the slave renders it over its
    // wire duration, which is what preserves the stream's timeline.
    uint8_t out[kFrameBytes] = {kOpSegment, ++_seq};
    const uint32_t dur_us = (_samp_ms - _chain_ms) * 1000u;
    memcpy(&out[2], &dur_us, 4);
    memcpy(&out[6], &_chain_p, 4);
    memcpy(&out[10], &_chain_v, 4);
    memcpy(&out[14], &_samp_p, 4);
    memcpy(&out[18], &_samp_v, 4);
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    // Keep the exact frame for loss recovery: same seq on resend, so the
    // slave's dedup can drop the copy when only the ack was lost.
    memcpy(_seg_frame, out, kFrameBytes);
    _seg_seq = _seq;
    _seg_unacked = true;
    _chain_p = _samp_p;
    _chain_v = _samp_v;
    _chain_ms = _samp_ms;
    _last_cmd_ms = millis();
}

void MlinkServoDriver::init() {
    pinMode(kCs, OUTPUT);
    digitalWrite(kCs, HIGH);
    pinMode(kIrq, INPUT_PULLDOWN);
    s_spi.begin(kSck, kMiso, kMosi, -1);
    // The machine's steps/rev model must mirror the drive's saved gear
    // (32768/4 = 8192 counts/rev). TODO(sd-dnz): adopt from the drive.
    if (aimMotorStepsPerRev() != 8192) aimSetMotorStepsPerRev(8192, true);
    motionPassthroughEnable();
    _begun = true;
    SLOGI("mlink", "MlinkServoDriver up: RP2350 quadrature backend, "
          "%.1f counts/mm, speed cap %.0f counts/s",
          (double)AIM_STEPS_PER_MM, (double)kMaxCountsPerSec);
}

void MlinkServoDriver::update() {
    if (!_begun) return;
    const uint32_t now = millis();
    if (now - _last_tick_ms < kTickMs) return;
    _last_tick_ms = now;

    // Status poll; every reply is CRC-gated before anything trusts it.
    uint8_t out[kFrameBytes] = {kOpPing, ++_seq};
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    const bool sane = crcOk(in);
    if (sane) {
        _state = in[0];
        // Rising-edge fault surfacing: JUMPED means the renderer teleported
        // its reference (physical position now differs from calculated until
        // re-home -- the encoder delta names the size); OVERFLOW is a credit-
        // gate bug; UNDERRUN mid-stream is starvation (normal at stream end).
        const uint8_t rising = uint8_t(in[1] & uint8_t(~_slave_flags));
        if (rising & kFlagJumped)
            SLOGW("mlink", "RP JUMPED: renderer teleported its reference -- "
                  "calculated vs physical diverged, re-home to reconcile");
        if (rising & kFlagOverflow)
            SLOGW("mlink", "RP segment ring OVERFLOW: credit gate failed, a curve chunk was dropped");
        if (rising & kFlagUnderran)
            SLOGI_EVERY_MS(5000, "mlink", "RP underran -> SETTLE (expected at stream "
                           "end; mid-stream = ring starved, each one adds latency)");
        _slave_flags = in[1];
        _runway_ms = uint16_t(in[2]) | uint16_t(uint16_t(in[3]) << 8);
        _depth = in[4];
        _seq_echo = in[5];
        memcpy(&_pos_counts, &in[6], 4);
        memcpy(&_vel_counts, &in[10], 4);
        _status_ms = now;
        _status_fresh = true;
    }

    if (_estop_pending) {
        if (!sane || _state != kStateEstop) sendOp(kOpEstop);
        else _estop_pending = false;
        return;                       // nothing else while stopping
    }
    if (_clear_pending) {
        if (sane && _state == kStateEstop) sendOp(kOpClear);
        else if (sane) _clear_pending = false;
        return;
    }

    if (_seg_mode) {
        // This tick's ping reply was preloaded after the slave processed last
        // tick's FINAL frame, so a landed segment echoes its seq here. On a
        // miss, resend the SAME frame: the slave dedups by segment seq, so a
        // lost-ack duplicate is dropped. (After ~256 straight losses the ping
        // seq wraps onto the segment's; the link is long dead before that.)
        if (_seg_unacked) {
            if (sane && _seq_echo == _seg_seq) {
                _seg_unacked = false;
            } else {
                uint8_t rein[kFrameBytes] = {};
                xfer(_seg_frame, rein);
                return;
            }
        }
        // Settled mid-stream = playback slipped behind (underrun). Re-anchor
        // at the settled point and NOW so slip is dropped, never compounded;
        // the next segment sweeps the gap in one tick.
        if (sane && _state == kStateSettled && !_seg_unacked &&
            _samp_ms != _chain_ms) {
            _chain_p = _pos_counts;
            _chain_v = 0.0f;
            _chain_ms = (_samp_ms > kTickMs) ? _samp_ms - kTickMs : _samp_ms;
        }
        // Reported runway is one tick STALE (reply preloaded last tick), so
        // gate at target+tick: the bare target shipped into an already-dry
        // ring (measured mid-stream underruns 2026-08-07).
        if (sane && _samp_ms != _chain_ms && _depth < kSegmentDepth &&
            _runway_ms < kRunwayTargetMs + kTickMs)
            sendSegment();
        return;                       // segment mode never refreshes retargets
    }

    if (_rt_valid && (_rt_dirty || now - _last_cmd_ms >= kRefreshMs))
        sendRetarget();
}

void MlinkServoDriver::emergencyStop() {
    _estop_pending = true;
    _rt_valid = false;
    _seg_mode = false;
    _seg_unacked = false;
    if (_begun) sendOp(kOpEstop);     // motorTask context: safe, immediate
}

void MlinkServoDriver::enable() {
    // Leaving an estop hold needs the explicit clear; harmless otherwise.
    if (_state == kStateEstop) _clear_pending = true;
}

bool MlinkServoDriver::home(int32_t) {
    SLOGW("mlink", "homing not implemented on the mlink backend yet -- "
          "use HOME_OVERRIDE (sd-dxy owns real homing)");
    return false;
}

void MlinkServoDriver::streamToSteps(int32_t target_steps,
                                     uint32_t speed_steps_s,
                                     uint32_t accel_steps_s2) {
    if (!_homed) return;
    float v = float(speed_steps_s);
    if (v > kMaxCountsPerSec) v = kMaxCountsPerSec;
    if (v < 1.0f) v = 1.0f;
    float a = float(accel_steps_s2);
    if (a < 1.0f) a = 1.0f;
    _seg_mode = false;                // point move: retarget reclaims the wire
    _seg_unacked = false;
    _rt_target = float(target_steps);
    _rt_v = v;
    _rt_a = a;
    _last_accel_native = accel_steps_s2;
    _rt_valid = true;
    _rt_dirty = true;
}

void MlinkServoDriver::streamSample(int32_t target_steps, float vel_steps_s,
                                    uint32_t /*speed_steps_s*/,
                                    uint32_t accel_steps_s2) {
    // Curve chase rides kOpSegment: the slave renders Hermite chunks over
    // their real durations, so the stream's own timeline IS the speed. The
    // ceiling params are already baked into the sampled curve upstream
    // (slopmotion Config); feeding them to a land-at-v=0 retarget instead is
    // the sd-ar3 sprint-and-stop failure.
    if (!_homed) return;
    float v = vel_steps_s;
    if (v >  kMaxCountsPerSec) v =  kMaxCountsPerSec;
    if (v < -kMaxCountsPerSec) v = -kMaxCountsPerSec;
    const uint32_t now = millis();
    // (Re-)anchor at the live rendered position on entry or after a stream
    // gap; a stale chain tail would ship one giant segment spanning the idle.
    // Store order matters: update() may preempt between statements (same
    // core), so _seg_mode flips true only after the chain is coherent.
    if (!_seg_mode || now - _samp_ms > kStreamGapMs) {
        _rt_valid = false;
        _rt_dirty = false;
        _seg_unacked = false;
        _chain_p = _pos_counts;
        _chain_v = 0.0f;
        _chain_ms = now;
        _samp_ms = now;
        _seg_mode = true;
    }
    _samp_p = float(target_steps);
    _samp_v = v;
    _samp_ms = now;
    _last_accel_native = accel_steps_s2;
}

void MlinkServoDriver::stop() {
    // Full stop clears homed (interface contract). Land where we are.
    _homed = false;
    _seg_mode = false;
    _seg_unacked = false;
    _rt_target = _pos_counts;
    _rt_v = kMaxCountsPerSec;
    if (_rt_a < 1.0f) _rt_a = 100000.0f;
    _rt_valid = true;
    _rt_dirty = true;
}

void MlinkServoDriver::hardStop() {
    _seg_mode = false;
    _seg_unacked = false;
    _rt_target = _pos_counts;
    _rt_v = kMaxCountsPerSec;
    if (_rt_a < 1.0f) _rt_a = 100000.0f;
    _rt_valid = true;
    _rt_dirty = true;
}

float MlinkServoDriver::getPosition() const {
    // Native frame is NEGATED vs mm (endstop 0, front negative), same as
    // every driver: report nativeToMm(-native), or the arbiter plans every
    // move from a mirror-image p0 (the sd-ar3 phantom-distance bug).
    // Dead-reckon the one-tick-stale status by reported velocity: the raw
    // 100 Hz staircase beats vs the ~20 ms 0x0080 cadence (trail zigzag).
    // Capped so a dead link freezes; v=0 at rest keeps standstill raw.
    uint32_t age = millis() - _status_ms + kTickMs;
    if (age > 3 * kTickMs) age = 3 * kTickMs;
    return -(_pos_counts + _vel_counts * (float(age) * 1e-3f)) / AIM_STEPS_PER_MM;
}

float MlinkServoDriver::getTargetPosition() const {
    const float t = _seg_mode ? _samp_p : (_rt_valid ? _rt_target : _pos_counts);
    return -t / AIM_STEPS_PER_MM;
}

int32_t MlinkServoDriver::mmToNative(float mm) const {
    return (int32_t)(mm * AIM_STEPS_PER_MM);
}

float MlinkServoDriver::nativeToMm(int32_t native) const {
    return (float)native / AIM_STEPS_PER_MM;
}
