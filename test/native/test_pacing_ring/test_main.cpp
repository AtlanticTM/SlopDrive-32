// test_pacing_ring -- native doctest suite for the hub's Core-0 hand-off ring.
// Constraints:
// - Exercises the SHIPPING header, not a model of it: include/comms/PacingRing.h
//   is hardware-free and FreeRTOS-free precisely so this suite can run it.
// - Times are plain microseconds; nothing here reads a clock.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "PacingRing.h"

using slopdrive::PacingEntry;
using slopdrive::PacingRing;

namespace {
constexpr uint64_t kMs = 1000;

PacingEntry seg(uint64_t due_us, float target, uint32_t dur_ms) {
    PacingEntry e;
    e.due_us = due_us;
    e.target = target;
    e.duration_us = dur_ms * uint32_t(kMs);
    e.has_duration = true;
    e.has_end_vel = true;
    return e;
}
}  // namespace

TEST_CASE("A segment is released before its due time, not at it") {
    PacingRing r;
    PacingEntry out;
    const uint64_t now = 1000 * kMs;

    r.push(seg(now + 120 * kMs, 0.8f, 60));
    REQUIRE(r.popReleased(now, out));
    CHECK(out.due_us == now + 120 * kMs);   // the anchor rides along untouched
    CHECK(r.size() == 0);
}

TEST_CASE("Release depth is one: the successor waits for the anchor, not for its own due") {
    PacingRing r;
    PacingEntry out;
    uint64_t now = 1000 * kMs;

    // A client running 120 ms ahead with 60 ms segments: both are in the ring
    // long before either starts.
    r.push(seg(now + 120 * kMs, 0.8f, 60));
    r.push(seg(now + 180 * kMs, 0.2f, 60));

    REQUIRE(r.popReleased(now, out));
    CHECK(out.target == doctest::Approx(0.8f));

    // The second one may NOT go out while the first is still parked on the
    // engine's one scheduled slot: releasing it there evicts a plan that never
    // ran (PacingRing.h, release depth).
    CHECK_FALSE(r.popReleased(now + 60 * kMs, out));
    CHECK_FALSE(r.popReleased(now + 119 * kMs, out));

    // It goes out the moment the first segment's anchor passes -- 60 ms before
    // its own start, which is the whole point.
    now += 120 * kMs;
    REQUIRE(r.popReleased(now, out));
    CHECK(out.target == doctest::Approx(0.2f));
    CHECK(out.due_us == 1000 * kMs + 180 * kMs);
}

TEST_CASE("Arrival order is release order, and a late anchor never holds anything back") {
    PacingRing r;
    PacingEntry out;
    const uint64_t now = 1000 * kMs;

    // Three points already past due (a client with no lookahead at all, or one
    // that fell behind): all three leave in one drain, oldest first.
    r.push(seg(now - 30 * kMs, 0.1f, 20));
    r.push(seg(now - 10 * kMs, 0.2f, 20));
    r.push(seg(now, 0.3f, 20));

    REQUIRE(r.popReleased(now, out));
    CHECK(out.target == doctest::Approx(0.1f));
    REQUIRE(r.popReleased(now, out));
    CHECK(out.target == doctest::Approx(0.2f));
    REQUIRE(r.popReleased(now, out));
    CHECK(out.target == doctest::Approx(0.3f));
    CHECK_FALSE(r.popReleased(now, out));
}

TEST_CASE("The lookahead pairs a released segment with the one behind it") {
    PacingRing r;
    PacingEntry out;
    const uint64_t now = 1000 * kMs;

    r.push(seg(now + 100 * kMs, 0.8f, 50));
    r.push(seg(now + 150 * kMs, 0.2f, 50));

    REQUIRE(r.popReleased(now, out));
    const PacingEntry* next = r.peekOldest();
    REQUIRE(next != nullptr);
    // RFC-008: the chord the engine bounds the released segment's end velocity
    // against is the SUCCESSOR's, computed from what is still in the ring.
    const float chord = (next->target - out.target) / (float(next->duration_us) * 1e-6f);
    CHECK(chord == doctest::Approx(-12.0f));
    CHECK(r.size() == 1);   // peek consumes nothing

    // At the tail there is no successor and the engine plans exactly as it did
    // before the guard existed.
    PacingEntry last;
    REQUIRE(r.popReleased(now + 100 * kMs, last));
    CHECK(r.peekOldest() == nullptr);
}

TEST_CASE("A full ring drops the oldest and says so") {
    PacingRing r;
    for (size_t i = 0; i < PacingRing::kCapacity; ++i)
        CHECK_FALSE(r.push(seg(1000 * kMs + i, float(i) / 100.0f, 10)));
    CHECK(r.size() == PacingRing::kCapacity);
    CHECK(r.push(seg(2000 * kMs, 0.5f, 10)));
    CHECK(r.size() == PacingRing::kCapacity);
}
