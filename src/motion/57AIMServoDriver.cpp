// 57AIMServoDriver — concrete MotorDriver for the 57AIM30 closed-loop servo.
// Build-guarded behind DRIVER_57AIM_SERVO (set in platformio.ini).
//
// This is a dumb Step/Direction driver. No SPI. No Modbus. No register soup.
// The 57AIM30 handles its own closed-loop control internally — we just send
// pulses and direction and it obeys like a very well-trained hole. :3
// Fisting the motion pipeline with raw step pulses, no lube required. yippie!
#if defined(DRIVER_57AIM_SERVO)

#include "57AIMServoDriver.h"
#include <FastAccelStepper.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "AppLog.h"

// In serial-control mode the USB Serial port is dedicated to Intiface TCode,
// so debug output must go to the in-memory web log instead of Serial (which
// would corrupt the command stream). These macros redirect this file's logging
// to applog when SERIAL_CONTROL_MODE is on, and to Serial otherwise. :3
#if SERIAL_CONTROL_MODE
  #define MLOGF(...)  applogf(__VA_ARGS__)
  #define MLOGLN(s)   applog(String(s).c_str())
#else
  #define MLOGF(...)  Serial.printf(__VA_ARGS__)
  #define MLOGLN(s)   Serial.println(s)
#endif

// FastAccelStepperEngine — the background pulse-generation engine on ESP32.
// One static instance shared across the whole driver. Named _fas_engine to
// avoid shadowing MotorDriver::_engine. It runs on Core 1 and generates the
// step pulses in hardware timer ISRs — relentless, rhythmic, and perfectly
// timed. Like a machine that doesn't stop until you tell it to. :3
static FastAccelStepperEngine _fas_engine;

// Endstop is active LOW — the optocoupler pulls the pin LOW when the carriage
// slams home. HIGH = clear, LOW = triggered. The carriage bottoms out and
// screams into the switch like it's trying to get as deep as possible. :3
#define ENDSTOP_ACTIVE_STATE LOW

Ai57AIMServoDriver::Ai57AIMServoDriver() {}

// ---- Lifecycle ---------------------------------------------------------------

void Ai57AIMServoDriver::init() {
    // No SPI bus to bring up. No TMC registers to write. No chip select to
    // assert. We just configure two GPIO pins and hand them to FAS. The 57AIM30
    // drive is already sitting there, energized, waiting to be told what to do.
    // Plug it in and it's ready to take everything we give it. :3

    // Endstop pin — INPUT_PULLUP keeps the line HIGH when the switch is open.
    // The optocoupler pulls it LOW when the carriage hits the endstop. If we
    // left it floating it'd false-trigger constantly — a twitchy, unreliable
    // mess. Pull it up and it only screams when it means it. :3
    pinMode(AIM_PIN_ENDSTOP, INPUT_PULLUP);

    // Store the engine pointer so everyone can share the toy. :3
    _engine = &_fas_engine;

    // Initialize FastAccelStepperEngine — creates the background timer task
    // on ESP32. This is the engine that generates the actual step pulses in
    // hardware, so the CPU doesn't have to bit-bang. Smooth, hardware-timed,
    // and absolutely relentless. The machine that keeps pounding no matter
    // what the OS is doing. :3
    _fas_engine.init();

    // Connect the stepper to the PUL pin. stepperConnectToPin() takes ONLY
    // the step pin — direction is set separately via setDirectionPin().
    // Previously passing (STEP, DIR) was NOT a valid overload — the DIR pin
    // was never set, so the motor could only ever thrust in one direction.
    // Forward toward the endstop worked, but pulling back out was just a sad
    // little nothing. Not anymore. :3
    _stepper = _fas_engine.stepperConnectToPin(AIM_PIN_STEP);

    if (_stepper) {
        // Direction pin — true = invert. New motor runs opposite polarity to
        // the old one, so we flip the DIR signal here instead of rewiring.
        // If it goes wrong again, flip back to false or swap the DIR wire. :3
        _stepper->setDirectionPin(AIM_PIN_DIR, true);

        // No enable pin on the 57AIM30 — the drive is always energized when
        // powered. We don't register an enable pin with FAS at all. The drive
        // is always ready, always hungry, always waiting for the next pulse. :3
        //
        // CRITICAL: setAutoEnable(false) means FAS won't auto-enable on move,
        // but it also means FAS's internal _outputEnabled flag starts as FALSE.
        // With no enable pin registered, every moveTo() call is silently blocked
        // until _outputEnabled is true. We call enableOutputs() ONCE here to
        // permanently open the gate — the 57AIM30 has no hardware to toggle so
        // this is purely a flag flip inside FAS. Without this the motor sits
        // completely dead and ignores every command. uhoh. :3
        _stepper->setAutoEnable(false);
        _stepper->enableOutputs();   // permanently open — no hardware pin, just the flag
        _enabled = true;
        _stepper->setCurrentPosition(0);

        MLOGLN(F("57AIMServo: FastAccelStepper initialized (PUL/DIR registered)"));
        MLOGF("57AIMServo: PUL=GPIO%d DIR=GPIO%d ENDSTOP=GPIO%d\n",
              AIM_PIN_STEP, AIM_PIN_DIR, AIM_PIN_ENDSTOP);
        MLOGF("57AIMServo: %u steps/rev, %.1f mm/rev, %.1f steps/mm, %.1f mm travel\n",
              (uint32_t)AIM_STEPS_PER_REV, AIM_MM_PER_REV,
              AIM_STEPS_PER_MM, AIM_MAX_TRAVEL_MM);
    } else {
        MLOGLN(F("57AIMServo: ERROR — FastAccelStepper failed to connect to PUL pin!"));
    }

    // Set initial speed/acceleration in mm units. These get converted to
    // steps/s and steps/s² inside setMaxSpeed/setAcceleration. :3
    setMaxSpeed(_max_speed_mm_s);
    setAcceleration(_accel_mm_s2);
}

void Ai57AIMServoDriver::update() {
    runMotorStep();

    // Stream stall watchdog: if the host has gone quiet (no fresh waypoint for
    // STREAM_STALL_MS) but we're still mid-thrust toward a streamed target,
    // settle firmly on the last REAL sample so the carriage can't keep coasting
    // toward a stale target after the stream drops. We don't extrapolate past
    // it — that toy got thrown out. We just stay put where we were last told.
    // Like a good hole that holds position until it gets new instructions. :3
    if (_have_last_sample && _stepper &&
        (millis() - _last_sample_ms >= STREAM_STALL_MS)) {
        _have_last_sample = false;   // one-shot; re-armed on the next streamTo()
        streamTo(_last_sample_mm, 0.0f);
    }
}

void Ai57AIMServoDriver::emergencyStop() {
    // The red button got slapped. Cut the pulse train NOW. The 57AIM30 will
    // decelerate on its own internal ramp — we just stop commanding it.
    // Everything goes soft. No ambiguity. No stale flags. :3
    hardStop();
    _homed  = false;
    _homing = false;
}

// ---- Enable / Disable --------------------------------------------------------
//
// The 57AIM30 has no software enable pin — it's always energized when powered.
// These calls satisfy the MotorDriver interface but do nothing to hardware.
// The drive is always ready. Always full. Always waiting. :3

void Ai57AIMServoDriver::enable() {
    // No enable pin. The 57AIM30 is always on. We track the flag for interface
    // compatibility but there's nothing to toggle. It's always hard. :3
    if (_stepper) _stepper->enableOutputs();
    _enabled = true;
}

void Ai57AIMServoDriver::disable() {
    // Same deal — no hardware to disable. We stop the pulse train so the drive
    // stops receiving commands, but it stays energized and holding position.
    // The shaft stays firm even when we stop talking to it. :3
    if (_stepper) {
        hardStop();
        _stepper->disableOutputs();
    }
    _enabled = false;
}

// ---- Stream-state reset ------------------------------------------------------

void Ai57AIMServoDriver::resetStreamState() {
    _have_last_sample  = false;
    _last_sample_ms    = 0;
    // Forget the in-flight target/direction so the next stream starts a fresh
    // blend instead of inheriting a stale reversal decision from before the
    // Halt/Home. Clean slate, ready to be stuffed full again. :3
    _have_last_target  = false;
    _last_target_steps = 0;
    _last_dir          = 0;
}

// ---- Homing ------------------------------------------------------------------
//
// Architecture: mirrors StrokeEngine's _homingProcedure exactly.
//
// home() spawns a one-shot FreeRTOS task on Core 1 that owns the entire homing
// sequence — sweep toward the endstop, poll until it triggers, hard-stop,
// back off AIM_HOMING_BACKOFF_MM (10mm), re-zero. The task blocks internally
// with vTaskDelay(20ms) between endstop polls. When done (success or failure)
// the task sets _homed/_homing and deletes itself.
//
// The homing sweep is like fisting — you push in slowly and steadily until
// you feel the resistance, then you stop and pull back just enough to breathe.
// The endstop is the deepest point. We zero there, back off 10mm, and that's
// home. Everything else is measured from that stretched-open position. :3

// Static trampoline — FreeRTOS needs a plain C function pointer, so we bounce
// through this into the member function. The `this` pointer rides in as param.
void Ai57AIMServoDriver::_homingTaskImpl(void* param) {
    static_cast<Ai57AIMServoDriver*>(param)->_homingTask();
}

// The actual homing procedure — runs entirely inside its own task on Core 1.
// Blocks with vTaskDelay() between polls so the scheduler stays happy.
//
// DIRECTION CONVENTION (critical for a 180W servo — get this wrong and it
// rams the frame at full speed, which is a very bad time):
//
//   POSITIVE steps = toward the endstop (motor end / rear of machine)
//   NEGATIVE steps = away from endstop  (front / extended position)
//
// The sweep uses move(+sweep_steps) to drive toward the endstop.
// The backoff uses move(-backoff_steps) to pull away from it.
// If the carriage moves the WRONG way on sweep, flip AIM_PIN_DIR in
// config_api.h or invert the setDirectionPin() bool in init(). :3
void Ai57AIMServoDriver::_homingTask() {
    // Dump full endstop state at entry so we can see it in the web log.
    // This is the first thing we check — if it's already LOW at boot,
    // the optocoupler may be wired normally-closed or the pin is floating. :3
    int endstop_now = digitalRead(AIM_PIN_ENDSTOP);
    MLOGF("57AIMServo Homing: START — endstop GPIO%d = %d (active=%d = LOW)\n",
          AIM_PIN_ENDSTOP, endstop_now, ENDSTOP_ACTIVE_STATE);
    MLOGF("57AIMServo Homing: speed=%u steps/s (%.1f mm/s)\n",
          (uint32_t)_home_speed_steps_s,
          (float)_home_speed_steps_s / AIM_STEPS_PER_MM);

    // Crawl speed for the entire homing sequence — same speed for sweep AND
    // backoff. 180W servo is not a toy; we keep it slow until we trust it. :3
    _stepper->setSpeedInHz((uint32_t)_home_speed_steps_s);
    // High accel so the ramp is negligible at crawl speed — effectively
    // constant velocity from the first step. :3
    _stepper->setAcceleration(10000);

    // If already on the endstop at boot, back off first then re-sweep.
    // Use move() (relative) so we don't need a known position. :3
    if (digitalRead(AIM_PIN_ENDSTOP) == ENDSTOP_ACTIVE_STATE) {
        MLOGLN(F("57AIMServo Homing: Already at endstop — backing off 20mm before sweep"));
        // Negative = away from endstop (toward front of machine). :3
        _stepper->move(-(int32_t)mmToNative(AIM_HOMING_BACKOFF_MM * 2.0f));
        while (_stepper->isRunning()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        MLOGF("57AIMServo Homing: pre-backoff done, endstop now=%d\n",
              digitalRead(AIM_PIN_ENDSTOP));
    }

    // Sweep toward the endstop. Positive steps = toward endstop (rear).
    // 1.5× travel gives plenty of margin — the endstop poll stops us before
    // we run out of steps. If the motor runs the full sweep without triggering,
    // homing fails and we log it. :3
    int32_t sweep_steps = (int32_t)(AIM_MAX_TRAVEL_MM * AIM_STEPS_PER_MM * 1.5f);
    _stepper->move(sweep_steps);

    MLOGF("57AIMServo Homing: Sweeping +%d steps toward endstop at %u steps/s\n",
          sweep_steps, (uint32_t)_home_speed_steps_s);

    // Poll every 20ms. Log the endstop state every 500ms so we can watch it
    // change in the web log without flooding it. :3
    uint32_t last_log_ms = 0;
    while (_stepper->isRunning()) {
        int es = digitalRead(AIM_PIN_ENDSTOP);

        // Verbose periodic log — shows endstop state + step position every 500ms
        uint32_t now_ms = millis();
        if (now_ms - last_log_ms >= 500) {
            MLOGF("57AIMServo Homing: polling... endstop=%d pos=%d\n",
                  es, _stepper->getCurrentPosition());
            last_log_ms = now_ms;
        }

        if (es == ENDSTOP_ACTIVE_STATE) {
            // Endstop triggered — STOP IMMEDIATELY. forceStopAndNewPosition()
            // atomically halts the pulse train AND sets the position counter.
            // This is the only safe stop for a 180W servo — no coasting. :3
            _stepper->forceStopAndNewPosition(0);
            MLOGF("57AIMServo Homing: *** ENDSTOP HIT *** pos zeroed, running=%d\n",
                  _stepper->isRunning());

            // Wait for the drive to fully settle before issuing the backoff.
            // FAS ignores moveTo() if the queue is still busy. :3
            uint32_t stop_to = millis() + 500;
            while (_stepper->isRunning() && millis() < stop_to) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }

            // Back off at the same crawl speed — no sudden fast moves on a
            // 180W servo. Negative steps = away from endstop (toward front).
            // We use move() (relative) so the direction is unambiguous. :3
            _stepper->setSpeedInHz((uint32_t)_home_speed_steps_s);
            _stepper->setAcceleration(10000);
            int32_t backoff_steps = (int32_t)mmToNative(AIM_HOMING_BACKOFF_MM);
            _stepper->move(-backoff_steps);  // negative = away from endstop

            MLOGF("57AIMServo Homing: backing off %d steps (%.1f mm) at %u steps/s\n",
                  backoff_steps, AIM_HOMING_BACKOFF_MM, (uint32_t)_home_speed_steps_s);

            uint32_t timeout = millis() + 30000;  // 30s timeout at crawl speed
            while (_stepper->isRunning() && millis() < timeout) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            MLOGF("57AIMServo Homing: backoff done, pos=%d endstop=%d\n",
                  _stepper->getCurrentPosition(), digitalRead(AIM_PIN_ENDSTOP));

            if (digitalRead(AIM_PIN_ENDSTOP) == ENDSTOP_ACTIVE_STATE) {
                MLOGLN(F("WARNING: Endstop still active after backoff! Check wiring/direction."));
            } else {
                MLOGLN(F("57AIMServo Homing: Endstop released OK"));
            }

            // Re-zero at this backed-off position — this is home (0mm).
            // The carriage is now AIM_HOMING_BACKOFF_MM (10mm) away from the
            // endstop. All subsequent moves are measured from here. :3
            _stepper->forceStopAndNewPosition(0);
            _current_position_mm = 0.0f;
            _homed  = true;
            _homing = false;

            MLOGLN(F("57AIMServo Homing: Complete — homed at 0mm, ready :3"));
            _homingTaskHandle = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Motor stopped without hitting the endstop — homing failed.
    // Log the final endstop state so we know if it was a wiring issue or a
    // travel issue. The carriage is at an unknown position. uhoh. :3
    MLOGF("57AIMServo Homing: FAILED — motor stopped before endstop. endstop=%d\n",
          digitalRead(AIM_PIN_ENDSTOP));
    MLOGLN(F("  Check: 1) direction (does carriage move toward endstop?)"));
    MLOGLN(F("         2) endstop wiring (GPIO12 should read HIGH when open)"));
    MLOGLN(F("         3) travel distance (AIM_MAX_TRAVEL_MM large enough?)"));
    _homing = false;
    _homed  = false;

    _homingTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool Ai57AIMServoDriver::home(int32_t home_speed_steps_s) {
    if (_homing) return false;

    MLOGLN(F("57AIMServo Homing: Starting..."));
    _homing = true;
    _homed  = false;
    // Default to AIM_HOMING_SPEED_STEPS_S (500 steps/s = 25 mm/s) — safe and
    // controlled. The old 4000 default was inherited from the TMC build where
    // 80 steps/mm made it 50 mm/s. At 20 steps/mm, 4000 = 200 mm/s — the
    // carriage would slam into the endstop like a freight train. uhoh. :3
    _home_speed_steps_s = (home_speed_steps_s > 0) ? home_speed_steps_s : AIM_HOMING_SPEED_STEPS_S;

    // Drop any leftover stream/target state — stale Intiface targets fight the
    // homing sweep and cause the motor to bang the endstop endlessly without
    // ever finishing. Nobody wants a partner who can't settle down. :3
    resetStreamState();

    // Enable outputs via FAS before spawning the task — FAS needs
    // _outputEnabled = true before move() will execute. :3
    if (_stepper) {
        _stepper->enableOutputs();
        _enabled = true;
    }

    // Spawn the self-contained homing task on Core 1 (same core as motorTask
    // and the FAS engine). Priority 20 matches StrokeEngine — high enough to
    // preempt normal motion but below the FAS ISR. The task deletes itself
    // when homing completes or fails. :3
    xTaskCreatePinnedToCore(
        _homingTaskImpl,        // static trampoline
        "AIMHoming",            // task name
        4096,                   // stack (bytes)
        this,                   // param = this pointer
        20,                     // priority — same as StrokeEngine
        &_homingTaskHandle,     // handle so we can kill it on E-stop
        1                       // Core 1 — same core as FAS engine
    );

    return false;  // homing is async — watch isHoming()/isHomed() for completion
}

// runHomingStep() is a no-op — homing now runs entirely inside its own task.
// The function is kept to satisfy the MotorDriver interface. :3
void Ai57AIMServoDriver::runHomingStep() {
    // Nothing to do here — the homing task owns the loop now.
    // motorTask watches _homing go false and syncs g_state. :3
}

// Push-to-home: let the user establish home by simply pushing the shaft into
// the endstop — no web UI needed, just good old-fashioned manual persuasion.
// The user shoves the carriage all the way in until the endstop triggers, we
// zero there, back off 10mm, and we're homed. Consent is important — the
// machine waits for the user to push it in before it takes over. :3
bool Ai57AIMServoDriver::checkPushToHome() {
    if (_homed || _homing || !_stepper) return false;

    static uint32_t active_since = 0;
    uint32_t now = millis();

    if (digitalRead(AIM_PIN_ENDSTOP) == ENDSTOP_ACTIVE_STATE) {
        // Endstop pressed — start/continue debounce window.
        if (active_since == 0) active_since = now;

        // Require it held ~50ms to avoid noise/bounce false-homing.
        // Nobody likes a premature homing. :3
        if (now - active_since >= 50) {
            active_since = 0;

            // Enable outputs so FAS can drive the backoff move. :3
            enable();

            // Zero at the pressed (endstop) position first so the backoff
            // move is measured from here.
            _stepper->forceStopAndNewPosition(0);

            // Back off AIM_HOMING_BACKOFF_MM (10mm) away from the endstop
            // so the carriage doesn't sit on the switch. Negative steps =
            // away from endstop toward the front. Pull out just the tip. :3
            _stepper->setSpeedInHz(2000);
            _stepper->setAcceleration(50000);
            int32_t backoff_target = -mmToNative(AIM_HOMING_BACKOFF_MM);
            _stepper->moveTo(backoff_target);

            // Wait for the backoff move to finish.
            uint32_t timeout = millis() + 5000;
            while (_stepper->isRunning() && millis() < timeout) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }

            // Re-zero at this backed-off position = home (0mm). :3
            _stepper->forceStopAndNewPosition(0);
            _current_position_mm = 0.0f;
            _homed = true;

            MLOGLN(F("57AIMServo Push-to-home: endstop pressed — backed off 10mm, HOMED at 0"));
            return true;
        }
    } else {
        // Released before debounce elapsed — reset.
        active_since = 0;
    }
    return false;
}

// ---- Motion ------------------------------------------------------------------

void Ai57AIMServoDriver::moveTo(float pos_mm) {
    if (!_homed) {
        MLOGLN(F("57AIMServo: Cannot move — not homed!"));
        return;
    }

    enable();

    // Clamp to physical limits — we don't let the carriage go past the end of
    // the rail. The machine has limits. Even the greediest hole has a bottom. :3
    pos_mm = constrain(pos_mm, 0.0f, AIM_MAX_TRAVEL_MM);

    // Coordinate system: home (endstop) = 0mm = step 0
    // "Out" (extended/front) = positive mm = NEGATIVE steps
    int32_t target_steps = -mmToNative(pos_mm);

    if (!_stepper) return;

    uint32_t speed_hz = (uint32_t)(_max_speed_mm_s * AIM_STEPS_PER_MM);
    if (speed_hz < 1) speed_hz = 1;
    uint32_t accel_hz = (uint32_t)(_accel_mm_s2 * AIM_STEPS_PER_MM);
    if (accel_hz < 100) accel_hz = 100;

    _stepper->setSpeedInHz(speed_hz);
    _stepper->setAcceleration(accel_hz);

    int32_t pos_before = _stepper->getCurrentPosition();

    if (target_steps == pos_before) {
        MLOGF("57AIMServo moveTo: %.1fmm already at target step %d\n",
              pos_mm, target_steps);
        return;
    }

    // If a previous move is still draining the queue, force-stop and re-sync
    // position first — otherwise FastAccelStepper ignores the new moveTo().
    // Pull out, reposition, then push back in. :3
    if (_stepper->isRunning()) {
        _stepper->forceStopAndNewPosition(pos_before);
        uint32_t to = millis() + 300;
        while (_stepper->isRunning() && millis() < to) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    // FAS 0.34.0+ returns MoveResultCode (an enum) instead of int8_t.
    // Cast to int for the log — the numeric value is identical. :3
    int mret = (int)_stepper->moveTo(target_steps);

    MLOGF("57AIMServo moveTo: %.1fmm -> step %d (from %d) at %u Hz ret=%d\n",
          pos_mm, target_steps, pos_before, speed_hz, mret);
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
// shaft should be, so we ALWAYS retarget — we never drop a waypoint.
//
// The shaft keeps thrusting through every command, relentless and full,
// stuffed all the way in until the belly bulges and it can't take anymore.
// No stopping. No hesitation. Just continuous, rhythmic pounding. yippie! :3
//
// RAISE-ONLY ACCELERATION: once we're moving, never SOFTEN the acceleration.
// FAS computes its braking distance from the current accel; lowering it
// mid-flight means FAS suddenly needs a longer brake ramp than the distance
// that's left, so it overshoots and lurches. Stay firm, never go limp. :3
void Ai57AIMServoDriver::streamTo(float pos_mm, float speed_mm_s) {
    if (!_homed || !_stepper) return;
    enable();

    pos_mm = constrain(pos_mm, 0.0f, AIM_MAX_TRAVEL_MM);
    int32_t target_steps = -mmToNative(pos_mm);  // front = negative steps

    // Arm the stall watchdog with this REAL commanded sample. Speed 0 means
    // "settle" and is re-issued BY the watchdog itself — don't let that re-arm
    // the timer or we'd never time out. :3
    if (speed_mm_s > 0.0f) {
        _last_sample_mm   = pos_mm;
        _last_sample_ms   = millis();
        _have_last_sample = true;
    }

    // Track direction for telemetry / future tuning. We NO LONGER drop
    // reversals — every sample retargets. Dropping reversals is what collapsed
    // the range. FAS handles mid-flight reversals cleanly. :3
    int32_t cur_steps = _stepper->getCurrentPosition();
    int32_t delta     = target_steps - cur_steps;
    int8_t  new_dir   = (delta > 0) ? 1 : (delta < 0) ? -1 : 0;

    // Speed / acceleration — always command the full configured accel.
    // The old raise-only guard was locking in stale accel values from previous
    // segments and preventing the configured accel from ever taking effect.
    // FAS handles mid-flight retargets cleanly at any accel — it re-plans
    // from current velocity. :3
    float spd = (speed_mm_s > 0.0f) ? speed_mm_s : _max_speed_mm_s;
    spd = constrain(spd, 1.0f, _max_speed_mm_s);

    uint32_t speed_hz = (uint32_t)(spd * AIM_STEPS_PER_MM);
    if (speed_hz < 1) speed_hz = 1;
    uint32_t accel_hz = (uint32_t)(_accel_mm_s2 * AIM_STEPS_PER_MM);
    if (accel_hz < 100) accel_hz = 100;

    _stepper->setSpeedInHz(speed_hz);
    _stepper->setAcceleration(accel_hz);

    // No force-stop, no busy-wait: just retarget. If the new target equals the
    // current commanded target FastAccelStepper ignores it cheaply. The shaft
    // just keeps going, adjusting course without missing a beat. :3
    _stepper->moveTo(target_steps);

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
// here, just arm the watchdog and fire straight to FAS.
//
// This is the clean path: the consumer owns all the kinematic math, this
// function owns the hardware dispatch. Single responsibility, no cross-core
// touching. The shaft gets told exactly what to do and does it. Obedient,
// precise, and absolutely relentless. Like a good hole that takes every
// command without question and holds position until the next one. :3
void Ai57AIMServoDriver::streamToSteps(int32_t target_steps,
                                        uint32_t speed_steps_s,
                                        uint32_t accel_steps_s2) {
    if (!_homed || !_stepper) return;
    enable();

    // Arm the stall watchdog — convert target_steps back to mm for the
    // existing watchdog logic (which works in mm). If the host goes quiet,
    // update() will settle here. :3
    float pos_mm = nativeToMm(-target_steps);  // negative because front=negative steps
    _last_sample_mm   = pos_mm;
    _last_sample_ms   = millis();
    _have_last_sample = true;

    // GRIT FIX: only call setAcceleration()/setSpeedInHz() when the values
    // actually change. Calling them on every waypoint (30–100x/sec) forces FAS
    // to recalculate its ramp on every single call — even mid-flight. That
    // recalc is the source of the gritty/stuttery feel: the motor gets a new
    // ramp profile injected into it 100 times a second whether it needs one or
    // not. Cache starts at 0 so the first call always goes through. :3
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

void Ai57AIMServoDriver::runMotorStep() {
    // NOP — moveTo()/streamTo()/streamToSteps() retarget through FAS directly.
    // Stream stall watchdog (streamTo() stashing) lives in update().
    // This stub exists to satisfy the MotorDriver interface. :3
}

// ---- Speed & Acceleration ----------------------------------------------------

void Ai57AIMServoDriver::setMaxSpeed(float speed_mm_s) {
    _max_speed_mm_s = constrain(speed_mm_s, 0.0f, MAX_SPEED_MM_S);
}

void Ai57AIMServoDriver::setAcceleration(float accel_mm_s2) {
    // Ceiling at 20000 mm/s² — well within what the 57AIM30 can handle for
    // short bursts. The planner uses this as the cruise accel; the raise-only
    // guard in streamToSteps() keeps it from softening mid-flight. :3
    _accel_mm_s2 = constrain(accel_mm_s2, 10.0f, 20000.0f);

    if (_stepper) {
        _stepper->setAcceleration((int32_t)(_accel_mm_s2 * AIM_STEPS_PER_MM));
    }
}

uint32_t Ai57AIMServoDriver::getLiveAcceleration() const {
    // Return the acceleration currently active inside the FAS ramp engine —
    // NOT the configured ceiling. This is what OSSM reads with
    // stepper->getAcceleration() in its raise-only guard. The full
    // FastAccelStepper header is available here (included at the top of this
    // .cpp), so the call resolves correctly. :3
    return _stepper ? (uint32_t)_stepper->getAcceleration() : 0u;
}

// ---- Status ------------------------------------------------------------------

bool Ai57AIMServoDriver::isMoving() {
    return _stepper ? _stepper->isRunning() : false;
}

float Ai57AIMServoDriver::getPosition() const {
    if (!_stepper) return 0.0f;
    // Steps are negative for positive mm positions (endstop=0, front=negative steps)
    return nativeToMm(-_stepper->getCurrentPosition());
}

float Ai57AIMServoDriver::getTargetPosition() const {
    return getPosition();
}

// ---- Stop / HardStop ---------------------------------------------------------

void Ai57AIMServoDriver::stop() {
    // E-stop: halt the pulse train, kill the homing task if it's running,
    // disable outputs via FAS, and clear all state. The shaft goes completely
    // soft — no ambiguity, no stale flags. The scene is over. :3

    // Kill the homing task if it's mid-sweep — we're pulling out NOW. :3
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
    // Full E-stop means we're no longer at a known position — the carriage
    // might've moved while the motor was limp, or someone might've shoved it
    // around. Clear _homed so the driver's internal state matches g_state.homed.
    // A pulled plug means nobody knows where the tip is anymore. :3
    _homed = false;
    // Drop stale stream/target state so the next Home/move starts clean. :3
    resetStreamState();
}

void Ai57AIMServoDriver::hardStop() {
    // Immediate stop without deceleration — the pulse train cuts off NOW.
    // The 57AIM30 will decelerate on its own internal ramp, but we stop
    // commanding it immediately. Clear the blend/stream state too, otherwise
    // the stall watchdog keeps humping the last target right after the stop.
    // Nobody needs a jealous ex-target clinging on after the scene's over. :3
    if (_stepper) {
        _stepper->forceStop();
    }
    resetStreamState();
}

// ---- Driver config -----------------------------------------------------------

void Ai57AIMServoDriver::applyDriverConfig(const DriverConfig& cfg) {
    // The 57AIM30 is configured via its own front-panel DIP switches and
    // parameter software (RS485 Modbus, future feature). There are no SPI
    // registers to write from here. We accept the struct so the rest of the
    // system (ConfigStore, WebUI) doesn't need to know we're a dumb drive.
    //
    // Speed and acceleration from the config ARE applied — those go to FAS,
    // not to the drive itself. The drive just follows the pulse rate. :3
    (void)cfg;  // suppress unused-parameter warning
    MLOGLN(F("57AIMServo: applyDriverConfig() — dumb drive, no registers to write"));
}

// ---- Unit conversion ---------------------------------------------------------

int32_t Ai57AIMServoDriver::mmToNative(float mm) const {
    // Convert mm to steps using the 57AIM30's geometry.
    // AIM_STEPS_PER_MM = 20.0 (1600 steps/rev ÷ 80mm/rev).
    // The result is a step count — positive = toward endstop. :3
    return (int32_t)(mm * AIM_STEPS_PER_MM);
}

float Ai57AIMServoDriver::nativeToMm(int32_t native) const {
    // Convert steps back to mm. Inverse of mmToNative(). :3
    return (float)native / AIM_STEPS_PER_MM;
}

#endif // defined(DRIVER_57AIM_SERVO)
