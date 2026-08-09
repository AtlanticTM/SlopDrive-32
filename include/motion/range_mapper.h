#pragma once

#include "config_api.h"

// RangeMapper — maps Buttplug intensity (0.0-1.0) to physical positions.
// User configures range_min_mm/range_max_mm (usable window) and
// max_speed_mm_s (movement ceiling). Intensity maps linearly onto
// [range_min_mm, range_max_mm].
class RangeMapper {
public:
    RangeMapper();

    // Runtime edits set the GOAL; the effective window GLIDES via tick()
    // (a snapped window = a violent one-sample reposition, see sd-ey0).
    void setRange(float min_mm, float max_mm);
    void setRangeImmediate(float min_mm, float max_mm);   // boot/load only
    void tick(float dt_s, float rate_mm_s);   // ONE caller: motorTask ~1 kHz

    // Effective (gliding) window: MOTION consumers read these.
    float getMinMm() const { return _range_min_mm; }
    float getMaxMm() const { return _range_max_mm; }
    float getRangeSize() const { return _range_max_mm - _range_min_mm; }
    // Goal window: echo/persist/broadcast read these, never the gliding pair.
    float getGoalMinMm() const { return _goal_min_mm; }
    float getGoalMaxMm() const { return _goal_max_mm; }

    // Map a Buttplug intensity (0.0-1.0) to a physical position in mm
    // This is the core function: intensity 0.0 -> range_min, 1.0 -> range_max
    float intensityToPosition(float intensity) const;

    // Map a physical position back to an intensity value (inverse)
    float positionToIntensity(float pos_mm) const;

    // Get the center position of the configured range
    float getCenterPosition() const;

    // Set the max rail length (mm) — the rail-length-agnostic physical ceiling
    // the window is clamped to. Pushed from ConfigStore/WebUI whenever the user
    // changes the max rail length setting. Re-clamps the current range.
    void setMaxRailMm(float mm);
    float getMaxRailMm() const { return _max_rail_mm; }

    // Validate and clamp a position to physical limits (0 .. max rail length)
    float clampToPhysicalLimits(float pos_mm) const;

    // Validate and clamp an intensity value
    static float clampIntensity(float intensity);

private:
    float _range_min_mm;  // Start of usable range (mm from home) -- gliding
    float _range_max_mm;  // End of usable range (mm from home) -- gliding
    float _goal_min_mm;   // Requested window; tick() glides toward it
    float _goal_max_mm;
    float _max_rail_mm;   // Physical ceiling (mm) — user-set max rail length
};