// slopsync-core — the FROZEN mini-catalog conformance fixture.
// This is `fixtures/mini-catalog.yaml` from vectors/manifest.yaml, as code
// (human-readable mirror: docs/slopsync/vectors/fixtures/mini-catalog.yaml).
//
// >>> DO NOT EDIT after the K-suite golden vectors are generated: <<<
// K-01 pins this catalog's deterministic encoding byte-for-byte and K-02 pins
// its etag. Coverage by construction: every PackedFieldType (u8 i8 u16 i16
// u32 i32 f32 bitfield8), every CborFieldType (uint int f32 bool tstr bstr),
// both layout- and schema-form entries, two bitfield8s, optional min/max both
// present and absent.
//
// CONTENT is frozen; the AUTHORING FORM tracks channel/catalog.hpp's builder
// API (M2a moved fields into shared pools; M2b interned bit labels into the
// label pool, so the two bitfield8 fields are authored through
// addBitfieldField() now — every id/name/class/access/rate/field below is
// byte-identical to the array-era fixture, which is exactly what K-01's
// 733-byte pin and K-02's etag pin prove. The fixture carries NO RFC-009
// annotations, and since every annotation key is OPTIONAL those pins are
// unmoved by M2b — that is the milestone's primary correctness signal).
#pragma once

#include "slopsync/channel/catalog.hpp"

namespace slopsync::conformance {

// Fills `c` with the frozen fixture (out-param: a catalog is tens of KiB and
// is never returned by value). Returns c.ok() — false only if `c`'s
// capacities are too small, which for Catalog32 cannot happen.
template <size_t E, size_t L, size_t S, size_t B, size_t T>
inline bool buildMiniCatalog(BasicCatalog<E, L, S, B, T>& c) {
    c.clear();

    // -- 0x0003 "safety" — STATE, critical, on-change; bitfield8 + u8/u32/u16
    // RFC-025c APPENDED `modes` (field 5) at the v1.0 base pass: manual
    // override + limit bypass are SAFETY-domain state. Bytes 0..7 are
    // unchanged, so this is append-only layout evolution — but it DOES move
    // K-01's length pin and K-02's etag, which is expected and sanctioned by
    // the RFC queue's break-allowed ruling (fixtures re-freeze at the v1.0
    // tag). It is the ONLY deliberate content change to this fixture.
    c.addEntry({.id = 0x0003, .name = "safety",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::critical});
    c.addBitfieldField({.name = "word", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f},
                       {"estop", "stop", "hold", "pause"});
    c.addLayoutField({.name = "cause", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "owner_session", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "estop_seq", .type = PackedFieldType::u16, .unit = "count", .scale = 1.0f});
    c.addBitfieldField({.name = "modes", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f},
                       {"override", "bypass"});

    // -- 0x0080 "position" — STREAM, elevated, 240 Hz; the real 6-byte sample
    c.addEntry({.id = 0x0080, .name = "position",
                .cls = ChannelClass::STREAM, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 240.0f,
                .defaultPriority = Priority::elevated});
    c.addLayoutField({.name = "pos_10um", .type = PackedFieldType::u16, .unit = "mm", .scale = 100.0f});
    c.addLayoutField({.name = "tgt_10um", .type = PackedFieldType::u16, .unit = "mm", .scale = 100.0f});
    c.addLayoutField({.name = "raw_10um", .type = PackedFieldType::u16, .unit = "mm", .scale = 100.0f});

    // -- 0x0082 "motion-status" — STATE, normal, 10 Hz; second bitfield8
    c.addEntry({.id = 0x0082, .name = "motion-status",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 10.0f,
                .defaultPriority = Priority::normal});
    c.addBitfieldField({.name = "flags", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f},
                       {"homed", "homing", "gen_running", "paused", "override"});
    c.addLayoutField({.name = "reserved", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f});

    // -- 0x0084 "motion-config-set" — INTENT, control; f32/uint/tstr/bool
    c.addEntry({.id = 0x0084, .name = "motion-config-set",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 10.0f,
                .defaultPriority = Priority::critical});
    c.addSchemaField({.key = 1, .name = "speed", .type = CborFieldType::f32_t, .unit = "mm/s",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 600.0f});
    c.addSchemaField({.key = 2, .name = "depth_pct", .type = CborFieldType::uint_t, .unit = "%",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f});
    c.addSchemaField({.key = 3, .name = "label", .type = CborFieldType::tstr_t, .unit = ""});
    c.addSchemaField({.key = 4, .name = "enable", .type = CborFieldType::bool_t, .unit = ""});

    // -- 0x008A "anomalies" — EVENT, watch; int + bstr coverage
    c.addEntry({.id = 0x008A, .name = "anomalies",
                .cls = ChannelClass::EVENT, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::normal});
    c.addSchemaField({.key = 1, .name = "kind", .type = CborFieldType::uint_t, .unit = ""});
    c.addSchemaField({.key = 2, .name = "t_dev", .type = CborFieldType::uint_t, .unit = "ms"});
    c.addSchemaField({.key = 3, .name = "delta", .type = CborFieldType::int_t, .unit = "mm"});
    c.addSchemaField({.key = 4, .name = "blob", .type = CborFieldType::bstr_t, .unit = ""});

    // -- 0x0090 "diag" — STATE, background, 2 Hz; i8/i16/i32/u32/f32 coverage
    c.addEntry({.id = 0x0090, .name = "diag",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 2.0f,
                .defaultPriority = Priority::background});
    c.addLayoutField({.name = "d_i8", .type = PackedFieldType::i8, .unit = "degC", .scale = 1.0f});
    c.addLayoutField({.name = "d_i16", .type = PackedFieldType::i16, .unit = "mm/s", .scale = 10.0f,
                      .hasMin = true, .hasMax = true, .min = -600.0f, .max = 600.0f});
    c.addLayoutField({.name = "d_i32", .type = PackedFieldType::i32, .unit = "count", .scale = 1.0f});
    c.addLayoutField({.name = "d_u32", .type = PackedFieldType::u32, .unit = "count", .scale = 1.0f});
    c.addLayoutField({.name = "d_f32", .type = PackedFieldType::f32, .unit = "mm/s2", .scale = 1.0f});

    return c.ok();
}

}  // namespace slopsync::conformance
