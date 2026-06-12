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

} // namespace kinematics