// slopsync-core — catalog wire codec, SPEC §8.1/§8.3 + schema/catalog.cddl
// (NORMATIVE, Appendix C). Encodes/decodes a BasicCatalog<N> (the data model
// in channel/catalog.hpp) to/from the exact bytes catalog_etag.hpp hashes and
// CATALOG_CHUNK transports (catalog_chunks.hpp) fragment.
//
// CDDL, reproduced for cross-reference while reading this file:
//
//   catalog = [ * channel-entry ]
//   channel-entry = {
//     1 => uint, 2 => tstr(1..32), 3 => class, 4 => direction, 5 => access,
//     6 => float32, 7 => priority,
//     ? 8 => [ + layout-field ],            ; STATE|STREAM
//     ? 9 => { + int => schema-field },     ; INTENT|EVENT
//   }
//   layout-field  = { 1=>tstr(1..24), 2=>packed-type, 3=>tstr(0..8), 4=>float32,
//                     ?5=>float32, ?6=>float32, ?7=>{ + uint => tstr } }
//   schema-field  = { 1=>tstr(1..24), 2=>cbor-type, 3=>tstr(0..8),
//                     ?5=>float32, ?6=>float32 }
//
// Every map in the CBOR profile (§5.3) is sorted-ascending-by-key, including
// the entry's OWN keys (always 1..7 then 8-xor-9, already ascending by
// construction) and the schema map's field keys (arbitrary per-entry
// integers — NOT guaranteed ascending in CatalogEntry::schema[]'s storage
// order, so the encoder sorts by SchemaField::key before emitting; the
// layout array has no such issue since layout-field order IS the wire order,
// §5.4, and is never reordered).
//
// Determinism/ordering rule (§8.3): entries MUST be ascending by id.
// encodeCatalog refuses (returns 0) a catalog violating this; decodeCatalog
// enforces it on the way in and reports DecodeError::Malformed. This is what
// makes the etag reproducible from catalog *content* alone, independent of
// whatever order a hub's internal registration happened to build it in.
//
// ---------------------------------------------------------------------------
// A depth-4 CborWriter/CborReader encoding a depth-5 CDDL shape
// ---------------------------------------------------------------------------
// catalog.cddl's deepest legal shape is FIVE containers deep: the catalog
// array, a channel-entry map, that entry's layout ARRAY (key 8), one
// layout-field MAP inside it, and — for a bitfield8 field that names any
// bits — that field's OWN "bits" map (key 7). CborWriter/CborReader enforce
// §5.3's "maximum nesting depth 4" as a hard, tested ceiling (see the
// depth-4-accepted/depth-5-rejected cases in test_slopsync_cbor); this is
// NOT something this file may loosen, and the mini-catalog conformance
// fixture deliberately exercises exactly this bitfield8-with-named-bits case
// (twice) — so it has to work.
//
// The resolution: kMaxDepth bounds how deep any ONE CborWriter/CborReader
// INSTANCE will follow nesting — it is not, and cannot be, baked into the
// bytes themselves (a definite-length CBOR map/array is self-delimiting by
// its own pair/element count; nothing about the wire format remembers which
// object decoded it). So each catalog ENTRY is encoded/decoded through its
// OWN fresh writer/reader instance, giving it a full, independent depth-4
// budget: entry-map[1] -> layout-array[2] -> field-map[3] -> bits-map[4] —
// exactly 4, fitting the ceiling. The catalog's outer array HEADER (the 5th
// level in the full-structure sense) is produced/consumed by a separate,
// even-more-transient instance that never itself descends into an entry.
// Entries are stitched together at the byte level by this file, tracking a
// running offset via CborWriter::size() (encode) and the additive
// CborReader::bytesConsumed() accessor (decode; added for exactly this).
// The resulting bytes are IDENTICAL to what a (nonexistent) depth-5-capable
// single writer would have produced — depth-4 is an instance-local
// production/consumption safety valve, not a wire-format constraint.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <variant>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"

namespace slopsync {

// ============================================================================
// Encode
// ============================================================================

namespace detail {

// Emits one layout-field map (CDDL `layout-field`): keys 1,2,3,4 always,
// [5],[6] iff hasMin/hasMax, [7] iff at least one bit slot is named. Key
// order is already ascending as written — no sort needed at this level.
// Depth, relative to whatever depth `w` is already at when called: +1 (this
// field's own map) [+1 more if it opens the bits map].
inline void encodeLayoutField(CborWriter& w, const LayoutField& f) {
    uint32_t nBits = 0;
    for (const auto& name : f.bits) {
        if (!name.empty()) ++nBits;
    }
    uint32_t nKeys = 4;
    if (f.hasMin) ++nKeys;
    if (f.hasMax) ++nKeys;
    if (nBits > 0) ++nKeys;

    w.mapHeader(nKeys);
    w.key(1).tstrVal(f.name);
    w.key(2).uintVal(uint64_t(f.type));
    w.key(3).tstrVal(f.unit);      // non-optional per CDDL: "" still encodes
    w.key(4).f32Val(f.scale);
    if (f.hasMin) w.key(5).f32Val(f.min);
    if (f.hasMax) w.key(6).f32Val(f.max);
    if (nBits > 0) {
        w.key(7).mapHeader(nBits);
        for (uint32_t bit = 0; bit < f.bits.size(); ++bit) {
            if (!f.bits[bit].empty()) w.key(uint64_t(bit)).tstrVal(f.bits[bit]);
        }
    }
}

// Emits one schema-field map (CDDL `schema-field`): keys 1,2,3 always,
// [5],[6] iff hasMin/hasMax.
inline void encodeSchemaField(CborWriter& w, const SchemaField& f) {
    uint32_t nKeys = 3;
    if (f.hasMin) ++nKeys;
    if (f.hasMax) ++nKeys;

    w.mapHeader(nKeys);
    w.key(1).tstrVal(f.name);
    w.key(2).uintVal(uint64_t(f.type));
    w.key(3).tstrVal(f.unit);
    if (f.hasMin) w.key(5).f32Val(f.min);
    if (f.hasMax) w.key(6).f32Val(f.max);
}

// Emits one channel-entry map into a FRESH writer `w` (see file-level depth
// note: `w` must not have been used for anything else, so this entry gets
// the full depth-4 budget to itself). Returns nothing; caller reads
// w.size() to learn success/length (0 => failure, same convention as every
// encoder in this library).
inline void encodeEntry(CborWriter& w, const CatalogEntry& e) {
    const bool useLayout = e.usesLayout();

    w.mapHeader(8);  // keys 1..7 always, plus exactly one of 8/9
    w.key(1).uintVal(e.id);
    w.key(2).tstrVal(e.name);
    w.key(3).uintVal(uint64_t(e.cls));
    w.key(4).uintVal(uint64_t(e.dir));
    w.key(5).uintVal(uint64_t(e.access));
    w.key(6).f32Val(e.maxRateHz);
    w.key(7).uintVal(uint64_t(e.defaultPriority));

    if (useLayout) {
        w.key(8).arrayHeader(e.fieldCount);
        for (uint8_t f = 0; f < e.fieldCount; ++f) {
            encodeLayoutField(w, e.layout[f]);
        }
    } else {
        // Schema sub-map keys are the fields' OWN integer keys, which need
        // not already be in ascending storage order (§5.3 requires
        // ascending regardless) — insertion-sort the (small, <=8) index set
        // by SchemaField::key before emitting. No heap: fixed array.
        std::array<uint8_t, CatalogEntry::kMaxFields> order{};
        for (uint8_t f = 0; f < e.fieldCount; ++f) order[f] = f;
        std::sort(order.begin(), order.begin() + e.fieldCount,
                  [&](uint8_t a, uint8_t b) { return e.schema[a].key < e.schema[b].key; });

        w.key(9).mapHeader(e.fieldCount);
        for (uint8_t f = 0; f < e.fieldCount; ++f) {
            const SchemaField& sf = e.schema[order[f]];
            w.key(uint64_t(sf.key));
            encodeSchemaField(w, sf);
        }
    }
}

}  // namespace detail

// Encodes `cat` per the deterministic CBOR profile. Returns bytes written,
// or 0 on any failure: `out` too small, an entry's fieldCount exceeding
// CatalogEntry::kMaxFields, or entries not strictly ascending by id (§8.3).
// Encoders otherwise can't fail (core/result.hpp's convention).
template <size_t N>
size_t encodeCatalog(const BasicCatalog<N>& cat, std::span<std::byte> out) {
    for (uint16_t i = 1; i < cat.count; ++i) {
        if (cat.entries[i].id <= cat.entries[i - 1].id) return 0;
    }

    // The array header gets its own transient writer (see file-level depth
    // note) — a shortest-form CBOR array head is at most 9 bytes (major
    // type byte + 8-byte length), though catalog_max_entries (256) never
    // needs more than 3.
    std::array<std::byte, 9> hdrBuf{};
    CborWriter hdrW(hdrBuf);
    hdrW.arrayHeader(cat.count);
    size_t hdrLen = hdrW.size();
    if (hdrLen == 0 || out.size() < hdrLen) return 0;
    std::memcpy(out.data(), hdrBuf.data(), hdrLen);

    size_t pos = hdrLen;
    for (uint16_t i = 0; i < cat.count; ++i) {
        const CatalogEntry& e = cat.entries[i];
        if (e.fieldCount > CatalogEntry::kMaxFields) return 0;
        if (pos > out.size()) return 0;

        // Fresh writer per entry (file-level depth note): full depth-4
        // budget, independent of the array-header writer above and of
        // every other entry's writer.
        CborWriter w(out.subspan(pos));
        detail::encodeEntry(w, e);
        size_t n = w.size();
        if (n == 0) return 0;
        pos += n;
    }
    return pos;
}

// ============================================================================
// Decode
// ============================================================================

namespace detail {

inline Result<LayoutField, DecodeError> decodeLayoutField(CborReader& r) {
    using Ret = Result<LayoutField, DecodeError>;
    auto mR = r.readMapHeader();
    if (!mR) return Ret::err(mR.error());

    LayoutField f{};
    bool gotName = false, gotType = false, gotUnit = false, gotScale = false;
    for (uint32_t i = 0; i < mR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case 1: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > 24) return Ret::err(DecodeError::Malformed);
                f.name = v.value();
                gotName = true;
                break;
            }
            case 2: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 7) return Ret::err(DecodeError::Malformed);
                f.type = PackedFieldType(v.value());
                gotType = true;
                break;
            }
            case 3: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() > 8) return Ret::err(DecodeError::Malformed);
                f.unit = v.value();
                gotUnit = true;
                break;
            }
            case 4: {
                auto v = r.readF32();
                if (!v) return Ret::err(v.error());
                f.scale = v.value();
                gotScale = true;
                break;
            }
            case 5: {
                auto v = r.readF32();
                if (!v) return Ret::err(v.error());
                f.min = v.value();
                f.hasMin = true;
                break;
            }
            case 6: {
                auto v = r.readF32();
                if (!v) return Ret::err(v.error());
                f.max = v.value();
                f.hasMax = true;
                break;
            }
            case 7: {
                auto bmR = r.readMapHeader();
                if (!bmR) return Ret::err(bmR.error());
                for (uint32_t b = 0; b < bmR.value(); ++b) {
                    auto bk = r.readKey();
                    if (!bk) return Ret::err(bk.error());
                    if (bk.value() >= f.bits.size()) return Ret::err(DecodeError::Malformed);
                    auto bv = r.readTstr();
                    if (!bv) return Ret::err(bv.error());
                    f.bits[bk.value()] = bv.value();
                }
                break;
            }
            default: {
                auto sv = r.skipValue();  // §4.3: unknown key -> ignore
                if (!sv) return Ret::err(sv.error());
                break;
            }
        }
    }
    if (!(gotName && gotType && gotUnit && gotScale)) return Ret::err(DecodeError::Malformed);
    return Ret::ok(f);
}

inline Result<SchemaField, DecodeError> decodeSchemaField(CborReader& r, uint8_t key) {
    using Ret = Result<SchemaField, DecodeError>;
    auto mR = r.readMapHeader();
    if (!mR) return Ret::err(mR.error());

    SchemaField f{};
    f.key = key;
    bool gotName = false, gotType = false, gotUnit = false;
    for (uint32_t i = 0; i < mR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case 1: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > 24) return Ret::err(DecodeError::Malformed);
                f.name = v.value();
                gotName = true;
                break;
            }
            case 2: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 5) return Ret::err(DecodeError::Malformed);
                f.type = CborFieldType(v.value());
                gotType = true;
                break;
            }
            case 3: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() > 8) return Ret::err(DecodeError::Malformed);
                f.unit = v.value();
                gotUnit = true;
                break;
            }
            case 5: {
                auto v = r.readF32();
                if (!v) return Ret::err(v.error());
                f.min = v.value();
                f.hasMin = true;
                break;
            }
            case 6: {
                auto v = r.readF32();
                if (!v) return Ret::err(v.error());
                f.max = v.value();
                f.hasMax = true;
                break;
            }
            default: {
                auto sv = r.skipValue();
                if (!sv) return Ret::err(sv.error());
                break;
            }
        }
    }
    if (!(gotName && gotType && gotUnit)) return Ret::err(DecodeError::Malformed);
    return Ret::ok(f);
}

// Decodes ONE channel-entry map from a FRESH reader `r` (see file-level
// depth note: `r` must be a reader constructed fresh over this entry's
// byte range so it has the full depth-4 budget available). After a
// successful return, r.bytesConsumed() is exactly this entry's encoded
// length — the caller's cue for where the next entry starts.
inline Result<CatalogEntry, DecodeError> decodeEntry(CborReader& r) {
    using Ret = Result<CatalogEntry, DecodeError>;

    auto mR = r.readMapHeader();
    if (!mR) return Ret::err(mR.error());

    CatalogEntry e{};
    bool gotId = false, gotName = false, gotCls = false, gotDir = false;
    bool gotAccess = false, gotRate = false, gotPrio = false;
    bool gotLayout = false, gotSchema = false;

    for (uint32_t k = 0; k < mR.value(); ++k) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case 1: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFFFF) return Ret::err(DecodeError::Malformed);
                e.id = uint16_t(v.value());
                gotId = true;
                break;
            }
            case 2: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > 32) return Ret::err(DecodeError::Malformed);
                e.name = v.value();
                gotName = true;
                break;
            }
            case 3: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 3) return Ret::err(DecodeError::Malformed);
                e.cls = ChannelClass(v.value());
                gotCls = true;
                break;
            }
            case 4: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 1) return Ret::err(DecodeError::Malformed);
                e.dir = Direction(v.value());
                gotDir = true;
                break;
            }
            case 5: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 2) return Ret::err(DecodeError::Malformed);
                e.access = AccessLevel(v.value());
                gotAccess = true;
                break;
            }
            case 6: {
                auto v = r.readF32();
                if (!v) return Ret::err(v.error());
                e.maxRateHz = v.value();
                gotRate = true;
                break;
            }
            case 7: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 3) return Ret::err(DecodeError::Malformed);
                e.defaultPriority = Priority(v.value());
                gotPrio = true;
                break;
            }
            case 8: {
                if (gotSchema) return Ret::err(DecodeError::Malformed);  // both 8 and 9 present
                auto aR = r.readArrayHeader();
                if (!aR) return Ret::err(aR.error());
                if (aR.value() == 0 || aR.value() > CatalogEntry::kMaxFields) {
                    return Ret::err(DecodeError::Malformed);
                }
                for (uint32_t f = 0; f < aR.value(); ++f) {
                    auto fr = decodeLayoutField(r);
                    if (!fr) return Ret::err(fr.error());
                    e.layout[f] = fr.value();
                }
                e.fieldCount = uint8_t(aR.value());
                gotLayout = true;
                break;
            }
            case 9: {
                if (gotLayout) return Ret::err(DecodeError::Malformed);  // both 8 and 9 present
                auto smR = r.readMapHeader();
                if (!smR) return Ret::err(smR.error());
                if (smR.value() == 0 || smR.value() > CatalogEntry::kMaxFields) {
                    return Ret::err(DecodeError::Malformed);
                }
                for (uint32_t f = 0; f < smR.value(); ++f) {
                    auto keyR = r.readKey();
                    if (!keyR) return Ret::err(keyR.error());
                    if (keyR.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                    auto fr = decodeSchemaField(r, uint8_t(keyR.value()));
                    if (!fr) return Ret::err(fr.error());
                    e.schema[f] = fr.value();
                }
                e.fieldCount = uint8_t(smR.value());
                gotSchema = true;
                break;
            }
            default: {
                auto sv = r.skipValue();  // §4.3
                if (!sv) return Ret::err(sv.error());
                break;
            }
        }
    }

    if (!(gotId && gotName && gotCls && gotDir && gotAccess && gotRate && gotPrio)) {
        return Ret::err(DecodeError::Malformed);
    }
    if (gotLayout == gotSchema) return Ret::err(DecodeError::Malformed);          // need exactly one
    if (gotLayout != e.usesLayout()) return Ret::err(DecodeError::Malformed);     // must match class
    return Ret::ok(e);
}

}  // namespace detail

// Decodes `in` into `cat` (any prior contents are discarded). Enforces
// everything §5.3/§8.1/§8.3 require: ascending entry ids, exactly one of
// keys 8/9 per entry (both present -> Malformed; neither -> Malformed),
// capacity limits (BasicCatalog<N>'s N, CatalogEntry::kMaxFields). Unknown
// map keys are skipped per §4.3.
template <size_t N>
Result<std::monostate, DecodeError> decodeCatalog(std::span<const std::byte> in, BasicCatalog<N>& cat) {
    using Ret = Result<std::monostate, DecodeError>;

    // The array header is read by its own transient reader (file-level
    // depth note) — discarded immediately after, never used for entries.
    CborReader headerReader(in);
    auto nR = headerReader.readArrayHeader();
    if (!nR) return Ret::err(nR.error());
    if (nR.value() > N) return Ret::err(DecodeError::CapacityExceeded);
    size_t pos = headerReader.bytesConsumed();

    BasicCatalog<N> result{};
    bool haveFirst = false;
    uint16_t prevId = 0;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        if (pos > in.size()) return Ret::err(DecodeError::Truncated);

        // Fresh reader per entry (file-level depth note): full depth-4
        // budget, independent of headerReader and every other entry.
        CborReader r(in.subspan(pos));
        auto er = detail::decodeEntry(r);
        if (!er) return Ret::err(er.error());
        const CatalogEntry& e = er.value();

        if (haveFirst && e.id <= prevId) return Ret::err(DecodeError::Malformed);  // §8.3 ascending
        prevId = e.id;
        haveFirst = true;

        result.entries[i] = e;
        pos += r.bytesConsumed();
    }
    result.count = uint16_t(nR.value());
    cat = result;
    return Ret::ok(std::monostate{});
}

}  // namespace slopsync
