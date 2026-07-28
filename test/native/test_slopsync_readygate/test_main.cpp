// test_slopsync_readygate — the CATALOG_READY dual-plane
// readiness gate (SPEC §8.4 / RFC-015) and NACK↔frame correlation (RFC-001).
//
// The problem being tested: a hub used to push retained STATE the instant a
// session was granted, but a client with no cached catalog cannot DECODE a
// packed payload yet — which is why the browser client shipped a hand-copied
// table of this device's layouts, exactly the coupling the self-describing
// catalog exists to prevent. The gate makes undecodable state never get
// transmitted at all, and (this was the review blocker) covers the CONTROL
// plane too: a pre-READY session that could send intents would be acting
// without ever having received the retained safety latch (§11.5(2)).
//
// Native (host-side, hardware-free), same harness as test_slopsync_session /
// test_slopsync_streamingress: InProcessLink + ManualClock + XorShift32 +
// conformance::miniCatalog(), doctest's bundled main(). Frames are hand-built
// and written raw so a test can choose its own frame-header seq (RG-07) and
// can deliberately NOT declare readiness.
//
// Suite ids: RG-xx = readiness gate.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/client/client.hpp"
#include "slopsync/conformance/mini_catalog.hpp"
#include "slopsync/core/clock.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/hub/hub.hpp"
#include "slopsync/transport/inprocess_binding.hpp"
#include "slopsync/util/byte_io.hpp"
#include "slopsync/wire/catalog_etag.hpp"
#include "slopsync/wire/frame_header.hpp"
#include "slopsync/wire/messages/goodbye.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/intent.hpp"
#include "slopsync/wire/messages/nack.hpp"
#include "slopsync/wire/messages/welcome.hpp"
#include "slopsync/wire/raw/catalog_ready.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <vector>

using namespace slopsync;

namespace {

constexpr uint16_t kSafetyCh = 0x0003;   // STATE, watch, on-change (retained at construction)
constexpr uint16_t kIntentCh = 0x0084;   // INTENT, control
constexpr uint16_t kStatusCh = 0x0082;   // STATE, watch, 10 Hz
constexpr uint16_t kUnknownCh = 0x00FF;  // in no catalog

// ---- delegate: counts applications so "NOT_READY was not applied" is provable
class ReadyHubDelegate final : public HubDelegate {
public:
    int applyIntentCallCount = 0;
    int sessionsLeft = 0;

    AccessLevel validateToken(std::span<const std::byte>, std::span<const std::byte>, bool hasToken) override {
        return hasToken ? AccessLevel::control : AccessLevel::watch;
    }
    Result<IntentValueMap, NackCode> applyIntent(uint16_t channel_id, const IntentValueMap& requested, AccessLevel,
                                                  bool& cfgChanged) override {
        ++applyIntentCallCount;
        if (channel_id != kIntentCh) return Result<IntentValueMap, NackCode>::err(NackCode::UNKNOWN_CHANNEL);
        cfgChanged = true;
        IntentValueMap out{};
        out.count = requested.count;
        out.fields = requested.fields;
        return Result<IntentValueMap, NackCode>::ok(out);
    }
    void onEstop(uint8_t, uint8_t) override {}
    void onSessionLeft(uint32_t) override { ++sessionsLeft; }
};

// ---- raw frame helpers ------------------------------------------------------
void writeFrame(ITransport& ep, FrameType type, uint16_t channel, std::span<const std::byte> payload,
                uint16_t seq = 0) {
    std::array<std::byte, 400> buf{};
    FrameHeader h;
    h.type = uint8_t(type);
    h.flags = 0;
    h.channel = channel;
    h.seq = seq;
    h.len = uint16_t(payload.size());
    size_t pos = encodeFrameHeader(h, std::span<std::byte>(buf));
    REQUIRE(pos > 0);
    if (!payload.empty()) std::memcpy(buf.data() + pos, payload.data(), payload.size());
    REQUIRE(ep.write(std::span<const std::byte>(buf.data(), pos + payload.size())));
}

// HELLO with optional cached etag + one subscription wish per entry.
struct SubWish { uint16_t channel_id; float rate_hz; uint8_t priority; };
void writeHello(ITransport& ep, uint8_t idByte, bool withToken, std::vector<SubWish> subs,
                std::optional<std::array<std::byte, limits::etag_bytes>> cachedEtag = std::nullopt) {
    HelloMsg m{};
    m.proto_ver = kProtocolVersion;
    m.client_kind = "sim";
    m.client_name = "readygate";
    m.instance_id.fill(std::byte{0});
    m.instance_id[0] = std::byte{idByte};
    if (withToken) {
        m.has_token = true;
        m.token.fill(std::byte{0xAA});
    }
    if (cachedEtag) {
        m.has_catalog_etag = true;
        m.catalog_etag = *cachedEtag;
    }
    m.subscriptions_count = uint32_t(subs.size());
    for (size_t i = 0; i < subs.size(); ++i) {
        m.subscriptions[i].channel_id = subs[i].channel_id;
        m.subscriptions[i].rate_hz = subs[i].rate_hz;
        m.subscriptions[i].priority = subs[i].priority;
    }
    std::array<std::byte, 400> buf{};
    size_t n = encodeHello(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(ep, FrameType::HELLO, 0, std::span<const std::byte>(buf.data(), n));
}

void writeCatalogReady(ITransport& ep, std::span<const std::byte> etag) {
    std::array<std::byte, kCatalogReadyBytes> buf{};
    size_t n = encodeCatalogReady(etag, std::span<std::byte>(buf));
    REQUIRE(n == kCatalogReadyBytes);
    writeFrame(ep, FrameType::CATALOG_READY, 0, std::span<const std::byte>(buf.data(), n));
}

void writeIntent(ITransport& ep, uint16_t channel_id, uint16_t intent_id, uint16_t frameSeq) {
    IntentMsg m{};
    m.channel_id = channel_id;
    m.intent_id = intent_id;
    m.value_count = 1;
    m.value[0] = IntentValueField{1, IntentValue::ofF32(120.0f)};
    std::array<std::byte, 200> buf{};
    size_t n = encodeIntent(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(ep, FrameType::INTENT, channel_id, std::span<const std::byte>(buf.data(), n), frameSeq);
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

int countType(const std::vector<DecodedReply>& replies, FrameType t) {
    int n = 0;
    for (const auto& r : replies)
        if (r.type == t) ++n;
    return n;
}

std::optional<WelcomeMsg> findWelcome(const std::vector<DecodedReply>& replies) {
    for (const auto& r : replies) {
        if (r.type != FrameType::WELCOME) continue;
        auto w = decodeWelcome(std::span<const std::byte>(r.payload));
        if (w) return w.value();
    }
    return std::nullopt;
}

std::optional<NackMsg> findNack(const std::vector<DecodedReply>& replies) {
    for (const auto& r : replies) {
        if (r.type != FrameType::NACK) continue;
        auto n = decodeNack(std::span<const std::byte>(r.payload));
        if (n) return n.value();
    }
    return std::nullopt;
}

// The hub's catalog etag, computed the same way Hub does internally (§8.3).
std::array<std::byte, limits::etag_bytes> miniEtag() {
    static Catalog32 cat;  // built once; never copied (a Catalog32 is tens of KiB)
    static const bool built = conformance::buildMiniCatalog(cat);
    (void)built;
    std::array<std::byte, 8192> scratch{};
    return catalogEtag(cat, std::span<std::byte>(scratch));
}

}  // namespace

// ---- RG-01 ------------------------------------------------------------------
// a session with no cached etag gets NOTHING on the data plane until
// it declares CATALOG_READY; the retained push then fires (not a spool: the
// value was never queued anywhere, it just stayed in the hub's channel table).
TEST_CASE("RG-01: retained STATE is withheld until CATALOG_READY, then flows") {
    Catalog32 cat;
    REQUIRE(conformance::buildMiniCatalog(cat));
    ManualClock clock;
    XorShift32 rng(201);
    ReadyHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    writeHello(link.endpointB(), 1, /*token=*/true, {{kSafetyCh, 0.0f, 3}});  // no cached etag
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());
    CHECK(w->limits_info.retained_pending == 1);         // the hub still ADVERTISES what is coming
    CHECK(countType(replies, FrameType::STATE) == 0);    // ...but sends none of it yet
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK_FALSE(hub.sessionBySlot(0)->ready);

    // Several more ticks: still gated, still nothing buffered up.
    for (int i = 0; i < 5; ++i) {
        auto more = tickAndDrain(hub, clock, link.endpointB());
        CHECK(countType(more, FrameType::STATE) == 0);
    }

    writeCatalogReady(link.endpointB(), std::span<const std::byte>(w->catalog_etag));
    auto after = tickAndDrain(hub, clock, link.endpointB());
    CHECK(hub.sessionBySlot(0)->ready);
    CHECK_FALSE(hub.sessionBySlot(0)->readyEtagMismatch);
    REQUIRE(countType(after, FrameType::STATE) == 1);    // exactly ONE retained snapshot, not six
    for (const auto& r : after) {
        if (r.type == FrameType::STATE) CHECK(r.channel == kSafetyCh);
    }
}

// ---- RG-02 ------------------------------------------------------------------
// HELLO carrying a MATCHING etag is proof of possession: ready
// immediately, retained push in the very same update() the WELCOME went out
// in. This is the 99% reconnect case and MUST keep its zero added latency.
TEST_CASE("RG-02: a matching HELLO etag is instantly ready — retained STATE rides the WELCOME tick") {
    Catalog32 cat;
    REQUIRE(conformance::buildMiniCatalog(cat));
    ManualClock clock;
    XorShift32 rng(202);
    ReadyHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    writeHello(link.endpointB(), 2, true, {{kSafetyCh, 0.0f, 3}}, miniEtag());
    auto replies = tickAndDrain(hub, clock, link.endpointB());

    REQUIRE(findWelcome(replies).has_value());
    CHECK(hub.sessionBySlot(0)->ready);
    CHECK(countType(replies, FrameType::STATE) == 1);  // same tick — no extra round trip
}

// ---- RG-03 ------------------------------------------------------------------
// the gate covers the CONTROL plane: a pre-READY INTENT is refused
// NOT_READY and NEVER applied (§11.5(2) — it has not adopted the safety latch).
// The same intent succeeds once readiness is declared.
TEST_CASE("RG-03: a pre-READY INTENT is NACKed NOT_READY and not applied; post-READY it applies") {
    Catalog32 cat;
    REQUIRE(conformance::buildMiniCatalog(cat));
    ManualClock clock;
    XorShift32 rng(203);
    ReadyHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    writeHello(link.endpointB(), 3, true, {{kSafetyCh, 0.0f, 3}});
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());

    writeIntent(link.endpointB(), kIntentCh, /*intent_id=*/7, /*frameSeq=*/0);
    auto refused = tickAndDrain(hub, clock, link.endpointB());

    auto n = findNack(refused);
    REQUIRE(n.has_value());
    CHECK(n->code == NackCode::NOT_READY);
    CHECK(n->has_intent_id);
    CHECK(n->intent_id == 7);
    CHECK(n->has_channel_id);
    CHECK(n->channel_id == kIntentCh);
    CHECK(countType(refused, FrameType::ECHO) == 0);
    CHECK(del.applyIntentCallCount == 0);  // refused, NOT queued for later

    // Declare readiness, retry the SAME intent id: now it applies.
    writeCatalogReady(link.endpointB(), std::span<const std::byte>(w->catalog_etag));
    tickAndDrain(hub, clock, link.endpointB());
    writeIntent(link.endpointB(), kIntentCh, /*intent_id=*/7, /*frameSeq=*/0);
    auto accepted = tickAndDrain(hub, clock, link.endpointB());

    CHECK(countType(accepted, FrameType::ECHO) == 1);
    CHECK(del.applyIntentCallCount == 1);
}

// ---- RG-04 ------------------------------------------------------------------
// CATALOG_READY is IDEMPOTENT (a client on a lossy binding re-sends it
// until STATE arrives), and a wrong-size payload is dropped like any raw frame.
TEST_CASE("RG-04: duplicate CATALOG_READY frames are harmless; a wrong-size one is dropped") {
    Catalog32 cat;
    REQUIRE(conformance::buildMiniCatalog(cat));
    ManualClock clock;
    XorShift32 rng(204);
    ReadyHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    writeHello(link.endpointB(), 4, true, {{kSafetyCh, 0.0f, 3}});
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());

    // A truncated declaration is not a declaration.
    std::array<std::byte, 4> shortEtag{};
    writeFrame(link.endpointB(), FrameType::CATALOG_READY, 0, std::span<const std::byte>(shortEtag));
    auto stillGated = tickAndDrain(hub, clock, link.endpointB());
    CHECK_FALSE(hub.sessionBySlot(0)->ready);
    CHECK(countType(stillGated, FrameType::STATE) == 0);
    CHECK(countType(stillGated, FrameType::NACK) == 0);  // raw plane: dropped, never NACKed

    // First real declaration opens the plane.
    writeCatalogReady(link.endpointB(), std::span<const std::byte>(w->catalog_etag));
    auto first = tickAndDrain(hub, clock, link.endpointB());
    CHECK(countType(first, FrameType::STATE) == 1);

    // Three more identical declarations: pure flag-sets. No re-push, no NACK,
    // no session churn — the retained value is only re-sent when it CHANGES.
    for (int i = 0; i < 3; ++i) writeCatalogReady(link.endpointB(), std::span<const std::byte>(w->catalog_etag));
    auto dupes = tickAndDrain(hub, clock, link.endpointB());
    CHECK(hub.sessionBySlot(0)->ready);
    CHECK(countType(dupes, FrameType::STATE) == 0);
    CHECK(countType(dupes, FrameType::NACK) == 0);
    CHECK(del.sessionsLeft == 0);
}

// ---- RG-05 ------------------------------------------------------------------
// §8.5 degraded operation: a client declaring a DIFFERENT etag still
// becomes ready (it told us what it operates against, and append-only layouts
// make its prefix-parse safe), but the divergence is recorded on the session.
TEST_CASE("RG-05: a mismatched CATALOG_READY etag still opens the plane, flagged degraded") {
    Catalog32 cat;
    REQUIRE(conformance::buildMiniCatalog(cat));
    ManualClock clock;
    XorShift32 rng(205);
    ReadyHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    writeHello(link.endpointB(), 5, true, {{kSafetyCh, 0.0f, 3}});
    REQUIRE(findWelcome(tickAndDrain(hub, clock, link.endpointB())).has_value());

    std::array<std::byte, limits::etag_bytes> stale{};
    stale.fill(std::byte{0x5A});
    writeCatalogReady(link.endpointB(), std::span<const std::byte>(stale));
    auto after = tickAndDrain(hub, clock, link.endpointB());

    CHECK(hub.sessionBySlot(0)->ready);
    CHECK(hub.sessionBySlot(0)->readyEtagMismatch);   // observable, for logs/roster
    CHECK(countType(after, FrameType::STATE) == 1);   // still served (§8.5)
}

// ---- RG-06 ------------------------------------------------------------------
// a client that PINGs forever but never READYs is ALIVE, so no
// liveness path would ever reap it; catalog_ready_timeout_ms GOODBYEs it with
// READY_TIMEOUT and frees the slot. A session that DID ready is never reaped.
TEST_CASE("RG-06: a never-READY session is GOODBYE'd READY_TIMEOUT; a ready one is untouched") {
    Catalog32 cat;
    REQUIRE(conformance::buildMiniCatalog(cat));
    ManualClock clock;
    XorShift32 rng(206);
    ReadyHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink linkA(clock, rng), linkB(clock, rng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));  // slot 0: never declares readiness
    REQUIRE(hub.attachTransport(linkB.endpointA()));  // slot 1: declares it
    REQUIRE(linkA.endpointB().open());
    REQUIRE(linkB.endpointB().open());

    writeHello(linkA.endpointB(), 6, true, {{kSafetyCh, 0.0f, 3}});
    writeHello(linkB.endpointB(), 7, true, {{kSafetyCh, 0.0f, 3}});
    tickAndDrain(hub, clock, linkA.endpointB());
    auto wB = findWelcome(tickAndDrain(hub, clock, linkB.endpointB()));
    REQUIRE(wB.has_value());
    writeCatalogReady(linkB.endpointB(), std::span<const std::byte>(wB->catalog_etag));
    tickAndDrain(hub, clock, linkB.endpointB());
    REQUIRE(hub.sessionCount() == 2);

    // Advance past catalog_ready_timeout_ms while BOTH keep PINGing (alive, but
    // A never adopting) — the exact shape that idle reaping cannot catch, and
    // the reason READY_TIMEOUT has to exist as a separate regime. B pings too:
    // since RFC-024 landed, a session silent for idle_reap_multiplier x
    // ping_interval_idle_ms would be reaped for THAT instead, which would prove
    // nothing about readiness.
    std::vector<DecodedReply> fromA;
    for (int i = 0; i < 40; ++i) {
        writeFrame(linkA.endpointB(), FrameType::PING, 0, std::span<const std::byte>{});
        writeFrame(linkB.endpointB(), FrameType::PING, 0, std::span<const std::byte>{});
        auto r = tickAndDrain(hub, clock, linkA.endpointB(), /*stepUs=*/500'000);
        fromA.insert(fromA.end(), r.begin(), r.end());
        tickAndDrain(hub, clock, linkB.endpointB(), /*stepUs=*/0);
    }

    CHECK(hub.sessionCount() == 1);            // A reaped, B alive
    CHECK(hub.sessionBySlot(1)->ready);
    CHECK(del.sessionsLeft == 1);

    bool sawReadyTimeout = false;
    for (const auto& r : fromA) {
        if (r.type != FrameType::GOODBYE) continue;
        auto gb = decodeGoodbye(std::span<const std::byte>(r.payload));
        if (gb && gb.value().code == NackCode::READY_TIMEOUT) sawReadyTimeout = true;
    }
    CHECK(sawReadyTimeout);
}

// ---- RG-07 ------------------------------------------------------------------
// RFC-001: a NACK carries `intent_seq`, the frame-header seq of the
// inbound frame it refuses, so a client pipelining several frames on ONE
// channel can tell which one was rejected. `intent_id` alone cannot: an
// UNKNOWN_CHANNEL on a SUBSCRIBE/PUBLISH has no intent id at all.
TEST_CASE("RG-07: NACKs echo the refused frame's header seq in intent_seq") {
    Catalog32 cat;
    REQUIRE(conformance::buildMiniCatalog(cat));
    ManualClock clock;
    XorShift32 rng(207);
    ReadyHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    writeHello(link.endpointB(), 8, true, {{kSafetyCh, 0.0f, 3}}, miniEtag());  // instantly ready
    REQUIRE(findWelcome(tickAndDrain(hub, clock, link.endpointB())).has_value());

    // Two intents in flight on the SAME channel, distinct frame seqs; the
    // second names an unknown channel so exactly one NACK comes back.
    writeIntent(link.endpointB(), kIntentCh, /*intent_id=*/11, /*frameSeq=*/4242);
    writeIntent(link.endpointB(), kUnknownCh, /*intent_id=*/12, /*frameSeq=*/4243);
    auto replies = tickAndDrain(hub, clock, link.endpointB());

    CHECK(countType(replies, FrameType::ECHO) == 1);
    auto n = findNack(replies);
    REQUIRE(n.has_value());
    CHECK(n->code == NackCode::UNKNOWN_CHANNEL);
    REQUIRE(n->has_intent_seq);
    CHECK(n->intent_seq == 4243);   // the SECOND frame, not the first
    CHECK(n->intent_id == 12);
}

// ---- RG-08 ------------------------------------------------------------------
// end-to-end with the library's own Client: a cold connect (no cached
// etag) fetches the catalog, verifies the hash locally, declares readiness on
// its own, and reaches LIVE. This is the loop every external client must
// mirror, so it is asserted from BOTH sides: hub-side ready bit + client LIVE.
TEST_CASE("RG-08: Client cold-connects, self-declares CATALOG_READY, and reaches LIVE") {
    class CountingDelegate final : public ClientDelegate {
    public:
        ClientSessionState last = ClientSessionState::CLOSED;
        int states = 0;
        void onStateChange(ClientSessionState s) override { last = s; }
        void onState(uint16_t, uint16_t, std::span<const std::byte>) override { ++states; }
        void onEcho(uint16_t, const IntentValueMap&, uint16_t) override {}
        void onNack(const NackMsg&) override {}
        void onPendingDropped(uint16_t) override {}
    };

    Catalog32 cat;
    REQUIRE(conformance::buildMiniCatalog(cat));
    ManualClock clock;
    XorShift32 rng(208);
    ReadyHubDelegate hubDel;
    Hub hub(cat, clock, rng, hubDel);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));

    ClientIdentity id;
    id.instance_id.fill(std::byte{0});
    id.instance_id[0] = std::byte{9};
    id.hasToken = true;
    id.token.fill(std::byte{0xAA});
    id.client_kind = "sim";
    id.client_name = "readygate-client";

    CountingDelegate cdel;
    Client client(id, link.endpointB(), clock, rng, cdel);
    REQUIRE(client.addSubscriptionWish(kSafetyCh, 0.0f, Priority::critical));
    REQUIRE(client.addSubscriptionWish(kStatusCh, 10.0f, Priority::normal));
    REQUIRE(client.connect());  // NO cached etag: the full fetch + declare path

    for (int i = 0; i < 60; ++i) {
        clock.advanceUs(1000);
        hub.update(clock.nowUs());
        client.update(clock.nowUs());
    }

    CHECK(client.state() == ClientSessionState::LIVE);
    CHECK(client.catalogReqCount() == 1);      // it really did transfer the catalog
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->ready);        // ...and really did declare readiness
    CHECK_FALSE(hub.sessionBySlot(0)->readyEtagMismatch);
    CHECK(cdel.states >= 1);                   // retained STATE actually arrived
}
