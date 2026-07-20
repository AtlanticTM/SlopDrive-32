#include "AdvancedPattern.h"

#include <math.h>

// Math ported from fray-d/OSSM-Lite advanced_penetration_structs.h — see the
// header for the full provenance note. Ints promoted to float where his u8
// arithmetic silently truncated; behavior otherwise byte-identical.

namespace advpat {

// ----------------------------------------------------------------------------
// Modifier::modification — fray-d getModification()
// ----------------------------------------------------------------------------
float Modifier::modification(int cycle) const {
    float ratio = (float)(100 - amplitude) / 100.0f;
    if (cycle < 0) return 1.0f - ratio;
    int steps = stepCount();
    if (steps > 0) {
        int in_s  = in_step;    // snapshot volatiles once per call
        int in_w  = in_wait;
        int out_s = out_step;
        cycle = (cycle + offset) % steps;
        if (cycle < in_s)
            return 1.0f - ratio / (float)in_s * (float)(cycle + 1);
        cycle -= in_s;
        if (cycle < in_w)
            return 1.0f - ratio;
        cycle -= in_w;
        if (cycle < out_s)
            return 1.0f - ratio + ratio / (float)out_s * (float)(cycle + 1);
    }
    return 1.0f;
}

// ----------------------------------------------------------------------------
// BaseControl::modifiedValue — fray-d getModifiedValue()
// ----------------------------------------------------------------------------
float BaseControl::modifiedValue(int stroke_count) const {
    if (!modifier.active()) return (float)value;
    float difference = (float)value - (float)(invert_ref ? max_value : min_value);
    int steps = modifier.stepCount();
    // Two half-strokes (one in + one out) advance the modifier cycle by one.
    int cycle = (stroke_count < 0) ? -1 : (int)((stroke_count / 2) % steps);
    return (float)value - difference * (1.0f - modifier.modification(cycle));
}

// ----------------------------------------------------------------------------
// BaseControl::rampedModified — fray-d getRampedModifiedValue():
// pow(1 - pow(1 - x, e), 1/e) — an ease curve that keeps low knob values
// gentle and expands resolution at the top end.
// ----------------------------------------------------------------------------
float BaseControl::rampedModified(float curve_exp, int stroke_count) const {
    float x = normalizedModified(stroke_count);
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return powf(1.0f - powf(1.0f - x, curve_exp), 1.0f / curve_exp);
}

// ----------------------------------------------------------------------------
// Settings::byId — wire-id lookup (BaseId order)
// ----------------------------------------------------------------------------
BaseControl* Settings::byId(uint8_t id) {
    switch (id) {
        case DEPTH_MAX: return &max_depth;
        case DEPTH_MIN: return &min_depth;
        case SPEED_IN:  return &in_speed;
        case SPEED_OUT: return &out_speed;
        case ACCEL_IN:  return &in_accel;
        case ACCEL_OUT: return &out_accel;
        default:        return nullptr;
    }
}

const BaseControl* Settings::byId(uint8_t id) const {
    return const_cast<Settings*>(this)->byId(id);
}

// ----------------------------------------------------------------------------
// Settings::planStroke — one half-stroke's demand, fray-d's motion task math
// ----------------------------------------------------------------------------
StrokePlan Settings::planStroke(uint32_t stroke_count) const {
    StrokePlan p = {};
    float master_frac = (float)master.value / 100.0f;
    if (master_frac <= 0.0f) return p;   // moving=false: master speed 0 holds
    float master_ramp = powf(1.0f - powf(1.0f - master_frac, SPEED_CURVE_EXP),
                             1.0f / SPEED_CURVE_EXP);

    int  sc        = (int)stroke_count;
    bool in_stroke = (stroke_count % 2u) == 0u;
    p.moving = true;
    if (in_stroke) {
        p.target_frac = max_depth.normalizedModified(sc);
        p.speed_frac  = master_ramp * in_speed.normalizedModified(sc);
        p.accel_knob  = in_accel.rampedModified(ACCEL_CURVE_EXP, sc);
    } else {
        p.target_frac = min_depth.normalizedModified(sc);
        p.speed_frac  = master_ramp * out_speed.normalizedModified(sc);
        p.accel_knob  = out_accel.rampedModified(ACCEL_CURVE_EXP, sc);
    }
    if (p.target_frac < 0.0f) p.target_frac = 0.0f;
    if (p.target_frac > 1.0f) p.target_frac = 1.0f;
    return p;
}

} // namespace advpat
