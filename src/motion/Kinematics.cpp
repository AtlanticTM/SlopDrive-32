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

// Optional "ease" smoothing pulls the carrier toward an S-curve so the ends of
// each stroke feel softer (less of a hard reversal). ease 0..1 blends in.
float ease(float v, float ease_factor) {
    if (ease_factor <= 0.0f) return v;
    float s = v * v * (3.0f - 2.0f * v);   // smoothstep
    return v + (s - v) * ease_factor;
}

// Apply the selected easing curve to a linear progress t (0..1) -> shaped 0..1.
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

} // namespace kinematics