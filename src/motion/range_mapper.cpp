#include "range_mapper.h"

RangeMapper::RangeMapper()
    : _range_min_mm(0.0f)
    , _range_max_mm(DEFAULT_MAX_RAIL_MM)
    , _goal_min_mm(0.0f)
    , _goal_max_mm(DEFAULT_MAX_RAIL_MM)
    , _max_rail_mm(DEFAULT_MAX_RAIL_MM) {}

// Update the physical ceiling and re-clamp the window to it. The physical
// bound clamps GOAL AND EFFECTIVE immediately -- a hard limit never glides.
void RangeMapper::setMaxRailMm(float mm) {
    if (mm <= 0.0f) return;
    _max_rail_mm = mm;
    setRange(_goal_min_mm, _goal_max_mm);
    _range_min_mm = clampToPhysicalLimits(_range_min_mm);
    _range_max_mm = clampToPhysicalLimits(_range_max_mm);
}

void RangeMapper::setRange(float min_mm, float max_mm) {
    if (min_mm > max_mm) {
        std::swap(min_mm, max_mm);
    }
    _goal_min_mm = clampToPhysicalLimits(min_mm);
    _goal_max_mm = clampToPhysicalLimits(max_mm);
    // Ensure at least some minimum range size
    if (_goal_max_mm - _goal_min_mm < 5.0f) {
        _goal_max_mm = _goal_min_mm + 5.0f;
    }
}

void RangeMapper::setRangeImmediate(float min_mm, float max_mm) {
    setRange(min_mm, max_mm);
    _range_min_mm = _goal_min_mm;
    _range_max_mm = _goal_max_mm;
}

void RangeMapper::tick(float dt_s, float rate_mm_s) {
    if (!(dt_s > 0.0f) || !(rate_mm_s > 0.0f)) return;
    const float step = rate_mm_s * dt_s;
    const float dmin = _goal_min_mm - _range_min_mm;
    const float dmax = _goal_max_mm - _range_max_mm;
    _range_min_mm += (fabsf(dmin) <= step) ? dmin : (dmin > 0 ? step : -step);
    _range_max_mm += (fabsf(dmax) <= step) ? dmax : (dmax > 0 ? step : -step);
}

float RangeMapper::intensityToPosition(float intensity) const {
    intensity = clampIntensity(intensity);
    return _range_min_mm + (intensity * getRangeSize());
}

float RangeMapper::positionToIntensity(float pos_mm) const {
    float range = getRangeSize();
    if (range <= 0.0f) return 0.0f;

    float intensity = (pos_mm - _range_min_mm) / range;
    return clampIntensity(intensity);
}

float RangeMapper::getCenterPosition() const {
    return (_range_min_mm + _range_max_mm) * 0.5f;
}

float RangeMapper::clampToPhysicalLimits(float pos_mm) const {
    // Rail-length agnostic: clamp to the user-set max rail length. There is no
    // hardcoded geometry ceiling anymore — _max_rail_mm is pushed from
    // ConfigStore/WebUI (default DEFAULT_MAX_RAIL_MM = 500mm).
    return constrain(pos_mm, 0.0f, _max_rail_mm);
}

float RangeMapper::clampIntensity(float intensity) {
    return constrain(intensity, 0.0f, 1.0f);
}