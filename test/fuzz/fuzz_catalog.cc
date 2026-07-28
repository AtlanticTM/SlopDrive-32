// fuzz_catalog — target: decodeCatalog (wire/catalog_codec.hpp) — the
// CLIENT-side surface.
//
// RFC-028's asymmetry point: a client that auto-connects to a discovered
// `_slopsync._tcp` beacon parses whatever catalog that hub sends, and the
// catalog is the fattest parse surface in the protocol — nested maps four
// deep, a dozen variable-length string keys per field, index-aligned pools
// (bit labels, option labels, option_access), STORE descriptors, and the
// limits::catalog_max_entry_bytes window. A hostile hub MUST NOT be able to
// crash a conforming client (RFC-028 §5).
//
// The catalog is a static (tens of KiB — never a stack temporary; that bug
// class has already blown a FreeRTOS task stack on this project) and is
// cleared by decodeCatalog itself on entry, so state does not leak between
// runs in a way that would make findings unreproducible.
//
// On a SUCCESSFUL decode we additionally exercise the paths a real client
// runs next — re-encode, etag, and per-entry field resolution — because a
// decoder that accepts a forged catalog into an internally-inconsistent
// state is a memory-safety bug that only fires downstream.
#include "fuzz_common.hpp"

using namespace slopsync;
using namespace slopfuzz;

static Catalog32 g_cat;
static std::array<std::byte, 64 * 1024> g_reencode;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    auto in = asBytes(data, size);

    auto r = decodeCatalog(in, g_cat);
    if (!r) return 0;

    // --- Downstream consumption, exactly as a client would ------------------
    for (uint16_t i = 0; i < g_cat.count; ++i) {
        const CatalogEntry& e = g_cat.entries[i];
        sink(touch(e.name));
        sink(touch(e.categoryLabel));

        switch (e.form()) {
            case FieldForm::Layout: {
                auto fields = g_cat.layoutFields(e);
                // TOTALITY: a decoded entry must resolve to exactly the field
                // count it declared, or a later index walk reads someone
                // else's pool slots.
                if (fields.size() != e.fieldCount) __builtin_trap();
                for (const LayoutField& f : fields) {
                    sink(touch(f.name));
                    sink(touch(f.unit));
                    sink(touch(f.group));
                    sink(touch(f.desc));
                    sink(touch(f.role));
                    for (std::string_view b : g_cat.bitLabels(f)) sink(touch(b));
                    for (std::string_view o : g_cat.optionLabels(f)) sink(touch(o));
                    sink(uint32_t(f.wireSize()));
                }
                break;
            }
            case FieldForm::Schema: {
                auto fields = g_cat.schemaFields(e);
                if (fields.size() != e.fieldCount) __builtin_trap();
                for (const SchemaField& f : fields) {
                    sink(touch(f.name));
                    sink(touch(f.unit));
                    sink(touch(f.group));
                    sink(touch(f.desc));
                    sink(touch(f.role));
                    for (std::string_view o : g_cat.optionLabels(f)) sink(touch(o));
                    for (AccessLevel a : g_cat.optionAccess(f)) sink(uint32_t(a));
                }
                break;
            }
            case FieldForm::Store: {
                const StoreDescriptor* d = g_cat.storeDescriptor(e);
                if (d == nullptr) __builtin_trap();  // decode said Store; pool says no
                sink(touch(d->kind));
                sink(d->capacity);
                break;
            }
        }
    }

    // --- Round-trip: re-encode and hash ------------------------------------
    // Not an equality assertion (see fuzz_common.hpp's "no semantics" rule) —
    // this is here because encodeCatalog walks the same pools decode filled,
    // so it is a second, independent reader of any inconsistency decode let
    // through.
    size_t n = encodeCatalog(g_cat, std::span<std::byte>(g_reencode));
    if (n > g_reencode.size()) __builtin_trap();
    auto etag = catalogEtag(g_cat, std::span<std::byte>(g_reencode));
    sink(touch(std::span<const std::byte>(etag)));
    return 0;
}
