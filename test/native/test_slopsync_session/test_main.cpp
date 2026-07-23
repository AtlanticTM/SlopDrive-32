// ============================================================================
// test_main.cpp — doctest behavioral tests for slopsync-core's M4 session
// engine: hub/hub.hpp (Hub) and client/client.hpp (Client), driven end-to-end
// over InProcessLink + ManualClock + XorShift32 + conformance::miniCatalog().
//
// Native (host-side, hardware-free) test, same pattern as the M2/M3 suites
// (test_slopsync_wire, test_slopsync_transport): doctest's bundled main(), no
// Arduino, no bus/FreeRTOS dependency. SPEC section numbers cite
// docs/slopsync/SPEC.md.
//
// Suite ids: S-xx = session lifecycle, I-xx = intent/echo/nack, E-04 = ESTOP
// repeat-until-latched under loss. Each maps to one behavioral requirement in
// the M4 milestone brief.
// ============================================================================

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

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <vector>

using namespace slopsync;

namespace {

// ============================================================================
// Test fixtures
// ============================================================================

ClientIdentity makeIdentity(uint8_t idByte, bool withToken) {
    ClientIdentity id;
    id.instance_id.fill(std::byte{0});
    id.instance_id[0] = std::byte{idByte};
    id.hasToken = withToken;
    if (withToken) id.token.fill(std::byte{0xAA});
    id.client_kind = "sim";
    id.client_name = "test-client";
    return id;
}

std::array<std::byte, 8> makeSafetyPayload(bool estop, uint8_t cause, uint32_t owner, uint16_t estopSeq) {
    std::array<std::byte, 8> buf{};
    std::span<std::byte> s(buf);
    putU8(s.subspan(0, 1), uint8_t(estop ? 1 : 0));
    putU8(s.subspan(1, 1), cause);
    putU32(s.subspan(2, 4), owner);
    putU16(s.subspan(6, 2), estopSeq);
    return buf;
}

std::array<std::byte, 2> makeMotionStatusPayload(uint8_t flags) {
    std::array<std::byte, 2> buf{};
    buf[0] = std::byte(flags);
    buf[1] = std::byte(0);
    return buf;
}

std::array<std::byte, 15> makeDiagPayload() {
    std::array<std::byte, 15> buf{};  // i8(1)+i16(2)+i32(4)+u32(4)+f32(4) = 15
    return buf;
}

bool bytesEqual(std::span<const std::byte> a, std::span<const std::byte> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// The catalog etag, computed the same way Hub does internally (§8.3) — lets
// tests preset a client's cached etag to a KNOWN-matching or KNOWN-wrong
// value without depending on Hub internals.
std::array<std::byte, 8> miniCatalogEtag() {
    static Catalog32 cat = conformance::miniCatalog();
    std::array<std::byte, 8192> scratch{};
    return catalogEtag(cat, std::span<std::byte>(scratch));
}

// ---- TestHubDelegate --------------------------------------------------------
// speed channel (0x0084, motion-config-set): clamps field key 1 (f32) to
// [0,400]; cfgChanged=true; mirrors real firmware by publishing a STATE
// broadcast (motion-status, 0x0082) from inside applyIntent. A NOT_HOMED
// refusal mode flag lets I-04 exercise the NACK path.
class TestHubDelegate final : public HubDelegate {
public:
    Hub* hub = nullptr;
    bool notHomedRefusal = false;
    int applyIntentCallCount = 0;
    int estopCallCount = 0;
    uint8_t lastEstopCause = 0;
    uint8_t lastEstopOrigin = 0;
    int sessionsJoined = 0;
    int sessionsLeft = 0;
    bool grantController = true;  // whether a token-bearing HELLO gets controller

    AccessLevel validateToken(std::span<const std::byte>, std::span<const std::byte>, bool hasToken) override {
        if (hasToken && grantController) return AccessLevel::controller;
        return AccessLevel::viewer;
    }

    Result<IntentValueMap, NackCode> applyIntent(uint16_t channel_id, const IntentValueMap& requested, AccessLevel,
                                                  bool& cfgChanged) override {
        ++applyIntentCallCount;
        if (channel_id != 0x0084) {
            return Result<IntentValueMap, NackCode>::err(NackCode::UNKNOWN_CHANNEL);
        }
        if (notHomedRefusal) {
            return Result<IntentValueMap, NackCode>::err(NackCode::NOT_HOMED);
        }

        IntentValueMap out{};
        out.count = requested.count;
        out.fields = requested.fields;
        for (uint32_t i = 0; i < out.count; ++i) {
            if (out.fields[i].key == 1) {  // speed, f32, [0,400]
                float v = out.fields[i].value.f32_val;
                if (v > 400.0f) v = 400.0f;
                if (v < 0.0f) v = 0.0f;
                out.fields[i].value.f32_val = v;
            }
        }
        cfgChanged = true;

        if (hub) {
            static uint8_t toggle = 0;
            ++toggle;
            auto payload = makeMotionStatusPayload(toggle);
            hub->publishState(0x0082, std::span<const std::byte>(payload));
        }
        return Result<IntentValueMap, NackCode>::ok(out);
    }

    void onEstop(uint8_t cause, uint8_t origin) override {
        ++estopCallCount;
        lastEstopCause = cause;
        lastEstopOrigin = origin;
    }
    void onSessionJoined(uint32_t) override { ++sessionsJoined; }
    void onSessionLeft(uint32_t) override { ++sessionsLeft; }
};

// ---- TestClientDelegate -----------------------------------------------------
// Records everything the frozen ClientDelegate contract can report.
struct RecordedNack {
    NackCode code = NackCode::MALFORMED;
    bool has_intent_id = false;
    uint16_t intent_id = 0;
    bool has_retry_after_ms = false;
    uint32_t retry_after_ms = 0;
    bool has_channel_id = false;
    uint16_t channel_id = 0;
};

struct RecordedEcho {
    uint16_t intent_id = 0;
    IntentValueMap applied{};
    uint16_t cfg_gen = 0;
};

class TestClientDelegate final : public ClientDelegate {
public:
    std::vector<ClientSessionState> stateHistory;
    std::map<uint16_t, std::vector<std::byte>> lastStateByChannel;
    std::map<uint16_t, uint16_t> lastSeqByChannel;
    int onStateCallCount = 0;
    std::vector<RecordedEcho> echoes;
    std::vector<RecordedNack> nacks;
    std::vector<uint16_t> droppedIntentIds;
    int onEventCallCount = 0;

    void onStateChange(ClientSessionState s) override { stateHistory.push_back(s); }

    void onState(uint16_t channel_id, uint16_t seq, std::span<const std::byte> payload) override {
        ++onStateCallCount;
        lastStateByChannel[channel_id] = std::vector<std::byte>(payload.begin(), payload.end());
        lastSeqByChannel[channel_id] = seq;
    }

    void onEcho(uint16_t intent_id, const IntentValueMap& applied, uint16_t cfg_gen) override {
        echoes.push_back(RecordedEcho{intent_id, applied, cfg_gen});
    }

    void onNack(const NackMsg& n) override {
        RecordedNack r;
        r.code = n.code;
        r.has_intent_id = n.has_intent_id;
        r.intent_id = n.intent_id;
        r.has_retry_after_ms = n.has_retry_after_ms;
        r.retry_after_ms = n.retry_after_ms;
        r.has_channel_id = n.has_channel_id;
        r.channel_id = n.channel_id;
        nacks.push_back(r);
    }

    void onEvent(uint16_t, std::span<const std::byte>) override { ++onEventCallCount; }

    void onPendingDropped(uint16_t intent_id) override { droppedIntentIds.push_back(intent_id); }
};

IntentValueMap makeSpeedIntent(float speed) {
    IntentValueMap m{};
    m.count = 1;
    m.fields[0] = IntentValueField{1, IntentValue::ofF32(speed)};
    return m;
}

// Pumps `rounds` ticks of (advance clock -> hub.update -> each client.update).
// Zero-latency InProcessLinks deliver within the same tick, so a handful of
// rounds is enough for any handshake/exchange in these tests.
void pump(Hub& hub, ManualClock& clock, std::initializer_list<Client*> clients, int rounds = 6,
          uint32_t stepUs = 1000) {
    for (int i = 0; i < rounds; ++i) {
        clock.advanceUs(stepUs);
        hub.update(clock.nowUs());
        for (auto* c : clients) c->update(clock.nowUs());
    }
}

}  // namespace

// ============================================================================
// S-01 — cold connect: retained push, SYNCING->LIVE gate, shadow fidelity
// ============================================================================
TEST_CASE("S-01: cold connect adopts retained STATE and gates SYNCING->LIVE exactly at retained_pending") {
    Catalog32 catalog = conformance::miniCatalog();
    ManualClock clock;
    XorShift32 hubRng(1001);
    TestHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    // Pre-publish safety + motion-status + diag BEFORE any client connects.
    auto safetyPayload = makeSafetyPayload(false, 0, 0, 0);
    auto motionPayload = makeMotionStatusPayload(0x00);
    auto diagPayload = makeDiagPayload();
    REQUIRE(hub.publishState(0x0003, std::span<const std::byte>(safetyPayload)));
    REQUIRE(hub.publishState(0x0082, std::span<const std::byte>(motionPayload)));
    REQUIRE(hub.publishState(0x0090, std::span<const std::byte>(diagPayload)));

    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));

    XorShift32 clientRng(2002);
    ClientIdentity id = makeIdentity(1, false);
    TestClientDelegate clientDelegate;
    Client client(id, link.endpointB(), clock, clientRng, clientDelegate);

    client.addSubscriptionWish(0x0003, 0.0f, Priority::critical);
    client.addSubscriptionWish(0x0082, 10.0f, Priority::normal);
    client.addSubscriptionWish(0x0090, 2.0f, Priority::background);

    auto etag = miniCatalogEtag();
    client.setCachedEtag(std::span<const std::byte, limits::etag_bytes>(etag));

    REQUIRE(client.connect());
    CHECK(client.state() == ClientSessionState::HELLO_SENT);

    pump(hub, clock, {&client}, /*rounds=*/2);

    CHECK(client.state() == ClientSessionState::LIVE);
    REQUIRE(clientDelegate.stateHistory.size() >= 3);
    CHECK(clientDelegate.stateHistory[0] == ClientSessionState::HELLO_SENT);
    CHECK(clientDelegate.stateHistory[1] == ClientSessionState::SYNCING);
    CHECK(clientDelegate.stateHistory.back() == ClientSessionState::LIVE);

    CHECK(clientDelegate.onStateCallCount == 3);
    REQUIRE(clientDelegate.lastStateByChannel.count(0x0003));
    REQUIRE(clientDelegate.lastStateByChannel.count(0x0082));
    REQUIRE(clientDelegate.lastStateByChannel.count(0x0090));
    CHECK(bytesEqual(clientDelegate.lastStateByChannel[0x0003], std::span<const std::byte>(safetyPayload)));
    CHECK(bytesEqual(clientDelegate.lastStateByChannel[0x0082], std::span<const std::byte>(motionPayload)));
    CHECK(bytesEqual(clientDelegate.lastStateByChannel[0x0090], std::span<const std::byte>(diagPayload)));

    CHECK(client.catalogReqCount() == 0);  // cached etag matched: no catalog transfer
    CHECK(hubDelegate.sessionsJoined == 1);
}

// ============================================================================
// S-02 — reconnect: cached-etag skip vs. wrong-etag full transfer
// ============================================================================
TEST_CASE("S-02: matching cached etag skips CATALOG_REQ on reconnect; wrong etag triggers chunk transfer") {
    Catalog32 catalog = conformance::miniCatalog();
    ManualClock clock;
    XorShift32 hubRng(1101);
    TestHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));

    XorShift32 clientRng(2102);
    ClientIdentity id = makeIdentity(2, false);
    TestClientDelegate clientDelegate;
    Client client(id, link.endpointB(), clock, clientRng, clientDelegate);

    // First-ever connect: no cache -> etag mismatch -> one catalog transfer.
    REQUIRE(client.connect());
    pump(hub, clock, {&client}, 6);
    CHECK(client.state() == ClientSessionState::LIVE);
    CHECK(client.catalogReqCount() == 1);
    size_t reqsAfterFirstConnect = client.catalogReqCount();

    // Reconnect with the now-cached (matching) etag: no NEW CATALOG_REQ.
    client.disconnect();
    REQUIRE(client.connect());
    pump(hub, clock, {&client}, 6);
    CHECK(client.state() == ClientSessionState::LIVE);
    CHECK(client.catalogReqCount() == reqsAfterFirstConnect);  // unchanged

    // A fresh client, different instance_id, WRONG cached etag -> full
    // CATALOG_REQ + chunk flow + verified adoption.
    InProcessLink link2(clock, hubRng);
    REQUIRE(hub.attachTransport(link2.endpointA()));
    XorShift32 clientRng2(2103);
    ClientIdentity id2 = makeIdentity(3, false);
    TestClientDelegate clientDelegate2;
    Client client2(id2, link2.endpointB(), clock, clientRng2, clientDelegate2);

    std::array<std::byte, limits::etag_bytes> wrongEtag{};
    wrongEtag.fill(std::byte{0xFF});
    client2.setCachedEtag(std::span<const std::byte, limits::etag_bytes>(wrongEtag));

    REQUIRE(client2.connect());
    pump(hub, clock, {&client2}, 8);

    CHECK(client2.catalogReqCount() >= 1);
    CHECK(client2.state() == ClientSessionState::LIVE);
    // Verified adoption: the client's hub-reported etag now matches the
    // hub's real catalog etag (proof the reassembled bytes hashed correctly).
    auto realEtag = miniCatalogEtag();
    CHECK(bytesEqual(client2.hubEtag(), std::span<const std::byte>(realEtag)));
}

// ============================================================================
// S-03 — reconcile: dropped-before-processed intent is flushed, never resent
// ============================================================================
TEST_CASE("S-03: an intent lost before the hub processes it is flushed via onPendingDropped, never auto-resent") {
    Catalog32 catalog = conformance::miniCatalog();
    ManualClock clock;
    XorShift32 hubRng(1201);
    TestHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));

    XorShift32 clientRng(2202);
    ClientIdentity id = makeIdentity(4, true);  // token -> controller role
    TestClientDelegate clientDelegate;
    Client client(id, link.endpointB(), clock, clientRng, clientDelegate);

    REQUIRE(client.connect());
    pump(hub, clock, {&client}, 6);
    REQUIRE(client.state() == ClientSessionState::LIVE);

    int applyCountBefore = hubDelegate.applyIntentCallCount;

    // 100% loss client->hub: the intent is accepted for "transmission" by the
    // transport (fire-and-forget) but never actually arrives.
    link.profileB().loss_pct = 100;
    auto id1 = client.sendIntent(0x0084, makeSpeedIntent(111.0f));
    REQUIRE(id1.has_value());

    pump(hub, clock, {&client}, 4);
    CHECK(hubDelegate.applyIntentCallCount == applyCountBefore);  // hub never saw it

    // Reconnect (restore delivery first): connect() flushes pending via
    // onPendingDropped, the library never blind-retransmits.
    link.profileB().loss_pct = 0;
    REQUIRE(client.connect());
    REQUIRE(clientDelegate.droppedIntentIds.size() == 1);
    CHECK(clientDelegate.droppedIntentIds[0] == *id1);

    pump(hub, clock, {&client}, 6);
    REQUIRE(client.state() == ClientSessionState::LIVE);
    CHECK(hubDelegate.applyIntentCallCount == applyCountBefore);  // still never saw the dropped one

    // App-level reissue (a NEW intent) succeeds normally.
    auto id2 = client.sendIntent(0x0084, makeSpeedIntent(222.0f));
    REQUIRE(id2.has_value());
    pump(hub, clock, {&client}, 4);
    CHECK(hubDelegate.applyIntentCallCount == applyCountBefore + 1);
    REQUIRE(clientDelegate.echoes.size() == 1);
    CHECK(clientDelegate.echoes[0].intent_id == *id2);
}

// ============================================================================
// S-04 — duplicate instance eviction, and BUSY admission
// ============================================================================
TEST_CASE("S-04: duplicate instance_id evicts the old session; a hub at capacity NACKs BUSY") {
    Catalog32 catalog = conformance::miniCatalog();
    ManualClock clock;
    XorShift32 hubRng(1301);
    TestHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    SUBCASE("duplicate instance_id: old session evicted, new one lives") {
        InProcessLink linkOld(clock, hubRng);
        REQUIRE(hub.attachTransport(linkOld.endpointA()));
        ClientIdentity idA = makeIdentity(10, false);
        XorShift32 rngOld(3001);
        TestClientDelegate delegateOld;
        Client clientOld(idA, linkOld.endpointB(), clock, rngOld, delegateOld);
        REQUIRE(clientOld.connect());
        pump(hub, clock, {&clientOld}, 6);
        REQUIRE(clientOld.state() == ClientSessionState::LIVE);

        InProcessLink linkNew(clock, hubRng);
        REQUIRE(hub.attachTransport(linkNew.endpointA()));
        XorShift32 rngNew(3002);
        TestClientDelegate delegateNew;
        Client clientNew(idA, linkNew.endpointB(), clock, rngNew, delegateNew);  // SAME instance_id
        REQUIRE(clientNew.connect());
        pump(hub, clock, {&clientNew}, 6);

        CHECK(clientNew.state() == ClientSessionState::LIVE);

        // Observed directly on the OLD link: a GOODBYE(DUPLICATE_INSTANCE).
        auto fb = linkOld.endpointB().read();
        REQUIRE(fb.has_value());
        auto header = fb->header();
        REQUIRE(header.has_value());
        CHECK(header->type == uint8_t(FrameType::GOODBYE));
        auto gb = decodeGoodbye(fb->payload());
        REQUIRE(gb.isOk());
        CHECK(gb.value().code == NackCode::DUPLICATE_INSTANCE);
    }

    SUBCASE("BUSY: a hub at capacity NACKs the admitting HELLO with retry_after_ms") {
        std::vector<std::unique_ptr<InProcessLink>> links;
        std::vector<std::unique_ptr<XorShift32>> rngs;
        std::vector<std::unique_ptr<TestClientDelegate>> delegates;
        std::vector<std::unique_ptr<Client>> clients;

        for (uint8_t i = 0; i < kHubMaxSessions; ++i) {
            links.push_back(std::make_unique<InProcessLink>(clock, hubRng));
            REQUIRE(hub.attachTransport(links.back()->endpointA()));
            rngs.push_back(std::make_unique<XorShift32>(4000 + i));
            delegates.push_back(std::make_unique<TestClientDelegate>());
            ClientIdentity id = makeIdentity(20 + i, false);
            clients.push_back(
                std::make_unique<Client>(id, links.back()->endpointB(), clock, *rngs.back(), *delegates.back()));
            REQUIRE(clients.back()->connect());
        }
        pump(hub, clock, {clients[0].get(), clients[1].get(), clients[2].get(), clients[3].get()}, 6);
        for (auto& c : clients) CHECK(c->state() == ClientSessionState::LIVE);
        CHECK(hub.sessionCount() == kHubMaxSessions);

        InProcessLink fifthLink(clock, hubRng);
        REQUIRE(hub.attachTransport(fifthLink.endpointA()));  // physical headroom slot (see hub.hpp's note)
        XorShift32 fifthRng(4999);
        TestClientDelegate fifthDelegate;
        Client fifthClient(makeIdentity(29, false), fifthLink.endpointB(), clock, fifthRng, fifthDelegate);
        REQUIRE(fifthClient.connect());
        pump(hub, clock, {&fifthClient}, 4);

        CHECK(fifthClient.state() == ClientSessionState::CLOSED);
        REQUIRE(fifthDelegate.nacks.size() == 1);
        CHECK(fifthDelegate.nacks[0].code == NackCode::BUSY);
        CHECK(fifthDelegate.nacks[0].has_retry_after_ms);
        CHECK(fifthDelegate.nacks[0].retry_after_ms > 0);
    }
}

// ============================================================================
// S-09 — unsolicited GRANT
// ============================================================================
TEST_CASE("S-09: an unsolicited GRANT updates one client's recorded grant without touching the other") {
    Catalog32 catalog = conformance::miniCatalog();
    ManualClock clock;
    XorShift32 hubRng(1401);
    TestHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink linkA(clock, hubRng);
    InProcessLink linkB(clock, hubRng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));
    REQUIRE(hub.attachTransport(linkB.endpointA()));

    XorShift32 rngA(5001), rngB(5002);
    TestClientDelegate delegateA, delegateB;
    Client clientA(makeIdentity(40, false), linkA.endpointB(), clock, rngA, delegateA);
    Client clientB(makeIdentity(41, false), linkB.endpointB(), clock, rngB, delegateB);

    clientA.addSubscriptionWish(0x0082, 5.0f, Priority::normal);
    clientB.addSubscriptionWish(0x0082, 5.0f, Priority::normal);
    REQUIRE(clientA.connect());
    REQUIRE(clientB.connect());
    pump(hub, clock, {&clientA, &clientB}, 6);
    REQUIRE(clientA.state() == ClientSessionState::LIVE);
    REQUIRE(clientB.state() == ClientSessionState::LIVE);

    REQUIRE(clientA.grantedRateHz(0x0082).has_value());
    CHECK(clientA.grantedRateHz(0x0082).value() == doctest::Approx(5.0f));
    REQUIRE(clientB.grantedRateHz(0x0082).has_value());
    CHECK(clientB.grantedRateHz(0x0082).value() == doctest::Approx(5.0f));

    // Find which slot holds clientA.
    size_t slotA = size_t(-1);
    for (size_t i = 0; i < kHubMaxSessions + 1; ++i) {
        const HubSession* s = hub.sessionBySlot(i);
        if (s && s->occupied() && s->session_id == clientA.sessionId()) {
            slotA = i;
            break;
        }
    }
    REQUIRE(slotA != size_t(-1));

    REQUIRE(hub.regrantForTest(slotA, 0x0082, 2.0f));
    pump(hub, clock, {&clientA, &clientB}, 4);

    CHECK(clientA.grantedRateHz(0x0082).value() == doctest::Approx(2.0f));
    CHECK(clientB.grantedRateHz(0x0082).value() == doctest::Approx(5.0f));  // untouched
}

// ============================================================================
// I-01 — clamp + cfg_gen bump + broadcast to other subscribers
// ============================================================================
TEST_CASE("I-01: an out-of-range intent is clamped, cfg_gen bumps, and other subscribers see the STATE broadcast") {
    Catalog32 catalog = conformance::miniCatalog();
    ManualClock clock;
    XorShift32 hubRng(1501);
    TestHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink linkCtrl(clock, hubRng);
    InProcessLink linkViewer(clock, hubRng);
    REQUIRE(hub.attachTransport(linkCtrl.endpointA()));
    REQUIRE(hub.attachTransport(linkViewer.endpointA()));

    XorShift32 rngCtrl(6001), rngViewer(6002);
    TestClientDelegate delegateCtrl, delegateViewer;
    Client controller(makeIdentity(50, true), linkCtrl.endpointB(), clock, rngCtrl, delegateCtrl);
    Client viewer(makeIdentity(51, false), linkViewer.endpointB(), clock, rngViewer, delegateViewer);
    viewer.addSubscriptionWish(0x0082, 10.0f, Priority::normal);

    REQUIRE(controller.connect());
    REQUIRE(viewer.connect());
    pump(hub, clock, {&controller, &viewer}, 6);
    REQUIRE(controller.state() == ClientSessionState::LIVE);
    REQUIRE(viewer.state() == ClientSessionState::LIVE);

    uint16_t cfgGenBefore = controller.lastCfgGen();
    auto id = controller.sendIntent(0x0084, makeSpeedIntent(420.0f));
    REQUIRE(id.has_value());
    pump(hub, clock, {&controller, &viewer}, 4);

    REQUIRE(delegateCtrl.echoes.size() == 1);
    const RecordedEcho& echo = delegateCtrl.echoes[0];
    CHECK(echo.intent_id == *id);
    CHECK(echo.cfg_gen == cfgGenBefore + 1);
    bool foundSpeed = false;
    for (uint32_t i = 0; i < echo.applied.count; ++i) {
        if (echo.applied.fields[i].key == 1) {
            foundSpeed = true;
            CHECK(echo.applied.fields[i].value.f32_val == doctest::Approx(400.0f));
        }
    }
    CHECK(foundSpeed);

    // The viewer, subscribed to motion-status, observes the config broadcast.
    REQUIRE(delegateViewer.lastStateByChannel.count(0x0082));
    CHECK(delegateViewer.onStateCallCount >= 1);
}

// ============================================================================
// I-02 — duplicate intent_id at the transport level: identical ECHO, one apply
// ============================================================================
TEST_CASE("I-02: resending the identical encoded INTENT frame re-emits identical ECHO bytes, applies once") {
    Catalog32 catalog = conformance::miniCatalog();
    ManualClock clock;
    XorShift32 hubRng(1601);
    TestHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    XorShift32 rng(7001);
    TestClientDelegate delegate;
    Client client(makeIdentity(60, true), link.endpointB(), clock, rng, delegate);
    REQUIRE(client.connect());
    pump(hub, clock, {&client}, 6);
    REQUIRE(client.state() == ClientSessionState::LIVE);

    int applyBefore = hubDelegate.applyIntentCallCount;

    // Hand-craft one INTENT frame and write it TWICE, byte-identical, direct
    // to the transport (bypassing Client::sendIntent's own id bookkeeping).
    IntentMsg m{};
    m.channel_id = 0x0084;
    m.intent_id = 999;
    m.value_count = 1;
    m.value[0] = IntentValueField{1, IntentValue::ofF32(150.0f)};

    std::array<std::byte, 128> payloadBuf{};
    size_t plen = encodeIntent(m, std::span<std::byte>(payloadBuf));
    REQUIRE(plen > 0);

    std::array<std::byte, 160> frameBuf{};
    FrameHeader fh;
    fh.type = uint8_t(FrameType::INTENT);
    fh.flags = 0;
    fh.channel = m.channel_id;
    fh.seq = 0;
    fh.len = uint16_t(plen);
    size_t hlen = encodeFrameHeader(fh, std::span<std::byte>(frameBuf));
    REQUIRE(hlen > 0);
    std::memcpy(frameBuf.data() + hlen, payloadBuf.data(), plen);
    size_t totalLen = hlen + plen;

    REQUIRE(link.endpointB().write(std::span<const std::byte>(frameBuf.data(), totalLen)));
    REQUIRE(link.endpointB().write(std::span<const std::byte>(frameBuf.data(), totalLen)));

    clock.advanceUs(1000);
    hub.update(clock.nowUs());

    auto fb1 = link.endpointB().read();
    auto fb2 = link.endpointB().read();
    REQUIRE(fb1.has_value());
    REQUIRE(fb2.has_value());
    auto h1 = fb1->header();
    auto h2 = fb2->header();
    REQUIRE(h1.has_value());
    REQUIRE(h2.has_value());
    CHECK(h1->type == uint8_t(FrameType::ECHO));
    CHECK(h2->type == uint8_t(FrameType::ECHO));
    CHECK(bytesEqual(fb1->bytes(), fb2->bytes()));

    CHECK(hubDelegate.applyIntentCallCount == applyBefore + 1);
}

// ============================================================================
// I-03 — precondition CAS mismatch -> NACK CONFLICT, no apply
// ============================================================================
TEST_CASE("I-03: a wrong precondition cfg_gen is refused with NACK CONFLICT and never applied") {
    Catalog32 catalog = conformance::miniCatalog();
    ManualClock clock;
    XorShift32 hubRng(1701);
    TestHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    XorShift32 rng(8001);
    TestClientDelegate delegate;
    Client client(makeIdentity(70, true), link.endpointB(), clock, rng, delegate);
    REQUIRE(client.connect());
    pump(hub, clock, {&client}, 6);
    REQUIRE(client.state() == ClientSessionState::LIVE);

    int applyBefore = hubDelegate.applyIntentCallCount;
    uint16_t wrongCfgGen = uint16_t(client.lastCfgGen() + 500);
    auto id = client.sendIntent(0x0084, makeSpeedIntent(200.0f), wrongCfgGen);
    REQUIRE(id.has_value());
    pump(hub, clock, {&client}, 4);

    CHECK(hubDelegate.applyIntentCallCount == applyBefore);
    REQUIRE(delegate.nacks.size() == 1);
    CHECK(delegate.nacks[0].code == NackCode::CONFLICT);
    CHECK(delegate.nacks[0].has_intent_id);
    CHECK(delegate.nacks[0].intent_id == *id);
    CHECK(delegate.echoes.empty());
}

// ============================================================================
// I-04 — refusal mode -> NACK NOT_HOMED
// ============================================================================
TEST_CASE("I-04: the delegate's NOT_HOMED refusal surfaces via onNack") {
    Catalog32 catalog = conformance::miniCatalog();
    ManualClock clock;
    XorShift32 hubRng(1801);
    TestHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;
    hubDelegate.notHomedRefusal = true;

    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    XorShift32 rng(9001);
    TestClientDelegate delegate;
    Client client(makeIdentity(80, true), link.endpointB(), clock, rng, delegate);
    REQUIRE(client.connect());
    pump(hub, clock, {&client}, 6);
    REQUIRE(client.state() == ClientSessionState::LIVE);

    auto id = client.sendIntent(0x0084, makeSpeedIntent(50.0f));
    REQUIRE(id.has_value());
    pump(hub, clock, {&client}, 4);

    REQUIRE(delegate.nacks.size() == 1);
    CHECK(delegate.nacks[0].code == NackCode::NOT_HOMED);
    CHECK(uint16_t(NackCode::NOT_HOMED) == 0x0401);
    CHECK(delegate.echoes.empty());
}

// ============================================================================
// E-04 — ESTOP repeat-until-latched under 30% loss both directions
// ============================================================================
TEST_CASE("E-04: initiateEstop survives 30% loss both directions within estop_repeat_max attempts") {
    Catalog32 catalog = conformance::miniCatalog();
    ManualClock clock;
    XorShift32 hubRng(1901);
    TestHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    XorShift32 rng(4242);
    TestClientDelegate delegate;
    Client client(makeIdentity(90, false), link.endpointB(), clock, rng, delegate);
    client.addSubscriptionWish(0x0003, 0.0f, Priority::critical);

    REQUIRE(client.connect());
    pump(hub, clock, {&client}, 6);
    REQUIRE(client.state() == ClientSessionState::LIVE);
    REQUIRE_FALSE(hub.estopLatched());

    // Seeded 30% loss, both directions, deterministic under this seed.
    link.profileA().loss_pct = 30;
    link.profileB().loss_pct = 30;

    client.initiateEstop(uint8_t(EstopCause::user));

    for (int i = 0; i < 25; ++i) {
        clock.advanceUs(50000);  // limits::estop_repeat_interval_ms == 50
        client.update(clock.nowUs());
        hub.update(clock.nowUs());
    }
    client.update(clock.nowUs());  // let the client observe the last hub reply

    CHECK(hub.estopLatched());
    CHECK(hubDelegate.estopCallCount == 1);
    CHECK_FALSE(client.estopSendFailed());
}
