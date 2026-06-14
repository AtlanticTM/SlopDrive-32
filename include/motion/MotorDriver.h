#pragma once

#include <cstdint>

// Forward declarations
class FastAccelStepperEngine;
class FastAccelStepper;

// ============================================================================
// MotorDriver — pure abstract base class for all motor hardware drivers
// ============================================================================
//
// All physical hardware interactions (steppers, servos, encoders) MUST be
// abstracted behind this interface.  Concrete drivers (TMC2160StepperDriver,
// future V2 brushless servo, etc.) implement these pure virtuals.
//
// Lifecycle hooks:
//   init()          — hardware setup, pin config, driver init (called once in setup())
//   update()        — per-tick maintenance (called from motorTask on Core 1)
//   emergencyStop() — immediate safe-stop, cut power, clear state
//
// Unit conversion (Risk #4):  mmToNative() / nativeToMm() are owned by the
// driver because a stepper uses steps/mm while a servo may use encoder counts,
// degrees, or raw DAC values.  Callers remain unit-agnostic.
//
// FastAccelStepper (Risk #5):  the engine lives as a protected member so every
// concrete driver has access to it — NOT buried inside a TMC-only #if block.

struct DriverConfig {
    uint16_t microsteps       = 16;
    uint16_t run_current_ma   = 2000;
    uint8_t  hold_current_pct = 50;
    int8_t   stallguard_dma   = -64;
    uint8_t  toff             = 4;
    uint8_t  tbl              = 1;
    uint8_t  stealthchop      = 0;       // 1=StealthChop, 0=SpreadCycle
    uint32_t tpwm_thrs        = 0;
    int8_t   hstart           = 5;
    int8_t   hend             = 1;
};

class MotorDriver {
public:
    virtual ~MotorDriver() = default;

    // ---- Lifecycle ----------------------------------------------------------
    virtual void init()           = 0;
    virtual void update()         = 0;
    virtual void emergencyStop()  = 0;

    // ---- Homing -------------------------------------------------------------
    virtual bool home(int32_t home_speed_steps_s = 4000) = 0;
    virtual void runHomingStep()   = 0;
    virtual bool isHomed()  const  = 0;
    virtual bool isHoming() const  = 0;

    // Push-to-home: when NOT homed and NOT actively homing, the user can simply
    // push the shaft into the endstop to establish home.  Returns true the
    // instant it completes homing this call.
    virtual bool checkPushToHome() = 0;

    // ---- Position monitor (call frequently to stop motor at target) ---------
    virtual void runMotorStep()    = 0;

    // ---- Motion -------------------------------------------------------------
    virtual void moveTo(float pos_mm)                              = 0;
    virtual void streamTo(float pos_mm, float speed_mm_s)          = 0;

    virtual void stop()      = 0;    // decelerate stop + cut power

    virtual void hardStop()  = 0;    // immediate stop, motor stays powered
    virtual void enable()    = 0;
    virtual void disable()   = 0;

    // ---- Speed & Acceleration -----------------------------------------------
    virtual void  setMaxSpeed(float speed_mm_s)     = 0;
    virtual void  setAcceleration(float accel_mm_s2) = 0;
    virtual float getMaxSpeed() const                = 0;

    // ---- Status -------------------------------------------------------------
    virtual bool  isMoving()            = 0;
    virtual float getPosition()   const = 0;
    virtual float getTargetPosition() const = 0;

    // ---- Driver config ------------------------------------------------------
    virtual void applyDriverConfig(const DriverConfig& cfg) = 0;

    // ---- Diagnostics --------------------------------------------------------
    virtual uint16_t getCurrentmA()  = 0;
    virtual uint8_t  getMicrosteps() = 0;

    // ---- Continuous-blend tuning --------------------------------------------
    // Selects how a streamed retarget that reverses direction mid-stroke is
    // handled: 1=let-it-land, 2=allow-reversal, 3=hybrid. Drivers that don't
    // do continuous blending may treat this as a no-op.
    virtual void    setBlendMode(uint8_t mode) = 0;
    virtual uint8_t getBlendMode() const       = 0;


    // ---- Unit conversion (driver-owned — Risk #4) ---------------------------
    // Convert a physical millimetre position to the driver's native unit
    // (steps for steppers, encoder counts for servos, etc.).
    virtual int32_t mmToNative(float mm)        const = 0;

    // Convert a driver-native position back to millimetres.
    virtual float   nativeToMm(int32_t native)  const = 0;

protected:
    // FastAccelStepper engine — shared across ALL concrete drivers (Risk #5).
    // Created once by the first driver's init(); must be forward-declared to
    // avoid pulling the full header into every compilation unit.
    FastAccelStepperEngine* _engine = nullptr;
};