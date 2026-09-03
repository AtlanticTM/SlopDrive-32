// MlinkServoDriver -- MotorDriver over the RP2350 SPI motion link.
// Constraints:
// - ONE OWNER TASK, captured on the first update() (motorTask, Core 1). Only
//   the owner runs xfer(); every other task posts an atomic flag and update()
//   ships it. The 10 ms tick is the command latency. See the header.
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
// Chain gaps above this re-seed the engine instead of sweeping: a stretched
// sweep is PERMANENT added latency on a renderer that never drains faster
// (operator ruling). Below it, a gentle glide is imperceptible.
constexpr float kReseedGapMm = 8.0f;
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

bool MlinkServoDriver::isOwner() const {
    return _owner.load(std::memory_order_acquire) ==
           (void*)xTaskGetCurrentTaskHandle();
}

void MlinkServoDriver::pushCeiling(float mm_s) {
    _ceiling_mm_s = mm_s;
    const float cps = mm_s * AIM_STEPS_PER_MM;
    uint8_t out[kFrameBytes] = {kOpSetLimits, ++_seq};
    memcpy(&out[2], &cps, 4);
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    SLOGI("mlink", "render ceiling -> %.0f mm/s (%.0f counts/s)",
          (double)mm_s, (double)cps);
}

void MlinkServoDriver::setRenderCeiling(float mm_s) {
    if (!(mm_s > 0.0f)) return;
    // Reached from httpTask (WebUI settings) and from setup() before the owner
    // exists: only the owner may drive the bus, so everyone else posts.
    if (_begun && isOwner()) { pushCeiling(mm_s); return; }
    _ceiling_req.store(mm_s, std::memory_order_release);
}

float MlinkServoDriver::liveCounts() const {
    // Same extrapolation getPosition() applies, in the raw count frame and
    // without the target clamp (an anchor wants the best estimate, not a
    // display value). v=0 at rest keeps standstill exact.
    uint32_t age = millis() - _status_ms + kTickMs;
    if (age > 3 * kTickMs) age = 3 * kTickMs;
    return _pos_counts + _vel_counts * (float(age) * 1e-3f);
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

// Hold-knot accel by centered difference across [chain, samp] -- both
// velocities come off the analytic curve, so the estimate is clean. The
// shipped a1 is reused verbatim as the next segment's a0 (exact C2 chain).
float MlinkServoDriver::holdKnotAccel() const {
    const uint32_t dt_us = _samp_us - _chain_us;
    if (dt_us == 0 || dt_us > 200000u) return 0.0f;
    return (_samp_v - _chain_v) / (float(dt_us) * 1e-6f);
}

void MlinkServoDriver::sendSegmentTo(float p1, float v1, float a1,
                                     uint32_t t1_us) {
    // Hermite chunk covering [chain, (p1,v1,a1,t1)]: the slave renders it
    // over its wire duration, which is what preserves the stream's timeline.
    uint32_t dur_us = t1_us - _chain_us;
    // Catch-up sweep after a re-anchor: stretch to the arbiter's ceiling so
    // the gap GLIDES closed instead of shooting at the render cap (the
    // ungoverned lunge cost 28.8 mm of drive-follow sync, 2026-08-08). The
    // extra render time lands as transient runway; the gate drains it.
    if (_sweep_pending) {
        _sweep_pending = false;
        // Anchor at SHIP time, never at detect time: the runway gate can
        // hold a sweep for the whole chunk already rendering, and a chain
        // anchored when the gap was noticed is that far behind the RP when
        // it finally ships. Measured 2026-09-02: an anchor 0.8 s old put p0
        // 63 mm behind the rendered position and the RP walked the
        // difference backward at the 1000 mm/s ceiling (mlink census
        // overrun=1349 residue=13238 counts). Live velocity keeps the
        // handoff C1 when the RP is still moving; the v-cap below bounds it.
        _chain_p = liveCounts();
        _chain_v = _vel_counts;
        _chain_a = 0.0f;
        // Too far to glide: ship NOTHING and request an engine re-seed at
        // the live position -- the gap becomes a COLD-start plan through the
        // engine's own feasibility machinery instead of wire-duration debt
        // the slave can never drain (sd-d77). ONE attempt per episode: a
        // reseed cannot converge when the live position is outside the
        // window (the engine's frame clamps), so a surviving gap falls
        // through to the gentle sweep below -- the window-entry glide.
        // Never mutate the chain here: a future-stamped hold once collided
        // with the sample clock and shipped a 317 us / 42 mm chunk.
        if (fabsf(p1 - _chain_p) > kReseedGapMm * AIM_STEPS_PER_MM &&
            !_reseed_tried) {
            _reseed_tried = true;
            _reseed_req = true;
            _sweep_pending = true;   // re-run this decision after the reset
            return;
        }
        _reseed_tried = false;
        // A sweep is RECOVERY, not content: cap it at the gentle USER limit
        // (same rule as window-entry and window glide). Content never sweeps,
        // so feel is untouched -- a 838 mm/s from-rest restart dart was the
        // last grit class standing (sd-d77, 2026-08-10).
        float vsweep = _samp_vcap;
        if (_recovery_mm_s > 0.0f)
            vsweep = fminf(vsweep, _recovery_mm_s * AIM_STEPS_PER_MM);
        if (vsweep > 1.0f) {
            // Stretch for the quintic's interior PEAK, not its chord: a
            // from-rest quintic peaks at ~1.875x its mean, so a chord-governed
            // sweep whips at nearly 2x the ceiling (mchunk census 2026-08-09:
            // vpk 1230-1278 mm/s against a 950 ceiling on every re-anchor --
            // the grit). Peak-governed, the same sweep glides.
            const float need_us =
                1.875f * fabsf(p1 - _chain_p) / vsweep * 1e6f;
            if (need_us > float(dur_us)) dur_us = uint32_t(need_us);
        }
        // A stretched sweep from v0=0 that still ARRIVES at the curve's full
        // velocity is a Hermite bulge: the polynomial overshoots hard and
        // whips back (the sharp-jitter + silent-teleport drift chain,
        // 2026-08-09). Fritsch-Carlson bound: |v1| <= 1.5x the chord slope.
        if (dur_us > 0) {
            const float chord =
                fabsf(p1 - _chain_p) / (float(dur_us) * 1e-6f);
            const float vcap = 1.5f * chord;
            if (v1 >  vcap) v1 =  vcap;
            if (v1 < -vcap) v1 = -vcap;
            if (_chain_v >  vcap) _chain_v =  vcap;
            if (_chain_v < -vcap) _chain_v = -vcap;
        }
        // A clamped v1 makes the passed a1 inconsistent; land the sweep flat.
        a1 = 0.0f;
    }
    // Interior-velocity scan of the EXACT polynomial the slave will render
    // (sd-ar3.1 grit hunt): chunks with clean endpoints carry >1900 mm/s
    // interior spikes ~100/s in fast content, and no upstream census sees
    // inside a chunk. Worst chunk per second, with the parameters that
    // built it, so the guilty term names itself. 9 derivative evals per
    // chunk at ~100/s: T27-negligible next to the SPI xfer below.
    {
        const float T = float(dur_us) * 1e-6f;
        if (T > 0.0f) {
            const float V0 = _chain_v * T, V1 = v1 * T;
            const float A0 = _chain_a * T * T, A1 = a1 * T * T;
            const float R1 = p1 - _chain_p - V0 - 0.5f * A0;
            const float R2 = V1 - V0 - A0;
            const float R3 = A1 - A0;
            const float c2 = 0.5f * A0;
            const float c3 = 10.0f * R1 - 4.0f * R2 + 0.5f * R3;
            const float c4 = -15.0f * R1 + 7.0f * R2 - R3;
            const float c5 = 6.0f * R1 - 3.0f * R2 + 0.5f * R3;
            float vpk = 0.0f;
            for (int k = 0; k <= 8; k++) {
                const float u = float(k) * 0.125f;
                const float v = ((((5.0f * c5 * u + 4.0f * c4) * u + 3.0f * c3)
                                  * u + 2.0f * c2) * u + V0) / T;
                if (fabsf(v) > vpk) vpk = fabsf(v);
            }
            if (vpk > _mc_vpk) {
                _mc_vpk = vpk;
                _mc_dur = dur_us;
                _mc_v0 = _chain_v; _mc_v1 = v1;
                _mc_a0 = _chain_a; _mc_a1 = a1;
                _mc_dp = p1 - _chain_p;
            }
            const uint32_t mnow = millis();
            if (mnow - _mc_ms >= 1000u && _mc_vpk > 0.0f) {
                _mc_ms = mnow;
                SLOGI("mchunk", "worst/s: vpk=%.0f c/s dur=%luus dp=%.1f "
                      "v0=%.0f v1=%.0f a0=%.0f a1=%.0f",
                      (double)_mc_vpk, (unsigned long)_mc_dur, (double)_mc_dp,
                      (double)_mc_v0, (double)_mc_v1,
                      (double)_mc_a0, (double)_mc_a1);
                _mc_vpk = 0.0f;
            }
        }
    }
    uint8_t out[kFrameBytes] = {kOpSegment2, ++_seq};
    memcpy(&out[2], &dur_us, 4);
    memcpy(&out[6], &_chain_p, 4);
    memcpy(&out[10], &_chain_v, 4);
    memcpy(&out[14], &_chain_a, 4);
    memcpy(&out[18], &p1, 4);
    memcpy(&out[22], &v1, 4);
    memcpy(&out[26], &a1, 4);
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
    _chain_a = a1;
    _chain_us = t1_us;
    _last_cmd_ms = millis();
}

void MlinkServoDriver::sendSegment() {
    sendSegmentTo(_hold_p, _hold_v, holdKnotAccel(), _hold_us);
}

void MlinkServoDriver::sendSegmentSplit() {
    // Two halves of the same quintic, evaluated at u=0.5, so an empty ring is
    // primed to depth 2 in one tick -- production is real-time-capped, so
    // steady one-per-tick shipping can never deepen the ring by itself.
    const float T = float(_hold_us - _chain_us) * 1e-6f;
    const float a1 = holdKnotAccel();
    const float V0 = _chain_v * T, V1 = _hold_v * T;
    const float A0 = _chain_a * T * T, A1 = a1 * T * T;
    const float R1 = _hold_p - _chain_p - V0 - 0.5f * A0;
    const float R2 = V1 - V0 - A0;
    const float R3 = A1 - A0;
    const float c2 = 0.5f * A0;
    const float c3 = 10.0f * R1 - 4.0f * R2 + 0.5f * R3;
    const float c4 = -15.0f * R1 + 7.0f * R2 - R3;
    const float c5 = 6.0f * R1 - 3.0f * R2 + 0.5f * R3;
    const float u = 0.5f;
    const float mid_p =
        ((((c5 * u + c4) * u + c3) * u + c2) * u + V0) * u + _chain_p;
    const float mid_v =
        ((((5.0f * c5 * u + 4.0f * c4) * u + 3.0f * c3) * u + 2.0f * c2) * u +
         V0) / T;
    const float mid_a =
        (((20.0f * c5 * u + 12.0f * c4) * u + 6.0f * c3) * u + 2.0f * c2) /
        (T * T);
    const uint32_t mid_us = _chain_us + (_hold_us - _chain_us) / 2u;
    sendSegmentTo(mid_p, mid_v, mid_a, mid_us);
    sendSegmentTo(_hold_p, _hold_v, a1, _hold_us);
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
    // Seed the ceiling: an RP that has not been told a limit renders
    // unlimited, which is the state that cost 84 mm. init() is not the owner
    // task, so this posts and the first update() tick ships it -- still long
    // before the arbiter will dispatch anything (homing gates motion).
    setRenderCeiling(_max_speed_mm_s);
    SLOGI("mlink", "MlinkServoDriver up: RP2350 quadrature backend, "
          "%.1f counts/mm, speed cap %.0f counts/s",
          (double)AIM_STEPS_PER_MM, (double)kMaxCountsPerSec);
}

void MlinkServoDriver::update() {
    if (!_begun) return;
    // First tick claims the link. init() runs on the setup task, so the claim
    // cannot live there; every poster before this point ships on this tick.
    if (!_owner.load(std::memory_order_relaxed))
        _owner.store((void*)xTaskGetCurrentTaskHandle(),
                     std::memory_order_release);
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
        memcpy(&_emitted_counts, &in[kStatusOffEmitted], 4);
        uint16_t qd = 0, ov = 0, lt = 0, vc = 0;
        memcpy(&qd, &in[kStatusOffQDrops], 2);
        memcpy(&ov, &in[kStatusOffEmitOverrun], 2);
        memcpy(&lt, &in[kStatusOffLateTicks], 2);
        memcpy(&vc, &in[kStatusOffVelClamped], 2);
        // T27: these are MONOTONIC counters, so "changed since last poll" is
        // true on every poll -- that is a level, not an edge, and it floods the
        // ring at the poll rate. Census the deltas, emit at most once a second,
        // and only when something actually moved. Residue rides along because
        // a counter without it is not evidence (sd-dxy.1.1).
        // The first sane frame SEEDS (the RP's counters predate this boot),
        // and any decrease is an RP restart: counters saturate, never wrap
        // (sd-dxy.3 -- an unseeded first delta manufactured a false P0, and a
        // restart read as deltas is two's-complement garbage). A restarted RP
        // also lost its RAM ceiling: re-push it before trusting any motion.
        const bool rp_restart = _rc_primed &&
            (qd < _qdrops || ov < _emit_overrun ||
             lt < _late_ticks || vc < _vel_clamped);
        if (!_rc_primed || rp_restart) {
            if (rp_restart) {
                SLOGW("mlink", "RP RESTARTED (telemetry counters reset); "
                      "re-pushing render ceiling %.0f mm/s",
                      (double)_ceiling_mm_s);
                if (_ceiling_mm_s > 0.0f) setRenderCeiling(_ceiling_mm_s);
            }
            _rc_primed = true;
        } else {
            _rc_qd += uint16_t(qd - _qdrops);
            _rc_ov += uint16_t(ov - _emit_overrun);
            _rc_lt += uint16_t(lt - _late_ticks);
            _rc_vc += uint16_t(vc - _vel_clamped);
        }
        _qdrops = qd; _emit_overrun = ov; _late_ticks = lt; _vel_clamped = vc;
        const float residue = _pos_counts - _emitted_counts;
        if (fabsf(residue) > fabsf(_rc_res_max)) _rc_res_max = residue;
        if (now - _rc_ms >= 1000u) {
            _rc_ms = now;
            if (_rc_qd || _rc_ov || _rc_lt || _rc_vc || fabsf(_rc_res_max) >= 1.0f) {
                SLOGW("mlink", "RP renderer/s: qdrops=%u overrun=%u late=%u clamped=%u "
                      "residue_max=%.1f counts",
                      unsigned(_rc_qd), unsigned(_rc_ov), unsigned(_rc_lt),
                      unsigned(_rc_vc), (double)_rc_res_max);
            }
            _rc_qd = _rc_ov = _rc_lt = _rc_vc = 0;
            _rc_res_max = 0.0f;
        }
        _status_ms = now;
        _status_fresh = true;
    }

    if (_estop_pending.load(std::memory_order_acquire)) {
        if (!sane || _state != kStateEstop) sendOp(kOpEstop);
        else _estop_pending.store(false, std::memory_order_relaxed);
        return;                       // nothing else while stopping
    }
    if (_clear_pending.load(std::memory_order_acquire)) {
        if (sane && _state == kStateEstop) sendOp(kOpClear);
        else if (sane) _clear_pending.store(false, std::memory_order_relaxed);
        return;
    }
    // Ceiling posted by another task. After the stop paths on purpose: a
    // pending estop owns the wire until the slave echoes it.
    const float req = _ceiling_req.exchange(0.0f, std::memory_order_acquire);
    if (req > 0.0f) pushCeiling(req);

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
            _samp_us != _hold_us) {
            _chain_p = liveCounts();
            _chain_v = 0.0f;
            _chain_a = 0.0f;
            _chain_us = _hold_us - kTickMs * 1000u;
            _sweep_pending = true;
        }
        // Blocked-interval re-base; unsigned compare also catches any
        // chain-ahead-of-hold ordering bug as a huge gap.
        if (_hold_us - _chain_us > kStreamGapMs * 1000u) {
            _chain_p = liveCounts();
            _chain_v = 0.0f;
            _chain_a = 0.0f;
            _chain_us = _hold_us - kTickMs * 1000u;
            _sweep_pending = true;
        }
        // Gate compensates the one-tick-stale runway report. Split ONLY at
        // depth 0: sustained multi-frame ticks exceed the slave's per-frame-
        // reset budget (2.4.87: torn 83k, qdrops 607); deeper waits on sd-dxy.
        const int32_t span_us = int32_t(_hold_us - _chain_us);
        if (sane && !_reseed_req && span_us > 0 && _depth < kSegmentDepth &&
            _runway_ms < kRunwayTargetMs + 2 * kTickMs) {
            // A governed sweep never splits: it is one stretched glide.
            if (_depth == 0 && span_us >= 4000 && !_sweep_pending)
                sendSegmentSplit();
            else sendSegment();
        }
        // Holdback advances AFTER the ship attempt: a fresh chunk always
        // exists to ship into a draining ring next tick.
        _hold_p = _samp_p;
        _hold_v = _samp_v;
        _hold_us = _samp_us;
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
    _rt_valid = false;
    _seg_mode = false;
    _seg_unacked = false;
    // Flag first: a non-owner caller (OtaService::prepareForOta -> arbiter, on
    // httpTask) must leave the bus alone, and the next update() tick ships it.
    _estop_pending.store(true, std::memory_order_release);
    if (_begun && isOwner()) sendOp(kOpEstop);   // owner: immediate
}

void MlinkServoDriver::enable() {
    // Leaving an estop hold needs the explicit clear; harmless otherwise.
    if (_state == kStateEstop)
        _clear_pending.store(true, std::memory_order_release);
}

// ---- mlink homing tunables --------------------------------------------------
// This motor free-runs at ~0.03 A (operator-measured 2026-08-08), so the AIM
// path's 3 A margin and 29.5 mm/s crawl are both far too timid here. Local to
// this backend on purpose: the AIM_* constants stay tuned for the FAS path.
namespace {
constexpr float kHomeFastMmS       = 60.0f;  // rough wall find
constexpr float kHomeSlowMmS       = 12.0f;  // accurate re-probe (rope settles)
constexpr float kHomeMarginA       = 0.4f;
constexpr float kHomeMarginMm      = 5.0f;   // usable window ends here, each wall
constexpr float kHomeReprobeBackMm = 15.0f;  // back off past the rope slack
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

bool MlinkServoDriver::homingAbort(const char* what) {
    _rt_valid = false;
    _rt_dirty = false;
    _homing = false;
    SLOGW("mlink", "homing FAILED: %s", what);
    return false;
}

// One stall probe: sweep `dir` (+1 = toward the home wall) at `speed_mm_s`,
// bounded by `bound_mm` of travel. pos_out = where the carriage stopped.
// False = no stall inside the bound, or a stall riding the bound (a fault,
// not a wall -- the AIM_HOME_STALL_PLAUSIBLE_FRAC rule).
bool MlinkServoDriver::sweepToStall(float dir, float speed_mm_s, float bound_mm,
                                    float& pos_out) {
    const float scale = AIM_STEPS_PER_MM;
    const float start = _pos_counts;
    _rt_target = start + dir * bound_mm * scale;
    _rt_v = speed_mm_s * scale;
    _rt_a = 8.0f * _rt_v;
    _rt_valid = true;
    _rt_dirty = true;

    const uint32_t poll_ms = 1000u / AIM_HOME_POLL_HZ;
    // Spin-up, then a MOVING free-run baseline (start transients settle out).
    for (int i = 0; i < 40; i++) {
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
        uint32_t(bound_mm / speed_mm_s * 1000.0f) + 4000u;
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
    pos_out = _pos_counts;
    if (!wall) return false;
    const float swept_mm = fabsf(pos_out - start) / scale;
    return swept_mm < AIM_HOME_STALL_PLAUSIBLE_FRAC * bound_mm;
}

void MlinkServoDriver::glideTo(float counts, float speed_mm_s,
                               uint32_t max_ms) {
    _rt_target = counts;
    _rt_v = speed_mm_s * AIM_STEPS_PER_MM;
    _rt_a = 8.0f * _rt_v;
    _rt_valid = true;
    _rt_dirty = true;
    const uint32_t t0 = millis();
    while (millis() - t0 < max_ms) {
        update();
        if (fabsf(_pos_counts - counts) < 8.0f && fabsf(_vel_counts) < 50.0f)
            break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Both-ends current-stall homing (operator spec 2026-08-08): each wall gets a
// FAST rough find then a SLOW re-probe from a backed-off start -- the rope
// slacks at the stops, so the fast contact position lies by that slack. The
// usable window keeps kHomeMarginMm off each wall; the measured stroke rides
// the existing homed-edge NVS persist. BLOCKING on motorTask (sanctioned);
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
    // A latched slave estop holds the renderer through the whole sweep (no
    // motion, no spike, clean-looking timeout) -- clear it first, exactly as
    // forceHomeState() does for the bench path.
    if (_state == kStateEstop) {
        _clear_pending.store(true, std::memory_order_release);
        for (int i = 0; i < 50 && _state == kStateEstop; i++) {
            update();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (_state == kStateEstop) {
            SLOGW("mlink", "homing refused: slave estop will not clear");
            return false;
        }
    }
    _current.resetPeaks();
    _homing = true;
    _homed = false;
    _seg_mode = false;
    _seg_unacked = false;

    const float scale = AIM_STEPS_PER_MM;
    const float rail  = getMaxRailMm();
    float wf = 0.0f;
    float wr = 0.0f;

    // FRONT wall first (- = away from home), REAR second: the ritual then
    // ENDS 5 mm from home, so the park is a glide instead of a full-rail
    // return trip (operator order: cut the wasted move).
    if (!sweepToStall(-1.0f, kHomeFastMmS, 1.2f * rail, wf))
        return homingAbort("front wall not found (fast sweep)");
    glideTo(wf + kHomeReprobeBackMm * scale, kHomeFastMmS, 4000u);
    if (!sweepToStall(-1.0f, kHomeSlowMmS, kHomeReprobeBackMm + 10.0f, wf))
        return homingAbort("front wall not confirmed (slow probe)");

    // Rear wall (+ = toward home): rough across the rail, back off, accurate.
    if (!sweepToStall(+1.0f, kHomeFastMmS, 1.2f * rail, wr))
        return homingAbort("rear wall not found (fast sweep)");
    glideTo(wr - kHomeReprobeBackMm * scale, kHomeFastMmS, 4000u);
    if (!sweepToStall(+1.0f, kHomeSlowMmS, kHomeReprobeBackMm + 10.0f, wr))
        return homingAbort("rear wall not confirmed (slow probe)");

    // Both walls measured in the same pre-zero frame; a margin comes off
    // each end of the usable window.
    const float span_mm = (wr - wf) / scale;
    const float usable  = span_mm - 2.0f * kHomeMarginMm;
    if (usable < 50.0f)
        return homingAbort("measured stroke implausibly short");

    // Zero: we are AT the accurate rear wall, which sits kHomeMarginMm
    // behind home. The refresh dies first -- it would seek an old-frame
    // target after the set.
    _rt_valid = false;
    _rt_dirty = false;
    if (!sendSetPos(kHomeMarginMm * scale))
        return homingAbort("kOpSetPos never confirmed");
    setMeasuredStrokeMm(usable);

    // Home is one margin away.
    glideTo(0.0f, kHomeSlowMmS, 3000u);
    _homing = false;
    _homed = true;
    SLOGI("mlink", "homed :3 wall-to-wall %.1f mm, usable %.1f mm "
          "(%.0f mm margin per wall, slow probe %.0f mm/s)",
          span_mm, usable, (float)kHomeMarginMm, (float)kHomeSlowMmS);
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
    const uint32_t now_us = micros();
    // (Re-)anchor at the live rendered position on entry or after a stream
    // gap; a stale chain tail would ship one giant segment spanning the idle.
    // Store order matters: update() may preempt between statements (same
    // core), so _seg_mode flips true only after the chain is coherent.
    if (!_seg_mode || now_us - _samp_us > kStreamGapMs * 1000u) {
        _rt_valid = false;
        _rt_dirty = false;
        _seg_unacked = false;
        _chain_p = liveCounts();
        _chain_v = 0.0f;
        _chain_a = 0.0f;
        _chain_us = now_us;
        _hold_p = _chain_p;
        _hold_v = 0.0f;
        _hold_us = now_us;
        _samp_us = now_us;
        _sweep_pending = true;
        _reseed_tried = false;
        _seg_mode = true;
    }
    _samp_p = float(target_steps);
    _samp_v = v;
    _samp_us = now_us;
    _samp_vcap = float(speed_steps_s);
    _last_accel_native = accel_steps_s2;
}

// Both stops are reachable from httpTask (WebUI -> arbiter), so they fill the
// retarget shadow BEFORE leaving segment mode: the owner must never find
// _seg_mode false next to a stale target. Same store-order rule streamSample
// documents. Neither touches the bus.
void MlinkServoDriver::stop() {
    // Full stop clears homed (interface contract). Land where we are.
    _homed = false;
    _rt_target = _pos_counts;
    _rt_v = kMaxCountsPerSec;
    if (_rt_a < 1.0f) _rt_a = 100000.0f;
    _rt_valid = true;
    _seg_unacked = false;
    _seg_mode = false;
    _rt_dirty = true;
}

void MlinkServoDriver::hardStop() {
    _rt_target = _pos_counts;
    _rt_v = kMaxCountsPerSec;
    if (_rt_a < 1.0f) _rt_a = 100000.0f;
    _rt_valid = true;
    _seg_unacked = false;
    _seg_mode = false;
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
