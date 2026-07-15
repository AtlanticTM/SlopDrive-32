#include "PatternEngine.h"
#include "pattern.h"         // vendored core pattern classes (lib/StrokeEnginePatterns/src/)
#include "patternExtended.h" // extended patterns from community branches
#include "range_mapper.h"
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
    // External representation: 0..100 (same as web UI slider).
    // Internal representation: -100..+100 (same as OSSM's pattern classes).
    // Map: 0-> -100, 50->0, 100->+100 linearly.
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
// Unit conversion / parameter plumbing
// ============================================================================

float PatternEngine::stepToMm(int step) const {
    return (float)step / (float)MAX_ABSTRACT_STEPS;
    // Returns [0..1] fraction, later multiplied by the mapper's mm span.
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

    // Stroke in mm from the user-facing 0..100 slider.
    float stroke_mm = (_stroke / 100.0f) * span;
    _stroke_steps = mmToStep(lo + stroke_mm) - mmToStep(lo);  // relative stroke in steps

    // Depth in mm — where the tip ends up at full insertion.
    float depth_mm = lo + (_depth / 100.0f) * span;
    _depth_steps = mmToStep(depth_mm);

    // Max speed — the fastest the pattern is allowed to demand.
    // We peg this to the user's configured max speed converted into step-space.
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
// Task loop — pattern evaluation on Core 1
// ============================================================================

void PatternEngine::run() {
    uint32_t last_diag_ms   = 0;
    uint32_t last_tick_us   = 0;
    bool     stroke_in_progress = false;  // true during the "in" half of a cycle
    bool     emitted_this_stroke = false;  // did we send a move command this idx?

    while (true) {
        // ---- Tick rate — mirror Generator's gen_rate_tick_hz. We run at the
        //      same cadence so the motor sees consistent update frequency. ----
        uint16_t ghz = _state.gen_rate_tick_hz;
        if (ghz < 5) ghz = 5;
        if (ghz > 200) ghz = 200;
        const TickType_t period = pdMS_TO_TICKS(1000 / ghz);

        // ---- Consistent snapshot of user parameters (Core 1 read side) ----
        float  speed     = _speed;
        float  depth     = _depth;
        float  stroke    = _stroke;
        float  sensation = _sensation;
        int    pat_idx   = _pattern_idx;
        bool   running   = _running;

        // ---- Gate checks — IDENTICAL to Generator's ---------------------------------
        bool intiface_recent = (_state.last_intiface_ms != 0) &&
                               (millis() - _state.last_intiface_ms < 250);
        bool user_has_control = _state.paused || _state.manual_override;
        bool emit = running && _state.homed &&
                    (user_has_control || !intiface_recent);

        // Expose activity state for WebUI so it can draw a "running" indicator.
        _state.gen_active = emit;

        // ---- Diagnostics ------------------------------------------------------------
        _diagnostics(last_diag_ms);

        if (emit) {
            // ---- Select active pattern (may have changed via setPattern) ------
            int idx = pat_idx;
            if (idx < 0) idx = 0;
            if (idx >= PATTERN_COUNT) idx = PATTERN_COUNT - 1;
            _active_pattern = _patterns[idx];

            // ---- Recompute step-space params each tick they may have changed --
            _recalcParameters();

            // ---- Feed pattern the current user params --------------------------
            // OSSM maps speed (0..100%) to time-of-stroke. In the stock firmware,
            // timeOfStroke = (stroke_steps / maxStepsPerSecond) / (speed% / 100).
            // That yields a time in seconds; the pattern uses it to compute peak
            // step rate. We replicate that here so the pattern math stays
            // byte-compatible.
            float timeOfStroke = 0.0f;
            if (speed > 0.0f && _max_steps_per_second > 0.0f) {
                float peak_rate_s = (speed / 100.0f) * _max_steps_per_second;
                if (peak_rate_s > 1.0f)
                    timeOfStroke = (float)_stroke_steps / peak_rate_s;
            }
            if (timeOfStroke <= 0.0f) timeOfStroke = 0.1f;  // floor to avoid div/0

            _active_pattern->setTimeOfStroke(timeOfStroke);
            _active_pattern->setStroke(_stroke_steps);
            _active_pattern->setDepth(_depth_steps);
            _active_pattern->setSensation(sensation);
            _active_pattern->setSpeedLimit((unsigned int)_max_steps_per_second,
                                           (unsigned int)_max_steps_per_second,
                                           1);  // stepsPerMM=1 (we handle mm conv ourselves)

            // ---- Ask the pattern for the NEXT target --------------------------
            motionParameter mp = _active_pattern->nextTarget(_stroke_index);

            // If the pattern says "skip" (StopNGo pauses), hold position.
            // Otherwise convert step-space output to mm and dispatch.
            if (!mp.skip) {
                // Convert the pattern's absolute step-space position into a physical
                // mm position within the user's configured range window.
                float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
                float span  = hi - lo;
                float pos_mm = lo + stepToMm(mp.stroke) * span;
                pos_mm = constrain(pos_mm, lo, hi);

                // Convert the pattern's step/s speed to mm/s for the motor.
                float speed_mm_s = stepsPerSecToMmPerSec(mp.speed);
                if (speed_mm_s > _state.config.max_speed_mm_s)
                    speed_mm_s = _state.config.max_speed_mm_s;

                // Safe-approach soft start — same as Generator's path.
                float cap = _state.safeSpeedCap(_state.config.max_speed_mm_s, millis());
                float effective_speed = (cap < _state.config.max_speed_mm_s) ? cap : speed_mm_s;

                // Dispatch through MotorDriver — NEVER call FastAccelStepper directly.
                _motor.streamTo(pos_mm, effective_speed);

                // Publish telemetry for the position graph — ACTUAL position must
                // be stored so the Core 0 telemetry sampler captures live motion,
                // not a stale value from the last TCode waypoint (Wave 1.5 fix).
                _state.actual_position_mm.store(pos_mm, std::memory_order_relaxed);
                _state.commanded_target_mm = pos_mm;
                _state.commanded_raw_mm    = pos_mm;

                emitted_this_stroke = true;
            }

            // ---- Advance stroke index -----------------------------------------
            // OSSM's StrokeEngine increments the index after every OUT stroke
            // (odd index = moving out), but the index is always incremented
            // regardless when a stroke completes. We track stroke completion by
            // alternating between in (even) and out (odd) cycles per tick.
            //
            // Simple approach: increment the index each tick. The pattern's own
            // nextTarget() uses index%2 to decide in vs out direction. This
            // matches how OSSM's _stroking() loop works — it calls moveTo() for
            // each stroke, waits for the motor to reach target, then increments
            // _index and calls nextTarget() again.
            //
            // Since we're streaming at 100 Hz (not waiting for completion), we
            // pace index advancement at a rate that matches the pattern's
            // expected tempo.  Each "stroke" (in or out) should take
            // timeOfStroke/2 seconds.  At our tick rate, that's roughly:
            //
            //   ticks_per_half_stroke = (timeOfStroke / 2) * gen_rate_tick_hz
            //
            // We accumulate a phase counter and increment _stroke_index only
            // when a half-stroke's worth of ticks has elapsed.
            static float stroke_phase_accum = 0.0f;
            float dt = (last_tick_us != 0)
                           ? (float)(int32_t)(micros() - last_tick_us) / 1e6f
                           : 0.0f;
            if (dt < 0.0f || dt > 0.5f) dt = 0.0f;
            last_tick_us = micros();
            stroke_phase_accum += dt;
            float half_stroke_dur = timeOfStroke / 2.0f;
            if (half_stroke_dur < 0.005f) half_stroke_dur = 0.005f;
            while (stroke_phase_accum >= half_stroke_dur) {
                stroke_phase_accum -= half_stroke_dur;
                _stroke_index++;
                stroke_in_progress = !stroke_in_progress;
            }
        } else {
            // Idle: reset timestamp so resume doesn't take a giant step.
            last_tick_us = 0;
            stroke_in_progress = false;
        }

        vTaskDelay(period);
    }
}