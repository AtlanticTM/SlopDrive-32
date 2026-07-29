// MotionArbiter — sole caller of MotorDriver for positioning; arbitrates
// MANUAL / PATTERN / TCODE_STREAM sources into one dispatched plan per call.
// Constraints:
//   submit(), submitStreamSample(), and processDeferred() run on Core 1 only;
//   submitDeferred() is the Core 0 -> Core 1 handoff (FreeRTOS queue) — never
//   call submit() directly from Core 0.
//   E-stop and hard step bounds are absolute: no source, including MANUAL,
//   may bypass the physical envelope. MANUAL bypasses only the stroke WINDOW.
//   Never write actual_position_mm from here — WebUI's 240Hz telemetry
//   sampler owns that atomic (see the callouts at each dispatch site).
#include "MotionArbiter.h"
#include "range_mapper.h"
#include "sloplog/sloplog.h"
#include "config_api.h"
#include <math.h>              // for fabsf, fminf, sqrtf

// ---- Planning assumption: v0 = 0 --------------------------------------------
// _planAndDispatch's trapezoid math always assumes the carriage starts each
// plan at rest (v0 = 0), never the true in-flight velocity.
// FAS's getCurrentSpeed() (vendored FastAccelStepper 0.34.x) returns the
// last SET speedInHz, not instantaneous velocity, and the vendored header
// has no getCurrentSpeedInMilliHz() equivalent — true v0 is not observable
// through this driver's API.
// Safe because FAS retargets velocity-continuously from its own internal
// state regardless of the v0 fed to the trapezoid math, so v0=0 only
// affects the DERIVED accel, never the actual motion: it overestimates the
// accel needed to hit the deadline (worst case), which the ceiling clamp
// then brings down. The error shrinks as retarget rate rises — at >100Hz
// the gap between true and assumed v0 is at most one 10ms segment's worth
// of velocity, ≤1% of the machine's speed range.

MotionArbiter::MotionArbiter(SystemState& state, RangeMapper& mapper, MotorDriver& motor)
    : _state(state), _mapper(mapper), _motor(motor)
{}

void MotionArbiter::init() {
    _defer_queue = xQueueCreate(DEFER_QUEUE_DEPTH, sizeof(MotionIntent));
    configASSERT(_defer_queue != nullptr);
    SLOGI("arbiter", "MotionArbiter: initialized — D4, %u-slot defer queue active", DEFER_QUEUE_DEPTH);
}

// ---- submitDeferred ---------------------------------------------------------
// Core 0 -> Core 1 handoff: DEFER_QUEUE_DEPTH-slot FreeRTOS queue, drained in
// full by processDeferred() every Core-1 tick.

void MotionArbiter::submitDeferred(const MotionIntent& intent) {
    // Non-blocking push — drops if full, correct for retarget semantics.
    if (xQueueSend(_defer_queue, &intent, 0) != pdTRUE) {
        SLOGW_EVERY_MS(2000, "arbiter",
                       "MotionArbiter DROP: defer queue full — intent dropped. "
                       "Core 1 consumer may be stalled or overloaded.");
    }
}

// ---- processDeferred --------------------------------------------------------
// Consumed by the Core 1 task.

void MotionArbiter::processDeferred() {
    // Drain the entire queue each tick. Each intent is planned from live FAS
    // state — sequential retargets are the normal D4 operating mode.
    MotionIntent intent;
    uint8_t drained = 0;
    uint32_t max_plan_us = 0;
    while (xQueueReceive(_defer_queue, &intent, 0) == pdTRUE) {
        PlanReport rpt = submit(intent);
        if (rpt.plan_us > max_plan_us) max_plan_us = rpt.plan_us;
        drained++;
    }
    // Log only a new depth watermark: a rising peak means the queue got
    // deeper than ever before, i.e. Core 1 started falling behind. total/
    // homed are already live in telemetry, so they add nothing here.
    static uint8_t peak_drain = 0;  // highest drain seen in any tick
    if (drained > peak_drain) {
        peak_drain = drained;
        SLOGD("arbiter", "MotionArbiter: new defer-drain peak %u/tick (total=%lu)",
              (unsigned)peak_drain, _intent_count);
    }
}

// ---- submitStreamSample -----------------------------------------------------
// Core 1 fast path for the streamSamplerTask sampler. NOT the trapezoid
// planner: streamSamplerTask's slopmotion::Engine (docs/canon doctrine
// §SlopMotion) has already shaped the curve — start/end position, velocity,
// curvature. Every ~1ms it hands us the sampled point on that curve and we
// feed it straight to FAS. The only work here is safety + unit conversion:
//   1. Gates: estop / homed / paused / manual_override (stream honors all).
//   2. Map normalized 0..1 into the configured stroke window (mm).
//   3. Hard physical step bounds — the machine envelope, never bypassed.
//   4. Speed feed by mode; accel is the constant input ceiling.
//
// Constant accel + (in ceiling-pegged mode) constant speed matters because
// the 57AIM streamToSteps() grit-cache only re-plans the FAS ramp when speed
// or accel change. Keeping them steady means each 1ms micro-target is a
// cheap moveTo() with no ramp recompute; the curve's velocity is reproduced
// by the position deltas between micro-targets, not by re-ramping FAS.
bool MotionArbiter::submitStreamSample(float norm_pos, float norm_vel_per_s) {
    // ---- Safety gates (a stream sample honors every gate) -------------------
    if (_state.estop_requested.load(std::memory_order_relaxed)) return false;
    if (!_state.homed)          return false;
    if (_state.paused)          return false;
    if (_state.manual_override) return false;

    // ---- Map normalized position into the stroke window ---------------------
    if (norm_pos < 0.0f) norm_pos = 0.0f;
    if (norm_pos > 1.0f) norm_pos = 1.0f;
    float target_mm = _mapper.intensityToPosition(norm_pos);
    target_mm = _clampToWindow(target_mm, MotionSource::TCODE_STREAM);

    int32_t target_steps = -_motor.mmToNative(target_mm);

    // ---- HARD STEP BOUNDS — machine envelope, always enforced ---------------
    // Ceiling is the effective physical bound: measured stroke once homed, else
    // the configured max rail length (rail-length agnostic).
    int32_t hard_min_steps = -_motor.mmToNative(_motor.effectiveCeilingMm());
    int32_t hard_max_steps = 0;
    target_steps = constrain(target_steps, hard_min_steps, hard_max_steps);

    // ---- Window-entry gentleness (honor USER limits on the way in) ----------
    // If the carriage is currently OUTSIDE the stroke window, the sample that
    // carries it in honors the gentle USER limits instead of the input ceiling
    // — the same "glide, don't lunge" rule as the trapezoid planner. Once the
    // carriage is inside the window, the normal INPUT set resumes.
    bool entering = _isOutsideWindow(_motor.getPosition());

    // ---- Speed ceiling (with safe-approach soft-start) ----------------------
    float speed_ceiling = entering ? fminf(_input_speed_limit_mm_s, _user_speed_limit_mm_s)
                                   : _input_speed_limit_mm_s;
    {
        uint32_t now_ms = millis();
        float safe_cap = _state.safeSpeedCap(speed_ceiling, now_ms);
        if (safe_cap < speed_ceiling) speed_ceiling = safe_cap;
    }
    float accel_ceiling = entering ? fminf(_input_accel_limit_mm_s2, _user_accel_limit_mm_s2)
                                   : _input_accel_limit_mm_s2;

    // Safe-approach floor, capped at the active ceiling so a gentle window-entry
    // ceiling (USER limit, possibly < SAFE_APPROACH_SPEED_MM_S) isn't overridden.
    float speed_floor = fminf(SAFE_APPROACH_SPEED_MM_S, speed_ceiling);

    // ---- Speed feed — mode dependent ----------------------------------------
    float speed_mm_s;
    if (_state.stream_speed_mode == SystemState::SPEED_VELOCITY_MATCHED) {
        // Convert the interpolator's normalized units/second into mm/s across
        // the window span, so FAS coasts the cubic's exact instantaneous speed.
        float span_mm = _mapper.getMaxMm() - _mapper.getMinMm();
        speed_mm_s = fabsf(norm_vel_per_s) * span_mm;
        if (speed_mm_s > speed_ceiling)  speed_mm_s = speed_ceiling;
        if (speed_mm_s < speed_floor)    speed_mm_s = speed_floor;
    } else {
        // Ceiling-pegged: constant speed; the 1ms micro-target position deltas
        // themselves shape velocity, keeping the grit-cache quiet.
        speed_mm_s = speed_ceiling;
        if (speed_mm_s < speed_floor)    speed_mm_s = speed_floor;
    }

    // Driver-owned native scale (Phase 2 fix): nativePerMm() replaces the
    // hardcoded AIM_STEPS_PER_MM here. For the FAS driver nativePerMm()
    // resolves to exactly the same value AIM_STEPS_PER_MM does (both derive
    // from aimStepsPerMm()), so FAS behavior is numerically identical to
    // before. A counts-native driver (Modbus, ~834/mm) now gets its own
    // correct scale instead of FAS's ~20/mm — the wrong constant would have
    // under/overstepped every Modbus move ~41x; fixed before any motion exists.
    uint32_t speed_steps_s  = (uint32_t)(speed_mm_s   * _motor.nativePerMm());
    uint32_t accel_steps_s2 = (uint32_t)(accel_ceiling * _motor.nativePerMm());
    if (speed_steps_s < 1)   speed_steps_s  = 1;
    if (accel_steps_s2 < 10) accel_steps_s2 = 10;

    // ---- Stream-lag diagnostic ----------------------------------------------
    // The interpolator glides open-loop: if the curve's instantaneous velocity
    // exceeds the input ceiling, the motor saturates and falls behind the
    // commanded curve — fast endpoints clip and any catch-up is a full-ceiling
    // lunge (felt as drift while streaming). Normal chase lag is ~1-2mm; a
    // sustained gap beyond that means the content is outrunning the machine.
    // Surface it instead of drifting silently.
    {
        float lag_mm = fabsf(target_mm - _motor.getPosition());
        if (lag_mm > 8.0f) {
            SLOGW_EVERY_MS(2000, "arbiter",
                           "Stream LAG: carriage %.1fmm behind commanded curve — content "
                           "demands more than input max speed (%.0f mm/s); endpoints will clip",
                           lag_mm, _input_speed_limit_mm_s);
        }
    }

    // ---- Dispatch to FAS ----------------------------------------------------
    // Lockless, matching _planAndDispatch: all motor callers are Core-1 tasks
    // and streamToSteps() owns its own grit-cache statics. During active
    // streaming the sampler is the primary caller; pattern is gated off.
    _motor.streamToSteps(target_steps, speed_steps_s, accel_steps_s2);

    // ---- Telemetry ----------------------------------------------------------
    _state.commanded_target_mm = target_mm;
    return true;
}

// ---- submit -----------------------------------------------------------------
// Core 1 direct entry point. Plans + dispatches.

PlanReport MotionArbiter::submit(const MotionIntent& intent) {
    if (!_state.homed && intent.source != MotionSource::MANUAL) {
        // Not homed and not a manual override — drop, but leave a trace.
        // Manual moves bypass homed check (push-to-home scenario).
        _rejected_count = _rejected_count + 1;
        SLOGW_EVERY_MS(2000, "arbiter", "MotionArbiter REJECT: not-homed (src=%u)",
                       (unsigned)intent.source);
        PlanReport rpt = {};
        return rpt;
    }

    // E-stop check — absolute gate, no source bypass. If the red button
    // was slapped, nothing moves. Period.
    if (_state.estop_requested.load(std::memory_order_relaxed)) {
        _rejected_count = _rejected_count + 1;
        SLOGW_EVERY_MS(2000, "arbiter", "MotionArbiter REJECT: e-stop (src=%u)",
                       (unsigned)intent.source);
        PlanReport rpt = {};
        return rpt;
    }

    // Evaluate per-source gates (paused, manual_override, window, etc.)
    if (!_gatesPass(intent)) {
        _rejected_count = _rejected_count + 1;
        SLOGW_EVERY_MS(2000, "arbiter",
                       "MotionArbiter REJECT: gates (paused/override/intiface-recency) (src=%u)",
                       (unsigned)intent.source);
        PlanReport rpt = {};
        rpt.deadline_feasible = false;
        return rpt;
    }

    // Plan + dispatch under spinlock — microcritical section
    PlanReport report = _planAndDispatch(intent, false);

    // Update telemetry atomically
    portENTER_CRITICAL(&_telemetry_mux);
    _last_report = report;
    _intent_count++;
    portEXIT_CRITICAL(&_telemetry_mux);

    // Publish to SystemState for the WebUI position graph
    _state.commanded_target_mm = intent.target_mm;
    // actual_position_mm is NOT ours to write — not here, not in
    // _planAndDispatch. WebUI's 240Hz telemetry sampler owns that atomic and
    // fills it from _motor.getPosition() (FAS step-counter truth). The
    // arbiter publishes INTENT only.

    return report;
}

// ---- Gate evaluation --------------------------------------------------------
// Centralized; encodes existing per-source semantics.

bool MotionArbiter::_gatesPass(const MotionIntent& intent) {
    // MANUAL always wins — bypasses all source gates
    if (intent.source == MotionSource::MANUAL) {
        return true;
    }

    // Homed guard — stream/pattern/OSSM require the machine to know where it is
    if (!_state.homed) return false;

    // Pause gate — when paused, only MANUAL can move
    if (_state.paused) return false;

    // Manual override — user has the reins via the WebUI; external sources yield
    if (_state.manual_override) return false;

    // Intiface recency gate — same as PatternEngine's current logic:
    // If Intiface has been active within 250ms AND the user hasn't seized
    // control (paused/override), Intiface stream commands take priority over
    // PatternEngine.
    if (intent.source == MotionSource::PATTERN) {
        // Yield on stream MOTION, not packets (keep-alive-only hosts must not
        // pin the pattern off) — same rule as PatternEngine's emit gate and
        // streamSamplerTask's reclaim.
        bool intiface_driving = (_state.last_intiface_move_ms != 0) &&
                                (millis() - _state.last_intiface_move_ms < 1500);
        if (intiface_driving) return false;
    }

    return true;
}

// ---- Window clamping --------------------------------------------------------
// MANUAL bypasses; every other source is window-bound.

float MotionArbiter::_clampToWindow(float mm, MotionSource source) {
    if (source == MotionSource::MANUAL) {
        // Manual moves can go anywhere 0 .. the effective physical ceiling
        // (measured stroke once homed, else the configured max rail length).
        return constrain(mm, 0.0f, _motor.effectiveCeilingMm());
    }
    // Stream/pattern/OSSM moves are clamped to the user's configured range window
    float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
    return constrain(mm, lo, hi);
}

// ---- Window-entry detection -------------------------------------------------
// Is the carriage currently outside the stroke window?

bool MotionArbiter::_isOutsideWindow(float p0_mm) const {
    const float eps = 0.5f;  // 0.5mm slack — sub-safety-zone, avoids edge chatter
    float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
    return (p0_mm < lo - eps) || (p0_mm > hi + eps);
}

// ---- _planAndDispatch -------------------------------------------------------
// The heart of D4; runs under _dispatch_mux on Core 1.
//
// Algorithm: trapezoid with initial velocity v0 = 0 (see the top-of-file
// rationale). Given distance d = |p1 - p0| and deadline T (seconds):
//
//   No deadline (T = 0): the "point move" case — plan at the source's
//   ceiling speed + accel, i.e. go there as fast as the user configured.
//
//   With deadline: derive the triangle-peak profile that is the minimum
//   needed to arrive on time.
//     derived_speed = d / T           (constant-velocity minimum)
//     derived_accel = 4 * d / T²      (triangle-peak accel, no cruise phase)
//   Apply the ramp multiplier: accel *= min(entryRamp, exitRamp).
//   Clamp speed and accel at the source's limit set; if the clamped accel
//   is below the derived accel, the deadline is infeasible and gets marked
//   late. Dispatch: set FAS speed + accel, call moveTo(target_steps).
//
//   A slow command plans gentle; a tight deadline plans fast but never
//   exceeds ceilings — the ceiling only activates when the command
//   genuinely demands more than is safe.
PlanReport MotionArbiter::_planAndDispatch(const MotionIntent& intent, bool /*locked*/) {
    uint32_t start_us = micros();
    PlanReport report = {};

    // ---- Select limit set by source -----------------------------------------
    float speed_ceiling, accel_ceiling;
    if (intent.source == MotionSource::MANUAL) {
        speed_ceiling = _user_speed_limit_mm_s;
        accel_ceiling = _user_accel_limit_mm_s2;
    } else {
        speed_ceiling = _input_speed_limit_mm_s;
        accel_ceiling = _input_accel_limit_mm_s2;
    }

    // ---- Apply safeSpeedCap soft-start --------------------------------------
    // On a new stream or after un-pause, the speed ceiling ramps up from
    // SAFE_APPROACH_SPEED_MM_S to the configured limit over
    // SAFE_RESUME_RAMP_MS. This prevents the first move from lunging.
    // Accel is NOT scaled — only speed. When slow commands arrive during
    // the ramp, the planner uses the full accel ceiling to reach whatever
    // speed is allowed, avoiding the "freeze then jump" where soft accel
    // can't even reach the already-soft speed cap.
    {
        uint32_t now_ms = millis();
        float safe_cap = _state.safeSpeedCap(speed_ceiling, now_ms);
        if (safe_cap < speed_ceiling) {
            speed_ceiling = safe_cap;
        }
    }

    // ---- Clamp target to window ---------------------------------------------
    float target_mm = _clampToWindow(intent.target_mm, intent.source);
    // Diagnostic: log when the intent was outside the window at submit time.
    // This tells us whether the TCode stream is producing targets outside the
    // configured window (RangeMapper issue) or the window changed mid-stream.
    if (intent.source != MotionSource::MANUAL && target_mm != intent.target_mm) {
        float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
        SLOGW_EVERY_MS(2000, "arbiter",
                       "MotionArbiter WINDOW CLAMP: intent=%.1f clamped to %.1f window=[%.1f,%.1f] src=%u",
                       intent.target_mm, target_mm, lo, hi, (unsigned)intent.source);
    }

    // ---- Read actual machine state ------------------------------------------
    // getCurrentPosition() returns int32_t native steps — open-loop commanded
    // position (no encoder). OSSM-correct.
    float p0_mm = _motor.getPosition();    // current position in mm (driver converts)
    int32_t p0_steps = -_motor.mmToNative(p0_mm);  // convert to our native step frame
    // NOTE: mmToNative returns positive for increase-toward-rear.
    // In our step frame: endstop = 0, front = negative. So target_steps = -mmToNative(mm).

    int32_t target_steps = -_motor.mmToNative(target_mm);
    float distance_mm = fabsf(target_mm - p0_mm);

    // ---- Window-entry gentleness (honor USER limits on the way in) ----------
    // A machine-driven source currently sitting OUTSIDE the stroke window is
    // about to be dragged to the clamped window edge. Doing that at the INPUT
    // ceiling is the "shoot to the window" lunge. Instead, cap this move at the
    // gentle USER limits so the carriage glides into the window; once it's
    // inside, subsequent intents fall back to the normal input set.
    if (intent.source != MotionSource::MANUAL && _isOutsideWindow(p0_mm)) {
        speed_ceiling = fminf(speed_ceiling, _user_speed_limit_mm_s);
        accel_ceiling = fminf(accel_ceiling, _user_accel_limit_mm_s2);
    }

    // ---- Always dispatch to FAS, even for zero-distance intents -------------
    // A zero-distance dispatch is internally a cheap no-op in FAS, and
    // skipping it would let FAS complete and stop between zero-distance
    // intents, so the next non-zero intent restarts from dead-stop (visible
    // freeze-jump on slow moves). Dispatching always is free; keep it.
    int32_t distance_steps = target_steps - p0_steps;

    // ---- Derive speed and acceleration from geometry ------------------------
    float derived_speed_mm_s = 0.0f;
    float derived_accel_mm_s2 = 0.0f;

    if (intent.deadline_ms == 0) {
        // No deadline — point move: plan AT the source's ceiling speed + accel.
        // This is the standard "go there" semantic: the ceiling IS the answer.
        derived_speed_mm_s  = speed_ceiling;
        derived_accel_mm_s2 = accel_ceiling;
    } else {
        // With deadline: derive from distance + time constraints.
        float T = intent.deadline_ms / 1000.0f;
        if (T < 0.001f) T = 0.001f;  // floor to avoid div/0

        // Single-symmetric trapezoid plan:
        // derived_accel = 4 * distance / T² (triangle minimum — the ACCELERATION
        // required for a purely triangular profile where accel+decel fill the
        // entire deadline with no cruise phase).
        // derived_speed = 2 * distance / T (triangle peak — the SPEED a triangle
        // profile reaches at its midpoint).
        //
        // If the speed hint (S-extension) is present, use it as the cruise speed
        // and derive accel from that instead:
        //   cruise_time = T - distance / speed_hint
        //   derived_accel = speed_hint / (cruise_time / 2) = 2*speed_hint / cruise_time
        // BUT this can produce a negative cruise_time if speed_hint is too slow.
        // Standard approach: use the larger of triangle-peak and speed_hint.
        float triangle_speed = 2.0f * distance_mm / T;
        float triangle_accel = 4.0f * distance_mm / (T * T);

        if (intent.speed_hint_mm_s > 0.0f && intent.accel_hint_mm_s2 > 0.0f) {
            // Advanced pattern mode: the pattern derived BOTH cruise speed and
            // accel from its own per-direction stroke geometry. Deadline+hint
            // alone cannot carry an independent accel demand (the T terms
            // cancel to a fixed 2v²/d), so the explicit hint is taken as the
            // derived dynamics. Ceilings still clamp below — never exceeded.
            derived_speed_mm_s  = intent.speed_hint_mm_s;
            derived_accel_mm_s2 = intent.accel_hint_mm_s2;
        } else if (intent.speed_hint_mm_s > 0.0f && intent.speed_hint_mm_s < triangle_speed) {
            // Speed hint is LOWER than triangle peak — use it as cruise speed,
            // which means there IS a cruise phase: longer time, lower accel.
            // This is a gentle trapezoid: accel to hint speed, cruise, decel.
            float cruise_time = T - (distance_mm / intent.speed_hint_mm_s);
            if (cruise_time > 0.0f) {
                // accel phase time = (T - cruise_time) / 2
                float accel_time = (T - cruise_time) / 2.0f;
                if (accel_time > 0.001f) {
                    derived_speed_mm_s  = intent.speed_hint_mm_s;
                    derived_accel_mm_s2 = derived_speed_mm_s / accel_time;
                } else {
                    derived_speed_mm_s  = triangle_speed;
                    derived_accel_mm_s2 = triangle_accel;
                }
            } else {
                // Speed hint too slow — fall back to triangle
                derived_speed_mm_s  = triangle_speed;
                derived_accel_mm_s2 = triangle_accel;
            }
        } else {
            // No speed hint, or hint is faster than the triangle peak (which
            // means there's no cruise phase — it's already the triangle limit).
            // Use the triangle profile.
            derived_speed_mm_s  = triangle_speed;
            derived_accel_mm_s2 = triangle_accel;
        }
    }

    // ---- Record derived values before clamp ---------------------------------
    report.derived_speed_mm_s  = derived_speed_mm_s;
    report.derived_accel_mm_s2 = derived_accel_mm_s2;

    // ---- Clamp at limit set -------------------------------------------------
    float clamped_speed = derived_speed_mm_s;
    float clamped_accel = derived_accel_mm_s2;

    bool deadline_late = false;
    if (clamped_speed > speed_ceiling) {
        clamped_speed = speed_ceiling;
        deadline_late = true;   // we can't go as fast as the deadline demands
    }
    if (clamped_accel > accel_ceiling) {
        clamped_accel = accel_ceiling;
        deadline_late = true;   // we can't accelerate as hard as needed
    }

    report.clamped_speed_mm_s  = clamped_speed;
    report.clamped_accel_mm_s2 = clamped_accel;
    report.deadline_feasible   = !deadline_late;
    report.deadline_late       = deadline_late;

    // ---- HARD STEP BOUNDS — last line of defense, ALWAYS enforced -----------
    // The machine's native step coordinate system:
    //   home (rear endstop)  = 0 steps
    //   front (max extended) = -mmToNative(effectiveCeilingMm()) steps
    // The ceiling is the measured stroke once homing has felt out the real wall,
    // else the configured max rail length (rail-length agnostic). No dispatch
    // may EVER target outside this absolute physical envelope, regardless of
    // source (MANUAL bypasses the WINDOW but NOT the machine).
    int32_t hard_min_steps = -_motor.mmToNative(_motor.effectiveCeilingMm());  // most negative allowed
    int32_t hard_max_steps = 0;                                            // home — cannot go past
    int32_t clamped_target = constrain(target_steps, hard_min_steps, hard_max_steps);
    if (clamped_target != target_steps) {
        // Diagnostic — this should never fire. If it does, the window clamp
        // or mm→step conversion has a bug. Log it so we can find it.
        SLOGW_EVERY_MS(2000, "arbiter",
                       "MotionArbiter HARD CLAMP: target_steps=%d clamped to [%d, %d] src=%u tgt_mm=%.1f",
                       target_steps, hard_min_steps, hard_max_steps,
                       (unsigned)intent.source, target_mm);
        target_steps = clamped_target;
        // Re-derive distance for the report — the effective distance is now
        // shorter than the intent requested.
        distance_mm = fabsf(_motor.nativeToMm(-target_steps) - _motor.nativeToMm(-p0_steps));
    }

    // ---- Dispatch to FAS ----------------------------------------------------
    // FAS moveTo() retargets velocity-continuously from current state.
    //
    // STALL PREVENTION: at near-zero distance (endpoint reversals, slow moves),
    // the triangle math produces speed≈0 which gets clamped to 1 step/s.
    // The grit-fix cache sees "changed from last (800→1)" and sets FAS to
    // minimum — motor decelerates to near-stop. Then the next real intent
    // restores full speed → visible freeze-jump. Fix: NEVER dispatch speed
    // below the safe-approach floor. FAS retargets smoothly from its current
    // velocity when we keep the speed/accel pegged at meaningful values.
    // Floor the dispatch speed at the safe-approach minimum — but never ABOVE
    // the active ceiling. During a window-entry glide the ceiling is the gentle
    // USER limit (e.g. 50 mm/s), which is below SAFE_APPROACH_SPEED_MM_S (100);
    // flooring at the raw constant there would undo the gentleness.
    float speed_floor = fminf(SAFE_APPROACH_SPEED_MM_S, speed_ceiling);
    if (clamped_speed < speed_floor && distance_mm > 0.01f) {
        clamped_speed = speed_floor;
    }

    // Driver-owned native scale (Phase 2 fix) — see the submitStreamSample
    // comment above for the full rationale; identical fix, second site.
    uint32_t speed_steps_s  = (uint32_t)(clamped_speed * _motor.nativePerMm());
    uint32_t accel_steps_s2 = (uint32_t)(clamped_accel * _motor.nativePerMm());

    if (speed_steps_s < 1)   speed_steps_s  = 1;
    if (accel_steps_s2 < 10) accel_steps_s2 = 10;

    // ---- Raise-only acceleration guard (D4, same as OSSM exact) -------------
    uint32_t final_accel = accel_steps_s2;
    if (_motor.isMoving()) {
        uint32_t live_accel = _motor.getLiveAcceleration();
        if (live_accel > final_accel) final_accel = live_accel;
    }

    // Single FAS dispatch — streamToSteps handles its own grit-fix caching
    // internally. DO NOT call _motor.setMaxSpeed / setAcceleration here —
    // those add a redundant FAS path that conflicts with streamToSteps'
    // internal statics and causes jitter at 333Hz.
    _motor.streamToSteps(target_steps, speed_steps_s, final_accel);

    report.dispatched_steps = target_steps;

    // D4: NEVER write actual_position_mm from the planner. WebUI's 240Hz
    // telemetry sampler (WebUI::telemetryTimerCb) is its writer, sourcing
    // _motor.getPosition() — FAS step-counter truth. A planner write would
    // publish INTENT as if it were reality, straight into the graph, the
    // SlopSync 0x0080 pos field, and the stream rising-edge re-seed.
    report.plan_us = micros() - start_us;
    return report;
}

// ---- Emergency / gate helpers -----------------------------------------------

void MotionArbiter::emergencyStop() {
    _motor.emergencyStop();
    // Drain the defer queue so no stale intent fires after E-stop
    MotionIntent dummy;
    while (xQueueReceive(_defer_queue, &dummy, 0) == pdTRUE) {}
}

void MotionArbiter::stopMotion() {
    // Full stop with power cut — the driver's stop() semantics (halts the pulse
    // train, kills any homing task, disables outputs, clears homed). Deliberately
    // DIFFERENT from hardStopMotion(), which halts but keeps the motor powered
    // and homed.
    _motor.stop();
}

void MotionArbiter::hardStopMotion() {
    _motor.hardStop();
}

void MotionArbiter::pause() {
    _state.paused = true;
}

void MotionArbiter::resume() {
    _state.paused = false;
    _state.resume_start_ms = millis();  // stamp for safeSpeedCap soft-start
}

// ---- Limit set setters ------------------------------------------------------

void MotionArbiter::setUserSpeedLimit(float mm_s) {
    _user_speed_limit_mm_s = constrain(mm_s, 1.0f, MAX_SPEED_MM_S);
}

void MotionArbiter::setUserAccelLimit(float mm_s2) {
    _user_accel_limit_mm_s2 = constrain(mm_s2, 10.0f, MAX_ACCEL_MM_S2);
}

void MotionArbiter::setInputSpeedLimit(float mm_s) {
    _input_speed_limit_mm_s = constrain(mm_s, 1.0f, MAX_SPEED_MM_S);
}

void MotionArbiter::setInputAccelLimit(float mm_s2) {
    _input_accel_limit_mm_s2 = constrain(mm_s2, 10.0f, MAX_ACCEL_MM_S2);
}

// ---- Telemetry --------------------------------------------------------------

PlanReport MotionArbiter::lastReport() const {
    PlanReport rpt;
    portENTER_CRITICAL(&_telemetry_mux);
    rpt = _last_report;
    portEXIT_CRITICAL(&_telemetry_mux);
    return rpt;
}

// ---- Blend/reversal policy --------------------------------------------------
// All modes alias to "allow".

void MotionArbiter::setBlendMode(uint8_t mode) {
    // Store the requested mode but always behave as "allow" (2).
    // let-it-land (1) and hybrid (3) are accepted-but-aliased — deprecated.
    if (mode < 1 || mode > 3) mode = 2;
    _blend_mode = mode;
    if (mode != 2) {
        // Log once so the operator knows their setting is being aliased.
        // This is a soft deprecation — the setting is still stored and
        // reported, just not actively enforced beyond FAS's native behavior.
        static bool warned = false;
        if (!warned) {
            SLOGW("arbiter", "MotionArbiter: blend mode %u aliased to 'allow' (2) — "
                  "FAS retarget handles reversals natively. 'let-it-land' and "
                  "'hybrid' settings are deprecated and will be removed in a "
                  "future release.", mode);
            warned = true;
        }
    }
}