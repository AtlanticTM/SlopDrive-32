// fuzz_messages — target: every control-plane CBOR message decoder, BOTH
// directions.
//
// Hub-side (untrusted CLIENT bytes): HELLO, INTENT, SUBSCRIBE, UNSUBSCRIBE,
// PUBLISH, PAIR_REQ, AUTH, BLOB_REQ, GOODBYE, PROBE_REPORT.
// Client-side (untrusted HUB bytes, RFC-028 §5 symmetry): WELCOME, GRANT,
// ECHO, EVENT (including its `body` sub-map), NACK, PAIR_GRANT, HUB_SIG.
//
// One binary with a leading selector byte rather than fifteen: the decoders
// share the CborReader substrate, the seeds carry their own selector prefix,
// and adding a sixteenth message is one line. Every decoder's zero-copy
// string_view/span outputs are TOUCHED — a view that escaped the input buffer
// is only a finding if something reads it.
#include "fuzz_common.hpp"

using namespace slopsync;
using namespace slopfuzz;

// Selector values are STABLE — the committed seed corpus encodes them, so
// renumbering silently invalidates the corpus. Append only.
enum : uint8_t {
    kHello = 0, kWelcome, kIntent, kSubscribe, kUnsubscribe, kPublish, kGrant,
    kEcho, kEvent, kNack, kGoodbye, kPairReq, kPairGrant, kBlobReq, kProbeReport,
    // M4c (RFC-029): AUTH (0x1C, c2h) and HUB_SIG (0x1D, h2c). Appended, never
    // renumbered — and note that appending only preserves the committed corpus
    // because gen_seeds emits selector bytes in [0, kMsgCount), so `sel %
    // kMsgCount` is identity for every seed already on disk.
    kAuth, kHubSig,
    kMsgCount
};

// The scoped `trust` (39) sub-map is NEW UNTRUSTED SURFACE as of M4b, and it
// rides HELLO (hub-side), WELCOME (client-side) and PAIR_GRANT (client-side) —
// i.e. it is reachable in BOTH directions, which is exactly the symmetry
// RFC-028.5 makes normative. Its bounded members are the interesting part: a
// length that escaped its cap is only a finding if something READS it, so every
// one of them is touched here.
static void checkTrust(const TrustMap& t, std::span<const std::byte> in) {
    if (t.client_ver.size() > limits::client_ver_max_bytes) __builtin_trap();
    if (t.client_ver.size() > in.size()) __builtin_trap();
    if (t.hub_pubkey_len > kTrustPubkeyMaxBytes) __builtin_trap();
    if (t.welcome_sig_len > kTrustSigMaxBytes) __builtin_trap();
    sink(touch(t.client_ver));
    sink(touch(std::span<const std::byte>(t.client_nonce)));
    sink(touch(std::span<const std::byte>(t.token_proof)));
    sink(touch(std::span<const std::byte>(t.hub_pubkey.data(), t.hub_pubkey_len)));
    sink(touch(std::span<const std::byte>(t.welcome_sig.data(), t.welcome_sig_len)));
    sink(uint32_t(t.presentation_mode) ^ uint32_t(t.pairing_modes_mask) ^ uint32_t(t.sig_request));
}

static void checkValue(const IntentValue& v, std::span<const std::byte> in) {
    switch (v.kind) {
        case IntentValue::Kind::Tstr:
            if (v.tstr_val.size() > in.size()) __builtin_trap();
            sink(touch(v.tstr_val));
            break;
        case IntentValue::Kind::Bstr:
            if (v.bstr_val.size() > in.size()) __builtin_trap();
            sink(touch(v.bstr_val));
            break;
        default:
            sink(uint32_t(v.u64_val) ^ uint32_t(v.i64_val) ^ uint32_t(v.bool_val));
            break;
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    const uint8_t sel = data[0] % kMsgCount;
    auto in = asBytes(data + 1, size - 1);

    switch (sel) {
        case kHello: {
            auto r = decodeHello(in);
            if (!r) break;
            const HelloMsg& m = r.value();
            if (m.client_kind.size() > kHelloMaxClientKindBytes) __builtin_trap();
            if (m.client_name.size() > kHelloMaxClientNameBytes) __builtin_trap();
            if (m.subscriptions_count > kHelloMaxSubscriptionWishes) __builtin_trap();
            if (m.publishes_count > kHelloMaxPublishWishes) __builtin_trap();
            sink(touch(m.client_kind));
            sink(touch(m.client_name));
            sink(touch(std::span<const std::byte>(m.instance_id)));
            sink(touch(std::span<const std::byte>(m.token)));
            sink(touch(std::span<const std::byte>(m.catalog_etag)));
            for (uint32_t i = 0; i < m.subscriptions_count; ++i) sink(m.subscriptions[i].channel_id);
            for (uint32_t i = 0; i < m.publishes_count; ++i) sink(m.publishes[i].channel_id);
            if (m.has_trust) checkTrust(m.trust_map, in);
            break;
        }
        case kWelcome: {
            auto r = decodeWelcome(in);
            if (!r) break;
            const WelcomeMsg& m = r.value();
            sink(touch(std::span<const std::byte>(m.catalog_etag)));
            sink(touch(std::span<const std::byte>(m.nonce)));
            if (m.grants_count > m.grants.size()) __builtin_trap();
            for (uint32_t i = 0; i < m.grants_count; ++i) sink(m.grants[i].channel_id);
            if (m.granted_publishes_count > m.granted_publishes.size()) __builtin_trap();
            for (uint32_t i = 0; i < m.granted_publishes_count; ++i) sink(m.granted_publishes[i].channel_id);
            if (m.has_trust) checkTrust(m.trust_map, in);
            break;
        }
        case kIntent: {
            auto r = decodeIntent(in);
            if (!r) break;
            const IntentMsg& m = r.value();
            if (m.value_count > kIntentMaxValueFields) __builtin_trap();
            for (uint32_t i = 0; i < m.value_count; ++i) checkValue(m.value[i].value, in);
            break;
        }
        case kSubscribe: {
            auto r = decodeSubscribe(in);
            if (!r) break;
            if (r.value().subscriptions_count > kSubscribeMaxWishes) __builtin_trap();
            for (uint32_t i = 0; i < r.value().subscriptions_count; ++i) {
                sink(r.value().subscriptions[i].channel_id);
            }
            break;
        }
        case kUnsubscribe: {
            auto r = decodeUnsubscribe(in);
            if (!r) break;
            if (r.value().channel_count > kUnsubscribeMaxChannels) __builtin_trap();
            for (uint32_t i = 0; i < r.value().channel_count; ++i) sink(r.value().channel_ids[i]);
            break;
        }
        case kPublish: {
            auto r = decodePublish(in);
            if (!r) break;
            if (r.value().publishes_count > kPublishMaxWishes) __builtin_trap();
            for (uint32_t i = 0; i < r.value().publishes_count; ++i) sink(r.value().publishes[i].channel_id);
            break;
        }
        case kGrant: {
            auto r = decodeGrant(in);
            if (!r) break;
            if (r.value().grants_count > r.value().grants.size()) __builtin_trap();
            for (uint32_t i = 0; i < r.value().grants_count; ++i) sink(r.value().grants[i].channel_id);
            if (r.value().granted_publishes_count > r.value().granted_publishes.size()) __builtin_trap();
            for (uint32_t i = 0; i < r.value().granted_publishes_count; ++i) {
                sink(r.value().granted_publishes[i].channel_id);
            }
            break;
        }
        case kEcho: {
            auto r = decodeEcho(in);
            if (!r) break;
            if (r.value().applied_count > kIntentMaxValueFields) __builtin_trap();
            for (uint32_t i = 0; i < r.value().applied_count; ++i) checkValue(r.value().applied[i].value, in);
            break;
        }
        case kEvent: {
            auto r = decodeEvent(in);
            if (!r) break;
            const EventMsg& m = r.value();
            if (m.body_count > kIntentMaxValueFields) __builtin_trap();
            for (uint32_t i = 0; i < m.body_count; ++i) checkValue(m.body[i].value, in);
            break;
        }
        case kNack: {
            auto r = decodeNack(in);
            if (!r) break;
            if (r.value().detail.size() > kNackMaxDetailBytes) __builtin_trap();
            sink(touch(r.value().detail));
            break;
        }
        case kGoodbye: {
            auto r = decodeGoodbye(in);
            if (!r) break;
            sink(touch(r.value().detail));
            break;
        }
        case kPairReq: {
            auto r = decodePairReq(in);
            if (!r) break;
            sink(touch(std::span<const std::byte>(r.value().instance_id)));
            // M4b: `pin_proof` is OPTIONAL now — its absence is the RFC-027
            // mode (a)/(c) KNOCK. So the proof bytes are meaningful only when
            // the flag says so, and reading them unconditionally would be
            // exactly the uninitialized-read class this target exists to catch.
            sink(uint32_t(r.value().has_pin_proof));
            if (r.value().has_pin_proof) sink(touch(std::span<const std::byte>(r.value().pin_proof)));
            break;
        }
        case kPairGrant: {
            auto r = decodePairGrant(in);
            if (!r) break;
            sink(touch(std::span<const std::byte>(r.value().token)));
            if (r.value().has_trust) checkTrust(r.value().trust_map, in);
            break;
        }
        case kBlobReq: {
            auto r = decodeBlobReq(in);
            if (!r) break;
            if (r.value().chunks_count > kBlobReqMaxChunks) __builtin_trap();
            for (uint32_t i = 0; i < r.value().chunks_count; ++i) sink(r.value().chunks[i]);
            break;
        }
        case kAuth: {
            // The AUTH decoder is the newest UNTRUSTED c2h surface in the
            // protocol and the one that gates an authorization decision, so it
            // gets the same treatment as HELLO: decode, cap-check, and TOUCH
            // every bounded member, because a length that escaped its cap is
            // only a finding if something reads it.
            auto r = decodeAuth(in);
            if (!r) break;
            checkTrust(r.value().trust_map, in);
            break;
        }
        case kHubSig: {
            // Same envelope, opposite direction — RFC-028.5's symmetry rule
            // says a client parsing hub bytes is exactly as exposed as a hub
            // parsing client bytes, so both are hammered here.
            auto r = decodeHubSig(in);
            if (!r) break;
            checkTrust(r.value().trust_map, in);
            break;
        }
        case kProbeReport: {
            auto r = decodeProbeReport(in);
            if (!r) break;
            sink(r.value().probe_result.rtt_ms ^ r.value().probe_result.bytes_received);
            break;
        }
        default: break;
    }
    return 0;
}
