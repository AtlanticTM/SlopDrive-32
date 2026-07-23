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
#pragma once

#include "slopsync/channel/catalog.hpp"

namespace slopsync::conformance {

inline Catalog32 miniCatalog() {
    Catalog32 c;
    c.count = 6;
    auto& e = c.entries;

    // -- 0x0003 "safety" — STATE, critical, on-change; bitfield8 + u8/u32/u16
    e[0].id = 0x0003; e[0].name = "safety";
    e[0].cls = ChannelClass::STATE; e[0].dir = Direction::h2c;
    e[0].access = AccessLevel::viewer; e[0].maxRateHz = 0.0f;
    e[0].defaultPriority = Priority::critical;
    e[0].fieldCount = 4;
    e[0].layout[0] = {.name = "word", .type = PackedFieldType::bitfield8, .unit = "flag",
                      .scale = 1.0f, .bits = {"estop", "stop", "hold", "pause", "", "", "", ""}};
    e[0].layout[1] = {.name = "cause", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f};
    e[0].layout[2] = {.name = "owner_session", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f};
    e[0].layout[3] = {.name = "estop_seq", .type = PackedFieldType::u16, .unit = "count", .scale = 1.0f};

    // -- 0x0080 "position" — STREAM, elevated, 240 Hz; the real 6-byte sample
    e[1].id = 0x0080; e[1].name = "position";
    e[1].cls = ChannelClass::STREAM; e[1].dir = Direction::h2c;
    e[1].access = AccessLevel::viewer; e[1].maxRateHz = 240.0f;
    e[1].defaultPriority = Priority::elevated;
    e[1].fieldCount = 3;
    e[1].layout[0] = {.name = "pos_10um", .type = PackedFieldType::u16, .unit = "mm", .scale = 100.0f};
    e[1].layout[1] = {.name = "tgt_10um", .type = PackedFieldType::u16, .unit = "mm", .scale = 100.0f};
    e[1].layout[2] = {.name = "raw_10um", .type = PackedFieldType::u16, .unit = "mm", .scale = 100.0f};

    // -- 0x0082 "motion-status" — STATE, normal, 10 Hz; second bitfield8
    e[2].id = 0x0082; e[2].name = "motion-status";
    e[2].cls = ChannelClass::STATE; e[2].dir = Direction::h2c;
    e[2].access = AccessLevel::viewer; e[2].maxRateHz = 10.0f;
    e[2].defaultPriority = Priority::normal;
    e[2].fieldCount = 2;
    e[2].layout[0] = {.name = "flags", .type = PackedFieldType::bitfield8, .unit = "flag",
                      .scale = 1.0f,
                      .bits = {"homed", "homing", "gen_running", "paused", "override", "", "", ""}};
    e[2].layout[1] = {.name = "reserved", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f};

    // -- 0x0084 "motion-config-set" — INTENT, controller; f32/uint/tstr/bool
    e[3].id = 0x0084; e[3].name = "motion-config-set";
    e[3].cls = ChannelClass::INTENT; e[3].dir = Direction::c2h;
    e[3].access = AccessLevel::controller; e[3].maxRateHz = 10.0f;
    e[3].defaultPriority = Priority::critical;
    e[3].fieldCount = 4;
    e[3].schema[0] = {.key = 1, .name = "speed", .type = CborFieldType::f32_t, .unit = "mm/s",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 600.0f};
    e[3].schema[1] = {.key = 2, .name = "depth_pct", .type = CborFieldType::uint_t, .unit = "%",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f};
    e[3].schema[2] = {.key = 3, .name = "label", .type = CborFieldType::tstr_t, .unit = ""};
    e[3].schema[3] = {.key = 4, .name = "enable", .type = CborFieldType::bool_t, .unit = ""};

    // -- 0x008A "anomalies" — EVENT, viewer; int + bstr coverage
    e[4].id = 0x008A; e[4].name = "anomalies";
    e[4].cls = ChannelClass::EVENT; e[4].dir = Direction::h2c;
    e[4].access = AccessLevel::viewer; e[4].maxRateHz = 0.0f;
    e[4].defaultPriority = Priority::normal;
    e[4].fieldCount = 4;
    e[4].schema[0] = {.key = 1, .name = "kind", .type = CborFieldType::uint_t, .unit = ""};
    e[4].schema[1] = {.key = 2, .name = "t_dev", .type = CborFieldType::uint_t, .unit = "ms"};
    e[4].schema[2] = {.key = 3, .name = "delta", .type = CborFieldType::int_t, .unit = "mm"};
    e[4].schema[3] = {.key = 4, .name = "blob", .type = CborFieldType::bstr_t, .unit = ""};

    // -- 0x0090 "diag" — STATE, background, 2 Hz; i8/i16/i32/u32/f32 coverage
    e[5].id = 0x0090; e[5].name = "diag";
    e[5].cls = ChannelClass::STATE; e[5].dir = Direction::h2c;
    e[5].access = AccessLevel::viewer; e[5].maxRateHz = 2.0f;
    e[5].defaultPriority = Priority::background;
    e[5].fieldCount = 5;
    e[5].layout[0] = {.name = "d_i8", .type = PackedFieldType::i8, .unit = "degC", .scale = 1.0f};
    e[5].layout[1] = {.name = "d_i16", .type = PackedFieldType::i16, .unit = "mm/s", .scale = 10.0f,
                      .hasMin = true, .hasMax = true, .min = -600.0f, .max = 600.0f};
    e[5].layout[2] = {.name = "d_i32", .type = PackedFieldType::i32, .unit = "count", .scale = 1.0f};
    e[5].layout[3] = {.name = "d_u32", .type = PackedFieldType::u32, .unit = "count", .scale = 1.0f};
    e[5].layout[4] = {.name = "d_f32", .type = PackedFieldType::f32, .unit = "mm/s2", .scale = 1.0f};

    return c;
}

}  // namespace slopsync::conformance
