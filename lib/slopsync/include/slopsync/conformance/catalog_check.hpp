// slopsync-core — mechanical catalog conformance checking (SPEC §9.1 fit
// rule, §8.1 invariants, §17.1). The engine behind the conformance CLI and
// the D-03 vector: feed it any catalog, get every violation, no opinions.
#pragma once

#include <array>
#include <cstdint>

#include "slopsync/channel/catalog.hpp"

namespace slopsync::conformance {

enum class ViolationKind : uint8_t {
    StateTooLarge,       // §9.1: STATE layout exceeds min_transport_payload (242)
    IdsNotAscending,     // §8.3: etag requires ascending-id encoding order
    NameTooLong,         // §8.1: name 1..32 bytes
    NameEmpty,
    WrongFieldForm,      // §8.1: STATE/STREAM need layout, INTENT/EVENT need schema
    NoFields,            // an entry with zero fields describes nothing
    CoreChannelMisclass, // 0x0001–0x007F ids must match registry core_channels classes (spot: 0x0003 STATE, 0x0005 INTENT)
};

struct Violation {
    ViolationKind kind;
    uint16_t channel_id;
};

struct ConformanceReport {
    static constexpr size_t kMax = 32;
    std::array<Violation, kMax> violations{};
    size_t count = 0;
    bool ok() const { return count == 0; }
    void add(ViolationKind k, uint16_t id) {
        if (count < kMax) violations[count++] = {k, id};
    }
};

template <size_t N>
inline ConformanceReport checkCatalog(const BasicCatalog<N>& c) {
    ConformanceReport r;
    uint16_t prevId = 0;
    for (uint16_t i = 0; i < c.count; ++i) {
        const CatalogEntry& e = c.entries[i];
        if (i > 0 && e.id <= prevId) r.add(ViolationKind::IdsNotAscending, e.id);
        prevId = e.id;

        if (e.name.empty()) r.add(ViolationKind::NameEmpty, e.id);
        else if (e.name.size() > 32) r.add(ViolationKind::NameTooLong, e.id);

        if (e.fieldCount == 0) r.add(ViolationKind::NoFields, e.id);

        const bool wantsLayout = e.usesLayout();
        // Heuristic form check: a layout-class entry whose first layout field
        // is unnamed while a schema field IS named (or vice versa) was built
        // in the wrong form. (The codec enforces the wire-level exclusivity;
        // this catches authoring mistakes pre-encode.)
        if (e.fieldCount > 0) {
            const bool layoutNamed = !e.layout[0].name.empty();
            const bool schemaNamed = !e.schema[0].name.empty();
            if (wantsLayout && !layoutNamed && schemaNamed) r.add(ViolationKind::WrongFieldForm, e.id);
            if (!wantsLayout && !schemaNamed && layoutNamed) r.add(ViolationKind::WrongFieldForm, e.id);
        }

        if (wantsLayout && e.cls == ChannelClass::STATE &&
            e.layoutWireSize() > limits::min_transport_payload) {
            r.add(ViolationKind::StateTooLarge, e.id);
        }

        // Spot-checks against registry core channel classes.
        if (e.id == channels::safety && e.cls != ChannelClass::STATE) r.add(ViolationKind::CoreChannelMisclass, e.id);
        if (e.id == channels::safety_intents && e.cls != ChannelClass::INTENT) r.add(ViolationKind::CoreChannelMisclass, e.id);
    }
    return r;
}

}  // namespace slopsync::conformance
