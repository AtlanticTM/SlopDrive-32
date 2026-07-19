#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "config_api.h"
#include "MotorDriver.h"
#include "CurrentSensor.h"


// Forward declarations — we only need the FAS types here, full headers in .cpp
class FastAccelStepper;

// ============================================================================
// AIMServoDriver — concrete MotorDriver for the AIM-class closed-loop servos
// ============================================================================
//
// Build-guarded behind DRIVER_AIM_SERVO (set in platformio.ini).
// Uses FastAccelStepper for pulse generation on ESP32-S3.
//
// This driver targets the whole AIM family of Step/Direction closed-loop servo
// drives (57AIM30 and functionally-compatible siblings), NOT one specific
// motor. It's a "dumb" Step/Direction driver — no SPI, no Modbus, no register
// config. The AIM drive handles all the closed-loop magic internally. We just
// send PUL (pulse) and DIR (direction) signals and let the drive do its thing.
// Clean, simple, and absolutely relentless. :3
//
// Hardware pinout (defined in config_api.h — custom v0.0 Nano ESP32 board):
//   AIM_PIN_STEP → PUL (GPIO 5, D2) — pulse train, one step per rising edge
//   AIM_PIN_DIR  → DIR (GPIO 6, D3) — direction signal
//   NO ENDSTOP   → homing is SENSORLESS via the INA228 current sensor. :3
//
// Machine geometry (capstan drum, 2:1 motor->drum reduction):
//   MAX_TRAVEL:    rail-length agnostic — no fixed ceiling. The user's max rail
//                  length setting bounds the homing sweep; homing MEASURES the
//                  real usable stroke between the two hard stops.
//   STEPS_PER_REV: 1600 per DRUM rev (800 motor steps × 2:1 reduction)
//   MM_PER_REV:    π × 25mm drum = 78.5398 mm/drum-rev
//   STEPS_PER_MM:  1600 / 78.5398 = ~20.372 steps/mm
//   HOMING_BACKOFF: 10.0 mm

//
// No enable pin — the AIM drive is always energized when powered. The driver
// enable/disable calls are no-ops that satisfy the MotorDriver interface.
//
// CONTINUOUS BLENDING: streamTo() never softens a committed brake ramp
// (raise-only accel) and handles same-direction vs. reversal per the
// selectable _blend_mode. No stop-and-go stutter between waypoints — we keep
// pounding through the stream instead of edging to a dead stop on every
// sample. The shaft just keeps thrusting, relentless and full, stuffed all
// the way in until the belly bulges and it can't take anymore yippie! :3

class AIMServoDriver : public MotorDriver {
public:
    AIMServoDriver();

    // ---- Lifecycle ----------------------------------------------------------
    void init() override;
    void update() override;
    void emergencyStop() override;

    // ---- Homing -------------------------------------------------------------
    bool home(int32_t home_speed_steps_s = AIM_HOMING_SPEED_STEPS_S) override;
    void runHomingStep() override;
    bool isHomed()  const override { return _homed; }
    bool isHoming() const override { return _homing; }

    // Force the driver's internal homed flag for bench/remote testing WITHOUT a
    // real homing cycle. Unlike the old inline flag-flip, this also enables FAS
    // outputs and establishes a zero reference so moveTo()/streamTo()/
    // streamToSteps() actually emit step/dir pulses to a (possibly disconnected)
    // motor — the whole point of a bench HOME_OVERRIDE. Implementation lives in
    // the .cpp because it has to touch the FastAccelStepper instance. Do NOT
    // call on real hardware you don't want moving without a genuine home. :3
    void forceHomeState(bool homed) override;
    bool checkPushToHome() override;

    // ---- Position monitor ---------------------------------------------------
    void runMotorStep() override;

    // ---- Motion -------------------------------------------------------------
    void moveTo(float pos_mm) override;
    void streamTo(float pos_mm, float speed_mm_s) override;

    // Pre-planned native-step dispatch — called from Core 1 motionConsumerTask.
    // Speed and accel arrive already converted to steps/s and steps/s².
    // Fires straight to FAS — no unit math here. The shaft gets told exactly
    // where to go and it goes there, no questions asked, no hesitation. :3
    void streamToSteps(int32_t target_steps,
                       uint32_t speed_steps_s,
                       uint32_t accel_steps_s2) override;

    void stop() override;
    void hardStop() override;

    // No enable pin on the 57AIM30 — always energized when powered.
    // These satisfy the interface but do nothing to hardware. :3
    void enable()  override;
    void disable() override;

    // ---- Speed & Acceleration -----------------------------------------------
    void     setMaxSpeed(float speed_mm_s) override;
    void     setAcceleration(float accel_mm_s2) override;
    float    getMaxSpeed() const override { return _max_speed_mm_s; }

    // Returns the live FAS acceleration — what the ramp engine is actually
    // using right now, not the configured ceiling. Mirrors OSSM's
    // stepper->getAcceleration() call in the raise-only guard. :3
    uint32_t getLiveAcceleration() const override;

    // ---- Status -------------------------------------------------------------
    bool  isMoving() override;
    float getPosition()       const override;
    float getTargetPosition() const override;

    // ---- Driver config ------------------------------------------------------
    // No TMC registers to write — the 57AIM30 is configured via its own
    // front-panel DIP switches and parameter software. This is a no-op that
    // satisfies the interface. We accept the struct so the rest of the system
    // (ConfigStore, WebUI) doesn't need to know we're a dumb drive. :3
    void applyDriverConfig(const DriverConfig& cfg) override;

    // ---- Diagnostics --------------------------------------------------------
    // No SPI readback on a dumb drive. Return the compile-time constants.
    uint16_t getCurrentmA()  override { return 0; }
    uint8_t  getMicrosteps() override { return (uint8_t)(AIM_STEPS_PER_REV / 200); }

    // ---- Continuous-blend tuning --------------------------------------------
    // Blend mode picks how streamTo() handles a new waypoint that reverses
    // direction while a stroke is still in flight:
    //   1 = let-it-land (DEFAULT) — finish the current stroke, ignore the
    //       reversing retarget. Smoothest; may drop a waypoint at extreme Hz.
    //   2 = allow-reversal        — retarget immediately; FAS does a clean
    //       decel→reverse. Tighter tracking, one decel per true turnaround.
    //   3 = hybrid                — let-it-land only while the remaining
    //       in-flight distance is large; otherwise allow the reversal.
    void    setBlendMode(uint8_t mode) { _blend_mode = constrain((int)mode, 1, 3); }
    uint8_t getBlendMode() const       { return _blend_mode; }

    // ---- Unit conversion (static for external use) --------------------------
    int32_t mmToNative(float mm)        const override;
    float   nativeToMm(int32_t native)  const override;

private:
    // Clears all streaming / blend / target-monitor state so that a stale
    // stream from before a Halt/Home can't keep issuing moveTo() commands
    // that fight a fresh homing cycle. Clean slate, ready to be filled again. :3
    void resetStreamState();

    // Self-contained homing task — spawned by home(), deletes itself when done.
    // Mirrors StrokeEngine's _homingProcedure pattern exactly. :3
    static void _homingTaskImpl(void* param);
    void        _homingTask();
    // Sweep in one direction (dir_sign +1 rear / -1 front) until an INA228
    // current spike says we've buried the carriage against a hard stop.
    // Returns true on stall, false if the full sweep ran with no wall. :3
    bool        _sweepToStall(int8_t dir_sign);
    TaskHandle_t _homingTaskHandle = nullptr;


    FastAccelStepper* _stepper = nullptr;

    // INA228 current sensor — the machine's sense of feel. Sensorless homing
    // reads this to know when the carriage has buried itself against the hard
    // stop (current spikes as it strains). Owned by the driver, initialised in
    // init() after the caller has brought up the Wire bus. :3
    CurrentSensor _current;

    bool    _homed   = false;
    bool    _homing  = false;
    bool    _enabled = false;
    int32_t _home_speed_steps_s = AIM_HOMING_SPEED_STEPS_S;  // 400 steps/s ≈ 20 mm/s

    // Measured usable stroke (mm), discovered by sensorless homing between the
    // two hard stops minus safety margins. 0 = not yet measured → fall back to
    // the configured max rail length (getMaxRailMm()). The WebUI reads this so
    // the stroke designer rescales to the REAL rail length once we've felt both
    // ends. :3
    float   _measured_stroke_mm = 0.0f;
public:
    // Measured stroke accessor for the WebUI / status layer. Returns 0 until
    // homing has measured both ends. :3
    float getMeasuredStrokeMm() const override { return _measured_stroke_mm; }
    // Restore a previously-measured stroke from NVS so the rail scale is
    // correct at boot BEFORE the first homing cycle. Homing itself overwrites
    // this with a fresh measurement when it completes. :3
    void setMeasuredStrokeMm(float mm) override {
        // Sanity bound only — NOT a rail-length clamp (measurement wins). The
        // homing sweep already caps how far a real span can be; this just
        // rejects a garbage value from a corrupt NVS restore. :3
        if (mm > 0.0f && mm < 2000.0f) _measured_stroke_mm = mm;
    }

    // ---- Live INA228 bus telemetry for the WebUI toolbar --------------------
    // These return the CACHED last reading (no I2C from the HTTP thread). The
    // cache is refreshed by update() on Core 1 at a low rate and by the homing
    // loop while it runs. Lets the operator watch the current live during
    // bring-up and confirm the sensor works before trusting homing. :3
    float getBusCurrentA()  const override { return _current.cachedCurrentA(); }
    float getBusVoltageV()  const override { return _current.cachedBusV(); }
    bool  hasCurrentSensor() const override { return _current.isReady(); }

    // ---- Extended INA228 power telemetry for the WebUI Health tab -----------
    // Same cached, I2C-free pattern as the current/voltage pair above — safe
    // to call from the Core 0 HTTP handler at any time. :3
    float getBusPowerW()      const override { return _current.cachedPowerW(); }
    float getDieTempC()       const override { return _current.cachedDieTempC(); }
    float getPeakBusCurrentA() const override { return _current.getPeakCurrentA(); }
    bool  hasPowerMonitor()   const override { return _current.isReady(); }
private:
    // Throttle for the update()-driven telemetry refresh. We only need the
    // toolbar number a few times a second, not every motion tick. :3
    uint32_t _last_current_poll_ms = 0;       // fast poll (current+busV, 40Hz)
    uint32_t _last_current_full_poll_ms = 0;  // full poll (temp/energy/shunt, 1Hz)



    float    _max_speed_mm_s      = MAX_SPEED_MM_S;
    float    _accel_mm_s2         = DEFAULT_ACCEL_MM_S2;
    float    _current_position_mm = 0.0f;

    // ---- Continuous-blend / stream state ------------------------------------
    // Blend mode: 1=let-it-land (default), 2=allow-reversal, 3=hybrid.
    uint8_t  _blend_mode         = 1;
    // Last commanded target + its direction sign, so streamTo() can detect a
    // reversal vs. the in-flight move and apply the selected blend policy.
    int32_t  _last_target_steps  = 0;
    int8_t   _last_dir           = 0;       // -1, 0, +1 (native-step sign)
    bool     _have_last_target   = false;
    // Watchdog: if the host stops talking dirty to us, settle on the last real
    // sample so the carriage can't keep coasting toward a stale target. :3
    float    _last_sample_mm     = 0.0f;
    uint32_t _last_sample_ms     = 0;
    bool     _have_last_sample   = false;
    static const uint16_t STREAM_STALL_MS = 80;

    // Speed/accel cache for streamToSteps() — only call FAS setters when the
    // value actually changes. Calling them every waypoint forces a ramp recalc
    // mid-flight on every single command, which is the source of gritty motion.
    // Cache starts at 0 so the first call always goes through. :3
    uint32_t _last_speed_steps_s  = 0;
    uint32_t _last_accel_steps_s2 = 0;

    // Hybrid (mode 3): below this remaining in-flight distance we allow the
    // reversal instead of letting the stroke finish. ~1.5mm worth of steps.
    static const int32_t BLEND_REVERSAL_THRESHOLD_STEPS = (int32_t)(1.5f * AIM_STEPS_PER_MM);
};
