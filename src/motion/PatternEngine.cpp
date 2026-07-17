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
}

void PatternEngine::stop() {
    _running = false;
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

    _max_steps_per_second = mmPerSecToStepsPerSec(_state.config.max_speed_mm_s);
}

// ============================================================================
// Diagnostics — mirrors Generator's heartbeat style
// ============================================================================

void PatternEngine::_diagnostics(uint32_t& last_diag_ms) {
    if (!_running) return;
    if (millis() - last_diag_ms <= 1000) return;
    last_diag_ms = millis();

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

        // ---- Gate checks — IDENTICAL to current semantics -----------------
        bool intiface_recent = (_state.last_intiface_ms != 0) &&
                               (millis() - _state.last_intiface_ms < 250);
        bool user_has_control = _state.paused || _state.manual_override;
        bool emit_ok = running && _state.homed &&
                       (user_has_control || !intiface_recent);

        // Expose activity state for WebUI
        _state.gen_active = emit_ok;

        // ---- Diagnostics --------------------------------------------------
        _diagnostics(last_diag_ms);

        if (emit_ok && _arbiter) {
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
                if (peak_rate_s > 1.0f)
                    timeOfStroke = (float)_stroke_steps / peak_rate_s;
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

            // Segment duration: half the stroke time (one in OR out segment).
            // The pattern's timeOfStroke is for a full in+out cycle; each
            // nextTarget() call produces one half-stroke.
            float half_stroke_s = timeOfStroke / 2.0f;
            if (half_stroke_s < 0.005f) half_stroke_s = 0.005f;
            uint32_t segment_ms = (uint32_t)(half_stroke_s * 1000.0f);
            if (segment_ms < 5) segment_ms = 5;

            // Pattern speed in mm/s (from the pattern's derived dynamics)
            float speed_mm_s = stepsPerSecToMmPerSec(mp.speed);

            MotionIntent intent;
            intent.source        = MotionSource::PATTERN;
            intent.target_mm     = pos_mm;
            intent.deadline_ms   = segment_ms;
            intent.speed_hint_mm_s = speed_mm_s;
            // No ramp data from patterns — use identity (1.0)
            intent.seq           = _stroke_index;

            // ---- Submit to the arbiter — ONE intent, ONE plan, FAS executes
            PlanReport report = _arbiter->submit(intent);

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