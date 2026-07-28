// AIMServoDriver — concrete MotorDriver for the 57AIM30 closed-loop servo.
// Constraints:
// - Build-guarded behind DRIVER_AIM_SERVO (set in platformio.ini).
// - Dumb Step/Direction driver: no SPI, no Modbus, no register access. The
//   57AIM30 handles its own closed-loop control internally — this driver
//   only sends step pulses and a direction level.
#if defined(DRIVER_AIM_SERVO)

#include "AIMServoDriver.h"
#include <FastAccelStepper.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sloplog/sloplog.h"

// FastAccelStepperEngine — background pulse-generation engine on ESP32. One
// static instance shared across the whole driver. Named _fas_engine to avoid
// shadowing MotorDriver::_engine. Runs on Core 1; generates step pulses in
// hardware timer ISRs.
static FastAccelStepperEngine _fas_engine;

// Endstop is active LOW — the optocoupler pulls the pin LOW when the carriage
// reaches home. HIGH = clear, LOW = triggered. Only relevant on the LEGACY
// endstop path (HOMING_USE_ENDSTOP) — the current board has no switch and
// homes on motor current instead.
#define ENDSTOP_ACTIVE_STATE LOW


AIMServoDriver::AIMServoDriver() {}

// ---- Lifecycle --------------------------------------------------------------

void AIMServoDriver::init() {
    // No SPI bus, no driver-chip registers, no chip select — just two GPIO
    // pins handed to FAS. The 57AIM30 is already energized and waiting.

#if defined(HOMING_USE_ENDSTOP)
    // LEGACY path only: endstop pin — INPUT_PULLUP keeps the line HIGH when the
    // switch is open. The current board has NO switch; this is compiled out by
    // default and only exists for a bench rig with an endstop wired in.
    pinMode(AIM_PIN_ENDSTOP, INPUT_PULLUP);
#endif

    // Bring up the INA228 current sensor — the ONLY way the driver senses a
    // hard stop now that there's no endstop switch. The Wire bus must already
    // be up (main setup calls Wire.begin(SDA,SCL)); this only probes the
    // device on it. If it's missing, homing refuses rather than blindly
    // ramming the frame.
    if (!_current.init()) {
        SLOGW("aim", "AIMServo: WARNING — INA228 not found, sensorless homing DISABLED. uhoh :3");
    }

    _engine = &_fas_engine;

    // Initialize FastAccelStepperEngine — creates the background timer task
    // on ESP32 that generates step pulses in hardware, off the CPU.
    _fas_engine.init();

    // Connect the stepper to the PUL pin. stepperConnectToPin() takes ONLY
    // the step pin — direction is set separately via setDirectionPin(). A
    // (STEP, DIR) two-arg call is NOT a valid overload; passing DIR there
    // silently leaves the DIR pin unset and the motor can only move one way.
    _stepper = _fas_engine.stepperConnectToPin(AIM_PIN_STEP);

    if (_stepper) {
        // Direction pin — true = invert. Flip here (not by rewiring) if a
        // motor swap reverses polarity again.
        _stepper->setDirectionPin(AIM_PIN_DIR, true);

        // No enable pin on the 57AIM30 — always energized when powered, so
        // no enable pin is registered with FAS at all.
        //
        // CRITICAL: setAutoEnable(false) means FAS won't auto-enable on move,
        // but it also means FAS's internal _outputEnabled flag starts FALSE.
        // With no enable pin registered, every moveTo() call is silently
        // blocked until _outputEnabled is true. enableOutputs() is called
        // ONCE here to permanently open that gate — the 57AIM30 has no
        // hardware to toggle, so this is purely a flag flip inside FAS.
        // Without this call the motor ignores every command.
        _stepper->setAutoEnable(false);
        _stepper->enableOutputs();   // permanently open the FAS output-enable flag
        _enabled = true;
        _stepper->setCurrentPosition(0);

        SLOGI("aim", "AIMServo: FastAccelStepper initialized (PUL/DIR registered)");
        SLOGI("aim", "AIMServo: PUL=GPIO%d DIR=GPIO%d ENDSTOP=GPIO%d",
              AIM_PIN_STEP, AIM_PIN_DIR, AIM_PIN_ENDSTOP);
        SLOGI("aim", "AIMServo: %u steps/rev, %.1f mm/rev, %.1f steps/mm, %.1f mm max rail",
              (uint32_t)AIM_STEPS_PER_REV, AIM_MM_PER_REV,
              AIM_STEPS_PER_MM, _max_rail_mm);
    } else {
        SLOGE("aim", "AIMServo: ERROR — FastAccelStepper failed to connect to PUL pin!");
    }

    // Set initial speed/acceleration in mm units. These get converted to
    // steps/s and steps/s² inside setMaxSpeed/setAcceleration.
    setMaxSpeed(_max_speed_mm_s);
    setAcceleration(_accel_mm_s2);
}

void AIMServoDriver::update() {
    runMotorStep();

    // Refresh the INA228 telemetry cache. Two tiers (both skipped while the
    // homing task owns the I2C bus — it refreshes the cache itself):
    //  - FAST (40Hz): bus current + bus voltage only — 2 I2C transactions.
    //    25ms matches the chip's own conversion cadence (540us conversions x
    //    AVG16 = ~26ms per fresh result), the fastest rate yielding NEW data;
    //    the hardware 16-sample averaging is untouched, so the noise floor is
    //    unchanged from a slower poll.
    //  - FULL (1Hz): temp/shunt/power/energy — the slow health set, 6
    //    transactions, no reason to burn bus time on it 40x a second.
    if (!_homing && _current.isReady()) {
        uint32_t now = millis();
        if (now - _last_current_poll_ms >= 25) {    // ~40Hz fast refresh
            _last_current_poll_ms = now;
            _current.pollFast();   // current + busV only
        }
        if (now - _last_current_full_poll_ms >= 1000) {  // 1Hz full refresh
            _last_current_full_poll_ms = now;
            _current.poll();       // temp/shunt/power/energy too
        }
    }


    // Stream stall watchdog: DISABLED in D4 event-driven mode (MotionArbiter).
    // Intents there are event-driven — sporadic retargets at arbitrary
    // intervals, not a continuous stream, so a gate-blocked intent (pause,
    // override, not-homed) is a valid resting state, not a stall. A watchdog
    // sized for a continuous push-model stream would fire during any
    // pause/override lasting >80ms and permanently disable motion, requiring
    // a reboot to recover.
}

void AIMServoDriver::emergencyStop() {
    // Kill the homing task FIRST — mid-sweep it would just see its wait loop
    // end, roll into the next sweep, and later re-assert _homed = true,
    // resuming motion right after the E-stop (same kill stop() does). THEN
    // cut the pulse train; the 57AIM30 decelerates on its own internal ramp,
    // this just stops commanding it.
    if (_homingTaskHandle != nullptr) {
        vTaskDelete(_homingTaskHandle);
        _homingTaskHandle = nullptr;
        SLOGW("aim", "AIMServo E-stop: homing task killed mid-sweep.");
    }
    hardStop();
    _homed  = false;
    _homing = false;
}

// ---- Enable / Disable -------------------------------------------------------
// The 57AIM30 has no software enable pin — it's always energized when
// powered. These calls satisfy the MotorDriver interface but do nothing to
// hardware.

void AIMServoDriver::enable() {
    // No enable pin. The 57AIM30 is always on; the flag is tracked only for
    // interface compatibility, nothing to toggle.
    if (_stepper) _stepper->enableOutputs();
    _enabled = true;
}

void AIMServoDriver::disable() {
    // No hardware to disable. Stops the pulse train so the drive stops
    // receiving commands, but it stays energized and holding position.
    if (_stepper) {
        hardStop();
        _stepper->disableOutputs();
    }
    _enabled = false;
}

// ---- Stream-state reset -----------------------------------------------------

void AIMServoDriver::resetStreamState() {
    _have_last_sample  = false;
    _last_sample_ms    = 0;
    // Forget the in-flight target/direction so the next stream starts a fresh
    // blend instead of inheriting a stale reversal decision from before the
    // Halt/Home.
    _have_last_target  = false;
    _last_target_steps = 0;
    _last_dir          = 0;
}

// ---- Homing -----------------------------------------------------------------
// Architecture mirrors StrokeEngine's _homingProcedure.
//
// home() spawns a one-shot FreeRTOS task on Core 1 that owns the entire homing
// sequence — sweep toward the endstop, poll until it triggers, hard-stop,
// back off AIM_HOMING_BACKOFF_MM (10mm), re-zero. The task blocks internally
// with vTaskDelay(20ms) between endstop polls. When done (success or failure)
// the task sets _homed/_homing and deletes itself.

// Static trampoline — FreeRTOS needs a plain C function pointer, so we bounce
// through this into the member function. The `this` pointer rides in as param.
void AIMServoDriver::_homingTaskImpl(void* param) {
    static_cast<AIMServoDriver*>(param)->_homingTask();
}

// The actual homing procedure — runs entirely inside its own task on Core 1.
// Blocks with vTaskDelay() between polls so the scheduler stays happy.
//
// DIRECTION CONVENTION (critical for a 180W servo — get this wrong and it
// rams the frame at full speed):
//
//   POSITIVE steps = toward the endstop (motor end / rear of machine)
//   NEGATIVE steps = away from endstop  (front / extended position)
//
// The sweep uses move(+sweep_steps) to drive toward the endstop.
// The backoff uses move(-backoff_steps) to pull away from it.
// If the carriage moves the WRONG way on sweep, flip AIM_PIN_DIR in
// config_api.h or invert the setDirectionPin() bool in init().
// -----------------------------------------------------------------------------
// _sweepToStall() — drive in one direction until the carriage hits a hard
// stop, detected by an INA228 current spike.
// -----------------------------------------------------------------------------
// dir_sign: +1 = sweep toward the rear (positive steps), -1 = toward the front.
// Returns true if a stall was detected, false if the full sweep completed
// without one (mechanical/electrical fault — carriage never hit a wall).
bool AIMServoDriver::_sweepToStall(int8_t dir_sign) {
    // Crawl speed + high accel so it's effectively constant velocity from the
    // first step — no ramp to confuse the current baseline.
    _stepper->setSpeedInHz((uint32_t)_home_speed_steps_s);
    _stepper->setAcceleration(10000);

    // 1.2x the configured max rail length in the requested direction — bounds
    // how far the sweep hunts for a wall so homing can't run forever on an
    // infinitely long (or faulted) rail. The stall poll normally stops the
    // sweep long before it runs out of steps; if it DOES run out, no wall was
    // felt within the configured rail and homing fails.
    int32_t sweep = (int32_t)(_max_rail_mm * AIM_STEPS_PER_MM * 1.2f);
    // Starting position for THIS sweep, so a debounced stall can be judged by
    // WHERE it happened relative to the sweep planned — not just THAT it
    // happened. See AIM_HOME_STALL_PLAUSIBLE_FRAC.
    int32_t start_steps = _stepper->getCurrentPosition();
    _stepper->move(dir_sign >= 0 ? sweep : -sweep);

    // Let the pulse train spin up and let any residual stall current from a
    // previous sweep decay out of the INA228's 16-sample averaging window
    // BEFORE collecting the free-run baseline. Sampling too early poisons the
    // baseline with leftover strain current and the next wall never reads as
    // a spike.
    vTaskDelay(pdMS_TO_TICKS(150));
    if (!_stepper->isRunning()) {
        SLOGW("aim", "AIMServo Homing: sweep move refused by FAS — stepper never started. uhoh :C");
        return false;
    }

    const uint32_t poll_ms   = 1000u / AIM_HOME_POLL_HZ;   // ~6-7ms @150Hz
    float    baseline_a      = 0.0f;
    uint32_t baseline_taken  = 0;
    float    baseline_sum    = 0.0f;
    uint16_t over_count      = 0;

    while (_stepper->isRunning()) {
        float amps = fabsf(_current.readCurrentA());

        // Build the free-run baseline from the FIRST N samples — while the
        // carriage is still gliding freely and drawing its light idle current.
        // Everything after is measured against this.
        if (baseline_taken < AIM_HOME_BASELINE_SAMPLES) {
            baseline_sum += amps;
            if (++baseline_taken == AIM_HOME_BASELINE_SAMPLES) {
                baseline_a = baseline_sum / (float)AIM_HOME_BASELINE_SAMPLES;
                SLOGI("aim", "AIMServo Homing: free-run baseline = %.2f A", baseline_a);
            }
        } else {
            // Stall = current sitting above baseline+margin for N consecutive
            // polls. One spike could be noise; N in a row means the carriage
            // is genuinely against the wall and straining, unable to go
            // deeper.
            if (amps > baseline_a + AIM_HOME_STALL_MARGIN_A) {
                if (++over_count >= AIM_HOME_STALL_CONSEC) {
                    // STOP NOW — no coasting on a 180W servo.
                    _stepper->forceStop();
                    int32_t stall_pos = _stepper->getCurrentPosition();
                    SLOGI("aim", "AIMServo Homing: *** STALL *** %.2f A (base %.2f + margin %.1f) "
                          "for %u polls, pos=%d",
                          amps, baseline_a, AIM_HOME_STALL_MARGIN_A, over_count, stall_pos);
                    // Wait for the pulse train to fully drain before returning.
                    uint32_t to = millis() + 500;
                    while (_stepper->isRunning() && millis() < to) {
                        vTaskDelay(pdMS_TO_TICKS(2));
                    }

                    // PLAUSIBILITY CHECK (safety): a stall debounced this deep
                    // into the planned search sweep is far more likely a
                    // sustained current-reading glitch (motor unplugged; the
                    // INA228 is a separate I2C device and a servo drive
                    // alarming on an open phase can read erratic current) than
                    // a real wall — a real wall is always found well inside
                    // the configured rail length. Reject it and fail exactly
                    // like "no stall found" rather than let a bogus
                    // ~1.2x-rail position become ground-truth geometry.
                    int32_t traveled = (stall_pos >= start_steps) ? (stall_pos - start_steps)
                                                                   : (start_steps - stall_pos);
                    float   frac     = (sweep > 0) ? (float)traveled / (float)sweep : 1.0f;
                    if (frac >= AIM_HOME_STALL_PLAUSIBLE_FRAC) {
                        SLOGW("aim", "AIMServo Homing: stall REJECTED as implausible — occurred at "
                              "%d/%d steps (%.0f%% of the search sweep, bound %.0f%%). Reads like "
                              "'ran out of search distance' (current glitch / unplugged motor / "
                              "open-phase alarm), not a real wall. Treating as a FAILED sweep. uhoh :C",
                              traveled, sweep, frac * 100.0f, AIM_HOME_STALL_PLAUSIBLE_FRAC * 100.0f);
                        return false;
                    }
                    return true;
                }
            } else {
                over_count = 0;  // dropped back under threshold — reset the run
            }
        }

        // Log on CHANGE: the current has to move by 50 mA, or the consecutive
        // over-threshold run has to change, before the sweep logs anything —
        // keeps a clean sweep to a few lines instead of a running transcript
        // of a number that barely moves.
        {
            static int16_t last_ca = INT16_MIN;   // centi-amps, quantized
            static uint8_t last_over = 0xFF;
            const int16_t ca = int16_t(amps * 100.0f);
            if (abs(int(ca) - int(last_ca)) >= 5 || over_count != last_over) {
                last_ca = ca;
                last_over = uint8_t(over_count);
                SLOGD("aim", "AIMServo Homing: sweeping dir=%d I=%.2fA base=%.2f over=%u pos=%d",
                      dir_sign, amps, baseline_a, over_count, _stepper->getCurrentPosition());
            }
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
    return false;  // ran the whole sweep without a stall — no wall found.
}

void AIMServoDriver::_homingTask() {
    SLOGI("aim", "AIMServo Homing: START (SENSORLESS via INA228) speed=%u steps/s (%.1f mm/s)",
          (uint32_t)_home_speed_steps_s,
          (float)_home_speed_steps_s / AIM_STEPS_PER_MM);

    // No current sensor = no way to feel the wall. Refuse rather than blindly
    // ram the frame at speed. The servo's own foldback is the last-ditch
    // backstop, but a normal home does not rely on it.
    if (!_current.isReady()) {
        SLOGW("aim", "AIMServo Homing: ABORT — INA228 not ready, cannot sense stalls. uhoh :C");
        _homing = false;
        _homed  = false;
        _homingTaskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // --- Stall #1: find the FRONT hard stop first — sweeps toward the out end
    SLOGI("aim", "AIMServo Homing: sweeping toward FRONT hard stop...");
    if (!_sweepToStall(-1)) {
        SLOGW("aim", "AIMServo Homing: FAILED — no stall on front sweep. Check current");
        SLOGW("aim", "  threshold (AIM_HOME_STALL_MARGIN_A), wiring, and travel distance.");
        _homing = false;
        _homed  = false;
        _homingTaskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Record the front stall position before zeroing at the rear.
    int32_t front_steps = _stepper->getCurrentPosition();
    SLOGI("aim", "AIMServo Homing: front wall touched at %d steps", front_steps);

    // CRITICAL: forceStopAndNewPosition re-syncs the FAS position counter and
    // clears the internal stopped/paused state. Without this, the `move()`
    // call inside the NEXT _sweepToStall() is silently ignored — FAS is still
    // stuck in its post-forceStop limbo and refuses to plan a new trajectory.
    _stepper->forceStopAndNewPosition(front_steps);
    vTaskDelay(pdMS_TO_TICKS(50));   // let the pulse train fully settle

    // Pull off the front wall a few mm before starting the rear sweep. If the
    // rear sweep begins while still jammed against the front stop, the servo
    // is straining to break free and those elevated readings become the
    // "free-run" baseline, making the rear wall undetectable. Back out and
    // let the current fall back to idle before sweeping.
    _stepper->setSpeedInHz((uint32_t)_home_speed_steps_s);
    _stepper->setAcceleration(10000);
    _stepper->move((int32_t)mmToNative(AIM_HOMING_BACKOFF_MM));  // + = toward rear
    {
        uint32_t free_to = millis() + 5000;
        while (_stepper->isRunning() && millis() < free_to) vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(250));  // drain the stall spike out of the INA228 average

    // --- Stall #2: sweep back to the REAR hard stop (becomes home / 0mm) ---
    SLOGI("aim", "AIMServo Homing: sweeping toward REAR hard stop to establish home...");
    if (!_sweepToStall(+1)) {
        SLOGW("aim", "AIMServo Homing: FAILED — no stall on rear sweep. Check current");
        SLOGW("aim", "  threshold (AIM_HOME_STALL_MARGIN_A), wiring, and travel distance.");
        _homing = false;
        _homed  = false;
        _homingTaskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Capture the rear stall position BEFORE zeroing — the true rail span is
    // rear-minus-front. In the front-then-rear flow, front_steps is relative
    // to wherever the carriage happened to sit at boot, so |front_steps|
    // alone is NOT the span.
    int32_t rear_steps = _stepper->getCurrentPosition();

    // Zero at the rear stop, then back off AIM_HOMING_BACKOFF_MM so the
    // carriage isn't resting against the wall. This backed-off spot = home.
    _stepper->forceStopAndNewPosition(0);
    vTaskDelay(pdMS_TO_TICKS(100));
    _stepper->setSpeedInHz((uint32_t)_home_speed_steps_s);
    _stepper->setAcceleration(10000);
    int32_t backoff_steps = (int32_t)mmToNative(AIM_HOMING_BACKOFF_MM);
    _stepper->move(-backoff_steps);   // negative = away from rear, toward front
    uint32_t to = millis() + 30000;
    while (_stepper->isRunning() && millis() < to) vTaskDelay(pdMS_TO_TICKS(20));
    _stepper->forceStopAndNewPosition(0);   // re-zero: THIS is home (0mm)
    _current_position_mm = 0.0f;
    SLOGI("aim", "AIMServo Homing: rear found, backed off %.1fmm — HOME set at 0mm :3",
          AIM_HOMING_BACKOFF_MM);

    // --- Measure usable stroke from front stall to rear ---
    // Span = distance between the two stall positions (both measured in the
    // same pre-zero counter frame). Subtract the backoff margin.
    int32_t span_steps = rear_steps - front_steps;   // rear is +dir, front is -dir
    if (span_steps > 0) {
        float   raw_span_mm = fabsf(nativeToMm(span_steps));
        // Subtract the rear backoff (home sits that far off the rear wall) AND
        // a front margin — the recorded stall positions overrun both physical
        // walls by the detection latency, so without a front margin the max
        // command lands INSIDE the front hard stop and the servo strains.
        float   usable_mm   = raw_span_mm - AIM_HOMING_BACKOFF_MM
                                          - AIM_HOMING_FRONT_MARGIN_MM;
        if (usable_mm < 0.0f) usable_mm = 0.0f;
        // MEASUREMENT WINS: the span between the two physically-detected hard
        // stops is ground truth for the usable stroke. This is NOT clamped to
        // the configured max rail length — that setting only bounds the
        // search sweep, not the result. A wall felt slightly past the
        // expected rail length is a real wall and is trusted — UP TO A SANITY
        // BOUND, since a bad number that slipped past the plausibility guard
        // above would otherwise become new ground truth, persist to NVS, and
        // haunt every boot after. Same 0..2000mm bound as the NVS-restore path
        // (MotorDriver::setMeasuredStrokeMm()).
        if (usable_mm > 0.0f && usable_mm < 2000.0f) {
            setMeasuredStrokeMm(usable_mm);
            SLOGI("aim", "AIMServo Homing: front-to-rear span %.1fmm -> usable stroke %.1fmm "
                  "(rail-length bound %.1fmm) :3", raw_span_mm, _measured_stroke_mm, _max_rail_mm);
        } else if (usable_mm >= 2000.0f) {
            SLOGE("aim", "AIMServo Homing: measured stroke %.1fmm is IMPLAUSIBLE (sanity bound "
                  "0..2000mm, same as the NVS-restore path) — REJECTED, NOT stored as ground "
                  "truth. Machine keeps its prior stroke (%.1fmm) / configured rail (%.1fmm). "
                  "Check wiring, AIM_HOME_STALL_MARGIN_A, and the plausibility guard above. "
                  "uhoh :C", usable_mm, _measured_stroke_mm, _max_rail_mm);
        }
        // usable_mm == 0.0f falls through with no log: a degenerate zero span
        // is not a NEW implausible measurement, it's the same "not measured"
        // sentinel _measured_stroke_mm already defaults to.
    } else {
        // Non-positive span — something's off, fall back to the configured
        // rail length. The home from the rear sweep is still valid.
        _measured_stroke_mm = 0.0f;
        SLOGW("aim", "AIMServo Homing: unexpected front position — using configured rail length.");
    }

    _current_position_mm = 0.0f;
    _homed  = true;
    _homing = false;
    SLOGI("aim", "AIMServo Homing: COMPLETE — homed at 0mm, usable stroke %.1fmm. yippie! :3",
          _measured_stroke_mm > 0.0f ? _measured_stroke_mm : _max_rail_mm);
    _homingTaskHandle = nullptr;
    vTaskDelete(nullptr);
}


bool AIMServoDriver::home(int32_t home_speed_steps_s) {
    if (_homing) return false;

    SLOGI("aim", "AIMServo Homing: Starting...");
    _homing = true;
    _homed  = false;

    // Fresh peak-current/peak-power tracking (and the INA228's own hardware
    // energy accumulator) for this homing cycle, so the reported peak reflects
    // THIS home, not a stale number from the last one. Only meaningful if the
    // sensor is actually ready.
    if (_current.isReady()) {
        _current.resetPeaks();
    }
    // Default to AIM_HOMING_SPEED_STEPS_S (500 steps/s = 25 mm/s). At the
    // current 20 steps/mm scale, a speed intended as ~50 mm/s under an old
    // steps/mm figure would instead be ~200 mm/s — fast enough to slam the
    // carriage into the endstop. Any caller-supplied home_speed_steps_s must
    // be sane for the CURRENT AIM_STEPS_PER_MM, not inherited from elsewhere.
    _home_speed_steps_s = (home_speed_steps_s > 0) ? home_speed_steps_s : AIM_HOMING_SPEED_STEPS_S;

    // Drop any leftover stream/target state — a stale in-flight target fights
    // the homing sweep and can bang the endstop without ever finishing.
    resetStreamState();

    // Enable outputs via FAS before spawning the task — FAS needs
    // _outputEnabled = true before move() will execute.
    if (_stepper) {
        _stepper->enableOutputs();
        _enabled = true;
    }

    // Spawn the self-contained homing task on Core 1 (same core as motorTask
    // and the FAS engine). Priority 20 matches StrokeEngine — high enough to
    // preempt normal motion but below the FAS ISR. The task deletes itself
    // when homing completes or fails.
    BaseType_t created = xTaskCreatePinnedToCore(
        _homingTaskImpl,        // static trampoline
        "AIMHoming",            // task name
        4096,                   // stack (bytes)
        this,                   // param = this pointer
        20,                     // priority — same as StrokeEngine
        &_homingTaskHandle,     // handle so we can kill it on E-stop
        1                       // Core 1 — same core as FAS engine
    );
    if (created != pdPASS) {
        // Task never spawned (heap pressure). Without this rollback _homing
        // stays true forever and home() silently refuses until reboot.
        _homing = false;
        _homingTaskHandle = nullptr;
        SLOGE("aim", "AIMServo Homing: FAILED to create homing task (out of memory?) — homing aborted. uhoh :C");
        return false;
    }

    return false;  // homing is async — watch isHoming()/isHomed() for completion
}

// runHomingStep() is a no-op — homing runs entirely inside its own task.
// The function is kept to satisfy the MotorDriver interface.
void AIMServoDriver::runHomingStep() {
    // Nothing to do here — the homing task owns the loop.
    // motorTask watches _homing go false and syncs g_state.
}

// ---- Bench/test fake-home ---------------------------------------------------
// Flip the driver's OWN _homed flag WITHOUT a real homing cycle so that
// moveTo()/streamTo()/streamToSteps() actually emit step/dir pulses. Setting
// _state.homed alone only opens the MotionArbiter gate — every motion entry
// point in THIS driver bails on `if (!_homed) return;`, so nothing reaches FAS
// until the flag is flipped here. Also energizes the FAS outputs (mirrors
// enable()) and zeros the position counter, exactly like a real home does at
// its final step, so the first move has a valid 0mm reference.
// Do NOT call on real hardware that must not move without a genuine home.
void AIMServoDriver::forceHomeState(bool homed) {
    if (homed) {
        if (_stepper) {
            _stepper->enableOutputs();               // pulse train can now go out
            _stepper->forceStopAndNewPosition(0);    // establish 0mm reference
            _enabled = true;
        }
        _current_position_mm = 0.0f;
        _homing = false;
        _homed  = true;
        SLOGI("aim", "AIMServo: forceHomeState(true) — bench fake-home, outputs live at 0mm :3");
    } else {
        _homed = false;
        SLOGI("aim", "AIMServo: forceHomeState(false) — cleared, real homing required.");
    }
}

// Push-to-home: user manually pushes the carriage into the endstop; the
// driver zeros there and backs off 10mm.
bool AIMServoDriver::checkPushToHome() {
#if !defined(HOMING_USE_ENDSTOP)
    // The current board has NO endstop switch — push-to-home relied on
    // reading it, so it's a no-op here. Homing is done via the sensorless
    // current-stall sweep (home()). This whole body only compiles on a
    // legacy endstop rig.
    return false;
#else
    if (_homed || _homing || !_stepper) return false;

    static uint32_t active_since = 0;
    uint32_t now = millis();

    if (digitalRead(AIM_PIN_ENDSTOP) == ENDSTOP_ACTIVE_STATE) {

        // Endstop pressed — start/continue debounce window.
        if (active_since == 0) active_since = now;

        // Require it held ~50ms to avoid noise/bounce false-homing.
        if (now - active_since >= 50) {
            active_since = 0;

            // Enable outputs so FAS can drive the backoff move.
            enable();

            // Zero at the pressed (endstop) position first so the backoff
            // move is measured from here.
            _stepper->forceStopAndNewPosition(0);

            // Back off AIM_HOMING_BACKOFF_MM (10mm) away from the endstop
            // so the carriage doesn't sit on the switch. Negative steps =
            // away from endstop toward the front.
            _stepper->setSpeedInHz(2000);
            _stepper->setAcceleration(50000);
            int32_t backoff_target = -mmToNative(AIM_HOMING_BACKOFF_MM);
            _stepper->moveTo(backoff_target);

            // Wait for the backoff move to finish.
            uint32_t timeout = millis() + 5000;
            while (_stepper->isRunning() && millis() < timeout) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }

            // Re-zero at this backed-off position = home (0mm).
            _stepper->forceStopAndNewPosition(0);
            _current_position_mm = 0.0f;
            _homed = true;

            SLOGI("aim", "AIMServo Push-to-home: endstop pressed — backed off 10mm, HOMED at 0");
            return true;
        }
    } else {
        // Released before debounce elapsed — reset.
        active_since = 0;
    }
    return false;
#endif // HOMING_USE_ENDSTOP
}

// ---- Motion -----------------------------------------------------------------

bool AIMServoDriver::moveTo(float pos_mm) {
    if (!_homed) {
        SLOGW("aim", "AIMServo: Cannot move — not homed!");
        return false;
    }

    enable();

    // Clamp to the effective physical ceiling — the measured stroke once
    // homing has felt out the real wall, else the configured max rail length.
    // The carriage never goes past the end of the rail.
    pos_mm = constrain(pos_mm, 0.0f, effectiveCeilingMm());

    // Coordinate system: home (endstop) = 0mm = step 0
    // "Out" (extended/front) = positive mm = NEGATIVE steps
    int32_t target_steps = -mmToNative(pos_mm);

    if (!_stepper) return false;

    uint32_t speed_hz = (uint32_t)(_max_speed_mm_s * AIM_STEPS_PER_MM);
    if (speed_hz < 1) speed_hz = 1;
    uint32_t accel_hz = (uint32_t)(_accel_mm_s2 * AIM_STEPS_PER_MM);
    if (accel_hz < 100) accel_hz = 100;

    _stepper->setSpeedInHz(speed_hz);
    _stepper->setAcceleration(accel_hz);

    int32_t pos_before = _stepper->getCurrentPosition();

    if (target_steps == pos_before) {
        SLOGD("aim", "AIMServo moveTo: %.1fmm already at target step %d",
              pos_mm, target_steps);
        return true;   // already there — a satisfied move, not a refusal
    }

    // If a previous move is still draining the queue, force-stop and re-sync
    // position first — otherwise FastAccelStepper ignores the new moveTo().
    if (_stepper->isRunning()) {
        _stepper->forceStopAndNewPosition(pos_before);
        uint32_t to = millis() + 300;
        while (_stepper->isRunning() && millis() < to) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    // FAS 0.34.0+ returns MoveResultCode (an enum) instead of int8_t.
    // Cast to int for the log — the numeric value is identical. 0 = OK.
    int mret = (int)_stepper->moveTo(target_steps);

    SLOGD("aim", "AIMServo moveTo: %.1fmm -> step %d (from %d) at %u Hz ret=%d",
          pos_mm, target_steps, pos_before, speed_hz, mret);
    // Propagate FAS's verdict — a silently-refused move must be visible to the
    // caller (MotionArbiter), not just this log line.
    return mret == 0;
}

// ---- streamTo() — continuous position streaming -----------------------------
// Smooth streaming move for Intiface/TCode. Unlike moveTo(), this NEVER
// force-stops between commands — FastAccelStepper accepts a fresh moveTo()
// target while already running and re-plans on the fly. Matches the pattern
// position-streaming engines (OSSM-Sauce / OSSM-stream / StrokeEngine) use:
// EVERY incoming sample is the latest truth about where the shaft should be,
// so this ALWAYS retargets — a waypoint is never dropped.
//
// This path always commands the full configured accel (no raise-only clamp
// here) — see streamToSteps()/setAcceleration() for the raise-only guard that
// DOES apply on the MotionArbiter dispatch path.
void AIMServoDriver::streamTo(float pos_mm, float speed_mm_s) {
    if (!_homed || !_stepper) return;
    enable();

    pos_mm = constrain(pos_mm, 0.0f, effectiveCeilingMm());
    int32_t target_steps = -mmToNative(pos_mm);  // front = negative steps

    // Arm the stall watchdog with this REAL commanded sample. Speed 0 means
    // "settle" and is re-issued BY the watchdog itself — this must not re-arm
    // the timer or it would never time out.
    if (speed_mm_s > 0.0f) {
        _last_sample_mm   = pos_mm;
        _last_sample_ms   = millis();
        _have_last_sample = true;
    }

    // Track direction for telemetry / future tuning. Reversals are never
    // dropped — every sample retargets; FAS handles mid-flight reversals
    // cleanly.
    int32_t cur_steps = _stepper->getCurrentPosition();
    int32_t delta     = target_steps - cur_steps;
    int8_t  new_dir   = (delta > 0) ? 1 : (delta < 0) ? -1 : 0;

    // Speed / acceleration — always command the full configured accel; FAS
    // re-plans from current velocity and handles mid-flight retargets
    // cleanly at any accel.
    float spd = (speed_mm_s > 0.0f) ? speed_mm_s : _max_speed_mm_s;
    spd = constrain(spd, 1.0f, _max_speed_mm_s);

    uint32_t speed_hz = (uint32_t)(spd * AIM_STEPS_PER_MM);
    if (speed_hz < 1) speed_hz = 1;
    uint32_t accel_hz = (uint32_t)(_accel_mm_s2 * AIM_STEPS_PER_MM);
    if (accel_hz < 100) accel_hz = 100;

    _stepper->setSpeedInHz(speed_hz);
    _stepper->setAcceleration(accel_hz);

    // No force-stop, no busy-wait: just retarget. If the new target equals
    // the current commanded target, FastAccelStepper ignores it cheaply.
    _stepper->moveTo(target_steps);

    _last_target_steps = target_steps;
    if (new_dir != 0) _last_dir = new_dir;
    _have_last_target = true;
}

// ---- streamToSteps() — pre-planned native-step dispatch ---------------------
// Called exclusively from Core 1, via MotionArbiter::submit() (motorTask) or
// ::submitStreamSample() (streamSamplerTask). Speed and accel arrive already
// converted to steps/s and steps/s² by the arbiter, including the arbiter's
// own raise-only acceleration clamp — no unit math or accel policy here, just
// arm the watchdog and fire straight to FAS.
void AIMServoDriver::streamToSteps(int32_t target_steps,
                                        uint32_t speed_steps_s,
                                        uint32_t accel_steps_s2) {
    if (!_homed || !_stepper) return;
    enable();

    // Arm the stall watchdog — convert target_steps back to mm for the
    // existing watchdog logic (which works in mm). If the host goes quiet,
    // update() will settle here.
    float pos_mm = nativeToMm(-target_steps);  // negative because front=negative steps
    _last_sample_mm   = pos_mm;
    _last_sample_ms   = millis();
    _have_last_sample = true;

    // Only call setAcceleration()/setSpeedInHz() when the value actually
    // changes. Calling them on every waypoint (30-100x/sec) forces FAS to
    // recalculate its ramp on every call, even mid-flight, which is felt as
    // motor grit/stutter. Cache starts at 0 so the first call always goes
    // through.
    if (accel_steps_s2 != _last_accel_steps_s2) {
        _stepper->setAcceleration(accel_steps_s2);
        _last_accel_steps_s2 = accel_steps_s2;
    }
    if (speed_steps_s != _last_speed_steps_s) {
        _stepper->setSpeedInHz(speed_steps_s);
        _last_speed_steps_s = speed_steps_s;
    }

    // moveTo() is non-blocking: FAS re-plans from current velocity to the new
    // target. No force-stop, no busy-wait.
    _stepper->moveTo(target_steps);
}

void AIMServoDriver::runMotorStep() {
    // NOP — moveTo()/streamTo()/streamToSteps() retarget through FAS directly.
    // Stream stall watchdog (streamTo() stashing) lives in update().
    // This stub exists to satisfy the MotorDriver interface.
}

// ---- Speed & Acceleration ---------------------------------------------------

void AIMServoDriver::setMaxSpeed(float speed_mm_s) {
    _max_speed_mm_s = constrain(speed_mm_s, 0.0f, MAX_SPEED_MM_S);
}

void AIMServoDriver::setAcceleration(float accel_mm_s2) {
    // Ceiling at 20000 mm/s² — well within what the 57AIM30 can handle for
    // short bursts. MotionArbiter uses this as the cruise accel; its
    // raise-only guard (reads getLiveAcceleration() before each
    // streamToSteps() dispatch) is what keeps it from softening mid-flight.
    _accel_mm_s2 = constrain(accel_mm_s2, 10.0f, 20000.0f);

    if (_stepper) {
        _stepper->setAcceleration((int32_t)(_accel_mm_s2 * AIM_STEPS_PER_MM));
    }
}

uint32_t AIMServoDriver::getLiveAcceleration() const {
    // Return the acceleration currently active inside the FAS ramp engine —
    // NOT the configured ceiling. MotionArbiter reads this via
    // stepper->getAcceleration() for its raise-only guard.
    return _stepper ? (uint32_t)_stepper->getAcceleration() : 0u;
}

// ---- Status -----------------------------------------------------------------

bool AIMServoDriver::isMoving() {
    return _stepper ? _stepper->isRunning() : false;
}

float AIMServoDriver::getPosition() const {
    if (!_stepper) return 0.0f;
    // Steps are negative for positive mm positions (endstop=0, front=negative steps)
    return nativeToMm(-_stepper->getCurrentPosition());
}

float AIMServoDriver::getTargetPosition() const {
    return getPosition();
}

// ---- Stop / HardStop --------------------------------------------------------

void AIMServoDriver::stop() {
    // E-stop: halt the pulse train, kill the homing task if it's running,
    // disable outputs via FAS, and clear all state. No ambiguity, no stale
    // flags left behind.

    // Kill the homing task first if it's mid-sweep.
    if (_homingTaskHandle != nullptr) {
        vTaskDelete(_homingTaskHandle);
        _homingTaskHandle = nullptr;
    }

    if (_stepper) {
        _stepper->forceStop();
        _stepper->disableOutputs();
    }
    _enabled = false;
    _homing  = false;
    // Full E-stop means the driver is no longer at a known position — the
    // carriage may have moved while disabled, or been moved manually. Clear
    // _homed so the driver's internal state matches g_state.homed.
    _homed = false;
    // Drop stale stream/target state so the next Home/move starts clean.
    resetStreamState();
}

void AIMServoDriver::hardStop() {
    // Immediate stop without deceleration — the pulse train cuts off NOW.
    // The 57AIM30 decelerates on its own internal ramp, but this stops
    // commanding it immediately. Also clears the blend/stream state, otherwise
    // the stall watchdog keeps re-targeting the last commanded position after
    // the stop.
    if (_stepper) {
        _stepper->forceStop();
    }
    resetStreamState();
}

// ---- Driver config ----------------------------------------------------------

void AIMServoDriver::applyDriverConfig(const DriverConfig& cfg) {
    // The 57AIM30 is configured via its own front-panel DIP switches and
    // parameter software (RS485 Modbus, future feature). There are no SPI
    // registers to write from here; the struct is accepted so the rest of
    // the system (ConfigStore, WebUI) doesn't need to know this is a dumb
    // drive.
    //
    // Speed and acceleration from the config ARE applied — those go to FAS,
    // not to the drive itself. The drive just follows the pulse rate.
    (void)cfg;  // suppress unused-parameter warning
    SLOGI("aim", "AIMServo: applyDriverConfig() — dumb drive, no registers to write");
}

// ---- Unit conversion --------------------------------------------------------

int32_t AIMServoDriver::mmToNative(float mm) const {
    // Convert mm to steps using the capstan-drum geometry.
    // AIM_STEPS_PER_MM ~= 20.372 (1600 steps/drum-rev / pi*25mm circumference).
    // The result is a step count — positive = toward the rear hard stop.
    return (int32_t)(mm * AIM_STEPS_PER_MM);
}

float AIMServoDriver::nativeToMm(int32_t native) const {
    // Convert steps back to mm. Inverse of mmToNative().
    return (float)native / AIM_STEPS_PER_MM;
}

#endif // defined(DRIVER_AIM_SERVO)
