#pragma once

// ConfigTypes — the parsed, protocol-agnostic model of a .bench config file.
// Constraints:
//   - Field order within a StateChannel is WIRE ORDER (positional packed
//     layout, catalog/channel/catalog.hpp LayoutField semantics) — never
//     reorder after ConfigParser has appended a field.
//   - `key` on an Intent/Event field is the CBOR sub-map key the wire uses;
//     ConfigParser enforces uniqueness and ascending order per channel.
//   - No slopsync/ include here on purpose: this layer only knows what a
//     config file SAYS. hub/BenchCatalog.h/.cpp own the translation into a
//     live slopsync::Catalog32.
// See: sim/slopbench/README.md (config format reference).

#include <cstdint>
#include <string>
#include <vector>

namespace slopbench {

enum class ClassKind : uint8_t { State, Intent, Event, Stream, Store };

enum class AccessKind : uint8_t { Watch, Control, Configure };

// STATE/STREAM (packed layout) field kinds — named identically to
// slopsync::PackedFieldType so ConfigParser's lookup table is a 1:1 string
// match with no renaming step.
enum class LayoutKind : uint8_t { U8, I8, U16, I16, U32, I32, F32, Bitfield8, Str16, Str32, Str64 };

// INTENT/EVENT (CBOR schema) field kinds — named identically to
// slopsync::CborFieldType's short forms.
enum class SchemaKind : uint8_t { Uint, Int, F32, Bool, Tstr, Bstr };

enum class AnimKind : uint8_t { None, Sine, Ramp };

struct FieldConfig {
    std::string name;
    // Exactly one of these is meaningful, selected by the owning channel's
    // ClassKind (State/Stream -> layoutType, Intent/Event -> schemaType).
    LayoutKind layoutType = LayoutKind::F32;
    SchemaKind schemaType = SchemaKind::F32;
    std::string unit;
    bool hasMin = false, hasMax = false;
    float min = 0.0f, max = 0.0f;
    bool hasDefault = false;
    float defaultValue = 0.0f;
    // INTENT/EVENT only: the wire sub-map key (required for those classes).
    bool hasKey = false;
    uint8_t key = 0;
    // STATE fields only, optional: a live auto-animation (task item 5) so a
    // client can watch telemetry move with nobody writing to it.
    AnimKind anim = AnimKind::None;
    float animHz = 0.0f, animA = 0.0f, animB = 0.0f;

    bool isNumericLayout() const {
        return layoutType != LayoutKind::Bitfield8 && !isStringLayout();
    }
    bool isStringLayout() const {
        return layoutType == LayoutKind::Str16 || layoutType == LayoutKind::Str32 ||
               layoutType == LayoutKind::Str64;
    }
    bool isNumericSchema() const {
        return schemaType == SchemaKind::Uint || schemaType == SchemaKind::Int ||
               schemaType == SchemaKind::F32;
    }
};

struct ChannelConfig {
    uint16_t id = 0;
    std::string name;
    ClassKind cls = ClassKind::State;
    AccessKind access = AccessKind::Watch;
    float rateHz = 0.0f;   // maxRateHz; 0 = on-change only
    std::string category;  // registry ui_categories name, or empty = none

    // INTENT only: the STATE channel this one's accepted writes mirror into.
    bool hasWrites = false;
    uint16_t writesChannelId = 0;
    // INTENT only: overrides the hub-wide echoDelayMs default.
    bool hasEchoDelay = false;
    uint32_t echoDelayMs = 0;

    // STORE only (RFC-021 descriptor; declared-only in this tool — see
    // README's "cut for leanness" note, no BLOB backend is wired).
    std::string storeKind;
    uint16_t storeCapacity = 0;
    uint32_t storePerItemMax = 0;
    uint16_t storeNameMax = 0;

    std::vector<FieldConfig> fields;
};

struct HubConfig {
    std::string name = "slopbench";
    std::string fwVersion = "0.1.0";
    std::string product = "SlopBench";
    uint32_t echoDelayMs = 0;  // hub-wide default fake STATE-echo delay
    std::vector<ChannelConfig> channels;

    const ChannelConfig* find(uint16_t id) const {
        for (const auto& c : channels) if (c.id == id) return &c;
        return nullptr;
    }
};

}  // namespace slopbench
