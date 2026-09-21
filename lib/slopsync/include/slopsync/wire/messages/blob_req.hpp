// slopsync-core — BLOB_REQ (client -> hub), SPEC §8.4 / RFC-021.
//
// THE transfer verb for the whole protocol, not a catalog thing with a store
// bolted on. A request names WHICH blob via the scoped `blob` (38) sub-map
// (sub-keys from the registry's `blob_keys`) and, optionally, WHICH CHUNKS of
// it via the top-level `chunks` (27) array:
//
//   {}                                  -> full transfer of blob namespace 0
//                                          (the catalog: `ns` defaults to
//                                          blob_ns::catalog, store_id/slot
//                                          absent by rule)
//   {38: {1: 1, 2: 3, 3: 7}}            -> full transfer of store 3, slot 7
//   {27: [4, 9], 38: {1: 1, 2: 3, 3: 7}}-> repair chunks 4 and 9 of that item
//
// `chunks` present WITH no blob map is still legal (repair of the catalog).
// What is MALFORMED (RFC-022.6) is `chunks` present as an EMPTY array — a
// repair request that asks for nothing — and that is rejected on encode.
//
// The `blob` map is OMITTED ENTIRELY for a bare catalog request, which is what
// keeps the catalog's own transfer byte-identical to the pre-RFC-021 shape:
// namespace 0 is the default, so the generalization costs the common case
// exactly zero bytes.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"

namespace slopsync {

inline constexpr uint32_t kBlobReqMaxChunks = 32;  // brief-mandated fixed cap

// Which blob. The SAME identity vocabulary the raw BLOB_CHUNK header carries
// (blob_chunks.hpp) and the same one store CRUD intents will use, so the three
// can never drift — that is the entire point of RFC-021's `blob_keys` being
// one table rather than three.
//
// `ns` is authoritative; the other three are namespace-dependent:
//   blob_ns::catalog (0): store_id / slot MUST be absent, generation unused.
//   blob_ns::store   (1): store_id picks the store (a STORE-class catalog
//                         entry), slot picks the item, generation is the
//                         roster generation the requester believes it is
//                         consistent with (lets either side notice the
//                         enumeration went stale mid-transfer).
struct BlobId {
    uint8_t ns = blob_ns::catalog;
    bool has_store_id = false;
    uint8_t store_id = 0;
    bool has_slot = false;
    uint8_t slot = 0;
    bool has_generation = false;
    uint16_t generation = 0;

    constexpr bool isCatalog() const { return ns == blob_ns::catalog; }
    // Identity equality used by receivers to detect "these chunks belong to a
    // DIFFERENT transfer than the one currently being assembled". `generation` is
    // deliberately excluded: a generation change means the same item moved on,
    // which the reassembler reports as staleness rather than as a mismatch.
    constexpr bool sameTarget(const BlobId& o) const {
        return ns == o.ns && store_id == o.store_id && slot == o.slot;
    }
};

struct BlobReqMsg {
    BlobId blob{};
    // true <=> full-transfer request (no `chunks` key on the wire); false <=>
    // selective repair, `chunks[0..chunks_count)` names the wanted indices.
    bool full = true;
    uint32_t chunks_count = 0;
    std::array<uint16_t, kBlobReqMaxChunks> chunks{};
};

// ---- Encode -----------------------------------------------------------------
// Encodes into `out`; returns bytes written, or 0 on failure: too many
// indices, an inconsistent full+chunks_count>0 combination, a repair naming
// zero chunks, a catalog-namespace request carrying store_id/slot, or `out`
// too small.
inline size_t encodeBlobReq(const BlobReqMsg& m, std::span<std::byte> out) {
    if (m.chunks_count > kBlobReqMaxChunks) return 0;
    if (m.full && m.chunks_count > 0) return 0;   // ambiguous input, refuse
    if (!m.full && m.chunks_count == 0) return 0;  // repair naming nothing (RFC-022.6)
    if (m.blob.isCatalog() && (m.blob.has_store_id || m.blob.has_slot)) return 0;

    // The blob map is emitted only when it says something a default doesn't.
    uint32_t nBlobKeys = 0;
    const bool emitNs = !m.blob.isCatalog();
    if (emitNs) ++nBlobKeys;
    if (m.blob.has_store_id) ++nBlobKeys;
    if (m.blob.has_slot) ++nBlobKeys;
    if (m.blob.has_generation) ++nBlobKeys;

    uint32_t nKeys = (m.full ? 0u : 1u) + (nBlobKeys > 0 ? 1u : 0u);

    CborWriter w(out);
    w.mapHeader(nKeys);
    if (!m.full) {  // key 27 < key 38: ascending order (§5.3)
        w.key(CborKey::chunks).arrayHeader(m.chunks_count);
        for (uint32_t i = 0; i < m.chunks_count; ++i) w.uintVal(m.chunks[i]);
    }
    if (nBlobKeys > 0) {
        w.key(CborKey::blob).mapHeader(nBlobKeys);
        if (emitNs) w.key(uint64_t(blob::ns)).uintVal(m.blob.ns);
        if (m.blob.has_store_id) w.key(uint64_t(blob::store_id)).uintVal(m.blob.store_id);
        if (m.blob.has_slot) w.key(uint64_t(blob::slot)).uintVal(m.blob.slot);
        if (m.blob.has_generation) w.key(uint64_t(blob::generation)).uintVal(m.blob.generation);
    }
    return w.size();
}

// Decodes the `blob` (38) sub-map into `id`. Unknown sub-keys are skipped
// (§4.3) — `name`/`kind`/`payload`/`chunk_*`/`total_bytes` are legal members
// of this vocabulary that simply have no meaning in a REQUEST.
inline Result<uint32_t, DecodeError> decodeBlobId(CborReader& r, BlobId& id) {
    using Ret = Result<uint32_t, DecodeError>;
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());
    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case uint64_t(blob::ns): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                id.ns = uint8_t(v.value());
                break;
            }
            case uint64_t(blob::store_id): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                id.store_id = uint8_t(v.value());
                id.has_store_id = true;
                break;
            }
            case uint64_t(blob::slot): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                id.slot = uint8_t(v.value());
                id.has_slot = true;
                break;
            }
            case uint64_t(blob::generation): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFFFF) return Ret::err(DecodeError::Malformed);
                id.generation = uint16_t(v.value());
                id.has_generation = true;
                break;
            }
            default: {
                auto sv = r.skipValue();  // §4.3
                if (!sv) return Ret::err(sv.error());
                break;
            }
        }
    }
    return Ret::ok(nR.value());
}

// ---- Decode -----------------------------------------------------------------
// Decodes `in` into a BlobReqMsg. Unknown keys are skipped per §4.3.
inline Result<BlobReqMsg, DecodeError> decodeBlobReq(std::span<const std::byte> in) {
    using Ret = Result<BlobReqMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    BlobReqMsg m{};
    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        if (kR.value() == uint64_t(CborKey::chunks)) {
            auto aR = r.readArrayHeader();
            if (!aR) return Ret::err(aR.error());
            if (aR.value() > kBlobReqMaxChunks) return Ret::err(DecodeError::CapacityExceeded);
            if (aR.value() == 0) return Ret::err(DecodeError::Malformed);  // RFC-022.6
            for (uint32_t j = 0; j < aR.value(); ++j) {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFFFF) return Ret::err(DecodeError::Malformed);
                m.chunks[j] = uint16_t(v.value());
            }
            m.chunks_count = aR.value();
            m.full = false;
        } else if (kR.value() == uint64_t(CborKey::blob)) {
            auto br = decodeBlobId(r, m.blob);
            if (!br) return Ret::err(br.error());
        } else {
            auto sv = r.skipValue();  // §4.3
            if (!sv) return Ret::err(sv.error());
        }
    }
    // A catalog request has no store/slot by rule (§8.4 / blob_namespaces 0).
    if (m.blob.isCatalog() && (m.blob.has_store_id || m.blob.has_slot)) {
        return Ret::err(DecodeError::Malformed);
    }
    return Ret::ok(m);
}

}  // namespace slopsync
