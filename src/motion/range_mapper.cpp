#include "range_mapper.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr Window kBootWindow{0.0f, DEFAULT_MAX_RAIL_MM};
}  // namespace

RangeMapper::RangeMapper()
    : _range(kBootWindow), _goal(kBootWindow), _max_rail_mm(DEFAULT_MAX_RAIL_MM) {}

// ---- fitToRail --------------------------------------------------------------
// The rail wins over the minimum span, in that order and never the reverse:
// widening a narrow window UP is only legal while max has room, otherwise min
// is pulled DOWN, and a rail shorter than the minimum span IS the window. A
// bare `max = min + MIN` with no re-clamp lets a rail-shrink edit push the
// window past the physical rail, which MotionArbiter::_clampToWindow then
// treats as legal travel for every non-MANUAL source (sd-tki.5).
Window RangeMapper::fitToRail(Window w, float rail_mm) {
    if (w.min_mm > w.max_mm) std::swap(w.min_mm, w.max_mm);
    w.min_mm = std::clamp(w.min_mm, 0.0f, rail_mm);
    w.max_mm = std::clamp(w.max_mm, 0.0f, rail_mm);
    if (w.max_mm - w.min_mm >= MIN_STROKE_WINDOW_MM) return w;
    if (rail_mm <= MIN_STROKE_WINDOW_MM) return {0.0f, rail_mm};
    if (w.min_mm + MIN_STROKE_WINDOW_MM <= rail_mm) {
        w.max_mm = w.min_mm + MIN_STROKE_WINDOW_MM;
    } else {
        w.max_mm = rail_mm;
        w.min_mm = rail_mm - MIN_STROKE_WINDOW_MM;
    }
    return w;
}

void RangeMapper::setMaxRailMm(float mm) {
    if (!(mm > 0.0f)) return;
    _max_rail_mm.store(mm, std::memory_order_relaxed);
    _goal.store(fitToRail(_goal.load(), mm));
    // The effective window is NOT written here. tick() is its sole runtime
    // producer (a second producer would break WindowSlot's publish ordering),
    // and it snaps to the rail there without a glide ramp -- a hard limit never
    // glides, it just does not land on this task.
}

void RangeMapper::setRange(float min_mm, float max_mm) {
    _goal.store(fitToRail({min_mm, max_mm}, _max_rail_mm.load(std::memory_order_relaxed)));
}

void RangeMapper::setRangeImmediate(float min_mm, float max_mm) {
    setRange(min_mm, max_mm);
    _range.store(_goal.load());
}

void RangeMapper::tick(float dt_s, float rate_mm_s) {
    const Window goal = _goal.load();
    const Window prev = _range.load();
    Window eff = prev;
    if (dt_s > 0.0f && rate_mm_s > 0.0f) {
        const float step = rate_mm_s * dt_s;
        const float dmin = goal.min_mm - eff.min_mm;
        const float dmax = goal.max_mm - eff.max_mm;
        // Lands exactly on the goal, never past it: sd-ey0's whole point.
        eff.min_mm += (fabsf(dmin) <= step) ? dmin : (dmin > 0.0f ? step : -step);
        eff.max_mm += (fabsf(dmax) <= step) ? dmax : (dmax > 0.0f ? step : -step);
    }
    // A rail SHRINK leaves the effective window outside the physical rail with
    // no goal delta to glide away; this is the one place that fixes it.
    const float rail = _max_rail_mm.load(std::memory_order_relaxed);
    eff.min_mm = std::clamp(eff.min_mm, 0.0f, rail);
    eff.max_mm = std::clamp(eff.max_mm, 0.0f, rail);
    if (eff.min_mm != prev.min_mm || eff.max_mm != prev.max_mm) _range.store(eff);
}

float RangeMapper::intensityToPosition(float intensity) const {
    const Window w = _range.load();
    return w.min_mm + clampIntensity(intensity) * (w.max_mm - w.min_mm);
}

float RangeMapper::positionToIntensity(float pos_mm) const {
    const Window w = _range.load();
    const float range = w.max_mm - w.min_mm;
    if (range <= 0.0f) return 0.0f;
    return clampIntensity((pos_mm - w.min_mm) / range);
}

float RangeMapper::getCenterPosition() const {
    const Window w = _range.load();
    return (w.min_mm + w.max_mm) * 0.5f;
}

// Rail-length agnostic: the ceiling is the user-set rail length pushed from
// ConfigStore/WebUI, never a hardcoded geometry constant.
float RangeMapper::clampToPhysicalLimits(float pos_mm) const {
    return std::clamp(pos_mm, 0.0f, _max_rail_mm.load(std::memory_order_relaxed));
}

float RangeMapper::clampIntensity(float intensity) {
    return std::clamp(intensity, 0.0f, 1.0f);
}
