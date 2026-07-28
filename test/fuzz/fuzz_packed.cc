// fuzz_packed — target: packed layout decode (wire/packed/layout_codec.hpp)
// and the STATE shadow apply/append-only read on top of it
// (channel/state_apply.hpp).
//
// Per the brief: fuzz against a FIXED known-good catalog so the fuzzer
// explores the DATA (the packed bytes a hub sends for a channel) rather than
// re-deriving a schema. The frozen mini-catalog is that fixture — it covers
// every PackedFieldType.
//
// The RFC-026 str16/str32/str64 types are new code with fixed-width
// NUL-padded semantics and offset math that just changed, and the mini-
// catalog predates them (it is FROZEN — its bytes are pinned by K-01/K-02
// and must never be edited). So string coverage comes from a SECOND,
// harness-local catalog built here with string fields interleaved among
// numeric ones, which is where the offset math actually gets stressed.
#include "fuzz_common.hpp"

#include "slopsync/conformance/mini_catalog.hpp"

using namespace slopsync;
using namespace slopfuzz;

static Catalog32 g_mini;
static Catalog32 g_strs;
static bool g_built = false;

static void buildStringCatalog() {
    // Strings interleaved with numerics, at every width, first/middle/last —
    // the arrangement that catches an off-by-N in a string field's advance.
    g_strs.clear();
    g_strs.addEntry({.id = 0x0100, .name = "strmix",
                     .cls = ChannelClass::STATE, .dir = Direction::h2c,
                     .access = AccessLevel::watch, .maxRateHz = 10.0f,
                     .defaultPriority = Priority::normal});
    g_strs.addLayoutField({.name = "s16", .type = PackedFieldType::str16, .unit = "", .scale = 1.0f});
    g_strs.addLayoutField({.name = "u8a", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});
    g_strs.addLayoutField({.name = "s32", .type = PackedFieldType::str32, .unit = "", .scale = 1.0f});
    g_strs.addLayoutField({.name = "i16a", .type = PackedFieldType::i16, .unit = "", .scale = 10.0f});
    g_strs.addLayoutField({.name = "s64", .type = PackedFieldType::str64, .unit = "", .scale = 1.0f});
    g_strs.addLayoutField({.name = "f32a", .type = PackedFieldType::f32, .unit = "", .scale = 1.0f});
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!g_built) {
        conformance::buildMiniCatalog(g_mini);
        buildStringCatalog();
        g_built = true;
    }
    if (size == 0) return 0;

    FuzzInput fi(data, size);
    const uint8_t sel = fi.u8();
    Catalog32& cat = (sel & 0x80) ? g_strs : g_mini;
    if (cat.count == 0) return 0;
    const CatalogEntry& entry = cat.entries[(sel & 0x7F) % cat.count];
    auto fields = cat.layoutFields(entry);

    auto payload = fi.rest();

    // --- decodeByLayout, exact field list -----------------------------------
    {
        std::array<float, CatalogEntry::kMaxFields> vals{};
        auto r = decodeByLayout(fields, payload, std::span<float>(vals.data(), fields.size()));
        if (r) {
            if (r.value() > payload.size()) __builtin_trap();  // TOTALITY
            for (size_t i = 0; i < fields.size(); ++i) sinkF(vals[i]);
        }
    }

    // --- The append-only prefix parse (§5.4): a SHORTER known field list
    //     against a longer payload, i.e. an old client vs a newer hub --------
    if (!fields.empty()) {
        size_t known = size_t(fi.u8()) % (fields.size() + 1);
        std::array<float, CatalogEntry::kMaxFields> vals{};
        auto r = appendOnlyRead(fields.subspan(0, known), payload,
                                std::span<float>(vals.data(), known));
        if (r && r.value() > payload.size()) __builtin_trap();
    }

    // --- String fields: the RFC-026 offset math -----------------------------
    {
        size_t offset = 0;
        for (const LayoutField& f : fields) {
            const size_t n = f.wireSize();
            if (offset + n > payload.size()) break;
            if (f.isString()) {
                auto sv = readStringField(f, payload.subspan(offset, n));
                // TOTALITY: a NUL-padded field's text can never exceed its
                // own declared width, and must alias only within the field.
                if (sv.size() > n) __builtin_trap();
                if (!sv.empty()) {
                    const char* base = reinterpret_cast<const char*>(payload.data() + offset);
                    if (sv.data() < base || sv.data() + sv.size() > base + n) __builtin_trap();
                }
                sink(touch(sv));
            }
            offset += n;
        }
    }

    // --- STATE shadow apply -------------------------------------------------
    {
        static ShadowSlot slot;
        bool capExceeded = false;
        FuzzInput f2(data, size);
        uint16_t seq = f2.u16();
        applyStateFrame(seq, f2.rest(), slot, &capExceeded);
        if (slot.size > slot.value.size()) __builtin_trap();  // TOTALITY
        sink(touch(std::span<const std::byte>(slot.value.data(), slot.size)));
    }
    return 0;
}
