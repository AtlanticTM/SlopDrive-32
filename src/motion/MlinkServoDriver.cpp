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
#include <Wire.h>
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
    // 200 us gap: the slave's 20 kHz tick (50 us period) ALWAYS fires inside
    // any inter-frame gap, and at 60 us a heavy render tick plus the per-frame
    // SPI re-arm did not reliably fit before the next frame's clocks arrived
    // (measured ~1 torn frame per 4-8 s under motor load, 2026-08-08).
    if (sinceUs < 200) delayMicroseconds(200 - sinceUs);
    crcStamp(out);
    s_spi.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE1));
    // Scheduler lock for the ~40 us transaction: the sampler (prio 4, same
    // core) otherwise preempts mid-frame -- CS low, clock frozen -- and the
    // slave's IRQ spin bails at ~300 us of silence, tearing the frame. Worst
    // during slopmotion commit() (ms-scale Ruckig planning), which is why
    // tears landed exactly on command boundaries. ISRs stay enabled.
    vTaskSuspendAll();
    digitalWrite(kCs, LOW);
    s_spi.transferBytes(out, in, kFrameBytes);
    digitalWrite(kCs, HIGH);
    xTaskResumeAll();
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
    _rt_seq = _seq;
    _rt_unacked = true;
    _rt_dirty = false;
    _last_cmd_ms = millis();
}

void MlinkServoDriver::sendSegmentTo(float p1, float v1, uint32_t t1_ms) {
    // Hermite chunk covering [chain, (p1,v1,t1)]: the slave renders it over
    // its wire duration, which is what preserves the stream's timeline.
    uint32_t dur_ms = t1_ms - _chain_ms;
    // Catch-up sweep after a re-anchor: stretch to the arbiter's ceiling so
    // the gap GLIDES closed instead of shooting at the render cap (the
    // ungoverned lunge cost 28.8 mm of drive-follow sync, 2026-08-08). The
    // extra render time lands as transient runway; the gate drains it.
    if (_sweep_pending) {
        _sweep_pending = false;
        if (_samp_vcap > 1.0f) {
            const float need_ms = fabsf(p1 - _chain_p) / _samp_vcap * 1000.0f;
            if (need_ms > float(dur_ms)) dur_ms = uint32_t(need_ms);
        }
    }
    uint8_t out[kFrameBytes] = {kOpSegment, ++_seq};
    const uint32_t dur_us = dur_ms * 1000u;
    memcpy(&out[2], &dur_us, 4);
    memcpy(&out[6], &_chain_p, 4);
    memcpy(&out[10], &_chain_v, 4);
    memcpy(&out[14], &p1, 4);
    memcpy(&out[18], &v1, 4);
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    // Keep the exact frame for loss recovery: same seq on resend, so the
    // slave's dedup can drop the copy when only the ack was lost. Tracks the
    // LAST frame of a tick; a torn first-of-pair costs a half-chunk slew.
    memcpy(_seg_frame, out, kFrameBytes);
    _seg_seq = _seq;
    _seg_unacked = true;
    _chain_p = p1;
    _chain_v = v1;
    _chain_ms = t1_ms;
    _last_cmd_ms = millis();
}

void MlinkServoDriver::sendSegment() {
    sendSegmentTo(_hold_p, _hold_v, _hold_ms);
}

void MlinkServoDriver::sendSegmentSplit() {
    // Two halves of the same cubic, evaluated at u=0.5, so an empty ring is
    // primed to depth 2 in one tick -- production is real-time-capped, so
    // steady one-per-tick shipping can never deepen the ring by itself.
    const float Ts = float(_hold_ms - _chain_ms) * 1e-3f;
    const float p0 = _chain_p, v0 = _chain_v;
    const float p1 = _hold_p, v1 = _hold_v;
    const float mid_p = 0.5f * (p0 + p1) + 0.125f * Ts * (v0 - v1);
    const float mid_v = 1.5f * (p1 - p0) / Ts - 0.25f * (v0 + v1);
    const uint32_t mid_ms = _chain_ms + (_hold_ms - _chain_ms) / 2u;
    sendSegmentTo(mid_p, mid_v, mid_ms);
    sendSegmentTo(_hold_p, _hold_v, _hold_ms);
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
        // Underrun settle: re-anchor one tick BEHIND the hold, dropping slip.
        // Never ahead: chain past hold underflows the u32 duration (the
        // 71-minute wedge segment, 2026-08-07). Only while samples ADVANCE:
        // a settled stream end otherwise loops hold-segments forever.
        if (sane && _state == kStateSettled && !_seg_unacked &&
            _samp_ms != _hold_ms) {
            _chain_p = _pos_counts;
            _chain_v = 0.0f;
            _chain_ms = (_hold_ms > kTickMs) ? _hold_ms - kTickMs : 0;
            _sweep_pending = true;
        }
        // Blocked-interval re-base; unsigned compare also catches any
        // chain-ahead-of-hold ordering bug as a huge gap.
        if (_hold_ms - _chain_ms > kStreamGapMs) {
            _chain_p = _pos_counts;
            _chain_v = 0.0f;
            _chain_ms = (_hold_ms > kTickMs) ? _hold_ms - kTickMs : 0;
            _sweep_pending = true;
        }
        // Gate compensates the one-tick-stale runway report. Split ONLY at
        // depth 0: sustained multi-frame ticks exceed the slave's per-frame-
        // reset budget (2.4.87: torn 83k, qdrops 607); deeper waits on sd-dxy.
        const int32_t span_ms = int32_t(_hold_ms - _chain_ms);
        if (sane && span_ms > 0 && _depth < kSegmentDepth &&
            _runway_ms < kRunwayTargetMs + 2 * kTickMs) {
            // A governed sweep never splits: it is one stretched glide.
            if (_depth == 0 && span_ms >= 4 && !_sweep_pending)
                sendSegmentSplit();
            else sendSegment();
        }
        // Holdback advances AFTER the ship attempt: a fresh chunk always
        // exists to ship into a draining ring next tick.
        _hold_p = _samp_p;
        _hold_v = _samp_v;
        _hold_ms = _samp_ms;
        return;                       // segment mode never refreshes retargets
    }

    if (_rt_valid) {
        // Retargets are idempotent/last-wins, so a resend needs no dedup: on
        // a missed seq echo just send again (fresh seq) next tick.
        const bool lost = _rt_unacked && sane && _seq_echo != _rt_seq;
        if (_rt_unacked && sane && _seq_echo == _rt_seq) _rt_unacked = false;
        if (_rt_dirty || lost || now - _last_cmd_ms >= kRefreshMs)
            sendRetarget();
    }
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

// ---- mlink homing tunables --------------------------------------------------
// This motor free-runs at ~0.03 A (operator-measured 2026-08-08), so the AIM
// path's 3 A margin and 29.5 mm/s crawl are both far too timid here. Local to
// this backend on purpose: the AIM_* constants stay tuned for the FAS path.
namespace {
constexpr float kHomeSpeedMmS = 60.0f;
constexpr float kHomeMarginA  = 0.4f;
}  // namespace

bool MlinkServoDriver::sendSetPos(float counts) {
    // Idempotent absolute set: repeat until the echoed position confirms.
    for (int i = 0; i < 10; i++) {
        uint8_t out[kFrameBytes] = {kOpSetPos, ++_seq};
        memcpy(&out[2], &counts, 4);
        uint8_t in[kFrameBytes] = {};
        xfer(out, in);
        vTaskDelay(pdMS_TO_TICKS(15));
        update();
        if (fabsf(_pos_counts - counts) < 4.0f) return true;
    }
    return false;
}

// Current-stall sensorless homing, the AIM recipe over the mlink: crawl
// rearward, watch the INA226 for the wall's current spike, plant the zero via
// kOpSetPos, glide off. Runs BLOCKING on motorTask (sanctioned for homing);
// update() inside every wait keeps the link serviced. No Modbus anywhere.
bool MlinkServoDriver::home(int32_t) {
    if (!_begun || !_status_fresh) {
        SLOGW("mlink", "homing refused: mlink link not up");
        return false;
    }
    if (!_current.isReady()) _current.init();
    if (!_current.isReady()) {
        SLOGW("mlink", "homing refused: INA228 not answering -- no stall sense");
        return false;
    }
    _current.resetPeaks();
    _homing = true;
    _homed = false;
    _seg_mode = false;
    _seg_unacked = false;

    const float scale    = AIM_STEPS_PER_MM;
    const float sweep_mm = 1.2f * getMaxRailMm();
    const float v        = kHomeSpeedMmS * scale;
    const float start    = _pos_counts;
    // Rearward = counts increasing (home 0, front negative).
    _rt_target = start + sweep_mm * scale;
    _rt_v = v;
    _rt_a = 8.0f * v;
    _rt_valid = true;
    _rt_dirty = true;

    const uint32_t poll_ms = 1000u / AIM_HOME_POLL_HZ;
    // Spin-up before the baseline: residual start transients settle out.
    for (int i = 0; i < 50; i++) {
        update();
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
    float base = 0.0f;
    for (int i = 0; i < AIM_HOME_BASELINE_SAMPLES; i++) {
        base += fabsf(_current.readCurrentA());
        update();
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
    base /= float(AIM_HOME_BASELINE_SAMPLES);

    const uint32_t t0 = millis();
    const uint32_t timeout_ms =
        uint32_t(sweep_mm / kHomeSpeedMmS * 1000.0f) + 5000u;
    int  consec = 0;
    bool wall   = false;
    while (millis() - t0 < timeout_ms) {
        update();
        if (fabsf(_current.readCurrentA()) > base + kHomeMarginA) {
            if (++consec >= AIM_HOME_STALL_CONSEC) { wall = true; break; }
        } else {
            consec = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }

    // Brake where we are, whatever happened.
    _rt_target = _pos_counts;
    _rt_dirty = true;
    for (int i = 0; i < 60 && fabsf(_vel_counts) > 50.0f; i++) {
        update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const float swept_mm = fabsf(_pos_counts - start) / scale;
    if (!wall) {
        _rt_valid = false;
        _homing = false;
        SLOGW("mlink", "homing FAILED: no current spike in %.0f mm of sweep "
              "(base %.2f A) -- check drive power/torque", sweep_mm, base);
        return false;
    }
    // A stall debounced at the far end of the sweep is a fault, not a wall
    // (AIM_HOME_STALL_PLAUSIBLE_FRAC rule, same reasoning as the FAS path).
    if (swept_mm >= AIM_HOME_STALL_PLAUSIBLE_FRAC * sweep_mm) {
        _rt_valid = false;
        _homing = false;
        SLOGW("mlink", "homing FAILED: stall at %.0f mm rides the sweep bound "
              "-- rejecting as implausible", swept_mm);
        return false;
    }

    // Plant the zero: the wall sits BACKOFF behind home in the count frame.
    // Kill the retarget refresh FIRST -- a refresh after the set would seek an
    // old-frame target.
    _rt_valid = false;
    _rt_dirty = false;
    if (!sendSetPos(AIM_HOMING_BACKOFF_MM * scale)) {
        _homing = false;
        SLOGW("mlink", "homing FAILED: kOpSetPos never confirmed");
        return false;
    }
    // Glide off the wall to home = 0.
    _rt_target = 0.0f;
    _rt_v = v;
    _rt_a = 8.0f * v;
    _rt_valid = true;
    _rt_dirty = true;
    const uint32_t t1 = millis();
    while (millis() - t1 < 5000u) {
        update();
        if (fabsf(_pos_counts) < 4.0f && fabsf(_vel_counts) < 50.0f) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    _homing = false;
    _homed = true;
    SLOGI("mlink", "homed :3 wall at %.1f mm of sweep, free-run base %.2f A, "
          "zero planted, backed off %.1f mm",
          swept_mm, base, (float)AIM_HOMING_BACKOFF_MM);
    return true;
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
                                    uint32_t speed_steps_s,
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
        _hold_p = _pos_counts;
        _hold_v = 0.0f;
        _hold_ms = now;
        _samp_ms = now;
        _sweep_pending = true;
        _seg_mode = true;
    }
    _samp_p = float(target_steps);
    _samp_v = v;
    _samp_ms = now;
    _samp_vcap = float(speed_steps_s);
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
    float ext = _pos_counts + _vel_counts * (float(age) * 1e-3f);
    // The renderer never passes its active target: clamp the extrapolation
    // to it, or a reversal-edge read lands past the window and trips the
    // arbiter's outside-window gentle cap (pattern pinned to USER speed,
    // self-reinforcing late strokes -- 2026-08-08). Idle: target==pos, so
    // extrapolation is disabled at rest by construction.
    const float tgt = _seg_mode ? _samp_p : (_rt_valid ? _rt_target : _pos_counts);
    const float lo = (_pos_counts < tgt) ? _pos_counts : tgt;
    const float hi = (_pos_counts < tgt) ? tgt : _pos_counts;
    if (ext < lo) ext = lo;
    if (ext > hi) ext = hi;
    return -ext / AIM_STEPS_PER_MM;
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
