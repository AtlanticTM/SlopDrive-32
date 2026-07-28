// test_slopsync_catalog — slopsync-core's catalog wire layer:
// wire/sha256.hpp, wire/catalog_codec.hpp, wire/catalog_etag.hpp,
// wire/blob_chunks.hpp, wire/messages/blob_req.hpp, and
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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/conformance/mini_catalog.hpp"
#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/session/shedding.hpp"
#include "slopsync/session/static_profile.hpp"
#include "slopsync/wire/blob_chunks.hpp"
#include "slopsync/wire/catalog_codec.hpp"
#include "slopsync/wire/catalog_etag.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"
#include "slopsync/wire/messages/blob_req.hpp"
#include "slopsync/wire/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "slopsync/channel/log_channel.hpp"

using namespace slopsync;

namespace {
constexpr std::byte B(int x) { return std::byte(uint8_t(x)); }
}  // namespace

// ---- SHA-256 known-answer tests. --------------------------------------------
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

// ---- K-01 -------------------------------------------------------------------
// mini-catalog deterministic encoding: exact bytes (pinned length),
// determinism (encode twice byte-identical; decode -> re-encode
// byte-identical), and hand-derived structural spot-checks.
TEST_CASE("K-01: mini-catalog deterministic encoding") {
    Catalog32 cat;
    conformance::buildMiniCatalog(cat);
    std::array<std::byte, 2048> buf1{};
    std::array<std::byte, 2048> buf2{};

    size_t n1 = encodeCatalog(cat, buf1);
    size_t n2 = encodeCatalog(cat, buf2);
    REQUIRE(n1 > 0);
    REQUIRE(n1 == n2);
    CHECK(std::equal(buf1.begin(), buf1.begin() + n1, buf2.begin()));

    // ---- Hand-derived structural spot-checks --------------------------------
    // catalog = [ * channel-entry ], miniCatalog() has 6 entries: an array
    // head of 6 elements (major type 4, additional-info == count since
    // 6 <= 23) is a single byte: (4<<5)|6 = 0x80|0x06 = 0x86.
    CHECK(buf1[0] == B(0x86));

    // First entry ("safety", id=0x0003): its own map has 8 pairs (keys
    // 1..7 always + key 8 since it's a STATE/layout-class entry) -> map
    // head byte (5<<5)|8 = 0xA0|0x08 = 0xA8. Unmoved by the appended `modes`
    // FIELD: a field lands inside key 8's layout array, not as a new entry key.
    CHECK(buf1[1] == B(0xA8));
    // Key 1 (id): unsigned int head, value 1 <= 23 -> single byte 0x01.
    CHECK(buf1[2] == B(0x01));
    // Value: id = 0x0003, also <= 23 -> single byte 0x03.
    CHECK(buf1[3] == B(0x03));

    // K-01: PIN the total encoded length. Computed once by running this
    // codec over the frozen miniCatalog() fixture and printed during
    // development (see file header's pinning methodology) — any future
    // change to the encoding shifts this number and must fail here.
    // v1.0 base pass: 733 -> 775. The ONE deliberate content change to the
    // frozen fixture is RFC-025c's appended `modes` bitfield8 on 0x0003
    // (verified by removing just that field and watching this pin and K-02's
    // etag return EXACTLY to 733 / 21 CB 26 C9 4F B3 88 B5 with every other
    // M4a change still in place). Re-frozen here per the RFC queue's
    // break-allowed ruling.
    CHECK(n1 == 775);

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

// ---- K-02 -------------------------------------------------------------------
// etag computation over K-01 bytes: exact 8-byte value (pinned),
// changes when catalog content changes, stable across re-encode.
TEST_CASE("K-02: catalogEtag over mini-catalog") {
    Catalog32 cat;
    conformance::buildMiniCatalog(cat);
    std::array<std::byte, 2048> scratch{};
    auto etag = catalogEtag(cat, std::span<std::byte>(scratch));

    // K-02: PIN all 8 bytes (same freeze technique as K-01 — computed once,
    // printed during development, now a hard assertion).
    // v1.0 base pass re-pin: 21 CB 26 C9 4F B3 88 B5 -> F4 A2 8F BB 58 CE D1 6A,
    // moved by RFC-025c's appended `modes` field on 0x0003 and by nothing else.
    std::array<std::byte, 8> expected = {
        B(0xF4), B(0xA2), B(0x8F), B(0xBB), B(0x58), B(0xCE), B(0xD1), B(0x6A),
    };
    CHECK(etag == expected);

    // Mutating a copy's maxRateHz must change the etag (§8.3: etag covers
    // "everything in §8.1", which includes max_rate_hz).
    Catalog32 mutated;
    conformance::buildMiniCatalog(mutated);
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

// ---- K-03 -------------------------------------------------------------------
// chunking at 192B: chunk_count for the encoded size, byte-exact
// reassembly in order and in reverse order (order independence).
TEST_CASE("K-03: chunking and reassembly") {
    Catalog32 cat;
    conformance::buildMiniCatalog(cat);
    std::array<std::byte, 2048> buf{};
    size_t n = encodeCatalog(cat, buf);
    REQUIRE(n == 775);  // pinned by K-01; chunk math below depends on it

    // ceil(775 / 192) = 5 chunks: 192, 192, 192, 192, 7.
    size_t cc = chunkCount(n);
    CHECK(cc == 5);

    SUBCASE("in-order delivery reassembles byte-exact") {
        ChunkReassembler<> reasm;
        reasm.begin(BlobId{}, uint16_t(cc), n, /*nowMs=*/0);
        for (uint16_t i = 0; i < cc; ++i) {
            std::array<std::byte, kBlobChunkHeaderBytes + limits::catalog_chunk_payload> chunkBuf{};
            size_t clen = fillBlobChunk(BlobId{}, std::span<const std::byte>(buf).first(n), i, chunkBuf);
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
        reasm.begin(BlobId{}, uint16_t(cc), n, /*nowMs=*/0);
        for (uint16_t i = uint16_t(cc); i-- > 0;) {
            std::array<std::byte, kBlobChunkHeaderBytes + limits::catalog_chunk_payload> chunkBuf{};
            size_t clen = fillBlobChunk(BlobId{}, std::span<const std::byte>(buf).first(n), i, chunkBuf);
            REQUIRE(clen > 0);
            CHECK(reasm.insert(std::span<const std::byte>(chunkBuf).first(clen), 1));
        }
        REQUIRE(reasm.complete());
        auto assembled = reasm.assembled();
        REQUIRE(assembled.size() == n);
        CHECK(std::equal(assembled.begin(), assembled.end(), buf.begin()));
    }
}

// ---- K-04 -------------------------------------------------------------------
// selective repair: withhold chunks {1,3}; missingIndices reports
// exactly {1,3}; deliver them; complete() true; reassembled bytes identical.
TEST_CASE("K-04: selective repair of withheld chunks {1,3}") {
    Catalog32 cat;
    conformance::buildMiniCatalog(cat);
    std::array<std::byte, 2048> buf{};
    size_t n = encodeCatalog(cat, buf);
    REQUIRE(n == 775);
    size_t cc = chunkCount(n);
    REQUIRE(cc == 5);

    ChunkReassembler<> reasm;
    reasm.begin(BlobId{}, uint16_t(cc), n, /*nowMs=*/0);

    for (uint16_t i = 0; i < cc; ++i) {
        if (i == 1 || i == 3) continue;  // withheld
        std::array<std::byte, kBlobChunkHeaderBytes + limits::catalog_chunk_payload> chunkBuf{};
        size_t clen = fillBlobChunk(BlobId{}, std::span<const std::byte>(buf).first(n), i, chunkBuf);
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
        std::array<std::byte, kBlobChunkHeaderBytes + limits::catalog_chunk_payload> chunkBuf{};
        size_t clen = fillBlobChunk(BlobId{}, std::span<const std::byte>(buf).first(n), missing[m], chunkBuf);
        REQUIRE(clen > 0);
        REQUIRE(reasm.insert(std::span<const std::byte>(chunkBuf).first(clen), 2));
    }
    REQUIRE(reasm.complete());
    auto assembled = reasm.assembled();
    REQUIRE(assembled.size() == n);
    CHECK(std::equal(assembled.begin(), assembled.end(), buf.begin()));
}

// ---- Bonus: blob_chunks.hpp timer logic -------------------------------------
// Not its own manifest id, but exercises timer logic that K-03/K-04 don't
// touch: the gap and total-abandon thresholds from SPEC §8.4 fire at
// exactly the registry's configured millisecond values.
TEST_CASE("blob_chunks: gapElapsed and timedOut fire at their SPEC §8.4 thresholds") {
    ChunkReassembler<> reasm;
    reasm.begin(BlobId{}, 2, 10, /*nowMs=*/1000);
    CHECK_FALSE(reasm.gapElapsed(1000));
    CHECK_FALSE(reasm.gapElapsed(1000 + limits::catalog_chunk_gap_timeout_ms - 1));
    CHECK(reasm.gapElapsed(1000 + limits::catalog_chunk_gap_timeout_ms));

    CHECK_FALSE(reasm.timedOut(1000 + limits::frag_reassembly_timeout_ms - 1));
    CHECK(reasm.timedOut(1000 + limits::frag_reassembly_timeout_ms));
}

// ---- BLOB_REQ message codec -------------------------------------------------
// empty map (full transfer) and {chunks:[...]}
// (selective repair) round-trip.
TEST_CASE("BLOB_REQ: full-transfer (empty map) and selective-repair round-trip") {
    SUBCASE("full transfer encodes to an empty CBOR map") {
        BlobReqMsg m{};  // full = true by default
        std::array<std::byte, 16> buf{};
        size_t n = encodeBlobReq(m, buf);
        REQUIRE(n == 1);
        CHECK(buf[0] == B(0xA0));  // map head, 0 pairs: (5<<5)|0

        auto dr = decodeBlobReq(std::span<const std::byte>(buf).first(n));
        REQUIRE(dr.isOk());
        CHECK(dr.value().full);
        CHECK(dr.value().chunks_count == 0);
    }
    SUBCASE("selective repair round-trips the requested indices") {
        BlobReqMsg m{};
        m.full = false;
        m.chunks_count = 3;
        m.chunks[0] = 1;
        m.chunks[1] = 3;
        m.chunks[2] = 4;

        std::array<std::byte, 32> buf{};
        size_t n = encodeBlobReq(m, buf);
        REQUIRE(n > 0);

        auto dr = decodeBlobReq(std::span<const std::byte>(buf).first(n));
        REQUIRE(dr.isOk());
        CHECK_FALSE(dr.value().full);
        REQUIRE(dr.value().chunks_count == 3);
        CHECK(dr.value().chunks[0] == 1);
        CHECK(dr.value().chunks[1] == 3);
        CHECK(dr.value().chunks[2] == 4);
    }
}

// ---- K-05 -------------------------------------------------------------------
// static-profile decisions (SPEC §8.5): etag match -> proceed, no
// suppression; mismatch + DegradeGracefully -> proceed with
// controlSuppressed; mismatch + RefuseLoudly -> !proceed.
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

// ---- CDDL conformance negatives. --------------------------------------------
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
    auto makeOneEntry = [](Catalog32& c, uint16_t id) {
        c.clear();
        c.addEntry({.id = id, .name = "a",
                    .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::normal});
        c.addLayoutField({.name = "f", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});
    };

    Catalog32 one;
    std::array<std::byte, 256> bufHigh{}, bufLow{};
    makeOneEntry(one, 5);
    size_t nHigh = encodeCatalog(one, bufHigh);
    makeOneEntry(one, 3);
    size_t nLow = encodeCatalog(one, bufLow);
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

// ---- M2b --------------------------------------------------------------------
// RFC-009 annotations, interned labels, STORE entries, the raised
// kMaxFields, and the per-entry byte cap.
//
// The BASELINE for all of it is K-01/K-02 above: the frozen fixture carries no
// annotations, every annotation key is OPTIONAL, and §5.3 does not emit absent
// optional keys — so its 775-byte length and its etag are UNMOVED by this whole
// block. If either pin ever shifts, something optional was made mandatory.

namespace {

// Compares two layout fields INCLUDING their interned label arrays, which live
// in different catalogs and therefore at different pool offsets — the refs
// cannot be compared, only what they resolve to.
void checkLayoutFieldEqual(const Catalog32& ca, const LayoutField& a,
                           const Catalog32& cb, const LayoutField& b) {
    CHECK(a.name == b.name);
    CHECK(a.type == b.type);
    CHECK(a.unit == b.unit);
    CHECK(a.scale == b.scale);
    CHECK(a.hasMin == b.hasMin);
    CHECK(a.hasMax == b.hasMax);
    if (a.hasMin) CHECK(a.min == b.min);
    if (a.hasMax) CHECK(a.max == b.max);
    CHECK(a.hasSettingKey == b.hasSettingKey);
    CHECK(a.settingKey == b.settingKey);
    CHECK(a.dflt == b.dflt);
    CHECK(a.group == b.group);
    CHECK(a.desc == b.desc);
    CHECK(a.role == b.role);
    CHECK(a.hasStep == b.hasStep);
    if (a.hasStep) CHECK(a.step == b.step);
    CHECK(a.flags == b.flags);

    auto bitsA = ca.bitLabels(a), bitsB = cb.bitLabels(b);
    REQUIRE(bitsA.size() == bitsB.size());
    for (size_t i = 0; i < bitsA.size(); ++i) CHECK(bitsA[i] == bitsB[i]);

    auto optA = ca.optionLabels(a), optB = cb.optionLabels(b);
    REQUIRE(optA.size() == optB.size());
    for (size_t i = 0; i < optA.size(); ++i) CHECK(optA[i] == optB[i]);
}

void checkSchemaFieldEqual(const Catalog32& ca, const SchemaField& a,
                           const Catalog32& cb, const SchemaField& b) {
    CHECK(a.key == b.key);
    CHECK(a.name == b.name);
    CHECK(a.type == b.type);
    CHECK(a.unit == b.unit);
    CHECK(a.hasMin == b.hasMin);
    CHECK(a.hasMax == b.hasMax);
    if (a.hasMin) CHECK(a.min == b.min);
    if (a.hasMax) CHECK(a.max == b.max);
    CHECK(a.dflt == b.dflt);
    CHECK(a.group == b.group);
    CHECK(a.desc == b.desc);
    CHECK(a.role == b.role);
    CHECK(a.hasStep == b.hasStep);
    if (a.hasStep) CHECK(a.step == b.step);
    CHECK(a.flags == b.flags);
    CHECK(a.hasAccess == b.hasAccess);
    if (a.hasAccess) CHECK(a.access == b.access);
    CHECK(a.hasOptionAccess == b.hasOptionAccess);

    auto optA = ca.optionLabels(a), optB = cb.optionLabels(b);
    REQUIRE(optA.size() == optB.size());
    for (size_t i = 0; i < optA.size(); ++i) CHECK(optA[i] == optB[i]);

    auto accA = ca.optionAccess(a), accB = cb.optionAccess(b);
    REQUIRE(accA.size() == accB.size());
    for (size_t i = 0; i < accA.size(); ++i) CHECK(accA[i] == accB[i]);
}

// A catalog exercising EVERY M2b addition at once: entry-level category /
// category_label / setting_channel, the full layout-field annotation block,
// interned bit labels (WITH a hole, so index-alignment is actually tested),
// interned select options, and a schema entry carrying per-op access and
// index-aligned per-option access.
void buildAnnotatedCatalog(Catalog32& c) {
    c.clear();

    c.addEntry({.id = 0x0081, .name = "machine-config",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::normal,
                .hasCategory = true, .category = ui_categories::limits,
                .hasSettingChannel = true, .settingChannel = 0x0101});
    // A SETTING: setting_key present -> writable through 0x0101 key 3.
    c.addLayoutField({.name = "user_speed", .type = PackedFieldType::f32, .unit = "mm/s",
                      .scale = 1.0f, .hasMin = true, .hasMax = true, .min = 0.0f, .max = 600.0f,
                      .dflt = SettingDefault::ofFloat(250.0f),
                      .group = "User limits",
                      .desc = "Speed ceiling for manual moves. Never a target.",
                      .role = field_roles::limit_user_speed,
                      .step = 5.0f,
                      .settingKey = 3, .flags = setting_flags::advanced,
                      .hasSettingKey = true, .hasStep = true});
    // READ-ONLY: no setting_key -> effective/telemetry, never written back.
    c.addLayoutField({.name = "max_rail", .type = PackedFieldType::f32, .unit = "mm",
                      .scale = 1.0f, .role = field_roles::telemetry_position});
    // Sparse bit labels: bit 1 deliberately unnamed, so the pool's
    // index-alignment (and the encoder's sparse bits map) is exercised.
    c.addBitfieldField({.name = "flags", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f, .desc = "Live machine flags."},
                       {"homed", "", "gen_running"});
    // Single-select: wire value = index into the interned option array.
    c.addSelectField({.name = "backend", .type = PackedFieldType::u8, .unit = "",
                      .scale = 1.0f, .dflt = SettingDefault::ofInt(1),
                      .group = "Motion", .settingKey = 9, .hasSettingKey = true},
                     {"stepper", "servo", "sim"});
    // A string setting (RFC-026) with a tstr default.
    c.addLayoutField({.name = "hub_name", .type = PackedFieldType::str16, .unit = "",
                      .scale = 1.0f, .dflt = SettingDefault::ofTstr("slopdrive"),
                      .role = field_roles::identity_name,
                      .settingKey = 11, .hasSettingKey = true});

    c.addEntry({.id = 0x0101, .name = "config-set",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 10.0f,
                .defaultPriority = Priority::normal,
                .hasCategory = true, .category = 200,
                .categoryLabel = "SlopDrive"});
    c.addSchemaField({.key = 3, .name = "user_speed", .type = CborFieldType::f32_t, .unit = "mm/s",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 600.0f,
                      .dflt = SettingDefault::ofFloat(250.0f),
                      .group = "User limits", .desc = "Applied post-clamp.",
                      .role = field_roles::limit_user_speed, .step = 5.0f,
                      .flags = setting_flags::restart_required,
                      .access = AccessLevel::configure, .hasAccess = true, .hasStep = true});
    // The op-style intent RFC-019/RFC-027 care about: ONE enum-valued field
    // whose per-OPTION access varies (stop is role-exempt, takeover is not).
    c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = "",
                            .role = "action.safety"},
                           {"stop", "hold", "takeover"},
                           {AccessLevel::watch, AccessLevel::control, AccessLevel::configure});
    c.addSchemaField({.key = 4, .name = "enable", .type = CborFieldType::bool_t, .unit = "",
                      .dflt = SettingDefault::ofBool(true)});
}

}  // namespace

TEST_CASE("M2b: a fully annotated catalog round-trips through the codec") {
    static Catalog32 cat;
    buildAnnotatedCatalog(cat);
    REQUIRE(cat.ok());
    REQUIRE(cat.count == 2);

    static std::array<std::byte, 8192> buf{};
    size_t n = encodeCatalog(cat, buf);
    REQUIRE(n > 0);

    static Catalog32 back;
    auto dr = decodeCatalog(std::span<const std::byte>(buf).first(n), back);
    REQUIRE(dr.isOk());
    REQUIRE(back.count == cat.count);

    for (uint16_t i = 0; i < cat.count; ++i) {
        const CatalogEntry& a = cat.entries[i];
        const CatalogEntry& b = back.entries[i];
        CHECK(a.id == b.id);
        CHECK(a.name == b.name);
        CHECK(a.cls == b.cls);
        CHECK(a.dir == b.dir);
        CHECK(a.access == b.access);
        CHECK(a.maxRateHz == b.maxRateHz);
        CHECK(a.defaultPriority == b.defaultPriority);
        CHECK(a.fieldCount == b.fieldCount);
        CHECK(a.hasCategory == b.hasCategory);
        CHECK(a.category == b.category);
        CHECK(a.categoryLabel == b.categoryLabel);
        CHECK(a.hasSettingChannel == b.hasSettingChannel);
        CHECK(a.settingChannel == b.settingChannel);

        auto la = cat.layoutFields(a), lb = back.layoutFields(b);
        REQUIRE(la.size() == lb.size());
        for (size_t f = 0; f < la.size(); ++f) checkLayoutFieldEqual(cat, la[f], back, lb[f]);

        // Schema fields are matched BY KEY, not by index: the encoder sorts the
        // sub-map ascending by key (§5.3), so a decoded catalog's schema-pool
        // order is key order, not the authoring order it went in as.
        auto sa = cat.schemaFields(a), sb = back.schemaFields(b);
        REQUIRE(sa.size() == sb.size());
        for (const SchemaField& fa : sa) {
            const SchemaField* fb = nullptr;
            for (const SchemaField& cand : sb)
                if (cand.key == fa.key) fb = &cand;
            REQUIRE(fb != nullptr);
            checkSchemaFieldEqual(cat, fa, back, *fb);
        }
    }

    // Decode -> re-encode must reproduce the exact same bytes: the interning is
    // storage, not content, and index-aligned bit holes survive the trip.
    static std::array<std::byte, 8192> buf2{};
    size_t n2 = encodeCatalog(back, buf2);
    REQUIRE(n2 == n);
    CHECK(std::equal(buf.begin(), buf.begin() + n, buf2.begin()));
}

TEST_CASE("M2b: interned labels — index alignment, sparsity, and per-option access") {
    static Catalog32 cat;
    buildAnnotatedCatalog(cat);
    const CatalogEntry& state = cat.entries[0];
    auto layout = cat.layoutFields(state);
    REQUIRE(layout.size() == 5);

    // "flags": {"homed", "", "gen_running"} — count stops at the last NAMED
    // bit (3), the middle hole is preserved by index, and bit 2 is NOT bit 1.
    auto bits = cat.bitLabels(layout[2]);
    REQUIRE(bits.size() == 3);
    CHECK(bits[0] == "homed");
    CHECK(bits[1].empty());
    CHECK(bits[2] == "gen_running");

    // "backend": options are dense and their INDEX is the wire value.
    auto opts = cat.optionLabels(layout[3]);
    REQUIRE(opts.size() == 3);
    CHECK(opts[0] == "stepper");
    CHECK(opts[2] == "sim");

    // Un-annotated fields resolve to EMPTY spans, not to slot 0 of the pool.
    CHECK(cat.bitLabels(layout[0]).empty());
    CHECK(cat.optionLabels(layout[0]).empty());

    const CatalogEntry& intent = cat.entries[1];
    auto schema = cat.schemaFields(intent);
    REQUIRE(schema.size() == 3);
    const SchemaField* op = nullptr;
    for (const SchemaField& f : schema)
        if (f.key == 1) op = &f;
    REQUIRE(op != nullptr);
    auto acc = cat.optionAccess(*op);
    REQUIRE(acc.size() == 3);
    CHECK(acc[0] == AccessLevel::watch);
    CHECK(acc[1] == AccessLevel::control);
    CHECK(acc[2] == AccessLevel::configure);
    // Index-aligned with the labels, by construction.
    CHECK(cat.optionLabels(*op).size() == acc.size());
}

TEST_CASE("M2b: a STORE-class entry round-trips (RFC-021)") {
    static Catalog32 cat;
    cat.clear();
    cat.addEntry({.id = channels::paired_devices, .name = "paired-devices",
                  .cls = ChannelClass::STORE, .dir = Direction::h2c,
                  .access = AccessLevel::configure, .maxRateHz = 0.0f,
                  .defaultPriority = Priority::normal});
    cat.addStoreDescriptor({.storeId = 1, .kind = "trust.ledger",
                            .capacity = uint16_t(limits::paired_devices_max),
                            .perItemMax = 192, .nameMax = 24});
    cat.addEntry({.id = 0x0110, .name = "patterns",
                  .cls = ChannelClass::STORE, .dir = Direction::h2c,
                  .access = AccessLevel::control, .maxRateHz = 0.0f,
                  .defaultPriority = Priority::background});
    cat.addStoreDescriptor({.storeId = 2, .kind = "pattern.frayd",
                            .capacity = uint16_t(limits::preset_capacity_min),
                            .perItemMax = limits::preset_item_max_bytes, .nameMax = 32});
    REQUIRE(cat.ok());

    // A STORE entry draws from the STORE pool — not layout, not schema.
    CHECK(cat.entries[0].form() == FieldForm::Store);
    CHECK_FALSE(cat.entries[0].usesLayout());
    CHECK_FALSE(cat.entries[0].usesSchema());
    CHECK(cat.layoutFields(cat.entries[0]).empty());
    CHECK(cat.schemaFields(cat.entries[0]).empty());
    REQUIRE(cat.storeDescriptor(cat.entries[0]) != nullptr);

    static std::array<std::byte, 1024> buf{};
    size_t n = encodeCatalog(cat, buf);
    REQUIRE(n > 0);

    static Catalog32 back;
    REQUIRE(decodeCatalog(std::span<const std::byte>(buf).first(n), back).isOk());
    REQUIRE(back.count == 2);
    for (uint16_t i = 0; i < 2; ++i) {
        const StoreDescriptor* a = cat.storeDescriptor(cat.entries[i]);
        const StoreDescriptor* b = back.storeDescriptor(back.entries[i]);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(a->storeId == b->storeId);
        CHECK(a->kind == b->kind);
        CHECK(a->capacity == b->capacity);
        CHECK(a->perItemMax == b->perItemMax);
        CHECK(a->nameMax == b->nameMax);
    }

    static std::array<std::byte, 1024> buf2{};
    size_t n2 = encodeCatalog(back, buf2);
    REQUIRE(n2 == n);
    CHECK(std::equal(buf.begin(), buf.begin() + n, buf2.begin()));
}

TEST_CASE("M2b: an entry wider than the old 8-field bound encodes and decodes") {
    // The whole point of raising CatalogEntry::kMaxFields: the ~50-field
    // flattened advanced-pattern channel. STREAM class so the §9.1 242 B STATE
    // fit rule (a conformance concern, not a codec one) isn't in play.
    static_assert(CatalogEntry::kMaxFields >= 50, "M2b raised this to hold the wide ap channel");

    static Catalog32 cat;
    cat.clear();
    cat.addEntry({.id = 0x0120, .name = "ap-params",
                  .cls = ChannelClass::STREAM, .dir = Direction::h2c,
                  .access = AccessLevel::watch, .maxRateHz = 10.0f,
                  .defaultPriority = Priority::background});
    for (int i = 0; i < 50; ++i) {
        cat.addLayoutField({.name = "p", .type = PackedFieldType::u16, .unit = "",
                            .scale = 1.0f});
    }
    REQUIRE(cat.ok());
    CHECK(cat.entries[0].fieldCount == 50);

    static std::array<std::byte, 4096> buf{};
    size_t n = encodeCatalog(cat, buf);
    REQUIRE(n > 0);

    static Catalog32 back;
    REQUIRE(decodeCatalog(std::span<const std::byte>(buf).first(n), back).isOk());
    REQUIRE(back.count == 1);
    CHECK(back.entries[0].fieldCount == 50);
    CHECK(back.layoutFields(back.entries[0]).size() == 50);

    // 40 schema fields with DESCENDING keys: the encoder's sort scratch is
    // sized by kMaxFields, so widening it must not have broken the
    // ascending-key rule (the writer refuses out-of-order keys outright).
    static Catalog32 sch;
    sch.clear();
    sch.addEntry({.id = 0x0121, .name = "ap-set",
                  .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                  .access = AccessLevel::control, .maxRateHz = 5.0f,
                  .defaultPriority = Priority::normal});
    for (int i = 0; i < 40; ++i) {
        sch.addSchemaField({.key = uint8_t(40 - i), .name = "k",
                            .type = CborFieldType::f32_t, .unit = ""});
    }
    REQUIRE(sch.ok());
    static std::array<std::byte, 4096> sbuf{};
    size_t sn = encodeCatalog(sch, sbuf);
    REQUIRE(sn > 0);
    static Catalog32 sback;
    REQUIRE(decodeCatalog(std::span<const std::byte>(sbuf).first(sn), sback).isOk());
    CHECK(sback.entries[0].fieldCount == 40);
}

TEST_CASE("M2b: an entry over catalog_max_entry_bytes is refused by the encoder") {
    // A 64-field entry with full-length desc/group/role strings — exactly the
    // shape the feasibility pass flagged as encoding to 8-10 KB, which RFC-028
    // forbids as an unbounded per-entry decode buffer.
    static const char kLongDesc[] =
        "A deliberately long description, so that a wide entry blows the "
        "per-entry byte cap the feasibility pass introduced. Padding.";
    static_assert(sizeof(kLongDesc) - 1 <= limits::desc_max_bytes, "desc must stay legal per-field");

    static Catalog32 cat;
    cat.clear();
    cat.addEntry({.id = 0x0130, .name = "ap-params-fat",
                  .cls = ChannelClass::STREAM, .dir = Direction::h2c,
                  .access = AccessLevel::watch, .maxRateHz = 1.0f,
                  .defaultPriority = Priority::background});
    for (size_t i = 0; i < CatalogEntry::kMaxFields; ++i) {
        cat.addLayoutField({.name = "parameter_with_a_name", .type = PackedFieldType::u16,
                            .unit = "mm", .scale = 1.0f,
                            .group = "A reasonably long group",
                            .desc = kLongDesc,
                            .role = "telemetry.position"});
    }
    REQUIRE(cat.ok());

    // `out` is far larger than the cap, so a 0 return can ONLY be the cap
    // itself — not "the buffer ran out".
    static std::array<std::byte, 65536> big{};
    CHECK(encodeCatalog(cat, big) == 0);

    // The same shape with few enough fields to fit encodes fine — proving the
    // refusal is the SIZE rule and not the field count.
    static Catalog32 slim;
    slim.clear();
    slim.addEntry({.id = 0x0130, .name = "ap-params-slim",
                   .cls = ChannelClass::STREAM, .dir = Direction::h2c,
                   .access = AccessLevel::watch, .maxRateHz = 1.0f,
                   .defaultPriority = Priority::background});
    for (size_t i = 0; i < 12; ++i) {
        slim.addLayoutField({.name = "parameter_with_a_name", .type = PackedFieldType::u16,
                             .unit = "mm", .scale = 1.0f,
                             .group = "A reasonably long group",
                             .desc = kLongDesc,
                             .role = "telemetry.position"});
    }
    size_t n = encodeCatalog(slim, big);
    REQUIRE(n > 0);
    CHECK(n <= limits::catalog_max_entry_bytes + 8);  // + the outer array header
}

// ---- replay_depth (entry key 13, RFC-017) -----------------------------------
// the last CDDL<->struct gap closed.
// It was in catalog.cddl but not in the data model, so a catalog could declare
// it and the reference implementation would silently discard it on the way
// through. Presence of this key IS the opt-in that makes a channel the one
// sanctioned exception to §9.4's no-replay rule.
TEST_CASE("catalog codec: replay_depth (key 13) round-trips and sorts between 12 and 14") {
    Catalog32 cat;
    cat.clear();
    REQUIRE(addLogChannel(cat, /*replayDepth=*/32));
    REQUIRE_FALSE(cat.overflow);

    std::array<std::byte, 2048> buf{};
    size_t n = encodeCatalog(cat, buf);
    REQUIRE(n > 0);

    Catalog32 back;
    auto r = decodeCatalog(std::span<const std::byte>(buf.data(), n), back);
    REQUIRE(r.isOk());

    const CatalogEntry* e = back.find(channels::log);
    REQUIRE(e);
    CHECK(e->cls == ChannelClass::EVENT);
    CHECK(e->hasReplayDepth);
    CHECK(e->replayDepth == 32);
    CHECK(e->defaultPriority == Priority::background);
    CHECK(e->access == AccessLevel::watch);

    // Absent replay_depth is the §9.4 default: events are edges, never replayed.
    Catalog32 plain;
    plain.clear();
    REQUIRE(addLogChannel(plain, /*replayDepth=*/0));
    std::array<std::byte, 2048> pbuf{};
    size_t pn = encodeCatalog(plain, pbuf);
    REQUIRE(pn > 0);
    Catalog32 pback;
    REQUIRE(decodeCatalog(std::span<const std::byte>(pbuf.data(), pn), pback).isOk());
    const CatalogEntry* pe = pback.find(channels::log);
    REQUIRE(pe);
    CHECK_FALSE(pe->hasReplayDepth);

    // The annotated encoding is strictly longer — key 13 really is on the wire.
    CHECK(n > pn);
}

// ---- stream_kind (entry key 15, RFC-014/023) --------------------------------
// the explicit, registered catalog
// property that REPLACES the M5 unit-string heuristic (isTimeUnit() /
// isSegmentLayout(), deleted from channel/catalog.hpp by this milestone: a
// STREAM channel is segment-class iff `stream_kind` == stream_kinds::segments,
// full stop, no inspection of layout field units). Absent means samples (0),
// the default, which is exactly why the K-01/K-02 pins above are UNMOVED —
// the mini-catalog fixture declares no stream_kind, so §5.3's "never emit an
// absent optional key" rule keeps its bytes identical to before this key
// existed.
TEST_CASE("catalog codec: stream_kind (key 15) classifies segment-class STREAM channels and round-trips") {
    Catalog32 cat;
    cat.clear();

    // A STREAM entry explicitly marked segments: non-decimable.
    cat.addEntry({.id = 0x0090, .name = "seg-ch",
                  .cls = ChannelClass::STREAM, .dir = Direction::c2h,
                  .access = AccessLevel::control, .maxRateHz = 50.0f,
                  .defaultPriority = Priority::elevated,
                  .streamKind = stream_kinds::segments});
    cat.addLayoutField({.name = "target",      .type = PackedFieldType::u16, .unit = "norm", .scale = 10000.0f});
    cat.addLayoutField({.name = "duration_ms", .type = PackedFieldType::u16, .unit = "ms",    .scale = 1.0f});

    // A STREAM entry left at the DEFAULT (no stream_kind authored at all):
    // decimable, per the absent-means-samples rule.
    cat.addEntry({.id = 0x0091, .name = "pts-ch",
                  .cls = ChannelClass::STREAM, .dir = Direction::c2h,
                  .access = AccessLevel::control, .maxRateHz = 200.0f,
                  .defaultPriority = Priority::elevated});
    cat.addLayoutField({.name = "target", .type = PackedFieldType::u16, .unit = "norm", .scale = 10000.0f});
    REQUIRE(cat.ok());

    const CatalogEntry* seg = cat.find(0x0090);
    const CatalogEntry* pts = cat.find(0x0091);
    REQUIRE(seg);
    REQUIRE(pts);
    CHECK(seg->streamKind == stream_kinds::segments);
    CHECK(pts->streamKind == stream_kinds::samples);
    CHECK(cat.isSegmentClass(*seg));
    CHECK_FALSE(cat.isSegmentClass(*pts));

    // Non-decimable, in practice: the shedding table collapses segment-class
    // to Send/Drop only, never any Decimate/ConflateHard variant, at every
    // priority and congestion level (§10.4's segment exception).
    for (uint8_t level = 0; level <= 2; ++level) {
        for (Priority pr : {Priority::background, Priority::normal, Priority::elevated, Priority::critical}) {
            ShedDecision d = shedDecision(pr, ChannelClass::STREAM, level, /*segmentClass=*/true);
            CHECK((d == ShedDecision::Send || d == ShedDecision::Drop));
        }
    }
    // The default-kind (samples) channel IS decimable under congestion.
    CHECK(shedDecision(Priority::normal, ChannelClass::STREAM, 1, /*segmentClass=*/false) == ShedDecision::Decimate2x);

    // The property survives an encode -> decode round trip.
    std::array<std::byte, 2048> buf{};
    size_t n = encodeCatalog(cat, buf);
    REQUIRE(n > 0);

    Catalog32 back{};
    auto dr = decodeCatalog(std::span<const std::byte>(buf).first(n), back);
    REQUIRE(dr.isOk());

    const CatalogEntry* segBack = back.find(0x0090);
    const CatalogEntry* ptsBack = back.find(0x0091);
    REQUIRE(segBack);
    REQUIRE(ptsBack);
    CHECK(segBack->streamKind == stream_kinds::segments);
    CHECK(ptsBack->streamKind == stream_kinds::samples);
    CHECK(back.isSegmentClass(*segBack));
    CHECK_FALSE(back.isSegmentClass(*ptsBack));

    // Re-encoding the decoded copy is byte-identical: the omit-when-default
    // rule round-trips, it doesn't just happen to decode right once.
    std::array<std::byte, 2048> buf2{};
    size_t n2 = encodeCatalog(back, buf2);
    REQUIRE(n2 == n);
    CHECK(std::equal(buf.begin(), buf.begin() + n, buf2.begin()));

    // The samples-kind entry (0x0091) omits key 15 entirely: its encoding is
    // no longer than the exact same entry with a `streamKind` field that
    // doesn't exist yet would be. Structural proof: re-clear the field to a
    // sentinel value and confirm ONLY entries with a non-default streamKind
    // grow the wire size, by comparing against a catalog where 0x0090 is also
    // left at the default.
    Catalog32 allDefault{};
    allDefault.clear();
    allDefault.addEntry({.id = 0x0090, .name = "seg-ch",
                         .cls = ChannelClass::STREAM, .dir = Direction::c2h,
                         .access = AccessLevel::control, .maxRateHz = 50.0f,
                         .defaultPriority = Priority::elevated});
    allDefault.addLayoutField({.name = "target",      .type = PackedFieldType::u16, .unit = "norm", .scale = 10000.0f});
    allDefault.addLayoutField({.name = "duration_ms", .type = PackedFieldType::u16, .unit = "ms",    .scale = 1.0f});
    allDefault.addEntry({.id = 0x0091, .name = "pts-ch",
                         .cls = ChannelClass::STREAM, .dir = Direction::c2h,
                         .access = AccessLevel::control, .maxRateHz = 200.0f,
                         .defaultPriority = Priority::elevated});
    allDefault.addLayoutField({.name = "target", .type = PackedFieldType::u16, .unit = "norm", .scale = 10000.0f});
    REQUIRE(allDefault.ok());
    std::array<std::byte, 2048> bufDefault{};
    size_t nDefault = encodeCatalog(allDefault, bufDefault);
    REQUIRE(nDefault > 0);
    CHECK(n > nDefault);  // key 15 on 0x0090 really is on the wire only when non-default
}

// ---- M4a --------------------------------------------------------------------
// authoring guard: an EMPTY option label is an authoring error, caught
// at build time instead of at a client's decoder.
//
// Found while adding index-aligned `option_access` to a device catalog whose
// enum values start at 1: the encoder happily wrote a zero-length option
// label, the CDDL says `tstr .size (1..24)`, and decodeOptions() rejects it
// as Malformed — so the hub would have shipped a catalog NO CONFORMING CLIENT
// COULD DECODE, discoverable only when a real client failed to sync. The
// asymmetry is now closed on the authoring side.
//
// Note the deliberate contrast with BIT labels, which MAY be empty: an
// unnamed bit is meaningful ("bit 5 has no name"), an unnamed option is not
// (there is no way to offer a choice with no label).
TEST_CASE("M4a: an empty OPTION label is rejected at authoring time") {
    SUBCASE("schema-field select") {
        Catalog32 c;
        c.clear();
        c.addEntry({.id = 0x0200, .name = "ops", .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                    .access = AccessLevel::control, .maxRateHz = 5.0f,
                    .defaultPriority = Priority::normal});
        CHECK_FALSE(c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = ""},
                                           {"", "go", "stop"}));
        CHECK_FALSE(c.ok());   // latched: the whole catalog is refused, not one field
    }

    SUBCASE("layout-field select") {
        Catalog32 c;
        c.clear();
        c.addEntry({.id = 0x0200, .name = "mode", .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::normal});
        CHECK_FALSE(c.addSelectField({.name = "backend", .type = PackedFieldType::u8, .unit = "",
                                      .scale = 1.0f},
                                     {"stepper", "", "servo"}));
        CHECK_FALSE(c.ok());
    }

    SUBCASE("a fully-named option list still authors, encodes and DECODES") {
        Catalog32 c;
        c.clear();
        c.addEntry({.id = 0x0200, .name = "ops", .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                    .access = AccessLevel::watch, .maxRateHz = 5.0f,
                    .defaultPriority = Priority::critical});
        REQUIRE(c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = ""},
                                       {"reserved", "go", "stop"},
                                       {AccessLevel::control, AccessLevel::control, AccessLevel::watch}));
        REQUIRE(c.ok());

        std::array<std::byte, 2048> buf{};
        size_t n = encodeCatalog(c, buf);
        REQUIRE(n > 0);
        Catalog32 back{};
        REQUIRE(decodeCatalog(std::span<const std::byte>(buf).first(n), back).isOk());

        const CatalogEntry* e = back.find(0x0200);
        REQUIRE(e != nullptr);
        auto fields = back.schemaFields(*e);
        REQUIRE(fields.size() == 1);
        REQUIRE(fields[0].hasOptionAccess);
        auto acc = back.optionAccess(fields[0]);
        REQUIRE(acc.size() == 3);
        CHECK(acc[0] == AccessLevel::control);
        CHECK(acc[2] == AccessLevel::watch);
    }

    SUBCASE("bitfield BIT labels may still be empty — the deliberate contrast") {
        Catalog32 c;
        c.clear();
        c.addEntry({.id = 0x0200, .name = "flags", .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::normal});
        REQUIRE(c.addBitfieldField({.name = "w", .type = PackedFieldType::bitfield8, .unit = "flag",
                                    .scale = 1.0f},
                                   {"a", "", "c"}));
        CHECK(c.ok());
    }
}
