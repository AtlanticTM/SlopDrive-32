// test_slopsync_staleness — RFC-042 (session staleness:
// separate "the session ends" from "motion stops") and the RFC-045 removal of
// the source-loss forced-stop latch, at the session-lifecycle level.
//
// RFC-042/RFC-045 changed the HUB-side behavior of the two silence regimes
// (§6.6) and of an out-of-band transport loss:
//   * silence (deadman for a source-owning session, idle reaping otherwise)
//     now marks the session STALE instead of tearing it down — the slot,
//     session_id, and every grant are RETAINED, and NOTHING latches (RFC-045:
//     source loss is liveness bookkeeping, never a safety event);
//   * a STALE session's instance_id reattaches on a fresh HELLO (any
//     transport) — same session_id, same grants, role re-derived from the
//     presented token;
//   * under slot pressure (a HELLO that would otherwise NACK BUSY), the
//     lowest-tier, longest-stale session yields its slot (GOODBYE
//     SLOT_RECLAIMED), never a LIVE one.
//
// Deadman/source-ownership coverage for the "nothing latches" half already
// lives in test_slopsync_safety (S-05/S-06) and test_slopsync_streamingress
// (SI-08/SI-15) — this suite covers the SESSION-LIFECYCLE half: staleness
// itself, reattach, and slot-pressure eviction, none of which existed before
// RFC-042.
//
// STALE-05..07 (added with the attachTransport() slot-selection fix): the
// live-bug-reported clobber — attachTransport() picking a STALE slot by
// `transport == nullptr` ALONE, before handleHello()'s identity-matched
// reattach path ever runs — is triggered via detachTransport() (the
// out-of-band transport-loss path, the actual live repro), not idle/deadman
// silence (STALE-01..04's trigger, which never nulls the transport and so
// never exercised attachTransport()'s buggy loop at all).
//
// Native (host-side, hardware-free): InProcessLink + ManualClock + XorShift32,
// doctest's bundled main(), same harness shape as the other M4/M5 suites.
// Suite ids: STALE-xx.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/core/clock.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/hub/hub.hpp"
#include "slopsync/transport/inprocess_binding.hpp"
#include "slopsync/wire/frame_header.hpp"
#include "slopsync/wire/messages/echo.hpp"
#include "slopsync/wire/messages/event.hpp"
#include "slopsync/wire/messages/goodbye.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/intent.hpp"
#include "slopsync/wire/messages/nack.hpp"
#include "slopsync/wire/messages/welcome.hpp"
#include "slopsync/wire/raw/catalog_ready.hpp"
#include "slopsync/wire/raw/ping_pong.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

using namespace slopsync;

namespace {

// ---- Test catalog -----------------------------------------------------------
// 0x0003 safety, 0x0007 session-events (RFC-042 observability),
// 0x0080 "motion" (STREAM c2h controller, mapped to source 0 for the
// deadman/reattach-under-ownership cases), 0x0082 "telemetry" (STATE h2c
// viewer, the grant this suite proves survives a reattach unrenegotiated).
constexpr uint16_t kMotionCh = 0x0080;
constexpr uint16_t kTelemetryCh = 0x0082;

void makeStaleCatalog(Catalog32& c) {
    c.clear();
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

    c.addEntry({.id = channels::session_events, .name = "session-events",
                .cls = ChannelClass::EVENT, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::background});

    c.addEntry({.id = kMotionCh, .name = "motion",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 50.0f,
                .defaultPriority = Priority::normal});
    c.addSchemaField({.key = 1, .name = "target", .type = CborFieldType::f32_t, .unit = "mm"});

    c.addEntry({.id = kTelemetryCh, .name = "telemetry",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 10.0f,
                .defaultPriority = Priority::normal});
    c.addLayoutField({.name = "pos", .type = PackedFieldType::u16, .unit = "mm", .scale = 1.0f});
    REQUIRE(c.ok());
}

class StaleDelegate final : public HubDelegate {
public:
    std::vector<std::pair<uint8_t, uint32_t>> ownership;  // {source_id, owner_session (0=released)}
    std::vector<uint8_t> deadmanStopped;

    AccessLevel validateToken(std::span<const std::byte>, std::span<const std::byte> token,
                              bool hasToken) override {
        if (!hasToken || token.empty()) return AccessLevel::watch;
        return token[0] == std::byte{0xC1} ? AccessLevel::control : AccessLevel::watch;
    }
    Result<IntentValueMap, NackCode> applyIntent(uint16_t channel_id, const IntentValueMap& requested,
                                                 AccessLevel, bool&) override {
        if (channel_id != kMotionCh) return Result<IntentValueMap, NackCode>::err(NackCode::UNKNOWN_CHANNEL);
        IntentValueMap applied{};
        applied.count = requested.count;
        applied.fields = requested.fields;
        return Result<IntentValueMap, NackCode>::ok(applied);
    }
    void onEstop(uint8_t, uint8_t) override {}
    std::optional<uint8_t> sourceForChannel(uint16_t channel_id) override {
        if (channel_id == kMotionCh) return uint8_t(0);
        return std::nullopt;
    }
    void onSourceOwnership(uint8_t source_id, uint32_t owner_session, uint8_t) override {
        ownership.emplace_back(source_id, owner_session);
    }
    void onDeadmanStop(uint8_t source_id) override { deadmanStopped.push_back(source_id); }
};

// ---- raw frame helpers, same shape as the other M4/M5 suites ----------------
void writeFrame(ITransport& ep, FrameType type, uint16_t channel, std::span<const std::byte> payload) {
    std::array<std::byte, 300> buf{};
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

// `cachedEtag`, when given, rides HELLO's `catalog_etag` (8) — exactly what a
// real reconnecting/reattaching client presents from its own cache, and the
// thing that keeps `ready` retained across a reattach (§8.4/RFC-015: an
// etag-less HELLO honestly gates readiness shut, same as any first contact).
void writeHello(ITransport& ep, uint8_t idByte, bool control, std::vector<SubWish> subs,
                std::optional<std::array<std::byte, limits::etag_bytes>> cachedEtag = std::nullopt) {
    HelloMsg m{};
    m.proto_ver = kProtocolVersion;
    m.client_kind = "sim";
    m.client_name = "stale-test";
    m.instance_id = instanceOf(idByte);
    if (control) {
        m.has_token = true;
        m.token.fill(std::byte{0xC1});
    }
    if (cachedEtag) {
        m.has_catalog_etag = true;
        m.catalog_etag = *cachedEtag;
    }
    m.subscriptions_count = uint32_t(subs.size());
    for (size_t i = 0; i < subs.size(); ++i) {
        m.subscriptions[i].channel_id = subs[i].channel_id;
        m.subscriptions[i].rate_hz = subs[i].rate;
        m.subscriptions[i].priority = subs[i].prio;
    }
    std::array<std::byte, 300> buf{};
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

std::optional<GoodbyeMsg> findGoodbye(const std::vector<DecodedReply>& replies) {
    for (const auto& r : replies) {
        if (r.type != FrameType::GOODBYE) continue;
        auto g = decodeGoodbye(std::span<const std::byte>(r.payload));
        if (g) return g.value();
    }
    return std::nullopt;
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

int countNacks(const std::vector<DecodedReply>& replies, NackCode code) {
    int n = 0;
    for (const auto& r : replies) {
        if (r.type != FrameType::NACK) continue;
        auto nm = decodeNack(std::span<const std::byte>(r.payload));
        if (nm && nm.value().code == code) ++n;
    }
    return n;
}

void writeCatalogReady(ITransport& ep, std::span<const std::byte> etag) {
    std::array<std::byte, kCatalogReadyBytes> buf{};
    size_t n = encodeCatalogReady(etag, std::span<std::byte>(buf));
    REQUIRE(n == kCatalogReadyBytes);
    writeFrame(ep, FrameType::CATALOG_READY, 0, std::span<const std::byte>(buf.data(), n));
}

WelcomeMsg connectSession(Hub& hub, ManualClock& clock, ITransport& ep, uint8_t idByte, bool control,
                          std::vector<SubWish> subs) {
    writeHello(ep, idByte, control, std::move(subs));
    auto replies = tickAndDrain(hub, clock, ep);
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());
    writeCatalogReady(ep, std::span<const std::byte>(w->catalog_etag));
    tickAndDrain(hub, clock, ep);
    return w.value();
}

}  // namespace

// ---- STALE-01 ---------------------------------------------------------------
// silence marks a NON-OWNING session STALE, never a teardown.
TEST_CASE("STALE-01: silence past the idle window marks a non-owning session STALE, not torn down") {
    Catalog32 cat;
    makeStaleCatalog(cat);
    ManualClock clock;
    XorShift32 rng(9001);
    StaleDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    WelcomeMsg w = connectSession(hub, clock, link.endpointB(), 1, /*control=*/false, {{kTelemetryCh, 5.0f, 1}});
    REQUIRE(hub.sessionCount() == 1);
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::LIVE);
    CHECK(hub.sessionBySlot(0)->session_id == w.session_id);

    constexpr uint32_t kIdleMs = limits::idle_reap_multiplier * limits::ping_interval_idle_ms;

    // Just short of the threshold: still LIVE.
    for (uint32_t t = 0; t < kIdleMs - 500; t += 250) tickAndDrain(hub, clock, link.endpointB(), 250000);
    CHECK(hub.sessionCount() == 1);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::LIVE);

    // Past it: STALE, slot retained, same session_id, NO GOODBYE (staleness is
    // not an ending — the client may never even notice).
    std::vector<DecodedReply> allReplies;
    for (int i = 0; i < 8; ++i) {
        auto r = tickAndDrain(hub, clock, link.endpointB(), 250000);
        allReplies.insert(allReplies.end(), r.begin(), r.end());
    }
    CHECK(hub.sessionCount() == 1);
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::STALE);
    CHECK(hub.sessionBySlot(0)->session_id == w.session_id);
    CHECK_FALSE(findGoodbye(allReplies).has_value());
}

// ---- STALE-02 ---------------------------------------------------------------
// a fresh HELLO naming a STALE session's instance_id reattaches:
// same session_id, same grant (retained, NOT renegotiated from this HELLO's
// — empty — wish list), role re-derived, and a session_resumed EVENT fires.
TEST_CASE("STALE-02: a fresh HELLO on a new transport reattaches a STALE session's identity and grants") {
    Catalog32 cat;
    makeStaleCatalog(cat);
    ManualClock clock;
    XorShift32 rng(9002);
    StaleDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink linkA(clock, rng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));
    REQUIRE(linkA.endpointB().open());

    WelcomeMsg w1 = connectSession(hub, clock, linkA.endpointB(), 7, /*control=*/true, {{kTelemetryCh, 5.0f, 1}});
    REQUIRE(w1.grants_count == 1);
    CHECK(w1.grants[0].channel_id == kTelemetryCh);
    CHECK(w1.roles == uint8_t(AccessLevel::control));

    constexpr uint32_t kIdleMs = limits::idle_reap_multiplier * limits::ping_interval_idle_ms;
    for (uint32_t t = 0; t <= kIdleMs + 500; t += 250) tickAndDrain(hub, clock, linkA.endpointB(), 250000);
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    REQUIRE(hub.sessionBySlot(0)->state == HubSessionState::STALE);
    REQUIRE(hub.sessionCount() == 1);

    // A NEW physical transport, SAME instance_id (7), NO subscription wishes
    // at all — proving the grant comes back from what was RETAINED, not from
    // anything this HELLO asked for.
    InProcessLink linkB(clock, rng);
    REQUIRE(hub.attachTransport(linkB.endpointA()));
    REQUIRE(linkB.endpointB().open());
    writeHello(linkB.endpointB(), 7, /*control=*/true, {}, w1.catalog_etag);
    auto replies = tickAndDrain(hub, clock, linkB.endpointB());
    auto w2 = findWelcome(replies);
    REQUIRE(w2.has_value());

    // Same identity: same session_id, no BUSY spent, role re-derived to the
    // SAME value (the token is unchanged).
    CHECK(w2->session_id == w1.session_id);
    CHECK(w2->roles == uint8_t(AccessLevel::control));
    // Same grant, retained verbatim — this HELLO declared NO wishes.
    REQUIRE(w2->grants_count == 1);
    CHECK(w2->grants[0].channel_id == kTelemetryCh);
    CHECK(w2->grants[0].granted_rate_hz == doctest::Approx(5.0f));

    // Exactly one occupied session total: the old slot was vacated, the new
    // one holds the (same) identity — never both, never neither.
    CHECK(hub.sessionCount() == 1);
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::FREE);
    REQUIRE(hub.sessionBySlot(1) != nullptr);
    CHECK(hub.sessionBySlot(1)->state == HubSessionState::LIVE);
    CHECK(hub.sessionBySlot(1)->session_id == w1.session_id);

    // The observability edge: session_resumed (kind 5) fired, naming the
    // (unchanged) session_id — but only reachable to a subscriber, and this
    // session's OWN grant for 0x0007 never existed, so assert on delivery
    // rather than requiring it: no subscriber, no frame, by §9.4 design.
    CHECK(collectEvents(replies, channels::session_events).empty());
}

// ---- STALE-02b --------------------------------------------------------------
// the SAME reattach, but the resuming session actually watches
// session-events, so the session_resumed edge is directly observable.
TEST_CASE("STALE-02b: reattach emits a session_resumed EVENT (kind 5) to a subscriber") {
    Catalog32 cat;
    makeStaleCatalog(cat);
    ManualClock clock;
    XorShift32 rng(9003);
    StaleDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink linkA(clock, rng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));
    REQUIRE(linkA.endpointB().open());
    WelcomeMsg w1 =
        connectSession(hub, clock, linkA.endpointB(), 8, /*control=*/false, {{channels::session_events, 0.0f, 0}});

    constexpr uint32_t kIdleMs = limits::idle_reap_multiplier * limits::ping_interval_idle_ms;
    for (uint32_t t = 0; t <= kIdleMs + 500; t += 250) tickAndDrain(hub, clock, linkA.endpointB(), 250000);
    REQUIRE(hub.sessionBySlot(0)->state == HubSessionState::STALE);

    InProcessLink linkB(clock, rng);
    REQUIRE(hub.attachTransport(linkB.endpointA()));
    REQUIRE(linkB.endpointB().open());
    writeHello(linkB.endpointB(), 8, /*control=*/false, {}, w1.catalog_etag);
    auto replies = tickAndDrain(hub, clock, linkB.endpointB());
    REQUIRE(findWelcome(replies).has_value());
    CHECK(findWelcome(replies)->session_id == w1.session_id);

    auto evs = collectEvents(replies, channels::session_events);
    REQUIRE(evs.size() == 1);
    CHECK(evs[0].event_kind == session_events::session_resumed);
    REQUIRE(evs[0].has_body);
    bool foundId = false;
    for (uint32_t i = 0; i < evs[0].body_count; ++i) {
        if (evs[0].body[i].key == 1) {
            foundId = true;
            CHECK(evs[0].body[i].value.u64_val == w1.session_id);
        }
    }
    CHECK(foundId);
}

// ---- STALE-03 ---------------------------------------------------------------
// slot pressure: a HELLO that would otherwise NACK BUSY instead
// reclaims the lowest-tier, longest-stale eligible session (GOODBYE
// SLOT_RECLAIMED). A LIVE session is NEVER evicted for pressure.
TEST_CASE("STALE-03: slot pressure reclaims a STALE session (lowest tier first), never a LIVE one") {
    Catalog32 cat;
    makeStaleCatalog(cat);
    ManualClock clock;
    XorShift32 rng(9004);
    StaleDelegate del;
    Hub hub(cat, clock, rng, del);

    std::vector<std::unique_ptr<InProcessLink>> links;
    for (int i = 0; i < int(kHubMaxSessions); ++i) {
        links.push_back(std::make_unique<InProcessLink>(clock, rng));
        REQUIRE(hub.attachTransport(links.back()->endpointA()));
        REQUIRE(links.back()->endpointB().open());
    }
    // Slot 0: a WATCH session that will go stale. Slots 1..N-1: CONTROL
    // sessions that stay LIVE throughout (pinging).
    connectSession(hub, clock, links[0]->endpointB(), 1, /*control=*/false, {});
    for (int i = 1; i < int(kHubMaxSessions); ++i) {
        connectSession(hub, clock, links[size_t(i)]->endpointB(), uint8_t(10 + i), /*control=*/true, {});
    }
    REQUIRE(hub.sessionCount() == kHubMaxSessions);

    constexpr uint32_t kIdleMs = limits::idle_reap_multiplier * limits::ping_interval_idle_ms;
    for (uint32_t t = 0; t <= kIdleMs + 500; t += 250) {
        // Everyone but slot 0 keeps pinging so only slot 0 goes idle.
        for (int i = 1; i < int(kHubMaxSessions); ++i) {
            writeFrame(links[size_t(i)]->endpointB(), FrameType::PING, 0, std::span<const std::byte>{});
        }
        tickAndDrain(hub, clock, links[0]->endpointB(), 250000);
        for (int i = 1; i < int(kHubMaxSessions); ++i) tickAndDrain(hub, clock, links[size_t(i)]->endpointB(), 0);
    }
    REQUIRE(hub.sessionBySlot(0)->state == HubSessionState::STALE);
    for (int i = 1; i < int(kHubMaxSessions); ++i) {
        REQUIRE(hub.sessionBySlot(size_t(i))->state == HubSessionState::LIVE);
    }
    REQUIRE(hub.sessionCount() == kHubMaxSessions);  // the hub is FULL — every slot still occupied

    // One more distinct identity arrives -> would BUSY, but slot 0 is
    // reclaimable and yields instead.
    InProcessLink newcomer(clock, rng);
    REQUIRE(hub.attachTransport(newcomer.endpointA()));
    REQUIRE(newcomer.endpointB().open());
    writeHello(newcomer.endpointB(), 99, /*control=*/false, {});
    auto replies = tickAndDrain(hub, clock, newcomer.endpointB());
    REQUIRE(findWelcome(replies).has_value());  // admitted, not BUSY
    CHECK(countNacks(replies, NackCode::BUSY) == 0);

    // The reclaimed slot got a best-effort GOODBYE naming SLOT_RECLAIMED —
    // drain slot 0's OWN transport for it.
    auto fromSlot0 = tickAndDrain(hub, clock, links[0]->endpointB(), 0);
    auto gb = findGoodbye(fromSlot0);
    REQUIRE(gb.has_value());
    CHECK(gb->code == NackCode::SLOT_RECLAIMED);

    // The other, LIVE sessions are completely undisturbed.
    for (int i = 1; i < int(kHubMaxSessions); ++i) {
        REQUIRE(hub.sessionBySlot(size_t(i)) != nullptr);
        CHECK(hub.sessionBySlot(size_t(i))->state == HubSessionState::LIVE);
    }
    CHECK(hub.sessionCount() == kHubMaxSessions);  // still full: one left, one joined
}

// ---- STALE-04 (TRAPS T3, applied to the new model) --------------------------
// TWO full stale/reattach
// cycles on the SAME identity, back to back, with no hub restart between
// them. Session-lifecycle field bugs have historically only shown up on the
// SECOND cycle (a leaked pending-knock, a stale ownership entry, a
// reset-that-wasn't) — this is the mandatory regression shape for any change
// in this area.
TEST_CASE("STALE-04: two full stale-then-reattach cycles on one identity, back to back, no reboot") {
    Catalog32 cat;
    makeStaleCatalog(cat);
    ManualClock clock;
    XorShift32 rng(9005);
    StaleDelegate del;
    Hub hub(cat, clock, rng, del);

    constexpr uint32_t kIdleMs = limits::idle_reap_multiplier * limits::ping_interval_idle_ms;
    uint32_t sessionId = 0;

    for (int cycle = 0; cycle < 2; ++cycle) {
        CAPTURE(cycle);
        InProcessLink link(clock, rng);
        REQUIRE(hub.attachTransport(link.endpointA()));
        REQUIRE(link.endpointB().open());

        // Own the source via an INTENT (proves ownership plumbing survives
        // repeated cycles, not just plain subscriptions).
        WelcomeMsg w = connectSession(hub, clock, link.endpointB(), 42, /*control=*/true, {});
        if (cycle == 0) {
            sessionId = w.session_id;
        } else {
            CHECK(w.session_id == sessionId);  // same identity, same session_id, every cycle
        }

        IntentMsg m{};
        m.channel_id = kMotionCh;
        m.intent_id = uint16_t(cycle + 1);
        m.value_count = 1;
        m.value[0] = IntentValueField{1, IntentValue::ofF32(1.0f)};
        std::array<std::byte, 128> buf{};
        size_t n = encodeIntent(m, std::span<std::byte>(buf));
        writeFrame(link.endpointB(), FrameType::INTENT, kMotionCh, std::span<const std::byte>(buf.data(), n));
        tickAndDrain(hub, clock, link.endpointB());
        REQUIRE_FALSE(del.ownership.empty());
        CHECK(del.ownership.back().first == 0);
        CHECK(del.ownership.back().second == w.session_id);  // acquired

        // Go stale (deadman: this session owns the source).
        for (uint32_t t = 0; t <= kIdleMs + 1000; t += 250) tickAndDrain(hub, clock, link.endpointB(), 250000);
        REQUIRE(del.ownership.back().first == 0);
        CHECK(del.ownership.back().second == 0);  // released
        CHECK_FALSE(hub.stopLatched());            // RFC-045: never latches

        InProcessLink link2(clock, rng);
        REQUIRE(hub.attachTransport(link2.endpointA()));
        REQUIRE(link2.endpointB().open());
        writeHello(link2.endpointB(), 42, /*control=*/true, {}, w.catalog_etag);
        auto replies = tickAndDrain(hub, clock, link2.endpointB());
        auto w2 = findWelcome(replies);
        REQUIRE(w2.has_value());
        CHECK(w2->session_id == sessionId);

        // Reacquire cleanly through the resumed identity — the released
        // source from THIS cycle must not still be conflict-dropping.
        IntentMsg m2{};
        m2.channel_id = kMotionCh;
        m2.intent_id = uint16_t(100 + cycle);
        m2.value_count = 1;
        m2.value[0] = IntentValueField{1, IntentValue::ofF32(2.0f)};
        std::array<std::byte, 128> buf2{};
        size_t n2 = encodeIntent(m2, std::span<std::byte>(buf2));
        writeFrame(link2.endpointB(), FrameType::INTENT, kMotionCh, std::span<const std::byte>(buf2.data(), n2));
        auto replies2 = tickAndDrain(hub, clock, link2.endpointB());
        CHECK(countNacks(replies2, NackCode::SOURCE_CONFLICT) == 0);
        REQUIRE_FALSE(del.ownership.empty());
        CHECK(del.ownership.back().first == 0);
        CHECK(del.ownership.back().second == sessionId);

        // Let THIS cycle's session go idle-stale too before starting the next
        // cycle with a brand-new transport (mirrors a real "close the tab").
        for (uint32_t t = 0; t <= kIdleMs + 1000; t += 250) tickAndDrain(hub, clock, link2.endpointB(), 250000);
    }

    // Exactly one occupied slot at the end — no leaked slots across cycles.
    CHECK(hub.sessionCount() == 1);
}

// ---- STALE-05 ---------------------------------------------------------------
// the live-bug clobber, reproduced directly: a session parked
// STALE by an OUT-OF-BAND transport loss (detachTransport() — the real
// trigger behind the field bug's SO_LINGER-RST/no-GOODBYE repro, not idle
// silence) must NOT be handed to an unrelated new connection's
// attachTransport() before identity is ever checked. The parked session
// survives the interloper untouched and its OWN identity can still reattach
// afterward — the exact "interleaved" scenario from the live verification.
TEST_CASE("STALE-05: attachTransport() never hands a STALE slot to an unrelated new identity") {
    Catalog32 cat;
    makeStaleCatalog(cat);
    ManualClock clock;
    XorShift32 rng(9006);
    StaleDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link1(clock, rng);
    REQUIRE(hub.attachTransport(link1.endpointA()));
    REQUIRE(link1.endpointB().open());
    WelcomeMsg w1 = connectSession(hub, clock, link1.endpointB(), 1, /*control=*/true, {});
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    REQUIRE(hub.sessionBySlot(0)->state == HubSessionState::LIVE);

    // Out-of-band transport loss: the slot goes STALE with its transport
    // pointer nulled — the exact shape attachTransport()'s old
    // `transport == nullptr`-only check could no longer tell apart from a
    // genuinely free slot.
    hub.detachTransport(link1.endpointA());
    REQUIRE(hub.sessionBySlot(0)->state == HubSessionState::STALE);
    const uint32_t staleSessionId = hub.sessionBySlot(0)->session_id;

    // A brand-new connection, a totally DIFFERENT identity, arrives next.
    InProcessLink link2(clock, rng);
    REQUIRE(hub.attachTransport(link2.endpointA()));
    REQUIRE(link2.endpointB().open());
    WelcomeMsg w2 = connectSession(hub, clock, link2.endpointB(), 2, /*control=*/false, {});
    CHECK(w2.session_id != staleSessionId);

    // The parked session at slot 0 is COMPLETELY UNTOUCHED by the newcomer.
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::STALE);
    CHECK(hub.sessionBySlot(0)->session_id == staleSessionId);

    // And it is STILL reattachable by its own identity afterward — RFC-042's
    // whole point, park-don't-kill.
    InProcessLink link3(clock, rng);
    REQUIRE(hub.attachTransport(link3.endpointA()));
    REQUIRE(link3.endpointB().open());
    writeHello(link3.endpointB(), 1, /*control=*/true, {}, w1.catalog_etag);
    auto replies = tickAndDrain(hub, clock, link3.endpointB());
    auto w3 = findWelcome(replies);
    REQUIRE(w3.has_value());
    CHECK(w3->session_id == staleSessionId);
}

// ---- STALE-06 ---------------------------------------------------------------
// same identity reattaches after a detachTransport()-triggered
// staleness (as opposed to STALE-02's idle-silence trigger, which never nulls
// the transport): session revives with session_id, role, and grants intact,
// no renegotiation from this HELLO's — empty — wish list.
TEST_CASE("STALE-06: same identity reattaches after a detachTransport()-triggered staleness, state intact") {
    Catalog32 cat;
    makeStaleCatalog(cat);
    ManualClock clock;
    XorShift32 rng(9007);
    StaleDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link1(clock, rng);
    REQUIRE(hub.attachTransport(link1.endpointA()));
    REQUIRE(link1.endpointB().open());
    WelcomeMsg w1 = connectSession(hub, clock, link1.endpointB(), 3, /*control=*/true, {{kTelemetryCh, 5.0f, 1}});
    REQUIRE(w1.grants_count == 1);

    hub.detachTransport(link1.endpointA());
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    REQUIRE(hub.sessionBySlot(0)->state == HubSessionState::STALE);

    InProcessLink link2(clock, rng);
    REQUIRE(hub.attachTransport(link2.endpointA()));
    REQUIRE(link2.endpointB().open());
    writeHello(link2.endpointB(), 3, /*control=*/true, {}, w1.catalog_etag);
    auto replies = tickAndDrain(hub, clock, link2.endpointB());
    auto w2 = findWelcome(replies);
    REQUIRE(w2.has_value());

    CHECK(w2->session_id == w1.session_id);
    CHECK(w2->roles == uint8_t(AccessLevel::control));
    REQUIRE(w2->grants_count == 1);
    CHECK(w2->grants[0].channel_id == kTelemetryCh);
    CHECK(w2->grants[0].granted_rate_hz == doctest::Approx(5.0f));
    CHECK(hub.sessionCount() == 1);
}

// ---- STALE-07 ---------------------------------------------------------------
// attachTransport()'s eviction fallback: when every PHYSICAL slot
// (kHubMaxSessions live + the one spare kSlotCapacity adds, §6.3/§17.1) is
// accounted for and none is genuinely free, a STALE occupant yields via the
// SAME findEvictableStale() policy HELLO's own slot-pressure eviction already
// uses (RFC-042 item 5) — no second reclaim rule invented for this fix.
TEST_CASE("STALE-07: attachTransport() evicts a STALE slot via findEvictableStale() when physically full") {
    Catalog32 cat;
    makeStaleCatalog(cat);
    ManualClock clock;
    XorShift32 rng(9008);
    StaleDelegate del;
    Hub hub(cat, clock, rng, del);

    // Fill kHubMaxSessions physical slots with bare attached transports (no
    // HELLO at all) — occupies each slot's transport pointer without
    // occupying a logical session, which is all attachTransport()'s
    // genuinely-free-slot check looks at.
    std::vector<std::unique_ptr<InProcessLink>> fillers;
    for (int i = 0; i < int(kHubMaxSessions); ++i) {
        fillers.push_back(std::make_unique<InProcessLink>(clock, rng));
        REQUIRE(hub.attachTransport(fillers.back()->endpointA()));
        REQUIRE(fillers.back()->endpointB().open());
    }

    // The one spare physical slot hosts a REAL session that then goes STALE
    // via the out-of-band transport-loss trigger — now EVERY physical slot is
    // accounted for: kHubMaxSessions bare-attached + one
    // STALE-with-null-transport.
    InProcessLink staleLink(clock, rng);
    REQUIRE(hub.attachTransport(staleLink.endpointA()));
    REQUIRE(staleLink.endpointB().open());
    WelcomeMsg w = connectSession(hub, clock, staleLink.endpointB(), 42, /*control=*/true, {});
    hub.detachTransport(staleLink.endpointA());

    int staleSlot = -1;
    for (size_t i = 0; i <= kHubMaxSessions; ++i) {
        const HubSession* s = hub.sessionBySlot(i);
        REQUIRE(s != nullptr);
        if (s->state == HubSessionState::STALE) staleSlot = int(i);
    }
    REQUIRE(staleSlot >= 0);
    CHECK(hub.sessionBySlot(size_t(staleSlot))->session_id == w.session_id);

    // A brand-new, unrelated identity now arrives: no physically free slot
    // exists, so attachTransport() must fall back to evicting the STALE one —
    // exactly like a HELLO that hit slot pressure would (STALE-03), just one
    // layer earlier.
    InProcessLink newcomer(clock, rng);
    REQUIRE(hub.attachTransport(newcomer.endpointA()));
    REQUIRE(newcomer.endpointB().open());
    writeHello(newcomer.endpointB(), 99, /*control=*/false, {});
    auto replies = tickAndDrain(hub, clock, newcomer.endpointB());
    REQUIRE(findWelcome(replies).has_value());
    CHECK(countNacks(replies, NackCode::BUSY) == 0);

    // The reclaimed slot's stale session is genuinely gone (evicted, not
    // merely relocated) — its old identity can no longer reattach.
    bool staleIdentityStillPresent = false;
    for (size_t i = 0; i <= kHubMaxSessions; ++i) {
        const HubSession* s = hub.sessionBySlot(i);
        if (s != nullptr && s->session_id == w.session_id) staleIdentityStillPresent = true;
    }
    CHECK_FALSE(staleIdentityStillPresent);

    // The four filler slots (bare-attached, never a session) are untouched —
    // the STALE one was the only eligible eviction target.
    for (int i = 0; i < int(kHubMaxSessions); ++i) {
        REQUIRE(hub.sessionBySlot(size_t(i)) != nullptr);
        CHECK(hub.sessionBySlot(size_t(i))->state == HubSessionState::FREE);
    }
}

// ---- STALE-08 ---------------------------------------------------------------
// THE PARKED-SLOT SAFETY BROADCAST (live panic, fw 2.1.81).
// broadcastSafetyNow() fans out to EVERY subscribed slot, not just the one
// being pumped, and gated only on occupied()/ready/subscription — none of
// which a detachTransport() clears. A session parked by RFC-042 therefore
// presented a null transport to a reference parameter, which is a
// LoadProhibited panic on the device (TRAPS T13), reached from any safety-word
// or safety-mode change while some other client sat parked.
//
// The two triggers below are the two the bench actually hit: latchEstop()
// (§11.2) and setSafetyModes() (RFC-025c override/bypass — the probe's own
// Step 5.9). Both must reach the LIVE subscriber and leave the parked one
// intact and reattachable.
namespace {

// The retained safety snapshot as it lands on the wire: 9 packed bytes, byte 0
// the safety word, byte 8 the RFC-025c modes.
std::optional<std::vector<std::byte>> findSafetyState(const std::vector<DecodedReply>& replies) {
    for (const auto& r : replies) {
        if (r.type == FrameType::STATE && r.channel == channels::safety) return r.payload;
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("STALE-08: a safety broadcast skips a PARKED slot instead of dereferencing its null transport") {
    Catalog32 cat;
    makeStaleCatalog(cat);
    ManualClock clock;
    XorShift32 rng(9009);
    StaleDelegate del;
    Hub hub(cat, clock, rng, del);

    // Two ready sessions, both subscribed to the critical safety channel.
    InProcessLink parkedLink(clock, rng);
    REQUIRE(hub.attachTransport(parkedLink.endpointA()));
    REQUIRE(parkedLink.endpointB().open());
    WelcomeMsg parked = connectSession(hub, clock, parkedLink.endpointB(), 11, /*control=*/true,
                                       {{channels::safety, 0.0f, uint8_t(Priority::critical)}});

    InProcessLink liveLink(clock, rng);
    REQUIRE(hub.attachTransport(liveLink.endpointA()));
    REQUIRE(liveLink.endpointB().open());
    connectSession(hub, clock, liveLink.endpointB(), 22, /*control=*/true,
                   {{channels::safety, 0.0f, uint8_t(Priority::critical)}});
    REQUIRE(hub.sessionCount() == 2);

    // Park the first one the way a dropped socket does: out-of-band detach.
    // Its session, grants and safety subscription all survive — that is
    // RFC-042 working, and precisely what makes the slot dangerous.
    hub.detachTransport(parkedLink.endpointA());
    const HubSession* parkedSession = hub.sessionBySlot(0);
    REQUIRE(parkedSession != nullptr);
    REQUIRE(parkedSession->state == HubSessionState::STALE);
    REQUIRE(parkedSession->ready);
    REQUIRE(parkedSession->subs.find(channels::safety) != nullptr);

    // Trigger 1 — §11.2 e-stop latch. Pre-fix this panicked on the parked slot
    // before the live subscriber was ever reached.
    hub.latchEstop(safety_causes::user, 0, 5);
    auto replies = tickAndDrain(hub, clock, liveLink.endpointB());
    auto snap = findSafetyState(replies);
    REQUIRE(snap.has_value());
    REQUIRE(snap->size() == 9);
    CHECK((uint8_t((*snap)[0]) & safety_bits::ESTOP) != 0);

    // Trigger 2 — RFC-025c modes, the probe's Step 5.9 override_on.
    hub.setSafetyModes(/*manualOverride=*/true, /*bypassLimits=*/false);
    replies = tickAndDrain(hub, clock, liveLink.endpointB());
    snap = findSafetyState(replies);
    REQUIRE(snap.has_value());
    REQUIRE(snap->size() == 9);
    CHECK((uint8_t((*snap)[8]) & safety_mode_bits::OVERRIDE) != 0);

    // The parked session was SKIPPED, not punished: still occupied, still
    // STALE, never evicted by a critical-stall timer it could not have served.
    parkedSession = hub.sessionBySlot(0);
    REQUIRE(parkedSession != nullptr);
    CHECK(parkedSession->state == HubSessionState::STALE);
    CHECK(parkedSession->session_id == parked.session_id);
    CHECK(hub.sessionCount() == 2);

    // And skipping cost it nothing: on reattach §9.1 hands it the CURRENT
    // retained snapshot, carrying both edges it slept through.
    InProcessLink rejoin(clock, rng);
    REQUIRE(hub.attachTransport(rejoin.endpointA()));
    REQUIRE(rejoin.endpointB().open());
    writeHello(rejoin.endpointB(), 11, /*control=*/true, {}, parked.catalog_etag);
    replies = tickAndDrain(hub, clock, rejoin.endpointB());
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());
    CHECK(w->session_id == parked.session_id);

    snap = findSafetyState(replies);
    for (int i = 0; i < 5 && !snap.has_value(); ++i) {
        snap = findSafetyState(tickAndDrain(hub, clock, rejoin.endpointB()));
    }
    REQUIRE(snap.has_value());
    CHECK((uint8_t((*snap)[0]) & safety_bits::ESTOP) != 0);
    CHECK((uint8_t((*snap)[8]) & safety_mode_bits::OVERRIDE) != 0);
}
