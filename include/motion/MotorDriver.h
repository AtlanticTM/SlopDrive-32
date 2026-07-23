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
// abstracted behind this interface.  Concrete drivers (AIMServoDriver,
// ModbusServoDriver, future V2 brushless servo, etc.) implement these pure virtuals.
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
// concrete driver has access to it — NOT buried inside a driver-specific #if block.

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

    // ---- Sole-caller enforcement ---------------------------------------------
    // MotionArbiter is the ONLY class permitted to command motor position.
    // All motion dispatch routes through MotionArbiter::submit(). The motion
    // methods (moveTo/streamTo/streamToSteps/stop/hardStop) live in the
    // `protected:` section below, so any direct call from an input source (UI,
    // transport, pattern, BLE) through a MotorDriver& is a COMPILE ERROR — this
    // friend grant is the single door in. Concrete drivers must keep their
    // overrides protected too, or the lock leaks through the derived type. :3
    friend class MotionArbiter;

    // MotorProxy is the runtime-backend forwarding shim (mode dispatch door):
    // it stands in for the concrete driver so main.cpp can pick FAS vs Modbus
    // at boot (NVS isn't readable at static-init time) without ever exposing
    // a raw switch to the input sources. The sole-caller lock is PRESERVED
    // through it — friendship isn't inherited, so without this grant a
    // MotorProxy couldn't reach the protected motion methods on the concrete
    // driver it forwards to. Only MotionArbiter can call motion methods on
    // the proxy (via the grant above), and only the proxy forwards them on to
    // whichever concrete driver is bound. One door in, one door further in. :3
    friend class MotorProxy;

    // ---- Homing -------------------------------------------------------------
    virtual bool home(int32_t home_speed_steps_s = 4000) = 0;
    virtual void runHomingStep()   = 0;
    virtual bool isHomed()  const  = 0;
    virtual bool isHoming() const  = 0;

    // Bench/test override: force the driver's internal homed flag WITHOUT a real
    // homing cycle, so HOME_OVERRIDE can actually drive step/dir pulses out to a
    // (possibly disconnected) motor for bench testing. Setting _state.homed
    // alone only opens the MotionArbiter gate — the concrete driver's own
    // moveTo()/streamTo()/streamToSteps() still refuse every command while their
    // internal _homed is false. This hook flips that flag (and enables outputs)
    // so pulses genuinely go out. Default no-op: drivers that don't support a
    // fake-home are simply unaffected. Do NOT call on real hardware you don't
    // want to move without homing first. :3
    virtual void forceHomeState(bool /*homed*/) {}

    // Push-to-home: when NOT homed and NOT actively homing, the user can simply
    // push the shaft into the endstop to establish home.  Returns true the
    // instant it completes homing this call.
    virtual bool checkPushToHome() = 0;

    // ---- Position monitor (call frequently to stop motor at target) ---------
    virtual void runMotorStep()    = 0;

protected:
    // ---- Motion (MotionArbiter-only — sole-caller rule, CLAUDE.md §2) --------
    // Protected + `friend class MotionArbiter` above: access is checked on the
    // static type (MotorDriver&), so no input source can dispatch motion
    // directly. Everything routes through MotionArbiter::submit() and its
    // stop/hardStop/emergencyStop helpers, which own every safety gate. :3
    // moveTo returns true when FAS accepted the move (false = refused/not homed)
    // so a silently-rejected move is distinguishable at the call site.
    virtual bool moveTo(float pos_mm)                              = 0;
    virtual void streamTo(float pos_mm, float speed_mm_s)          = 0;

    // Dispatch a pre-planned move in native steps — called exclusively from
    // Core 1 (motionConsumerTask). Speed and accel are already in steps/s and
    // steps/s² (converted by the consumer before calling). No unit conversion
    // happens inside this function — it goes straight to FAS. :3
    virtual void streamToSteps(int32_t target_steps,
                               uint32_t speed_steps_s,
                               uint32_t accel_steps_s2)            = 0;

    virtual void stop()      = 0;    // full stop + cut power (also clears homed)

    virtual void hardStop()  = 0;    // immediate stop, motor stays powered

public:
    virtual void enable()    = 0;
    virtual void disable()   = 0;

    // ---- Speed & Acceleration -----------------------------------------------
    virtual void     setMaxSpeed(float speed_mm_s)      = 0;
    virtual void     setAcceleration(float accel_mm_s2)  = 0;
    virtual float    getMaxSpeed()          const        = 0;
    // Acceleration ACTUALLY applied by the driver (mm/s², post-internal-clamp).
    // The driver may cap lower than config_api.h's MAX_ACCEL_MM_S2 ceiling —
    // Ground Truth Doctrine: echoes must report this, never the raw request. :3
    virtual float    getAcceleration()      const        = 0;
    // Returns the acceleration currently active inside the FAS ramp engine —
    // NOT the configured ceiling. Used by the raise-only guard in
    // motionConsumerTask to match OSSM's stepper->getAcceleration() call. :3
    virtual uint32_t getLiveAcceleration()  const        = 0;

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

    // Usable stroke (mm) measured by sensorless homing between the two hard
    // stops. Default 0 = "not measured / not supported" so drivers without
    // sensorless homing (an endstop-switch build) don't have to implement it. The
    // 57AIM servo driver overrides this once it's felt out both ends. :3
    virtual float   getMeasuredStrokeMm() const { return 0.0f; }
    // Restore a previously-measured stroke from NVS after boot. Default no-op
    // for drivers that don't support sensorless homing — ConfigStore calls this
    // with the persisted value so the rail scale is correct before the first
    // homing cycle runs. The homing task itself overwrites this with a fresh
    // measurement when it completes. :3
    virtual void    setMeasuredStrokeMm(float /*mm*/) {}

    // ---- Max rail length (rail-length-agnostic ceiling) ---------------------
    // The machine is agnostic to the physical rail length; there is NO fixed
    // geometry ceiling. This is the user-configured max rail length (WebUI
    // setting, persisted to NVS, default DEFAULT_MAX_RAIL_MM = 500mm). It bounds
    // the sensorless homing search sweep and serves as the position ceiling
    // BEFORE homing has measured the real stroke. Written from Core 0
    // (WebUI/ConfigStore), read from Core 1 (arbiter + driver clamps). An aligned
    // 32-bit float is atomic on the ESP32-S3, same as _measured_stroke_mm. :3
    virtual void    setMaxRailMm(float mm) { if (mm > 0.0f) _max_rail_mm = mm; }
    virtual float   getMaxRailMm() const   { return _max_rail_mm; }

    // Effective physical position ceiling (mm): once sensorless homing has felt
    // out the real front wall the MEASURED stroke is the source of truth and
    // wins (it may even exceed the configured rail length — the search sweep
    // bounds hunting, not the result). Until then, fall back to the configured
    // max rail length. This is the true outer bound every position command is
    // clamped to, regardless of source. :3
    virtual float   effectiveCeilingMm() const {
        float m = getMeasuredStrokeMm();
        return (m > 0.0f) ? m : getMaxRailMm();
    }

    // ---- Live bus telemetry (INA228 on the 57AIM board) ---------------------
    // Instantaneous motor-bus current in AMPS and the 36V rail voltage. Only the
    // 57AIM servo driver has an INA228 to gulp these off the shunt; every other
    // driver returns 0 so the WebUI just shows a flat, harmless zero. This is
    // the live readout the operator watches during bring-up to confirm the
    // sensor is alive BEFORE trusting sensorless homing not to ram anything. :3
    virtual float   getBusCurrentA() const { return 0.0f; }
    virtual float   getBusVoltageV() const { return 0.0f; }
    // True when a real current sensor is present and calibrated. Lets the UI
    // grey out / hide the readout on boards that don't have one. :3
    virtual bool    hasCurrentSensor() const { return false; }

    // ---- Extended power telemetry (INA228 full measurement set) -------------
    // NEW virtuals alongside the existing current/voltage ones above — kept
    // separate so drivers that only implemented the basic pair don't need any
    // changes. Every driver without a power monitor returns a harmless 0/false
    // default, same pattern as getBusCurrentA()/getBusVoltageV(). :3
    virtual float   getBusPowerW()      const { return 0.0f; }
    virtual float   getDieTempC()       const { return 0.0f; }
    // Highest |current| seen since boot / since the last resetPeaks() call —
    // lets the operator glance at the Health tab after a session and see how
    // hard the machine strained without having to watch the live number. :3
    virtual float   getPeakBusCurrentA() const { return 0.0f; }
    // True when the driver has a real power monitor (INA228 or similar) that
    // exposes power/temp/peak telemetry beyond basic current/voltage. Lets the
    // UI show/hide the extended Health-tab power card. :3
    virtual bool    hasPowerMonitor()   const { return false; }

    // Session energy in WATT-HOURS from the power monitor's own hardware
    // accumulator (integrates continuously in the chip). Drives the dashboard
    // SESSION card. Zeroed by resetPowerStats(). 0 when no monitor. :3
    virtual float   getBusEnergyWh()    const { return 0.0f; }
    // Reset the session power stats — software peaks AND the hardware energy
    // accumulator — back to zero. Called on home and by the reset-session
    // control. No-op on drivers without a monitor. :3
    virtual void    resetPowerStats()   {}




    // ---- Unit conversion (driver-owned — Risk #4) ---------------------------
    // Convert a physical millimetre position to the driver's native unit
    // (steps for steppers, encoder counts for servos, etc.).
    virtual int32_t mmToNative(float mm)        const = 0;

    // Convert a driver-native position back to millimetres.
    virtual float   nativeToMm(int32_t native)  const = 0;

    // Native units per millimetre — derived from mmToNative so every driver
    // gets this for free without a separate override. The arbiter uses this
    // to convert speed/accel into native units instead of hardcoding the FAS
    // steps/mm scale (AIM_STEPS_PER_MM): a counts-native driver (encoder
    // counts, ~834/mm) would otherwise get its dynamics scaled ~41x wrong if
    // the arbiter assumed steps/mm (~20/mm) universally. Drivers whose native
    // unit isn't a simple linear scale of mm can still override this. :3
    virtual float   nativePerMm() const { return (float)mmToNative(1000.0f) / 1000.0f; }

protected:
    // FastAccelStepper engine — shared across ALL concrete drivers (Risk #5).
    // Created once by the first driver's init(); must be forward-declared to
    // avoid pulling the full header into every compilation unit.
    FastAccelStepperEngine* _engine = nullptr;

    // User-configured max rail length (mm). Literal default mirrors
    // DEFAULT_MAX_RAIL_MM in config_api.h (not included here to keep this
    // interface header dependency-free). ConfigStore overwrites it at boot with
    // the persisted value; the WebUI updates it live via setMaxRailMm(). :3
    float _max_rail_mm = 500.0f;
};