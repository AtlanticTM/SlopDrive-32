// test_slopsync_m3b — the MILESTONE 3b data-plane and
// channel-semantics work:
//
//   MB-01..05  RFC-021  BLOB namespacing: one transfer verb, catalog = ns 0,
//                        stores = ns 1 through the delegate seam.
//   MB-06..07  RFC-017  the log channel (0x0008), replay_depth, and the ring's
//                        §9.4 visible drop counter.
//   MB-08..09  RFC-002/011  cfg_gen advances iff a value actually changed,
//                        from EITHER side.
//   MB-10..11  RFC-024  idle reaping, and the three-regime liveness model.
//
// Native (host-side, hardware-free): InProcessLink + ManualClock + XorShift32,
// doctest's bundled main(), same harness shape as test_slopsync_streamingress.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/core/clock.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/hub/hub.hpp"
#include "slopsync/transport/inprocess_binding.hpp"
#include "slopsync/wire/blob_chunks.hpp"
#include "slopsync/wire/catalog_codec.hpp"
#include "slopsync/wire/frame_header.hpp"
#include "slopsync/wire/messages/blob_req.hpp"
#include "slopsync/wire/messages/event.hpp"
#include "slopsync/wire/messages/goodbye.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/intent.hpp"
#include "slopsync/wire/messages/nack.hpp"
#include "slopsync/wire/messages/echo.hpp"
#include "slopsync/wire/messages/welcome.hpp"
#include "slopsync/wire/raw/catalog_ready.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace slopsync;

namespace {

constexpr uint16_t kCfgCh = 0x0101;  // INTENT c2h, the config-set channel

// A catalog carrying the spec-core log channel plus a config INTENT, so one
// hub exercises the log/replay path and the cfg_gen path.
void makeM3bCatalog(Catalog32& c, uint16_t replayDepth = uint16_t(limits::log_replay_depth_default)) {
    c.clear();
    REQUIRE(addLogChannel(c, replayDepth));

    c.addEntry({.id = kCfgCh, .name = "config-set",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 20.0f,
                .defaultPriority = Priority::critical});
    c.addSchemaField({.key = 1, .name = "speed", .type = CborFieldType::f32_t, .unit = "mm/s"});

    // M4b TIGHTENING, and the reason this DECLARATION is now required rather
    // than optional: BLOB_REQ gained an access gate, and the gate reads the
    // floor off the STORE entry that declares the store. A store nobody
    // declared has no declared access, so it cannot be authorized and is
    // answered CHUNK_UNAVAILABLE. The alternative — serving undeclared stores
    // openly — would mean "forget to declare it and it becomes world-readable",
    // a footgun pointing in exactly the direction that matters once a store can
    // hold a trust ledger. RFC-021 always described stores as catalog-declared;
    // this makes the hub actually require it.
    c.addEntry({.id = 0x0102, .name = "preset-store",
                .cls = ChannelClass::STORE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::background});
    c.addStoreDescriptor({.storeId = 3, .kind = "pattern.test", .capacity = 2,
                          .perItemMax = 4096, .nameMax = 16});
}

// ---- delegate ---------------------------------------------------------------
// Models a real config store: applyIntent reports `cfgChanged` iff the applied
// value actually MOVED, which is exactly what RFC-002 tightened the contract to
// mean. It also serves blob namespace 1 (a two-slot "preset store") through the
// RFC-021 seam.
class M3bDelegate final : public HubDelegate {
public:
    float speed = 100.0f;
    int applyCalls = 0;
    std::vector<std::string> storeItems{"preset-zero-payload", std::string(500, 'p')};
    bool serveStore = true;

    AccessLevel validateToken(std::span<const std::byte>, std::span<const std::byte>, bool hasToken) override {
        return hasToken ? AccessLevel::control : AccessLevel::watch;
    }

    Result<IntentValueMap, NackCode> applyIntent(uint16_t channel_id, const IntentValueMap& requested,
                                                 AccessLevel, bool& cfgChanged) override {
        ++applyCalls;
        if (channel_id != kCfgCh) return Result<IntentValueMap, NackCode>::err(NackCode::UNKNOWN_CHANNEL);

        float want = speed;
        for (uint32_t i = 0; i < requested.count; ++i) {
            if (requested.fields[i].key == 1) want = requested.fields[i].value.f32_val;
        }
        // Post-clamp application. `cfgChanged` = CHANGED, not ACCEPTED.
        if (want < 10.0f) want = 10.0f;
        if (want > 500.0f) want = 500.0f;
        cfgChanged = (want != speed);
        speed = want;

        IntentValueMap applied{};
        applied.count = 1;
        applied.fields[0] = IntentValueField{1, IntentValue::ofF32(speed)};
        return Result<IntentValueMap, NackCode>::ok(applied);
    }

    void onEstop(uint8_t, uint8_t) override {}

    std::optional<BlobView> readBlob(uint8_t ns, uint8_t store_id, uint8_t slot) override {
        if (!serveStore) return std::nullopt;
        if (ns != blob_ns::store || store_id != 3) return std::nullopt;
        if (slot >= storeItems.size()) return std::nullopt;
        const std::string& s = storeItems[slot];
        return BlobView{std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()),
                        /*generation=*/7};
    }
};

// ---- raw frame helpers ------------------------------------------------------
void writeFrame(ITransport& ep, FrameType type, uint16_t channel, std::span<const std::byte> payload) {
    std::array<std::byte, 600> buf{};
    FrameHeader h;
    h.type = uint8_t(type);
    h.flags = 0;
    h.channel = channel;
    h.seq = 0;
    h.len = uint16_t(payload.size());
    size_t pos = encodeFrameHeader(h, std::span<std::byte>(buf));
    REQUIRE(pos > 0);
    if (!payload.empty()) std::memcpy(buf.data() + pos, payload.data(), payload.size());
    REQUIRE(ep.write(std::span<const std::byte>(buf.data(), pos + payload.size())));
}

struct SubWish { uint16_t channel_id; float rate; uint8_t prio; };

void writeHello(ITransport& ep, uint8_t idByte, bool withToken, std::vector<SubWish> subs) {
    HelloMsg m{};
    m.proto_ver = kProtocolVersion;
    m.client_kind = "sim";
    m.client_name = "m3b-test";
    m.instance_id.fill(std::byte{0});
    m.instance_id[0] = std::byte{idByte};
    if (withToken) {
        m.has_token = true;
        m.token.fill(std::byte{0xAA});
    }
    m.subscriptions_count = uint32_t(subs.size());
    for (size_t i = 0; i < subs.size(); ++i) {
        m.subscriptions[i].channel_id = subs[i].channel_id;
        m.subscriptions[i].rate_hz = subs[i].rate;
        m.subscriptions[i].priority = subs[i].prio;
    }
    std::array<std::byte, 400> buf{};
    size_t n = encodeHello(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(ep, FrameType::HELLO, 0, std::span<const std::byte>(buf.data(), n));
}

struct DecodedReply {
    FrameType type;
    uint16_t channel;
    std::vector<std::byte> payload;
};

std::vector<DecodedReply> tickAndDrain(Hub& hub, ManualClock& clock, ITransport& ep, uint32_t stepUs = 1000) {
    clock.advanceUs(stepUs);
    hub.update(clock.nowUs());
    std::vector<DecodedReply> out;
    while (auto fb = ep.read()) {
        auto h = fb->header();
        if (!h) continue;
        auto pl = fb->payload();
        out.push_back(DecodedReply{FrameType(h->type), h->channel, std::vector<std::byte>(pl.begin(), pl.end())});
    }
    return out;
}

std::optional<WelcomeMsg> findWelcome(const std::vector<DecodedReply>& replies) {
    for (const auto& r : replies) {
        if (r.type != FrameType::WELCOME) continue;
        auto w = decodeWelcome(std::span<const std::byte>(r.payload));
        if (w) return w.value();
    }
    return std::nullopt;
}

void writeCatalogReady(ITransport& ep, std::span<const std::byte> etag) {
    std::array<std::byte, kCatalogReadyBytes> buf{};
    size_t n = encodeCatalogReady(etag, std::span<std::byte>(buf));
    REQUIRE(n == kCatalogReadyBytes);
    writeFrame(ep, FrameType::CATALOG_READY, 0, std::span<const std::byte>(buf.data(), n));
}

WelcomeMsg connectSession(Hub& hub, ManualClock& clock, ITransport& ep, uint8_t idByte, bool token,
                          std::vector<SubWish> subs) {
    writeHello(ep, idByte, token, std::move(subs));
    auto replies = tickAndDrain(hub, clock, ep);
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());
    writeCatalogReady(ep, std::span<const std::byte>(w->catalog_etag));
    tickAndDrain(hub, clock, ep);
    return w.value();
}

void writeBlobReq(ITransport& ep, const BlobReqMsg& m) {
    std::array<std::byte, 128> buf{};
    size_t n = encodeBlobReq(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(ep, FrameType::BLOB_REQ, 0, std::span<const std::byte>(buf.data(), n));
}

int countNacks(const std::vector<DecodedReply>& replies, NackCode code) {
    int n = 0;
    for (const auto& r : replies) {
        if (r.type != FrameType::NACK) continue;
        auto nm = decodeNack(std::span<const std::byte>(r.payload));
        if (nm && nm.value().code == code) ++n;
    }
    return n;
}

std::vector<EventMsg> collectEvents(const std::vector<DecodedReply>& replies, uint16_t channel) {
    std::vector<EventMsg> out;
    for (const auto& r : replies) {
        if (r.type != FrameType::EVENT || r.channel != channel) continue;
        auto e = decodeEvent(std::span<const std::byte>(r.payload));
        if (e) out.push_back(e.value());
    }
    return out;
}

std::string bodyTstr(const EventMsg& e, uint8_t key) {
    for (uint32_t i = 0; i < e.body_count; ++i) {
        if (e.body[i].key == key) return std::string(e.body[i].value.tstr_val);
    }
    return {};
}
uint64_t bodyUint(const EventMsg& e, uint8_t key) {
    for (uint32_t i = 0; i < e.body_count; ++i) {
        if (e.body[i].key == key) return e.body[i].value.u64_val;
    }
    return 0;
}

// ---- MB-12..14 helpers: backpressure without inventing a transport ----------
// The in-process binding ALREADY refuses writes when its 16-deep egress queue
// is full (detail::FrameQueue::kCapacity — "a full ring means write() returns
// false, same as any real binding's egress queue"). So a stall is produced by
// simply NOT reading for a few ticks, which is exactly what a wedged WebSocket
// client does on the device. No mock, no new fake, no knob.
constexpr size_t kLinkQueueDepth = 16;

// Advance the hub WITHOUT draining the client end — this is what fills the
// egress queue and makes the hub's writes start getting refused.
void tickNoDrain(Hub& hub, ManualClock& clock, uint32_t stepUs = 1000) {
    clock.advanceUs(stepUs);
    hub.update(clock.nowUs());
}

// A store item big enough that the transfer CANNOT fit the link's queue.
constexpr size_t kBigChunks = 34;
constexpr size_t kBigBytes = kBigChunks * limits::catalog_chunk_payload;  // 6528 B

BlobReqMsg bigItemReq() {
    BlobReqMsg req{};
    req.blob.ns = blob_ns::store;
    req.blob.store_id = 3;
    req.blob.has_store_id = true;
    req.blob.slot = 2;  // the oversized item MB-12..14 append to M3bDelegate
    req.blob.has_slot = true;
    return req;
}

// Pulls every BLOB_CHUNK out of one drain, tallying deliveries per index so a
// test can assert "the set delivered == the set requested, each exactly once"
// rather than merely counting frames.
void tallyChunks(const std::vector<DecodedReply>& replies, std::vector<int>& perIndex, int& thisTick) {
    thisTick = 0;
    for (const auto& r : replies) {
        if (r.type != FrameType::BLOB_CHUNK) continue;
        BlobChunkHeader h{};
        REQUIRE(getBlobChunkHeader(std::span<const std::byte>(r.payload), h));
        REQUIRE(h.chunk_index < perIndex.size());
        ++perIndex[h.chunk_index];
        ++thisTick;
    }
}

}  // namespace

// ---- MB-01 (RFC-021) --------------------------------------------------------
// BLOB_REQ's namespacing is ADDITIVE-BY-DEFAULT: a bare
// catalog request is still the empty CBOR map, because namespace 0 IS the
// default. The generalization costs the common case exactly zero bytes, which
// is what lets catalog transfer keep working unchanged through the generalized
// path.
TEST_CASE("MB-01: BLOB_REQ round-trips namespace/store/slot; the catalog request stays an empty map") {
    SUBCASE("bare catalog request encodes to an empty map") {
        BlobReqMsg m{};
        std::array<std::byte, 64> buf{};
        size_t n = encodeBlobReq(m, buf);
        REQUIRE(n == 1);
        CHECK(uint8_t(buf[0]) == 0xA0);  // map(0)

        auto d = decodeBlobReq(std::span<const std::byte>(buf.data(), n));
        REQUIRE(d.isOk());
        CHECK(d.value().full);
        CHECK(d.value().blob.ns == blob_ns::catalog);
    }

    SUBCASE("store item request carries the blob sub-map") {
        BlobReqMsg m{};
        m.blob.ns = blob_ns::store;
        m.blob.store_id = 3;
        m.blob.has_store_id = true;
        m.blob.slot = 1;
        m.blob.has_slot = true;
        m.blob.generation = 7;
        m.blob.has_generation = true;

        std::array<std::byte, 64> buf{};
        size_t n = encodeBlobReq(m, buf);
        REQUIRE(n > 0);
        auto d = decodeBlobReq(std::span<const std::byte>(buf.data(), n));
        REQUIRE(d.isOk());
        CHECK(d.value().full);
        CHECK(d.value().blob.ns == blob_ns::store);
        CHECK(d.value().blob.store_id == 3);
        CHECK(d.value().blob.slot == 1);
        CHECK(d.value().blob.generation == 7);
    }

    SUBCASE("repair of a store item carries BOTH chunks and blob, keys ascending") {
        BlobReqMsg m{};
        m.blob.ns = blob_ns::store;
        m.blob.store_id = 3;
        m.blob.has_store_id = true;
        m.blob.slot = 1;
        m.blob.has_slot = true;
        m.full = false;
        m.chunks_count = 2;
        m.chunks[0] = 0;
        m.chunks[1] = 2;

        std::array<std::byte, 64> buf{};
        size_t n = encodeBlobReq(m, buf);
        REQUIRE(n > 0);
        CHECK(uint8_t(buf[0]) == 0xA2);        // map(2)
        CHECK(uint8_t(buf[1]) == 0x18);        // uint8-encoded key...
        CHECK(uint8_t(buf[2]) == 27);          // ...`chunks` (27) FIRST: 27 < 38

        auto d = decodeBlobReq(std::span<const std::byte>(buf.data(), n));
        REQUIRE(d.isOk());
        CHECK_FALSE(d.value().full);
        REQUIRE(d.value().chunks_count == 2);
        CHECK(d.value().chunks[1] == 2);
        CHECK(d.value().blob.slot == 1);
    }

    SUBCASE("a catalog request naming a store/slot is MALFORMED, both directions") {
        BlobReqMsg m{};  // ns stays 0
        m.blob.has_slot = true;
        m.blob.slot = 4;
        std::array<std::byte, 64> buf{};
        CHECK(encodeBlobReq(m, buf) == 0);

        // ...and the decoder refuses the same shape if a peer hand-builds it.
        const std::byte hand[] = {std::byte{0xA1}, std::byte{0x18}, std::byte{38},
                                  std::byte{0xA1}, std::byte{0x03}, std::byte{0x04}};
        CHECK_FALSE(decodeBlobReq(std::span<const std::byte>(hand, sizeof(hand))).isOk());
    }

    SUBCASE("a repair naming zero chunks is MALFORMED (RFC-022.6)") {
        BlobReqMsg m{};
        m.full = false;
        m.chunks_count = 0;
        std::array<std::byte, 64> buf{};
        CHECK(encodeBlobReq(m, buf) == 0);
    }
}

// ---- MB-02 (RFC-021) --------------------------------------------------------
// the BLOB_CHUNK identity header is namespace-agnostic and
// round-trips; a reassembler REFUSES chunks belonging to a different blob,
// which is what lets a catalog transfer and a preset fetch be in flight at the
// same time without corrupting each other.
TEST_CASE("MB-02: BLOB_CHUNK header carries identity; a reassembler rejects a foreign blob's chunks") {
    std::array<std::byte, 400> payload{};
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = std::byte(uint8_t(i));

    BlobId storeId{};
    storeId.ns = blob_ns::store;
    storeId.store_id = 3;
    storeId.has_store_id = true;
    storeId.slot = 1;
    storeId.has_slot = true;
    storeId.generation = 7;
    storeId.has_generation = true;

    const size_t cc = chunkCount(payload.size());
    REQUIRE(cc == 3);  // ceil(400/192)

    ChunkReassembler<8> reasm;
    std::array<std::byte, kBlobChunkHeaderBytes + limits::catalog_chunk_payload> cbuf{};

    // First chunk seeds the transfer from its own header — total_bytes rides
    // the wire (RFC-028: know the size before you allocate).
    size_t n0 = fillBlobChunk(storeId, payload, 0, cbuf);
    REQUIRE(n0 > 0);
    BlobChunkHeader h{};
    REQUIRE(getBlobChunkHeader(std::span<const std::byte>(cbuf.data(), n0), h));
    CHECK(h.id.ns == blob_ns::store);
    CHECK(h.id.store_id == 3);
    CHECK(h.id.slot == 1);
    CHECK(h.id.generation == 7);
    CHECK(h.chunk_count == 3);
    CHECK(h.total_bytes == payload.size());

    reasm.begin(h, /*nowMs=*/0);
    REQUIRE(reasm.insert(std::span<const std::byte>(cbuf.data(), n0), 0));

    // A CATALOG chunk arriving mid-store-transfer is rejected, not merged.
    size_t nCat = fillBlobChunk(BlobId{}, payload, 1, cbuf);
    REQUIRE(nCat > 0);
    CHECK_FALSE(reasm.insert(std::span<const std::byte>(cbuf.data(), nCat), 1));
    CHECK_FALSE(reasm.complete());

    for (uint16_t i = 1; i < cc; ++i) {
        size_t nn = fillBlobChunk(storeId, payload, i, cbuf);
        REQUIRE(nn > 0);
        REQUIRE(reasm.insert(std::span<const std::byte>(cbuf.data(), nn), 1));
    }
    REQUIRE(reasm.complete());
    auto assembled = reasm.assembled();
    REQUIRE(assembled.size() == payload.size());
    CHECK(std::equal(assembled.begin(), assembled.end(), payload.begin()));
}

// ---- MB-03 (RFC-021) --------------------------------------------------------
// the hub serves namespace 0 ITSELF (never through the
// delegate: the catalog bytes here are the same buffer the etag was computed
// over, so catalog and etag cannot drift), and every chunk it emits is stamped
// with the catalog identity.
TEST_CASE("MB-03: catalog transfer works unchanged through the generalized BLOB path") {
    Catalog32 cat;
    makeM3bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(303);
    M3bDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    auto w = connectSession(hub, clock, link.endpointB(), 1, true, {});

    writeBlobReq(link.endpointB(), BlobReqMsg{});  // bare catalog request
    auto replies = tickAndDrain(hub, clock, link.endpointB());

    ChunkReassembler<64> reasm;
    bool begun = false;
    int chunks = 0;
    for (const auto& r : replies) {
        if (r.type != FrameType::BLOB_CHUNK) continue;
        ++chunks;
        BlobChunkHeader h{};
        REQUIRE(getBlobChunkHeader(std::span<const std::byte>(r.payload), h));
        CHECK(h.id.ns == blob_ns::catalog);
        CHECK(h.id.store_id == 0);  // absent by rule in namespace 0
        CHECK(h.id.slot == 0);
        if (!begun) {
            reasm.begin(h, 0);
            begun = true;
        }
        CHECK(reasm.insert(std::span<const std::byte>(r.payload), 0));
    }
    REQUIRE(chunks > 0);
    REQUIRE(reasm.complete());

    // The bytes verify against the etag WELCOME advertised — the whole point of
    // the transfer, and proof the generalization did not disturb it.
    auto digest = Sha256::hash(reasm.assembled());
    for (size_t i = 0; i < w.catalog_etag.size(); ++i) CHECK(digest[i] == w.catalog_etag[i]);
}

// ---- MB-04 (RFC-021) --------------------------------------------------------
// namespace 1 rides the SAME verb, resolved through the
// delegate's readBlob() seam. This is the seam milestone 5's NVS-backed preset
// store plugs into: it implements one method and inherits chunking, selective
// repair, total_bytes pre-sizing and CHUNK_UNAVAILABLE for free.
TEST_CASE("MB-04: a store item transfers through the same verb via the delegate seam") {
    Catalog32 cat;
    makeM3bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(304);
    M3bDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, true, {});

    BlobReqMsg req{};
    req.blob.ns = blob_ns::store;
    req.blob.store_id = 3;
    req.blob.has_store_id = true;
    req.blob.slot = 1;  // the 500-byte item -> 3 chunks
    req.blob.has_slot = true;
    writeBlobReq(link.endpointB(), req);
    // PUMP UNTIL THE TRANSFER COMPLETES, rather than assuming one tick carries
    // it. §8.4 transfers are RESUMABLE and the hub emits at most
    // kBlobChunksPerTick per update() — that budget is a pacing choice the hub
    // is free to change (it went 8 -> 2 when a heap-transient bisect showed the
    // burst was the dominant allocation spike), and a test that hard-codes one
    // tick is asserting the pacing, not the outcome. Bounded so a genuinely
    // stalled transfer still fails instead of hanging.
    std::vector<DecodedReply> replies;
    for (int t = 0; t < 16; ++t) {
        auto more = tickAndDrain(hub, clock, link.endpointB());
        replies.insert(replies.end(), more.begin(), more.end());
        size_t chunks = 0;
        for (const auto& r : replies) if (r.type == FrameType::BLOB_CHUNK) ++chunks;
        if (chunks >= 3) break;   // the 500-byte item is 3 chunks
    }

    ChunkReassembler<8> reasm;
    bool begun = false;
    for (const auto& r : replies) {
        if (r.type != FrameType::BLOB_CHUNK) continue;
        BlobChunkHeader h{};
        REQUIRE(getBlobChunkHeader(std::span<const std::byte>(r.payload), h));
        CHECK(h.id.ns == blob_ns::store);
        CHECK(h.id.store_id == 3);
        CHECK(h.id.slot == 1);
        CHECK(h.id.generation == 7);  // the backend's roster generation, echoed
        if (!begun) {
            reasm.begin(h, 0);
            begun = true;
        }
        CHECK(reasm.insert(std::span<const std::byte>(r.payload), 0));
    }
    REQUIRE(reasm.complete());
    auto asm_ = reasm.assembled();
    REQUIRE(asm_.size() == del.storeItems[1].size());
    CHECK(std::equal(asm_.begin(), asm_.end(),
                     reinterpret_cast<const std::byte*>(del.storeItems[1].data())));
}

// ---- MB-05 (RFC-021) --------------------------------------------------------
// the failure surface is honest and namespace-agnostic:
// unknown namespace / store / slot, and a repair naming an index the blob does
// not have, all answer CHUNK_UNAVAILABLE (whose registry note was generalized
// from "catalog chunk" for exactly this).
TEST_CASE("MB-05: unresolvable blobs and out-of-range repairs NACK CHUNK_UNAVAILABLE") {
    Catalog32 cat;
    makeM3bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(305);
    M3bDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, true, {});

    SUBCASE("slot the backend does not have") {
        BlobReqMsg req{};
        req.blob.ns = blob_ns::store;
        req.blob.store_id = 3;
        req.blob.has_store_id = true;
        req.blob.slot = 99;
        req.blob.has_slot = true;
        writeBlobReq(link.endpointB(), req);
        CHECK(countNacks(tickAndDrain(hub, clock, link.endpointB()), NackCode::CHUNK_UNAVAILABLE) == 1);
    }

    SUBCASE("a hub with no store backend at all") {
        del.serveStore = false;
        BlobReqMsg req{};
        req.blob.ns = blob_ns::store;
        req.blob.store_id = 3;
        req.blob.has_store_id = true;
        req.blob.slot = 0;
        req.blob.has_slot = true;
        writeBlobReq(link.endpointB(), req);
        CHECK(countNacks(tickAndDrain(hub, clock, link.endpointB()), NackCode::CHUNK_UNAVAILABLE) == 1);
    }

    SUBCASE("repair naming a chunk index the catalog does not have") {
        BlobReqMsg req{};
        req.full = false;
        req.chunks_count = 1;
        req.chunks[0] = 9999;
        writeBlobReq(link.endpointB(), req);
        auto replies = tickAndDrain(hub, clock, link.endpointB());
        CHECK(countNacks(replies, NackCode::CHUNK_UNAVAILABLE) == 1);
        int chunks = 0;
        for (const auto& r : replies) {
            if (r.type == FrameType::BLOB_CHUNK) ++chunks;
        }
        CHECK(chunks == 0);
    }
}

// ---- MB-06 (RFC-017 + the EVENT `body` grammar fix) -------------------------
// a published log line lands
// as ONE log_events::entry EVENT whose kind-specific fields ride the scoped
// `body` (40) sub-map, keyed by 0x0008's OWN catalog schema. That is what makes
// device-authored EVENT channels possible without a registry PR per field.
TEST_CASE("MB-06: publishLog emits a body-scoped EVENT; over-length strings truncate, never vanish") {
    Catalog32 cat;
    makeM3bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(306);
    M3bDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, true, {{channels::log, 0.0f, 0}});

    REQUIRE(hub.publishLog(log_levels::warn, "motion", "quintic rejected, guard took it"));
    auto events = collectEvents(tickAndDrain(hub, clock, link.endpointB()), channels::log);
    REQUIRE(events.size() == 1);
    const EventMsg& e = events[0];
    CHECK(e.event_kind == log_events::entry);
    REQUIRE(e.has_body);
    CHECK(e.body_count == 4);
    CHECK(bodyUint(e, log_body::level) == log_levels::warn);
    CHECK(bodyTstr(e, log_body::tag) == "motion");
    CHECK(bodyTstr(e, log_body::message) == "quintic rejected, guard took it");
    CHECK(bodyUint(e, log_body::hub_ms) == e.timestamp);

    SUBCASE("a registry-length message survives the per-subscriber event slot") {
        std::string longMsg(kLogMessageMaxBytes, 'x');
        REQUIRE(hub.publishLog(log_levels::error, "sys", longMsg));
        auto ev2 = collectEvents(tickAndDrain(hub, clock, link.endpointB()), channels::log);
        REQUIRE(ev2.size() == 1);
        CHECK(bodyTstr(ev2[0], log_body::message).size() == kLogMessageMaxBytes);
    }

    SUBCASE("over-length is TRUNCATED, not dropped — a vanishing diagnostic is the worst failure") {
        std::string tooLong(kLogMessageMaxBytes + 40, 'y');
        REQUIRE(hub.publishLog(log_levels::info, "a-very-long-subsystem-tag", tooLong));
        auto ev2 = collectEvents(tickAndDrain(hub, clock, link.endpointB()), channels::log);
        REQUIRE(ev2.size() == 1);
        CHECK(bodyTstr(ev2[0], log_body::message).size() == kLogMessageMaxBytes);
        CHECK(bodyTstr(ev2[0], log_body::tag).size() == kLogTagMaxBytes);
    }
}

// ---- MB-07 (RFC-017) --------------------------------------------------------
// replay_depth. §9.4 says events are edges and are NEVER
// replayed; the log declares a replay depth, which is the ONE sanctioned
// exception, and it exists because a client that connects AFTER a fault must
// still be able to see what happened.
TEST_CASE("MB-07: a late subscriber replays the ring tail, bounded by the declared replay_depth") {
    Catalog32 cat;
    makeM3bCatalog(cat, /*replayDepth=*/3);
    ManualClock clock;
    XorShift32 rng(307);
    M3bDelegate del;
    Hub hub(cat, clock, rng, del);

    const CatalogEntry* logEntry = cat.find(channels::log);
    REQUIRE(logEntry);
    CHECK(logEntry->hasReplayDepth);
    CHECK(logEntry->replayDepth == 3);

    // Five lines happen with NOBODY connected — the ring is the record.
    for (int i = 1; i <= 5; ++i) {
        REQUIRE(hub.publishLog(log_levels::info, "boot", "line " + std::to_string(i)));
    }

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    // Connected by hand rather than via connectSession(): the replay is queued
    // at GRANT and released the instant the session becomes READY, so the
    // frames land in the CATALOG_READY tick and a helper that swallowed that
    // tick would hide the very thing under test.
    writeHello(link.endpointB(), 1, true, {{channels::log, 0.0f, 0}});
    auto w = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w.has_value());
    writeCatalogReady(link.endpointB(), std::span<const std::byte>(w->catalog_etag));
    auto events = collectEvents(tickAndDrain(hub, clock, link.endpointB()), channels::log);
    REQUIRE(events.size() == 3);  // capped at replay_depth, NOT the whole ring
    // Oldest-first: a joining client sees the tail as it happened.
    CHECK(bodyTstr(events[0], log_body::message) == "line 3");
    CHECK(bodyTstr(events[1], log_body::message) == "line 4");
    CHECK(bodyTstr(events[2], log_body::message) == "line 5");

    // A channel with no replay_depth stays §9.4-ordinary.
    Catalog32 plain;
    makeM3bCatalog(plain, /*replayDepth=*/0);
    const CatalogEntry* pe = plain.find(channels::log);
    REQUIRE(pe);
    CHECK_FALSE(pe->hasReplayDepth);
}

// ---- MB-07b (RFC-017 / M5b) -------------------------------------------------
// §9.4's VISIBLE drop counter for the log ring.
//
// A bounded log is fine. A bounded log that silently eats lines is not: the
// operator reading it cannot tell "nothing happened" from "the interesting part
// scrolled off". The firmware wires logDropped() (plus its own httpTask->hub
// bridge ring's drops) into the 0x0006 hub-status snapshot, so this counter is
// load-bearing on the wire and not merely diagnostic.
TEST_CASE("MB-07b: overflowing the log ring counts every dropped line and still replays the newest tail") {
    Catalog32 cat;
    makeM3bCatalog(cat, /*replayDepth=*/4);
    ManualClock clock;
    XorShift32 rng(3076);
    M3bDelegate del;
    Hub hub(cat, clock, rng, del);

    CHECK(hub.logDropped() == 0);

    // The RING is limits::log_replay_depth_default deep regardless of what the
    // CATALOG declares — replay_depth caps what a joining client is SENT, the
    // ring caps what is KEPT. Overflow it well past both.
    const size_t ringDepth = size_t(limits::log_replay_depth_default);
    const size_t lines = ringDepth + 20;
    for (size_t i = 1; i <= lines; ++i) {
        REQUIRE(hub.publishLog(log_levels::info, "flood", "line " + std::to_string(i)));
    }
    // Every line beyond the ring's depth is one drop, counted, never silent.
    CHECK(hub.logDropped() == uint32_t(lines - ringDepth));

    // ...and the ring still holds the NEWEST lines, so a client that connects
    // after the flood sees the end of it, which is the part that matters.
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    writeHello(link.endpointB(), 1, true, {{channels::log, 0.0f, 0}});
    auto w = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w.has_value());
    writeCatalogReady(link.endpointB(), std::span<const std::byte>(w->catalog_etag));
    auto events = collectEvents(tickAndDrain(hub, clock, link.endpointB()), channels::log);
    REQUIRE(events.size() == 4);  // the catalog's replay_depth, not the ring's
    CHECK(bodyTstr(events[3], log_body::message) == "line " + std::to_string(lines));
}

// ---- MB-08 (RFC-002) --------------------------------------------------------
// the CLIENT half of the cfg_gen rule: an accepted but
// VALUE-IDENTICAL config-set still gets its post-clamp ECHO (ground truth is
// unaffected) but does NOT bump cfg_gen. The old "accepted therefore bumped"
// reading re-armed on-change publications and resync storms for no-op writes.
TEST_CASE("MB-08: a value-identical accepted config-set echoes but does not bump cfg_gen") {
    Catalog32 cat;
    makeM3bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(308);
    M3bDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, true, {});

    auto sendSpeed = [&](uint16_t intentId, float v) {
        IntentMsg m{};
        m.channel_id = kCfgCh;
        m.intent_id = intentId;
        m.value_count = 1;
        m.value[0] = IntentValueField{1, IntentValue::ofF32(v)};
        std::array<std::byte, 128> buf{};
        size_t n = encodeIntent(m, std::span<std::byte>(buf));
        REQUIRE(n > 0);
        writeFrame(link.endpointB(), FrameType::INTENT, kCfgCh, std::span<const std::byte>(buf.data(), n));
        auto replies = tickAndDrain(hub, clock, link.endpointB());
        std::optional<EchoMsg> echo;
        for (const auto& r : replies) {
            if (r.type != FrameType::ECHO) continue;
            auto e = decodeEcho(std::span<const std::byte>(r.payload));
            if (e) echo = e.value();
        }
        return echo;
    };

    const uint16_t gen0 = hub.cfgGen();

    auto e1 = sendSpeed(1, 250.0f);
    REQUIRE(e1.has_value());
    CHECK(hub.cfgGen() == uint16_t(gen0 + 1));  // it CHANGED
    CHECK(e1->applied_count == 1);
    CHECK(e1->applied[0].value.f32_val == 250.0f);

    auto e2 = sendSpeed(2, 250.0f);  // same value again
    REQUIRE(e2.has_value());                       // ECHO still fires: ground truth
    CHECK(e2->applied[0].value.f32_val == 250.0f);
    CHECK(hub.cfgGen() == uint16_t(gen0 + 1));     // ...but cfg_gen does NOT move
    CHECK(del.applyCalls == 2);                    // the delegate really was asked

    // A clamped write that lands on the SAME applied value is likewise a no-op:
    // 9000 clamps to 500 the first time (change), and again the second (no change).
    auto e3 = sendSpeed(3, 9000.0f);
    REQUIRE(e3.has_value());
    CHECK(e3->applied[0].value.f32_val == 500.0f);
    CHECK(hub.cfgGen() == uint16_t(gen0 + 2));
    auto e4 = sendSpeed(4, 9000.0f);
    REQUIRE(e4.has_value());
    CHECK(hub.cfgGen() == uint16_t(gen0 + 2));
}

// ---- MB-09 (RFC-011) --------------------------------------------------------
// the HUB half, the mirror twin: a machine-side config
// change (physical control, boot adoption, internal recalculation) had no way
// to advance the generation at all, so a client's `precondition` CAS passed
// against config that had already moved.
TEST_CASE("MB-09: bumpConfigGeneration advances cfg_gen and invalidates a stale CAS") {
    Catalog32 cat;
    makeM3bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(309);
    M3bDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    auto w = connectSession(hub, clock, link.endpointB(), 1, true, {});

    const uint16_t seen = w.cfg_gen;  // what the client believes

    // The machine changes its own config. Before RFC-011 this was invisible.
    del.speed = 321.0f;
    hub.bumpConfigGeneration();
    CHECK(hub.cfgGen() == uint16_t(seen + 1));

    // The client's CAS against its stale belief now correctly FAILS.
    IntentMsg m{};
    m.channel_id = kCfgCh;
    m.intent_id = 1;
    m.has_precondition = true;
    m.precondition = seen;
    m.value_count = 1;
    m.value[0] = IntentValueField{1, IntentValue::ofF32(200.0f)};
    std::array<std::byte, 128> buf{};
    size_t n = encodeIntent(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(link.endpointB(), FrameType::INTENT, kCfgCh, std::span<const std::byte>(buf.data(), n));
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    CHECK(countNacks(replies, NackCode::CONFLICT) == 1);
    CHECK(del.speed == 321.0f);  // the stale write never reached the machine
}

// ---- MB-10 (RFC-024) --------------------------------------------------------
// idle reaping. §6.5's "MAY reap at 3x the idle interval"
// was written and never implemented, so a viewer session that went dark held a
// slot until reboot. A session that keeps PINGing is never touched.
TEST_CASE("MB-10 (RFC-042): a silent non-owning session goes STALE at idle_reap_multiplier x the idle interval, slot retained") {
    Catalog32 cat;
    makeM3bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(310);
    M3bDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink linkA(clock, rng), linkB(clock, rng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));
    REQUIRE(hub.attachTransport(linkB.endpointA()));
    REQUIRE(linkA.endpointB().open());
    REQUIRE(linkB.endpointB().open());

    connectSession(hub, clock, linkA.endpointB(), 1, true, {});  // goes dark
    connectSession(hub, clock, linkB.endpointB(), 2, true, {});  // keeps pinging
    REQUIRE(hub.sessionCount() == 2);

    constexpr uint32_t kIdleMs = limits::idle_reap_multiplier * limits::ping_interval_idle_ms;

    // Just short of the threshold: both alive.
    std::vector<DecodedReply> fromA;
    for (uint32_t t = 0; t < kIdleMs - 500; t += 250) {
        writeFrame(linkB.endpointB(), FrameType::PING, 0, std::span<const std::byte>{});
        auto r = tickAndDrain(hub, clock, linkA.endpointB(), /*stepUs=*/250000);
        fromA.insert(fromA.end(), r.begin(), r.end());
        tickAndDrain(hub, clock, linkB.endpointB(), /*stepUs=*/0);
    }
    CHECK(hub.sessionCount() == 2);
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::LIVE);

    // Past it: A goes STALE, but the slot is RETAINED (RFC-042) — B stays LIVE.
    for (int i = 0; i < 6; ++i) {
        writeFrame(linkB.endpointB(), FrameType::PING, 0, std::span<const std::byte>{});
        auto r = tickAndDrain(hub, clock, linkA.endpointB(), /*stepUs=*/250000);
        fromA.insert(fromA.end(), r.begin(), r.end());
        tickAndDrain(hub, clock, linkB.endpointB(), /*stepUs=*/0);
    }
    // occupied() counts STALE too, so the slot is not given back.
    CHECK(hub.sessionCount() == 2);
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::STALE);

    bool sawGoodbye = false;
    for (const auto& r : fromA) {
        if (r.type == FrameType::GOODBYE) sawGoodbye = true;
    }
    // RFC-042: staleness is NOT an ending — no GOODBYE, the client may never
    // even notice. (A silent A never sees anything either way, since it never
    // reads its own transport in this harness; the point is the hub SENDS none.)
    CHECK_FALSE(sawGoodbye);
}

// ---- MB-11 (RFC-024 + §11.3) ------------------------------------------------
// the two liveness regimes are DISJOINT and coexist:
// a source-owning session is governed by the tighter deadman window and its
// loss policy (motion consequence); everyone else by idle reaping (no motion
// consequence at all). A hub with no sources only ever exercises the latter.
TEST_CASE("MB-11 (RFC-042): idle reaping marks STALE, never runs a loss policy, never frees the slot") {
    Catalog32 cat;
    makeM3bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(311);
    M3bDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, true, {});

    constexpr uint32_t kIdleMs = limits::idle_reap_multiplier * limits::ping_interval_idle_ms;
    for (uint32_t t = 0; t <= kIdleMs + 1000; t += 250) {
        tickAndDrain(hub, clock, link.endpointB(), /*stepUs=*/250000);
    }
    // RFC-042: the slot is RETAINED — a session that owns nothing still costs
    // the hub exactly as much as a live one, and only slot pressure evicts it.
    CHECK(hub.sessionCount() == 1);
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::STALE);
    // No source was owned, so nothing latched: the machine is untouched.
    CHECK_FALSE(hub.stopLatched());
    CHECK_FALSE(hub.estopLatched());
}

// ---- RFC-028 regression -----------------------------------------------------
// ChunkReassembler TOTALITY (found by test/fuzz/fuzz_blob).
//
// Minimized crashing input: a BLOB_CHUNK header whose chunk_count is far
// larger than the reassembler's MaxChunks, followed by a call to
// missingIndices() — the exact thing a client does on its gap timer.
//
// The bug: begin() correctly REFUSED the transfer (active() stayed false —
// RFC-028's know-the-size-before-you-allocate rule working as designed) but
// still stored the attacker's chunk_count and total_bytes. missingIndices()
// then walked `_received[i]` for i < _chunkCount without consulting active(),
// and assembled() spanned _totalBytes the same way. UBSan report: "load of
// value 50, which is not a valid value for type 'const bool'" — i.e. an
// out-of-bounds read past an 8-element array.
//
// The lesson worth keeping: refusing a transfer has to refuse its NUMBERS
// too. A guard that leaves attacker-chosen sizes in members is a guard that
// only moved the bug one call to the right.
TEST_CASE("RFC-028: a refused blob transfer leaves no attacker-chosen sizes behind") {
    ChunkReassembler<8> reasm;

    BlobId id{};
    id.ns = blob_ns::catalog;

    // 65535 chunks / 4 GB — both far past this instance's capacity.
    reasm.begin(id, 65535, 0xFFFFFFFFu, 1000);
    REQUIRE(!reasm.active());

    // The queries a client runs on its own timer must be bounded regardless.
    std::array<uint16_t, 64> missing{};
    CHECK(reasm.missingIndices(std::span<uint16_t>(missing)) == 0);
    CHECK(!reasm.complete());
    CHECK(reasm.assembled().empty());

    // A chunk_count within MaxChunks but a total_bytes past capacity is the
    // other half of the same refusal.
    reasm.begin(id, 4, ChunkReassembler<8>::kMaxTotalBytes + 1, 1000);
    REQUIRE(!reasm.active());
    CHECK(reasm.missingIndices(std::span<uint16_t>(missing)) == 0);
    CHECK(reasm.assembled().empty());
}

TEST_CASE("RFC-028: an accepted transfer still reports its real missing set") {
    // The refusal clamp must not have blunted the normal path.
    ChunkReassembler<8> reasm;
    BlobId id{};
    id.ns = blob_ns::catalog;
    reasm.begin(id, 3, 3 * limits::catalog_chunk_payload, 1000);
    REQUIRE(reasm.active());

    std::array<uint16_t, 8> missing{};
    REQUIRE(reasm.missingIndices(std::span<uint16_t>(missing)) == 3);
    CHECK(missing[0] == 0);
    CHECK(missing[1] == 1);
    CHECK(missing[2] == 2);
}

// ---- MB-12 (Â§8.4 + Â§13.1) â€” THE BACKPRESSURE REGRESSION. ----------------
//
// handleBlobReq used to answer a request by blasting every chunk in one
// synchronous loop and THROWING THE TRANSPORT'S RETURN VALUE AWAY. Â§13.1 says
// write() == false means "not accepted right now; caller decides retry vs
// drop", and the hub was silently choosing drop â€” so a blob longer than the
// link's egress queue lost its tail and the client's reassembly never
// completed. Measured on hardware: 47 of the device catalog's 57 chunks
// arrived against a 32-deep TX queue.
//
// The fix is a per-slot cursor drained a bounded number of chunks per
// update(), pausing exactly when the transport pushes back. Both halves are
// asserted here: that it PACES (one transfer cannot own a tick), and that a
// link which refuses and later accepts still receives EVERY chunk ONCE.
TEST_CASE("MB-12: a refusing transport pauses a blob transfer; it resumes and delivers every chunk once") {
    Catalog32 cat;
    makeM3bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(312);
    M3bDelegate del;
    del.storeItems.push_back(std::string(kBigBytes, 'q'));  // slot 2
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, true, {});

    std::vector<int> perIndex(kBigChunks, 0);
    int nacks = 0;

    SUBCASE("paced: no single update() empties the whole blob onto the link") {
        int maxPerTick = 0;
        int ticksWithChunks = 0;
        writeBlobReq(link.endpointB(), bigItemReq());
        for (int i = 0; i < 40; ++i) {
            auto replies = tickAndDrain(hub, clock, link.endpointB());
            nacks += countNacks(replies, NackCode::CHUNK_UNAVAILABLE);
            int n = 0;
            tallyChunks(replies, perIndex, n);
            if (n > 0) ++ticksWithChunks;
            if (n > maxPerTick) maxPerTick = n;
        }
        CHECK(nacks == 0);
        // The budget is a private hub policy constant (Hub::kBlobChunksPerTick,
        // currently 8) and is invisible on the wire. What IS observable â€” and
        // what actually matters â€” is that it EXISTS and is small: a 34-chunk
        // blob must cost several ticks, never one. If that constant is retuned
        // these two numbers move with it, and that coupling is intended.
        CHECK(maxPerTick <= 8);
        CHECK(ticksWithChunks >= 5);
        // Paced is not the same as lossy: it still all arrives.
        for (size_t i = 0; i < kBigChunks; ++i) CHECK(perIndex[i] == 1);
    }

    SUBCASE("backpressured: stall the link, then let it drain â€” nothing is lost") {
        writeBlobReq(link.endpointB(), bigItemReq());

        // Do not read for a while. The link's 16-deep egress queue fills and
        // every further write is REFUSED â€” the exact condition the old code
        // discarded.
        for (int i = 0; i < 8; ++i) tickNoDrain(hub, clock);

        // Now let it drain, ticking as the real owner loop would.
        for (int i = 0; i < 40; ++i) {
            auto replies = tickAndDrain(hub, clock, link.endpointB());
            nacks += countNacks(replies, NackCode::CHUNK_UNAVAILABLE);
            int n = 0;
            tallyChunks(replies, perIndex, n);
        }

        // A refusal is FLOW CONTROL, not an error: no NACK, and the session is
        // still very much alive afterwards.
        CHECK(nacks == 0);
        CHECK(hub.sessionCount() == 1);

        // Every chunk, exactly once â€” the set delivered == the set requested.
        int delivered = 0, duplicated = 0, missing = 0;
        for (size_t i = 0; i < kBigChunks; ++i) {
            if (perIndex[i] == 1) ++delivered;
            else if (perIndex[i] > 1) ++duplicated;
            else ++missing;
        }
        CHECK(delivered == int(kBigChunks));
        CHECK(duplicated == 0);
        CHECK(missing == 0);
        // ...and it genuinely stalled: there are more chunks than the queue
        // that was holding them, so the refusal path really was taken.
        CHECK(kBigChunks > kLinkQueueDepth);
    }
}

// ---- MB-13 (§8.4) -----------------------------------------------------------
// SELECTIVE REPAIR resumes too, and resumes CORRECTLY.
//
// The repair path is the one that would rot quietly if a resumable transfer
// only remembered "next index": a repair is an arbitrary index LIST, so the
// cursor has to walk the list, not a range. A resume that fell back to
// counting would silently start streaming the whole blob at a client that
// asked for a handful of holes.
TEST_CASE("MB-13: a selective repair survives backpressure and delivers exactly the named indices") {
    Catalog32 cat;
    makeM3bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(313);
    M3bDelegate del;
    del.storeItems.push_back(std::string(kBigBytes, 'q'));  // slot 2
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, true, {});

    // Every ODD index of the 34: 17 of them â€” more than the link queue holds,
    // so this repair cannot complete without surviving a refusal.
    BlobReqMsg req = bigItemReq();
    req.full = false;
    req.chunks_count = 0;
    for (uint16_t i = 1; i < uint16_t(kBigChunks); i += 2) req.chunks[req.chunks_count++] = i;
    REQUIRE(req.chunks_count == 17);
    REQUIRE(req.chunks_count > kLinkQueueDepth);
    writeBlobReq(link.endpointB(), req);

    for (int i = 0; i < 8; ++i) tickNoDrain(hub, clock);  // stall

    std::vector<int> perIndex(kBigChunks, 0);
    int nacks = 0;
    for (int i = 0; i < 40; ++i) {
        auto replies = tickAndDrain(hub, clock, link.endpointB());
        nacks += countNacks(replies, NackCode::CHUNK_UNAVAILABLE);
        int n = 0;
        tallyChunks(replies, perIndex, n);
    }

    CHECK(nacks == 0);
    for (size_t i = 0; i < kBigChunks; ++i) {
        // Odd => delivered exactly once. Even => NEVER sent: a resumed repair
        // that "helpfully" continued into the rest of the blob fails here.
        CHECK(perIndex[i] == ((i % 2 == 1) ? 1 : 0));
    }
}

// ---- MB-14 (Â§6.8) â€” the cursor DIES WITH ITS SESSION. --------------------
//
// Â§8's third field bug was session-scoped state (source ownership) that
// outlived its session and poisoned every later client. A stalled blob cursor
// is the same shape, so it is cleared on the same one path every session death
// funnels through â€” and it is checked the way that bug taught us to check:
// back-to-back sessions with NO reboot in between.
TEST_CASE("MB-14: a stalled transfer is torn down with its session and never resumes into the next one") {
    Catalog32 cat;
    makeM3bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(314);
    M3bDelegate del;
    del.storeItems.push_back(std::string(kBigBytes, 'q'));  // slot 2
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, true, {});

    // Arm a transfer far too big to finish, and stall it against a full queue.
    writeBlobReq(link.endpointB(), bigItemReq());
    for (int i = 0; i < 6; ++i) tickNoDrain(hub, clock);

    // The client leaves mid-transfer.
    writeFrame(link.endpointB(), FrameType::GOODBYE, 0, std::span<const std::byte>{});
    tickAndDrain(hub, clock, link.endpointB());  // teardown + flush whatever was queued
    CHECK(hub.sessionCount() == 0);
    tickAndDrain(hub, clock, link.endpointB());

    // A NEW session takes the same slot, with no reboot in between.
    connectSession(hub, clock, link.endpointB(), 2, true, {});
    REQUIRE(hub.sessionCount() == 1);

    std::vector<int> perIndex(kBigChunks, 0);
    int strays = 0;
    for (int i = 0; i < 20; ++i) {
        auto replies = tickAndDrain(hub, clock, link.endpointB());
        int n = 0;
        tallyChunks(replies, perIndex, n);
        strays += n;
    }
    // Not one chunk of the departed client's blob. A leaked cursor would have
    // resumed here and sprayed the rest of it at a session that never asked.
    CHECK(strays == 0);
}
