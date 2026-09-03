// MotionLink v2 vocabulary -- host suite for the S3 <-> RP2350 SPI link.
// Constraints:
// - Hardware-free: the header is the whole unit under test, no board, no SPI.
// - The clock cases drive a SIMULATED slave whose pump quantizes its stamp,
//   so a filter change that widens the bias fails here rather than on a rail.
// See: include/comms/MotionLinkProtocol.h, dev board sd-4k1.2.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "MotionLinkProtocol.h"

using namespace motionlink;

namespace {

using Frame = std::array<uint8_t, kFrameBytes>;

// Slave-clock model: slave_us = master_us + offset, stamped at the next 20 kHz
// pump boundary, which is the whole source of the raw sample's late bias.
struct SlaveSim {
    uint32_t offset = 0;
    uint32_t tick = 50;
    uint32_t xfer = 40;

    uint32_t stamp(uint32_t master_us) const {
        const uint32_t slave = master_us + offset;
        const uint32_t q = slave % tick;
        return q == 0 ? slave : slave + (tick - q);
    }
    void probe(ClockFilter& f, uint32_t t0) const {
        const uint32_t mid = t0 + xfer / 2;
        f.push(t0, t0 + xfer, stamp(mid));
    }
};

// Error of the estimate against the truth, signed, wrap-safe.
int32_t offsetError(const ClockFilter& f, uint32_t truth) {
    return int32_t(f.offsetUs() - truth);
}

LinkCommand sampleCommand() {
    LinkCommand c;
    c.kind = kCmdWaveform;
    c.axis = 0;
    c.flags = kCmdHasEndVel | kCmdHasAnchor | kCmdHasNextChord;
    c.curve_family = 1;
    c.target = 0.375f;
    c.duration_us = 167000;
    c.end_vel = -1.25f;
    c.next_chord = 0.75f;
    c.anchor_us = 0xFEED1234u;
    c.limit_set = kLimitInput;
    return c;
}

StatusV2 sampleStatus() {
    StatusV2 s;
    s.state = kStateRunning;
    s.flags = kFlagUnderran;
    s.seq_echo = 0x9A;
    s.event_seq = 7;
    s.pos = -1234.5f;
    s.vel = 98765.5f;
    s.residue = -300;
    s.mode = 1;
    s.plan_kind = 3;
    s.clock_t1 = 0x01020304u;
    s.config_fp = 0xBEEF;
    s.qdrops = 61191;   // the ar3 capture's one-second census
    s.emit_overrun = 200;
    s.late_ticks = 3;
    s.link_errs = 1;
    s.vel_clamped = 42;
    return s;
}

}  // namespace

TEST_CASE("command frame round trips and stamps a valid CRC") {
    Frame f{};
    const LinkCommand c = sampleCommand();
    encodeCommand(f, 0x5A, c);

    CHECK(f[0] == kOpCommand);
    CHECK(f[1] == 0x5A);
    CHECK(crcOk(f));

    const LinkCommand d = decodeCommand(f);
    CHECK(d.kind == c.kind);
    CHECK(d.axis == c.axis);
    CHECK(d.flags == c.flags);
    CHECK(d.curve_family == c.curve_family);
    CHECK(d.target == doctest::Approx(c.target));
    CHECK(d.duration_us == c.duration_us);
    CHECK(d.end_vel == doctest::Approx(c.end_vel));
    CHECK(d.next_chord == doctest::Approx(c.next_chord));
    CHECK(d.anchor_us == c.anchor_us);
    CHECK(d.limit_set == c.limit_set);
}

TEST_CASE("limit_set rides the command, both values round trip") {
    LinkCommand c = sampleCommand();
    Frame f{};

    c.limit_set = kLimitUser;
    encodeCommand(f, 1, c);
    CHECK(crcOk(f));
    CHECK(decodeCommand(f).limit_set == kLimitUser);

    c.limit_set = kLimitInput;
    encodeCommand(f, 2, c);
    CHECK(crcOk(f));
    CHECK(decodeCommand(f).limit_set == kLimitInput);

    // The default is the GENTLE set: a command that forgets to say cannot
    // silently plan at the machine-driven ceiling.
    CHECK(LinkCommand{}.limit_set == kLimitUser);
}

TEST_CASE("a command's limit_set selects its ceiling set") {
    ConfigImage cfg;
    cfg.setF(kCfgInputVmax, 5.0f);
    cfg.setF(kCfgInputAmax, 250.0f);
    cfg.setF(kCfgInputJmax, 10000.0f);
    cfg.setF(kCfgUserVmax, 0.25f);
    cfg.setF(kCfgUserAmax, 1.0f);

    const SelectedLimits in = selectedLimits(cfg, kLimitInput);
    CHECK(in.vmax == doctest::Approx(5.0f));
    CHECK(in.amax == doctest::Approx(250.0f));

    const SelectedLimits user = selectedLimits(cfg, kLimitUser);
    CHECK(user.vmax == doctest::Approx(0.25f));
    CHECK(user.amax == doctest::Approx(1.0f));
    // The UI has no user jerk, so a user-set plan takes the INPUT jerk: one
    // jerk fact with one home.
    CHECK(user.jmax == doctest::Approx(in.jmax));
}

TEST_CASE("the soft-start cap trims speed only, on whichever set is selected") {
    ConfigImage cfg;
    cfg.setF(kCfgInputVmax, 5.0f);
    cfg.setF(kCfgInputAmax, 250.0f);
    cfg.setF(kCfgInputJmax, 10000.0f);
    cfg.setF(kCfgUserVmax, 0.25f);
    cfg.setF(kCfgUserAmax, 1.0f);
    cfg.setF(kCfgSoftStartCap, 0.1f);

    for (uint8_t set : {uint8_t(kLimitUser), uint8_t(kLimitInput)}) {
        const SelectedLimits l = selectedLimits(cfg, set);
        CHECK(l.vmax == doctest::Approx(0.1f));
    }
    // Accel is untouched by the ramp: a soft accel cannot reach even the
    // already-soft speed cap, which renders as freeze-then-jump.
    CHECK(selectedLimits(cfg, kLimitInput).amax == doctest::Approx(250.0f));
    CHECK(selectedLimits(cfg, kLimitUser).amax == doctest::Approx(1.0f));

    // A cap above the set is not a floor.
    cfg.setF(kCfgSoftStartCap, 99.0f);
    CHECK(selectedLimits(cfg, kLimitUser).vmax == doctest::Approx(0.25f));
    // 0 disables the cap entirely.
    cfg.setF(kCfgSoftStartCap, 0.0f);
    CHECK(selectedLimits(cfg, kLimitInput).vmax == doctest::Approx(5.0f));
}

TEST_CASE("an unpushed set reads as zero so the slave can gate it") {
    ConfigImage cfg;
    cfg.setF(kCfgInputVmax, 5.0f);
    CHECK(selectedLimits(cfg, kLimitUser).vmax == doctest::Approx(0.0f));
    CHECK(selectedLimits(cfg, kLimitInput).vmax == doctest::Approx(5.0f));
    CHECK(selectedLimits(cfg, kLimitInput).jmax == doctest::Approx(0.0f));
}

TEST_CASE("the end-velocity sentinel is distinct from an explicit rest") {
    Frame sentinel{};
    Frame rest{};
    LinkCommand c = sampleCommand();
    c.end_vel = 0.0f;

    c.flags = 0;
    encodeCommand(sentinel, 1, c);
    c.flags = kCmdHasEndVel;
    encodeCommand(rest, 2, c);

    CHECK((decodeCommand(sentinel).flags & kCmdHasEndVel) == 0);
    CHECK((decodeCommand(rest).flags & kCmdHasEndVel) != 0);
    CHECK(decodeCommand(sentinel).end_vel == doctest::Approx(0.0f));
    CHECK(decodeCommand(rest).end_vel == doctest::Approx(0.0f));
}

TEST_CASE("config frame round trips a full five-field push") {
    const std::array<ConfigField, 5> src{{
        {kCfgInputVmax, f32Bits(5.0f)},
        {kCfgInputAmax, f32Bits(250.0f)},
        {kCfgInputJmax, f32Bits(10000.0f)},
        {kCfgSettleGraceUs, 30000u},
        {kCfgGates, kGateHomed | kGateSoftStart},
    }};
    Frame f{};
    encodeConfig(f, 3, std::span<const ConfigField>(src));

    CHECK(f[0] == kOpConfig);
    CHECK(crcOk(f));

    std::array<ConfigField, 8> out{};
    const size_t n = decodeConfig(f, std::span<ConfigField>(out));
    REQUIRE(n == src.size());
    for (size_t k = 0; k < n; ++k) {
        CHECK(out[k].tag == src[k].tag);
        CHECK(out[k].raw == src[k].raw);
    }
}

TEST_CASE("config encode clamps to the per-frame field budget") {
    const std::array<ConfigField, 7> src{{
        {0x01, 1}, {0x02, 2}, {0x03, 3}, {0x04, 4},
        {0x05, 5}, {0x06, 6}, {0x07, 7},
    }};
    Frame f{};
    encodeConfig(f, 4, std::span<const ConfigField>(src));
    CHECK(f[2] == kConfigFieldsPerFrame);

    std::array<ConfigField, 8> out{};
    CHECK(decodeConfig(f, std::span<ConfigField>(out)) == kConfigFieldsPerFrame);
}

TEST_CASE("the config image is order independent and idempotent") {
    ConfigImage a;
    ConfigImage b;

    a.setF(kCfgInputVmax, 5.0f);
    a.set(kCfgGates, kGateHomed);
    a.setF(kCfgWindowMaxCounts, 40000.0f);
    a.setF(kCfgInputAmax, 250.0f);

    b.setF(kCfgWindowMaxCounts, 40000.0f);
    b.setF(kCfgInputAmax, 250.0f);
    b.setF(kCfgInputVmax, 5.0f);
    b.set(kCfgGates, kGateHomed);

    CHECK(a.size() == b.size());
    CHECK(a.fingerprint() == b.fingerprint());

    // A re-send of the whole set changes nothing: that is what makes a config
    // push safe to repeat whenever the fingerprint disagrees.
    b.setF(kCfgInputVmax, 5.0f);
    b.set(kCfgGates, kGateHomed);
    CHECK(b.size() == a.size());
    CHECK(b.fingerprint() == a.fingerprint());
}

TEST_CASE("the config fingerprint is zero when empty and moves with any field") {
    ConfigImage img;
    CHECK(img.fingerprint() == 0);

    img.setF(kCfgInputVmax, 5.0f);
    const uint16_t one = img.fingerprint();
    CHECK(one != 0);

    img.setF(kCfgInputAmax, 250.0f);
    const uint16_t two = img.fingerprint();
    CHECK(two != one);

    // A one-bit change in a value is a different image.
    img.setF(kCfgInputAmax, 250.0001f);
    CHECK(img.fingerprint() != two);

    img.setF(kCfgInputAmax, 250.0f);
    CHECK(img.fingerprint() == two);
}

TEST_CASE("an unknown tag still counts toward the fingerprint") {
    ConfigImage master;
    ConfigImage slave;
    master.setF(kCfgInputVmax, 5.0f);
    master.set(0xFE, 0xDEADBEEFu);   // a tag this slave build cannot apply
    slave.setF(kCfgInputVmax, 5.0f);
    slave.set(0xFE, 0xDEADBEEFu);
    CHECK(master.fingerprint() == slave.fingerprint());

    ConfigImage dropped;
    dropped.setF(kCfgInputVmax, 5.0f);
    CHECK(dropped.fingerprint() != master.fingerprint());
}

TEST_CASE("the config image reports a full table instead of silently dropping") {
    ConfigImage img;
    for (size_t k = 0; k < kConfigImageSlots; ++k)
        CHECK(img.set(uint8_t(k + 1), uint32_t(k)));
    CHECK(img.size() == kConfigImageSlots);
    CHECK_FALSE(img.set(0xFF, 1));
    // An existing tag is still writable when the table is full.
    CHECK(img.set(1, 99));
    CHECK(img.get(1) == 99u);
}

TEST_CASE("event pull and event record round trip") {
    Frame pull{};
    encodeEventPull(pull, 11, 0x2A);
    CHECK(pull[0] == kOpEventPull);
    CHECK(pull[1] == 11);
    CHECK(pull[2] == 0x2A);
    CHECK(crcOk(pull));

    EventRecord e;
    e.state = kStateRunning;
    e.kind = 2;            // slopmotion SettleEngaged, passed through
    e.seq = 0x2B;
    e.remaining = 4;
    e.axis = 0;
    e.cmd_seq = 0x5A;
    e.t_us = 0x7FFFFFFFu;
    e.target = 0.25f;
    e.detail = -3.5f;

    Frame f{};
    encodeEvent(f, e);
    CHECK(crcOk(f));
    CHECK(f[kStatusOffVariant] == kStatusEvent);

    const EventRecord d = decodeEvent(f);
    CHECK(d.state == e.state);
    CHECK(d.kind == e.kind);
    CHECK(d.seq == e.seq);
    CHECK(d.remaining == e.remaining);
    CHECK(d.axis == e.axis);
    CHECK(d.cmd_seq == e.cmd_seq);
    CHECK(d.t_us == e.t_us);
    CHECK(d.target == doctest::Approx(e.target));
    CHECK(d.detail == doctest::Approx(e.detail));
}

TEST_CASE("link events never collide with the slopmotion anomaly ordinals") {
    CHECK(kEvtCommandGated >= kEvtLinkBase);
    CHECK(kEvtConfigTagUnknown >= kEvtLinkBase);
    CHECK(kEvtClockStep >= kEvtLinkBase);
    // The anomaly table is append-only from 0 and nowhere near this base.
    CHECK(kEvtLinkBase == 0x80);
}

TEST_CASE("status v2 round trips and is self-describing") {
    const StatusV2 s = sampleStatus();
    Frame f{};
    encodeStatusV2(f, s);

    CHECK(crcOk(f));
    CHECK(f[kStatusOffVariant] == kStatusV2);

    const StatusV2 d = decodeStatusV2(f);
    CHECK(d.state == s.state);
    CHECK(d.flags == s.flags);
    CHECK(d.seq_echo == s.seq_echo);
    CHECK(d.event_seq == s.event_seq);
    CHECK(d.pos == doctest::Approx(s.pos));
    CHECK(d.vel == doctest::Approx(s.vel));
    CHECK(d.residue == s.residue);
    CHECK(d.mode == s.mode);
    CHECK(d.plan_kind == s.plan_kind);
    CHECK(d.clock_t1 == s.clock_t1);
    CHECK(d.config_fp == s.config_fp);
    CHECK(d.qdrops == s.qdrops);
    CHECK(d.emit_overrun == s.emit_overrun);
    CHECK(d.late_ticks == s.late_ticks);
    CHECK(d.link_errs == s.link_errs);
    CHECK(d.vel_clamped == s.vel_clamped);
}

TEST_CASE("a reply that never writes the variant byte declares itself pre-v2") {
    // A slave older than this vocabulary fills its own telemetry and leaves
    // byte 28 alone, so it declares itself for free.
    Frame f{};
    f[0] = kStateRunning;
    for (size_t at = 1; at < kStatusOffVariant; ++at) f[at] = 0xAB;
    crcStamp(f);

    CHECK(crcOk(f));
    CHECK(f[kStatusOffVariant] == kStatusPreV2);
}

TEST_CASE("residue saturates instead of wrapping") {
    CHECK(satResidue(0.0f) == 0);
    CHECK(satResidue(64.0f) == 64);
    CHECK(satResidue(-64.0f) == -64);
    CHECK(satResidue(1e9f) == 32767);
    CHECK(satResidue(-1e9f) == -32768);
}

TEST_CASE("a flipped payload byte fails the CRC on every v2 op") {
    Frame cmd{};
    encodeCommand(cmd, 1, sampleCommand());
    Frame cfg{};
    const std::array<ConfigField, 1> one{{{kCfgInputVmax, f32Bits(5.0f)}}};
    encodeConfig(cfg, 2, std::span<const ConfigField>(one));
    Frame pull{};
    encodeEventPull(pull, 3, 0);
    Frame st{};
    encodeStatusV2(st, sampleStatus());
    Frame ev{};
    encodeEvent(ev, EventRecord{});

    for (Frame* f : {&cmd, &cfg, &pull, &st, &ev}) {
        REQUIRE(crcOk(*f));
        (*f)[7] = uint8_t((*f)[7] ^ 0x01);
        CHECK_FALSE(crcOk(*f));
    }
}

TEST_CASE("the anchor domain is wrap-exact across the 32-bit boundary") {
    // Due when the delta is at or below zero, unambiguous inside +/-35.8 min.
    CHECK(clockDelta(0xFFFFFFF0u, 0xFFFFFFF0u) == 0);
    CHECK(clockDelta(0x00000010u, 0xFFFFFFF0u) == 32);    // anchor is ahead
    CHECK(clockDelta(0xFFFFFFF0u, 0x00000010u) == -32);   // anchor is due
    CHECK(clockDelta(0x7FFFFFFFu, 0u) == 2147483647);

    ClockFilter f;
    CHECK(f.toSlave(0xFFFFFFF0u) == 0xFFFFFFF0u);   // zero offset before a push
}

TEST_CASE("the clock filter converges under pump quantization") {
    const SlaveSim sim{123456789u, 50, 40};
    ClockFilter f;
    CHECK_FALSE(f.converged());

    // 100 Hz poll dithered off the pump period: an exact multiple aliases the
    // quantization to a constant, which the next case pins.
    uint32_t t = 1000000u;
    for (int k = 0; k < 64; ++k) {
        sim.probe(f, t);
        t += 10000u + uint32_t(k % 7) * 3u;
    }
    CHECK(f.converged());
    CHECK(f.samples() == kClockWindow);
    CHECK(f.rttUs() == 40u);

    // The raw sample is late by 0..50 us; the windowed minimum must land well
    // inside one 50 us slave tick, which is the whole error budget.
    const int32_t err = offsetError(f, sim.offset);
    CHECK(err >= 0);
    CHECK(err < 15);
}

TEST_CASE("a probe cadence aliased to the pump pins the bias") {
    // Documents the ONE requirement the filter cannot meet on its own: with a
    // cadence that is an exact multiple of the pump period, every sample
    // carries the SAME quantization and the minimum has nothing to select.
    const SlaveSim sim{1000u, 50, 40};
    ClockFilter f;
    uint32_t t = 1000021u;   // deliberately off a tick boundary
    for (int k = 0; k < 64; ++k) {
        sim.probe(f, t);
        t += 10000u;   // an exact multiple of the 50 us pump
    }
    CHECK(f.converged());
    const int32_t err = offsetError(f, sim.offset);
    CHECK(err > 0);
    CHECK(err < 50);   // still bounded by one tick, never worse
}

TEST_CASE("the clock filter tracks crystal drift") {
    // 100 ppm, the worst pair of ordinary crystals, over five seconds.
    ClockFilter f;
    uint32_t t = 500000u;
    const uint32_t base = 7777777u;
    int32_t worst = 0;
    for (int k = 0; k < 500; ++k) {
        SlaveSim sim{base + uint32_t(int64_t(t - 500000) / 10000), 50, 40};
        sim.probe(f, t);
        if (f.converged()) {
            const int32_t e = offsetError(f, sim.offset);
            if (e > worst) worst = e;
            if (-e > worst) worst = -e;
        }
        t += 10000u + uint32_t(k % 5) * 7u;
    }
    CHECK(f.converged());
    // Window lag plus quantization, both far inside the 50 us tick budget.
    CHECK(worst < 40);
}

TEST_CASE("the clock filter re-converges after a time base step") {
    SlaveSim sim{50000u, 50, 40};
    ClockFilter f;
    uint32_t t = 200000u;
    for (int k = 0; k < 40; ++k) {
        sim.probe(f, t);
        t += 10000u + uint32_t(k % 7) * 3u;
    }
    REQUIRE(f.converged());
    REQUIRE(offsetError(f, sim.offset) < 15);

    // The slave rebooted: its time base restarts, which is a step, not drift.
    sim.offset = 900000u;
    sim.probe(f, t);
    t += 10000u;
    CHECK_FALSE(f.converged());   // the window was flushed, not averaged

    for (size_t k = 0; k < kClockWindow; ++k) {
        sim.probe(f, t);
        t += 10000u + uint32_t(k % 7) * 3u;
    }
    CHECK(f.converged());
    const int32_t err = offsetError(f, sim.offset);
    CHECK(err >= 0);
    CHECK(err < 15);
}

TEST_CASE("the clock filter is exact across the 32-bit wrap") {
    // Master time straddles the wrap; the slave's own base is far from it.
    const SlaveSim sim{0x40000000u, 50, 40};
    ClockFilter f;
    uint32_t t = 0xFFFFF000u;
    for (int k = 0; k < 40; ++k) {
        sim.probe(f, t);
        t += 10000u + uint32_t(k % 7) * 3u;
    }
    REQUIRE(f.converged());
    const int32_t err = offsetError(f, sim.offset);
    CHECK(err >= 0);
    CHECK(err < 15);

    // The converted anchor must land in the slave's domain, wrap included.
    const uint32_t anchor_master = 0xFFFFFF00u;
    const uint32_t anchor_slave = f.toSlave(anchor_master);
    CHECK(int32_t(anchor_slave - (anchor_master + sim.offset)) < 15);
    CHECK(int32_t(anchor_slave - (anchor_master + sim.offset)) >= 0);
}

TEST_CASE("the gate bits name every reason a command is dropped, distinctly") {
    // One u32 field carries the whole set, so every bit must be its own power
    // of two and the union must round trip through kEvtCommandGated's f32
    // detail (all of these are exactly representable).
    const std::array<uint32_t, 5> bits{{kGateHomed, kGatePaused, kGateSoftStart,
                                        kGateAxis, kGateUnconfigured}};
    uint32_t all = 0;
    for (uint32_t b : bits) {
        CHECK(b != 0);
        CHECK((b & (b - 1)) == 0);   // one bit each
        CHECK((all & b) == 0);       // and no two share one
        all |= b;
    }
    CHECK(uint32_t(float(all)) == all);

    // The two reported-only bits are what a bad axis and an unpushed ceiling
    // set look like on the wire, and neither can be mistaken for a gate the
    // master pushed.
    CHECK((kGateAxis & (kGateHomed | kGatePaused)) == 0);
    CHECK((kGateUnconfigured & (kGateHomed | kGatePaused)) == 0);

    EventRecord e;
    e.kind = kEvtCommandGated;
    e.detail = float(kGateAxis | kGateUnconfigured);
    Frame f{};
    encodeEvent(f, e);
    CHECK(crcOk(f));
    CHECK(uint32_t(decodeEvent(f).detail) == (kGateAxis | kGateUnconfigured));
}

TEST_CASE("every v2 payload ends before the CRC field") {
    CHECK(kCommandWireEnd <= kCrcOffset);
    CHECK(kConfigWireEnd <= kCrcOffset);
    CHECK(kEventWireEnd <= kCrcOffset);
    CHECK(kStatusV2WireEnd == kCrcOffset);
    CHECK(kStatusOffVariant < kCrcOffset);
    // Ops are append-only and the v2 block sits above every v1 number.
    CHECK(kOpCommand > kOpSetLimits);
    CHECK(kOpConfig > kOpCommand);
    CHECK(kOpEventPull > kOpConfig);
    CHECK(kCommandWireEnd == 27);   // limit_set is the last command byte
}
