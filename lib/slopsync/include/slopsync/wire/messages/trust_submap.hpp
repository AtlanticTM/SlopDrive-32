// slopsync-core — the scoped `trust` (CBOR key 39) SUB-MAP, shared verbatim by
// HELLO, WELCOME, AUTH and PAIR_GRANT. Sub-keys come from the registry's
// `trust_keys` table, NOT from the global cbor_keys space.
//
// WHY A SUB-MAP AT ALL (feasibility-pass conservation rule, binding): per-
// feature keys do not each get a global key. RFC-029 alone wanted eight, which
// would have burned a third of the remaining 1-63 space on one feature. It gets
// ONE global key and its own tiny interior instead — the same shape `limits`
// (22) and `probe_result` (26) already had. This is what kept the whole v1.0
// base pass at ~7 new global keys rather than ~18.
//
// ABSENT IS THE POTATO PATH. A client that omits the entire map gets exactly
// the v1-draft handshake: bearer token, no crypto, no extra bytes. Everything
// in here is optional, and the mandatory client floor is UNCHANGED — same
// parser, zero required crypto, 24 bytes of stored identity. A coin-cell remote
// never encodes this map and loses nothing but the tripwire it could not have
// fed anyway.
//
// M4b IMPLEMENTS: client_ver (1) — the RFC-029 item-2 change tripwire — and
// pairing_modes (8), the hub's advertisement of which association ceremonies it
// is offering right now. client_nonce (2), sig_request (3) and
// presentation_mode (7) are CARRIED (decoded, stored, echoed) but not acted on;
// hub_pubkey (4), welcome_sig (5) and token_proof (6) are carried purely as the
// M4c seam. Carrying them now is deliberate: it means the decoder that a fuzzer
// hammers is the final one, and M4c adds behavior rather than wire surface.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"

namespace slopsync {

// SEC1-compressed P-256 is 33 bytes; a raw r||s ECDSA signature is 64 and a DER
// one is at most 72. Sized for the largest legal form so M4c never has to move
// the struct, and BOUNDED so RFC-028.2's "length fields are never trusted"
// holds at the parse, not later.
inline constexpr size_t kTrustPubkeyMaxBytes = 33;
inline constexpr size_t kTrustSigMaxBytes = 72;
inline constexpr size_t kTrustTokenProofBytes = 16;
inline constexpr size_t kTrustClientNonceBytes = 8;

struct TrustMap {
    // ---- HELLO --------------------------------------------------------------
    bool has_client_ver = false;
    std::string_view client_ver{};  // zero-copy view into the decoded frame
    bool has_client_nonce = false;
    std::array<std::byte, kTrustClientNonceBytes> client_nonce{};
    bool has_sig_request = false;
    bool sig_request = false;
    // ---- HELLO / AUTH -------------------------------------------------------
    bool has_presentation_mode = false;
    uint8_t presentation_mode = 0;
    bool has_token_proof = false;
    std::array<std::byte, kTrustTokenProofBytes> token_proof{};
    // ---- PAIR_GRANT (M4c) ---------------------------------------------------
    bool has_hub_pubkey = false;
    uint8_t hub_pubkey_len = 0;
    std::array<std::byte, kTrustPubkeyMaxBytes> hub_pubkey{};
    // ---- WELCOME ------------------------------------------------------------
    bool has_welcome_sig = false;
    uint8_t welcome_sig_len = 0;
    std::array<std::byte, kTrustSigMaxBytes> welcome_sig{};
    bool has_pairing_modes = false;
    uint8_t pairing_modes_mask = 0;

    bool any() const {
        return has_client_ver || has_client_nonce || has_sig_request || has_presentation_mode ||
               has_token_proof || has_hub_pubkey || has_welcome_sig || has_pairing_modes;
    }
    uint32_t keyCount() const {
        return uint32_t(has_client_ver) + uint32_t(has_client_nonce) + uint32_t(has_sig_request) +
               uint32_t(has_hub_pubkey) + uint32_t(has_welcome_sig) + uint32_t(has_token_proof) +
               uint32_t(has_presentation_mode) + uint32_t(has_pairing_modes);
    }
};

// Writes `39: { ... }` into an already-open map. The caller counted this as ONE
// key of the enclosing map and must call it in the enclosing map's ascending
// order (39 is the highest key any message that carries it uses today).
// Interior keys are emitted ascending 1..8, which the writer also enforces.
inline void encodeTrustMap(CborWriter& w, const TrustMap& t) {
    w.key(CborKey::trust).mapHeader(t.keyCount());
    if (t.has_client_ver) w.key(uint64_t(trust::client_ver)).tstrVal(t.client_ver);
    if (t.has_client_nonce)
        w.key(uint64_t(trust::client_nonce)).bstrVal(std::span<const std::byte>(t.client_nonce));
    if (t.has_sig_request) w.key(uint64_t(trust::sig_request)).boolVal(t.sig_request);
    if (t.has_hub_pubkey)
        w.key(uint64_t(trust::hub_pubkey))
            .bstrVal(std::span<const std::byte>(t.hub_pubkey.data(), t.hub_pubkey_len));
    if (t.has_welcome_sig)
        w.key(uint64_t(trust::welcome_sig))
            .bstrVal(std::span<const std::byte>(t.welcome_sig.data(), t.welcome_sig_len));
    if (t.has_token_proof)
        w.key(uint64_t(trust::token_proof)).bstrVal(std::span<const std::byte>(t.token_proof));
    if (t.has_presentation_mode) w.key(uint64_t(trust::presentation_mode)).uintVal(t.presentation_mode);
    if (t.has_pairing_modes) w.key(uint64_t(trust::pairing_modes)).uintVal(t.pairing_modes_mask);
}

// Reads the VALUE of key 39 (the reader is positioned at the sub-map). Every
// bounded field is REJECTED when over-cap rather than truncated — RFC-028.2:
// "reject, never truncate-and-continue, on structural payloads". A `trust` map
// is structural (it decides authorization), so an over-long client_ver is a
// malformed frame, not a long name.
inline Result<bool, DecodeError> decodeTrustMap(CborReader& r, TrustMap& t) {
    using Ret = Result<bool, DecodeError>;
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());
    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case trust::client_ver: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() > limits::client_ver_max_bytes) return Ret::err(DecodeError::CapacityExceeded);
                t.client_ver = v.value();
                t.has_client_ver = true;
                break;
            }
            case trust::client_nonce: {
                auto v = r.readBstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() != t.client_nonce.size()) return Ret::err(DecodeError::Malformed);
                std::memcpy(t.client_nonce.data(), v.value().data(), t.client_nonce.size());
                t.has_client_nonce = true;
                break;
            }
            case trust::sig_request: {
                auto v = r.readBool();
                if (!v) return Ret::err(v.error());
                t.sig_request = v.value();
                t.has_sig_request = true;
                break;
            }
            case trust::hub_pubkey: {
                auto v = r.readBstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > kTrustPubkeyMaxBytes)
                    return Ret::err(DecodeError::CapacityExceeded);
                t.hub_pubkey_len = uint8_t(v.value().size());
                std::memcpy(t.hub_pubkey.data(), v.value().data(), t.hub_pubkey_len);
                t.has_hub_pubkey = true;
                break;
            }
            case trust::welcome_sig: {
                auto v = r.readBstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > kTrustSigMaxBytes)
                    return Ret::err(DecodeError::CapacityExceeded);
                t.welcome_sig_len = uint8_t(v.value().size());
                std::memcpy(t.welcome_sig.data(), v.value().data(), t.welcome_sig_len);
                t.has_welcome_sig = true;
                break;
            }
            case trust::token_proof: {
                auto v = r.readBstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() != t.token_proof.size()) return Ret::err(DecodeError::Malformed);
                std::memcpy(t.token_proof.data(), v.value().data(), t.token_proof.size());
                t.has_token_proof = true;
                break;
            }
            case trust::presentation_mode: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                t.presentation_mode = uint8_t(v.value());
                t.has_presentation_mode = true;
                break;
            }
            case trust::pairing_modes: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                t.pairing_modes_mask = uint8_t(v.value());
                t.has_pairing_modes = true;
                break;
            }
            default: {
                // §4.3: unknown sub-key -> ignore the pair. Forward compatibility
                // inside the sub-map works exactly as it does outside it, which
                // is the other reason scoped maps were the right shape.
                auto sv = r.skipValue();
                if (!sv) return Ret::err(sv.error());
                break;
            }
        }
    }
    return Ret::ok(true);
}

}  // namespace slopsync
