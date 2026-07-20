#pragma once

#include <cstdint>

// ============================================================================
// AdvancedPattern — per-direction stroke math for the Advanced pattern mode
// ============================================================================
//
// Algorithm ported from fray-d/OSSM-Lite's "Advanced Penetration" mode
// (https://github.com/fray-d/OSSM-Lite — CERN-OHL-S v2, building on
// Research+Desire / KinkyMakers OSSM). The BLE characteristic plumbing,
// display UI, and preset string codecs were dropped; only the numerics
// survive: seven base controls (master speed, per-direction speed/accel,
// min/max depth) plus per-control cyclic modifiers that modulate a control
// across a repeating window of strokes (ramp in → hold → ramp out → rest).
//
// Everything here is pure math — no Arduino, no motor calls, no tasks.
// PatternEngine owns the instance, converts the returned fractions into a
// mm-space MotionIntent (target + speed + accel + deadline), and submits it
// through the MotionArbiter like every other input source.
//
// Cross-core contract: Core 0 (WebUI) writes the volatile uint8_t fields via
// PatternEngine setters; Core 1 reads them per half-stroke. Single-byte
// volatile writes are hardware-atomic on ESP32-S3 — same convention as the
// rest of PatternEngine's parameter block.

namespace advpat {

// Wire ids — byte-identical to fray-d's BaseControls enum order so presets
// and remotes speaking his protocol map 1:1.
enum BaseId : uint8_t {
    DEPTH_MAX = 0,   // in-stroke target ("Depth 1")
    DEPTH_MIN = 1,   // out-stroke target ("Depth 2")
    SPEED_IN  = 2,
    SPEED_OUT = 3,
    ACCEL_IN  = 4,
    ACCEL_OUT = 5,
    BASE_COUNT = 6
};

// Knob-feel curve exponents (fray-d: NVS "SpeedCurve" default 0.8; the accel
// curve is hardcoded 0.6 at his call site).
constexpr float SPEED_CURVE_EXP = 0.8f;
constexpr float ACCEL_CURVE_EXP = 0.6f;

// ----------------------------------------------------------------------------
// Modifier — cyclic per-stroke modulation of one base control.
// One cycle = in_step strokes ramping toward full modification, in_wait
// strokes held there, out_step strokes ramping back, out_wait strokes at
// rest. amplitude 100 = modifier off; offset phase-shifts the cycle.
// ----------------------------------------------------------------------------
struct Modifier {
    volatile uint8_t amplitude = 100;  // 0..100 (100 = off)
    volatile uint8_t in_step   = 1;    // 1..25
    volatile uint8_t in_wait   = 0;    // 0..25
    volatile uint8_t out_step  = 1;    // 1..25
    volatile uint8_t out_wait  = 0;    // 0..25
    volatile uint8_t offset    = 0;    // 0..100

    uint8_t stepCount() const { return in_step + in_wait + out_step + out_wait; }
    bool    active()    const { return amplitude < 100 && stepCount() > 0; }

    // 0..1 multiplier applied to the control's swing for a given cycle index.
    float modification(int cycle) const;
};

// ----------------------------------------------------------------------------
// BaseControl — one 0..100 knob with an optional modifier. invert_ref marks
// the control whose modifier swings toward its MAX bound (DEPTH_MIN pulls up
// toward max depth) instead of toward its MIN bound.
// ----------------------------------------------------------------------------
struct BaseControl {
    volatile uint8_t value;
    volatile uint8_t min_value;   // dynamic for the depth pair (coupled)
    volatile uint8_t max_value;
    bool invert_ref;
    Modifier modifier;

    BaseControl(uint8_t v, uint8_t mn, uint8_t mx, bool inv)
        : value(v), min_value(mn), max_value(mx), invert_ref(inv) {}

    void set(int v) {
        if (v < (int)min_value) v = min_value;
        if (v > (int)max_value) v = max_value;
        value = (uint8_t)v;
    }

    float modifiedValue(int stroke_count) const;
    float normalizedModified(int stroke_count) const { return modifiedValue(stroke_count) / 100.0f; }
    float rampedModified(float curve_exp, int stroke_count) const;
};

// ----------------------------------------------------------------------------
// StrokePlan — what one half-stroke asks of the machine, in unitless
// fractions. PatternEngine turns this into mm + mm/s + mm/s² + deadline.
// ----------------------------------------------------------------------------
struct StrokePlan {
    bool  moving;        // false = master speed is 0: hold position
    float target_frac;   // 0..1 within the stroke window
    float speed_frac;    // 0..1 of the input speed ceiling
    float accel_knob;    // 0..1 → accel = minAccel × (1 + 9·knob), fray-d's 1×–10× range
};

// ----------------------------------------------------------------------------
// Settings — the full advanced-mode control set (fray-d defaults: shallow
// 10% max depth and master speed 0, so a fresh boot cannot lunge).
// ----------------------------------------------------------------------------
struct Settings {
    BaseControl master    { 0,   0, 100, false };  // master speed (no modifier)
    BaseControl max_depth { 10,  0, 100, false };
    BaseControl min_depth { 0,   0, 100, true  };
    BaseControl in_speed  { 100, 1, 100, false };
    BaseControl out_speed { 100, 1, 100, false };
    BaseControl in_accel  { 40,  0, 100, false };
    BaseControl out_accel { 40,  0, 100, false };

    BaseControl*       byId(uint8_t id);
    const BaseControl* byId(uint8_t id) const;

    // Depth coupling (fray-d setDepthLimits): min can never cross max.
    void coupleDepths() {
        max_depth.min_value = min_depth.value;
        min_depth.max_value = max_depth.value;
    }

    // Even stroke_count = in-stroke (toward max_depth), odd = out-stroke.
    StrokePlan planStroke(uint32_t stroke_count) const;
};

} // namespace advpat
