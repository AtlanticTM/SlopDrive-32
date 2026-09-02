// RangeMapper -- host suite for the stroke-window math (board review D1/D2).
// Constraints:
// - Compiles range_mapper.cpp directly: env:native leaves test_build_src off,
//   so the translation unit under test has to come in through this file.
// - Arduino.h in this directory is the host shim config_api.h needs.
// - Pure float math, no clock, no hardware: every assertion here is
//   deterministic and must stay that way.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "../../../src/motion/range_mapper.cpp"

namespace {

// The invariants every published window must satisfy, whatever the caller asked
// for: inside the rail, ordered, and at least MIN_STROKE_WINDOW_MM wide unless
// the rail itself is shorter than that.
void checkWindow(Window w, float rail) {
    CHECK(w.min_mm >= 0.0f);
    CHECK(w.max_mm <= doctest::Approx(rail));
    CHECK(w.min_mm <= w.max_mm);
    if (rail >= MIN_STROKE_WINDOW_MM) {
        CHECK(w.max_mm - w.min_mm >= doctest::Approx(MIN_STROKE_WINDOW_MM));
    } else {
        CHECK(w.min_mm == doctest::Approx(0.0f));
        CHECK(w.max_mm == doctest::Approx(rail));
    }
}

// Settle the glide, asserting at every step that the effective window moved
// toward the goal and never past it. Returns the tick count consumed.
int glideToRest(RangeMapper& m, float dt_s, float rate_mm_s, int max_ticks) {
    for (int i = 0; i < max_ticks; ++i) {
        const Window before = m.effectiveWindow();
        const Window goal   = m.goalWindow();
        if (before.min_mm == goal.min_mm && before.max_mm == goal.max_mm) return i;
        m.tick(dt_s, rate_mm_s);
        const Window after = m.effectiveWindow();
        // Closer than before, and never on the far side of the goal.
        CHECK(std::fabs(after.min_mm - goal.min_mm) <= std::fabs(before.min_mm - goal.min_mm));
        CHECK(std::fabs(after.max_mm - goal.max_mm) <= std::fabs(before.max_mm - goal.max_mm));
        CHECK(((after.min_mm - goal.min_mm) * (before.min_mm - goal.min_mm)) >= 0.0f);
        CHECK(((after.max_mm - goal.max_mm) * (before.max_mm - goal.max_mm)) >= 0.0f);
    }
    FAIL("glide never reached the goal");
    return max_ticks;
}

}  // namespace

TEST_CASE("boot state is the whole default rail, goal and effective agreeing") {
    RangeMapper m;
    CHECK(m.getMaxRailMm() == doctest::Approx(DEFAULT_MAX_RAIL_MM));
    CHECK(m.getMinMm() == doctest::Approx(0.0f));
    CHECK(m.getMaxMm() == doctest::Approx(DEFAULT_MAX_RAIL_MM));
    CHECK(m.getGoalMinMm() == doctest::Approx(m.getMinMm()));
    CHECK(m.getGoalMaxMm() == doctest::Approx(m.getMaxMm()));
    checkWindow(m.effectiveWindow(), DEFAULT_MAX_RAIL_MM);
}

TEST_CASE("setRange swaps a reversed pair and clamps to the rail") {
    RangeMapper m;

    m.setRange(80.0f, 20.0f);
    CHECK(m.getGoalMinMm() == doctest::Approx(20.0f));
    CHECK(m.getGoalMaxMm() == doctest::Approx(80.0f));

    m.setRange(-50.0f, 9999.0f);
    CHECK(m.getGoalMinMm() == doctest::Approx(0.0f));
    CHECK(m.getGoalMaxMm() == doctest::Approx(DEFAULT_MAX_RAIL_MM));

    // setRange moves the GOAL only: the effective window still has to glide.
    m.setRangeImmediate(100.0f, 200.0f);
    m.setRange(150.0f, 160.0f);
    CHECK(m.getMinMm() == doctest::Approx(100.0f));
    CHECK(m.getMaxMm() == doctest::Approx(200.0f));
}

TEST_CASE("minimum span widens away from whichever rail end is blocked") {
    RangeMapper m;   // 500 mm rail

    SUBCASE("room above: max moves up") {
        m.setRange(10.0f, 11.0f);
        CHECK(m.getGoalMinMm() == doctest::Approx(10.0f));
        CHECK(m.getGoalMaxMm() == doctest::Approx(10.0f + MIN_STROKE_WINDOW_MM));
        checkWindow(m.goalWindow(), 500.0f);
    }

    SUBCASE("max pinned at the rail: min is pulled DOWN, never past the rail") {
        m.setRange(498.0f, 499.0f);
        CHECK(m.getGoalMaxMm() == doctest::Approx(500.0f));
        CHECK(m.getGoalMinMm() == doctest::Approx(500.0f - MIN_STROKE_WINDOW_MM));
        checkWindow(m.goalWindow(), 500.0f);
    }

    SUBCASE("degenerate request exactly at the rail") {
        m.setRange(500.0f, 500.0f);
        CHECK(m.getGoalMaxMm() == doctest::Approx(500.0f));
        checkWindow(m.goalWindow(), 500.0f);
    }

    SUBCASE("degenerate request exactly at home") {
        m.setRange(0.0f, 0.0f);
        CHECK(m.getGoalMinMm() == doctest::Approx(0.0f));
        CHECK(m.getGoalMaxMm() == doctest::Approx(MIN_STROKE_WINDOW_MM));
        checkWindow(m.goalWindow(), 500.0f);
    }
}

TEST_CASE("a rail shorter than the minimum span IS the window") {
    RangeMapper m;
    m.setMaxRailMm(3.0f);
    checkWindow(m.goalWindow(), 3.0f);
    CHECK(m.getGoalMinMm() == doctest::Approx(0.0f));
    CHECK(m.getGoalMaxMm() == doctest::Approx(3.0f));

    m.setRange(1.0f, 2.0f);
    CHECK(m.getGoalMinMm() == doctest::Approx(0.0f));
    CHECK(m.getGoalMaxMm() == doctest::Approx(3.0f));
    checkWindow(m.goalWindow(), 3.0f);
}

// sd-tki.5, the bug this suite exists for: setMaxRailMm re-enters setRange with
// the existing goals, and the old min-span bump ran AFTER the rail clamp with
// no re-clamp, so a rail-length edit could publish a window past the rail.
TEST_CASE("a rail shrink never leaves a window outside the rail") {
    RangeMapper m;
    m.setRangeImmediate(0.0f, 500.0f);

    SUBCASE("shrink under a full-rail window") {
        m.setMaxRailMm(100.0f);
        checkWindow(m.goalWindow(), 100.0f);
        m.tick(1.0f, 1000.0f);
        checkWindow(m.effectiveWindow(), 100.0f);
    }

    SUBCASE("shrink under a window already at the minimum span") {
        m.setRange(496.0f, 500.0f);      // widened to [495, 500]
        m.setMaxRailMm(50.0f);
        CHECK(m.getGoalMaxMm() == doctest::Approx(50.0f));
        CHECK(m.getGoalMinMm() == doctest::Approx(45.0f));
        checkWindow(m.goalWindow(), 50.0f);
    }

    SUBCASE("shrink below the minimum span entirely") {
        m.setRange(400.0f, 450.0f);
        m.setMaxRailMm(2.0f);
        checkWindow(m.goalWindow(), 2.0f);
    }

    SUBCASE("the effective window is snapped by tick(), with no glide ramp") {
        m.setRangeImmediate(0.0f, 500.0f);
        m.setMaxRailMm(100.0f);
        // One tick at a glide rate far too slow to have walked there.
        m.tick(0.001f, 1.0f);
        CHECK(m.getMaxMm() <= doctest::Approx(100.0f));
        checkWindow(m.effectiveWindow(), 100.0f);
    }

    SUBCASE("a rail GROW does not widen the window on its own") {
        m.setRangeImmediate(10.0f, 40.0f);
        m.setMaxRailMm(2000.0f);
        CHECK(m.getGoalMinMm() == doctest::Approx(10.0f));
        CHECK(m.getGoalMaxMm() == doctest::Approx(40.0f));
    }
}

TEST_CASE("glide lands exactly on the goal and never overshoots, for any dt") {
    // Parametrized on the STEP PER TICK relative to the longest leg (350 mm),
    // which is the only thing the glide math can be sensitive to. Each step is
    // then produced by two different (dt, rate) factorizations, so a bug that
    // reads only one of the two factors still fails.
    const float steps[] = {0.02f, 0.137f, 1.0f, 13.5f, 349.0f, 350.0f, 351.0f, 5000.0f};

    for (float step : steps) {
        for (int form = 0; form < 2; ++form) {
            const float dt   = (form == 0) ? 0.001f : (step / 50.0f);
            const float rate = (form == 0) ? (step / 0.001f) : 50.0f;
            CAPTURE(step);
            CAPTURE(dt);
            CAPTURE(rate);
            const int budget = int(350.0f / step) + 8;

            RangeMapper m;
            m.setRangeImmediate(50.0f, 400.0f);
            m.setRange(120.0f, 130.0f);          // both ends move inward
            glideToRest(m, dt, rate, budget);
            CHECK(m.getMinMm() == doctest::Approx(m.getGoalMinMm()));
            CHECK(m.getMaxMm() == doctest::Approx(m.getGoalMaxMm()));
            CHECK(m.getMinMm() == doctest::Approx(120.0f));
            CHECK(m.getMaxMm() == doctest::Approx(130.0f));

            m.setRange(20.0f, 480.0f);           // and back outward
            glideToRest(m, dt, rate, budget);
            CHECK(m.getMinMm() == doctest::Approx(20.0f));
            CHECK(m.getMaxMm() == doctest::Approx(480.0f));
        }
    }
}

TEST_CASE("a single oversized tick lands on the goal, not past it") {
    RangeMapper m;
    m.setRangeImmediate(0.0f, 500.0f);
    m.setRange(200.0f, 210.0f);
    m.tick(1000.0f, 1e6f);               // step dwarfs the distance
    CHECK(m.getMinMm() == doctest::Approx(200.0f));
    CHECK(m.getMaxMm() == doctest::Approx(210.0f));
}

TEST_CASE("tick is a no-op for a non-positive dt or rate") {
    RangeMapper m;
    m.setRangeImmediate(100.0f, 200.0f);
    m.setRange(300.0f, 400.0f);
    for (float dt : {0.0f, -1.0f}) m.tick(dt, 100.0f);
    for (float rate : {0.0f, -1.0f}) m.tick(0.01f, rate);
    CHECK(m.getMinMm() == doctest::Approx(100.0f));
    CHECK(m.getMaxMm() == doctest::Approx(200.0f));
}

TEST_CASE("intensity maps onto the effective window and clamps at both ends") {
    RangeMapper m;
    m.setRangeImmediate(100.0f, 200.0f);

    CHECK(m.getRangeSize() == doctest::Approx(100.0f));
    CHECK(m.getCenterPosition() == doctest::Approx(150.0f));
    CHECK(m.intensityToPosition(0.0f) == doctest::Approx(100.0f));
    CHECK(m.intensityToPosition(1.0f) == doctest::Approx(200.0f));
    CHECK(m.intensityToPosition(0.25f) == doctest::Approx(125.0f));
    CHECK(m.intensityToPosition(-3.0f) == doctest::Approx(100.0f));
    CHECK(m.intensityToPosition(9.0f) == doctest::Approx(200.0f));

    CHECK(m.positionToIntensity(100.0f) == doctest::Approx(0.0f));
    CHECK(m.positionToIntensity(200.0f) == doctest::Approx(1.0f));
    CHECK(m.positionToIntensity(175.0f) == doctest::Approx(0.75f));
    CHECK(m.positionToIntensity(-500.0f) == doctest::Approx(0.0f));
    CHECK(m.positionToIntensity(5000.0f) == doctest::Approx(1.0f));

    for (int i = 0; i <= 20; ++i) {
        const float x = float(i) / 20.0f;
        CHECK(m.positionToIntensity(m.intensityToPosition(x)) == doctest::Approx(x));
    }
}

TEST_CASE("clampToPhysicalLimits tracks the rail, not the window") {
    RangeMapper m;
    m.setRangeImmediate(100.0f, 200.0f);
    CHECK(m.clampToPhysicalLimits(-1.0f) == doctest::Approx(0.0f));
    CHECK(m.clampToPhysicalLimits(450.0f) == doctest::Approx(450.0f));
    CHECK(m.clampToPhysicalLimits(600.0f) == doctest::Approx(500.0f));

    m.setMaxRailMm(120.0f);
    CHECK(m.clampToPhysicalLimits(450.0f) == doctest::Approx(120.0f));
}

TEST_CASE("a non-positive rail is refused, leaving the ceiling intact") {
    RangeMapper m;
    m.setMaxRailMm(0.0f);
    m.setMaxRailMm(-10.0f);
    CHECK(m.getMaxRailMm() == doctest::Approx(DEFAULT_MAX_RAIL_MM));
}

// Every reachable (request, rail) combination has to satisfy the invariants.
// This is the guard that makes the ordering inside fitToRail load-bearing
// rather than incidental.
TEST_CASE("fitToRail invariants hold across the request/rail grid") {
    const float rails[]  = {1.0f, 4.999f, 5.0f, 5.001f, 10.0f, 100.0f, 500.0f, 2000.0f};
    const float points[] = {-1000.0f, -0.001f, 0.0f, 0.001f, 2.5f, 4.9f, 5.0f,
                            9.9f, 50.0f, 499.0f, 500.0f, 501.0f, 1e6f};

    for (float rail : rails) {
        for (float a : points) {
            for (float b : points) {
                CAPTURE(rail);
                CAPTURE(a);
                CAPTURE(b);
                RangeMapper m;
                m.setMaxRailMm(rail);
                m.setRange(a, b);
                checkWindow(m.goalWindow(), rail);
                // And the effective window after a settle is equally legal.
                m.tick(1.0f, 1e6f);
                checkWindow(m.effectiveWindow(), rail);
            }
        }
    }
}
