#pragma once

#if defined(DRIVER_AIM_SERVO) && defined(FEATURE_RS485_MODBUS)

#include <cstdint>
#include "freertos/FreeRTOS.h"

class ServoModbus;

// ============================================================================
// IServoExecutor — pluggable motion-execution strategy for ModbusServoDriver
// ============================================================================
//
// The executor is the "ISR" for Modbus-mode motion. Current design (see
// StreamedSetpointExecutor below): callers move a target via track(), and
// onTick() integrates its own (pos, vel, acc) state toward that target every
// tick under vmax/amax/jmax, streaming FC 0x10 incremental deltas.
class IServoExecutor {
public:
    virtual ~IServoExecutor() = default;

    // Advance the executor by one tick: sample the active profile (or hold
    // the last sample if frozen/done), map to wire units, and stream a
    // setpoint if the schedule calls for it. Called from servoBusTask,
    // Core 1, every 2ms — BEFORE servoModbus.update() so a setpoint always
    // gets first crack at an IDLE bus (main.cpp "setpoint-first priority"). :3
    virtual void onTick(int64_t now_us) = 0;

    // Last SAMPLED position/velocity — this IS "commanded = truth" (plan.md
    // "Position truth" doctrine). ModbusServoDriver::getPosition() and
    // getLiveAcceleration() read straight off these. :3
    virtual float commandedPos() const = 0;
    virtual float commandedVel() const = 0;
    virtual float liveAccel()    const = 0;

    // True while a profile is actively generating motion — not done, not
    // frozen, not merely idle-holding. Drives MotorDriver::isMoving(). :3
    virtual bool active() const = 0;

    // Latch the current sample as a hold position and invalidate the active
    // profile. Used by hardStop()/stop()/emergencyStop() and the bus-health
    // watchdog — after this call the executor keeps streaming the SAME
    // position (keep-alive cadence) until a fresh track() target. :3
    virtual void freeze() = 0;

    // Establish the FIRST motionless sample, in the driver's native cmd-frame
    // (home=0, front=negative — same units/sign as track()'s target, NOT raw
    // wire counts). Nothing is EVER sent to the bus before
    // this runs — the hard safety requirement behind "first bench step: send
    // current encoder position as setpoint, observe zero motion" (plan.md). :3
    virtual void seed(float cmd_pos) = 0;
};

// ============================================================================
// StreamedSetpointExecutor — jerk-limited target tracker, FC 0x10 delta stream
// ============================================================================
//
// track() moves the target; onTick() (every servoBusTask tick, 2ms) integrates
// this executor's own (pos, vel, acc) toward it under vmax/amax/jmax and
// streams the result as FC 0x10 incremental position deltas via
// ServoModbus::sendPositionDelta(). Idle (done or frozen) drops to a
// keep-alive cadence, re-sending a zero delta — both "the drive holds last
// commanded position if we stop talking" and a passive bus-liveness probe.
//
// Deadline-scheduled, not a fixed-phase timer: onTick() only advances its
// "last sent" mark on an ACTUAL send, so a busy bus never drifts the schedule
// forward past its true period.
class StreamedSetpointExecutor : public IServoExecutor {
public:
    explicit StreamedSetpointExecutor(ServoModbus& bus);

    void onTick(int64_t now_us) override;

    float commandedPos() const override;
    float commandedVel() const override;
    float liveAccel()    const override;
    bool  active()        const override;

    void freeze() override;
    void seed(float cmd_pos) override;

    // ---- JERK-LIMITED TARGET TRACKER (operator decision, bench night 1) -----
    // OSSM-RS parity architecture: every motion source just MOVES THE TARGET;
    // this executor glides toward it under vmax/amax/jmax limits, integrating
    // its own (pos, vel, acc) state every servoBusTask tick and streaming the
    // result as FC 0x10 deltas. This replaced per-intent TrapezoidProfile
    // sampling because (a) trapezoids carry jerk spikes at every accel
    // transition and (b) the 1kHz stream path re-planned a fresh trapezoid
    // toward every interpolator waypoint — trapezoid confetti, felt as the
    // "clocked waypoint" roughness. OSSM-RS streams Ruckig S-curve output;
    // this tracker is the same idea in ~20 lines. Doctrine framing: intent ->
    // target update (the "plan" IS the target + limits); the tracker is the
    // executor/ISR. :3
    void track(float target_counts, float vmax_counts_s, float amax_counts_s2);
    void setJerkLimit(float jmax_counts_s3);

    // Wire mapping: wire_counts = offset + sign * cmd_pos. `offset` is the
    // encoder reading captured at home/force-home time; `sign` (+1/-1) is
    // AIM_MODBUS_WIRE_SIGN, bench-determined (config_api.h). Set by the
    // driver whenever it (re)establishes home. :3
    void setWireMap(int32_t offset, int8_t sign);

private:
    ServoModbus& _bus;

    // Every mutable field below is guarded by this spinlock — same pattern
    // ServoModbus itself uses for its cross-core telemetry (_mux). Entered
    // for a handful of assignments only, NEVER held across UART I/O. :3
    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

    // Tracker target + limits (guarded; updated up to 1kHz by track()).
    float   _target   = 0.0f;
    float   _vmax     = 1.0f;
    float   _amax     = 1.0f;
    float   _jmax     = 1.0e7f;       // counts/s^3 — set from AIM_MODBUS_JERK_MM_S3
    // Tracker state (guarded; integrated by onTick()).
    float   _trk_acc  = 0.0f;
    bool    _seeded = false;          // guarded — true once seed() has run
    bool    _frozen = false;          // guarded — true after freeze()
    bool    _active = false;          // guarded — last onTick's "moving" verdict
    float   _cmd_pos = 0.0f;          // guarded — tracker position (truth)
    float   _cmd_vel = 0.0f;          // guarded — tracker velocity
    int32_t _wire_offset = 0;         // guarded — see setWireMap()
    int8_t  _wire_sign   = 1;         // guarded

    // Setpoint-send scheduling — only ever touched from onTick(), and onTick()
    // is only ever called from servoBusTask (single task, single core), so
    // these need no lock. Fast cadence whenever the WIRE value would change;
    // keep-alive (zero-delta liveness probe) otherwise. :3
    uint32_t _last_sent_ms   = 0;
    int32_t  _last_sent_wire = 0;
    bool     _have_sent      = false;
    int64_t  _last_tick_us   = 0;     // for the integration dt
};

#endif // defined(DRIVER_AIM_SERVO) && defined(FEATURE_RS485_MODBUS)
