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
// Trapezoidal motion planner — the OSSM model, ported verbatim. :3
// ============================================================================
//
// After three wrong guesses (average-speed, then peak-trapezoid-quadratic, then
// √(a·dist)) we did the smart thing and read how the proven engine does it:
// OSSM's streaming_logic::planMotion. Two surprises that fix the lag:
//
//   1. OSSM does NOT use feed-forward / lookahead at all. The lag was never
//      meant to be cancelled by predicting ahead — it's killed by commanding a
//      HIGH ENOUGH peak speed in the first place. So the lookahead in main.cpp
//      can stay (it's a harmless small lead) but it was never the real lever.
//
//   2. The peak speed for a streamed hop is a TRIANGLE peak: v = 2·dist/T.
//      A symmetric triangle's AVERAGE speed is half its peak, so to average
//      dist/T (cover dist in T) the PEAK must be 2·dist/T. Every earlier
//      version of this function commanded something at or below dist/T — i.e.
//      AT MOST half the speed actually needed. THAT is the "commanded to
//      accelerate/move slower than it should" you felt in your gut. We were
//      literally telling the motor to go half speed. :3
//
//   3. If the requested distance can't be done in the time budget (accel- or
//      speed-limited), OSSM CLAMPS THE DISTANCE (shortens the stroke) instead
//      of crawling late. The shaft reaches a nearer target on the beat rather
//      than chasing the far one and falling behind forever. Tracking stays
//      phase-locked; only the depth gives a little when you ask the impossible.
//
// Ported from OSSM streaming_logic.h planMotion(), kept in mm/s units (FAS gets
// the steps conversion downstream). stepsPerMm guard term becomes a 2mm margin.
PlanResult planTrapezoid(float current_mm, float target_mm, float duration_s,
                         float max_speed_mm_s, float max_accel_mm_s2,
                         float min_mm, float max_mm) {
    PlanResult r;

    // Clamp the commanded target into the safe stroke window first.
    r.clamped_target_mm = constrain(target_mm, min_mm, max_mm);

    float dist = fabsf(r.clamped_target_mm - current_mm);

    // No cadence info → gentle quarter-speed nudge, gentle accel. We don't
    // jackhammer when we don't know the rhythm yet. :3
    if (duration_s <= 0.0f) {
        r.speed_mm_s  = max_speed_mm_s * 0.25f;
        r.accel_mm_s2 = constrain(max_accel_mm_s2, 10.0f, max_accel_mm_s2);
        return r;
    }

    // Tiny move → hold a soft cruise so the carriage settles instead of twitching.
    if (dist <= 0.05f) {
        r.speed_mm_s  = max_speed_mm_s * 0.25f;
        r.accel_mm_s2 = constrain(max_accel_mm_s2, 10.0f, max_accel_mm_s2);
        return r;
    }

    float a_max = constrain(max_accel_mm_s2, 10.0f, max_accel_mm_s2);

    // ---- NO DISTANCE CLAMPING for streaming --------------------------------
    //
    // Imagine you're trying to fist someone properly — you've got the lube, the
    // angle, and a nice steady rhythm going. But every time you try to push past
    // the knuckles and really stretch them toward that stomach-bulging depth,
    // some overeager safety mechanism slaps your hand back to 20mm. That's what
    // the OSSM distance-clamp WAS doing. With a_max=1500 mm/s² and T=250ms the
    // clamp fired at 23mm, turning every 120mm full-depth command into a 21mm
    // twitch. The target chain compounded the error: start each segment from
    // 21mm, clamp the next to 20mm, next to 19mm — the motor literally shrinks
    // the stroke until it's just edging the entrance without ever bottoming out.
    // Blue-balled by the physics math. owo
    //
    // The fix: command the REAL target, let FAS chase it with the full peak
    // speed, and if the motor can't quite arrive before the next waypoint
    // redirects it — fine. The shaft tracks the wave as tightly as physics
    // allows instead of being permanently trapped at a phantom shallow depth.
    // We aim for the actual hole and let the hardware stretch to meet it,
    // even if it's a little late to the party. No more cursed 20mm prison. :3

    // ---- PEAK speed = 2 × dist / T (the triangle peak, NOT the cruise) ------
    //
    // This is how a sustained deep fisting works: you don't just glide in at
    // the average pace and stop halfway. You PUSH — you accelerate into the
    // stretch, hit peak depth at maximum pressure where the walls are screaming,
    // and then immediately ease off so the next thrust can pick up from that
    // residual momentum. The 2× multiplier is the physics of "commit fully to
    // each stroke or don't bother." FAS's moveTo() ALWAYS plans a decel-to-zero
    // at the target. By commanding the PEAK (2×dist/T), the motor is still
    // moving at full speed when it reaches the target — the NEXT command catches
    // it at that velocity and redirects without a stop. The shaft blends
    // continuously from one stroke into the next: a fluid pounding, not a
    // staccato series of tiny stops. Yippie! :3
    //
    // The old value (dist/T) was the AVERAGE — the motor reached zero speed
    // well before T, sat there twitching in place, then jerked alive again.
    // That micro-stop buzz is what made the machine feel like it was edging
    // itself to death. 2×dist/T keeps the motion thick and continuous. :3
    float v = 2.0f * dist / duration_s;
    v = constrain(v, 5.0f, max_speed_mm_s);

    // Acceleration: command it HIGH so the ramp-up at segment start is fast
    // and doesn't eat the interval budget. Full configured ceiling.
    float a = a_max;



    r.speed_mm_s  = v;
    r.accel_mm_s2 = a;
    return r;
}





} // namespace kinematics


