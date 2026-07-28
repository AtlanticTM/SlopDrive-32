// slopsync-core — conformance-checker suite (M6): the D-03 vector's full
// form. checkCatalog() is the engine a future standalone CLI wraps; here it
// runs against the frozen fixture (must be clean) and against deliberately
// broken catalogs (must catch every seeded violation).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopsync/conformance/catalog_check.hpp"
#include "slopsync/conformance/mini_catalog.hpp"

using namespace slopsync;
using namespace slopsync::conformance;

TEST_CASE("frozen mini-catalog passes conformance clean") {
    Catalog32 c;
    REQUIRE(buildMiniCatalog(c));
    auto r = checkCatalog(c);
    CHECK(r.ok());
    CHECK(r.count == 0);
}

TEST_CASE("D-03: oversized STATE layout is caught mechanically") {
    // Pre-M2b this case could only prove the checker's ARITHMETIC: kMaxFields
    // was 8 and the widest numeric packed type is 4 B, so 8*4 = 32 B could not
    // reach the 242 B floor and the violation was unconstructible. M2b raised
    // kMaxFields to 64, so it is constructible now — and this case asserts the
    // VIOLATION, exactly as its old note asked whoever raised it to do.
    auto buildWide = [](Catalog32& c, size_t nF32) {
        c.clear();
        c.addEntry({.id = 0x0090, .name = "wide-diag",
                    .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 2.0f,
                    .defaultPriority = Priority::background});
        for (size_t i = 0; i < nF32; ++i)
            c.addLayoutField({.name = "f", .type = PackedFieldType::f32, .unit = "", .scale = 1.0f});
    };

    SUBCASE("the largest layout that still fits passes clean") {
        Catalog32 c;
        buildWide(c, 60);  // 60 * 4 = 240 <= 242
        REQUIRE(c.ok());
        CHECK(c.layoutWireSize(c.entries[0]) == 240);
        CHECK(checkCatalog(c).ok());
    }
    SUBCASE("one field past the floor is reported StateTooLarge") {
        Catalog32 c;
        buildWide(c, 61);  // 61 * 4 = 244 > 242
        REQUIRE(c.ok());
        CHECK(c.layoutWireSize(c.entries[0]) == 244);
        auto r = checkCatalog(c);
        CHECK_FALSE(r.ok());
        CHECK(r.has(ViolationKind::StateTooLarge));
    }
    SUBCASE("RFC-026 str64 blows the floor in four fields") {
        Catalog32 c;
        c.clear();
        c.addEntry({.id = 0x0090, .name = "stringy",
                    .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::background});
        for (size_t i = 0; i < 4; ++i)
            c.addLayoutField({.name = "s", .type = PackedFieldType::str64, .unit = "", .scale = 1.0f});
        REQUIRE(c.ok());
        CHECK(c.layoutWireSize(c.entries[0]) == 256);
        CHECK(checkCatalog(c).has(ViolationKind::StateTooLarge));
    }

    // The checker's own comparison, exercised directly against the boundary.
    CHECK(size_t(242) <= limits::min_transport_payload);
    CHECK_FALSE(size_t(243) <= limits::min_transport_payload);
}

TEST_CASE("seeded violations are each caught") {
    SUBCASE("ids not ascending") {
        Catalog32 c;
    buildMiniCatalog(c);
        std::swap(c.entries[0], c.entries[1]);
        auto r = checkCatalog(c);
        CHECK_FALSE(r.ok());
        bool found = false;
        for (size_t i = 0; i < r.count; ++i)
            if (r.violations[i].kind == ViolationKind::IdsNotAscending) found = true;
        CHECK(found);
    }
    SUBCASE("empty name") {
        Catalog32 c;
    buildMiniCatalog(c);
        c.entries[2].name = "";
        auto r = checkCatalog(c);
        CHECK_FALSE(r.ok());
        CHECK(r.violations[0].kind == ViolationKind::NameEmpty);
        CHECK(r.violations[0].channel_id == 0x0082);
    }
    SUBCASE("zero fields") {
        Catalog32 c;
    buildMiniCatalog(c);
        c.entries[1].fieldCount = 0;
        auto r = checkCatalog(c);
        CHECK_FALSE(r.ok());
        CHECK(r.violations[0].kind == ViolationKind::NoFields);
    }
    SUBCASE("core channel misclassified") {
        Catalog32 c;
    buildMiniCatalog(c);
        c.entries[0].cls = ChannelClass::STREAM;  // 0x0003 safety must be STATE
        auto r = checkCatalog(c);
        CHECK_FALSE(r.ok());
        bool found = false;
        for (size_t i = 0; i < r.count; ++i)
            if (r.violations[i].kind == ViolationKind::CoreChannelMisclass) found = true;
        CHECK(found);
    }
}

// ---- M2b --------------------------------------------------------------------
// the checks that came in with RFC-009 annotations, STORE entries, and
// the per-entry byte cap.

TEST_CASE("M2b: a well-formed STORE entry is clean; a malformed one is caught") {
    SUBCASE("clean") {
        Catalog32 c;
        c.addEntry({.id = channels::paired_devices, .name = "paired-devices",
                    .cls = ChannelClass::STORE, .dir = Direction::h2c,
                    .access = AccessLevel::configure, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::normal});
        c.addStoreDescriptor({.storeId = 1, .kind = "trust.ledger",
                              .capacity = uint16_t(limits::paired_devices_max),
                              .perItemMax = 192, .nameMax = 24});
        REQUIRE(c.ok());
        CHECK(checkCatalog(c).ok());
    }
    SUBCASE("a STORE entry with no descriptor describes nothing") {
        Catalog32 c;
        c.addEntry({.id = channels::paired_devices, .name = "paired-devices",
                    .cls = ChannelClass::STORE, .dir = Direction::h2c,
                    .access = AccessLevel::configure, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::normal});
        auto r = checkCatalog(c);
        CHECK_FALSE(r.ok());
        CHECK(r.has(ViolationKind::NoFields));
    }
    SUBCASE("a STORE entry whose descriptor does not resolve is WrongFieldForm") {
        Catalog32 c;
        c.addEntry({.id = channels::paired_devices, .name = "paired-devices",
                    .cls = ChannelClass::STORE, .dir = Direction::h2c,
                    .access = AccessLevel::configure, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::normal});
        c.addStoreDescriptor({.storeId = 1, .kind = "trust.ledger",
                              .capacity = 8, .perItemMax = 192, .nameMax = 24});
        c.entries[0].fieldOffset = 99;  // hand-mutated: points outside the store pool
        auto r = checkCatalog(c);
        CHECK_FALSE(r.ok());
        CHECK(r.has(ViolationKind::WrongFieldForm));
    }
}

TEST_CASE("M2b: RFC-009 annotation coherence") {
    SUBCASE("a vendor-range ui_categories id (0x40..0x7E) without a label is caught") {
        Catalog32 c;
        c.addEntry({.id = 0x0080, .name = "motion",
                    .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::normal,
                    .hasCategory = true, .category = 0x64});
        c.addLayoutField({.name = "pos", .type = PackedFieldType::u16, .unit = "mm", .scale = 100.0f});
        REQUIRE(c.ok());
        auto r = checkCatalog(c);
        CHECK(r.has(ViolationKind::CategoryLabelMissing));

        c.entries[0].categoryLabel = "SlopDrive";
        CHECK(checkCatalog(c).ok());
    }
    SUBCASE("a registered ui_categories id (1..14) needs no label") {
        Catalog32 c;
        c.addEntry({.id = 0x0080, .name = "motion",
                    .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::normal,
                    .hasCategory = true, .category = ui_categories::limits});
        c.addLayoutField({.name = "pos", .type = PackedFieldType::u16, .unit = "mm", .scale = 100.0f});
        REQUIRE(c.ok());
        CHECK(checkCatalog(c).ok());
    }
    SUBCASE("setting_key without the entry's setting_channel is unwritable") {
        Catalog32 c;
        c.addEntry({.id = 0x0081, .name = "machine-config",
                    .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::normal});
        c.addLayoutField({.name = "user_speed", .type = PackedFieldType::f32, .unit = "mm/s",
                          .scale = 1.0f, .settingKey = 3, .hasSettingKey = true});
        REQUIRE(c.ok());
        auto r = checkCatalog(c);
        CHECK(r.has(ViolationKind::SettingChannelMissing));

        c.entries[0].hasSettingChannel = true;
        c.entries[0].settingChannel = 0x0101;
        CHECK(checkCatalog(c).ok());
    }
}

TEST_CASE("M2b: EntryTooLarge needs the scratch overload and fires at the cap") {
    static const char kLongDesc[] =
        "A deliberately long description, so that a wide entry blows the "
        "per-entry byte cap the feasibility pass introduced. Padding.";

    static Catalog32 c;
    c.clear();
    c.addEntry({.id = 0x0130, .name = "ap-params-fat",
                .cls = ChannelClass::STREAM, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 1.0f,
                .defaultPriority = Priority::background});
    for (size_t i = 0; i < CatalogEntry::kMaxFields; ++i) {
        c.addLayoutField({.name = "parameter_with_a_name", .type = PackedFieldType::u16,
                          .unit = "mm", .scale = 1.0f,
                          .group = "A reasonably long group",
                          .desc = kLongDesc,
                          .role = "telemetry.position"});
    }
    REQUIRE(c.ok());

    // Structural-only overload cannot see it — measuring means encoding, and
    // this library never allocates.
    CHECK_FALSE(checkCatalog(c).has(ViolationKind::EntryTooLarge));

    static std::array<std::byte, limits::catalog_max_entry_bytes + 64> scratch{};
    auto r = checkCatalog(c, scratch);
    CHECK_FALSE(r.ok());
    CHECK(r.has(ViolationKind::EntryTooLarge));
    CHECK(r.violations[0].channel_id == 0x0130);

    // The frozen fixture is nowhere near the cap, and the scratch overload
    // agrees with the structural one on a clean catalog.
    static Catalog32 mini;
    buildMiniCatalog(mini);
    CHECK(checkCatalog(mini, scratch).ok());
}
