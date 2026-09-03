// EngineConfigMap -- the SystemState tuning set to slopmotion::Config, one home.
// Constraints:
// - Hardware-free and clock-free: no Arduino, no globals, no SystemState
//   reference. The caller snapshots the volatile sm_tune_* fields into an
//   EngineTuning and hands it in, which is what lets test/native/
//   test_engine_config compile this on the host.
// - buildEngineConfig is PURE: equal EngineTuning in, equal Config out. That
//   is what makes comparing the TUNING enough to decide whether to push, so
//   the caller never memcmps a Config (padding is not part of its value).
// - Every Config field this function does not write is engine-default BY
//   DECISION; the list is enumerated once, in the body, and nowhere else.
// - Out-of-range enum ordinals fall through to the engine's own default,
//   never to an arbitrary policy.
// See: docs/reviews/slopmotion-2026-09-02/05-engine-host-contract.md section 1,
//      dev board sd-6b2.4.
#pragma once

#include <cstdint>

#include "slopmotion/slopmotion.hpp"

namespace slopdrive {

// Plain snapshot of everything the map reads. Aggregate of scalars only, so
// the defaulted operator== is a memberwise compare with no padding in it.
// Defaults mirror SystemState's sm_tune_* so EngineTuning{} is the boot set.
struct EngineTuning {
    // Stroke window span in mm. 1 normalized engine unit == this span, so the
    // three mm-domain ceilings divide by it. <= 1.0 means "no usable window":
    // the engine's own Limits defaults stand instead of a divide by nothing.
    float    span_mm            = 0.0f;
    float    input_max_speed    = 0.0f;   // mm/s
    float    input_max_accel    = 0.0f;   // mm/s^2
    float    input_max_jerk     = 0.0f;   // mm/s^3
    float    user_max_speed     = 0.0f;   // mm/s, the cold-start (recovery) ceiling
    float    vmax_ovr           = 0.0f;   // >0 overrides the derived normalized ceiling
    float    amax_ovr           = 0.0f;
    float    jmax_ovr           = 0.0f;
    bool     chase_ff           = true;
    bool     chase_aff          = true;
    bool     aim_extrap         = true;
    float    chase_gain         = 0.9f;
    float    chase_look         = 3.0f;
    uint32_t dense_us           = 60000;
    uint8_t  infeas_policy      = 0;      // slopmotion::InfeasiblePolicy ordinal
    float    infeas_margin      = 0.92f;
    float    infeas_blend       = 0.5f;
    uint8_t  reshape_steps      = 6;
    float    smooth_budget      = 0.5f;
    float    amp_budget         = 0.5f;
    uint8_t  blend_steps        = 6;
    uint8_t  curve_policy       = 0;      // slopmotion::CurvePolicy ordinal
    bool     centering          = true;
    float    centering_gain     = 1.0f;
    float    handoff_k          = 1.5f;
    uint32_t settle_grace_us    = 30000;

    bool operator==(const EngineTuning&) const = default;
};

inline slopmotion::Config buildEngineConfig(const EngineTuning& t) {
    slopmotion::Config c;   // engine defaults; anything left alone below is one

    // ENGINE-DEFAULT BY DECISION -- the host deliberately does not expose
    // these, so the engine's own value is the policy. Adding a knob means
    // moving a name off this list, never writing the field somewhere else:
    //   chase_jerk_scale, chase_jerk_floor, chase_stale_us,
    //   overshoot_guard, overshoot_chord_slack, infeasible_soften,
    //   infeasible_soften_floor, infeasible_soften_steps, bridge_ratio.

    // Ceilings derive from the mm-domain INPUT limit set over the stroke
    // window (1 normalized unit == the window span), overridable for bench
    // tuning. ALL THREE derive the same way: a bare normalized jerk constant
    // made the PHYSICAL jerk ceiling shrink as the operator narrowed the
    // window and silently bound fast segments.
    const bool span_ok = t.span_mm > 1.0f;
    c.limits.vmax = t.vmax_ovr > 0.0f ? t.vmax_ovr
                  : (span_ok ? t.input_max_speed / t.span_mm : c.limits.vmax);
    c.limits.amax = t.amax_ovr > 0.0f ? t.amax_ovr
                  : (span_ok ? t.input_max_accel / t.span_mm : c.limits.amax);
    c.limits.jmax = t.jmax_ovr > 0.0f ? t.jmax_ovr
                  : (span_ok ? t.input_max_jerk / t.span_mm : c.limits.jmax);
    // Cold-start plans run at the USER (gentle) limit: the opening move of a
    // stream is positioning, not content (sd-d77).
    c.recovery_vmax = span_ok ? t.user_max_speed / t.span_mm : 0.0f;

    c.chase_feedforward      = t.chase_ff;
    c.chase_accel_ff         = t.chase_aff;
    c.chase_ff_gain          = t.chase_gain;
    c.chase_lookahead        = t.chase_look;
    c.chase_dense_us         = t.dense_us;
    c.chase_aim_accel_extrap = t.aim_extrap;

    // Infeasible-segment policy: a 3-WAY map, not a boolean. An out-of-range
    // stored value falls through to the ENGINE default, never to an arbitrary
    // policy (fw 2.1.49: a boolean map could only produce Scale or Stretch, so
    // Reshape was unreachable).
    switch (t.infeas_policy) {
        case 0: c.infeasible_policy = slopmotion::InfeasiblePolicy::Stretch; break;
        case 1: c.infeasible_policy = slopmotion::InfeasiblePolicy::Scale;   break;
        case 2: c.infeasible_policy = slopmotion::InfeasiblePolicy::Reshape; break;
        case 3: c.infeasible_policy =
                    slopmotion::InfeasiblePolicy::PrioritizeAmplitude;       break;
        case 4: c.infeasible_policy =
                    slopmotion::InfeasiblePolicy::PrioritizeSmooth;          break;
        case 5: c.infeasible_policy = slopmotion::InfeasiblePolicy::Blend;   break;
        default: break;
    }
    c.infeasible_scale_margin  = t.infeas_margin;
    c.infeasible_reshape_steps = t.reshape_steps;
    c.settle_grace_us          = t.settle_grace_us;
    // Budgeted-policy spend limits + alpha-search depth. Inert unless
    // infeasible_policy is one of the budgeted ones; the engine clamps both
    // budgets to [0,1] and the step count to [1,10] itself, so pushing what the
    // host stored is safe -- the clamp on the intake side keeps the echo
    // honest, it does not protect the engine.
    c.infeasible_smooth_budget    = t.smooth_budget;
    c.infeasible_amplitude_budget = t.amp_budget;
    c.infeasible_blend_steps      = t.blend_steps;
    // Blend's one slider: what an infeasible segment gives up, as an exchange
    // rate between amplitude and shape. Clamped [0,1] by the engine.
    c.infeasible_blend            = t.infeas_blend;

    // Curve family for waveform-segment reconstruction. Same fall-through rule
    // as the policy map above.
    switch (t.curve_policy) {
        case 0: c.curve_policy = slopmotion::CurvePolicy::FollowClient; break;
        case 1: c.curve_policy = slopmotion::CurvePolicy::ForceC1;      break;
        case 2: c.curve_policy = slopmotion::CurvePolicy::ForceC2;      break;
        default: break;
    }

    // DC centering of a degraded band, and the RFC-008 handoff sanity guard
    // (0 = off). The guard only engages when the INGRESS supplied a
    // one-segment lookahead, so this knob is the aggressiveness dial plus off
    // switch, never the arming condition. Both are engine-clamped.
    c.wave_centering       = t.centering;
    c.wave_centering_gain  = t.centering_gain;
    c.handoff_chord_factor = t.handoff_k;

    return c;
}

}  // namespace slopdrive
