#pragma once

#include "MotorDriver.h"
#include "freertos/FreeRTOS.h"

// ============================================================================
// MotorProxy — runtime motion-backend forwarding shim
// ============================================================================
//
// Why this exists (Static-init trap, plan.md "Design"): main.cpp constructs
// `motor` as a static global, and patternEngine/arbiter/webui/encoderValidator
// all capture `MotorDriver&` at static-init time — long before NVS is
// readable (Preferences needs the filesystem/partition table up, which only
// happens inside setup()). We can't pick FAS-vs-Modbus with a conditional
// `#if` at construction time because the choice is a runtime NVS setting, not
// a compile-time one. So instead every consumer is wired to ONE stable
// address — this proxy — and `bind()` decides at the top of setup() which
// concrete driver that address actually forwards to.
//
// `MotorDriver` grants `friend class MotorProxy;` (see MotorDriver.h) so this
// class can reach the PROTECTED motion methods (moveTo/streamTo/streamToSteps/
// stop/hardStop) on whichever concrete driver `_impl` points at. That grant
// is load-bearing: friendship isn't inherited, so without it a MotorProxy
// holding a `MotorDriver&` to a sibling object couldn't call its protected
// members at all — same rule that makes the sole-caller lock work in the
// first place. The result: MotionArbiter is still the only thing that can
// drive position (it's the only class trusted with a motion-capable
// reference), and now there are exactly two links in that chain instead of
// one — arbiter -> proxy -> concrete driver — with every hop compiler-
// enforced.
//
// Completeness is compiler-enforced too: MotorDriver has pure virtuals, so if
// this class ever misses one, MotorProxy itself becomes abstract and every
// `MotorProxy motor;` instantiation in main.cpp fails to compile. There is no
// way to silently forget a method here.
//
// Unbound safety: every forward goes through the private `d()` helper, which
// asserts `_impl` is non-null before dereferencing. `bind()` must be called
// exactly once, at the very top of setup(), before ANY other call through
// `motor` (including ConfigStore::load(), which touches setMeasuredStrokeMm/
// setMaxRailMm). Calling through an unbound proxy is a boot-order bug, not a
// runtime condition to degrade gracefully from — configASSERT halts loudly
// instead of silently no-op'ing a motion call. :3
class MotorProxy : public MotorDriver {
public:
    // Bind the concrete driver this proxy forwards to. Call ONCE in setup(),
    // before any other use of the proxy. Rebinding mid-session is not a
    // supported flow (the runtime backend selection is reboot-to-apply). :3
    void bind(MotorDriver& impl) { _impl = &impl; }

    // ---- Lifecycle ----------------------------------------------------------
    void init()           override { d().init(); }
    void update()         override { d().update(); }
    void emergencyStop()  override { d().emergencyStop(); }

    // ---- Homing ---------------------------------------------------------------
    // Default is -1 = "concrete driver's own default", NOT the base class's
    // 4000. C++ default args bind to the STATIC type: `motor.home()` through
    // this proxy uses THIS default, and copying the base's 4000 here silently
    // overrode AIMServoDriver's deliberate AIM_HOMING_SPEED_STEPS_S (~601
    // steps/s = 29.5mm/s crawl) with 4000 steps/s = 196mm/s — freight-train
    // homing sweeps, wall slams, polluted stall baselines, failed rear
    // sweeps. Bench night 1's "homing too fast / never completes" bug. Both
    // concrete drivers treat <=0 as "use my own safe default". :3
    bool home(int32_t home_speed_steps_s = -1) override { return d().home(home_speed_steps_s); }
    void runHomingStep()   override { d().runHomingStep(); }
    bool isHomed()  const  override { return d().isHomed(); }
    bool isHoming() const  override { return d().isHoming(); }
    void forceHomeState(bool homed) override { d().forceHomeState(homed); }
    bool checkPushToHome() override { return d().checkPushToHome(); }

    // ---- Position monitor -----------------------------------------------------
    void runMotorStep()    override { d().runMotorStep(); }

protected:
    // ---- Motion (MotionArbiter-only — sole-caller rule, CLAUDE.md §2) --------
    // Kept protected here too, exactly like every concrete driver — otherwise
    // holding a `MotorProxy&` instead of a `MotorDriver&` would reopen the
    // door the base class closes. :3
    bool moveTo(float pos_mm) override { return d().moveTo(pos_mm); }
    void streamTo(float pos_mm, float speed_mm_s) override { d().streamTo(pos_mm, speed_mm_s); }
    void streamToSteps(int32_t target_steps,
                       uint32_t speed_steps_s,
                       uint32_t accel_steps_s2) override {
        d().streamToSteps(target_steps, speed_steps_s, accel_steps_s2);
    }
    void stop()      override { d().stop(); }
    void hardStop()  override { d().hardStop(); }

public:
    void enable()    override { d().enable(); }
    void disable()   override { d().disable(); }

    // ---- Speed & Acceleration -------------------------------------------------
    void     setMaxSpeed(float speed_mm_s)      override { d().setMaxSpeed(speed_mm_s); }
    void     setAcceleration(float accel_mm_s2) override { d().setAcceleration(accel_mm_s2); }
    float    getMaxSpeed()          const       override { return d().getMaxSpeed(); }
    float    getAcceleration()      const       override { return d().getAcceleration(); }
    uint32_t getLiveAcceleration()  const       override { return d().getLiveAcceleration(); }

    // ---- Status -----------------------------------------------------------------
    bool  isMoving()            override { return d().isMoving(); }
    float getPosition()   const override { return d().getPosition(); }
    float getTargetPosition() const override { return d().getTargetPosition(); }

    // ---- Driver config ------------------------------------------------------------
    void applyDriverConfig(const DriverConfig& cfg) override { d().applyDriverConfig(cfg); }

    // ---- Diagnostics --------------------------------------------------------------
    uint16_t getCurrentmA()  override { return d().getCurrentmA(); }
    uint8_t  getMicrosteps() override { return d().getMicrosteps(); }

    // ---- Continuous-blend tuning ---------------------------------------------------
    void    setBlendMode(uint8_t mode) override { d().setBlendMode(mode); }
    uint8_t getBlendMode() const       override { return d().getBlendMode(); }

    // ---- Measured stroke / max rail --------------------------------------------------
    float   getMeasuredStrokeMm() const  override { return d().getMeasuredStrokeMm(); }
    void    setMeasuredStrokeMm(float mm) override { d().setMeasuredStrokeMm(mm); }
    void    setMaxRailMm(float mm)       override { d().setMaxRailMm(mm); }
    float   getMaxRailMm() const         override { return d().getMaxRailMm(); }
    float   effectiveCeilingMm() const   override { return d().effectiveCeilingMm(); }

    // ---- Live bus telemetry (INA228) -------------------------------------------------
    float   getBusCurrentA()  const override { return d().getBusCurrentA(); }
    float   getBusVoltageV()  const override { return d().getBusVoltageV(); }
    bool    hasCurrentSensor() const override { return d().hasCurrentSensor(); }

    // ---- Extended power telemetry ------------------------------------------------------
    float   getBusPowerW()      const override { return d().getBusPowerW(); }
    float   getDieTempC()       const override { return d().getDieTempC(); }
    float   getPeakBusCurrentA() const override { return d().getPeakBusCurrentA(); }
    bool    hasPowerMonitor()   const override { return d().hasPowerMonitor(); }
    float   getBusEnergyWh()    const override { return d().getBusEnergyWh(); }
    void    resetPowerStats()   override { d().resetPowerStats(); }

    // ---- Unit conversion ----------------------------------------------------------------
    int32_t mmToNative(float mm)        const override { return d().mmToNative(mm); }
    float   nativeToMm(int32_t native)  const override { return d().nativeToMm(native); }
    float   nativePerMm()               const override { return d().nativePerMm(); }

private:
    MotorDriver* _impl = nullptr;

    // Terse unbound guard — every forward routes through here. Halts loudly
    // (configASSERT) on a call before bind(), rather than silently degrading
    // into a driver-less no-op that would let motion "work" against nothing
    // and leave the operator staring at a shaft that never moves. :3
    MotorDriver& d() const {
        configASSERT(_impl);
        return *_impl;
    }
};
