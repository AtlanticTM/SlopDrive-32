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
//   update() → runMotorStep()  (per-tick, Core 1)
//   emergencyStop()        — hardStop + disable, clear state
//
// CONTINUOUS BLENDING (the Decel-Trap fix): streamTo() never softens a committed
// brake ramp (raise-only accel) and handles same-direction vs. reversal per the
// selectable _blend_mode. No more stop-and-go stutter between waypoints — we keep
// pounding through the stream instead of edging to a dead stop on every sample. :3


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
    // Force the driver's internal homed flag. Only for bench/remote testing
    // builds (HOMING_DISABLED) — do NOT call on real hardware with a motor
    // plugged in, or the firmware will cheerfully ram the endstop. :3
    void forceHomeState(bool homed) { _homed = homed; }
    bool checkPushToHome() override;

    // ---- Position monitor ---------------------------------------------------
    void runMotorStep() override;

protected:
    // ---- Motion (MotionArbiter-only — see MotorDriver.h sole-caller lock) ----
    // Kept protected in the derived class too so the compile-time lock can't be
    // bypassed by holding a concrete TMC2160StepperDriver& instead of a
    // MotorDriver&.
    bool moveTo(float pos_mm) override;
    void streamTo(float pos_mm, float speed_mm_s) override;

    // Pre-planned native-step dispatch — called from Core 1 motionConsumerTask.
    // Speed and accel arrive already converted to steps/s and steps/s².
    // Arms the stall watchdog and fires straight to FAS — no unit math here. :3
    void streamToSteps(int32_t target_steps,
                       uint32_t speed_steps_s,
                       uint32_t accel_steps_s2) override;

    void stop() override;

    void hardStop() override;

public:
    void enable() override;
    void disable() override;

    // ---- Speed & Acceleration -----------------------------------------------
    void     setMaxSpeed(float speed_mm_s) override;
    void     setAcceleration(float accel_mm_s2) override;
    float    getMaxSpeed() const override { return _max_speed_mm_s; }
    // The accel ACTUALLY applied (post the driver's internal 20000 mm/s² clamp)
    // — this is what settings echoes report back, never the raw request. :3
    float    getAcceleration() const override { return _accel_mm_s2; }
    // Returns the live FAS acceleration — what the ramp engine is actually
    // using right now, not the configured ceiling. Mirrors OSSM's
    // stepper->getAcceleration() call in the raise-only guard. :3
    uint32_t getLiveAcceleration() const override;

    // ---- Status -------------------------------------------------------------
    bool  isMoving() override;
    float getPosition()   const override;
    float getTargetPosition() const override;

    // ---- Driver config ------------------------------------------------------
    void applyDriverConfig(const DriverConfig& cfg) override;

    // ---- Diagnostics --------------------------------------------------------
    uint16_t getCurrentmA()  override;
    uint8_t  getMicrosteps() override;

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
    // Clears all streaming / blend / target-monitor state so that a stale stream
    // from before a Halt/Home can't keep issuing moveTo() commands that fight a
    // fresh homing cycle.
    void resetStreamState();


    // Self-contained homing task — spawned by home(), deletes itself when done.
    // Mirrors StrokeEngine's _homingProcedure pattern exactly. :3
    static void _homingTaskImpl(void* param);
    void        _homingTask();
    TaskHandle_t _homingTaskHandle = nullptr;

    TMC2160Stepper*  _tmc     = nullptr;
    FastAccelStepper* _stepper = nullptr;

    bool    _homed  = false;
    bool    _homing = false;
    bool    _enabled = false;
    int32_t _home_speed_steps_s = 4000;  // stored by home(), read by _homingTask()

    float    _max_speed_mm_s       = MAX_SPEED_MM_S;
    float    _accel_mm_s2          = DEFAULT_ACCEL_MM_S2;
    float    _current_position_mm  = 0.0f;

    // S-curve jerk limiting: FAS ramps acceleration linearly from 0 to the
    // target over this many steps at each stroke start/reversal. 0 = pure
    // trapezoid (sharp corners). ~40 steps at 80 steps/mm = 0.5mm of S-curve
    // lead-in — silky smooth without eating the interval budget. The shaft
    // eases into each thrust instead of snapping like a jackhammer. :3
    uint32_t _linear_accel_steps   = 40;


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
    static const int32_t BLEND_REVERSAL_THRESHOLD_STEPS = (int32_t)(1.5f * STEPS_PER_MM);

};


