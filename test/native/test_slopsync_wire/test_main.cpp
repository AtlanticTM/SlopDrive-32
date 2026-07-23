// ============================================================================
// test_main.cpp — doctest unit tests for slopsync-core's wire modules:
// frame_header.hpp, crc32.hpp, estop_frame.hpp, serial_cobs.hpp, and the
// serial-arithmetic helpers in util/serial_arithmetic.hpp.
//
// Native (host-side, hardware-free) test, same pattern as
// test/native/test_motion_profile/test_main.cpp: these headers have no
// bus/FreeRTOS/I-O dependencies, so they're exercised directly on the host.
//
// Suite ids below (H-01..H-04, E-01..E-03) match
// docs/slopsync/vectors/manifest.yaml; SPEC section numbers cite
// docs/slopsync/SPEC.md. E-04 (repeat-until-latch, behavioral) is out of
// scope for this native/unit suite per the brief.
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/crc32.hpp"
#include "slopsync/wire/estop_frame.hpp"
#include "slopsync/wire/frame_header.hpp"
#include "slopsync/wire/serial_cobs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace slopsync;

// ============================================================================
// H-01 — encode/decode every FrameType's header, seq 0 and 0xFFFF.
// ============================================================================
TEST_CASE("H-01: header round-trip for every registered FrameType, seq 0 and 0xFFFF") {
    // The full registry list (registry_constants.hpp). ESTOP (0xE5) is
    // included here too: this test only exercises the generic 8-byte codec's
    // byte-level correctness for that type value — a REAL ESTOP frame is
    // never header-shaped on the wire (frame_header.hpp's own note, SPEC
    // §5.5), that's a caller-level dispatch rule, not a limitation of
    // encodeFrameHeader/decodeFrameHeader themselves.
    constexpr FrameType kAllTypes[] = {
        FrameType::HELLO,        FrameType::WELCOME,      FrameType::PING,
        FrameType::PONG,         FrameType::CLOCK,        FrameType::SUBSCRIBE,
        FrameType::UNSUBSCRIBE,  FrameType::GRANT,        FrameType::CATALOG_REQ,
        FrameType::CATALOG_CHUNK, FrameType::STATE,       FrameType::STREAM,
        FrameType::INTENT,       FrameType::ECHO,         FrameType::EVENT,
        FrameType::NACK,         FrameType::GOODBYE,      FrameType::PROBE,
        FrameType::PROBE_REPORT, FrameType::PAIR_REQ,     FrameType::PAIR_GRANT,
        FrameType::ACKMASK,      FrameType::BEACON,       FrameType::ESTOP,
    };

    for (uint16_t seq : {uint16_t(0), uint16_t(0xFFFF)}) {
        for (auto t : kAllTypes) {
            FrameHeader h;
            h.type = uint8_t(t);
            h.flags = 0;
            h.channel = 0x1234;
            h.seq = seq;
            h.len = 99;

            std::array<std::byte, kHeaderBytes> buf{};
            REQUIRE(encodeFrameHeader(h, buf) == kHeaderBytes);

            auto decoded = decodeFrameHeader(buf);
            REQUIRE(decoded.has_value());
            CHECK(decoded->type == h.type);
            CHECK(decoded->flags == h.flags);
            CHECK(decoded->channel == h.channel);
            CHECK(decoded->seq == h.seq);
            CHECK(decoded->len == h.len);
        }
    }
}

TEST_CASE("H-01: byte-exact header encoding for HELLO and STATE") {
    // HELLO: type=0x00 flags=0x00 channel=0x0000 seq=0x0000 len=0x0010
    // LE bytes: [type][flags] [channel_lo][channel_hi] [seq_lo][seq_hi] [len_lo][len_hi]
    //         =   00     00      00          00           00     00       10     00
    {
        FrameHeader h;
        h.type = uint8_t(FrameType::HELLO);
        h.flags = 0;
        h.channel = 0;
        h.seq = 0;
        h.len = 0x0010;

        std::array<std::byte, 8> expected = {
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x10}, std::byte{0x00},
        };
        std::array<std::byte, 8> buf{};
        REQUIRE(encodeFrameHeader(h, buf) == 8);
        CHECK(buf == expected);

        auto decoded = decodeFrameHeader(buf);
        REQUIRE(decoded.has_value());
        CHECK(decoded->type == uint8_t(FrameType::HELLO));
        CHECK(decoded->len == 0x0010);
    }

    // Same HELLO with seq=0xFFFF: 00 00 | 00 00 | FF FF | 10 00
    {
        FrameHeader h;
        h.type = uint8_t(FrameType::HELLO);
        h.flags = 0;
        h.channel = 0;
        h.seq = 0xFFFF;
        h.len = 0x0010;

        std::array<std::byte, 8> expected = {
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0xFF}, std::byte{0xFF}, std::byte{0x10}, std::byte{0x00},
        };
        std::array<std::byte, 8> buf{};
        REQUIRE(encodeFrameHeader(h, buf) == 8);
        CHECK(buf == expected);
    }

    // STATE: type=0x0B flags=0x00 channel=channels::safety(0x0003) seq=0x0000
    // len=limits::min_transport_payload (242 = 0x00F2)
    // LE bytes: 0B 00 | 03 00 | 00 00 | F2 00
    {
        FrameHeader h;
        h.type = uint8_t(FrameType::STATE);
        h.flags = 0;
        h.channel = channels::safety;
        h.seq = 0;
        h.len = uint16_t(limits::min_transport_payload);

        std::array<std::byte, 8> expected = {
            std::byte{0x0B}, std::byte{0x00}, std::byte{0x03}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x00}, std::byte{0xF2}, std::byte{0x00},
        };
        std::array<std::byte, 8> buf{};
        REQUIRE(encodeFrameHeader(h, buf) == 8);
        CHECK(buf == expected);
    }

    // Same STATE with seq=0xFFFF: 0B 00 | 03 00 | FF FF | F2 00
    {
        FrameHeader h;
        h.type = uint8_t(FrameType::STATE);
        h.flags = 0;
        h.channel = channels::safety;
        h.seq = 0xFFFF;
        h.len = uint16_t(limits::min_transport_payload);

        std::array<std::byte, 8> expected = {
            std::byte{0x0B}, std::byte{0x00}, std::byte{0x03}, std::byte{0x00},
            std::byte{0xFF}, std::byte{0xFF}, std::byte{0xF2}, std::byte{0x00},
        };
        std::array<std::byte, 8> buf{};
        REQUIRE(encodeFrameHeader(h, buf) == 8);
        CHECK(buf == expected);
    }
}

// ============================================================================
// H-02 — len bounds: 0, min_transport_payload (242), 65535; short-buffer decode.
// ============================================================================
TEST_CASE("H-02: len bounds round-trip (0, 242, 65535); 7-byte buffer decodes to nullopt") {
    for (uint16_t len : {uint16_t(0), uint16_t(limits::min_transport_payload), uint16_t(0xFFFF)}) {
        FrameHeader h;
        h.type = uint8_t(FrameType::STREAM);
        h.channel = 1;
        h.seq = 5;
        h.len = len;

        std::array<std::byte, 8> buf{};
        REQUIRE(encodeFrameHeader(h, buf) == 8);
        auto decoded = decodeFrameHeader(buf);
        REQUIRE(decoded.has_value());
        CHECK(decoded->len == len);
    }

    // A 7-byte buffer is one short of a full header: decode must fail
    // cleanly (nullopt), never read past the end of `in`.
    std::array<std::byte, 7> shortIn{};
    CHECK_FALSE(decodeFrameHeader(shortIn).has_value());

    // encode into an under-sized buffer must also fail cleanly (0 return).
    std::array<std::byte, 7> shortOut{};
    FrameHeader h;
    CHECK(encodeFrameHeader(h, shortOut) == 0);
}

// ============================================================================
// H-03 — unknown frame type decodes fine; tolerance is the caller's concern.
// ============================================================================
TEST_CASE("H-03: unknown frame type 0x7A decodes without failure (SPEC §4.3 tolerance)") {
    FrameHeader h;
    h.type = 0x7A;
    h.channel = 0;
    h.seq = 0;
    h.len = 10;

    std::array<std::byte, 8> buf{};
    REQUIRE(encodeFrameHeader(h, buf) == 8);

    auto decoded = decodeFrameHeader(buf);
    REQUIRE(decoded.has_value());
    CHECK(decoded->type == 0x7A);
    CHECK(decoded->len == 10);
}

// ============================================================================
// H-04 — reserved flag bits are preserved; fragStart/fragMore read only bits 0/1.
// ============================================================================
TEST_CASE("H-04: reserved flag bits are preserved and readable; fragStart/fragMore reflect only bits 0/1") {
    SUBCASE("all 8 bits set: both frag accessors true, flags fully readable") {
        FrameHeader h;
        h.type = uint8_t(FrameType::STREAM);
        h.flags = 0xFF;
        std::array<std::byte, 8> buf{};
        REQUIRE(encodeFrameHeader(h, buf) == 8);
        auto decoded = decodeFrameHeader(buf);
        REQUIRE(decoded.has_value());
        CHECK(decoded->flags == 0xFF);
        CHECK(decoded->fragStart());
        CHECK(decoded->fragMore());
    }
    SUBCASE("only reserved bits (2-7) set: neither frag accessor trips, flags still readable") {
        FrameHeader h;
        h.type = uint8_t(FrameType::STREAM);
        h.flags = 0xFC;  // 0b1111_1100 — bits 0/1 clear, all reserved bits set
        std::array<std::byte, 8> buf{};
        REQUIRE(encodeFrameHeader(h, buf) == 8);
        auto decoded = decodeFrameHeader(buf);
        REQUIRE(decoded.has_value());
        CHECK(decoded->flags == 0xFC);
        CHECK_FALSE(decoded->fragStart());
        CHECK_FALSE(decoded->fragMore());
    }
    SUBCASE("only FRAG_START set") {
        FrameHeader h;
        h.flags = flags::FRAG_START;
        std::array<std::byte, 8> buf{};
        REQUIRE(encodeFrameHeader(h, buf) == 8);
        auto decoded = decodeFrameHeader(buf);
        REQUIRE(decoded.has_value());
        CHECK(decoded->fragStart());
        CHECK_FALSE(decoded->fragMore());
    }
    SUBCASE("only FRAG_MORE set") {
        FrameHeader h;
        h.flags = flags::FRAG_MORE;
        std::array<std::byte, 8> buf{};
        REQUIRE(encodeFrameHeader(h, buf) == 8);
        auto decoded = decodeFrameHeader(buf);
        REQUIRE(decoded.has_value());
        CHECK_FALSE(decoded->fragStart());
        CHECK(decoded->fragMore());
    }
}

// ============================================================================
// CRC-32 known-answer check (IEEE 802.3 / ISO-HDLC).
// ============================================================================
TEST_CASE("CRC-32 known-answer: crc32(\"123456789\") == 0xCBF43926") {
    const char check[] = "123456789";
    std::array<std::byte, 9> bytes{};
    for (size_t i = 0; i < 9; ++i) bytes[i] = std::byte(uint8_t(check[i]));

    CHECK(crc32(bytes) == 0xCBF43926u);

    // The incremental API must agree with the one-shot function, split
    // across two update() calls to prove state actually carries over.
    uint32_t state = crc32Init();
    state = crc32Update(state, std::span(bytes).subspan(0, 4));
    state = crc32Update(state, std::span(bytes).subspan(4));
    CHECK(crc32Final(state) == 0xCBF43926u);
}

// ============================================================================
// E-01 — ESTOP frame bytes: cause/origin/seq/CRC, exact 12 bytes.
// ============================================================================
TEST_CASE("E-01: ESTOP frame encoding — exact 12 bytes for cause=user origin=1 seq=1") {
    // Hand-derivation: the 8 header bytes to CRC are
    //   E5 E5 E5 E5  00  01  01 00      (magic | cause=0 | origin=1 | seq=1 LE)
    // Running the CRC-32/IEEE algorithm (poly 0xEDB88320 reflected,
    // init/final-xor 0xFFFFFFFF — the same table this file's known-answer
    // check above validates) over those 8 bytes gives 0x41EEE434, written
    // little-endian as 34 E4 EE 41. (Cross-checked with a standalone
    // reference implementation of the same algorithm; also re-derived below
    // via the library's own incremental API so the test doesn't just trust
    // one code path.)
    EstopFrame f;
    f.cause = uint8_t(EstopCause::user);  // 0
    f.origin = 1;
    f.seq = 1;

    std::array<std::byte, kEstopFrameBytes> buf{};
    REQUIRE(encodeEstop(f, buf) == kEstopFrameBytes);

    std::array<std::byte, 12> expected = {
        std::byte{0xE5}, std::byte{0xE5}, std::byte{0xE5}, std::byte{0xE5},
        std::byte{0x00}, std::byte{0x01}, std::byte{0x01}, std::byte{0x00},
        std::byte{0x34}, std::byte{0xE4}, std::byte{0xEE}, std::byte{0x41},
    };
    CHECK(buf == expected);

    uint32_t crc = crc32Final(crc32Update(crc32Init(), std::span(buf).subspan(0, 8)));
    CHECK(crc == 0x41EEE434u);

    auto decoded = decodeEstop(buf);
    REQUIRE(decoded.isOk());
    CHECK(decoded.value().cause == 0);
    CHECK(decoded.value().origin == 1);
    CHECK(decoded.value().seq == 1);
}

// ============================================================================
// E-02 — magic scanner: offsets 0..3, CRC rejection with continued scan,
// truncated-tail safety (no OOB read).
// ============================================================================
TEST_CASE("E-02: scanForEstop finds the frame at offsets 0..3 in a noise buffer") {
    EstopFrame f;
    f.cause = uint8_t(EstopCause::fault);
    f.origin = 2;
    f.seq = 0xABCD;

    std::array<std::byte, kEstopFrameBytes> frameBytes{};
    encodeEstop(f, frameBytes);

    for (size_t offset = 0; offset <= 3; ++offset) {
        // Leading/trailing noise deliberately avoids 0xE5 so it can't create
        // an accidental extra magic match.
        std::vector<std::byte> stream(offset, std::byte{0x5A});
        stream.insert(stream.end(), frameBytes.begin(), frameBytes.end());
        stream.push_back(std::byte{0x99});

        auto result = scanForEstop(stream);
        REQUIRE(result.found);
        CHECK(result.offset == offset);
        CHECK(result.frame.cause == f.cause);
        CHECK(result.frame.origin == f.origin);
        CHECK(result.frame.seq == f.seq);
    }
}

TEST_CASE("E-02: corrupted CRC is rejected; scan continues past it to a real frame") {
    EstopFrame f;
    f.cause = 1;
    f.origin = 1;
    f.seq = 42;

    std::array<std::byte, kEstopFrameBytes> good{};
    encodeEstop(f, good);

    std::vector<std::byte> corrupted(good.begin(), good.end());
    corrupted[8] ^= std::byte{0xFF};  // flip a CRC byte: guaranteed mismatch

    SUBCASE("corrupted frame alone: not found") {
        CHECK_FALSE(scanForEstop(corrupted).found);
    }

    SUBCASE("corrupted frame followed by a real one: scan continues past it") {
        std::vector<std::byte> stream = corrupted;
        stream.push_back(std::byte{0x00});  // one separating byte
        stream.insert(stream.end(), good.begin(), good.end());

        auto result = scanForEstop(stream);
        REQUIRE(result.found);
        CHECK(result.offset == corrupted.size() + 1);
        CHECK(result.frame.seq == 42);
    }
}

TEST_CASE("E-02: 4x0xE5 at buffer end with fewer than 8 trailing bytes -> not found, no OOB read") {
    // Exactly the magic, nothing after it.
    std::vector<std::byte> stream(4, kEstopMagicByte);
    CHECK_FALSE(scanForEstop(stream).found);

    // Magic plus some, but not enough (5 of the needed 8), trailing bytes.
    std::vector<std::byte> stream2(4, kEstopMagicByte);
    for (int i = 0; i < 5; ++i) stream2.push_back(std::byte{0x11});
    CHECK_FALSE(scanForEstop(stream2).found);

    // Degenerate inputs: empty, and shorter than the magic itself.
    CHECK_FALSE(scanForEstop(std::span<const std::byte>{}).found);
    std::array<std::byte, 3> tooShort = {kEstopMagicByte, kEstopMagicByte, kEstopMagicByte};
    CHECK_FALSE(scanForEstop(tooShort).found);
}

// ============================================================================
// E-03 — COBS: classic small vectors (byte-exact), max-run boundary, ESTOP
// round-trip through COBS, and raw-scan transparency when no 0x00 falls
// inside the frame (SPEC §13.5).
// ============================================================================
TEST_CASE("E-03: COBS classic vectors — exact encoded bytes") {
    auto encodeToVec = [](std::span<const std::byte> src) {
        std::vector<std::byte> dst(src.size() + src.size() / 254 + 2, std::byte{0xCC});
        size_t n = cobsEncode(src, dst);
        REQUIRE(n > 0);
        dst.resize(n);
        return dst;
    };

    SUBCASE("empty input -> single code byte 0x01 (no data, no delimiter here)") {
        std::vector<std::byte> src;
        auto out = encodeToVec(src);
        std::vector<std::byte> expected = {std::byte{0x01}};
        CHECK(out == expected);
    }
    SUBCASE("{0x00} -> 01 01") {
        std::vector<std::byte> src = {std::byte{0x00}};
        auto out = encodeToVec(src);
        std::vector<std::byte> expected = {std::byte{0x01}, std::byte{0x01}};
        CHECK(out == expected);
    }
    SUBCASE("{0x11,0x22,0x00,0x33} -> 03 11 22 02 33") {
        std::vector<std::byte> src = {std::byte{0x11}, std::byte{0x22}, std::byte{0x00},
                                       std::byte{0x33}};
        auto out = encodeToVec(src);
        std::vector<std::byte> expected = {
            std::byte{0x03}, std::byte{0x11}, std::byte{0x22}, std::byte{0x02}, std::byte{0x33},
        };
        CHECK(out == expected);
    }
    SUBCASE("254 nonzero bytes -> FF + the 254 bytes verbatim (max-run boundary)") {
        // A maximal 254-byte non-zero run ending exactly at end-of-input
        // needs no further code byte (see serial_cobs.hpp's doc comment):
        // encoded length is 255 (FF + 254 data bytes), not 256.
        std::vector<std::byte> src;
        for (int i = 0; i < 254; ++i) src.push_back(std::byte(uint8_t(i + 1)));  // 0x01..0xFE
        auto out = encodeToVec(src);
        REQUIRE(out.size() == 255);
        CHECK(out[0] == std::byte{0xFF});
        for (size_t i = 0; i < 254; ++i) CHECK(out[i + 1] == src[i]);

        // And it must still decode back to the original 254 bytes.
        std::vector<std::byte> decoded(254);
        auto n = cobsDecode(out, decoded);
        REQUIRE(n.isOk());
        CHECK(n.value() == 254);
        CHECK(std::equal(decoded.begin(), decoded.end(), src.begin()));
    }
}

TEST_CASE("E-03: COBS round-trip of an encoded ESTOP frame") {
    EstopFrame f;
    f.cause = uint8_t(EstopCause::relay);
    f.origin = 2;
    f.seq = 0x0102;

    std::array<std::byte, kEstopFrameBytes> raw{};
    encodeEstop(f, raw);

    std::array<std::byte, kEstopFrameBytes + kEstopFrameBytes / 254 + 2> cobsBuf{};
    size_t n = cobsEncode(raw, cobsBuf);
    REQUIRE(n > 0);

    std::array<std::byte, kEstopFrameBytes + 4> decodedBuf{};
    auto decodedLen = cobsDecode(std::span(cobsBuf).first(n), decodedBuf);
    REQUIRE(decodedLen.isOk());
    REQUIRE(decodedLen.value() == kEstopFrameBytes);
    CHECK(std::equal(decodedBuf.begin(), decodedBuf.begin() + kEstopFrameBytes, raw.begin()));

    auto decodedFrame = decodeEstop(std::span(decodedBuf).first(kEstopFrameBytes));
    REQUIRE(decodedFrame.isOk());
    CHECK(decodedFrame.value().cause == f.cause);
    CHECK(decodedFrame.value().origin == f.origin);
    CHECK(decodedFrame.value().seq == f.seq);
}

TEST_CASE("E-03: raw magic scan survives COBS encoding when no 0x00 falls inside the ESTOP frame") {
    // cause=deadman(1) origin=1 seq=0x0101 was hand-picked (brute-force
    // search over cause/origin/seq — see crc32.hpp's known-answer derivation
    // notes for the algorithm being trusted here) so that NONE of the
    // frame's 12 bytes are zero: E5 E5 E5 E5 01 01 01 01 C7 B3 55 8E. That's
    // the precondition SPEC §13.5 relies on ("0xE5 survives COBS encoding
    // unchanged when no zero bytes occur in the window"). The REQUIRE below
    // pins that precondition so a future edit to these inputs fails loudly
    // here rather than silently testing the wrong thing.
    EstopFrame f;
    f.cause = uint8_t(EstopCause::deadman);
    f.origin = 1;
    f.seq = 0x0101;

    std::array<std::byte, kEstopFrameBytes> raw{};
    encodeEstop(f, raw);
    for (auto b : raw) REQUIRE(b != std::byte{0x00});

    std::array<std::byte, kEstopFrameBytes + 2> cobsBuf{};
    size_t n = cobsEncode(raw, cobsBuf);
    // Exactly one leading COBS code byte, nothing else inserted — the
    // whole 12-byte frame passes through untouched.
    REQUIRE(n == kEstopFrameBytes + 1);

    auto result = scanForEstop(std::span(cobsBuf).first(n));
    REQUIRE(result.found);
    CHECK(result.offset == 1);
    CHECK(result.frame.cause == f.cause);
    CHECK(result.frame.origin == f.origin);
    CHECK(result.frame.seq == f.seq);
}

// ============================================================================
// serial_arithmetic — seqIsNewer truth table (incl. wrap), timeDelta/
// timeReached across the u32 wrap boundary.
// ============================================================================
TEST_CASE("serial_arithmetic: seqIsNewer truth table incl. wrap") {
    CHECK(seqIsNewer(1, 0));
    CHECK_FALSE(seqIsNewer(0, 0));            // equal is NOT newer
    CHECK_FALSE(seqIsNewer(0, 1));             // strictly older

    CHECK(seqIsNewer(0x0001, 0xFFFF));         // wraps: 1 is newer than 0xFFFF
    CHECK_FALSE(seqIsNewer(0xFFFF, 0x0001));   // and not vice versa

    // Exactly half the range apart (0x8000) is explicitly excluded from
    // "newer" in either direction — the boundary case SPEC §7.3 calls out.
    CHECK_FALSE(seqIsNewer(0x8000, 0x0000));
    CHECK_FALSE(seqIsNewer(0x0000, 0x8000));

    CHECK(seqIsNewer(0x7FFF, 0x0000));         // just inside the newer window
    CHECK_FALSE(seqIsNewer(0x8001, 0x0000));   // just past it (wraps to "older")
}

TEST_CASE("serial_arithmetic: timeDelta/timeReached across the u32 wrap boundary") {
    CHECK(timeDelta(1000, 900) == 100);
    CHECK(timeDelta(900, 1000) == -100);

    // reference sits 16 counts before the u32 wrap; t sits 5 counts after it
    // -> the windowed delta is +21, not some huge unwrapped negative number.
    constexpr uint32_t reference = 0xFFFFFFF0u;
    constexpr uint32_t t = 0x00000005u;
    CHECK(timeDelta(t, reference) == 21);

    CHECK(timeReached(0xFFFFFFF0u, reference));        // exactly at the deadline
    CHECK_FALSE(timeReached(0xFFFFFFEFu, reference));  // one count before
    CHECK(timeReached(0x00000005u, reference));        // wrapped 21 counts past
    CHECK_FALSE(timeReached(0xFFFFFFDFu, reference));  // clearly before (delta -17)
}

TEST_CASE("serial_arithmetic: MonotonicMs matches plain division off the wrap") {
    // §7.2 regression (the 71.6-minute bug): nowUs/1000 does not wrap mod
    // 2^32, so ms deadlines strand at the µs wrap. MonotonicMs must be
    // bit-identical to the old division for any non-wrapping run...
    MonotonicMs m;
    CHECK(m.advance(5000) == 5);
    CHECK(m.advance(5999) == 5);   // remainder carried, not truncated away
    CHECK(m.advance(6000) == 6);
    CHECK(m.advance(6001) == 6);
    CHECK(m.advance(1'000'000) == 1000);
    CHECK(m.advance(1'000'999) == 1000);
    CHECK(m.advance(1'001'000) == 1001);

    // Seeding mid-stream (first call at an arbitrary time) also matches.
    MonotonicMs m2;
    CHECK(m2.advance(123'456'789) == 123'456);
    CHECK(m2.advance(123'457'788) == 123'457);
}

TEST_CASE("serial_arithmetic: MonotonicMs is continuous across the u32 microsecond wrap") {
    // ...and must keep counting smoothly where the division jumps 4294967 -> 0.
    MonotonicMs m;
    uint32_t t = 0xFFFFFFFFu - 8'000'000u;  // 8 s before the wrap
    uint32_t prev = m.advance(t);
    CHECK(prev == t / 1000u);

    // Step 5 ms at a time through the wrap and 8 s beyond. Every step the
    // returned ms must advance by exactly 5 — no jump, no stall.
    for (int i = 0; i < 3200; ++i) {
        t += 5000;  // wraps mod 2^32 partway through — that's the point
        uint32_t now = m.advance(t);
        CHECK(now - prev == 5);
        prev = now;
    }

    // A deadline armed before the wrap is reached ON TIME after it: the
    // pre-fix division would have parked it ~71 minutes in the future.
    MonotonicMs m3;
    uint32_t t3 = 0xFFFFFFFFu - 20'000u;            // 20 ms before wrap
    uint32_t deadlineMs = m3.advance(t3) + 30;      // 30 ms window (deadman-style)
    t3 += 50'000;                                    // +50 ms, crossing the wrap
    CHECK(timeReached(m3.advance(t3), deadlineMs));
}
