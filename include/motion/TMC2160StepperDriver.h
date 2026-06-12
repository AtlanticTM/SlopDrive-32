#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "config_api.h"
#include "MotorDriver.h"

// Forward declarations
class TMC2160Stepper;
class FastAccelStepper;

// ============================================================================
// TMC2160StepperDriver — concrete MotorDriver for the TMC2160 stepper (V1)
// ============================================================================
//
// Build-guarded behind DRIVER_TMC2160 (set in platformio.ini).
// Uses hardware SPI + FastAccelStepper for pulse generation on ESP32-S3.
//
// Legacy driver readback (DRV_STATUS polling, DriverStatus struct, diagTask,
// StallGuard load monitoring) has been REMOVED per project owner directive.
// Config WRITES (current, chopper, stealthchop, etc.) are preserved.
//
// Lifecycle:
//   begin() → init()      — SPI setup, TMC init, FastAccelStepper creation
//   update() → runMotorStep() + updateExtrapolation()  (per-tick, Core 1)
//   emergencyStop()        — hardStop + disable, clear state

class TMC2160StepperDriver : public MotorDriver {
public:
    TMC2160StepperDriver();

    // ---- Lifecycle ----------------------------------------------------------
    void init() override;
    void update() override;
    void emergencyStop() override;

    // ---- Homing -------------------------------------------------------------
    bool home(int32_t home_speed_steps_s = 4000) override;
    void runHomingStep() override;
    bool isHomed()  const override { return _homed; }
    bool isHoming() const override { return _homing; }
    bool checkPushToHome() override;

    // ---- Position monitor ---------------------------------------------------
    void runMotorStep() override;

    // ---- Motion -------------------------------------------------------------
    void moveTo(float pos_mm) override;
    void streamTo(float pos_mm, float speed_mm_s) override;
    void streamExtrapolated(float pos_mm, float speed_mm_s) override;
    void updateExtrapolation() override;

    void stop() override;
    void hardStop() override;
    void enable() override;
    void disable() override;

    // ---- Speed & Acceleration -----------------------------------------------
    void  setMaxSpeed(float speed_mm_s) override;
    void  setAcceleration(float accel_mm_s2) override;
    float getMaxSpeed() const override { return _max_speed_mm_s; }

    // ---- Status -------------------------------------------------------------
    bool  isMoving() override;
    float getPosition()   const override;
    float getTargetPosition() const override;

    // ---- Driver config ------------------------------------------------------
    void applyDriverConfig(const DriverConfig& cfg) override;

    // ---- Diagnostics --------------------------------------------------------
    uint16_t getCurrentmA()  override;
    uint8_t  getMicrosteps() override;

    // ---- Predictive extrapolation tuning ------------------------------------
    void     setLookaheadMs(uint16_t ms) override { _lookahead_ms = ms; }
    void     setMaxOvershootMm(float mm) override { _max_overshoot_mm = constrain(mm, 0.0f, 50.0f); }
    uint16_t getLookaheadMs()    const override { return _lookahead_ms; }
    float    getMaxOvershootMm() const override { return _max_overshoot_mm; }

    // ---- Unit conversion (static for external use) --------------------------
    int32_t mmToNative(float mm)        const override;
    float   nativeToMm(int32_t native)  const override;

private:
    // Clears all streaming / extrapolation / target-monitor state so that a
    // stale stream from before a Halt/Home can't keep issuing moveTo() commands
    // that fight a fresh homing cycle.
    void resetStreamState();

    TMC2160Stepper*  _tmc     = nullptr;
    FastAccelStepper* _stepper = nullptr;

    bool _homed  = false;
    bool _homing = false;
    bool _enabled = false;
    bool _moving_to_target = false;
    int32_t _target_steps = 0;

    float _max_speed_mm_s = MAX_SPEED_MM_S;
    float _accel_mm_s2 = DEFAULT_ACCEL_MM_S2;
    float _current_position_mm = 0.0f;

    // Predictive extrapolation state
    uint16_t _lookahead_ms       = 20;
    float    _max_overshoot_mm   = 8.0f;
    float    _last_sample_mm     = 0.0f;
    float    _stream_velocity    = 0.0f;
    uint32_t _last_sample_ms     = 0;
    bool     _have_last_sample   = false;
    bool     _coasting           = false;
    static const uint16_t STREAM_STALL_MS = 80;

};
