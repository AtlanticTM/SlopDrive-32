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

TEST_CASE("A segment is released on arrival, with its anchor intact") {
    PacingRing r;
    PacingEntry out;
    const uint64_t now = 1000 * kMs;

    r.push(seg(now + 120 * kMs, 0.8f, 60));
    REQUIRE(r.pop(out));
    CHECK(out.due_us == now + 120 * kMs);   // the anchor rides along untouched
    CHECK(r.size() == 0);
}

TEST_CASE("A whole client lookahead leaves in ONE drain") {
    PacingRing r;
    PacingEntry out;
    const uint64_t now = 1000 * kMs;

    // A client running 120 ms ahead with 60 ms segments. Both go out now: the
    // RP's schedule queue parks them in anchor order (sd-4k1.19), so holding
    // either back would only move planning INTO its own span.
    r.push(seg(now + 120 * kMs, 0.8f, 60));
    r.push(seg(now + 180 * kMs, 0.2f, 60));

    REQUIRE(r.pop(out));
    CHECK(out.target == doctest::Approx(0.8f));
    REQUIRE(r.pop(out));
    CHECK(out.target == doctest::Approx(0.2f));
    CHECK(out.due_us == now + 180 * kMs);
    CHECK_FALSE(r.pop(out));
}

TEST_CASE("Arrival order is release order, whatever the anchors say") {
    PacingRing r;
    PacingEntry out;
    const uint64_t now = 1000 * kMs;

    // Three points already past due (a client with no lookahead at all, or one
    // that fell behind): all three leave in one drain, oldest first.
    r.push(seg(now - 30 * kMs, 0.1f, 20));
    r.push(seg(now - 10 * kMs, 0.2f, 20));
    r.push(seg(now, 0.3f, 20));

    REQUIRE(r.pop(out));
    CHECK(out.target == doctest::Approx(0.1f));
    REQUIRE(r.pop(out));
    CHECK(out.target == doctest::Approx(0.2f));
    REQUIRE(r.pop(out));
    CHECK(out.target == doctest::Approx(0.3f));
    CHECK_FALSE(r.pop(out));
}

TEST_CASE("The lookahead pairs a released segment with the one behind it") {
    PacingRing r;
    PacingEntry out;
    const uint64_t now = 1000 * kMs;

    r.push(seg(now + 100 * kMs, 0.8f, 50));
    r.push(seg(now + 150 * kMs, 0.2f, 50));

    REQUIRE(r.pop(out));
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
    REQUIRE(r.pop(last));
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
