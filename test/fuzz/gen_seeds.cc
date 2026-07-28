// gen_seeds — seed-corpus generator for the SlopSync fuzz targets.
//
// Fuzzing raw random bytes against a deterministic CBOR profile mostly
// produces immediate rejects — the profile rejects on the FIRST byte for most
// random inputs. The highest-value seeds are therefore produced by running the
// project's OWN ENCODERS, which is what this program does: every seed below is
// a byte string the library itself considers valid (or a deliberate one-field
// mutation of one), so libFuzzer starts inside the accepting region and
// mutates outward.
//
// Sources, per the brief:
//   * the frozen mini-catalog (733 bytes, etag-pinned) — encodeCatalog
//   * a catalog carrying the RFC-009/021/026 annotation keys, STORE
//     descriptors and str16/32/64 layout fields, which the frozen fixture
//     deliberately does not (it is frozen; new coverage cannot go in it)
//   * every control-plane message, via its own encoder, with the selector
//     byte fuzz_messages.cc expects prepended
//   * bundles, blob chunks, fragments, packed layouts — via their writers
//
// Build+run: see build.sh (target `gen_seeds`). Output is written under
// <outdir>/<target>/ , one file per seed, named by content hash so
// regenerating is idempotent and re-running never duplicates.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "slopsync/slopsync.h"
#include "slopsync/conformance/mini_catalog.hpp"

using namespace slopsync;

static std::string g_root;

static uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static void emit(const char* target, std::span<const std::byte> bytes) {
    if (bytes.empty()) return;
    std::string dir = g_root + "/" + target;
    ::mkdir(dir.c_str(), 0755);
    char name[64];
    std::snprintf(name, sizeof name, "%016llx",
                  (unsigned long long)fnv1a(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    std::string path = dir + "/" + name;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

// A seed with a one-byte selector/mode prefix (fuzz_messages, fuzz_cbor,
// fuzz_packed, fuzz_bundle all take one).
static void emitPrefixed(const char* target, uint8_t prefix, std::span<const std::byte> body) {
    std::vector<std::byte> v;
    v.push_back(std::byte(prefix));
    v.insert(v.end(), body.begin(), body.end());
    emit(target, std::span<const std::byte>(v.data(), v.size()));
}

// ---- Catalogs ---------------------------------------------------------------

static std::array<std::byte, 64 * 1024> g_buf;

// A catalog exercising everything the FROZEN mini-catalog cannot carry: the
// RFC-009 annotation block (category/category_label/group/desc/role/step/
// flags/options/option_access/setting_channel/setting_key/defaults), the
// RFC-021 STORE descriptor, RFC-026 str16/str32/str64 layout fields, and
// RFC-014/023 stream_kind. This is the shape the client-side attack surface
// actually looks like once the RFC queue lands.
static void buildRichCatalog(Catalog32& c) {
    c.clear();

    // STATE with a bitfield8 + named bits, min/max, units, annotations.
    c.addEntry({.id = 0x0003, .name = "safety",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::critical});
    c.addBitfieldField({.name = "word", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f, .desc = "latched safety flags"},
                       {"estop", "stop", "hold", "pause"});
    c.addLayoutField({.name = "cause", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 255.0f,
                      .group = "safety", .desc = "estop cause code", .role = "diag"});

    // STATE with string fields (RFC-026) interleaved among numerics — the new
    // fixed-width NUL-padded offset math.
    c.addEntry({.id = 0x0010, .name = "session-roster",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 1.0f,
                .defaultPriority = Priority::normal});
    c.addLayoutField({.name = "name", .type = PackedFieldType::str32, .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "kind", .type = PackedFieldType::str16, .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "role", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "note", .type = PackedFieldType::str64, .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "rtt", .type = PackedFieldType::u16, .unit = "ms", .scale = 1.0f});

    // STREAM with an explicit stream_kind (RFC-014/023).
    CatalogEntry st{.id = 0x0085, .name = "motion-segment",
                    .cls = ChannelClass::STREAM, .dir = Direction::c2h,
                    .access = AccessLevel::control, .maxRateHz = 60.0f,
                    .defaultPriority = Priority::elevated};
    st.streamKind = 1;
    c.addEntry(st);
    c.addLayoutField({.name = "target", .type = PackedFieldType::u16, .unit = "", .scale = 10000.0f});
    c.addLayoutField({.name = "dur", .type = PackedFieldType::u16, .unit = "ms", .scale = 1.0f});
    c.addLayoutField({.name = "endv", .type = PackedFieldType::i16, .unit = "", .scale = 1000.0f});

    // INTENT with a schema carrying options + per-option access (RFC-009/026).
    CatalogEntry ie{.id = 0x0201, .name = "motion-config",
                    .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                    .access = AccessLevel::configure, .maxRateHz = 5.0f,
                    .defaultPriority = Priority::normal};
    ie.hasCategory = true;
    ie.category = 2;
    ie.categoryLabel = "motion";
    ie.hasSettingChannel = true;
    ie.settingChannel = 0x0082;
    c.addEntry(ie);
    c.addSelectSchemaField({.key = 1, .name = "backend", .type = CborFieldType::tstr_t, .unit = "",
                            .group = "motion", .desc = "motion backend", .role = "select"},
                           {"stepper", "servo", "sim"},
                           {AccessLevel::control, AccessLevel::configure, AccessLevel::configure});
    c.addSchemaField({.key = 2, .name = "jmax", .type = CborFieldType::f32_t, .unit = "mm",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1e6f,
                      .step = 0.5f, .access = AccessLevel::configure,
                      .hasAccess = true, .hasStep = true});

    // STORE (RFC-021).
    CatalogEntry se{.id = 0x0300, .name = "presets",
                    .cls = ChannelClass::STORE, .dir = Direction::h2c,
                    .access = AccessLevel::configure, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::background};
    c.addEntry(se);
    c.addStoreDescriptor({.storeId = 1, .kind = "pattern-preset", .capacity = 32,
                          .perItemMax = 4096, .nameMax = 24});
}

static void seedCatalogs() {
    static Catalog32 mini, rich;
    conformance::buildMiniCatalog(mini);
    size_t n = encodeCatalog(mini, std::span<std::byte>(g_buf));
    emit("catalog", std::span<const std::byte>(g_buf.data(), n));
    std::vector<std::byte> miniBytes(g_buf.begin(), g_buf.begin() + n);

    buildRichCatalog(rich);
    size_t rn = encodeCatalog(rich, std::span<std::byte>(g_buf));
    emit("catalog", std::span<const std::byte>(g_buf.data(), rn));

    // Degenerate-but-valid: the empty catalog.
    static Catalog32 empty;
    empty.clear();
    size_t en = encodeCatalog(empty, std::span<std::byte>(g_buf));
    emit("catalog", std::span<const std::byte>(g_buf.data(), en));

    // Truncations of a real encoding — the shape a hostile hub reaches for
    // first, and a corpus entry the mutator can grow back out of.
    for (size_t cut : {size_t(1), size_t(8), size_t(64), size_t(300), miniBytes.size() - 1}) {
        if (cut < miniBytes.size()) {
            emit("catalog", std::span<const std::byte>(miniBytes.data(), cut));
        }
    }

    // The same encoded catalog is also a first-class CBOR seed.
    emitPrefixed("cbor", 0, std::span<const std::byte>(miniBytes.data(), miniBytes.size()));
    emitPrefixed("cbor", 1, std::span<const std::byte>(miniBytes.data(), miniBytes.size()));
}

// ---- Control-plane messages -------------------------------------------------
// Selector byte matches fuzz_messages.cc's enum.

static void seedMessages() {
    std::array<std::byte, 2048> buf{};
    auto sp = std::span<std::byte>(buf);

    {  // HELLO — minimal and maximal
        HelloMsg m{};
        m.client_kind = "probe";
        m.client_name = "fuzz-seed";
        size_t n = encodeHello(m, sp);
        emitPrefixed("messages", 0, std::span<const std::byte>(buf.data(), n));
        emitPrefixed("cbor", 0, std::span<const std::byte>(buf.data(), n));

        m.has_token = true;
        m.has_catalog_etag = true;
        m.subscriptions_count = kHelloMaxSubscriptionWishes;
        for (uint32_t i = 0; i < m.subscriptions_count; ++i) {
            m.subscriptions[i] = {uint16_t(0x0080 + i), 30.0f + float(i), uint8_t(i % 4)};
        }
        m.publishes_count = kHelloMaxPublishWishes;
        for (uint32_t i = 0; i < m.publishes_count; ++i) {
            m.publishes[i] = {uint16_t(0x0084 + i), 50.0f, i % 2 == 0, 64.0f};
        }
        n = encodeHello(m, sp);
        emitPrefixed("messages", 0, std::span<const std::byte>(buf.data(), n));

        // M4b: the scoped `trust` (39) sub-map, at its caps. Structure-aware
        // seeds matter most where a NEW nested length lives — a mutator finds
        // the sub-map's interior far faster from a valid one than from bytes.
        m.has_trust = true;
        m.trust_map.has_client_ver = true;
        m.trust_map.client_ver = "12.34.56-rc9+bld.1";  // under the 24 B cap
        m.trust_map.has_client_nonce = true;
        for (size_t i = 0; i < m.trust_map.client_nonce.size(); ++i)
            m.trust_map.client_nonce[i] = std::byte(0xE0 + i);
        m.trust_map.has_sig_request = true;
        m.trust_map.sig_request = true;
        m.trust_map.has_presentation_mode = true;
        m.trust_map.presentation_mode = 1;
        n = encodeHello(m, sp);
        emitPrefixed("messages", 0, std::span<const std::byte>(buf.data(), n));
        emitPrefixed("cbor", 0, std::span<const std::byte>(buf.data(), n));
    }
    {  // WELCOME
        WelcomeMsg m{};
        m.session_id = 0xDEADBEEF;
        m.boot_id = 7;
        m.cfg_gen = 3;
        m.roles = 2;
        m.grants_count = 2;
        m.grants[0] = {0x0080, 30.0f, 1};
        m.grants[1] = {0x0003, 0.0f, 3};
        m.granted_publishes_count = 1;
        m.granted_publishes[0] = {0x0084, 50.0f};
        // M4b: WELCOME's own `trust` sub-map — the pairing-mode advertisement,
        // plus a maximal welcome_sig so the M4c-sized bstr is already in the
        // corpus before M4c ever populates it.
        m.has_trust = true;
        m.trust_map.has_pairing_modes = true;
        m.trust_map.pairing_modes_mask =
            uint8_t(pairing_modes::knock_approve | pairing_modes::pin_proof | pairing_modes::push_to_pair);
        m.trust_map.has_welcome_sig = true;
        m.trust_map.welcome_sig_len = uint8_t(kTrustSigMaxBytes);
        for (size_t i = 0; i < kTrustSigMaxBytes; ++i) m.trust_map.welcome_sig[i] = std::byte(0x30 + i);
        size_t n = encodeWelcome(m, sp);
        emitPrefixed("messages", 1, std::span<const std::byte>(buf.data(), n));
    }
    {  // INTENT — one of every IntentValue kind
        IntentMsg m{};
        m.channel_id = 0x0201;
        m.intent_id = 42;
        m.value_count = 6;
        m.value[0] = {1, IntentValue::ofU64(1234)};
        m.value[1] = {2, IntentValue::ofI64(-7)};
        m.value[2] = {3, IntentValue::ofF32(1.5f)};
        m.value[3] = {4, IntentValue::ofBool(true)};
        m.value[4] = {5, IntentValue::ofTstr("servo")};
        static const std::array<std::byte, 4> blob{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        m.value[5] = {6, IntentValue::ofBstr(std::span<const std::byte>(blob))};
        m.has_takeover = true;
        m.takeover = true;
        size_t n = encodeIntent(m, sp);
        emitPrefixed("messages", 2, std::span<const std::byte>(buf.data(), n));
        emitPrefixed("cbor", 0, std::span<const std::byte>(buf.data(), n));
    }
    {  // SUBSCRIBE / UNSUBSCRIBE
        SubscribeMsg s{};
        s.subscriptions_count = 3;
        for (uint32_t i = 0; i < 3; ++i) s.subscriptions[i] = {uint16_t(0x80 + i), 10.0f, 1};
        size_t n = encodeSubscribe(s, sp);
        emitPrefixed("messages", 3, std::span<const std::byte>(buf.data(), n));

        UnsubscribeMsg u{};
        u.channel_count = 2;
        u.channel_ids[0] = 0x0080;
        u.channel_ids[1] = 0x0082;
        n = encodeUnsubscribe(u, sp);
        emitPrefixed("messages", 4, std::span<const std::byte>(buf.data(), n));
    }
    {  // PUBLISH
        PublishMsg p{};
        p.publishes_count = 2;
        p.publishes[0] = {0x0084, 50.0f, true, 32.0f};
        p.publishes[1] = {0x0085, 4.0f, false, 0.0f};
        size_t n = encodePublish(p, sp);
        emitPrefixed("messages", 5, std::span<const std::byte>(buf.data(), n));
    }
    {  // GRANT
        GrantMsg g{};
        g.grants_count = 2;
        g.grants[0] = {0x0080, 20.0f, 1};
        g.grants[1] = {0x0082, 10.0f, 2};
        g.granted_publishes_count = 1;
        g.granted_publishes[0] = {0x0084, 50.0f};
        size_t n = encodeGrant(g, sp);
        emitPrefixed("messages", 6, std::span<const std::byte>(buf.data(), n));
    }
    {  // ECHO
        EchoMsg e{};
        e.intent_id = 42;
        e.cfg_gen = 4;
        e.applied_count = 2;
        e.applied[0] = {1, IntentValue::ofF32(0.5f)};
        e.applied[1] = {2, IntentValue::ofU64(9)};
        size_t n = encodeEcho(e, sp);
        emitPrefixed("messages", 7, std::span<const std::byte>(buf.data(), n));
    }
    {  // EVENT, with and without the `body` sub-map
        EventMsg e{};
        e.channel_id = 0x0300;
        e.timestamp = 123456;
        e.event_kind = 2;
        size_t n = encodeEvent(e, sp);
        emitPrefixed("messages", 8, std::span<const std::byte>(buf.data(), n));

        e.has_seq_of_state = true;
        e.seq_of_state = 9;
        e.has_body = true;
        e.body_count = 3;
        e.body[0] = {1, IntentValue::ofTstr("preset-a")};
        e.body[1] = {2, IntentValue::ofU64(5)};
        e.body[2] = {3, IntentValue::ofBool(false)};
        n = encodeEvent(e, sp);
        emitPrefixed("messages", 8, std::span<const std::byte>(buf.data(), n));
    }
    {  // NACK / GOODBYE
        NackMsg nk{};
        nk.code = NackCode::RATE_LIMITED;
        nk.has_channel_id = true;
        nk.channel_id = 0x0084;
        nk.has_detail = true;
        nk.detail = "bucket empty";
        nk.has_retry_after_ms = true;
        nk.retry_after_ms = 250;
        nk.has_intent_seq = true;
        nk.intent_seq = 11;
        size_t n = encodeNack(nk, sp);
        emitPrefixed("messages", 9, std::span<const std::byte>(buf.data(), n));

        GoodbyeMsg gb{};
        gb.code = NackCode::UNAUTHORIZED;
        gb.has_detail = true;
        gb.detail = "evicted";
        n = encodeGoodbye(gb, sp);
        emitPrefixed("messages", 10, std::span<const std::byte>(buf.data(), n));
    }
    {  // PAIR_REQ / PAIR_GRANT
        PairReqMsg pr{};
        for (size_t i = 0; i < pr.instance_id.size(); ++i) pr.instance_id[i] = std::byte(i);
        // M4b: the BARE KNOCK — no proof at all. Seeded FIRST because it is the
        // shape the primary association mode actually puts on the wire.
        size_t n = encodePairReq(pr, sp);
        emitPrefixed("messages", 11, std::span<const std::byte>(buf.data(), n));

        pr.has_pin_proof = true;
        for (size_t i = 0; i < pr.pin_proof.size(); ++i) pr.pin_proof[i] = std::byte(0xA0 + i);
        n = encodePairReq(pr, sp);
        emitPrefixed("messages", 11, std::span<const std::byte>(buf.data(), n));

        PairGrantMsg pg{};
        for (size_t i = 0; i < pg.token.size(); ++i) pg.token[i] = std::byte(0x10 + i);
        n = encodePairGrant(pg, sp);
        emitPrefixed("messages", 12, std::span<const std::byte>(buf.data(), n));

        // ...and the M4c-seam form: PAIR_GRANT carrying the hub pubkey.
        pg.has_trust = true;
        pg.trust_map.has_hub_pubkey = true;
        pg.trust_map.hub_pubkey_len = uint8_t(kTrustPubkeyMaxBytes);
        for (size_t i = 0; i < kTrustPubkeyMaxBytes; ++i) pg.trust_map.hub_pubkey[i] = std::byte(0x02 + i);
        n = encodePairGrant(pg, sp);
        emitPrefixed("messages", 12, std::span<const std::byte>(buf.data(), n));
    }
    {  // BLOB_REQ — full and selective repair, both namespaces
        BlobReqMsg b{};
        b.blob.ns = blob_ns::catalog;
        b.full = true;
        size_t n = encodeBlobReq(b, sp);
        emitPrefixed("messages", 13, std::span<const std::byte>(buf.data(), n));

        b.blob.ns = blob_ns::store;
        b.blob.has_store_id = true;
        b.blob.store_id = 1;
        b.blob.has_slot = true;
        b.blob.slot = 4;
        b.blob.has_generation = true;
        b.blob.generation = 9;
        b.full = false;
        b.chunks_count = 4;
        for (uint32_t i = 0; i < 4; ++i) b.chunks[i] = uint16_t(i * 3);
        n = encodeBlobReq(b, sp);
        emitPrefixed("messages", 13, std::span<const std::byte>(buf.data(), n));
    }
    {  // PROBE_REPORT
        ProbeReportMsg p{};
        p.probe_result = {4096, 1000, 250, 12};
        size_t n = encodeProbeReport(p, sp);
        emitPrefixed("messages", 14, std::span<const std::byte>(buf.data(), n));
    }
    {  // M4c: AUTH (selector 15) and HUB_SIG (16) — the RFC-029 trust envelope.
        AuthMsg a{};
        a.trust_map.has_token_proof = true;
        for (size_t i = 0; i < kTrustTokenProofBytes; ++i) a.trust_map.token_proof[i] = std::byte(0x70 + i);
        size_t n = encodeAuth(a, sp);
        emitPrefixed("messages", 15, std::span<const std::byte>(buf.data(), n));
        emitPrefixed("cbor", 0, std::span<const std::byte>(buf.data(), n));

        a.trust_map.has_presentation_mode = true;
        a.trust_map.presentation_mode = 1;
        n = encodeAuth(a, sp);
        emitPrefixed("messages", 15, std::span<const std::byte>(buf.data(), n));

        // A DELIBERATELY OVER-DECORATED AUTH: every trust key at once. Nothing
        // legitimate sends this, which is exactly why the corpus should carry
        // it — the interesting mutations live where an authorization decoder
        // meets fields it was not expecting on this frame.
        a.trust_map.has_client_ver = true;
        a.trust_map.client_ver = "0.0.0-fuzz";
        a.trust_map.has_client_nonce = true;
        for (size_t i = 0; i < a.trust_map.client_nonce.size(); ++i)
            a.trust_map.client_nonce[i] = std::byte(0x11 * i);
        a.trust_map.has_sig_request = true;
        a.trust_map.sig_request = true;
        a.trust_map.has_pairing_modes = true;
        a.trust_map.pairing_modes_mask = 0xFF;
        n = encodeAuth(a, sp);
        emitPrefixed("messages", 15, std::span<const std::byte>(buf.data(), n));

        for (uint8_t len : {uint8_t(64), uint8_t(kTrustSigMaxBytes)}) {
            HubSigMsg h{};
            h.trust_map.has_welcome_sig = true;
            h.trust_map.welcome_sig_len = len;
            for (uint8_t i = 0; i < len; ++i) h.trust_map.welcome_sig[i] = std::byte(uint8_t(0x90 + i));
            n = encodeHubSig(h, sp);
            emitPrefixed("messages", 16, std::span<const std::byte>(buf.data(), n));
        }
    }
    {  // M4c: GRANT carrying `roles` (23) — the AUTH answer, a NEW key on an
       // EXISTING client-side decoder, which is the sneakier of the two kinds
       // of new surface.
        GrantMsg g{};
        g.has_roles = true;
        g.roles = 2;
        g.grants_count = 1;
        g.grants[0] = {0x0080, 30.0f, 1};
        size_t n = encodeGrant(g, sp);
        emitPrefixed("messages", 6, std::span<const std::byte>(buf.data(), n));
        g.grants_count = 0;
        n = encodeGrant(g, sp);
        emitPrefixed("messages", 6, std::span<const std::byte>(buf.data(), n));
    }
}

// ---- Bundles / blobs / frames / packed --------------------------------------

static void seedBundles() {
    for (size_t S : {size_t(4), size_t(6), size_t(8)}) {
        for (uint8_t n : {uint8_t(1), uint8_t(8), uint8_t(limits::bundle_max_samples)}) {
            std::array<std::byte, 1024> buf{};
            BundleWriter w(std::span<std::byte>(buf), 0x11223344u, S);
            std::array<std::byte, 32> sample{};
            for (uint8_t i = 0; i < n; ++i) {
                for (size_t k = 0; k < S; ++k) sample[k] = std::byte(i * 7 + k);
                if (!w.addSample(uint16_t(i * 600), std::span<const std::byte>(sample.data(), S))) break;
            }
            size_t sz = w.finalize();
            if (sz == 0) continue;
            // fuzz_bundle.cc's first byte is S.
            emitPrefixed("bundle", uint8_t(S), std::span<const std::byte>(buf.data(), sz));
        }
    }
}

static void seedBlobs() {
    // A real transfer: chunk the frozen mini-catalog's encoding.
    static Catalog32 mini;
    conformance::buildMiniCatalog(mini);
    size_t n = encodeCatalog(mini, std::span<std::byte>(g_buf));
    std::span<const std::byte> encoded(g_buf.data(), n);

    BlobId id{};
    id.ns = blob_ns::catalog;
    size_t cc = chunkCount(n);

    // fuzz_blob.cc's driver reads: ns, store_id, slot, generation(2),
    // chunk_count(2), total(4), then (dt:2, len:2, bytes...)* — build one
    // well-formed replay of the whole transfer.
    std::vector<std::byte> seed;
    auto push8 = [&](uint8_t v) { seed.push_back(std::byte(v)); };
    auto push16 = [&](uint16_t v) { push8(uint8_t(v)); push8(uint8_t(v >> 8)); };
    auto push32 = [&](uint32_t v) { push16(uint16_t(v)); push16(uint16_t(v >> 16)); };

    push8(id.ns); push8(0); push8(0); push16(0);
    push16(uint16_t(cc)); push32(uint32_t(n));
    for (size_t i = 0; i < cc; ++i) {
        std::array<std::byte, 256> chunk{};
        size_t len = fillBlobChunk(id, encoded, uint16_t(i), std::span<std::byte>(chunk));
        if (len == 0) break;
        push16(1);                    // dt ms
        push16(uint16_t(len));        // payload length
        seed.insert(seed.end(), chunk.begin(), chunk.begin() + len);
    }
    emit("blob", std::span<const std::byte>(seed.data(), seed.size()));

    // A bare chunk payload on its own — the stateless-header path.
    std::array<std::byte, 256> chunk{};
    size_t len = fillBlobChunk(id, encoded, 0, std::span<std::byte>(chunk));
    emit("blob", std::span<const std::byte>(chunk.data(), len));
}

static void seedFrames() {
    // A fragmented control frame, replayed in fuzz_frame.cc's driver format:
    // (type, flags, seq:2, channel:2, dt:2, len:2, bytes...)*
    static Catalog32 mini;
    conformance::buildMiniCatalog(mini);
    size_t n = encodeCatalog(mini, std::span<std::byte>(g_buf));

    std::array<std::byte, kFrameBufferCapacity> whole{};
    FrameHeader h{};
    h.type = 0x11;
    h.channel = 0x0002;
    h.seq = 5;
    size_t payloadLen = n < (kFrameBufferCapacity - kHeaderBytes) ? n : (kFrameBufferCapacity - kHeaderBytes);
    h.len = uint16_t(payloadLen);
    encodeFrameHeader(h, std::span<std::byte>(whole));
    std::memcpy(whole.data() + kHeaderBytes, g_buf.data(), payloadLen);

    std::vector<std::byte> seed;
    auto push8 = [&](uint8_t v) { seed.push_back(std::byte(v)); };
    auto push16 = [&](uint16_t v) { push8(uint8_t(v)); push8(uint8_t(v >> 8)); };

    fragmentFrame(std::span<const std::byte>(whole.data(), kHeaderBytes + payloadLen), 242,
                  [&](std::span<const std::byte> frag) {
                      auto fh = decodeFrameHeader(frag);
                      if (!fh) return;
                      push8(fh->type);
                      push8(fh->flags);
                      push16(fh->seq);
                      push16(fh->channel);
                      push16(1);  // dt
                      auto body = frag.subspan(kHeaderBytes);
                      push16(uint16_t(body.size()));
                      seed.insert(seed.end(), body.begin(), body.end());
                  });
    emit("frame", std::span<const std::byte>(seed.data(), seed.size()));

    // Raw single frames: ESTOP, clock, beacon, ackmask, probe.
    {
        std::array<std::byte, 32> b{};
        size_t k = encodeEstop({1, 2, 3}, std::span<std::byte>(b));
        emit("frame", std::span<const std::byte>(b.data(), k));
        k = encodeClockRequest(0x01020304, std::span<std::byte>(b));
        emit("frame", std::span<const std::byte>(b.data(), k));
        k = encodeClockReply(1, 2, 3, std::span<std::byte>(b));
        emit("frame", std::span<const std::byte>(b.data(), k));
        BeaconFrame bf{};
        bf.boot_id = 9;
        bf.pairing_open = true;
        k = encodeBeacon(bf, std::span<std::byte>(b));
        emit("frame", std::span<const std::byte>(b.data(), k));
        k = encodeAckMask({7, 0xF0F0F0F0}, std::span<std::byte>(b));
        emit("frame", std::span<const std::byte>(b.data(), k));
    }
}

static void seedPacked() {
    // fuzz_packed.cc's first byte selects the catalog+entry; the rest is the
    // packed payload. Emit one well-formed payload per layout entry of both
    // catalogs the target builds (mini = selector<0x80, strings = >=0x80).
    static Catalog32 mini;
    conformance::buildMiniCatalog(mini);
    for (uint16_t i = 0; i < mini.count; ++i) {
        const CatalogEntry& e = mini.entries[i];
        if (!e.usesLayout()) continue;
        auto fields = mini.layoutFields(e);
        std::array<float, CatalogEntry::kMaxFields> vals{};
        for (size_t k = 0; k < fields.size(); ++k) vals[k] = float(k) * 1.25f;
        std::array<std::byte, 512> out{};
        size_t n = encodeByLayout(fields, std::span<const float>(vals.data(), fields.size()),
                                  std::span<std::byte>(out));
        if (n == 0) continue;
        emitPrefixed("packed", uint8_t(i), std::span<const std::byte>(out.data(), n));
        // Longer-than-declared: the §5.4 append-only prefix-parse case.
        std::array<std::byte, 512> grown{};
        std::memcpy(grown.data(), out.data(), n);
        for (size_t k = n; k < n + 16 && k < grown.size(); ++k) grown[k] = std::byte(0xAB);
        emitPrefixed("packed", uint8_t(i), std::span<const std::byte>(grown.data(), n + 16));
    }
    // A string-field payload for the RFC-026 catalog the target builds
    // (str16 + u8 + str32 + i16 + str64 + f32 = 16+1+32+2+64+4 = 119 bytes).
    {
        std::array<std::byte, 119> s{};
        const char* a = "roster-entry";
        std::memcpy(s.data(), a, std::strlen(a));
        s[16] = std::byte(3);
        const char* b = "mfp-slopsync-plugin";
        std::memcpy(s.data() + 17, b, std::strlen(b));
        const char* c = "granted control at 50 Hz";
        std::memcpy(s.data() + 51, c, std::strlen(c));
        emitPrefixed("packed", 0x80, std::span<const std::byte>(s));
        // Every byte non-NUL: the "string exactly fills the field, no
        // terminator" edge the RFC-026 semantics call out.
        std::array<std::byte, 119> full{};
        for (auto& v : full) v = std::byte('x');
        emitPrefixed("packed", 0x80, std::span<const std::byte>(full));
    }
}

// ---- Regression seeds -------------------------------------------------------
// The minimized inputs that crashed the library in this gate's first pass.
// They live in the corpus so CI re-executes them on every run (`-runs=0`
// replay is the cheap, deterministic half of the gate), and they live HERE
// rather than as opaque committed blobs so the bytes are readable and the
// reason is written down next to them. See test/fuzz/README and the
// matching doctest cases named "RFC-028: ...".
static void seedRegressions() {
    // #1 CborReader::readTstr/readBstr length-check integer overflow.
    // 7B FF*8 = tstr claiming 2^64-1 bytes; `start + len` wrapped past the
    // bound check and yielded a 2^64-1-byte view into a 9-byte buffer.
    // Emitted for both fuzz_cbor modes and as an INTENT-shaped message body,
    // since every message decoder reaches readTstr.
    {
        const std::array<std::byte, 9> evilT{
            std::byte{0x7B}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
            std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
        const std::array<std::byte, 9> evilB{
            std::byte{0x5B}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
            std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
        // fuzz_cbor mode 1 (typed reads) splits the remainder in half: CBOR
        // bytes first, then the op selector stream. 0x07 % 10 == 7 == readTstr.
        for (const auto& evil : {evilT, evilB}) {
            std::vector<std::byte> v;
            v.push_back(std::byte{0x01});  // odd mode -> walkTyped
            v.insert(v.end(), evil.begin(), evil.end());
            for (int i = 0; i < 9; ++i) v.push_back(std::byte{0x07});
            emit("cbor", std::span<const std::byte>(v.data(), v.size()));
            emitPrefixed("cbor", 0, std::span<const std::byte>(evil));
        }
        // The same head buried where a decoder's unknown-key skipValue() will
        // hit it: a 1-pair map whose value is the oversized string.
        std::array<std::byte, 11> viaMap{std::byte{0xA1}, std::byte{0x18}, std::byte{0xFE},
                                         std::byte{0x7B}, std::byte{0xFF}, std::byte{0xFF},
                                         std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                                         std::byte{0xFF}, std::byte{0xFF}};
        emitPrefixed("cbor", 0, std::span<const std::byte>(viaMap));
        for (uint8_t sel = 0; sel < 15; ++sel) {
            emitPrefixed("messages", sel, std::span<const std::byte>(viaMap));
        }
        emit("catalog", std::span<const std::byte>(viaMap));
    }

    // #3 Reassembler::accept unbounded memcpy into pendingLastBytes.
    // fuzz_frame's replay format: type, flags, seq:2, channel:2, dt:2, len:2,
    // payload. flags=0 (last fragment) + index 0 + a slice far over
    // kMaxSlotPayload(504). NOTE the length: the overflow is INTRA-OBJECT and
    // ASan only reports it once the write leaves the whole Reassembler, so a
    // ~600-byte reproducer looks clean. This one is 4000.
    {
        std::vector<std::byte> v{std::byte{0x11}, std::byte{0x00}, std::byte{0x00},
                                 std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                 std::byte{0x00}, std::byte{0x00},
                                 std::byte{0xA0}, std::byte{0x0F},   // len = 4000
                                 std::byte{0x00}, std::byte{0x00}};  // frag_index = 0
        for (size_t i = 0; i < 3998; ++i) v.push_back(std::byte('A'));
        emit("frame", std::span<const std::byte>(v.data(), v.size()));
    }

    // #2 ChunkReassembler kept a REFUSED transfer's attacker-chosen sizes.
    // fuzz_blob's driver reads: ns, store_id, slot, generation:2,
    // chunk_count:2, total_bytes:4 — then calls missingIndices() immediately,
    // which is where the OOB read happened.
    {
        const std::array<std::byte, 11> v{
            std::byte{0x00},                                          // ns = catalog
            std::byte{0x00}, std::byte{0x00},                          // store_id, slot
            std::byte{0x00}, std::byte{0x00},                          // generation
            std::byte{0xFF}, std::byte{0xFF},                          // chunk_count = 65535
            std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};  // total = 4G
        emit("blob", std::span<const std::byte>(v));
    }
}

int main(int argc, char** argv) {
    g_root = (argc > 1) ? argv[1] : "corpus";
    ::mkdir(g_root.c_str(), 0755);
    seedRegressions();
    seedCatalogs();
    seedMessages();
    seedBundles();
    seedBlobs();
    seedFrames();
    seedPacked();
    std::printf("seeds written under %s\n", g_root.c_str());
    return 0;
}
