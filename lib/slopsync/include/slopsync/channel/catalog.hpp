// slopsync-core — catalog data model, SPEC §8.1 / schema/catalog.cddl.
// Pure data structures (fixed capacity, no heap); the codec + etag live in
// wire/catalog_codec.hpp + wire/catalog_etag.hpp. CDDL map keys are cited on
// each field so the codec cannot drift from Appendix C.
//
// ---------------------------------------------------------------------------
// STORAGE MODEL — shared POOLS, not per-entry arrays
// ---------------------------------------------------------------------------
// A CatalogEntry does NOT own its payload. It carries (fieldOffset,
// fieldCount) indices into ONE of three pools owned by the catalog, selected
// entirely by the entry's CLASS:
//
//   * layoutPool  — LayoutField,     STATE|STREAM entries (CDDL key 8)
//   * schemaPool  — SchemaField,     INTENT|EVENT entries (CDDL key 9)
//   * storePool   — StoreDescriptor, STORE entries        (CDDL key 12)
//
// Separate pools (rather than one union'd pool) because the three payload
// kinds are mutually exclusive per entry BY CLASS: an entry never needs a slot
// in a pool it doesn't use, so there is no union and zero waste. A STORE entry
// carries exactly one descriptor, so its fieldCount is 1 — the (offset,count)
// idiom is uniform across all three forms and nothing has to special-case
// "entries that have no fields".
//
// A FOURTH pool holds the variable-length TEXT that hangs off fields:
//
//   * labelPool   — std::string_view, with a parallel labelAccessPool
//                   (AccessLevel, one per slot)
//
// It backs bitfield8 bit labels (CDDL layout-field key 7), RFC-009 `options`
// label arrays (key 10 on both field kinds), and — through the parallel access
// array at the SAME offsets — RFC-009 `option_access` (schema-field key 17,
// index-aligned with key 10 by definition). A LabelRef is 4 bytes; the labels
// themselves are paid for once, by whoever actually declares any.
//
// WHY POOLS (measured, xtensa 32-bit):
//   * M2a: the previous model gave every entry BOTH a std::array<LayoutField,8>
//     AND a std::array<SchemaField,8> — 800 + 288 B per entry, of which exactly
//     one array was ever populated — making sizeof(CatalogEntry) 1112 B and a
//     32-entry catalog 34.8 KiB. Worse, field capacity was UNIFORM: one channel
//     needing ~50 fields would have forced every entry to 50, projecting a
//     48-entry catalog to 320 KiB.
//   * M2b: LayoutField itself carried std::array<std::string_view,8> bits —
//     64 of its 100 bytes — used by 2 of the device catalog's 42 layout fields,
//     i.e. 12.8 KiB of dead weight at 200 pool slots. Interning bit labels
//     drops that to a 4-byte LabelRef: LayoutField would have been 40 B on its
//     own. The reclaimed 60 B is what pays for the RFC-009 annotation block,
//     which lands LayoutField back at 92 B — measured, xtensa:
//         LayoutField 100 -> 92,  SchemaField 36 -> 92,  CatalogEntry 24 -> 40,
//         Catalog32 21.98 KiB -> 24.76 KiB (+2.8 KiB) for the ENTIRE
//         self-describing-settings surface plus STORE descriptors.
//     Almost all of that increase is annotation storage on pool slots nothing
//     has annotated yet; the one lever if a hub needs it back is the pool
//     capacities in the BasicCatalog alias it chooses, not the field layout.
//
// CONSEQUENCE FOR CALLERS: reach a field list, a store descriptor, or a label
// array through the OWNING catalog — cat.layoutFields(entry) /
// cat.schemaFields(entry) / cat.storeDescriptor(entry) / cat.bitLabels(field) /
// cat.optionLabels(field) — never do pointer math on the pools yourself. A
// CatalogEntry or LayoutField copied out of its catalog (see the append-only
// prefix-parse idiom in wire/packed/layout_codec.hpp) stays valid ONLY against
// that same catalog.
//
// AUTHORING: catalogs are built through the out-param builder API below
// (addEntry + addLayoutField/addBitfieldField/addSelectField/addSchemaField/
// addSelectSchemaField/addStoreDescriptor), never returned by value. A
// realistically-sized catalog is tens of KiB; a by-value return or copy is a
// stack temporary of that size, and this project has already had a FreeRTOS
// task stack blown by exactly that bug class. All adders are bounds-checked
// and latch a sticky `overflow` flag (the library has no exceptions).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

#include "slopsync/generated/registry_constants.hpp"

namespace slopsync {

// ---------------------------------------------------------------------------
// RFC-009 annotation value types
// ---------------------------------------------------------------------------

// CDDL `setting-default = int / float32 / bool / tstr` (key 9 on both field
// kinds): the FACTORY value of a setting, same type as the field it annotates.
// A tagged union rather than four optional members — 16 B on xtensa, of which
// the string_view is the only part a numeric field wastes.
struct SettingDefault {
    enum class Kind : uint8_t { None = 0, Int, Float, Bool, Tstr };

    std::string_view s{};                   // Kind::Tstr only
    union Num {
        int32_t i;
        float f;
    } num{.i = 0};                          // Int / Float / Bool (0|1)
    Kind kind = Kind::None;

    constexpr bool has() const { return kind != Kind::None; }
    constexpr int32_t asInt() const { return num.i; }
    constexpr float asFloat() const { return num.f; }
    constexpr bool asBool() const { return num.i != 0; }

    static constexpr SettingDefault ofInt(int32_t v) {
        SettingDefault d{};
        d.kind = Kind::Int;
        d.num.i = v;
        return d;
    }
    static constexpr SettingDefault ofFloat(float v) {
        SettingDefault d{};
        d.kind = Kind::Float;
        d.num.f = v;
        return d;
    }
    static constexpr SettingDefault ofBool(bool v) {
        SettingDefault d{};
        d.kind = Kind::Bool;
        d.num.i = v ? 1 : 0;
        return d;
    }
    static constexpr SettingDefault ofTstr(std::string_view v) {
        SettingDefault d{};
        d.kind = Kind::Tstr;
        d.s = v;
        return d;
    }

    // Kind-aware: never reads an inactive union member.
    friend constexpr bool operator==(const SettingDefault& a, const SettingDefault& b) {
        if (a.kind != b.kind) return false;
        switch (a.kind) {
            case Kind::None: return true;
            case Kind::Int:
            case Kind::Bool: return a.num.i == b.num.i;
            case Kind::Float: return a.num.f == b.num.f;
            case Kind::Tstr: return a.s == b.s;
        }
        return false;
    }
};

// A (offset,count) slice of the owning catalog's LABEL POOL. Resolve it with
// cat.labels(ref) / cat.labelAccess(ref) — or, more usually, the field-level
// helpers cat.bitLabels(f) / cat.optionLabels(f) / cat.optionAccess(f).
// count == 0 means ABSENT (and `offset` is then meaningless; it is left at
// kNone so a stale ref can never silently resolve to slot 0).
struct LabelRef {
    static constexpr uint16_t kNone = 0xFFFF;
    // Storage bound: `count` is a uint8_t, so a single label array can hold at
    // most 255 entries. The codec rejects anything larger as Malformed rather
    // than truncating.
    static constexpr uint32_t kMaxCount = 255;

    uint16_t offset = kNone;
    uint8_t count = 0;

    constexpr bool empty() const { return count == 0; }
};

// A packed field of a STATE/STREAM layout (CDDL `layout-field`).
//
// Keys 1..7 are the wire description; keys 8..15 are the RFC-009 ANNOTATION
// BLOCK — every one optional and ignorable. The deterministic CBOR profile
// (§5.3) does not emit absent optional keys, so a field that carries no
// annotations encodes to EXACTLY the bytes it did before this block existed.
// That is what keeps the frozen mini-catalog fixture's length + etag pins
// valid across this milestone.
//
// MEMBER ORDER IS LOAD-BEARING: designated-initializer authoring must list
// members in declaration order, so the annotation block is APPENDED at the
// tail and every pre-M2b authoring site still compiles unchanged.
struct LayoutField {
    std::string_view name{};            // key 1, 1..24 bytes
    PackedFieldType type = PackedFieldType::u8;  // key 2
    std::string_view unit{};            // key 3, 0..8 bytes ("mm", "flag", ...)
    float scale = 1.0f;                 // key 4: wire = physical * scale
    bool hasMin = false, hasMax = false;
    float min = 0.0f;                   // key 5 (optional)
    float max = 0.0f;                   // key 6 (optional)
    // key 7 (bitfield8 only): bit meanings, INDEX-ALIGNED into the label pool
    // — slot i is bit i, an empty view is an unnamed bit, and `count` stops at
    // the last NAMED bit (trailing unnamed bits cost nothing). Author with
    // addBitfieldField().
    LabelRef bits{};

    // ---- RFC-009 annotation block (all optional) ----------------------------
    // Ordered widest-first so the block costs 56 B rather than 64 — the four
    // one-byte members share one tail word instead of each opening a padding
    // hole. Declaration order is authoring order (designated initializers), so
    // the presence flags trail the values they gate; that reads fine and is
    // what the packing buys.
    //
    // key 10: single-select labels; WIRE VALUE = ARRAY INDEX. Author with
    // addSelectField().
    LabelRef options{};
    SettingDefault dflt{};              // key 9: factory value
    std::string_view group{};           // key 11: card heading within the category
    std::string_view desc{};            // key 12: tooltip, <= limits::desc_max_bytes
    std::string_view role{};            // key 13: field_roles vocabulary (extensible)
    float step = 0.0f;                  // key 14: range granularity hint
    // key 8: the CBOR key in the entry's `settingChannel` that WRITES this
    // field. PRESENT = this field is a writable SETTING (stored config);
    // ABSENT = read-only (effective state or telemetry — display it, never
    // write it back into a setting's shadow). RFC-003's stored/effective
    // distinction IS this presence test; there is deliberately no second flag.
    uint8_t settingKey = 0;
    uint8_t flags = 0;                  // key 15: setting_flags bitmask
    bool hasSettingKey = false;
    bool hasStep = false;

    // ---- RFC-048 rendering metamodel (Phase C2, all optional) ---------------
    // APPENDED AT THE TAIL, same authoring-order rule as the RFC-009 block
    // above. Keys 19-23 (wire/catalog_codec.hpp's file banner explains why
    // they start at 19, not 16): registry vocabulary ids
    // (generated/registry_constants.hpp) ui_ranks, value_aspects,
    // value_scopes, value_provenance, unit_ids. Absent = the RENDERING.md
    // default for that axis (rank->detail, aspect->live, scope->session,
    // provenance->actual, unit_id-> render the tstr `unit` above verbatim).
    // has-flag BEFORE value, matching this struct's own hasMin/min convention,
    // so `.hasRank = true, .rank = x` authors in declaration order.
    bool hasRank = false;
    uint8_t rank = 0;
    bool hasAspect = false;
    uint8_t aspect = 0;
    bool hasScope = false;
    uint8_t scope = 0;
    bool hasProvenance = false;
    uint8_t provenance = 0;
    bool hasUnitId = false;
    uint8_t unitId = 0;

    // Bytes this field occupies in a packed payload. EXHAUSTIVE by design:
    // no `default:` arm, so adding a PackedFieldType without giving it a
    // width is a -Wswitch compile error rather than a silent wrong answer.
    // (It used to end in `default: return 4`, which made the str16/str32/
    // str64 types report 4 bytes and shifted the offset of every field after
    // them — silent wire corruption. Do not reintroduce a default arm.)
    constexpr size_t wireSize() const {
        switch (type) {
            case PackedFieldType::u8:
            case PackedFieldType::i8:
            case PackedFieldType::bitfield8: return 1;
            case PackedFieldType::u16:
            case PackedFieldType::i16: return 2;
            case PackedFieldType::u32:
            case PackedFieldType::i32:
            case PackedFieldType::f32: return 4;
            case PackedFieldType::str16: return 16;   // fixed-width, NUL-padded UTF-8
            case PackedFieldType::str32: return 32;
            case PackedFieldType::str64: return 64;
        }
        return 0;  // unreachable for a valid enum value; keeps -Wreturn-type quiet
    }

    // True for the fixed-width NUL-padded UTF-8 string types (RFC-026), which
    // carry no numeric value and therefore no scale.
    constexpr bool isString() const {
        return type == PackedFieldType::str16 || type == PackedFieldType::str32 ||
               type == PackedFieldType::str64;
    }
};

// A CBOR field of an INTENT/EVENT schema (CDDL `schema-field`).
enum class CborFieldType : uint8_t { uint_t = 0, int_t = 1, f32_t = 2, bool_t = 3, tstr_t = 4, bstr_t = 5 };

// Annotation keys are NUMBERED IDENTICALLY to layout-field wherever the
// meaning matches, so one reader handles both. Key 8 (setting_key) is absent
// by construction: for a schema-field the MAP KEY *is* the write key. Keys 16
// (per-op access) and 17 (per-option access) exist only here.
struct SchemaField {
    uint8_t key = 0;                    // the sub-map integer key this field uses
    std::string_view name{};            // key 1
    CborFieldType type = CborFieldType::uint_t;  // key 2
    std::string_view unit{};            // key 3
    bool hasMin = false, hasMax = false;
    float min = 0.0f;                   // key 5 (optional)
    float max = 0.0f;                   // key 6 (optional)

    // ---- RFC-009 annotation block (all optional) ----------------------------
    // Widest-first, same packing rationale as LayoutField's block.
    SettingDefault dflt{};              // key 9
    LabelRef options{};                 // key 10 (author with addSelectSchemaField)
    std::string_view group{};           // key 11
    std::string_view desc{};            // key 12
    std::string_view role{};            // key 13, incl. RFC-019 `action.<name>`
    float step = 0.0f;                  // key 14
    uint8_t flags = 0;                  // key 15
    AccessLevel access = AccessLevel::watch;  // key 16: per-op role override
    bool hasAccess = false;
    bool hasStep = false;
    // key 17: per-OPTION minimum role, index-aligned with key 10. Stored in the
    // label pool's PARALLEL access array at the very same offsets, so it needs
    // no ref of its own — only a presence bit.
    bool hasOptionAccess = false;

    // ---- RFC-048 rendering metamodel (Phase C2, all optional) ---------------
    // Numbered IDENTICALLY to LayoutField's own 19..23 (shared numbering, see
    // that struct's comment for per-key meaning) — rare on a schema field
    // (INTENT/EVENT payloads are mostly verbs, not display values) but not
    // disallowed, e.g. an aspect-group `action.reset` targeting a specific
    // aspect (RENDERING.md §5.4).
    bool hasRank = false;
    uint8_t rank = 0;
    bool hasAspect = false;
    uint8_t aspect = 0;
    bool hasScope = false;
    uint8_t scope = 0;
    bool hasProvenance = false;
    uint8_t provenance = 0;
    bool hasUnitId = false;
    uint8_t unitId = 0;
};

// A STORE-class entry's descriptor (CDDL `store-descriptor`, RFC-021). The
// store's ITEMS never appear in the catalog — they move over BLOB_REQ/
// BLOB_CHUNK in blob_ns::store, addressed by {store_id, slot}.
struct StoreDescriptor {
    uint8_t storeId = 0;                // key 1: u8, unique per hub
    std::string_view kind{};            // key 2: namespaced, e.g. "pattern.frayd", 1..32
    uint16_t capacity = 0;              // key 3: item slots
    uint32_t perItemMax = 0;            // key 4: max encoded payload bytes
    uint16_t nameMax = 0;               // key 5: max item-name bytes
};

enum class Direction : uint8_t { h2c = 0, c2h = 1 };

// Which pool an entry's (fieldOffset, fieldCount) indexes. Derived from the
// entry's CLASS and nothing else — see CatalogEntry::form().
enum class FieldForm : uint8_t { Layout, Schema, Store };

// One channel entry (CDDL `channel-entry`). Its payload lives in the owning
// catalog's layout pool (STATE|STREAM), schema pool (INTENT|EVENT), or store
// pool (STORE) — which one is determined ENTIRELY by `cls`, so no two are ever
// both populated.
//
// Member ORDER is load-bearing twice over: designated-initializer authoring
// must list keys in declaration order (so the M2b entry-level annotations are
// APPENDED at the tail), and the small members are packed to keep
// sizeof(CatalogEntry) down.
struct CatalogEntry {
    uint16_t id = 0;                    // key 1
    std::string_view name{};            // key 2, 1..32 bytes
    ChannelClass cls = ChannelClass::STATE;   // key 3
    Direction dir = Direction::h2c;     // key 4
    AccessLevel access = AccessLevel::watch; // key 5 — the FLOOR; a schema-field's
                                             // key 16 may raise or lower it per op
    float maxRateHz = 0.0f;             // key 6 (0.0 = on-change only)
    Priority defaultPriority = Priority::normal;  // key 7

    // Upper bound on the number of fields ONE entry may declare. This is not a
    // storage limit (the pools are) — it is the codec's sanity bound on keys
    // 8/9 and the width of the encoder's schema-key sort scratch.
    //
    // M2b RAISED IT 8 -> 64 to unlock the ~50-field flattened advanced-pattern
    // channel. Costs: (a) the encoder's std::array<uint8_t,kMaxFields> sort
    // scratch grows 8 B -> 64 B of stack, once, in the schema branch of
    // encodeEntry (nothing recursive, nothing per-field); (b) entries with
    // 9..64 fields now DECODE instead of being rejected Malformed — which is
    // precisely the intended semantic change. The real ceiling on a fat entry
    // is now limits::catalog_max_entry_bytes (4096), enforced by the codec and
    // reported by conformance::checkCatalog.
    static constexpr size_t kMaxFields = 64;

    uint8_t fieldCount = 0;             // payload slots owned, starting at fieldOffset
                                        // (STORE: always 1 — its descriptor)
    uint16_t fieldOffset = 0;           // index into the OWNING catalog's pool

    // ---- RFC-009 entry-level annotations (all optional) ---------------------
    bool hasCategory = false;
    // key 10: registry ui_categories (RFC-047/048, Phase C2) — 1..14 registered,
    // 0x40..0x7E vendor/device-defined. Was setting_categories (0..4) pre-Phase-
    // C2: same wire key, new vocabulary, a pre-tag restructuring the registry
    // header permits before v1.0.
    uint8_t category = 0;
    std::string_view categoryLabel{};   // key 11: REQUIRED iff category is in the vendor range (0x40..0x7E)
    // key 13 (RFC-017): how many PAST events this channel replays to a newly
    // granted subscriber. PRESENCE IS THE EXCEPTION to §9.4's no-replay rule —
    // a channel that declares one is saying "a client that connects after a
    // fault must still be able to see what happened", which is why the log
    // channel (0x0008) exists at all. Absent/0 = §9.4's default: events are
    // edges, never replayed.
    bool hasReplayDepth = false;
    uint16_t replayDepth = 0;
    bool hasSettingChannel = false;
    uint16_t settingChannel = 0;        // key 14: the INTENT channel whose CBOR keys
                                        // this entry's `settingKey`s refer to.
                                        // REQUIRED iff any field carries one.
    // key 15 (RFC-014/023): registry `stream_kinds` — STREAM class only. WHAT
    // a STREAM sample IS (an instantaneous point vs a commanded time extent),
    // replacing the M5 unit-string heuristic (isTimeUnit/isSegmentLayout) that
    // this milestone deleted: two conforming hubs could disagree on whether
    // "ms"/"msec"/"millis" meant the same thing and therefore shed
    // differently under identical congestion. 0 (stream_kinds::samples) is
    // the default and is OMITTED on the wire — see isSegmentClass() below for
    // the one place this field is read.
    uint8_t streamKind = 0;             // stream_kinds::samples (0) / ::segments (1)

    // key 16 (RFC-048, Phase C2): registry ui_ranks — how much THIS CHANNEL
    // matters (RENDERING.md §4), independent of any per-field rank (key 19 on
    // its own fields — the two are separate axes, not an inheritance chain).
    // Absent = detail (2).
    bool hasRank = false;
    uint8_t rank = 0;

    // EXHAUSTIVE by design (no `default:` arm): adding a ChannelClass without
    // giving it a payload form is a -Wswitch compile error, not a silent
    // mis-pooling. STORE used to fall through to the schema pool because
    // usesLayout() was a bare bool — that is why this is an enum now.
    constexpr FieldForm form() const {
        switch (cls) {
            case ChannelClass::STATE:
            case ChannelClass::STREAM: return FieldForm::Layout;
            case ChannelClass::INTENT:
            case ChannelClass::EVENT: return FieldForm::Schema;
            case ChannelClass::STORE: return FieldForm::Store;
        }
        return FieldForm::Layout;  // unreachable for a valid enum value
    }

    constexpr bool usesLayout() const { return form() == FieldForm::Layout; }
    constexpr bool usesSchema() const { return form() == FieldForm::Schema; }
    constexpr bool usesStore() const { return form() == FieldForm::Store; }
};

// Total packed payload size of a layout field list (SPEC §9.1: a STATE
// payload must fit limits::min_transport_payload unfragmented —
// conformance-checkable). Free function so it works on any span, including
// the truncated prefix spans the append-only rule (§5.4) produces.
constexpr size_t layoutWireSize(std::span<const LayoutField> fields) {
    size_t s = 0;
    for (const LayoutField& f : fields) s += f.wireSize();
    return s;
}

// Fixed-capacity catalog: `Entries` channel entries drawing payload from a
// `LayoutFields`-slot layout pool, a `SchemaFields`-slot schema pool, a
// `Stores`-slot store pool, and a `Labels`-slot label pool (bit labels +
// option labels + their per-option access levels).
// Entries MUST be kept sorted ascending by id — the etag (§8.3) is computed
// over the deterministic encoding in that order, and the authoring API
// appends, so authoring order IS wire order.
template <size_t Entries = 32, size_t LayoutFields = 200, size_t SchemaFields = 48,
          size_t Labels = 128, size_t Stores = 4>
struct BasicCatalog {
    static constexpr size_t kEntryCapacity = Entries;
    static constexpr size_t kLayoutCapacity = LayoutFields;
    static constexpr size_t kSchemaCapacity = SchemaFields;
    static constexpr size_t kLabelCapacity = Labels;
    static constexpr size_t kStoreCapacity = Stores;

    uint16_t count = 0;        // entries in use
    uint16_t layoutUsed = 0;   // layout-pool slots in use
    uint16_t schemaUsed = 0;   // schema-pool slots in use
    uint16_t labelUsed = 0;    // label-pool slots in use
    uint16_t storeUsed = 0;    // store-pool slots in use
    // Sticky: set by ANY adder that could not fit. Never silently truncates
    // without recording it — check ok() after building.
    bool overflow = false;

    std::array<CatalogEntry, Entries> entries{};
    std::array<LayoutField, LayoutFields> layoutPool{};
    std::array<SchemaField, SchemaFields> schemaPool{};
    std::array<StoreDescriptor, Stores> storePool{};
    std::array<std::string_view, Labels> labelPool{};
    // Parallel to labelPool, slot for slot: the RFC-009 `option_access` of the
    // option label at the same index. Meaningless (and left at watch) for bit
    // labels and for options whose field declares no option_access.
    std::array<AccessLevel, Labels> labelAccessPool{};

    bool ok() const { return !overflow; }

    void clear() {
        count = 0;
        layoutUsed = 0;
        schemaUsed = 0;
        labelUsed = 0;
        storeUsed = 0;
        overflow = false;
    }

    // ---- Authoring (the human-facing builder) -------------------------------
    // Usage, and the ONLY supported authoring shape:
    //
    //   c.addEntry({.id = 0x0080, .name = "motion", .cls = ChannelClass::STATE,
    //               .dir = Direction::h2c, .access = AccessLevel::watch,
    //               .maxRateHz = 60.0f, .defaultPriority = Priority::elevated});
    //   c.addLayoutField({.name = "pos_10um", .type = PackedFieldType::u16, ...});
    //   c.addBitfieldField({.name = "flags", .type = PackedFieldType::bitfield8,
    //                       .unit = "flag", .scale = 1.0f},
    //                      {"homed", "homing", "", "paused"});   // index-aligned
    //   c.addSelectField({.name = "mode", .type = PackedFieldType::u8, ...},
    //                    {"off", "chase", "waveform"});          // value = index
    //
    // Payload attaches to the MOST RECENTLY added entry (pool storage per
    // entry is contiguous, so an entry's fields must be appended before the
    // next entry is opened). Entries must be added in ascending id order.

    // Opens a new entry. `header`'s fieldCount/fieldOffset are ignored and
    // reset — the catalog assigns the pool offset from the class. Returns a
    // pointer to the stored entry, or nullptr (and latches overflow) if full.
    CatalogEntry* addEntry(CatalogEntry header) {
        if (count >= Entries) {
            overflow = true;
            return nullptr;
        }
        header.fieldCount = 0;
        switch (header.form()) {
            case FieldForm::Layout: header.fieldOffset = layoutUsed; break;
            case FieldForm::Schema: header.fieldOffset = schemaUsed; break;
            case FieldForm::Store: header.fieldOffset = storeUsed; break;
        }
        entries[count] = header;
        return &entries[count++];
    }

    // Appends one packed field to the most recent entry (which must be a
    // STATE/STREAM entry whose fields are the layout pool's tail).
    bool addLayoutField(const LayoutField& f) {
        if (count == 0 || layoutUsed >= LayoutFields) {
            overflow = true;
            return false;
        }
        CatalogEntry& e = entries[count - 1];
        if (!e.usesLayout() || size_t(e.fieldOffset) + e.fieldCount != layoutUsed) {
            overflow = true;  // wrong payload form, or fields added out of order
            return false;
        }
        if (e.fieldCount >= CatalogEntry::kMaxFields) {
            overflow = true;
            return false;
        }
        layoutPool[layoutUsed++] = f;
        ++e.fieldCount;
        return true;
    }

    // addLayoutField + interned bitfield8 bit labels (CDDL key 7). `bits` is
    // INDEX-ALIGNED: element i names bit i, "" leaves bit i unnamed, and
    // trailing unnamed bits may simply be omitted.
    bool addBitfieldField(LayoutField f, std::initializer_list<std::string_view> bits) {
        if (!internLabels(bits, /*maxCount=*/8, f.bits)) return false;
        return addLayoutField(f);
    }

    // addLayoutField + interned single-select option labels (CDDL key 10).
    // The WIRE VALUE of the field is the INDEX into this list.
    bool addSelectField(LayoutField f, std::initializer_list<std::string_view> options) {
        if (!checkOptionLabels(options)) return false;
        if (!internLabels(options, LabelRef::kMaxCount, f.options)) return false;
        return addLayoutField(f);
    }

    // Appends one CBOR field to the most recent entry (INTENT/EVENT).
    bool addSchemaField(const SchemaField& f) {
        if (count == 0 || schemaUsed >= SchemaFields) {
            overflow = true;
            return false;
        }
        CatalogEntry& e = entries[count - 1];
        if (!e.usesSchema() || size_t(e.fieldOffset) + e.fieldCount != schemaUsed) {
            overflow = true;
            return false;
        }
        if (e.fieldCount >= CatalogEntry::kMaxFields) {
            overflow = true;
            return false;
        }
        schemaPool[schemaUsed++] = f;
        ++e.fieldCount;
        return true;
    }

    // addSchemaField + interned option labels, and optionally the index-aligned
    // per-option access levels (CDDL key 17). Passing a non-empty
    // `optionAccess` whose length differs from `options` is an authoring error
    // and latches overflow.
    bool addSelectSchemaField(SchemaField f, std::initializer_list<std::string_view> options,
                              std::initializer_list<AccessLevel> optionAccess = {}) {
        if (optionAccess.size() != 0 && optionAccess.size() != options.size()) {
            overflow = true;
            return false;
        }
        if (!checkOptionLabels(options)) return false;
        if (!internLabels(options, LabelRef::kMaxCount, f.options)) return false;
        if (optionAccess.size() != 0) {
            const AccessLevel* a = optionAccess.begin();
            for (uint8_t i = 0; i < f.options.count; ++i) {
                labelAccessPool[size_t(f.options.offset) + i] = a[i];
            }
            f.hasOptionAccess = true;
        }
        return addSchemaField(f);
    }

    // Attaches the STORE descriptor to the most recent entry (which must be a
    // STORE-class entry). Exactly one per entry — a second call is an
    // authoring error.
    bool addStoreDescriptor(const StoreDescriptor& d) {
        if (count == 0 || storeUsed >= Stores) {
            overflow = true;
            return false;
        }
        CatalogEntry& e = entries[count - 1];
        if (!e.usesStore() || e.fieldCount != 0 || size_t(e.fieldOffset) != storeUsed) {
            overflow = true;
            return false;
        }
        storePool[storeUsed++] = d;
        e.fieldCount = 1;
        return true;
    }

    // Copies one entry AND its payload out of another catalog, preserving
    // content exactly (fields, labels and store descriptors are re-pooled into
    // this catalog and the refs remapped). The "superset catalog" idiom: build
    // on top of a frozen fixture without duplicating its content or copying a
    // whole catalog by value.
    template <size_t E2, size_t L2, size_t S2, size_t B2, size_t T2>
    bool addEntryFrom(const BasicCatalog<E2, L2, S2, B2, T2>& src, const CatalogEntry& e) {
        if (addEntry(e) == nullptr) return false;
        switch (e.form()) {
            case FieldForm::Layout:
                for (const LayoutField& f : src.layoutFields(e)) {
                    LayoutField copy = f;
                    if (!reintern(src, f.bits, copy.bits)) return false;
                    if (!reintern(src, f.options, copy.options)) return false;
                    if (!addLayoutField(copy)) return false;
                }
                break;
            case FieldForm::Schema:
                for (const SchemaField& f : src.schemaFields(e)) {
                    SchemaField copy = f;
                    if (!reintern(src, f.options, copy.options)) return false;
                    if (!addSchemaField(copy)) return false;
                }
                break;
            case FieldForm::Store: {
                const StoreDescriptor* d = src.storeDescriptor(e);
                if (d == nullptr) return false;
                if (!addStoreDescriptor(*d)) return false;
                break;
            }
        }
        return true;
    }

    // ---- Low-level pool API (decoders; payload arrives before the entry is
    //      complete, so offsets are captured by mark rather than assigned) ---
    uint16_t layoutMark() const { return layoutUsed; }
    uint16_t schemaMark() const { return schemaUsed; }
    uint16_t labelMark() const { return labelUsed; }
    uint16_t storeMark() const { return storeUsed; }

    bool poolAddLayoutField(const LayoutField& f) {
        if (layoutUsed >= LayoutFields) {
            overflow = true;
            return false;
        }
        layoutPool[layoutUsed++] = f;
        return true;
    }
    bool poolAddSchemaField(const SchemaField& f) {
        if (schemaUsed >= SchemaFields) {
            overflow = true;
            return false;
        }
        schemaPool[schemaUsed++] = f;
        return true;
    }
    bool poolAddStore(const StoreDescriptor& d) {
        if (storeUsed >= Stores) {
            overflow = true;
            return false;
        }
        storePool[storeUsed++] = d;
        return true;
    }
    bool poolAddLabel(std::string_view s, AccessLevel a = AccessLevel::watch) {
        if (labelUsed >= Labels) {
            overflow = true;
            return false;
        }
        labelAccessPool[labelUsed] = a;
        labelPool[labelUsed++] = s;
        return true;
    }
    // Overwrites the access level of an already-interned label slot — how a
    // decoder applies schema-field key 17, which necessarily arrives after the
    // key-10 options it is index-aligned with (§5.3 sorts map keys ascending).
    bool poolSetLabelAccess(uint16_t index, AccessLevel a) {
        if (index >= labelUsed) {
            overflow = true;
            return false;
        }
        labelAccessPool[index] = a;
        return true;
    }
    // Stores `e` verbatim — fieldOffset/fieldCount are the caller's (they
    // were captured from layoutMark()/schemaMark()/storeMark() before its
    // payload was pushed). Authoring code wants addEntry(), not this.
    bool addEntryVerbatim(const CatalogEntry& e) {
        if (count >= Entries) {
            overflow = true;
            return false;
        }
        entries[count++] = e;
        return true;
    }

    // ---- Access -------------------------------------------------------------
    const CatalogEntry* find(uint16_t id) const {
        for (uint16_t i = 0; i < count; ++i)
            if (entries[i].id == id) return &entries[i];
        return nullptr;
    }

    // `entry`'s packed fields, or an EMPTY span if it isn't a layout-class
    // entry or its (offset,count) doesn't resolve inside this catalog's pool
    // (a hand-mutated entry, or one belonging to a different catalog).
    std::span<const LayoutField> layoutFields(const CatalogEntry& entry) const {
        if (!entry.usesLayout()) return {};
        if (size_t(entry.fieldOffset) + entry.fieldCount > layoutUsed) return {};
        return std::span<const LayoutField>(layoutPool.data() + entry.fieldOffset, entry.fieldCount);
    }

    std::span<const SchemaField> schemaFields(const CatalogEntry& entry) const {
        if (!entry.usesSchema()) return {};
        if (size_t(entry.fieldOffset) + entry.fieldCount > schemaUsed) return {};
        return std::span<const SchemaField>(schemaPool.data() + entry.fieldOffset, entry.fieldCount);
    }

    // `entry`'s store descriptor, or nullptr if it isn't a STORE-class entry
    // or its descriptor doesn't resolve inside this catalog's store pool.
    const StoreDescriptor* storeDescriptor(const CatalogEntry& entry) const {
        if (!entry.usesStore() || entry.fieldCount != 1) return nullptr;
        if (size_t(entry.fieldOffset) + 1 > storeUsed) return nullptr;
        return &storePool[entry.fieldOffset];
    }

    // Resolve a LabelRef against this catalog's label pool. EMPTY span when
    // absent or unresolvable — same "belongs to another catalog" safety as
    // layoutFields()/schemaFields().
    std::span<const std::string_view> labels(LabelRef ref) const {
        if (ref.count == 0 || size_t(ref.offset) + ref.count > labelUsed) return {};
        return std::span<const std::string_view>(labelPool.data() + ref.offset, ref.count);
    }
    std::span<const AccessLevel> labelAccess(LabelRef ref) const {
        if (ref.count == 0 || size_t(ref.offset) + ref.count > labelUsed) return {};
        return std::span<const AccessLevel>(labelAccessPool.data() + ref.offset, ref.count);
    }

    // Field-level conveniences (what client code and the codec actually use).
    // bitLabels() is INDEX-ALIGNED: element i is bit i, "" = unnamed.
    std::span<const std::string_view> bitLabels(const LayoutField& f) const { return labels(f.bits); }
    std::span<const std::string_view> optionLabels(const LayoutField& f) const { return labels(f.options); }
    std::span<const std::string_view> optionLabels(const SchemaField& f) const { return labels(f.options); }
    // Per-option minimum roles, index-aligned with optionLabels(f) — EMPTY
    // unless the field actually declares key 17.
    std::span<const AccessLevel> optionAccess(const SchemaField& f) const {
        if (!f.hasOptionAccess) return {};
        return labelAccess(f.options);
    }

    // Packed payload size of a layout-class entry (see the free
    // layoutWireSize(span) above for the append-only prefix case).
    size_t layoutWireSize(const CatalogEntry& entry) const {
        return slopsync::layoutWireSize(layoutFields(entry));
    }

    // RFC-014/023: is this a segment-class STREAM channel (non-decimable,
    // schedule-carrying)? Reads the entry's explicit `streamKind` (catalog
    // key 15) ONLY — no unit-string inference. Only STREAM channels can be
    // segment-class — a STATE snapshot with a duration field is still a
    // snapshot.
    bool isSegmentClass(const CatalogEntry& entry) const {
        return entry.cls == ChannelClass::STREAM && entry.streamKind == stream_kinds::segments;
    }

  private:
    // OPTION labels (CDDL key 10) may not be empty, and this is asymmetric
    // with BIT labels on purpose: an unnamed BIT is meaningful ("bit 5 has no
    // name"), whereas an unnamed OPTION is unrenderable — there is no way for
    // a client to offer a choice with no label. The DECODER has always
    // enforced it (decodeOptions -> Malformed); the encoder never did, so a
    // hub could author an empty label, encode it happily, and ship a catalog
    // NO CONFORMING CLIENT COULD DECODE — undiscoverable until a real client
    // failed to sync. Caught here instead, at authoring time, where it shows
    // up as catalog.ok() == false in the hub's own build/test.
    //
    // Consequence for enum-valued fields whose wire values do not start at 0
    // (safety_ops starts at 1): index 0 needs a real placeholder LABEL, not
    // an empty string. See SlopSyncCatalog.h's 0x0005 entry.
    bool checkOptionLabels(std::initializer_list<std::string_view> options) {
        for (std::string_view o : options) {
            if (o.empty()) {
                overflow = true;
                return false;
            }
        }
        return true;
    }

    // Interns a label list into the pool, trimming TRAILING empty labels (an
    // unnamed high bit costs nothing) and writing the resulting ref to `out`.
    bool internLabels(std::initializer_list<std::string_view> in, uint32_t maxCount, LabelRef& out) {
        size_t n = in.size();
        const std::string_view* p = in.begin();
        while (n > 0 && p[n - 1].empty()) --n;   // trailing holes are free
        if (n == 0) {
            out = LabelRef{};
            return true;
        }
        if (n > maxCount) {
            overflow = true;
            return false;
        }
        const uint16_t mark = labelUsed;
        for (size_t i = 0; i < n; ++i) {
            if (!poolAddLabel(p[i])) return false;
        }
        out = LabelRef{mark, uint8_t(n)};
        return true;
    }

    // Copies `src`'s labels (and their access levels) into THIS catalog's pool
    // and writes the remapped ref to `out`. Used by addEntryFrom().
    template <size_t E2, size_t L2, size_t S2, size_t B2, size_t T2>
    bool reintern(const BasicCatalog<E2, L2, S2, B2, T2>& src, LabelRef ref, LabelRef& out) {
        if (ref.count == 0) {
            out = LabelRef{};
            return true;
        }
        auto text = src.labels(ref);
        auto acc = src.labelAccess(ref);
        if (text.size() != ref.count) return false;
        const uint16_t mark = labelUsed;
        for (size_t i = 0; i < text.size(); ++i) {
            if (!poolAddLabel(text[i], acc.empty() ? AccessLevel::watch : acc[i])) return false;
        }
        out = LabelRef{mark, ref.count};
        return true;
    }
};

// The workhorse alias for tests/tools and for this project's firmware hub;
// hubs pick capacities fitting their RAM. Budgets, not per-entry maxima:
// 200 layout slots is room for the whole device catalog PLUS the wide
// (~50-field) advanced-pattern channel RFC-009 wants; 192 label slots (above
// BasicCatalog's 128 default) cover every bit label and dropdown a device
// this size declares; 4 stores is RFC-021's presets + saved positions + the
// trust ledger with a spare.
// 32 -> 40 entries at M5c. RFC-009 called this out as expected ("the library's
// per-entry cap is a RAM knob that must rise for settings-dense categories"),
// and the SlopMotion tuning surface is what cashed it: 17 live-tune knobs
// cannot live on one settings channel, because `enabled_mask` is a bitfield8
// and bit i gates the i-th setting of ITS channel -- so 8 settings per channel
// is a hard ceiling. Three STATE cards sharing one INTENT writer is the shape
// that fits without widening a wire type; see SlopDrive-32's
// docs/http-plane-retirement.md 5.9.
// 28 entries were in use with 4 more landing, and stopping exactly on the old
// cap is not a margin.
//
// 40 -> 48 for the advanced-pattern channel set (SlopSyncCatalog.h 0x008E..
// 0x0094 + 0x0107): the SAME "settings-dense category, bitfield8 enabled_mask
// caps a channel at 8 settings" arithmetic as the M5c bump above, applied to
// AdvancedPattern.h's 8 base controls + 6-per-control cyclic Modifier (36
// fields) — 7 STATE cards + 1 shared INTENT writer, 8 more entries. Landed at
// 33 in use; 48 leaves real headroom rather than stopping on the new line.
//
// SchemaFields 96 -> 160 in the SAME change. 0x0107 alone is 44 schema
// fields (kIntentMaxValueFields caps what ONE wire frame carries, not what a
// catalog may DECLARE — see SlopSyncCatalog.h's 0x0107), which took the pool
// from measured 52/96 in use to 96/96 — overflow, latched silently (`ok()`
// false, no diagnostic beyond that) until measured with a scratch instrumented
// build. 160 leaves the same kind of real headroom as the entries bump rather
// than landing on the new line a second time.
using Catalog32 = BasicCatalog<48, 200, 160, 192, 4>;

}  // namespace slopsync
