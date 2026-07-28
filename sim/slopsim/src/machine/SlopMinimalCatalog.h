#pragma once

// ============================================================================
// SlopMinimalCatalog — the `--profile minimal` catalog: the potato-client
// floor (docs/slopdeck/DESIGN.md §5, "the smallest conformant catalog...the
// 'any hub at all' floor").
//
// UNLIKE benchrig (SlopSimCatalog.h), this is explicitly a SUBSET OF THE REAL
// DEVICE catalog, not a different one: every id/name/field shape below is
// copied verbatim from `slopdrive::` (include/comms/SlopSyncCatalog.h) — it
// just declares far fewer channels. That is the point of the test this
// profile exists for: a Tier-0 client that renders `device` fully must also
// render THIS with nothing but its own generic catalog renderer, because
// there is nothing device-specific to fall back on — no plan-strip, no
// tuning surface, no patterns, not even a configurable stroke window.
//
// SPEC-CORE (0x0003-0x000E) is unconditional — every conformant hub carries
// it, minimal or not — declared the same way both other profiles do (hand-
// authored for the ones the Hub internals pin, library builders for the
// log/trust/safety-events triplet).
//
// Device range: just enough to prove liveness and control exist at all —
// `motion` (0x1100, watch the carriage), `move` (0x3100, point control),
// `home` (0x3101, the only way this floor machine ever gets a measured
// stroke). No `machine-config`/`config_set`: this machine's window and
// limits are NOT settable — proving Tier 0 renders a machine with literally
// no settings page just as well as one with twenty.
// ============================================================================

#include <cstdint>

#include "comms/SlopSyncCatalog.h"
#include "slopsync/channel/catalog.hpp"
#include "slopsync/channel/log_channel.hpp"
#include "slopsync/channel/safety_events_channel.hpp"
#include "slopsync/channel/trust_channels.hpp"

namespace slopdrive {

// Fills `c` with the minimal profile's catalog. Same out-param contract as
// buildSlopDriveCatalog(). Returns c.ok().
inline bool buildMinimalCatalog(slopsync::Catalog32& c) {
    using slopsync::AccessLevel;
    using slopsync::CborFieldType;
    using slopsync::ChannelClass;
    using slopsync::Direction;
    using slopsync::PackedFieldType;
    using slopsync::Priority;

    c.clear();

    // ---- SPEC-CORE (0x0003-0x000E) — verbatim, same shapes every profile --
    c.addEntry({.id = slopsync::channels::safety, .name = "safety",
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

    c.addEntry({.id = slopsync::channels::control_owner, .name = "control-owner",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::critical});
    c.addLayoutField({.name = "src0",   .type = PackedFieldType::u8,  .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "owner0", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "src1",   .type = PackedFieldType::u8,  .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "owner1", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "src2",   .type = PackedFieldType::u8,  .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "owner2", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "src3",   .type = PackedFieldType::u8,  .unit = "", .scale = 1.0f});
    c.addLayoutField({.name = "owner3", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});

    c.addEntry({.id = slopsync::channels::safety_intents, .name = "safety-intents",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::watch, .maxRateHz = 20.0f,
                .defaultPriority = Priority::critical});
    c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = "",
                            .role = "action.safety"},
                           {"reserved", "estop_clear", "stop", "hold", "pause", "resume",
                            "estop", "override_on", "override_off", "bypass_on", "bypass_off"},
                           {AccessLevel::control, AccessLevel::control, AccessLevel::watch,
                            AccessLevel::control, AccessLevel::control, AccessLevel::control,
                            AccessLevel::watch, AccessLevel::control, AccessLevel::control,
                            AccessLevel::control, AccessLevel::control});

    c.addEntry({.id = slopsync::channels::hub_status, .name = "hub-status",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 1.0f,
                .defaultPriority = Priority::background});
    c.addLayoutField({.name = "heap_free", .type = PackedFieldType::u32, .unit = "B",     .scale = 1.0f});
    c.addLayoutField({.name = "uptime_s",  .type = PackedFieldType::u32, .unit = "s",     .scale = 1.0f});
    c.addLayoutField({.name = "rssi",      .type = PackedFieldType::i8,  .unit = "dBm",   .scale = 1.0f});
    c.addLayoutField({.name = "sessions",  .type = PackedFieldType::u8,  .unit = "count", .scale = 1.0f});
    c.addLayoutField({.name = "log_dropped", .type = PackedFieldType::u32, .unit = "count", .scale = 1.0f});

    c.addEntry({.id = slopsync::channels::session_events, .name = "session-events",
                .cls = ChannelClass::EVENT, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::normal});
    c.addSchemaField({.key = 1, .name = "source",  .type = CborFieldType::uint_t, .unit = ""});
    c.addSchemaField({.key = 2, .name = "session", .type = CborFieldType::uint_t, .unit = ""});

    if (!slopsync::addLogChannel(c)) return false;
    if (!slopsync::addTrustChannels(c)) return false;
    if (!slopsync::addSafetyEventsChannel(c)) return false;

    // ---- 0x1100 "motion" — the SAME 9-byte shape as the device, verbatim --
    c.addEntry({.id = ch::motion, .name = "motion",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 60.0f,
                .defaultPriority = Priority::elevated,
                .hasCategory = true, .category = slopsync::ui_categories::motion,
                .hasRank = true, .rank = slopsync::ui_ranks::hero});
    c.addLayoutField({.name = "pos_10um", .type = PackedFieldType::u16, .unit = "mm",   .scale = 100.0f,
                      .role = slopsync::field_roles::telemetry_position,
                      .hasRank = true, .rank = slopsync::ui_ranks::hero,
                      .hasProvenance = true, .provenance = slopsync::value_provenance::actual,
                      .hasUnitId = true, .unitId = slopsync::unit_ids::mm});
    c.addLayoutField({.name = "tgt_10um", .type = PackedFieldType::u16, .unit = "mm",   .scale = 100.0f,
                      .role = slopsync::field_roles::telemetry_target,
                      .hasRank = true, .rank = slopsync::ui_ranks::hero,
                      .hasProvenance = true, .provenance = slopsync::value_provenance::planned,
                      .hasUnitId = true, .unitId = slopsync::unit_ids::mm});
    c.addLayoutField({.name = "speed",    .type = PackedFieldType::i16, .unit = "mm/s", .scale = 10.0f,
                      .role = slopsync::field_roles::telemetry_velocity,
                      .hasRank = true, .rank = slopsync::ui_ranks::hero,
                      .hasProvenance = true, .provenance = slopsync::value_provenance::actual,
                      .hasUnitId = true, .unitId = slopsync::unit_ids::mm_s});
    c.addBitfieldField({.name = "flags", .type = PackedFieldType::bitfield8, .unit = "flag", .scale = 1.0f,
                        .hasRank = true, .rank = slopsync::ui_ranks::detail},
                       {"homed", "homing", "gen_running", "paused", "override", "estop", "stream"});
    c.addLayoutField({.name = "raw_10um", .type = PackedFieldType::u16, .unit = "mm",   .scale = 100.0f,
                      .hasRank = true, .rank = slopsync::ui_ranks::diagnostic,
                      .hasProvenance = true, .provenance = slopsync::value_provenance::demand,
                      .hasUnitId = true, .unitId = slopsync::unit_ids::mm});

    // ---- 0x3100 "move" — verbatim -----------------------------------------
    c.addEntry({.id = ch::move, .name = "move",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 20.0f,
                .defaultPriority = Priority::critical});
    c.addSchemaField({.key = 1, .name = "position", .type = CborFieldType::f32_t, .unit = "mm",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = ceiling::rail_mm});
    c.addSchemaField({.key = 2, .name = "bypass", .type = CborFieldType::bool_t, .unit = ""});

    // ---- 0x3101 "home" — verbatim ------------------------------------------
    c.addEntry({.id = ch::home, .name = "home",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 5.0f,
                .defaultPriority = Priority::normal});
    c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = "",
                            .role = "action.home"},
                           {"reserved", "home", "force_home", "clear_override"},
                           {AccessLevel::control, AccessLevel::control, AccessLevel::control,
                            AccessLevel::control});
    c.addSchemaField({.key = 2, .name = "stroke", .type = CborFieldType::f32_t, .unit = "mm",
                      .hasMin = true, .hasMax = true, .min = 1.0f, .max = ceiling::rail_mm});

    return c.ok();
}

}  // namespace slopdrive
