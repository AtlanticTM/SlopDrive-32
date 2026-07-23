// ============================================================================
// test_main.cpp — doctest unit tests for TrapezoidProfile (MotionProfile.h)
//
// This is a native (host-side, hardware-free) test: MotionProfile.h has no
// bus/FreeRTOS/I-O dependencies, so it is exercised directly with doctest's
// bundled main(). PlatformIO's native test runner reads the process exit
// code, which doctest's default main already provides on failure.
//
// Ground truth for the "Example A..E" cases below is transcribed from the
// worked-example comments at the bottom of MotionProfile.h — every numeric
// expectation here matches those comments by construction, not by guessing.
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "MotionProfile.h"

#include <cmath>
#include <cstdint>

namespace {

// Convert a float count of seconds to the int64_t microsecond epoch
// TrapezoidProfile expects for t0_us / sample()'s now_us.
int64_t usAt(double seconds) {
    return static_cast<int64_t>(seconds * 1.0e6);
}

} // namespace

// ============================================================================
// 1. Trivial case — already at target, already at rest.
// ============================================================================
TEST_CASE("Trivial: already at target and at rest completes immediately") {
    auto pr = TrapezoidProfile::plan(/*p0=*/1000.0f, /*v0=*/0.0f,
                                      /*target=*/1000.0f,
                                      /*vmax=*/5000.0f, /*accel=*/25000.0f,
                                      /*now_us=*/0);

    REQUIRE(pr.valid);
    CHECK(pr.t_pre == doctest::Approx(0.0f));
    CHECK(pr.t_acc == doctest::Approx(0.0f));
    CHECK(pr.t_cruise == doctest::Approx(0.0f));
    CHECK(pr.t_dec == doctest::Approx(0.0f));
    CHECK(pr.totalTimeS() == doctest::Approx(0.0f));
    CHECK(pr.done(0));

    float pos, vel;
    pr.sample(0, pos, vel);
    CHECK(pos == doctest::Approx(1000.0f));
    CHECK(vel == doctest::Approx(0.0f));

    // Sampling later still clamps to (target, 0) — nothing to "complete" twice.
    pr.sample(usAt(5.0), pos, vel);
    CHECK(pos == doctest::Approx(1000.0f));
    CHECK(vel == doctest::Approx(0.0f));
}

TEST_CASE("Trivial: within position/velocity epsilon still counts as trivial") {
    // POSITION_EPS == VELOCITY_EPS == 0.5f — a sub-epsilon gap and a
    // sub-epsilon creep velocity must still take the trivial branch.
    auto pr = TrapezoidProfile::plan(/*p0=*/1000.0f, /*v0=*/0.3f,
                                      /*target=*/1000.3f,
                                      /*vmax=*/5000.0f, /*accel=*/25000.0f,
                                      /*now_us=*/0);
    REQUIRE(pr.valid);
    CHECK(pr.totalTimeS() == doctest::Approx(0.0f));
}

// ============================================================================
// 2. Rest-to-rest trapezoid — Example A from the header comments.
//    plan(p0=0, v0=0, target=10000, vmax=5000, accel=25000)
//    -> t_acc=0.2s, t_cruise=1.8s, t_dec=0.2s, total=2.2s
// ============================================================================
TEST_CASE("Rest-to-rest trapezoid reaches vmax (Example A)") {
    auto pr = TrapezoidProfile::plan(0.0f, 0.0f, 10000.0f, 5000.0f, 25000.0f, 0);

    REQUIRE(pr.valid);
    CHECK(pr.t_pre == doctest::Approx(0.0f));
    CHECK(pr.t_acc == doctest::Approx(0.2f).epsilon(0.002));
    CHECK(pr.t_cruise == doctest::Approx(1.8f).epsilon(0.002));
    CHECK(pr.t_dec == doctest::Approx(0.2f).epsilon(0.002));
    CHECK(pr.totalTimeS() == doctest::Approx(2.2f).epsilon(0.002));

    float pos, vel;

    // Start: at rest, at the origin.
    pr.sample(usAt(0.0), pos, vel);
    CHECK(pos == doctest::Approx(0.0f));
    CHECK(vel == doctest::Approx(0.0f));

    // Cruise phase (anywhere in (0.2s, 2.0s)) must peak at exactly vmax.
    pr.sample(usAt(1.1), pos, vel);
    CHECK(vel == doctest::Approx(5000.0f).epsilon(0.002));
    // Midpoint-in-time of a symmetric rest-to-rest trapezoid lands at the
    // midpoint-in-distance too: 500 (accel dist) + 4500 (cruise so far) = 5000.
    CHECK(pos == doctest::Approx(5000.0f).epsilon(0.002));

    // End: fully at rest at the target.
    pr.sample(usAt(2.2), pos, vel);
    CHECK(pos == doctest::Approx(10000.0f).epsilon(0.002));
    CHECK(vel == doctest::Approx(0.0f));
    CHECK(pr.done(usAt(2.2)));

    // Symmetry: pos(t) + pos(total - t) == target for a rest-to-rest profile,
    // since the decel leg mirrors the accel leg.
    float posEarly, velEarly, posLate, velLate;
    pr.sample(usAt(0.1), posEarly, velEarly);
    pr.sample(usAt(2.1), posLate, velLate);
    CHECK((posEarly + posLate) == doctest::Approx(10000.0f).epsilon(0.002));
}

// ============================================================================
// 3. Rest-to-rest triangle — Example B from the header comments.
//    plan(p0=0, v0=0, target=500, vmax=5000, accel=25000)
//    -> vp = sqrt(25000*500) ~= 3535.53, t_acc=t_dec ~= 0.14142s, t_cruise=0
// ============================================================================
TEST_CASE("Rest-to-rest triangle never reaches vmax (Example B)") {
    auto pr = TrapezoidProfile::plan(0.0f, 0.0f, 500.0f, 5000.0f, 25000.0f, 0);

    const float vp = std::sqrt(25000.0f * 500.0f); // ~3535.53
    const float tAccExpected = vp / 25000.0f;       // ~0.14142s

    REQUIRE(pr.valid);
    CHECK(pr.t_cruise == doctest::Approx(0.0f));
    CHECK(pr.t_acc == doctest::Approx(tAccExpected).epsilon(0.002));
    CHECK(pr.t_dec == doctest::Approx(tAccExpected).epsilon(0.002)); // symmetric accel/decel
    CHECK(pr.totalTimeS() == doctest::Approx(2.0f * tAccExpected).epsilon(0.002));

    float pos, vel;
    pr.sample(usAt(tAccExpected), pos, vel); // exact peak, boundary of accel/decel
    CHECK(vel < 5000.0f);
    CHECK(vel == doctest::Approx(vp).epsilon(0.01));

    pr.sample(usAt(pr.totalTimeS() + 0.001), pos, vel); // just PAST the end:
    // sampling exactly AT totalTimeS() lands 1us inside the decel leg via
    // float->us truncation, leaving a tiny residual velocity. Final-state
    // semantics are "at or after completion" -> sample past the boundary.
    CHECK(pos == doctest::Approx(500.0f).epsilon(0.005));
    CHECK(vel == doctest::Approx(0.0f));
}

// ============================================================================
// 4. Moving-start, same direction — Example C from the header comments.
//    plan(p0=0, v0=2000, target=10000, vmax=5000, accel=25000)
//    -> no pre-segment; entry speed folds straight into the accel leg.
// ============================================================================
TEST_CASE("Moving-start same-direction folds v0 into accel leg (Example C)") {
    auto pr = TrapezoidProfile::plan(0.0f, 2000.0f, 10000.0f, 5000.0f, 25000.0f, 0);

    REQUIRE(pr.valid);
    CHECK(pr.t_pre == doctest::Approx(0.0f));
    CHECK(pr.t_acc == doctest::Approx(0.12f).epsilon(0.005));
    CHECK(pr.t_cruise == doctest::Approx(1.816f).epsilon(0.002));
    CHECK(pr.t_dec == doctest::Approx(0.2f).epsilon(0.005));

    float pos, vel;

    // Continuity: sample(0) must reproduce the initial velocity and position
    // exactly — the executor hands off from "wherever the machine already is."
    pr.sample(usAt(0.0), pos, vel);
    CHECK(pos == doctest::Approx(0.0f));
    CHECK(vel == doctest::Approx(2000.0f).epsilon(0.01));

    // Velocity keeps climbing (never dips) through the accel leg.
    pr.sample(usAt(0.06), pos, vel); // halfway through t_acc
    CHECK(vel > 2000.0f);
    CHECK(vel < 5000.0f);

    pr.sample(usAt(pr.totalTimeS() + 0.001), pos, vel); // just PAST the end:
    // sampling exactly AT totalTimeS() lands 1us inside the decel leg via
    // float->us truncation, leaving a tiny residual velocity. Final-state
    // semantics are "at or after completion" -> sample past the boundary.
    CHECK(pos == doctest::Approx(10000.0f).epsilon(0.002));
    CHECK(vel == doctest::Approx(0.0f));
}

// ============================================================================
// 5. Reversal — Example D from the header comments.
//    plan(p0=0, v0=-3000, target=10000, vmax=5000, accel=25000)
//    -> t_pre=0.12s, p1=-180 (overshoots backward while braking), then a
//       fresh trapezoid from -180 to 10000.
// ============================================================================
TEST_CASE("Reversal: v0 away from target brakes through a pre-segment (Example D)") {
    auto pr = TrapezoidProfile::plan(0.0f, -3000.0f, 10000.0f, 5000.0f, 25000.0f, 0);

    REQUIRE(pr.valid);
    CHECK(pr.t_pre == doctest::Approx(0.12f).epsilon(0.005));
    CHECK(pr._p1 == doctest::Approx(-180.0f).epsilon(0.01));
    CHECK(pr.t_acc == doctest::Approx(0.2f).epsilon(0.005));
    CHECK(pr.t_cruise == doctest::Approx(1.836f).epsilon(0.002));
    CHECK(pr.t_dec == doctest::Approx(0.2f).epsilon(0.005));
    CHECK(pr.totalTimeS() == doctest::Approx(2.356f).epsilon(0.003));

    float pos, vel;

    // sample(0) must reproduce the (wrong-way) initial velocity.
    pr.sample(usAt(0.0), pos, vel);
    CHECK(pos == doctest::Approx(0.0f));
    CHECK(vel == doctest::Approx(-3000.0f).epsilon(0.01));

    // Mid-pre-segment: still braking, velocity still negative (hasn't
    // crossed zero yet) — this IS the "pre-decel segment exists" check.
    pr.sample(usAt(0.06), pos, vel);
    CHECK(vel < 0.0f);
    CHECK(vel == doctest::Approx(-1500.0f).epsilon(0.02));

    // End of the pre-segment: velocity has crossed to (approximately) zero,
    // and position has landed exactly where the header says it should: -180.
    pr.sample(usAt(0.12), pos, vel);
    CHECK(vel == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(pos == doctest::Approx(-180.0f).epsilon(0.01));

    // Shortly after: main trapezoid has taken over, now accelerating toward
    // +target (positive velocity).
    pr.sample(usAt(0.2), pos, vel);
    CHECK(vel > 0.0f);

    // Final state: parked exactly on target, at rest.
    pr.sample(usAt(pr.totalTimeS() + 0.001), pos, vel); // just PAST the end:
    // sampling exactly AT totalTimeS() lands 1us inside the decel leg via
    // float->us truncation, leaving a tiny residual velocity. Final-state
    // semantics are "at or after completion" -> sample past the boundary.
    CHECK(pos == doctest::Approx(10000.0f).epsilon(0.003));
    CHECK(vel == doctest::Approx(0.0f));
    CHECK(pr.done(usAt(pr.totalTimeS() + 0.001)));
}

// ============================================================================
// 6. Overshoot — Example E from the header comments.
//    plan(p0=0, v0=4000, target=100, vmax=5000, accel=25000)
//    -> same-direction but can't stop in time (stopDist=320 > D=100):
//       pre-segment carries the carriage PAST the target to p1=320, then a
//       short reverse triangle brings it back to 100.
// ============================================================================
TEST_CASE("Overshoot: same-direction but too fast to stop in time (Example E)") {
    auto pr = TrapezoidProfile::plan(0.0f, 4000.0f, 100.0f, 5000.0f, 25000.0f, 0);

    const float vp = std::sqrt(25000.0f * 220.0f); // ~2345.21
    const float tAccExpected = vp / 25000.0f;       // ~0.09381s

    REQUIRE(pr.valid);
    CHECK(pr.t_pre == doctest::Approx(0.16f).epsilon(0.005));
    CHECK(pr._p1 == doctest::Approx(320.0f).epsilon(0.01));
    CHECK(pr.t_cruise == doctest::Approx(0.0f)); // triangle, not trapezoid
    CHECK(pr.t_acc == doctest::Approx(tAccExpected).epsilon(0.005));
    CHECK(pr.t_dec == doctest::Approx(tAccExpected).epsilon(0.005));
    CHECK(pr.totalTimeS() == doctest::Approx(0.16f + 2.0f * tAccExpected).epsilon(0.005));

    float pos, vel;

    // During the pre-segment the carriage is still moving FORWARD (positive
    // velocity, decreasing) — it doesn't reverse until the main trapezoid.
    pr.sample(usAt(0.1), pos, vel);
    CHECK(vel > 0.0f);
    CHECK(pos > 0.0f);
    CHECK(pos < 320.0f);

    // End of pre-segment: parked (momentarily) at the overshoot point 320,
    // 220 counts past the 100 target.
    pr.sample(usAt(0.16), pos, vel);
    CHECK(pos == doctest::Approx(320.0f).epsilon(0.01));
    CHECK(vel == doctest::Approx(0.0f).epsilon(0.05));

    // Main (reverse) triangle: velocity goes negative, heading back to target.
    pr.sample(usAt(0.16 + tAccExpected), pos, vel);
    CHECK(vel < 0.0f);
    CHECK(vel == doctest::Approx(-vp).epsilon(0.02));

    // Final state: exactly on target, at rest.
    pr.sample(usAt(pr.totalTimeS() + 0.001), pos, vel); // just PAST the end:
    // sampling exactly AT totalTimeS() lands 1us inside the decel leg via
    // float->us truncation, leaving a tiny residual velocity. Final-state
    // semantics are "at or after completion" -> sample past the boundary.
    CHECK(pos == doctest::Approx(100.0f).epsilon(0.02));
    CHECK(vel == doctest::Approx(0.0f));
}

// ============================================================================
// 7. Edge inputs: zero / negative vmax or accel must be floored, never NaN.
//    plan() clamps vmax/accel to a 1.0f floor before doing any math with them
//    (division, sqrtf) — verify that floor is actually applied and the
//    resulting profile is finite and sane, not that literal zero survives.
// ============================================================================
TEST_CASE("Zero or negative vmax/accel are floored — never produce NaN/Inf") {
    SUBCASE("vmax == 0, accel == 0") {
        auto pr = TrapezoidProfile::plan(0.0f, 0.0f, 1000.0f, 0.0f, 0.0f, 0);
        REQUIRE(pr.valid);
        CHECK(pr.vmax == doctest::Approx(1.0f));   // floored
        CHECK(pr.accel == doctest::Approx(1.0f));  // floored
        CHECK_FALSE(std::isnan(pr.t_acc));
        CHECK_FALSE(std::isnan(pr.t_cruise));
        CHECK_FALSE(std::isnan(pr.t_dec));
        CHECK_FALSE(std::isinf(pr.totalTimeS()));

        float pos, vel;
        pr.sample(usAt(0.0), pos, vel);
        CHECK_FALSE(std::isnan(pos));
        CHECK_FALSE(std::isnan(vel));
        pr.sample(usAt(pr.totalTimeS() + 0.001), pos, vel); // just PAST the end:
    // sampling exactly AT totalTimeS() lands 1us inside the decel leg via
    // float->us truncation, leaving a tiny residual velocity. Final-state
    // semantics are "at or after completion" -> sample past the boundary.
        CHECK(pos == doctest::Approx(1000.0f).epsilon(0.002));
        CHECK(vel == doctest::Approx(0.0f));
    }

    SUBCASE("negative vmax, negative accel (garbage input)") {
        auto pr = TrapezoidProfile::plan(0.0f, 0.0f, 1000.0f, -5.0f, -10.0f, 0);
        REQUIRE(pr.valid);
        CHECK(pr.vmax == doctest::Approx(1.0f));
        CHECK(pr.accel == doctest::Approx(1.0f));
        CHECK_FALSE(std::isnan(pr.t_acc));
        CHECK_FALSE(std::isnan(pr.t_dec));

        float pos, vel;
        pr.sample(usAt(pr.totalTimeS() + 0.001), pos, vel); // just PAST the end:
    // sampling exactly AT totalTimeS() lands 1us inside the decel leg via
    // float->us truncation, leaving a tiny residual velocity. Final-state
    // semantics are "at or after completion" -> sample past the boundary.
        CHECK_FALSE(std::isnan(pos));
        CHECK(pos == doctest::Approx(1000.0f).epsilon(0.002));
    }
}

// ============================================================================
// 8. Fine-grained sampling: position stays continuous, velocity never exceeds
//    vmax (by more than a hair), and finite-difference acceleration never
//    exceeds the accel ceiling by more than the stated 1.1x tolerance.
// ============================================================================
static void checkFineSampling(const TrapezoidProfile& pr, float vmax, float accel,
                               double dt = 0.0005) {
    float prevPos, prevVel;
    pr.sample(usAt(0.0), prevPos, prevVel);

    double t = dt;
    const double total = static_cast<double>(pr.totalTimeS());
    while (t <= total + dt) {
        float pos, vel;
        pr.sample(usAt(t), pos, vel);

        // Position continuity: no jump bigger than one dt's worth of travel
        // at vmax (with slack for the coarse dt used here).
        float posJump = std::fabs(pos - prevPos);
        CHECK(posJump <= vmax * static_cast<float>(dt) * 1.5f);

        // Velocity never exceeds the ceiling (beyond float slop).
        CHECK(std::fabs(vel) <= vmax * 1.001f);

        // Finite-difference acceleration never exceeds the accel ceiling by
        // more than the requested 1.1x tolerance.
        float fdAccel = std::fabs(vel - prevVel) / static_cast<float>(dt);
        CHECK(fdAccel <= accel * 1.1f + 5.0f); // +5.0f: absolute float-noise floor

        prevPos = pos;
        prevVel = vel;
        t += dt;
    }
}

TEST_CASE("Fine dt sampling is continuous and respects vmax/accel ceilings") {
    SUBCASE("rest-to-rest trapezoid (Example A)") {
        auto pr = TrapezoidProfile::plan(0.0f, 0.0f, 10000.0f, 5000.0f, 25000.0f, 0);
        checkFineSampling(pr, 5000.0f, 25000.0f);
    }
    SUBCASE("rest-to-rest triangle (Example B)") {
        auto pr = TrapezoidProfile::plan(0.0f, 0.0f, 500.0f, 5000.0f, 25000.0f, 0);
        checkFineSampling(pr, 5000.0f, 25000.0f);
    }
    SUBCASE("reversal with pre-segment (Example D)") {
        auto pr = TrapezoidProfile::plan(0.0f, -3000.0f, 10000.0f, 5000.0f, 25000.0f, 0);
        checkFineSampling(pr, 5000.0f, 25000.0f);
    }
    SUBCASE("overshoot with pre-segment (Example E)") {
        auto pr = TrapezoidProfile::plan(0.0f, 4000.0f, 100.0f, 5000.0f, 25000.0f, 0);
        checkFineSampling(pr, 5000.0f, 25000.0f);
    }
}

// ============================================================================
// 9. done()/totalTimeS() agreement with sample()'s own clamping.
// ============================================================================
TEST_CASE("done() flips exactly when sample() starts clamping to target") {
    auto pr = TrapezoidProfile::plan(0.0f, 0.0f, 10000.0f, 5000.0f, 25000.0f, 0);

    CHECK_FALSE(pr.done(usAt(pr.totalTimeS() - 0.05)));
    CHECK(pr.done(usAt(pr.totalTimeS() + 0.001)));

    float pos, vel;
    pr.sample(usAt(pr.totalTimeS() + 1.0), pos, vel); // long after completion
    CHECK(pos == doctest::Approx(10000.0f).epsilon(0.002));
    CHECK(vel == doctest::Approx(0.0f));
}
