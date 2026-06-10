#pragma once

#include "config.h"

/**
 * RangeMapper - Maps Buttplug intensity (0.0-1.0) to physical positions
 *
 * The user configures:
 * - range_min_mm: Start of the usable range (e.g., 0mm)
 * - range_max_mm: End of the usable range (e.g., 120mm out of 240mm max)
 * - max_speed_mm_s: Maximum speed for movements
 *
 * Buttplug sends intensity values 0.0-1.0 which are mapped linearly
 * to [range_min_mm, range_max_mm].
 */
class RangeMapper {
public:
    RangeMapper();

    // Set the usable position range in mm
    void setRange(float min_mm, float max_mm);

    // Get current range settings
    float getMinMm() const { return _range_min_mm; }
    float getMaxMm() const { return _range_max_mm; }
    float getRangeSize() const { return _range_max_mm - _range_min_mm; }

    // Map a Buttplug intensity (0.0-1.0) to a physical position in mm
    // This is the core function: intensity 0.0 -> range_min, 1.0 -> range_max
    float intensityToPosition(float intensity) const;

    // Map a physical position back to an intensity value (inverse)
    float positionToIntensity(float pos_mm) const;

    // Get the center position of the configured range
    float getCenterPosition() const;

    // Validate and clamp a position to physical limits
    static float clampToPhysicalLimits(float pos_mm);

    // Validate and clamp an intensity value
    static float clampIntensity(float intensity);

private:
    float _range_min_mm;  // Start of usable range (mm from home)
    float _range_max_mm;  // End of usable range (mm from home)
};