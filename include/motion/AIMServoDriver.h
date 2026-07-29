#pragma once

// AIMServoDriver — concrete MotorDriver for the AIM-class closed-loop servos.
//
// Constraints:
// - Build-guarded behind DRIVER_AIM_SERVO (platformio.ini). Uses
//   FastAccelStepper for pulse generation on ESP32-S3.
// - Targets the whole AIM family of Step/Direction closed-loop servo drives
//   (57AIM30 and functionally-compatible siblings), not one specific motor.
//   Step/Direction only — no SPI, no Modbus, no register config; the AIM
//   drive owns the closed-loop control internally.
// - Pinout (config_api.h, custom v0.0 Nano ESP32 board): AIM_PIN_STEP -> PUL
//   (GPIO 5, D2), pulse train, one step per rising edge; AIM_PIN_DIR -> DIR
//   (GPIO 6, D3). No endstop — homing is SENSORLESS via the INA228 current
//   sensor.
// - Geometry (capstan drum, 2:1 motor->drum reduction): rail-length agnostic,
//   no fixed travel ceiling — the user's max rail length setting bounds the
//   homing sweep, and homing MEASURES the real usable stroke between the two
//   hard stops. STEPS_PER_REV = 1600/drum-rev (800 motor steps x 2:1);
//   MM_PER_REV = pi x 25mm drum = 78.5398 mm/drum-rev; STEPS_PER_MM =~ 20.372;
//   HOMING_BACKOFF = 10.0 mm.
// - No enable pin: the AIM drive is always energized when powered. enable()/
//   disable() are no-ops that satisfy the MotorDriver interface.
// - CONTINUOUS RETARGETING: streamToSteps() never force-stops — every dispatch
//   just retargets FAS's in-flight move, which handles same-direction moves
//   and reversals on its own.
// - _blend_mode is VESTIGIAL (operator ruling 2026-07-27): streamToSteps()
//   does not read it — FAS retargeting handles every case uniformly, which is why MotionArbiter::setBlendMode() already aliases
//   every mode to "allow" (MotionArbiter.cpp). The getter/setter pair stays
//   only because MotorDriver's ABC contract, NVS persistence, and the WebUI
//   HTTP settings JSON still reference it; SlopSync's wire exposure was
//   retired outright (SlopSyncCatalog.h's `blend_mode_reserved`).

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "config_api.h"
#include "MotorDriver.h"
#include "CurrentSensor.h"

// Forward-declared: only the FAS type is needed here, the full header stays
// in the .cpp.
class FastAccelStepper;

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
    // real homing cycle. Enables FAS outputs and establishes a zero reference
    // so streamToSteps() actually emits step/dir pulses to
    // a (possibly disconnected) motor — the whole point of a bench
    // HOME_OVERRIDE. Implementation lives in the .cpp because it has to touch
    // the FastAccelStepper instance. Do NOT call on real hardware you don't
    // want moving without a genuine home.
    void forceHomeState(bool homed) override;
    bool checkPushToHome() override;

    // ---- Position monitor ---------------------------------------------------
    void runMotorStep() override;

protected:
    // ---- Motion (MotionArbiter-only — see MotorDriver.h sole-caller lock) ---
    // Kept protected in the derived class too so the compile-time lock can't be
    // bypassed by holding a concrete AIMServoDriver& instead of a MotorDriver&.

    // Pre-planned native-step dispatch — called from Core 1 via MotionArbiter.
    // Speed and accel arrive already converted to steps/s and steps/s².
    // Fires straight to FAS — no unit math here.
    void streamToSteps(int32_t target_steps,
                       uint32_t speed_steps_s,
                       uint32_t accel_steps_s2) override;

    void stop() override;
    void hardStop() override;

public:
    // No enable pin on the 57AIM30 — always energized when powered.
    // These satisfy the interface but do nothing to hardware.
    void enable()  override;
    void disable() override;

    // ---- Speed & Acceleration -----------------------------------------------
    void     setMaxSpeed(float speed_mm_s) override;
    void     setAcceleration(float accel_mm_s2) override;
    float    getMaxSpeed() const override { return _max_speed_mm_s; }
    // The accel ACTUALLY applied (post the driver's internal 20000 mm/s² clamp)
    // — this is what settings echoes report back, never the raw request.
    float    getAcceleration() const override { return _accel_mm_s2; }

    // Returns the live FAS acceleration — what the ramp engine is actually
    // using right now, not the configured ceiling. Mirrors OSSM's
    // stepper->getAcceleration() call in the raise-only guard.
    uint32_t getLiveAcceleration() const override;

    // ---- Status -------------------------------------------------------------
    bool  isMoving() override;
    float getPosition()       const override;
    float getTargetPosition() const override;

    // ---- Driver config ------------------------------------------------------
    // No driver chip registers to write — the 57AIM30 is configured via its own
    // front-panel DIP switches and parameter software. This is a no-op that
    // satisfies the interface. We accept the struct so the rest of the system
    // (ConfigStore, WebUI) doesn't need to know we're a dumb drive.
    void applyDriverConfig(const DriverConfig& cfg) override;

    // ---- Diagnostics --------------------------------------------------------
    // No SPI readback on a dumb drive. Return the compile-time constants.
    uint16_t getCurrentmA()  override { return 0; }
    uint8_t  getMicrosteps() override { return (uint8_t)(AIM_STEPS_PER_REV / 200); }

    // ---- Continuous-blend tuning (VESTIGIAL) --------------------------------
    // Reversal policy selector (1=let-it-land, 2=allow-reversal, 3=hybrid).
    // Dead: streamToSteps() does not read _blend_mode — see the
    // file-header Constraints note. This accessor pair just keeps the ABC
    // contract / NVS / legacy HTTP JSON compiling. Still clamped to [1,3]
    // purely to keep old callers' NVS round-trip harmless.
    void    setBlendMode(uint8_t mode) { _blend_mode = constrain((int)mode, 1, 3); }
    uint8_t getBlendMode() const       { return _blend_mode; }

    // ---- Unit conversion (static for external use) --------------------------
    int32_t mmToNative(float mm)        const override;
    float   nativeToMm(int32_t native)  const override;

private:
    // Self-contained homing task — spawned by home(), deletes itself when done.
    static void _homingTaskImpl(void* param);
    void        _homingTask();
    // Sweep in one direction (dir_sign +1 rear / -1 front) until an INA228
    // current spike says we've buried the carriage against a hard stop.
    // Returns true on stall, false if the full sweep ran with no wall.
    bool        _sweepToStall(int8_t dir_sign);
    TaskHandle_t _homingTaskHandle = nullptr;


    FastAccelStepper* _stepper = nullptr;

    // INA228 current sensor — the machine's sense of feel. Sensorless homing
    // reads this to know when the carriage has buried itself against the hard
    // stop (current spikes as it strains). Owned by the driver, initialized in
    // init() after the caller has brought up the Wire bus.
    CurrentSensor _current;

    bool    _homed   = false;
    bool    _homing  = false;
    bool    _enabled = false;
    int32_t _home_speed_steps_s = AIM_HOMING_SPEED_STEPS_S;  // 400 steps/s ≈ 20 mm/s

    // Measured usable stroke (mm), discovered by sensorless homing between the
    // two hard stops minus safety margins. 0 = not yet measured → fall back to
    // the configured max rail length (getMaxRailMm()). The WebUI reads this so
    // the stroke designer rescales to the REAL rail length once we've felt both
    // ends.
    float   _measured_stroke_mm = 0.0f;
public:
    // Measured stroke accessor for the WebUI / status layer. Returns 0 until
    // homing has measured both ends.
    float getMeasuredStrokeMm() const override { return _measured_stroke_mm; }
    // Restore a previously-measured stroke from NVS so the rail scale is
    // correct at boot BEFORE the first homing cycle. Homing itself overwrites
    // this with a fresh measurement when it completes.
    void setMeasuredStrokeMm(float mm) override {
        // Sanity bound only — NOT a rail-length clamp (measurement wins). The
        // homing sweep already caps how far a real span can be; this just
        // rejects a garbage value from a corrupt NVS restore.
        if (mm > 0.0f && mm < 2000.0f) _measured_stroke_mm = mm;
    }

    // ---- Live INA228 bus telemetry for the WebUI toolbar --------------------
    // These return the CACHED last reading (no I2C from the HTTP thread). The
    // cache is refreshed by update() on Core 1 at a low rate and by the homing
    // loop while it runs. Lets the operator watch the current live during
    // bring-up and confirm the sensor works before trusting homing.
    float getBusCurrentA()  const override { return _current.cachedCurrentA(); }
    float getBusVoltageV()  const override { return _current.cachedBusV(); }
    bool  hasCurrentSensor() const override { return _current.isReady(); }

    // ---- Extended INA228 power telemetry for the WebUI Health tab -----------
    // Same cached, I2C-free pattern as the current/voltage pair above — safe
    // to call from the Core 0 HTTP handler at any time.
    float getBusPowerW()      const override { return _current.cachedPowerW(); }
    float getDieTempC()       const override { return _current.cachedDieTempC(); }
    float getPeakBusCurrentA() const override { return _current.getPeakCurrentA(); }
    bool  hasPowerMonitor()   const override { return _current.isReady(); }
    float getBusEnergyWh()    const override { return _current.cachedEnergyWh(); }
    void  resetPowerStats()   override { _current.resetPeaks(); }  // clears peaks + INA228 Wh accumulator
private:
    // Throttle for the update()-driven telemetry refresh. We only need the
    // toolbar number a few times a second, not every motion tick.
    uint32_t _last_current_poll_ms = 0;       // fast poll (current+busV, 40Hz)
    uint32_t _last_current_full_poll_ms = 0;  // full poll (temp/energy/shunt, 1Hz)



    float    _max_speed_mm_s      = MAX_SPEED_MM_S;
    float    _accel_mm_s2         = DEFAULT_ACCEL_MM_S2;
    float    _current_position_mm = 0.0f;

    // ---- Stream state -------------------------------------------------------
    // Retired mode byte, still published on 0x1030 byte 0 (wire compatibility;
    // see SlopSyncCatalog.h's `blend_mode_reserved`). Do not consume.
    uint8_t  _blend_mode         = 1;

    // Speed/accel cache for streamToSteps() — only call FAS setters when the
    // value actually changes. Calling them every waypoint forces a ramp recalc
    // mid-flight on every single command, which is the source of gritty motion.
    // Cache starts at 0 so the first call always goes through.
    uint32_t _last_speed_steps_s  = 0;
    uint32_t _last_accel_steps_s2 = 0;

    // Hybrid (mode 3): below this remaining in-flight distance we allow the
    // reversal instead of letting the stroke finish. ~1.5mm worth of steps.
    // Uses the compile-time DEFAULT steps/mm (in-class constant needs a
    // constant expression); currently unused by the .cpp.
    static const int32_t BLEND_REVERSAL_THRESHOLD_STEPS = (int32_t)(1.5f * AIM_STEPS_PER_MM_DEFAULT);
};
