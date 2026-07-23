// ============================================================================
// test_main.cpp — doctest unit tests for slopsync-core's remaining
// control-plane message codecs: subscribe.hpp, grant.hpp, intent.hpp,
// echo.hpp, event.hpp, nack.hpp, goodbye.hpp, pair.hpp, probe_report.hpp.
//
// Native (host-side, hardware-free) test, same pattern as
// test/native/test_slopsync_cbor/test_main.cpp: doctest's bundled main(), no
// Arduino, no bus/FreeRTOS dependency — every header under test is pure
// header-only math/logic. SPEC section numbers cite docs/slopsync/SPEC.md.
//
// Per-family coverage, mirrored across every message below:
//   (a) full round-trip with every optional field present
//   (b) round-trip with every optional field/array absent (or, for message
//       shapes with no top-level optionals at all — GRANT, SUBSCRIBE,
//       UNSUBSCRIBE, ECHO's applied-map, PAIR_REQ, PAIR_GRANT, PROBE_REPORT
//       — the analogous "empty array" / "all-zero" minimal shape, called out
//       explicitly at each such test since the SPEC gives those messages no
//       optional keys to omit)
//   (d) unknown-key tolerance (§4.3): one shared splice helper, reused per
//       family, appends an out-of-range top-level key to a valid encode and
//       asserts decode still succeeds
//   (e) ascending-key discipline: every full/minimal test also asserts the
//       encoder produces byte-identical output on a repeat call
// (c) is the brief's byte-exact hand-derived goldens, done once each for
// INTENT (set-speed) and NACK (NOT_CONTROLLER alone).
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"
#include "slopsync/wire/messages/echo.hpp"
#include "slopsync/wire/messages/event.hpp"
#include "slopsync/wire/messages/goodbye.hpp"
#include "slopsync/wire/messages/grant.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/intent.hpp"
#include "slopsync/wire/messages/nack.hpp"
#include "slopsync/wire/messages/pair.hpp"
#include "slopsync/wire/messages/probe_report.hpp"
#include "slopsync/wire/messages/subscribe.hpp"
#include "slopsync/wire/messages/welcome.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

using namespace slopsync;

namespace {

constexpr std::byte B(int x) { return std::byte(uint8_t(x)); }

template <size_t N>
bool bytesEqual(std::span<const std::byte> got, const std::byte (&expected)[N]) {
    if (got.size() != N) return false;
    for (size_t i = 0; i < N; ++i) {
        if (got[i] != expected[i]) return false;
    }
    return true;
}

// (e) ascending-key discipline, shared across every family: encoding the
// same message struct twice must produce byte-identical output. Takes any
// `encodeFn(const Msg&, std::span<std::byte>) -> size_t`.
template <typename EncodeFn, typename Msg>
size_t checkDeterministic(EncodeFn encodeFn, const Msg& m) {
    std::array<std::byte, 256> buf1{};
    std::array<std::byte, 256> buf2{};
    size_t n1 = encodeFn(m, buf1);
    size_t n2 = encodeFn(m, buf2);
    REQUIRE(n1 > 0);
    REQUIRE(n1 == n2);
    CHECK(std::memcmp(buf1.data(), buf2.data(), n1) == 0);
    return n1;
}

// (d) unknown-key tolerance, shared across every family (§4.3). Precondition:
// `encoded` starts with a direct-form (short) CBOR map head (major 5, pair
// count <= 22 so count+1 stays direct-form, true of every message tested
// below — the largest is NACK's/EVENT's 5 pairs). Appends one extra pair
// {99: 7} — 99 is numerically larger than every top-level key used by any
// message in this suite (max is GrantMsg's grants=35), so appending it last
// preserves the required ascending order.
std::vector<std::byte> spliceUnknownKey(std::span<const std::byte> encoded) {
    std::vector<std::byte> out(encoded.begin(), encoded.end());
    uint8_t header = uint8_t(out[0]);
    out[0] = std::byte(uint8_t(header + 1));  // map pair count + 1, still direct-form
    out.push_back(B(0x18));  // key head: 1-byte-follow (ai=24)
    out.push_back(B(0x63));  // key = 99
    out.push_back(B(0x07));  // value: uint(7), direct form
    return out;
}

}  // namespace

// ============================================================================
// SUBSCRIBE (§6.6)
// ============================================================================
TEST_CASE("SUBSCRIBE: full round-trip (2 wishes) + determinism") {
    SubscribeMsg m{};
    m.subscriptions_count = 2;
    m.subscriptions[0] = SubscriptionWish{/*channel_id=*/10, /*rate_hz=*/30.0f, /*priority=*/1};
    m.subscriptions[1] = SubscriptionWish{/*channel_id=*/20, /*rate_hz=*/0.0f, /*priority=*/0};

    size_t n = checkDeterministic(encodeSubscribe, m);

    std::array<std::byte, 128> buf{};
    n = encodeSubscribe(m, buf);
    auto dec = decodeSubscribe(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    REQUIRE(dec.value().subscriptions_count == 2);
    CHECK(dec.value().subscriptions[0].channel_id == 10);
    CHECK(dec.value().subscriptions[0].rate_hz == doctest::Approx(30.0f));
    CHECK(dec.value().subscriptions[0].priority == 1);
    CHECK(dec.value().subscriptions[1].channel_id == 20);
    CHECK(dec.value().subscriptions[1].priority == 0);
}

TEST_CASE("SUBSCRIBE: minimal (empty wish array) — no top-level optionals to omit, "
          "so the 'shrink' case is the array going to zero length") {
    SubscribeMsg m{};  // subscriptions_count = 0
    std::array<std::byte, 32> buf{};
    size_t n = encodeSubscribe(m, buf);
    REQUIRE(n > 0);

    auto dec = decodeSubscribe(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(dec.value().subscriptions_count == 0);
}

// ============================================================================
// UNSUBSCRIBE (§6.6) — bare channel_id array under the same key 10.
// ============================================================================
TEST_CASE("UNSUBSCRIBE: full round-trip (3 channel ids) + determinism") {
    UnsubscribeMsg m{};
    m.channel_count = 3;
    m.channel_ids = {5, 6, 80};

    checkDeterministic(encodeUnsubscribe, m);

    std::array<std::byte, 32> buf{};
    size_t n = encodeUnsubscribe(m, buf);
    auto dec = decodeUnsubscribe(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    REQUIRE(dec.value().channel_count == 3);
    CHECK(dec.value().channel_ids[0] == 5);
    CHECK(dec.value().channel_ids[1] == 6);
    CHECK(dec.value().channel_ids[2] == 80);
}

TEST_CASE("UNSUBSCRIBE: minimal (empty channel array)") {
    UnsubscribeMsg m{};  // channel_count = 0
    std::array<std::byte, 16> buf{};
    size_t n = encodeUnsubscribe(m, buf);
    REQUIRE(n > 0);

    auto dec = decodeUnsubscribe(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(dec.value().channel_count == 0);
}

// ============================================================================
// GRANT (§10.2) — reuses WELCOME's Grant struct.
// ============================================================================
TEST_CASE("GRANT: full round-trip (2 grants) + determinism") {
    GrantMsg m{};
    m.grants_count = 2;
    m.grants[0] = Grant{/*channel_id=*/80, /*granted_rate_hz=*/240.0f, /*priority=*/2};
    m.grants[1] = Grant{/*channel_id=*/81, /*granted_rate_hz=*/60.0f, /*priority=*/1};

    checkDeterministic(encodeGrant, m);

    std::array<std::byte, 128> buf{};
    size_t n = encodeGrant(m, buf);
    auto dec = decodeGrant(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    REQUIRE(dec.value().grants_count == 2);
    CHECK(dec.value().grants[0].channel_id == 80);
    CHECK(dec.value().grants[0].granted_rate_hz == doctest::Approx(240.0f));
    CHECK(dec.value().grants[1].channel_id == 81);
    CHECK(dec.value().grants[1].priority == 1);
}

TEST_CASE("GRANT: minimal (empty grants array) — no top-level optionals to omit") {
    GrantMsg m{};  // grants_count = 0
    std::array<std::byte, 16> buf{};
    size_t n = encodeGrant(m, buf);
    REQUIRE(n > 0);

    auto dec = decodeGrant(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(dec.value().grants_count == 0);
}

// ============================================================================
// INTENT (§9.3)
// ============================================================================
TEST_CASE("INTENT: full round-trip (precondition + takeover present) + determinism") {
    IntentMsg m{};
    m.channel_id = 5;
    m.intent_id = 9;
    m.value_count = 2;
    m.value[0] = IntentValueField{1, IntentValue::ofU64(7)};
    m.value[1] = IntentValueField{2, IntentValue::ofBool(true)};
    m.has_precondition = true;
    m.precondition = 42;
    m.has_takeover = true;
    m.takeover = true;

    checkDeterministic(encodeIntent, m);

    std::array<std::byte, 128> buf{};
    size_t n = encodeIntent(m, buf);
    auto dec = decodeIntent(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    const IntentMsg& d = dec.value();
    CHECK(d.channel_id == 5);
    CHECK(d.intent_id == 9);
    REQUIRE(d.value_count == 2);
    CHECK(d.value[0].key == 1);
    CHECK(d.value[0].value.kind == IntentValue::Kind::U64);
    CHECK(d.value[0].value.u64_val == 7);
    CHECK(d.value[1].key == 2);
    CHECK(d.value[1].value.kind == IntentValue::Kind::Bool);
    CHECK(d.value[1].value.bool_val == true);
    REQUIRE(d.has_precondition);
    CHECK(d.precondition == 42);
    REQUIRE(d.has_takeover);
    CHECK(d.takeover == true);
}

TEST_CASE("INTENT: minimal (precondition/takeover absent) — map shrinks from 5 to 3 pairs") {
    IntentMsg m{};
    m.channel_id = 5;
    m.intent_id = 9;
    m.value_count = 1;
    m.value[0] = IntentValueField{1, IntentValue::ofF32(1.5f)};
    // has_precondition / has_takeover left false.

    std::array<std::byte, 64> buf{};
    size_t n = encodeIntent(m, buf);
    REQUIRE(n > 0);
    CHECK(uint8_t(buf[0]) == (0xA0 | 3));  // map(3 pairs): channel_id, intent_id, value

    auto dec = decodeIntent(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    const IntentMsg& d = dec.value();
    CHECK_FALSE(d.has_precondition);
    CHECK_FALSE(d.has_takeover);
    REQUIRE(d.value_count == 1);
    CHECK(d.value[0].value.kind == IntentValue::Kind::F32);
    CHECK(d.value[0].value.f32_val == doctest::Approx(1.5f));
}

TEST_CASE("INTENT: I64 kind collapses to U64 on decode for non-negative values (documented "
          "wire-collapse, see intent.hpp) — only a genuinely negative value survives as I64") {
    IntentMsg m{};
    m.channel_id = 1;
    m.intent_id = 1;
    m.value_count = 2;
    m.value[0] = IntentValueField{1, IntentValue::ofI64(-5)};   // negative -> stays I64
    m.value[1] = IntentValueField{2, IntentValue::ofI64(100)};  // non-negative -> becomes U64

    std::array<std::byte, 64> buf{};
    size_t n = encodeIntent(m, buf);
    auto dec = decodeIntent(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(dec.value().value[0].value.kind == IntentValue::Kind::I64);
    CHECK(dec.value().value[0].value.i64_val == -5);
    CHECK(dec.value().value[1].value.kind == IntentValue::Kind::U64);
    CHECK(dec.value().value[1].value.u64_val == 100);
}

// ----------------------------------------------------------------------------
// (c) INTENT golden: set-speed, channel 0x0084 (132), intent_id 1,
// value {1: 400.0f}. Hand-derived per RFC 8949 / SPEC §5.3:
//
//   A3                     map(3 pairs)
//   0F 18 84               key=15(channel_id, direct) val=132 (>=24 -> 1-byte follow)
//   12 01                  key=18(intent_id, direct)  val=1
//   14 A1                  key=20(value, direct)      val=map(1 pair)
//      01                     subkey=1 (direct)
//      FA 43 C8 00 00         val=f32(400.0) — 400 = 1.5625 * 2^8, exponent
//                             8+127=135=0x87, mantissa 0b1001000...0 ->
//                             bits 0x43C80000 (binary32, big-endian on wire)
// ----------------------------------------------------------------------------
TEST_CASE("INTENT golden (c): set-speed channel=0x0084 intent_id=1 value{1:400.0f}") {
    IntentMsg m{};
    m.channel_id = 0x0084;
    m.intent_id = 1;
    m.value_count = 1;
    m.value[0] = IntentValueField{1, IntentValue::ofF32(400.0f)};

    std::array<std::byte, 32> buf{};
    size_t n = encodeIntent(m, buf);

    static constexpr std::byte kExpected[] = {
        B(0xA3),
        B(0x0F), B(0x18), B(0x84),
        B(0x12), B(0x01),
        B(0x14), B(0xA1),
        B(0x01), B(0xFA), B(0x43), B(0xC8), B(0x00), B(0x00),
    };
    static_assert(sizeof(kExpected) == 14, "hand count must match derivation");

    REQUIRE(n == sizeof(kExpected));
    CHECK(bytesEqual(std::span<const std::byte>(buf.data(), n), kExpected));

    auto dec = decodeIntent(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(dec.value().channel_id == 0x0084);
    CHECK(dec.value().intent_id == 1);
    REQUIRE(dec.value().value_count == 1);
    CHECK(dec.value().value[0].key == 1);
    CHECK(dec.value().value[0].value.kind == IntentValue::Kind::F32);
    CHECK(dec.value().value[0].value.f32_val == doctest::Approx(400.0f));
}

// ============================================================================
// ECHO (§9.3)
// ============================================================================
TEST_CASE("ECHO: full round-trip (2 applied fields) + determinism") {
    EchoMsg m{};
    m.intent_id = 9;
    m.cfg_gen = 3;
    m.applied_count = 2;
    m.applied[0] = IntentValueField{1, IntentValue::ofF32(400.0f)};
    m.applied[1] = IntentValueField{2, IntentValue::ofBool(false)};

    checkDeterministic(encodeEcho, m);

    std::array<std::byte, 64> buf{};
    size_t n = encodeEcho(m, buf);
    auto dec = decodeEcho(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    const EchoMsg& d = dec.value();
    CHECK(d.intent_id == 9);
    CHECK(d.cfg_gen == 3);
    REQUIRE(d.applied_count == 2);
    CHECK(d.applied[0].value.f32_val == doctest::Approx(400.0f));
    CHECK(d.applied[1].value.bool_val == false);
}

TEST_CASE("ECHO: minimal (empty applied map) — ECHO has no top-level optionals to omit; "
          "the analogous shrink is the applied map itself going empty") {
    EchoMsg m{};
    m.intent_id = 1;
    m.cfg_gen = 0;
    // applied_count = 0

    std::array<std::byte, 16> buf{};
    size_t n = encodeEcho(m, buf);
    REQUIRE(n > 0);
    CHECK(uint8_t(buf[0]) == (0xA0 | 3));  // map(3 pairs): cfg_gen, intent_id, applied(=empty map)

    auto dec = decodeEcho(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(dec.value().applied_count == 0);
}

// ============================================================================
// EVENT (§9.4)
// ============================================================================
TEST_CASE("EVENT: full round-trip (payload + seq_of_state present) + determinism") {
    EventMsg m{};
    m.channel_id = 7;
    m.timestamp = 123456;
    m.event_kind = 2;
    m.has_seq_of_state = true;
    m.seq_of_state = 55;
    m.has_payload = true;
    m.payload_count = 1;
    m.payload[0] = IntentValueField{1, IntentValue::ofU64(3)};

    checkDeterministic(encodeEvent, m);

    std::array<std::byte, 64> buf{};
    size_t n = encodeEvent(m, buf);
    auto dec = decodeEvent(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    const EventMsg& d = dec.value();
    CHECK(d.channel_id == 7);
    CHECK(d.timestamp == 123456);
    CHECK(d.event_kind == 2);
    REQUIRE(d.has_seq_of_state);
    CHECK(d.seq_of_state == 55);
    REQUIRE(d.has_payload);
    REQUIRE(d.payload_count == 1);
    CHECK(d.payload[0].value.u64_val == 3);
}

TEST_CASE("EVENT: minimal (no payload, no seq_of_state) — map shrinks from 5 to 3 pairs") {
    EventMsg m{};
    m.channel_id = 7;
    m.timestamp = 999;
    m.event_kind = 1;
    // has_seq_of_state / has_payload left false.

    std::array<std::byte, 32> buf{};
    size_t n = encodeEvent(m, buf);
    REQUIRE(n > 0);
    CHECK(uint8_t(buf[0]) == (0xA0 | 3));  // map(3 pairs): channel_id, timestamp, event_kind

    auto dec = decodeEvent(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK_FALSE(dec.value().has_seq_of_state);
    CHECK_FALSE(dec.value().has_payload);
}

// ============================================================================
// NACK (§16.1)
// ============================================================================
TEST_CASE("NACK: full round-trip (all optionals present) + determinism") {
    NackMsg m{};
    m.code = NackCode::BUSY;
    m.has_channel_id = true;
    m.channel_id = 11;
    m.has_intent_id = true;
    m.intent_id = 22;
    m.has_detail = true;
    m.detail = "busy right now";
    m.has_retry_after_ms = true;
    m.retry_after_ms = 500;

    checkDeterministic(encodeNack, m);

    std::array<std::byte, 64> buf{};
    size_t n = encodeNack(m, buf);
    auto dec = decodeNack(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    const NackMsg& d = dec.value();
    CHECK(d.code == NackCode::BUSY);
    REQUIRE(d.has_channel_id);
    CHECK(d.channel_id == 11);
    REQUIRE(d.has_intent_id);
    CHECK(d.intent_id == 22);
    REQUIRE(d.has_detail);
    CHECK(d.detail == "busy right now");
    REQUIRE(d.has_retry_after_ms);
    CHECK(d.retry_after_ms == 500);
}

// ----------------------------------------------------------------------------
// (c) NACK golden: code NOT_CONTROLLER (0x0102) alone, no optionals. Also
// doubles as the (b) "everything absent" case — this IS the minimal form.
// Hand-derived:
//   A1              map(1 pair)
//   10              key=16(code, direct)
//   19 01 02        val=0x0102 (>255 -> 2-byte-follow, big-endian)
// ----------------------------------------------------------------------------
TEST_CASE("NACK golden (c) + minimal (b): code=NOT_CONTROLLER alone") {
    NackMsg m{};
    m.code = NackCode::NOT_CONTROLLER;

    std::array<std::byte, 16> buf{};
    size_t n = encodeNack(m, buf);

    static constexpr std::byte kExpected[] = {
        B(0xA1),
        B(0x10), B(0x19), B(0x01), B(0x02),
    };
    static_assert(sizeof(kExpected) == 5, "hand count must match derivation");

    REQUIRE(n == sizeof(kExpected));
    CHECK(bytesEqual(std::span<const std::byte>(buf.data(), n), kExpected));

    auto dec = decodeNack(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    const NackMsg& d = dec.value();
    CHECK(d.code == NackCode::NOT_CONTROLLER);
    CHECK_FALSE(d.has_channel_id);
    CHECK_FALSE(d.has_intent_id);
    CHECK_FALSE(d.has_detail);
    CHECK_FALSE(d.has_retry_after_ms);
}

// ============================================================================
// GOODBYE (§6.8)
// ============================================================================
TEST_CASE("GOODBYE: full round-trip (detail present) + determinism") {
    GoodbyeMsg m{};
    m.code = NackCode::SESSION_EVICTED;
    m.has_detail = true;
    m.detail = "slow consumer";

    checkDeterministic(encodeGoodbye, m);

    std::array<std::byte, 32> buf{};
    size_t n = encodeGoodbye(m, buf);
    auto dec = decodeGoodbye(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(dec.value().code == NackCode::SESSION_EVICTED);
    REQUIRE(dec.value().has_detail);
    CHECK(dec.value().detail == "slow consumer");
}

TEST_CASE("GOODBYE: minimal (detail absent) — map shrinks from 2 to 1 pair") {
    GoodbyeMsg m{};
    m.code = NackCode::DUPLICATE_INSTANCE;
    // has_detail left false.

    std::array<std::byte, 16> buf{};
    size_t n = encodeGoodbye(m, buf);
    REQUIRE(n > 0);
    CHECK(uint8_t(buf[0]) == (0xA0 | 1));  // map(1 pair): code only

    auto dec = decodeGoodbye(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(dec.value().code == NackCode::DUPLICATE_INSTANCE);
    CHECK_FALSE(dec.value().has_detail);
}

// ============================================================================
// PAIR_REQ / PAIR_GRANT (§12.2) — both fields are mandatory in both
// messages (no optional keys the SPEC allows omitting), so only the "full"
// round-trip case applies; noted rather than silently skipped.
// ============================================================================
TEST_CASE("PAIR_REQ: round-trip + determinism (no optional fields exist to omit)") {
    PairReqMsg m{};
    m.instance_id = {B(0x01), B(0x02), B(0x03), B(0x04), B(0x05), B(0x06), B(0x07), B(0x08)};
    m.pin_proof = {B(0xA0), B(0xA1), B(0xA2), B(0xA3), B(0xA4), B(0xA5), B(0xA6), B(0xA7),
                   B(0xA8), B(0xA9), B(0xAA), B(0xAB), B(0xAC), B(0xAD), B(0xAE), B(0xAF)};

    checkDeterministic(encodePairReq, m);

    std::array<std::byte, 64> buf{};
    size_t n = encodePairReq(m, buf);
    auto dec = decodePairReq(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(std::memcmp(dec.value().instance_id.data(), m.instance_id.data(), 8) == 0);
    CHECK(std::memcmp(dec.value().pin_proof.data(), m.pin_proof.data(), 16) == 0);
}

TEST_CASE("PAIR_GRANT: round-trip + determinism (no optional fields exist to omit)") {
    PairGrantMsg m{};
    m.token = {B(0x00), B(0x01), B(0x02), B(0x03), B(0x04), B(0x05), B(0x06), B(0x07),
               B(0x08), B(0x09), B(0x0A), B(0x0B), B(0x0C), B(0x0D), B(0x0E), B(0x0F)};
    m.roles = 1;  // controller

    checkDeterministic(encodePairGrant, m);

    std::array<std::byte, 64> buf{};
    size_t n = encodePairGrant(m, buf);
    auto dec = decodePairGrant(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(std::memcmp(dec.value().token.data(), m.token.data(), 16) == 0);
    CHECK(dec.value().roles == 1);
}

// ============================================================================
// PROBE_REPORT (§6.4) — no top-level optionals; the sub-map's four fields
// are always all present (mirrors WELCOME's `limits` treatment). "Minimal"
// here means the natural all-zero report a client would send if it measured
// nothing, not a shorter map.
// ============================================================================
TEST_CASE("PROBE_REPORT: full round-trip (populated sub-fields) + determinism") {
    ProbeReportMsg m{};
    m.probe_result = ProbeResult{/*bytes_received=*/8192, /*span_ms=*/1200,
                                  /*loss_pct_x100=*/250, /*rtt_ms=*/18};

    checkDeterministic(encodeProbeReport, m);

    std::array<std::byte, 32> buf{};
    size_t n = encodeProbeReport(m, buf);
    auto dec = decodeProbeReport(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(dec.value().probe_result.bytes_received == 8192);
    CHECK(dec.value().probe_result.span_ms == 1200);
    CHECK(dec.value().probe_result.loss_pct_x100 == 250);
    CHECK(dec.value().probe_result.rtt_ms == 18);
}

TEST_CASE("PROBE_REPORT: all-zero sub-fields still decode correctly") {
    ProbeReportMsg m{};  // every ProbeResult field defaults to 0

    std::array<std::byte, 32> buf{};
    size_t n = encodeProbeReport(m, buf);
    REQUIRE(n > 0);

    auto dec = decodeProbeReport(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(dec.value().probe_result.bytes_received == 0);
    CHECK(dec.value().probe_result.span_ms == 0);
    CHECK(dec.value().probe_result.loss_pct_x100 == 0);
    CHECK(dec.value().probe_result.rtt_ms == 0);
}

// ============================================================================
// (d) Unknown-key tolerance (§4.3): every family, one shared splice helper.
// ============================================================================
TEST_CASE("Unknown-key tolerance (§4.3): an out-of-range top-level key is skipped, "
          "decode still succeeds, for every message family in this suite") {
    SUBCASE("SUBSCRIBE") {
        SubscribeMsg m{};
        m.subscriptions_count = 1;
        m.subscriptions[0] = SubscriptionWish{1, 10.0f, 0};
        std::array<std::byte, 64> buf{};
        size_t n = encodeSubscribe(m, buf);
        auto spliced = spliceUnknownKey(std::span<const std::byte>(buf.data(), n));
        auto dec = decodeSubscribe(spliced);
        REQUIRE(dec.isOk());
        CHECK(dec.value().subscriptions_count == 1);
    }
    SUBCASE("UNSUBSCRIBE") {
        UnsubscribeMsg m{};
        m.channel_count = 1;
        m.channel_ids[0] = 42;
        std::array<std::byte, 32> buf{};
        size_t n = encodeUnsubscribe(m, buf);
        auto spliced = spliceUnknownKey(std::span<const std::byte>(buf.data(), n));
        auto dec = decodeUnsubscribe(spliced);
        REQUIRE(dec.isOk());
        CHECK(dec.value().channel_count == 1);
    }
    SUBCASE("GRANT") {
        GrantMsg m{};
        m.grants_count = 1;
        m.grants[0] = Grant{80, 240.0f, 2};
        std::array<std::byte, 64> buf{};
        size_t n = encodeGrant(m, buf);
        auto spliced = spliceUnknownKey(std::span<const std::byte>(buf.data(), n));
        auto dec = decodeGrant(spliced);
        REQUIRE(dec.isOk());
        CHECK(dec.value().grants_count == 1);
    }
    SUBCASE("INTENT") {
        IntentMsg m{};
        m.channel_id = 1;
        m.intent_id = 1;
        m.value_count = 1;
        m.value[0] = IntentValueField{1, IntentValue::ofU64(9)};
        std::array<std::byte, 64> buf{};
        size_t n = encodeIntent(m, buf);
        auto spliced = spliceUnknownKey(std::span<const std::byte>(buf.data(), n));
        auto dec = decodeIntent(spliced);
        REQUIRE(dec.isOk());
        CHECK(dec.value().value[0].value.u64_val == 9);
    }
    SUBCASE("ECHO") {
        EchoMsg m{};
        m.intent_id = 1;
        m.cfg_gen = 1;
        m.applied_count = 1;
        m.applied[0] = IntentValueField{1, IntentValue::ofBool(true)};
        std::array<std::byte, 64> buf{};
        size_t n = encodeEcho(m, buf);
        auto spliced = spliceUnknownKey(std::span<const std::byte>(buf.data(), n));
        auto dec = decodeEcho(spliced);
        REQUIRE(dec.isOk());
        CHECK(dec.value().applied[0].value.bool_val == true);
    }
    SUBCASE("EVENT") {
        EventMsg m{};
        m.channel_id = 1;
        m.timestamp = 1;
        m.event_kind = 1;
        std::array<std::byte, 64> buf{};
        size_t n = encodeEvent(m, buf);
        auto spliced = spliceUnknownKey(std::span<const std::byte>(buf.data(), n));
        auto dec = decodeEvent(spliced);
        REQUIRE(dec.isOk());
        CHECK(dec.value().event_kind == 1);
    }
    SUBCASE("NACK") {
        NackMsg m{};
        m.code = NackCode::MALFORMED;
        std::array<std::byte, 16> buf{};
        size_t n = encodeNack(m, buf);
        auto spliced = spliceUnknownKey(std::span<const std::byte>(buf.data(), n));
        auto dec = decodeNack(spliced);
        REQUIRE(dec.isOk());
        CHECK(dec.value().code == NackCode::MALFORMED);
    }
    SUBCASE("GOODBYE") {
        GoodbyeMsg m{};
        m.code = NackCode::SESSION_EVICTED;
        std::array<std::byte, 16> buf{};
        size_t n = encodeGoodbye(m, buf);
        auto spliced = spliceUnknownKey(std::span<const std::byte>(buf.data(), n));
        auto dec = decodeGoodbye(spliced);
        REQUIRE(dec.isOk());
        CHECK(dec.value().code == NackCode::SESSION_EVICTED);
    }
    SUBCASE("PAIR_REQ") {
        PairReqMsg m{};
        std::array<std::byte, 64> buf{};
        size_t n = encodePairReq(m, buf);
        auto spliced = spliceUnknownKey(std::span<const std::byte>(buf.data(), n));
        auto dec = decodePairReq(spliced);
        REQUIRE(dec.isOk());
    }
    SUBCASE("PAIR_GRANT") {
        PairGrantMsg m{};
        m.roles = 2;
        std::array<std::byte, 64> buf{};
        size_t n = encodePairGrant(m, buf);
        auto spliced = spliceUnknownKey(std::span<const std::byte>(buf.data(), n));
        auto dec = decodePairGrant(spliced);
        REQUIRE(dec.isOk());
        CHECK(dec.value().roles == 2);
    }
    SUBCASE("PROBE_REPORT") {
        ProbeReportMsg m{};
        m.probe_result.rtt_ms = 5;
        std::array<std::byte, 32> buf{};
        size_t n = encodeProbeReport(m, buf);
        auto spliced = spliceUnknownKey(std::span<const std::byte>(buf.data(), n));
        auto dec = decodeProbeReport(spliced);
        REQUIRE(dec.isOk());
        CHECK(dec.value().probe_result.rtt_ms == 5);
    }
}
