#include "range_mapper.h"

RangeMapper::RangeMapper()
    : _range_min_mm(0.0f)
    , _range_max_mm(PHYSICAL_MAX_TRAVEL_MM) {}

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

float RangeMapper::clampToPhysicalLimits(float pos_mm) {
    return constrain(pos_mm, 0.0f, PHYSICAL_MAX_TRAVEL_MM);
}

float RangeMapper::clampIntensity(float intensity) {
    return constrain(intensity, 0.0f, 1.0f);
}