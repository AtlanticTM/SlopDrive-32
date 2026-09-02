// ModbusServoDriver — FAS-bypass direct-drive backend, Phase 3: real motion.
//
// Constraints:
// - Build-guarded behind DRIVER_AIM_SERVO && FEATURE_RS485_MODBUS.
// - Streamed 0x7B setpoints via StreamedSetpointExecutor (the "ISR" for this
//   backend — see ServoMotionExecutor.h). Every motion entry point stays
//   gated behind the exact same homed/enabled discipline FAS mode uses
//   (architecture.md — nothing moves spontaneously); the only way _homed
//   becomes true this phase is the BENCH forceHomeState() path (real homing
//   is Phase 4).
// See: ModbusServoDriver.h for the full doctrine writeup.
#if defined(DRIVER_AIM_SERVO) && defined(FEATURE_RS485_MODBUS)

#include "ModbusServoDriver.h"
#include "ServoModbus.h"
#include "MachineConfig.h"   // home_style selects which homing cycle runs
#include "sloplog/sloplog.h"
#include <math.h>       // lroundf
#include <esp_timer.h>  // esp_timer_get_time()

ModbusServoDriver::ModbusServoDriver(ServoModbus& bus)
    : _bus(bus)
    , _executor(bus)
{}

// ---- Lifecycle --------------------------------------------------------------

void ModbusServoDriver::init() {
    // Bring up the INA228 the same way AIMServoDriver does — the Wire bus is
    // already up (main setup() calls Wire.begin() before motor.init()).
    if (!_current.init()) {
        SLOGW("servo", "ModbusServoDriver: WARNING — INA228 not found, live current telemetry unavailable. uhoh :3");
    }

    // Jerk ceiling for the executor's tracking integrator — OSSM-RS parity
    // (their Ruckig runs 100000 mm/s^3), converted to native counts.
    _executor.setJerkLimit(AIM_MODBUS_JERK_MM_S3 * AIM_ENC_COUNTS_PER_MM);

    // 0x7B is fire-and-forget: the drive does not echo it, and echo-waiting
    // would burn a 15ms timeout per frame and stall the motion path.
    _bus.setSetpointNoEcho(true);
    // Encoder priority is DELIBERATELY OFF. Giving it every other poll slot
    // starved the setpoint stream and garbled the 2-register read badly enough
    // that the staleness watchdog froze motion mid-stroke. Faster feedback is
    // not worth stuttering the thing the feedback is about.
    _bus.setEncoderPriority(false);
    // BLOCKING (~900ms), init context only. Without it the drive accepts
    // setpoints and does not move.
    _bus.armMotionControl();

    // Deliberately do NOT enable the drive output here — Modbus mode never
    // spontaneously energizes anything at boot, exactly like FAS mode never
    // pulses the motor before a home. Output only comes on via
    // forceHomeState(true)/enable() (Phase 4's real home() will do the same).
    SLOGI("servo", "ModbusServoDriver: init — Modbus direct-drive backend, %.1f counts/mm, "
          "wire_sign=%d (bench-tune AIM_MODBUS_WIRE_SIGN if position runs backwards)",
          AIM_ENC_COUNTS_PER_MM, (int)_wire_sign);
}

void ModbusServoDriver::update() {
    // ---- INA228 telemetry cache refresh — identical two-tier cadence to -----
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

    // ---- Link watchdog: encoder staleness -----------------------------------
    // The setpoint is fire-and-forget, so a fresh encoder sample is the only
    // honest proof the drive is still listening. Only armed once homed: an
    // unhomed machine is not streaming and must not fault on a quiet bus.
    // Both branches are one-shot logged (this runs on the 1ms motorTask tick),
    // and the fault clears ONLY via home()/forceHomeState().
    ServoTelemetry t = _bus.getTelemetry();

    // Encoder counts run back through the SAME mapping the setpoints go out
    // on, so a nonzero difference against getPosition() is real following
    // error and never a frame mismatch. _wire_sign is +/-1, so dividing is
    // multiplying. Refreshed BEFORE the unhomed early-return and invalidated
    // with _homed, because _wire_offset means nothing without a home.
    // ONE getTelemetry() serves both this and the watchdog below.
    _measured_valid = _homed && t.enc_valid;
    if (_measured_valid) {
        _measured_mm = nativeToMm(-((t.enc_counts - _wire_offset) * (int32_t)_wire_sign));
    }

    if (!_homed) {
        _fault_freeze_logged = false;
        _fault_estop_logged  = false;
        return;
    }

    // REPORT-ONLY. This detector does not stop motion, by ruling: at 400ms and
    // again at 2000ms it fired on a healthy link that was merely busy and
    // FROZE the machine mid-stroke, twice, while catching no real fault. The
    // encoder poll shares one wire with the setpoint stream, so its freshness
    // measures BUS LOAD, not link liveness, and a load measurement must never
    // gate motion (T16, and T30: a detector reports, it does not adjudicate).
    // A genuinely dead link still surfaces: _ready drops, setpoints stop
    // landing, and the operator's e-stop is unaffected.
    uint32_t age = t.enc_valid ? (millis() - t.enc_stamp_ms) : AIM_ENC_STALE_ESTOP_MS;
    if (age >= AIM_ENC_STALE_ESTOP_MS) {
        if (!_fault_estop_logged) {
            _fault_estop_logged = true;
            SLOGW("servo", "ModbusServoDriver: encoder silent %ums -- feedback is stale, motion "
                  "CONTINUES (report-only). Check bus load. uhoh :3", (unsigned)age);
        }
    } else if (age >= AIM_ENC_STALE_FREEZE_MS) {
        if (!_fault_freeze_logged) {
            _fault_freeze_logged = true;
            SLOGI("servo", "ModbusServoDriver: encoder sample %ums old (report-only)",
                  (unsigned)age);
        }
    } else {
        _fault_freeze_logged = false;
        _fault_estop_logged  = false;
    }
}

// A homing task drives the executor DIRECTLY, bypassing the _homed gate that
// keeps the arbiter out. Any path that declares the machine homed or stopped
// must therefore kill it first, or two writers fight over _target and the
// carriage oscillates between them. Bit us live at fw 2.4.6 via force_home.
static constexpr uint32_t kHomingAbortWaitMs = 200;
void ModbusServoDriver::_killHomingTask() {
    std::atomic_ref<TaskHandle_t> h(_homing_task);
    if (h.load(std::memory_order_acquire) != nullptr) {
        // Cooperative: the task polls _homing_abort at every wait and exits
        // through _homingExit(); wait, bounded, so no second writer overlaps
        // it on the executor. Task context only (vTaskDelay).
        _homing_abort.store(true, std::memory_order_release);
        const uint32_t t0 = millis();
        while (h.load(std::memory_order_acquire) != nullptr &&
               millis() - t0 < kHomingAbortWaitMs) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (h.load(std::memory_order_acquire) == nullptr) {
            SLOGW("servo", "ModbusServoDriver: homing task aborted (%lu ms) -- it owns the "
                  "executor and must never run alongside another writer.",
                  (unsigned long)(millis() - t0));
        } else {
            SLOGE("servo", "ModbusServoDriver: homing task did not exit within %lu ms; "
                  "executor unseeded, task left to unwind. uhoh :C",
                  (unsigned long)kHomingAbortWaitMs);
        }
    }
    _homing = false;
}

// The ONE exit of the homing task. Clears the handle last (release).
void ModbusServoDriver::_homingExit() {
    _homing_abort.store(false, std::memory_order_relaxed);
    std::atomic_ref<TaskHandle_t>(_homing_task).store(nullptr, std::memory_order_release);
    vTaskDelete(nullptr);
    for (;;) {}
}

void ModbusServoDriver::emergencyStop() {
    _killHomingTask();
    // unseed(), NOT freeze(): freeze keeps re-asserting the held position on
    // the keep-alive cadence, so an e-stop issued while the carriage is
    // leaning on a hard stop leaves it leaning. Measured live at fw 2.4.2.
    _executor.unseed();
    _have_last_stream = false;
    _bus.emergencyStop();   // immediate wire write (0x01=0) + write-queue clear
    _homing  = false;
    _homed   = false;
    _enabled = false;
    SLOGW("servo", "ModbusServoDriver: EMERGENCY STOP -- output off, setpoint stream stopped.");
}

// ---- Modbus re-arm ----------------------------------------------------------
// The drive falls out of Modbus mode on its own: after a built-in home reg
// 0x00 reads 0 and every 0x7B setpoint is silently ignored while position is
// still held. Setting 0x00 back clobbers speed and torque, so both follow it.
// Queue-only, no delays: callers include shared tasks, and blocking one is
// what wedged fw 2.4.0 (sd-2ns). See docs/reversal-drift.md.
void ModbusServoDriver::_rearmModbus() {
    _bus.queueWrite(0x00, 1);
    _bus.queueWrite(0x02, AIM_MODBUS_ARM_SPEED_RPM);
    // Re-arm is the other 0x03 writer, so it defers to the override too.
    _bus.queueWrite(0x03, _bus.accelRegOverride() ? _bus.accelRegOverride()
                                                  : AIM_MODBUS_ARM_ACCEL);
    _bus.queueWrite(0x05, AIM_MODBUS_ARM_SPEED_P);
    _bus.queueWrite(0x07, AIM_MODBUS_ARM_POS_P);
    _bus.queueWrite(0x18, AIM_MODBUS_STANDSTILL_MAX);
    _bus.queueWrite(0x01, 1);
    // This just overwrote 0x02/0x03 with the arm values, so the per-move limit
    // cache is now a lie. Invalidate it or the next stream keeps the drive
    // wide open and the user's speed/accel are silently ignored.
    _wire_rpm = _wire_accel = 0;
}

// ---- Homing -----------------------------------------------------------------

// Sensorless two-wall home, same flow and same resulting geometry as
// AIMServoDriver::_homingTask(): find the front stop, back off, find the rear
// stop, back off, and call THAT home (0mm). The only difference is the wall
// detector. See the header.
bool ModbusServoDriver::home(int32_t home_speed_steps_s) {
    if (_homing || std::atomic_ref<TaskHandle_t>(_homing_task).load(std::memory_order_acquire) != nullptr)
        return false;
    _homing_abort.store(false, std::memory_order_release);
    ServoTelemetry t = _bus.getTelemetry();
    if (!t.enc_valid) {
        SLOGW("servo", "ModbusServoDriver: home() REFUSED -- no encoder sample, so no way "
              "to feel a wall or to seed a motionless first frame. uhoh :C");
        return false;
    }

    // MotorProxy forwards -1 as "no opinion" (T9: a proxy passes a sentinel,
    // never a restated default). Anything non-positive means use the doctrine
    // speed, and the ceiling is never exceeded: homing drives into a hard stop.
    float mm_s = (home_speed_steps_s > 0)
                     ? (float)home_speed_steps_s / AIM_STEPS_PER_MM
                     : AIM_HOMING_SPEED_MM_S;
    if (mm_s > AIM_HOMING_SPEED_MM_S) mm_s = AIM_HOMING_SPEED_MM_S;
    _home_speed_counts_s = (int32_t)(mm_s * AIM_ENC_COUNTS_PER_MM);

    // Adopt the shaft's CURRENT position as cmd 0 so the first streamed frame
    // commands exactly where it already is: never command motion before a live
    // encoder seed exists. The real zero is re-established at the end.
    _wire_offset = t.enc_counts;
    _executor.setWireMap(_wire_offset, AIM_MODBUS_WIRE_SIGN);
    _executor.seed(0.0f);
    _target_counts       = 0.0f;
    _have_last_stream    = false;
    _bus_fault           = false;
    _fault_freeze_logged = false;
    _fault_estop_logged  = false;
    _homed   = false;
    _homing  = true;
    _enabled = true;
    // Homing is the RECOVERY action, so it re-arms rather than assuming the
    // drive is still listening: after an e-stop, or after a previous built-in
    // home, reg 0x00 may be 0 and nothing would move.
    _rearmModbus();

    // Core 1 with the rest of motion. 4 KB is ample: this task does float
    // compare and vTaskDelay, and never touches Ruckig (T1 does not apply).
    if (xTaskCreatePinnedToCore(_homingTaskImpl, "mbhome", 4096, this, 2,
                                &_homing_task, 1) != pdPASS) {
        _homing_task = nullptr;
        _homing = false;
        SLOGE("servo", "ModbusServoDriver: home() FAILED -- could not create homing task.");
        return false;
    }
    return true;
}

void ModbusServoDriver::_homingTaskImpl(void* param) {
    static_cast<ModbusServoDriver*>(param)->_homingTask();
}

// Blocking point move, homing context ONLY: the arbiter is locked out while
// _homed is false, so nothing else is competing for the executor here.
void ModbusServoDriver::_moveAndWait(float target_counts, uint32_t timeout_ms) {
    const float accel = AIM_MODBUS_HOME_ACCEL_MM_S2 * AIM_ENC_COUNTS_PER_MM;
    _executor.track(target_counts, (float)_home_speed_counts_s, accel);
    // Arrival is the DRIVE's to report, not ours. The executor is a
    // pass-through now, so its commanded position reaches the target on the
    // very next tick while the shaft is still traveling; waiting on that
    // would return instantly and call the move done before it happened.
    uint32_t deadline = millis() + timeout_ms;
    uint32_t stamp = 0;
    while (millis() < deadline) {
        if (_homing_abort.load(std::memory_order_acquire)) return;
        _bus.requestRemaining();
        vTaskDelay(pdMS_TO_TICKS(30));
        ServoTelemetry t = _bus.getTelemetry();
        if (t.rem_valid && t.rem_stamp_ms != stamp) {
            stamp = t.rem_stamp_ms;
            if (labs((long)t.rem_counts) < AIM_DRIVE_HOME_ARRIVE_CNT) return;
        }
    }
}

// Drive one direction until the encoder stops keeping up. On success the
// executor is RE-SEEDED onto the shaft's real position, which both records the
// wall and stops the drive pushing into it; commandedPos() is then the wall.
bool ModbusServoDriver::_sweepToWall(int8_t dir_sign) {
    const float start        = _executor.commandedPos();
    const float sweep_counts = (float)mmToNative(_max_rail_mm * 1.2f);
    const float target       = start + (float)dir_sign * sweep_counts;
    const float stall_counts = AIM_MODBUS_HOME_STALL_MM * AIM_ENC_COUNTS_PER_MM;
    const float accel        = AIM_MODBUS_HOME_ACCEL_MM_S2 * AIM_ENC_COUNTS_PER_MM;
    const float move_eps     = AIM_MODBUS_HOME_MOVED_MM * AIM_ENC_COUNTS_PER_MM;
    const uint32_t poll_ms   = 1000u / AIM_HOME_POLL_HZ;

    ServoTelemetry t0 = _bus.getTelemetry();
    const float    a0 = (float)((t0.enc_counts - _wire_offset) * (int32_t)_wire_sign);
    uint32_t last_stamp = t0.enc_stamp_ms;

    _executor.track(target, (float)_home_speed_counts_s, accel);

    // ARMING. At t=0 the shaft is stationary and the commanded position
    // accelerates away from it, so following error at the START of a sweep is
    // indistinguishable from a wall -- which is exactly how the first version
    // of this "found" a wall in 186 ms at 0.0 mm. Hold the detector until the
    // shaft has demonstrably moved, or the grace window expires. Both outcomes
    // are correct: free travel arms on real motion, and a carriage already
    // against a stop never moves, so grace expires and the detector arms into
    // a genuine stall.
    bool     armed    = false;
    uint8_t  over     = 0;
    uint32_t started  = millis();
    uint32_t deadline = started +
        (uint32_t)(1000.0f * (_max_rail_mm * 1.2f) / (AIM_HOMING_SPEED_MM_S * 0.5f)) + 4000;
    while (millis() < deadline) {
        if (_homing_abort.load(std::memory_order_acquire)) return false;
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        ServoTelemetry t = _bus.getTelemetry();
        if (!t.enc_valid) continue;
        // FRESH SAMPLES ONLY. The poll runs at AIM_HOME_POLL_HZ but the encoder
        // reports at ~25 Hz, so re-reading the cache would let one observation
        // satisfy the whole CONSEC debounce and make it decorative.
        if (t.enc_stamp_ms == last_stamp) continue;
        last_stamp = t.enc_stamp_ms;

        const float cmd = _executor.commandedPos();
        const float act = (float)((t.enc_counts - _wire_offset) * (int32_t)_wire_sign);
        if (!armed) {
            if (fabsf(act - a0) > move_eps ||
                (millis() - started) > AIM_MODBUS_HOME_GRACE_MS) {
                armed = true;
            }
            continue;
        }

        if (fabsf(cmd - act) > stall_counts) {
            if (++over >= AIM_MODBUS_HOME_STALL_CONSEC) {
                // PLAUSIBILITY FLOOR, the mirror of FAS's
                // AIM_HOME_STALL_PLAUSIBLE_FRAC: a wall found at the sweep
                // bound is a fault, and so is one found without traveling.
                // The carriage cannot be against a stop it started clear of,
                // so this means it never moved -- output not energized, or the
                // drive is not listening. Never call that a home.
                if (fabsf(act - a0) < move_eps) {
                    SLOGW("servo", "ModbusServo Homing: 'wall' at %.1f mm of travel -- the shaft "
                          "never moved. Output not energized, or the drive is ignoring "
                          "setpoints. NOT a home. uhoh :C",
                          (double)nativeToMm((int32_t)(act - a0)));
                    return false;
                }
                // Snap the model onto the shaft. Without this the drive keeps
                // leaning on the stop by the whole detection threshold.
                _executor.seed(act);
                return true;
            }
        } else {
            over = 0;
        }
        // Ran the whole search bound without touching anything: that is a
        // fault, not a home. Never trust a wall found at the sweep limit.
        if (!_executor.active() && fabsf(cmd - target) < 4.0f) return false;
    }
    return false;
}

// The drive runs its own cycle; we watch the ENCODER rather than a status
// register, because the only completion status the translated datasheet
// documents is the save flag (0x14), which this does not touch.
//
// TRAP (quad_probe, fw 2.3.x): 0x19 is INERT while the electronic gear 0x0A is
// programmed nonzero. The drive ACCEPTS the write and does nothing, which is
// indistinguishable from a dead command. Reported, never "fixed" here: 0x0A is
// step/dir geometry that FAS mode boots against, so zeroing it silently would
// break the other backend.
bool ModbusServoDriver::_driveHome() {
    ServoConfig cfg = _bus.getConfig();
    if (cfg.valid && (cfg.known & (1UL << 0x0A)) && cfg.regs[0x0A] != 0) {
        SLOGW("servo", "ModbusServo Homing: reg 0x0A (e-gear) = %u. That gates the drive's "
              "special functions, so 0x19 home may be silently ignored. NOT zeroing it: it is "
              "step/dir geometry FAS mode depends on.", (unsigned)cfg.regs[0x0A]);
    }

    const float move_eps = AIM_MODBUS_HOME_MOVED_MM * AIM_ENC_COUNTS_PER_MM;
    ServoTelemetry t0 = _bus.getTelemetry();
    if (!t0.enc_valid) return false;
    int32_t  ref        = t0.enc_counts;
    uint32_t last_stamp = t0.enc_stamp_ms;

    // Gentle approach, ossm-rs order: slow speed and REDUCED torque before the
    // cycle starts, so the drive eases into the hard stop instead of slamming
    // it (measured 4 A at full torque). _rearmModbus() restores both after.
    _bus.queueWrite(0x02, AIM_DRIVE_HOME_SPEED_RPM);
    _bus.queueWrite(0x18, AIM_DRIVE_HOME_TORQUE);
    vTaskDelay(pdMS_TO_TICKS(120));   // let the queue land before the GO
    _bus.queueWrite(0x19, 1);   // GO. Thread-safe queue: the bus is servoBusTask's.
    SLOGI("servo", "ModbusServo Homing: drive cycle requested (0x19=1) at speed %u torque %u",
          (unsigned)AIM_DRIVE_HOME_SPEED_RPM, (unsigned)AIM_DRIVE_HOME_TORQUE);

    // Phase 1: it has to actually start.
    const uint32_t t_start = millis();
    bool moved = false;
    while (millis() - t_start < AIM_DRIVE_HOME_START_MS) {
        if (_homing_abort.load(std::memory_order_acquire)) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
        ServoTelemetry t = _bus.getTelemetry();
        if (!t.enc_valid || t.enc_stamp_ms == last_stamp) continue;
        last_stamp = t.enc_stamp_ms;
        if (fabsf((float)(t.enc_counts - ref)) > move_eps) { moved = true; break; }
    }
    if (!moved) {
        SLOGW("servo", "ModbusServo Homing: drive never moved after 0x19=1. Most likely reg 0x0A "
              "is nonzero (special functions gated), or the drive output is not energized. "
              "uhoh :C");
        return false;
    }

    // Phase 2: ARRIVAL, from the drive's own steps-to-go register (0x0C/0x0D).
    // This is a positive signal, so the cycle ends the instant the drive says
    // it is there. The old stillness window could only ever GUESS, and it cost
    // 5-10s of dead time per home plus a false completion on any mid-cycle
    // dwell. Stillness survives ONLY as the fallback for a drive that never
    // answers the register.
    uint32_t last_move_ms = millis();
    uint32_t rem_stamp    = 0;
    bool     rem_seen     = false;
    while (millis() - t_start < AIM_DRIVE_HOME_TIMEOUT_MS) {
        if (_homing_abort.load(std::memory_order_acquire)) return false;
        _bus.requestRemaining();
        vTaskDelay(pdMS_TO_TICKS(40));
        ServoTelemetry t = _bus.getTelemetry();

        if (t.rem_valid && t.rem_stamp_ms != rem_stamp) {
            rem_stamp = t.rem_stamp_ms;
            rem_seen  = true;
            if (labs((long)t.rem_counts) < AIM_DRIVE_HOME_ARRIVE_CNT) {
                SLOGI("servo", "ModbusServo Homing: drive reports ARRIVED (%ld steps to go) "
                      "after %lums", (long)t.rem_counts,
                      (unsigned long)(millis() - t_start));
                return true;
            }
        }

        if (!t.enc_valid || t.enc_stamp_ms == last_stamp) continue;
        last_stamp = t.enc_stamp_ms;
        if (fabsf((float)(t.enc_counts - ref)) > move_eps) {
            ref = t.enc_counts;
            last_move_ms = millis();
        } else if (!rem_seen && millis() - last_move_ms > AIM_DRIVE_HOME_STILL_MS) {
            SLOGW("servo", "ModbusServo Homing: 0x0C/0x0D never answered -- fell back to the "
                  "stillness guess, settled after %lums",
                  (unsigned long)(millis() - t_start));
            return true;
        }
    }
    SLOGW("servo", "ModbusServo Homing: drive cycle did not settle within %lums. uhoh :C",
          (unsigned long)AIM_DRIVE_HOME_TIMEOUT_MS);
    return false;
}

void ModbusServoDriver::_homingTask() {
    if (_homing_abort.load(std::memory_order_acquire)) { _homing = false; _homed = false; _homingExit(); }
    // ---- home_style 1: the drive homes itself ------------------------------
    // Preferred on this machine (operator: the built-in cycle works well), and
    // it never runs at boot: nothing calls home() until an operator or the
    // arbiter asks for it.
    if (machineHomeStyleLoad() == 1) {
        // The executor must stop transmitting before the drive moves on its
        // own, or its keep-alive re-asserts the seeded position and the two
        // fight. freeze() is NOT enough -- it keeps re-sending.
        _executor.unseed();
        bool ok = _driveHome();
        if (ok) {
            ServoTelemetry t = _bus.getTelemetry();
            if (t.enc_valid) {
                // The drive homes INTO the hard stop and parks there pushing:
                // measured live, 4 A of stall with no reverse of its own. So
                // the resting position is the WALL, not home. Adopt it, back
                // off exactly like the FAS path does, and call THAT 0 -- the
                // carriage must never be left leaning on a stop.
                SLOGI("servo", "ModbusServo Homing: drive parked at the stop (motor %.2f A), "
                      "backing off %.1f mm", (double)t.current_a, AIM_HOMING_BACKOFF_MM);

                _rearmModbus();
                vTaskDelay(pdMS_TO_TICKS(1200));   // let the queue land and settle

                // PROVE it took, never assume: a home that leaves the drive
                // deaf is worse than a refused home, because the machine then
                // reports homed and silently ignores every command.
                if (!_bus.getTelemetry().enabled) {
                    SLOGE("servo", "ModbusServo Homing: reg 0x00 did NOT come back after the "
                          "drive cycle -- setpoints would be ignored. NOT declaring homed. uhoh :C");
                    _executor.unseed();
                    _homing = false;
                    _homed  = false;
                    _homingExit();
                }

                _wire_offset = t.enc_counts;
                _executor.setWireMap(_wire_offset, AIM_MODBUS_WIRE_SIGN);
                _executor.seed(0.0f);
                _home_speed_counts_s = (int32_t)(AIM_HOMING_SPEED_MM_S * AIM_ENC_COUNTS_PER_MM);
                _moveAndWait(-(float)mmToNative(AIM_HOMING_BACKOFF_MM), 8000);
                vTaskDelay(pdMS_TO_TICKS(200));

                // Re-zero from a LIVE sample, so any following error left over
                // from the backoff cannot bake itself into the origin.
                ServoTelemetry t2 = _bus.getTelemetry();
                if (t2.enc_valid) _wire_offset = t2.enc_counts;
                _executor.setWireMap(_wire_offset, AIM_MODBUS_WIRE_SIGN);
                _executor.seed(0.0f);
                _target_counts    = 0.0f;
                _have_last_stream = false;
                _homed  = true;
                _homing = false;
                SLOGI("servo", "ModbusServo Homing: COMPLETE (drive built-in) -- home = 0mm, "
                      "%.1f mm off the stop, stroke %.1f mm. yippie! :3",
                      AIM_HOMING_BACKOFF_MM, (double)getMeasuredStrokeMm());
                _homingExit();
            }
            SLOGW("servo", "ModbusServo Homing: drive finished but no encoder sample to adopt "
                  "as zero -- refusing to declare homed. uhoh :C");
        }
        _executor.freeze();
        _homing = false;
        _homed  = false;
        _homingExit();
    }

    SLOGI("servo", "ModbusServo Homing: START (following-error detector) %.1f mm/s",
          (float)_home_speed_counts_s / AIM_ENC_COUNTS_PER_MM);

    if (_homing_abort.load(std::memory_order_acquire)) {
        SLOGW("servo", "ModbusServo Homing: ABORTED during the front sweep.");
        _executor.unseed(); _homing = false; _homed = false; _homingExit();
    }
    if (!_sweepToWall(-1)) {
        SLOGW("servo", "ModbusServo Homing: FAILED -- no wall on the front sweep. Check "
              "AIM_MODBUS_HOME_STALL_MM, the rail length, and that output is energized.");
        _executor.freeze();
        _homing = false;
        _homed  = false;
        _homingExit();
    }
    const float front_counts = _executor.commandedPos();
    SLOGI("servo", "ModbusServo Homing: front wall at %.1f mm", nativeToMm((int32_t)-front_counts));

    // Pull off the front stop before sweeping back. Unlike the FAS path this
    // is not about a current baseline; it is so the rear sweep starts with
    // zero following error instead of the stall it just latched.
    _moveAndWait(front_counts + (float)mmToNative(AIM_HOMING_BACKOFF_MM), 8000);
    vTaskDelay(pdMS_TO_TICKS(150));

    if (_homing_abort.load(std::memory_order_acquire)) {
        SLOGW("servo", "ModbusServo Homing: ABORTED during the rear sweep.");
        _executor.unseed(); _homing = false; _homed = false; _homingExit();
    }
    if (!_sweepToWall(+1)) {
        SLOGW("servo", "ModbusServo Homing: FAILED -- no wall on the rear sweep.");
        _executor.freeze();
        _homing = false;
        _homed  = false;
        _homingExit();
    }
    const float rear_counts = _executor.commandedPos();

    // Back off the rear stop; THAT spot is home.
    _moveAndWait(rear_counts - (float)mmToNative(AIM_HOMING_BACKOFF_MM), 8000);
    vTaskDelay(pdMS_TO_TICKS(150));

    // Re-establish the wire mapping so home reads exactly 0. Taken from a
    // LIVE encoder sample, not from the model, so any following error left
    // over from the backoff move cannot bake itself into the zero.
    ServoTelemetry t = _bus.getTelemetry();
    if (t.enc_valid) _wire_offset = t.enc_counts;
    _executor.setWireMap(_wire_offset, AIM_MODBUS_WIRE_SIGN);
    _executor.seed(0.0f);
    _target_counts    = 0.0f;
    _have_last_stream = false;

    // MEASUREMENT WINS, same rule as the FAS path: the wall-to-wall span is
    // ground truth for the usable stroke, bounded only by a sanity check.
    float span_mm = fabsf(nativeToMm((int32_t)(rear_counts - front_counts)));
    float usable  = span_mm - AIM_HOMING_BACKOFF_MM - AIM_HOMING_FRONT_MARGIN_MM;
    if (usable > 0.0f && usable < 2000.0f) {
        setMeasuredStrokeMm(usable);
        SLOGI("servo", "ModbusServo Homing: span %.1f mm -> usable stroke %.1f mm :3",
              span_mm, usable);
    } else {
        SLOGW("servo", "ModbusServo Homing: span %.1f mm is implausible -- REJECTED, keeping "
              "the prior stroke. Home itself is still valid.", span_mm);
    }

    _homed  = true;
    _homing = false;
    SLOGI("servo", "ModbusServo Homing: COMPLETE -- homed at 0mm. yippie! :3");
    _homingExit();
}

// BENCH PATH (HOME_OVERRIDE) — see the header's Constraints note.
void ModbusServoDriver::forceHomeState(bool homed) {
    // Before declaring homed: a live homing task would keep writing the
    // executor while the arbiter is now unblocked, and the two fight.
    _killHomingTask();
    if (homed) {
        ServoTelemetry t = _bus.getTelemetry();
        if (!t.enc_valid) {
            SLOGW("servo", "ModbusServoDriver: forceHomeState(true) REFUSED — no valid encoder "
                  "reading yet (bus not ready / never polled). uhoh :C");
            return;
        }

        // Establish the wire mapping from WHATEVER the shaft is sitting at
        // right now — NOT a real homed position. Seeds the executor at cmd=0
        // so the wire mapping's identity (wire_offset + sign*0 == wire_offset
        // == the encoder reading we just read) makes the FIRST setpoint we
        // ever send exactly equal to the current physical position: zero
        // motion — never command motion before a live encoder seed exists.
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
        // Re-arm, not just energize: the drive may have been left out of
        // Modbus mode by an earlier built-in home, in which case output-on
        // alone leaves it deaf to every setpoint.
        _rearmModbus();

        SLOGI("servo", "ModbusServoDriver: forceHomeState(true) — BENCH fake-home, "
              "wire_offset=%ld counts, wire_sign=%d :3",
              (long)_wire_offset, (int)AIM_MODBUS_WIRE_SIGN);
    } else {
        _homed = false;
        SLOGI("servo", "ModbusServoDriver: forceHomeState(false) — cleared, real homing required.");
    }
}

// ---- Motion -----------------------------------------------------------------

// Pre-planned native-count dispatch — called from Core 1 via MotionArbiter at
// up to ~1kHz. GRIT-CACHE FIRST: skip the executor
// hand-off entirely when nothing changed since last call.
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
    // suspenders: this driver is the last stop before a wire frame goes out.
    int32_t ceiling_counts = mmToNative(effectiveCeilingMm());
    int32_t clamped = target_steps;
    if (clamped > 0) clamped = 0;
    if (clamped < -ceiling_counts) clamped = -ceiling_counts;

    // ---- Push the LIMITS to the drive, not to a controller in front of it ---
    // The drive profiles to every setpoint using regs 0x02/0x03. Left at the
    // wide-open arm values it sprints to each waypoint and idles, which is
    // felt as jitter, and the user's speed/accel are ignored entirely. Written
    // only on CHANGE: limits move on operator action, never per sample.
    // Motor RPM = mm/s * 60 * REDUCTION / MM_PER_REV.
    const float mm_s      = (float)speed_steps_s / AIM_ENC_COUNTS_PER_MM;
    const float mm_s2     = (float)accel_steps_s2 / AIM_ENC_COUNTS_PER_MM;
    const float to_rpm    = 60.0f * AIM_REDUCTION / AIM_MM_PER_REV;
    uint16_t rpm   = (uint16_t)constrain(mm_s  * to_rpm, 1.0f, 3000.0f);
    uint16_t accel = (uint16_t)constrain(mm_s2 * to_rpm, 1.0f, 60000.0f);
    // The override outranks the derived value, or the next limit change stomps
    // the operator's setting one move after they made it.
    if (uint16_t ovr = _bus.accelRegOverride()) accel = ovr;
    if (rpm != _wire_rpm || accel != _wire_accel) {
        _wire_rpm   = rpm;
        _wire_accel = accel;
        _bus.queueWrite(0x02, rpm);
        _bus.queueWrite(0x03, accel);
    }

    _target_counts = (float)clamped;
    // Jerk-limited tracker — the 1kHz stream path just moves the target along
    // the interpolator's cubic; the tracker glides after it. This is what
    // fixed the trapezoid-confetti roughness (see ServoMotionExecutor.h).
    _executor.track((float)clamped, (float)speed_steps_s, (float)accel_steps_s2);
}

void ModbusServoDriver::stop() {
    // Same rule as emergencyStop(): "cut power" must also stop transmitting,
    // or the stream keeps commanding a position the output is no longer on for.
    _executor.unseed();
    _have_last_stream = false;
    // Queued, not a raw sendWriteCommand() — stop() can be called from any
    // task/core (arbiter's Core 1 e-stop path, WebUI's Core 0 handlers), and
    // the write queue is the only thread-safe way into ServoModbus from
    // outside whichever task currently owns update() (servoBusTask in
    // Modbus mode). "Output off" per MotorDriver.h's "cut power" semantics
    // for stop() vs hardStop().
    _bus.queueWrite(0x01, 0);
    _homed   = false;
    _enabled = false;
    SLOGI("servo", "ModbusServoDriver: stop() — output off (queued), homed cleared.");
}

void ModbusServoDriver::hardStop() {
    // Setpoint-freeze: servo-hold at the current sample, stays powered,
    // _homed is NOT touched — this is "stop moving," not "cut power."
    _executor.freeze();
    _have_last_stream = false;
}

// ---- Enable / Disable -------------------------------------------------------

void ModbusServoDriver::enable() {
    _bus.queueWrite(0x01, 1);
    _enabled = true;
}

void ModbusServoDriver::disable() {
    _executor.freeze();
    _bus.queueWrite(0x01, 0);
    _enabled = false;
}

// ---- Speed & Acceleration ---------------------------------------------------

void ModbusServoDriver::setMaxSpeed(float speed_mm_s) {
    _max_speed_mm_s = constrain(speed_mm_s, 0.0f, MAX_SPEED_MM_S);
}

void ModbusServoDriver::setAcceleration(float accel_mm_s2) {
    _accel_mm_s2 = constrain(accel_mm_s2, 10.0f, MAX_ACCEL_MM_S2);
}

// ---- Status -----------------------------------------------------------------

float ModbusServoDriver::getPosition() const {
    // "Commanded = truth" — the executor's last sampled position IS
    // the reported position, exactly like AIMServoDriver reads FAS's own
    // commanded step counter rather than any external feedback.
    return nativeToMm(-(int32_t)lroundf(_executor.commandedPos()));
}

float ModbusServoDriver::getTargetPosition() const {
    return nativeToMm(-(int32_t)lroundf(_target_counts));
}

// Plain member reads: both are called from the 250 Hz telemetry sampler, and
// getTelemetry() takes the same spinlock the Core-1 setpoint path needs.
// The cache is refreshed in update() (motorTask, 1 ms), far faster than the
// ~20 Hz the encoder itself reports at.
float ModbusServoDriver::getActualPosition() const {
    return _measured_valid ? _measured_mm : getPosition();
}

bool ModbusServoDriver::hasActualPosition() const {
    return _homed && _measured_valid;
}

// ---- Driver config ----------------------------------------------------------

void ModbusServoDriver::applyDriverConfig(const DriverConfig& cfg) {
    // No stepper-chip register map to apply — the AIM drive's gains live on the
    // Configure pane (handleApiServo talks to ServoModbus directly). This is
    // the MINIMAL Modbus-mode expected register set: output state (matches
    // our own _enabled flag) + torque/current clamp. No PID writes.
    (void)cfg;
    _bus.queueWrite(0x01, _enabled ? 1 : 0);
    _bus.queueWrite(0x18, AIM_MODBUS_STANDSTILL_MAX);
    SLOGI("servo", "ModbusServoDriver: applyDriverConfig() — queued output=%d, torque clamp reg 0x18=%d",
          (int)_enabled, (int)AIM_MODBUS_STANDSTILL_MAX);
}

// ---- Diagnostics ------------------------------------------------------------

uint16_t ModbusServoDriver::getCurrentmA() {
    return (uint16_t)(_current.cachedCurrentA() * 1000.0f);
}

// ---- Unit conversion (encoder counts — see class doc in the header) ---------

int32_t ModbusServoDriver::mmToNative(float mm) const {
    return (int32_t)lroundf(mm * AIM_ENC_COUNTS_PER_MM);
}

float ModbusServoDriver::nativeToMm(int32_t native) const {
    return (float)native / AIM_ENC_COUNTS_PER_MM;
}

#endif // defined(DRIVER_AIM_SERVO) && defined(FEATURE_RS485_MODBUS)
