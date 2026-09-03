// MlinkServoDriver -- MotorDriver over the RP2350 SPI motion link.
// Constraints:
// - The drive is SAVED in encoder-follow (0x19=2, gear 4/1, 8192 counts/rev);
//   the RP2350 is the only pulse source. FAS step/dir never drives it again.
// - The link has ONE OWNER TASK: whichever task first runs update(), which is
//   motorTask (Core 1). ONLY the owner may drive the SPI bus. Entry points
//   that other tasks can reach (emergencyStop and setRenderCeiling from
//   httpTask, forceHomeState/enable from the web API) POST an atomic flag and
//   return; update() ships it on the next tick. Never a mutex -- a bus with
//   two owners is the thing this rule forbids, and sd-4k1.4 keeps this shape.
// - Cost of that deferral is ONE kTickMs tick (10 ms) of added estop latency,
//   which SPEC H1 already covers: the protocol ESTOP is a software convenience
//   above the hardware e-stop path, never the guarantee.
// - Motion state (retarget shadow, chain) is written by motorTask and the
//   sampler task, both Core 1; cross-task posters order their stores so the
//   flag the owner tests is the LAST one written.
// - Native unit = drive input counts, scaled by the runtime geometry
//   (aimStepsPerMm at 8192 steps/rev). Speed clamps to kMaxCountsPerSec.
// - Homing v1 = HOME_OVERRIDE only (forceHomeState); real homing rides sd-dxy.
// See: include/comms/MotionLinkProtocol.h, dev board sd-dxy.
#pragma once

#include <atomic>
#include <cstdint>

#include "MotorDriver.h"
#include "CurrentSensor.h"
#include "comms/MotionLinkProtocol.h"

class MlinkServoDriver final : public MotorDriver {
public:
    void init() override;
    void update() override;
    void emergencyStop() override;

    bool home(int32_t home_speed_steps_s = 4000) override;
    void runHomingStep() override {}
    bool isHomed()  const override { return _homed; }
    bool isHoming() const override { return _homing; }
    // Bench force-home also un-latches a slave-side estop: the S3 clears its
    // own latch, and without kOpClear the RP holds position forever while
    // every command silently queues (2026-08-07: frozen pos, zero flags).
    void forceHomeState(bool homed) override {
        _homed = homed;
        if (homed && _state == motionlink::kStateEstop)
            _clear_pending.store(true, std::memory_order_release);
    }
    bool checkPushToHome() override { return false; }
    void runMotorStep() override {}

    void enable() override;
    void disable() override {}

    void  setMaxSpeed(float mm_s) override { _max_speed_mm_s = mm_s; }
    // Push the renderer's speed ceiling to the RP (kOpSetLimits). The
    // coprocessor is OPEN LOOP: without a ceiling it commands whatever the
    // trajectory asks and the drive silently drops what it cannot follow.
    // Call whenever the INPUT limit set changes, not the user set -- the RP
    // only ever renders machine-driven motion.
    void  setRenderCeiling(float mm_s) override;
    void  setRecoverySpeed(float mm_s) override { _recovery_mm_s = mm_s; }
    bool  consumeReseedRequest() override {
        const bool r = _reseed_req; _reseed_req = false; return r;
    }
    void  setAcceleration(float mm_s2) override { _accel_mm_s2 = mm_s2; }
    float getMaxSpeed() const override { return _max_speed_mm_s; }
    float getAcceleration() const override { return _accel_mm_s2; }
    uint32_t getLiveAcceleration() const override { return _last_accel_native; }

    bool  isMoving() override { return _state == motionlink::kStateRunning; }
    float getPosition() const override;
    float getTargetPosition() const override;

    void applyDriverConfig(const DriverConfig&) override {}
    uint16_t getCurrentmA()  override { return 0; }
    uint8_t  getMicrosteps() override { return 1; }
    void     setBlendMode(uint8_t m) override { _blend = m; }
    uint8_t  getBlendMode() const override { return _blend; }

    int32_t mmToNative(float mm) const override;
    float   nativeToMm(int32_t native) const override;

    float getMeasuredStrokeMm() const override { return _stroke_mm; }
    void  setMeasuredStrokeMm(float mm) override { _stroke_mm = mm; }

protected:
    void streamToSteps(int32_t target_steps,
                       uint32_t speed_steps_s,
                       uint32_t accel_steps_s2) override;
    void streamSample(int32_t target_steps, float vel_steps_s,
                      uint32_t speed_steps_s, uint32_t accel_steps_s2) override;
    void stop() override;
    void hardStop() override;

private:
    // True only on the owner task; false before the first update() too, so
    // the setup-task seed defers to the first tick like any other poster.
    bool isOwner() const;
    void pushCeiling(float mm_s);
    void xfer(uint8_t (&out)[motionlink::kFrameBytes],
              uint8_t (&in)[motionlink::kFrameBytes]);
    void sendOp(uint8_t op);
    void sendRetarget();
    void sendSegment();
    void sendSegmentTo(float p1, float v1, float a1, uint32_t t1_us);
    void sendSegmentSplit();
    float holdKnotAccel() const;
    bool sendSetPos(float counts);
    bool sweepToStall(float dir, float speed_mm_s, float bound_mm,
                      float& pos_out);
    void glideTo(float counts, float speed_mm_s, uint32_t max_ms);
    bool homingAbort(const char* what);

    // Link state (owner task only)
    // TaskHandle_t as void*: keeps the FreeRTOS headers out of this header.
    std::atomic<void*> _owner{nullptr};
    uint8_t  _seq = 0;
    bool     _begun = false;
    uint32_t _last_tick_ms = 0;
    uint32_t _last_cmd_ms = 0;

    // Slave status from the last CRC-valid frame
    float   _pos_counts = 0.0f;
    float   _vel_counts = 0.0f;
    // Renderer truth from the RP (sd-dxy.1.1). _pos_counts is COMMANDED;
    // _emitted_counts is what was actually pulsed. Their difference is the
    // residue -- the number that separates "the coprocessor dropped it" from
    // "the drive did not follow it". Never read one without the other.
    float    _emitted_counts = 0.0f;
    uint16_t _qdrops = 0;
    uint16_t _emit_overrun = 0;
    uint16_t _late_ticks = 0;
    uint16_t _vel_clamped = 0;
    // Per-second census accumulators -- see the T27 note at the read site.
    uint16_t _rc_qd = 0, _rc_ov = 0, _rc_lt = 0, _rc_vc = 0;
    float    _rc_res_max = 0.0f;
    uint32_t _rc_ms = 0;
    // Counters are monotonic per RP boot (they saturate, never wrap), so the
    // first sane frame SEEDS and any decrease is an RP restart (sd-dxy.3).
    bool     _rc_primed = false;
    // Dead-reckoned RP position in counts: the raw report is up to ~2 ticks
    // stale, and staleness x velocity is exactly the chain-start gap that
    // made re-anchors teleport (2026-08-09). Use for every chain re-base.
    float    liveCounts() const;
    // Worst-chunk interior-velocity census (grit hunt, see sendSegmentTo).
    float    _mc_vpk = 0.0f;
    uint32_t _mc_dur = 0;
    float    _mc_v0 = 0.0f, _mc_v1 = 0.0f, _mc_a0 = 0.0f, _mc_a1 = 0.0f;
    float    _mc_dp = 0.0f;
    uint32_t _mc_ms = 0;
    // Last ceiling pushed via kOpSetLimits; re-pushed when an RP restart is
    // detected -- the RP holds it in RAM and boots unlimited without it.
    float    _ceiling_mm_s = 0.0f;
    // Ceiling posted by a non-owner task (WebUI settings, Core 0); >0 means
    // the owner owes it a kOpSetLimits on its next tick.
    std::atomic<float> _ceiling_req{0.0f};
    // USER (gentle) limit; caps recovery sweeps only, never content.
    float    _recovery_mm_s = 0.0f;
    // Chain gap exceeded kReseedGapMm: hold the wire, ask for an engine
    // re-seed at the live position instead of gliding the gap.
    bool     _reseed_req = false;
    // One reseed per episode: if the gap SURVIVES a reseed the live position
    // is outside the window (the engine's frame clamps and cannot converge --
    // the 42 mm phantom loop, 2026-08-10); fall back to the gentle sweep.
    bool     _reseed_tried = false;
    // True only between a stream-entry re-anchor and its first ship: the
    // one place a re-seed is allowed (see sendSegmentTo).
    bool     _reseed_armed = false;
    uint8_t _state = 0;
    uint8_t _slave_flags = 0;
    bool    _status_fresh = false;

    // Retarget shadow (idempotent; refreshed every 100 ms as drop insurance)
    float _rt_target = 0.0f;
    float _rt_v = 0.0f;
    float _rt_a = 0.0f;
    bool  _rt_valid = false;
    bool  _rt_dirty = false;
    // Seq-echo ack, same scheme as segments: a torn retarget otherwise waits
    // out the full 100 ms refresh (felt as a mid-stroke stall under EMI).
    uint8_t _rt_seq = 0;
    bool    _rt_unacked = false;

    // Segment-stream shadow (curve chase rides kOpSegment; retarget stays the
    // point-move path). Writers: streamSample() on the sampler task, update()
    // on motorTask -- both Core 1, so torn state is a preemption between two
    // statements, never true concurrency. streamSample() orders its stores so
    // _seg_mode reads true only after the chain fields are coherent.
    // All segment timeline stamps are MICROSECONDS (micros(), wrap-safe via
    // unsigned diffs): ms quantization put +/-10% speed error on a 10 ms
    // chunk, which at speed exceeded the slave's 64-count jump guard at
    // every boundary -- the fast-chord teleport drift (2026-08-09).
    bool     _seg_mode = false;
    float    _chain_p = 0.0f;      // last shipped segment endpoint
    float    _chain_v = 0.0f;
    // Shipped endpoint accel, reused verbatim as the next segment's a0 so the
    // kOpSegment2 chain is exactly C2 (accel steps at knots read as texture).
    float    _chain_a = 0.0f;
    uint32_t _chain_us = 0;
    float    _samp_p = 0.0f;       // freshest arbiter sample
    float    _samp_v = 0.0f;
    uint32_t _samp_us = 0;
    // One-tick holdback: ship to LAST tick's sample so a tick of produced
    // curve stays in reserve (production is real-time-capped, so without it
    // ring depth never exceeds 1 and jitter lands on the underrun edge).
    float    _hold_p = 0.0f;
    float    _hold_v = 0.0f;
    uint32_t _hold_us = 0;
    // Sweep governance: a re-anchored chain has no upstream speed limit (the
    // curve's governance lives in sample spacing, which a re-anchor discards),
    // so the catch-up segment stretches to the arbiter's active ceiling.
    float    _samp_vcap = 0.0f;    // arbiter dispatch ceiling, counts/s
    bool     _sweep_pending = false;
    uint8_t  _seg_frame[motionlink::kFrameBytes] = {};  // resend copy
    uint8_t  _seg_seq = 0;
    bool     _seg_unacked = false;

    // Credit fields from the last CRC-valid status
    uint16_t _runway_ms = 0;
    uint8_t  _depth = 0;
    uint8_t  _seq_echo = 0;
    uint32_t _status_ms = 0;   // millis() at the last CRC-valid status

    // Posted from any task, shipped by the owner (see the header note).
    std::atomic<bool> _estop_pending{false};
    std::atomic<bool> _clear_pending{false};

    bool     _homed = false;
    bool     _homing = false;
    CurrentSensor _current;    // INA228 on the carrier (behind the ISO1640)
    uint8_t  _blend = 1;
    float    _max_speed_mm_s = 0.0f;
    float    _accel_mm_s2 = 0.0f;
    float    _stroke_mm = 0.0f;
    uint32_t _last_accel_native = 0;
};
