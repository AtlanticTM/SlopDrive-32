// ============================================================================
// test_main.cpp — doctest unit tests for slopsync-core's data-plane + raw
// wire layer: wire/stream_bundle.hpp, wire/packed/layout_codec.hpp,
// channel/state_apply.hpp, wire/raw/{ping_pong,clock_frame,probe,ackmask,
// beacon}.hpp.
//
// Native (host-side, hardware-free) test, same pattern as
// test/native/test_slopsync_wire/test_main.cpp. Suite ids D-01..D-06 and
// T-01..T-02 match docs/slopsync/vectors/manifest.yaml's `state_stream` and
// `clock` suites; SPEC section numbers cite docs/slopsync/SPEC.md. ACKMASK/
// BEACON/PROBE/PING have no assigned vector ids in the current manifest —
// their round-trip checks below are plain byte-exact coverage, not a
// numbered vector.
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/channel/state_apply.hpp"
#include "slopsync/conformance/mini_catalog.hpp"
#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/packed/layout_codec.hpp"
#include "slopsync/wire/raw/ackmask.hpp"
#include "slopsync/wire/raw/beacon.hpp"
#include "slopsync/wire/raw/clock_frame.hpp"
#include "slopsync/wire/raw/ping_pong.hpp"
#include "slopsync/wire/raw/probe.hpp"
#include "slopsync/wire/stream_bundle.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace slopsync;
using namespace slopsync::conformance;

namespace {

// Loose-enough-to-survive-a-round-trip, tight-enough-to-catch-a-bug
// comparison: `quantum` is the field's smallest representable physical step
// (1/scale for integer types; ~0 for f32 fields, where no rounding happens
// at all).
bool approxEq(float a, float b, float quantum) {
    return std::fabs(a - b) <= quantum * 0.5f + 1e-4f;
}

}  // namespace

// ============================================================================
// D-01 — STATE/STREAM layout round-trip for every layout-class mini-catalog
// entry, via the runtime layout_codec (wire/packed/layout_codec.hpp).
// ============================================================================
TEST_CASE("D-01: safety (0x0003) round-trip — bitfield8 + u8/u32/u16") {
    Catalog32 cat = miniCatalog();
    const CatalogEntry* e = cat.find(channels::safety);
    REQUIRE(e != nullptr);
    REQUIRE(e->usesLayout());
    CHECK(e->fieldCount == 4);

    // word=0x0A (bitfield8, raw passthrough, no scale), cause=2, owner_session=123456
    // (an exact float integer, well under 2^24), estop_seq=7.
    const float physical[4] = {10.0f, 2.0f, 123456.0f, 7.0f};

    std::array<std::byte, 32> buf{};
    size_t n = encodeByLayout(*e, physical, buf);
    REQUIRE(n == e->layoutWireSize());
    CHECK(n == 8);  // 1(bitfield8) + 1(u8) + 4(u32) + 2(u16)

    // Hand-derived: 0A | 02 | 40 E2 01 00 (123456 = 0x0001E240, LE) | 07 00
    const std::array<std::byte, 8> expected = {
        std::byte{0x0A}, std::byte{0x02},
        std::byte{0x40}, std::byte{0xE2}, std::byte{0x01}, std::byte{0x00},
        std::byte{0x07}, std::byte{0x00},
    };
    CHECK(std::equal(buf.begin(), buf.begin() + 8, expected.begin()));

    float decoded[4] = {};
    auto dr = decodeByLayout(*e, std::span(buf).first(n), decoded);
    REQUIRE(dr.isOk());
    CHECK(dr.value() == n);
    for (int i = 0; i < 4; ++i) CHECK(approxEq(decoded[i], physical[i], 1.0f));
}

TEST_CASE("D-01: position (0x0080) round-trip — hand-checked exact bytes {123.45,124.0,122.9} mm") {
    Catalog32 cat = miniCatalog();
    const CatalogEntry* e = cat.find(0x0080);
    REQUIRE(e != nullptr);
    CHECK(e->fieldCount == 3);

    const float physical[3] = {123.45f, 124.0f, 122.9f};  // mm

    std::array<std::byte, 16> buf{};
    size_t n = encodeByLayout(*e, physical, buf);
    REQUIRE(n == e->layoutWireSize());
    CHECK(n == 6);

    // Derivation (scale=100, wire = round(physical*100)):
    //   pos_10um: 123.45*100 = 12345.0(ish) -> round -> 12345 = 0x3039 -> LE 39 30
    //   tgt_10um: 124.0 *100 = 12400        -> 12400 = 0x3070 -> LE 70 30
    //   raw_10um: 122.9 *100 = 12290.0(ish) -> round -> 12290 = 0x3002 -> LE 02 30
    const std::array<std::byte, 6> expected = {
        std::byte{0x39}, std::byte{0x30}, std::byte{0x70},
        std::byte{0x30}, std::byte{0x02}, std::byte{0x30},
    };
    CHECK(std::equal(buf.begin(), buf.begin() + 6, expected.begin()));

    float decoded[3] = {};
    auto dr = decodeByLayout(*e, std::span(buf).first(n), decoded);
    REQUIRE(dr.isOk());
    for (int i = 0; i < 3; ++i) CHECK(approxEq(decoded[i], physical[i], 1.0f / 100.0f));
}

TEST_CASE("D-01: motion-status (0x0082) round-trip — bitfield8 + u8") {
    Catalog32 cat = miniCatalog();
    const CatalogEntry* e = cat.find(0x0082);
    REQUIRE(e != nullptr);
    CHECK(e->fieldCount == 2);

    const float physical[2] = {21.0f, 0.0f};  // flags=0b10101, reserved=0

    std::array<std::byte, 8> buf{};
    size_t n = encodeByLayout(*e, physical, buf);
    REQUIRE(n == e->layoutWireSize());
    CHECK(n == 2);
    CHECK(buf[0] == std::byte{0x15});
    CHECK(buf[1] == std::byte{0x00});

    float decoded[2] = {};
    auto dr = decodeByLayout(*e, std::span(buf).first(n), decoded);
    REQUIRE(dr.isOk());
    for (int i = 0; i < 2; ++i) CHECK(approxEq(decoded[i], physical[i], 1.0f));
}

TEST_CASE("D-01: diag (0x0090) round-trip — signed i8/i16/i32 negative values, exact 15-byte payload") {
    Catalog32 cat = miniCatalog();
    const CatalogEntry* e = cat.find(0x0090);
    REQUIRE(e != nullptr);
    CHECK(e->fieldCount == 5);

    // d_i8=-5 (degC), d_i16=-450mm/s (scale 10 -> wire -4500), d_i32=-123456
    // (count), d_u32=999999 (count), d_f32=3.5 (mm/s2).
    const float physical[5] = {-5.0f, -450.0f, -123456.0f, 999999.0f, 3.5f};

    std::array<std::byte, 20> buf{};
    size_t n = encodeByLayout(*e, physical, buf);
    REQUIRE(n == e->layoutWireSize());
    CHECK(n == 15);  // 1 + 2 + 4 + 4 + 4

    // Derivations:
    //   d_i8:  -5                          -> two's complement i8  0xFB
    //   d_i16: -450 * scale(10) = -4500     -> two's complement i16 0xEE6C -> LE 6C EE
    //   d_i32: -123456                      -> two's complement i32 0xFFFE1DC0 -> LE C0 1D FE FF
    //   d_u32: 999999 = 0x000F423F          -> LE 3F 42 0F 00
    //   d_f32: 3.5f = IEEE754 0x40600000    -> LE 00 00 60 40
    const std::array<std::byte, 15> expected = {
        std::byte{0xFB},
        std::byte{0x6C}, std::byte{0xEE},
        std::byte{0xC0}, std::byte{0x1D}, std::byte{0xFE}, std::byte{0xFF},
        std::byte{0x3F}, std::byte{0x42}, std::byte{0x0F}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x60}, std::byte{0x40},
    };
    CHECK(std::equal(buf.begin(), buf.begin() + 15, expected.begin()));

    float decoded[5] = {};
    auto dr = decodeByLayout(*e, std::span(buf).first(n), decoded);
    REQUIRE(dr.isOk());
    CHECK(approxEq(decoded[0], physical[0], 1.0f));
    CHECK(approxEq(decoded[1], physical[1], 1.0f / 10.0f));
    CHECK(approxEq(decoded[2], physical[2], 1.0f));
    CHECK(approxEq(decoded[3], physical[3], 1.0f));
    CHECK(approxEq(decoded[4], physical[4], 1e-5f));
}

// ============================================================================
// D-02 — state_apply newest-wins (SPEC §9.1/§7.3): accept/reject by seq,
// including the u16 wraparound case.
// ============================================================================
TEST_CASE("D-02: newest-wins — seq 10 accepted, then seq 9 rejected") {
    ShadowSlot slot;
    const std::array<std::byte, 3> payloadA = {std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array<std::byte, 3> payloadB = {std::byte{9}, std::byte{9}, std::byte{9}};

    CHECK(applyStateFrame(10, payloadA, slot));
    CHECK(slot.valid);
    CHECK(slot.seq == 10);
    CHECK(slot.size == 3);

    // seq 9 is older than the shadow's seq 10: silently dropped, shadow
    // unchanged.
    CHECK_FALSE(applyStateFrame(9, payloadB, slot));
    CHECK(slot.seq == 10);
    CHECK(slot.value[0] == payloadA[0]);
}

TEST_CASE("D-02: equal seq is NOT newer — rejected, not re-applied") {
    ShadowSlot slot;
    const std::array<std::byte, 2> payloadA = {std::byte{0xAA}, std::byte{0xBB}};
    const std::array<std::byte, 2> payloadB = {std::byte{0xCC}, std::byte{0xDD}};

    CHECK(applyStateFrame(42, payloadA, slot));
    CHECK_FALSE(applyStateFrame(42, payloadB, slot));  // same seq: dropped
    CHECK(slot.value[0] == payloadA[0]);
    CHECK(slot.value[1] == payloadA[1]);
}

TEST_CASE("D-02: wraparound chain 0xFFFE -> 0x0001 is accepted (§7.3 serial comparison)") {
    ShadowSlot slot;
    const std::array<std::byte, 1> p1 = {std::byte{1}};
    const std::array<std::byte, 1> p2 = {std::byte{2}};

    CHECK(applyStateFrame(0xFFFE, p1, slot));
    CHECK(slot.seq == 0xFFFE);

    // 0x0001 is newer than 0xFFFE by serial arithmetic (wraps): must accept.
    CHECK(applyStateFrame(0x0001, p2, slot));
    CHECK(slot.seq == 0x0001);
    CHECK(slot.value[0] == p2[0]);
}

TEST_CASE("D-02: oversized payload is rejected with the capacity flag, shadow untouched") {
    ShadowSlot slot;
    std::vector<std::byte> big(limits::min_transport_payload + 1, std::byte{0x11});

    bool capacityExceeded = false;
    CHECK_FALSE(applyStateFrame(1, big, slot, &capacityExceeded));
    CHECK(capacityExceeded);
    CHECK_FALSE(slot.valid);  // never touched

    // A same-size-as-cap payload (exactly min_transport_payload) is fine.
    std::vector<std::byte> fits(limits::min_transport_payload, std::byte{0x22});
    capacityExceeded = false;
    CHECK(applyStateFrame(1, fits, slot, &capacityExceeded));
    CHECK_FALSE(capacityExceeded);
    CHECK(slot.valid);
    CHECK(slot.size == limits::min_transport_payload);
}

// ============================================================================
// D-03 — STATE-fit conformance check: every layout-class mini-catalog entry
// fits limits::min_transport_payload (242 B). A genuinely oversized
// CatalogEntry can't be constructed in this data model (kMaxFields=8, widest
// field is 4 bytes -> worst case 32 B, far under 242) — that mechanical
// impossibility is exactly why the FULL D-03 vector (a real 243-byte-layout
// catalog rejected by conformance tooling) is deferred to the M6 CLI, which
// builds catalogs from parsed YAML/CBOR rather than this fixed-capacity
// struct. What we CAN and do test natively is that the comparison the CLI
// will run is the right one.
// ============================================================================
TEST_CASE("D-03: every mini-catalog layout entry fits the 242-byte STATE floor") {
    Catalog32 cat = miniCatalog();
    bool sawAny = false;
    for (uint16_t i = 0; i < cat.count; ++i) {
        const CatalogEntry& e = cat.entries[i];
        if (!e.usesLayout()) continue;
        sawAny = true;
        CHECK(e.layoutWireSize() <= limits::min_transport_payload);
    }
    CHECK(sawAny);

    // The mechanical maximum this data model can even represent: 8 fields,
    // 4 bytes each (u32/i32/f32, the widest PackedFieldType).
    constexpr size_t kMaxPossibleLayoutBytes = CatalogEntry::kMaxFields * 4;
    CHECK(kMaxPossibleLayoutBytes < limits::min_transport_payload);
}

TEST_CASE("D-03: the fit comparison itself, against synthetic sizes (243 over / 242 at-the-line)") {
    CHECK(size_t(242) <= limits::min_transport_payload);
    CHECK_FALSE(size_t(243) <= limits::min_transport_payload);
}

// ============================================================================
// D-04 — BundleWriter: 240 Hz-spaced samples, byte-exact header + t_off
// array, sampleTimeUs correctness.
//
// NOTE ON SAMPLE COUNT (deviation from the literal "8 samples" brief):
// 240 Hz's true sample interval is 1e6/240 = 4166.67 us; truncated to 4166
// us per the brief. But limits::bundle_max_span_ms is 20 (i.e. a bundle's
// span, t_off[n-1], is capped at 20000 us) — and 7 * 4166 = 29162 us, which
// is OVER that cap. Eight contiguous 240 Hz samples in ONE bundle is
// therefore not actually constructible without violating SPEC §5.4's own
// span cap, even though §9.2's illustrative "30 fps x 8-sample bundles"
// example describes exactly that shape. This looks like a genuine tension
// between §9.2's prose example and §5.4/Appendix G's normative 20 ms cap
// (worth flagging upstream). This test uses 5 samples (span 16664 us,
// within the cap) to check byte-exact 240 Hz-spaced encoding; D-05 below
// uses the literal 8-sample/240 Hz case to prove the span cap correctly
// REJECTS it.
// ============================================================================
TEST_CASE("D-04: BundleWriter — 5 samples @240Hz spacing, byte-exact header/t_off, sampleTimeUs") {
    constexpr uint32_t kTBase = 1000000;  // fixtures.hub_time_origin_us
    constexpr size_t kSampleSize = 6;
    constexpr int kN = 5;
    constexpr uint16_t kStepUs = 4166;  // floor(1e6/240), documented above

    std::array<std::byte, kStreamBundleHeaderBytes + 2 * kN + kN * kSampleSize> buf{};
    BundleWriter w(buf, kTBase, kSampleSize);

    for (int i = 0; i < kN; ++i) {
        std::array<std::byte, kSampleSize> sample{};
        sample.fill(std::byte(uint8_t(0xA0 + i)));  // distinct, checkable fill
        REQUIRE(w.addSample(uint16_t(i * kStepUs), sample));
    }
    CHECK(w.sampleCount() == kN);
    size_t total = w.finalize();
    CHECK(total == buf.size());

    // Header: t_base=1000000=0x000F4240 LE 40 42 0F 00 | n=5 | reserved=0
    CHECK(buf[0] == std::byte{0x40});
    CHECK(buf[1] == std::byte{0x42});
    CHECK(buf[2] == std::byte{0x0F});
    CHECK(buf[3] == std::byte{0x00});
    CHECK(buf[4] == std::byte{0x05});
    CHECK(buf[5] == std::byte{0x00});

    // t_off[i] = i*4166: 0, 4166=0x1046, 8332=0x208C, 12498=0x30D2, 16664=0x4118
    const std::array<std::byte, 10> expectedTOffs = {
        std::byte{0x00}, std::byte{0x00}, std::byte{0x46}, std::byte{0x10},
        std::byte{0x8C}, std::byte{0x20}, std::byte{0xD2}, std::byte{0x30},
        std::byte{0x18}, std::byte{0x41},
    };
    CHECK(std::equal(buf.begin() + 6, buf.begin() + 16, expectedTOffs.begin()));

    auto view = BundleView::parse(buf, kSampleSize);
    REQUIRE(view.isOk());
    CHECK(view.value().tBase() == kTBase);
    CHECK(view.value().sampleCount() == kN);
    for (int i = 0; i < kN; ++i) {
        CHECK(view.value().sampleTimeUs(size_t(i)) == kTBase + uint32_t(i * kStepUs));
        auto s = view.value().sample(size_t(i));
        REQUIRE(s.size() == kSampleSize);
        for (auto b : s) CHECK(b == std::byte(uint8_t(0xA0 + i)));
    }
}

// ============================================================================
// D-05 — bundle caps enforced: 33rd sample rejected, span > 20ms rejected,
// out-span too small rejected, BundleView on a truncated buffer errors
// (never crashes).
// ============================================================================
TEST_CASE("D-05: 33rd sample rejected purely by the n<=32 cap") {
    std::array<std::byte, 300> buf{};  // ample room; not the limiting factor
    BundleWriter w(buf, 0, 1);
    for (int i = 0; i < 32; ++i) {
        std::array<std::byte, 1> s = {std::byte(uint8_t(i))};
        REQUIRE(w.addSample(uint16_t(i), s));
    }
    CHECK(w.sampleCount() == 32);

    std::array<std::byte, 1> s33 = {std::byte{0xFF}};
    CHECK_FALSE(w.addSample(uint16_t(32), s33));
    CHECK(w.sampleCount() == 32);  // unchanged
}

TEST_CASE("D-05: span > 20000us rejected") {
    std::array<std::byte, 64> buf{};
    BundleWriter w(buf, 0, 1);
    std::array<std::byte, 1> s = {std::byte{0}};
    REQUIRE(w.addSample(0, s));

    // 20000us itself is the cap boundary (<=): must be ACCEPTED.
    CHECK(w.addSample(uint16_t(limits::bundle_max_span_ms * 1000), s));
    CHECK(w.sampleCount() == 2);

    // One microsecond further would be a new monotonic max but crosses the
    // span cap (> 20000): rejected.
    CHECK_FALSE(w.addSample(uint16_t(limits::bundle_max_span_ms * 1000 + 1), s));
    CHECK(w.sampleCount() == 2);
}

TEST_CASE("D-05: literal 8 samples @240Hz spacing overflows the 20ms span cap (see D-04's note)") {
    std::array<std::byte, 300> buf{};
    BundleWriter w(buf, 0, 1);
    std::array<std::byte, 1> s = {std::byte{0}};
    constexpr uint16_t kStepUs = 4166;
    int accepted = 0;
    for (int i = 0; i < 8; ++i) {
        if (w.addSample(uint16_t(i * kStepUs), s)) ++accepted;
        else break;
    }
    // 7*4166 = 29162 > 20000: the 6th sample (index 5, t_off=20830) is the
    // first to exceed the cap (index 4's t_off=16664 is still within it).
    CHECK(accepted == 5);
}

TEST_CASE("D-05: out-span too small is rejected, not partially written") {
    std::array<std::byte, 13> tooSmall{};  // first sample needs 6+2+6=14 bytes
    BundleWriter w(tooSmall, 0, 6);
    std::array<std::byte, 6> sample{};
    CHECK_FALSE(w.addSample(0, sample));
    CHECK(w.sampleCount() == 0);
    // The writer itself is still valid (13 bytes is enough for the 6-byte
    // header) — it's only this particular sample that didn't fit. A valid,
    // empty (n=0) bundle is 6 bytes: just the header.
    CHECK(w.finalize() == kStreamBundleHeaderBytes);

    // A buffer too small even for the header is unconditionally invalid.
    std::array<std::byte, 5> wayTooSmall{};
    BundleWriter w2(wayTooSmall, 0, 6);
    CHECK_FALSE(w2.addSample(0, sample));
    CHECK(w2.finalize() == 0);
}

TEST_CASE("D-05: BundleView on a truncated buffer errors, does not crash") {
    constexpr size_t kSampleSize = 4;
    std::array<std::byte, kStreamBundleHeaderBytes + 2 * 2 + 2 * kSampleSize> buf{};
    BundleWriter w(buf, 5000, kSampleSize);
    std::array<std::byte, kSampleSize> s0{}, s1{};
    s0.fill(std::byte{0x01});
    s1.fill(std::byte{0x02});
    REQUIRE(w.addSample(0, s0));
    REQUIRE(w.addSample(100, s1));
    size_t full = w.finalize();
    REQUIRE(full == buf.size());

    auto truncated = std::span(buf).first(full - 1);
    auto view = BundleView::parse(truncated, kSampleSize);
    CHECK_FALSE(view.isOk());
    CHECK(view.error() == DecodeError::Truncated);

    // A buffer too short even for the 6-byte header is likewise a clean error.
    std::array<std::byte, 3> wayTooShort{};
    auto view2 = BundleView::parse(wayTooShort, kSampleSize);
    CHECK_FALSE(view2.isOk());
    CHECK(view2.error() == DecodeError::Truncated);
}

// ============================================================================
// D-06 — append-only evolution: a short 3-field prefix view reads diag's
// full 15-byte payload correctly, ignoring the trailing (unknown-to-it)
// bytes (SPEC §5.4).
// ============================================================================
TEST_CASE("D-06: append-only prefix parse — 3-field view over diag's full 15-byte payload") {
    Catalog32 cat = miniCatalog();
    const CatalogEntry* diag = cat.find(0x0090);
    REQUIRE(diag != nullptr);

    const float physical[5] = {-5.0f, -450.0f, -123456.0f, 999999.0f, 3.5f};
    std::array<std::byte, 15> full{};
    size_t n = encodeByLayout(*diag, physical, full);
    REQUIRE(n == 15);

    // A synthetic "old client" catalog entry that only knows diag's first
    // three fields (d_i8, d_i16, d_i32) — as if compiled against an earlier
    // catalog etag, before d_u32/d_f32 were appended.
    CatalogEntry prefix = *diag;
    prefix.fieldCount = 3;

    constexpr size_t kPrefixBytes = 1 + 2 + 4;  // i8 + i16 + i32
    CHECK(prefix.layoutWireSize() == kPrefixBytes);

    float decoded[3] = {};
    auto r = appendOnlyRead(prefix, full, decoded);  // `full` is longer than the prefix needs
    REQUIRE(r.isOk());
    CHECK(r.value() == kPrefixBytes);  // only 7 of the 15 bytes were consumed
    CHECK(approxEq(decoded[0], physical[0], 1.0f));
    CHECK(approxEq(decoded[1], physical[1], 1.0f / 10.0f));
    CHECK(approxEq(decoded[2], physical[2], 1.0f));
    // The trailing 8 bytes (d_u32, d_f32) were never touched by this read —
    // that IS the append-only guarantee: no error, no truncation, no need
    // for the reader to know they exist.
}

// ============================================================================
// T-01 — CLOCK offset/RTT math, exact values.
// ============================================================================
TEST_CASE("T-01: computeClockSync exact values — t0=1000,t1=6000,t2=6500,t3=3500") {
    // offset = ((t1-t0)+(t2-t3))/2 = ((6000-1000)+(6500-3500))/2 = (5000+3000)/2 = 4000
    // rtt    = (t3-t0)-(t2-t1)           = (3500-1000)-(6500-6000) = 2500-500 = 2000
    ClockSync s = computeClockSync(1000, 6000, 6500, 3500);
    CHECK(s.offsetUs == 4000);
    CHECK(s.rttUs == 2000);
}

TEST_CASE("T-01: computeClockSync asymmetric case — large hub/client clock offset") {
    // t0=2000, t1=100000, t2=100200, t3=2500
    // offset = ((100000-2000)+(100200-2500))/2 = (98000+97700)/2 = 97850
    // rtt    = (2500-2000)-(100200-100000)     = 500-200 = 300
    ClockSync s = computeClockSync(2000, 100000, 100200, 2500);
    CHECK(s.offsetUs == 97850);
    CHECK(s.rttUs == 300);
}

TEST_CASE("CLOCK request/reply byte-exact round-trip") {
    std::array<std::byte, kClockRequestBytes> reqBuf{};
    CHECK(encodeClockRequest(0x00001000, reqBuf) == kClockRequestBytes);
    // 0x00001000 LE -> 00 10 00 00
    CHECK(reqBuf[0] == std::byte{0x00});
    CHECK(reqBuf[1] == std::byte{0x10});
    CHECK(reqBuf[2] == std::byte{0x00});
    CHECK(reqBuf[3] == std::byte{0x00});
    auto reqDecoded = decodeClockRequest(reqBuf);
    REQUIRE(reqDecoded.isOk());
    CHECK(reqDecoded.value().t0 == 0x00001000);

    std::array<std::byte, kClockReplyBytes> replyBuf{};
    CHECK(encodeClockReply(1000, 6000, 6500, replyBuf) == kClockReplyBytes);
    auto replyDecoded = decodeClockReply(replyBuf);
    REQUIRE(replyDecoded.isOk());
    CHECK(replyDecoded.value().t0 == 1000);
    CHECK(replyDecoded.value().t1 == 6000);
    CHECK(replyDecoded.value().t2 == 6500);
}

// ============================================================================
// T-02 — µs wraparound: BundleView.sampleTimeUs across the u32 wrap, and
// computeClockSync with values straddling it.
// ============================================================================
TEST_CASE("T-02: BundleView.sampleTimeUs wraps correctly across 0xFFFFFFFF") {
    constexpr uint32_t kTBase = 0xFFFFFFF0u;
    constexpr size_t kSampleSize = 1;
    std::array<std::byte, kStreamBundleHeaderBytes + 2 * 3 + 3 * kSampleSize> buf{};
    BundleWriter w(buf, kTBase, kSampleSize);
    std::array<std::byte, 1> s{};
    REQUIRE(w.addSample(0, s));
    REQUIRE(w.addSample(10, s));
    REQUIRE(w.addSample(20, s));
    REQUIRE(w.finalize() == buf.size());

    auto view = BundleView::parse(buf, kSampleSize);
    REQUIRE(view.isOk());
    // 0xFFFFFFF0 + 0  = 0xFFFFFFF0  (no wrap yet)
    // 0xFFFFFFF0 + 10 = 0xFFFFFFFA  (no wrap yet)
    // 0xFFFFFFF0 + 20 = 0x100000004 mod 2^32 = 0x00000004 (wraps)
    CHECK(view.value().sampleTimeUs(0) == 0xFFFFFFF0u);
    CHECK(view.value().sampleTimeUs(1) == 0xFFFFFFFAu);
    CHECK(view.value().sampleTimeUs(2) == 0x00000004u);
}

TEST_CASE("T-02: computeClockSync with t0/t2/t3 straddling the u32 wrap still yields small correct offset/rtt") {
    // Hand-derived (see docs/slopsync SPEC §7.2 windowed-delta rule):
    //   t0 = 0xFFFFFF00
    //   t1 = (t0 + 352)  mod 2^32 = 0x00000060   (d1 = t1-t0 windowed = 352)
    //   t3 = 0xFFFFFF50
    //   t2 = (t3 + 280)  mod 2^32 = 0x00000068   (d2 = t2-t3 windowed = 280)
    // offset = (352+280)/2 = 316
    // dA = t3-t0 windowed = 0xFFFFFF50-0xFFFFFF00 = 80
    // dB = t2-t1 windowed = 0x68-0x60             = 8
    // rtt = 80-8 = 72
    constexpr uint32_t t0 = 0xFFFFFF00u;
    constexpr uint32_t t1 = 0x00000060u;
    constexpr uint32_t t2 = 0x00000068u;
    constexpr uint32_t t3 = 0xFFFFFF50u;

    ClockSync s = computeClockSync(t0, t1, t2, t3);
    CHECK(s.offsetUs == 316);
    CHECK(s.rttUs == 72);
}

// ============================================================================
// PING/PONG — SPEC §6.5.
// ============================================================================
TEST_CASE("PING/PONG: empty PING payload, and PONG echoes a non-empty PING payload byte-exact") {
    std::array<std::byte, 4> out{};
    CHECK(encodePing(std::span<const std::byte>{}, out) == 0);

    const std::array<std::byte, 2> pingPayload = {std::byte{0xAB}, std::byte{0xCD}};
    size_t n = encodePong(pingPayload, out);
    REQUIRE(n == 2);
    CHECK(out[0] == std::byte{0xAB});
    CHECK(out[1] == std::byte{0xCD});
}

// ============================================================================
// PROBE — SPEC §6.4.
// ============================================================================
TEST_CASE("PROBE: burst frame byte-exact — probe_index=5, zero-filled padding") {
    std::array<std::byte, 8> out{};
    out.fill(std::byte{0xEE});  // pre-fill with non-zero to prove padding gets zeroed
    CHECK(encodeProbeFrame(5, out) == out.size());
    CHECK(out[0] == std::byte{0x05});
    CHECK(out[1] == std::byte{0x00});
    for (size_t i = 2; i < out.size(); ++i) CHECK(out[i] == std::byte{0x00});

    auto decoded = decodeProbeFrame(out);
    REQUIRE(decoded.isOk());
    CHECK(decoded.value() == 5);

    // Padding-free decode (just the 2-byte index) also works — trailing
    // bytes are optional, never required (§4.3).
    std::array<std::byte, 2> justIndex = {std::byte{0x2A}, std::byte{0x00}};
    auto decoded2 = decodeProbeFrame(justIndex);
    REQUIRE(decoded2.isOk());
    CHECK(decoded2.value() == 0x2A);
}

// ============================================================================
// ACKMASK — SPEC §13.3.
// ============================================================================
TEST_CASE("ACKMASK: byte-exact round-trip — base_seq=100, mask=0x00000005") {
    AckMask m{100, 0x00000005u};
    std::array<std::byte, kAckMaskBytes> buf{};
    CHECK(encodeAckMask(m, buf) == kAckMaskBytes);
    // base_seq=100=0x0064 LE 64 00 | mask=0x00000005 LE 05 00 00 00
    const std::array<std::byte, 6> expected = {
        std::byte{0x64}, std::byte{0x00}, std::byte{0x05},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    CHECK(buf == expected);

    auto decoded = decodeAckMask(buf);
    REQUIRE(decoded.isOk());
    CHECK(decoded.value().base_seq == 100);
    CHECK(decoded.value().mask == 0x00000005u);
}

TEST_CASE("ACKMASK: unpackAckMask visits exactly the set bits' seqs") {
    std::vector<uint16_t> seen;
    unpackAckMask(100, 0x00000005u, [&](uint16_t seq) { seen.push_back(seq); });
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == 100);  // bit 0
    CHECK(seen[1] == 102);  // bit 2
}

TEST_CASE("ACKMASK: AckTracker record() + window slide + stale-drop") {
    AckTracker t(100);
    t.record(100);
    t.record(101);
    CHECK(t.base() == 100);
    CHECK(t.mask() == 0b11u);

    // seq 133 is 33 past base -> outside the 32-wide window -> slides:
    // shift = (33)-31 = 2, new base = 102, old mask (0b11) >> 2 = 0, then
    // set the new top bit (31): mask = 0x80000000.
    t.record(133);
    CHECK(t.base() == 102);
    CHECK(t.mask() == 0x80000000u);

    // A seq older than the new base is stale: no-op.
    t.record(50);
    CHECK(t.base() == 102);
    CHECK(t.mask() == 0x80000000u);
}

// ============================================================================
// BEACON — SPEC §13.7.
// ============================================================================
TEST_CASE("BEACON: byte-exact round-trip — boot_id=0xB007CAFE, etag=0102..08, pairing_open=true") {
    BeaconFrame b;
    b.boot_id = 0xB007CAFE;
    b.etag = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
              std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
    b.pairing_open = true;

    std::array<std::byte, kBeaconFrameBytes> buf{};
    CHECK(encodeBeacon(b, buf) == kBeaconFrameBytes);
    CHECK(kBeaconFrameBytes == 13);

    // boot_id=0xB007CAFE LE -> FE CA 07 B0
    const std::array<std::byte, 13> expected = {
        std::byte{0xFE}, std::byte{0xCA}, std::byte{0x07}, std::byte{0xB0},
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
        std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
        std::byte{0x01},
    };
    CHECK(buf == expected);

    auto decoded = decodeBeacon(buf);
    REQUIRE(decoded.isOk());
    CHECK(decoded.value().boot_id == 0xB007CAFE);
    CHECK(decoded.value().etag == b.etag);
    CHECK(decoded.value().pairing_open == true);
}
