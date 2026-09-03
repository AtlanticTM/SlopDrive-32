// test_rpmotion -- native doctest suite for the RP2350's motion core.
// Constraints:
// - Exercises the SHIPPING core, not a model of it: frames are built with the
//   header's own encoders exactly as the S3 builds them, and the clock is a
//   synthetic microsecond counter, so nothing here is timing-dependent.
// - The harness runs the same three contexts the coprocessor does: ingest at
//   frame arrival, service on the core-1 cadence, sampleCounts on the 20 kHz
//   tick. A test that services and samples in one call proves nothing about
//   the hand-off it is testing.
// - Field-replay rows are the GENERATED figures from test/native/
//   test_slopmotion (tools/segtrace_to_case.py), replayed through the link.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "RpMotionCore.h"
#include "slopmotion/slopmotion.hpp"

namespace ml = motionlink;
using rpmotion::Core;
using slopmotion::Command;
using slopmotion::Config;
using slopmotion::Engine;

namespace {

constexpr uint32_t kTickUs = 50;        // the coprocessor's 20 kHz trajectory tick
constexpr uint32_t kServiceUs = 200;    // core 1's drain cadence
constexpr uint32_t kMs = 1000;

// ---- The machine is the fixture --------------------------------------------
// The tuning read off the device on 2026-09-02, the same figures
// test/native/test_slopmotion's liveTuning() carries: a 100 mm window mapped to
// 0..1, 1000 mm/s input / 200 mm/s user, Blend at 0.5 over 6 ray steps,
// c1_cubic segments, 200 ms of settle grace, chase gain 0.1 with no lookahead.
// Pushed here as CONFIG TAGS, which is the point: the RP holds no tuning of its
// own and the S3 owns every number.
Config liveTuning() {
    Config c;
    c.limits.vmax = 10.0f;
    c.limits.amax = 400.0f;
    c.limits.jmax = 50000.0f;
    c.recovery_vmax = 2.0f;
    c.infeasible_policy = slopmotion::InfeasiblePolicy::Blend;
    c.infeasible_blend = 0.5f;
    c.infeasible_smooth_budget = 0.5f;
    c.infeasible_amplitude_budget = 0.5f;
    c.infeasible_blend_steps = 6;
    c.settle_grace_us = 200000;
    c.chase_ff_gain = 0.1f;
    c.chase_lookahead = 0.0f;
    return c;
}

// The same tuning as the tag list the S3 would push. Window 0..1 counts so a
// rendered count IS a normalized unit and a parity check reads directly.
std::vector<ml::ConfigField> liveTags(uint32_t gates = ml::kGateHomed,
                                      float win_lo = 0.0f,
                                      float win_hi = 1.0f) {
    const Config c = liveTuning();
    return {
        {ml::kCfgInputVmax, ml::f32Bits(c.limits.vmax)},
        {ml::kCfgInputAmax, ml::f32Bits(c.limits.amax)},
        {ml::kCfgInputJmax, ml::f32Bits(c.limits.jmax)},
        {ml::kCfgUserVmax, ml::f32Bits(c.recovery_vmax)},
        {ml::kCfgUserAmax, ml::f32Bits(c.limits.amax)},
        {ml::kCfgSettleGraceUs, c.settle_grace_us},
        {ml::kCfgInfeasiblePolicy, uint32_t(c.infeasible_policy)},
        {ml::kCfgInfeasibleBlend, ml::f32Bits(c.infeasible_blend)},
        {ml::kCfgCurvePolicy, uint32_t(c.curve_policy)},
        {ml::kCfgHandoffChordFactor, ml::f32Bits(c.handoff_chord_factor)},
        {ml::kCfgOvershootGuard, ml::f32Bits(c.overshoot_guard)},
        {ml::kCfgOvershootChordSlack, ml::f32Bits(c.overshoot_chord_slack)},
        {ml::kCfgBlendSteps, c.infeasible_blend_steps},
        {ml::kCfgSmoothBudget, ml::f32Bits(c.infeasible_smooth_budget)},
        {ml::kCfgAmplitudeBudget, ml::f32Bits(c.infeasible_amplitude_budget)},
        {ml::kCfgChaseFeedforward, c.chase_feedforward ? 1u : 0u},
        {ml::kCfgChaseAccelFf, c.chase_accel_ff ? 1u : 0u},
        {ml::kCfgChaseFfGain, ml::f32Bits(c.chase_ff_gain)},
        {ml::kCfgChaseDenseUs, c.chase_dense_us},
        {ml::kCfgChaseLookahead, ml::f32Bits(c.chase_lookahead)},
        {ml::kCfgChaseAimExtrap, c.chase_aim_accel_extrap ? 1u : 0u},
        {ml::kCfgChaseStaleUs, c.chase_stale_us},
        {ml::kCfgGates, gates},
        {ml::kCfgWindowMinCounts, ml::f32Bits(win_lo)},
        {ml::kCfgWindowMaxCounts, ml::f32Bits(win_hi)},
    };
}

// ---- The harness: one slave, three contexts --------------------------------
struct Slave {
    Core core;
    uint32_t now = 1000 * kMs;
    uint8_t seq = 0;
    uint32_t next_service = 0;
    float pos = 0.0f, vel = 0.0f;
    std::vector<ml::EventRecord> pulled;
    uint8_t acked = 0;

    Slave() : core(1000 * kMs) { next_service = now; }

    void send(std::span<const uint8_t, ml::kFrameBytes> f) {
        REQUIRE(ml::crcOk(f));
        core.ingestFrame(f, now);
    }

    uint8_t nextSeq() { return ++seq; }

    void pushConfig(const std::vector<ml::ConfigField>& all) {
        for (size_t i = 0; i < all.size(); i += ml::kConfigFieldsPerFrame) {
            const size_t n = std::min(ml::kConfigFieldsPerFrame, all.size() - i);
            std::array<uint8_t, ml::kFrameBytes> f{};
            ml::encodeConfig(f, nextSeq(),
                             std::span<const ml::ConfigField>(all.data() + i, n));
            send(f);
        }
    }

    uint8_t command(const ml::LinkCommand& c) {
        std::array<uint8_t, ml::kFrameBytes> f{};
        const uint8_t s = nextSeq();
        ml::encodeCommand(f, s, c);
        send(f);
        return s;
    }

    // Advance the synthetic clock, running core 1 on its cadence and the tick
    // on its own, in the order the silicon runs them.
    void run(uint32_t us) {
        const uint32_t end = now + us;
        while (now < end) {
            if (int32_t(now - next_service) >= 0) {
                core.service(now);
                next_service = now + kServiceUs;
            }
            core.sampleCounts(now, pos, vel);
            now += kTickUs;
        }
    }

    // Drain the event ring the way the master does: pull, ack, repeat.
    void drainEvents() {
        for (;;) {
            ml::EventRecord e;
            if (!core.nextEvent(acked, e)) break;
            pulled.push_back(e);
            acked = e.seq;
        }
    }

    int countEvents(uint8_t kind) {
        drainEvents();
        int n = 0;
        for (const auto& e : pulled)
            if (e.kind == kind) ++n;
        return n;
    }
};

// A waveform intent, planned at the input set unless told otherwise.
ml::LinkCommand wave(float target, uint32_t dur_ms, float vf, bool has_vf,
                     uint8_t set = ml::kLimitInput) {
    ml::LinkCommand c;
    c.kind = ml::kCmdWaveform;
    c.target = target;
    c.duration_us = dur_ms * kMs;
    c.end_vel = vf;
    c.flags = has_vf ? ml::kCmdHasEndVel : 0;
    c.curve_family = slopmotion::kClientCurveC1Cubic;
    c.limit_set = set;
    return c;
}

}  // namespace

// =============================================================================

TEST_CASE("A waveform command renders what the engine renders for the same command") {
    Slave s;
    s.pushConfig(liveTags());
    s.run(kServiceUs * 2);

    // The reference engine is seeded and clocked exactly as the core's is: same
    // seed, same commit instant, same command.
    Engine ref(liveTuning(), 0.0f);
    ref.resetAt(0.0f, s.now);

    ml::LinkCommand c = wave(0.8f, 300, 0.0f, true);
    s.command(c);
    s.core.service(s.now);   // the core commits at this instant
    Command rc;
    rc.target = c.target;
    rc.duration_us = c.duration_us;
    rc.has_duration = true;
    rc.has_end_vel = true;
    rc.end_vel = c.end_vel;
    rc.client_curve_family = c.curve_family;
    REQUIRE(ref.commit(rc, s.now));

    double worst = 0.0;
    const uint32_t t_end = s.now + 320 * kMs;
    while (s.now < t_end) {
        if (int32_t(s.now - s.next_service) >= 0) {
            s.core.service(s.now);
            s.next_service = s.now + kServiceUs;
        }
        s.core.sampleCounts(s.now, s.pos, s.vel);
        const double d = std::fabs(double(s.pos) - double(ref.positionAt(s.now)));
        if (d > worst) worst = d;
        s.now += kTickUs;
    }
    // Reconstruction error is bounded by a slice straddling a jerk switch; a
    // Hermite plan is reproduced exactly, so the whole span sits far under one
    // count of the machine's 3200-count window.
    CHECK(worst < 1e-4);
    CHECK(s.pos == doctest::Approx(ref.positionAt(s.now)).epsilon(0.0001));
    // ...and the stroke really ran, so the parity above is not two engines
    // agreeing on standing still. The commanded 0.8 in 300 ms is over the
    // COLD-START ceiling (the first plan out of rest is a positioning move at
    // the user limit), so Blend shortens it -- which is the engine's answer and
    // the link reproduces it rather than second-guessing it.
    CHECK(s.pos > 0.3f);
}

TEST_CASE("A future anchor is not rendered before its time and is continuous at it") {
    Slave s;
    s.pushConfig(liveTags());
    s.run(kServiceUs * 2);

    // Park at 0.2 first, let it land, then schedule a move 100 ms out.
    s.command(wave(0.2f, 200, 0.0f, true));
    s.run(260 * kMs);
    const float parked = s.pos;
    CHECK(parked == doctest::Approx(0.2f).epsilon(0.01));

    const uint32_t anchor = s.now + 100 * kMs;
    ml::LinkCommand c = wave(0.9f, 200, 0.0f, true);
    c.flags |= ml::kCmdHasAnchor;
    c.anchor_us = anchor;
    s.command(c);

    // Nothing may move before the anchor.
    float worst_before = 0.0f;
    while (int32_t(s.now - anchor) < -int32_t(kTickUs)) {
        if (int32_t(s.now - s.next_service) >= 0) {
            s.core.service(s.now);
            s.next_service = s.now + kServiceUs;
        }
        s.core.sampleCounts(s.now, s.pos, s.vel);
        const float d = std::fabs(s.pos - parked);
        if (d > worst_before) worst_before = d;
        s.now += kTickUs;
    }
    CHECK(worst_before < 1e-4f);

    // ...and the join at the anchor carries no step: the largest tick-to-tick
    // move through the promotion stays inside what the ceiling allows in 50 us.
    float prev = s.pos, worst_step = 0.0f;
    const uint32_t t_end = s.now + 40 * kMs;
    while (s.now < t_end) {
        if (int32_t(s.now - s.next_service) >= 0) {
            s.core.service(s.now);
            s.next_service = s.now + kServiceUs;
        }
        s.core.sampleCounts(s.now, s.pos, s.vel);
        const float step = std::fabs(s.pos - prev);
        if (step > worst_step) worst_step = step;
        prev = s.pos;
        s.now += kTickUs;
    }
    CHECK(worst_step < 10.0f * float(kTickUs) * 1e-6f * 1.2f);
    s.run(220 * kMs);
    CHECK(s.pos == doctest::Approx(0.9f).epsilon(0.01));
}

TEST_CASE("Both limit sets select per command and the soft-start cap folds in") {
    // A POINT move is time-optimal under the ceilings it was planned with, so
    // its peak speed reads the selected set directly instead of through the
    // waveform referees. The warm-up matters: the FIRST plan out of rest is a
    // positioning move clamped at the user limit whatever set it named, and
    // measuring that would compare the governor with itself.
    auto peakSpeed = [](uint8_t set, float soft_cap) {
        Slave s;
        auto tags = liveTags();
        if (soft_cap > 0.0f)
            tags.push_back({ml::kCfgSoftStartCap, ml::f32Bits(soft_cap)});
        s.pushConfig(tags);
        s.run(kServiceUs * 2);
        ml::LinkCommand warm;
        warm.target = 0.2f;
        warm.limit_set = ml::kLimitInput;
        s.command(warm);
        s.run(400 * kMs);

        ml::LinkCommand c;
        c.target = 0.9f;
        c.limit_set = set;
        s.command(c);
        float vpk = 0.0f;
        const uint32_t t_end = s.now + 900 * kMs;
        while (s.now < t_end) {
            if (int32_t(s.now - s.next_service) >= 0) {
                s.core.service(s.now);
                s.next_service = s.now + kServiceUs;
            }
            s.core.sampleCounts(s.now, s.pos, s.vel);
            const float a = std::fabs(s.vel);
            if (a > vpk) vpk = a;
            s.now += kTickUs;
        }
        CHECK(s.pos == doctest::Approx(0.9f).epsilon(0.02));
        return vpk;
    };

    const float fast = peakSpeed(ml::kLimitInput, 0.0f);
    const float slow = peakSpeed(ml::kLimitUser, 0.0f);
    CHECK(fast > 5.0f);                // the input ceiling is 10
    CHECK(slow <= 2.0f * 1.02f);       // the user ceiling is 2, and it holds
    CHECK(fast > slow * 2.0f);

    // The cap is a transient min() over the SELECTED set's vmax, so it bites
    // the fast set and cannot make the slow one faster.
    const float capped = peakSpeed(ml::kLimitInput, 1.0f);
    CHECK(capped <= 1.0f * 1.02f);
    CHECK(capped < fast);
    CHECK(peakSpeed(ml::kLimitUser, 4.0f) == doctest::Approx(slow).epsilon(0.02));
}

TEST_CASE("A gated command is dropped, reported, and moves nothing") {
    Slave s;
    s.pushConfig(liveTags(0));   // not homed
    s.run(kServiceUs * 2);
    const float start = s.pos;

    const uint8_t seq = s.command(wave(0.9f, 200, 0.0f, true));
    s.run(300 * kMs);

    CHECK(s.pos == doctest::Approx(start));
    s.drainEvents();
    int gated = 0;
    for (const auto& e : s.pulled) {
        if (e.kind != ml::kEvtCommandGated) continue;
        ++gated;
        CHECK(e.cmd_seq == seq);
        CHECK(uint32_t(e.detail) == ml::kGateHomed);
        CHECK(e.target == doctest::Approx(0.9f));
    }
    CHECK(gated == 1);

    // Paused names its own bit, and homed-and-paused names both.
    Slave p;
    p.pushConfig(liveTags(ml::kGateHomed | ml::kGatePaused));
    p.run(kServiceUs * 2);
    p.command(wave(0.9f, 200, 0.0f, true));
    p.run(20 * kMs);
    p.drainEvents();
    bool seen = false;
    for (const auto& e : p.pulled)
        if (e.kind == ml::kEvtCommandGated) {
            CHECK(uint32_t(e.detail) == ml::kGatePaused);
            seen = true;
        }
    CHECK(seen);
}

TEST_CASE("kOpSetPos re-seeds honestly outside the window and the first plan moves inward") {
    // A 3200-count window, and the homing ritual declares the wall 320 counts
    // BELOW its bottom: the seed is -0.1 normalized, which is a real position
    // and is never clamped (docs/rp-motion-port.md, Units).
    Slave s;
    s.pushConfig(liveTags(ml::kGateHomed, 0.0f, 3200.0f));
    s.run(kServiceUs * 2);

    std::array<uint8_t, ml::kFrameBytes> f{};
    f[0] = ml::kOpSetPos;
    f[1] = s.nextSeq();
    ml::putF32(f, 2, -320.0f);
    ml::crcStamp(f);
    s.send(f);

    // The hold is published from the IRQ, so the very next tick renders it.
    s.core.sampleCounts(s.now, s.pos, s.vel);
    CHECK(s.pos == doctest::Approx(-320.0f));
    s.run(kServiceUs * 4);
    CHECK(s.pos == doctest::Approx(-320.0f));

    // The first plan from out there must move INWARD and never further out.
    // 0.2 normalized in 800 ms is inside the cold-start ceiling, so the plan
    // lands on its target rather than being shortened by a referee.
    s.command(wave(0.1f, 800, 0.0f, true));
    float lowest = s.pos;
    const uint32_t t_end = s.now + 900 * kMs;
    while (s.now < t_end) {
        if (int32_t(s.now - s.next_service) >= 0) {
            s.core.service(s.now);
            s.next_service = s.now + kServiceUs;
        }
        s.core.sampleCounts(s.now, s.pos, s.vel);
        if (s.pos < lowest) lowest = s.pos;
        s.now += kTickUs;
    }
    CHECK(lowest >= -320.0f - 1e-3f);
    CHECK(s.pos == doctest::Approx(320.0f).epsilon(0.02));
}

TEST_CASE("The clock filter converts a known offset from the slave's own stamps") {
    // The slave stamps clock_t1 at frame receipt; the master pairs it with the
    // (t0, t3) it recorded for that seq. Offset is a constant here, so the
    // windowed minimum must recover it exactly.
    constexpr uint32_t kOffset = 1234567;
    constexpr uint32_t kRttUs = 40;
    Slave s;
    ml::ClockFilter filter;
    uint32_t master = 500000;
    for (size_t i = 0; i < ml::kClockWindow + 4; ++i) {
        const uint32_t t0 = master;
        s.now = master + kRttUs / 2 + kOffset;
        std::array<uint8_t, ml::kFrameBytes> f{};
        f[0] = ml::kOpPing;
        f[1] = s.nextSeq();
        ml::crcStamp(f);
        s.send(f);
        ml::StatusV2 st;
        s.core.fillStatus(st);
        CHECK(st.seq_echo == f[1]);
        filter.push(t0, t0 + kRttUs, st.clock_t1);
        master += 10000 + uint32_t(i) * 37;   // dithered, never a clean multiple
    }
    REQUIRE(filter.converged());
    CHECK(filter.offsetUs() == kOffset);
    CHECK(filter.toSlave(master) == master + kOffset);
}

TEST_CASE("Events pull in order and remaining counts down") {
    Slave s;
    s.pushConfig(liveTags(0));   // gated, so every command mints one event
    s.run(kServiceUs * 2);
    for (int i = 0; i < 4; ++i) s.command(wave(0.5f + 0.05f * float(i), 100, 0, true));
    s.run(kServiceUs * 2);

    ml::StatusV2 st;
    s.core.fillStatus(st);
    CHECK(st.event_seq != 0);

    std::vector<ml::EventRecord> got;
    uint8_t ack = 0;
    for (;;) {
        ml::EventRecord e;
        if (!s.core.nextEvent(ack, e)) break;
        got.push_back(e);
        ack = e.seq;
    }
    // The clock step from construction, then the four gated commands.
    REQUIRE(got.size() >= 5);
    CHECK(got.front().kind == ml::kEvtClockStep);
    for (size_t i = 0; i + 1 < got.size(); ++i) {
        CHECK(uint8_t(got[i + 1].seq - got[i].seq) == 1);
        CHECK(got[i].remaining == uint8_t(got.size() - 1 - i));
    }
    CHECK(got.back().remaining == 0);
    // Everything acked: the ring is empty and the seq did not restart.
    ml::EventRecord none;
    CHECK_FALSE(s.core.nextEvent(ack, none));
}

TEST_CASE("Status fills every field the master reads") {
    Slave s;
    s.pushConfig(liveTags(ml::kGateHomed, 100.0f, 3300.0f));
    s.run(kServiceUs * 2);
    // Homing declares where the carriage is before anything commands it.
    std::array<uint8_t, ml::kFrameBytes> seed{};
    seed[0] = ml::kOpSetPos;
    seed[1] = s.nextSeq();
    ml::putF32(seed, 2, 1700.0f);
    ml::crcStamp(seed);
    s.send(seed);
    s.run(kServiceUs * 2);

    const uint8_t cseq = s.command(wave(0.75f, 600, 0.0f, true));
    s.run(40 * kMs);

    ml::StatusV2 st;
    s.core.fillStatus(st);
    CHECK(st.state == ml::kStateRunning);
    CHECK(st.seq_echo == cseq);
    CHECK(st.event_seq != 0);
    CHECK(st.pos > 100.0f);
    CHECK(st.pos < 3300.0f);
    CHECK(st.vel > 0.0f);
    CHECK(st.mode == uint8_t(slopmotion::Mode::Waveform));
    CHECK(st.plan_kind != uint8_t(slopmotion::PlanKind::None));
    CHECK(st.clock_t1 != 0);
    CHECK(st.config_fp != 0);

    // A round trip through the wire keeps every field.
    std::array<uint8_t, ml::kFrameBytes> f{};
    ml::encodeStatusV2(f, st);
    CHECK(f[ml::kStatusOffVariant] == ml::kStatusV2);
    const ml::StatusV2 back = ml::decodeStatusV2(f);
    CHECK(back.state == st.state);
    CHECK(back.seq_echo == st.seq_echo);
    CHECK(back.event_seq == st.event_seq);
    CHECK(back.pos == doctest::Approx(st.pos));
    CHECK(back.vel == doctest::Approx(st.vel));
    CHECK(back.mode == st.mode);
    CHECK(back.plan_kind == st.plan_kind);
    CHECK(back.clock_t1 == st.clock_t1);
    CHECK(back.config_fp == st.config_fp);

    // Estop claims byte 0 whatever the plan was doing.
    s.core.estop(s.now);
    s.core.fillStatus(st);
    CHECK(st.state == ml::kStateEstop);
}

TEST_CASE("A lost middle config frame leaves a fingerprint the master can see") {
    // Fifteen tags is three frames. Drop the middle one on the wire and the
    // slave's image no longer matches the S3's, which a counter could not have
    // seen: the last frame still bumps a counter.
    const auto all = liveTags();
    REQUIRE(all.size() > 2 * ml::kConfigFieldsPerFrame);

    ml::ConfigImage master;
    for (const auto& fd : all) master.set(fd.tag, fd.raw);

    Slave s;
    uint8_t seq = 0;
    size_t frame = 0;
    for (size_t i = 0; i < all.size(); i += ml::kConfigFieldsPerFrame, ++frame) {
        if (frame == 1) continue;   // the lost one
        const size_t n = std::min(ml::kConfigFieldsPerFrame, all.size() - i);
        std::array<uint8_t, ml::kFrameBytes> f{};
        ml::encodeConfig(f, ++seq,
                         std::span<const ml::ConfigField>(all.data() + i, n));
        s.send(f);
    }
    s.run(kServiceUs * 2);
    ml::StatusV2 st;
    s.core.fillStatus(st);
    CHECK(st.config_fp != 0);
    CHECK(st.config_fp != master.fingerprint());

    // Re-sending only the lost frame heals it: the image is a SET, so the
    // fingerprint never depends on arrival order.
    const size_t at = ml::kConfigFieldsPerFrame;
    std::array<uint8_t, ml::kFrameBytes> f{};
    ml::encodeConfig(f, ++seq,
                     std::span<const ml::ConfigField>(all.data() + at,
                                                      ml::kConfigFieldsPerFrame));
    s.send(f);
    s.run(kServiceUs * 2);
    s.core.fillStatus(st);
    CHECK(st.config_fp == master.fingerprint());
}

TEST_CASE("An unknown config tag is stored, reported once, and never applied") {
    Slave s;
    auto tags = liveTags();
    tags.push_back({0x7E, 42});
    s.pushConfig(tags);
    s.pushConfig({{0x7E, 42}});   // a re-push is not a second report
    s.run(kServiceUs * 4);

    CHECK(s.countEvents(ml::kEvtConfigTagUnknown) == 1);
    for (const auto& e : s.pulled)
        if (e.kind == ml::kEvtConfigTagUnknown) CHECK(uint32_t(e.detail) == 0x7E);
    // Stored: the image still agrees with a master holding the same tag.
    ml::ConfigImage master;
    for (const auto& fd : tags) master.set(fd.tag, fd.raw);
    ml::StatusV2 st;
    s.core.fillStatus(st);
    CHECK(st.config_fp == master.fingerprint());
}

TEST_CASE("kEvtPlanAdopted carries the plan the S3 draws its strip from") {
    Slave s;
    s.pushConfig(liveTags());
    s.run(kServiceUs * 2);
    s.drainEvents();
    s.pulled.clear();

    const uint32_t at = s.now;
    const uint8_t seq = s.command(wave(0.7f, 600, 0.0f, true));
    s.run(kServiceUs * 2);
    s.drainEvents();

    bool seen = false;
    for (const auto& e : s.pulled) {
        if (e.kind != ml::kEvtPlanAdopted) continue;
        seen = true;
        CHECK(e.cmd_seq == seq);
        CHECK(e.target == doctest::Approx(0.7f).epsilon(0.05));
        CHECK(e.detail == doctest::Approx(0.6f).epsilon(0.05));
        CHECK(int32_t(e.t_us - at) >= 0);
        CHECK(int32_t(e.t_us - at) < int32_t(2 * kServiceUs));
    }
    CHECK(seen);

    // A hold is an adoption too, and it reports duration 0.
    s.pulled.clear();
    s.run(800 * kMs);
    s.drainEvents();
    bool hold = false;
    for (const auto& e : s.pulled)
        if (e.kind == ml::kEvtPlanAdopted && e.detail == 0.0f) hold = true;
    CHECK(hold);
}

TEST_CASE("Motion denied mid-stroke brakes to rest instead of freezing") {
    Slave s;
    s.pushConfig(liveTags());
    s.run(kServiceUs * 2);
    s.command(wave(1.0f, 600, 0.0f, true));
    s.run(150 * kMs);
    REQUIRE(std::fabs(s.vel) > 0.5f);

    s.pushConfig({{ml::kCfgGates, ml::kGateHomed | ml::kGatePaused}});
    s.run(400 * kMs);
    CHECK(std::fabs(s.vel) < 1e-3f);
    CHECK(s.pos > 0.0f);
    CHECK(s.pos < 1.0f + 1e-3f);
}

// ---- Field replay ----------------------------------------------------------
// The generated figures from test/native/test_slopmotion (segtrace-jitter-06,
// tools/segtrace_to_case.py), replayed THROUGH THE LINK: the same rows, the
// same per-row queue-drain lateness, but each one crossing as an anchored
// kOpCommand under config pushed as tags.
namespace replay {

struct S {
    float tgt;
    uint32_t dur_ms;
    float vf;
    bool has_vf;
    float next;
    bool has_next;
    uint32_t late_ms;
};

struct Census {
    int settles = 0;
    int gated = 0;
    int failed = 0;
    float lo = 2.0f;
    float hi = -1.0f;
};

// Play a script at the pacing the device saw: each row is anchored at its own
// due time and the frame arrives late_ms after it, which is exactly what the
// queue drain does on the S3.
Census play(Slave& s, const S* seq, size_t n, uint32_t due0) {
    Census cen;
    uint32_t due = due0;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t arrive = due + seq[i].late_ms * kMs;
        while (int32_t(s.now - arrive) < 0) {
            if (int32_t(s.now - s.next_service) >= 0) {
                s.core.service(s.now);
                s.next_service = s.now + kServiceUs;
            }
            s.core.sampleCounts(s.now, s.pos, s.vel);
            if (s.pos < cen.lo) cen.lo = s.pos;
            if (s.pos > cen.hi) cen.hi = s.pos;
            s.now += kTickUs;
        }
        ml::LinkCommand c = wave(seq[i].tgt, seq[i].dur_ms, seq[i].vf,
                                 seq[i].has_vf);
        c.flags |= ml::kCmdHasAnchor;
        c.anchor_us = due;
        if (seq[i].has_next) {
            c.flags |= ml::kCmdHasNextChord;
            c.next_chord = seq[i].next;
        }
        s.command(c);
        due += seq[i].dur_ms * kMs;
    }
    while (int32_t(s.now - due) < 0) {
        if (int32_t(s.now - s.next_service) >= 0) {
            s.core.service(s.now);
            s.next_service = s.now + kServiceUs;
        }
        s.core.sampleCounts(s.now, s.pos, s.vel);
        if (s.pos < cen.lo) cen.lo = s.pos;
        if (s.pos > cen.hi) cen.hi = s.pos;
        s.now += kTickUs;
    }
    s.drainEvents();
    for (const auto& e : s.pulled) {
        if (e.kind == uint8_t(slopmotion::AnomalyType::SettleEngaged)) cen.settles++;
        if (e.kind == uint8_t(slopmotion::AnomalyType::PlanFailed)) cen.failed++;
        if (e.kind == ml::kEvtCommandGated) cen.gated++;
    }
    return cen;
}

}  // namespace replay

TEST_CASE("Field replay through the link: the reversal figure does not settle") {
    // segtrace-jitter-06 168.117-170.042 s, two full strokes.
    using namespace replay;
    Slave s;
    s.pushConfig(liveTags());
    s.run(kServiceUs * 2);
    // Seed where the device was, through the link's own door.
    std::array<uint8_t, ml::kFrameBytes> f{};
    f[0] = ml::kOpSetPos;
    f[1] = s.nextSeq();
    ml::putF32(f, 2, 0.35f);
    ml::crcStamp(f);
    s.send(f);
    s.run(kServiceUs * 2);
    s.drainEvents();
    s.pulled.clear();

    const S REVERSAL[] = {
        {0.350f, 166, 0.000f, true, 0.000f, false, 3},
        {0.400f, 85, 0.859f, true, 2.400f, true, 1},
        {1.000f, 250, 0.000f, true, 0.000f, false, 3},
        {0.900f, 207, -0.730f, true, 0.000f, false, 4},
        {0.500f, 250, -1.134f, true, 0.000f, false, 2},
        {0.350f, 166, 0.000f, true, 0.000f, false, 5},
        {0.400f, 85, 0.859f, true, 2.400f, true, 1},
        {1.000f, 250, 0.000f, true, 0.000f, false, 2},
        {0.900f, 208, -0.727f, true, 0.000f, false, 1},
        {0.500f, 250, -1.134f, true, 0.000f, false, 3},
        {0.350f, 166, 0.000f, true, 0.000f, false, 3},
    };
    const Census cen = play(s, REVERSAL, sizeof(REVERSAL) / sizeof(REVERSAL[0]),
                            s.now + 200 * kMs);
    CHECK(cen.settles == 0);
    CHECK(cen.gated == 0);
    CHECK(cen.failed == 0);
    CHECK(cen.lo >= -0.05f);
    CHECK(cen.hi <= 1.05f);
}

TEST_CASE("Field replay through the link: the long hold at the rail is content") {
    // segtrace-jitter-06 158.412-166.122 s: the approach, a 6.875 s hold at
    // 1.000, then the exit.
    using namespace replay;
    Slave s;
    s.pushConfig(liveTags());
    s.run(kServiceUs * 2);
    std::array<uint8_t, ml::kFrameBytes> f{};
    f[0] = ml::kOpSetPos;
    f[1] = s.nextSeq();
    ml::putF32(f, 2, 0.45f);
    ml::crcStamp(f);
    s.send(f);
    s.run(kServiceUs * 2);
    s.drainEvents();
    s.pulled.clear();

    const S HOLD[] = {
        {0.550f, 41, 2.817f, true, 3.600f, true, 3},
        {1.000f, 125, 0.000f, true, 0.000f, false, 1},
        {1.000f, 6875, 0.000f, true, 0.000f, false, 3},
        {0.650f, 167, -2.245f, true, 0.000f, false, 1},
        {0.350f, 125, 0.000f, true, 0.000f, false, 4},
        {1.000f, 208, 0.000f, true, 0.000f, false, 4},
        {0.900f, 169, -0.839f, true, 0.000f, false, 1},
        {0.500f, 250, -1.134f, true, 0.000f, false, 4},
    };
    // The approach and the hold first; the census is the EXIT, which enters
    // mid-stream and therefore takes every invariant.
    play(s, HOLD, 3, s.now + 200 * kMs);
    s.drainEvents();
    s.pulled.clear();
    const Census cen = play(s, HOLD + 3, 5, s.now - 3 * kMs);
    CHECK(cen.settles == 0);
    CHECK(cen.gated == 0);
    CHECK(cen.lo >= -0.05f);
    CHECK(cen.hi <= 1.05f);
}
