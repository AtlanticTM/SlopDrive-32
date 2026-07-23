// ModbusServoDriver — FAS-bypass direct-drive backend, Phase 3: real motion.
// Build-guarded behind DRIVER_AIM_SERVO && FEATURE_RS485_MODBUS.
//
// Streamed 0x7B setpoints via StreamedSetpointExecutor (the "ISR" for this
// backend — see ServoMotionExecutor.h). Every motion entry point stays
// gated behind the exact same homed/enabled discipline FAS mode uses
// (CLAUDE.md §2 — nothing moves spontaneously); the only way _homed becomes
// true this phase is the BENCH forceHomeState() path (real homing is
// Phase 4). See ModbusServoDriver.h for the full doctrine writeup. :3
#if defined(DRIVER_AIM_SERVO) && defined(FEATURE_RS485_MODBUS)

#include "ModbusServoDriver.h"
#include "ServoModbus.h"
#include "sloplog/sloplog.h"
#include <math.h>       // lroundf
#include <esp_timer.h>  // esp_timer_get_time()

ModbusServoDriver::ModbusServoDriver(ServoModbus& bus)
    : _bus(bus)
    , _executor(bus)
{}

// ---- Lifecycle ---------------------------------------------------------------

void ModbusServoDriver::init() {
    // Bring up the INA228 the same way AIMServoDriver does — the Wire bus is
    // already up (main setup() calls Wire.begin() before motor.init()). :3
    if (!_current.init()) {
        SLOGW("servo", "ModbusServoDriver: WARNING — INA228 not found, live current telemetry unavailable. uhoh :3");
    }

    // Jerk ceiling for the executor's tracking integrator — OSSM-RS parity
    // (their Ruckig runs 100000 mm/s^3), converted to native counts. :3
    _executor.setJerkLimit(AIM_MODBUS_JERK_MM_S3 * AIM_ENC_COUNTS_PER_MM);

    // Deliberately do NOT enable the drive output here — Modbus mode never
    // spontaneously energizes anything at boot, exactly like FAS mode never
    // pulses the motor before a home. Output only comes on via
    // forceHomeState(true)/enable() (Phase 4's real home() will do the same). :3
    SLOGI("servo", "ModbusServoDriver: init — Modbus direct-drive backend, %.1f counts/mm, "
          "wire_sign=%d (bench-tune AIM_MODBUS_WIRE_SIGN if position runs backwards)",
          AIM_ENC_COUNTS_PER_MM, (int)_wire_sign);
}

void ModbusServoDriver::update() {
    // ---- INA228 telemetry cache refresh — identical two-tier cadence to ----
    // ---- AIMServoDriver::update() (40Hz fast / 1Hz full). -------------------
    if (_current.isReady()) {
        uint32_t now = millis();
        if (now - _last_current_poll_ms >= 25) {
            _last_current_poll_ms = now;
            _current.pollFast();
        }
        if (now - _last_current_full_poll_ms >= 1000) {
            _last_current_full_poll_ms = now;
            _current.poll();
        }
    }

    // ---- Bus-health watchdog — missed 0x7B setpoint echoes ------------------
    // Runs on the 1ms motorTask tick — both branches are one-shot logged so
    // a sustained fault never spams the ring. Fault clears ONLY via
    // forceHomeState()/home(), never automatically just because the bus
    // health streak recovers on its own. :3
    ServoBusHealth health = _bus.getBusHealth();
    if (health.sp_fail_streak >= AIM_SP_FAIL_ESTOP) {
        if (!_fault_estop_logged) {
            _fault_estop_logged = true;
            _executor.freeze();
            _bus.emergencyStop();     // immediate output-off + write-queue clear
            _homing    = false;
            _homed     = false;
            _bus_fault = true;
            SLOGE("servo", "ModbusServoDriver: WATCHDOG — %u missed setpoint echoes, E-STOP "
                  "(output off, re-home required). uhoh :C", (unsigned)health.sp_fail_streak);
        }
    } else if (health.sp_fail_streak >= AIM_SP_FAIL_FREEZE) {
        if (!_fault_freeze_logged) {
            _fault_freeze_logged = true;
            _executor.freeze();
            _bus_fault = true;
            SLOGE("servo", "ModbusServoDriver: WATCHDOG — %u missed setpoint echoes, FREEZE "
                  "(holding position, re-home required). uhoh :C", (unsigned)health.sp_fail_streak);
        }
    } else {
        // Streak dropped back under the freeze threshold — reset the LOG
        // latches (not the fault/homed gates) so a fresh episode logs again
        // instead of staying silently latched from the first one. :3
        _fault_freeze_logged = false;
        _fault_estop_logged  = false;
    }
}

void ModbusServoDriver::emergencyStop() {
    _executor.freeze();
    _have_last_stream = false;
    _bus.emergencyStop();   // immediate wire write (0x01=0) + write-queue clear
    _homing  = false;
    _homed   = false;
    _enabled = false;
    SLOGW("servo", "ModbusServoDriver: EMERGENCY STOP — output off, homed cleared.");
}

// ---- Homing ------------------------------------------------------------------

bool ModbusServoDriver::home(int32_t /*home_speed_steps_s*/) {
    SLOGW("servo", "ModbusServoDriver: home() refused — real homing lands in Phase 4. "
          "Use forceHomeState(true) for bench bring-up.");
    return false;
}

// BENCH PATH (HOME_OVERRIDE) — see the loud doc comment in the header. :3
void ModbusServoDriver::forceHomeState(bool homed) {
    if (homed) {
        ServoTelemetry t = _bus.getTelemetry();
        if (!t.enc_valid) {
            SLOGW("servo", "ModbusServoDriver: forceHomeState(true) REFUSED — no valid encoder "
                  "reading yet (bus not ready / never polled). uhoh :C");
            return;
        }

        // ====================================================================
        // Establish the wire mapping from WHATEVER the shaft is sitting at
        // right now — NOT a real homed position. Seeds the executor at cmd=0
        // so the wire mapping's identity (wire_offset + sign*0 == wire_offset
        // == the encoder reading we just read) makes the FIRST setpoint we
        // ever send exactly equal to the current physical position: zero
        // motion, per plan.md's hard safety requirement. :3
        // ====================================================================
        _wire_offset = t.enc_counts;
        _executor.setWireMap(_wire_offset, AIM_MODBUS_WIRE_SIGN);
        _executor.seed(0.0f);
        _target_counts     = 0.0f;
        _have_last_stream  = false;
        _bus_fault         = false;
        _fault_freeze_logged = false;
        _fault_estop_logged  = false;
        _homing = false;
        _homed  = true;
        _enabled = true;
        _bus.queueWrite(0x01, 1);   // energize output

        SLOGI("servo", "ModbusServoDriver: forceHomeState(true) — BENCH fake-home, "
              "wire_offset=%ld counts, wire_sign=%d :3",
              (long)_wire_offset, (int)AIM_MODBUS_WIRE_SIGN);
    } else {
        _homed = false;
        SLOGI("servo", "ModbusServoDriver: forceHomeState(false) — cleared, real homing required.");
    }
}

// ---- Motion --------------------------------------------------------------------

bool ModbusServoDriver::moveTo(float pos_mm) {
    if (!_homed || !_enabled || _bus_fault) {
        SLOGW("servo", "ModbusServoDriver: moveTo() refused — not homed/enabled, or a bus fault is latched.");
        return false;
    }

    pos_mm = constrain(pos_mm, 0.0f, effectiveCeilingMm());
    float target_counts = -(float)mmToNative(pos_mm);   // front = negative counts, same convention as FAS

    float speed_counts_s  = _max_speed_mm_s * nativePerMm();
    float accel_counts_s2 = _accel_mm_s2    * nativePerMm();

    _target_counts = target_counts;
    // Jerk-limited tracker: just move the target; the executor glides. :3
    _executor.track(target_counts, speed_counts_s, accel_counts_s2);
    // A discrete moveTo() must not be masked by a stale streamToSteps()
    // grit-cache hit later. :3
    _have_last_stream = false;

    SLOGD("servo", "ModbusServoDriver moveTo: %.1fmm -> %.0f counts (v=%.0f a=%.0f counts/s, counts/s^2)",
          pos_mm, target_counts, speed_counts_s, accel_counts_s2);
    return true;
}

void ModbusServoDriver::streamTo(float pos_mm, float speed_mm_s) {
    if (!_homed || !_enabled || _bus_fault) return;

    pos_mm = constrain(pos_mm, 0.0f, effectiveCeilingMm());
    float target_counts = -(float)mmToNative(pos_mm);

    float spd = (speed_mm_s > 0.0f) ? speed_mm_s : _max_speed_mm_s;
    spd = constrain(spd, 1.0f, _max_speed_mm_s);
    float speed_counts_s  = spd          * nativePerMm();
    float accel_counts_s2 = _accel_mm_s2 * nativePerMm();

    _target_counts = target_counts;
    _executor.track(target_counts, speed_counts_s, accel_counts_s2);
    _have_last_stream = false;
}

// Pre-planned native-count dispatch — called from Core 1 (the stream sampler
// / motionConsumerTask) at up to ~1kHz. GRIT-CACHE FIRST (plan.md): skip
// TrapezoidProfile::plan() entirely (sqrtf + branches) when nothing changed
// since last call. :3
void ModbusServoDriver::streamToSteps(int32_t target_steps,
                                       uint32_t speed_steps_s,
                                       uint32_t accel_steps_s2) {
    if (!_homed || !_enabled || _bus_fault) return;

    if (_have_last_stream &&
        target_steps  == _last_target_steps &&
        speed_steps_s  == _last_speed_steps_s &&
        accel_steps_s2 == _last_accel_steps_s2) {
        return;   // byte-identical waypoint — nothing to re-plan
    }
    _have_last_stream     = true;
    _last_target_steps    = target_steps;
    _last_speed_steps_s   = speed_steps_s;
    _last_accel_steps_s2  = accel_steps_s2;

    // Hard bounds — clamp into [-ceiling_counts, 0] in cmd-frame, the same
    // physical fence the arbiter itself enforces before dispatch. Belt and
    // suspenders: this driver is the last stop before a wire frame goes out. :3
    int32_t ceiling_counts = mmToNative(effectiveCeilingMm());
    int32_t clamped = target_steps;
    if (clamped > 0) clamped = 0;
    if (clamped < -ceiling_counts) clamped = -ceiling_counts;

    _target_counts = (float)clamped;
    // Jerk-limited tracker — the 1kHz stream path just moves the target along
    // the interpolator's cubic; the tracker glides after it. This is what
    // fixed the trapezoid-confetti roughness (see ServoMotionExecutor.h). :3
    _executor.track((float)clamped, (float)speed_steps_s, (float)accel_steps_s2);
}

void ModbusServoDriver::stop() {
    _executor.freeze();
    _have_last_stream = false;
    // Queued, not a raw sendWriteCommand() — stop() can be called from any
    // task/core (arbiter's Core 1 e-stop path, WebUI's Core 0 handlers), and
    // the write queue is the only thread-safe way into ServoModbus from
    // outside whichever task currently owns update() (servoBusTask in
    // Modbus mode). "Output off" per MotorDriver.h's "cut power" semantics
    // for stop() vs hardStop(). :3
    _bus.queueWrite(0x01, 0);
    _homed   = false;
    _enabled = false;
    SLOGI("servo", "ModbusServoDriver: stop() — output off (queued), homed cleared.");
}

void ModbusServoDriver::hardStop() {
    // Setpoint-freeze: servo-hold at the current sample, stays powered,
    // _homed is NOT touched — this is "stop moving," not "cut power." :3
    _executor.freeze();
    _have_last_stream = false;
}

// ---- Enable / Disable ----------------------------------------------------------

void ModbusServoDriver::enable() {
    _bus.queueWrite(0x01, 1);
    _enabled = true;
}

void ModbusServoDriver::disable() {
    _executor.freeze();
    _bus.queueWrite(0x01, 0);
    _enabled = false;
}

// ---- Speed & Acceleration ----------------------------------------------------

void ModbusServoDriver::setMaxSpeed(float speed_mm_s) {
    _max_speed_mm_s = constrain(speed_mm_s, 0.0f, MAX_SPEED_MM_S);
}

void ModbusServoDriver::setAcceleration(float accel_mm_s2) {
    _accel_mm_s2 = constrain(accel_mm_s2, 10.0f, MAX_ACCEL_MM_S2);
}

// ---- Status --------------------------------------------------------------------

float ModbusServoDriver::getPosition() const {
    // "Commanded = truth" (plan.md) — the executor's last sampled position IS
    // the reported position, exactly like AIMServoDriver reads FAS's own
    // commanded step counter rather than any external feedback. :3
    return nativeToMm(-(int32_t)lroundf(_executor.commandedPos()));
}

float ModbusServoDriver::getTargetPosition() const {
    return nativeToMm(-(int32_t)lroundf(_target_counts));
}

// ---- Driver config -------------------------------------------------------------

void ModbusServoDriver::applyDriverConfig(const DriverConfig& cfg) {
    // No TMC-style register map to apply — the AIM drive's gains live on the
    // Configure pane (handleApiServo talks to ServoModbus directly). This is
    // the MINIMAL Modbus-mode expected register set: output state (matches
    // our own _enabled flag) + torque/current clamp. No PID writes. :3
    (void)cfg;
    _bus.queueWrite(0x01, _enabled ? 1 : 0);
    _bus.queueWrite(0x18, AIM_MODBUS_STANDSTILL_MAX);
    SLOGI("servo", "ModbusServoDriver: applyDriverConfig() — queued output=%d, torque clamp reg 0x18=%d",
          (int)_enabled, (int)AIM_MODBUS_STANDSTILL_MAX);
}

// ---- Diagnostics -----------------------------------------------------------------

uint16_t ModbusServoDriver::getCurrentmA() {
    return (uint16_t)(_current.cachedCurrentA() * 1000.0f);
}

// ---- Unit conversion (encoder counts — see class doc in the header) -----------

int32_t ModbusServoDriver::mmToNative(float mm) const {
    return (int32_t)lroundf(mm * AIM_ENC_COUNTS_PER_MM);
}

float ModbusServoDriver::nativeToMm(int32_t native) const {
    return (float)native / AIM_ENC_COUNTS_PER_MM;
}

#endif // defined(DRIVER_AIM_SERVO) && defined(FEATURE_RS485_MODBUS)
