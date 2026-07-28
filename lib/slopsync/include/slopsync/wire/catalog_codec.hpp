// slopsync-core — catalog wire codec, SPEC §8.1/§8.3 + schema/catalog.cddl
// (NORMATIVE, Appendix C). Encodes/decodes a BasicCatalog<...> (the data model
// in channel/catalog.hpp) to/from the exact bytes catalog_etag.hpp hashes and
// BLOB_CHUNK transports (blob_chunks.hpp) fragment.
//
// CDDL, reproduced for cross-reference while reading this file:
//
//   catalog = [ * channel-entry ]
//   channel-entry = {
//     1 => uint, 2 => tstr(1..32), 3 => class, 4 => direction, 5 => access,
//     6 => float32, 7 => priority,
//     ? 8  => [ + layout-field ],            ; STATE|STREAM
//     ? 9  => { + int => schema-field },     ; INTENT|EVENT
//     ? 10 => uint,                          ; category    (RFC-009; ui_categories vocab as of RFC-047/048)
//     ? 11 => tstr(1..24),                   ; category_label     (RFC-009)
//     ? 12 => store-descriptor,              ; STORE              (RFC-021)
//     ? 13 => uint,                          ; replay_depth       (RFC-017)
//     ? 14 => uint,                          ; setting_channel    (RFC-009)
//     ? 15 => uint,                          ; stream_kind        (RFC-014/023, STREAM only)
//     ? 16 => uint,                          ; rank               (RFC-048, Phase C2)
//   }
//   layout-field  = { 1=>tstr(1..24), 2=>packed-type, 3=>tstr(0..8), 4=>float32,
//                     ?5=>float32, ?6=>float32, ?7=>{ + uint => tstr },
//                     ?8=>uint, ?9=>setting-default, ?10=>[ + tstr ],
//                     ?11=>tstr, ?12=>tstr, ?13=>tstr, ?14=>float32, ?15=>uint,
//                     ?18=>uint, ?19=>uint, ?20=>uint, ?21=>uint, ?22=>uint, ?23=>uint }
//   schema-field  = { 1=>tstr(1..24), 2=>cbor-type, 3=>tstr(0..8),
//                     ?5=>float32, ?6=>float32,
//                     ?9=>setting-default, ?10=>[ + tstr ], ?11=>tstr, ?12=>tstr,
//                     ?13=>tstr, ?14=>float32, ?15=>uint, ?16=>access,
//                     ?17=>[ + access ], ?19=>uint, ?20=>uint, ?21=>uint, ?22=>uint, ?23=>uint }
//   store-descriptor = { 1=>uint, 2=>tstr(1..32), 3=>uint, 4=>uint, 5=>uint }
//
//   RFC-048 (Phase C2) added: layout-field/schema-field 19=rank, 20=aspect,
//   21=scope, 22=provenance, 23=unit_id (numbered identically across both
//   kinds, shared-numbering convention); channel-entry 16=rank. See
//   schema/catalog.cddl for the per-key prose.
//
// THE RFC-009 ANNOTATION BLOCK IS ENTIRELY OPTIONAL, and §5.3's deterministic
// profile does not emit absent optional keys. A catalog carrying NO annotations
// therefore encodes to byte-identical output before and after M2b — which is
// what the frozen mini-catalog fixture's pinned 733-byte length and pinned etag
// verify on every test run. If either pin moves, something that should have
// been optional was made mandatory.
//
// Every map in the CBOR profile (§5.3) is sorted-ascending-by-key, including
// the entry's OWN keys (1..7, then 8-xor-9, then 10/11/12/13/14 — ascending by
// construction as written below) and the schema map's field keys (arbitrary
// per-entry integers — NOT guaranteed ascending in the catalog's schema-pool
// storage order, so the encoder sorts by SchemaField::key before emitting; the
// layout array has no such issue since layout-field order IS the wire order,
// §5.4, and is never reordered).
//
// Payload STORAGE lives in the owning catalog's pools (channel/catalog.hpp) —
// this file reaches fields, store descriptors and interned labels exclusively
// through cat.layoutFields(entry) / cat.schemaFields(entry) /
// cat.storeDescriptor(entry) / cat.bitLabels(f) / cat.optionLabels(f) and never
// indexes a pool directly (decode being the exception: it PUSHES into the pools
// through the pool API). The bytes are unaffected by that: the encoding is a
// function of catalog CONTENT.
//
// Determinism/ordering rule (§8.3): entries MUST be ascending by id.
// encodeCatalog refuses (returns 0) a catalog violating this; decodeCatalog
// enforces it on the way in and reports DecodeError::Malformed. This is what
// makes the etag reproducible from catalog *content* alone, independent of
// whatever order a hub's internal registration happened to build it in.
//
// ---------------------------------------------------------------------------
// PER-ENTRY BYTE CAP (limits::catalog_max_entry_bytes = 4096)
// ---------------------------------------------------------------------------
// SPEC §8.1 makes each entry an INDEPENDENTLY-DECODABLE document and RFC-028
// forbids unbounded per-entry decode buffers. With kMaxFields now 64 and full
// `desc` strings, an entry could otherwise encode to 8–10 KB. Both directions
// enforce the cap, cheaply:
//   * encodeCatalog REFUSES (returns 0) if any single entry exceeds it — an
//     oversize entry is simply not emittable, so it can never reach a peer.
//   * decodeCatalog gives each entry a reader window of at most the cap, so a
//     forged entry cannot induce an unbounded scan; it fails Truncated instead.
// conformance::checkCatalog reports WHICH entry is oversize (ViolationKind::
// EntryTooLarge) — that is the authoring-time diagnostic; these two are the
// mechanical guards.
//
// ---------------------------------------------------------------------------
// A depth-4 CborWriter/CborReader encoding a depth-5 CDDL shape
// ---------------------------------------------------------------------------
// catalog.cddl's deepest legal shape is FIVE containers deep: the catalog
// array, a channel-entry map, that entry's layout ARRAY (key 8), one
// layout-field MAP inside it, and — for a bitfield8 field that names any
// bits — that field's OWN "bits" map (key 7). (RFC-009's `options` array and
// `option_access` array sit at exactly the same depth, which is why the CDDL
// says the field-map level is AT the cap and any future annotation wanting a
// container inside a field map must ride the entry level instead.)
// CborWriter/CborReader enforce §5.3's "maximum nesting depth 4" as a hard,
// tested ceiling (see the depth-4-accepted/depth-5-rejected cases in
// test_slopsync_cbor); this is NOT something this file may loosen, and the
// mini-catalog conformance fixture deliberately exercises exactly this
// bitfield8-with-named-bits case (twice) — so it has to work.
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

// ---- Encode -----------------------------------------------------------------

namespace detail {

// Emits a `setting-default` value (CDDL: int / float32 / bool / tstr). The
// Kind tag picks the CBOR type; Kind::None is never passed here (the caller
// tests has() before counting the key).
inline void encodeSettingDefault(CborWriter& w, const SettingDefault& d) {
    switch (d.kind) {
        case SettingDefault::Kind::Int: w.intVal(d.asInt()); break;
        case SettingDefault::Kind::Float: w.f32Val(d.asFloat()); break;
        case SettingDefault::Kind::Bool: w.boolVal(d.asBool()); break;
        case SettingDefault::Kind::Tstr: w.tstrVal(d.s); break;
        case SettingDefault::Kind::None: break;
    }
}

// Emits one layout-field map (CDDL `layout-field`): keys 1,2,3,4 always, then
// every present optional in ascending key order. Key order is already
// ascending as written — no sort needed at this level.
// Depth, relative to whatever depth `w` is already at when called: +1 (this
// field's own map) [+1 more if it opens the bits map or the options array].
inline void encodeLayoutField(CborWriter& w, const LayoutField& f,
                              std::span<const std::string_view> bits,
                              std::span<const std::string_view> options) {
    uint32_t nBits = 0;
    for (std::string_view name : bits) {
        if (!name.empty()) ++nBits;
    }
    uint32_t nKeys = 4;
    if (f.hasMin) ++nKeys;
    if (f.hasMax) ++nKeys;
    if (nBits > 0) ++nKeys;
    if (f.hasSettingKey) ++nKeys;
    if (f.dflt.has()) ++nKeys;
    if (!options.empty()) ++nKeys;
    if (!f.group.empty()) ++nKeys;
    if (!f.desc.empty()) ++nKeys;
    if (!f.role.empty()) ++nKeys;
    if (f.hasStep) ++nKeys;
    if (f.flags != 0) ++nKeys;
    if (f.hasRank) ++nKeys;
    if (f.hasAspect) ++nKeys;
    if (f.hasScope) ++nKeys;
    if (f.hasProvenance) ++nKeys;
    if (f.hasUnitId) ++nKeys;

    w.mapHeader(nKeys);
    w.key(1).tstrVal(f.name);
    w.key(2).uintVal(uint64_t(f.type));
    w.key(3).tstrVal(f.unit);      // non-optional per CDDL: "" still encodes
    w.key(4).f32Val(f.scale);
    if (f.hasMin) w.key(5).f32Val(f.min);
    if (f.hasMax) w.key(6).f32Val(f.max);
    if (nBits > 0) {
        w.key(7).mapHeader(nBits);
        for (size_t bit = 0; bit < bits.size(); ++bit) {
            if (!bits[bit].empty()) w.key(uint64_t(bit)).tstrVal(bits[bit]);
        }
    }
    if (f.hasSettingKey) w.key(8).uintVal(f.settingKey);
    if (f.dflt.has()) {
        w.key(9);
        encodeSettingDefault(w, f.dflt);
    }
    if (!options.empty()) {
        w.key(10).arrayHeader(uint32_t(options.size()));
        for (std::string_view o : options) w.tstrVal(o);
    }
    if (!f.group.empty()) w.key(11).tstrVal(f.group);
    if (!f.desc.empty()) w.key(12).tstrVal(f.desc);
    if (!f.role.empty()) w.key(13).tstrVal(f.role);
    if (f.hasStep) w.key(14).f32Val(f.step);
    if (f.flags != 0) w.key(15).uintVal(f.flags);
    // RFC-048 (Phase C2), keys 19-23 — ascending, after 18 (RFC-037 size, this
    // library never authors) and the reserved-for-schema-field 16/17.
    if (f.hasRank) w.key(19).uintVal(f.rank);
    if (f.hasAspect) w.key(20).uintVal(f.aspect);
    if (f.hasScope) w.key(21).uintVal(f.scope);
    if (f.hasProvenance) w.key(22).uintVal(f.provenance);
    if (f.hasUnitId) w.key(23).uintVal(f.unitId);
}

// Emits one schema-field map (CDDL `schema-field`): keys 1,2,3 always, then
// every present optional in ascending key order.
inline void encodeSchemaField(CborWriter& w, const SchemaField& f,
                              std::span<const std::string_view> options,
                              std::span<const AccessLevel> optionAccess) {
    uint32_t nKeys = 3;
    if (f.hasMin) ++nKeys;
    if (f.hasMax) ++nKeys;
    if (f.dflt.has()) ++nKeys;
    if (!options.empty()) ++nKeys;
    if (!f.group.empty()) ++nKeys;
    if (!f.desc.empty()) ++nKeys;
    if (!f.role.empty()) ++nKeys;
    if (f.hasStep) ++nKeys;
    if (f.flags != 0) ++nKeys;
    if (f.hasAccess) ++nKeys;
    if (!optionAccess.empty()) ++nKeys;
    if (f.hasRank) ++nKeys;
    if (f.hasAspect) ++nKeys;
    if (f.hasScope) ++nKeys;
    if (f.hasProvenance) ++nKeys;
    if (f.hasUnitId) ++nKeys;

    w.mapHeader(nKeys);
    w.key(1).tstrVal(f.name);
    w.key(2).uintVal(uint64_t(f.type));
    w.key(3).tstrVal(f.unit);
    if (f.hasMin) w.key(5).f32Val(f.min);
    if (f.hasMax) w.key(6).f32Val(f.max);
    if (f.dflt.has()) {
        w.key(9);
        encodeSettingDefault(w, f.dflt);
    }
    if (!options.empty()) {
        w.key(10).arrayHeader(uint32_t(options.size()));
        for (std::string_view o : options) w.tstrVal(o);
    }
    if (!f.group.empty()) w.key(11).tstrVal(f.group);
    if (!f.desc.empty()) w.key(12).tstrVal(f.desc);
    if (!f.role.empty()) w.key(13).tstrVal(f.role);
    if (f.hasStep) w.key(14).f32Val(f.step);
    if (f.flags != 0) w.key(15).uintVal(f.flags);
    if (f.hasAccess) w.key(16).uintVal(uint64_t(f.access));
    if (!optionAccess.empty()) {
        w.key(17).arrayHeader(uint32_t(optionAccess.size()));
        for (AccessLevel a : optionAccess) w.uintVal(uint64_t(a));
    }
    // RFC-048 (Phase C2), numbered identically to LayoutField's own 19-23.
    if (f.hasRank) w.key(19).uintVal(f.rank);
    if (f.hasAspect) w.key(20).uintVal(f.aspect);
    if (f.hasScope) w.key(21).uintVal(f.scope);
    if (f.hasProvenance) w.key(22).uintVal(f.provenance);
    if (f.hasUnitId) w.key(23).uintVal(f.unitId);
}

// Emits one store-descriptor map (CDDL `store-descriptor`, RFC-021). All five
// keys are mandatory — a store that declares no capacity describes nothing.
inline void encodeStoreDescriptor(CborWriter& w, const StoreDescriptor& d) {
    w.mapHeader(5);
    w.key(1).uintVal(d.storeId);
    w.key(2).tstrVal(d.kind);
    w.key(3).uintVal(d.capacity);
    w.key(4).uintVal(d.perItemMax);
    w.key(5).uintVal(d.nameMax);
}

// Emits one channel-entry map into a FRESH writer `w` (see file-level depth
// note: `w` must not have been used for anything else, so this entry gets
// the full depth-4 budget to itself). The entry's payload is resolved from
// `cat` — exactly one of layout / schema / store, selected by its class.
// Returns nothing; caller reads w.size() to learn success/length (0 =>
// failure, same convention as every encoder in this library).
template <size_t E, size_t L, size_t S, size_t B, size_t T>
inline void encodeEntry(CborWriter& w, const BasicCatalog<E, L, S, B, T>& cat, const CatalogEntry& e) {
    const auto layout = cat.layoutFields(e);
    const auto schema = cat.schemaFields(e);
    const StoreDescriptor* store = cat.storeDescriptor(e);

    // Bail BEFORE writing anything if the entry declares more fields than the
    // codec's per-entry bound: w.size() stays 0, which is encodeCatalog's
    // failure signal. encodeCatalog already rejects such entries, so this is
    // belt-and-braces — but it is also what makes the schema-key sort scratch
    // below provably in-bounds (a span's size is not a compile-time bound, and
    // GCC 16 rightly complains without this).
    size_t nFields = 0;
    switch (e.form()) {
        case FieldForm::Layout: nFields = layout.size(); break;
        case FieldForm::Schema: nFields = schema.size(); break;
        case FieldForm::Store: nFields = 0; break;
    }
    if (nFields > CatalogEntry::kMaxFields) return;
    if (e.usesStore() && store == nullptr) return;

    uint32_t nKeys = 7;
    ++nKeys;  // exactly one of 8 / 9 / 12
    if (e.hasCategory) ++nKeys;
    if (!e.categoryLabel.empty()) ++nKeys;
    if (e.hasReplayDepth) ++nKeys;
    if (e.hasSettingChannel) ++nKeys;
    if (e.streamKind != 0) ++nKeys;  // RFC-014/023: omitted when 0 (stream_kinds::samples, the default)
    if (e.hasRank) ++nKeys;  // RFC-048 (Phase C2), key 16

    w.mapHeader(nKeys);
    w.key(1).uintVal(e.id);
    w.key(2).tstrVal(e.name);
    w.key(3).uintVal(uint64_t(e.cls));
    w.key(4).uintVal(uint64_t(e.dir));
    w.key(5).uintVal(uint64_t(e.access));
    w.key(6).f32Val(e.maxRateHz);
    w.key(7).uintVal(uint64_t(e.defaultPriority));

    switch (e.form()) {
        case FieldForm::Layout: {
            w.key(8).arrayHeader(uint32_t(layout.size()));
            for (const LayoutField& lf : layout) {
                encodeLayoutField(w, lf, cat.bitLabels(lf), cat.optionLabels(lf));
            }
            break;
        }
        case FieldForm::Schema: {
            // Schema sub-map keys are the fields' OWN integer keys, which need
            // not already be in ascending storage order (§5.3 requires
            // ascending regardless) — sort the (small, <=kMaxFields) index set
            // by SchemaField::key before emitting. No heap: fixed array.
            const size_t n = nFields;
            std::array<uint8_t, CatalogEntry::kMaxFields> order{};
            for (size_t f = 0; f < n; ++f) order[f] = uint8_t(f);
            std::sort(order.begin(), order.begin() + n,
                      [&](uint8_t a, uint8_t b) { return schema[a].key < schema[b].key; });

            w.key(9).mapHeader(uint32_t(n));
            for (size_t f = 0; f < n; ++f) {
                const SchemaField& sf = schema[order[f]];
                w.key(uint64_t(sf.key));
                encodeSchemaField(w, sf, cat.optionLabels(sf), cat.optionAccess(sf));
            }
            break;
        }
        case FieldForm::Store: break;  // key 12, emitted below in ascending order
    }

    // Ascending key order: 10 and 11 come BEFORE the store descriptor's 12.
    if (e.hasCategory) w.key(10).uintVal(e.category);
    if (!e.categoryLabel.empty()) w.key(11).tstrVal(e.categoryLabel);
    if (e.usesStore()) {
        w.key(12);
        encodeStoreDescriptor(w, *store);
    }
    if (e.hasReplayDepth) w.key(13).uintVal(e.replayDepth);  // RFC-017
    if (e.hasSettingChannel) w.key(14).uintVal(e.settingChannel);
    if (e.streamKind != 0) w.key(15).uintVal(e.streamKind);  // RFC-014/023
    if (e.hasRank) w.key(16).uintVal(e.rank);  // RFC-048 (Phase C2)
}

}  // namespace detail

// Encodes `cat` per the deterministic CBOR profile. Returns bytes written,
// or 0 on any failure: `out` too small, an entry's fieldCount exceeding
// CatalogEntry::kMaxFields, an entry whose payload doesn't resolve in `cat`'s
// pools, an entry exceeding limits::catalog_max_entry_bytes, or entries not
// strictly ascending by id (§8.3). Encoders otherwise can't fail
// (core/result.hpp's convention).
template <size_t E, size_t L, size_t S, size_t B, size_t T>
size_t encodeCatalog(const BasicCatalog<E, L, S, B, T>& cat, std::span<std::byte> out) {
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

        // Resolve the entry's payload through its owning catalog. A span
        // shorter than fieldCount (or a null store descriptor) means the
        // entry's (offset,count) doesn't sit inside `cat`'s pools — an entry
        // from a different catalog, or a hand-mutated one. Refuse rather than
        // emit a short field list.
        switch (e.form()) {
            case FieldForm::Layout:
                if (cat.layoutFields(e).size() != e.fieldCount) return 0;
                break;
            case FieldForm::Schema:
                if (cat.schemaFields(e).size() != e.fieldCount) return 0;
                break;
            case FieldForm::Store:
                if (cat.storeDescriptor(e) == nullptr) return 0;
                break;
        }

        // Fresh writer per entry (file-level depth note): full depth-4
        // budget, independent of the array-header writer above and of
        // every other entry's writer.
        CborWriter w(out.subspan(pos));
        detail::encodeEntry(w, cat, e);
        size_t n = w.size();
        if (n == 0) return 0;
        // §8.1 / RFC-028: an entry must be an independently-decodable document
        // of bounded size. Refuse to emit one no conforming peer may buffer.
        if (n > limits::catalog_max_entry_bytes) return 0;
        pos += n;
    }
    return pos;
}

// ---- Decode -----------------------------------------------------------------

namespace detail {

// Reads a `setting-default` value, dispatching on the CBOR major type.
// float32 and bool share major 7, so the float read is attempted first — a
// failed read of this reader consumes nothing (it parses the head, checks the
// kind, and only then advances), so falling through to readBool() is safe.
inline Result<SettingDefault, DecodeError> decodeSettingDefault(CborReader& r) {
    using Ret = Result<SettingDefault, DecodeError>;
    auto t = r.peekType();
    if (!t) return Ret::err(t.error());
    switch (t.value()) {
        case CborReader::MajorType::UInt:
        case CborReader::MajorType::NegInt: {
            auto v = r.readInt();
            if (!v) return Ret::err(v.error());
            if (v.value() > INT32_MAX || v.value() < INT32_MIN) return Ret::err(DecodeError::Malformed);
            return Ret::ok(SettingDefault::ofInt(int32_t(v.value())));
        }
        case CborReader::MajorType::TextString: {
            auto v = r.readTstr();
            if (!v) return Ret::err(v.error());
            return Ret::ok(SettingDefault::ofTstr(v.value()));
        }
        case CborReader::MajorType::Simple: {
            auto fv = r.readF32();
            if (fv) return Ret::ok(SettingDefault::ofFloat(fv.value()));
            auto bv = r.readBool();
            if (!bv) return Ret::err(DecodeError::Malformed);
            return Ret::ok(SettingDefault::ofBool(bv.value()));
        }
        default: return Ret::err(DecodeError::Malformed);
    }
}

// Reads an `[ + tstr ]` option-label array straight into `cat`'s label pool,
// writing the resulting ref to `out`.
template <size_t E, size_t L, size_t S, size_t B, size_t T>
inline Result<std::monostate, DecodeError> decodeOptions(CborReader& r, BasicCatalog<E, L, S, B, T>& cat,
                                                         LabelRef& out) {
    using Ret = Result<std::monostate, DecodeError>;
    auto aR = r.readArrayHeader();
    if (!aR) return Ret::err(aR.error());
    if (aR.value() == 0 || aR.value() > LabelRef::kMaxCount) return Ret::err(DecodeError::Malformed);
    const uint16_t mark = cat.labelMark();
    for (uint32_t i = 0; i < aR.value(); ++i) {
        auto v = r.readTstr();
        if (!v) return Ret::err(v.error());
        if (v.value().empty() || v.value().size() > limits::option_label_max_bytes) {
            return Ret::err(DecodeError::Malformed);
        }
        if (!cat.poolAddLabel(v.value())) return Ret::err(DecodeError::CapacityExceeded);
    }
    out = LabelRef{mark, uint8_t(aR.value())};
    return Ret::ok(std::monostate{});
}

template <size_t E, size_t L, size_t S, size_t B, size_t T>
inline Result<LayoutField, DecodeError> decodeLayoutField(CborReader& r, BasicCatalog<E, L, S, B, T>& cat) {
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
                // 0..10 — the RFC-026 str16/str32/str64 types are 8/9/10.
                if (v.value() > uint64_t(PackedFieldType::str64)) return Ret::err(DecodeError::Malformed);
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
                // Bit labels are INDEX-ALIGNED in the pool: keys ascend (§5.3),
                // so gaps are back-filled with empty views and `count` stops at
                // the last NAMED bit. Re-encoding therefore reproduces exactly
                // the same sparse bits map it came from.
                auto bmR = r.readMapHeader();
                if (!bmR) return Ret::err(bmR.error());
                if (bmR.value() == 0 || bmR.value() > 8) return Ret::err(DecodeError::Malformed);
                const uint16_t mark = cat.labelMark();
                uint8_t filled = 0;
                for (uint32_t b = 0; b < bmR.value(); ++b) {
                    auto bk = r.readKey();
                    if (!bk) return Ret::err(bk.error());
                    if (bk.value() >= 8) return Ret::err(DecodeError::Malformed);
                    auto bv = r.readTstr();
                    if (!bv) return Ret::err(bv.error());
                    if (bv.value().empty() || bv.value().size() > 24) return Ret::err(DecodeError::Malformed);
                    while (filled < bk.value()) {
                        if (!cat.poolAddLabel({})) return Ret::err(DecodeError::CapacityExceeded);
                        ++filled;
                    }
                    if (!cat.poolAddLabel(bv.value())) return Ret::err(DecodeError::CapacityExceeded);
                    ++filled;
                }
                f.bits = LabelRef{mark, filled};
                break;
            }
            case 8: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.settingKey = uint8_t(v.value());
                f.hasSettingKey = true;
                break;
            }
            case 9: {
                auto v = decodeSettingDefault(r);
                if (!v) return Ret::err(v.error());
                f.dflt = v.value();
                break;
            }
            case 10: {
                auto v = decodeOptions(r, cat, f.options);
                if (!v) return Ret::err(v.error());
                break;
            }
            case 11: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > 24) return Ret::err(DecodeError::Malformed);
                f.group = v.value();
                break;
            }
            case 12: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > limits::desc_max_bytes) {
                    return Ret::err(DecodeError::Malformed);
                }
                f.desc = v.value();
                break;
            }
            case 13: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > 24) return Ret::err(DecodeError::Malformed);
                f.role = v.value();
                break;
            }
            case 14: {
                auto v = r.readF32();
                if (!v) return Ret::err(v.error());
                f.step = v.value();
                f.hasStep = true;
                break;
            }
            case 15: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.flags = uint8_t(v.value());
                break;
            }
            // RFC-048 (Phase C2), keys 19-23 — see catalog_codec.hpp banner.
            case 19: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.rank = uint8_t(v.value());
                f.hasRank = true;
                break;
            }
            case 20: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.aspect = uint8_t(v.value());
                f.hasAspect = true;
                break;
            }
            case 21: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.scope = uint8_t(v.value());
                f.hasScope = true;
                break;
            }
            case 22: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.provenance = uint8_t(v.value());
                f.hasProvenance = true;
                break;
            }
            case 23: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.unitId = uint8_t(v.value());
                f.hasUnitId = true;
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

template <size_t E, size_t L, size_t S, size_t B, size_t T>
inline Result<SchemaField, DecodeError> decodeSchemaField(CborReader& r, BasicCatalog<E, L, S, B, T>& cat,
                                                          uint8_t key) {
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
            case 9: {
                auto v = decodeSettingDefault(r);
                if (!v) return Ret::err(v.error());
                f.dflt = v.value();
                break;
            }
            case 10: {
                auto v = decodeOptions(r, cat, f.options);
                if (!v) return Ret::err(v.error());
                break;
            }
            case 11: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > 24) return Ret::err(DecodeError::Malformed);
                f.group = v.value();
                break;
            }
            case 12: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > limits::desc_max_bytes) {
                    return Ret::err(DecodeError::Malformed);
                }
                f.desc = v.value();
                break;
            }
            case 13: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > 24) return Ret::err(DecodeError::Malformed);
                f.role = v.value();
                break;
            }
            case 14: {
                auto v = r.readF32();
                if (!v) return Ret::err(v.error());
                f.step = v.value();
                f.hasStep = true;
                break;
            }
            case 15: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.flags = uint8_t(v.value());
                break;
            }
            case 16: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 2) return Ret::err(DecodeError::Malformed);
                f.access = AccessLevel(v.value());
                f.hasAccess = true;
                break;
            }
            case 17: {
                // Index-aligned with key 10, which §5.3's ascending key order
                // guarantees has already been decoded. Without options there is
                // nothing to align to — that is a malformed field, not a
                // silently-ignored one.
                auto aR = r.readArrayHeader();
                if (!aR) return Ret::err(aR.error());
                if (f.options.count == 0 || aR.value() != f.options.count) {
                    return Ret::err(DecodeError::Malformed);
                }
                for (uint32_t a = 0; a < aR.value(); ++a) {
                    auto v = r.readUint();
                    if (!v) return Ret::err(v.error());
                    if (v.value() > 2) return Ret::err(DecodeError::Malformed);
                    if (!cat.poolSetLabelAccess(uint16_t(f.options.offset + a), AccessLevel(v.value()))) {
                        return Ret::err(DecodeError::Malformed);
                    }
                }
                f.hasOptionAccess = true;
                break;
            }
            // RFC-048 (Phase C2) — numbered identically to layout-field's own 19-23.
            case 19: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.rank = uint8_t(v.value());
                f.hasRank = true;
                break;
            }
            case 20: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.aspect = uint8_t(v.value());
                f.hasAspect = true;
                break;
            }
            case 21: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.scope = uint8_t(v.value());
                f.hasScope = true;
                break;
            }
            case 22: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.provenance = uint8_t(v.value());
                f.hasProvenance = true;
                break;
            }
            case 23: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                f.unitId = uint8_t(v.value());
                f.hasUnitId = true;
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

inline Result<StoreDescriptor, DecodeError> decodeStoreDescriptor(CborReader& r) {
    using Ret = Result<StoreDescriptor, DecodeError>;
    auto mR = r.readMapHeader();
    if (!mR) return Ret::err(mR.error());

    StoreDescriptor d{};
    bool gotId = false, gotKind = false, gotCap = false, gotItemMax = false, gotNameMax = false;
    for (uint32_t i = 0; i < mR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case 1: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                d.storeId = uint8_t(v.value());
                gotId = true;
                break;
            }
            case 2: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > 32) return Ret::err(DecodeError::Malformed);
                d.kind = v.value();
                gotKind = true;
                break;
            }
            case 3: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFFFF) return Ret::err(DecodeError::Malformed);
                d.capacity = uint16_t(v.value());
                gotCap = true;
                break;
            }
            case 4: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFFFFFFFFull) return Ret::err(DecodeError::Malformed);
                d.perItemMax = uint32_t(v.value());
                gotItemMax = true;
                break;
            }
            case 5: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFFFF) return Ret::err(DecodeError::Malformed);
                d.nameMax = uint16_t(v.value());
                gotNameMax = true;
                break;
            }
            default: {
                auto sv = r.skipValue();
                if (!sv) return Ret::err(sv.error());
                break;
            }
        }
    }
    if (!(gotId && gotKind && gotCap && gotItemMax && gotNameMax)) return Ret::err(DecodeError::Malformed);
    return Ret::ok(d);
}

// Decodes ONE channel-entry map from a FRESH reader `r` (see file-level
// depth note: `r` must be a reader constructed fresh over this entry's
// byte range so it has the full depth-4 budget available). After a
// successful return, r.bytesConsumed() is exactly this entry's encoded
// length — the caller's cue for where the next entry starts.
//
// Payload is pushed STRAIGHT INTO `cat`'s pools as it is read (the entry's
// fieldOffset is the pool mark taken before this entry started), so no
// per-entry field buffer is ever materialized on the stack — which is what
// lets kMaxFields be 64 at zero stack cost. The returned entry is NOT yet in
// `cat`; the caller validates ordering and then pushes it with
// addEntryVerbatim(). Failure may leave orphaned fields/labels in the pools:
// harmless (the counts are the truth, and decodeCatalog fails whole either
// way).
template <size_t E, size_t L, size_t S, size_t B, size_t T>
inline Result<CatalogEntry, DecodeError> decodeEntry(CborReader& r, BasicCatalog<E, L, S, B, T>& cat) {
    using Ret = Result<CatalogEntry, DecodeError>;

    auto mR = r.readMapHeader();
    if (!mR) return Ret::err(mR.error());

    CatalogEntry e{};
    const uint16_t layoutMark = cat.layoutMark();
    const uint16_t schemaMark = cat.schemaMark();
    const uint16_t storeMark = cat.storeMark();
    bool gotId = false, gotName = false, gotCls = false, gotDir = false;
    bool gotAccess = false, gotRate = false, gotPrio = false;
    bool gotLayout = false, gotSchema = false, gotStore = false;

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
                if (v.value() > uint64_t(ChannelClass::STORE)) return Ret::err(DecodeError::Malformed);
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
                if (gotSchema || gotStore) return Ret::err(DecodeError::Malformed);  // >1 payload key
                auto aR = r.readArrayHeader();
                if (!aR) return Ret::err(aR.error());
                if (aR.value() == 0 || aR.value() > CatalogEntry::kMaxFields) {
                    return Ret::err(DecodeError::Malformed);
                }
                e.fieldOffset = layoutMark;
                for (uint32_t f = 0; f < aR.value(); ++f) {
                    auto fr = decodeLayoutField(r, cat);
                    if (!fr) return Ret::err(fr.error());
                    if (!cat.poolAddLayoutField(fr.value())) return Ret::err(DecodeError::CapacityExceeded);
                }
                e.fieldCount = uint8_t(aR.value());
                gotLayout = true;
                break;
            }
            case 9: {
                if (gotLayout || gotStore) return Ret::err(DecodeError::Malformed);  // >1 payload key
                auto smR = r.readMapHeader();
                if (!smR) return Ret::err(smR.error());
                if (smR.value() == 0 || smR.value() > CatalogEntry::kMaxFields) {
                    return Ret::err(DecodeError::Malformed);
                }
                e.fieldOffset = schemaMark;
                for (uint32_t f = 0; f < smR.value(); ++f) {
                    auto keyR = r.readKey();
                    if (!keyR) return Ret::err(keyR.error());
                    if (keyR.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                    auto fr = decodeSchemaField(r, cat, uint8_t(keyR.value()));
                    if (!fr) return Ret::err(fr.error());
                    if (!cat.poolAddSchemaField(fr.value())) return Ret::err(DecodeError::CapacityExceeded);
                }
                e.fieldCount = uint8_t(smR.value());
                gotSchema = true;
                break;
            }
            case 10: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                e.category = uint8_t(v.value());
                e.hasCategory = true;
                break;
            }
            case 11: {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().empty() || v.value().size() > 24) return Ret::err(DecodeError::Malformed);
                e.categoryLabel = v.value();
                break;
            }
            case 12: {
                if (gotLayout || gotSchema) return Ret::err(DecodeError::Malformed);  // >1 payload key
                auto dr = decodeStoreDescriptor(r);
                if (!dr) return Ret::err(dr.error());
                e.fieldOffset = storeMark;
                if (!cat.poolAddStore(dr.value())) return Ret::err(DecodeError::CapacityExceeded);
                e.fieldCount = 1;
                gotStore = true;
                break;
            }
            case 13: {  // RFC-017 replay_depth
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFFFF) return Ret::err(DecodeError::Malformed);
                e.replayDepth = uint16_t(v.value());
                e.hasReplayDepth = true;
                break;
            }
            case 14: {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFFFF) return Ret::err(DecodeError::Malformed);
                e.settingChannel = uint16_t(v.value());
                e.hasSettingChannel = true;
                break;
            }
            case 15: {  // RFC-014/023 stream_kind (STREAM only; absent = 0 samples)
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                e.streamKind = uint8_t(v.value());
                break;
            }
            case 16: {  // RFC-048 (Phase C2) rank
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                e.rank = uint8_t(v.value());
                e.hasRank = true;
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
    // Exactly one payload key, and it must be the one this entry's CLASS says.
    const int payloadKeys = int(gotLayout) + int(gotSchema) + int(gotStore);
    if (payloadKeys != 1) return Ret::err(DecodeError::Malformed);
    switch (e.form()) {
        case FieldForm::Layout:
            if (!gotLayout) return Ret::err(DecodeError::Malformed);
            break;
        case FieldForm::Schema:
            if (!gotSchema) return Ret::err(DecodeError::Malformed);
            break;
        case FieldForm::Store:
            if (!gotStore) return Ret::err(DecodeError::Malformed);
            break;
    }
    return Ret::ok(e);
}

}  // namespace detail

// Decodes `in` into `cat` (any prior contents are discarded). Enforces
// everything §5.3/§8.1/§8.3 require: ascending entry ids, exactly one of
// keys 8/9/12 per entry and matching its class (more than one -> Malformed;
// none -> Malformed), capacity limits (the catalog's entry AND pool
// capacities, CatalogEntry::kMaxFields), and limits::catalog_max_entry_bytes
// (each entry is read through a reader window capped at it, so a forged
// oversize entry fails Truncated rather than inducing an unbounded scan —
// RFC-028). Unknown map keys are skipped per §4.3.
//
// Decodes IN PLACE. The previous implementation built a whole
// BasicCatalog temporary on the stack and assigned it on success — at
// realistic catalog sizes that is a tens-of-KiB stack frame, the exact bug
// class that has already blown a FreeRTOS task stack on this project. The
// visible consequence: on failure `cat` is left cleared/partially filled
// rather than untouched. A failed decode yields no usable catalog either
// way, so callers must check the Result before reading `cat` — they always
// did.
template <size_t E, size_t L, size_t S, size_t B, size_t T>
Result<std::monostate, DecodeError> decodeCatalog(std::span<const std::byte> in, BasicCatalog<E, L, S, B, T>& cat) {
    using Ret = Result<std::monostate, DecodeError>;

    cat.clear();

    // The array header is read by its own transient reader (file-level
    // depth note) — discarded immediately after, never used for entries.
    CborReader headerReader(in);
    auto nR = headerReader.readArrayHeader();
    if (!nR) return Ret::err(nR.error());
    if (nR.value() > E) return Ret::err(DecodeError::CapacityExceeded);
    size_t pos = headerReader.bytesConsumed();

    bool haveFirst = false;
    uint16_t prevId = 0;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        if (pos > in.size()) return Ret::err(DecodeError::Truncated);

        // Fresh reader per entry (file-level depth note): full depth-4
        // budget, independent of headerReader and every other entry. The
        // window is ALSO capped at limits::catalog_max_entry_bytes — §8.1
        // says an entry is an independently-decodable document and RFC-028
        // bounds what one may cost, so an entry claiming more simply runs
        // out of input.
        const size_t avail = in.size() - pos;
        const size_t window = avail < size_t(limits::catalog_max_entry_bytes)
                                  ? avail
                                  : size_t(limits::catalog_max_entry_bytes);
        CborReader r(in.subspan(pos, window));
        auto er = detail::decodeEntry(r, cat);
        if (!er) return Ret::err(er.error());
        const CatalogEntry& e = er.value();

        if (haveFirst && e.id <= prevId) return Ret::err(DecodeError::Malformed);  // §8.3 ascending
        prevId = e.id;
        haveFirst = true;

        if (!cat.addEntryVerbatim(e)) return Ret::err(DecodeError::CapacityExceeded);
        pos += r.bytesConsumed();
    }
    return Ret::ok(std::monostate{});
}

}  // namespace slopsync
