// slopsync-core — mechanical catalog conformance checking (SPEC §9.1 fit
// rule, §8.1 invariants, §17.1). The engine behind the conformance CLI and
// the D-03 vector: feed it any catalog, get every violation, no opinions.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/wire/catalog_codec.hpp"

namespace slopsync::conformance {

enum class ViolationKind : uint8_t {
    StateTooLarge,       // §9.1: STATE layout exceeds min_transport_payload (242)
    IdsNotAscending,     // §8.3: etag requires ascending-id encoding order
    NameTooLong,         // §8.1: name 1..32 bytes
    NameEmpty,
    WrongFieldForm,      // §8.1: STATE/STREAM need layout, INTENT/EVENT need schema,
                         //       STORE needs a store descriptor
    NoFields,            // an entry with zero fields describes nothing
    CoreChannelMisclass, // 0x0001–0x007F ids must match registry core_channels classes (spot: 0x0003 STATE, 0x0005 INTENT)
    // ---- appended by M2b (existing values are unchanged on purpose) ---------
    EntryTooLarge,       // §8.1 / RFC-028: encoded entry exceeds limits::catalog_max_entry_bytes
                         //   (4096). An entry is an INDEPENDENTLY-DECODABLE document and a
                         //   conforming peer must be able to buffer one; a ~50-field entry with
                         //   full `desc` strings encodes to 8–10 KB, so this is an AUTHORING
                         //   error the catalog's owner fixes by splitting the channel or
                         //   trimming descriptions. Only the scratch-buffer overload of
                         //   checkCatalog() can see it — measuring means encoding.
    CategoryLabelMissing, // RFC-047/048 (Phase C2): a ui_categories vendor-range id
                          //   (0x40..0x7E, device-defined) MUST carry a category_label, else
                          //   clients have no name for the tab. Was "category >= 128" under
                          //   the retired setting_categories vocabulary (same wire key).
    SettingChannelMissing, // RFC-009: a field carrying setting_key is unwritable without the
                           //   entry's setting_channel naming the INTENT it writes through.
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
    bool has(ViolationKind k) const {
        for (size_t i = 0; i < count; ++i)
            if (violations[i].kind == k) return true;
        return false;
    }
};

// Structural checks (everything that needs no encoding). `scratch`, when
// non-empty, additionally enables the per-entry byte-size check — measuring an
// entry means encoding it, and this library never allocates, so the buffer is
// the caller's. Size it at least limits::catalog_max_entry_bytes + a little
// (a 4 KiB + 64 B array is the usual choice); a smaller buffer makes an
// oversize entry indistinguishable from a buffer-too-small failure, and both
// report EntryTooLarge.
template <size_t E, size_t L, size_t S, size_t B, size_t T>
inline ConformanceReport checkCatalog(const BasicCatalog<E, L, S, B, T>& c,
                                      std::span<std::byte> scratch) {
    ConformanceReport r;
    uint16_t prevId = 0;
    for (uint16_t i = 0; i < c.count; ++i) {
        const CatalogEntry& e = c.entries[i];
        if (i > 0 && e.id <= prevId) r.add(ViolationKind::IdsNotAscending, e.id);
        prevId = e.id;

        if (e.name.empty()) r.add(ViolationKind::NameEmpty, e.id);
        else if (e.name.size() > 32) r.add(ViolationKind::NameTooLong, e.id);

        if (e.fieldCount == 0) r.add(ViolationKind::NoFields, e.id);

        const auto layout = c.layoutFields(e);
        const auto schema = c.schemaFields(e);
        // Form check, mechanical rather than heuristic: the pool an entry draws
        // from is chosen by its CLASS, so a mis-authored entry (or one whose
        // (offset,count) doesn't resolve inside this catalog's pools at all)
        // resolves to fewer fields than it declares — or, for a STORE, to no
        // descriptor at all.
        if (e.fieldCount > 0) {
            switch (e.form()) {
                case FieldForm::Layout:
                    if (layout.size() != e.fieldCount) r.add(ViolationKind::WrongFieldForm, e.id);
                    break;
                case FieldForm::Schema:
                    if (schema.size() != e.fieldCount) r.add(ViolationKind::WrongFieldForm, e.id);
                    break;
                case FieldForm::Store:
                    // A store entry carries exactly ONE descriptor; anything else
                    // (a second one, or none resolvable) is a form violation.
                    if (e.fieldCount != 1 || c.storeDescriptor(e) == nullptr) {
                        r.add(ViolationKind::WrongFieldForm, e.id);
                    }
                    break;
            }
        }

        if (e.form() == FieldForm::Layout && e.cls == ChannelClass::STATE &&
            layoutWireSize(layout) > limits::min_transport_payload) {
            r.add(ViolationKind::StateTooLarge, e.id);
        }

        // ---- RFC-009/047/048 annotation coherence ---------------------------
        // A vendor-range ui_categories id (0x40..0x7E) with no label renders as
        // a nameless tab.
        if (e.hasCategory && e.category >= 0x40 && e.category <= 0x7E && e.categoryLabel.empty()) {
            r.add(ViolationKind::CategoryLabelMissing, e.id);
        }
        // setting_key says "write me through the entry's setting_channel" — so
        // there had better be one.
        if (!e.hasSettingChannel) {
            for (const LayoutField& f : layout) {
                if (f.hasSettingKey) {
                    r.add(ViolationKind::SettingChannelMissing, e.id);
                    break;
                }
            }
        }

        // ---- §8.1 / RFC-028 per-entry byte cap ------------------------------
        if (!scratch.empty()) {
            CborWriter w(scratch);
            detail::encodeEntry(w, c, e);
            const size_t n = w.size();
            if (n == 0 || n > limits::catalog_max_entry_bytes) {
                r.add(ViolationKind::EntryTooLarge, e.id);
            }
        }

        // Spot-checks against registry core channel classes.
        if (e.id == channels::safety && e.cls != ChannelClass::STATE) r.add(ViolationKind::CoreChannelMisclass, e.id);
        if (e.id == channels::safety_intents && e.cls != ChannelClass::INTENT) r.add(ViolationKind::CoreChannelMisclass, e.id);
    }
    return r;
}

// Structural-only convenience: everything above EXCEPT EntryTooLarge, which
// cannot be evaluated without a buffer to encode into. Tooling that wants the
// complete verdict passes a scratch span.
template <size_t E, size_t L, size_t S, size_t B, size_t T>
inline ConformanceReport checkCatalog(const BasicCatalog<E, L, S, B, T>& c) {
    return checkCatalog(c, std::span<std::byte>{});
}

}  // namespace slopsync::conformance
