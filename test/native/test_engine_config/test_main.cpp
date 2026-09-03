// test_engine_config -- host suite for the tuning-to-kOpConfig-tag census.
// Constraints:
// - Guards sd-6b2.4 in its post-port form: a field the host means to expose
//   but forgets to emit is silently the slave's default forever. Every mapped
//   field is asserted to MOVE its tag off the boot value under a sentinel
//   input, and every deliberately unmapped ConfigTag is asserted to be ABSENT.
//   The two lists together are the whole vocabulary.
// - Pure integer/float math, no clock, no hardware: deterministic, and must
//   stay that way.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopmotion/slopmotion.hpp"

#include "EngineConfigMap.h"

#include <cstdint>

using motionlink::ConfigField;
using slopdrive::EngineTuning;
using slopdrive::buildConfigTags;
using slopdrive::kEngineConfigTagCount;

namespace {

// Every input at a value the boot tuning is NOT, so "tag moved" is a clean
// signal. Span is 100 mm, so a mm-domain ceiling divides by 100.
EngineTuning sentinelTuning() {
    EngineTuning t;
    t.span_mm         = 100.0f;
    t.input_max_speed = 700.0f;      // -> vmax 7
    t.input_max_accel = 9000.0f;     // -> amax 90
    t.input_max_jerk  = 120000.0f;   // -> jmax 1200
    t.user_max_speed  = 250.0f;      // -> user vmax 2.5
    t.user_max_accel  = 800.0f;      // -> user amax 8
    t.chase_ff        = false;       // boot true
    t.chase_aff       = false;       // boot true
    t.aim_extrap      = false;       // boot true
    t.chase_gain      = 0.4f;        // boot 0.9
    t.chase_look      = 5.0f;        // boot 3
    t.dense_us        = 33000;       // boot 60000
    t.infeas_policy   = 0;           // Stretch (boot ordinal 0 too, see below)
    t.infeas_blend    = 0.875f;      // boot 0.5
    t.smooth_budget   = 0.31f;       // boot 0.5
    t.amp_budget      = 0.77f;       // boot 0.5
    t.blend_steps     = 9;           // boot 6
    t.curve_policy    = 1;           // ForceC1 (boot FollowClient)
    t.handoff_k       = 2.75f;       // boot 1.5
    t.settle_grace_us = 200000;      // boot 30000
    return t;
}

bool hasTag(const std::array<ConfigField, kEngineConfigTagCount>& tags,
            uint8_t tag) {
    for (const ConfigField& f : tags)
        if (f.tag == tag) return true;
    return false;
}

uint32_t rawOf(const std::array<ConfigField, kEngineConfigTagCount>& tags,
               uint8_t tag) {
    for (const ConfigField& f : tags)
        if (f.tag == tag) return f.raw;
    return 0xDEADBEEFu;
}

float fOf(const std::array<ConfigField, kEngineConfigTagCount>& tags,
          uint8_t tag) {
    return motionlink::bitsF32(rawOf(tags, tag));
}

}  // namespace

TEST_CASE("every mapped tuning field moves its own tag") {
    using namespace motionlink;
    const auto boot = buildConfigTags(EngineTuning{});
    const auto s = buildConfigTags(sentinelTuning());

    CHECK(rawOf(s, kCfgInputVmax) != rawOf(boot, kCfgInputVmax));
    CHECK(rawOf(s, kCfgInputAmax) != rawOf(boot, kCfgInputAmax));
    CHECK(rawOf(s, kCfgInputJmax) != rawOf(boot, kCfgInputJmax));
    CHECK(rawOf(s, kCfgUserVmax) != rawOf(boot, kCfgUserVmax));
    CHECK(rawOf(s, kCfgUserAmax) != rawOf(boot, kCfgUserAmax));
    CHECK(rawOf(s, kCfgSettleGraceUs) != rawOf(boot, kCfgSettleGraceUs));
    CHECK(rawOf(s, kCfgInfeasibleBlend) != rawOf(boot, kCfgInfeasibleBlend));
    CHECK(rawOf(s, kCfgCurvePolicy) != rawOf(boot, kCfgCurvePolicy));
    CHECK(rawOf(s, kCfgHandoffChordFactor) != rawOf(boot, kCfgHandoffChordFactor));
    CHECK(rawOf(s, kCfgBlendSteps) != rawOf(boot, kCfgBlendSteps));
    CHECK(rawOf(s, kCfgSmoothBudget) != rawOf(boot, kCfgSmoothBudget));
    CHECK(rawOf(s, kCfgAmplitudeBudget) != rawOf(boot, kCfgAmplitudeBudget));
    CHECK(rawOf(s, kCfgChaseFeedforward) != rawOf(boot, kCfgChaseFeedforward));
    CHECK(rawOf(s, kCfgChaseAccelFf) != rawOf(boot, kCfgChaseAccelFf));
    CHECK(rawOf(s, kCfgChaseFfGain) != rawOf(boot, kCfgChaseFfGain));
    CHECK(rawOf(s, kCfgChaseDenseUs) != rawOf(boot, kCfgChaseDenseUs));
    CHECK(rawOf(s, kCfgChaseLookahead) != rawOf(boot, kCfgChaseLookahead));
    CHECK(rawOf(s, kCfgChaseAimExtrap) != rawOf(boot, kCfgChaseAimExtrap));
    // infeas_policy is the one field whose boot value IS the sentinel value
    // (both ordinal 0); its own cases below cover it.
    CHECK(hasTag(s, kCfgInfeasiblePolicy));
}

TEST_CASE("the unmapped list is exactly the set of tags never emitted") {
    using namespace motionlink;
    const auto s = buildConfigTags(sentinelTuning());

    // ENGINE-DEFAULT BY DECISION. This list and the CHECKs above are the whole
    // ConfigTag vocabulary the ENGINE owns; a tag in neither is a tag nobody
    // decided about. The four here plus the 19 emitted is that whole set.
    CHECK_FALSE(hasTag(s, kCfgOvershootGuard));
    CHECK_FALSE(hasTag(s, kCfgOvershootChordSlack));
    CHECK_FALSE(hasTag(s, kCfgChaseStaleUs));
    // RETIRED: synthesis left the engine 2026-09-03. Emitting it would be a
    // host telling a slave about a knob that no longer exists.
    CHECK_FALSE(hasTag(s, kCfgSampleSynthesis));

    // The ARBITER owns these four (window, gates, soft start); this map must
    // never emit them or two producers would fight over one tag.
    CHECK_FALSE(hasTag(s, kCfgWindowMinCounts));
    CHECK_FALSE(hasTag(s, kCfgWindowMaxCounts));
    CHECK_FALSE(hasTag(s, kCfgGates));
    CHECK_FALSE(hasTag(s, kCfgSoftStartCap));

    CHECK(s.size() == 19);
}

TEST_CASE("no tag is emitted twice") {
    const auto s = buildConfigTags(sentinelTuning());
    for (size_t i = 0; i < s.size(); ++i)
        for (size_t j = i + 1; j < s.size(); ++j)
            CHECK(s[i].tag != s[j].tag);
}

TEST_CASE("out-of-range enum ordinals fall through to the engine default") {
    using namespace motionlink;
    const slopmotion::Config def;
    EngineTuning t = sentinelTuning();
    t.infeas_policy = 200;
    t.curve_policy  = 200;
    const auto s = buildConfigTags(t);
    CHECK(rawOf(s, kCfgInfeasiblePolicy) == uint32_t(def.infeasible_policy));
    CHECK(rawOf(s, kCfgCurvePolicy) == uint32_t(def.curve_policy));
}

TEST_CASE("a retired policy ordinal runs as Blend, never as Stretch") {
    using namespace motionlink;
    // Ordinals 1..kInfeasiblePolicyMax name policies deleted 2026-09-02 that an
    // older NVS still holds. Every one was a timing-first amplitude/shape
    // trade, so the honest remap is Blend -- reverting the operator to Stretch
    // would silently change the CONTRACT (deadline kept vs stroke kept).
    EngineTuning t;
    t.infeas_policy = 0;
    CHECK(rawOf(buildConfigTags(t), kCfgInfeasiblePolicy) ==
          uint32_t(slopmotion::InfeasiblePolicy::Stretch));
    for (uint8_t ord = 1; ord <= slopmotion::kInfeasiblePolicyMax; ++ord) {
        t.infeas_policy = ord;
        CHECK(rawOf(buildConfigTags(t), kCfgInfeasiblePolicy) ==
              uint32_t(slopmotion::InfeasiblePolicy::Blend));
    }
}

TEST_CASE("no usable window emits zero ceilings, which the slave gates on") {
    using namespace motionlink;
    EngineTuning t = sentinelTuning();
    t.span_mm = 0.0f;
    const auto s = buildConfigTags(t);
    // A zero vmax means the set was never usefully pushed; the slave treats
    // that as unconfigured and gates the command rather than planning at zero
    // (MotionLinkProtocol.h, SelectedLimits).
    CHECK(fOf(s, kCfgInputVmax) == 0.0f);
    CHECK(fOf(s, kCfgInputAmax) == 0.0f);
    CHECK(fOf(s, kCfgInputJmax) == 0.0f);
    CHECK(fOf(s, kCfgUserVmax) == 0.0f);
    CHECK(fOf(s, kCfgUserAmax) == 0.0f);
}

TEST_CASE("an override beats the mm-derived ceiling; the map stays pure") {
    using namespace motionlink;
    EngineTuning t = sentinelTuning();
    t.vmax_ovr = 4.25f;
    t.amax_ovr = 44.0f;
    t.jmax_ovr = 640.0f;
    const auto s = buildConfigTags(t);
    CHECK(fOf(s, kCfgInputVmax) == doctest::Approx(4.25f));
    CHECK(fOf(s, kCfgInputAmax) == doctest::Approx(44.0f));
    CHECK(fOf(s, kCfgInputJmax) == doctest::Approx(640.0f));
    // The same three values the host publishes as sm_eff_*: ONE derivation.
    const slopdrive::NormalizedLimits lim = slopdrive::normalizedLimits(t);
    CHECK(lim.vmax == doctest::Approx(4.25f));
    CHECK(lim.amax == doctest::Approx(44.0f));
    CHECK(lim.jmax == doctest::Approx(640.0f));

    // Purity is what lets the DRIVER own change detection: equal tuning in,
    // byte-equal tags out, so an unchanged field never ships a frame.
    CHECK(sentinelTuning() == sentinelTuning());
    CHECK_FALSE(sentinelTuning() == t);
    const auto again = buildConfigTags(t);
    for (size_t i = 0; i < s.size(); ++i) {
        CHECK(again[i].tag == s[i].tag);
        CHECK(again[i].raw == s[i].raw);
    }
}
