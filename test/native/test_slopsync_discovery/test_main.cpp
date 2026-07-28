// ============================================================================
// test_main.cpp — doctest unit tests for SlopSyncDiscoveryWire.h (Phase E:
// the radios). Pure byte encode/decode, no Arduino/NimBLE/socket — this is
// exactly the host-testable slice of the UDP discovery probe/reply (SPEC
// §13.8) and the BLE-advertising flags byte (§13.4/§13.6, `ble_adv_flags`).
// The AsyncUDP and NimBLE glue that call into this header are hardware-only
// and are LIVE-VERIFY scope, not unit-tested here (see the Phase E report).
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "SlopSyncDiscoveryWire.h"

#include <array>
#include <cstring>

using namespace slopdrive::discovery;

namespace {
constexpr std::byte B(int x) { return std::byte(uint8_t(x)); }
}  // namespace

// ============================================================================
// buildFlags — the byte shared between BLE advertising and DISCOVER_REPLY.
// ============================================================================
TEST_CASE("buildFlags: every bit combination, and bits 2-7 are structurally unreachable") {
    CHECK(buildFlags(false, false) == 0x00);
    CHECK(buildFlags(true, false) == 0x01);
    CHECK(buildFlags(false, true) == 0x02);
    CHECK(buildFlags(true, true) == 0x03);
}

// ============================================================================
// DISCOVER_PROBE (0x1E) parsing
// ============================================================================
TEST_CASE("parseProbe: a well-formed probe round-trips proto_ver and nonce") {
    std::array<std::byte, kProbeBytes> buf{B('S'), B('L'), B('O'), B('P'), B(1),
                                            B(0xEF), B(0xBE), B(0xAD), B(0xDE)};  // nonce=0xDEADBEEF LE
    auto p = parseProbe(buf);
    REQUIRE(p.has_value());
    CHECK(p->proto_ver == 1);
    CHECK(p->nonce == 0xDEADBEEFu);
}

TEST_CASE("parseProbe: bad magic is rejected silently (nullopt, not a crash)") {
    std::array<std::byte, kProbeBytes> buf{B('X'), B('L'), B('O'), B('P'), B(1), B(0), B(0), B(0), B(0)};
    CHECK_FALSE(parseProbe(buf).has_value());
}

TEST_CASE("parseProbe: wrong length is rejected (too short and too long)") {
    std::array<std::byte, kProbeBytes - 1> shortBuf{};
    CHECK_FALSE(parseProbe(shortBuf).has_value());

    std::array<std::byte, kProbeBytes + 1> longBuf{};
    longBuf[0] = B('S'); longBuf[1] = B('L'); longBuf[2] = B('O'); longBuf[3] = B('P');
    CHECK_FALSE(parseProbe(longBuf).has_value());
}

// ============================================================================
// DISCOVER_REPLY (0x1F) building — RFC-048's 76-byte layout
// ============================================================================
TEST_CASE("buildReply: exact byte layout for a fully-populated reply") {
    ReplyFields f;
    f.nonce = 0xDEADBEEFu;
    f.hub_name = "slopdrive-32";
    f.hub_instance_id = 0x0123456789ABCDEFull;
    f.proto_ver = 1;
    f.ws_port = 82;
    f.fw_version = "2.2.0";
    f.catalog_etag = {std::byte(0xAA), std::byte(0xBB), std::byte(0xCC), std::byte(0xDD),
                       std::byte(0xEE), std::byte(0xFF), std::byte(0x00), std::byte(0x11)};
    f.flags = buildFlags(true, true);

    std::array<std::byte, kReplyBytes> buf{};
    size_t n = buildReply(f, buf);
    REQUIRE(n == kReplyBytes);
    REQUIRE(n == 76);

    // magic
    CHECK(uint8_t(buf[0]) == 'S');
    CHECK(uint8_t(buf[1]) == 'L');
    CHECK(uint8_t(buf[2]) == 'O');
    CHECK(uint8_t(buf[3]) == 'P');
    // nonce echoed, little-endian
    CHECK(uint8_t(buf[4]) == 0xEF);
    CHECK(uint8_t(buf[5]) == 0xBE);
    CHECK(uint8_t(buf[6]) == 0xAD);
    CHECK(uint8_t(buf[7]) == 0xDE);
    // hub_name: text then zero-padding to 32 bytes (offset 8..39)
    CHECK(std::memcmp(&buf[8], "slopdrive-32", 12) == 0);
    for (size_t i = 8 + 12; i < 8 + kHubNameMaxBytes; ++i) CHECK(uint8_t(buf[i]) == 0);
    // hub_instance_id: u64 little-endian at offset 40
    CHECK(uint8_t(buf[40]) == 0xEF);
    CHECK(uint8_t(buf[47]) == 0x01);
    // proto_ver at offset 48
    CHECK(uint8_t(buf[48]) == 1);
    // ws_port at offset 49-50
    CHECK(uint8_t(buf[49]) == 82);
    CHECK(uint8_t(buf[50]) == 0);
    // fw_version at offset 51..66 (16 bytes)
    CHECK(std::memcmp(&buf[51], "2.2.0", 5) == 0);
    for (size_t i = 51 + 5; i < 51 + kFwVersionMaxBytes; ++i) CHECK(uint8_t(buf[i]) == 0);
    // catalog_etag at offset 67..74
    CHECK(uint8_t(buf[67]) == 0xAA);
    CHECK(uint8_t(buf[74]) == 0x11);
    // flags at offset 75 (last byte)
    CHECK(uint8_t(buf[75]) == 0x03);
}

TEST_CASE("buildReply: hub_name/fw_version longer than their field width are truncated, not overrun") {
    ReplyFields f;
    f.hub_name = std::string_view("a-name-that-is-definitely-longer-than-32-bytes-wide");
    f.fw_version = std::string_view("9.9.9-a-version-string-way-past-16-bytes");

    std::array<std::byte, kReplyBytes> buf{};
    size_t n = buildReply(f, buf);
    REQUIRE(n == kReplyBytes);

    // hub_name field is exactly kHubNameMaxBytes of the source text, no NUL
    // guaranteed (it exactly fills the field) — matches packStringField()'s
    // documented truncation semantics.
    CHECK(std::memcmp(&buf[8], f.hub_name.data(), kHubNameMaxBytes) == 0);
    CHECK(std::memcmp(&buf[51], f.fw_version.data(), kFwVersionMaxBytes) == 0);
}

TEST_CASE("buildReply: a too-small output buffer fails honestly (0), never a partial/OOB write") {
    ReplyFields f;
    std::array<std::byte, kReplyBytes - 1> tooSmall{};
    CHECK(buildReply(f, tooSmall) == 0);
}

TEST_CASE("buildReply: an all-default ReplyFields still produces exactly kReplyBytes of well-formed output") {
    ReplyFields f{};
    std::array<std::byte, kReplyBytes> buf{};
    CHECK(buildReply(f, buf) == kReplyBytes);
    CHECK(uint8_t(buf[0]) == 'S');
    CHECK(uint8_t(buf[75]) == 0);
}
