// test_slopsync_safety — slopsync-core's M5 safety
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

// ---- Test catalog -----------------------------------------------------------
// miniCatalog() + 0x0004 control-owner + 0x0005 safety-intents,
// ids kept ascending (superset copy — mini_catalog.hpp itself is untouched).

// Out-param builder (a Catalog32 is tens of KiB — never a return value).
// Entries are APPENDED in ascending id order, which is also the order the
// shared field pools fill in, so the mini fixture's entries are copied across
// in two halves with 0x0004/0x0005 authored in between.
void safetyCatalog(Catalog32& c) {
    Catalog32 mini;  // 0x0003,0x0080,0x0082,0x0084,0x008A,0x0090 (6, ascending)
    conformance::buildMiniCatalog(mini);
    REQUIRE(mini.count == 6);

    c.clear();

    // -- 0x0003 "safety", copied verbatim (entry AND its pooled fields).
    c.addEntryFrom(mini, mini.entries[0]);

    // -- 0x0004 "control-owner" — STATE, viewer, critical (§10.1's minimum
    // never-shed set explicitly names this channel). Layout: 4x
    // {source_id:u8, owner_session:u32} = 20 bytes, matching
    // Hub::buildControlOwnerPayload().
    c.addEntry({.id = 0x0004, .name = "control-owner",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::critical});
    for (size_t i = 0; i < 4; ++i) {
        c.addLayoutField({.name = "source_id", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});
        c.addLayoutField({.name = "owner_session", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});
    }

    // -- 0x0005 "safety-intents" — INTENT. RFC-025b: the channel ACCESS FLOOR
    // is `watch`, and the per-op minimum role rides `option_access` (catalog
    // key 17), index-aligned with the option labels so that element i
    // describes WIRE VALUE i. `estop` (6) and `stop` (2) are role-EXEMPT;
    // everything else needs `control`. Index 0 is a "reserved" placeholder
    // (safety_ops starts at 1) gated at `control` — an option label may never
    // be empty, and the strict side is the safe side for a non-op.
    //
    // This mirrors include/comms/SlopSyncCatalog.h's real device entry exactly;
    // if the two drift, a device-catalog test in this repo would still pass
    // while the shipped machine gated differently, which is the failure this
    // duplication is deliberately shaped to make loud.
    c.addEntry({.id = 0x0005, .name = "safety-intents",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::watch, .maxRateHz = 10.0f,
                .defaultPriority = Priority::critical});
    c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = ""},
                           {"reserved", "estop_clear", "stop", "hold", "pause", "resume",
                            "estop", "override_on", "override_off", "bypass_on", "bypass_off"},
                           {AccessLevel::control, AccessLevel::control, AccessLevel::watch,
                            AccessLevel::control, AccessLevel::control, AccessLevel::control,
                            AccessLevel::watch, AccessLevel::control, AccessLevel::control,
                            AccessLevel::control, AccessLevel::control});

    // -- the rest of the mini fixture (0x0080 .. 0x0090); ids stay ascending.
    for (uint16_t i = 1; i < mini.count; ++i) c.addEntryFrom(mini, mini.entries[i]);
    REQUIRE(c.count == 8);
    REQUIRE(c.ok());
}

// ---- Shared fixtures --------------------------------------------------------
// Same shapes as test_slopsync_session's, independent copy.

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

// One safety_ops verb as an INTENT value map: {1: op}.
IntentValueMap makeSafetyOp(uint8_t op) {
    IntentValueMap m{};
    m.count = 1;
    m.fields[0] = IntentValueField{1, IntentValue::ofU64(op)};
    return m;
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

    // RFC-025a: ops this delegate refuses with UNSUPPORTED_OP, so a test can
    // model "this machine does not implement HOLD" and assert the hub latches
    // NOTHING for it.
    std::vector<uint8_t> refuseOps;
    std::vector<uint8_t> acceptedOps;   // safety ops the delegate actually applied

    std::vector<uint8_t> deadmanStopped;
    struct OwnershipEvent {
        uint8_t source;
        uint32_t owner;
        uint8_t reason;
    };
    std::vector<OwnershipEvent> ownershipEvents;
    int estopCallCount = 0;

    AccessLevel validateToken(std::span<const std::byte>, std::span<const std::byte>, bool hasToken) override {
        if (hasToken && grantController) return AccessLevel::control;
        return AccessLevel::watch;
    }

    Result<IntentValueMap, NackCode> applyIntent(uint16_t channel_id, const IntentValueMap& requested, AccessLevel,
                                                  bool& cfgChanged) override {
        if (channel_id == 0x0005) {
            uint8_t op = 0;
            for (uint32_t i = 0; i < requested.count; ++i) {
                if (requested.fields[i].key == 1) op = uint8_t(requested.fields[i].value.u64_val);
            }
            if (std::find(refuseOps.begin(), refuseOps.end(), op) != refuseOps.end()) {
                return Result<IntentValueMap, NackCode>::err(NackCode::UNSUPPORTED_OP);
            }
            acceptedOps.push_back(op);
        }
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
    AccessLevel roles = AccessLevel::watch;
};

class TestClientDelegate final : public ClientDelegate {
public:
    std::vector<ClientSessionState> stateHistory;
    std::map<uint16_t, std::vector<std::byte>> lastStateByChannel;
    std::map<uint16_t, int> stateCountByChannel;
    // Every seq observed per channel, in receipt order — S-11 asserts this is
    // monotonic non-decreasing (never-reordered) across a coalesced burst.
    std::map<uint16_t, std::vector<uint16_t>> seqHistoryByChannel;
    std::vector<RecordedEcho> echoes;
    std::vector<RecordedNack> nacks;
    std::vector<uint16_t> droppedIntentIds;
    std::vector<RecordedPairGrant> pairGrants;
    // Every EVENT received, in receipt order, bytes intact — EVENT never
    // coalesces (S-11), so this stays a full list, not a last-value map.
    std::vector<std::vector<std::byte>> events;

    void onStateChange(ClientSessionState s) override { stateHistory.push_back(s); }

    void onState(uint16_t channel_id, uint16_t seq, std::span<const std::byte> payload) override {
        lastStateByChannel[channel_id] = std::vector<std::byte>(payload.begin(), payload.end());
        ++stateCountByChannel[channel_id];
        seqHistoryByChannel[channel_id].push_back(seq);
    }

    void onEcho(uint16_t intent_id, const IntentValueMap& applied, uint16_t cfg_gen) override {
        echoes.push_back(RecordedEcho{intent_id, applied, cfg_gen});
    }

    void onEvent(uint16_t /*channel_id*/, std::span<const std::byte> encodedPayload) override {
        events.emplace_back(encodedPayload.begin(), encodedPayload.end());
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

// ---- Pure function ----------------------------------------------------------
// shedDecision() exhaustive table (§10.4, M5 milestone brief)
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

// ---- S-05 -------------------------------------------------------------------
// RFC-042/RFC-045: deadman on a Stop-policy source releases ownership
// and marks the session STALE — it no longer latches STOP or calls
// onDeadmanStop, and the session's SLOT is RETAINED (not freed). The declared
// SourceLossPolicy::Stop is proven INERT: the reference hub no longer consults
// it at all (see releaseSessionSources()'s RFC-045 comment).
TEST_CASE("S-05: deadman on a Stop-policy source releases ownership, marks STALE, latches nothing") {
    Catalog32 catalog;
    safetyCatalog(catalog);
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

    CHECK(hubDelegate.deadmanStopped.empty());  // RFC-045: never called, any more, any policy
    REQUIRE(hubDelegate.ownershipEvents.size() == 2);
    CHECK(hubDelegate.ownershipEvents[1].source == 1);
    CHECK(hubDelegate.ownershipEvents[1].owner == 0);
    CHECK(hubDelegate.ownershipEvents[1].reason == 3);  // deadman-release, still reported

    CHECK_FALSE(hub.stopLatched());
    // RFC-042: the slot is RETAINED (marked STALE), not freed.
    CHECK(hub.sessionCount() == sessionsBefore);
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::STALE);

    // B observes NO STOP latch via its own safety shadow — a deadman never
    // was a safety edge after RFC-045.
    CHECK_FALSE(clientB.stopLatched());
    auto w = clientB.safetyWord();
    REQUIRE(w.has_value());
    CHECK((*w & safety_bits::STOP) == 0);
}

// ---- S-06 -------------------------------------------------------------------
// RFC-042/RFC-045: deadman on a Continue-policy source is
// behaviorally IDENTICAL to S-05's Stop-policy case at the hub-library level —
// ownership releases, the session goes STALE, nothing latches, regardless of
// what sourcePolicy() answers. The source is immediately reacquirable by a
// different session, exactly as before.
TEST_CASE("S-06: deadman on a Continue-policy source releases ownership, marks STALE, immediately reacquirable") {
    Catalog32 catalog;
    safetyCatalog(catalog);
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

    CHECK(hubDelegate.deadmanStopped.empty());  // Continue policy: no onDeadmanStop (and never called at all now)
    REQUIRE(hubDelegate.ownershipEvents.size() == 2);
    CHECK(hubDelegate.ownershipEvents[1].source == 2);
    CHECK(hubDelegate.ownershipEvents[1].owner == 0);
    CHECK(hubDelegate.ownershipEvents[1].reason == 3);
    CHECK_FALSE(hub.stopLatched());
    // RFC-042: the session goes STALE, not gone — slot count is unchanged.
    CHECK(hub.sessionCount() == sessionsBefore);

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

// ---- S-07 -------------------------------------------------------------------
// takeover: NACK TAKEOVER_REQUIRED without the flag; TakenOver with it
// (equal role); control-owner STATE (0x0004) reflects the change to both.
TEST_CASE("S-07: same-source contention — TAKEOVER_REQUIRED, then takeover=true transfers ownership") {
    Catalog32 catalog;
    safetyCatalog(catalog);
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

// ---- S-08 -------------------------------------------------------------------
// congestion shedding order (§10.4) live through the hub's STATE
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
TEST_CASE("S-08: congestion shedding decimates background before normal/critical; stalled critical writes evict") {
    Catalog32 catalog;
    safetyCatalog(catalog);
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

// ---- S-11 -------------------------------------------------------------------
// STATE congestion coalescing is last-value-wins (LEDGER "Morning
// ruling batch" item 1, 2026-07-28). RetainedStore already holds exactly one
// value per channel and pumpStatePacing() always reads it fresh at send time
// (see retained_store.hpp/subscription.hpp's own design notes) — no queue
// ever exists to stack duplicates in. S-08 above proves the shedDecision()
// COUNTS; this suite proves the CORRECTNESS invariants a count can't show:
// the final value is bit-exact, seq order is never violated, every channel's
// final value survives even a fully-Dropped congestion window once it
// clears, and EVENT never coalesces or drops under the same congestion this
// throttles STATE under. This is the same hub-side mechanism
// SlopSyncAsyncWsPort::loop() now feeds from the real WS queue watermark
// (src/comms/SlopSyncAsyncWsTransport.cpp) — previously wired for the
// in-process/sim binding only.
TEST_CASE("S-11: STATE congestion coalescing is last-value-wins, never reordered, never permanently lost") {
    Catalog32 catalog;
    safetyCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(3001);
    SafetyHubDelegate hubDelegate;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    XorShift32 rng(3101);
    TestClientDelegate delegate;
    Client client(makeIdentity(9, true), link.endpointB(), clock, rng, delegate);
    client.addSubscriptionWish(0x0090, 2.0f, Priority::background);   // diag
    client.addSubscriptionWish(0x0082, 10.0f, Priority::normal);      // motion-status
    client.addSubscriptionWish(0x008A, 0.0f, Priority::normal);       // anomalies (EVENT)
    client.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);

    REQUIRE(client.connect());
    pump(hub, clock, {&client}, 6);
    REQUIRE(client.state() == ClientSessionState::LIVE);

    size_t slot = findSlotForSession(hub, client.sessionId());
    REQUIRE(slot != size_t(-1));

    // §9.1's push-on-grant freebie, drained before congestion is set — same
    // reasoning as S-08: a never-shed first push must not be mistaken for a
    // coalesced one.
    {
        auto diagPayload = makeDiagPayload();
        auto motionPayload = makeMotionStatusPayload(0);
        hub.publishState(0x0090, std::span<const std::byte>(diagPayload));
        hub.publishState(0x0082, std::span<const std::byte>(motionPayload));
        pump(hub, clock, {&client}, 1, 600000);
    }

    SUBCASE("burst to one channel under severe congestion: bit-exact final value, seq never regresses") {
        hub.setCongestionLevel(slot, 2);  // normal priority -> Decimate4x (§10.4 row 14)

        constexpr int kBurst = 16;
        for (int i = 1; i <= kBurst; ++i) {
            auto motionPayload = makeMotionStatusPayload(uint8_t(i));
            hub.publishState(0x0082, std::span<const std::byte>(motionPayload));
            pump(hub, clock, {&client}, 1, 600000);  // one due-opportunity per iteration
        }

        // Fewer than half the burst actually crossed the wire (Decimate4x) —
        // the point being proven is not the count (S-08 already covers that)
        // but that whichever ones did arrive are correct and in order.
        CHECK(delegate.stateCountByChannel[0x0082] < kBurst);
        CHECK(delegate.stateCountByChannel[0x0082] > 0);

        const auto& lastPayload = delegate.lastStateByChannel[0x0082];
        REQUIRE(lastPayload.size() == 2);
        CHECK(uint8_t(lastPayload[0]) == uint8_t(kBurst));  // last-value-wins, exact bytes

        const auto& seqs = delegate.seqHistoryByChannel[0x0082];
        for (size_t i = 1; i < seqs.size(); ++i) {
            CHECK(seqIsNewer(seqs[i], seqs[i - 1]));  // never delivered out of order
        }

        // Recovery: once congestion clears, the FINAL retained value is not
        // stranded behind the Decimate4x counter forever — the very next due
        // opportunity delivers it (changePending stays true until a send
        // actually lands, see pumpStatePacing's pr->lastSeq bookkeeping).
        hub.setCongestionLevel(slot, 0);
        pump(hub, clock, {&client}, 1, 600000);
        REQUIRE(delegate.lastStateByChannel[0x0082].size() == 2);
        CHECK(uint8_t(delegate.lastStateByChannel[0x0082][0]) == uint8_t(kBurst));
    }

    SUBCASE("interleaved multi-channel burst: every channel's final value survives, even one fully Dropped") {
        hub.setCongestionLevel(slot, 2);  // background -> Drop entirely (row 13), normal -> Decimate4x (row 14)
        const int diag0 = delegate.stateCountByChannel[0x0090];  // baseline: the pre-congestion priming push

        constexpr int kBurst = 12;
        for (int i = 1; i <= kBurst; ++i) {
            auto diagPayload = makeDiagPayload();
            diagPayload[0] = std::byte(i);
            auto motionPayload = makeMotionStatusPayload(uint8_t(i));
            hub.publishState(0x0090, std::span<const std::byte>(diagPayload));
            hub.publishState(0x0082, std::span<const std::byte>(motionPayload));
            pump(hub, clock, {&client}, 1, 600000);
        }

        // 0x0090 is background: every single one of these kBurst pushes was
        // Dropped (delta since the pre-congestion priming push is zero).
        CHECK(delegate.stateCountByChannel[0x0090] - diag0 == 0);

        // Congestion clears; both channels' CURRENT (latest-published) value
        // must still reach the client — a Dropped channel is never a
        // permanently lost one, only a deferred one (§10.4's own framing).
        hub.setCongestionLevel(slot, 0);
        pump(hub, clock, {&client}, 2, 600000);

        REQUIRE(delegate.lastStateByChannel[0x0090].size() == 15);
        CHECK(uint8_t(delegate.lastStateByChannel[0x0090][0]) == uint8_t(kBurst));
        REQUIRE(delegate.lastStateByChannel[0x0082].size() == 2);
        CHECK(uint8_t(delegate.lastStateByChannel[0x0082][0]) == uint8_t(kBurst));
    }

    SUBCASE("EVENT never coalesces or drops while STATE is being shed under the same congestion") {
        hub.setCongestionLevel(slot, 2);  // severe: 0x0090 STATE would Drop entirely (see above)

        constexpr int kEvents = 5;
        for (int i = 1; i <= kEvents; ++i) {
            std::array<std::byte, 1> ev{std::byte(i)};
            REQUIRE(hub.publishEvent(0x008A, std::span<const std::byte>(ev)));
            pump(hub, clock, {&client}, 1, 10000);  // short step: EVENT has no pacing gate to clear
        }

        // Every one arrived, in order, bytes intact — the shedding table's
        // congestion levels never touch EVENT (its own bounded drop-OLDEST
        // queue, §9.4, is independent of link congestion entirely).
        REQUIRE(delegate.events.size() == size_t(kEvents));
        for (int i = 0; i < kEvents; ++i) {
            REQUIRE(delegate.events[size_t(i)].size() == 1);
            CHECK(uint8_t(delegate.events[size_t(i)][0]) == uint8_t(i + 1));
        }
    }
}

// ---- S-10 -------------------------------------------------------------------
// pairing ceremony (§12.2)
TEST_CASE("S-10: pairing grants a controller token via correct PIN proof; a reconnect with it adopts controller") {
    Catalog32 catalog;
    safetyCatalog(catalog);
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
    CHECK(client.roles() == AccessLevel::watch);

    const char pin[] = "4821";
    hub.openPairingWindow(std::span<const char>(pin, 4));

    auto proof = pairingPinProof(std::span<const char>(pin, 4), client.nonce());
    REQUIRE(client.sendPairReq(std::span<const std::byte>(proof)));
    pump(hub, clock, {&client}, 4);

    REQUIRE(delegate.pairGrants.size() == 1);
    CHECK(delegate.pairGrants[0].roles == AccessLevel::control);
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
    CHECK(client2.roles() == AccessLevel::control);
}

TEST_CASE("S-10: wrong PIN denies pairing; three failures close the window; further attempts NACK PAIRING_REQUIRED") {
    Catalog32 catalog;
    safetyCatalog(catalog);
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

// ---- HMAC-SHA256 known-answer test ------------------------------------------
// RFC 4231 Test Case 2 (key "Jefe").
// Ground truth independently computed via .NET's HMACSHA256 (not transcribed
// from memory) to avoid a hand-copied-hex-digit error.
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

// ############################################################################
// M4a — SAFETY SEMANTICS FOR PUBLIC v1.0
//
// RFC-010 (client-assertable e-stop), RFC-025a (the hub latches all four
// levels), RFC-025b (per-op role exemption, expressed in the CATALOG),
// RFC-025c (override/bypass as safety-domain state on the appended snapshot
// byte), RFC-022.3 (session_loss vs deadman cause).
//
// Shared harness note: `safetyCatalog()` above now authors 0x0005 with a
// `watch` access FLOOR plus index-aligned `option_access`, which is the thing
// under test in half of these cases — a viewer must be able to STOP and to
// ESTOP, and must NOT be able to HOLD, PAUSE, RESUME, clear a latch, or flip
// override/bypass.
// ############################################################################

namespace {

// Brings up hub + one client and returns once the client is LIVE. `withToken`
// selects the role the delegate hands out (control vs watch).
struct SafetyRig {
    Catalog32 catalog{};
    ManualClock clock{};
    XorShift32 hubRng{4242};
    SafetyHubDelegate hubDelegate{};
    std::optional<Hub> hub{};
    std::optional<InProcessLink> link{};
    XorShift32 clientRng{4243};
    TestClientDelegate del{};
    std::optional<Client> client{};

    explicit SafetyRig(bool withToken, uint8_t idByte = 60) {
        safetyCatalog(catalog);
        hub.emplace(catalog, clock, hubRng, hubDelegate);
        hubDelegate.hub = &*hub;
        link.emplace(clock, hubRng);
        REQUIRE(hub->attachTransport(link->endpointA()));
        ClientIdentity id = makeIdentity(idByte, withToken);
        client.emplace(id, link->endpointB(), clock, clientRng, del);
        client->addSubscriptionWish(0x0003, 0.0f, Priority::critical);
        REQUIRE(client->connect());
        pump(*hub, clock, {&*client}, 12);
        REQUIRE(client->state() == ClientSessionState::LIVE);
    }

    void step(int rounds = 8) { pump(*hub, clock, {&*client}, rounds); }

    // The `modes` byte (index 8) of the last 0x0003 snapshot this client saw.
    uint8_t lastModes() {
        auto it = del.lastStateByChannel.find(0x0003);
        REQUIRE(it != del.lastStateByChannel.end());
        REQUIRE(it->second.size() == 9);
        return uint8_t(it->second[8]);
    }
    uint8_t lastWord() {
        auto it = del.lastStateByChannel.find(0x0003);
        REQUIRE(it != del.lastStateByChannel.end());
        REQUIRE(it->second.size() >= 1);
        return uint8_t(it->second[0]);
    }
    uint8_t lastCause() {
        auto it = del.lastStateByChannel.find(0x0003);
        REQUIRE(it != del.lastStateByChannel.end());
        REQUIRE(it->second.size() >= 2);
        return uint8_t(it->second[1]);
    }
};

}  // namespace

// ---- RFC-025c ---------------------------------------------------------------
// the snapshot itself: 9 bytes, appended `modes`, prefix-stable.
TEST_CASE("M4a: the 0x0003 snapshot is 9 bytes and its first 8 are unchanged") {
    SafetyRig rig(/*withToken=*/true);

    auto& snap = rig.del.lastStateByChannel[0x0003];
    REQUIRE(snap.size() == 9);
    // Fresh hub, nothing latched: every byte clear, INCLUDING the new one.
    for (size_t i = 0; i < snap.size(); ++i) {
        CAPTURE(i);
        CHECK(uint8_t(snap[i]) == 0);
    }
    // And the catalog agrees with the encoder — the invariant that actually
    // matters, since a hub publishing a payload its own catalog cannot decode
    // is a client-side decode failure on the SAFETY channel.
    const CatalogEntry* e = rig.catalog.find(channels::safety);
    REQUIRE(e != nullptr);
    CHECK(rig.catalog.layoutWireSize(*e) == 9);
}

// ---- RFC-010 ----------------------------------------------------------------
// a client can ASSERT the e-stop, and a WATCH session can too.
TEST_CASE("M4a/RFC-010: safety_ops::estop latches exactly like a 0xE5 frame") {
    SafetyRig rig(/*withToken=*/true);
    REQUIRE_FALSE(rig.hub->estopLatched());

    REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::estop)).has_value());
    rig.step();

    // Motion stopped BEFORE bookkeeping (11.2): the delegate hook ran.
    CHECK(rig.hubDelegate.estopCallCount == 1);
    CHECK(rig.hub->estopLatched());
    CHECK((rig.hub->safetyWord() & safety_bits::ESTOP) != 0);
    // Published at critical priority to the subscriber, with cause=user.
    CHECK((rig.lastWord() & safety_bits::ESTOP) != 0);
    CHECK(rig.lastCause() == safety_causes::user);
    // ECHOed, not NACKed.
    CHECK(rig.del.nacks.empty());
    REQUIRE(rig.del.echoes.size() == 1);
    REQUIRE(rig.del.echoes[0].applied.count == 1);
    CHECK(rig.del.echoes[0].applied.fields[0].value.u64_val == safety_ops::estop);
    // It never reached the delegate's applyIntent — hub-handled, like
    // estop_clear. That equivalence with the raw 0xE5 path is the point.
    CHECK(rig.hubDelegate.acceptedOps.empty());
}

TEST_CASE("M4a/RFC-025b: a WATCH session may estop and stop, but nothing else") {
    SafetyRig rig(/*withToken=*/false);   // no token -> AccessLevel::watch

    SUBCASE("estop is role-EXEMPT — the person in the room can stop the machine") {
        REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::estop)).has_value());
        rig.step();
        CHECK(rig.hub->estopLatched());
        CHECK(rig.del.nacks.empty());
    }

    SUBCASE("stop is role-EXEMPT too") {
        REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::stop)).has_value());
        rig.step();
        CHECK(rig.del.nacks.empty());
        CHECK((rig.hub->safetyWord() & safety_bits::STOP) != 0);
    }

    SUBCASE("hold / pause / resume / estop_clear / override / bypass need control") {
        const uint8_t gated[] = {safety_ops::estop_clear, safety_ops::hold,        safety_ops::pause,
                                 safety_ops::resume,      safety_ops::override_on, safety_ops::override_off,
                                 safety_ops::bypass_on,   safety_ops::bypass_off};
        for (uint8_t op : gated) {
            CAPTURE(int(op));
            rig.del.nacks.clear();
            REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(op)).has_value());
            rig.step();
            REQUIRE(rig.del.nacks.size() == 1);
            CHECK(rig.del.nacks[0].code == NackCode::NOT_CONTROLLER);
        }
        // Nothing latched, nothing applied, machine untouched.
        CHECK(rig.hub->safetyWord() == 0);
        CHECK(rig.hub->safetyModes() == 0);
        CHECK(rig.hubDelegate.acceptedOps.empty());
    }

    SUBCASE("an UNKNOWN op is at least as gated as the strictest known one") {
        // Option index past the end of the option_access vector: it must NOT
        // fall back to the channel floor, or an unregistered verb would be the
        // cheapest thing on a safety channel to reach.
        REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(200)).has_value());
        rig.step();
        REQUIRE(rig.del.nacks.size() == 1);
        CHECK(rig.del.nacks[0].code == NackCode::NOT_CONTROLLER);
    }
}

TEST_CASE("M4a/RFC-025b: role-exempt ops are STILL rate-limited") {
    SafetyRig rig(/*withToken=*/false);   // watch
    // The 9.3 limiter is per SESSION, and it is what bounds a viewer
    // loop-stopping the machine. Fire a burst far past any plausible bucket
    // depth WITHOUT advancing the clock.
    int accepted = 0;
    for (int i = 0; i < 80; ++i) {
        if (rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::stop)).has_value()) ++accepted;
        rig.hub->update(rig.clock.nowUs());
        rig.client->update(rig.clock.nowUs());
    }
    REQUIRE(accepted > 0);
    rig.step(4);
    const size_t rateLimited =
        size_t(std::count_if(rig.del.nacks.begin(), rig.del.nacks.end(),
                             [](const RecordedNack& n) { return n.code == NackCode::RATE_LIMITED; }));
    CHECK(rateLimited > 0);   // exemption is from ROLE, never from the limiter
}

// ---- RFC-025a ---------------------------------------------------------------
// the hub latches all four levels, on delegate ACCEPTANCE.
TEST_CASE("M4a/RFC-025a: the HUB latches STOP/HOLD/PAUSE and RESUME lifts the right two") {
    SafetyRig rig(/*withToken=*/true);

    REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::hold)).has_value());
    rig.step();
    CHECK((rig.hub->safetyWord() & safety_bits::HOLD) != 0);
    CHECK((rig.lastWord() & safety_bits::HOLD) != 0);   // subscribers see it, not just the sender

    REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::pause)).has_value());
    rig.step();
    CHECK((rig.hub->safetyWord() & safety_bits::PAUSE) != 0);

    REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::stop)).has_value());
    rig.step();
    CHECK((rig.hub->safetyWord() & safety_bits::STOP) != 0);
    CHECK(rig.hub->safetyWord() == (safety_bits::STOP | safety_bits::HOLD | safety_bits::PAUSE));

    REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::resume)).has_value());
    rig.step();
    // RESUME lifts HOLD and PAUSE only. STOP is cleared by NEW MOTION (11.1),
    // never by resume, and ESTOP needs estop_clear + its preconditions.
    CHECK((rig.hub->safetyWord() & safety_bits::HOLD) == 0);
    CHECK((rig.hub->safetyWord() & safety_bits::PAUSE) == 0);
    CHECK((rig.hub->safetyWord() & safety_bits::STOP) != 0);
    CHECK(rig.del.nacks.empty());
}

TEST_CASE("M4a/RFC-025a: a delegate that does not implement a level NACKs and latches NOTHING") {
    SafetyRig rig(/*withToken=*/true);
    rig.hubDelegate.refuseOps.push_back(safety_ops::hold);

    REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::hold)).has_value());
    rig.step();

    REQUIRE(rig.del.nacks.size() == 1);
    CHECK(rig.del.nacks[0].code == NackCode::UNSUPPORTED_OP);   // discoverable and honest
    CHECK(rig.hub->safetyWord() == 0);                          // and NOT silently latched
}

// ---- RFC-025c ---------------------------------------------------------------
// override/bypass write through 0x0005 and read back on 0x0003.
TEST_CASE("M4a/RFC-025c: override/bypass ops drive the appended modes byte") {
    SafetyRig rig(/*withToken=*/true);
    CHECK(rig.hub->safetyModes() == 0);

    REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::override_on)).has_value());
    rig.step();
    CHECK(rig.hub->safetyModes() == safety_mode_bits::OVERRIDE);
    CHECK(rig.lastModes() == safety_mode_bits::OVERRIDE);

    REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::bypass_on)).has_value());
    rig.step();
    CHECK(rig.hub->safetyModes() == (safety_mode_bits::OVERRIDE | safety_mode_bits::BYPASS));
    CHECK(rig.lastModes() == (safety_mode_bits::OVERRIDE | safety_mode_bits::BYPASS));

    REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::override_off)).has_value());
    rig.step();
    CHECK(rig.hub->safetyModes() == safety_mode_bits::BYPASS);

    REQUIRE(rig.client->sendIntent(0x0005, makeSafetyOp(safety_ops::bypass_off)).has_value());
    rig.step();
    CHECK(rig.hub->safetyModes() == 0);
    CHECK(rig.lastModes() == 0);
    CHECK(rig.del.nacks.empty());

    // The four ops DID reach the delegate (unlike estop/estop_clear): the
    // machine is what actually engages an override, the hub only latches the
    // fact afterwards.
    CHECK(rig.hubDelegate.acceptedOps.size() == 4);
}

TEST_CASE("M4a/RFC-025c: setSafetyModes is the machine-side direction, publishing on change only") {
    SafetyRig rig(/*withToken=*/true);
    const int before = rig.del.stateCountByChannel[0x0003];

    rig.hub->setSafetyModes(true, false);   // e.g. the legacy UI plane flipped it
    rig.step();
    CHECK(rig.lastModes() == safety_mode_bits::OVERRIDE);
    const int afterChange = rig.del.stateCountByChannel[0x0003];
    CHECK(afterChange > before);

    // Value-identical re-assert: ground truth did not move, so neither does
    // the wire (the same discipline RFC-002 imposed on cfg_gen).
    rig.hub->setSafetyModes(true, false);
    rig.step();
    CHECK(rig.del.stateCountByChannel[0x0003] == afterChange);
}

// ---- RFC-045 ----------------------------------------------------------------
// source loss (any door, any cause) latches NOTHING any more. This
// supersedes the old RFC-022.3 test pair, which proved the latched CAUSE told
// GOODBYE apart from a real silence timeout; RFC-045 removed the latch itself,
// so there is no cause byte left to distinguish — both tests now prove the
// stronger, simpler property directly.
TEST_CASE("M4a/RFC-045: a GOODBYE releases ownership but latches nothing") {
    Catalog32 catalog;
    safetyCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(7311);
    SafetyHubDelegate hubDelegate;
    hubDelegate.channelToSource[0x0084] = 1;
    hubDelegate.sourcePolicies[1] = SourceLossPolicy::Stop;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink owner(clock, hubRng), watcher(clock, hubRng);
    REQUIRE(hub.attachTransport(owner.endpointA()));
    REQUIRE(hub.attachTransport(watcher.endpointA()));

    XorShift32 rngA(7312), rngB(7313);
    TestClientDelegate delA, delB;
    Client a(makeIdentity(71, true), owner.endpointB(), clock, rngA, delA);
    Client b(makeIdentity(72, true), watcher.endpointB(), clock, rngB, delB);
    b.addSubscriptionWish(0x0003, 0.0f, Priority::critical);
    REQUIRE(a.connect());
    REQUIRE(b.connect());
    pump(hub, clock, {&a, &b}, 14);
    REQUIRE(a.state() == ClientSessionState::LIVE);
    REQUIRE(b.state() == ClientSessionState::LIVE);

    // A takes the source, then says GOODBYE — a graceful departure. Before
    // RFC-045 this latched STOP with cause=session_loss (the fix that stopped
    // it being misreported as a deadman); RFC-045 removes the latch entirely,
    // so a graceful departure is now — correctly — silent on the safety plane.
    REQUIRE(a.sendIntent(0x0084, makeSpeedIntent(100.0f)).has_value());
    pump(hub, clock, {&a, &b}, 6);
    a.disconnect();
    pump(hub, clock, {&a, &b}, 10);

    CHECK((hub.safetyWord() & safety_bits::STOP) == 0);
    CHECK_FALSE(hub.stopLatched());
    // The retained snapshot is UNCHANGED (still all-clear) — release-only
    // teardown never republishes it at all now that nothing latches.
    auto& snap = delB.lastStateByChannel[0x0003];
    REQUIRE(snap.size() == 9);
    CHECK((uint8_t(snap[0]) & safety_bits::STOP) == 0);
}

TEST_CASE("M4a/RFC-045: an actual silence timeout ALSO latches nothing — the deadman is not a safety mechanism") {
    Catalog32 catalog;
    safetyCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(7321);
    SafetyHubDelegate hubDelegate;
    hubDelegate.channelToSource[0x0084] = 1;
    hubDelegate.sourcePolicies[1] = SourceLossPolicy::Stop;
    Hub hub(catalog, clock, hubRng, hubDelegate);
    hubDelegate.hub = &hub;

    InProcessLink owner(clock, hubRng);
    REQUIRE(hub.attachTransport(owner.endpointA()));
    XorShift32 rngA(7322);
    TestClientDelegate delA;
    Client a(makeIdentity(73, true), owner.endpointB(), clock, rngA, delA);
    a.addSubscriptionWish(0x0003, 0.0f, Priority::critical);
    REQUIRE(a.connect());
    pump(hub, clock, {&a}, 14);
    REQUIRE(a.state() == ClientSessionState::LIVE);
    REQUIRE(a.sendIntent(0x0084, makeSpeedIntent(100.0f)).has_value());
    pump(hub, clock, {&a}, 6);
    REQUIRE(hubDelegate.deadmanStopped.empty());

    // Go silent: pump the HUB ONLY, past the deadman window.
    for (int i = 0; i < 400; ++i) {
        clock.advanceUs(5000);
        hub.update(clock.nowUs());
    }
    // RFC-045: onDeadmanStop is never called any more, for any policy — the
    // session went STALE (RFC-042) and its source was released, and that is
    // the whole story.
    CHECK(hubDelegate.deadmanStopped.empty());
    CHECK((hub.safetyWord() & safety_bits::STOP) == 0);
    CHECK_FALSE(hub.stopLatched());
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::STALE);

    // Nothing new is on the wire to drain — no safety edge happened — but the
    // formerly-silent client still gets a coherent, all-clear shadow.
    for (int i = 0; i < 6; ++i) {
        clock.advanceUs(1000);
        hub.update(clock.nowUs());
        a.update(clock.nowUs());
    }
    auto& snap = delA.lastStateByChannel[0x0003];
    REQUIRE(snap.size() == 9);
    CHECK((uint8_t(snap[0]) & safety_bits::STOP) == 0);
}
