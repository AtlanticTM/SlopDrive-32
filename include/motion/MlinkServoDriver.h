// MlinkServoDriver -- MotorDriver over the RP2350 SPI motion link.
// Constraints:
// - The RP2350 HOLDS THE PLAN (docs/rp-motion-port.md). This class FORWARDS:
//   gated intents out as kOpCommand, policy out as kOpConfig, position and
//   events back in. It never plans, never renders, never re-anchors.
// - The drive is SAVED in encoder-follow (0x19=2, gear 4/1, 8192 counts/rev);
//   the RP2350 is the only pulse source. FAS step/dir never drives it again.
// - The link has ONE OWNER TASK: whichever task first runs update(), which is
//   motorTask (Core 1). ONLY the owner may drive the SPI bus. A non-owner
//   caller POSTS -- an atomic flag for estop/clear/ceiling, a fixed-capacity
//   queue for commands and config -- and update() ships it on the next tick.
//   Never a mutex: a bus with two owners is the thing this rule forbids.
// - Commands are shipped ON ARRIVAL from the owner task, never on a tick
//   (architecture.md section 2). kTickMs paces the STATUS POLL only.
// - Cost of the non-owner deferral is ONE kTickMs tick (10 ms) of added estop
//   latency, which SPEC H1 already covers: the protocol ESTOP is a software
//   convenience above the hardware e-stop path, never the guarantee.
// - Native unit = drive input counts, scaled by the runtime geometry
//   (aimStepsPerMm at 8192 steps/rev). Wire commands are NORMALIZED over the
//   stroke window; the arbiter owns that conversion, this class owns time.
// - Anchors convert master -> slave ONCE, at send, through ClockFilter. An
//   unconverged filter ships has_anchor false (plan at arrival), which is the
//   honest degradation.
// See: include/comms/MotionLinkProtocol.h, docs/rp-motion-port.md, sd-4k1.4.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "MotorDriver.h"
#include "CurrentSensor.h"
#include "comms/MotionLinkProtocol.h"

class MlinkServoDriver final : public MotorDriver {
public:
    // THE ONE BUS. Every frame on the link, motion or flash, goes through
    // busXfer: one SPIClass, one lock, one 200 us gap. The flash path may call
    // it only while the owner task stands off (standoff(true) posted by the
    // OTA path, standingOff() acknowledged from the owner's loop).
    static void busXfer(uint8_t (&out)[motionlink::kFrameBytes],
                        uint8_t (&in)[motionlink::kFrameBytes]);
    static void standoff(bool on) { s_standoff.store(on, std::memory_order_release); }
    static bool standoffRequested() { return s_standoff.load(std::memory_order_acquire); }
    static void ackStandoff(bool off) { s_standing_off.store(off, std::memory_order_release); }
    static bool standingOff() { return s_standing_off.load(std::memory_order_acquire); }
    void init() override;
    void update() override;
    void emergencyStop() override;

    bool home(int32_t home_speed_steps_s = 4000) override;
    bool isHomed()  const override { return _homed; }
    bool isLinkUp() const override { return _status_ms != 0; }
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
    // 0x1020 energy: the INA228's hardware accumulator, refreshed at 1 Hz by
    // the stall guard's cadence (one I2C read on motorTask).
    float getBusEnergyWh() const override { return _current.cachedEnergyWh(); }
    void  resetPowerStats() override { _current.resetPeaks(); }
    const SessionOdometer* odometer() const override { return &_odo; }
    void  resetOdometer() override { _odo.reset(); }

    void enable() override;
    void disable() override {}

    void  setMaxSpeed(float mm_s) override { _max_speed_mm_s = mm_s; }
    // Push the renderer's speed ceiling to the RP (kOpSetLimits). The
    // coprocessor is OPEN LOOP: without a ceiling it commands whatever the
    // trajectory asks and the drive silently drops what it cannot follow.
    // A FAULT DETECTOR, never a shaper -- the engine's ceilings ride kOpConfig.
    void  setRenderCeiling(float mm_s) override;
    void  setAcceleration(float mm_s2) override { _accel_mm_s2 = mm_s2; }
    float getMaxSpeed() const override { return _max_speed_mm_s; }
    float getAcceleration() const override { return _accel_mm_s2; }
    uint32_t getLiveAcceleration() const override { return _last_accel_native; }

    bool  isMoving() override { return _state == motionlink::kStateRunning; }
    // The RP's rendered position IS the machine's position (docs/rp-motion-
    // port.md). There is no second model to disagree with it, so getPosition
    // and the base's getActualPosition are one number; EncoderValidator is
    // what audits it against the drive encoder.
    float getPosition() const override;
    float getTargetPosition() const override;

    void applyDriverConfig(const DriverConfig&) override {}
    void     setBlendMode(uint8_t m) override { _blend = m; }
    uint8_t  getBlendMode() const override { return _blend; }

    int32_t mmToNative(float mm) const override;
    float   nativeToMm(int32_t native) const override;

    float getMeasuredStrokeMm() const override { return _stroke_mm; }
    void  setMeasuredStrokeMm(float mm) override { _stroke_mm = mm; }

    // ---- Link telemetry, for the S3-side publishers -------------------------
    // Status fields the hub and the plan strip read. All owner-written, read
    // by Core 0 as aligned scalars: a torn set across fields is a display
    // artifact, never a control input.
    const motionlink::StatusV2& status() const { return _status; }
    uint32_t statusAgeMs() const;
    // Last kEvtPlanAdopted, in MASTER microseconds (the filter's inverse), so
    // the plan strip's elapsed is measured in the clock the UI already uses.
    struct PlanEvent {
        uint32_t due_master_us = 0;
        int32_t  late_us = 0;
        float    target = 0.0f;
        uint32_t duration_us = 0;
        bool     valid = false;
    };
    PlanEvent lastPlan() const { return _last_plan; }
    // Pulled engine/link events, drained by the Core-1 caller into the
    // SlopSync anomaly feed. Returns false when the ring is empty.
    bool popEvent(motionlink::EventRecord& out);

protected:
    void sendCommand(const motionlink::LinkCommand& c) override;
    void pushConfig(uint8_t tag, uint32_t raw) override;
    void stop() override;
    void hardStop() override;

private:
    // True only on the owner task; false before the first update() too, so
    // the setup-task seed defers to the first tick like any other poster.
    bool isOwner() const;
    // Owner-task only: ask the RP its firmware string; answered on the next poll.
    void requestVersion();
    const char* rpFirmware() const { return _rp_fw; }
    void pushCeiling(float mm_s);
    void xfer(uint8_t (&out)[motionlink::kFrameBytes],
              uint8_t (&in)[motionlink::kFrameBytes]);
    void sendOp(uint8_t op);
    void sendRetarget();
    // Ships one command frame and files it against the config image; anchors
    // are already in slave time by the time this runs.
    void shipCommand(const motionlink::LinkCommand& c);
    void shipConfig(uint8_t tag, uint32_t raw);
    void drainPosts();
    void pumpEvents();
    void pollStatus();
    // One parse for every reply, by variant byte (see consumeReply).
    void consumeReply(std::span<const uint8_t, motionlink::kFrameBytes> reply);
    void applyStatus(const motionlink::StatusV2& s);
    void applyEvent(const motionlink::EventRecord& e);
    bool sendSetPos(float counts);
    bool sweepToStall(float dir, float speed_mm_s, float bound_mm,
                      float& pos_out, bool retry_once = false);
    void glideTo(float counts, float speed_mm_s, uint32_t max_ms);
    bool homingAbort(const char* what);

    // Link state (owner task only)
    // TaskHandle_t as void*: keeps the FreeRTOS headers out of this header.
    std::atomic<void*> _owner{nullptr};
    static inline std::atomic<bool> s_standoff{false};
    static inline std::atomic<bool> s_standing_off{false};
    uint8_t  _seq = 0;
    bool     _begun = false;
    uint32_t _last_tick_ms = 0;
    uint32_t _last_cmd_ms = 0;

    // Last CRC-valid v2 status. A v1 slave never writes the variant byte, so
    // a v1 reply parses as variant 0 and is DISCARDED rather than misread.
    motionlink::StatusV2 _status{};
    uint32_t _status_ms = 0;   // millis() at the last CRC-valid v2 status
    bool     _status_fresh = false;
    uint32_t _reply_crc_bad = 0;
    // Stall guard (sd-4k1.29): sustained bus current with no commanded motion
    // is the drive pushing into something. Relief is an unwind against the
    // last motion direction until the current drops; no bus, no encoder.
    uint32_t _stall_since_ms = 0;
    bool     _relieving = false;
    bool     _relief_flipped = false;
    uint32_t _relief_t0 = 0;
    float    _relief_from = 0.0f;
    float    _relief_dir = 0.0f;
    // Where the demand was when the current first crossed the trip level: the
    // shaft is pinned somewhere along the demand's path since then, so the
    // unwind goes back toward that point. Tracked through homing too.
    bool     _press_on = false;
    float    _press_from = 0.0f;
    uint8_t  _guard_div = 0;
    uint8_t  _energy_div = 0;
    void stallGuard(uint32_t now);
    // kOpFlashVersion was sent; the NEXT reply carries the RP fw string over
    // the status tail (kFlashStatusOffVersion). The C-8 instrument for the
    // coprocessor, logged under mlink so the deploy script can read it.
    bool     _ver_pending = false;
    uint8_t  _ver_tries = 0;
    char     _rp_fw[motionlink::kFlashVersionBytes + 1] = {};
    uint8_t  _state = 0;
    uint8_t  _slave_flags = 0;
    // Per-interval counters accumulate into lifetime totals HERE: the v2
    // status resets them on preload, so the master is the only place a total
    // can live (MotionLinkProtocol.h, StatusV2).
    uint32_t _tot_qdrops = 0, _tot_overrun = 0, _tot_late = 0, _tot_clamped = 0;
    uint32_t _tot_link_errs = 0;
    int16_t  _rc_res_max = 0;
    uint32_t _rc_ms = 0;
    uint32_t _rc_qd = 0, _rc_ov = 0, _rc_lt = 0, _rc_vc = 0, _rc_le = 0;

    // Master-side clock estimate; every reply feeds it (no clock op exists).
    motionlink::ClockFilter _clock;
    // Clock samples are paired BY SEQ, never by position: the slave preloads
    // its reply after processing a frame, so a reply is one transaction behind
    // its request (MotionLinkProtocol.h, ClockFilter). Four slots cover that
    // lag with room for the flash and event frames that interleave.
    struct Stamp { uint32_t t0 = 0; uint32_t t3 = 0; uint8_t seq = 0; bool ok = false; };
    std::array<Stamp, 4> _stamp{};

    // The applied-config image. Its fingerprint against status config_fp is
    // the LOST-MIDDLE-FRAME detector: a counter cannot see a dropped middle
    // push, a checksum of the whole set can.
    motionlink::ConfigImage _cfg;
    uint32_t _fp_mismatch_ms = 0;
    uint8_t  _resync_at = 0;   // index of the next tag to re-ship on mismatch

    // Event pull bookkeeping.
    uint8_t _evt_acked = 0;
    bool    _evt_more = false;
    PlanEvent _last_plan{};

    // Cross-task post queues. Fixed capacity, drop-oldest-caller (the newest
    // command wins under retarget semantics), drained by the owner in arrival
    // order. Never a mutex: see the header note.
    // TaskHandle_t-free: plain arrays plus atomic indices, SPSC per producer
    // core, which is what the estop/ceiling posts already do.
    static constexpr uint8_t kPostDepth = 8;
    std::array<motionlink::LinkCommand, kPostDepth> _cmd_post{};
    std::atomic<uint8_t> _cmd_head{0}, _cmd_tail{0};
    std::array<motionlink::ConfigField, kPostDepth> _cfg_post{};
    std::atomic<uint8_t> _cfgp_head{0}, _cfgp_tail{0};

    // Pulled events, owner writes, Core-1 caller drains.
    static constexpr uint8_t kEventDepth = 16;
    std::array<motionlink::EventRecord, kEventDepth> _evt_ring{};
    std::atomic<uint8_t> _evt_head{0}, _evt_tail{0};

    // Last ceiling pushed via kOpSetLimits; re-pushed when an RP restart is
    // detected -- the RP holds it in RAM and boots unlimited without it.
    float    _ceiling_mm_s = 0.0f;
    // Ceiling posted by a non-owner task (WebUI settings, Core 0); >0 means
    // the owner owes it a kOpSetLimits on its next tick.
    std::atomic<float> _ceiling_req{0.0f};

    // Retarget shadow. HOMING ONLY: the sweeps and glides are S3-driven point
    // moves in counts, below the engine, and they stay that way (the port
    // contract's homing row). Refreshed every kRefreshMs as drop insurance.
    float _rt_target = 0.0f;
    float _rt_v = 0.0f;
    float _rt_a = 0.0f;
    bool  _rt_valid = false;
    bool  _rt_dirty = false;
    uint8_t _rt_seq = 0;
    bool    _rt_unacked = false;
    // stop()/hardStop() land ONE retarget: refreshed until acked, then dropped.
    // A standing refresh outlives the frame it was aimed in: an RP restart
    // zeroes the count and the refresh becomes a real move (2026-09-13
    // incident, sd-4k1.28).
    bool     _rt_oneshot = false;

    // Posted from any task, shipped by the owner (see the header note).
    std::atomic<bool> _estop_pending{false};
    std::atomic<bool> _clear_pending{false};

    bool     _homed = false;
    bool     _homing = false;
    CurrentSensor _current;    // INA228 on the carrier (behind the ISO1640)
    SessionOdometer _odo;      // fed once per RP status in applyStatus()
    uint8_t  _blend = 1;
    float    _max_speed_mm_s = 0.0f;
    float    _accel_mm_s2 = 0.0f;
    float    _stroke_mm = 0.0f;
    uint32_t _last_accel_native = 0;
};
