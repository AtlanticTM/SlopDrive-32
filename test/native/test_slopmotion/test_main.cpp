// ============================================================================
// test_slopmotion — native doctest suite for the SlopMotion engine
// ============================================================================
//
// Hardware-free, fully deterministic: time is a synthetic uint64 microsecond
// counter, no clocks, no randomness. Every kinematic assertion is checked by
// SAMPLING the produced trajectory on a 1 ms grid (the same cadence the
// firmware's stream sampler uses) — limits are verified as sampled reality,
// not trusted from Ruckig's promises.
//
// Finite-difference tolerances: velocity/accel come from Ruckig analytically,
// but jerk is checked as Δa/Δt on the 1 ms grid, which averages across the
// bang-bang jerk switching instants — so the jerk bound uses a small margin.
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopmotion/slopmotion.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using slopmotion::AnomalyType;
using slopmotion::Command;
using slopmotion::Config;
using slopmotion::Engine;
using slopmotion::Mode;

namespace {

constexpr uint64_t kMs = 1000ULL;        // µs per ms
constexpr uint64_t kS  = 1000000ULL;     // µs per s

Config testConfig() {
    Config cfg;
    cfg.limits.vmax = 2.0f;
    cfg.limits.amax = 20.0f;
    cfg.limits.jmax = 300.0f;
    return cfg;
}

struct SweepStats {
    double max_abs_v = 0.0;
    double max_abs_a = 0.0;
    double max_abs_j = 0.0;   // finite-difference of accel on the grid
    double min_p = 1e9, max_p = -1e9;
    double max_dv = 0.0;      // largest velocity step between adjacent samples
};

// Sample [t0, t1] on a 1 ms grid and accumulate kinematic extremes.
SweepStats sweep(Engine& e, uint64_t t0_us, uint64_t t1_us) {
    SweepStats s;
    double prev_v = 0.0, prev_a = 0.0;
    bool first = true;
    for (uint64_t t = t0_us; t <= t1_us; t += kMs) {
        const double p = e.positionAt(t);
        const double v = e.velocityAt(t);
        const double a = e.accelerationAt(t);
        REQUIRE(std::isfinite(p));
        REQUIRE(std::isfinite(v));
        REQUIRE(std::isfinite(a));
        s.max_abs_v = std::max(s.max_abs_v, std::fabs(v));
        s.max_abs_a = std::max(s.max_abs_a, std::fabs(a));
        s.min_p = std::min(s.min_p, p);
        s.max_p = std::max(s.max_p, p);
        if (!first) {
            s.max_abs_j = std::max(s.max_abs_j, std::fabs(a - prev_a) / 1e-3);
            s.max_dv    = std::max(s.max_dv, std::fabs(v - prev_v));
        }
        prev_v = v; prev_a = a; first = false;
    }
    return s;
}

} // namespace

// ============================================================================
TEST_CASE("Idle: fresh engine holds its seed position, not busy") {
    Engine e(testConfig(), 0.3f);
    CHECK(e.positionAt(0) == doctest::Approx(0.3f));
    CHECK(e.velocityAt(5 * kS) == doctest::Approx(0.0));
    CHECK_FALSE(e.isBusy(0));
    CHECK(e.mode() == Mode::Idle);
}

TEST_CASE("One-shot rest-to-rest: lands on target, at rest, on the deadline") {
    Engine e(testConfig(), 0.2f);

    Command c;
    c.target       = 0.8f;
    c.duration_us  = 600 * (uint32_t)kMs;
    c.has_duration = true;
    const uint64_t t0 = 1 * kS;
    REQUIRE(e.commit(c, t0));
    CHECK(e.mode() == Mode::OneShot);
    CHECK(e.isBusy(t0 + 10 * kMs));

    auto snap = e.snapshot(t0);
    // The move is easy under these limits, so minimum_duration should land
    // the deadline exactly.
    CHECK(snap.duration_s == doctest::Approx(0.6).epsilon(0.01));

    // Landed: position, rest, not busy.
    const uint64_t tEnd = t0 + 700 * kMs;
    CHECK(e.positionAt(tEnd) == doctest::Approx(0.8).epsilon(1e-4));
    CHECK(e.velocityAt(tEnd) == doctest::Approx(0.0).epsilon(1e-4));
    CHECK_FALSE(e.isBusy(tEnd));

    // No anomalies for a clean feasible move.
    slopmotion::Anomaly ev;
    CHECK_FALSE(e.popAnomaly(ev));
}

TEST_CASE("One-shot respects the v/a/j ceilings (sampled, 1 ms grid)") {
    auto cfg = testConfig();
    Engine e(cfg, 0.0f);

    Command c;
    c.target       = 1.0f;
    c.duration_us  = 900 * (uint32_t)kMs;
    c.has_duration = true;
    REQUIRE(e.commit(c, 0));

    auto s = sweep(e, 0, 1 * kS);
    CHECK(s.max_abs_v <= cfg.limits.vmax * 1.001);
    CHECK(s.max_abs_a <= cfg.limits.amax * 1.001);
    // Finite-difference jerk averages across switching instants — margin.
    CHECK(s.max_abs_j <= cfg.limits.jmax * 1.05 + 1.0);
    CHECK(s.min_p >= -1e-9);
    CHECK(s.max_p <= 1.0 + 1e-9);
}

TEST_CASE("Infeasible deadline stretches to physical minimum + anomaly") {
    auto cfg = testConfig();          // vmax = 2 → 0→1 takes ≥ 0.5 s
    Engine e(cfg, 0.0f);

    Command c;
    c.target       = 1.0f;
    c.duration_us  = 50 * (uint32_t)kMs;   // ludicrous 50 ms demand
    c.has_duration = true;
    REQUIRE(e.commit(c, 0));

    auto snap = e.snapshot(0);
    CHECK(snap.duration_s > 0.5f);    // stretched to ≥ distance / vmax

    slopmotion::Anomaly ev;
    REQUIRE(e.popAnomaly(ev));
    CHECK(ev.kind == (uint8_t)AnomalyType::DeadlineStretched);
    CHECK(ev.detail == doctest::Approx(snap.duration_s).epsilon(0.01));
}

TEST_CASE("Retarget mid-move is C2-continuous at the commit instant") {
    Engine e(testConfig(), 0.2f);

    Command a;
    a.target = 0.9f; a.duration_us = 800 * (uint32_t)kMs; a.has_duration = true;
    REQUIRE(e.commit(a, 0));

    const uint64_t tSwitch = 300 * kMs;
    const double p1 = e.positionAt(tSwitch);
    const double v1 = e.velocityAt(tSwitch);
    const double a1 = e.accelerationAt(tSwitch);
    REQUIRE(std::fabs(v1) > 0.1);     // genuinely mid-flight

    Command b;                        // bare chase point, hard reversal
    b.target = 0.1f;
    REQUIRE(e.commit(b, tSwitch));
    CHECK(e.mode() == Mode::Chase);

    // The new plan starts EXACTLY from the sampled state — no kink in p/v/a.
    CHECK(e.positionAt(tSwitch)     == doctest::Approx(p1).epsilon(1e-6));
    CHECK(e.velocityAt(tSwitch)     == doctest::Approx(v1).epsilon(1e-6));
    CHECK(e.accelerationAt(tSwitch) == doctest::Approx(a1).epsilon(1e-6));

    // And it still lands on the new target at rest.
    const uint64_t tEnd = tSwitch + 3 * kS;
    CHECK(e.positionAt(tEnd) == doctest::Approx(0.1).epsilon(1e-4));
    CHECK(e.velocityAt(tEnd) == doctest::Approx(0.0).epsilon(1e-4));
}

TEST_CASE("Chase: 60 Hz sine stream tracks smoothly within limits") {
    Config cfg;
    cfg.limits.vmax = 3.0f;
    cfg.limits.amax = 30.0f;
    cfg.limits.jmax = 500.0f;
    Engine e(cfg, 0.5f);

    // 0.75 Hz sine, amplitude 0.4 → peak vel ≈ 1.88, peak acc ≈ 8.9: well
    // inside the ceilings, so a good tracker should hug it.
    const double f = 0.75;
    auto target = [&](double t) {
        return 0.5 + 0.4 * std::sin(2.0 * M_PI * f * t);
    };

    double worst_err = 0.0;
    double prev_v = 0.0;
    bool have_prev = false;
    uint64_t next_cmd = 0;
    for (uint64_t t = 0; t <= 3 * kS; t += kMs) {
        if (t >= next_cmd) {
            Command c;
            c.target = (float)target((double)t * 1e-6);
            REQUIRE(e.commit(c, t));
            next_cmd += 16667;        // ~60 Hz point stream
        }
        const double p = e.positionAt(t);
        const double v = e.velocityAt(t);
        REQUIRE(std::isfinite(p));
        REQUIRE(p >= -1e-9);
        REQUIRE(p <= 1.0 + 1e-9);
        // Velocity must stay ceiling-bounded and step-continuous ACROSS
        // replans — this is the C2 no-microstutter claim, sampled.
        CHECK(std::fabs(v) <= cfg.limits.vmax * 1.001);
        if (have_prev) {
            CHECK(std::fabs(v - prev_v) <=
                  (double)cfg.limits.amax * 1e-3 * 1.05 + 1e-6);
        }
        prev_v = v; have_prev = true;
        if (t > 500 * kMs) {          // after initial catch-up
            worst_err = std::max(worst_err,
                                 std::fabs(p - target((double)t * 1e-6)));
        }
    }
    // Tracking lag exists (the engine chases points, it cannot see the
    // future): measured 0.125 peak on this sine ≈ 65 ms effective lag at
    // peak velocity — the ZOH + damped-feedforward price. This bound is a
    // regression tripwire, not a quality target; lag tuning is done with
    // eyes on the scenario graphs (examples/slopmotion_traces).
    CHECK(worst_err < 0.15);
}

TEST_CASE("Starve-settle: dead stream brakes to rest and holds") {
    Config cfg;
    cfg.limits.vmax = 3.0f;
    cfg.limits.amax = 30.0f;
    cfg.limits.jmax = 500.0f;
    Engine e(cfg, 0.5f);

    // Feed an ascending ramp with feedforward so the trajectory is mid-glide
    // with real velocity when the stream dies.
    uint64_t t = 0;
    for (int i = 0; i < 30; i++) {
        Command c;
        c.target = 0.2f + 0.02f * (float)i;   // steady 1.2 units/s ramp
        REQUIRE(e.commit(c, t));
        t += 16667;
    }
    const uint64_t t_dead = t;

    // Keep sampling — no more commands. The engine must brake to rest.
    double final_p = -1.0;
    for (uint64_t ts = t_dead; ts <= t_dead + 2 * kS; ts += kMs) {
        final_p = e.positionAt(ts);
    }
    CHECK(e.velocityAt(t_dead + 2 * kS) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK_FALSE(e.isBusy(t_dead + 2 * kS));
    CHECK(e.mode() == Mode::Idle);

    // Position frozen after settle (hold, no drift).
    CHECK(e.positionAt(t_dead + 3 * kS) == doctest::Approx(final_p).epsilon(1e-9));

    // A SettleEngaged anomaly was recorded.
    slopmotion::Anomaly ev;
    bool saw_settle = false;
    while (e.popAnomaly(ev)) {
        if (ev.kind == (uint8_t)AnomalyType::SettleEngaged) saw_settle = true;
    }
    CHECK(saw_settle);
}

TEST_CASE("End velocity near a wall is clamped bound-safe") {
    auto cfg = testConfig();
    Engine e(cfg, 0.5f);

    Command c;                        // v4 point: land at 0.98 STILL MOVING fast
    c.target       = 0.98f;
    c.duration_us  = 400 * (uint32_t)kMs;
    c.has_duration = true;
    c.end_vel      = 1.5f;
    c.has_end_vel  = true;
    REQUIRE(e.commit(c, 0));

    slopmotion::Anomaly ev;
    bool saw_clamp = false;
    while (e.popAnomaly(ev)) {
        if (ev.kind == (uint8_t)AnomalyType::EndVelClamped) {
            saw_clamp = true;
            // clamped to √(amax·dist) = √(20·0.02) ≈ 0.632, not the asked 1.5
            CHECK(std::fabs(ev.detail) <= std::sqrt(20.0 * 0.02) + 1e-6);
        }
    }
    CHECK(saw_clamp);

    // Sampled trajectory (then starve-settle) never leaves the window.
    auto s = sweep(e, 0, 2 * kS);
    CHECK(s.max_p <= 1.0 + 1e-9);
    CHECK(s.min_p >= -1e-9);
    CHECK(e.velocityAt(2 * kS) == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("Non-finite input is rejected; previous plan keeps executing") {
    Engine e(testConfig(), 0.2f);

    Command good;
    good.target = 0.7f; good.duration_us = 500 * (uint32_t)kMs;
    good.has_duration = true;
    REQUIRE(e.commit(good, 0));

    Command evil;
    evil.target = std::nanf("");
    CHECK_FALSE(e.commit(evil, 100 * kMs));

    slopmotion::Anomaly ev;
    REQUIRE(e.popAnomaly(ev));
    CHECK(ev.kind == (uint8_t)AnomalyType::PlanFailed);
    CHECK(ev.detail == doctest::Approx(-99.0f));

    // The good plan is untouched and still lands.
    CHECK(e.positionAt(600 * kMs) == doctest::Approx(0.7).epsilon(1e-4));
    CHECK(e.snapshot(600 * kMs).failures == 1);
}

TEST_CASE("Determinism: identical command/time sequences → identical samples") {
    auto run = [](std::vector<float>& out) {
        Config cfg;
        cfg.limits.vmax = 3.0f; cfg.limits.amax = 25.0f; cfg.limits.jmax = 400.0f;
        Engine e(cfg, 0.5f);
        uint64_t next_cmd = 0;
        int i = 0;
        for (uint64_t t = 0; t <= 2 * kS; t += kMs) {
            if (t >= next_cmd) {
                Command c;
                c.target = 0.5f + 0.35f * std::sin(0.3 * (double)i);
                if (i % 3 == 0) {      // mix modes
                    c.duration_us = 120 * (uint32_t)kMs;
                    c.has_duration = true;
                }
                e.commit(c, t);
                next_cmd += 40 * kMs;
                i++;
            }
            out.push_back(e.positionAt(t));
            out.push_back(e.velocityAt(t));
        }
    };
    std::vector<float> a, b;
    run(a); run(b);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); i++) REQUIRE(a[i] == b[i]);
}

TEST_CASE("Reset drops everything back to a hold") {
    Engine e(testConfig(), 0.5f);
    Command c;
    c.target = 0.9f; c.duration_us = 500 * (uint32_t)kMs; c.has_duration = true;
    REQUIRE(e.commit(c, 0));
    REQUIRE(e.isBusy(100 * kMs));

    e.resetAt(0.42f, 200 * kMs);
    CHECK_FALSE(e.isBusy(200 * kMs));
    CHECK(e.positionAt(250 * kMs) == doctest::Approx(0.42f));
    CHECK(e.velocityAt(250 * kMs) == doctest::Approx(0.0));
    CHECK(e.mode() == Mode::Idle);
}
