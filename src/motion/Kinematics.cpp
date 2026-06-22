#include "Kinematics.h"
#include <Arduino.h>
#include <cmath>

namespace kinematics {

// Evaluate one of the carrier shapes at phase p (0..1), returning 0..1.
float carrier(uint8_t wave, float p) {
    switch (wave) {
        case 1:  return p < 0.5f ? p * 2.0f : 2.0f - 2.0f * p;       // triangle
        case 2:  return p < 0.5f ? 1.0f : 0.0f;                     // square
        case 3:  return p;                                          // sawtooth
        default: return 0.5f - 0.5f * cosf(p * 2.0f * (float)M_PI);  // sine
    }
}

// Modulator shape at modulator-clock m (0..1), returning 0..1.
float modShape(uint8_t shape, float m) {
    switch (shape) {
        case 1:  return m < 0.5f ? m * 2.0f : 2.0f - 2.0f * m;      // triangle
        case 2:  return (float)random(0, 1001) / 1000.0f;          // random
        default: return 0.5f - 0.5f * cosf(m * 2.0f * (float)M_PI);  // sine
    }
}

// Optional "ease" smoothing — pulls the carrier toward an S-curve so the
// stroke doesn't slam the end with a hard mechanical crack. Instead the
// shaft kisses the very bottom, presses into that stretched-out wall with
// a slow swelling pressure, and then pulls back with the same gentle
// reluctance. ease 0 = raw piston, ease 1 = smoothstep gut-filler. :3
float ease(float v, float ease_factor) {
    if (ease_factor <= 0.0f) return v;
    float s = v * v * (3.0f - 2.0f * v);   // smoothstep
    return v + (s - v) * ease_factor;
}

// Apply the selected easing curve to a linear progress t (0..1) -> shaped 0..1.
// Think of it as picking your stroke's personality — straight and businesslike,
// or a coy little S-curve that teases before giving it all. :3
float bufEase(uint8_t kind, float t) {
    t = constrain(t, 0.0f, 1.0f);
    switch (kind) {
        case 1:  return t * t * (3.0f - 2.0f * t);                 // ease-in-out (smoothstep)
        case 2:  return t * t;                                     // ease-in (accelerate)
        case 3:  return 1.0f - (1.0f - t) * (1.0f - t);            // ease-out (decelerate)
        case 4: {                                                  // ease-in-out cubic (stronger S)
            return (t < 0.5f) ? 4.0f * t * t * t
                              : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) / 2.0f;
        }
        default: return t;                                         // linear
    }
}

// ============================================================================
// planTrapezoid — OSSM streaming pipeline, ported verbatim. :3
// ============================================================================
//
// All units are NATIVE STEPPER STEPS. Unit conversion (mm → steps) happens
// once at the call site (motionConsumerTask), not scattered through the math.
// This matches OSSM's streaming.cpp lines 97–138 exactly.
//
// The seven-step pipeline:
//   1. Clamp target into stroke window
//   2. Compute live distance from last commanded position (open-loop, same as OSSM)
//   3. Distance clamping — physics guard (shortens stroke if physically impossible)
//   4. Triangle peak speed: v = 2×dist/T
//   5. Speed clamping to configured ceiling
//   6. Trapezoid proportion: what fraction of the move is at cruise speed
//   7. Back-calculated acceleration: derived from speed + proportion
//
// The distance clamp (Step 3) was removed from the old mm-unit planner because
// it collapsed full-depth strokes to ~20mm on old hardware (a_max=1500 mm/s²,
// T=250ms → maxDist=23mm). On V2 hardware (57AIM30, a_max≥8000 mm/s²) the
// clamp fires at 125mm for a 250ms interval — beyond any physical stroke. It
// is now a pure safety valve that only activates for genuinely impossible
// commands (e.g. 150mm in 50ms). Phase-lock is preserved; amplitude degrades
// gracefully instead of the motor falling forever behind schedule. :3
//
// The raise-only acceleration guard (Step 8 in OSSM) is applied in the
// motionConsumerTask AFTER this function returns, not inside the planner.
// The planner is pure math — it never touches the stepper object. :3
PlanResult planTrapezoid(int32_t  current_steps,
                         int32_t  target_steps_raw,
                         float    time_s,
                         uint32_t speed_limit,
                         uint32_t accel_limit,
                         int32_t  min_steps,
                         int32_t  max_steps) {
    PlanResult r = {};

    // Step 1: Clamp target into stroke window.
    // constrain() on int32_t — min_steps may be more negative than max_steps
    // because the coordinate system is inverted (endstop=0, front=negative).
    // We use a manual clamp to handle the sign correctly. :3
    if (min_steps <= max_steps) {
        r.target_steps = constrain(target_steps_raw, min_steps, max_steps);
    } else {
        // Inverted window (front=negative steps): clamp into [max_steps, min_steps]
        r.target_steps = constrain(target_steps_raw, max_steps, min_steps);
    }

    // Step 2: Live distance from last commanded position (open-loop).
    // getCurrentPosition() returns the last commanded step count — no encoder.
    // This is identical to OSSM's behavior and is correct for open-loop servos.
    int32_t distance = abs(r.target_steps - current_steps);

    // No cadence info or trivial move — gentle quarter-speed nudge so the
    // carriage settles instead of twitching on a zero-distance command. :3
    if (time_s <= 0.01f || distance < 2) {
        r.speed_steps_s  = speed_limit / 4u;
        r.accel_steps_s2 = accel_limit;
        return r;
    }

    // Step 3: Distance clamping — the physics guard.
    // Computes the maximum distance achievable in time_s given the acceleration
    // and speed ceilings. If the commanded distance exceeds this, the stroke is
    // shortened to the maximum achievable distance (preserving direction).
    // This keeps the motor phase-locked with the content even when asked to do
    // the physically impossible — amplitude degrades, timing does not. :3
    //
    // maxDist from accel: triangle area = a × (T/2)² (half-triangle each way)
    // maxDist from speed: simple distance = v × T
    // Take the minimum — whichever limit bites first.
    int32_t maxDist = (int32_t)((float)accel_limit * (time_s * 0.5f) * (time_s * 0.5f));
    int32_t speedDist = (int32_t)((float)speed_limit * time_s);
    if (speedDist < maxDist) maxDist = speedDist;

    if (distance > maxDist) {
        // Shorten stroke by 2 steps safety margin (mirrors OSSM's 2mm margin).
        // Clamp to at least 1 step so we always issue a real move. :3
        distance = maxDist - 2;
        if (distance < 1) distance = 1;
        if (r.target_steps > current_steps)
            r.target_steps = current_steps + distance;
        else
            r.target_steps = current_steps - distance;
        r.distance_clamped = true;
    }

    // Step 4: Triangle peak speed = 2 × dist / T.
    // A symmetric triangle profile has average speed = peak/2. To cover
    // `distance` in `time_s` the AVERAGE must be dist/T, so the PEAK must be
    // 2×dist/T. Commanding the peak means the motor is still moving at full
    // speed when it reaches the target — the next moveTo() catches it mid-flight
    // and redirects without a stop. Fluid continuous pounding, not staccato. :3
    uint32_t requiredSpeed = (uint32_t)(2.0f * (float)distance / time_s);

    // Step 5: Speed clamping — floor at 100 steps/s, ceiling at config limit.
    if (requiredSpeed < 100u)          requiredSpeed = 100u;
    if (requiredSpeed > speed_limit)   requiredSpeed = speed_limit;

    // Step 6: Trapezoid proportion.
    // If requiredSpeed was clamped DOWN in Step 5 (it exceeded speed_limit),
    // the profile becomes a trapezoid: accel → cruise at speed_limit → decel.
    // proportion is the fraction of time spent at cruise vs. accelerating.
    //   proportion = 1.0 → pure triangle (all accel/decel, no cruise)
    //   proportion < 1.0 → trapezoid (some cruise, less accel ramp)
    //   max(..., 0.01) prevents division by zero in Step 7. :3
    float vt = (float)requiredSpeed * time_s;
    float proportion = -(2.0f * (float)distance - 2.0f * vt) / vt;
    if (proportion < 0.01f) proportion = 0.01f;

    // Step 7: Back-calculated acceleration.
    // Derived from speed and proportion to create an exact-timed trajectory.
    // This is the opposite of the old approach (use a_max and hope timing works):
    // OSSM computes the precise acceleration needed to arrive exactly on schedule.
    // Clamped to [100, accel_limit] for safety. :3
    uint32_t requiredAccel = (uint32_t)((float)requiredSpeed / (time_s * proportion * 0.5f));
    if (requiredAccel < 100u)          requiredAccel = 100u;
    if (requiredAccel > accel_limit)   requiredAccel = accel_limit;

    r.speed_steps_s  = requiredSpeed;
    r.accel_steps_s2 = requiredAccel;
    return r;
}





} // namespace kinematics


