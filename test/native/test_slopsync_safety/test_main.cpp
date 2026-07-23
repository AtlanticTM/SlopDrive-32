// ============================================================================
// test_main.cpp — doctest behavioral tests for slopsync-core's M5 safety
// milestone: full stop taxonomy (§11), QoS shedding (§10.4), pairing (§12.2),
// and the network probe (§6.4). Driven end-to-end over InProcessLink +
// ManualClock + XorShift32, same harness shape as test_slopsync_session's M4
// suite.
//
// Test catalog: conformance::miniCatalog() (the FROZEN K-suite fixture,
// untouched) plus two appended entries — 0x0004 control-owner (STATE,
// viewer, critical) and 0x0005 safety-intents (INTENT, controller) — kept in
// ascending id order, per the M5 task brief. This is a superset COPY built at
// test-file scope; it does not modify mini_catalog.hpp.
//
// Suite ids: S-05/S-06 deadman policy dispatch, S-07 takeover, S-08 shedding
// + slow-consumer eviction, S-10 pairing. A pure shedDecision() table test
// and an HMAC-SHA256 known-answer test round out the coverage the milestone
// brief asks for. Existing M4 suites (S-01..04, S-09, I-*, E-04) are
// untouched — see the M5 report for the regression statement.
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/client/client.hpp"
#include "slopsync/conformance/mini_catalog.hpp"
#include "slopsync/core/clock.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/hub/hub.hpp"
#include "slopsync/session/pairing.hpp"
#include "slopsync/session/safety.hpp"
#include "slopsync/session/shedding.hpp"
#include "slopsync/transport/inprocess_binding.hpp"
#include "slopsync/util/byte_io.hpp"
#include "slopsync/wire/frame_header.hpp"
#include "slopsync/wire/hmac_sha256.hpp"
#include "slopsync/wire/messages/intent.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace slopsync;

namespace {

// ============================================================================
// Test catalog: miniCatalog() + 0x0004 control-owner + 0x0005 safety-intents,
// ids kept ascending (superset copy — mini_catalog.hpp itself is untouched).
// ============================================================================

Catalog32 safetyCatalog() {
    Catalog32 c = conformance::miniCatalog();  // 0x0003,0x0080,0x0082,0x0084,0x008A,0x0090 (6, ascending)
    REQUIRE(c.count == 6);

    // Make room for two new entries right after 0x0003 (index 0): shift
    // [1..count) up by two slots.
    for (int i = int(c.count) - 1; i >= 1; --i) c.entries[size_t(i) + 2] = c.entries[size_t(i)];
    c.count = uint16_t(c.count + 2);

    auto& e = c.entries;

    // -- 0x0004 "control-owner" — STATE, viewer, critical (§10.1's minimum
    // never-shed set explicitly names this channel). Layout: 4x
    // {source_id:u8, owner_session:u32} = 20 bytes, matching
    // Hub::buildControlOwnerPayload().
    e[1] = CatalogEntry{};
    e[1].id = 0x0004;
    e[1].name = "control-owner";
    e[1].cls = ChannelClass::STATE;
    e[1].dir = Direction::h2c;
    e[1].access = AccessLevel::viewer;
    e[1].maxRateHz = 0.0f;
    e[1].defaultPriority = Priority::critical;
    e[1].fieldCount = 8;
    for (size_t i = 0; i < 4; ++i) {
        e[1].layout[i * 2] = {.name = "source_id", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f};
        e[1].layout[i * 2 + 1] = {.name = "owner_session", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f};
    }

    // -- 0x0005 "safety-intents" — INTENT, controller. Minimal schema (this
    // M5 pass hub-handles the ESTOP_CLEAR op directly by value key, not via
    // catalog schema dispatch — see hub_impl.hpp's handleIntent step 5b).
    e[2] = CatalogEntry{};
    e[2].id = 0x0005;
    e[2].name = "safety-intents";
    e[2].cls = ChannelClass::INTENT;
    e[2].dir = Direction::c2h;
    e[2].access = AccessLevel::controller;
    e[2].maxRateHz = 10.0f;
    e[2].defaultPriority = Priority::critical;
    e[2].fieldCount = 1;
    e[2].schema[0] = {.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = ""};

    return c;
}

// ============================================================================
// Shared fixtures (same shapes as test_slopsync_session's, independent copy)
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

void pump(Hub& hub, ManualClock& clock, std::initializer_list<Client*> clients, int rounds = 6,
          uint32_t stepUs = 1000) {
    for (int i = 0; i < rounds; ++i) {
        clock.advanceUs(stepUs);
        hub.update(clock.nowUs());
        for (auto* c : clients) c->update(clock.nowUs());
    }
}

size_t findSlotForSession(const Hub& hub, uint32_t sessionId) {
    for (size_t i = 0; i < kHubMaxSessions + 1; ++i) {
        const HubSession* s = hub.sessionBySlot(i);
        if (s && s->occupied() && s->session_id == sessionId) return i;
    }
    return size_t(-1);
}

IntentValueMap makeSpeedIntent(float speed) {
    IntentValueMap m{};
    m.count = 1;
    m.fields[0] = IntentValueField{1, IntentValue::ofF32(speed)};
    return m;
}

uint32_t ownerFromControlOwnerPayload(std::span<const std::byte> payload, uint8_t sourceIdx) {
    size_t off = size_t(sourceIdx) * 5;
    REQUIRE(payload.size() >= off + 5);
    return getU32(payload.subspan(off + 1, 4));
}

// Writes a raw, hand-encoded INTENT frame directly onto a transport,
// bypassing the Client object entirely — used by S-08's eviction sub-test to
// generate hub traffic without the Client's own kMaxPendingIntents cap
// getting in the way of "never drain the reply queue".
void sendRawIntent(ITransport& t, uint16_t channel, uint16_t intentId, float value) {
    IntentMsg m{};
    m.channel_id = channel;
    m.intent_id = intentId;
    m.value_count = 1;
    m.value[0] = IntentValueField{1, IntentValue::ofF32(value)};

    std::array<std::byte, 300> ibuf{};
    size_t ilen = encodeIntent(m, std::span<std::byte>(ibuf));
    REQUIRE(ilen > 0);

    std::array<std::byte, kFrameBufferCapacity> fbuf{};
    FrameHeader h;
    h.type = uint8_t(FrameType::INTENT);
    h.flags = 0;
    h.channel = channel;
    h.seq = 0;
    h.len = uint16_t(ilen);
    size_t pos = encodeFrameHeader(h, std::span<std::byte>(fbuf));
    REQUIRE(pos > 0);
    std::memcpy(fbuf.data() + pos, ibuf.data(), ilen);
    t.write(std::span<const std::byte>(fbuf.data(), pos + ilen));
}

std::array<std::byte, 15> makeDiagPayload() {
    std::array<std::byte, 15> buf{};  // i8(1)+i16(2)+i32(4)+u32(4)+f32(4) = 15
    return buf;
}

std::array<std::byte, 2> makeMotionStatusPayload(uint8_t flags) {
    std::array<std::byte, 2> buf{};
    buf[0] = std::byte(flags);
    buf[1] = std::byte(0);
    return buf;
}

// ---- SafetyHubDelegate ------------------------------------------------------
// Configurable source mapping/policy + full recording of every M5 delegate
// hook, so each TEST_CASE below can assert exactly what the hub told it.
class SafetyHubDelegate final : public HubDelegate {
public:
    Hub* hub = nullptr;
    bool grantController = true;

    std::map<uint16_t, uint8_t> channelToSource;
    std::map<uint8_t, SourceLossPolicy> sourcePolicies;
    bool allowClearEstop = true;

    std::vector<uint8_t> deadmanStopped;
    struct OwnershipEvent {
        uint8_t source;
        uint32_t owner;
        uint8_t reason;
    };
    std::vector<OwnershipEvent> ownershipEvents;
    int estopCallCount = 0;

    AccessLevel validateToken(std::span<const std::byte>, std::span<const std::byte>, bool hasToken) override {
        if (hasToken && grantController) return AccessLevel::controller;
        return AccessLevel::viewer;
    }

    Result<IntentValueMap, NackCode> applyIntent(uint16_t, const IntentValueMap& requested, AccessLevel,
                                                  bool& cfgChanged) override {
        cfgChanged = true;
        return Result<IntentValueMap, NackCode>::ok(requested);
    }

    void onEstop(uint8_t, uint8_t) override { ++estopCallCount; }

    std::optional<uint8_t> sourceForChannel(uint16_t channel_id) override {
        auto it = channelToSource.find(channel_id);
        if (it == channelToSource.end()) return std::nullopt;
        return it->second;
    }
    SourceLossPolicy sourcePolicy(uint8_t source_id) override {
        auto it = sourcePolicies.find(source_id);
        return it != sourcePolicies.end() ? it->second : SourceLossPolicy::Stop;
    }
    void onDeadmanStop(uint8_t source_id) override { deadmanStopped.push_back(source_id); }
    void onSourceOwnership(uint8_t source_id, uint32_t owner_session, uint8_t reason) override {
        ownershipEvents.push_back(OwnershipEvent{source_id, owner_session, reason});
    }
    bool canClearEstop() override { return allowClearEstop; }
};

// ---- TestClientDelegate -----------------------------------------------------
struct RecordedNack {
    NackCode code = NackCode::MALFORMED;
    bool has_intent_id = false;
    uint16_t intent_id = 0;
};

struct RecordedEcho {
    uint16_t intent_id = 0;
    IntentValueMap applied{};
    uint16_t cfg_gen = 0;
};

struct RecordedPairGrant {
    std::array<std::byte, 16> token{};
    AccessLevel roles = AccessLevel::viewer;
};

class TestClientDelegate final : public ClientDelegate {
public:
    std::vector<ClientSessionState> stateHistory;
    std::map<uint16_t, std::vector<std::byte>> lastStateByChannel;
    std::map<uint16_t, int> stateCountByChannel;
    std::vector<RecordedEcho> echoes;
    std::vector<RecordedNack> nacks;
    std::vector<uint16_t> droppedIntentIds;
    std::vector<RecordedPairGrant> pairGrants;

    void onStateChange(ClientSessionState s) override { stateHistory.push_back(s); }

    void onState(uint16_t channel_id, uint16_t, std::span<const std::byte> payload) override {
        lastStateByChannel[channel_id] = std::vector<std::byte>(payload.begin(), payload.end());
        ++stateCountByChannel[channel_id];
    }

    void onEcho(uint16_t intent_id, const IntentValueMap& applied, uint16_t cfg_gen) override {
        echoes.push_back(RecordedEcho{intent_id, applied, cfg_gen});
    }

    void onNack(const NackMsg& n) override {
        nacks.push_back(RecordedNack{n.code, n.has_intent_id, n.intent_id});
    }

    void onPendingDropped(uint16_t intent_id) override { droppedIntentIds.push_back(intent_id); }

    void onPairGrant(std::span<const std::byte> token, AccessLevel roles) override {
        RecordedPairGrant r;
        std::memcpy(r.token.data(), token.data(), std::min(token.size(), r.token.size()));
        r.roles = roles;
        pairGrants.push_back(r);
    }
};

}  // namespace

// ============================================================================
// Pure function: shedDecision() exhaustive table (§10.4, M5 milestone brief)
// ============================================================================
TEST_CASE("shed table (pure): shedDecision matches the M5 exhaustive table") {
    using SD = ShedDecision;
    struct Case {
        Priority p;
        ChannelClass c;
        uint8_t level;
        SD expect;
    };

    const std::vector<Case> cases = {
        // level 0: Send, unconditionally, every priority x class.
        {Priority::background, ChannelClass::STATE, 0, SD::Send},
        {Priority::background, ChannelClass::STREAM, 0, SD::Send},
        {Priority::background, ChannelClass::INTENT, 0, SD::Send},
        {Priority::background, ChannelClass::EVENT, 0, SD::Send},
        {Priority::normal, ChannelClass::STATE, 0, SD::Send},
        {Priority::normal, ChannelClass::STREAM, 0, SD::Send},
        {Priority::normal, ChannelClass::INTENT, 0, SD::Send},
        {Priority::normal, ChannelClass::EVENT, 0, SD::Send},
        {Priority::elevated, ChannelClass::STATE, 0, SD::Send},
        {Priority::elevated, ChannelClass::STREAM, 0, SD::Send},
        {Priority::elevated, ChannelClass::INTENT, 0, SD::Send},
        {Priority::elevated, ChannelClass::EVENT, 0, SD::Send},
        {Priority::critical, ChannelClass::STATE, 0, SD::Send},
        {Priority::critical, ChannelClass::STREAM, 0, SD::Send},
        {Priority::critical, ChannelClass::INTENT, 0, SD::Send},
        {Priority::critical, ChannelClass::EVENT, 0, SD::Send},

        // level 1: background/normal class-specific; elevated/critical untouched.
        {Priority::background, ChannelClass::STATE, 1, SD::ConflateHard},
        {Priority::background, ChannelClass::STREAM, 1, SD::Decimate4x},
        {Priority::background, ChannelClass::INTENT, 1, SD::Send},
        {Priority::background, ChannelClass::EVENT, 1, SD::Send},
        {Priority::normal, ChannelClass::STATE, 1, SD::Send},
        {Priority::normal, ChannelClass::STREAM, 1, SD::Decimate2x},
        {Priority::normal, ChannelClass::INTENT, 1, SD::Send},
        {Priority::normal, ChannelClass::EVENT, 1, SD::Send},
        {Priority::elevated, ChannelClass::STATE, 1, SD::Send},
        {Priority::elevated, ChannelClass::STREAM, 1, SD::Send},
        {Priority::elevated, ChannelClass::INTENT, 1, SD::Send},
        {Priority::elevated, ChannelClass::EVENT, 1, SD::Send},
        {Priority::critical, ChannelClass::STATE, 1, SD::Send},
        {Priority::critical, ChannelClass::STREAM, 1, SD::Send},
        {Priority::critical, ChannelClass::INTENT, 1, SD::Send},
        {Priority::critical, ChannelClass::EVENT, 1, SD::Send},

        // level 2: priority alone decides, uniformly across every class.
        {Priority::background, ChannelClass::STATE, 2, SD::Drop},
        {Priority::background, ChannelClass::STREAM, 2, SD::Drop},
        {Priority::background, ChannelClass::INTENT, 2, SD::Drop},
        {Priority::background, ChannelClass::EVENT, 2, SD::Drop},
        {Priority::normal, ChannelClass::STATE, 2, SD::Decimate4x},
        {Priority::normal, ChannelClass::STREAM, 2, SD::Decimate4x},
        {Priority::normal, ChannelClass::INTENT, 2, SD::Decimate4x},
        {Priority::normal, ChannelClass::EVENT, 2, SD::Decimate4x},
        {Priority::elevated, ChannelClass::STATE, 2, SD::Decimate2x},
        {Priority::elevated, ChannelClass::STREAM, 2, SD::Decimate2x},
        {Priority::elevated, ChannelClass::INTENT, 2, SD::Decimate2x},
        {Priority::elevated, ChannelClass::EVENT, 2, SD::Decimate2x},
        {Priority::critical, ChannelClass::STATE, 2, SD::Send},
        {Priority::critical, ChannelClass::STREAM, 2, SD::Send},
        {Priority::critical, ChannelClass::INTENT, 2, SD::Send},
        {Priority::critical, ChannelClass::EVENT, 2, SD::Send},
    };
    CHECK(cases.size() == 48);  // 4 priorities x 4 classes x 3 levels, exhaustive

    for (const auto& c : cases) {
        CAPTURE(int(c.p));
        CAPTURE(int(c.c));
        CAPTURE(int(c.level));
        CHECK(shedDecision(c.p, c.c, c.level) == c.expect);
    }
}

// ============================================================================
// S-05 — deadman, Stop policy: silent streamer's source stops motion, latches
// STOP, and the session itself is freed.
// ============================================================================
TEST_CASE("S-05: deadman fires onDeadmanStop + latches STOP for a Stop-policy source, then frees the session") {
    Catalog32 catalog = safetyCatalog();
    ManualClock clock;
    XorShift32 hubRng(2001);
    SafetyHubDelegate hubDelegate;
    hubDelegate.channelToSource[0x0084] = 1;
    hubDelegate.sourcePolicies[1] = SourceLossPolicy::Stop;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink linkA(clock, hubRng), linkB(clock, hubRng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));
    REQUIRE(hub.attachTransport(linkB.endpointA()));

    XorShift32 rngA(2101), rngB(2102);
    TestClientDelegate delegateA, delegateB;
    Client clientA(makeIdentity(1, true), linkA.endpointB(), clock, rngA, delegateA);
    Client clientB(makeIdentity(2, true), linkB.endpointB(), clock, rngB, delegateB);
    clientB.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);

    REQUIRE(clientA.connect());
    REQUIRE(clientB.connect());
    pump(hub, clock, {&clientA, &clientB}, 6);
    REQUIRE(clientA.state() == ClientSessionState::LIVE);
    REQUIRE(clientB.state() == ClientSessionState::LIVE);

    // A activates (acquires) source 1 by sending an intent on the mapped channel.
    REQUIRE(clientA.sendIntent(0x0084, makeSpeedIntent(100.0f)).has_value());
    pump(hub, clock, {&clientA, &clientB}, 4);
    REQUIRE(delegateA.echoes.size() == 1);
    REQUIRE(hubDelegate.ownershipEvents.size() == 1);
    CHECK(hubDelegate.ownershipEvents[0].source == 1);
    CHECK(hubDelegate.ownershipEvents[0].owner == clientA.sessionId());
    CHECK(hubDelegate.ownershipEvents[0].reason == 0);  // acquire

    size_t sessionsBefore = hub.sessionCount();
    CHECK_FALSE(hub.stopLatched());

    // A goes silent (stop pumping it entirely); advance well past deadman_ms
    // in one jump — B keeps being pumped so it can observe the result.
    pump(hub, clock, {&clientB}, /*rounds=*/1, /*stepUs=*/700000);

    REQUIRE(hubDelegate.deadmanStopped.size() == 1);
    CHECK(hubDelegate.deadmanStopped[0] == 1);
    REQUIRE(hubDelegate.ownershipEvents.size() == 2);
    CHECK(hubDelegate.ownershipEvents[1].source == 1);
    CHECK(hubDelegate.ownershipEvents[1].owner == 0);
    CHECK(hubDelegate.ownershipEvents[1].reason == 3);  // deadman-release

    CHECK(hub.stopLatched());
    CHECK(hub.sessionCount() == sessionsBefore - 1);

    // B observes the STOP latch via its own safety shadow.
    CHECK(clientB.stopLatched());
    auto w = clientB.safetyWord();
    REQUIRE(w.has_value());
    CHECK((*w & safety_bits::STOP) != 0);
}

// ============================================================================
// S-06 — deadman, Continue policy: hub-autonomous source keeps running,
// ownership just releases; no STOP; the source is immediately reacquirable.
// ============================================================================
TEST_CASE("S-06: deadman on a Continue-policy source releases ownership only, no STOP, immediately reacquirable") {
    Catalog32 catalog = safetyCatalog();
    ManualClock clock;
    XorShift32 hubRng(2201);
    SafetyHubDelegate hubDelegate;
    hubDelegate.channelToSource[0x0084] = 2;
    hubDelegate.sourcePolicies[2] = SourceLossPolicy::Continue;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink linkA(clock, hubRng), linkB(clock, hubRng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));
    REQUIRE(hub.attachTransport(linkB.endpointA()));

    XorShift32 rngA(2301), rngB(2302);
    TestClientDelegate delegateA, delegateB;
    Client clientA(makeIdentity(3, true), linkA.endpointB(), clock, rngA, delegateA);
    Client clientB(makeIdentity(4, true), linkB.endpointB(), clock, rngB, delegateB);

    REQUIRE(clientA.connect());
    REQUIRE(clientB.connect());
    pump(hub, clock, {&clientA, &clientB}, 6);
    REQUIRE(clientA.state() == ClientSessionState::LIVE);
    REQUIRE(clientB.state() == ClientSessionState::LIVE);

    REQUIRE(clientA.sendIntent(0x0084, makeSpeedIntent(50.0f)).has_value());
    pump(hub, clock, {&clientA, &clientB}, 4);
    REQUIRE(hubDelegate.ownershipEvents.size() == 1);

    size_t sessionsBefore = hub.sessionCount();

    pump(hub, clock, {&clientB}, /*rounds=*/1, /*stepUs=*/700000);

    CHECK(hubDelegate.deadmanStopped.empty());  // Continue policy: no onDeadmanStop
    REQUIRE(hubDelegate.ownershipEvents.size() == 2);
    CHECK(hubDelegate.ownershipEvents[1].source == 2);
    CHECK(hubDelegate.ownershipEvents[1].owner == 0);
    CHECK(hubDelegate.ownershipEvents[1].reason == 3);
    CHECK_FALSE(hub.stopLatched());
    CHECK(hub.sessionCount() == sessionsBefore - 1);  // the session itself still dies (§6.5, M5 scope note)

    // B can acquire source 2 immediately — no lingering conflict.
    REQUIRE(clientB.sendIntent(0x0084, makeSpeedIntent(75.0f)).has_value());
    pump(hub, clock, {&clientB}, 4);
    REQUIRE(delegateB.echoes.size() == 1);
    REQUIRE(delegateB.nacks.empty());
    REQUIRE(hubDelegate.ownershipEvents.size() == 3);
    CHECK(hubDelegate.ownershipEvents[2].source == 2);
    CHECK(hubDelegate.ownershipEvents[2].owner == clientB.sessionId());
    CHECK(hubDelegate.ownershipEvents[2].reason == 0);
}

// ============================================================================
// S-07 — takeover: NACK TAKEOVER_REQUIRED without the flag; TakenOver with it
// (equal role); control-owner STATE (0x0004) reflects the change to both.
// ============================================================================
TEST_CASE("S-07: same-source contention — TAKEOVER_REQUIRED, then takeover=true transfers ownership") {
    Catalog32 catalog = safetyCatalog();
    ManualClock clock;
    XorShift32 hubRng(2401);
    SafetyHubDelegate hubDelegate;
    hubDelegate.channelToSource[0x0084] = 1;
    hubDelegate.sourcePolicies[1] = SourceLossPolicy::Stop;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink linkA(clock, hubRng), linkB(clock, hubRng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));
    REQUIRE(hub.attachTransport(linkB.endpointA()));

    XorShift32 rngA(2501), rngB(2502);
    TestClientDelegate delegateA, delegateB;
    Client clientA(makeIdentity(5, true), linkA.endpointB(), clock, rngA, delegateA);
    Client clientB(makeIdentity(6, true), linkB.endpointB(), clock, rngB, delegateB);
    clientA.addSubscriptionWish(0x0004, 0.0f, Priority::critical);
    clientB.addSubscriptionWish(0x0004, 0.0f, Priority::critical);

    REQUIRE(clientA.connect());
    REQUIRE(clientB.connect());
    pump(hub, clock, {&clientA, &clientB}, 6);
    REQUIRE(clientA.state() == ClientSessionState::LIVE);
    REQUIRE(clientB.state() == ClientSessionState::LIVE);

    // A owns source 1.
    REQUIRE(clientA.sendIntent(0x0084, makeSpeedIntent(100.0f)).has_value());
    pump(hub, clock, {&clientA, &clientB}, 4);
    REQUIRE(delegateA.echoes.size() == 1);
    REQUIRE(hubDelegate.ownershipEvents.size() == 1);
    CHECK(hubDelegate.ownershipEvents[0].reason == 0);

    // Both observed the initial control-owner STATE with source 1 -> A.
    REQUIRE(delegateA.lastStateByChannel.count(0x0004));
    REQUIRE(delegateB.lastStateByChannel.count(0x0004));
    CHECK(ownerFromControlOwnerPayload(delegateB.lastStateByChannel[0x0004], 1) == clientA.sessionId());

    // B intents the same channel WITHOUT takeover -> NACK TAKEOVER_REQUIRED.
    REQUIRE(clientB.sendIntent(0x0084, makeSpeedIntent(200.0f)).has_value());
    pump(hub, clock, {&clientA, &clientB}, 4);
    REQUIRE(delegateB.nacks.size() == 1);
    CHECK(delegateB.nacks[0].code == NackCode::TAKEOVER_REQUIRED);
    REQUIRE(hubDelegate.ownershipEvents.size() == 1);  // unchanged: no transfer happened

    // B retries with takeover=true; equal role (both controller) -> TakenOver.
    REQUIRE(clientB.sendIntent(0x0084, makeSpeedIntent(200.0f), std::nullopt, /*takeover=*/true).has_value());
    pump(hub, clock, {&clientA, &clientB}, 4);
    REQUIRE(delegateB.echoes.size() == 1);
    REQUIRE(hubDelegate.ownershipEvents.size() == 2);
    CHECK(hubDelegate.ownershipEvents[1].source == 1);
    CHECK(hubDelegate.ownershipEvents[1].owner == clientB.sessionId());
    CHECK(hubDelegate.ownershipEvents[1].reason == 1);  // takeover

    // control-owner STATE (0x0004) updates, observed by both A and B.
    REQUIRE(delegateA.lastStateByChannel.count(0x0004));
    REQUIRE(delegateB.lastStateByChannel.count(0x0004));
    CHECK(ownerFromControlOwnerPayload(delegateA.lastStateByChannel[0x0004], 1) == clientB.sessionId());
    CHECK(ownerFromControlOwnerPayload(delegateB.lastStateByChannel[0x0004], 1) == clientB.sessionId());
}

// ============================================================================
// S-08 — congestion shedding order (§10.4) live through the hub's STATE
// pacing loop, plus slow-consumer eviction after a stalled never-shed queue.
//
// Deviation note: the milestone brief frames the shedding half of S-08 around
// a background-priority STREAM ("diag") vs an elevated-priority STREAM
// ("position"). This hub/hub_impl.hpp implementation does not wire ANY
// STREAM-class publish/pacing path (STREAM pacing is explicitly out of scope
// per hub_impl.hpp's own M4/M5 comments — there is no publishStream() to
// drive live traffic through). The STREAM/elevated combinations are still
// covered exhaustively and correctly by the pure shedDecision() table test
// above; THIS test demonstrates the same §10.4 ordering live, through the
// only pacing loop that exists, using two real STATE channels of different
// priority (0x0090 diag/background, 0x0082 motion-status/normal).
// ============================================================================
TEST_CASE("S-08: congestion shedding decimates background before normal/critical; stalled critical writes evict") {
    Catalog32 catalog = safetyCatalog();
    ManualClock clock;
    XorShift32 hubRng(2601);
    SafetyHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    XorShift32 rng(2701);
    TestClientDelegate delegate;
    Client client(makeIdentity(7, true), link.endpointB(), clock, rng, delegate);
    client.addSubscriptionWish(0x0090, 2.0f, Priority::background);   // diag
    client.addSubscriptionWish(0x0082, 10.0f, Priority::normal);      // motion-status
    client.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);

    REQUIRE(client.connect());
    pump(hub, clock, {&client}, 6);
    REQUIRE(client.state() == ClientSessionState::LIVE);

    size_t slot = findSlotForSession(hub, client.sessionId());
    REQUIRE(slot != size_t(-1));

    // §9.1's mandatory push-on-grant is itself never shed (see
    // pumpStatePacing's firstPushSinceGrant bypass) — publish+deliver one
    // value for each channel BEFORE congestion is set and BEFORE the counted
    // loops below, so that one-time freebie doesn't skew the decimation
    // ratios the SUBCASEs measure.
    {
        auto diagPayload = makeDiagPayload();
        auto motionPayload = makeMotionStatusPayload(0);
        hub.publishState(0x0090, std::span<const std::byte>(diagPayload));
        hub.publishState(0x0082, std::span<const std::byte>(motionPayload));
        pump(hub, clock, {&client}, 1, 600000);
    }

    SUBCASE("level 1: background ConflateHard-throttled (1 of 4), normal untouched") {
        hub.setCongestionLevel(slot, 1);
        int diag0 = delegate.stateCountByChannel[0x0090];
        int motion0 = delegate.stateCountByChannel[0x0082];

        for (int i = 0; i < 8; ++i) {
            auto diagPayload = makeDiagPayload();
            auto motionPayload = makeMotionStatusPayload(uint8_t(i));
            hub.publishState(0x0090, std::span<const std::byte>(diagPayload));
            hub.publishState(0x0082, std::span<const std::byte>(motionPayload));
            // 600ms step: clears diag's 500ms period AND motion's 100ms
            // period exactly once per iteration (one natural due-opportunity
            // per channel per loop).
            pump(hub, clock, {&client}, 1, 600000);
        }

        int diagDelta = delegate.stateCountByChannel[0x0090] - diag0;
        int motionDelta = delegate.stateCountByChannel[0x0082] - motion0;
        CHECK(motionDelta == 8);  // normal-priority STATE: Send (untouched) at level 1
        CHECK(diagDelta == 2);    // background-priority STATE: ConflateHard, 1 of every 4 gets through
    }

    SUBCASE("level 2: background Drop entirely, normal Decimate4x") {
        hub.setCongestionLevel(slot, 2);
        int diag0 = delegate.stateCountByChannel[0x0090];
        int motion0 = delegate.stateCountByChannel[0x0082];

        for (int i = 0; i < 8; ++i) {
            auto diagPayload = makeDiagPayload();
            auto motionPayload = makeMotionStatusPayload(uint8_t(i));
            hub.publishState(0x0090, std::span<const std::byte>(diagPayload));
            hub.publishState(0x0082, std::span<const std::byte>(motionPayload));
            pump(hub, clock, {&client}, 1, 600000);
        }

        int diagDelta = delegate.stateCountByChannel[0x0090] - diag0;
        int motionDelta = delegate.stateCountByChannel[0x0082] - motion0;
        CHECK(diagDelta == 0);    // background: Drop
        CHECK(motionDelta == 2);  // normal: Decimate4x, 1 of every 4
    }

    SUBCASE("slow-consumer eviction: level 2 + a never-shed queue stalled > 2s evicts the session") {
        hub.setCongestionLevel(slot, 2);

        // Fill the hub->client ring (capacity 16) with ECHOs by feeding raw
        // INTENT frames directly onto the wire and pumping ONLY the hub side
        // (never draining the client's inbound queue) — the same "stop
        // reading client side" fault the milestone brief calls for.
        uint16_t nextId = 1000;
        for (int i = 0; i < 25; ++i) {
            sendRawIntent(link.endpointB(), 0x0084, nextId++, 10.0f);
            hub.update(clock.nowUs());
        }
        CHECK(hub.sessionCount() == 1);  // stalled, but < 2s elapsed: not evicted yet

        clock.advanceUs(2100u * 1000u);  // > never_shed_stall_eviction_ms (2000)
        sendRawIntent(link.endpointB(), 0x0084, nextId++, 10.0f);
        hub.update(clock.nowUs());

        CHECK(hub.sessionCount() == 0);  // GOODBYE SESSION_EVICTED, slot freed
    }
}

// ============================================================================
// S-10 — pairing ceremony (§12.2)
// ============================================================================
TEST_CASE("S-10: pairing grants a controller token via correct PIN proof; a reconnect with it adopts controller") {
    Catalog32 catalog = safetyCatalog();
    ManualClock clock;
    XorShift32 hubRng(2801);
    SafetyHubDelegate hubDelegate;
    hubDelegate.grantController = false;  // the delegate itself must NOT be the source of the controller grant
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    XorShift32 rng(2901);
    TestClientDelegate delegate;
    Client client(makeIdentity(8, false), link.endpointB(), clock, rng, delegate);  // no token: viewer

    REQUIRE(client.connect());
    pump(hub, clock, {&client}, 6);
    REQUIRE(client.state() == ClientSessionState::LIVE);
    CHECK(client.roles() == AccessLevel::viewer);

    const char pin[] = "4821";
    hub.openPairingWindow(std::span<const char>(pin, 4));

    auto proof = pairingPinProof(std::span<const char>(pin, 4), client.nonce());
    REQUIRE(client.sendPairReq(std::span<const std::byte>(proof)));
    pump(hub, clock, {&client}, 4);

    REQUIRE(delegate.pairGrants.size() == 1);
    CHECK(delegate.pairGrants[0].roles == AccessLevel::controller);
    std::array<std::byte, 16> token = delegate.pairGrants[0].token;
    CHECK_FALSE(std::all_of(token.begin(), token.end(), [](std::byte b) { return b == std::byte{0}; }));

    // Reconnect over the SAME transport (same physical link — a realistic
    // "reconnect", §6.7) presenting the granted token -> controller.
    ClientIdentity id2 = makeIdentity(8, true);
    id2.token = token;
    XorShift32 rng2(2902);
    TestClientDelegate delegate2;
    Client client2(id2, link.endpointB(), clock, rng2, delegate2);
    REQUIRE(client2.connect());
    pump(hub, clock, {&client2}, 6);
    REQUIRE(client2.state() == ClientSessionState::LIVE);
    CHECK(client2.roles() == AccessLevel::controller);
}

TEST_CASE("S-10: wrong PIN denies pairing; three failures close the window; further attempts NACK PAIRING_REQUIRED") {
    Catalog32 catalog = safetyCatalog();
    ManualClock clock;
    XorShift32 hubRng(3001);
    SafetyHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    XorShift32 rng(3101);
    TestClientDelegate delegate;
    Client client(makeIdentity(9, false), link.endpointB(), clock, rng, delegate);

    REQUIRE(client.connect());
    pump(hub, clock, {&client}, 6);
    REQUIRE(client.state() == ClientSessionState::LIVE);

    const char rightPin[] = "4821";
    const char wrongPin[] = "0000";
    hub.openPairingWindow(std::span<const char>(rightPin, 4));

    auto wrongProof = pairingPinProof(std::span<const char>(wrongPin, 4), client.nonce());

    for (int i = 0; i < 3; ++i) {
        REQUIRE(client.sendPairReq(std::span<const std::byte>(wrongProof)));
        pump(hub, clock, {&client}, 4);
        REQUIRE(delegate.nacks.size() == size_t(i + 1));
        CHECK(delegate.nacks.back().code == NackCode::PAIRING_DENIED);
    }

    // Window is now closed (3 consecutive failures, §12.2) — even the
    // CORRECT proof gets PAIRING_REQUIRED, not a grant.
    auto rightProof = pairingPinProof(std::span<const char>(rightPin, 4), client.nonce());
    REQUIRE(client.sendPairReq(std::span<const std::byte>(rightProof)));
    pump(hub, clock, {&client}, 4);
    REQUIRE(delegate.nacks.size() == 4);
    CHECK(delegate.nacks.back().code == NackCode::PAIRING_REQUIRED);
    CHECK(delegate.pairGrants.empty());
}

// ============================================================================
// HMAC-SHA256 known-answer test — RFC 4231 Test Case 2 (key "Jefe").
// Ground truth independently computed via .NET's HMACSHA256 (not transcribed
// from memory) to avoid a hand-copied-hex-digit error.
// ============================================================================
TEST_CASE("S-10 (HMAC KAT): RFC 4231 test case 2 — key \"Jefe\", full 32-byte digest") {
    const std::string key = "Jefe";
    const std::string msg = "what do ya want for nothing?";

    auto keyBytes = std::as_bytes(std::span<const char>(key.data(), key.size()));
    auto msgBytes = std::as_bytes(std::span<const char>(msg.data(), msg.size()));
    auto mac = hmacSha256(keyBytes, msgBytes);

    const std::array<uint8_t, 32> expected = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
        0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43,
    };
    REQUIRE(mac.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(uint8_t(mac[i]) == expected[i]);
    }
}
