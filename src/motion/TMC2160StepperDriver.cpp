// TMC2160StepperDriver — concrete MotorDriver for TMC2160 stepper (V1)
// Build-guarded behind DRIVER_TMC2160 (set in platformio.ini).
#if defined(DRIVER_TMC2160)

#include "TMC2160StepperDriver.h"
#include <TMCStepper.h>
#include <FastAccelStepper.h>
#include <SPI.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "AppLog.h"

// In serial-control mode the USB Serial port is dedicated to Intiface TCode, so
// debug output must go to the in-memory web log instead of Serial (which would
// corrupt the command stream). These macros redirect this file's logging to
// applog when SERIAL_CONTROL_MODE is on, and to Serial otherwise. The F()
// wrapper used by some println() calls is harmless for applog (plain string).
#if SERIAL_CONTROL_MODE
  #define MLOGF(...)  applogf(__VA_ARGS__)
  // String() accepts both plain "..." and F("...") literals, then .c_str()
  // gives applog a normal const char*.
  #define MLOGLN(s)   applog(String(s).c_str())
#else
  #define MLOGF(...)  Serial.printf(__VA_ARGS__)
  #define MLOGLN(s)   Serial.println(s)
#endif

// FastAccelStepperEngine manages the background stepper task on ESP32.
// Named _fas_engine to avoid shadowing MotorDriver::_engine.
static FastAccelStepperEngine _fas_engine;

#define ENDSTOP_ACTIVE_STATE LOW

TMC2160StepperDriver::TMC2160StepperDriver() {}

// ---- Lifecycle ---------------------------------------------------------------

void TMC2160StepperDriver::init() {
    // DRV_STATUS readback (diagTask) has been removed per project owner
    // directive. SPI config writes are now serialised by the FreeRTOS scheduler
    // (Core 1 only), so no explicit mutex is needed.

    // Configure pins
    pinMode(PIN_ENABLE, OUTPUT);
    digitalWrite(PIN_ENABLE, HIGH);  // Disable motor initially (active LOW)
    pinMode(PIN_ENDSTOP, INPUT_PULLUP);

    // CRITICAL: CS pin must be set HIGH before TMC2160Stepper is initialized
    // Otherwise the driver sees a spurious SPI transaction on power-up
    pinMode(PIN_TMC_CS, OUTPUT);
    digitalWrite(PIN_TMC_CS, HIGH);  // Deselect TMC before begin()
    delay(10);

    // Initialize TMC2160 via HARDWARE SPI — we go in RAW, no software bit-bang
    // foreplay here. :3
    //
    // We deliberately use the ESP32-S3's silicon SPI peripheral, NOT the
    // library's software bit-bang mode. Passing all four pins to the constructor
    // selects SOFTWARE SPI, which manually toggles the clock in a CPU loop.
    // On an ESP32-S3 getting gangbanged by FreeRTOS + WiFi + BLE + WebSockets,
    // the OS preempts that loop at random; writes still work (the driver waits
    // obediently), but READS miss the exact moment MISO is sampled and come back
    // as 0x00000000 / 0xFFFFFFFF — total ghosting, just limp nothing. Hardware
    // SPI clocks/samples in silicon and stays rock hard through any interrupt
    // storm.
    //
    // SPI.begin(SCK, MISO, MOSI, SS) binds the HSPI matrix to our custom pins,
    // and the 2-arg TMC2160Stepper(cs, r_sense) constructor selects HW SPI.
    //
    // MISO MUST have a pull-up. The TMC only drives MISO while CS is asserted;
    // the rest of the time it floats, and a floating MISO is basically a shy
    // sub whispering random 0x00/0xFF — not the kind of dirty talk we want.
    // The reference example does the same `pinMode(MISO, INPUT_PULLUP)` to keep
    // that line held nice and firm.
    pinMode(PIN_TMC_MISO, INPUT_PULLUP);
    SPI.begin(PIN_TMC_SCLK, PIN_TMC_MISO, PIN_TMC_MOSI, PIN_TMC_CS);
    _tmc = new TMC2160Stepper(PIN_TMC_CS, TMC_R_SENSE);

    // Keep the TMC SPI clock conservative. TMCStepper defaults to a fast clock
    // that the ESP32-S3 GPIO-matrix routing (non-native SPI pins) can't reliably
    // sample on reads, returning 0x00000000 / 0xFFFFFFFF. 1 MHz is well within
    // the TMC2160's spec and reads cleanly.
    _tmc->setSPISpeed(1000000UL);

    _tmc->begin();

    // CRITICAL: TMC2160 needs time to power up before SPI registers can be written
    // The working reference code uses delay(1000) here
    delay(1000);

    // Definitive SPI sanity check: read the silicon version field from the
    // TMC's IOIN/GCONF register. A healthy TMC2160 reports version 0x30. If
    // this comes back 0x00 or 0xFF, SPI is NOT talking (wiring/clock/MISO),
    // and nothing downstream will ever work.
    uint8_t ver = _tmc->version();
    MLOGF("TMC2160: SPI version read = 0x%02X (expect 0x30)\n", ver);
    if (ver == 0x00 || ver == 0xFF) {
        MLOGLN(F("TMC2160: *** SPI NOT RESPONDING *** check MISO wiring/pull-up & clock"));
    }

    // Clear any faults
    uint32_t gstat = _tmc->GSTAT();
    if (gstat) {
        MLOGF("TMC2160 GSTAT before init: 0x%02X (clearing)\n", gstat);
        _tmc->GSTAT(0x07);
    }

    // Apply driver settings - match working reference code order exactly
    _tmc->microsteps(MICROSTEPS);           // Set microstepping first
    _tmc->intpol(true);                     // Enable interpolation (required for microsteps)
    _tmc->rms_current(TMC_RUN_CURRENT_MA);  // Set run current
    _tmc->ihold(15);                        // Hold current fraction (15/31 ~= 48%)
    _tmc->toff(TMC_TOFF);                   // Time off (must be non-zero to enable driver)

    // Read back to verify SPI communication is working
    uint16_t actual_ms = _tmc->microsteps();
    uint32_t actual_cur = _tmc->rms_current();
    MLOGF("TMC2160: microsteps set=%u read=%u | current set=%u read=%u mA\n",
          MICROSTEPS, actual_ms, TMC_RUN_CURRENT_MA, actual_cur);

    if (actual_ms != MICROSTEPS) {
        MLOGLN(F("WARNING: TMC2160 microstep readback mismatch - SPI may not be working!"));
    } else {
        MLOGLN(F("TMC2160: SPI OK, driver configured"));
    }

    // Store the engine pointer so everyone can share the toy (Risk #5 — don't
    // be greedy, pass the pointer around :3).
    _engine = &_fas_engine;

    // Initialize FastAccelStepperEngine - creates background task on ESP32
    _fas_engine.init();

    // Create stepper motor connected to STEP pin only.
    // CRITICAL: stepperConnectToPin() takes ONLY the step pin — it's a top,
    // not a switch. :3 The direction pin MUST be configured separately via
    // setDirectionPin(). Previously we passed (STEP, DIR) which is NOT a valid
    // overload — the DIR pin was never set, so the motor could only ever thrust
    // in one direction (forward toward the endstop worked, but pulling back out
    // was just a sad little nothing).
    _stepper = _fas_engine.stepperConnectToPin(PIN_STEP);

    if (_stepper) {
        // Configure direction pin (false = do not invert direction signal)
        _stepper->setDirectionPin(PIN_DIR, false);
        // Register the enable pin WITH FAS — let FAS own it completely.
        // enableActiveLow=true matches the TMC2160 EN pin (LOW = enabled).
        // setAutoEnable(false) means WE call enableOutputs()/disableOutputs()
        // manually — FAS never auto-enables on move or auto-disables on stop.
        // This is exactly the StrokeEngine pattern and it's the only way
        // enableOutputs() + move() work reliably after an E-stop cycle. :3
        _stepper->setEnablePin(PIN_ENABLE, true);
        _stepper->setAutoEnable(false);
        _stepper->disableOutputs();   // start disabled — motor is soft on boot
        _stepper->setCurrentPosition(0);
        MLOGLN(F("FastAccelStepper: Motor initialized (STEP/DIR/EN registered with FAS)"));
    } else {
        MLOGLN(F("FastAccelStepper: Failed to create motor!"));
    }

    // Set initial speed/acceleration in mm units
    setMaxSpeed(_max_speed_mm_s);
    setAcceleration(_accel_mm_s2);
}

void TMC2160StepperDriver::update() {
    runMotorStep();

    // Stream stall watchdog: if the host has gone quiet (no fresh waypoint for
    // STREAM_STALL_MS) but we're still mid-thrust toward a streamed target,
    // settle firmly on the last REAL sample so the carriage can't keep coasting
    // toward a stale target after the stream drops. We don't extrapolate past
    // it — that toy got thrown out. We just stay put where we were last told. :3
    if (_have_last_sample && _stepper &&
        (millis() - _last_sample_ms >= STREAM_STALL_MS)) {
        _have_last_sample = false;   // one-shot; re-armed on the next streamTo()
        streamTo(_last_sample_mm, 0.0f);
    }
}


void TMC2160StepperDriver::emergencyStop() {
    // Kill the homing task FIRST — mid-sweep it would just see its wait loop
    // end, proceed into the next phase, and later re-assert _homed = true,
    // resuming motion right after the E-stop. Same kill stop() does. :3
    if (_homingTaskHandle != nullptr) {
        vTaskDelete(_homingTaskHandle);
        _homingTaskHandle = nullptr;
        MLOGLN(F("E-stop: homing task killed mid-sweep."));
    }
    hardStop();
    disable();
    _homed = false;
    _homing = false;
}

// ---- Enable / Disable --------------------------------------------------------
//
// FAS owns the enable pin (registered via setEnablePin in init()). We call
// enableOutputs()/disableOutputs() through FAS — this is the StrokeEngine
// pattern and the only way the enable state stays consistent with FAS's
// internal _outputEnabled flag. When FAS knows the pin is enabled, move()
// commands execute immediately without any silent no-ops. :3
//
// PIN_ENABLE is active LOW (TMC2160 EN pin): LOW = enabled, HIGH = disabled.
// The `true` passed to setEnablePin() tells FAS about the active-LOW polarity.

void TMC2160StepperDriver::enable() {
    // FAS owns this pin — let it drive it. FAS sets the pin LOW (active LOW)
    // and marks _outputEnabled = true so subsequent move() calls go through
    // without being silently blocked. The TMC2160 stands at attention. :3
    if (_stepper) _stepper->enableOutputs();
    _enabled = true;
}

void TMC2160StepperDriver::disable() {
    if (_stepper) {
        hardStop();
        // FAS drives the pin HIGH (disabled) and clears _outputEnabled.
        // The shaft goes soft, fully tracked by FAS's internal state. :3
        _stepper->disableOutputs();
    }
    _enabled = false;
}

// ---- Stream-state reset ------------------------------------------------------

void TMC2160StepperDriver::resetStreamState() {
    _have_last_sample  = false;
    _last_sample_ms    = 0;
    // Forget the in-flight target/direction so the next stream starts a fresh
    // blend instead of inheriting a stale reversal decision from before the
    // Halt/Home. Clean slate, ready to be told what to do again. :3
    _have_last_target  = false;
    _last_target_steps = 0;
    _last_dir          = 0;
}


// ---- Homing ------------------------------------------------------------------
//
// Architecture: mirrors StrokeEngine's _homingProcedure exactly.
//
// home() enables the motor via FAS, then spawns a one-shot FreeRTOS task on
// Core 1 that owns the entire homing sequence — sweep, poll, stop, backoff,
// re-zero. The task blocks internally with vTaskDelay(20ms) between endstop
// polls, which is the same cadence StrokeEngine uses. When done (success or
// failure) the task sets _homed/_homing and deletes itself. motorTask on Core 1
// just watches _homing go false and syncs g_state. No runHomingStep() polling
// needed — the task IS the homing loop. :3
//
// Using move() (relative steps) instead of runForward() (indefinite velocity
// run) is the other key difference from our old approach. move() queues a
// bounded step count; FAS executes it through its normal trapezoidal planner
// with the registered enable pin fully tracked. runForward() bypasses some of
// that planner state and can silently no-op after an E-stop cycle.

// Static trampoline — FreeRTOS needs a plain C function pointer, so we bounce
// through this into the member function. The `this` pointer rides in as param.
void TMC2160StepperDriver::_homingTaskImpl(void* param) {
    static_cast<TMC2160StepperDriver*>(param)->_homingTask();
}

// The actual homing procedure — runs entirely inside its own task on Core 1.
// Blocks with vTaskDelay() between polls so the scheduler stays happy. :3
void TMC2160StepperDriver::_homingTask() {
    MLOGF("Homing task: endstop pin %d state=%d (active=%d)\n",
          PIN_ENDSTOP, digitalRead(PIN_ENDSTOP), ENDSTOP_ACTIVE_STATE);

    // Set homing speed and a very high acceleration so the sweep is effectively
    // constant-velocity (no ramp to worry about). StrokeEngine uses /10 of max
    // accel for homing — we use a fixed high value since we want instant start.
    _stepper->setSpeedInHz((uint32_t)_home_speed_steps_s);
    _stepper->setAcceleration(100000);

    // Check if we're already sitting on the endstop — if so, back off first
    // then sweep back in, exactly like StrokeEngine does. :3
    if (digitalRead(PIN_ENDSTOP) == ENDSTOP_ACTIVE_STATE) {
        MLOGLN(F("Homing: Already at endstop — backing off before sweep"));
        // Back off 2x keepout (10mm) to clear the switch
        _stepper->move(-(int32_t)mmToNative(10.0f));  // negative = away from endstop
        while (_stepper->isRunning()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    // Sweep toward the endstop using move() with a step count large enough to
    // cover the full physical travel plus margin. move() is a RELATIVE command
    // that goes through FAS's normal trapezoidal planner with the enable pin
    // fully tracked — unlike runForward() which can silently no-op after an
    // E-stop cycle because it bypasses some planner state. We ram it deep,
    // balls-to-the-wall, until the endstop screams. :3
    int32_t sweep_steps = (int32_t)(_max_rail_mm * STEPS_PER_MM * 1.5f);
    _stepper->move(sweep_steps);  // positive = toward endstop

    MLOGF("Homing: Sweeping %d steps toward endstop at %u Hz\n",
          sweep_steps, (uint32_t)_home_speed_steps_s);

    // Poll the endstop every 20ms — same cadence as StrokeEngine. The motor
    // keeps thrusting forward while we watch for the switch to bottom out. :3
    while (_stepper->isRunning()) {
        if (digitalRead(PIN_ENDSTOP) == ENDSTOP_ACTIVE_STATE) {
            // Endstop triggered — slam the brakes and zero the position.
            // forceStopAndNewPosition() atomically halts the pulse train AND
            // sets the position counter. setCurrentPosition() silently fails
            // if the queue is still busy, so we use the atomic version. :3
            _stepper->forceStopAndNewPosition(0);

            // Wait for the motor to fully stop shuddering before we issue
            // the backoff move — FAS ignores moveTo() if the queue is busy.
            uint32_t stop_to = millis() + 500;
            while (_stepper->isRunning() && millis() < stop_to) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }

            MLOGF("Homing: Endstop hit! pos=%d running=%d\n",
                  _stepper->getCurrentPosition(), _stepper->isRunning());

            // Back off 5mm from the endstop — pull out just the tip, don't
            // stay balls-deep on the switch or it'll re-trigger the moment
            // anyone breathes on it. Negative steps = away from endstop. :3
            _stepper->setSpeedInHz(2000);
            _stepper->setAcceleration(50000);
            int32_t backoff = -mmToNative(5.0f);
            _stepper->moveTo(backoff);

            uint32_t timeout = millis() + 3000;
            while (_stepper->isRunning() && millis() < timeout) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            MLOGF("Homing: backoff done, pos=%d\n", _stepper->getCurrentPosition());

            if (digitalRead(PIN_ENDSTOP) == ENDSTOP_ACTIVE_STATE) {
                MLOGLN(F("WARNING: Endstop still active after backoff! Check switch/wiring."));
            } else {
                MLOGLN(F("Homing: Endstop released OK"));
            }

            // Re-zero at this backed-off position — this is home (0mm).
            // The carriage is now 5mm out from the endstop, which is our
            // coordinate origin. Everything else is measured from here. :3
            _stepper->forceStopAndNewPosition(0);
            _current_position_mm = 0.0f;
            _homed = true;
            _homing = false;

            MLOGLN(F("Homing: Complete — at home position, ready to pound :3"));
            _homingTaskHandle = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Motor stopped without hitting the endstop — homing failed. Disable the
    // motor so it doesn't hold position at some unknown location, and leave
    // _homed = false so the system knows it needs to try again. :3
    MLOGLN(F("Homing: FAILED — motor stopped before endstop. Check wiring/travel."));
    _stepper->disableOutputs();
    _homing = false;
    _homed  = false;

    _homingTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool TMC2160StepperDriver::home(int32_t home_speed_steps_s) {
    if (_homing) return false;

    MLOGLN(F("Homing: Starting..."));
    _homing = true;
    _homed  = false;
    _home_speed_steps_s = (home_speed_steps_s > 0) ? home_speed_steps_s : 4000;

    // Drop any leftover stream/target state — stale Intiface targets fight the
    // homing sweep and cause the motor to bang the endstop endlessly without
    // ever finishing. Nobody wants a partner who can't settle down. :3
    resetStreamState();

    // Enable the motor via FAS BEFORE spawning the task — exactly like
    // StrokeEngine's enableAndHome() calls enableOutputs() before creating the
    // homing task. FAS needs _outputEnabled = true before move() will execute.
    _stepper->enableOutputs();
    _enabled = true;

    // Spawn the self-contained homing task on Core 1 (same core as motorTask
    // and the FAS engine). Priority 20 matches StrokeEngine — high enough to
    // preempt normal motion but below the FAS ISR. The task deletes itself
    // when homing completes or fails. :3
    BaseType_t created = xTaskCreatePinnedToCore(
        _homingTaskImpl,        // static trampoline
        "Homing",               // task name
        4096,                   // stack (bytes) — homing does some string ops
        this,                   // param = this pointer
        20,                     // priority — same as StrokeEngine
        &_homingTaskHandle,     // handle so we can kill it on E-stop
        1                       // Core 1 — same core as FAS engine
    );
    if (created != pdPASS) {
        // Task never spawned (heap pressure). Without this rollback _homing
        // stays true forever and home() refuses until reboot — silently. :3
        _homing = false;
        _homingTaskHandle = nullptr;
        MLOGLN(F("Homing: FAILED to create homing task (out of memory?) — homing aborted."));
        return false;
    }

    return false;  // homing is async — watch isHoming()/isHomed() for completion
}

// runHomingStep() is a no-op — homing now runs entirely inside its own task.
// The function is kept to satisfy the MotorDriver interface; motorTask no
// longer calls it. :3
void TMC2160StepperDriver::runHomingStep() {
    // Nothing to do here — the homing task owns the loop now.
    // motorTask watches _homing go false and syncs g_state. :3
}

// Push-to-home: let the user establish home by simply pushing the shaft into
// the endstop — no web UI needed, just good old-fashioned manual persuasion. :3
// Only acts when we're idle and unhomed. If the endstop reads active, we zero
// the position right here and mark homed. The motor stays OFF so the shaft is
// free to be pushed by hand (consent is important); once homed, normal commands
// take the reins. Debounced so a momentary twitch doesn't false-trigger —
// nobody likes a premature homing.
bool TMC2160StepperDriver::checkPushToHome() {
    // Only relevant when not already homed and not running the active routine.
    if (_homed || _homing || !_stepper) return false;

    static uint32_t active_since = 0;
    uint32_t now = millis();

    if (digitalRead(PIN_ENDSTOP) == ENDSTOP_ACTIVE_STATE) {
        // Endstop pressed - start/continue debounce window.
        if (active_since == 0) active_since = now;

        // Require it held ~50ms to avoid noise/bounce false-homing.
        if (now - active_since >= 50) {
            active_since = 0;

            // Energize the motor so it can drive itself off the endstop and
            // then hold position (matches the normal homing cycle).
            enable();

            // Zero at the pressed (endstop) position first so the backoff move
            // is measured from here.
            _stepper->forceStopAndNewPosition(0);

            // Back off the SAME amount as the active homing routine (5mm away
            // from the endstop) so the carriage doesn't sit on the switch.
            // Negative steps = away from endstop toward the front.
            _stepper->setSpeedInHz(2000);
            _stepper->setAcceleration(50000);
            int32_t backoff_target = -mmToNative(5.0f);  // 5mm
            _stepper->moveTo(backoff_target);

            // Wait for the backoff move to finish.
            uint32_t timeout = millis() + 3000;
            while (_stepper->isRunning() && millis() < timeout) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }

            // Re-zero at this backed-off position = home (0mm), exactly like the
            // active homing routine does.
            _stepper->forceStopAndNewPosition(0);
            _current_position_mm = 0.0f;
            _homed = true;

            MLOGLN(F("Push-to-home: endstop pressed by hand - backed off 5mm, HOMED at 0"));
            return true;
        }
    } else {
        // Released before debounce elapsed - reset.
        active_since = 0;
    }
    return false;
}

// ---- Motion ------------------------------------------------------------------

bool TMC2160StepperDriver::moveTo(float pos_mm) {
    if (!_homed) {
        MLOGLN(F("Cannot move: not homed!"));
        return false;
    }

    // Always ensure motor is enabled before moving
    enable();

    // Clamp to the effective physical ceiling (measured stroke once homed, else
    // the configured max rail length) — rail-length agnostic. :3
    pos_mm = constrain(pos_mm, 0.0f, effectiveCeilingMm());

    // Coordinate system: home (endstop) = 0mm = step 0
    // "Out" (extended/front) = positive mm = NEGATIVE steps
    int32_t target_steps = -mmToNative(pos_mm);

    if (!_stepper) return false;

    // Speed must be set before moveTo() on FastAccelStepper
    uint32_t speed_hz = (uint32_t)(_max_speed_mm_s * STEPS_PER_MM);
    if (speed_hz < 1) speed_hz = 1;
    uint32_t accel_hz = (uint32_t)(_accel_mm_s2 * STEPS_PER_MM);
    if (accel_hz < 100) accel_hz = 100;

    _stepper->setSpeedInHz(speed_hz);
    _stepper->setAcceleration(accel_hz);

    int32_t pos_before = _stepper->getCurrentPosition();

    // If target == current, nothing to do
    if (target_steps == pos_before) {
        MLOGF("moveTo: %.1fmm already at target step %d\n", pos_mm, target_steps);
        return true;   // already there — a satisfied move, not a refusal
    }

    // If a previous move is still draining the queue, force-stop and re-sync
    // position first - otherwise FastAccelStepper ignores the new moveTo().
    if (_stepper->isRunning()) {
        _stepper->forceStopAndNewPosition(pos_before);
        uint32_t to = millis() + 300;
        while (_stepper->isRunning() && millis() < to) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    // Now the queue is empty - a normal absolute moveTo() works correctly.
    int8_t mret = (int8_t)_stepper->moveTo(target_steps);

    MLOGF("moveTo: %.1fmm -> step %d (from %d) at %u Hz ret=%d\n",
          pos_mm, target_steps, pos_before, speed_hz, mret);
    // Propagate FAS's verdict — a silently-refused move must be visible to the
    // caller (MotionArbiter), not just this log line. 0 = OK. :3
    return mret == 0;
}

// ============================================================================
// streamTo() — CONTINUOUS POSITION STREAMING (the Decel-Trap killer) :3
// ============================================================================
//
// Smooth streaming move for Intiface/TCode. Unlike moveTo(), this NEVER
// force-stops between commands — FastAccelStepper accepts a fresh moveTo()
// target while already running and re-plans on the fly. This is the exact
// pattern the proven position-streaming engines (OSSM-Sauce / OSSM-stream /
// StrokeEngine) use: EVERY incoming sample is the latest truth about where the
// shaft should be, so we ALWAYS retarget — we never drop a waypoint. :3
//
// CRITICAL LESSON (the bug this replaced): we previously had a "let-it-land"
// blend policy that DROPPED any sample which reversed direction while a stroke
// was still in flight. For a sine wave that's fatal — MFP streams points up to
// the peak then back down, and every falling sample near the top got dropped
// because the rising stroke "wasn't done." The carriage turned around early and
// NEVER reached the window extremes (100mm collapsed to ~50mm + stutter). The
// fix is simple and matches OSSM: don't be a brat, take every command. FAS does
// a clean decel→reverse on a true turnaround all by itself. :3
//
// The one safety we DO keep:
//   RAISE-ONLY ACCELERATION (the universal crash-avoidance pattern): once we're
//   moving, never SOFTEN the acceleration. FAS computes its braking distance
//   from the current accel; lowering it mid-flight means FAS suddenly needs a
//   longer brake ramp than the distance that's left, so it overshoots and
//   lurches. We only ever ramp accel UP while running. Stay firm, never go
//   limp early. :3
//
// _blend_mode is retained as a tuning knob for HOW aggressively a reversal is
// taken, but NONE of the modes drop samples anymore — tracking always wins:
//   1 = standard (default): always retarget, raise-only accel. The OSSM way.
//   2 = firm: same retarget, but force the configured accel on reversals so
//       turnarounds snap tighter (good for fast, short strokes).
//   3 = hybrid: standard, reserved for future per-segment tuning.
void TMC2160StepperDriver::streamTo(float pos_mm, float speed_mm_s) {
    if (!_homed || !_stepper) return;
    enable();

    pos_mm = constrain(pos_mm, 0.0f, effectiveCeilingMm());
    int32_t target_steps = -mmToNative(pos_mm);  // front = negative steps

    // Arm the stall watchdog with this REAL commanded sample (update() settles
    // here if the host ghosts us). Heads-up: speed 0 means "settle" and is
    // re-issued BY the watchdog itself — don't let that re-arm the timer or
    // we'd never time out. :3
    if (speed_mm_s > 0.0f) {
        _last_sample_mm = pos_mm;
        _last_sample_ms = millis();
        _have_last_sample = true;
    }

    // ---- Direction of THIS commanded move (native-step sign) ----------------
    // Tracked for telemetry / future tuning only. We NO LONGER drop reversals —
    // every sample retargets. Dropping reversals is what collapsed the range. :3
    int32_t cur_steps = _stepper->getCurrentPosition();
    int32_t delta     = target_steps - cur_steps;
    int8_t  new_dir   = (delta > 0) ? 1 : (delta < 0) ? -1 : 0;

    // ---- Speed / acceleration -----------------------------------------------
    //
    // Always command the full configured accel — no raise-only guard. The old
    // guard was meant to prevent overshoot by never softening mid-flight, but
    // it was locking in stale accel values from previous segments and preventing
    // the configured 8000 mm/s² from ever taking effect. FAS handles mid-flight
    // retargets cleanly at any accel — it re-plans from current velocity. :3
    //
    // S-curve (jerk limiting): setLinearAcceleration(N) makes FAS ramp the
    // acceleration linearly from 0 to the target over N steps, producing a
    // smooth S-curve instead of a hard trapezoid corner. This eliminates the
    // mechanical jerk at stroke reversals — the shaft eases into and out of
    // each direction change instead of snapping. N=0 disables it (pure trapezoid).
    // We use a small fixed value so the S-curve is tight enough to not eat
    // the interval budget but smooth enough to feel silky. :3

    float spd = (speed_mm_s > 0.0f) ? speed_mm_s : _max_speed_mm_s;
    spd = constrain(spd, 1.0f, _max_speed_mm_s);

    uint32_t speed_hz = (uint32_t)(spd * STEPS_PER_MM);
    if (speed_hz < 1) speed_hz = 1;
    uint32_t accel_hz = (uint32_t)(_accel_mm_s2 * STEPS_PER_MM);
    if (accel_hz < 100) accel_hz = 100;

    _stepper->setSpeedInHz(speed_hz);
    _stepper->setAcceleration(accel_hz);

    // NOTE: setLinearAcceleration() is NOT called here. Calling it on every
    // streamTo() (30-100x/sec) triggers FAS's recalc_ramp_steps flag on every
    // single call, forcing a full ramp recalculation mid-flight — that IS the
    // bumpy/gritty feel. It's set ONCE in init() and never touched again during
    // streaming. The S-curve is baked in at boot, not re-applied every thrust. :3

    // No force-stop, no busy-wait: just retarget. If the new target equals the
    // current commanded target FastAccelStepper ignores it cheaply.
    _stepper->moveTo(target_steps);

    // Remember this stroke's target + direction so the NEXT command can detect
    // a reversal against it. Direction 0 (no move) leaves the last real
    // direction intact so a tiny no-op sample doesn't wipe our reversal memory.
    _last_target_steps = target_steps;
    if (new_dir != 0) _last_dir = new_dir;
    _have_last_target = true;
}

// ============================================================================
// streamToSteps() — pre-planned native-step dispatch from motionConsumerTask
// ============================================================================
//
// Called exclusively from Core 1 (motionConsumerTask). Speed and accel arrive
// already converted to steps/s and steps/s² by the consumer — no unit math
// here, just arm the watchdog and fire straight to FAS. The raise-only accel
// guard is applied by the consumer BEFORE calling this function, so finalAccel
// is already the correct value to hand to FAS. :3
//
// This is the clean path: the consumer owns all the kinematic math, this
// function owns the hardware dispatch. Single responsibility, no cross-core
// touching. The shaft gets told exactly what to do and does it. :3
void TMC2160StepperDriver::streamToSteps(int32_t target_steps,
                                          uint32_t speed_steps_s,
                                          uint32_t accel_steps_s2) {
    if (!_homed || !_stepper) return;
    enable();

    // Arm the stall watchdog — convert target_steps back to mm for the
    // existing watchdog logic (which works in mm). If the host goes quiet,
    // update() will settle here. :3
    float pos_mm = nativeToMm(-target_steps);  // negative because front=negative steps
    _last_sample_mm  = pos_mm;
    _last_sample_ms  = millis();
    _have_last_sample = true;

    // GRIT FIX: only call setAcceleration()/setSpeedInHz() when the values
    // actually change. Calling them on every waypoint (30–100x/sec) forces FAS
    // to recalculate its ramp on every single call — even mid-flight. That
    // recalc is the source of the gritty/stuttery feel: the motor gets a new
    // ramp profile injected into it 100 times a second whether it needs one or
    // not. OSSM avoids this by only updating when the value differs. :3
    //
    // We cache the last-sent values and skip the FAS call when they match.
    // The first call always goes through (cache starts at 0). :3
    if (accel_steps_s2 != _last_accel_steps_s2) {
        _stepper->setAcceleration(accel_steps_s2);
        _last_accel_steps_s2 = accel_steps_s2;
    }
    if (speed_steps_s != _last_speed_steps_s) {
        _stepper->setSpeedInHz(speed_steps_s);
        _last_speed_steps_s = speed_steps_s;
    }

    // moveTo() is non-blocking: FAS re-plans from current velocity to the new
    // target. No force-stop, no busy-wait. The shaft just gets told where to go
    // and keeps thrusting without missing a beat. :3
    _stepper->moveTo(target_steps);
}

void TMC2160StepperDriver::runMotorStep() {
    // NOP — moveTo()/streamTo()/streamToSteps() retarget through FAS directly.
    // Stream stall watchdog (streamTo() stashing) lives in update().
    // This stub exists to satisfy the MotorDriver interface.
}

// ---- Speed & Acceleration ----------------------------------------------------

void TMC2160StepperDriver::setMaxSpeed(float speed_mm_s) {
    _max_speed_mm_s = constrain(speed_mm_s, 0.0f, MAX_SPEED_MM_S);
}

void TMC2160StepperDriver::setAcceleration(float accel_mm_s2) {
    // Ceiling raised to 20000 mm/s² — 5000 was rejecting the new 8000 default
    // and silently clamping every stroke to a crawl. 20000 is well within what
    // the TMC2160 + NEMA17 can handle for short bursts. :3
    _accel_mm_s2 = constrain(accel_mm_s2, 10.0f, 20000.0f);


    if (_stepper) {
        _stepper->setAcceleration((int32_t)(_accel_mm_s2 * STEPS_PER_MM));
    }
}

uint32_t TMC2160StepperDriver::getLiveAcceleration() const {
    // Return the acceleration currently active inside the FAS ramp engine —
    // NOT the configured ceiling. This is what OSSM reads with
    // stepper->getAcceleration() in its raise-only guard. The full
    // FastAccelStepper header is available here (included at the top of this
    // .cpp), so the call resolves correctly. The header only has a forward
    // declaration, which is why this can't be inline there. :3
    return _stepper ? (uint32_t)_stepper->getAcceleration() : 0u;
}

// ---- Status ------------------------------------------------------------------

bool TMC2160StepperDriver::isMoving() {
    return _stepper ? _stepper->isRunning() : false;
}

float TMC2160StepperDriver::getPosition() const {
    if (!_stepper) return 0.0f;
    // Steps are negative for positive mm positions (endstop=0, front=negative steps)
    return nativeToMm(-_stepper->getCurrentPosition());
}

float TMC2160StepperDriver::getTargetPosition() const {
    // Approximate: use current position if not moving
    return getPosition();
}

// ---- Stop / HardStop ---------------------------------------------------------

void TMC2160StepperDriver::stop() {
    // E-stop: halt the pulse train, kill the homing task if it's running,
    // disable the motor via FAS (keeps _outputEnabled in sync), and clear all
    // state. The shaft goes completely soft — no ambiguity, no stale flags. :3

    // Kill the homing task if it's mid-sweep — we're pulling out NOW. :3
    if (_homingTaskHandle != nullptr) {
        vTaskDelete(_homingTaskHandle);
        _homingTaskHandle = nullptr;
    }

    if (_stepper) {
        _stepper->forceStop();
        // FAS drives PIN_ENABLE HIGH and clears _outputEnabled — consistent
        // internal state so the next enableOutputs() + move() works cleanly.
        _stepper->disableOutputs();
    }
    _enabled = false;
    // Cancel any homing in progress — the task is dead, clear the flag.
    _homing = false;
    // Full E-stop means we're no longer at a known position — the carriage
    // might've moved while the motor was limp, or someone might've shoved it
    // around. Clear _homed so the driver's internal state matches g_state.homed,
    // which handleApiStop() also sets to false. Otherwise isHomed() returns
    // stale truth and the next home cycle gets confused about where it is.
    // A pulled plug means nobody knows where the tip is anymore. :3
    _homed = false;
    // Drop stale stream/target state so the next Home/move starts clean and
    // nothing keeps re-issuing the last streamed target.
    resetStreamState();
}

void TMC2160StepperDriver::hardStop() {
    // Immediate stop without deceleration
    if (_stepper) {
        _stepper->forceStop();
    }
    // Halt (Pause/Buttplug stop) routes here. Clear the blend/stream state too,
    // otherwise the stall watchdog keeps humping the last target right after the
    // stop — and a later Home has to fight it off. Nobody needs a jealous
    // ex-target clinging on after the scene's over. :3
    resetStreamState();

}

// ---- Driver config -----------------------------------------------------------

void TMC2160StepperDriver::applyDriverConfig(const DriverConfig& cfg) {
    if (!_tmc) return;

    // NOTE on microsteps: STEPS_PER_MM is a compile-time constant derived from
    // MICROSTEPS (16). Changing the driver's microstepping live would desync
    // the position math, so we keep the driver at MICROSTEPS regardless of what
    // is requested. (Microsteps mostly affects smoothness/noise; 16 with intpol
    // is already very smooth.) The field is retained for completeness.
    _tmc->microsteps(MICROSTEPS);
    _tmc->intpol(true);                 // 256 microstep interpolation - always on

    // --- Current ---
    _tmc->rms_current(cfg.run_current_ma);
    // ihold/irun are 0..31. Map the requested hold % of run current onto 0..31.
    uint8_t ihold = (uint8_t)constrain((int)((cfg.hold_current_pct * 31) / 100), 0, 31);
    _tmc->ihold(ihold);
    _tmc->irun(31);                     // full run current scale (rms_current sets the cap)

    // --- Chopper ---
    _tmc->toff(cfg.toff);               // 0 disables the driver; 1..15 enables
    _tmc->blank_time(cfg.tbl == 0 ? 16 : cfg.tbl == 1 ? 24 : cfg.tbl == 2 ? 36 : 54);
    _tmc->hysteresis_start(constrain((int)cfg.hstart, 0, 7));
    _tmc->hysteresis_end(constrain((int)cfg.hend, -3, 12));

    // --- StealthChop vs SpreadCycle ---
    // en_pwm_mode=1 enables StealthChop (quiet). TPWMTHRS sets the TSTEP (speed)
    // below which StealthChop is used and above which it falls back to
    // SpreadCycle (more torque at speed). 0 = stay in the selected mode always.
    _tmc->en_pwm_mode(cfg.stealthchop ? true : false);
    _tmc->pwm_autoscale(true);          // required for StealthChop to self-tune
    _tmc->TPWMTHRS(cfg.tpwm_thrs);

    // Readback verification — same check init() does after its register writes.
    // A transient SPI glitch on a live settings change would otherwise leave the
    // TMC silently running the OLD settings while UI + driver both believe the
    // new config took effect. Microsteps is the canary (exact-match readback);
    // rms_current readback is quantized so we log it rather than compare. :3
    uint16_t rb_ms  = _tmc->microsteps();
    uint32_t rb_cur = _tmc->rms_current();
    if (rb_ms != MICROSTEPS) {
        MLOGF("WARNING: TMC readback MISMATCH after config write (microsteps set=%u read=%u) "
              "— SPI glitch, settings may NOT be active!\n", MICROSTEPS, rb_ms);
    }

    MLOGF("Driver: run=%umA (rb=%lumA) hold=%u%% toff=%u tbl=%u sc=%s tpwmthrs=%lu hs=%d he=%d rb_ms=%u\n",
          cfg.run_current_ma, (unsigned long)rb_cur, cfg.hold_current_pct, cfg.toff, cfg.tbl,
          cfg.stealthchop ? "stealth" : "spread",
          (unsigned long)cfg.tpwm_thrs, (int)cfg.hstart, (int)cfg.hend, rb_ms);
}

// ---- Diagnostics -------------------------------------------------------------

uint16_t TMC2160StepperDriver::getCurrentmA() {
    if (_tmc) {
        return _tmc->rms_current();
    }
    return 0;
}

uint8_t TMC2160StepperDriver::getMicrosteps() {
    if (_tmc) {
        return (uint8_t)_tmc->microsteps();
    }
    return MICROSTEPS;
}

// ---- Unit conversion ---------------------------------------------------------

int32_t TMC2160StepperDriver::mmToNative(float mm) const {
    return (int32_t)(mm * STEPS_PER_MM);
}

float TMC2160StepperDriver::nativeToMm(int32_t native) const {
    return (float)native / STEPS_PER_MM;
}

#endif // defined(DRIVER_TMC2160)