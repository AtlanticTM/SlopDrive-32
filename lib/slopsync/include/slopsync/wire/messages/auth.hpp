// slopsync-core — AUTH (0x1C, c2h) and HUB_SIG (0x1D, h2c): the two frames
// whose ENTIRE payload is the scoped `trust` (39) sub-map. RFC-029 items 1
// and 6.
//
// -- AUTH (c2h) --------------------------------------------------------------
// Carries `token_proof` (6) — HMAC-SHA256(key = token, message = the WELCOME
// nonce) truncated to 16 bytes — and optionally `presentation_mode` (7). The
// hub re-issues `roles` (23) in a GRANT on success and NACKs UNAUTHORIZED on
// failure.
//
// WHY A FRAME AND NOT A SECOND HELLO: `roles` is fixed by WELCOME and the
// session grammar has no "upgrade role mid-session" verb. A second HELLO would
// self-evict via §6.3's duplicate-instance rule, i.e. the client would kill its
// own session trying to authenticate it.
//
// WHAT AUTH DELIBERATELY DOES NOT CARRY: an `instance_id`. The session already
// knows who it is (it was fixed by HELLO and is what the ledger lookup keys
// on). Letting AUTH name an identity would let a live session present a proof
// AS SOMEBODY ELSE, which is a whole authentication bypass in one optional
// field. The omission is the security property.
//
// -- HUB_SIG (h2c) -----------------------------------------------------------
// Carries `welcome_sig` (5) — the deferred half of RFC-029 item 1. Identical
// signature material, identical client handling, and identical meaning to a
// `welcome_sig` that arrived inline in WELCOME; the ONLY difference is WHEN.
// See the frame_types note in registry.yaml for the latency reasoning that
// makes deferral the S3's path and inline the fast-signer's path.
//
// -- WHY ONE FILE ------------------------------------------------------------
// Both are the same envelope in opposite directions, and RFC-028.5 makes
// symmetric parser totality normative — keeping them adjacent is how the
// c2h/h2c pair stays impossible to harden on one side only. The decode surface
// they add is ONE map header plus decodeTrustMap(), which the fuzzer already
// hammers through HELLO, WELCOME and PAIR_GRANT.
#pragma once

#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"
#include "slopsync/wire/messages/trust_submap.hpp"

namespace slopsync {

// The 16 bytes a hub signs and a client verifies (RFC-029 item 1, as amended
// by the feasibility pass's replay fix): client_nonce(8) || session_id(u32 LE)
// || boot_id(u32 LE).
//
// THE CLIENT NONCE IS THE WHOLE POINT. The original design had the hub sign
// its own WELCOME nonce, which contains zero client entropy — so an evil twin
// that captured ONE handshake could replay {nonce, signature} verbatim at every
// future victim and pass verification. With the client's own fresh entropy in
// the signed string, a captured signature is worth exactly one session that
// already happened. `session_id` and `boot_id` are belt-and-braces: they make
// the material differ even against a client with a broken RNG that reuses its
// nonce, and they bind the signature to THIS session on THIS boot rather than
// to a machine in general.
inline constexpr size_t kHubSigMaterialBytes = kTrustClientNonceBytes + 4 + 4;

inline std::array<std::byte, kHubSigMaterialBytes> hubSigMaterial(
    std::span<const std::byte, kTrustClientNonceBytes> client_nonce, uint32_t session_id, uint32_t boot_id) {
    std::array<std::byte, kHubSigMaterialBytes> out{};
    for (size_t i = 0; i < kTrustClientNonceBytes; ++i) out[i] = client_nonce[i];
    // Little-endian, matching every other multi-byte integer this protocol puts
    // on the wire outside CBOR (§5.2's packed layouts). Stated explicitly
    // because a signature is the one place where a byte-order disagreement
    // between two independent implementations reads as "forgery" rather than
    // as a wrong number.
    for (size_t i = 0; i < 4; ++i) out[kTrustClientNonceBytes + i] = std::byte((session_id >> (8 * i)) & 0xFF);
    for (size_t i = 0; i < 4; ++i) out[kTrustClientNonceBytes + 4 + i] = std::byte((boot_id >> (8 * i)) & 0xFF);
    return out;
}

// ---------------------------------------------------------------------------
// The shared envelope: a one-key CBOR map `{39: <trust>}`.
// ---------------------------------------------------------------------------
struct TrustEnvelopeMsg {
    TrustMap trust_map{};
};

using AuthMsg = TrustEnvelopeMsg;     // 0x1C, c2h — token_proof / presentation_mode
using HubSigMsg = TrustEnvelopeMsg;   // 0x1D, h2c — welcome_sig

inline size_t encodeTrustEnvelope(const TrustEnvelopeMsg& m, std::span<std::byte> out) {
    if (!m.trust_map.any()) return 0;  // an empty envelope says nothing; never emit one
    CborWriter w(out);
    w.mapHeader(1);
    encodeTrustMap(w, m.trust_map);
    return w.size();
}

inline Result<TrustEnvelopeMsg, DecodeError> decodeTrustEnvelope(std::span<const std::byte> in) {
    using Ret = Result<TrustEnvelopeMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    TrustEnvelopeMsg m{};
    bool gotTrust = false;
    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        if (kR.value() == uint64_t(CborKey::trust)) {
            auto tR = decodeTrustMap(r, m.trust_map);
            if (!tR) return Ret::err(tR.error());
            gotTrust = true;
        } else {
            // §4.3: unknown map key -> ignore the pair.
            auto sv = r.skipValue();
            if (!sv) return Ret::err(sv.error());
        }
    }
    // The `trust` map is the entire message; a frame without it is malformed
    // rather than "an AUTH that means nothing", because tolerating it would
    // make an empty AUTH a free way to burn an attempt counter.
    if (!gotTrust) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

inline size_t encodeAuth(const AuthMsg& m, std::span<std::byte> out) { return encodeTrustEnvelope(m, out); }
inline Result<AuthMsg, DecodeError> decodeAuth(std::span<const std::byte> in) { return decodeTrustEnvelope(in); }
inline size_t encodeHubSig(const HubSigMsg& m, std::span<std::byte> out) { return encodeTrustEnvelope(m, out); }
inline Result<HubSigMsg, DecodeError> decodeHubSig(std::span<const std::byte> in) { return decodeTrustEnvelope(in); }

}  // namespace slopsync
