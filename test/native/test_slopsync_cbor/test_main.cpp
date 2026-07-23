// ============================================================================
// test_main.cpp — doctest unit tests for the slopsync-core deterministic
// CBOR profile codec (cbor_writer.hpp / cbor_reader.hpp) and the HELLO /
// WELCOME message codecs (wire/messages/hello.hpp, welcome.hpp).
//
// This is a native (host-side, hardware-free) test, same pattern as
// test/native/test_motion_profile/test_main.cpp: doctest's bundled main(),
// no Arduino, no bus/FreeRTOS dependency — the library is header-only and
// entirely math/logic.
//
// Implements golden vectors C-01..C-06 from
// docs/slopsync/vectors/manifest.yaml suite `cbor_profile`, using the
// manifest's frozen fixtures:
//   session_id  = 0x5E551D01
//   boot_id     = 0xB007CAFE
//   instance_id = 01 02 03 04 05 06 07 08
//   token       = 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F
//   nonce       = A1 A2 A3 A4 A5 A6 A7 A8
// `catalog_etag` has no fixed fixture value in the manifest (that's suite
// K-01/K-02's job, computed from fixtures/mini-catalog.yaml — out of scope
// for this codec); this file uses an arbitrary placeholder etag
// (11 22 33 44 55 66 77 88), clearly marked, purely to exercise the field.
//
// Every "expected bytes" array below is HAND-DERIVED per RFC 8949 / SPEC
// §5.3 (definite lengths, shortest-form integer heads, sorted ascending
// integer map keys, binary32-only floats, no tags/indefinite/other-simples)
// — see the per-byte comments. These byte arrays ARE the golden vectors;
// the point of writing them by hand instead of just trusting the encoder's
// own output is that a bug shared between encoder and "expected" would
// otherwise go undetected.
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/welcome.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

using namespace slopsync;

namespace {

// Shorthand for building expected-byte arrays as std::byte literals.
constexpr std::byte B(int x) { return std::byte(uint8_t(x)); }

template <size_t N>
bool bytesEqual(std::span<const std::byte> got, const std::byte (&expected)[N]) {
    if (got.size() != N) return false;
    for (size_t i = 0; i < N; ++i) {
        if (got[i] != expected[i]) return false;
    }
    return true;
}

// Manifest fixtures (docs/slopsync/vectors/manifest.yaml).
constexpr std::array<std::byte, 8> kInstanceId = {
    B(0x01), B(0x02), B(0x03), B(0x04), B(0x05), B(0x06), B(0x07), B(0x08)};
constexpr std::array<std::byte, 16> kToken = {
    B(0x00), B(0x01), B(0x02), B(0x03), B(0x04), B(0x05), B(0x06), B(0x07),
    B(0x08), B(0x09), B(0x0A), B(0x0B), B(0x0C), B(0x0D), B(0x0E), B(0x0F)};
constexpr std::array<std::byte, 8> kNonce = {
    B(0xA1), B(0xA2), B(0xA3), B(0xA4), B(0xA5), B(0xA6), B(0xA7), B(0xA8)};
constexpr uint32_t kSessionId = 0x5E551D01u;
constexpr uint32_t kBootId = 0xB007CAFEu;

// No manifest fixture exists for catalog_etag (that's suite K-01/K-02,
// computed from fixtures/mini-catalog.yaml — a different codec entirely).
// Arbitrary placeholder, used only to exercise the field's 8 bytes.
constexpr std::array<std::byte, 8> kPlaceholderEtag = {
    B(0x11), B(0x22), B(0x33), B(0x44), B(0x55), B(0x66), B(0x77), B(0x88)};

}  // namespace

// ============================================================================
// C-01 — HELLO minimal (viewer, no token): exact bytes, round-trip.
// ============================================================================
TEST_CASE("C-01: HELLO minimal (viewer, no token) — exact bytes + round-trip") {
    HelloMsg m{};
    m.proto_ver = 1;
    m.client_kind = "webui";   // 5 bytes
    m.client_name = "vector";  // 6 bytes
    m.instance_id = kInstanceId;
    // has_token / has_catalog_etag left false; subscriptions/publishes empty
    // -> all four are OMITTED from the map (SPEC §6.2 + this codec's brief).

    std::array<std::byte, 64> buf{};
    size_t n = encodeHello(m, buf);

    // Hand-derived bytes. Map has exactly 4 pairs (proto_ver, client_kind,
    // client_name, instance_id) since nothing else is present:
    //   A4                      map(4 pairs)
    //   01 01                   key=1 (proto_ver), val=1 (uint, shortest form)
    //   02 65 77 65 62 75 69    key=2 (client_kind), val=tstr(5)="webui"
    //   03 66 76 65 63 74 6F 72 key=3 (client_name), val=tstr(6)="vector"
    //   04 48 01..08            key=4 (instance_id), val=bstr(8)=fixture id
    static constexpr std::byte kExpected[] = {
        B(0xA4),
        B(0x01), B(0x01),
        B(0x02), B(0x65), B('w'), B('e'), B('b'), B('u'), B('i'),
        B(0x03), B(0x66), B('v'), B('e'), B('c'), B('t'), B('o'), B('r'),
        B(0x04), B(0x48), B(0x01), B(0x02), B(0x03), B(0x04), B(0x05), B(0x06), B(0x07), B(0x08),
    };
    static_assert(sizeof(kExpected) == 28, "hand count must match derivation");

    REQUIRE(n == sizeof(kExpected));
    CHECK(bytesEqual(std::span<const std::byte>(buf.data(), n), kExpected));

    auto dec = decodeHello(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    CHECK(dec.value().proto_ver == 1);
    CHECK(dec.value().client_kind == "webui");
    CHECK(dec.value().client_name == "vector");
    CHECK(std::memcmp(dec.value().instance_id.data(), kInstanceId.data(), 8) == 0);
    CHECK_FALSE(dec.value().has_token);
    CHECK_FALSE(dec.value().has_catalog_etag);
    CHECK(dec.value().subscriptions_count == 0);
    CHECK(dec.value().publishes_count == 0);
}

// ============================================================================
// C-02 — HELLO full (token, etag, wish-lists): exact bytes, sorted keys,
// round-trip.
// ============================================================================
TEST_CASE("C-02: HELLO full (token, etag, wish-lists) — exact bytes + round-trip") {
    HelloMsg m{};
    m.proto_ver = 1;
    m.client_kind = "webui";
    m.client_name = "vector";
    m.instance_id = kInstanceId;
    m.has_token = true;
    m.token = kToken;
    m.has_catalog_etag = true;
    m.catalog_etag = kPlaceholderEtag;
    m.subscriptions_count = 1;
    m.subscriptions[0] = SubscriptionWish{/*channel_id=*/80, /*rate_hz=*/240.0f, /*priority=*/2};
    m.publishes_count = 1;
    m.publishes[0] = PublishWish{/*channel_id=*/81, /*rate_hz=*/60.0f};

    std::array<std::byte, 128> buf{};
    size_t n = encodeHello(m, buf);

    // Map has 8 pairs, keys ascending 1,2,3,4,5,8,10,11.
    //
    // Wish-entry keys are ALSO sorted ascending by their own numbers, which
    // is NOT the order the fields are listed in prose (SPEC §6.2 says
    // "{channel_id, rate_hz, priority}") — the wire order is rate_hz(12) <
    // priority(13) < channel_id(15) because §5.3 requires ascending encoded
    // key bytes, full stop, regardless of documentation order.
    //
    // float32 constants used (binary32, big-endian on the wire):
    //   240.0f = 0x43700000   (1.875 * 2^7:  sign0 exp10000110 mant1110...0)
    //    60.0f = 0x42700000   (1.875 * 2^5:  same mantissa, exponent -2)
    static constexpr std::byte kExpected[] = {
        B(0xA8),  // map(8 pairs)
        B(0x01), B(0x01),                                            // proto_ver=1
        B(0x02), B(0x65), B('w'), B('e'), B('b'), B('u'), B('i'),     // client_kind="webui"
        B(0x03), B(0x66), B('v'), B('e'), B('c'), B('t'), B('o'), B('r'),  // client_name="vector"
        B(0x04), B(0x48), B(0x01), B(0x02), B(0x03), B(0x04), B(0x05), B(0x06), B(0x07), B(0x08),  // instance_id
        // token: len16 < 24 -> shortest form is a single-byte head (0x40|16=0x50)
        B(0x05), B(0x50),
        B(0x00), B(0x01), B(0x02), B(0x03), B(0x04), B(0x05), B(0x06), B(0x07),
        B(0x08), B(0x09), B(0x0A), B(0x0B), B(0x0C), B(0x0D), B(0x0E), B(0x0F),
        // catalog_etag: len8 -> single-byte head 0x40|8=0x48
        B(0x08), B(0x48), B(0x11), B(0x22), B(0x33), B(0x44), B(0x55), B(0x66), B(0x77), B(0x88),
        // subscriptions: array(1) of {rate_hz:12, priority:13, channel_id:15}
        B(0x0A), B(0x81),
        B(0xA3),                                     // wish map(3 pairs)
        B(0x0C), B(0xFA), B(0x43), B(0x70), B(0x00), B(0x00),  // rate_hz=240.0f
        B(0x0D), B(0x02),                             // priority=2
        B(0x0F), B(0x18), B(0x50),                    // channel_id=80 (>=24 -> 1-byte follow)
        // publishes: array(1) of {rate_hz:12, channel_id:15}
        B(0x0B), B(0x81),
        B(0xA2),                                     // wish map(2 pairs)
        B(0x0C), B(0xFA), B(0x42), B(0x70), B(0x00), B(0x00),  // rate_hz=60.0f
        B(0x0F), B(0x18), B(0x51),                    // channel_id=81
    };
    static_assert(sizeof(kExpected) == 82, "hand count must match derivation");

    REQUIRE(n == sizeof(kExpected));
    CHECK(bytesEqual(std::span<const std::byte>(buf.data(), n), kExpected));

    auto dec = decodeHello(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    const HelloMsg& d = dec.value();
    CHECK(d.proto_ver == 1);
    CHECK(d.client_kind == "webui");
    CHECK(d.client_name == "vector");
    CHECK(std::memcmp(d.instance_id.data(), kInstanceId.data(), 8) == 0);
    REQUIRE(d.has_token);
    CHECK(std::memcmp(d.token.data(), kToken.data(), 16) == 0);
    REQUIRE(d.has_catalog_etag);
    CHECK(std::memcmp(d.catalog_etag.data(), kPlaceholderEtag.data(), 8) == 0);
    REQUIRE(d.subscriptions_count == 1);
    CHECK(d.subscriptions[0].channel_id == 80);
    CHECK(d.subscriptions[0].rate_hz == doctest::Approx(240.0f));
    CHECK(d.subscriptions[0].priority == 2);
    REQUIRE(d.publishes_count == 1);
    CHECK(d.publishes[0].channel_id == 81);
    CHECK(d.publishes[0].rate_hz == doctest::Approx(60.0f));
}

// ============================================================================
// C-03 — WELCOME with grants/limits/nonce: exact bytes, round-trip.
// ============================================================================
TEST_CASE("C-03: WELCOME with grants/limits/nonce — exact bytes + round-trip") {
    WelcomeMsg m{};
    m.proto_ver = 1;
    m.session_id = kSessionId;
    m.boot_id = kBootId;
    m.catalog_etag = kPlaceholderEtag;
    m.cfg_gen = 5;
    m.limits_info = WelcomeLimits{/*max_frame=*/242, /*max_subscriptions=*/8, /*retained_pending=*/3};
    m.roles = 1;               // controller
    m.deadman_ms = 600;
    m.deadman_policy = 0;      // stop(decel)
    m.nonce = kNonce;
    m.grants_count = 1;
    m.grants[0] = Grant{/*channel_id=*/80, /*granted_rate_hz=*/240.0f, /*priority=*/2};

    std::array<std::byte, 128> buf{};
    size_t n = encodeWelcome(m, buf);

    // Map has 11 pairs, keys ascending 1,6,7,8,9,22,23,24,25,29,35(grants —
    // registry CborKey 35, allocated after implementation surfaced the gap).
    //
    // session_id (0x5E551D01) and boot_id (0xB007CAFE) both exceed 0xFFFF,
    // so both need the 4-byte-follow uint form (major0, ai=26 -> head 0x1A).
    // deadman_ms=600 exceeds 0xFF, needs the 2-byte-follow form (ai=25 ->
    // head 0x19); every key/value >23 and <=255 (24,25,29,35,242,80) needs
    // the 1-byte-follow form (ai=24 -> head 0x18).
    static constexpr std::byte kExpected[] = {
        B(0xAB),  // map(11 pairs)
        B(0x01), B(0x01),                                              // proto_ver=1
        B(0x06), B(0x1A), B(0x5E), B(0x55), B(0x1D), B(0x01),          // session_id=0x5E551D01
        B(0x07), B(0x1A), B(0xB0), B(0x07), B(0xCA), B(0xFE),          // boot_id=0xB007CAFE
        B(0x08), B(0x48), B(0x11), B(0x22), B(0x33), B(0x44), B(0x55), B(0x66), B(0x77), B(0x88),  // catalog_etag
        B(0x09), B(0x05),                                              // cfg_gen=5
        // limits (key=22, single byte since <24): nested map(3 pairs),
        // registry welcome_limits sub-keys 1=max_frame,2=max_subscriptions,3=retained_pending
        B(0x16), B(0xA3),
        B(0x01), B(0x18), B(0xF2),   // max_frame=242 (>=24 -> 1-byte follow)
        B(0x02), B(0x08),            // max_subscriptions=8
        B(0x03), B(0x03),            // retained_pending=3
        B(0x17), B(0x01),            // roles=1 (key 23, single byte)
        B(0x18), B(0x18), B(0x19), B(0x02), B(0x58),  // deadman_ms: key=24(1-byte follow) val=600(2-byte follow, BE 0x0258)
        B(0x18), B(0x19), B(0x00),                    // deadman_policy: key=25(1-byte follow) val=0
        B(0x18), B(0x1D), B(0x48),                    // nonce: key=29(1-byte follow), bstr(8) head
        B(0xA1), B(0xA2), B(0xA3), B(0xA4), B(0xA5), B(0xA6), B(0xA7), B(0xA8),
        // grants: key=35 (CborKey::grants, 1-byte follow), array(1) of
        // {priority:13, granted_rate_hz:14, channel_id:15} ascending
        B(0x18), B(0x23), B(0x81),
        B(0xA3),
        B(0x0D), B(0x02),                                       // priority=2
        B(0x0E), B(0xFA), B(0x43), B(0x70), B(0x00), B(0x00),   // granted_rate_hz=240.0f
        B(0x0F), B(0x18), B(0x50),                              // channel_id=80
    };
    static_assert(sizeof(kExpected) == 72, "hand count must match derivation");

    REQUIRE(n == sizeof(kExpected));
    CHECK(bytesEqual(std::span<const std::byte>(buf.data(), n), kExpected));

    auto dec = decodeWelcome(std::span<const std::byte>(buf.data(), n));
    REQUIRE(dec.isOk());
    const WelcomeMsg& d = dec.value();
    CHECK(d.proto_ver == 1);
    CHECK(d.session_id == kSessionId);
    CHECK(d.boot_id == kBootId);
    CHECK(std::memcmp(d.catalog_etag.data(), kPlaceholderEtag.data(), 8) == 0);
    CHECK(d.cfg_gen == 5);
    CHECK(d.limits_info.max_frame == 242);
    CHECK(d.limits_info.max_subscriptions == 8);
    CHECK(d.limits_info.retained_pending == 3);
    CHECK(d.roles == 1);
    CHECK(d.deadman_ms == 600);
    CHECK(d.deadman_policy == 0);
    CHECK(std::memcmp(d.nonce.data(), kNonce.data(), 8) == 0);
    REQUIRE(d.grants_count == 1);
    CHECK(d.grants[0].channel_id == 80);
    CHECK(d.grants[0].granted_rate_hz == doctest::Approx(240.0f));
    CHECK(d.grants[0].priority == 2);
}

// ============================================================================
// C-04 — profile violations rejected (decode direction). Each hand-crafted
// fragment is fed directly to CborReader (not a full HELLO/WELCOME) — these
// probe the profile-enforcement machinery in isolation.
// ============================================================================
TEST_CASE("C-04: profile violations are rejected") {
    SUBCASE("indefinite-length map (0xBF)") {
        static constexpr std::byte buf[] = {B(0xBF)};
        CborReader r(buf);
        auto res = r.readMapHeader();
        REQUIRE_FALSE(res.isOk());
        CHECK(res.error() == DecodeError::ProfileViolation);
    }
    SUBCASE("unsorted keys (second key < first)") {
        // map(2 pairs): key=2 val=1(uint), key=1 val=1(uint) -- 1 <= 2, rejected
        static constexpr std::byte buf[] = {B(0xA2), B(0x02), B(0x01), B(0x01), B(0x01)};
        CborReader r(buf);
        auto n = r.readMapHeader();
        REQUIRE(n.isOk());
        auto k1 = r.readKey();
        REQUIRE(k1.isOk());
        CHECK(k1.value() == 2);
        auto v1 = r.readUint();
        REQUIRE(v1.isOk());
        auto k2 = r.readKey();
        REQUIRE_FALSE(k2.isOk());
        CHECK(k2.error() == DecodeError::ProfileViolation);
    }
    SUBCASE("duplicate keys (second key == first)") {
        // map(2 pairs): key=1 val=1, key=1 (dup) val=1 -- 1 <= 1, rejected
        static constexpr std::byte buf[] = {B(0xA2), B(0x01), B(0x01), B(0x01), B(0x01)};
        CborReader r(buf);
        auto n = r.readMapHeader();
        REQUIRE(n.isOk());
        auto k1 = r.readKey();
        REQUIRE(k1.isOk());
        auto v1 = r.readUint();
        REQUIRE(v1.isOk());
        auto k2 = r.readKey();
        REQUIRE_FALSE(k2.isOk());
        CHECK(k2.error() == DecodeError::ProfileViolation);
    }
    SUBCASE("float64 (0xFB + 8 payload bytes)") {
        static constexpr std::byte buf[] = {
            B(0xFB), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00)};
        CborReader r(buf);
        auto res = r.readF32();
        REQUIRE_FALSE(res.isOk());
        CHECK(res.error() == DecodeError::ProfileViolation);
    }
    SUBCASE("tagged value (0xC0 = tag 0)") {
        static constexpr std::byte buf[] = {B(0xC0), B(0x01)};
        CborReader r(buf);
        auto res = r.readUint();
        REQUIRE_FALSE(res.isOk());
        CHECK(res.error() == DecodeError::ProfileViolation);
    }
    SUBCASE("non-shortest uint (0x18 0x05 encodes 5, which fits in the head byte)") {
        static constexpr std::byte buf[] = {B(0x18), B(0x05)};
        CborReader r(buf);
        auto res = r.readUint();
        REQUIRE_FALSE(res.isOk());
        CHECK(res.error() == DecodeError::ProfileViolation);
    }
}

// ============================================================================
// C-05 — unknown map key ignored (SPEC §4.3). A valid deterministic HELLO
// map with an extra key 99 (ascending, after instance_id=4) whose value is a
// nested 2-element array must decode fine, with 99 silently skipped.
// ============================================================================
TEST_CASE("C-05: unknown key (99) with nested array value is skipped") {
    // map(5 pairs): 1,2,3,4 (the required HELLO fields) then 99 -> [7,8]
    static constexpr std::byte buf[] = {
        B(0xA5),
        B(0x01), B(0x01),
        B(0x02), B(0x65), B('w'), B('e'), B('b'), B('u'), B('i'),
        B(0x03), B(0x66), B('v'), B('e'), B('c'), B('t'), B('o'), B('r'),
        B(0x04), B(0x48), B(0x01), B(0x02), B(0x03), B(0x04), B(0x05), B(0x06), B(0x07), B(0x08),
        // key=99 (>=24,<=255 -> 1-byte follow), value = array(2) [7, 8]
        B(0x18), B(0x63), B(0x82), B(0x07), B(0x08),
    };
    auto dec = decodeHello(std::span<const std::byte>(buf, sizeof(buf)));
    REQUIRE(dec.isOk());
    const HelloMsg& d = dec.value();
    CHECK(d.proto_ver == 1);
    CHECK(d.client_kind == "webui");
    CHECK(d.client_name == "vector");
    CHECK(std::memcmp(d.instance_id.data(), kInstanceId.data(), 8) == 0);
    CHECK_FALSE(d.has_token);
    CHECK(d.subscriptions_count == 0);
    CHECK(d.publishes_count == 0);
}

// ============================================================================
// C-06 — canned-template equivalence: patching C-01's instance_id bytes in
// place equals a fresh encode with that id (proves template-patching, §5.3
// / §8.5's whole reason for existing, is legal for this message shape).
// ============================================================================
TEST_CASE("C-06: template-patched instance_id equals a fresh encode") {
    HelloMsg m{};
    m.proto_ver = 1;
    m.client_kind = "webui";
    m.client_name = "vector";
    m.instance_id = kInstanceId;

    std::array<std::byte, 64> templateBuf{};
    size_t templateLen = encodeHello(m, templateBuf);
    REQUIRE(templateLen == 28);

    // instance_id's 8 payload bytes are the last 8 bytes of the C-01 layout
    // (map hdr + 2 + 7 + 8 = byte 20 through 27; see C-01's derivation).
    const size_t idOffset = 20;
    constexpr std::array<std::byte, 8> kNewId = {
        B(0xAA), B(0xBB), B(0xCC), B(0xDD), B(0xEE), B(0xFF), B(0x11), B(0x22)};

    std::array<std::byte, 64> patchedBuf = templateBuf;
    std::memcpy(patchedBuf.data() + idOffset, kNewId.data(), kNewId.size());

    HelloMsg m2 = m;
    m2.instance_id = kNewId;
    std::array<std::byte, 64> freshBuf{};
    size_t freshLen = encodeHello(m2, freshBuf);

    REQUIRE(freshLen == templateLen);
    CHECK(std::memcmp(patchedBuf.data(), freshBuf.data(), freshLen) == 0);
}

// ============================================================================
// Shortest-form integer boundary tests (23/24, 255/256, 65535/65536).
// ============================================================================
TEST_CASE("Shortest-form integer boundaries: writer produces the minimal head") {
    auto encodeOne = [](uint64_t v) {
        std::array<std::byte, 16> buf{};
        CborWriter w(buf);
        w.uintVal(v);
        return std::pair<std::array<std::byte, 16>, size_t>{buf, w.size()};
    };

    {
        auto [buf, n] = encodeOne(23);
        static constexpr std::byte exp[] = {B(0x17)};
        REQUIRE(n == 1);
        CHECK(bytesEqual(std::span<const std::byte>(buf.data(), n), exp));
    }
    {
        auto [buf, n] = encodeOne(24);
        static constexpr std::byte exp[] = {B(0x18), B(0x18)};
        REQUIRE(n == 2);
        CHECK(bytesEqual(std::span<const std::byte>(buf.data(), n), exp));
    }
    {
        auto [buf, n] = encodeOne(255);
        static constexpr std::byte exp[] = {B(0x18), B(0xFF)};
        REQUIRE(n == 2);
        CHECK(bytesEqual(std::span<const std::byte>(buf.data(), n), exp));
    }
    {
        auto [buf, n] = encodeOne(256);
        static constexpr std::byte exp[] = {B(0x19), B(0x01), B(0x00)};
        REQUIRE(n == 3);
        CHECK(bytesEqual(std::span<const std::byte>(buf.data(), n), exp));
    }
    {
        auto [buf, n] = encodeOne(65535);
        static constexpr std::byte exp[] = {B(0x19), B(0xFF), B(0xFF)};
        REQUIRE(n == 3);
        CHECK(bytesEqual(std::span<const std::byte>(buf.data(), n), exp));
    }
    {
        auto [buf, n] = encodeOne(65536);
        static constexpr std::byte exp[] = {B(0x1A), B(0x00), B(0x01), B(0x00), B(0x00)};
        REQUIRE(n == 5);
        CHECK(bytesEqual(std::span<const std::byte>(buf.data(), n), exp));
    }
}

TEST_CASE("Shortest-form integer boundaries: reader rejects non-shortest encodings") {
    SUBCASE("23 encoded via 1-byte-follow instead of the direct single byte") {
        static constexpr std::byte buf[] = {B(0x18), B(0x17)};
        CborReader r(buf);
        auto res = r.readUint();
        REQUIRE_FALSE(res.isOk());
        CHECK(res.error() == DecodeError::ProfileViolation);
    }
    SUBCASE("255 encoded via 2-byte-follow instead of 1-byte-follow") {
        static constexpr std::byte buf[] = {B(0x19), B(0x00), B(0xFF)};
        CborReader r(buf);
        auto res = r.readUint();
        REQUIRE_FALSE(res.isOk());
        CHECK(res.error() == DecodeError::ProfileViolation);
    }
    SUBCASE("65535 encoded via 4-byte-follow instead of 2-byte-follow") {
        static constexpr std::byte buf[] = {B(0x1A), B(0x00), B(0x00), B(0xFF), B(0xFF)};
        CborReader r(buf);
        auto res = r.readUint();
        REQUIRE_FALSE(res.isOk());
        CHECK(res.error() == DecodeError::ProfileViolation);
    }
    SUBCASE("round-trip at each boundary still works when shortest-form") {
        for (uint64_t v : {0ull, 23ull, 24ull, 255ull, 256ull, 65535ull, 65536ull, 4294967295ull, 4294967296ull}) {
            std::array<std::byte, 16> buf{};
            CborWriter w(buf);
            w.uintVal(v);
            REQUIRE(w.size() > 0);
            CborReader r(std::span<const std::byte>(buf.data(), w.size()));
            auto res = r.readUint();
            REQUIRE(res.isOk());
            CHECK(res.value() == v);
        }
    }
}

// ============================================================================
// float32 encode of 1.5 and -0.0 (binary32, big-endian payload).
// ============================================================================
TEST_CASE("float32: 1.5 and -0.0 encode to the exact IEEE-754 big-endian bytes") {
    {
        // 1.5 = 1.1(binary) * 2^0 -> sign0 exp01111111 mant1000...0 = 0x3FC00000
        std::array<std::byte, 8> buf{};
        CborWriter w(buf);
        w.f32Val(1.5f);
        static constexpr std::byte exp[] = {B(0xFA), B(0x3F), B(0xC0), B(0x00), B(0x00)};
        REQUIRE(w.size() == 5);
        CHECK(bytesEqual(std::span<const std::byte>(buf.data(), w.size()), exp));

        CborReader r(std::span<const std::byte>(buf.data(), w.size()));
        auto res = r.readF32();
        REQUIRE(res.isOk());
        CHECK(res.value() == doctest::Approx(1.5f));
    }
    {
        // -0.0f: sign bit set, exponent and mantissa all zero -> 0x80000000.
        // Bit-compared (not `==`, since -0.0f == 0.0f under IEEE equality —
        // the whole point of this vector is that the SIGN BIT survives).
        float negZero;
        uint32_t bits = 0x80000000u;
        std::memcpy(&negZero, &bits, 4);

        std::array<std::byte, 8> buf{};
        CborWriter w(buf);
        w.f32Val(negZero);
        static constexpr std::byte exp[] = {B(0xFA), B(0x80), B(0x00), B(0x00), B(0x00)};
        REQUIRE(w.size() == 5);
        CHECK(bytesEqual(std::span<const std::byte>(buf.data(), w.size()), exp));

        CborReader r(std::span<const std::byte>(buf.data(), w.size()));
        auto res = r.readF32();
        REQUIRE(res.isOk());
        uint32_t gotBits;
        float gotVal = res.value();
        std::memcpy(&gotBits, &gotVal, 4);
        CHECK(gotBits == 0x80000000u);
    }
}

// ============================================================================
// Nesting depth: depth-4 accepted, depth-5 rejected. Top-level container is
// depth 1; opening a 5th nested container must fail on both writer and
// reader. These are structural probes of the depth ceiling in isolation —
// they don't produce a decodable message (each open has no matching keys),
// which is fine: only the depth bookkeeping is under test here.
// ============================================================================
TEST_CASE("Nesting depth 4 is accepted, depth 5 is rejected (writer)") {
    std::array<std::byte, 32> buf{};
    CborWriter w(buf);
    CHECK_FALSE(w.mapHeader(1).failed());    // depth 1
    CHECK_FALSE(w.mapHeader(1).failed());    // depth 2
    CHECK_FALSE(w.mapHeader(1).failed());    // depth 3
    CHECK_FALSE(w.mapHeader(1).failed());    // depth 4 (== kMaxDepth, still OK)
    CHECK(w.mapHeader(1).failed());          // depth 5 -> rejected
    CHECK(w.size() == 0);                    // sticky failure
}

TEST_CASE("Nesting depth 4 is accepted, depth 5 is rejected (reader)") {
    // Five map(1) heads back-to-back: 0xA1 encodes major5|1 each time.
    static constexpr std::byte buf[] = {B(0xA1), B(0xA1), B(0xA1), B(0xA1), B(0xA1)};
    CborReader r(buf);
    REQUIRE(r.readMapHeader().isOk());   // depth 1
    REQUIRE(r.readMapHeader().isOk());   // depth 2
    REQUIRE(r.readMapHeader().isOk());   // depth 3
    REQUIRE(r.readMapHeader().isOk());   // depth 4
    auto fifth = r.readMapHeader();      // depth 5 -> rejected
    REQUIRE_FALSE(fifth.isOk());
    CHECK(fifth.error() == DecodeError::DepthExceeded);
}

// ============================================================================
// tstr/bstr length boundaries (23/24 byte lengths): 23 fits the direct
// single-byte head; 24 needs the 1-byte-follow form.
// ============================================================================
TEST_CASE("tstr/bstr length boundary: 23 vs 24 bytes") {
    const std::string_view s23(
        "01234567890123456789012");  // 23 chars
    const std::string_view s24(
        "012345678901234567890123");  // 24 chars
    REQUIRE(s23.size() == 23);
    REQUIRE(s24.size() == 24);

    {
        std::array<std::byte, 32> buf{};
        CborWriter w(buf);
        w.tstrVal(s23);
        REQUIRE(w.size() == 1 + 23);
        CHECK(uint8_t(buf[0]) == (0x60 | 23));  // major3 | direct length
    }
    {
        std::array<std::byte, 32> buf{};
        CborWriter w(buf);
        w.tstrVal(s24);
        REQUIRE(w.size() == 2 + 24);
        CHECK(uint8_t(buf[0]) == (0x60 | 24));  // major3 | ai=24 (1-byte follow)
        CHECK(uint8_t(buf[1]) == 24);
    }
    {
        std::array<std::byte, 23> data23{};
        for (size_t i = 0; i < 23; ++i) data23[i] = std::byte(uint8_t(i));
        std::array<std::byte, 32> buf{};
        CborWriter w(buf);
        w.bstrVal(std::span<const std::byte>(data23));
        REQUIRE(w.size() == 1 + 23);
        CHECK(uint8_t(buf[0]) == (0x40 | 23));  // major2 | direct length
    }
    {
        std::array<std::byte, 24> data24{};
        for (size_t i = 0; i < 24; ++i) data24[i] = std::byte(uint8_t(i));
        std::array<std::byte, 32> buf{};
        CborWriter w(buf);
        w.bstrVal(std::span<const std::byte>(data24));
        REQUIRE(w.size() == 2 + 24);
        CHECK(uint8_t(buf[0]) == (0x40 | 24));  // major2 | ai=24 (1-byte follow)
        CHECK(uint8_t(buf[1]) == 24);
    }
}

// ============================================================================
// Bonus coverage (not in the manifest, but part of the writer/reader API
// surface the brief asked for): bool/null round trip, and overflow /
// out-of-order-key detection on the writer.
// ============================================================================
TEST_CASE("bool and null round-trip") {
    std::array<std::byte, 8> buf{};
    CborWriter w(buf);
    w.boolVal(true).boolVal(false).nullVal();
    REQUIRE(w.size() == 3);
    CHECK(uint8_t(buf[0]) == 0xF5);
    CHECK(uint8_t(buf[1]) == 0xF4);
    CHECK(uint8_t(buf[2]) == 0xF6);

    CborReader r(std::span<const std::byte>(buf.data(), w.size()));
    auto a = r.readBool();
    REQUIRE(a.isOk());
    CHECK(a.value() == true);
    auto b = r.readBool();
    REQUIRE(b.isOk());
    CHECK(b.value() == false);
    auto c = r.readNull();
    CHECK(c.isOk());
}

TEST_CASE("Writer catches an out-of-order key and fails sticky") {
    std::array<std::byte, 32> buf{};
    CborWriter w(buf);
    w.mapHeader(2);
    w.key(CborKey::client_name).uintVal(1);   // key 3
    w.key(CborKey::client_kind).uintVal(1);   // key 2 <= 3 -> caught
    CHECK(w.failed());
    CHECK(w.size() == 0);
}

TEST_CASE("Writer sets failed on output-span overflow") {
    std::array<std::byte, 2> tiny{};
    CborWriter w(tiny);
    w.mapHeader(4);  // needs far more than 2 bytes once real pairs are written
    w.key(CborKey::proto_ver).uintVal(1);
    w.key(CborKey::client_kind).tstrVal("this string will not fit");
    CHECK(w.failed());
    CHECK(w.size() == 0);
}
