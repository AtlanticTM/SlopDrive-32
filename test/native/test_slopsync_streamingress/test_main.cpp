// ============================================================================
// test_main.cpp — doctest behavioral tests for slopsync-core's inbound
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
// ============================================================================

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
#include "slopsync/wire/messages/welcome.hpp"
#include "slopsync/wire/stream_bundle.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <vector>

using namespace slopsync;

namespace {

// ============================================================================
// Local catalog with a c2h STREAM channel (ascending ids, §8.1):
//   0x0080 "motion-input"  STREAM c2h controller  maxRate 200 Hz  sample 2 B (i16)
//   0x0081 "pos-tele"      STREAM h2c viewer       maxRate 240 Hz  (wrong DIR target)
//   0x0084 "cfg-set"       INTENT c2h controller                   (wrong CLASS target)
// ============================================================================
constexpr uint16_t kStreamCh = 0x0080;   // the c2h STREAM channel under test
constexpr uint16_t kH2cStreamCh = 0x0081;
constexpr uint16_t kIntentCh = 0x0084;
constexpr uint16_t kSegCh = 0x0085;       // c2h STREAM, 6-B timed-segment layout
constexpr uint16_t kUnknownCh = 0x00FF;
constexpr size_t kSampleSize = 2;         // one i16
constexpr size_t kSegSampleSize = 6;      // {target u16, dur u16, end_vel i16}
constexpr int16_t kSegNoEndVel = -32768;  // INT16_MIN sentinel = "no end velocity"

Catalog32 makeStreamCatalog() {
    Catalog32 c;
    c.count = 4;
    auto& e = c.entries;

    e[0].id = kStreamCh; e[0].name = "motion-input";
    e[0].cls = ChannelClass::STREAM; e[0].dir = Direction::c2h;
    e[0].access = AccessLevel::controller; e[0].maxRateHz = 200.0f;
    e[0].defaultPriority = Priority::elevated;
    e[0].fieldCount = 1;
    e[0].layout[0] = {.name = "vel_mm_s", .type = PackedFieldType::i16, .unit = "mm/s", .scale = 1.0f};

    e[1].id = kH2cStreamCh; e[1].name = "pos-tele";
    e[1].cls = ChannelClass::STREAM; e[1].dir = Direction::h2c;
    e[1].access = AccessLevel::viewer; e[1].maxRateHz = 240.0f;
    e[1].defaultPriority = Priority::elevated;
    e[1].fieldCount = 1;
    e[1].layout[0] = {.name = "pos_10um", .type = PackedFieldType::u16, .unit = "mm", .scale = 100.0f};

    e[2].id = kIntentCh; e[2].name = "cfg-set";
    e[2].cls = ChannelClass::INTENT; e[2].dir = Direction::c2h;
    e[2].access = AccessLevel::controller; e[2].maxRateHz = 10.0f;
    e[2].defaultPriority = Priority::critical;
    e[2].fieldCount = 1;
    e[2].schema[0] = {.key = 1, .name = "speed", .type = CborFieldType::f32_t, .unit = "mm/s"};

    // 0x0085 "motion-segment": the 6-byte timed-segment layout the SlopDrive
    // device advertises (target + duration + sentinel-bearing end velocity).
    // Mirrors include/comms/SlopSyncCatalog.h's 0x0085 entry so the harness
    // exercises the exact wire size + field order the firmware decodes.
    e[3].id = kSegCh; e[3].name = "motion-segment";
    e[3].cls = ChannelClass::STREAM; e[3].dir = Direction::c2h;
    e[3].access = AccessLevel::controller; e[3].maxRateHz = 50.0f;
    e[3].defaultPriority = Priority::elevated;
    e[3].fieldCount = 3;
    e[3].layout[0] = {.name = "target_norm",  .type = PackedFieldType::u16, .unit = "norm",   .scale = 10000.0f};
    e[3].layout[1] = {.name = "duration_ms",  .type = PackedFieldType::u16, .unit = "ms",     .scale = 1.0f};
    e[3].layout[2] = {.name = "end_vel_norm", .type = PackedFieldType::i16, .unit = "norm/s", .scale = 1000.0f};

    return c;
}

// ---- delegate: records ingress + ownership + deadman callbacks -------------
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
        return hasToken ? AccessLevel::controller : AccessLevel::viewer;
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
};

// ---- raw frame helpers -----------------------------------------------------
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

int countNacks(const std::vector<DecodedReply>& replies, NackCode code) {
    int n = 0;
    for (const auto& r : replies) {
        if (r.type != FrameType::NACK) continue;
        auto nm = decodeNack(std::span<const std::byte>(r.payload));
        if (nm && nm.value().code == code) ++n;
    }
    return n;
}

// Connects a session on `ep` (attached slot already present), returns its
// WELCOME. Drains the WELCOME off the wire.
WelcomeMsg connectSession(Hub& hub, ManualClock& clock, ITransport& ep, uint8_t idByte, bool token,
                          std::vector<PublishWish> publishes) {
    writeHello(ep, idByte, token, std::move(publishes));
    auto replies = tickAndDrain(hub, clock, ep);
    auto w = findWelcome(replies);
    REQUIRE(w.has_value());
    return w.value();
}

}  // namespace

// ============================================================================
// SI-01 — publish wish on a c2h STREAM channel is granted, rate clamped
// ============================================================================
TEST_CASE("SI-01: a publish wish clamps to catalog max_rate_hz and echoes in granted_publishes") {
    Catalog32 cat = makeStreamCatalog();
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

// ============================================================================
// SI-02 — unknown / wrong-class / wrong-dir wishes are omitted, no NACK
// ============================================================================
TEST_CASE("SI-02: invalid publish wishes are silently omitted; the session still comes up") {
    Catalog32 cat = makeStreamCatalog();
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

// ============================================================================
// SI-03 — a viewer wishing a controller-access channel is not granted
// ============================================================================
TEST_CASE("SI-03: a viewer session cannot be granted a controller-access publish") {
    Catalog32 cat = makeStreamCatalog();
    ManualClock clock;
    XorShift32 rng(103);
    StreamHubDelegate del;
    Hub hub(cat, clock, rng, del);

    InProcessLink link(clock, rng);
    REQUIRE(hub.attachTransport(link.endpointA()));
    REQUIRE(link.endpointB().open());  // no Client owns endpointB here — open it so raw write()s go through

    WelcomeMsg w = connectSession(hub, clock, link.endpointB(), 3, /*token=*/false,  // no token -> viewer
                                  {PublishWish{kStreamCh, 100.0f}});
    CHECK(w.roles == uint8_t(AccessLevel::viewer));
    CHECK(w.granted_publishes_count == 0);
}

// ============================================================================
// SI-04 — a granted session's valid bundle is delivered to the delegate
// ============================================================================
TEST_CASE("SI-04: a valid bundle on a granted channel reaches onStreamBundle with a parseable view") {
    Catalog32 cat = makeStreamCatalog();
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

// ============================================================================
// SI-05 — an ungranted session's bundle is silently dropped (no NACK)
// ============================================================================
TEST_CASE("SI-05: a bundle on a channel the session never published is dropped, uncounted-as-error") {
    Catalog32 cat = makeStreamCatalog();
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

// ============================================================================
// SI-06 — malformed bundles are dropped whole, delegate never called
// ============================================================================
TEST_CASE("SI-06: n=0 / over-span / non-monotonic / first!=0 / truncated bundles all drop") {
    Catalog32 cat = makeStreamCatalog();
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

// ============================================================================
// SI-07 — sustained overage NACKs RATE_LIMITED; legal traffic resumes after
// ============================================================================
TEST_CASE("SI-07: flooding samples past the grant NACKs RATE_LIMITED, then a legal bundle is delivered") {
    Catalog32 cat = makeStreamCatalog();
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

// ============================================================================
// SI-08 — source ownership: first bundle acquires; silence fires the deadman
// ============================================================================
TEST_CASE("SI-08: first accepted bundle acquires the source; quiet past the deadman window fires onDeadmanStop") {
    Catalog32 cat = makeStreamCatalog();
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

    // Go quiet. deadman_default_ms is 600 — advance well past it with no frames.
    CHECK(del.deadmanStops.empty());
    for (int i = 0; i < 20; ++i) {   // 20 * 50 ms = 1 s > 600 ms
        clock.advanceUs(50'000);
        hub.update(clock.nowUs());
    }
    REQUIRE(del.deadmanStops.size() == 1);
    CHECK(del.deadmanStops[0] == 0);   // source 0 stopped
}

// ============================================================================
// SI-09 — GOODBYE/reset clears the publish grant; reconnect must re-grant
// ============================================================================
TEST_CASE("SI-09: a session reset clears publish grants — a reconnect without re-wishing cannot stream") {
    Catalog32 cat = makeStreamCatalog();
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

// ============================================================================
// SI-10 — §7.1 CLOCK exchange: hub answers with the 13-byte (header + 12)
// reply; a truncated CLOCK request is silently dropped
// ============================================================================
TEST_CASE("SI-10: the hub answers a CLOCK frame with echoed t0 + hub-time t1/t2, and drops a truncated one") {
    Catalog32 cat = makeStreamCatalog();
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

// ============================================================================
// SI-11 — source-ownership release on GOODBYE (§6.8/§11.4). REGRESSION for the
// field bug: a streaming owner's ownership used to leak past teardown, so after
// the owner left, EVERY later client's bundles were Conflict-dropped until
// reboot. Proves: A owns -> B is Conflict-dropped while A lives -> A GOODBYEs
// -> B's bundles are now ACCEPTED (ownership was released, not orphaned).
// ============================================================================
TEST_CASE("SI-11: after a streaming owner sends GOODBYE, a new session can acquire the source and stream") {
    Catalog32 cat = makeStreamCatalog();
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

// ============================================================================
// SI-12 — source-ownership release on rude transport detach (§6.8: a socket
// death is handled identically to GOODBYE). Same regression as SI-11 but the
// owner never says goodbye — the hub's detachTransport() must still release.
// ============================================================================
TEST_CASE("SI-12: after a streaming owner's transport detaches (no GOODBYE), a new session can acquire the source") {
    Catalog32 cat = makeStreamCatalog();
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

// ============================================================================
// SI-13 — slot reuse: a re-HELLO on the SAME transport without a GOODBYE (a
// reconnect reusing the socket) recycles the slot. The outgoing session's
// source ownership must be released as the new session is minted, else the new
// session — on the very same transport — could never re-acquire its own source.
// ============================================================================
TEST_CASE("SI-13: a re-HELLO recycling a live slot releases the old session's source before the new one streams") {
    Catalog32 cat = makeStreamCatalog();
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

// ============================================================================
// SI-14 — a 6-byte timed-SEGMENT bundle (0x0085) round-trips through the hub's
// generic STREAM ingress: it is granted, delivered, and every byte — crucially
// the INT16_MIN "no end velocity" sentinel that 0 cannot stand in for — reaches
// the delegate's BundleView intact. Proves the segment layout rides the same
// channel-generic path as motion-input with zero library changes.
// ============================================================================
TEST_CASE("SI-14: a 6-B motion-segment bundle is granted and its sentinel end_vel round-trips to the delegate") {
    Catalog32 cat = makeStreamCatalog();
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

// ============================================================================
// SI-15 — GROUND TRUTH: after a deadman STOP latch, the FIRST accepted stream
// bundle from the (new) owning source clears the STOP bit, exactly as a
// source-mapped INTENT does (§11.1 "cleared by any new motion intent"). Without
// the fix the resumed stream drives the arbiter's soft-start while the safety
// STATE still reports STOP — a lie. Verified here on the SEGMENT channel so the
// clear covers 0x0085 too (both map to source 0).
// ============================================================================
TEST_CASE("SI-15: a resumed stream bundle clears the deadman STOP latch (safety STATE stops lying)") {
    Catalog32 cat = makeStreamCatalog();
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

    // 2) Go silent past the 600 ms deadman -> STOP latches + the session is torn
    // down (owner-loss release runs the Stop policy: onDeadmanStop + STOP bit).
    for (int i = 0; i < 20; ++i) { clock.advanceUs(50'000); hub.update(clock.nowUs()); }
    REQUIRE(del.deadmanStops.size() == 1);
    REQUIRE(hub.stopLatched());       // safety STATE now says STOP

    // 3) Reconnect on the same transport, re-wish, and resume streaming. The
    // first ACCEPTED bundle acquires the freed source AND must clear STOP.
    connectSession(hub, clock, link.endpointB(), 15, true, {PublishWish{kSegCh, 50.0f}});
    REQUIRE(hub.stopLatched());       // still latched right up until the bundle lands
    writeSegmentBundle(link.endpointB(), {SegSample{7000, 900, kSegNoEndVel}}, /*tBase=*/900000);
    tickAndDrain(hub, clock, link.endpointB());
    REQUIRE(del.bundles.size() == 2);           // resumed bundle delivered
    CHECK_FALSE(hub.stopLatched());             // STOP cleared by the accepted bundle
    CHECK_FALSE((hub.safetyWord() & slopsync::safety_bits::STOP));
}
