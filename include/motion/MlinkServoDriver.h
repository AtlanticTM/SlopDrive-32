// MlinkServoDriver -- MotorDriver over the RP2350 SPI motion link.
// Constraints:
// - The drive is SAVED in encoder-follow (0x19=2, gear 4/1, 8192 counts/rev);
//   the RP2350 is the only pulse source. FAS step/dir never drives it again.
// - Single-task by construction: every entry point runs on motorTask (Core 1)
//   through MotionArbiter/MotorProxy, and update() owns the SPI link.
// - Native unit = drive input counts, scaled by the runtime geometry
//   (aimStepsPerMm at 8192 steps/rev). Speed clamps to kMaxCountsPerSec.
// - Homing v1 = HOME_OVERRIDE only (forceHomeState); real homing rides sd-dxy.
// See: include/comms/MotionLinkProtocol.h, dev board sd-dxy.
#pragma once

#include <cstdint>
#include "MotorDriver.h"
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
        if (homed && _state == motionlink::kStateEstop) _clear_pending = true;
    }
    bool checkPushToHome() override { return false; }
    void runMotorStep() override {}

    void enable() override;
    void disable() override {}

    void  setMaxSpeed(float mm_s) override { _max_speed_mm_s = mm_s; }
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
    void xfer(uint8_t (&out)[motionlink::kFrameBytes],
              uint8_t (&in)[motionlink::kFrameBytes]);
    void sendOp(uint8_t op);
    void sendRetarget();
    void sendSegment();
    void sendSegmentTo(float p1, float v1, uint32_t t1_ms);
    void sendSegmentSplit();
    bool sendSetPos(float counts);

    // Link state (motorTask only)
    uint8_t  _seq = 0;
    bool     _begun = false;
    uint32_t _last_tick_ms = 0;
    uint32_t _last_cmd_ms = 0;

    // Slave status from the last CRC-valid frame
    float   _pos_counts = 0.0f;
    float   _vel_counts = 0.0f;
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
    bool     _seg_mode = false;
    float    _chain_p = 0.0f;      // last shipped segment endpoint
    float    _chain_v = 0.0f;
    uint32_t _chain_ms = 0;
    float    _samp_p = 0.0f;       // freshest arbiter sample
    float    _samp_v = 0.0f;
    uint32_t _samp_ms = 0;
    // One-tick holdback: ship to LAST tick's sample so a tick of produced
    // curve stays in reserve (production is real-time-capped, so without it
    // ring depth never exceeds 1 and jitter lands on the underrun edge).
    float    _hold_p = 0.0f;
    float    _hold_v = 0.0f;
    uint32_t _hold_ms = 0;
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

    bool _estop_pending = false;
    bool _clear_pending = false;

    bool     _homed = false;
    bool     _homing = false;
    uint8_t  _ina_addr = 0;    // INA226 on the carrier, found by die-id scan
    uint8_t  _blend = 1;
    float    _max_speed_mm_s = 0.0f;
    float    _accel_mm_s2 = 0.0f;
    float    _stroke_mm = 0.0f;
    uint32_t _last_accel_native = 0;
};
