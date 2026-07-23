// slopsync-core — catalog data model, SPEC §8.1 / schema/catalog.cddl.
// Pure data structures (fixed capacity, no heap); the codec + etag live in
// wire/catalog_codec.hpp + wire/catalog_etag.hpp. CDDL map keys are cited on
// each field so the codec cannot drift from Appendix C.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "slopsync/generated/registry_constants.hpp"

namespace slopsync {

// A packed field of a STATE/STREAM layout (CDDL `layout-field`).
struct LayoutField {
    std::string_view name;              // key 1, 1..24 bytes
    PackedFieldType type;               // key 2
    std::string_view unit;              // key 3, 0..8 bytes ("mm", "flag", ...)
    float scale = 1.0f;                 // key 4: wire = physical * scale
    bool hasMin = false, hasMax = false;
    float min = 0.0f;                   // key 5 (optional)
    float max = 0.0f;                   // key 6 (optional)
    // key 7 (bitfield8 only): bit meanings. Fixed 8 slots; empty = unused bit.
    std::array<std::string_view, 8> bits{};

    constexpr size_t wireSize() const {
        switch (type) {
            case PackedFieldType::u8:
            case PackedFieldType::i8:
            case PackedFieldType::bitfield8: return 1;
            case PackedFieldType::u16:
            case PackedFieldType::i16: return 2;
            default: return 4;  // u32/i32/f32
        }
    }
};

// A CBOR field of an INTENT/EVENT schema (CDDL `schema-field`).
enum class CborFieldType : uint8_t { uint_t = 0, int_t = 1, f32_t = 2, bool_t = 3, tstr_t = 4, bstr_t = 5 };

struct SchemaField {
    uint8_t key = 0;                    // the sub-map integer key this field uses
    std::string_view name;              // key 1
    CborFieldType type;                 // key 2
    std::string_view unit;              // key 3
    bool hasMin = false, hasMax = false;
    float min = 0.0f;                   // key 5 (optional)
    float max = 0.0f;                   // key 6 (optional)
};

enum class Direction : uint8_t { h2c = 0, c2h = 1 };

// One channel entry (CDDL `channel-entry`). Exactly one of layout/schema is
// populated, matching the class: STATE|STREAM -> layout, INTENT|EVENT -> schema.
struct CatalogEntry {
    uint16_t id = 0;                    // key 1
    std::string_view name;              // key 2, 1..32 bytes
    ChannelClass cls = ChannelClass::STATE;   // key 3
    Direction dir = Direction::h2c;     // key 4
    AccessLevel access = AccessLevel::viewer; // key 5
    float maxRateHz = 0.0f;             // key 6 (0.0 = on-change only)
    Priority defaultPriority = Priority::normal;  // key 7

    static constexpr size_t kMaxFields = 8;
    uint8_t fieldCount = 0;
    std::array<LayoutField, kMaxFields> layout{};   // key 8 (packed classes)
    std::array<SchemaField, kMaxFields> schema{};   // key 9 (CBOR classes)

    bool usesLayout() const {
        return cls == ChannelClass::STATE || cls == ChannelClass::STREAM;
    }
    // Total packed payload size of a layout-class entry (SPEC §9.1: STATE must
    // fit limits::min_transport_payload unfragmented — conformance-checkable).
    constexpr size_t layoutWireSize() const {
        size_t s = 0;
        for (size_t i = 0; i < fieldCount; ++i) s += layout[i].wireSize();
        return s;
    }
};

// Fixed-capacity catalog. Entries MUST be kept sorted ascending by id — the
// etag (§8.3) is computed over the deterministic encoding in that order.
template <size_t Capacity = limits::catalog_max_entries>
struct BasicCatalog {
    uint16_t count = 0;
    std::array<CatalogEntry, Capacity> entries{};

    const CatalogEntry* find(uint16_t id) const {
        for (uint16_t i = 0; i < count; ++i)
            if (entries[i].id == id) return &entries[i];
        return nullptr;
    }
};

// The workhorse alias for tests/tools; hubs pick a Capacity fitting their RAM.
using Catalog32 = BasicCatalog<32>;

}  // namespace slopsync
