// test_slopsync_streamingress — slopsync-core's inbound
// STREAM-ingress path (client→hub motion-input bundles): the HELLO/WELCOME
// `publishes` grant (§6.2/§6.3), Hub::handleStream ingress validation
// (§9.2/§5.4), granted-rate token bucket (§10.5), and source-ownership +
// deadman participation (§11.3/§11.4).
//
// Native (host-side, hardware-free), same harness as test_slopsync_session:
// InProcessLink + ManualClock + XorShift32, doctest's bundled main(). Frames
// are hand-crafted and written raw onto the transport (there is no client-side
// STREAM publish API yet — that is a later phase), exactly as I-02 in the
// session suite hand-crafts INTENT frames. A LOCAL catalog with a c2h STREAM
// channel is built here — the frozen mini-catalog has no c2h STREAM entry.
//
// Suite ids: SI-xx = stream ingress.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/core/clock.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/hub/hub.hpp"
#include "slopsync/transport/inprocess_binding.hpp"
#include "slopsync/wire/frame_header.hpp"
#include "slopsync/wire/raw/clock_frame.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/nack.hpp"
#include "slopsync/wire/messages/grant.hpp"
#include "slopsync/wire/messages/publish.hpp"
#include "slopsync/wire/messages/subscribe.hpp"
#include "slopsync/wire/messages/welcome.hpp"
#include "slopsync/wire/raw/catalog_ready.hpp"
#include "slopsync/wire/stream_bundle.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <vector>

using namespace slopsync;

namespace {

// ---- Local catalog with a c2h STREAM channel (ascending ids, §8.1): ---------
//   0x0080 "motion-input"  STREAM c2h controller  maxRate 200 Hz  sample 2 B (i16)
//   0x0081 "pos-tele"      STREAM h2c viewer       maxRate 240 Hz  (wrong DIR target)
//   0x0084 "cfg-set"       INTENT c2h controller                   (wrong CLASS target)
constexpr uint16_t kStreamCh = 0x0080;   // the c2h STREAM channel under test
constexpr uint16_t kH2cStreamCh = 0x0081;
constexpr uint16_t kIntentCh = 0x0084;
constexpr uint16_t kSegCh = 0x0085;       // c2h STREAM, 6-B timed-segment layout
constexpr uint16_t kUnknownCh = 0x00FF;
constexpr size_t kSampleSize = 2;         // one i16
constexpr size_t kSegSampleSize = 6;      // {target u16, dur u16, end_vel i16}
constexpr int16_t kSegNoEndVel = -32768;  // INT16_MIN sentinel = "no end velocity"

void makeStreamCatalog(Catalog32& c) {
    c.clear();

    c.addEntry({.id = kStreamCh, .name = "motion-input",
                .cls = ChannelClass::STREAM, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 200.0f,
                .defaultPriority = Priority::elevated});
    c.addLayoutField({.name = "vel_mm_s", .type = PackedFieldType::i16, .unit = "mm/s", .scale = 1.0f});

    c.addEntry({.id = kH2cStreamCh, .name = "pos-tele",
                .cls = ChannelClass::STREAM, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 240.0f,
                .defaultPriority = Priority::elevated});
    c.addLayoutField({.name = "pos_10um", .type = PackedFieldType::u16, .unit = "mm", .scale = 100.0f});

    c.addEntry({.id = kIntentCh, .name = "cfg-set",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 10.0f,
                .defaultPriority = Priority::critical});
    c.addSchemaField({.key = 1, .name = "speed", .type = CborFieldType::f32_t, .unit = "mm/s"});

    // 0x0085 "motion-segment": the 6-byte timed-segment layout the SlopDrive
    // device advertises (target + duration + sentinel-bearing end velocity).
    // Mirrors include/comms/SlopSyncCatalog.h's 0x0085 entry so the harness
    // exercises the exact wire size + field order the firmware decodes.
    // streamKind = segments (RFC-014/023): the explicit catalog property that
    // replaced the M5 unit-string heuristic.
    c.addEntry({.id = kSegCh, .name = "motion-segment",
                .cls = ChannelClass::STREAM, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 50.0f,
                .defaultPriority = Priority::elevated,
                .streamKind = stream_kinds::segments});
    c.addLayoutField({.name = "target_norm",  .type = PackedFieldType::u16, .unit = "norm",   .scale = 10000.0f});
    c.addLayoutField({.name = "duration_ms",  .type = PackedFieldType::u16, .unit = "ms",     .scale = 1.0f});
    c.addLayoutField({.name = "end_vel_norm", .type = PackedFieldType::i16, .unit = "norm/s", .scale = 1000.0f});
}

// ---- delegate: records ingress + ownership + deadman callbacks --------------
struct RecordedBundle {
    uint16_t channel_id = 0;
    uint32_t session_id = 0;
    uint8_t sampleCount = 0;
    uint32_t tBase = 0;
};
struct RecordedOwnership {
    uint8_t source_id = 0;
    uint32_t owner_session = 0;
    uint8_t reason = 0;
};

class StreamHubDelegate final : public HubDelegate {
public:
    bool mapSource = true;                 // 0x0080 -> source 0 when true
    std::vector<RecordedBundle> bundles;
    std::vector<RecordedOwnership> ownership;
    std::vector<uint8_t> deadmanStops;
    // Raw bytes of the most-recent bundle's samples — lets a test confirm the
    // 6-B segment layout (incl. the sentinel end_vel) round-trips through the
    // hub's BundleView unchanged.
    std::vector<std::vector<std::byte>> lastSamples;

    AccessLevel validateToken(std::span<const std::byte>, std::span<const std::byte>, bool hasToken) override {
        return hasToken ? AccessLevel::control : AccessLevel::watch;
    }
    Result<IntentValueMap, NackCode> applyIntent(uint16_t, const IntentValueMap&, AccessLevel, bool&) override {
        return Result<IntentValueMap, NackCode>::err(NackCode::UNKNOWN_CHANNEL);  // not exercised here
    }
    void onEstop(uint8_t, uint8_t) override {}

    std::optional<uint8_t> sourceForChannel(uint16_t channel_id) override {
        // Both stream channels map to the same arbiter source (0), exactly as
        // the firmware maps 0x0084 + 0x0085 to TCODE_STREAM.
        if (mapSource && (channel_id == kStreamCh || channel_id == kSegCh)) return uint8_t{0};
        return std::nullopt;
    }
    void onSourceOwnership(uint8_t source_id, uint32_t owner_session, uint8_t reason) override {
        ownership.push_back(RecordedOwnership{source_id, owner_session, reason});
    }
    void onDeadmanStop(uint8_t source_id) override { deadmanStops.push_back(source_id); }

    void onStreamBundle(uint16_t channel_id, uint32_t session_id, const BundleView& bundle) override {
        bundles.push_back(RecordedBundle{channel_id, session_id, bundle.sampleCount(), bundle.tBase()});
        lastSamples.clear();
        for (uint8_t k = 0; k < bundle.sampleCount(); ++k) {
            auto sp = bundle.sample(k);
            lastSamples.emplace_back(sp.begin(), sp.end());
        }
    }

    // RFC-030: 0 = honor the wish (the library default); nonzero = act like a
    // machine whose curve_policy forces a family, so tests can see the grant
    // echo the EFFECTIVE value rather than parroting the request.
    uint8_t forceCurveFamily = 0;
    uint8_t effectiveCurveFamily(uint16_t, uint8_t requested) override {
        return forceCurveFamily != 0 ? forceCurveFamily : requested;
    }
};

// ---- raw frame helpers ------------------------------------------------------
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

// Build a HELLO with the given publish wishes and write it (raw) to `ep`.
void writeHello(ITransport& ep, uint8_t idByte, bool withToken,
                std::vector<PublishWish> publishes) {
    HelloMsg m{};
    m.proto_ver = kProtocolVersion;
    m.client_kind = "sim";
    m.client_name = "stream-test";
    m.instance_id.fill(std::byte{0});
    m.instance_id[0] = std::byte{idByte};
    if (withToken) {
        m.has_token = true;
        m.token.fill(std::byte{0xAA});
    }
    m.publishes_count = uint32_t(publishes.size());
    for (size_t i = 0; i < publishes.size(); ++i) m.publishes[i] = publishes[i];

    std::array<std::byte, 300> buf{};
    size_t n = encodeHello(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(ep, FrameType::HELLO, 0, std::span<const std::byte>(buf.data(), n));
}

// A valid bundle: n samples, 1 us apart, first t_off 0. Written raw as a
// STREAM frame on `channel`.
void writeValidBundle(ITransport& ep, uint16_t channel, uint8_t n, uint32_t tBase = 1000) {
    std::array<std::byte, 256> buf{};
    BundleWriter w(std::span<std::byte>(buf), tBase, kSampleSize);
    std::array<std::byte, kSampleSize> sample{};
    for (uint8_t i = 0; i < n; ++i) REQUIRE(w.addSample(uint16_t(i), std::span<const std::byte>(sample)));
    size_t len = w.finalize();
    REQUIRE(len > 0);
    writeFrame(ep, FrameType::STREAM, channel, std::span<const std::byte>(buf.data(), len));
}

// A valid 6-byte-sample segment bundle: each sample is
// {target_norm u16, duration_ms u16, end_vel_norm i16} little-endian, samples
// 1 us apart with first t_off 0. Written raw as a STREAM frame on kSegCh.
struct SegSample { uint16_t target; uint16_t durMs; int16_t endVel; };
void writeSegmentBundle(ITransport& ep, const std::vector<SegSample>& samples, uint32_t tBase = 2000) {
    std::array<std::byte, 256> buf{};
    BundleWriter w(std::span<std::byte>(buf), tBase, kSegSampleSize);
    for (size_t i = 0; i < samples.size(); ++i) {
        std::array<std::byte, kSegSampleSize> s{};
        auto put16 = [&](size_t off, uint16_t v) {
            s[off] = std::byte(v & 0xFF);
            s[off + 1] = std::byte((v >> 8) & 0xFF);
        };
        put16(0, samples[i].target);
        put16(2, samples[i].durMs);
        put16(4, uint16_t(samples[i].endVel));
        REQUIRE(w.addSample(uint16_t(i), std::span<const std::byte>(s)));
    }
    size_t len = w.finalize();
    REQUIRE(len > 0);
    writeFrame(ep, FrameType::STREAM, kSegCh, std::span<const std::byte>(buf.data(), len));
}

// Decode the end_vel_norm (i16 at byte offset 4) out of a captured sample.
int16_t sampleEndVel(const std::vector<std::byte>& s) {
    return int16_t(uint16_t(uint8_t(s[4])) | (uint16_t(uint8_t(s[5])) << 8));
}

// Hand-assembled bundle bytes so we can inject illegal layouts BundleWriter
// would refuse to produce (non-monotonic t_off, first!=0, over-span, n=0).
std::vector<std::byte> rawBundleBytes(uint32_t tBase, const std::vector<uint16_t>& tOffs) {
    std::vector<std::byte> b;
    auto push16 = [&](uint16_t v) { b.push_back(std::byte(v & 0xFF)); b.push_back(std::byte((v >> 8) & 0xFF)); };
    auto push32 = [&](uint32_t v) {
        b.push_back(std::byte(v & 0xFF)); b.push_back(std::byte((v >> 8) & 0xFF));
        b.push_back(std::byte((v >> 16) & 0xFF)); b.push_back(std::byte((v >> 24) & 0xFF));
    };
    push32(tBase);
    b.push_back(std::byte(uint8_t(tOffs.size())));  // n
    b.push_back(std::byte(0));                        // reserved
    for (uint16_t o : tOffs) push16(o);
    for (size_t i = 0; i < tOffs.size(); ++i)
        for (size_t k = 0; k < kSampleSize; ++k) b.push_back(std::byte(0));
    return b;
}

// Drives one tick and returns every frame the hub sent back on `ep`.
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

// Mid-session publish renegotiation (§6.6/RFC-013): PUBLISH carries the same
// `publishes` array HELLO does and is answered with a GRANT.
void writePublish(ITransport& ep, const std::vector<PublishWish>& wishes) {
    PublishMsg m{};
    m.publishes_count = uint32_t(wishes.size());
    for (size_t i = 0; i < wishes.size(); ++i) m.publishes[i] = wishes[i];
    std::array<std::byte, 300> buf{};
    size_t n = encodePublish(m, std::span<std::byte>(buf));
    REQUIRE(n > 0);
    writeFrame(ep, FrameType::PUBLISH, 0, std::span<const std::byte>(buf.data(), n));
}

std::optional<GrantMsg> findGrant(const std::vector<DecodedReply>& replies) {
    for (const auto& r : replies) {
        if (r.type != FrameType::GRANT) continue;
        auto g = decodeGrant(std::span<const std::byte>(r.payload));
        if (g) return g.value();
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

// Declares catalog readiness (§8.4/RFC-015) with the etag WELCOME advertised.
// These sessions HELLO without a cached etag, so the hub gates both planes
// until this lands — a raw-frame client has to say it can decode.
void writeCatalogReady(ITransport& ep, std::span<const std::byte> etag) {
    std::array<std::byte, kCatalogReadyBytes> buf{};
    size_t n = encodeCatalogReady(etag, std::span<std::byte>(buf));
    REQUIRE(n == kCatalogReadyBytes);
    writeFrame(ep, FrameType::CATALOG_READY, 0, std::span<const std::byte>(buf.data(), n));
}

// Connects a session on `ep` (attached slot already present), returns its
// WELCOME. Drains the WELCOME off the wire. `ready` (default true) also
// declares CATALOG_READY so the session's data plane is open — pass false to
// exercise the pre-READY gate itself.
WelcomeMsg connectSession(Hub& hub, ManualClock& clock, ITransport& ep, uint8_t idByte, bool token,
                          std::vector<PublishWish> publishes, bool ready = true) {
    writeHello(ep, idByte, token, std::move(publishes));
    auto replies = tickAndDrain(hub, clock, ep);
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());
    if (ready) {
        writeCatalogReady(ep, std::span<const std::byte>(w->catalog_etag));
        tickAndDrain(hub, clock, ep);
    }
    return w.value();
}

}  // namespace

// ---- SI-01 ------------------------------------------------------------------
// publish wish on a c2h STREAM channel is granted, rate clamped
TEST_CASE("SI-01: a publish wish clamps to catalog max_rate_hz and echoes in granted_publishes") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(101);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through

    WelcomeMsg w = connectSession(hub, clock, link.endpointB(), 1, /*token=*/true,
                                  {PublishWish{kStreamCh, 500.0f}});  // wish 500 > max 200

    REQUIRE(w.granted_publishes_count == 1);
    CHECK(w.granted_publishes[0].channel_id == kStreamCh);
    CHECK(w.granted_publishes[0].granted_rate_hz == doctest::Approx(200.0f));
}

// ---- SI-02 ------------------------------------------------------------------
// unknown / wrong-class / wrong-dir wishes are omitted, no NACK
TEST_CASE("SI-02: invalid publish wishes are silently omitted; the session still comes up") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(102);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through

    writeHello(link.endpointB(), 2, /*token=*/true,
               {PublishWish{kUnknownCh, 100.0f}, PublishWish{kIntentCh, 100.0f}, PublishWish{kH2cStreamCh, 100.0f}});
    auto replies = tickAndDrain(hub, clock, link.endpointB());

    auto w = findWelcome(replies);
    REQUIRE(w.has_value());
    CHECK(w->granted_publishes_count == 0);           // none granted
    CHECK(countNacks(replies, NackCode::UNKNOWN_CHANNEL) == 0);   // §6.2: absence, not error
    CHECK(w->session_id != 0);                        // session is live
}

// ---- SI-03 ------------------------------------------------------------------
// a viewer wishing a controller-access channel is not granted
TEST_CASE("SI-03: a viewer session cannot be granted a controller-access publish") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(103);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through

    WelcomeMsg w = connectSession(hub, clock, link.endpointB(), 3, /*token=*/false,  // no token -> viewer
                                  {PublishWish{kStreamCh, 100.0f}});
    CHECK(w.roles == uint8_t(AccessLevel::watch));
    CHECK(w.granted_publishes_count == 0);
}

// ---- SI-04 ------------------------------------------------------------------
// a granted session's valid bundle is delivered to the delegate
TEST_CASE("SI-04: a valid bundle on a granted channel reaches onStreamBundle with a parseable view") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(104);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through
    WelcomeMsg w = connectSession(hub, clock, link.endpointB(), 4, true, {PublishWish{kStreamCh, 200.0f}});
    REQUIRE(w.granted_publishes_count == 1);

    writeValidBundle(link.endpointB(), kStreamCh, /*n=*/5, /*tBase=*/7777);
    tickAndDrain(hub, clock, link.endpointB());

    REQUIRE(del.bundles.size() == 1);
    CHECK(del.bundles[0].channel_id == kStreamCh);
    CHECK(del.bundles[0].session_id == w.session_id);
    CHECK(del.bundles[0].sampleCount == 5);
    CHECK(del.bundles[0].tBase == 7777);
}

// ---- SI-05 ------------------------------------------------------------------
// an ungranted session's bundle is silently dropped (no NACK)
TEST_CASE("SI-05: a bundle on a channel the session never published is dropped, uncounted-as-error") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(105);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through
    // Connect WITHOUT any publish wish.
    connectSession(hub, clock, link.endpointB(), 5, true, {});

    writeValidBundle(link.endpointB(), kStreamCh, 4);
    auto replies = tickAndDrain(hub, clock, link.endpointB());

    CHECK(del.bundles.empty());
    CHECK(countNacks(replies, NackCode::RATE_LIMITED) == 0);
    CHECK(hub.streamIngressCounters(0).accepted == 0);
    CHECK(hub.streamIngressCounters(0).dropped == 1);
}

// ---- SI-06 ------------------------------------------------------------------
// malformed bundles are dropped whole, delegate never called
TEST_CASE("SI-06: n=0 / over-span / non-monotonic / first!=0 / truncated bundles all drop") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(106);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through
    connectSession(hub, clock, link.endpointB(), 6, true, {PublishWish{kStreamCh, 200.0f}});

    // n == 0
    { auto b = rawBundleBytes(1000, {}); writeFrame(link.endpointB(), FrameType::STREAM, kStreamCh, b); }
    // span > 20 ms (20000 us cap): 30000 us
    { auto b = rawBundleBytes(1000, {0, 30000}); writeFrame(link.endpointB(), FrameType::STREAM, kStreamCh, b); }
    // non-monotonic (5 then 3)
    { auto b = rawBundleBytes(1000, {0, 5, 3}); writeFrame(link.endpointB(), FrameType::STREAM, kStreamCh, b); }
    // first t_off != 0
    { auto b = rawBundleBytes(1000, {5, 10}); writeFrame(link.endpointB(), FrameType::STREAM, kStreamCh, b); }
    // truncated: valid 3-sample bundle minus its last byte
    {
        auto b = rawBundleBytes(1000, {0, 1, 2});
        b.pop_back();
        writeFrame(link.endpointB(), FrameType::STREAM, kStreamCh, b);
    }

    tickAndDrain(hub, clock, link.endpointB());

    CHECK(del.bundles.empty());
    CHECK(hub.streamIngressCounters(0).accepted == 0);
    CHECK(hub.streamIngressCounters(0).dropped == 5);
}

// ---- SI-07 ------------------------------------------------------------------
// sustained overage NACKs RATE_LIMITED; legal traffic resumes after
TEST_CASE("SI-07: flooding samples past the grant NACKs RATE_LIMITED, then a legal bundle is delivered") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(107);
    StreamHubDelegate del;
    del.mapSource = false;  // isolate rate-limiting: no source means no deadman teardown during the 1 s refill
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through
    // Grant at 200 Hz -> bucket capacity 200 samples.
    connectSession(hub, clock, link.endpointB(), 7, true, {PublishWish{kStreamCh, 200.0f}});

    // Fire 10 bundles of 32 samples (320 samples) within a SINGLE tick — no
    // clock advance between writes means no bucket refill. 200 capacity means
    // the first ~6 bundles pass (192) and the rest overdraw -> RATE_LIMITED.
    for (int i = 0; i < 10; ++i) writeValidBundle(link.endpointB(), kStreamCh, 32, /*tBase=*/uint32_t(1000 + i));
    auto replies = tickAndDrain(hub, clock, link.endpointB());

    CHECK(del.bundles.size() >= 1);
    CHECK(del.bundles.size() < 10);                         // some dropped
    CHECK(countNacks(replies, NackCode::RATE_LIMITED) >= 1);
    size_t acceptedBefore = del.bundles.size();

    // Let the bucket refill a full second, then a legal small bundle lands.
    clock.advanceUs(1'000'000);
    hub.update(clock.nowUs());
    writeValidBundle(link.endpointB(), kStreamCh, 4, /*tBase=*/50000);
    tickAndDrain(hub, clock, link.endpointB());

    CHECK(del.bundles.size() == acceptedBefore + 1);        // legal traffic delivered again
}

// ---- SI-08 ------------------------------------------------------------------
// source ownership: first bundle acquires; silence fires the deadman
TEST_CASE("SI-08 (RFC-042/RFC-045): first accepted bundle acquires the source; quiet past the deadman window releases it, latches nothing, and marks the session STALE") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(108);
    StreamHubDelegate del;  // mapSource = true -> 0x0080 maps to source 0
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through
    WelcomeMsg w = connectSession(hub, clock, link.endpointB(), 8, true, {PublishWish{kStreamCh, 200.0f}});

    writeValidBundle(link.endpointB(), kStreamCh, 3);
    tickAndDrain(hub, clock, link.endpointB());

    REQUIRE(del.bundles.size() == 1);
    REQUIRE(del.ownership.size() == 1);
    CHECK(del.ownership[0].source_id == 0);
    CHECK(del.ownership[0].owner_session == w.session_id);
    CHECK(del.ownership[0].reason == 0);  // acquire

    size_t sessionsBefore = hub.sessionCount();

    // Go quiet. deadman_default_ms is 600 — advance well past it with no frames.
    CHECK(del.deadmanStops.empty());
    for (int i = 0; i < 20; ++i) {   // 20 * 50 ms = 1 s > 600 ms
        clock.advanceUs(50'000);
        hub.update(clock.nowUs());
    }
    // RFC-045: onDeadmanStop is never called any more — the deadman is
    // liveness bookkeeping, not a safety mechanism. Ownership is still
    // released (unconditionally, §11.4), it just latches nothing on the way.
    CHECK(del.deadmanStops.empty());
    REQUIRE(del.ownership.size() == 2);
    CHECK(del.ownership[1].source_id == 0);
    CHECK(del.ownership[1].owner_session == 0);
    CHECK(del.ownership[1].reason == 3);  // deadman-release, still reported
    CHECK_FALSE(hub.stopLatched());
    // RFC-042: the session goes STALE, the slot is RETAINED (not freed).
    CHECK(hub.sessionCount() == sessionsBefore);
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::STALE);
}

// ---- SI-09 ------------------------------------------------------------------
// GOODBYE/reset clears the publish grant; reconnect must re-grant
TEST_CASE("SI-09: a session reset clears publish grants — a reconnect without re-wishing cannot stream") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(109);
    StreamHubDelegate del;
    del.mapSource = false;  // isolate the grant lifecycle from ownership
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through

    // 1) Connect WITH a publish wish -> a bundle is delivered.
    connectSession(hub, clock, link.endpointB(), 9, true, {PublishWish{kStreamCh, 200.0f}});
    writeValidBundle(link.endpointB(), kStreamCh, 3);
    tickAndDrain(hub, clock, link.endpointB());
    REQUIRE(del.bundles.size() == 1);

    // 2) GOODBYE tears down the session (reset clears publishGrants).
    writeFrame(link.endpointB(), FrameType::GOODBYE, 0, std::span<const std::byte>{});
    tickAndDrain(hub, clock, link.endpointB());

    // 3) Reconnect WITHOUT re-wishing -> the old grant must not survive.
    connectSession(hub, clock, link.endpointB(), 9, true, {});
    writeValidBundle(link.endpointB(), kStreamCh, 3);
    tickAndDrain(hub, clock, link.endpointB());
    CHECK(del.bundles.size() == 1);   // still 1 — the post-reset bundle was dropped

    // 4) Reconnect WITH the wish again -> streaming works once more (re-grant).
    connectSession(hub, clock, link.endpointB(), 9, true, {PublishWish{kStreamCh, 200.0f}});
    writeValidBundle(link.endpointB(), kStreamCh, 3);
    tickAndDrain(hub, clock, link.endpointB());
    CHECK(del.bundles.size() == 2);
}

// ---- SI-10 ------------------------------------------------------------------
// §7.1 CLOCK exchange: hub answers with the 13-byte (header + 12)
// reply; a truncated CLOCK request is silently dropped
TEST_CASE("SI-10: the hub answers a CLOCK frame with echoed t0 + hub-time t1/t2, and drops a truncated one") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(110);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 10, /*token=*/false, {});  // viewer is fine — CLOCK is role-free

    SUBCASE("well-formed request -> 12-byte reply payload, t0 echoed, t1==t2==hub clock") {
        const uint32_t t0 = 0xDEADBEEFu;  // arbitrary client-µs
        std::array<std::byte, kClockRequestBytes> reqBuf{};
        REQUIRE(encodeClockRequest(t0, std::span<std::byte>(reqBuf)) == kClockRequestBytes);
        writeFrame(link.endpointB(), FrameType::CLOCK, 0, std::span<const std::byte>(reqBuf));

        auto replies = tickAndDrain(hub, clock, link.endpointB());
        const uint32_t hubUsAtProcess = clock.nowUs();  // the tick's clock value = hub-µs during handleClock

        int clockReplies = 0;
        for (const auto& r : replies) {
            if (r.type != FrameType::CLOCK) continue;
            ++clockReplies;
            CHECK(r.channel == 0);
            REQUIRE(r.payload.size() == kClockReplyBytes);  // 12 bytes = the "13-byte" frame minus its header type
            auto rep = decodeClockReply(std::span<const std::byte>(r.payload));
            REQUIRE(rep);
            CHECK(rep.value().t0 == t0);                 // echoed unchanged
            CHECK(rep.value().t1 == hubUsAtProcess);     // hub-µs at receipt, from the injected clock
            CHECK(rep.value().t2 == hubUsAtProcess);     // == t1 under a ManualClock (no advance mid-tick)
            CHECK(rep.value().t1 <= rep.value().t2);     // §7.1 ordering invariant
        }
        CHECK(clockReplies == 1);
    }

    SUBCASE("truncated request (< 4 bytes) is silently dropped, no reply") {
        std::array<std::byte, 3> shortReq{};  // one byte short of a u32 t0
        writeFrame(link.endpointB(), FrameType::CLOCK, 0, std::span<const std::byte>(shortReq));

        auto replies = tickAndDrain(hub, clock, link.endpointB());
        int clockReplies = 0;
        for (const auto& r : replies)
            if (r.type == FrameType::CLOCK) ++clockReplies;
        CHECK(clockReplies == 0);
    }
}

// ---- SI-11 ------------------------------------------------------------------
// source-ownership release on GOODBYE (§6.8/§11.4). REGRESSION for the
// field bug: a streaming owner's ownership used to leak past teardown, so after
// the owner left, EVERY later client's bundles were Conflict-dropped until
// reboot. Proves: A owns -> B is Conflict-dropped while A lives -> A GOODBYEs
// -> B's bundles are now ACCEPTED (ownership was released, not orphaned).
TEST_CASE("SI-11: after a streaming owner sends GOODBYE, a new session can acquire the source and stream") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(111);
    StreamHubDelegate del;  // mapSource=true -> 0x0080 maps to source 0 (Stop policy by default)
    Hub hub(cat, clock, rng, del);

    InProcessLink linkA(clock, rng), linkB(clock, rng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));  // slot 0
    REQUIRE(hub.attachTransport(linkB.endpointA()));  // slot 1
    REQUIRE(linkA.endpointB().open());
    REQUIRE(linkB.endpointB().open());

    // A connects + streams -> acquires source 0.
    WelcomeMsg wA = connectSession(hub, clock, linkA.endpointB(), 1, true, {PublishWish{kStreamCh, 200.0f}});
    writeValidBundle(linkA.endpointB(), kStreamCh, 3);
    tickAndDrain(hub, clock, linkA.endpointB());
    REQUIRE(del.bundles.size() == 1);
    REQUIRE(del.ownership.size() == 1);
    CHECK(del.ownership[0].owner_session == wA.session_id);
    CHECK(del.ownership[0].reason == 0);  // acquire

    // B connects + streams WHILE A still owns -> Conflict, dropped (proves the
    // ownership is genuinely exclusive, so the release below is what unblocks B).
    WelcomeMsg wB = connectSession(hub, clock, linkB.endpointB(), 2, true, {PublishWish{kStreamCh, 200.0f}});
    writeValidBundle(linkB.endpointB(), kStreamCh, 3);
    tickAndDrain(hub, clock, linkB.endpointB());
    CHECK(del.bundles.size() == 1);                         // B's bundle did NOT reach the delegate
    CHECK(hub.streamIngressCounters(1).dropped == 1);       // dropped on slot 1 (B)
    CHECK(del.ownership.size() == 1);                        // no new ownership event

    // A sends GOODBYE -> teardown releases source 0.
    writeFrame(linkA.endpointB(), FrameType::GOODBYE, 0, std::span<const std::byte>{});
    tickAndDrain(hub, clock, linkA.endpointB());
    REQUIRE(del.ownership.size() == 2);
    CHECK(del.ownership[1].owner_session == 0);              // released
    CHECK(del.ownership[1].reason == 4);                    // session-loss-release

    // B streams again -> now ACCEPTED (acquires the freed source).
    writeValidBundle(linkB.endpointB(), kStreamCh, 3);
    tickAndDrain(hub, clock, linkB.endpointB());
    REQUIRE(del.bundles.size() == 2);
    CHECK(del.bundles[1].session_id == wB.session_id);
    REQUIRE(del.ownership.size() == 3);
    CHECK(del.ownership[2].owner_session == wB.session_id);
    CHECK(del.ownership[2].reason == 0);                    // B acquires
}

// ---- SI-12 ------------------------------------------------------------------
// source-ownership release on rude transport detach (§6.8: a socket
// death is handled identically to GOODBYE). Same regression as SI-11 but the
// owner never says goodbye — the hub's detachTransport() must still release.
TEST_CASE("SI-12: after a streaming owner's transport detaches (no GOODBYE), a new session can acquire the source") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(112);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink linkA(clock, rng), linkB(clock, rng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));  // slot 0
    REQUIRE(hub.attachTransport(linkB.endpointA()));  // slot 1
    REQUIRE(linkA.endpointB().open());
    REQUIRE(linkB.endpointB().open());

    WelcomeMsg wA = connectSession(hub, clock, linkA.endpointB(), 1, true, {PublishWish{kStreamCh, 200.0f}});
    writeValidBundle(linkA.endpointB(), kStreamCh, 3);
    tickAndDrain(hub, clock, linkA.endpointB());
    REQUIRE(del.bundles.size() == 1);
    REQUIRE(del.ownership.size() == 1);
    CHECK(del.ownership[0].owner_session == wA.session_id);

    WelcomeMsg wB = connectSession(hub, clock, linkB.endpointB(), 2, true, {PublishWish{kStreamCh, 200.0f}});
    writeValidBundle(linkB.endpointB(), kStreamCh, 3);
    tickAndDrain(hub, clock, linkB.endpointB());
    CHECK(del.bundles.size() == 1);                         // conflict while A owns
    CHECK(del.ownership.size() == 1);

    // Rude death: the transport layer detaches A's endpoint. No GOODBYE frame.
    hub.detachTransport(linkA.endpointA());
    REQUIRE(del.ownership.size() == 2);
    CHECK(del.ownership[1].owner_session == 0);              // released by detach
    CHECK(del.ownership[1].reason == 4);

    // B streams again -> accepted.
    writeValidBundle(linkB.endpointB(), kStreamCh, 3);
    tickAndDrain(hub, clock, linkB.endpointB());
    REQUIRE(del.bundles.size() == 2);
    CHECK(del.bundles[1].session_id == wB.session_id);
    REQUIRE(del.ownership.size() == 3);
    CHECK(del.ownership[2].owner_session == wB.session_id);
    CHECK(del.ownership[2].reason == 0);
}

// ---- SI-13 ------------------------------------------------------------------
// slot reuse: a re-HELLO on the SAME transport without a GOODBYE (a
// reconnect reusing the socket) recycles the slot. The outgoing session's
// source ownership must be released as the new session is minted, else the new
// session — on the very same transport — could never re-acquire its own source.
TEST_CASE("SI-13: a re-HELLO recycling a live slot releases the old session's source before the new one streams") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(113);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink linkA(clock, rng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));
    REQUIRE(linkA.endpointB().open());

    // First session A owns source 0.
    WelcomeMsg wA = connectSession(hub, clock, linkA.endpointB(), 1, true, {PublishWish{kStreamCh, 200.0f}});
    writeValidBundle(linkA.endpointB(), kStreamCh, 3);
    tickAndDrain(hub, clock, linkA.endpointB());
    REQUIRE(del.bundles.size() == 1);
    REQUIRE(del.ownership.size() == 1);
    CHECK(del.ownership[0].owner_session == wA.session_id);

    // Re-HELLO on the SAME transport, no GOODBYE -> slot recycled into a new
    // session A'. The old session's ownership must be released in the process.
    WelcomeMsg wA2 = connectSession(hub, clock, linkA.endpointB(), 1, true, {PublishWish{kStreamCh, 200.0f}});
    CHECK(wA2.session_id != wA.session_id);                 // genuinely a fresh session
    REQUIRE(del.ownership.size() == 2);
    CHECK(del.ownership[1].owner_session == 0);              // old session's source released
    CHECK(del.ownership[1].reason == 4);

    // A' streams -> must ACQUIRE the freed source (would Conflict against the
    // orphaned old session_id under the bug).
    writeValidBundle(linkA.endpointB(), kStreamCh, 3);
    tickAndDrain(hub, clock, linkA.endpointB());
    REQUIRE(del.bundles.size() == 2);
    CHECK(del.bundles[1].session_id == wA2.session_id);
    REQUIRE(del.ownership.size() == 3);
    CHECK(del.ownership[2].owner_session == wA2.session_id);
    CHECK(del.ownership[2].reason == 0);                    // A' acquires cleanly
}

// ---- SI-14 ------------------------------------------------------------------
// a 6-byte timed-SEGMENT bundle (0x0085) round-trips through the hub's
// generic STREAM ingress: it is granted, delivered, and every byte — crucially
// the INT16_MIN "no end velocity" sentinel that 0 cannot stand in for — reaches
// the delegate's BundleView intact. Proves the segment layout rides the same
// channel-generic path as motion-input with zero library changes.
TEST_CASE("SI-14: a 6-B motion-segment bundle is granted and its sentinel end_vel round-trips to the delegate") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(114);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    // Wish 200 Hz on a 50 Hz channel -> granted at the catalog ceiling (proves
    // the grant loop is channel-generic, min(wish, max_rate), no 0x0084 special-
    // casing).
    WelcomeMsg w = connectSession(hub, clock, link.endpointB(), 14, true, {PublishWish{kSegCh, 200.0f}});
    REQUIRE(w.granted_publishes_count == 1);
    CHECK(w.granted_publishes[0].channel_id == kSegCh);
    CHECK(w.granted_publishes[0].granted_rate_hz == doctest::Approx(50.0f));

    // Two segments: one carrying a real handoff velocity (250 -> 0.25 units/s),
    // one carrying the -32768 sentinel (engine should estimate vf itself).
    writeSegmentBundle(link.endpointB(),
                       {SegSample{3000, 900, 250}, SegSample{7000, 900, kSegNoEndVel}},
                       /*tBase=*/2000);
    tickAndDrain(hub, clock, link.endpointB());

    REQUIRE(del.bundles.size() == 1);
    CHECK(del.bundles[0].channel_id == kSegCh);
    CHECK(del.bundles[0].sampleCount == 2);
    REQUIRE(del.lastSamples.size() == 2);
    REQUIRE(del.lastSamples[0].size() == kSegSampleSize);
    // Sample 0: a genuine end velocity, NOT the sentinel.
    CHECK(sampleEndVel(del.lastSamples[0]) == int16_t(250));
    CHECK(sampleEndVel(del.lastSamples[0]) != kSegNoEndVel);
    // Sample 1: the sentinel survives the wire byte-for-byte (it must, or the
    // firmware would decode "arrive at rest" instead of "no constraint").
    CHECK(sampleEndVel(del.lastSamples[1]) == kSegNoEndVel);
}

// ---- SI-15 (RFC-045) --------------------------------------------------------
// GROUND TRUTH, restated: a deadman fire never lies in the
// first place, so there is nothing left for a resumed stream to "clear". The
// original SI-15 proved a workaround (an accepted STREAM bundle silently
// clearing a latched STOP) that existed only to un-wedge reconnect ergonomics
// after the deadman's OWN forced-STOP latch — RFC-045 removed that latch
// entirely, making the workaround moot (not merely obsolete: there is no
// longer a deadman-born STOP to clear). This test proves the STRONGER
// property directly: silence never latches STOP, so a resumed stream finds
// the safety plane exactly as it left it. Verified on the SEGMENT channel so
// the release covers 0x0085 too (both map to source 0).
TEST_CASE("SI-15: a deadman fire never latches STOP, so a resumed stream finds nothing to clear") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(115);
    StreamHubDelegate del;  // mapSource = true -> both stream channels -> source 0
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    // 1) A owns source 0 via a segment stream.
    connectSession(hub, clock, link.endpointB(), 15, true, {PublishWish{kSegCh, 50.0f}});
    writeSegmentBundle(link.endpointB(), {SegSample{3000, 900, kSegNoEndVel}});
    tickAndDrain(hub, clock, link.endpointB());
    REQUIRE(del.bundles.size() == 1);
    REQUIRE(del.ownership.size() == 1);
    CHECK_FALSE(hub.stopLatched());   // no latch yet

    // 2) Go silent past the 600 ms deadman -> ownership releases (RFC-042: the
    // session goes STALE, slot retained) but RFC-045 means NOTHING latches.
    for (int i = 0; i < 20; ++i) { clock.advanceUs(50'000); hub.update(clock.nowUs()); }
    CHECK(del.deadmanStops.empty());          // never called any more
    CHECK_FALSE(hub.stopLatched());           // safety STATE was never touched
    REQUIRE(hub.sessionBySlot(0) != nullptr);
    CHECK(hub.sessionBySlot(0)->state == HubSessionState::STALE);

    // 3) Reconnect on the same transport, re-wish, and resume streaming (this
    // is a fresh HELLO on the SAME physical slot the stale session already
    // occupies, so it recycles the slot exactly like any same-transport
    // re-HELLO — SI-13's path — rather than RFC-042's cross-transport
    // reattach). The first ACCEPTED bundle acquires the freed source; the
    // safety plane was clean the entire time.
    connectSession(hub, clock, link.endpointB(), 15, true, {PublishWish{kSegCh, 50.0f}});
    CHECK_FALSE(hub.stopLatched());
    writeSegmentBundle(link.endpointB(), {SegSample{7000, 900, kSegNoEndVel}}, /*tBase=*/900000);
    tickAndDrain(hub, clock, link.endpointB());
    REQUIRE(del.bundles.size() == 2);           // resumed bundle delivered
    CHECK_FALSE(hub.stopLatched());
    CHECK_FALSE((hub.safetyWord() & slopsync::safety_bits::STOP));
}

// ---- SI-16 ------------------------------------------------------------------
// PUBLISH (0x18): a producer adds a publish grant MID-SESSION. Before
// RFC-013 the only way to want a new c2h STREAM channel was to tear the whole
// session down and reconnect with a different HELLO. Also proves the §6.2
// validation rules are unchanged: an invalid wish is silently OMITTED from the
// grants (absence, never a NACK), exactly as in HELLO.
TEST_CASE("SI-16: PUBLISH grants a new publish mid-session; invalid wishes are silently omitted") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(116);
    StreamHubDelegate del;
    del.mapSource = false;  // isolate the grant mechanics from ownership
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    // Connect wishing NOTHING: the segment channel is ungranted, so its bundles
    // are dropped (the pre-RFC-013 reconnect-or-nothing situation).
    connectSession(hub, clock, link.endpointB(), 16, true, {});
    writeSegmentBundle(link.endpointB(), {SegSample{3000, 900, kSegNoEndVel}});
    tickAndDrain(hub, clock, link.endpointB());
    REQUIRE(del.bundles.empty());

    // Renegotiate: one good wish (clamped 200 -> 50 by the catalog ceiling) and
    // three that must fail validation exactly as they would in HELLO.
    writePublish(link.endpointB(), {PublishWish{kSegCh, 200.0f},
                                    PublishWish{kUnknownCh, 10.0f},      // unknown channel
                                    PublishWish{kIntentCh, 10.0f},       // wrong class
                                    PublishWish{kH2cStreamCh, 10.0f}});  // wrong direction
    auto replies = tickAndDrain(hub, clock, link.endpointB());

    auto g = findGrant(replies);
    REQUIRE(g.has_value());
    REQUIRE(g->granted_publishes_count == 1);              // only the legal wish survives
    CHECK(g->granted_publishes[0].channel_id == kSegCh);
    CHECK(g->granted_publishes[0].granted_rate_hz == doctest::Approx(50.0f));
    CHECK(countNacks(replies, NackCode::UNKNOWN_CHANNEL) == 0);  // §6.2: absence, not error
    CHECK(countNacks(replies, NackCode::CLASS_MISMATCH) == 0);

    // The grant is truth: bundles now reach the delegate, no reconnect involved.
    writeSegmentBundle(link.endpointB(), {SegSample{7000, 900, kSegNoEndVel}}, /*tBase=*/9000);
    tickAndDrain(hub, clock, link.endpointB());
    REQUIRE(del.bundles.size() == 1);
    CHECK(del.bundles[0].channel_id == kSegCh);
}

// ---- SI-17 ------------------------------------------------------------------
// PUBLISH REPLACES an existing grant for the same channel (semantics
// mirror SUBSCRIBE's re-subscribe), rather than stacking a second entry.
TEST_CASE("SI-17: a PUBLISH for an already-granted channel replaces that grant in place") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(117);
    StreamHubDelegate del;
    del.mapSource = false;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    WelcomeMsg w = connectSession(hub, clock, link.endpointB(), 17, true, {PublishWish{kStreamCh, 200.0f}});
    REQUIRE(w.granted_publishes_count == 1);
    CHECK(w.granted_publishes[0].granted_rate_hz == doctest::Approx(200.0f));

    // Same channel, lower ask -> the grant is REPLACED at 20 Hz.
    writePublish(link.endpointB(), {PublishWish{kStreamCh, 20.0f}});
    auto replies = tickAndDrain(hub, clock, link.endpointB());
    auto g = findGrant(replies);
    REQUIRE(g.has_value());
    REQUIRE(g->granted_publishes_count == 1);
    CHECK(g->granted_publishes[0].channel_id == kStreamCh);
    CHECK(g->granted_publishes[0].granted_rate_hz == doctest::Approx(20.0f));

    // The replacement is enforced, not cosmetic: the bucket now holds 20
    // samples, so a 32-sample bundle in one tick overdraws where it used to fit.
    writeValidBundle(link.endpointB(), kStreamCh, 32, /*tBase=*/4000);
    auto after = tickAndDrain(hub, clock, link.endpointB());
    CHECK(del.bundles.empty());
    CHECK(countNacks(after, NackCode::RATE_LIMITED) == 1);
}

// ---- SI-18 ------------------------------------------------------------------
// RFC-013 burst: the token bucket's CAPACITY is the granted burst
// while its REFILL RATE stays the granted sample rate. This is what lets the
// real segment streamer (2-4/s mean, ~25/s peak) declare what it actually is
// instead of inflating its rate 10x to buy burst headroom. The hub clamps to
// max_burst_multiple x rate and ECHOES the applied value (ground truth).
TEST_CASE("SI-18: a requested burst is clamped to max_burst_multiple, echoed, and becomes the bucket depth") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(118);
    StreamHubDelegate del;
    del.mapSource = false;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    // Ask 5 Hz with a wildly optimistic burst of 100 -> clamped to 5 x 4 = 20.
    PublishWish wish{kSegCh, 5.0f};
    wish.has_burst = true;
    wish.burst = 100.0f;
    WelcomeMsg w = connectSession(hub, clock, link.endpointB(), 18, true, {wish});

    REQUIRE(w.granted_publishes_count == 1);
    CHECK(w.granted_publishes[0].granted_rate_hz == doctest::Approx(5.0f));  // rate untouched by the burst
    REQUIRE(w.granted_publishes[0].has_burst);                               // echoed because it was asked for
    CHECK(w.granted_publishes[0].burst == doctest::Approx(5.0f * float(limits::max_burst_multiple)));

    // 20 samples in ONE tick (no refill between them) all fit the deeper bucket.
    for (int i = 0; i < 4; ++i) {
        writeSegmentBundle(link.endpointB(),
                           {SegSample{1000, 100, kSegNoEndVel}, SegSample{2000, 100, kSegNoEndVel},
                            SegSample{3000, 100, kSegNoEndVel}, SegSample{4000, 100, kSegNoEndVel},
                            SegSample{5000, 100, kSegNoEndVel}},
                           /*tBase=*/uint32_t(10000 + i));
    }
    auto burstReplies = tickAndDrain(hub, clock, link.endpointB());
    CHECK(del.bundles.size() == 4);  // all 20 samples admitted
    CHECK(countNacks(burstReplies, NackCode::RATE_LIMITED) == 0);

    // The 21st sample in the same drained bucket overdraws -> the limiter is
    // still a limiter, just a deeper one.
    writeSegmentBundle(link.endpointB(), {SegSample{6000, 100, kSegNoEndVel}}, /*tBase=*/20000);
    auto over = tickAndDrain(hub, clock, link.endpointB(), /*stepUs=*/0);
    CHECK(del.bundles.size() == 4);
    CHECK(countNacks(over, NackCode::RATE_LIMITED) == 1);
}

// ---- SI-19 ------------------------------------------------------------------
// no burst asked = today's behavior EXACTLY: capacity equals the
// granted rate and the grant echoes no `burst` key at all (so a non-bursty
// client's WELCOME stays byte-identical to a pre-RFC-013 hub's).
TEST_CASE("SI-19: an unrequested burst defaults to the granted rate and is not echoed") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(119);
    StreamHubDelegate del;
    del.mapSource = false;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    WelcomeMsg w = connectSession(hub, clock, link.endpointB(), 19, true, {PublishWish{kSegCh, 5.0f}});
    REQUIRE(w.granted_publishes_count == 1);
    CHECK_FALSE(w.granted_publishes[0].has_burst);  // absent key: the wire is unchanged

    // Capacity == rate == 5 samples: five fit, the sixth overdraws.
    writeSegmentBundle(link.endpointB(),
                       {SegSample{1000, 100, kSegNoEndVel}, SegSample{2000, 100, kSegNoEndVel},
                        SegSample{3000, 100, kSegNoEndVel}, SegSample{4000, 100, kSegNoEndVel},
                        SegSample{5000, 100, kSegNoEndVel}},
                       /*tBase=*/30000);
    auto first = tickAndDrain(hub, clock, link.endpointB());
    CHECK(del.bundles.size() == 1);
    CHECK(countNacks(first, NackCode::RATE_LIMITED) == 0);

    writeSegmentBundle(link.endpointB(), {SegSample{6000, 100, kSegNoEndVel}}, /*tBase=*/40000);
    auto second = tickAndDrain(hub, clock, link.endpointB(), /*stepUs=*/0);
    CHECK(del.bundles.size() == 1);
    CHECK(countNacks(second, NackCode::RATE_LIMITED) == 1);
}

// ---- SI-20 ------------------------------------------------------------------
// RFC-015 gates the c2h data plane too: a granted producer that has
// not declared CATALOG_READY has never received the retained safety latch, so
// its bundles must not reach the arbiter. Dropped + counted, never NACKed
// (§9.2's data-plane rule); accepted the moment readiness is declared.
TEST_CASE("SI-20: bundles from a pre-READY session are dropped, then accepted once it declares readiness") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(120);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());

    WelcomeMsg w =
        connectSession(hub, clock, link.endpointB(), 20, true, {PublishWish{kStreamCh, 200.0f}}, /*ready=*/false);
    REQUIRE(w.granted_publishes_count == 1);  // granted, but gated

    writeValidBundle(link.endpointB(), kStreamCh, 4);
    auto gated = tickAndDrain(hub, clock, link.endpointB());
    CHECK(del.bundles.empty());
    CHECK(del.ownership.empty());  // no source acquired by a blind producer
    CHECK(hub.streamIngressCounters(0).accepted == 0);
    CHECK(hub.streamIngressCounters(0).dropped == 1);
    CHECK(countNacks(gated, NackCode::RATE_LIMITED) == 0);  // silent, per §9.2

    writeCatalogReady(link.endpointB(), std::span<const std::byte>(w.catalog_etag));
    tickAndDrain(hub, clock, link.endpointB());
    writeValidBundle(link.endpointB(), kStreamCh, 4, /*tBase=*/60000);
    tickAndDrain(hub, clock, link.endpointB());

    REQUIRE(del.bundles.size() == 1);
    CHECK(hub.streamIngressCounters(0).accepted == 1);
    REQUIRE(del.ownership.size() == 1);
    CHECK(del.ownership[0].owner_session == w.session_id);
}

// ---- SI-16 (RFC-012) --------------------------------------------------------
// a producer whose arbiter source is owned by another LIVE
// session gets NACK SOURCE_CONFLICT on its FIRST dropped bundle, then is
// throttled exactly like §10.5's RATE_LIMITED NACK.
//
// This is the §9.2 carve-out. Before it, such a producer was silently, totally
// dead: every bundle dropped for ownership, zero wire signal, no way to tell
// "the machine ignores me" from "my socket is fine". The bundles are STILL
// dropped — nothing is queued or retried — the client is just told why.
TEST_CASE("SI-16: second live producer gets SOURCE_CONFLICT, throttled per (session, source)") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(316);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink linkA(clock, rng), linkB(clock, rng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));
    REQUIRE(hub.attachTransport(linkB.endpointA()));
    REQUIRE(linkA.endpointB().open());
    REQUIRE(linkB.endpointB().open());

    connectSession(hub, clock, linkA.endpointB(), 1, true, {{kStreamCh, 100.0f}});
    connectSession(hub, clock, linkB.endpointB(), 2, true, {{kStreamCh, 100.0f}});

    // A takes the source first.
    writeValidBundle(linkA.endpointB(), kStreamCh, 2, /*tBase=*/1000);
    tickAndDrain(hub, clock, linkA.endpointB());
    REQUIRE(del.bundles.size() == 1);

    // B's first bundle: dropped AND told.
    writeValidBundle(linkB.endpointB(), kStreamCh, 2, /*tBase=*/2000);
    auto first = tickAndDrain(hub, clock, linkB.endpointB());
    CHECK(del.bundles.size() == 1);                 // still only A's
    CHECK(hub.streamIngressCounters(1).dropped == 1);
    CHECK(countNacks(first, NackCode::SOURCE_CONFLICT) == 1);

    // The NACK names the channel, which is what makes it actionable.
    bool sawChannel = false;
    for (const auto& r : first) {
        if (r.type != FrameType::NACK) continue;
        auto nm = decodeNack(std::span<const std::byte>(r.payload));
        if (nm && nm.value().code == NackCode::SOURCE_CONFLICT && nm.value().has_channel_id &&
            nm.value().channel_id == kStreamCh) {
            sawChannel = true;
        }
    }
    CHECK(sawChannel);

    // Immediately following conflicts inside the throttle interval are silent —
    // a NACK per dropped bundle would mirror the very flood it reports.
    int extraNacks = 0;
    for (int i = 0; i < 5; ++i) {
        writeValidBundle(linkB.endpointB(), kStreamCh, 2, /*tBase=*/uint32_t(3000 + i));
        extraNacks += countNacks(tickAndDrain(hub, clock, linkB.endpointB(), /*stepUs=*/1000),
                                 NackCode::SOURCE_CONFLICT);
    }
    CHECK(extraNacks == 0);
    CHECK(hub.streamIngressCounters(1).dropped == 6);

    // Past the interval, exactly one more.
    writeValidBundle(linkB.endpointB(), kStreamCh, 2, /*tBase=*/9000);
    auto later = tickAndDrain(hub, clock, linkB.endpointB(),
                              /*stepUs=*/(1000u / limits::stream_ingress_overage_nack_per_s) * 1000u);
    CHECK(countNacks(later, NackCode::SOURCE_CONFLICT) == 1);
}

// ---- SI-17 (RFC-012) --------------------------------------------------------
// the throttle is keyed by SOURCE, not by channel. This
// device maps BOTH 0x0084 and 0x0085 to one arbiter source, and a producer
// failing over between them is ONE dead producer, not two.
TEST_CASE("SI-17: SOURCE_CONFLICT throttle is per-source across channels sharing it") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(317);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink linkA(clock, rng), linkB(clock, rng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));
    REQUIRE(hub.attachTransport(linkB.endpointA()));
    REQUIRE(linkA.endpointB().open());
    REQUIRE(linkB.endpointB().open());

    connectSession(hub, clock, linkA.endpointB(), 1, true, {{kStreamCh, 100.0f}});
    connectSession(hub, clock, linkB.endpointB(), 2, true, {{kStreamCh, 100.0f}, {kSegCh, 50.0f}});

    writeValidBundle(linkA.endpointB(), kStreamCh, 2, /*tBase=*/1000);
    tickAndDrain(hub, clock, linkA.endpointB());

    writeValidBundle(linkB.endpointB(), kStreamCh, 2, /*tBase=*/2000);
    CHECK(countNacks(tickAndDrain(hub, clock, linkB.endpointB()), NackCode::SOURCE_CONFLICT) == 1);

    // Same source, DIFFERENT channel, inside the interval: still silent.
    writeSegmentBundle(linkB.endpointB(), {{5000, 200, kSegNoEndVel}}, /*tBase=*/clock.nowUs());
    CHECK(countNacks(tickAndDrain(hub, clock, linkB.endpointB()), NackCode::SOURCE_CONFLICT) == 0);
}

// ---- SI-18 (RFC-014) --------------------------------------------------------
// the segment SCHEDULING CONTRACT. For a segment-class
// channel, t_base + t_off[i] IS the intended execution start of sample i,
// resolved via §7.2's nearest-window rule. A schedule further ahead than
// limits::max_future_schedule_ms is rejected whole; a PAST one is fine (a late
// bundle is ordinary jitter, and the engine plays it now).
//
// This replaces the unregistered 250 ms folklore constant the MFP plugin was
// guessing against with a private SegLookaheadMs = 120.
TEST_CASE("SI-18: segment schedules beyond max_future_schedule_ms are rejected; past ones accepted") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(318);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, true, {{kSegCh, 50.0f}});

    // NOTE: deliberately NO big clock jump here. t_base is a WRAPPING u32 of
    // microseconds and the schedule test is a signed nearest-window difference,
    // so "in the past" is representable at any hub time — while a multi-second
    // jump with no traffic would trip RFC-024 idle reaping and tear the session
    // down before the bundle ever landed.
    const uint32_t now = clock.nowUs();
    const uint32_t limitUs = limits::max_future_schedule_ms * 1000u;

    // Just inside the window: accepted.
    writeSegmentBundle(link.endpointB(), {{5000, 200, kSegNoEndVel}}, /*tBase=*/now + limitUs - 50000);
    tickAndDrain(hub, clock, link.endpointB(), /*stepUs=*/0);
    CHECK(del.bundles.size() == 1);

    // Beyond it: dropped whole, counted, and NOT NACKed (§9.2 — the carve-out
    // is ownership only; a malformed schedule is a producer bug, not a
    // contended resource).
    writeSegmentBundle(link.endpointB(), {{6000, 200, kSegNoEndVel}}, /*tBase=*/now + limitUs + 100000);
    auto tooFar = tickAndDrain(hub, clock, link.endpointB(), /*stepUs=*/0);
    CHECK(del.bundles.size() == 1);
    CHECK(hub.streamIngressCounters(0).dropped == 1);
    CHECK(countNacks(tooFar, NackCode::RATE_LIMITED) == 0);
    CHECK(countNacks(tooFar, NackCode::SOURCE_CONFLICT) == 0);

    // Late (in the past): accepted — jitter is normal, and the nearest-window
    // rule reads a wrapped-back t_base as "behind", never as "+71 minutes".
    writeSegmentBundle(link.endpointB(), {{7000, 200, kSegNoEndVel}}, /*tBase=*/now - 100000);
    tickAndDrain(hub, clock, link.endpointB(), /*stepUs=*/0);
    CHECK(del.bundles.size() == 2);
}

// ---- SI-19 (RFC-014) --------------------------------------------------------
// the future-schedule clamp applies ONLY to segment-class
// channels. A dense point-sample stream carries timestamps, not schedules, and
// clamping it would break legitimate lookahead buffering.
TEST_CASE("SI-19: the schedule clamp does not apply to non-segment STREAM channels") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(319);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    connectSession(hub, clock, link.endpointB(), 1, true, {{kStreamCh, 100.0f}});

    writeValidBundle(link.endpointB(), kStreamCh, 2,
                     /*tBase=*/clock.nowUs() + limits::max_future_schedule_ms * 1000u + 500000);
    tickAndDrain(hub, clock, link.endpointB(), /*stepUs=*/0);
    CHECK(del.bundles.size() == 1);  // accepted: 0x0080 declares no time-unit field
}

// ---- SI-20 (RFC-014/023) ----------------------------------------------------
// the CLASSIFICATION itself, evaluated straight from the
// catalog's explicit `stream_kind` entry property (registry key 15): a STREAM
// channel is segment-class iff it declares stream_kind = segments, because a
// sample that carries its own DURATION commands a time extent rather than
// reporting a value at an instant. This replaced the M5 unit-string heuristic
// ("any layout field declares a time unit") — struck because `unit` is a
// free-form tstr and two conforming hubs could disagree ("ms" vs "msec") and
// therefore shed differently under identical congestion. Plus the shedding
// table's segment exception.
TEST_CASE("SI-20: segment-class is the catalog's explicit stream_kind property") {
    Catalog32 cat;
    makeStreamCatalog(cat);

    const CatalogEntry* seg = cat.find(kSegCh);
    const CatalogEntry* pts = cat.find(kStreamCh);
    const CatalogEntry* tele = cat.find(kH2cStreamCh);
    const CatalogEntry* intent = cat.find(kIntentCh);
    REQUIRE(seg);
    REQUIRE(pts);
    REQUIRE(tele);
    REQUIRE(intent);

    CHECK(cat.isSegmentClass(*seg));           // streamKind = stream_kinds::segments
    CHECK_FALSE(cat.isSegmentClass(*pts));     // streamKind left at default (samples)
    CHECK_FALSE(cat.isSegmentClass(*tele));    // streamKind left at default (samples)
    CHECK_FALSE(cat.isSegmentClass(*intent));  // not STREAM at all

    // RFC-014/023 in the shedding table: segment-class is NEVER decimated. Its
    // decisions collapse to Send or Drop — shed whole-source or not at all,
    // because a dropped segment is a permanently lost command and its
    // neighbors describe different intervals, not adjacent points on a curve.
    CHECK(shedDecision(Priority::normal, ChannelClass::STREAM, 1, false) == ShedDecision::Decimate2x);
    CHECK(shedDecision(Priority::normal, ChannelClass::STREAM, 1, true) == ShedDecision::Send);
    CHECK(shedDecision(Priority::background, ChannelClass::STREAM, 1, true) == ShedDecision::Send);
    CHECK(shedDecision(Priority::normal, ChannelClass::STREAM, 2, false) == ShedDecision::Decimate4x);
    CHECK(shedDecision(Priority::normal, ChannelClass::STREAM, 2, true) == ShedDecision::Drop);
    CHECK(shedDecision(Priority::background, ChannelClass::STREAM, 2, true) == ShedDecision::Drop);
    CHECK(shedDecision(Priority::elevated, ChannelClass::STREAM, 2, true) == ShedDecision::Send);
    CHECK(shedDecision(Priority::critical, ChannelClass::STREAM, 2, true) == ShedDecision::Send);
}

// ---- SI-21 (RFC-033) --------------------------------------------------------
// an unacceptable SUBSCRIBE is ANSWERED, never dropped.
// The exact silent failure that cost two debugging nights: a frame carrying
// more wishes than the decoder's 16-entry cap produced nothing at all — no
// GRANT, no NACK — and the session sat LIVE with zero STATE. Now it NACKs
// SUBSCRIBE_REJECTED, and the cap itself is advertised in WELCOME limits so
// no client ever has to binary-search it against a live machine again.
TEST_CASE("SI-21: oversized SUBSCRIBE answers NACK SUBSCRIBE_REJECTED; WELCOME advertises the per-frame cap") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(211);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through
    ITransport& ep = link.endpointB();

    writeHello(ep, 0x21, /*withToken=*/true, {});
    auto helloReplies = tickAndDrain(hub, clock, ep);
    auto w = findWelcome(helloReplies);
    REQUIRE(w.has_value());
    CHECK(w->limits_info.max_subscriptions_per_frame == kSubscribeMaxWishes);
    writeCatalogReady(ep, std::span<const std::byte>(w->catalog_etag));
    tickAndDrain(hub, clock, ep);

    // Hand-encode a SUBSCRIBE with 17 wishes — encodeSubscribe() itself
    // refuses to build one, which is exactly why the overflow could only ever
    // arrive from a foreign client and why the hub must answer it.
    std::array<std::byte, 512> buf{};
    CborWriter cw{std::span<std::byte>(buf)};
    cw.mapHeader(1);
    cw.key(CborKey::subscriptions).arrayHeader(kSubscribeMaxWishes + 1);
    for (uint32_t i = 0; i < kSubscribeMaxWishes + 1; ++i) {
        cw.mapHeader(3);
        cw.key(CborKey::rate_hz).f32Val(10.0f);
        cw.key(CborKey::priority).uintVal(1);
        cw.key(CborKey::channel_id).uintVal(kH2cStreamCh);
    }
    REQUIRE(cw.size() > 0);
    writeFrame(ep, FrameType::SUBSCRIBE, 0, std::span<const std::byte>(buf.data(), cw.size()));

    auto replies = tickAndDrain(hub, clock, ep);
    CHECK(countNacks(replies, NackCode::SUBSCRIBE_REJECTED) == 1);
    CHECK_FALSE(findGrant(replies).has_value());  // rejected wholesale, no partial grant

    // Sanity: a legal SUBSCRIBE on the same session still grants normally.
    std::array<std::byte, 128> ok{};
    CborWriter cw2{std::span<std::byte>(ok)};
    cw2.mapHeader(1);
    cw2.key(CborKey::subscriptions).arrayHeader(1);
    cw2.mapHeader(3);
    cw2.key(CborKey::rate_hz).f32Val(10.0f);
    cw2.key(CborKey::priority).uintVal(1);
    cw2.key(CborKey::channel_id).uintVal(kH2cStreamCh);
    writeFrame(ep, FrameType::SUBSCRIBE, 0, std::span<const std::byte>(ok.data(), cw2.size()));
    auto replies2 = tickAndDrain(hub, clock, ep);
    auto g = findGrant(replies2);
    REQUIRE(g.has_value());
    CHECK(g->grants_count == 1);
}

// ---- SI-22 (RFC-038) --------------------------------------------------------
// the deadman window is a negotiation, not a decree.
// A HELLO wish is clamped into [deadman_min_ms, deadman_max_ms] and the
// APPLIED value comes back on WELCOME key 24 — which was already the echo, so
// a pre-RFC-038 client sees nothing new. No wish = default = today.
TEST_CASE("SI-22: deadman_wish_ms clamps to registry bounds and echoes applied on key 24") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(222);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    auto helloWithWish = [&](ITransport& ep, uint8_t idByte, uint32_t wishMs) {
        HelloMsg m{};
        m.proto_ver = kProtocolVersion;
        m.client_kind = "sim";
        m.client_name = "deadman-test";
        m.instance_id.fill(std::byte{0});
        m.instance_id[0] = std::byte{idByte};
        m.has_token = true;
        m.token.fill(std::byte{0xAA});
        m.deadman_wish_ms = wishMs;
        std::array<std::byte, 300> buf{};
        size_t n = encodeHello(m, std::span<std::byte>(buf));
        REQUIRE(n > 0);
        writeFrame(ep, FrameType::HELLO, 0, std::span<const std::byte>(buf.data(), n));
    };

    InProcessLink linkA(clock, rng), linkB(clock, rng), linkC(clock, rng);
    REQUIRE(hub.attachTransport(linkA.endpointA()));
    REQUIRE(hub.attachTransport(linkB.endpointA()));
    REQUIRE(hub.attachTransport(linkC.endpointA()));
    REQUIRE(linkA.endpointB().open());
    REQUIRE(linkB.endpointB().open());
    REQUIRE(linkC.endpointB().open());

    helloWithWish(linkA.endpointB(), 0x31, 999999);  // over max -> clamp down
    auto wa = findWelcome(tickAndDrain(hub, clock, linkA.endpointB()));
    REQUIRE(wa.has_value());
    CHECK(wa->deadman_ms == limits::deadman_max_ms);

    helloWithWish(linkB.endpointB(), 0x32, 60);      // under min -> clamp up
    auto wb = findWelcome(tickAndDrain(hub, clock, linkB.endpointB()));
    REQUIRE(wb.has_value());
    CHECK(wb->deadman_ms == limits::deadman_min_ms);

    helloWithWish(linkC.endpointB(), 0x33, 0);       // no wish -> default
    auto wc = findWelcome(tickAndDrain(hub, clock, linkC.endpointB()));
    REQUIRE(wc.has_value());
    CHECK(wc->deadman_ms == limits::deadman_default_ms);
}

// ---- SI-23 (RFC-030) --------------------------------------------------------
// curve family: wish in, EFFECTIVE value out.
// A declaring client sees its family echoed by an honoring hub, sees the
// FORCED family from an overriding hub (never a parroted lie), and the
// application can read the granted family back at drain time.
TEST_CASE("SI-23: curve_family wish echoes effective value and is readable via publishCurveFamily") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(233);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through
    ITransport& ep = link.endpointB();

    PublishWish wish{};
    wish.channel_id = kSegCh;
    wish.rate_hz = 30.0f;
    wish.has_curve_family = true;
    wish.curve_family = curve_families::c1_cubic;
    WelcomeMsg w = connectSession(hub, clock, ep, 0x41, /*token=*/true, {wish});

    REQUIRE(w.granted_publishes_count == 1);
    CHECK(w.granted_publishes[0].has_curve_family);
    CHECK(w.granted_publishes[0].curve_family == curve_families::c1_cubic);  // honored
    CHECK(hub.publishCurveFamily(w.session_id, kSegCh) == curve_families::c1_cubic);
    CHECK(hub.publishCurveFamily(w.session_id, kStreamCh) == 0);  // no grant -> unspecified

    // Mid-session renegotiation via PUBLISH, against a machine now FORCING C2:
    // the echo carries what the machine will DO, not what was asked.
    del.forceCurveFamily = curve_families::c2_quintic;
    writePublish(ep, {wish});
    auto replies = tickAndDrain(hub, clock, ep);
    auto g = findGrant(replies);
    REQUIRE(g.has_value());
    REQUIRE(g->granted_publishes_count == 1);
    CHECK(g->granted_publishes[0].has_curve_family);
    CHECK(g->granted_publishes[0].curve_family == curve_families::c2_quintic);  // downgrade is VISIBLE
    CHECK(hub.publishCurveFamily(w.session_id, kSegCh) == curve_families::c2_quintic);

    // A wish that declares nothing gets no family key back (byte-compat rule).
    PublishWish plain{};
    plain.channel_id = kSegCh;
    plain.rate_hz = 30.0f;
    writePublish(ep, {plain});
    auto replies2 = tickAndDrain(hub, clock, ep);
    auto g2 = findGrant(replies2);
    REQUIRE(g2.has_value());
    REQUIRE(g2->granted_publishes_count == 1);
    CHECK_FALSE(g2->granted_publishes[0].has_curve_family);
}

// ---- SI-23b (RFC-049b) ------------------------------------------------------
// downgrade visibility: `requested_curve_family` (key 48)
// echoes the client's ORIGINAL wish verbatim, alongside the EFFECTIVE
// `curve_family` (45) a curve_policy override may have replaced it with. A
// client compares the two present keys directly instead of remembering what
// it asked for.
TEST_CASE("SI-23b: requested_curve_family echoes the original wish verbatim, distinct from a downgraded effective value") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(2331);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());
    ITransport& ep = link.endpointB();

    PublishWish wish{};
    wish.channel_id = kSegCh;
    wish.rate_hz = 30.0f;
    wish.has_curve_family = true;
    wish.curve_family = curve_families::c1_cubic;

    // Honored (no override): requested == effective, both present.
    WelcomeMsg w = connectSession(hub, clock, ep, 0x42, /*token=*/true, {wish});
    REQUIRE(w.granted_publishes_count == 1);
    CHECK(w.granted_publishes[0].has_curve_family);
    CHECK(w.granted_publishes[0].curve_family == curve_families::c1_cubic);
    CHECK(w.granted_publishes[0].has_requested_curve_family);
    CHECK(w.granted_publishes[0].requested_curve_family == curve_families::c1_cubic);

    // Downgraded via PUBLISH renegotiation against a machine forcing C2: the
    // requested key stays the CLIENT's original ask, unmodified by the
    // override — the two now visibly disagree, which IS the downgrade fact.
    del.forceCurveFamily = curve_families::c2_quintic;
    writePublish(ep, {wish});
    auto replies = tickAndDrain(hub, clock, ep);
    auto g = findGrant(replies);
    REQUIRE(g.has_value());
    REQUIRE(g->granted_publishes_count == 1);
    CHECK(g->granted_publishes[0].curve_family == curve_families::c2_quintic);          // effective: downgraded
    CHECK(g->granted_publishes[0].has_requested_curve_family);
    CHECK(g->granted_publishes[0].requested_curve_family == curve_families::c1_cubic);  // requested: unchanged

    // A wish that declares no family gets neither key back (byte-compat rule
    // extends to the new key exactly like the existing one).
    PublishWish plain{};
    plain.channel_id = kSegCh;
    plain.rate_hz = 30.0f;
    writePublish(ep, {plain});
    auto replies2 = tickAndDrain(hub, clock, ep);
    auto g2 = findGrant(replies2);
    REQUIRE(g2.has_value());
    REQUIRE(g2->granted_publishes_count == 1);
    CHECK_FALSE(g2->granted_publishes[0].has_curve_family);
    CHECK_FALSE(g2->granted_publishes[0].has_requested_curve_family);
}

// ---- SI-24 (RFC-016a) -------------------------------------------------------
// WELCOME identity: fw_version finally has an in-band home.
// A hub that declares identity serves it on key 37; one that doesn't stays
// byte-identical to a pre-identity hub (implicitly proven by every other test
// in this suite decoding WELCOMEs from an identity-less hub).
TEST_CASE("SI-24: setIdentity() serves product/fw_version/hub_name on WELCOME key 37") {
    Catalog32 cat;
    makeStreamCatalog(cat);
    ManualClock clock;
    XorShift32 rng(244);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);
    hub.setIdentity("slopsim-bench", "9.9.9", "test rig");

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through
    ITransport& ep = link.endpointB();

    writeHello(ep, 0x51, /*withToken=*/false, {});
    auto replies = tickAndDrain(hub, clock, ep);  // kept alive: identity views point into the payload
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());
    REQUIRE(w->has_identity);
    CHECK(w->identity.product == "slopsim-bench");
    CHECK(w->identity.fw_version == "9.9.9");
    CHECK(w->identity.hub_name == "test rig");
}
