#pragma once

#include <cstdint>

// ============================================================================
// kinematics — Pure, stateless waveform/carrier/easing math (FPU float).
// No hardware, RTOS, or state dependencies. Free functions in a namespace.
// ============================================================================
namespace kinematics {

// Evaluate one of the carrier shapes at phase p (0..1), returning 0..1.
//   wave: 0=sine  1=triangle  2=square  3=sawtooth
float carrier(uint8_t wave, float p);

// Modulator shape at modulator-clock m (0..1), returning 0..1.
//   shape: 0=sine  1=triangle  2=random
float modShape(uint8_t shape, float m);

// Optional "ease" smoothing pulls the carrier toward an S-curve so the ends of
// each stroke feel softer (less of a hard reversal). ease 0..1 blends in.
float ease(float v, float ease_factor);

// Apply the selected easing curve to a linear progress t (0..1) -> shaped 0..1.
//   kind: 0=linear  1=ease-in-out(smoothstep)  2=ease-in(accel)
//         3=ease-out(decel)  4=ease-in-out-cubic(stronger S)
float bufEase(uint8_t kind, float t);

// ============================================================================
// Trapezoidal motion planner — the ONE TRUE PATH now :3
// ============================================================================
//
// Given where the shaft is, where the host wants it, and how long it has to get
// there, solve for the cruise speed (and a sane acceleration) of a classic
// trapezoidal velocity profile so we bottom out right on the beat — no jitter
// buffer foreplay, no predictive coast, just honest planned thrusting.
//
// All inputs/outputs are physical millimetres / mm·s⁻¹ / mm·s⁻². The target is
// clamped into [min_mm, max_mm] before the math so we never plan to ram past
// the safe stroke window. duration_s <= 0 means "no cadence info" → we fall back
// to a gentle quarter-speed nudge toward the target.
struct PlanResult {
    float clamped_target_mm;   // target after window clamp
    float speed_mm_s;          // planned cruise speed (>= 0)
    float accel_mm_s2;         // planned acceleration (>= a floor)
};

PlanResult planTrapezoid(float current_mm, float target_mm, float duration_s,
                         float max_speed_mm_s, float max_accel_mm_s2,
                         float min_mm, float max_mm);

} // namespace kinematics


