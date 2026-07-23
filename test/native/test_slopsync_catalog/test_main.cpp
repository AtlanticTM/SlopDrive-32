// ============================================================================
// test_main.cpp — doctest unit tests for slopsync-core's catalog wire layer:
// wire/sha256.hpp, wire/catalog_codec.hpp, wire/catalog_etag.hpp,
// wire/catalog_chunks.hpp, wire/messages/catalog_req.hpp, and
// session/static_profile.hpp.
//
// Native (host-side, hardware-free) test, same pattern as
// test/native/test_slopsync_wire/test_main.cpp and test_slopsync_cbor: no
// Arduino, no bus/FreeRTOS dependency — header-only, entirely math/logic.
//
// Implements golden vectors K-01..K-05 from
// docs/slopsync/vectors/manifest.yaml suite `catalog`, against the frozen
// fixture conformance::miniCatalog() (lib/slopsync/include/slopsync/
// conformance/mini_catalog.hpp) — 6 entries covering every PackedFieldType,
// every CborFieldType, both layout- and schema-form entries, two bitfield8s,
// optional min/max both present and absent. SPEC section numbers cite
// docs/slopsync/SPEC.md.
//
// PINNING METHODOLOGY (documented once, applies to every pinned literal
// below): the encoded length (K-01) and etag bytes (K-02) were computed by
// running this exact codec against miniCatalog() during development, printed
// once, and are now hard-coded as literals. This freezes THIS fixture's
// deterministic encoding: any future change to catalog_codec.hpp,
// channel/catalog.hpp, or conformance/mini_catalog.hpp that shifts a single
// byte of the encoding will fail these tests loudly — which is the point
// (§8.3's whole premise is that the encoding is reproducible byte-for-byte).
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/conformance/mini_catalog.hpp"
#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/session/static_profile.hpp"
#include "slopsync/wire/catalog_chunks.hpp"
#include "slopsync/wire/catalog_codec.hpp"
#include "slopsync/wire/catalog_etag.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"
#include "slopsync/wire/messages/catalog_req.hpp"
#include "slopsync/wire/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace slopsync;

namespace {
constexpr std::byte B(int x) { return std::byte(uint8_t(x)); }
}  // namespace

// ============================================================================
// SHA-256 known-answer tests.
// ============================================================================
TEST_CASE("SHA-256 known-answer: empty string and \"abc\"") {
    auto h1 = Sha256::hash(std::span<const std::byte>{});
    std::array<std::byte, 32> expectEmpty = {
        B(0xe3), B(0xb0), B(0xc4), B(0x42), B(0x98), B(0xfc), B(0x1c), B(0x14),
        B(0x9a), B(0xfb), B(0xf4), B(0xc8), B(0x99), B(0x6f), B(0xb9), B(0x24),
        B(0x27), B(0xae), B(0x41), B(0xe4), B(0x64), B(0x9b), B(0x93), B(0x4c),
        B(0xa4), B(0x95), B(0x99), B(0x1b), B(0x78), B(0x52), B(0xb8), B(0x55),
    };
    CHECK(h1 == expectEmpty);

    const char abc[] = "abc";
    auto h2 = Sha256::hash(std::span<const std::byte>(reinterpret_cast<const std::byte*>(abc), 3));
    std::array<std::byte, 32> expectAbc = {
        B(0xba), B(0x78), B(0x16), B(0xbf), B(0x8f), B(0x01), B(0xcf), B(0xea),
        B(0x41), B(0x41), B(0x40), B(0xde), B(0x5d), B(0xae), B(0x22), B(0x23),
        B(0xb0), B(0x03), B(0x61), B(0xa3), B(0x96), B(0x17), B(0x7a), B(0x9c),
        B(0xb4), B(0x10), B(0xff), B(0x61), B(0xf2), B(0x00), B(0x15), B(0xad),
    };
    CHECK(h2 == expectAbc);

    // Incremental API must agree with the one-shot function, split across
    // multiple update() calls spanning a block boundary (64 bytes/block) so
    // the partial-buffer-carryover path is actually exercised.
    std::vector<std::byte> big(130);
    for (size_t i = 0; i < big.size(); ++i) big[i] = B(int(i));
    auto oneShot = Sha256::hash(big);

    Sha256 sha;
    sha.update(std::span(big).subspan(0, 10));
    sha.update(std::span(big).subspan(10, 54));   // completes first 64-byte block
    sha.update(std::span(big).subspan(64, 64));    // exactly one more block
    sha.update(std::span(big).subspan(128));       // trailing partial block
    auto incremental = sha.final();
    CHECK(incremental == oneShot);
}

// ============================================================================
// K-01 — mini-catalog deterministic encoding: exact bytes (pinned length),
// determinism (encode twice byte-identical; decode -> re-encode
// byte-identical), and hand-derived structural spot-checks.
// ============================================================================
TEST_CASE("K-01: mini-catalog deterministic encoding") {
    auto cat = conformance::miniCatalog();
    std::array<std::byte, 2048> buf1{};
    std::array<std::byte, 2048> buf2{};

    size_t n1 = encodeCatalog(cat, buf1);
    size_t n2 = encodeCatalog(cat, buf2);
    REQUIRE(n1 > 0);
    REQUIRE(n1 == n2);
    CHECK(std::equal(buf1.begin(), buf1.begin() + n1, buf2.begin()));

    // ---- Hand-derived structural spot-checks -------------------------------
    // catalog = [ * channel-entry ], miniCatalog() has 6 entries: an array
    // head of 6 elements (major type 4, additional-info == count since
    // 6 <= 23) is a single byte: (4<<5)|6 = 0x80|0x06 = 0x86.
    CHECK(buf1[0] == B(0x86));

    // First entry ("safety", id=0x0003): its own map has 8 pairs (keys
    // 1..7 always + key 8 since it's a STATE/layout-class entry) -> map
    // head byte (5<<5)|8 = 0xA0|0x08 = 0xA8.
    CHECK(buf1[1] == B(0xA8));
    // Key 1 (id): unsigned int head, value 1 <= 23 -> single byte 0x01.
    CHECK(buf1[2] == B(0x01));
    // Value: id = 0x0003, also <= 23 -> single byte 0x03.
    CHECK(buf1[3] == B(0x03));

    // K-01: PIN the total encoded length. Computed once by running this
    // codec over the frozen miniCatalog() fixture and printed during
    // development (see file header's pinning methodology) — any future
    // change to the encoding shifts this number and must fail here.
    CHECK(n1 == 733);

    // Decode -> re-encode must reproduce the exact same bytes (round-trip
    // determinism, not just "encode is stable").
    Catalog32 decoded{};
    auto dr = decodeCatalog(std::span<const std::byte>(buf1).first(n1), decoded);
    REQUIRE(dr.isOk());
    CHECK(decoded.count == cat.count);

    std::array<std::byte, 2048> buf3{};
    size_t n3 = encodeCatalog(decoded, buf3);
    REQUIRE(n3 == n1);
    CHECK(std::equal(buf1.begin(), buf1.begin() + n1, buf3.begin()));
}

// ============================================================================
// K-02 — etag computation over K-01 bytes: exact 8-byte value (pinned),
// changes when catalog content changes, stable across re-encode.
// ============================================================================
TEST_CASE("K-02: catalogEtag over mini-catalog") {
    auto cat = conformance::miniCatalog();
    std::array<std::byte, 2048> scratch{};
    auto etag = catalogEtag(cat, std::span<std::byte>(scratch));

    // K-02: PIN all 8 bytes (same freeze technique as K-01 — computed once,
    // printed during development, now a hard assertion).
    std::array<std::byte, 8> expected = {
        B(0x21), B(0xCB), B(0x26), B(0xC9), B(0x4F), B(0xB3), B(0x88), B(0xB5),
    };
    CHECK(etag == expected);

    // Mutating a copy's maxRateHz must change the etag (§8.3: etag covers
    // "everything in §8.1", which includes max_rate_hz).
    auto mutated = cat;
    mutated.entries[1].maxRateHz = 241.0f;  // was 240.0f (the "position" channel)
    std::array<std::byte, 2048> scratch2{};
    auto etagMutated = catalogEtag(mutated, std::span<std::byte>(scratch2));
    CHECK(etagMutated != expected);

    // Re-encoding (encode -> decode -> re-derive etag from the decoded
    // copy) must NOT change the etag: it's a function of catalog content,
    // not of which in-memory instance produced the bytes.
    std::array<std::byte, 2048> encodeBuf{};
    size_t n = encodeCatalog(cat, encodeBuf);
    REQUIRE(n > 0);
    Catalog32 decoded{};
    REQUIRE(decodeCatalog(std::span<const std::byte>(encodeBuf).first(n), decoded).isOk());
    std::array<std::byte, 2048> scratch3{};
    auto etagFromDecoded = catalogEtag(decoded, std::span<std::byte>(scratch3));
    CHECK(etagFromDecoded == expected);
}

// ============================================================================
// K-03 — chunking at 192B: chunk_count for the encoded size, byte-exact
// reassembly in order and in reverse order (order independence).
// ============================================================================
TEST_CASE("K-03: chunking and reassembly") {
    auto cat = conformance::miniCatalog();
    std::array<std::byte, 2048> buf{};
    size_t n = encodeCatalog(cat, buf);
    REQUIRE(n == 733);  // pinned by K-01; chunk math below depends on it

    // ceil(733 / 192) = 4 chunks: 192, 192, 192, 157.
    size_t cc = chunkCount(n);
    CHECK(cc == 4);

    SUBCASE("in-order delivery reassembles byte-exact") {
        ChunkReassembler<> reasm;
        reasm.begin(uint16_t(cc), n, /*nowMs=*/0);
        for (uint16_t i = 0; i < cc; ++i) {
            std::array<std::byte, 4 + limits::catalog_chunk_payload> chunkBuf{};
            size_t clen = fillChunk(std::span<const std::byte>(buf).first(n), i, chunkBuf);
            REQUIRE(clen > 0);
            CHECK(reasm.insert(std::span<const std::byte>(chunkBuf).first(clen), 1));
        }
        REQUIRE(reasm.complete());
        auto assembled = reasm.assembled();
        REQUIRE(assembled.size() == n);
        CHECK(std::equal(assembled.begin(), assembled.end(), buf.begin()));
    }

    SUBCASE("reverse-order delivery reassembles identically (order independence)") {
        ChunkReassembler<> reasm;
        reasm.begin(uint16_t(cc), n, /*nowMs=*/0);
        for (uint16_t i = uint16_t(cc); i-- > 0;) {
            std::array<std::byte, 4 + limits::catalog_chunk_payload> chunkBuf{};
            size_t clen = fillChunk(std::span<const std::byte>(buf).first(n), i, chunkBuf);
            REQUIRE(clen > 0);
            CHECK(reasm.insert(std::span<const std::byte>(chunkBuf).first(clen), 1));
        }
        REQUIRE(reasm.complete());
        auto assembled = reasm.assembled();
        REQUIRE(assembled.size() == n);
        CHECK(std::equal(assembled.begin(), assembled.end(), buf.begin()));
    }
}

// ============================================================================
// K-04 — selective repair: withhold chunks {1,3}; missingIndices reports
// exactly {1,3}; deliver them; complete() true; reassembled bytes identical.
// ============================================================================
TEST_CASE("K-04: selective repair of withheld chunks {1,3}") {
    auto cat = conformance::miniCatalog();
    std::array<std::byte, 2048> buf{};
    size_t n = encodeCatalog(cat, buf);
    REQUIRE(n == 733);
    size_t cc = chunkCount(n);
    REQUIRE(cc == 4);

    ChunkReassembler<> reasm;
    reasm.begin(uint16_t(cc), n, /*nowMs=*/0);

    for (uint16_t i = 0; i < cc; ++i) {
        if (i == 1 || i == 3) continue;  // withheld
        std::array<std::byte, 4 + limits::catalog_chunk_payload> chunkBuf{};
        size_t clen = fillChunk(std::span<const std::byte>(buf).first(n), i, chunkBuf);
        REQUIRE(clen > 0);
        REQUIRE(reasm.insert(std::span<const std::byte>(chunkBuf).first(clen), 1));
    }
    CHECK_FALSE(reasm.complete());

    std::array<uint16_t, 8> missing{};
    size_t nMissing = reasm.missingIndices(missing);
    REQUIRE(nMissing == 2);
    CHECK(missing[0] == 1);
    CHECK(missing[1] == 3);

    // Deliver exactly the reported repair set.
    for (size_t m = 0; m < nMissing; ++m) {
        std::array<std::byte, 4 + limits::catalog_chunk_payload> chunkBuf{};
        size_t clen = fillChunk(std::span<const std::byte>(buf).first(n), missing[m], chunkBuf);
        REQUIRE(clen > 0);
        REQUIRE(reasm.insert(std::span<const std::byte>(chunkBuf).first(clen), 2));
    }
    REQUIRE(reasm.complete());
    auto assembled = reasm.assembled();
    REQUIRE(assembled.size() == n);
    CHECK(std::equal(assembled.begin(), assembled.end(), buf.begin()));
}

// ============================================================================
// Bonus (not its own manifest id, but exercises catalog_chunks.hpp's timer
// logic that K-03/K-04 don't touch): the gap and total-abandon thresholds
// from SPEC §8.4 fire at exactly the registry's configured millisecond
// values.
// ============================================================================
TEST_CASE("catalog_chunks: gapElapsed and timedOut fire at their SPEC §8.4 thresholds") {
    ChunkReassembler<> reasm;
    reasm.begin(2, 10, /*nowMs=*/1000);
    CHECK_FALSE(reasm.gapElapsed(1000));
    CHECK_FALSE(reasm.gapElapsed(1000 + limits::catalog_chunk_gap_timeout_ms - 1));
    CHECK(reasm.gapElapsed(1000 + limits::catalog_chunk_gap_timeout_ms));

    CHECK_FALSE(reasm.timedOut(1000 + limits::frag_reassembly_timeout_ms - 1));
    CHECK(reasm.timedOut(1000 + limits::frag_reassembly_timeout_ms));
}

// ============================================================================
// CATALOG_REQ message codec: empty map (full transfer) and {chunks:[...]}
// (selective repair) round-trip.
// ============================================================================
TEST_CASE("CATALOG_REQ: full-transfer (empty map) and selective-repair round-trip") {
    SUBCASE("full transfer encodes to an empty CBOR map") {
        CatalogReqMsg m{};  // full = true by default
        std::array<std::byte, 16> buf{};
        size_t n = encodeCatalogReq(m, buf);
        REQUIRE(n == 1);
        CHECK(buf[0] == B(0xA0));  // map head, 0 pairs: (5<<5)|0

        auto dr = decodeCatalogReq(std::span<const std::byte>(buf).first(n));
        REQUIRE(dr.isOk());
        CHECK(dr.value().full);
        CHECK(dr.value().chunks_count == 0);
    }
    SUBCASE("selective repair round-trips the requested indices") {
        CatalogReqMsg m{};
        m.full = false;
        m.chunks_count = 3;
        m.chunks[0] = 1;
        m.chunks[1] = 3;
        m.chunks[2] = 4;

        std::array<std::byte, 32> buf{};
        size_t n = encodeCatalogReq(m, buf);
        REQUIRE(n > 0);

        auto dr = decodeCatalogReq(std::span<const std::byte>(buf).first(n));
        REQUIRE(dr.isOk());
        CHECK_FALSE(dr.value().full);
        REQUIRE(dr.value().chunks_count == 3);
        CHECK(dr.value().chunks[0] == 1);
        CHECK(dr.value().chunks[1] == 3);
        CHECK(dr.value().chunks[2] == 4);
    }
}

// ============================================================================
// K-05 — static-profile decisions (SPEC §8.5): etag match -> proceed, no
// suppression; mismatch + DegradeGracefully -> proceed with
// controlSuppressed; mismatch + RefuseLoudly -> !proceed.
// ============================================================================
TEST_CASE("K-05: static-profile decision table") {
    std::array<std::byte, 8> etagA = {B(1), B(2), B(3), B(4), B(5), B(6), B(7), B(8)};
    std::array<std::byte, 8> etagB = {B(9), B(9), B(9), B(9), B(9), B(9), B(9), B(9)};

    SUBCASE("etag match -> proceed, nothing suppressed, regardless of policy") {
        for (auto policy : {StaticProfilePolicy::DegradeGracefully, StaticProfilePolicy::RefuseLoudly}) {
            auto d = decideStaticProfile(etagA, etagA, policy);
            CHECK(d.proceed);
            CHECK_FALSE(d.controlSuppressed);
        }
    }
    SUBCASE("mismatch + DegradeGracefully -> proceed degraded, control suppressed") {
        auto d = decideStaticProfile(etagA, etagB, StaticProfilePolicy::DegradeGracefully);
        CHECK(d.proceed);
        CHECK(d.controlSuppressed);
    }
    SUBCASE("mismatch + RefuseLoudly -> refuse outright") {
        auto d = decideStaticProfile(etagA, etagB, StaticProfilePolicy::RefuseLoudly);
        CHECK_FALSE(d.proceed);
        CHECK_FALSE(d.controlSuppressed);
    }
}

// ============================================================================
// CDDL conformance negatives.
// ============================================================================
TEST_CASE("catalog decode: an entry map with BOTH keys 8 and 9 is Malformed") {
    // Hand-built (bypassing encodeCatalog, which never emits both keys on
    // one entry by construction): one entry, map of 9 pairs (1..7, then
    // BOTH 8 [a trivial 1-field layout array] and 9 [a trivial 1-field
    // schema map]) -- a structural violation of catalog.cddl's "exactly one
    // of 8/9" rule that only a hand-crafted message can produce.
    std::array<std::byte, 512> buf{};
    CborWriter w(buf);
    w.arrayHeader(1);
    w.mapHeader(9);
    w.key(1).uintVal(1);       // id
    w.key(2).tstrVal("x");     // name
    w.key(3).uintVal(0);       // class = STATE
    w.key(4).uintVal(0);       // dir = h2c
    w.key(5).uintVal(0);       // access = viewer
    w.key(6).f32Val(0.0f);     // max_rate_hz
    w.key(7).uintVal(0);       // priority = background
    w.key(8).arrayHeader(1);   // layout: [ { one trivial field } ]
    w.mapHeader(4);
    w.key(1).tstrVal("f");
    w.key(2).uintVal(0);       // packed-type = u8
    w.key(3).tstrVal("");
    w.key(4).f32Val(1.0f);
    w.key(9).mapHeader(1);     // schema: { 1 => { one trivial field } }
    w.key(1).mapHeader(3);
    w.key(1).tstrVal("g");
    w.key(2).uintVal(0);       // cbor-type = uint
    w.key(3).tstrVal("");
    REQUIRE_FALSE(w.failed());
    REQUIRE(w.size() > 0);

    Catalog32 out{};
    auto dr = decodeCatalog(std::span<const std::byte>(buf).first(w.size()), out);
    REQUIRE_FALSE(dr.isOk());
    CHECK(dr.error() == DecodeError::Malformed);
}

TEST_CASE("catalog decode: entries out of ascending id order is Malformed") {
    // Two valid single-entry catalogs (id=5, id=3), each encoded on its
    // own, then hand-stitched into one 2-entry catalog with entry order
    // (id=5, id=3) -- descending, which encodeCatalog itself refuses to
    // produce (returns 0), so this ONLY exercises decodeCatalog's own
    // ascending-order enforcement (§8.3).
    auto makeOneEntry = [](uint16_t id) {
        Catalog32 c{};
        c.count = 1;
        CatalogEntry& e = c.entries[0];
        e.id = id;
        e.name = "a";
        e.cls = ChannelClass::STATE;
        e.dir = Direction::h2c;
        e.access = AccessLevel::viewer;
        e.maxRateHz = 0.0f;
        e.defaultPriority = Priority::normal;
        e.fieldCount = 1;
        e.layout[0] = {.name = "f", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f};
        return c;
    };

    std::array<std::byte, 256> bufHigh{}, bufLow{};
    size_t nHigh = encodeCatalog(makeOneEntry(5), bufHigh);
    size_t nLow = encodeCatalog(makeOneEntry(3), bufLow);
    REQUIRE(nHigh > 1);
    REQUIRE(nLow > 1);
    // Both single-entry catalogs share the same 1-byte array header (0x81,
    // count 1 <= 23) -- confirmed so the "+1"/"skip 1 byte" splice below is
    // known-correct rather than an assumption.
    REQUIRE(bufHigh[0] == B(0x81));
    REQUIRE(bufLow[0] == B(0x81));

    std::array<std::byte, 9> hdrBuf{};
    CborWriter hdrW(hdrBuf);
    hdrW.arrayHeader(2);

    std::vector<std::byte> forged;
    forged.insert(forged.end(), hdrBuf.begin(), hdrBuf.begin() + hdrW.size());
    forged.insert(forged.end(), bufHigh.begin() + 1, bufHigh.begin() + nHigh);  // id=5 first
    forged.insert(forged.end(), bufLow.begin() + 1, bufLow.begin() + nLow);     // id=3 second: descending

    Catalog32 out{};
    auto dr = decodeCatalog(std::span<const std::byte>(forged), out);
    REQUIRE_FALSE(dr.isOk());
    CHECK(dr.error() == DecodeError::Malformed);
}
