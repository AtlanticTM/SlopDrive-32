#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "SystemState.h"
#include "AdvancedPattern.h"

class RangeMapper;
class MotorDriver;
class Pattern; // forward — vendored from lib/StrokeEnginePatterns/src/pattern.h

// ============================================================================
// PatternEngine — OSSM StrokeEngine pattern math driving OUR motor layer
// ============================================================================
//
// Structural sibling of Generator: same construction style (SystemState&,
// RangeMapper&, MotorDriver&), same Core-1 task cadence and gates (homed /
// paused / e-stop / streaming-override / Intiface-yield), dispatching motion
// exclusively through MotorDriver so soft-start caps, accel guards, and
// telemetry keep working unchanged.
//
// The 7 vendored pattern classes (SimpleStroke through Insist) live in
// lib/StrokeEnginePatterns/src/pattern.h. They are pure-math function objects
// that return motionParameter { position, speed, accel, skip } in abstract
// "steps". PatternEngine converts these into mm-space and streams them to the
// motor via MotorDriver::streamTo().
//
// Lifecycle:
//   PatternEngine(SystemState&, RangeMapper&, MotorDriver&)
//   init()          — creates the FreeRTOS task (pinned Core 1, priority 2)
//   emergencyStop() — flags running=false; task holds position
//   start()/stop()  — user-facing gate (requires homed)
//
// Public setters run on Core 0 (WebUI / API); volatile scalars are hardware-
// atomic on ESP32-S3 for single-field reads/writes. No portMUX needed here.

class PatternEngine {
public:
    PatternEngine(SystemState& state, RangeMapper& mapper, MotorDriver& motor);
    ~PatternEngine();

    // ---- Lifecycle ----------------------------------------------------------
    void init();
    void emergencyStop();

    bool isRunning() const;
    bool isActive()  const;

    // ---- Read-only telemetry (Core 0 safe — volatile reads) -----------------
    float getSpeedPercent()     const { return _speed; }
    float getDepthPercent()     const { return _depth; }
    float getStrokePercent()    const { return _stroke; }
    float getSensationPercent() const { return (_sensation + 100.0f) / 2.0f; }  // -100..+100 → 0..100
    int   getPatternIdx()       const { return _pattern_idx; }

    // ---- Advanced mode (fray-d Advanced Penetration port) -------------------
    // Advanced is the PRIMARY pattern generator; the vendored StrokeEngine
    // pattern list above stays available as the legacy/classic option.
    bool  isAdvancedMode() const { return _advanced; }
    void  setAdvancedMode(bool on);
    void  setApMaster(int v);                       // 0..100, 0 = stopped/hold
    void  setApBase(uint8_t base_id, int v);        // advpat::BaseId, clamps + depth coupling
    int   getApMaster() const { return _ap.master.value; }
    int   getApBase(uint8_t base_id) const;
    // Full-modifier write for one base control (single Core-0 writer; the
    // WebUI reads current values via apSettings() and overrides the fields
    // the request carried).
    void  setApModifier(uint8_t base_id, int amplitude, int in_step, int in_wait,
                        int out_step, int out_wait, int offset);
    // fray-d "reset" baseline: speeds/accels to defaults, all modifiers off.
    // Depths and master speed deliberately untouched — presets change stroke
    // character, never the user's safety window or throttle.
    void  resetAdvanced();
    const advpat::Settings& apSettings() const { return _ap; }

    // ---- User-facing controls (Core 0 — WebUI / API) ------------------------
    // Must already be homed.  Boots in stopped state.
    void start();
    void stop();

    // ---- Parameter setters (Core 0; each is a single aligned volatile write) -
    void setSpeed(float speed);          // 0..100 — strokes-per-minute scaling
    void setDepth(float depth);          // 0..100 — insertion depth into window
    void setStroke(float stroke);        // 0..100 — stroke amplitude
    void setSensation(float sensation);  // 0..100 external; maps to -100..+100 internal
    void setPattern(int idx);            // 0..patternCount()-1; out-of-range clamps

    // ---- Arbiter wiring — called once from setup() after arbiter is created -- 
    void setArbiter(class MotionArbiter* arbiter);

    // ---- Metadata -----------------------------------------------------------
    static int   patternCount();
    static const char* patternName(int idx);

private:
    SystemState& _state;
    RangeMapper& _mapper;
    MotorDriver& _motor;

    TaskHandle_t _task = nullptr;

    // Pattern registry — 7 vendored classes allocated once in init()
    static constexpr int CORE_PATTERN_COUNT = 7;
    static constexpr int PATTERN_COUNT =
        CORE_PATTERN_COUNT
#if PATTERN_EXT_TESTPATTERN1 && PATTERN_EXT_ARRAYPATTERN
        + 1
#endif
#if PATTERN_EXT_TESTPATTERN2 && PATTERN_EXT_ARRAYPATTERN
        + 1
#endif
    ;
    Pattern* _patterns[PATTERN_COUNT] = {};

    // Pinned active pattern pointer (never null after init)
    Pattern* _active_pattern = nullptr;

    // Per-stroke index fed to pattern::nextTarget(); wraps naturally.
    unsigned int _stroke_index = 0;

    // ---- Parameters (Core 0 writes, Core 1 reads — 32-bit aligned = hardware
    //      atomic on ESP32-S3; no need for portMUX on single fields) ----------
    volatile float _speed      = 50.0f;     // 0..100
    volatile float _depth      = 100.0f;    // 0..100
    volatile float _stroke     = 100.0f;    // 0..100
    volatile float _sensation  = 0.0f;      // -100..+100 internal representation
    volatile int   _pattern_idx = 0;
    volatile bool  _running     = false;

    // ---- Advanced mode state --------------------------------------------------
    advpat::Settings  _ap;                  // Core 0 writes via setters, Core 1 reads
    volatile bool     _advanced = true;     // advanced IS the default generator
    volatile uint32_t _ap_gen   = 0;        // bumped on any advanced write → live retarget

    // ---- Virtual step system --------------------------------------------------
    // The vendored pattern classes work in integer "steps". We define a fixed
    // abstract step range; all conversions to/from mm happen in this module.
    static constexpr int MAX_ABSTRACT_STEPS = 10000;

    // Cached step-space state, recalculated in run() when user params change.
    int   _depth_steps           = MAX_ABSTRACT_STEPS;
    int   _stroke_steps          = MAX_ABSTRACT_STEPS;
    float _max_steps_per_second  = 50000.0f;

    // Static trampoline for FreeRTOS → member function dispatch.
    static void taskFunction(void* param);

    // The actual task loop (runs on Core 1).
    void run();

    // ---- Advanced-mode stroke cycle (Core 1) ---------------------------------
    // One half-stroke: plan → submit ONE intent → event-driven wait (with live
    // retarget when a parameter write bumps _ap_gen mid-stroke).
    void _advancedStroke();
    // Convert a StrokePlan into a MotionIntent and submit. Returns the
    // expected stroke duration in seconds, or -1 if there was no meaningful
    // travel (nothing dispatched).
    float _submitApStroke(uint32_t stroke_count, const advpat::StrokePlan& sp);

    // ---- Unit conversion helpers (all inline, float) --------------------------
    float stepToMm(int step) const;
    int   mmToStep(float mm) const;
    float stepsPerSecToMmPerSec(int steps_s) const;
    int   mmPerSecToStepsPerSec(float mm_s) const;

    // Re-derive step-space parameters from user-facing 0..100 values.
    void _recalcParameters();

    // Debug heartbeat — mirrors Generator's APPLOG heartbeat pattern.
    void _diagnostics(uint32_t& last_diag_ms);

    // ---- D4: MotionArbiter reference (set once by main.cpp) ------------------
    MotionArbiter* _arbiter = nullptr;
};
