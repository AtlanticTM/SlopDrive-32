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
// Trapezoidal motion planner — OSSM pipeline, ported verbatim :3
// ============================================================================
//
// All units are NATIVE STEPPER STEPS — unit conversion happens once at the
// call site (motionConsumerTask), not scattered across the planner. This
// matches OSSM's streaming.cpp exactly.
//
// current_steps    : stepper->getCurrentPosition() — last commanded position
//                    (open-loop: no encoder feedback, same as OSSM)
// target_steps_raw : TCode position mapped to native steps (pre-clamp)
// time_s           : command interval in seconds (TCode I-parameter / 1000)
// speed_limit      : max speed in steps/s (config ceiling)
// accel_limit      : max accel in steps/s² (config ceiling)
// min_steps        : stroke window minimum (native steps, may be negative)
// max_steps        : stroke window maximum (native steps, may be negative)
//
// Returns a PlanResult with the back-calculated speed, acceleration, and
// clamped target ready to hand directly to FAS. :3
struct PlanResult {
    int32_t  target_steps;      // final clamped target in native steps
    uint32_t speed_steps_s;     // peak speed in steps/s  (2×dist/T triangle peak)
    uint32_t accel_steps_s2;    // back-calculated acceleration in steps/s²
    bool     distance_clamped;  // true if the move was shortened by the physics guard
};

PlanResult planTrapezoid(int32_t  current_steps,
                         int32_t  target_steps_raw,
                         float    time_s,
                         uint32_t speed_limit,
                         uint32_t accel_limit,
                         int32_t  min_steps,
                         int32_t  max_steps);

} // namespace kinematics


