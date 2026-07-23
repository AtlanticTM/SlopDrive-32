#include "PatternEngine.h"
#include "pattern.h"         // vendored core pattern classes (lib/StrokeEnginePatterns/src/)
#include "patternExtended.h" // extended patterns from community branches
#include "range_mapper.h"
#include "MotionArbiter.h"
#include "MotorDriver.h"
#include "AppLog.h"
#include "config_api.h"

#include <Arduino.h>

// ============================================================================
// Static pattern registry — byte-identical names to upstream OSSM
// ============================================================================

static const char* kExtendedNames[] = {
#if PATTERN_EXT_TESTPATTERN1 && PATTERN_EXT_ARRAYPATTERN
    "Test Pattern 1",
#endif
#if PATTERN_EXT_TESTPATTERN2 && PATTERN_EXT_ARRAYPATTERN
    "Test Pattern 2",
#endif
};
static constexpr int EXT_COUNT = (sizeof(kExtendedNames) / sizeof(kExtendedNames[0]));

int PatternEngine::patternCount() { return PATTERN_COUNT; }

const char* PatternEngine::patternName(int idx) {
    if (idx < 0 || idx >= PATTERN_COUNT) return "Invalid";
    if (idx < CORE_PATTERN_COUNT) {
        static const char* kCoreNames[] = {
            "Simple Stroke", "Teasing Pounding", "Robo Stroke",
            "Half'n'Half", "Deeper", "Stop'n'Go", "Insist"
        };
        return kCoreNames[idx];
    }
    int extIdx = idx - CORE_PATTERN_COUNT;
    return (extIdx < EXT_COUNT) ? kExtendedNames[extIdx] : "Invalid";
}

// ============================================================================
// Constructor / destructor
// ============================================================================

PatternEngine::PatternEngine(SystemState& state, RangeMapper& mapper, MotorDriver& motor)
    : _state(state), _mapper(mapper), _motor(motor)
{}

PatternEngine::~PatternEngine() {
    if (_task) vTaskDelete(_task);
}

// ============================================================================
// Lifecycle
// ============================================================================

void PatternEngine::init() {
    // Allocate the 7 vendored pattern objects once — no heap churn in the loop.
    _patterns[0] = new SimpleStroke("Simple Stroke");
    _patterns[1] = new TeasingPounding("Teasing Pounding");
    _patterns[2] = new RoboStroke("Robo Stroke");
    _patterns[3] = new HalfnHalf("Half'n'Half");
    _patterns[4] = new Deeper("Deeper");
    _patterns[5] = new StopNGo("Stop'n'Go");
    _patterns[6] = new Insist("Insist");

    // Extended patterns from community branches (if compiled in)
    int nextIdx = CORE_PATTERN_COUNT;
#if PATTERN_EXT_TESTPATTERN1 && PATTERN_EXT_ARRAYPATTERN
    if (nextIdx < PATTERN_COUNT)
        _patterns[nextIdx++] = new TestPattern1();
#endif
#if PATTERN_EXT_TESTPATTERN2 && PATTERN_EXT_ARRAYPATTERN
    if (nextIdx < PATTERN_COUNT)
        _patterns[nextIdx++] = new TestPattern2();
#endif

    _active_pattern = _patterns[0];

    // Pin to Core 1, priority 2, 4k stack — identical to Generator's task
    // so they share the same scheduling tier.
    xTaskCreatePinnedToCore(taskFunction, "PatternEng", 4096, this, 2, &_task, 1);
}

void PatternEngine::emergencyStop() {
    _running = false;
    _state.pattern_running = false;
}

bool PatternEngine::isRunning() const { return _running; }

bool PatternEngine::isActive() const {
    return _running && _state.homed;
}

// ============================================================================
// User-facing controls
// ============================================================================

void PatternEngine::start() {
    if (!_state.homed) return;
    _running        = true;
    _stroke_index   = 0;   // fresh start
    // Explicit user claim: starting the pattern takes the machine back from a
    // chattering (keep-alive-only) stream. A stream that actually MOVES again
    // re-stamps last_intiface_move_ms and reclaims — see the yield gate.
    _state.pattern_running = true;
}

void PatternEngine::stop() {
    _running = false;
    _state.pattern_running = false;
}

// ============================================================================
// Parameter setters (Core 0 — single volatile fields, hardware-atomic)
// ============================================================================

void PatternEngine::setSpeed(float speed) {
    if (speed < 0.0f) speed = 0.0f;
    if (speed > 100.0f) speed = 100.0f;
    _speed = speed;
}

void PatternEngine::setDepth(float depth) {
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 100.0f) depth = 100.0f;
    _depth = depth;
}

void PatternEngine::setStroke(float stroke) {
    if (stroke < 0.0f) stroke = 0.0f;
    if (stroke > 100.0f) stroke = 100.0f;
    _stroke = stroke;
}

void PatternEngine::setSensation(float sensation) {
    if (sensation < 0.0f) sensation = 0.0f;
    if (sensation > 100.0f) sensation = 100.0f;
    _sensation = (sensation - 50.0f) * 2.0f;   // 0→-100, 50→0, 100→+100
}

void PatternEngine::setPattern(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= PATTERN_COUNT) idx = PATTERN_COUNT - 1;
    _pattern_idx = idx;
}

// ============================================================================
// Advanced-mode setters (Core 0 — single volatile u8 writes, hardware-atomic).
// Every write bumps _ap_gen so a stroke in flight retargets immediately
// (fray-d semantics: sliders act now, not at the next stroke boundary).
// ============================================================================

void PatternEngine::setAdvancedMode(bool on) {
    if (_advanced == on) return;
    _advanced = on;
    _stroke_index = 0;      // fresh parity/modifier cycle on mode entry
    _ap_gen = _ap_gen + 1;
}

void PatternEngine::setApMaster(int v) {
    _ap.master.set(v);
    _ap_gen = _ap_gen + 1;
}

void PatternEngine::setApBase(uint8_t base_id, int v) {
    advpat::BaseControl* c = _ap.byId(base_id);
    if (!c) return;
    c->set(v);
    // fray-d setDepthLimits: the depth pair may never cross.
    if (base_id == advpat::DEPTH_MAX || base_id == advpat::DEPTH_MIN)
        _ap.coupleDepths();
    _ap_gen = _ap_gen + 1;
}

int PatternEngine::getApBase(uint8_t base_id) const {
    const advpat::BaseControl* c = _ap.byId(base_id);
    return c ? (int)c->value : 0;
}

void PatternEngine::resetAdvanced() {
    _ap.in_speed.set(100);
    _ap.out_speed.set(100);
    _ap.in_accel.set(40);
    _ap.out_accel.set(40);
    for (uint8_t id = 0; id < advpat::BASE_COUNT; id++) {
        advpat::BaseControl* c = _ap.byId(id);
        if (!c) continue;
        advpat::Modifier& m = c->modifier;
        m.amplitude = 100;
        m.in_step   = 1;
        m.in_wait   = 0;
        m.out_step  = 1;
        m.out_wait  = 0;
        m.offset    = 0;
    }
    _ap_gen = _ap_gen + 1;
}

void PatternEngine::setApModifier(uint8_t base_id, int amplitude, int in_step, int in_wait,
                                  int out_step, int out_wait, int offset) {
    advpat::BaseControl* c = _ap.byId(base_id);
    if (!c) return;
    advpat::Modifier& m = c->modifier;
    m.amplitude = (uint8_t)constrain(amplitude, 0, 100);
    m.in_step   = (uint8_t)constrain(in_step,   1, 25);
    m.in_wait   = (uint8_t)constrain(in_wait,   0, 25);
    m.out_step  = (uint8_t)constrain(out_step,  1, 25);
    m.out_wait  = (uint8_t)constrain(out_wait,  0, 25);
    m.offset    = (uint8_t)constrain(offset,    0, 100);
    _ap_gen = _ap_gen + 1;
}

// ============================================================================
// Set the arbiter reference — called from main.cpp after arbiter is created.
// ============================================================================

void PatternEngine::setArbiter(MotionArbiter* arbiter) {
    _arbiter = arbiter;
}

// ============================================================================
// Unit conversion / parameter plumbing
// ============================================================================

float PatternEngine::stepToMm(int step) const {
    return (float)step / (float)MAX_ABSTRACT_STEPS;
}

int PatternEngine::mmToStep(float mm) const {
    float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
    float frac = (hi > lo) ? (mm - lo) / (hi - lo) : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return (int)(frac * (float)MAX_ABSTRACT_STEPS);
}

float PatternEngine::stepsPerSecToMmPerSec(int steps_s) const {
    float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
    float span = hi - lo;
    return (float)steps_s * span / (float)MAX_ABSTRACT_STEPS;
}

int PatternEngine::mmPerSecToStepsPerSec(float mm_s) const {
    float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
    float span = hi - lo;
    if (span <= 0.0f) return 0;
    return (int)(mm_s * (float)MAX_ABSTRACT_STEPS / span);
}

void PatternEngine::_recalcParameters() {
    float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
    float span = hi - lo;

    float stroke_mm = (_stroke / 100.0f) * span;
    _stroke_steps = mmToStep(lo + stroke_mm) - mmToStep(lo);

    float depth_mm = lo + (_depth / 100.0f) * span;
    _depth_steps = mmToStep(depth_mm);

    // Patterns are machine-driven input: their speed authority is the INPUT
    // limit set (what the arbiter clamps PATTERN intents at), not the generic
    // max-speed box (which caps the motor driver / manual point moves). Tying
    // the knob to a different ceiling than the clamp meant the dial lied
    // whenever the two settings diverged. :3
    _max_steps_per_second = mmPerSecToStepsPerSec(_state.config.input_max_speed_mm_s);
}

// ============================================================================
// Diagnostics — mirrors Generator's heartbeat style
// ============================================================================

void PatternEngine::_diagnostics(uint32_t& last_diag_ms) {
    if (!_running) return;
    if (millis() - last_diag_ms <= 1000) return;
    last_diag_ms = millis();

    if (_advanced) {
        APPLOGF("PatternEngine: ADVANCED master=%u depth=%u..%u v_in=%u v_out=%u a_in=%u a_out=%u stroke#%u",
                (unsigned)_ap.master.value, (unsigned)_ap.min_depth.value, (unsigned)_ap.max_depth.value,
                (unsigned)_ap.in_speed.value, (unsigned)_ap.out_speed.value,
                (unsigned)_ap.in_accel.value, (unsigned)_ap.out_accel.value, _stroke_index);
        return;
    }
    const char* pname = patternName(_pattern_idx);
    APPLOGF("PatternEngine: running pattern[%d]=\"%s\" speed=%.0f depth=%.0f stroke=%.0f sens=%.0f",
            _pattern_idx, pname, _speed, _depth, _stroke, _sensation);
}

// ============================================================================
// Static trampoline → member function
// ============================================================================

void PatternEngine::taskFunction(void* param) {
    static_cast<PatternEngine*>(param)->run();
}

// ============================================================================
// Task loop — D4 REFACTORED: emit ONE intent per stroke segment (event-driven)
// ============================================================================
//
// The old disease (v3 era): tick at gen_rate_tick_hz, emit dense position
// points, replan at MAX accel. Gone.
//
// The cure (D4): ONE COMMAND → ONE PLAN → FAS EXECUTES. The pattern's
// nextTarget() generates a motionParameter {stroke, speed, accel, skip}
// for the next half-stroke. We convert that into a MotionIntent with the
// segment's deadline (derived from the pattern's speed parameter), submit
// it to the arbiter, and SLEEP for the segment duration. The task wakes
// only to emit the next stroke — no periodic clock, no chase loop.
//
// StopNGo's skip=true segments: the task simply blocks for the pattern's
// internal delay without submitting an intent.
//
// D4 bidirectionality: the task reads MachineState and publishes
// telemetry AFTER the arbiter returns — the report contains derived/
// clamped dynamics from the actual plan.
//
// gen_rate_tick_hz: legacy, kept parsed but the task no longer ticks at
// this rate. Used only for diagnostics cadence and WebUI reporting.
// Marked deprecated.

void PatternEngine::run() {
    uint32_t last_diag_ms = 0;

    while (true) {
        // ---- Consistent snapshot of user parameters (Core 1 read side) ----
        float  speed     = _speed;
        float  depth     = _depth;
        float  stroke    = _stroke;
        float  sensation = _sensation;
        int    pat_idx   = _pattern_idx;
        bool   running   = _running;

        // ---- Gate checks — yield to stream MOTION, not stream packets -----
        // Packet recency (last_intiface_ms) let keep-alive-only hosts pin the
        // pattern off forever. The pattern now yields only while the stream is
        // actually driving the target (moved within 1.5s) — matching the
        // sampler's reclaim window in streamSamplerTask.
        bool intiface_driving = (_state.last_intiface_move_ms != 0) &&
                                (millis() - _state.last_intiface_move_ms < 1500);
        bool user_has_control = _state.paused || _state.manual_override;
        bool emit_ok = running && _state.homed &&
                       (user_has_control || !intiface_driving);

        // Expose activity state for WebUI
        _state.gen_active = emit_ok;

        // ---- Diagnostics --------------------------------------------------
        _diagnostics(last_diag_ms);

        if (emit_ok && _arbiter && _advanced) {
            // ---- Advanced mode (fray-d Advanced Penetration port) ---------
            // One half-stroke per call; the wait happens inside so a live
            // parameter write retargets the stroke in flight.
            _advancedStroke();
        } else if (emit_ok && _arbiter) {
            // ---- Select active pattern -----------------------------------
            int idx = pat_idx;
            if (idx < 0) idx = 0;
            if (idx >= PATTERN_COUNT) idx = PATTERN_COUNT - 1;
            _active_pattern = _patterns[idx];

            // ---- Recompute step-space params -----------------------------
            _recalcParameters();

            // ---- Feed pattern the current user params ---------------------
            float timeOfStroke = 0.0f;
            if (speed > 0.0f && _max_steps_per_second > 0.0f) {
                float peak_rate_s = (speed / 100.0f) * _max_steps_per_second;
                // Knob % = fraction of the input ceiling as ACTUAL peak
                // carriage speed. The SimpleStroke family halves timeOfStroke
                // per direction and cruises at 1.5×stroke/half — i.e. peak =
                // 3×stroke/timeOfStroke — so feed it 3× the naive traversal
                // time. (Before this, 33% on the knob already demanded the
                // full ceiling and the top ⅔ of the dial did nothing.) :3
                if (peak_rate_s > 1.0f)
                    timeOfStroke = 3.0f * (float)_stroke_steps / peak_rate_s;
            }
            if (timeOfStroke <= 0.0f) timeOfStroke = 0.1f;

            _active_pattern->setTimeOfStroke(timeOfStroke);
            _active_pattern->setStroke(_stroke_steps);
            _active_pattern->setDepth(_depth_steps);
            _active_pattern->setSensation(sensation);
            _active_pattern->setSpeedLimit((unsigned int)_max_steps_per_second,
                                           (unsigned int)_max_steps_per_second,
                                           1);

            // ---- Ask the pattern for the NEXT target ----------------------
            motionParameter mp = _active_pattern->nextTarget(_stroke_index);

            if (mp.skip) {
                // StopNGo pause: the pattern has an internal millis() timer.
                // nextTarget() returns skip=true while the delay is active,
                // then returns skip=false with a real move when the timer
                // expires. We poll by calling nextTarget() repeatedly — each
                // call checks the timer internally. This is exactly what the
                // old tick-based loop did, but now we only run this polling
                // during StopNGo pauses (not during normal stroke execution).
                //
                // The pattern's _isStillDelayed() is protected, so we can't
                // call it directly from PatternEngine. Instead we rely on the
                // skip-handshake: sleep 10ms, re-call nextTarget(), repeat
                // until skip becomes false. This is the only polling remnant
                // in the entire D4 architecture.
                while (_running) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    if (!_running) break;
                    mp = _active_pattern->nextTarget(_stroke_index);
                    if (!mp.skip) break;  // delay expired, we have a real move
                }
                if (!_running) continue;
            }

            // ---- Convert pattern output to MotionIntent -------------------
            float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
            float span  = hi - lo;
            float pos_mm = lo + stepToMm(mp.stroke) * span;
            pos_mm = constrain(pos_mm, lo, hi);

            // Pattern speed/accel in mm units (from the pattern's derived
            // dynamics — the linear span/MAX_ABSTRACT_STEPS scaling applies
            // equally to /s and /s² quantities).
            float speed_mm_s  = stepsPerSecToMmPerSec(mp.speed);
            float accel_mm_s2 = stepsPerSecToMmPerSec(mp.acceleration);

            // Segment duration: derive from THIS half-stroke's own dynamics
            // (trapezoid at cruise v with accel a: T ≈ d/v + v/a), the same
            // math advanced mode uses. The old uniform timeOfStroke/2 deadline
            // erased every per-direction asymmetry — sensation's whole job in
            // most patterns (fast-in/slow-out ratios up to 5×): the fast half
            // arrived early and idled, the slow half got an infeasible T/2
            // deadline, fell back to triangle math and executed FAST anyway.
            // Sensation felt near-dead because pacing overrode the pattern. :3
            float half_stroke_s = timeOfStroke / 2.0f;   // fallback pacing
            float d_mm = fabsf(pos_mm - _motor.getPosition());
            float seg_s;
            if (speed_mm_s > 1.0f && d_mm > 0.05f) {
                seg_s = d_mm / speed_mm_s
                      + ((accel_mm_s2 > 1.0f) ? (speed_mm_s / accel_mm_s2) : 0.0f);
            } else {
                seg_s = half_stroke_s;
            }
            if (seg_s < 0.005f) seg_s = 0.005f;
            uint32_t segment_ms = (uint32_t)(seg_s * 1000.0f);
            if (segment_ms < 5) segment_ms = 5;

            // ---- Feasibility guard ------------------------------------------
            // StrokeEngine trapezoids peak at 3× the engine's assumed rate
            // (patterns halve timeOfStroke per direction, then cruise at
            // 1.5×d/T), so above ~⅓ on the speed knob the demand exceeds the
            // input ceiling. Submitting that as-is makes the arbiter clamp the
            // SPEED while the DEADLINE stays infeasible — every stroke lands
            // late, the next intent launches from short of the endpoint, and
            // the stroke drifts. Never ask for more than the machine may give:
            // clamp the demand at the input ceiling and stretch the deadline
            // by the same ratio so pacing, planner and motor all agree on when
            // the stroke lands. :3
            float input_ceiling = _state.config.input_max_speed_mm_s;
            if (input_ceiling >= 1.0f && speed_mm_s > input_ceiling) {
                float stretch = speed_mm_s / input_ceiling;
                segment_ms   = (uint32_t)((float)segment_ms * stretch);
                speed_mm_s   = input_ceiling;
            }

            MotionIntent intent;
            intent.source        = MotionSource::PATTERN;
            intent.target_mm     = pos_mm;
            intent.deadline_ms   = segment_ms;
            intent.speed_hint_mm_s = speed_mm_s;
            // No ramp data from patterns — use identity (1.0)
            intent.seq           = _stroke_index;

            // ---- Submit to the arbiter — ONE intent, ONE plan, FAS executes
            PlanReport report = _arbiter->submit(intent);

            // Deadline-feasibility feedback: if the plan got clamped at the
            // INPUT limit set, the pattern is running slower/softer than the
            // operator dialed in. Surface that (rate-limited) instead of
            // silently degrading the stroke. :3
            if (report.deadline_late) {
                static uint32_t last_late_log_ms = 0;
                uint32_t now_late = millis();
                if (now_late - last_late_log_ms > 2000) {
                    last_late_log_ms = now_late;
                    APPLOGF("PatternEngine CLAMPED: stroke wants %.0f mm/s @ %.0f mm/s² "
                            "but input limits allow %.0f/%.0f — pattern runs slower than configured",
                            report.derived_speed_mm_s, report.derived_accel_mm_s2,
                            report.clamped_speed_mm_s, report.clamped_accel_mm_s2);
                }
            }

            // ---- Publish telemetry ----------------------------------------
            // D4: actual_position_mm is NEVER written — the telemetry sampler
            // reads _motor.getPosition() directly. We only set commanded_target
            // (what the pattern told the machine to do) and raw (pre-planner demand).
            _state.commanded_target_mm = pos_mm;
            _state.commanded_raw_mm    = pos_mm;

            // ---- Advance stroke index -------------------------------------
            _stroke_index++;

            // ---- D4 pacing: sleep for the segment duration ----------------
            // The arbiter dispatched to FAS non-blockingly. We sleep here
            // so the next stroke arrives when the current segment should
            // have completed. This is the event-driven clock replacement.
            // No loop, no tick, no chase — the task blocks until it's time
            // for the NEXT intent.
            //
            // This also handles the pulse train stall watchdog: if the
            // pattern stops (running=false), the while() exits cleanly
            // and we don't keep submitting.
            uint32_t wake_at = millis() + segment_ms;
            while (_running && (int32_t)(millis() - wake_at) < 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        } else {
            // Idle: nothing to submit. Sleep briefly.
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        // No vTaskDelay at the bottom — the per-segment delay above handles pacing.
        // The only fallthrough to here would be from emit_ok=false or no arbiter yet.
    }
}

// ============================================================================
// Advanced mode — one half-stroke, event-driven (fray-d Advanced Penetration)
// ============================================================================
//
// D4 preserved: ONE intent per half-stroke, dynamics DERIVED from the stroke's
// own geometry (per-direction speed/accel controls), arbiter clamps at the
// INPUT ceilings. The wait below is not a motion clock — it sleeps out the
// stroke and wakes for exactly two events: the stroke completing (next intent
// due) or a parameter write (_ap_gen bump → retarget the stroke in flight,
// which is fray-d's live-slider semantics).

void PatternEngine::_advancedStroke() {
    advpat::StrokePlan sp = _ap.planStroke(_stroke_index);
    if (!sp.moving) {                       // master speed 0 → hold position
        vTaskDelay(pdMS_TO_TICKS(50));
        return;
    }

    uint32_t gen_seen   = _ap_gen;
    float    expected_s = _submitApStroke(_stroke_index, sp);
    if (expected_s < 0.0f) {
        // No meaningful travel (depths pinched together) — keep parity
        // advancing so modifier cycles don't stall, then breathe.
        _stroke_index++;
        vTaskDelay(pdMS_TO_TICKS(50));
        return;
    }

    uint32_t       started_ms  = millis();
    uint32_t       expect_ms   = (uint32_t)(expected_s * 1000.0f);
    const uint32_t hard_cap_ms = expect_ms * 3u + 2000u;  // wedge guard: ceiling-clamped
                                                          // strokes run long, never forever
    while (_running && _advanced) {
        vTaskDelay(pdMS_TO_TICKS(5));

        if (_ap_gen != gen_seen) {
            gen_seen = _ap_gen;
            advpat::StrokePlan rp = _ap.planStroke(_stroke_index);
            if (!rp.moving) {
                // Master speed hit 0 mid-stroke: decelerate to a stop where we
                // are instead of finishing the plunge. Deadline-less intent =
                // plan at the input ceilings, i.e. stop as fast as allowed.
                MotionIntent halt = {};
                halt.source    = MotionSource::PATTERN;
                halt.target_mm = _motor.getPosition();
                halt.seq       = (uint16_t)_stroke_index;
                _arbiter->submit(halt);
                _state.commanded_target_mm = halt.target_mm;
                _state.commanded_raw_mm    = halt.target_mm;
                break;
            }
            float t = _submitApStroke(_stroke_index, rp);
            if (t >= 0.0f) {
                started_ms = millis();
                expect_ms  = (uint32_t)(t * 1000.0f);
            }
        }

        uint32_t elapsed = millis() - started_ms;
        if (elapsed >= expect_ms && !_motor.isMoving()) break;  // stroke landed
        if (elapsed >= hard_cap_ms) break;
    }
    _stroke_index++;
}

// Convert one StrokePlan into a MotionIntent and submit it. fray-d's dynamics:
// cruise speed v = speed_frac × input ceiling; accel spans 1×..10× of the
// minimum accel that can reach v over this distance (minA = v²/d — at 1× the
// profile is a pure triangle peaking at v, at 10× it's nearly all cruise).
// The deadline is the trapezoid's own duration T = d/v + v/a, so the pacing
// wait and the planner agree on when the stroke should land.
float PatternEngine::_submitApStroke(uint32_t stroke_count, const advpat::StrokePlan& sp) {
    float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
    float span = hi - lo;
    if (span <= 0.0f) return -1.0f;

    float target_mm = lo + sp.target_frac * span;
    float d = fabsf(target_mm - _motor.getPosition());
    if (d < 0.25f) return -1.0f;            // sub-quarter-mm: nothing worth dispatching

    // Input limit set, not the max-speed box — same authority the arbiter
    // clamps PATTERN intents at (the header comment always said "input
    // ceiling"; the code just didn't). :3
    float v = sp.speed_frac * _state.config.input_max_speed_mm_s;
    if (v < 1.0f) v = 1.0f;                 // keep the deadline finite at knob extremes

    float min_a = (v * v) / d;
    float a     = min_a * (1.0f + 9.0f * sp.accel_knob);
    float T     = d / v + v / a;

    MotionIntent intent = {};
    intent.source           = MotionSource::PATTERN;
    intent.target_mm        = target_mm;
    intent.deadline_ms      = (uint32_t)(T * 1000.0f) + 1;
    intent.speed_hint_mm_s  = v;
    intent.accel_hint_mm_s2 = a;
    intent.seq              = (uint16_t)stroke_count;

    PlanReport report = _arbiter->submit(intent);

    if (report.deadline_late) {
        static uint32_t last_late_log_ms = 0;
        uint32_t now_late = millis();
        if (now_late - last_late_log_ms > 2000) {
            last_late_log_ms = now_late;
            APPLOGF("PatternEngine ADV CLAMPED: stroke wants %.0f mm/s @ %.0f mm/s² "
                    "but input limits allow %.0f/%.0f — running softer than dialed",
                    report.derived_speed_mm_s, report.derived_accel_mm_s2,
                    report.clamped_speed_mm_s, report.clamped_accel_mm_s2);
        }
    }

    _state.commanded_target_mm = target_mm;
    _state.commanded_raw_mm    = target_mm;
    return T;
}