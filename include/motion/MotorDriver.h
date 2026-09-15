#pragma once

// MotorDriver -- abstract port for the motion backend.
//
// Constraints:
// - ONE MOTION BACKEND (architecture.md section 1): MlinkServoDriver, the
//   RP2350 over the SPI link, is the only implementation. The type stays
//   because it is the arbiter's PORT: the sole-caller lock is expressed as
//   friendship on an abstract type, and a native test can stand a fake here.
// - Sole-caller rule (architecture.md): the motion methods (sendCommand/
//   pushConfig/stop/hardStop) are protected, with `friend class
//   MotionArbiter` as the only grant, so any call through a MotorDriver&
//   from outside MotionArbiter is a compile error. Friendship does not
//   inherit: an implementation's overrides must stay protected too.
// - init() runs once from setup(); update() runs every tick from motorTask
//   (Core 1); emergencyStop() cuts power and clears state immediately.
// - mmToNative()/nativeToMm() are backend-owned (the link speaks drive input
//   counts); nativePerMm() derives from mmToNative().

#include <cstdint>
#include "SessionOdometer.h"

#include "sloplog/sloplog.h"

// Declaration only: the link vocabulary is a two-board detail, and pulling
// MotionLinkProtocol.h in here would put it in every translation unit that
// merely holds a MotorDriver&.
namespace motionlink { struct LinkCommand; }

// Drive-side electrical configuration, persisted in NVS and echoed to the
// UI. TODO(sd-pln): the web flasher owns one-time drive programming.
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

    // ---- Sole-caller enforcement --------------------------------------------
    // See the file-header constraint above. This friend grant is the single
    // door in for MotionArbiter.
    friend class MotionArbiter;

    // ---- Homing -------------------------------------------------------------
    virtual bool home(int32_t home_speed_steps_s = 4000) = 0;
    virtual bool isHomed()  const  = 0;
    // True once the processor has answered at all; position is hearsay before.
    virtual bool isLinkUp() const { return true; }
    virtual bool isHoming() const  = 0;

    // Bench/test override: force the backend's internal homed flag WITHOUT a
    // real homing cycle. Setting _state.homed alone only opens the
    // MotionArbiter gate; the backend's own dispatch still refuses every
    // command while its internal homed flag is false. Do NOT call on real
    // hardware you do not want to move without homing first.
    virtual void forceHomeState(bool /*homed*/) {}

    // Push-to-home: when NOT homed and NOT actively homing, the user can simply
    // push the shaft into the endstop to establish home.  Returns true the
    // instant it completes homing this call.
    virtual bool checkPushToHome() = 0;

protected:
    // ---- Motion (MotionArbiter-only, sole-caller rule, architecture.md) -----
    // Protected + `friend class MotionArbiter` above: access is checked on the
    // static type (MotorDriver&), so no input source can dispatch motion
    // directly. Everything routes through MotionArbiter::submit() and its
    // stop/hardStop/emergencyStop helpers, which own every safety gate.
    // The ONLY motion dispatch entry point: one gated intent, forwarded to
    // whatever holds the plan. Normalized over the stroke window, anchored in
    // the target's own clock domain by the driver (docs/rp-motion-port.md).
    // Default drops: a backend that does not speak the link cannot move, and
    // saying so once beats a silent no-op.
    virtual void sendCommand(const motionlink::LinkCommand&) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SLOGW("motor", "backend cannot accept motion commands: no motion link");
        }
    }

    // Field-tagged policy push (ceilings, window, gates, soft-start cap,
    // engine tuning). Same default and the same reason.
    virtual void pushConfig(uint8_t /*tag*/, uint32_t /*raw*/) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SLOGW("motor", "backend cannot accept config pushes: no motion link");
        }
    }

    virtual void stop()      = 0;    // full stop + cut power (also clears homed)

    virtual void hardStop()  = 0;    // immediate stop, motor stays powered

public:
    virtual void enable()    = 0;
    virtual void disable()   = 0;

    // ---- Speed & Acceleration -----------------------------------------------
    virtual void     setMaxSpeed(float speed_mm_s)      = 0;
    // Renderer speed ceiling for a coprocessor that generates its own pulses.
    // Default no-op: only an open-loop offboard renderer needs one -- an
    // onboard stepper is already bounded by the planner that feeds it.
    virtual void setRenderCeiling(float /*mm_s*/) {}
    virtual void     setAcceleration(float accel_mm_s2)  = 0;
    virtual float    getMaxSpeed()          const        = 0;
    // Acceleration ACTUALLY applied by the driver (mm/s², post-internal-clamp).
    // The driver may cap lower than config_api.h's MAX_ACCEL_MM_S2 ceiling —
    // Ground Truth Doctrine: echoes must report this, never the raw request.
    virtual float    getAcceleration()      const        = 0;
    // Acceleration active in the backend, native units, NOT the configured
    // ceiling. MotionArbiter's raise-only guard reads this.
    virtual uint32_t getLiveAcceleration()  const        = 0;

    // ---- Status -------------------------------------------------------------
    virtual bool  isMoving()            = 0;
    virtual float getPosition()   const = 0;
    virtual float getTargetPosition() const = 0;

    // Where the shaft PHYSICALLY is, mm, for backends with real feedback.
    // Default is the commanded position: an open-loop backend has nothing
    // else, and returning a lie is worse than returning the known model.
    virtual float getActualPosition() const { return getPosition(); }
    virtual bool  hasActualPosition() const { return false; }

    // ---- Driver config ------------------------------------------------------
    virtual void applyDriverConfig(const DriverConfig& cfg) = 0;

    // ---- Blend mode ---------------------------------------------------------
    // Retired as a motion policy: the byte survives on 0x008A as
    // `blend_mode_reserved` and NVS round-trips it, so these are the store for
    // a value nothing acts on. See include/comms/SlopSyncCatalog.h.
    virtual void    setBlendMode(uint8_t mode) = 0;
    virtual uint8_t getBlendMode() const       = 0;

    // Usable stroke (mm) measured by sensorless homing between the two hard
    // stops. Default 0 = "not measured / not supported".
    virtual float   getMeasuredStrokeMm() const { return 0.0f; }
    // Restore a previously-measured stroke from NVS after boot. ConfigStore
    // calls this with the persisted value so the rail scale is correct before
    // the first homing cycle; the cycle overwrites it when it completes.
    virtual void    setMeasuredStrokeMm(float /*mm*/) {}

    // ---- Max rail length (rail-length-agnostic ceiling) ---------------------
    // The machine is agnostic to the physical rail length; there is NO fixed
    // geometry ceiling. This is the user-configured max rail length (WebUI
    // setting, persisted to NVS, default DEFAULT_MAX_RAIL_MM = 500mm). It bounds
    // the sensorless homing search sweep and serves as the position ceiling
    // BEFORE homing has measured the real stroke. Written from Core 0
    // (WebUI/ConfigStore), read from Core 1 (arbiter + driver clamps). An aligned
    // 32-bit float is atomic on the ESP32-S3, same as _measured_stroke_mm.
    virtual void    setMaxRailMm(float mm) { if (mm > 0.0f) _max_rail_mm = mm; }
    virtual float   getMaxRailMm() const   { return _max_rail_mm; }

    // Effective physical position ceiling (mm): once sensorless homing has felt
    // out the real front wall the MEASURED stroke is the source of truth and
    // wins (it may even exceed the configured rail length — the search sweep
    // bounds hunting, not the result). Until then, fall back to the configured
    // max rail length. This is the true outer bound every position command is
    // clamped to, regardless of source.
    virtual float   effectiveCeilingMm() const {
        float m = getMeasuredStrokeMm();
        return (m > 0.0f) ? m : getMaxRailMm();
    }

    // ---- Live bus telemetry (INA228 on the 57AIM board) ---------------------
    // Instantaneous motor-bus current in AMPS and the 36V rail voltage off an
    // INA228 shunt. Default 0 so the WebUI shows a flat, harmless zero.
    // TODO(sd-4k1): the link backend reads the INA228 for homing stall
    // detection but publishes nothing here, so these read 0 on the live build.
    virtual float   getBusCurrentA() const { return 0.0f; }
    virtual float   getBusVoltageV() const { return 0.0f; }
    // True when a real current sensor is present and calibrated. Lets the UI
    // gray out / hide the readout on boards that don't have one.
    virtual bool    hasCurrentSensor() const { return false; }

    // ---- Extended power telemetry (INA228 full measurement set) -------------
    // Kept separate from the current/voltage pair above so a backend that only
    // implements the basic pair needs no changes.
    virtual float   getBusPowerW()      const { return 0.0f; }
    virtual float   getDieTempC()       const { return 0.0f; }
    // Highest |current| seen since boot / since the last resetPeaks() call —
    // lets the operator glance at the Health tab after a session and see how
    // hard the machine strained without having to watch the live number.
    virtual float   getPeakBusCurrentA() const { return 0.0f; }
    // True when the driver has a real power monitor (INA228 or similar) that
    // exposes power/temp/peak telemetry beyond basic current/voltage. Lets the
    // UI show/hide the extended Health-tab power card.
    virtual bool    hasPowerMonitor()   const { return false; }

    // Session energy in WATT-HOURS from the power monitor's own hardware
    // accumulator (integrates continuously in the chip). Drives the dashboard
    // SESSION card. Zeroed by resetPowerStats(). 0 when no monitor.
    virtual float   getBusEnergyWh()    const { return 0.0f; }
    // Reset the session power stats — software peaks AND the hardware energy
    // accumulator — back to zero. Called on home and by the reset-session
    // control. No-op on drivers without a monitor.
    virtual void    resetPowerStats()   {}
    // Session odometer (0x1020), fed by the backend from its own position
    // samples; null when the backend has none. Glue mirrors it into state.
    virtual const SessionOdometer* odometer() const { return nullptr; }
    virtual void    resetOdometer()     {}




    // ---- Unit conversion (driver-owned) -------------------------------------
    // Convert a physical millimeter position to the backend's native unit
    // (drive input counts on the link backend).
    virtual int32_t mmToNative(float mm)        const = 0;

    // Convert a driver-native position back to millimeters.
    virtual float   nativeToMm(int32_t native)  const = 0;

    // Native units per millimeter, derived from mmToNative. The arbiter uses
    // it to convert speed/accel rather than assuming a scale: counts-native
    // (~834/mm) and steps-native (~20/mm) differ by ~41x, so an assumed ratio
    // scales dynamics wrong by that factor.
    virtual float   nativePerMm() const { return (float)mmToNative(1000.0f) / 1000.0f; }

protected:
    // User-configured max rail length (mm). Literal default mirrors
    // DEFAULT_MAX_RAIL_MM in config_api.h (not included here to keep this
    // interface header dependency-free). ConfigStore overwrites it at boot with
    // the persisted value; the WebUI updates it live via setMaxRailMm().
    float _max_rail_mm = 500.0f;
};
