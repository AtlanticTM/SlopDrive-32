// test_slopsync_m4c — MILESTONE 4c: SlopSync's
// CRYPTOGRAPHIC PROOF layer (RFC-029 items 1 and 6).
//
//   M4C-01..04  the signature MATERIAL and its codecs (AUTH / HUB_SIG / the
//               GRANT `roles` key) — the wire, before any behavior.
//   M4C-05..12  ITEM 1, HUB SIDE: on-request signing, the DEFERRED sign queue
//               (the answer to "an ECDSA sign would stall the hub's tick"),
//               the inline alternative, and session-scoped cleanup.
//   M4C-13..16  ITEM 1, THE REPLAY FIX. The load-bearing tests: a signature
//               captured from one session must not verify for another. This
//               was a feasibility-pass BLOCKER and these are its receipts.
//   M4C-17..24  ITEM 1, CLIENT SIDE: pinning at PAIR_GRANT, verify, the
//               evil-twin mismatch, silence-as-answer, and the fact that
//               intents are WITHHELD by the library and not by hope.
//   M4C-25..33  ITEMS 2+3: token presentation modes and the AUTH frame —
//               proof round trip, wrong proof, three strikes, cross-session
//               proof replay, roster posture, and the tripwire on the AUTH path.
//   M4C-34..36  THE POTATO FLOOR. Zero-crypto clients must be untouched by all
//               of the above. If these fail, the weight covenant is broken and
//               the milestone is wrong regardless of what else passes.
//
// Native (host-side, hardware-free): InProcessLink + ManualClock + XorShift32
// + ScriptedCrypto, doctest's bundled main(), same harness shape as
// test_slopsync_m4b.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/channel/trust_channels.hpp"
#include "slopsync/client/client.hpp"
#include "slopsync/conformance/mini_catalog.hpp"
#include "slopsync/core/clock.hpp"
#include "slopsync/core/crypto.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/hub/hub.hpp"
#include "slopsync/transport/inprocess_binding.hpp"
#include "slopsync/wire/frame_header.hpp"
#include "slopsync/wire/messages/auth.hpp"
#include "slopsync/wire/messages/goodbye.hpp"
#include "slopsync/wire/messages/grant.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/intent.hpp"
#include "slopsync/wire/messages/nack.hpp"
#include "slopsync/wire/messages/pair.hpp"
#include "slopsync/wire/messages/welcome.hpp"
#include "slopsync/wire/raw/catalog_ready.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace slopsync;

namespace {

constexpr uint16_t kMotionCh = 0x0101;  // an ordinary control-access INTENT channel

// ---------------------------------------------------------------------------
// Catalog: safety (so the hub has a retained snapshot to seed), the trust
// channels (so the roster/pending state has somewhere to publish) and one
// control-access motion INTENT — enough for a role upgrade to be OBSERVABLE
// rather than merely reported.
// ---------------------------------------------------------------------------
void makeM4cCatalog(Catalog32& c) {
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

    REQUIRE(addTrustChannels(c));

    c.addEntry({.id = kMotionCh, .name = "motion",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 50.0f,
                .defaultPriority = Priority::normal});
    c.addSchemaField({.key = 1, .name = "target", .type = CborFieldType::f32_t, .unit = "mm"});

    // A control-access STATE channel, so "did the role upgrade actually buy
    // this session anything?" is answerable by looking at a GRANT rather than
    // by trusting the roles field.
    c.addEntry({.id = 0x0102, .name = "motion-status",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::control, .maxRateHz = 20.0f,
                .defaultPriority = Priority::normal});
    c.addLayoutField({.name = "flags", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});
}

class M4cDelegate final : public HubDelegate {
public:
    Result<IntentValueMap, NackCode> applyIntent(uint16_t channel_id, const IntentValueMap& requested,
                                                 AccessLevel, bool&) override {
        IntentValueMap applied{};
        applied.count = requested.count;
        applied.fields = requested.fields;
        (void)channel_id;
        return Result<IntentValueMap, NackCode>::ok(applied);
    }
    void onEstop(uint8_t, uint8_t) override {}
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

std::optional<WelcomeMsg> findWelcome(const std::vector<DecodedReply>& r) {
    for (const auto& x : r) {
        if (x.type != FrameType::WELCOME) continue;
        auto w = decodeWelcome(std::span<const std::byte>(x.payload));
        if (w) return w.value();
    }
    return std::nullopt;
}

std::optional<std::vector<std::byte>> findHubSig(const std::vector<DecodedReply>& r) {
    for (const auto& x : r) {
        if (x.type != FrameType::HUB_SIG) continue;
        auto m = decodeHubSig(std::span<const std::byte>(x.payload));
        if (!m || !m.value().trust_map.has_welcome_sig) continue;
        const TrustMap& t = m.value().trust_map;
        return std::vector<std::byte>(t.welcome_sig.begin(), t.welcome_sig.begin() + t.welcome_sig_len);
    }
    return std::nullopt;
}

std::optional<GrantMsg> findGrant(const std::vector<DecodedReply>& r) {
    for (const auto& x : r) {
        if (x.type != FrameType::GRANT) continue;
        auto g = decodeGrant(std::span<const std::byte>(x.payload));
        if (g) return g.value();
    }
    return std::nullopt;
}

int countNacks(const std::vector<DecodedReply>& r, NackCode code) {
    int n = 0;
    for (const auto& x : r) {
        if (x.type != FrameType::NACK) continue;
        auto nm = decodeNack(std::span<const std::byte>(x.payload));
        if (nm && nm.value().code == code) ++n;
    }
    return n;
}

bool hasGoodbye(const std::vector<DecodedReply>& r, NackCode code) {
    for (const auto& x : r) {
        if (x.type != FrameType::GOODBYE) continue;
        auto g = decodeGoodbye(std::span<const std::byte>(x.payload));
        if (g && g.value().code == code) return true;
    }
    return false;
}

std::array<std::byte, limits::instance_id_bytes> instanceOf(uint8_t idByte) {
    std::array<std::byte, limits::instance_id_bytes> id{};
    id[0] = std::byte{idByte};
    return id;
}

std::array<std::byte, kTrustClientNonceBytes> nonceOf(uint8_t tag) {
    std::array<std::byte, kTrustClientNonceBytes> n{};
    for (size_t i = 0; i < n.size(); ++i) n[i] = std::byte(uint8_t(tag + i));
    return n;
}

struct HelloOpts {
    uint8_t idByte = 1;
    bool hasToken = false;
    std::array<std::byte, limits::token_bytes> token{};
    bool sigRequest = false;
    bool hasClientNonce = false;
    std::array<std::byte, kTrustClientNonceBytes> clientNonce{};
    std::string clientVer{};
    bool hasPresentationMode = false;
    uint8_t presentationMode = 0;
    bool wantMotionStatus = false;
};

void writeHello(ITransport& ep, const HelloOpts& o) {
    HelloMsg m{};
    m.proto_ver = kProtocolVersion;
    m.client_kind = "sim";
    m.client_name = "m4c-test";
    m.instance_id = instanceOf(o.idByte);
    m.has_token = o.hasToken;
    m.token = o.token;
    if (o.sigRequest || o.hasClientNonce || !o.clientVer.empty() || o.hasPresentationMode) {
        m.has_trust = true;
        if (!o.clientVer.empty()) {
            m.trust_map.has_client_ver = true;
            m.trust_map.client_ver = o.clientVer;
        }
        if (o.hasClientNonce) {
            m.trust_map.has_client_nonce = true;
            m.trust_map.client_nonce = o.clientNonce;
        }
        if (o.sigRequest) {
            m.trust_map.has_sig_request = true;
            m.trust_map.sig_request = true;
        }
        if (o.hasPresentationMode) {
            m.trust_map.has_presentation_mode = true;
            m.trust_map.presentation_mode = o.presentationMode;
        }
    }
    m.subscriptions_count = 1;
    m.subscriptions[0].channel_id = channels::safety;
    m.subscriptions[0].rate_hz = 0.0f;
    m.subscriptions[0].priority = 3;
    if (o.wantMotionStatus) {
        m.subscriptions[1].channel_id = 0x0102;
        m.subscriptions[1].rate_hz = 5.0f;
        m.subscriptions[1].priority = 1;
        m.subscriptions_count = 2;
    }
    std::array<std::byte, 400> buf{};
    size_t n = encodeHello(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(ep, FrameType::HELLO, 0, std::span<const std::byte>(buf.data(), n));
}

// HMAC-SHA256(token, nonce) truncated 16 — the RFC-029 item 6 proof, computed
// the way an external client would.
std::array<std::byte, kTrustTokenProofBytes> tokenProof(std::span<const std::byte> token,
                                                        std::span<const std::byte> nonce) {
    auto mac = hmacSha256(token, nonce);
    std::array<std::byte, kTrustTokenProofBytes> out{};
    for (size_t i = 0; i < out.size(); ++i) out[i] = mac[i];
    return out;
}

void writeAuth(ITransport& ep, std::span<const std::byte> proof, bool declareMode = true) {
    AuthMsg m{};
    m.trust_map.has_token_proof = true;
    for (size_t i = 0; i < kTrustTokenProofBytes && i < proof.size(); ++i) m.trust_map.token_proof[i] = proof[i];
    if (declareMode) {
        m.trust_map.has_presentation_mode = true;
        m.trust_map.presentation_mode = uint8_t(presentation_modes::proof);
    }
    std::array<std::byte, 64> buf{};
    size_t n = encodeAuth(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(ep, FrameType::AUTH, 0, std::span<const std::byte>(buf.data(), n));
}

// A ScriptedCrypto with a distinct keypair identity. `keyTag` differing is what
// makes one instance an EVIL TWIN of another: same protocol, different key.
ScriptedCrypto makeCrypto(uint8_t keyTag) {
    ScriptedCrypto c;
    c.p256Supported = true;
    for (size_t i = 0; i < c.pubkey.size(); ++i) c.pubkey[i] = std::byte(uint8_t(keyTag + i));
    return c;
}

// ---- client-side harness ----------------------------------------------------

class M4cClientDelegate final : public ClientDelegate {
public:
    std::vector<HubAuthState> authHistory;
    std::vector<AccessLevel> roleHistory;
    std::vector<std::byte> pinnedKey;
    std::vector<std::byte> grantedToken;
    int pairGrants = 0;

    void onStateChange(ClientSessionState) override {}
    void onState(uint16_t, uint16_t, std::span<const std::byte>) override {}
    void onEcho(uint16_t, const IntentValueMap&, uint16_t) override {}
    void onNack(const NackMsg&) override {}
    void onPendingDropped(uint16_t) override {}
    void onHubAuth(HubAuthState s) override { authHistory.push_back(s); }
    void onRolesChanged(AccessLevel r) override { roleHistory.push_back(r); }
    void onHubPublicKey(std::span<const std::byte> k) override { pinnedKey.assign(k.begin(), k.end()); }
    void onPairGrant(std::span<const std::byte> t, AccessLevel) override {
        ++pairGrants;
        grantedToken.assign(t.begin(), t.end());
    }
};

void pump(Hub& hub, ManualClock& clock, Client& c, int rounds = 6, uint32_t stepUs = 1000) {
    for (int i = 0; i < rounds; ++i) {
        clock.advanceUs(stepUs);
        hub.update(clock.nowUs());
        hub.signPendingNow();  // stands in for the application's low-priority signer
        c.update(clock.nowUs());
    }
}

void pumpNoSigner(Hub& hub, ManualClock& clock, Client& c, int rounds, uint32_t stepUs = 1000) {
    for (int i = 0; i < rounds; ++i) {
        clock.advanceUs(stepUs);
        hub.update(clock.nowUs());
        c.update(clock.nowUs());
    }
}

ClientIdentity makeIdentity(uint8_t idByte, bool withToken, uint8_t tokenTag = 0x55) {
    ClientIdentity id{};
    id.instance_id = instanceOf(idByte);
    id.client_kind = "sim";
    id.client_name = "m4c-client";
    if (withToken) {
        id.hasToken = true;
        id.token.fill(std::byte{tokenTag});
    }
    return id;
}

}  // namespace

// ---- M4C-01..04 -------------------------------------------------------------
// the material and the codecs

TEST_CASE("M4C-01: signature material is client_nonce || session_id(LE) || boot_id(LE)") {
    auto n = nonceOf(0xA0);
    auto m = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(n), 0x11223344u, 0xAABBCCDDu);
    REQUIRE(m.size() == 16);
    for (size_t i = 0; i < 8; ++i) CHECK(m[i] == n[i]);
    CHECK(m[8] == std::byte{0x44});
    CHECK(m[9] == std::byte{0x33});
    CHECK(m[10] == std::byte{0x22});
    CHECK(m[11] == std::byte{0x11});
    CHECK(m[12] == std::byte{0xDD});
    CHECK(m[13] == std::byte{0xCC});
    CHECK(m[14] == std::byte{0xBB});
    CHECK(m[15] == std::byte{0xAA});

    // The whole anti-replay claim in one assertion: change ONLY the client's
    // entropy and the signed bytes change.
    auto n2 = nonceOf(0xB0);
    auto m2 = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(n2), 0x11223344u, 0xAABBCCDDu);
    CHECK(m != m2);
    // ...and change only the session id, likewise.
    auto m3 = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(n), 0x11223345u, 0xAABBCCDDu);
    CHECK(m != m3);
}

TEST_CASE("M4C-02: AUTH round-trips its token proof and rejects an empty envelope") {
    AuthMsg a{};
    a.trust_map.has_token_proof = true;
    for (size_t i = 0; i < kTrustTokenProofBytes; ++i) a.trust_map.token_proof[i] = std::byte(uint8_t(0x40 + i));
    a.trust_map.has_presentation_mode = true;
    a.trust_map.presentation_mode = uint8_t(presentation_modes::proof);

    std::array<std::byte, 64> buf{};
    size_t n = encodeAuth(a, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    auto back = decodeAuth(std::span<const std::byte>(buf.data(), n));
    REQUIRE(back);
    CHECK(back.value().trust_map.has_token_proof);
    CHECK(back.value().trust_map.token_proof == a.trust_map.token_proof);
    CHECK(back.value().trust_map.presentation_mode == uint8_t(presentation_modes::proof));

    // An envelope with nothing in it is never emitted...
    AuthMsg empty{};
    CHECK(encodeAuth(empty, std::span<std::byte>(buf)) == 0);
    // ...and a map that carries no `trust` key at all is malformed, not "an
    // AUTH that means nothing" — tolerating it would make an empty frame a free
    // way to burn a hub's attempt counter.
    CborWriter w{std::span<std::byte>(buf)};
    w.mapHeader(1);
    w.key(uint64_t(CborKey::roles)).uintVal(2);
    CHECK(!decodeAuth(std::span<const std::byte>(buf.data(), w.size())));
}

TEST_CASE("M4C-03: HUB_SIG round-trips a signature of any legal length") {
    for (uint8_t len : {uint8_t(64), uint8_t(70), uint8_t(72)}) {
        HubSigMsg m{};
        m.trust_map.has_welcome_sig = true;
        m.trust_map.welcome_sig_len = len;
        for (uint8_t i = 0; i < len; ++i) m.trust_map.welcome_sig[i] = std::byte(uint8_t(i ^ 0x5A));
        std::array<std::byte, 128> buf{};
        size_t n = encodeHubSig(m, std::span<std::byte>(buf));
        REQUIRE(n > 0);
        auto back = decodeHubSig(std::span<const std::byte>(buf.data(), n));
        REQUIRE(back);
        REQUIRE(back.value().trust_map.welcome_sig_len == len);
        for (uint8_t i = 0; i < len; ++i) CHECK(back.value().trust_map.welcome_sig[i] == m.trust_map.welcome_sig[i]);
    }
}

TEST_CASE("M4C-04: GRANT carries `roles` additively — absent by default, wire-identical to pre-M4c") {
    GrantMsg g{};
    g.grants_count = 1;
    g.grants[0] = Grant{.channel_id = 0x0003, .granted_rate_hz = 2.0f, .priority = 3};
    std::array<std::byte, 128> plain{};
    size_t nPlain = encodeGrant(g, std::span<std::byte>(plain));
    REQUIRE(nPlain > 0);
    auto backPlain = decodeGrant(std::span<const std::byte>(plain.data(), nPlain));
    REQUIRE(backPlain);
    CHECK_FALSE(backPlain.value().has_roles);

    g.has_roles = true;
    g.roles = uint8_t(AccessLevel::control);
    std::array<std::byte, 128> withRoles{};
    size_t nRoles = encodeGrant(g, std::span<std::byte>(withRoles));
    REQUIRE(nRoles > nPlain);  // strictly additive: the old bytes are still in there
    auto backRoles = decodeGrant(std::span<const std::byte>(withRoles.data(), nRoles));
    REQUIRE(backRoles);
    CHECK(backRoles.value().has_roles);
    CHECK(backRoles.value().roles == uint8_t(AccessLevel::control));
    CHECK(backRoles.value().grants_count == 1);

    // A `roles` value outside the access_levels enum is malformed — an
    // authorization field is structural, and RFC-028.2 says structural payloads
    // are rejected, never coerced.
    CborWriter w{std::span<std::byte>(withRoles)};
    w.mapHeader(2);
    w.key(CborKey::roles).uintVal(99);
    w.key(CborKey::grants).arrayHeader(0);
    CHECK(!decodeGrant(std::span<const std::byte>(withRoles.data(), w.size())));
}

// ---- M4C-05..12 -------------------------------------------------------------
// ITEM 1, hub side: on-request signing and the deferred queue

TEST_CASE("M4C-05: no sig_request means no signature, no job, and no extra WELCOME bytes") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(4001);
    M4cDelegate del;
    auto crypto = makeCrypto(0x10);
    Hub hub(catalog, clock, rng, del, crypto);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    writeHello(link.endpointB(), HelloOpts{.idByte = 1});
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());
    // The map is present only because knock-and-approve is always on offer
    // (RFC-027.3's per-session mode advert); the SIGNATURE is what must be
    // absent, and nothing was queued to produce one later either.
    CHECK_FALSE(w->trust_map.has_welcome_sig);
    CHECK(w->trust_map.has_pairing_modes);
    CHECK(hub.pendingSignJobs() == 0);
    CHECK(hub.signPendingNow() == 0);
    CHECK_FALSE(findHubSig(tickAndDrain(hub, clock, link.endpointB())).has_value());
}

TEST_CASE("M4C-06: sig_request defers — WELCOME carries no signature, HUB_SIG delivers it later") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(4002);
    M4cDelegate del;
    auto crypto = makeCrypto(0x20);
    Hub hub(catalog, clock, rng, del, crypto);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    auto nonce = nonceOf(0x30);
    writeHello(link.endpointB(),
               HelloOpts{.idByte = 2, .sigRequest = true, .hasClientNonce = true, .clientNonce = nonce});
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());
    // THE POINT OF THE WHOLE DESIGN: WELCOME went out at handshake speed. No
    // ECDSA ran on the tick that produced it.
    CHECK_FALSE(w->trust_map.has_welcome_sig);
    CHECK(hub.pendingSignJobs() == 1);

    Hub::SignJob job;
    REQUIRE(hub.takePendingSignJob(job));
    CHECK(job.session_id == w->session_id);
    auto expected = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(nonce), w->session_id,
                                   w->boot_id);
    CHECK(job.message == expected);
    CHECK(hub.pendingSignJobs() == 0);  // at-most-once: taken means taken

    std::array<std::byte, kTrustSigMaxBytes> sig{};
    size_t sigLen = crypto.signP256(std::span<const std::byte>(job.message), std::span<std::byte>(sig));
    REQUIRE(sigLen == 64);
    REQUIRE(hub.submitSignature(job.session_id, std::span<const std::byte>(sig.data(), sigLen)));

    auto after = tickAndDrain(hub, clock, link.endpointB());
    auto delivered = findHubSig(after);
    REQUIRE(delivered.has_value());
    REQUIRE(delivered->size() == sigLen);
    for (size_t i = 0; i < sigLen; ++i) CHECK((*delivered)[i] == sig[i]);

    // One signature per session; a second submit is refused rather than
    // allowed to spam a client with contradictory answers.
    CHECK_FALSE(hub.submitSignature(job.session_id, std::span<const std::byte>(sig.data(), sigLen)));
}

TEST_CASE("M4C-07: inline signing is the wire-alternative — same material, delivered in WELCOME") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(4003);
    M4cDelegate del;
    auto crypto = makeCrypto(0x21);
    Hub hub(catalog, clock, rng, del, crypto);
    hub.setInlineSigning(true);
    CHECK(hub.inlineSigning());
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    auto nonce = nonceOf(0x40);
    writeHello(link.endpointB(),
               HelloOpts{.idByte = 3, .sigRequest = true, .hasClientNonce = true, .clientNonce = nonce});
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());
    REQUIRE(w->has_trust);
    REQUIRE(w->trust_map.has_welcome_sig);
    CHECK(hub.pendingSignJobs() == 0);  // nothing deferred: it is already delivered

    auto material = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(nonce), w->session_id,
                                   w->boot_id);
    std::array<std::byte, kTrustSigMaxBytes> expect{};
    size_t n = crypto.signP256(std::span<const std::byte>(material), std::span<std::byte>(expect));
    REQUIRE(n == w->trust_map.welcome_sig_len);
    for (size_t i = 0; i < n; ++i) CHECK(w->trust_map.welcome_sig[i] == expect[i]);
}

TEST_CASE("M4C-08: a hub never signs a nonce the client did not send") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(4004);
    M4cDelegate del;
    auto crypto = makeCrypto(0x22);
    Hub hub(catalog, clock, rng, del, crypto);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    // sig_request WITHOUT client_nonce. Signing hub-chosen material is exactly
    // the replayable design the feasibility pass killed, so the hub declines.
    writeHello(link.endpointB(), HelloOpts{.idByte = 4, .sigRequest = true});
    auto w = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w.has_value());
    CHECK_FALSE(w->trust_map.has_welcome_sig);
    CHECK(hub.pendingSignJobs() == 0);
}

TEST_CASE("M4C-09: a hub with no keypair signs nothing and stays conformant") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(4005);
    M4cDelegate del;
    SoftwareCrypto potato;  // real HMAC + constant-time compare; NO P-256
    Hub hub(catalog, clock, rng, del, potato);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    writeHello(link.endpointB(), HelloOpts{.idByte = 5, .sigRequest = true, .hasClientNonce = true,
                                           .clientNonce = nonceOf(0x50)});
    auto w = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w.has_value());
    CHECK(w->roles == uint8_t(AccessLevel::watch));  // the session is perfectly normal
    CHECK(hub.pendingSignJobs() == 1);               // it armed...
    CHECK(hub.signPendingNow() == 0);                // ...and produced nothing, which is a legal answer
    CHECK_FALSE(findHubSig(tickAndDrain(hub, clock, link.endpointB())).has_value());
}

TEST_CASE("M4C-10: a signature for a dead session is dropped, never delivered into its successor") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(4006);
    M4cDelegate del;
    auto crypto = makeCrypto(0x23);
    Hub hub(catalog, clock, rng, del, crypto);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    writeHello(link.endpointB(), HelloOpts{.idByte = 6, .sigRequest = true, .hasClientNonce = true,
                                           .clientNonce = nonceOf(0x60)});
    auto w1 = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w1.has_value());
    Hub::SignJob job;
    REQUIRE(hub.takePendingSignJob(job));

    // The slow signer is still grinding when the client reconnects. A NEW
    // session now owns the slot.
    writeHello(link.endpointB(), HelloOpts{.idByte = 6, .sigRequest = true, .hasClientNonce = true,
                                           .clientNonce = nonceOf(0x61)});
    auto w2 = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w2.has_value());
    CHECK(w2->session_id != w1->session_id);

    std::array<std::byte, kTrustSigMaxBytes> sig{};
    size_t n = crypto.signP256(std::span<const std::byte>(job.message), std::span<std::byte>(sig));
    REQUIRE(n == 64);
    // The stale answer names a session that no longer exists. Delivering it
    // would make an innocent client fail a verification it could never explain.
    CHECK_FALSE(hub.submitSignature(job.session_id, std::span<const std::byte>(sig.data(), n)));
    CHECK_FALSE(findHubSig(tickAndDrain(hub, clock, link.endpointB())).has_value());
}

TEST_CASE("M4C-11 (RFC-042): a pending sign job dies on rude detach even though the session goes STALE, not gone") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(4007);
    M4cDelegate del;
    auto crypto = makeCrypto(0x24);
    Hub hub(catalog, clock, rng, del, crypto);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    writeHello(link.endpointB(), HelloOpts{.idByte = 7, .sigRequest = true, .hasClientNonce = true,
                                           .clientNonce = nonceOf(0x70)});
    REQUIRE(findWelcome(tickAndDrain(hub, clock, link.endpointB())).has_value());
    CHECK(hub.pendingSignJobs() == 1);

    // Rude detach: RFC-042 reclassifies "transport reports closed out of
    // band" as a STALE transition, not a teardown — the session's SLOT is
    // RETAINED (sessionCount unchanged). The pending sign job is per-slot
    // handshake state scoped to "if the transport itself is still attached"
    // (it plainly is not, here), so it is reset exactly like a true reattach's
    // path-B reset would — it was mid-flight against a socket that no longer
    // exists.
    size_t sessionsBefore = hub.sessionCount();
    hub.detachTransport(link.endpointA());
    CHECK(hub.pendingSignJobs() == 0);
    CHECK(hub.sessionCount() == sessionsBefore);
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::STALE);
}

TEST_CASE("M4C-12: submitSignature refuses garbage sizes and unknown sessions") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(4008);
    M4cDelegate del;
    auto crypto = makeCrypto(0x25);
    Hub hub(catalog, clock, rng, del, crypto);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    writeHello(link.endpointB(), HelloOpts{.idByte = 8, .sigRequest = true, .hasClientNonce = true,
                                           .clientNonce = nonceOf(0x80)});
    auto w = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w.has_value());

    std::array<std::byte, kTrustSigMaxBytes + 8> tooBig{};
    CHECK_FALSE(hub.submitSignature(w->session_id, std::span<const std::byte>(tooBig)));
    CHECK_FALSE(hub.submitSignature(w->session_id, std::span<const std::byte>()));
    std::array<std::byte, 64> ok{};
    CHECK_FALSE(hub.submitSignature(0, std::span<const std::byte>(ok)));
    CHECK_FALSE(hub.submitSignature(w->session_id ^ 0xFFFFu, std::span<const std::byte>(ok)));
}

// ---- M4C-13..16 -------------------------------------------------------------
// THE REPLAY FIX. These are the milestone's load-bearing tests.
//
// The design this replaces had the hub sign its own WELCOME nonce, which
// contains ZERO client entropy: an evil twin that captured one handshake could
// replay {nonce, signature} verbatim at every future victim and pass. The fix
// is that the client contributes the entropy. What follows is the receipt.

TEST_CASE("M4C-13: a signature captured from one session does NOT verify for a second") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(4009);
    M4cDelegate del;
    auto crypto = makeCrypto(0x31);
    Hub hub(catalog, clock, rng, del, crypto);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    // ---- session 1: the attacker records everything on the cleartext wire ---
    auto n1 = nonceOf(0x90);
    writeHello(link.endpointB(),
               HelloOpts{.idByte = 9, .sigRequest = true, .hasClientNonce = true, .clientNonce = n1});
    auto w1 = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w1.has_value());
    REQUIRE(hub.signPendingNow() == 1);
    auto capturedSig = findHubSig(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(capturedSig.has_value());

    // ---- session 2: an honest client with FRESH entropy ---------------------
    auto n2 = nonceOf(0xA5);
    writeHello(link.endpointB(),
               HelloOpts{.idByte = 9, .sigRequest = true, .hasClientNonce = true, .clientNonce = n2});
    auto w2 = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w2.has_value());
    REQUIRE(hub.signPendingNow() == 1);
    auto freshSig = findHubSig(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(freshSig.has_value());

    CHECK(*capturedSig != *freshSig);

    // The verification an evil twin would have to pass: session 2's material.
    auto material2 = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(n2), w2->session_id,
                                    w2->boot_id);
    auto verifier = makeCrypto(0x31);  // same key: this is the REAL machine's key
    CHECK(verifier.verifyP256(std::span<const std::byte>(verifier.pubkey),
                              std::span<const std::byte>(material2),
                              std::span<const std::byte>(*freshSig)));
    // ...and the replay fails. THIS IS THE BLOCKER, DEAD.
    CHECK_FALSE(verifier.verifyP256(std::span<const std::byte>(verifier.pubkey),
                                    std::span<const std::byte>(material2),
                                    std::span<const std::byte>(*capturedSig)));
}

TEST_CASE("M4C-14: replay still fails when the CLIENT reuses its nonce (session_id saves it)") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(4010);
    M4cDelegate del;
    auto crypto = makeCrypto(0x32);
    Hub hub(catalog, clock, rng, del, crypto);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    auto n = nonceOf(0xC0);  // the SAME nonce both times — a broken client RNG
    writeHello(link.endpointB(),
               HelloOpts{.idByte = 10, .sigRequest = true, .hasClientNonce = true, .clientNonce = n});
    auto w1 = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w1.has_value());
    REQUIRE(hub.signPendingNow() == 1);
    auto sig1 = findHubSig(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(sig1.has_value());

    writeHello(link.endpointB(),
               HelloOpts{.idByte = 10, .sigRequest = true, .hasClientNonce = true, .clientNonce = n});
    auto w2 = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w2.has_value());
    REQUIRE(w2->session_id != w1->session_id);  // §6.1: random non-zero PER SESSION
    REQUIRE(hub.signPendingNow() == 1);
    auto sig2 = findHubSig(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(sig2.has_value());

    CHECK(*sig1 != *sig2);
    auto material2 = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(n), w2->session_id,
                                    w2->boot_id);
    auto verifier = makeCrypto(0x32);
    CHECK_FALSE(verifier.verifyP256(std::span<const std::byte>(verifier.pubkey),
                                    std::span<const std::byte>(material2),
                                    std::span<const std::byte>(*sig1)));
}

TEST_CASE("M4C-15: an evil twin's own signature fails against the pinned key") {
    auto real = makeCrypto(0x40);
    auto twin = makeCrypto(0x41);  // every string copied; ONE thing it cannot copy
    auto n = nonceOf(0xD0);
    auto material = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(n), 7777u, 8888u);

    std::array<std::byte, kTrustSigMaxBytes> twinSig{};
    REQUIRE(twin.signP256(std::span<const std::byte>(material), std::span<std::byte>(twinSig)) == 64);

    // The client pinned the REAL machine's key at the pairing ceremony.
    CHECK_FALSE(real.verifyP256(std::span<const std::byte>(real.pubkey), std::span<const std::byte>(material),
                                std::span<const std::byte>(twinSig.data(), 64)));
    std::array<std::byte, kTrustSigMaxBytes> realSig{};
    REQUIRE(real.signP256(std::span<const std::byte>(material), std::span<std::byte>(realSig)) == 64);
    CHECK(real.verifyP256(std::span<const std::byte>(real.pubkey), std::span<const std::byte>(material),
                          std::span<const std::byte>(realSig.data(), 64)));
}

TEST_CASE("M4C-16: boot_id is in the material, so a rebooted machine cannot be impersonated by its own past") {
    auto n = nonceOf(0xE0);
    auto a = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(n), 1234u, 1u);
    auto b = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(n), 1234u, 2u);
    CHECK(a != b);
    auto crypto = makeCrypto(0x42);
    std::array<std::byte, kTrustSigMaxBytes> sigA{};
    REQUIRE(crypto.signP256(std::span<const std::byte>(a), std::span<std::byte>(sigA)) == 64);
    CHECK_FALSE(crypto.verifyP256(std::span<const std::byte>(crypto.pubkey), std::span<const std::byte>(b),
                                  std::span<const std::byte>(sigA.data(), 64)));
}

// ---- M4C-17..24 -------------------------------------------------------------
// ITEM 1, client side

TEST_CASE("M4C-17: client pins the hub key at PAIR_GRANT and verifies on the next session") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(5001);
    M4cDelegate hubDel;
    auto hubCrypto = makeCrypto(0x50);
    Hub hub(catalog, clock, hubRng, hubDel, hubCrypto);
    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    XorShift32 clientRng(6001);
    M4cClientDelegate cDel;
    auto clientCrypto = makeCrypto(0x50);  // the SAME machine, from the client's side
    ClientIdentity id = makeIdentity(20, false);
    Client client(id, link.endpointB(), clock, clientRng, cDel, clientCrypto);
    client.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);

    REQUIRE(client.connect());
    pump(hub, clock, client);

    // Ceremony: push-to-pair, so the ceremony itself needs no operator UI here.
    hub.openPresenceWindow();
    REQUIRE(client.sendPairKnock());
    pump(hub, clock, client);
    REQUIRE(cDel.pairGrants == 1);
    // TOFU AT A VERIFIED MOMENT: the key arrived with the grant.
    REQUIRE(cDel.pinnedKey.size() == hubCrypto.pubkey.size());
    CHECK(client.hubPublicKey().size() == hubCrypto.pubkey.size());
    for (size_t i = 0; i < cDel.pinnedKey.size(); ++i) CHECK(cDel.pinnedKey[i] == hubCrypto.pubkey[i]);

    // Next session: present the token, and ask the machine to prove itself.
    client.disconnect();
    ClientIdentity id2 = makeIdentity(20, false);
    id2.hasToken = true;
    std::memcpy(id2.token.data(), cDel.grantedToken.data(), id2.token.size());
    M4cClientDelegate cDel2;
    Client client2(id2, link.endpointB(), clock, clientRng, cDel2, clientCrypto);
    client2.setHubPublicKey(client.hubPublicKey());
    client2.requestHubSignature(true);
    client2.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);
    REQUIRE(client2.connect());
    CHECK(client2.hubAuthState() == HubAuthState::Pending);
    pump(hub, clock, client2, 8);
    CHECK(client2.hubAuthState() == HubAuthState::Verified);
}

TEST_CASE("M4C-18: an evil twin is DETECTED and the library itself withholds intents") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(5002);
    M4cDelegate hubDel;
    auto twinCrypto = makeCrypto(0x61);  // the clone's key
    Hub hub(catalog, clock, hubRng, hubDel, twinCrypto);
    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    XorShift32 clientRng(6002);
    M4cClientDelegate cDel;
    auto clientCrypto = makeCrypto(0x60);  // pinned: the REAL machine's key
    ClientIdentity id = makeIdentity(21, false);
    Client client(id, link.endpointB(), clock, clientRng, cDel, clientCrypto);
    client.setHubPublicKey(std::span<const std::byte>(clientCrypto.pubkey));
    client.requestHubSignature(true);
    client.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);

    REQUIRE(client.connect());
    pump(hub, clock, client, 8);

    CHECK(client.hubAuthState() == HubAuthState::Mismatch);
    REQUIRE_FALSE(cDel.authHistory.empty());
    CHECK(cDel.authHistory.back() == HubAuthState::Mismatch);
    // NORMATIVE: intents are withheld. Not "the app should stop" — the library
    // stops, because on this product driving a stranger's machine is a physical
    // failure and a UI that forgets is not an acceptable single point of trust.
    IntentValueMap v{};
    v.count = 1;
    v.fields[0] = IntentValueField{1, IntentValue::ofF32(1.0f)};
    CHECK_FALSE(client.sendIntent(kMotionCh, v).has_value());
}

TEST_CASE("M4C-19: silence from a hub whose key we hold becomes Timeout, and also withholds") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(5003);
    M4cDelegate hubDel;
    auto hubCrypto = makeCrypto(0x70);
    Hub hub(catalog, clock, hubRng, hubDel, hubCrypto);
    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    XorShift32 clientRng(6003);
    M4cClientDelegate cDel;
    auto clientCrypto = makeCrypto(0x70);
    ClientIdentity id = makeIdentity(22, false);
    Client client(id, link.endpointB(), clock, clientRng, cDel, clientCrypto);
    client.setHubPublicKey(std::span<const std::byte>(clientCrypto.pubkey));
    client.requestHubSignature(true);
    client.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);

    REQUIRE(client.connect());
    // NOBODY EVER SIGNS: the application's signer never runs. The cheapest
    // evil-twin strategy is silence, so silence has to have an answer.
    pumpNoSigner(hub, clock, client, 20, 1000);
    CHECK(client.hubAuthState() == HubAuthState::Pending);  // still inside the window
    pumpNoSigner(hub, clock, client, 40, 100000);           // ...past hub_sig_timeout_ms
    CHECK(client.hubAuthState() == HubAuthState::Timeout);
    IntentValueMap v{};
    v.count = 1;
    v.fields[0] = IntentValueField{1, IntentValue::ofF32(1.0f)};
    CHECK_FALSE(client.sendIntent(kMotionCh, v).has_value());
}

TEST_CASE("M4C-20: no pinned key means Unverifiable — silence is NOT evidence, and nothing is withheld") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(5004);
    M4cDelegate hubDel;
    SoftwareCrypto potatoHub;  // a hub with no keypair at all
    Hub hub(catalog, clock, hubRng, hubDel, potatoHub);
    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    XorShift32 clientRng(6004);
    M4cClientDelegate cDel;
    ClientIdentity id = makeIdentity(23, false);
    Client client(id, link.endpointB(), clock, clientRng, cDel);  // default crypto: no verify either
    client.requestHubSignature(true);
    client.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);

    REQUIRE(client.connect());
    pumpNoSigner(hub, clock, client, 40, 100000);
    CHECK(client.hubAuthState() == HubAuthState::Unverifiable);
    CHECK(client.state() == ClientSessionState::LIVE);  // an ordinary, working session
}

TEST_CASE("M4C-21: a second, bogus HUB_SIG cannot downgrade an already-verified client") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(5005);
    M4cDelegate hubDel;
    auto hubCrypto = makeCrypto(0x80);
    Hub hub(catalog, clock, hubRng, hubDel, hubCrypto);
    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    XorShift32 clientRng(6005);
    M4cClientDelegate cDel;
    auto clientCrypto = makeCrypto(0x80);
    ClientIdentity id = makeIdentity(24, false);
    Client client(id, link.endpointB(), clock, clientRng, cDel, clientCrypto);
    client.setHubPublicKey(std::span<const std::byte>(clientCrypto.pubkey));
    client.requestHubSignature(true);
    client.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);
    REQUIRE(client.connect());
    pump(hub, clock, client, 8);
    REQUIRE(client.hubAuthState() == HubAuthState::Verified);

    // Inject a forged HUB_SIG straight at the client, as a frame-injecting
    // attacker on a cleartext binding would.
    HubSigMsg forged{};
    forged.trust_map.has_welcome_sig = true;
    forged.trust_map.welcome_sig_len = 64;
    for (uint8_t i = 0; i < 64; ++i) forged.trust_map.welcome_sig[i] = std::byte{0xEE};
    std::array<std::byte, 128> buf{};
    size_t n = encodeHubSig(forged, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(link.endpointA(), FrameType::HUB_SIG, 0, std::span<const std::byte>(buf.data(), n));
    pump(hub, clock, client, 4);
    CHECK(client.hubAuthState() == HubAuthState::Verified);  // first valid answer wins
}

TEST_CASE("M4C-22: an UNSOLICITED signature claims nothing") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(5006);
    M4cDelegate hubDel;
    auto hubCrypto = makeCrypto(0x90);
    Hub hub(catalog, clock, hubRng, hubDel, hubCrypto);
    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    XorShift32 clientRng(6006);
    M4cClientDelegate cDel;
    auto clientCrypto = makeCrypto(0x90);
    ClientIdentity id = makeIdentity(25, false);
    Client client(id, link.endpointB(), clock, clientRng, cDel, clientCrypto);
    client.setHubPublicKey(std::span<const std::byte>(clientCrypto.pubkey));
    // NOTE: requestHubSignature() never called.
    client.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);
    REQUIRE(client.connect());
    pump(hub, clock, client, 4);

    auto material = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(nonceOf(0)),
                                   client.sessionId(), client.bootId());
    std::array<std::byte, kTrustSigMaxBytes> sig{};
    size_t sn = hubCrypto.signP256(std::span<const std::byte>(material), std::span<std::byte>(sig));
    HubSigMsg m{};
    m.trust_map.has_welcome_sig = true;
    m.trust_map.welcome_sig_len = uint8_t(sn);
    for (size_t i = 0; i < sn; ++i) m.trust_map.welcome_sig[i] = sig[i];
    std::array<std::byte, 128> buf{};
    size_t n = encodeHubSig(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(link.endpointA(), FrameType::HUB_SIG, 0, std::span<const std::byte>(buf.data(), n));
    pump(hub, clock, client, 4);
    CHECK(client.hubAuthState() == HubAuthState::NotRequested);
}

TEST_CASE("M4C-23: hubAuthState resets per connect — a verified session does not vouch for the next one") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(5007);
    M4cDelegate hubDel;
    auto hubCrypto = makeCrypto(0xA0);
    Hub hub(catalog, clock, hubRng, hubDel, hubCrypto);
    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    XorShift32 clientRng(6007);
    M4cClientDelegate cDel;
    auto clientCrypto = makeCrypto(0xA0);
    ClientIdentity id = makeIdentity(26, false);
    Client client(id, link.endpointB(), clock, clientRng, cDel, clientCrypto);
    client.setHubPublicKey(std::span<const std::byte>(clientCrypto.pubkey));
    client.requestHubSignature(true);
    client.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);
    REQUIRE(client.connect());
    pump(hub, clock, client, 8);
    REQUIRE(client.hubAuthState() == HubAuthState::Verified);

    REQUIRE(client.connect());  // reconnect
    CHECK(client.hubAuthState() == HubAuthState::Pending);
    pump(hub, clock, client, 8);
    CHECK(client.hubAuthState() == HubAuthState::Verified);
}

TEST_CASE("M4C-24: the client draws FRESH entropy every connect") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(5008);
    M4cDelegate hubDel;
    auto hubCrypto = makeCrypto(0xB0);
    Hub hub(catalog, clock, hubRng, hubDel, hubCrypto);
    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    XorShift32 clientRng(6008);
    M4cClientDelegate cDel;
    auto clientCrypto = makeCrypto(0xB0);
    ClientIdentity id = makeIdentity(27, false);
    Client client(id, link.endpointB(), clock, clientRng, cDel, clientCrypto);
    client.setHubPublicKey(std::span<const std::byte>(clientCrypto.pubkey));
    client.requestHubSignature(true);

    REQUIRE(client.connect());
    clock.advanceUs(1000);
    hub.update(clock.nowUs());
    Hub::SignJob j1;
    REQUIRE(hub.takePendingSignJob(j1));

    REQUIRE(client.connect());
    clock.advanceUs(1000);
    hub.update(clock.nowUs());
    Hub::SignJob j2;
    REQUIRE(hub.takePendingSignJob(j2));

    CHECK(j1.message != j2.message);
    // Specifically the NONCE half differs, not merely the session id — a client
    // that reused its nonce would be leaning entirely on the hub for freshness.
    bool nonceDiffers = false;
    for (size_t i = 0; i < kTrustClientNonceBytes; ++i) {
        if (j1.message[i] != j2.message[i]) nonceDiffers = true;
    }
    CHECK(nonceDiffers);
}

// ---- M4C-25..33 -------------------------------------------------------------
// ITEMS 2+3: presentation modes and the AUTH frame

namespace {

// Puts `instance` in the ledger at `role` and returns its token.
std::array<std::byte, limits::token_bytes> pairInto(Hub& hub, uint8_t idByte, AccessLevel role, IRandom& rng) {
    std::array<std::byte, limits::token_bytes> tok{};
    auto inst = instanceOf(idByte);
    REQUIRE(hub.pairing().grant(std::span<const std::byte>(inst), role, pairing_modes::knock_approve, rng,
                                std::span<std::byte>(tok)) != nullptr);
    return tok;
}

}  // namespace

TEST_CASE("M4C-25: bearer presentation is unchanged — a raw token in HELLO still grants its role") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(7001);
    M4cDelegate del;
    SoftwareCrypto potato;  // NO P-256 anywhere: the floor must not need it
    Hub hub(catalog, clock, rng, del, potato);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    auto tok = pairInto(hub, 30, AccessLevel::control, rng);
    writeHello(link.endpointB(), HelloOpts{.idByte = 30, .hasToken = true, .token = tok});
    auto w = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w.has_value());
    CHECK(w->roles == uint8_t(AccessLevel::control));
}

TEST_CASE("M4C-26: proof mode keeps the token OFF the wire — session starts at watch, AUTH upgrades it") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(7002);
    M4cDelegate del;
    SoftwareCrypto potato;
    Hub hub(catalog, clock, rng, del, potato);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    auto tok = pairInto(hub, 31, AccessLevel::control, rng);
    // NO TOKEN IN HELLO. That is the whole point on a cleartext binding.
    writeHello(link.endpointB(), HelloOpts{.idByte = 31, .hasPresentationMode = true,
                                           .presentationMode = uint8_t(presentation_modes::proof)});
    auto w = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w.has_value());
    CHECK(w->roles == uint8_t(AccessLevel::watch));

    auto proof = tokenProof(std::span<const std::byte>(tok), std::span<const std::byte>(w->nonce));
    writeAuth(link.endpointB(), std::span<const std::byte>(proof));
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    auto g = findGrant(replies);
    REQUIRE(g.has_value());
    REQUIRE(g->has_roles);
    CHECK(g->roles == uint8_t(AccessLevel::control));
    CHECK(countNacks(replies, NackCode::UNAUTHORIZED) == 0);
}

TEST_CASE("M4C-27: a wrong proof is a NACK UNAUTHORIZED and changes nothing") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(7003);
    M4cDelegate del;
    SoftwareCrypto potato;
    Hub hub(catalog, clock, rng, del, potato);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    pairInto(hub, 32, AccessLevel::configure, rng);
    writeHello(link.endpointB(), HelloOpts{.idByte = 32, .hasPresentationMode = true,
                                           .presentationMode = uint8_t(presentation_modes::proof)});
    auto w = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w.has_value());

    std::array<std::byte, kTrustTokenProofBytes> junk{};
    junk.fill(std::byte{0x5A});
    writeAuth(link.endpointB(), std::span<const std::byte>(junk));
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    CHECK(countNacks(replies, NackCode::UNAUTHORIZED) == 1);
    CHECK_FALSE(findGrant(replies).has_value());
    const HubSession* s = hub.sessionBySlot(0);
    REQUIRE(s != nullptr);
    CHECK(s->role == AccessLevel::watch);  // a failed proof never downgrades OR upgrades
}

TEST_CASE("M4C-28: three failed proofs end the session with GOODBYE UNAUTHORIZED") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(7004);
    M4cDelegate del;
    SoftwareCrypto potato;
    Hub hub(catalog, clock, rng, del, potato);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    pairInto(hub, 33, AccessLevel::control, rng);
    writeHello(link.endpointB(), HelloOpts{.idByte = 33, .hasPresentationMode = true,
                                           .presentationMode = uint8_t(presentation_modes::proof)});
    REQUIRE(findWelcome(tickAndDrain(hub, clock, link.endpointB())).has_value());
    CHECK(hub.sessionCount() == 1);

    std::array<std::byte, kTrustTokenProofBytes> junk{};
    junk.fill(std::byte{0x11});
    bool sawGoodbye = false;
    for (uint32_t i = 0; i < limits::auth_attempts_max; ++i) {
        writeAuth(link.endpointB(), std::span<const std::byte>(junk));
        auto r = tickAndDrain(hub, clock, link.endpointB());
        CHECK(countNacks(r, NackCode::UNAUTHORIZED) == 1);
        if (hasGoodbye(r, NackCode::UNAUTHORIZED)) sawGoodbye = true;
    }
    CHECK(sawGoodbye);
    CHECK(hub.sessionCount() == 0);
}

TEST_CASE("M4C-29: an AUTH from an unpaired instance is UNAUTHORIZED, not an identity oracle") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(7005);
    M4cDelegate del;
    SoftwareCrypto potato;
    Hub hub(catalog, clock, rng, del, potato);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    // Nothing in the ledger for instance 34.
    writeHello(link.endpointB(), HelloOpts{.idByte = 34, .hasPresentationMode = true,
                                           .presentationMode = uint8_t(presentation_modes::proof)});
    auto w = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w.has_value());
    std::array<std::byte, kTrustTokenProofBytes> anything{};
    writeAuth(link.endpointB(), std::span<const std::byte>(anything));
    auto r = tickAndDrain(hub, clock, link.endpointB());
    // Identical answer to a wrong proof for a KNOWN device (M4C-27): the hub
    // never tells a caller whether an instance_id exists.
    CHECK(countNacks(r, NackCode::UNAUTHORIZED) == 1);
}

TEST_CASE("M4C-30: a malformed AUTH is MALFORMED and does not spend a strike") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(7006);
    M4cDelegate del;
    SoftwareCrypto potato;
    Hub hub(catalog, clock, rng, del, potato);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    auto tok = pairInto(hub, 35, AccessLevel::control, rng);
    writeHello(link.endpointB(), HelloOpts{.idByte = 35, .hasPresentationMode = true,
                                           .presentationMode = uint8_t(presentation_modes::proof)});
    auto w = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w.has_value());

    // An AUTH whose trust map carries no token_proof at all.
    AuthMsg m{};
    m.trust_map.has_presentation_mode = true;
    m.trust_map.presentation_mode = uint8_t(presentation_modes::proof);
    std::array<std::byte, 64> buf{};
    size_t n = encodeAuth(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    for (int i = 0; i < 5; ++i) {
        writeFrame(link.endpointB(), FrameType::AUTH, 0, std::span<const std::byte>(buf.data(), n));
        auto r = tickAndDrain(hub, clock, link.endpointB());
        CHECK(countNacks(r, NackCode::MALFORMED) == 1);
    }
    CHECK(hub.sessionCount() == 1);  // five malformed frames did NOT evict

    // ...and the real proof still works afterwards.
    auto proof = tokenProof(std::span<const std::byte>(tok), std::span<const std::byte>(w->nonce));
    writeAuth(link.endpointB(), std::span<const std::byte>(proof));
    auto g = findGrant(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(g.has_value());
    CHECK(g->roles == uint8_t(AccessLevel::control));
}

TEST_CASE("M4C-31: a proof captured from one session does NOT authenticate the next") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(7007);
    M4cDelegate del;
    SoftwareCrypto potato;
    Hub hub(catalog, clock, rng, del, potato);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    auto tok = pairInto(hub, 36, AccessLevel::control, rng);
    writeHello(link.endpointB(), HelloOpts{.idByte = 36, .hasPresentationMode = true,
                                           .presentationMode = uint8_t(presentation_modes::proof)});
    auto w1 = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w1.has_value());
    auto capturedProof = tokenProof(std::span<const std::byte>(tok), std::span<const std::byte>(w1->nonce));
    writeAuth(link.endpointB(), std::span<const std::byte>(capturedProof));
    REQUIRE(findGrant(tickAndDrain(hub, clock, link.endpointB())).has_value());

    // New session, new nonce (§6.3). The sniffer's capture is already spent.
    writeHello(link.endpointB(), HelloOpts{.idByte = 36, .hasPresentationMode = true,
                                           .presentationMode = uint8_t(presentation_modes::proof)});
    auto w2 = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w2.has_value());
    CHECK(w2->nonce != w1->nonce);
    writeAuth(link.endpointB(), std::span<const std::byte>(capturedProof));
    auto r = tickAndDrain(hub, clock, link.endpointB());
    CHECK(countNacks(r, NackCode::UNAUTHORIZED) == 1);
    CHECK_FALSE(findGrant(r).has_value());
}

TEST_CASE("M4C-32: the trust ledger records the presentation mode a device actually used") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(7008);
    M4cDelegate del;
    SoftwareCrypto potato;
    Hub hub(catalog, clock, rng, del, potato);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    auto tok = pairInto(hub, 37, AccessLevel::control, rng);
    auto inst = instanceOf(37);
    // A HELLO that declares NOTHING, then an AUTH: the ledger must still learn
    // this device is a proof-mode device, because posture the operator cannot
    // see is posture the operator cannot fix.
    writeHello(link.endpointB(), HelloOpts{.idByte = 37});
    auto w = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w.has_value());
    const auto* before = hub.pairing().findByInstance(std::span<const std::byte>(inst));
    REQUIRE(before != nullptr);
    CHECK(before->presentationMode == uint8_t(presentation_modes::bearer));

    auto proof = tokenProof(std::span<const std::byte>(tok), std::span<const std::byte>(w->nonce));
    writeAuth(link.endpointB(), std::span<const std::byte>(proof), /*declareMode=*/false);
    REQUIRE(findGrant(tickAndDrain(hub, clock, link.endpointB())).has_value());
    const auto* after = hub.pairing().findByInstance(std::span<const std::byte>(inst));
    REQUIRE(after != nullptr);
    CHECK(after->presentationMode == uint8_t(presentation_modes::proof));
}

TEST_CASE("M4C-33: the RFC-029 item-2 tripwire fires on the AUTH path too") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 rng(7009);
    M4cDelegate del;
    SoftwareCrypto potato;
    Hub hub(catalog, clock, rng, del, potato);
    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    auto tok = pairInto(hub, 38, AccessLevel::control, rng);
    auto inst = instanceOf(38);

    // Session 1: proof mode, version 1.0.0 — the baseline the wire is set from.
    writeHello(link.endpointB(), HelloOpts{.idByte = 38, .clientVer = "1.0.0", .hasPresentationMode = true,
                                           .presentationMode = uint8_t(presentation_modes::proof)});
    auto w1 = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w1.has_value());
    writeAuth(link.endpointB(),
              std::span<const std::byte>(tokenProof(std::span<const std::byte>(tok),
                                                    std::span<const std::byte>(w1->nonce))));
    auto g1 = findGrant(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(g1.has_value());
    CHECK(g1->roles == uint8_t(AccessLevel::control));

    // Session 2: same device, DIFFERENT version. Without the shared helper this
    // client would have sailed straight past the tripwire, because it never
    // carries a token in HELLO for the bearer path to observe.
    writeHello(link.endpointB(), HelloOpts{.idByte = 38, .clientVer = "1.1.0", .hasPresentationMode = true,
                                           .presentationMode = uint8_t(presentation_modes::proof)});
    auto w2 = findWelcome(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(w2.has_value());
    writeAuth(link.endpointB(),
              std::span<const std::byte>(tokenProof(std::span<const std::byte>(tok),
                                                    std::span<const std::byte>(w2->nonce))));
    auto g2 = findGrant(tickAndDrain(hub, clock, link.endpointB()));
    REQUIRE(g2.has_value());
    CHECK(g2->roles == uint8_t(AccessLevel::watch));  // suspended pending re-approval
    const auto* e = hub.pairing().findByInstance(std::span<const std::byte>(inst));
    REQUIRE(e != nullptr);
    CHECK(e->state == trust_states::recognized_pending);
    CHECK(e->role == AccessLevel::control);  // SUSPENDED, not revoked
}

TEST_CASE("M4C-34: client-side proof round trip re-subscribes what the upgrade unlocked") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(8001);
    M4cDelegate hubDel;
    SoftwareCrypto potato;
    Hub hub(catalog, clock, hubRng, hubDel, potato);
    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    auto tok = pairInto(hub, 40, AccessLevel::control, hubRng);

    XorShift32 clientRng(9001);
    M4cClientDelegate cDel;
    ClientIdentity id = makeIdentity(40, false);
    id.hasToken = true;
    id.token = tok;
    Client client(id, link.endpointB(), clock, clientRng, cDel);  // default crypto: HMAC only, no P-256
    client.setTokenPresentationMode(uint8_t(presentation_modes::proof));
    client.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);
    client.addSubscriptionWish(0x0102, 5.0f, Priority::normal);  // control-access

    REQUIRE(client.connect());
    clock.advanceUs(1000);
    hub.update(clock.nowUs());
    client.update(clock.nowUs());
    // WELCOME landed at `watch`, so the control-access wish was NOT granted.
    CHECK(client.roles() == AccessLevel::watch);
    CHECK_FALSE(client.grantedRateHz(0x0102).has_value());

    pump(hub, clock, client, 6);
    CHECK(client.roles() == AccessLevel::control);
    REQUIRE_FALSE(cDel.roleHistory.empty());
    CHECK(cDel.roleHistory.back() == AccessLevel::control);
    // The upgrade re-sent the standing wish list, and the hub re-authorized it.
    REQUIRE(client.grantedRateHz(0x0102).has_value());
    CHECK(*client.grantedRateHz(0x0102) == doctest::Approx(5.0f));
}

// ---- M4C-35..37 -------------------------------------------------------------
// THE POTATO FLOOR. If these fail, the weight covenant is broken.

TEST_CASE("M4C-35: a zero-crypto client's HELLO is byte-identical with and without M4c") {
    // The floor, stated as bytes: instance_id + a raw token + subscription
    // wishes. No `trust` map, no nonce, no signature request, no AUTH.
    ClientIdentity id = makeIdentity(50, true);
    HelloMsg m{};
    m.proto_ver = kProtocolVersion;
    m.client_kind = id.client_kind;
    m.client_name = id.client_name;
    m.instance_id = id.instance_id;
    m.has_token = true;
    m.token = id.token;
    std::array<std::byte, 400> buf{};
    size_t n = encodeHello(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    auto back = decodeHello(std::span<const std::byte>(buf.data(), n));
    REQUIRE(back);
    CHECK_FALSE(back.value().has_trust);  // no key 39 was emitted at all

    // And the SAME shape comes out of the real Client when no M4c setter is
    // touched — this is the assertion that would catch a future "helpful"
    // default that starts sending crypto nobody asked for.
    ManualClock clock;
    XorShift32 rng(9100);
    InProcessLink link(clock, rng);
    REQUIRE(link.endpointA().open());
    M4cClientDelegate cDel;
    Client client(id, link.endpointB(), clock, rng, cDel);
    REQUIRE(client.connect());
    auto fb = link.endpointA().read();
    REQUIRE(fb.has_value());
    auto h = fb->header();
    REQUIRE(h);
    CHECK(FrameType(h->type) == FrameType::HELLO);
    auto sent = decodeHello(fb->payload());
    REQUIRE(sent);
    CHECK_FALSE(sent.value().has_trust);
    CHECK(sent.value().has_token);
    CHECK(client.hubAuthState() == HubAuthState::NotRequested);
}

TEST_CASE("M4C-36: a zero-crypto client reaches LIVE and drives intents against a signing hub") {
    Catalog32 catalog;
    makeM4cCatalog(catalog);
    ManualClock clock;
    XorShift32 hubRng(9200);
    M4cDelegate hubDel;
    auto hubCrypto = makeCrypto(0xC0);  // the hub CAN sign; nobody asks it to
    Hub hub(catalog, clock, hubRng, hubDel, hubCrypto);
    InProcessLink link(clock, hubRng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    auto tok = pairInto(hub, 51, AccessLevel::control, hubRng);
    XorShift32 clientRng(9201);
    M4cClientDelegate cDel;
    ClientIdentity id = makeIdentity(51, false);
    id.hasToken = true;
    id.token = tok;
    Client client(id, link.endpointB(), clock, clientRng, cDel);  // defaultCrypto(): zero P-256
    client.addSubscriptionWish(channels::safety, 0.0f, Priority::critical);
    REQUIRE(client.connect());
    pump(hub, clock, client, 8);

    CHECK(client.state() == ClientSessionState::LIVE);
    CHECK(client.roles() == AccessLevel::control);
    CHECK(client.hubAuthState() == HubAuthState::NotRequested);
    CHECK(hub.pendingSignJobs() == 0);  // nothing was ever queued for it
    IntentValueMap v{};
    v.count = 1;
    v.fields[0] = IntentValueField{1, IntentValue::ofF32(1.0f)};
    CHECK(client.sendIntent(kMotionCh, v).has_value());
    CHECK(cDel.authHistory.empty());  // never bothered with any of it
}

TEST_CASE("M4C-37: stored identity for the floor is still 24 bytes — instance_id + token") {
    // The weight covenant, as an arithmetic assertion rather than a promise.
    CHECK(size_t(limits::instance_id_bytes) + size_t(limits::token_bytes) == 24);
    // Everything M4c added is optional wire, not stored state: a bearer client
    // persists nothing new. A client that OPTS IN to hub verification persists
    // one more thing, and exactly one: the 33-byte SEC1 public key.
    CHECK(kTrustPubkeyMaxBytes == 33);
}
