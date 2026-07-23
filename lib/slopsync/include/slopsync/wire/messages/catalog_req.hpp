// slopsync-core — CATALOG_REQ (client -> hub), SPEC §8.4.
//
// CBOR map: an EMPTY map means "send everything" (full transfer); a map
// carrying `chunks` (27) => [indices] means "selective repair" of exactly
// those CATALOG_CHUNK indices. There is no other key in this message.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"

namespace slopsync {

inline constexpr uint32_t kCatalogReqMaxChunks = 32;  // brief-mandated fixed cap

struct CatalogReqMsg {
    // true <=> full-transfer request (empty map on the wire); false <=>
    // selective repair, `chunks[0..chunks_count)` names the wanted indices.
    bool full = true;
    uint32_t chunks_count = 0;
    std::array<uint16_t, kCatalogReqMaxChunks> chunks{};
};

// Encodes into `out`; returns bytes written, or 0 on failure: too many
// indices, an inconsistent full+chunks_count>0 combination, or `out` too
// small.
inline size_t encodeCatalogReq(const CatalogReqMsg& m, std::span<std::byte> out) {
    if (m.chunks_count > kCatalogReqMaxChunks) return 0;
    if (m.full && m.chunks_count > 0) return 0;  // ambiguous input, refuse

    CborWriter w(out);
    if (m.full) {
        w.mapHeader(0);
    } else {
        w.mapHeader(1);
        w.key(CborKey::chunks).arrayHeader(m.chunks_count);
        for (uint32_t i = 0; i < m.chunks_count; ++i) w.uintVal(m.chunks[i]);
    }
    return w.size();
}

// Decodes `in` into a CatalogReqMsg. Unknown keys are skipped per §4.3 (this
// message currently has only one key, but tolerance is still exercised).
inline Result<CatalogReqMsg, DecodeError> decodeCatalogReq(std::span<const std::byte> in) {
    using Ret = Result<CatalogReqMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    CatalogReqMsg m{};
    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        if (kR.value() == uint64_t(CborKey::chunks)) {
            auto aR = r.readArrayHeader();
            if (!aR) return Ret::err(aR.error());
            if (aR.value() > kCatalogReqMaxChunks) return Ret::err(DecodeError::CapacityExceeded);
            for (uint32_t j = 0; j < aR.value(); ++j) {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFFFF) return Ret::err(DecodeError::Malformed);
                m.chunks[j] = uint16_t(v.value());
            }
            m.chunks_count = aR.value();
            m.full = false;
        } else {
            auto sv = r.skipValue();  // §4.3
            if (!sv) return Ret::err(sv.error());
        }
    }
    return Ret::ok(m);
}

}  // namespace slopsync
