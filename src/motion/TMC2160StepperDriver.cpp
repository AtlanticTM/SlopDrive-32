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
        // Configure enable pin (true = active LOW enable, matches TMC2160 EN)
        _stepper->setEnablePin(PIN_ENABLE, true);
        // Do NOT use AutoEnable - we manage the enable pin manually
        // so we can cut power on stop() and control homing precisely
        _stepper->setAutoEnable(false);
        _stepper->disableOutputs();
        _stepper->setCurrentPosition(0);
        MLOGLN(F("FastAccelStepper: Motor initialized (STEP/DIR/EN configured)"));
    } else {
        MLOGLN(F("FastAccelStepper: Failed to create motor!"));
    }

    // Set initial speed/acceleration in mm units
    setMaxSpeed(_max_speed_mm_s);
    setAcceleration(_accel_mm_s2);
}

void TMC2160StepperDriver::update() {
    runMotorStep();
    updateExtrapolation();
}

void TMC2160StepperDriver::emergencyStop() {
    hardStop();
    disable();
    _homed = false;
    _homing = false;
}

// ---- Enable / Disable --------------------------------------------------------

void TMC2160StepperDriver::enable() {
    if (_stepper) {
        _stepper->enableOutputs();
        _enabled = true;
    }
}

void TMC2160StepperDriver::disable() {
    if (_stepper) {
        hardStop();
        _stepper->disableOutputs();
        _enabled = false;
    }
}

// ---- Stream-state reset ------------------------------------------------------

void TMC2160StepperDriver::resetStreamState() {
    _moving_to_target = false;
    _coasting         = false;
    _have_last_sample = false;
    _stream_velocity  = 0.0f;
    _last_sample_ms   = 0;
}

// ---- Homing ------------------------------------------------------------------

bool TMC2160StepperDriver::home(int32_t home_speed_steps_s) {
    if (_homing) return false;

    MLOGLN(F("Homing: Starting..."));
    MLOGF("Homing: Endstop pin %d state = %d (active=%d)\n",
          PIN_ENDSTOP, digitalRead(PIN_ENDSTOP), ENDSTOP_ACTIVE_STATE);
    _homing = true;
    _homed = false;

    // Drop any leftover stream/target state from before this home request,
    // otherwise the motorTask keeps humping old Intiface targets and fights
    // the homing runForward() — the motor just bangs the endstop endlessly
    // without ever finishing. Nobody wants a partner who can't settle down. :3
    resetStreamState();

    // Make sure no previous move is still draining the queue before we start
    // the homing sweep, or runForward() gets ghosted like a bad hookup.
    if (_stepper && _stepper->isRunning()) {
        _stepper->forceStop();
        uint32_t to = millis() + 300;
        while (_stepper->isRunning() && millis() < to) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    // Make sure motor is enabled
    enable();

    // First, check if we're already at the endstop
    if (digitalRead(PIN_ENDSTOP) == ENDSTOP_ACTIVE_STATE) {
        MLOGLN(F("Homing: Already at home position"));
        _stepper->setCurrentPosition(0);
        _current_position_mm = 0.0f;
        _homed = true;
        _homing = false;
        return true;
    }

    // Homing speed: 200 steps/s = 2.5mm/s at 16x microsteps (80 steps/mm)
    // Use runForward() for constant-speed homing with no deceleration target
    int32_t home_speed_abs = (home_speed_steps_s > 0) ? home_speed_steps_s : 200;

    // Set very high acceleration so motion is effectively instant/linear (no ramp)
    _stepper->setAcceleration(100000);
    _stepper->setSpeedInHz(home_speed_abs);

    // Brief delay to ensure driver is fully enabled before stepping
    delay(100);

    // runForward() moves indefinitely in positive direction (toward endstop)
    // This avoids the "stopped before endstop" false trigger from moveTo()
    _stepper->runForward();

    MLOGF("Homing: Running forward at %d steps/s\n", home_speed_abs);

    return false;  // Will complete via runHomingStep() calls
}

void TMC2160StepperDriver::runHomingStep() {
    if (!_homing) return;

    // Periodically log endstop state so user can debug via the web log
    static uint32_t _last_log_ms = 0;
    uint32_t now = millis();
    if (now - _last_log_ms > 500) {
        _last_log_ms = now;
        MLOGF("Homing poll: endstop=%d running=%d pos=%d\n",
              digitalRead(PIN_ENDSTOP), _stepper->isRunning(),
              _stepper->getCurrentPosition());
    }

    // Check endstop during homing
    if (digitalRead(PIN_ENDSTOP) == ENDSTOP_ACTIVE_STATE) {
        // Hit the endstop - stop immediately and clear the queue.
        // forceStopAndNewPosition() clears the command queue AND sets position
        // atomically. This is REQUIRED: setCurrentPosition() silently fails if
        // the queue is still busy after a plain forceStop().
        _stepper->forceStopAndNewPosition(0);

        // Wait until the stepper is fully stopped before issuing a new move.
        // FastAccelStepper ignores runBackward()/moveTo() if a previous command
        // is still draining from the queue - this is why backoff never moved.
        uint32_t stop_to = millis() + 500;
        while (_stepper->isRunning() && millis() < stop_to) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        _current_position_mm = 0.0f;
        _homed = true;
        _homing = false;

        MLOGF("Homing: Endstop triggered! Stopped at step pos=%d running=%d\n",
              _stepper->getCurrentPosition(), _stepper->isRunning());

        // Back off from endstop — pull out just the tip (5mm), don't stay
        // balls-deep on the switch. Move NEGATIVE = away from endstop toward
        // front. Use a bounded moveTo() now that the queue is empty and the
        // motor's finished shuddering against the endstop. Gotta leave some
        // breathing room or he'll trigger again the second anyone twitches. :3
        _stepper->setSpeedInHz(2000);
        _stepper->setAcceleration(50000);
        int32_t backoff_target = -mmToNative(5.0f);  // 5mm = step -400
        int8_t bret = _stepper->moveTo(backoff_target);
        MLOGF("Homing: backoff moveTo(%d) ret=%d\n", backoff_target, bret);

        // Wait for backoff move to complete
        uint32_t timeout = millis() + 3000;
        while (_stepper->isRunning() && millis() < timeout) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        MLOGF("Homing: backoff done, pos=%d\n", _stepper->getCurrentPosition());

        // Verify endstop is released after backoff
        if (digitalRead(PIN_ENDSTOP) == ENDSTOP_ACTIVE_STATE) {
            MLOGLN(F("WARNING: Endstop still active after backoff! Check switch/wiring."));
        } else {
            MLOGLN(F("Homing: Endstop released OK"));
        }

        // Re-zero at this backed-off position = home (0mm)
        _stepper->forceStopAndNewPosition(0);
        _current_position_mm = 0.0f;

        MLOGLN(F("Homing: Complete - at home position"));
        return;
    }

    // Note: with runForward(), motor runs indefinitely until endstop is hit.
    // No "stopped before endstop" check needed - motor won't stop on its own.
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

void TMC2160StepperDriver::moveTo(float pos_mm) {
    if (!_homed) {
        MLOGLN(F("Cannot move: not homed!"));
        return;
    }

    // Always ensure motor is enabled before moving
    enable();

    // Clamp to physical limits
    pos_mm = constrain(pos_mm, 0.0f, PHYSICAL_MAX_TRAVEL_MM);

    // Coordinate system: home (endstop) = 0mm = step 0
    // "Out" (extended/front) = positive mm = NEGATIVE steps
    int32_t target_steps = -mmToNative(pos_mm);

    if (!_stepper) return;

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
        return;
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
    int8_t mret = _stepper->moveTo(target_steps);

    _moving_to_target = false;  // moveTo() handles its own deceleration to target

    MLOGF("moveTo: %.1fmm -> step %d (from %d) at %u Hz ret=%d\n",
          pos_mm, target_steps, pos_before, speed_hz, mret);
}

// Smooth streaming move for Intiface/TCode. Unlike moveTo(), this NEVER
// force-stops the motor between commands - FastAccelStepper accepts a new
// moveTo() target while already running and smoothly re-plans the trajectory.
// The force-stop + busy-wait in moveTo() is what made streamed motion stutter.
void TMC2160StepperDriver::streamTo(float pos_mm, float speed_mm_s) {
    if (!_homed || !_stepper) return;
    enable();

    pos_mm = constrain(pos_mm, 0.0f, PHYSICAL_MAX_TRAVEL_MM);
    int32_t target_steps = -mmToNative(pos_mm);  // front = negative steps

    // Resolve speed: use per-command speed if given, else configured max.
    float spd = (speed_mm_s > 0.0f) ? speed_mm_s : _max_speed_mm_s;
    spd = constrain(spd, 1.0f, _max_speed_mm_s);

    uint32_t speed_hz = (uint32_t)(spd * STEPS_PER_MM);
    if (speed_hz < 1) speed_hz = 1;
    uint32_t accel_hz = (uint32_t)(_accel_mm_s2 * STEPS_PER_MM);
    if (accel_hz < 100) accel_hz = 100;

    _stepper->setSpeedInHz(speed_hz);
    _stepper->setAcceleration(accel_hz);

    // No force-stop, no busy-wait: just retarget. If the new target equals the
    // current commanded target FastAccelStepper ignores it cheaply.
    _stepper->moveTo(target_steps);
    _moving_to_target = false;
}

// Predictive extrapolation: read the heat of the incoming sample stream and
// command the motor to a point slightly AHEAD, so it keeps coasting between
// samples instead of bottoming out on each one. Bounded overshoot + a stall
// timeout (updateExtrapolation) keep it safe — we edge, we don't crash. :3
void TMC2160StepperDriver::streamExtrapolated(float pos_mm, float speed_mm_s) {
    if (!_homed || !_stepper) return;

    uint32_t now = millis();
    pos_mm = constrain(pos_mm, 0.0f, PHYSICAL_MAX_TRAVEL_MM);

    float projected = pos_mm;

    if (_have_last_sample) {
        uint32_t dt = now - _last_sample_ms;
        // Ignore absurdly long gaps (treat as a fresh start) and zero dt.
        if (dt > 0 && dt < STREAM_STALL_MS * 3) {
            // Measure how fast these samples are slamming into us (mm/s).
            // The velocity between consecutive positions — read their heat,
            // smooth it just a little to reject jittery little trembles
            // between thrusts. A steady rhythm means a good pounding. :3
            float v = (pos_mm - _last_sample_mm) / ((float)dt / 1000.0f);
            _stream_velocity = 0.5f * _stream_velocity + 0.5f * v;

            // Project ahead — we're not stopping AT the sample, we're ramming
            // PAST it by the lookahead window, inflating the target deeper than
            // commanded. The motor overshoots, kept on a tight leash so it
            // doesn't run away and wreck the furniture.
            float overshoot = _stream_velocity * ((float)_lookahead_ms / 1000.0f);
            // Bound how far past the sample we're willing to let it stretch.
            overshoot = constrain(overshoot, -_max_overshoot_mm, _max_overshoot_mm);
            projected = pos_mm + overshoot;
            projected = constrain(projected, 0.0f, PHYSICAL_MAX_TRAVEL_MM);
            _coasting = true;
        }
    }

    _last_sample_mm = pos_mm;
    _last_sample_ms = now;
    _have_last_sample = true;

    // Drive to the projected (slightly-ahead) target. streamTo() never
    // force-stops, so consecutive projections blend into continuous motion.
    streamTo(projected, speed_mm_s);
}

// Stall fallback: if nobody's been talking dirty to us for a while, stop
// coasting and settle exactly on the last REAL sample so the motor can't
// run away past it like an overexcited pup off-leash. :3
void TMC2160StepperDriver::updateExtrapolation() {
    if (!_coasting || !_have_last_sample || !_stepper) return;
    if (millis() - _last_sample_ms >= STREAM_STALL_MS) {
        _coasting = false;
        _stream_velocity = 0.0f;
        // Ease back to the real last sample at the configured max speed.
        streamTo(_last_sample_mm, 0.0f);
    }
}

void TMC2160StepperDriver::runMotorStep() {
    if (!_moving_to_target || !_stepper) return;

    int32_t current = _stepper->getCurrentPosition();

    // Check if we've reached or passed the target
    // _target_steps is negative for positive mm (e.g. -800 for 10mm)
    // runBackward() decrements position, runForward() increments
    bool reached = (_target_steps < 0)
        ? (current <= _target_steps)   // moving backward (negative): stop when <= target
        : (current >= _target_steps);  // moving forward (positive): stop when >= target

    if (reached) {
        _stepper->forceStop();
        _moving_to_target = false;
        _current_position_mm = nativeToMm(-_target_steps);
        MLOGF("runMotorStep: reached target step %d (actual %d)\n",
              _target_steps, current);
    }
}

// ---- Speed & Acceleration ----------------------------------------------------

void TMC2160StepperDriver::setMaxSpeed(float speed_mm_s) {
    _max_speed_mm_s = constrain(speed_mm_s, 0.0f, MAX_SPEED_MM_S);
}

void TMC2160StepperDriver::setAcceleration(float accel_mm_s2) {
    _accel_mm_s2 = constrain(accel_mm_s2, 10.0f, 5000.0f);

    if (_stepper) {
        _stepper->setAcceleration((int32_t)(_accel_mm_s2 * STEPS_PER_MM));
    }
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
    // Stop movement and cut motor power
    if (_stepper) {
        _stepper->forceStop();
        _stepper->disableOutputs();
    }
    _enabled = false;
    // Also cancel any homing in progress
    _homing = false;
    // Drop stale stream/target state so the next Home/move starts clean and
    // nothing keeps re-issuing the last streamed target.
    resetStreamState();
}

void TMC2160StepperDriver::hardStop() {
    // Immediate stop without deceleration
    if (_stepper) {
        _stepper->forceStop();
    }
    // Halt (Pause/Buttplug stop) routes here. Clear the coast/extrapolation
    // state too, otherwise updateExtrapolation() keeps humping the last target
    // right after the stop — and a later Home has to fight it off. Nobody needs
    // a jealous ex-target clinging on after the scene's over.
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

    MLOGF("Driver: run=%umA hold=%u%% toff=%u tbl=%u sc=%s tpwmthrs=%lu hs=%d he=%d\n",
          cfg.run_current_ma, cfg.hold_current_pct, cfg.toff, cfg.tbl,
          cfg.stealthchop ? "stealth" : "spread",
          (unsigned long)cfg.tpwm_thrs, (int)cfg.hstart, (int)cfg.hend);
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