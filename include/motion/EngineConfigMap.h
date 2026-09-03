// EngineConfigMap -- the SystemState tuning set to kOpConfig tags, one home.
// Constraints:
// - Hardware-free and clock-free: no Arduino, no globals, no SystemState
//   reference. The caller snapshots the volatile sm_tune_* fields into an
//   EngineTuning and hands it in, which is what lets test/native/
//   test_engine_config compile this on the host.
// - buildConfigTags is PURE: equal EngineTuning in, equal tags out. The
//   DRIVER owns change detection (one tag ships once per change), so this
//   function is called freely and repeatedly.
// - Every tuning field here becomes exactly ONE tag. A ConfigTag this
//   function does not emit is engine-default BY DECISION; the list is
//   enumerated once, in the body, and nowhere else.
// - slopmotion.hpp is included for the ENUM ORDINALS only, which ride the
//   wire verbatim (MotionLinkProtocol.h, ConfigTag). Transcribing them here
//   would be the T20 hand-copied vocabulary; nothing else in this file
//   touches the engine, and no Engine is instantiated on the S3.
// - Out-of-range enum ordinals fall through to the engine's own default,
//   never to an arbitrary policy.
// See: docs/rp-motion-port.md, include/comms/MotionLinkProtocol.h,
//      dev board sd-4k1.4.
#pragma once

#include <array>
#include <cstdint>

#include "MotionLinkProtocol.h"
#include "slopmotion/slopmotion.hpp"

namespace slopdrive {

// Plain snapshot of everything the map reads. Aggregate of scalars only, so
// the defaulted operator== is a memberwise compare with no padding in it.
// Defaults mirror SystemState's sm_tune_* so EngineTuning{} is the boot set.
struct EngineTuning {
    // Stroke window span in mm. 1 normalized engine unit == this span, so the
    // four mm-domain ceilings divide by it. <= 1.0 means "no usable window":
    // the tags then carry 0, which the slave reads as unconfigured and gates
    // rather than planning at zero.
    float    span_mm            = 0.0f;
    float    input_max_speed    = 0.0f;   // mm/s
    float    input_max_accel    = 0.0f;   // mm/s^2
    float    input_max_jerk     = 0.0f;   // mm/s^3
    float    user_max_speed     = 0.0f;   // mm/s, the gentle (recovery) ceiling
    float    user_max_accel     = 0.0f;   // mm/s^2
    float    vmax_ovr           = 0.0f;   // >0 overrides the derived normalized ceiling
    float    amax_ovr           = 0.0f;
    float    jmax_ovr           = 0.0f;
    bool     chase_ff           = true;
    bool     chase_aff          = true;
    bool     aim_extrap         = true;
    float    chase_gain         = 0.9f;
    float    chase_look         = 3.0f;
    uint32_t dense_us           = 60000;
    uint8_t  infeas_policy      = 0;      // stored ordinal, see the map below
    float    infeas_blend       = 0.5f;
    float    smooth_budget      = 0.5f;
    float    amp_budget         = 0.5f;
    uint8_t  blend_steps        = 6;
    uint8_t  curve_policy       = 0;      // slopmotion::CurvePolicy ordinal
    float    handoff_k          = 1.5f;
    uint32_t settle_grace_us    = 30000;

    bool operator==(const EngineTuning&) const = default;
};

// The three INPUT ceilings in normalized units, which is what the wire and
// the engine speak. ONE home: buildConfigTags emits these and the host's
// sm_eff_* readout reads the same call rather than repeating the arithmetic.
// ALL THREE derive the same way: a bare normalized jerk constant made the
// PHYSICAL jerk ceiling shrink as the operator narrowed the window and
// silently bound fast segments.
struct NormalizedLimits {
    float vmax = 0.0f;
    float amax = 0.0f;
    float jmax = 0.0f;
};
inline NormalizedLimits normalizedLimits(const EngineTuning& t) {
    const bool span_ok = t.span_mm > 1.0f;
    NormalizedLimits l;
    l.vmax = t.vmax_ovr > 0.0f ? t.vmax_ovr
           : (span_ok ? t.input_max_speed / t.span_mm : 0.0f);
    l.amax = t.amax_ovr > 0.0f ? t.amax_ovr
           : (span_ok ? t.input_max_accel / t.span_mm : 0.0f);
    l.jmax = t.jmax_ovr > 0.0f ? t.jmax_ovr
           : (span_ok ? t.input_max_jerk / t.span_mm : 0.0f);
    return l;
}

// Every tag this map emits. Growing the tuning grows this number, which is
// what makes the host-side census (test/native/test_engine_config) a census.
inline constexpr size_t kEngineConfigTagCount = 19;

inline std::array<motionlink::ConfigField, kEngineConfigTagCount>
buildConfigTags(const EngineTuning& t) {
    using namespace motionlink;

    // ENGINE-DEFAULT BY DECISION -- tags the vocabulary defines and this host
    // deliberately does not drive, so the slave's own value is the policy.
    // Exposing a knob means moving a name off this list, never writing the
    // tag somewhere else:
    //   kCfgOvershootGuard, kCfgOvershootChordSlack, kCfgChaseStaleUs.
    // RETIRED and never emitted: kCfgSampleSynthesis (synthesis left the
    // engine 2026-09-03; a slave ignores it).

    const NormalizedLimits lim = normalizedLimits(t);
    const bool span_ok = t.span_mm > 1.0f;

    // The USER set is the gentle pair and it has NO jerk of its own, because
    // the UI has none to offer: user-set plans take the INPUT jerk ceiling,
    // so jerk stays one fact with one home (MotionLinkProtocol.h, ConfigTag).
    const float user_v = span_ok ? t.user_max_speed / t.span_mm : 0.0f;
    const float user_a = span_ok ? t.user_max_accel / t.span_mm : 0.0f;

    // Infeasible-segment policy. TWO policies, but the stored ordinal runs
    // 0..5: the catalog select's wire value is 0 = stretch / 1 = blend, and
    // 2..5 are ORDINALS OF POLICIES DELETED 2026-09-02 that an older NVS or an
    // older client may still hold. Every one of them ran a timing-first
    // amplitude/shape trade, which is what Blend is, so they map there rather
    // than silently reverting an operator to Stretch. Out of range falls
    // through to the ENGINE default (also Blend), never to an arbitrary policy
    // (fw 2.1.49: a boolean map could only produce two of three, so the third
    // was unreachable). The host logs the retired case once; the engine and
    // this map stay log-free.
    uint8_t policy = uint8_t(slopmotion::InfeasiblePolicy::Blend);
    if (t.infeas_policy == 0) policy = uint8_t(slopmotion::InfeasiblePolicy::Stretch);
    else if (t.infeas_policy > slopmotion::kInfeasiblePolicyMax)
        policy = uint8_t(slopmotion::Config{}.infeasible_policy);

    // Curve family for waveform-segment reconstruction. Same fall-through.
    uint8_t curve = uint8_t(slopmotion::Config{}.curve_policy);
    switch (t.curve_policy) {
        case 0: curve = uint8_t(slopmotion::CurvePolicy::FollowClient); break;
        case 1: curve = uint8_t(slopmotion::CurvePolicy::ForceC1);      break;
        case 2: curve = uint8_t(slopmotion::CurvePolicy::ForceC2);      break;
        default: break;
    }

    return {{
        {kCfgInputVmax,          f32Bits(lim.vmax)},
        {kCfgInputAmax,          f32Bits(lim.amax)},
        {kCfgInputJmax,          f32Bits(lim.jmax)},
        {kCfgUserVmax,           f32Bits(user_v)},
        {kCfgUserAmax,           f32Bits(user_a)},
        {kCfgSettleGraceUs,      t.settle_grace_us},
        {kCfgInfeasiblePolicy,   policy},
        // The engine clamps both budgets to [0,1], the step count to [1,10]
        // and the blend slider to [0,1] itself, so pushing what the host
        // stored is safe: the intake clamp keeps the ECHO honest, it does not
        // protect the slave.
        {kCfgInfeasibleBlend,    f32Bits(t.infeas_blend)},
        {kCfgCurvePolicy,        curve},
        // The RFC-008 handoff sanity guard (0 = off). The guard only engages
        // when the INGRESS supplied a one-segment lookahead, so this knob is
        // the aggressiveness dial plus off switch, never the arming condition.
        {kCfgHandoffChordFactor, f32Bits(t.handoff_k)},
        {kCfgBlendSteps,         t.blend_steps},
        {kCfgSmoothBudget,       f32Bits(t.smooth_budget)},
        {kCfgAmplitudeBudget,    f32Bits(t.amp_budget)},
        {kCfgChaseFeedforward,   uint32_t(t.chase_ff)},
        {kCfgChaseAccelFf,       uint32_t(t.chase_aff)},
        {kCfgChaseFfGain,        f32Bits(t.chase_gain)},
        {kCfgChaseDenseUs,       t.dense_us},
        {kCfgChaseLookahead,     f32Bits(t.chase_look)},
        {kCfgChaseAimExtrap,     uint32_t(t.aim_extrap)},
    }};
}

}  // namespace slopdrive
