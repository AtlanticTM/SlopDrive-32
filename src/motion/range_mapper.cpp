#include "range_mapper.h"

RangeMapper::RangeMapper()
    : _range_min_mm(0.0f)
    , _range_max_mm(MACHINE_MAX_TRAVEL_MM) {}

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

float RangeMapper::clampToPhysicalLimits(float pos_mm) {
    // Use MACHINE_MAX_TRAVEL_MM so the 57AIM build clamps to 260mm, not 240mm.
    // PHYSICAL_MAX_TRAVEL_MM is the TMC-era constant — it stays for the TMC
    // build but we can't use it here without breaking the 57AIM rail. :3
    return constrain(pos_mm, 0.0f, MACHINE_MAX_TRAVEL_MM);
}

float RangeMapper::clampIntensity(float intensity) {
    return constrain(intensity, 0.0f, 1.0f);
}