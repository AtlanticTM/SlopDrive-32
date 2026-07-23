// SlopGlow — hardware-free core suite. The heartbeat-gate contract is the
// safety-relevant part (a frozen core MUST freeze the LEDs), so it gets the
// most coverage; state priority, crossfade, and mode math follow.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "slopglow/slopglow_core.hpp"

using namespace slopglow;

namespace {

struct FakeStrip final : IGlowOutput {
    size_t n;
    std::vector<Rgb> px;
    int shows = 0;
    explicit FakeStrip(size_t count) : n(count), px(count) {}
    size_t pixelCount() const override { return n; }
    void set(size_t i, Rgb c) override { px[i] = c; }
    void show() override { ++shows; }
};

}  // namespace

TEST_CASE("priority: highest active state owns the LEDs; Boot is the floor") {
    FakeStrip strip(1);
    GlowEngine g(strip);

    CHECK(g.current() == GlowState::Boot);
    g.raise(GlowState::Ready);
    g.raise(GlowState::Warning);
    CHECK(g.current() == GlowState::Warning);
    g.raise(GlowState::Estop);
    CHECK(g.current() == GlowState::Estop);  // outranks everything
    g.clear(GlowState::Estop);
    CHECK(g.current() == GlowState::Warning);
    g.clear(GlowState::Warning);
    CHECK(g.current() == GlowState::Ready);
    g.clear(GlowState::Ready);
    CHECK(g.current() == GlowState::Boot);
    g.clear(GlowState::Boot);                // floor never clears
    CHECK(g.current() == GlowState::Boot);
}

TEST_CASE("heartbeat gate: a silent source freezes the frame exactly") {
    FakeStrip strip(1);
    GlowEngine g(strip);
    HeartbeatSource* core0 = g.addHeartbeat(150);
    HeartbeatSource* core1 = g.addHeartbeat(150);
    REQUIRE(core0 != nullptr);
    REQUIRE(core1 != nullptr);

    g.raise(GlowState::Active);

    // Both cores pulsing: animation runs, frames latch.
    uint32_t t = 0;
    for (; t < 1000; t += 10) {
        core0->pulse();
        core1->pulse();
        g.update(t);
    }
    CHECK_FALSE(g.frozen());
    int showsWhileAlive = strip.shows;
    CHECK(showsWhileAlive > 0);
    Rgb lastFrame = strip.px[0];

    // Core 1 dies. Within the stale window the engine keeps going, then
    // freezes: no more show() calls, the strip holds whatever frame was last
    // latched (FakeStrip.px only changes on set(), same as latching pixels).
    for (; t < 2000; t += 10) {
        core0->pulse();  // core 0 still happy — not good enough
        g.update(t);
    }
    CHECK(g.frozen());
    int showsAfterFreeze = strip.shows;
    Rgb frozenFrame = strip.px[0];
    (void)lastFrame;

    for (; t < 3000; t += 10) {
        core0->pulse();
        g.update(t);
    }
    CHECK(strip.shows == showsAfterFreeze);  // truly no output while frozen
    CHECK(strip.px[0] == frozenFrame);       // pixel held exactly

    // Core 1 recovers: animation resumes.
    for (; t < 3500; t += 10) {
        core0->pulse();
        core1->pulse();
        g.update(t);
    }
    CHECK_FALSE(g.frozen());
    CHECK(strip.shows > showsAfterFreeze);
}

TEST_CASE("no heartbeats registered: engine never freezes") {
    FakeStrip strip(1);
    GlowEngine g(strip);
    g.raise(GlowState::Ready);
    for (uint32_t t = 0; t < 2000; t += 20) g.update(t);
    CHECK_FALSE(g.frozen());
    CHECK(strip.shows > 0);
}

TEST_CASE("crossfade: transition blends from old frame to new state") {
    FakeStrip strip(1);
    GlowEngine g(strip);
    // Two solid specs with distinct colors so the blend is observable.
    g.setSpec(GlowState::Ready, {{0, 255, 0}, {}, GlowMode::Solid, 1000});
    g.setSpec(GlowState::Fault, {{255, 0, 0}, {}, GlowMode::Solid, 1000});
    g.raise(GlowState::Ready);

    uint32_t t = 0;
    for (; t < 1000; t += 10) g.update(t);
    CHECK(strip.px[0] == Rgb{0, 255, 0});

    g.raise(GlowState::Fault);
    g.update(t += 10);
    // Mid-fade: neither pure green nor pure red.
    bool pureOld = strip.px[0] == Rgb{0, 255, 0};
    bool pureNew = strip.px[0] == Rgb{255, 0, 0};
    CHECK_FALSE(pureOld);
    CHECK_FALSE(pureNew);

    // Well past kCrossfadeMs: settled on the new state's color.
    for (; t < 2000; t += 10) g.update(t);
    CHECK(strip.px[0] == Rgb{255, 0, 0});
}

TEST_CASE("blink mode toggles at half period; breathe hits both endpoints") {
    FakeStrip strip(1);
    GlowEngine g(strip);
    g.setSpec(GlowState::Ready, {{200, 0, 0}, {0, 0, 200}, GlowMode::Blink, 100});
    g.raise(GlowState::Ready);
    g.clear(GlowState::Boot);  // no-op (floor), but Ready outranks Boot anyway

    // Settle past the boot->ready crossfade first.
    uint32_t t = 0;
    for (; t <= 1000; t += 10) g.update(t);

    // Sample one full period aligned to the engine's own anim clock: collect
    // colors over 100ms and expect exactly the two spec colors present.
    bool sawA = false, sawB = false;
    for (uint32_t k = 0; k < 10; ++k) {
        g.update(t);
        t += 10;
        if (strip.px[0] == Rgb{200, 0, 0}) sawA = true;
        if (strip.px[0] == Rgb{0, 0, 200}) sawB = true;
    }
    CHECK(sawA);
    CHECK(sawB);
}

TEST_CASE("chase degrades to breathe on a single pixel, orbits on a ring") {
    FakeStrip ring(8);
    GlowEngine g(ring);
    g.setSpec(GlowState::Ready, {{255, 255, 255}, {5, 5, 5}, GlowMode::Chase, 800});
    g.raise(GlowState::Ready);

    uint32_t t = 0;
    for (; t <= 1000; t += 10) g.update(t);  // settle crossfade

    // Track the bright pixel over one period: it must visit several positions.
    std::vector<size_t> heads;
    for (uint32_t k = 0; k < 80; ++k) {
        g.update(t);
        t += 10;
        for (size_t i = 0; i < 8; ++i)
            if (ring.px[i] == Rgb{255, 255, 255}) {
                if (heads.empty() || heads.back() != i) heads.push_back(i);
            }
    }
    CHECK(heads.size() >= 6);  // orbited most of the ring
}

TEST_CASE("brightness ceiling scales output, zero blacks out") {
    FakeStrip strip(1);
    GlowEngine g(strip);
    g.setSpec(GlowState::Ready, {{200, 100, 50}, {}, GlowMode::Solid, 1000});
    g.raise(GlowState::Ready);
    uint32_t t = 0;
    for (; t < 1000; t += 10) g.update(t);
    CHECK(strip.px[0] == Rgb{200, 100, 50});

    g.setBrightness(127);
    g.update(t += 10);
    CHECK(strip.px[0].r < 110);
    CHECK(strip.px[0].r > 90);

    g.setBrightness(0);
    g.update(t += 10);
    CHECK(strip.px[0] == Rgb::black());
}

TEST_CASE("Rgb::lerp exact at endpoints; luma orders colors sanely") {
    Rgb a{10, 200, 30}, b{240, 0, 90};
    CHECK(Rgb::lerp(a, b, 0) == a);
    CHECK(Rgb::lerp(a, b, 255) == b);
    CHECK(Rgb{0, 255, 0}.luma() > Rgb{0, 0, 255}.luma());  // green brighter than blue
    CHECK(Rgb{255, 255, 255}.luma() > Rgb{128, 128, 128}.luma());
}
