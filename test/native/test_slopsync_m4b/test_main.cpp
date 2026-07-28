// test_slopsync_m4b — MILESTONE 4b: SlopSync PAIRING
// and TRUST.
//
//   M4B-01..06  ITEM 0  the §9.4 EVENT TWIN of the safety latch (0x000E) —
//                       the twin §5.5/§11.2 have always required and no
//                       registry ever had a home for.
//   M4B-07..11  ITEM 1  the ICrypto seam (RFC-028 obligation 3): constant-time
//                       compare, injection, and the M4c P-256 null object.
//   M4B-12..21  ITEM 2  knock-and-approve (RFC-027 mode (a)) — the headline
//                       ceremony: one button, no display, any configure tier
//                       approves.
//   M4B-22..26  ITEM 3  push-to-pair (RFC-027 mode (c)) + mode advertisement,
//                       and PIN mode (b) still working beside them.
//   M4B-27..31  ITEM 4  the trust ledger as a BLOB STORE (RFC-029/027.4) and
//                       its access gate.
//   M4B-32..36  ITEM 5  the client-change tripwire (RFC-029 item 2).
//
// Native (host-side, hardware-free): InProcessLink + ManualClock + XorShift32,
// doctest's bundled main(), same harness shape as test_slopsync_m3b.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/channel/safety_events_channel.hpp"
#include "slopsync/channel/trust_channels.hpp"
#include "slopsync/core/clock.hpp"
#include "slopsync/core/crypto.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/hub/hub.hpp"
#include "slopsync/transport/inprocess_binding.hpp"
#include "slopsync/wire/blob_chunks.hpp"
#include "slopsync/wire/frame_header.hpp"
#include "slopsync/wire/messages/blob_req.hpp"
#include "slopsync/wire/messages/echo.hpp"
#include "slopsync/wire/messages/event.hpp"
#include "slopsync/wire/messages/goodbye.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/intent.hpp"
#include "slopsync/wire/messages/nack.hpp"
#include "slopsync/wire/messages/pair.hpp"
#include "slopsync/wire/messages/welcome.hpp"
#include "slopsync/wire/raw/catalog_ready.hpp"
#include "slopsync/wire/raw/ping_pong.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace slopsync;

namespace {

constexpr uint16_t kMotionCh = 0x0101;  // an ordinary source-mapped INTENT channel

// ---------------------------------------------------------------------------
// A catalog carrying the WHOLE M4b surface plus one ordinary motion INTENT, so
// one hub exercises safety edges, the admin plane, the pending list and the
// ledger store together.
// ---------------------------------------------------------------------------
void makeM4bCatalog(Catalog32& c, bool withSafetyEvents = true, bool withTrust = true) {
    c.clear();
    // 0x0003 safety — the STATE half of the duality pair.
    c.addEntry({.id = channels::safety, .name = "safety",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::critical});
    c.addBitfieldField({.name = "word", .type = PackedFieldType::bitfield8, .unit = "flag", .scale = 1.0f},
                       {"estop", "stop", "hold", "pause"});
    c.addLayoutField({.name = "cause", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "owner_session", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "estop_seq", .type = PackedFieldType::u16, .unit = "count", .scale = 1.0f});
    c.addBitfieldField({.name = "modes", .type = PackedFieldType::bitfield8, .unit = "flag", .scale = 1.0f},
                       {"override", "bypass"});

    // 0x0005 safety-intents, with the role-exempt ops the device catalog uses.
    c.addEntry({.id = channels::safety_intents, .name = "safety-intents",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::watch, .maxRateHz = 20.0f,
                .defaultPriority = Priority::critical});
    c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = ""},
                           {"reserved", "estop_clear", "stop", "hold", "pause", "resume", "estop",
                            "override_on", "override_off", "bypass_on", "bypass_off"},
                           {AccessLevel::control, AccessLevel::control, AccessLevel::watch,
                            AccessLevel::control, AccessLevel::control, AccessLevel::control,
                            AccessLevel::watch, AccessLevel::control, AccessLevel::control,
                            AccessLevel::control, AccessLevel::control});

    if (withTrust) REQUIRE(addTrustChannels(c));
    if (withSafetyEvents) REQUIRE(addSafetyEventsChannel(c));

    c.addEntry({.id = kMotionCh, .name = "motion",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 50.0f,
                .defaultPriority = Priority::normal});
    c.addSchemaField({.key = 1, .name = "target", .type = CborFieldType::f32_t, .unit = "mm"});
}

// ---------------------------------------------------------------------------
// Delegate. `validateToken` is the DELEGATE-side role source (the hub consults
// its own PairingManager first); tests use a magic token byte to mint an
// administrator without having to pair one first, which keeps each test about
// the one thing it is testing.
// ---------------------------------------------------------------------------
class M4bDelegate final : public HubDelegate {
public:
    int estops = 0;
    int deadmanStops = 0;
    bool clearOk = true;

    // token[0] == 0xC0 -> configure, 0xC1 -> control. Anything else -> watch.
    AccessLevel validateToken(std::span<const std::byte>, std::span<const std::byte> token,
                              bool hasToken) override {
        if (!hasToken || token.empty()) return AccessLevel::watch;
        if (token[0] == std::byte{0xC0}) return AccessLevel::configure;
        if (token[0] == std::byte{0xC1}) return AccessLevel::control;
        return AccessLevel::watch;
    }

    Result<IntentValueMap, NackCode> applyIntent(uint16_t channel_id, const IntentValueMap& requested,
                                                 AccessLevel, bool&) override {
        if (channel_id == channels::safety_intents) {
            IntentValueMap applied{};
            applied.count = requested.count;
            applied.fields = requested.fields;
            return Result<IntentValueMap, NackCode>::ok(applied);
        }
        if (channel_id == kMotionCh) {
            IntentValueMap applied{};
            applied.count = 1;
            applied.fields[0] = IntentValueField{1, IntentValue::ofF32(1.0f)};
            return Result<IntentValueMap, NackCode>::ok(applied);
        }
        return Result<IntentValueMap, NackCode>::err(NackCode::UNKNOWN_CHANNEL);
    }

    void onEstop(uint8_t, uint8_t) override { ++estops; }
    bool canClearEstop() override { return clearOk; }

    std::optional<uint8_t> sourceForChannel(uint16_t channel_id) override {
        if (channel_id == kMotionCh) return uint8_t(1);
        return std::nullopt;
    }
    SourceLossPolicy sourcePolicy(uint8_t) override { return SourceLossPolicy::Stop; }
    void onDeadmanStop(uint8_t) override { ++deadmanStops; }
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

std::array<std::byte, limits::instance_id_bytes> instanceOf(uint8_t idByte) {
    std::array<std::byte, limits::instance_id_bytes> id{};
    id[0] = std::byte{idByte};
    return id;
}

// `tokenTag`: 0 = no token, else token[0]. `ver`: empty = no client_ver.
void writeHello(ITransport& ep, uint8_t idByte, uint8_t tokenTag, std::vector<SubWish> subs,
                std::string_view ver = {}, std::string_view name = "m4b-test",
                const std::array<std::byte, limits::token_bytes>* explicitToken = nullptr) {
    HelloMsg m{};
    m.proto_ver = kProtocolVersion;
    m.client_kind = "sim";
    m.client_name = name;
    m.instance_id = instanceOf(idByte);
    if (explicitToken != nullptr) {
        m.has_token = true;
        m.token = *explicitToken;
    } else if (tokenTag != 0) {
        m.has_token = true;
        m.token.fill(std::byte{0x11});
        m.token[0] = std::byte{tokenTag};
    }
    if (!ver.empty()) {
        m.has_trust = true;
        m.trust_map.has_client_ver = true;
        m.trust_map.client_ver = ver;
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

// Every channel a test might want, so a session sees every edge without each
// case re-listing them.
std::vector<SubWish> allSubs() {
    return {{channels::safety, 0.0f, 3},
            {channels::safety_events, 0.0f, 3},
            {channels::pairing_events, 0.0f, 1},
            {channels::pending_pairing, 0.0f, 1},
            {channels::paired_devices_roster, 0.0f, 0}};
}

WelcomeMsg connectSession(Hub& hub, ManualClock& clock, ITransport& ep, uint8_t idByte, uint8_t tokenTag,
                          std::vector<SubWish> subs, std::string_view ver = {},
                          std::string_view name = "m4b-test",
                          const std::array<std::byte, limits::token_bytes>* tok = nullptr) {
    writeHello(ep, idByte, tokenTag, std::move(subs), ver, name, tok);
    auto replies = tickAndDrain(hub, clock, ep);
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());
    writeCatalogReady(ep, std::span<const std::byte>(w->catalog_etag));
    tickAndDrain(hub, clock, ep);
    return w.value();
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

std::optional<uint64_t> bodyU64(const EventMsg& e, uint8_t key) {
    for (uint32_t i = 0; i < e.body_count; ++i) {
        if (e.body[i].key == key && e.body[i].value.kind == IntentValue::Kind::U64) return e.body[i].value.u64_val;
    }
    return std::nullopt;
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

std::optional<PairGrantMsg> findPairGrant(const std::vector<DecodedReply>& replies) {
    for (const auto& r : replies) {
        if (r.type != FrameType::PAIR_GRANT) continue;
        auto g = decodePairGrant(std::span<const std::byte>(r.payload));
        if (g) return g.value();
    }
    return std::nullopt;
}

std::optional<std::vector<std::byte>> latestState(const std::vector<DecodedReply>& replies, uint16_t channel) {
    std::optional<std::vector<std::byte>> out;
    for (const auto& r : replies) {
        if (r.type == FrameType::STATE && r.channel == channel) out = r.payload;
    }
    return out;
}

void writeSafetyOp(ITransport& ep, uint16_t intentId, uint8_t op) {
    IntentMsg m{};
    m.channel_id = channels::safety_intents;
    m.intent_id = intentId;
    m.value_count = 1;
    m.value[0] = IntentValueField{1, IntentValue::ofU64(op)};
    std::array<std::byte, 128> buf{};
    size_t n = encodeIntent(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(ep, FrameType::INTENT, m.channel_id, std::span<const std::byte>(buf.data(), n));
}

// One admin verb on 0x0009.
void writeAdmin(ITransport& ep, uint16_t intentId, uint8_t op,
                std::optional<std::array<std::byte, limits::instance_id_bytes>> inst = std::nullopt,
                std::optional<uint32_t> sessionId = std::nullopt,
                std::optional<AccessLevel> role = std::nullopt) {
    IntentMsg m{};
    m.channel_id = channels::session_admin;
    m.intent_id = intentId;
    m.value_count = 0;
    m.value[m.value_count++] = IntentValueField{admin_value::op, IntentValue::ofU64(op)};
    if (sessionId) m.value[m.value_count++] = IntentValueField{admin_value::session_id, IntentValue::ofU64(*sessionId)};
    if (inst) {
        m.value[m.value_count++] =
            IntentValueField{admin_value::instance_id, IntentValue::ofBstr(std::span<const std::byte>(*inst))};
    }
    if (role) m.value[m.value_count++] = IntentValueField{admin_value::role, IntentValue::ofU64(uint8_t(*role))};
    std::array<std::byte, 160> buf{};
    size_t n = encodeIntent(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(ep, FrameType::INTENT, m.channel_id, std::span<const std::byte>(buf.data(), n));
}

void writeKnock(ITransport& ep, uint8_t idByte) {
    PairReqMsg m{};
    m.instance_id = instanceOf(idByte);  // has_pin_proof stays false: the knock
    std::array<std::byte, 64> buf{};
    size_t n = encodePairReq(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(ep, FrameType::PAIR_REQ, 0, std::span<const std::byte>(buf.data(), n));
}

}  // namespace

// ---- ITEM 0 -----------------------------------------------------------------
// the §9.4 EVENT TWIN of the safety latch (channel 0x000E)

TEST_CASE("M4B-01: an ESTOP latch emits the estop_latched EVENT twin, carrying seq_of_state") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4001);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, 0, allSubs());

    hub.latchEstop(safety_causes::user, 0, 7);
    auto replies = tickAndDrain(hub, clock, link.endpointB());

    auto evs = collectEvents(replies, channels::safety_events);
    REQUIRE(evs.size() == 1);
    CHECK(evs[0].event_kind == safety_events::estop_latched);
    CHECK(bodyU64(evs[0], safety_body::word).value_or(0) == safety_bits::ESTOP);
    CHECK(bodyU64(evs[0], safety_body::cause).value_or(99) == safety_causes::user);
    CHECK(bodyU64(evs[0], safety_body::estop_seq).value_or(0) == 7);
    // §9.4: the edge NAMES the STATE frame it corresponds to, which is what
    // lets a client that missed the edge reconcile against the latch it got.
    CHECK(evs[0].has_seq_of_state);
}

TEST_CASE("M4B-02: a REPEATED estop re-broadcasts the STATE but emits NO second edge") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4002);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, 0, allSubs());

    hub.latchEstop(safety_causes::user, 0, 7);
    tickAndDrain(hub, clock, link.endpointB());

    // §11.2's repeat-until-latch loss recovery must keep working...
    hub.latchEstop(safety_causes::user, 0, 7);
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    bool sawSafetyState = false;
    for (const auto& r : replies) {
        if (r.type == FrameType::STATE && r.channel == channels::safety) sawSafetyState = true;
    }
    CHECK(sawSafetyState);
    // ...while announcing no edge that did not happen.
    CHECK(collectEvents(replies, channels::safety_events).empty());
    CHECK(del.estops == 1);  // and the delegate was stopped exactly once
}

TEST_CASE("M4B-03: estop_clear emits the estop_cleared edge") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4003);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, 0xC1, allSubs());  // control: may clear

    hub.latchEstop(safety_causes::user, 0, 1);
    tickAndDrain(hub, clock, link.endpointB());

    writeSafetyOp(link.endpointB(), 1, safety_ops::estop_clear);
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    auto evs = collectEvents(replies, channels::safety_events);
    REQUIRE(evs.size() == 1);
    CHECK(evs[0].event_kind == safety_events::estop_cleared);
    CHECK(bodyU64(evs[0], safety_body::word).value_or(0xFF) == 0);
    CHECK_FALSE(hub.estopLatched());
}

TEST_CASE("M4B-04: stop/hold latch and resume clear emit edges whose `level` names the bits that moved") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4004);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, 0xC1, allSubs());

    writeSafetyOp(link.endpointB(), 1, safety_ops::hold);
    auto r1 = tickAndDrain(hub, clock, link.endpointB());
    auto e1 = collectEvents(r1, channels::safety_events);
    REQUIRE(e1.size() == 1);
    CHECK(e1[0].event_kind == safety_events::stop_latched);
    CHECK(bodyU64(e1[0], safety_body::level).value_or(0) == safety_bits::HOLD);
    CHECK(bodyU64(e1[0], safety_body::cause).value_or(99) == safety_causes::user);

    writeSafetyOp(link.endpointB(), 2, safety_ops::pause);
    tickAndDrain(hub, clock, link.endpointB());

    // resume lifts HOLD *and* PAUSE: ONE edge whose level carries both, because
    // that was one operator action.
    writeSafetyOp(link.endpointB(), 3, safety_ops::resume);
    auto r3 = tickAndDrain(hub, clock, link.endpointB());
    auto e3 = collectEvents(r3, channels::safety_events);
    REQUIRE(e3.size() == 1);
    CHECK(e3[0].event_kind == safety_events::stop_cleared);
    CHECK(bodyU64(e3[0], safety_body::level).value_or(0) ==
          uint8_t(safety_bits::HOLD | safety_bits::PAUSE));
}

TEST_CASE("M4B-05 (RFC-042/RFC-045): a rude transport detach releases ownership, goes STALE, and latches nothing") {
    // Superseded expectation: this used to prove the session-loss STOP latch
    // carried the right `cause` (an operator STOP, a §11.3 deadman and a
    // teardown loss policy were otherwise INDISTINGUISHABLE in the 0x0003
    // snapshot). RFC-045 removes the latch itself, so there is no cause byte
    // left to prove. RFC-042 additionally reclassifies "transport reports
    // closed out of band" as a STALE transition, not a teardown — the slot
    // (session_id, grants) is RETAINED rather than freed, exactly like the
    // deadman/idle-reap triggers.
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4005);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink linkA(clock, rng), linkB(clock, rng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));
    REQUIRE(hub.attachTransport(linkB.endpointA()));
    REQUIRE(linkA.endpointB().open());
    REQUIRE(linkB.endpointB().open());

    connectSession(hub, clock, linkA.endpointB(), 1, 0xC1, {});         // the owner
    connectSession(hub, clock, linkB.endpointB(), 2, 0, allSubs());     // the observer

    IntentMsg m{};
    m.channel_id = kMotionCh;
    m.intent_id = 1;
    m.value_count = 1;
    m.value[0] = IntentValueField{1, IntentValue::ofF32(5.0f)};
    std::array<std::byte, 128> buf{};
    size_t n = encodeIntent(m, std::span<std::byte>(buf));
    writeFrame(linkA.endpointB(), FrameType::INTENT, kMotionCh, std::span<const std::byte>(buf.data(), n));
    tickAndDrain(hub, clock, linkA.endpointB());
    tickAndDrain(hub, clock, linkB.endpointB());

    size_t sessionsBefore = hub.sessionCount();

    // Owner departs rudely.
    hub.detachTransport(linkA.endpointA());
    auto replies = tickAndDrain(hub, clock, linkB.endpointB());

    // Nothing latches — no safety edge of any kind.
    CHECK(collectEvents(replies, channels::safety_events).empty());
    CHECK_FALSE(hub.stopLatched());
    CHECK_FALSE(hub.estopLatched());

    // The slot is RETAINED, marked STALE (not freed).
    CHECK(hub.sessionCount() == sessionsBefore);
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::STALE);
}

TEST_CASE("M4B-06: a hub whose catalog omits 0x000E still latches, and is simply silent") {
    Catalog32 cat;
    makeM4bCatalog(cat, /*withSafetyEvents=*/false);
    ManualClock clock;
    XorShift32 rng(4006);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, 0, {{channels::safety, 0.0f, 3}});

    hub.latchEstop(safety_causes::user, 0, 3);
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    CHECK(hub.estopLatched());  // the LATCH was always the load-bearing half
    CHECK(collectEvents(replies, channels::safety_events).empty());
}

// ---- ITEM 1 -----------------------------------------------------------------
// the ICrypto seam (RFC-028 obligation 3)

TEST_CASE("M4B-07: constantTimeEqual is correct across lengths and every differing position") {
    SoftwareCrypto c;
    std::array<std::byte, 16> a{}, b{};
    for (size_t i = 0; i < 16; ++i) { a[i] = std::byte(uint8_t(i)); b[i] = std::byte(uint8_t(i)); }
    CHECK(c.constantTimeEqual(std::span<const std::byte>(a), std::span<const std::byte>(b)));

    for (size_t i = 0; i < 16; ++i) {
        auto bb = b;
        bb[i] = std::byte(uint8_t(uint8_t(bb[i]) ^ 0x80));
        CHECK_FALSE(c.constantTimeEqual(std::span<const std::byte>(a), std::span<const std::byte>(bb)));
    }
    // A length mismatch is not a secret and short-circuits legitimately.
    CHECK_FALSE(c.constantTimeEqual(std::span<const std::byte>(a),
                                    std::span<const std::byte>(b.data(), 8)));
    // Two empty spans are equal, and that must not be a crash or a "true" that
    // accidentally authorizes an absent token — the caller's size checks are
    // what stop that, and they are tested via the hub below.
    CHECK(c.constantTimeEqual({}, {}));
}

TEST_CASE("M4B-08: the injected ICrypto is what the hub actually validates tokens with") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4008);
    M4bDelegate del;
    ScriptedCrypto crypto;
    Hub hub(cat, clock, rng, del, crypto);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    // Pair a device so there IS a stored token to compare against.
    hub.openPresenceWindow();
    connectSession(hub, clock, link.endpointB(), 7, 0, allSubs());
    writeKnock(link.endpointB(), 7);
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    auto grant = findPairGrant(replies);
    REQUIRE(grant.has_value());

    const uint32_t before = crypto.compares;
    connectSession(hub, clock, link.endpointB(), 7, 0, allSubs(), {}, "m4b-test", &grant->token);
    CHECK(crypto.compares > before);  // it went through the delegate, not std::equal
}

TEST_CASE("M4B-09: the default ICrypto reports P-256 unsupported, and that is a normal answer") {
    SoftwareCrypto c;
    std::array<std::byte, 64> sig{};
    std::array<std::byte, 33> pk{};
    CHECK(c.signP256({}, std::span<std::byte>(sig)) == 0);
    CHECK(c.publicKey(std::span<std::byte>(pk)) == 0);
    CHECK_FALSE(c.verifyP256({}, {}, std::span<const std::byte>(sig)));
}

TEST_CASE("M4B-10: a hub WITH a durable identity delivers hub_pubkey in PAIR_GRANT (the M4c seam)") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4010);
    M4bDelegate del;
    ScriptedCrypto crypto;
    crypto.p256Supported = true;
    for (size_t i = 0; i < crypto.pubkey.size(); ++i) crypto.pubkey[i] = std::byte(uint8_t(0x40 + i));
    Hub hub(cat, clock, rng, del, crypto);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    hub.openPresenceWindow();
    connectSession(hub, clock, link.endpointB(), 5, 0, allSubs());
    writeKnock(link.endpointB(), 5);
    auto grant = findPairGrant(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(grant.has_value());
    REQUIRE(grant->has_trust);
    REQUIRE(grant->trust_map.has_hub_pubkey);
    CHECK(grant->trust_map.hub_pubkey_len == 33);
    CHECK(grant->trust_map.hub_pubkey[0] == std::byte{0x40});
}

TEST_CASE("M4B-11: the ICrypto hmacSha256 delegate agrees with the library's own PIN proof") {
    SoftwareCrypto c;
    const char pin[] = "4821";
    std::array<std::byte, 8> nonce{};
    for (size_t i = 0; i < 8; ++i) nonce[i] = std::byte(uint8_t(0x30 + i));

    auto direct = pairingPinProof(std::span<const char>(pin, 4), std::span<const std::byte>(nonce));
    std::array<std::byte, 32> viaDelegate{};
    REQUIRE(c.hmacSha256(std::as_bytes(std::span<const char>(pin, 4)), std::span<const std::byte>(nonce),
                         std::span<std::byte>(viaDelegate)));
    CHECK(std::memcmp(direct.data(), viaDelegate.data(), direct.size()) == 0);
}

// ---- ITEM 2 -----------------------------------------------------------------
// knock-and-approve (RFC-027 mode (a))

TEST_CASE("M4B-12: a bare knock enters the pending list, EVENTs, and shows up in 0x000A STATE") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4012);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink admin(clock, rng), joiner(clock, rng);
    REQUIRE(hub.attachTransport(admin.endpointA()));
    REQUIRE(hub.attachTransport(joiner.endpointA()));
    REQUIRE(admin.endpointB().open());
    REQUIRE(joiner.endpointB().open());

    connectSession(hub, clock, admin.endpointB(), 1, 0xC0, allSubs());
    connectSession(hub, clock, joiner.endpointB(), 2, 0, {}, {}, "remote");

    writeKnock(joiner.endpointB(), 2);
    tickAndDrain(hub, clock, joiner.endpointB());
    auto adminReplies = tickAndDrain(hub, clock, admin.endpointB());

    CHECK(hub.pairing().pending().count() == 1);
    auto evs = collectEvents(adminReplies, channels::pairing_events);
    REQUIRE(evs.size() >= 1);
    CHECK(evs.back().event_kind == pairing_events::knocked);

    auto st = latestState(adminReplies, channels::pending_pairing);
    REQUIRE(st.has_value());
    REQUIRE(st->size() == kPendingPayloadBytes);
    CHECK(uint8_t((*st)[2]) == 1);                    // count
    CHECK(uint8_t((*st)[4]) == 2);                    // slot 0 inst_lo byte 0 == instance id byte
    CHECK(uint8_t((*st)[12]) == pairing_modes::knock_approve);  // `kind` = the mode that knocked
    // The joiner itself is answered with SILENCE — see handleKnock's comment.
}

TEST_CASE("M4B-13: a configure session approves a knock; the joiner is granted and upgraded in place") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4013);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink admin(clock, rng), joiner(clock, rng);
    REQUIRE(hub.attachTransport(admin.endpointA()));
    REQUIRE(hub.attachTransport(joiner.endpointA()));
    REQUIRE(admin.endpointB().open());
    REQUIRE(joiner.endpointB().open());

    connectSession(hub, clock, admin.endpointB(), 1, 0xC0, allSubs());
    connectSession(hub, clock, joiner.endpointB(), 2, 0, {}, {}, "remote");
    CHECK(hub.sessionBySlot(1)->role == AccessLevel::watch);

    writeKnock(joiner.endpointB(), 2);
    tickAndDrain(hub, clock, joiner.endpointB());
    tickAndDrain(hub, clock, admin.endpointB());

    writeAdmin(admin.endpointB(), 1, session_admin_ops::pair_approve, instanceOf(2), std::nullopt,
               AccessLevel::control);
    auto adminReplies = tickAndDrain(hub, clock, admin.endpointB());
    auto joinerReplies = tickAndDrain(hub, clock, joiner.endpointB());

    auto grant = findPairGrant(joinerReplies);
    REQUIRE(grant.has_value());
    CHECK(grant->roles == uint8_t(AccessLevel::control));
    CHECK(hub.sessionBySlot(1)->role == AccessLevel::control);  // upgraded in place
    CHECK(hub.pairing().pending().count() == 0);
    CHECK(hub.pairing().entryCount() == 1);

    bool sawEcho = false;
    for (const auto& r : adminReplies) {
        if (r.type == FrameType::ECHO && r.channel == channels::session_admin) sawEcho = true;
    }
    CHECK(sawEcho);

    // A reconnect with the granted token resolves to the same role.
    auto w = connectSession(hub, clock, joiner.endpointB(), 2, 0, {}, {}, "remote", &grant->token);
    CHECK(w.roles == uint8_t(AccessLevel::control));
}

TEST_CASE("M4B-14: pair_deny clears the knock, EVENTs, and mints nothing") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4014);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink admin(clock, rng), joiner(clock, rng);
    REQUIRE(hub.attachTransport(admin.endpointA()));
    REQUIRE(hub.attachTransport(joiner.endpointA()));
    REQUIRE(admin.endpointB().open());
    REQUIRE(joiner.endpointB().open());
    connectSession(hub, clock, admin.endpointB(), 1, 0xC0, allSubs());
    connectSession(hub, clock, joiner.endpointB(), 2, 0, {});

    writeKnock(joiner.endpointB(), 2);
    tickAndDrain(hub, clock, joiner.endpointB());
    tickAndDrain(hub, clock, admin.endpointB());

    writeAdmin(admin.endpointB(), 1, session_admin_ops::pair_deny, instanceOf(2));
    auto adminReplies = tickAndDrain(hub, clock, admin.endpointB());
    auto joinerReplies = tickAndDrain(hub, clock, joiner.endpointB());

    CHECK(hub.pairing().pending().count() == 0);
    CHECK(hub.pairing().entryCount() == 0);
    CHECK_FALSE(findPairGrant(joinerReplies).has_value());
    bool sawDenied = false;
    for (const auto& e : collectEvents(adminReplies, channels::pairing_events)) {
        if (e.event_kind == pairing_events::denied) sawDenied = true;
    }
    CHECK(sawDenied);
}

TEST_CASE("M4B-15: the knock list is BOUNDED — a flood costs a fixed number of slots, no more") {
    // The bound is what makes an UNAUTHENTICATED queue safe to have at all: a
    // knock costs a stranger nothing to send, so the only defense is that it
    // can never cost the hub more than pairing_pending_max slots.
    PendingPairingList<> list;
    for (uint8_t i = 0; i < uint8_t(limits::pairing_pending_max); ++i) {
        auto id = instanceOf(uint8_t(20 + i));
        CHECK(list.add(std::span<const std::byte>(id), 100u + i, 1000, pairing_modes::knock_approve) != nullptr);
    }
    CHECK(list.count() == size_t(limits::pairing_pending_max));

    auto overflow = instanceOf(99);
    CHECK(list.add(std::span<const std::byte>(overflow), 999, 1000, pairing_modes::knock_approve) == nullptr);
    CHECK(list.count() == size_t(limits::pairing_pending_max));  // nobody was evicted

    // A RE-knock from an already-pending device refreshes rather than
    // consuming a second slot — otherwise a client retrying politely would
    // exhaust the list by itself.
    auto existing = instanceOf(20);
    PendingKnock* again = list.add(std::span<const std::byte>(existing), 100, 5000, pairing_modes::knock_approve);
    REQUIRE(again != nullptr);
    CHECK(again->expiresAtMs == 5000);
    CHECK(list.count() == size_t(limits::pairing_pending_max));
}

TEST_CASE("M4B-15b: the hub saturates at the same bound, and a re-knock is not a second slot") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4015);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);

    std::vector<std::unique_ptr<InProcessLink>> links;
    for (int i = 0; i < int(kHubMaxSessions); ++i) {
        links.push_back(std::make_unique<InProcessLink>(clock, rng));
        REQUIRE(hub.attachTransport(links.back()->endpointA()));
        REQUIRE(links.back()->endpointB().open());
        connectSession(hub, clock, links.back()->endpointB(), uint8_t(10 + i), 0, {});
        writeKnock(links.back()->endpointB(), uint8_t(10 + i));
        tickAndDrain(hub, clock, links.back()->endpointB());
    }
    CHECK(hub.pairing().pending().count() == size_t(limits::pairing_pending_max));

    writeKnock(links[0]->endpointB(), 10);  // same device knocks again
    auto replies = tickAndDrain(hub, clock, links[0]->endpointB());
    CHECK(hub.pairing().pending().count() == size_t(limits::pairing_pending_max));
    CHECK(countNacks(replies, NackCode::BUSY) == 0);
}

TEST_CASE("M4B-16: an unanswered knock expires and says so") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4016);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink admin(clock, rng), joiner(clock, rng);
    REQUIRE(hub.attachTransport(admin.endpointA()));
    REQUIRE(hub.attachTransport(joiner.endpointA()));
    REQUIRE(admin.endpointB().open());
    REQUIRE(joiner.endpointB().open());
    connectSession(hub, clock, admin.endpointB(), 1, 0xC0, allSubs());
    connectSession(hub, clock, joiner.endpointB(), 2, 0, {});
    writeKnock(joiner.endpointB(), 2);
    tickAndDrain(hub, clock, joiner.endpointB());
    tickAndDrain(hub, clock, admin.endpointB());
    REQUIRE(hub.pairing().pending().count() == 1);

    // Past the window. (Sessions are reaped meanwhile; the knock's own expiry
    // is what this case is about, and it fires either way.)
    for (int i = 0; i < 130; ++i) {
        clock.advanceUs(1000000);
        hub.update(clock.nowUs());
    }
    CHECK(hub.pairing().pending().count() == 0);
    CHECK(hub.pairing().entryCount() == 0);
}

TEST_CASE("M4B-17: approval requires the CONFIGURE tier — a control session is refused") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4017);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink ctrl(clock, rng), joiner(clock, rng);
    REQUIRE(hub.attachTransport(ctrl.endpointA()));
    REQUIRE(hub.attachTransport(joiner.endpointA()));
    REQUIRE(ctrl.endpointB().open());
    REQUIRE(joiner.endpointB().open());
    connectSession(hub, clock, ctrl.endpointB(), 1, 0xC1, allSubs());  // control, not configure
    connectSession(hub, clock, joiner.endpointB(), 2, 0, {});
    writeKnock(joiner.endpointB(), 2);
    tickAndDrain(hub, clock, joiner.endpointB());

    writeAdmin(ctrl.endpointB(), 1, session_admin_ops::pair_approve, instanceOf(2), std::nullopt,
               AccessLevel::control);
    auto replies = tickAndDrain(hub, clock, ctrl.endpointB());
    CHECK(countNacks(replies, NackCode::ACCESS_DENIED) == 1);
    CHECK(hub.pairing().entryCount() == 0);
}

TEST_CASE("M4B-18: a configure session may grant configure, but not more than it holds") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4018);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink admin(clock, rng), joiner(clock, rng);
    REQUIRE(hub.attachTransport(admin.endpointA()));
    REQUIRE(hub.attachTransport(joiner.endpointA()));
    REQUIRE(admin.endpointB().open());
    REQUIRE(joiner.endpointB().open());
    connectSession(hub, clock, admin.endpointB(), 1, 0xC0, allSubs());
    connectSession(hub, clock, joiner.endpointB(), 2, 0, {});
    writeKnock(joiner.endpointB(), 2);
    tickAndDrain(hub, clock, joiner.endpointB());
    tickAndDrain(hub, clock, admin.endpointB());

    // OPERATOR RULING: up to its own tier, configure included — otherwise the
    // first administrator could never appoint a second.
    writeAdmin(admin.endpointB(), 1, session_admin_ops::pair_approve, instanceOf(2), std::nullopt,
               AccessLevel::configure);
    tickAndDrain(hub, clock, admin.endpointB());
    auto grant = findPairGrant(tickAndDrain(hub, clock, joiner.endpointB()));
    REQUIRE(grant.has_value());
    CHECK(grant->roles == uint8_t(AccessLevel::configure));
    CHECK(hub.pairing().hasConfigureToken());
}

TEST_CASE("M4B-19: a knock DIES WITH ITS SESSION — the lifecycle regression this project has paid for once") {
    // Field bug #3 was session-scoped state (source ownership) outliving its
    // session forever, invisible because deploys rebooted between runs. A
    // leaked knock is the same shape: it would hold one of only four pending
    // slots permanently and let an operator "approve" a device that left.
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4019);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink admin(clock, rng), joiner(clock, rng);
    REQUIRE(hub.attachTransport(admin.endpointA()));
    REQUIRE(hub.attachTransport(joiner.endpointA()));
    REQUIRE(admin.endpointB().open());
    REQUIRE(joiner.endpointB().open());
    connectSession(hub, clock, admin.endpointB(), 1, 0xC0, allSubs());

    // Two full knock->depart cycles BACK TO BACK with no hub restart between
    // them: the mandatory verification pattern for anything session-scoped.
    for (int round = 0; round < 2; ++round) {
        connectSession(hub, clock, joiner.endpointB(), 2, 0, {});
        writeKnock(joiner.endpointB(), 2);
        tickAndDrain(hub, clock, joiner.endpointB());
        REQUIRE(hub.pairing().pending().count() == 1);

        hub.detachTransport(joiner.endpointA());
        CHECK(hub.pairing().pending().count() == 0);

        REQUIRE(hub.attachTransport(joiner.endpointA()));
        REQUIRE(joiner.endpointB().open());
    }

    // And approving a departed knocker fails honestly instead of minting a
    // token nobody can be handed.
    writeAdmin(admin.endpointB(), 1, session_admin_ops::pair_approve, instanceOf(2), std::nullopt,
               AccessLevel::control);
    auto replies = tickAndDrain(hub, clock, admin.endpointB());
    CHECK(countNacks(replies, NackCode::INVALID_VALUE) == 1);
    CHECK(hub.pairing().entryCount() == 0);
}

TEST_CASE("M4B-20: a session may only knock for its OWN instance id") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4020);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 2, 0, {});

    writeKnock(link.endpointB(), 3);  // somebody ELSE's identity
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    CHECK(countNacks(replies, NackCode::PAIRING_DENIED) == 1);
    CHECK(hub.pairing().pending().count() == 0);
}

TEST_CASE("M4B-21: evict runs the full teardown and is reachable from the same admin surface") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4021);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink admin(clock, rng), victim(clock, rng);
    REQUIRE(hub.attachTransport(admin.endpointA()));
    REQUIRE(hub.attachTransport(victim.endpointA()));
    REQUIRE(admin.endpointB().open());
    REQUIRE(victim.endpointB().open());
    connectSession(hub, clock, admin.endpointB(), 1, 0xC0, allSubs());
    auto vw = connectSession(hub, clock, victim.endpointB(), 2, 0xC1, {});
    REQUIRE(hub.sessionCount() == 2);

    writeAdmin(admin.endpointB(), 1, session_admin_ops::evict, std::nullopt, vw.session_id);
    tickAndDrain(hub, clock, admin.endpointB());
    auto victimReplies = tickAndDrain(hub, clock, victim.endpointB());

    CHECK(hub.sessionCount() == 1);
    bool sawGoodbye = false;
    for (const auto& r : victimReplies) {
        if (r.type != FrameType::GOODBYE) continue;
        auto g = decodeGoodbye(std::span<const std::byte>(r.payload));
        if (g && g.value().code == NackCode::SESSION_EVICTED) sawGoodbye = true;
    }
    CHECK(sawGoodbye);
}

// ---- ITEM 3 -----------------------------------------------------------------
// push-to-pair (RFC-027 mode (c)) and mode advertisement

TEST_CASE("M4B-22: FACTORY-FRESH — with zero configure tokens the presence window grants configure") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4022);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 2, 0, allSubs());

    CHECK_FALSE(hub.pairing().hasConfigureToken());
    hub.openPresenceWindow();
    writeKnock(link.endpointB(), 2);
    auto replies = tickAndDrain(hub, clock, link.endpointB());

    auto grant = findPairGrant(replies);
    REQUIRE(grant.has_value());
    CHECK(grant->roles == uint8_t(AccessLevel::configure));  // possession is root
    // SINGLE-GRANT: the window is consumed by the first knock.
    CHECK_FALSE(hub.presenceWindowOpen());
}

TEST_CASE("M4B-23: once a configure token exists the presence window drops to the configured default") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4023);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink a(clock, rng), b(clock, rng);
    REQUIRE(hub.attachTransport(a.endpointA()));
    REQUIRE(hub.attachTransport(b.endpointA()));
    REQUIRE(a.endpointB().open());
    REQUIRE(b.endpointB().open());

    connectSession(hub, clock, a.endpointB(), 2, 0, {});
    hub.openPresenceWindow();
    writeKnock(a.endpointB(), 2);
    REQUIRE(findPairGrant(tickAndDrain(hub, clock, a.endpointB())).has_value());
    REQUIRE(hub.pairing().hasConfigureToken());

    connectSession(hub, clock, b.endpointB(), 3, 0, {});
    hub.openPresenceWindow();
    writeKnock(b.endpointB(), 3);
    auto grant = findPairGrant(tickAndDrain(hub, clock, b.endpointB()));
    REQUIRE(grant.has_value());
    CHECK(grant->roles == uint8_t(AccessLevel::control));
}

TEST_CASE("M4B-24: window state is observable IN BAND three ways, none of them an LED") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4024);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    // A plain WATCH session — RFC-027(c) requires it to be able to see this.
    auto w0 = connectSession(hub, clock, link.endpointB(), 2, 0, allSubs());

    // (1) WELCOME advertises only knock_approve while nothing else is offered.
    REQUIRE(w0.has_trust);
    CHECK(w0.trust_map.pairing_modes_mask == pairing_modes::knock_approve);

    // (2) the EVENT edge, on a watch-access channel.
    hub.openPresenceWindow();
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    bool sawOpen = false;
    for (const auto& e : collectEvents(replies, channels::pairing_events)) {
        if (e.event_kind == pairing_events::window_opened) sawOpen = true;
    }
    CHECK(sawOpen);

    // (3) a WELCOME issued DURING the window advertises push_to_pair.
    auto w1 = connectSession(hub, clock, link.endpointB(), 2, 0, allSubs());
    REQUIRE(w1.has_trust);
    CHECK((w1.trust_map.pairing_modes_mask & pairing_modes::push_to_pair) != 0);

    hub.closePresenceWindow();
    auto w2 = connectSession(hub, clock, link.endpointB(), 2, 0, allSubs());
    CHECK((w2.trust_map.pairing_modes_mask & pairing_modes::push_to_pair) == 0);
}

TEST_CASE("M4B-25: the presence window's own expiry produces a closing edge even if nobody knocked") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4025);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 2, 0, allSubs());

    hub.openPresenceWindow();
    tickAndDrain(hub, clock, link.endpointB());
    REQUIRE(hub.presenceWindowOpen());

    bool sawClose = false;
    std::array<std::byte, 8> pingPayload{};
    for (int i = 0; i < 130 && !sawClose; ++i) {
        // Keep the session alive across the whole window: RFC-024 idle reaping
        // fires at 3x the PING interval, long before a 120 s pairing window
        // elapses, and a torn-down session obviously observes nothing.
        std::array<std::byte, 16> pbuf{};
        size_t pn = encodePing(std::span<const std::byte>(pingPayload.data(), 4), std::span<std::byte>(pbuf));
        writeFrame(link.endpointB(), FrameType::PING, 0, std::span<const std::byte>(pbuf.data(), pn));
        clock.advanceUs(1000000);
        hub.update(clock.nowUs());
        while (auto fb = link.endpointB().read()) {
            auto h = fb->header();
            if (!h || FrameType(h->type) != FrameType::EVENT) continue;
            if (h->channel != channels::pairing_events) continue;
            auto e = decodeEvent(fb->payload());
            if (e && e.value().event_kind == pairing_events::window_closed) sawClose = true;
        }
    }
    CHECK(sawClose);
    CHECK_FALSE(hub.presenceWindowOpen());
}

TEST_CASE("M4B-26: PIN mode (b) still pairs, and is advertised only while its window is open") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4026);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    const char pin[] = "4821";
    hub.openPairingWindow(std::span<const char>(pin, 4));
    auto w = connectSession(hub, clock, link.endpointB(), 2, 0, allSubs());
    REQUIRE(w.has_trust);
    CHECK((w.trust_map.pairing_modes_mask & pairing_modes::pin_proof) != 0);

    auto proof = pairingPinProof(std::span<const char>(pin, 4), std::span<const std::byte>(w.nonce));
    PairReqMsg m{};
    m.instance_id = instanceOf(2);
    m.has_pin_proof = true;
    std::memcpy(m.pin_proof.data(), proof.data(), proof.size());
    std::array<std::byte, 64> buf{};
    size_t n = encodePairReq(m, std::span<std::byte>(buf));
    writeFrame(link.endpointB(), FrameType::PAIR_REQ, 0, std::span<const std::byte>(buf.data(), n));

    auto grant = findPairGrant(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(grant.has_value());
    CHECK(grant->roles == uint8_t(AccessLevel::control));
    REQUIRE(hub.pairing().entryCount() == 1);
    CHECK(hub.pairing().entry(0)->pairingMode == pairing_modes::pin_proof);
}

// ---- ITEM 4 -----------------------------------------------------------------
// the trust ledger as a BLOB STORE

TEST_CASE("M4B-27: a configure session enumerates the ledger over BLOB_REQ, and the item carries NO token") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4027);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink admin(clock, rng), joiner(clock, rng);
    REQUIRE(hub.attachTransport(admin.endpointA()));
    REQUIRE(hub.attachTransport(joiner.endpointA()));
    REQUIRE(admin.endpointB().open());
    REQUIRE(joiner.endpointB().open());
    connectSession(hub, clock, admin.endpointB(), 1, 0xC0, allSubs());
    connectSession(hub, clock, joiner.endpointB(), 2, 0, {}, "0.2.3", "my-remote");
    hub.openPresenceWindow();
    writeKnock(joiner.endpointB(), 2);
    REQUIRE(findPairGrant(tickAndDrain(hub, clock, joiner.endpointB())).has_value());

    BlobReqMsg req{};
    req.blob.ns = blob_ns::store;
    req.blob.store_id = 1;
    req.blob.has_store_id = true;
    req.blob.slot = 0;
    req.blob.has_slot = true;
    std::array<std::byte, 128> rbuf{};
    size_t rn = encodeBlobReq(req, std::span<std::byte>(rbuf));
    writeFrame(admin.endpointB(), FrameType::BLOB_REQ, 0, std::span<const std::byte>(rbuf.data(), rn));
    auto replies = tickAndDrain(hub, clock, admin.endpointB());

    ChunkReassembler<8> reasm;
    bool begun = false;
    for (const auto& r : replies) {
        if (r.type != FrameType::BLOB_CHUNK) continue;
        BlobChunkHeader h{};
        REQUIRE(getBlobChunkHeader(std::span<const std::byte>(r.payload), h));
        if (!begun) { reasm.begin(h, 0); begun = true; }
        CHECK(reasm.insert(std::span<const std::byte>(r.payload), 0));
    }
    REQUIRE(reasm.complete());

    CborReader cr(reasm.assembled());
    auto mh = cr.readMapHeader();
    REQUIRE(mh.isOk());
    bool sawInstance = false, sawName = false, sawVersion = false, sawRole = false;
    for (uint32_t i = 0; i < mh.value(); ++i) {
        auto k = cr.readKey();
        REQUIRE(k.isOk());
        switch (k.value()) {
            case trust_ledger::instance_id: { auto v = cr.readBstr(); REQUIRE(v.isOk());
                CHECK(v.value()[0] == std::byte{2}); sawInstance = true; break; }
            case trust_ledger::name: { auto v = cr.readTstr(); REQUIRE(v.isOk());
                CHECK(v.value() == "my-remote"); sawName = true; break; }
            case trust_ledger::version: { auto v = cr.readTstr(); REQUIRE(v.isOk());
                CHECK(v.value() == "0.2.3"); sawVersion = true; break; }
            case trust_ledger::role: { auto v = cr.readUint(); REQUIRE(v.isOk());
                CHECK(v.value() == uint64_t(AccessLevel::configure)); sawRole = true; break; }
            case trust_ledger::pairing_mode: { auto v = cr.readUint(); REQUIRE(v.isOk());
                CHECK(v.value() == pairing_modes::push_to_pair); break; }
            default: REQUIRE(cr.skipValue().isOk()); break;
        }
    }
    CHECK(sawInstance);
    CHECK(sawName);
    CHECK(sawVersion);
    CHECK(sawRole);
    // THE ONE THING THAT MUST NEVER BE THERE: a live credential. Listing paired
    // devices must not be a way to become one.
    CHECK(reasm.assembled().size() < 200);
}

TEST_CASE("M4B-28: a WATCH session is DENIED the ledger store — the gate BLOB_REQ never had") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4028);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 2, 0, allSubs());  // watch

    BlobReqMsg req{};
    req.blob.ns = blob_ns::store;
    req.blob.store_id = 1;
    req.blob.has_store_id = true;
    req.blob.slot = 0;
    req.blob.has_slot = true;
    std::array<std::byte, 128> rbuf{};
    size_t rn = encodeBlobReq(req, std::span<std::byte>(rbuf));
    writeFrame(link.endpointB(), FrameType::BLOB_REQ, 0, std::span<const std::byte>(rbuf.data(), rn));
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    CHECK(countNacks(replies, NackCode::ACCESS_DENIED) == 1);
    for (const auto& r : replies) CHECK(r.type != FrameType::BLOB_CHUNK);

    // And the catalog itself (namespace 0) is unaffected — §8.6 makes it
    // client-invariant and deliberately readable by anyone who can connect.
    BlobReqMsg catReq{};
    std::array<std::byte, 64> cbuf{};
    size_t cn = encodeBlobReq(catReq, std::span<std::byte>(cbuf));
    writeFrame(link.endpointB(), FrameType::BLOB_REQ, 0, std::span<const std::byte>(cbuf.data(), cn));
    auto catReplies = tickAndDrain(hub, clock, link.endpointB());
    int chunks = 0;
    for (const auto& r : catReplies) {
        if (r.type == FrameType::BLOB_CHUNK) ++chunks;
    }
    CHECK(chunks > 0);
}

TEST_CASE("M4B-29: the ledger round-trips through the NVS seam and fits one flash page at capacity") {
    PairingManager pm;
    pm.setWallClockSeconds(1750000000u);
    XorShift32 rng(4029);
    for (uint8_t i = 0; i < uint8_t(PairingManager::kMaxPaired); ++i) {
        std::array<std::byte, limits::instance_id_bytes> id{};
        id.fill(std::byte{i});
        auto* e = pm.grant(std::span<const std::byte>(id), AccessLevel::control,
                           pairing_modes::knock_approve, rng, {});
        REQUIRE(e != nullptr);
        e->kind.assign("mfp-plugin-xxxx");
        e->name.assign("a-very-long-name");
        e->version.assign("10.20.30-rc1+build");
    }
    REQUIRE(pm.entryCount() == PairingManager::kMaxPaired);

    std::array<std::byte, limits::trust_ledger_max_bytes> blob{};
    const size_t n = pm.encodeLedger(std::span<std::byte>(blob));
    REQUIRE(n > 0);
    // The feasibility pass's binding constraint: ONE blob, one NVS page.
    CHECK(n <= limits::trust_ledger_max_bytes);

    PairingManager restored;
    REQUIRE(restored.decodeLedger(std::span<const std::byte>(blob.data(), n)));
    REQUIRE(restored.entryCount() == pm.entryCount());
    for (size_t i = 0; i < pm.entryCount(); ++i) {
        const auto* a = pm.entry(i);
        const auto* b = restored.entry(i);
        REQUIRE(b != nullptr);
        CHECK(std::equal(a->instance_id.begin(), a->instance_id.end(), b->instance_id.begin()));
        CHECK(std::equal(a->token.begin(), a->token.end(), b->token.begin()));  // persistence form keeps it
        CHECK(a->name.view() == b->name.view());
        CHECK(a->version.view() == b->version.view());
        CHECK(a->role == b->role);
        CHECK(a->lastSeen == b->lastSeen);
    }

    // ALL-OR-NOTHING: a truncated blob leaves the ledger it already had.
    PairingManager partial;
    CHECK_FALSE(partial.decodeLedger(std::span<const std::byte>(blob.data(), n / 2)));
    CHECK(partial.entryCount() == 0);
}

TEST_CASE("M4B-30: `dirty` is the write-only-on-change half of the NVS seam") {
    PairingManager pm;
    XorShift32 rng(4030);
    CHECK_FALSE(pm.dirty());
    std::array<std::byte, limits::instance_id_bytes> id{};
    id[0] = std::byte{9};
    REQUIRE(pm.grant(std::span<const std::byte>(id), AccessLevel::control, pairing_modes::knock_approve, rng, {}));
    CHECK(pm.dirty());
    pm.clearDirty();
    CHECK_FALSE(pm.dirty());
    CHECK(pm.revoke(std::span<const std::byte>(id)));
    CHECK(pm.dirty());
    CHECK(pm.entryCount() == 0);
}

// ---- M4B-30b (M5b) ----------------------------------------------------------
// the LOAD half of the same seam, and the trap in it.
//
// decodeLedger() touch()es the ledger, because from the manager's point of view
// the entries genuinely changed. But an application restoring from NVS at boot
// has changed NOTHING that flash does not already hold — so if it does not
// clearDirty() straight after a successful load, its "write only on change"
// pump rewrites the identical bytes back on its very first tick, every boot,
// forever. That is flash wear bought with nothing, and it is invisible in
// testing because the resulting blob is byte-identical.
TEST_CASE("M4B-30b: a restored ledger reports dirty — the loader MUST clear it or it rewrites flash at boot") {
    PairingManager src;
    XorShift32 rng(40302);
    std::array<std::byte, limits::instance_id_bytes> id{};
    id[0] = std::byte{0x5A};
    REQUIRE(src.grant(std::span<const std::byte>(id), AccessLevel::control, pairing_modes::pin_proof, rng, {}));
    std::array<std::byte, limits::trust_ledger_max_bytes> blob{};
    const size_t n = src.encodeLedger(std::span<std::byte>(blob));
    REQUIRE(n > 0);

    PairingManager restored;
    CHECK_FALSE(restored.dirty());
    REQUIRE(restored.decodeLedger(std::span<const std::byte>(blob.data(), n)));
    CHECK(restored.dirty());          // the trap
    restored.clearDirty();            // what the firmware loader does
    CHECK_FALSE(restored.dirty());

    // And a real change after the load still dirties, so clearing at load time
    // costs nothing: the ledger is not "silenced", only told that the disk and
    // the memory already agree.
    std::array<std::byte, limits::instance_id_bytes> id2{};
    id2[0] = std::byte{0x5B};
    REQUIRE(restored.grant(std::span<const std::byte>(id2), AccessLevel::watch,
                           pairing_modes::knock_approve, rng, {}));
    CHECK(restored.dirty());
    CHECK(restored.entryCount() == 2);
}

TEST_CASE("M4B-31: revocation is PROTOCOL — it bumps the roster, EVENTs, and bites at the next HELLO") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4031);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink admin(clock, rng), dev(clock, rng);
    REQUIRE(hub.attachTransport(admin.endpointA()));
    REQUIRE(hub.attachTransport(dev.endpointA()));
    REQUIRE(admin.endpointB().open());
    REQUIRE(dev.endpointB().open());
    connectSession(hub, clock, admin.endpointB(), 1, 0xC0, allSubs());
    connectSession(hub, clock, dev.endpointB(), 2, 0, {});
    hub.openPresenceWindow();
    writeKnock(dev.endpointB(), 2);
    auto grant = findPairGrant(tickAndDrain(hub, clock, dev.endpointB()));
    REQUIRE(grant.has_value());
    const uint16_t genBefore = hub.pairing().generation();

    writeAdmin(admin.endpointB(), 1, session_admin_ops::revoke, instanceOf(2));
    auto replies = tickAndDrain(hub, clock, admin.endpointB());
    CHECK(hub.pairing().entryCount() == 0);
    CHECK(hub.pairing().generation() != genBefore);
    bool sawRevoked = false;
    for (const auto& e : collectEvents(replies, channels::pairing_events)) {
        if (e.event_kind == pairing_events::revoked) sawRevoked = true;
    }
    CHECK(sawRevoked);

    auto roster = latestState(replies, channels::paired_devices_roster);
    REQUIRE(roster.has_value());
    CHECK(uint8_t((*roster)[2]) == 0);                                     // count
    CHECK(uint8_t((*roster)[3]) == uint8_t(PairingManager::kMaxPaired));   // capacity

    // The revoked token no longer buys anything.
    auto w = connectSession(hub, clock, dev.endpointB(), 2, 0, {}, {}, "m4b-test", &grant->token);
    CHECK(w.roles == uint8_t(AccessLevel::watch));
}

// ---- ITEM 5 -----------------------------------------------------------------
// the client-change tripwire (RFC-029 item 2)
//
// HONESTY CLAUSE, repeated where a reader of the TESTS meets it: `client_ver`
// is self-reported. Everything below proves the tripwire fires on an HONEST
// version change. None of it proves anything about a dishonest one, and no
// test could — a malicious update reports whatever version it likes.

namespace {
// Pair a device via the presence window and hand back its token.
std::array<std::byte, limits::token_bytes> pairDevice(Hub& hub, ManualClock& clock, InProcessLink& link,
                                                      uint8_t idByte, AccessLevel role, std::string_view ver) {
    connectSession(hub, clock, link.endpointB(), idByte, 0, {}, ver);
    hub.setPresenceDefaultRole(role);
    hub.openPresenceWindow();
    writeKnock(link.endpointB(), idByte);
    auto g = findPairGrant(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(g.has_value());
    return g->token;
}
}  // namespace

TEST_CASE("M4B-32: an UNCHANGED version keeps full trust") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4032);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    // Two configure tokens would be needed to make the presence window grant
    // `control`; simplest is to let it grant configure and then check parity.
    auto token = pairDevice(hub, clock, link, 2, AccessLevel::control, "0.2.3");
    const AccessLevel granted = hub.pairing().entry(0)->role;

    auto w = connectSession(hub, clock, link.endpointB(), 2, 0, {}, "0.2.3", "m4b-test", &token);
    CHECK(w.roles == uint8_t(granted));
    CHECK(hub.pairing().entry(0)->state == trust_states::trusted);
}

TEST_CASE("M4B-33: a CHANGED version suspends a control-or-above role to watch and queues a re-approval") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4033);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink admin(clock, rng), dev(clock, rng);
    REQUIRE(hub.attachTransport(admin.endpointA()));
    REQUIRE(hub.attachTransport(dev.endpointA()));
    REQUIRE(admin.endpointB().open());
    REQUIRE(dev.endpointB().open());
    connectSession(hub, clock, admin.endpointB(), 1, 0xC0, allSubs());

    auto token = pairDevice(hub, clock, dev, 2, AccessLevel::control, "0.2.3");
    const AccessLevel granted = hub.pairing().entry(0)->role;
    REQUIRE(uint8_t(granted) > uint8_t(AccessLevel::watch));

    auto w = connectSession(hub, clock, dev.endpointB(), 2, 0, {}, "0.3.0", "m4b-test", &token);
    CHECK(w.roles == uint8_t(AccessLevel::watch));  // admitted, but suspended
    CHECK(hub.pairing().entry(0)->state == trust_states::recognized_pending);
    CHECK(hub.pairing().entry(0)->role == granted);  // SUSPENDED, not revoked

    auto adminReplies = tickAndDrain(hub, clock, admin.endpointB());
    bool sawPending = false;
    for (const auto& e : collectEvents(adminReplies, channels::pairing_events)) {
        if (e.event_kind == pairing_events::recognized_pending) sawPending = true;
    }
    CHECK(sawPending);
    // Surfaced to configure sessions as an ordinary approval, flagged as a
    // RE-approval so the prompt can say "0.2.3 -> 0.3.0, keep trusting?".
    REQUIRE(hub.pairing().pending().count() == 1);
    CHECK(hub.pairing().pending().find(std::span<const std::byte>(instanceOf(2)))->reapproval);
}

TEST_CASE("M4B-34: re-approval restores the suspended role and records the new version") {
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4034);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink admin(clock, rng), dev(clock, rng);
    REQUIRE(hub.attachTransport(admin.endpointA()));
    REQUIRE(hub.attachTransport(dev.endpointA()));
    REQUIRE(admin.endpointB().open());
    REQUIRE(dev.endpointB().open());
    connectSession(hub, clock, admin.endpointB(), 1, 0xC0, allSubs());

    auto token = pairDevice(hub, clock, dev, 2, AccessLevel::control, "0.2.3");
    const AccessLevel granted = hub.pairing().entry(0)->role;
    connectSession(hub, clock, dev.endpointB(), 2, 0, {}, "0.3.0", "m4b-test", &token);
    REQUIRE(hub.pairing().entry(0)->state == trust_states::recognized_pending);

    writeAdmin(admin.endpointB(), 1, session_admin_ops::pair_approve, instanceOf(2), std::nullopt, granted);
    tickAndDrain(hub, clock, admin.endpointB());
    tickAndDrain(hub, clock, dev.endpointB());

    CHECK(hub.pairing().entry(0)->state == trust_states::trusted);
    CHECK(hub.pairing().entry(0)->role == granted);
    CHECK(hub.pairing().entry(0)->version.view() == "0.3.0");
    CHECK(hub.pairing().pending().count() == 0);
}

TEST_CASE("M4B-34b: REVOKING a suspended device also clears its pending re-approval") {
    // FOUND BY THE LIVE PAIRING PROBE against slopsim, not by this suite: a
    // device revoked while sitting in RECOGNIZED-PENDING left its re-approval
    // in the queue, so a later `pair_approve` would have RESURRECTED it —
    // minting a fresh token with no new ceremony, from a queue entry the
    // operator had already answered by revoking. Revocation has to clear the
    // question as well as the answer.
    Catalog32 cat;
    makeM4bCatalog(cat);
    ManualClock clock;
    XorShift32 rng(4034);
    M4bDelegate del;
    Hub hub(cat, clock, rng, del);
    InProcessLink admin(clock, rng), dev(clock, rng);
    REQUIRE(hub.attachTransport(admin.endpointA()));
    REQUIRE(hub.attachTransport(dev.endpointA()));
    REQUIRE(admin.endpointB().open());
    REQUIRE(dev.endpointB().open());
    connectSession(hub, clock, admin.endpointB(), 1, 0xC0, allSubs());

    auto token = pairDevice(hub, clock, dev, 2, AccessLevel::control, "0.2.3");
    connectSession(hub, clock, dev.endpointB(), 2, 0, {}, "0.3.0", "m4b-test", &token);
    REQUIRE(hub.pairing().pending().count() == 1);
    REQUIRE(hub.pairing().entryCount() == 1);

    writeAdmin(admin.endpointB(), 1, session_admin_ops::revoke, instanceOf(2));
    tickAndDrain(hub, clock, admin.endpointB());
    CHECK(hub.pairing().entryCount() == 0);
    CHECK(hub.pairing().pending().count() == 0);

    // ...and approving it now fails, rather than quietly re-minting a token.
    writeAdmin(admin.endpointB(), 2, session_admin_ops::pair_approve, instanceOf(2), std::nullopt,
               AccessLevel::control);
    auto replies = tickAndDrain(hub, clock, admin.endpointB());
    CHECK(countNacks(replies, NackCode::INVALID_VALUE) == 1);
    CHECK(hub.pairing().entryCount() == 0);
}

TEST_CASE("M4B-35: a WATCH-tier device auto-rekeeps on a version change (the default policy)") {
    PairingManager pm;
    XorShift32 rng(4035);
    std::array<std::byte, limits::instance_id_bytes> id{};
    id[0] = std::byte{4};
    REQUIRE(pm.grant(std::span<const std::byte>(id), AccessLevel::watch, pairing_modes::knock_approve, rng, {}));
    pm.observeHello(std::span<const std::byte>(id), "sim", "viewer", "1.0.0", true, 0);
    auto obs = pm.observeHello(std::span<const std::byte>(id), "sim", "viewer", "2.0.0", true, 0);
    CHECK(obs.known);
    CHECK(obs.versionChanged);
    CHECK_FALSE(obs.suspended);  // nothing to suspend: it never had authority
    CHECK(pm.findByInstance(std::span<const std::byte>(id))->version.view() == "2.0.0");

    // The policy is hub-configurable, per RFC-029.
    pm.setTrustChangeAutoKeepMax(AccessLevel::configure);
    std::array<std::byte, limits::instance_id_bytes> id2{};
    id2[0] = std::byte{5};
    REQUIRE(pm.grant(std::span<const std::byte>(id2), AccessLevel::configure, pairing_modes::pin_proof, rng, {}));
    pm.observeHello(std::span<const std::byte>(id2), "sim", "cli", "1.0.0", true, 0);
    auto obs2 = pm.observeHello(std::span<const std::byte>(id2), "sim", "cli", "9.9.9", true, 0);
    CHECK(obs2.versionChanged);
    CHECK_FALSE(obs2.suspended);
}

TEST_CASE("M4B-36: a device that reports NO version can never trip the wire — stated, not hidden") {
    PairingManager pm;
    XorShift32 rng(4036);
    std::array<std::byte, limits::instance_id_bytes> id{};
    id[0] = std::byte{6};
    REQUIRE(pm.grant(std::span<const std::byte>(id), AccessLevel::control, pairing_modes::push_to_pair, rng, {}));

    for (int i = 0; i < 3; ++i) {
        auto obs = pm.observeHello(std::span<const std::byte>(id), "c5-remote", "potato", {}, false, 0);
        CHECK(obs.known);
        CHECK_FALSE(obs.versionChanged);
        CHECK_FALSE(obs.suspended);
        CHECK(obs.effectiveRole == AccessLevel::control);
    }
    // The FIRST version ever reported establishes a baseline; it does not fire.
    auto first = pm.observeHello(std::span<const std::byte>(id), "c5-remote", "potato", "1.0.0", true, 0);
    CHECK_FALSE(first.suspended);
    CHECK(pm.findByInstance(std::span<const std::byte>(id))->version.view() == "1.0.0");
    // And NOW it can.
    auto second = pm.observeHello(std::span<const std::byte>(id), "c5-remote", "potato", "1.0.1", true, 0);
    CHECK(second.versionChanged);
    CHECK(second.suspended);
}
