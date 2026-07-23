// ============================================================================
// test_main.cpp — doctest unit tests for slopsync-core's M3 pieces:
// wire/fragmentation.hpp (Fragmenter + Reassembler, SPEC §5.6) and
// transport/inprocess_binding.hpp (the in-process fault-injection binding,
// SPEC §13.1, §13.6).
//
// Native (host-side, hardware-free) test, same pattern as
// test/native/test_slopsync_wire/test_main.cpp: doctest's bundled main(), no
// Arduino, no bus/FreeRTOS dependency. SPEC section numbers cite
// docs/slopsync/SPEC.md.
//
// Suite ids: T-xx = transport/binding mechanics, F-xx = fragmentation unit
// tests. T-10 is the M3 verification gate (determinism proof); T-12 is the
// M3 "fragmentation end-to-end over a lossy simulated link" requirement.
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/core/clock.hpp"
#include "slopsync/core/result.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/transport/inprocess_binding.hpp"
#include "slopsync/util/byte_io.hpp"
#include "slopsync/wire/estop_frame.hpp"
#include "slopsync/wire/fragmentation.hpp"
#include "slopsync/wire/frame_buffer.hpp"
#include "slopsync/wire/frame_header.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace slopsync;

namespace {

// Builds a whole control frame (header + payload) with a deterministic,
// position-dependent (and seed-dependent) byte pattern in the payload —
// distinct fill seeds let byte-identity checks catch cross-contamination
// between messages in the same test.
std::vector<std::byte> makeFrame(FrameType type, uint16_t channel, uint16_t seq, size_t payloadSize,
                                  uint8_t fillSeed = 0) {
    std::vector<std::byte> buf(kHeaderBytes + payloadSize);
    FrameHeader h;
    h.type = uint8_t(type);
    h.flags = 0;
    h.channel = channel;
    h.seq = seq;
    h.len = uint16_t(payloadSize);
    encodeFrameHeader(h, std::span<std::byte>(buf.data(), kHeaderBytes));
    for (size_t i = 0; i < payloadSize; ++i) {
        buf[kHeaderBytes + i] = std::byte(uint8_t((i * 31 + fillSeed) & 0xFF));
    }
    return buf;
}

std::vector<std::byte> makeEstop(uint8_t cause, uint8_t origin, uint16_t seq) {
    std::vector<std::byte> buf(kEstopFrameBytes);
    EstopFrame ef;
    ef.cause = cause;
    ef.origin = origin;
    ef.seq = seq;
    encodeEstop(ef, std::span<std::byte>(buf.data(), kEstopFrameBytes));
    return buf;
}

// One emitted fragment, decoded back into header + payload-after-header, for
// convenient inspection/feeding into Reassembler::accept().
struct Piece {
    FrameHeader header;
    std::vector<std::byte> payload;
};

std::vector<Piece> fragmentToPieces(std::span<const std::byte> whole, uint16_t mtu) {
    std::vector<Piece> pieces;
    fragmentFrame(whole, mtu, [&](std::span<const std::byte> frag) {
        auto h = decodeFrameHeader(frag);
        REQUIRE(h.has_value());
        Piece p;
        p.header = *h;
        p.payload.assign(frag.begin() + kHeaderBytes, frag.end());
        pieces.push_back(std::move(p));
    });
    return pieces;
}

bool bytesEqual(std::span<const std::byte> a, std::span<const std::byte> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

}  // namespace

// ============================================================================
// T-01..T-02 — clean link mechanics
// ============================================================================
TEST_CASE("T-01: clean link round-trips A->B and B->A byte-identical; read-before-write is nullopt") {
    ManualClock clock;
    XorShift32 rng(12345);
    InProcessLink link(clock, rng);
    auto& a = link.endpointA();
    auto& b = link.endpointB();
    REQUIRE(a.open());
    REQUIRE(b.open());

    CHECK(!a.read().has_value());
    CHECK(!b.read().has_value());

    auto frame = makeFrame(FrameType::INTENT, 0x0005, 1, 40, 0x10);
    REQUIRE(a.write(frame));
    auto gotB = b.read();
    REQUIRE(gotB.has_value());
    CHECK(bytesEqual(gotB->bytes(), frame));

    auto reply = makeFrame(FrameType::ECHO, 0x0005, 1, 24, 0x20);
    REQUIRE(b.write(reply));
    auto gotA = a.read();
    REQUIRE(gotA.has_value());
    CHECK(bytesEqual(gotA->bytes(), reply));
}

TEST_CASE("T-02: ring-full (capacity 16) makes write() return false") {
    ManualClock clock;
    XorShift32 rng(1);
    InProcessLink link(clock, rng);
    auto& a = link.endpointA();
    a.open();

    auto frame = makeFrame(FrameType::EVENT, 0, 1, 8, 0x30);
    for (int i = 0; i < 16; ++i) {
        REQUIRE(a.write(frame));
    }
    CHECK(!a.write(frame));
}

// ============================================================================
// T-03 — MTU admission gate, and Fragmenter + link + Reassembler together
// ============================================================================
TEST_CASE("T-03: oversized frame rejected by MTU; fragmented pieces fit and reassemble byte-identical") {
    ManualClock clock;
    XorShift32 rng(7);
    InProcessLink link(clock, rng);
    auto& a = link.endpointA();
    auto& b = link.endpointB();
    a.open();
    b.open();
    REQUIRE(link.profileA().mtu == 250);

    auto whole = makeFrame(FrameType::INTENT, 0x0005, 2, 292, 0x40);  // whole = 300 bytes
    REQUIRE(whole.size() == 300);
    CHECK(!a.write(whole));  // 300 > mtu 250

    std::vector<std::vector<std::byte>> fragments;
    bool ok = fragmentFrame(std::span<const std::byte>(whole), uint16_t(250), [&](std::span<const std::byte> frag) {
        fragments.emplace_back(frag.begin(), frag.end());
    });
    REQUIRE(ok);
    for (auto& f : fragments) {
        CHECK(f.size() <= 250);
        REQUIRE(a.write(f));
    }

    Reassembler reassembler;
    std::optional<FrameBuffer> complete;
    for (size_t i = 0; i < fragments.size(); ++i) {
        auto got = b.read();
        REQUIRE(got.has_value());
        auto hdr = got->header();
        REQUIRE(hdr.has_value());
        auto res = reassembler.accept(*hdr, got->payload(), clock.nowMs());
        REQUIRE(res.isOk());
        if (res.value().has_value()) complete = res.value();
    }
    REQUIRE(complete.has_value());
    CHECK(bytesEqual(complete->bytes(), whole));
}

// ============================================================================
// F-01..F-06 — Fragmenter/Reassembler unit tests
// ============================================================================
TEST_CASE("F-01: 600-byte control frame at mtu 250 => 3 fragments with derived flag pattern") {
    // Hand derivation (SPEC §5.6):
    //   budget = maxFrameBytes - header(8) - frag_index(2) = 250-8-2 = 240
    //   whole frame = 600B  =>  payload = 600-8 = 592B
    //   numFragments = ceil(592/240) = ceil(2.4667) = 3
    //   frag0: slice [0,240)   len=240  flags = START|MORE  (i=0, i+1<3)
    //   frag1: slice [240,480) len=240  flags = MORE         (i=1, i+1<3)
    //   frag2: slice [480,592) len=112  flags = none         (i=2, i+1==3)
    auto whole = makeFrame(FrameType::INTENT, 0x0005, 99, 592, 0x11);
    REQUIRE(whole.size() == 600);
    auto pieces = fragmentToPieces(whole, 250);
    REQUIRE(pieces.size() == 3);

    CHECK(pieces[0].payload.size() == kFragIndexBytes + 240);
    CHECK(pieces[1].payload.size() == kFragIndexBytes + 240);
    CHECK(pieces[2].payload.size() == kFragIndexBytes + 112);

    CHECK(pieces[0].header.flags == (flags::FRAG_START | flags::FRAG_MORE));
    CHECK(pieces[1].header.flags == flags::FRAG_MORE);
    CHECK(pieces[2].header.flags == 0);

    for (auto& p : pieces) {
        CHECK(p.header.type == uint8_t(FrameType::INTENT));
        CHECK(p.header.channel == 0x0005);
        CHECK(p.header.seq == 99);
    }

    CHECK(getU16(std::span<const std::byte>(pieces[0].payload).subspan(0, 2)) == 0);
    CHECK(getU16(std::span<const std::byte>(pieces[1].payload).subspan(0, 2)) == 1);
    CHECK(getU16(std::span<const std::byte>(pieces[2].payload).subspan(0, 2)) == 2);
}

TEST_CASE("F-02: a frame that already fits mtu passes through unchanged with no frag flags") {
    auto whole = makeFrame(FrameType::HELLO, 0, 5, 100, 0x22);  // whole = 108B, well under mtu 250
    std::vector<std::vector<std::byte>> emitted;
    bool ok = fragmentFrame(std::span<const std::byte>(whole), uint16_t(250),
                             [&](std::span<const std::byte> frag) { emitted.emplace_back(frag.begin(), frag.end()); });
    REQUIRE(ok);
    REQUIRE(emitted.size() == 1);
    CHECK(emitted[0] == whole);  // byte-identical, flags untouched (==0, no frag bits added)
}

TEST_CASE("F-03: out-of-order and duplicate fragment delivery reassembles byte-identically") {
    // 500-byte payload (not 592): budget=240 still yields 3 fragments
    // (ceil(500/240)=3) while keeping the reassembled frame's payload
    // (<=504) within kFrameBufferCapacity(512)-kHeaderBytes(8). F-01 uses
    // the brief's exact 592B number for pure flag-pattern derivation, which
    // never goes through Reassembler/FrameBuffer and so isn't capacity-bound.
    auto whole = makeFrame(FrameType::INTENT, 0x0005, 55, 500, 0x33);
    auto pieces = fragmentToPieces(whole, 250);
    REQUIRE(pieces.size() == 3);

    Reassembler r;
    // Deliver out of order: frag2, frag0, frag0 (duplicate), frag1.
    auto res = r.accept(pieces[2].header, pieces[2].payload, 0);
    REQUIRE(res.isOk());
    CHECK(!res.value().has_value());

    res = r.accept(pieces[0].header, pieces[0].payload, 1);
    REQUIRE(res.isOk());
    CHECK(!res.value().has_value());

    res = r.accept(pieces[0].header, pieces[0].payload, 2);  // duplicate: idempotent no-op
    REQUIRE(res.isOk());
    CHECK(!res.value().has_value());

    res = r.accept(pieces[1].header, pieces[1].payload, 3);
    REQUIRE(res.isOk());
    REQUIRE(res.value().has_value());
    CHECK(bytesEqual(res.value()->bytes(), whole));
    CHECK(r.stats().reassemblies_completed == 1);
}

TEST_CASE("F-04: 5s timeout expires a stale partial; the next fragment on that key starts fresh") {
    auto whole = makeFrame(FrameType::INTENT, 0x0006, 7, 500, 0xD4);  // see F-03 for the 500-vs-592 note
    auto pieces = fragmentToPieces(whole, 250);
    REQUIRE(pieces.size() == 3);

    Reassembler r;
    auto res = r.accept(pieces[0].header, pieces[0].payload, 0);
    REQUIRE(res.isOk());
    CHECK(!res.value().has_value());
    res = r.accept(pieces[1].header, pieces[1].payload, 100);
    REQUIRE(res.isOk());
    CHECK(!res.value().has_value());

    r.expireStale(5001);  // limits::frag_reassembly_timeout_ms == 5000
    CHECK(r.stats().timeouts == 1);

    // The 3rd (last) fragment now lands on a clean slate: it must start a
    // brand-new partial, NOT complete the timed-out one.
    res = r.accept(pieces[2].header, pieces[2].payload, 5002);
    REQUIRE(res.isOk());
    CHECK(!res.value().has_value());
    CHECK(r.stats().reassemblies_completed == 0);
}

TEST_CASE("F-05: 3rd concurrent partial evicts the OLDEST of the 2 in-flight ones") {
    auto wholeA = makeFrame(FrameType::INTENT, 0x0005, 1, 300, 0xA1);
    auto wholeB = makeFrame(FrameType::INTENT, 0x0005, 2, 300, 0xB2);
    auto wholeC = makeFrame(FrameType::INTENT, 0x0005, 3, 300, 0xC3);
    auto piecesA = fragmentToPieces(wholeA, 250);
    auto piecesB = fragmentToPieces(wholeB, 250);
    auto piecesC = fragmentToPieces(wholeC, 250);
    REQUIRE(piecesA.size() == 2);
    REQUIRE(piecesB.size() == 2);
    REQUIRE(piecesC.size() == 2);

    Reassembler r;  // limits::frag_max_concurrent_per_session == 2

    auto res = r.accept(piecesA[0].header, piecesA[0].payload, 0);  // A: slot 1
    REQUIRE(res.isOk());
    CHECK(!res.value().has_value());

    res = r.accept(piecesB[0].header, piecesB[0].payload, 10);  // B: slot 2 (both slots full)
    REQUIRE(res.isOk());
    CHECK(!res.value().has_value());

    res = r.accept(piecesC[0].header, piecesC[0].payload, 20);  // C: forces eviction of oldest (A, t=0)
    REQUIRE(res.isOk());
    CHECK(!res.value().has_value());
    CHECK(r.stats().evictions == 1);

    // A's remaining fragment now targets an evicted slot: it starts a fresh,
    // empty partial rather than completing the old one — and doing so must
    // itself evict the (now) oldest of {B, C}.
    res = r.accept(piecesA[1].header, piecesA[1].payload, 30);
    REQUIRE(res.isOk());
    CHECK(!res.value().has_value());
    CHECK(r.stats().evictions == 2);

    // C should have survived (it was newer than B at the moment of the 2nd
    // eviction) and can still be completed.
    res = r.accept(piecesC[1].header, piecesC[1].payload, 40);
    REQUIRE(res.isOk());
    REQUIRE(res.value().has_value());
    CHECK(bytesEqual(res.value()->bytes(), wholeC));
}

TEST_CASE("F-06: reassembled size exceeding capacity is reported via DecodeError, slot discarded") {
    // A "full" (FRAG_START|FRAG_MORE) fragment establishes unitLen=200 at
    // index 0.
    FrameHeader h;
    h.type = uint8_t(FrameType::INTENT);
    h.channel = 0x0005;
    h.seq = 1;
    h.flags = flags::FRAG_START | flags::FRAG_MORE;
    std::vector<std::byte> payload0(kFragIndexBytes + 200, std::byte{0});
    putU16(std::span<std::byte>(payload0).subspan(0, 2), uint16_t(0));
    h.len = uint16_t(payload0.size());

    Reassembler r;
    auto res = r.accept(h, payload0, 0);
    REQUIRE(res.isOk());
    CHECK(!res.value().has_value());

    // A middle fragment at a huge index: offset = index*unitLen blows past
    // the reassembly slot's capacity.
    FrameHeader h2 = h;
    h2.flags = flags::FRAG_MORE;
    std::vector<std::byte> payload1(kFragIndexBytes + 200, std::byte{0});
    putU16(std::span<std::byte>(payload1).subspan(0, 2), uint16_t(10));  // 10*200 = 2000 > capacity
    auto res2 = r.accept(h2, payload1, 0);
    CHECK(!res2.isOk());
    CHECK(res2.error() == DecodeError::CapacityExceeded);
}

// ============================================================================
// T-04..T-06 — latency, jitter, properties()
// ============================================================================
TEST_CASE("T-04: latency delays delivery until the clock catches up") {
    ManualClock clock;
    XorShift32 rng(2);
    InProcessLink link(clock, rng);
    link.profileA().latency_us = 10000;  // 10ms
    auto& a = link.endpointA();
    auto& b = link.endpointB();
    a.open();
    b.open();

    auto frame = makeFrame(FrameType::EVENT, 0, 1, 8, 0x50);
    REQUIRE(a.write(frame));
    CHECK(!b.read().has_value());
    clock.advanceUs(9999);
    CHECK(!b.read().has_value());
    clock.advanceUs(1);  // now == deliver_at exactly
    auto got = b.read();
    REQUIRE(got.has_value());
    CHECK(bytesEqual(got->bytes(), frame));
}

TEST_CASE("T-05: jitter keeps delivery within [latency-jitter, latency+jitter]") {
    ManualClock clock;
    XorShift32 rng(99);
    InProcessLink link(clock, rng);
    link.profileA().latency_us = 1000;
    link.profileA().jitter_us = 200;
    auto& a = link.endpointA();
    auto& b = link.endpointB();
    a.open();
    b.open();

    auto frame = makeFrame(FrameType::EVENT, 0, 1, 8, 0x60);
    REQUIRE(a.write(frame));
    clock.setUs(1000 - 200);  // earliest theoretically possible instant
    // (may or may not be ready yet, depending on the roll — no assertion here)
    clock.setUs(1000 + 200);  // latest theoretically possible instant: MUST be ready
    auto got = b.read();
    REQUIRE(got.has_value());
    CHECK(bytesEqual(got->bytes(), frame));
}

TEST_CASE("T-06: properties() reflects mtu/ordered/reliable/congestion") {
    ManualClock clock;
    XorShift32 rng(3);
    InProcessLink link(clock, rng);
    auto& a = link.endpointA();

    auto p0 = a.properties();
    CHECK(p0.mtu == 250);
    CHECK(p0.ordered == true);
    CHECK(p0.reliable == true);
    CHECK(p0.congestion == CongestionSignal::Simulated);

    link.profileA().reorder_pct = 10;
    CHECK(a.properties().ordered == false);
    link.profileA().reorder_pct = 0;

    link.profileA().jitter_us = 5;
    CHECK(a.properties().ordered == false);
    link.profileA().jitter_us = 0;
    CHECK(a.properties().ordered == true);

    link.profileA().loss_pct = 5;
    CHECK(a.properties().reliable == false);
    link.profileA().loss_pct = 0;

    link.profileA().dup_pct = 5;
    CHECK(a.properties().reliable == false);
}

// ============================================================================
// T-07..T-09 — loss / duplication / reorder smoke, seeded & deterministic
// ============================================================================
TEST_CASE("T-07: seeded loss over 200 frames is deterministic and lands in (100,180)") {
    auto run = [](uint32_t seed) {
        ManualClock clock;
        XorShift32 rng(seed);
        InProcessLink link(clock, rng);
        link.profileA().loss_pct = 30;
        auto& a = link.endpointA();
        auto& b = link.endpointB();
        a.open();
        b.open();
        int delivered = 0;
        for (uint16_t i = 0; i < 200; ++i) {
            auto frame = makeFrame(FrameType::EVENT, 0, i, 8, uint8_t(i));
            a.write(frame);
            if (b.read().has_value()) ++delivered;  // drain immediately: ring never backs up
        }
        return delivered;
    };

    int d1 = run(42);
    CHECK(d1 > 100);
    CHECK(d1 < 180);
    int d2 = run(42);
    CHECK(d2 == d1);  // determinism: identical seed => identical outcome
}

TEST_CASE("T-08: dup_pct=100 produces byte-identical duplicate deliveries") {
    ManualClock clock;
    XorShift32 rng(5);
    InProcessLink link(clock, rng);
    link.profileA().dup_pct = 100;
    auto& a = link.endpointA();
    auto& b = link.endpointB();
    a.open();
    b.open();

    auto frame = makeFrame(FrameType::EVENT, 0, 1, 12, 0x70);
    REQUIRE(a.write(frame));
    auto g1 = b.read();
    auto g2 = b.read();
    REQUIRE(g1.has_value());
    REQUIRE(g2.has_value());
    CHECK(bytesEqual(g1->bytes(), frame));
    CHECK(bytesEqual(g2->bytes(), frame));
    CHECK(!b.read().has_value());  // no 3rd copy
}

TEST_CASE("T-09: reorder produces at least one inversion; the delivered sequence is deterministic") {
    auto run = [](uint32_t seed) {
        ManualClock clock;
        XorShift32 rng(seed);
        InProcessLink link(clock, rng);
        link.profileA().reorder_pct = 50;
        auto& a = link.endpointA();
        auto& b = link.endpointB();
        a.open();
        b.open();
        std::vector<uint16_t> delivered;
        for (uint16_t i = 0; i < 10; ++i) {
            auto frame = makeFrame(FrameType::EVENT, 0, i, 8, uint8_t(i));
            REQUIRE(a.write(frame));
        }
        while (true) {
            auto g = b.read();
            if (!g) break;
            delivered.push_back(g->header()->seq);
        }
        return delivered;
    };

    auto delivered1 = run(123);
    REQUIRE(delivered1.size() == 10);
    bool inversion = false;
    for (size_t i = 0; i + 1 < delivered1.size(); ++i) {
        if (delivered1[i] > delivered1[i + 1]) {
            inversion = true;
            break;
        }
    }
    CHECK(inversion);

    auto delivered2 = run(123);
    CHECK(delivered2 == delivered1);  // determinism, exact sequence
}

// ============================================================================
// T-10 — THE M3 VERIFICATION GATE: determinism proof
// ============================================================================
namespace {

// A busy scripted scenario mixing loss/dup/reorder/latency/jitter, driven by
// a fresh ManualClock + XorShift32(seed) each call. The "transcript" is the
// exact sequence of delivered frame byte-vectors — the strongest possible
// bit-identity claim (stronger than a count or a hash-of-counts).
std::vector<std::vector<std::byte>> runScenario(uint32_t seed) {
    ManualClock clock;
    XorShift32 rng(seed);
    InProcessLink link(clock, rng);
    link.profileA().loss_pct = 15;
    link.profileA().dup_pct = 10;
    link.profileA().reorder_pct = 20;
    link.profileA().latency_us = 500;
    link.profileA().jitter_us = 300;
    auto& a = link.endpointA();
    auto& b = link.endpointB();
    a.open();
    b.open();

    std::vector<std::vector<std::byte>> transcript;
    auto drain = [&] {
        while (true) {
            auto g = b.read();
            if (!g) break;
            transcript.emplace_back(g->bytes().begin(), g->bytes().end());
        }
    };

    for (uint16_t i = 0; i < 40; ++i) {
        auto frame = makeFrame(FrameType::EVENT, 0, i, 16, uint8_t(i));
        a.write(frame);
        clock.advanceUs(200);
        drain();
    }
    clock.advanceUs(5000);  // flush anything still in flight
    drain();
    return transcript;
}

}  // namespace

TEST_CASE("T-10: identical seed+script => bit-identical transcript; a different seed must differ") {
    auto t1 = runScenario(2026);
    auto t2 = runScenario(2026);
    REQUIRE(t1.size() == t2.size());
    for (size_t i = 0; i < t1.size(); ++i) {
        CHECK(t1[i] == t2[i]);
    }

    auto t3 = runScenario(31337);
    bool differs = (t3.size() != t1.size());
    for (size_t i = 0; !differs && i < t1.size() && i < t3.size(); ++i) {
        if (t1[i] != t3[i]) differs = true;
    }
    CHECK(differs);
}

// ============================================================================
// T-11 — ESTOP fast path
// ============================================================================
TEST_CASE("T-11: ESTOP jumps the queue ahead of everything already waiting") {
    ManualClock clock;
    XorShift32 rng(9);
    InProcessLink link(clock, rng);
    auto& a = link.endpointA();
    auto& b = link.endpointB();
    a.open();
    b.open();

    for (uint16_t i = 0; i < 3; ++i) {
        auto frame = makeFrame(FrameType::EVENT, 0, i, 8, uint8_t(i));
        REQUIRE(a.write(frame));
    }
    auto estop = makeEstop(uint8_t(EstopCause::user), uint8_t(AccessLevel::controller), 1);
    REQUIRE(a.write(estop));

    auto got = b.read();
    REQUIRE(got.has_value());
    CHECK(got->size() == kEstopFrameBytes);
    CHECK(bytesEqual(got->bytes(), estop));
}

// ============================================================================
// T-12 — fragmentation end-to-end over a lossy simulated link (M3 requirement)
// ============================================================================
TEST_CASE("T-12: a large control frame reassembles byte-identical over a 20%-loss link, retried <=10x") {
    ManualClock clock;
    XorShift32 rng(777);
    InProcessLink link(clock, rng);
    link.profileA().mtu = 250;
    link.profileA().loss_pct = 20;
    auto& a = link.endpointA();
    auto& b = link.endpointB();
    a.open();
    b.open();

    // 500B payload (see F-03's note): 3 fragments at mtu 250, and the
    // reassembled frame's payload (500) still fits kFrameBufferCapacity(512)
    // - kHeaderBytes(8) = 504.
    auto whole = makeFrame(FrameType::INTENT, 0x0005, 42, 500, 0xAB);

    Reassembler reassembler;
    std::optional<FrameBuffer> complete;

    for (int attempt = 0; attempt < 10 && !complete; ++attempt) {
        fragmentFrame(std::span<const std::byte>(whole), uint16_t(250),
                       [&](std::span<const std::byte> frag) { a.write(frag); });

        uint32_t nowMs = clock.nowMs();
        while (true) {
            auto got = b.read();
            if (!got) break;
            auto hdr = got->header();
            REQUIRE(hdr.has_value());
            auto res = reassembler.accept(*hdr, got->payload(), nowMs);
            REQUIRE(res.isOk());
            if (res.value().has_value()) {
                complete = res.value();
                break;
            }
        }
        if (complete) break;

        clock.advanceUs(100000);  // 100ms per attempt, well under the 5s reassembly timeout
        reassembler.expireStale(clock.nowMs());
    }

    REQUIRE(complete.has_value());
    CHECK(complete->size() == whole.size());
    CHECK(bytesEqual(complete->bytes(), whole));
}
