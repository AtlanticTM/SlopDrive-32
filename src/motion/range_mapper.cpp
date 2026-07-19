#include "range_mapper.h"

RangeMapper::RangeMapper()
    : _range_min_mm(0.0f)
    , _range_max_mm(DEFAULT_MAX_RAIL_MM)
    , _max_rail_mm(DEFAULT_MAX_RAIL_MM) {}

// Update the physical ceiling and re-clamp the current window to it so a
// shortened rail immediately trims an over-long window. :3
void RangeMapper::setMaxRailMm(float mm) {
    if (mm <= 0.0f) return;
    _max_rail_mm = mm;
    setRange(_range_min_mm, _range_max_mm);   // re-clamp to the new ceiling
}

void RangeMapper::setRange(float min_mm, float max_mm) {
    // Validate range
    if (min_mm > max_mm) {
        std::swap(min_mm, max_mm);
    }

    // Clamp to physical limits
    _range_min_mm = clampToPhysicalLimits(min_mm);
    _range_max_mm = clampToPhysicalLimits(max_mm);

    // Ensure at least some minimum range size
    if (_range_max_mm - _range_min_mm < 5.0f) {
        _range_max_mm = _range_min_mm + 5.0f;
    }
}

// Take a raw 0..1 intensity value from Intiface and ram it deep into our
// physical range, stretching from the rearmost snuggle spot all the way to
// the forwardmost bulge. The higher the intensity, the deeper we pump it —
// at 1.0 he's absolutely packed to the hilt with nowhere left to go. :3
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
    // ConfigStore/WebUI (default DEFAULT_MAX_RAIL_MM = 500mm). :3
    return constrain(pos_mm, 0.0f, _max_rail_mm);
}

float RangeMapper::clampIntensity(float intensity) {
    return constrain(intensity, 0.0f, 1.0f);
}