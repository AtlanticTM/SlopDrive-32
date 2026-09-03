// test_engine_config -- host suite for the tuning-to-slopmotion::Config map.
// Constraints:
// - Guards sd-6b2.4: a field the host means to expose but forgets to write is
//   silently the engine default forever. Every mapped field is asserted to
//   MOVE off its default under a sentinel input, and every deliberately
//   unmapped field is asserted to STAY on it. The two lists together are the
//   whole struct; the sizeof tripwire at the bottom fails when the engine
//   grows a field so the census gets revisited instead of silently rotting.
// - Pure float math, no clock, no hardware: deterministic, and must stay that
//   way. slopmotion.hpp is included first so the native env's LDF resolves
//   lib/slopmotion from this directory.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopmotion/slopmotion.hpp"

#include "EngineConfigMap.h"

#include <cstdint>

using slopdrive::EngineTuning;
using slopdrive::buildEngineConfig;

namespace {

// Every input at a value the engine default is NOT, so "field moved" is a
// clean signal. Span is 100 mm, so a mm-domain ceiling divides by 100.
EngineTuning sentinelTuning() {
    EngineTuning t;
    t.span_mm         = 100.0f;
    t.input_max_speed = 700.0f;      // -> vmax 7 (default 3)
    t.input_max_accel = 9000.0f;     // -> amax 90 (default 30)
    t.input_max_jerk  = 120000.0f;   // -> jmax 1200 (default 500)
    t.user_max_speed  = 250.0f;      // -> recovery_vmax 2.5 (default 0)
    t.chase_ff        = false;       // default true
    t.chase_aff       = false;       // default true
    t.aim_extrap      = false;       // default true
    t.chase_gain      = 0.4f;        // default 0.9
    t.chase_look      = 5.0f;        // default 3
    t.dense_us        = 33000;       // default 60000
    t.infeas_policy   = 2;           // Reshape (default Blend)
    t.infeas_margin   = 0.71f;       // default 0.92
    t.infeas_blend    = 0.875f;      // default 0.5
    t.reshape_steps   = 4;           // default 6
    t.smooth_budget   = 0.31f;       // default 0.5
    t.amp_budget      = 0.77f;       // default 0.5
    t.blend_steps     = 9;           // default 6
    t.curve_policy    = 1;           // ForceC1 (default FollowClient)
    t.centering       = false;       // default true
    t.centering_gain  = 0.25f;       // default 1.0
    t.handoff_k       = 2.75f;       // default 1.5
    t.settle_grace_us = 200000;      // default 30000
    return t;
}

}  // namespace

TEST_CASE("every mapped Config field leaves its engine default") {
    const slopmotion::Config def;
    const slopmotion::Config c = buildEngineConfig(sentinelTuning());

    CHECK(c.limits.vmax != def.limits.vmax);
    CHECK(c.limits.amax != def.limits.amax);
    CHECK(c.limits.jmax != def.limits.jmax);
    CHECK(c.recovery_vmax != def.recovery_vmax);
    CHECK(c.chase_feedforward != def.chase_feedforward);
    CHECK(c.chase_accel_ff != def.chase_accel_ff);
    CHECK(c.chase_aim_accel_extrap != def.chase_aim_accel_extrap);
    CHECK(c.chase_ff_gain != def.chase_ff_gain);
    CHECK(c.chase_lookahead != def.chase_lookahead);
    CHECK(c.chase_dense_us != def.chase_dense_us);
    CHECK(c.infeasible_policy != def.infeasible_policy);
    CHECK(c.infeasible_scale_margin != def.infeasible_scale_margin);
    CHECK(c.infeasible_blend != def.infeasible_blend);
    CHECK(c.infeasible_reshape_steps != def.infeasible_reshape_steps);
    CHECK(c.infeasible_smooth_budget != def.infeasible_smooth_budget);
    CHECK(c.infeasible_amplitude_budget != def.infeasible_amplitude_budget);
    CHECK(c.infeasible_blend_steps != def.infeasible_blend_steps);
    CHECK(c.curve_policy != def.curve_policy);
    CHECK(c.wave_centering != def.wave_centering);
    CHECK(c.wave_centering_gain != def.wave_centering_gain);
    CHECK(c.handoff_chord_factor != def.handoff_chord_factor);
    CHECK(c.settle_grace_us != def.settle_grace_us);
}

TEST_CASE("the unmapped list is exactly the set still at the engine default") {
    const slopmotion::Config def;
    const slopmotion::Config c = buildEngineConfig(sentinelTuning());

    // ENGINE-DEFAULT BY DECISION. This list and the CHECK-!= list above are
    // the whole of Config; a field in neither is a field nobody decided about.
    CHECK(c.chase_jerk_scale == def.chase_jerk_scale);
    CHECK(c.chase_jerk_floor == def.chase_jerk_floor);
    CHECK(c.chase_stale_us == def.chase_stale_us);
    CHECK(c.overshoot_guard == def.overshoot_guard);
    CHECK(c.overshoot_chord_slack == def.overshoot_chord_slack);
    CHECK(c.infeasible_soften == def.infeasible_soften);
    CHECK(c.infeasible_soften_floor == def.infeasible_soften_floor);
    CHECK(c.infeasible_soften_steps == def.infeasible_soften_steps);
    CHECK(c.bridge_ratio == def.bridge_ratio);

    // 22 mapped (Limits' three included) + 9 unmapped is the whole struct. A
    // changed size means the engine grew or dropped a field: classify it into
    // one of the two lists above, then update this number. Host-only (the
    // native env has one toolchain), never a wire fact.
    static_assert(sizeof(slopmotion::Config) == 112,
                  "slopmotion::Config changed shape -- re-census the two lists above");
}

TEST_CASE("out-of-range enum ordinals fall through to the engine default") {
    const slopmotion::Config def;
    EngineTuning t = sentinelTuning();
    t.infeas_policy = 200;
    t.curve_policy  = 200;
    const slopmotion::Config c = buildEngineConfig(t);
    CHECK(c.infeasible_policy == def.infeasible_policy);
    CHECK(c.curve_policy == def.curve_policy);
}

TEST_CASE("no usable window keeps the engine's own ceilings") {
    const slopmotion::Config def;
    EngineTuning t = sentinelTuning();
    t.span_mm = 0.0f;
    const slopmotion::Config c = buildEngineConfig(t);
    CHECK(c.limits.vmax == def.limits.vmax);
    CHECK(c.limits.amax == def.limits.amax);
    CHECK(c.limits.jmax == def.limits.jmax);
    CHECK(c.recovery_vmax == 0.0f);
}

TEST_CASE("an override beats the mm-derived ceiling; the map stays pure") {
    EngineTuning t = sentinelTuning();
    t.vmax_ovr = 4.25f;
    t.amax_ovr = 44.0f;
    t.jmax_ovr = 640.0f;
    const slopmotion::Config c = buildEngineConfig(t);
    CHECK(c.limits.vmax == doctest::Approx(4.25f));
    CHECK(c.limits.amax == doctest::Approx(44.0f));
    CHECK(c.limits.jmax == doctest::Approx(640.0f));

    // Purity is what lets the caller compare TUNINGS instead of Configs.
    CHECK(sentinelTuning() == sentinelTuning());
    CHECK_FALSE(sentinelTuning() == t);
    const slopmotion::Config again = buildEngineConfig(t);
    CHECK(again.limits.vmax == c.limits.vmax);
    CHECK(again.infeasible_blend == c.infeasible_blend);
}
