#pragma once

#if defined(DRIVER_AIM_SERVO) && defined(FEATURE_RS485_MODBUS)

#include <Arduino.h>
#include "MotorDriver.h"
#include "config_api.h"
#include "ServoMotionExecutor.h"
#include "CurrentSensor.h"

class ServoModbus;

// ============================================================================
// ModbusServoDriver — FAS-bypass direct-drive backend over RS485 Modbus
// ============================================================================
//
// Phase 3 (see plan.md "Phases"): real motion. Streamed 0x7B setpoints via
// StreamedSetpointExecutor, gated behind the SAME homed/enabled discipline
// FAS mode uses — nothing here ever commands motion on its own; the arbiter
// is still the sole caller (MotorDriver.h friend grant), this driver just
// turns arbiter intents into TrapezoidProfiles and hands them to the
// executor. Homing itself (both styles) is Phase 4 — this phase only has the
// BENCH forceHomeState() path to get outputs live for bring-up.
//
// Native unit: ENCODER COUNTS, not FAS steps. mmToNative()/nativeToMm() use
// AIM_ENC_COUNTS_PER_MM (config_api.h) — the wire unit of BOTH the 0x7B
// setpoint frame and the feedback registers 0x16/0x17, and it's fixed
// silicon (32768 counts/motor-rev), independent of the e-gear register 0x0B.
// A Configure-pane e-gear change can never rescale Modbus-mode motion the way
// it rescales FAS mode's steps/mm.
//
// Virtual mapping ("cmd frame" below = this driver's own native-counts
// frame, home=0, front=NEGATIVE — identical sign convention to the arbiter's
// dispatch and to AIMServoDriver's FAS steps):
//   - Wire mapping: wire_counts = _wire_offset + _wire_sign * cmd_counts.
//     _wire_offset is the encoder reading captured at (force-)home time;
//     _wire_sign is AIM_MODBUS_WIRE_SIGN (config_api.h, bench-determined).
//   - streamToSteps()/moveTo()/streamTo() plan a TrapezoidProfile from the
//     executor's OWN live commanded pos/vel (never a stale target) and adopt
//     it — one plan per intent, the executor just samples it (CLAUDE.md §2).
//   - getPosition()/getTargetPosition() read the executor's commanded sample
//     — "commanded = truth," open-loop, exactly like AIMServoDriver's FAS
//     position readback (plan.md "Position truth" doctrine).
class ModbusServoDriver : public MotorDriver {
public:
    explicit ModbusServoDriver(ServoModbus& bus);

    // ---- Lifecycle ----------------------------------------------------------
    void init()           override;
    void update()          override;   // INA228 cache + bus-health watchdog (motorTask, 1ms)
    void emergencyStop()   override;

    // Public thin wrapper so main.cpp's servoBusTask (Core 1, 2ms) can drive
    // the executor's tick without reaching into a protected/private member —
    // this is NOT a motion-dispatch entry point (it doesn't plan anything,
    // just samples whatever profile is already adopted), so it sits outside
    // the sole-caller lock deliberately. :3
    void executorTick(int64_t now_us) { _executor.onTick(now_us); }

    // ---- Homing ---------------------------------------------------------------
    // Refuses until Phase 4 (both homing styles land there).
    bool home(int32_t home_speed_steps_s = 4000) override;
    void runHomingStep()   override {}
    bool isHomed()  const  override { return _homed; }
    bool isHoming() const  override { return _homing; }
    bool checkPushToHome() override { return false; }

    // Bench/test override — mirrors AIMServoDriver::forceHomeState() but over
    // the wire: reads the LIVE encoder position (regs 0x16/0x17 via
    // ServoModbus::getTelemetry()) and adopts it as the wire-mapping zero, so
    // the very first setpoint sent is motionless (plan.md hard safety
    // requirement). Refuses if the bus has never delivered a valid encoder
    // sample — no encoder reading means no safe zero to adopt. THIS IS THE
    // BENCH PATH (HOME_OVERRIDE) — do NOT call on a machine you're not
    // physically watching. Real homing (both styles) lands in Phase 4. :3
    void forceHomeState(bool homed) override;

    // ---- Position monitor -----------------------------------------------------
    void runMotorStep()    override {}

protected:
    // ---- Motion (MotionArbiter-only — sole-caller rule, see MotorDriver.h) ----
    bool moveTo(float pos_mm) override;
    void streamTo(float pos_mm, float speed_mm_s) override;
    void streamToSteps(int32_t target_steps,
                       uint32_t speed_steps_s,
                       uint32_t accel_steps_s2) override;
    void stop()      override;
    void hardStop()  override;

public:
    // No enable pin over Modbus either — "enable" here means the drive's
    // output-enable register (0x01), queued (thread-safe from any caller
    // core) rather than written directly off whatever task happens to call
    // this. :3
    void enable()    override;
    void disable()   override;

    // ---- Speed & Acceleration -------------------------------------------------
    void     setMaxSpeed(float speed_mm_s)      override;
    void     setAcceleration(float accel_mm_s2) override;
    float    getMaxSpeed()          const       override { return _max_speed_mm_s; }
    float    getAcceleration()      const       override { return _accel_mm_s2; }
    // Live accel currently driving the adopted profile (0 while idle/frozen)
    // — mirrors AIMServoDriver's stepper->getAcceleration() raise-only-guard
    // read, in counts/s² instead of steps/s². :3
    uint32_t getLiveAcceleration()  const       override { return (uint32_t)_executor.liveAccel(); }

    // ---- Status -----------------------------------------------------------------
    bool  isMoving()            override { return _executor.active(); }
    float getPosition()   const override;
    float getTargetPosition() const override;

    // ---- Driver config ------------------------------------------------------------
    // Writes the MINIMAL Modbus-mode register set (output state + torque
    // clamp) — no PID/gain writes, those stay on the Configure pane
    // (handleApiServo talks to ServoModbus directly). :3
    void applyDriverConfig(const DriverConfig& cfg) override;

    // ---- Diagnostics --------------------------------------------------------------
    uint16_t getCurrentmA()  override;
    uint8_t  getMicrosteps() override { return 0; }   // no step/dir side on this backend

    // ---- Continuous-blend tuning ---------------------------------------------------
    // Store-only — the streamed executor doesn't have a blend policy of its
    // own yet (it always retargets, same as AIMServoDriver's mode 2).
    void    setBlendMode(uint8_t mode) override { _blend_mode = mode; }
    uint8_t getBlendMode() const       override { return _blend_mode; }

    // ---- Unit conversion (counts, not steps — see class doc) ------------------------
    int32_t mmToNative(float mm)        const override;
    float   nativeToMm(int32_t native)  const override;
    float   nativePerMm()               const override { return AIM_ENC_COUNTS_PER_MM; }

    // ---- INA228 telemetry (identical pattern to AIMServoDriver) ---------------------
    float getBusCurrentA()  const override { return _current.cachedCurrentA(); }
    float getBusVoltageV()  const override { return _current.cachedBusV(); }
    bool  hasCurrentSensor() const override { return _current.isReady(); }
    float getBusPowerW()      const override { return _current.cachedPowerW(); }
    float getDieTempC()       const override { return _current.cachedDieTempC(); }
    float getPeakBusCurrentA() const override { return _current.getPeakCurrentA(); }
    bool  hasPowerMonitor()   const override { return _current.isReady(); }
    float getBusEnergyWh()    const override { return _current.cachedEnergyWh(); }
    void  resetPowerStats()   override { _current.resetPeaks(); }

private:
    ServoModbus& _bus;

    // The executor IS the "ISR" for this backend — see ServoMotionExecutor.h.
    // Constructed bound to the same bus reference the driver itself holds. :3
    StreamedSetpointExecutor _executor;

    // ---- Wire mapping (see class doc) ------------------------------------------
    int32_t _wire_offset = 0;
    int8_t  _wire_sign   = AIM_MODBUS_WIRE_SIGN;

    // ---- Homed/enabled/fault gates — nothing moves unless ALL clear -----------
    bool _homed   = false;
    bool _homing  = false;
    bool _enabled = false;
    // Latched by the update() bus-health watchdog; clears ONLY via
    // forceHomeState()/home() — a recovered bus does not, on its own,
    // re-trust a link that just dropped ~3-15 setpoint echoes in a row. :3
    bool _bus_fault = false;
    // One-shot log latches for the two watchdog tiers (freeze / e-stop) —
    // reset when the fail-streak drops back under the freeze threshold so a
    // LATER fault episode logs again, without spamming while one streak
    // stays elevated. :3
    bool _fault_freeze_logged = false;
    bool _fault_estop_logged  = false;

    float _max_speed_mm_s = MAX_SPEED_MM_S;
    float _accel_mm_s2    = DEFAULT_ACCEL_MM_S2;
    uint8_t _blend_mode   = 1;

    // Last commanded target, cmd-frame counts (float) — getTargetPosition()
    // reads this; the executor itself only tracks the SAMPLED position, not
    // the profile's ultimate target. :3
    float _target_counts = 0.0f;

    // ---- streamToSteps() grit-cache (plan.md — MUST be the first check) -------
    // Called up to ~1kHz from the stream sampler; skip re-planning entirely
    // when target/speed/accel are byte-identical to last time. :3
    bool     _have_last_stream    = false;
    int32_t  _last_target_steps   = 0;
    uint32_t _last_speed_steps_s  = 0;
    uint32_t _last_accel_steps_s2 = 0;

    // ---- INA228 current sensor (identical pattern to AIMServoDriver) ----------
    CurrentSensor _current;
    uint32_t _last_current_poll_ms      = 0;   // fast poll (current+busV, 40Hz)
    uint32_t _last_current_full_poll_ms = 0;   // full poll (temp/energy/shunt, 1Hz)
};

#endif // defined(DRIVER_AIM_SERVO) && defined(FEATURE_RS485_MODBUS)
