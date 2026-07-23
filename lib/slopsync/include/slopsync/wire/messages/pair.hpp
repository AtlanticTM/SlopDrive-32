// slopsync-core — PAIR_REQ / PAIR_GRANT, SPEC §12.2: the pairing ceremony
// that turns a viewer into a controller.
//
// PAIR_REQ (client -> hub), CBOR map, keys ascending: instance_id(4),
// pin_proof(28). `pin_proof` = HMAC-SHA256(key = PIN ASCII, message =
// WELCOME's 8-byte nonce) truncated to 16 bytes (§12.2) — this codec only
// carries the 16 bytes, it does not compute the HMAC.
//
// PAIR_GRANT (hub -> client), CBOR map, keys ascending: token(5), roles(23).
// `token` is the random 16-byte value bound to the requester's instance_id;
// wrong proof or a closed window is a NACK `PAIRING_DENIED` instead
// (nack.hpp), never a PAIR_GRANT with an error inside it.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"

namespace slopsync {

// SPEC §12.2: "truncated to 16 bytes" — fixed by the ceremony's own design,
// not a registry-wide limits:: constant like token_bytes/instance_id_bytes.
inline constexpr size_t kPinProofBytes = 16;

struct PairReqMsg {
    std::array<std::byte, limits::instance_id_bytes> instance_id{};
    std::array<std::byte, kPinProofBytes> pin_proof{};
};

struct PairGrantMsg {
    std::array<std::byte, limits::token_bytes> token{};
    uint8_t roles = 0;
};

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodePairReq(const PairReqMsg& m, std::span<std::byte> out) {
    CborWriter w(out);
    w.mapHeader(2);
    w.key(CborKey::instance_id).bstrVal(std::span<const std::byte>(m.instance_id));
    w.key(CborKey::pin_proof).bstrVal(std::span<const std::byte>(m.pin_proof));
    return w.size();
}

// Decodes `in` into a PairReqMsg. Unknown keys are skipped per §4.3.
inline Result<PairReqMsg, DecodeError> decodePairReq(std::span<const std::byte> in) {
    using Ret = Result<PairReqMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    PairReqMsg m{};
    bool gotInstance = false, gotProof = false;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case uint64_t(CborKey::instance_id): {
                auto v = r.readBstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() != m.instance_id.size()) return Ret::err(DecodeError::Malformed);
                std::memcpy(m.instance_id.data(), v.value().data(), m.instance_id.size());
                gotInstance = true;
                break;
            }
            case uint64_t(CborKey::pin_proof): {
                auto v = r.readBstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() != m.pin_proof.size()) return Ret::err(DecodeError::Malformed);
                std::memcpy(m.pin_proof.data(), v.value().data(), m.pin_proof.size());
                gotProof = true;
                break;
            }
            default: {
                // §4.3: unknown map key -> ignore the pair.
                auto sv = r.skipValue();
                if (!sv) return Ret::err(sv.error());
                break;
            }
        }
    }

    if (!(gotInstance && gotProof)) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodePairGrant(const PairGrantMsg& m, std::span<std::byte> out) {
    CborWriter w(out);
    w.mapHeader(2);
    w.key(CborKey::token).bstrVal(std::span<const std::byte>(m.token));
    w.key(CborKey::roles).uintVal(m.roles);
    return w.size();
}

// Decodes `in` into a PairGrantMsg. Unknown keys are skipped per §4.3.
inline Result<PairGrantMsg, DecodeError> decodePairGrant(std::span<const std::byte> in) {
    using Ret = Result<PairGrantMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    PairGrantMsg m{};
    bool gotToken = false, gotRoles = false;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case uint64_t(CborKey::token): {
                auto v = r.readBstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() != m.token.size()) return Ret::err(DecodeError::Malformed);
                std::memcpy(m.token.data(), v.value().data(), m.token.size());
                gotToken = true;
                break;
            }
            case uint64_t(CborKey::roles): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.roles = uint8_t(v.value());
                gotRoles = true;
                break;
            }
            default: {
                // §4.3: unknown map key -> ignore the pair.
                auto sv = r.skipValue();
                if (!sv) return Ret::err(sv.error());
                break;
            }
        }
    }

    if (!(gotToken && gotRoles)) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

}  // namespace slopsync
