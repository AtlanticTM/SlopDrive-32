#pragma once

// ============================================================================
// SlopSyncCatalog — the SlopDrive-32 device's SlopSync channel catalog (§8.1).
//
// This is the machine-specific half of the protocol: WHICH channels this hub
// advertises, their classes, directions, access levels, rates, and packed/CBOR
// layouts. The etag (§8.3) is a hash over this catalog's deterministic encoding
// — every field, unit, scale, and bit label below is wire-visible and part of
// the client-invariant, so it is authored with the same care as the FROZEN
// conformance fixture (conformance/mini_catalog.hpp), whose authoring style
// this imitates verbatim.
//
// INVARIANTS this file must uphold (the codec + etag depend on them):
//   * entries MUST be in ASCENDING id order (etag is order-sensitive, §8.3).
//   * layout entries (STATE/STREAM) must fit limits::min_transport_payload
//     (242 B) unfragmented — the largest here is 0x0081 at 28 B. ✓
//   * names ≤32 B, field names ≤24 B, units ≤8 B (schema/catalog.cddl).
//   * 0x0003/0x0004/0x0005/0x0007 layouts are pinned by the hub's own internal
//     encoders/handlers (buildSafetyPayload / buildControlOwnerPayload /
//     handleIntent's ESTOP_CLEAR path / emitTakeoverEvent) — those field
//     shapes are NOT free to change here without changing the hub in lockstep.
//
// Wire sizes are commented per entry (layout classes) so a future edit that
// blows the 242 B budget is caught by eye; there is no packed struct to
// static_assert against (the catalog is field-descriptors, not a struct).
// ============================================================================

#include <cstdint>

#include "slopsync/channel/catalog.hpp"

namespace slopdrive {

// Device-catalog channel ids (the reserved 0x0001–0x0007 range is owned by the
// registry — slopsync::channels::; everything ≥0x0080 is this device's own
// allocation). Named here so buildSlopDriveCatalog() AND the telemetry
// publisher in SlopSyncHubService reference ONE definition — a literal in only
// one of the two would be a silent wire mismatch.
namespace ch {
inline constexpr uint16_t motion         = 0x0080;
inline constexpr uint16_t machine_config = 0x0081;
inline constexpr uint16_t pattern_state  = 0x0082;
inline constexpr uint16_t odometer       = 0x0083;
inline constexpr uint16_t motion_input   = 0x0084;
inline constexpr uint16_t move           = 0x0100;
inline constexpr uint16_t config_set     = 0x0101;
inline constexpr uint16_t pattern_cmd    = 0x0102;
inline constexpr uint16_t home           = 0x0103;
}  // namespace ch

inline slopsync::Catalog32 buildSlopDriveCatalog() {
    using slopsync::AccessLevel;
    using slopsync::CborFieldType;
    using slopsync::ChannelClass;
    using slopsync::Direction;
    using slopsync::PackedFieldType;
    using slopsync::Priority;

    slopsync::Catalog32 c;
    c.count = 14;
    auto& e = c.entries;
    int i = 0;

    // ---- 0x0003 "safety" — STATE, critical, on-change --------------------
    // VERBATIM copy of conformance/mini_catalog.hpp's safety entry: the hub's
    // buildSafetyPayload() hardcodes exactly this 8-byte layout (word bitfield8,
    // cause u8, owner_session u32, estop_seq u16). Do NOT reshape it.  [8 B]
    e[i].id = slopsync::channels::safety; e[i].name = "safety";
    e[i].cls = ChannelClass::STATE; e[i].dir = Direction::h2c;
    e[i].access = AccessLevel::viewer; e[i].maxRateHz = 0.0f;
    e[i].defaultPriority = Priority::critical;
    e[i].fieldCount = 4;
    e[i].layout[0] = {.name = "word", .type = PackedFieldType::bitfield8, .unit = "flag",
                      .scale = 1.0f, .bits = {"estop", "stop", "hold", "pause", "", "", "", ""}};
    e[i].layout[1] = {.name = "cause", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f};
    e[i].layout[2] = {.name = "owner_session", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f};
    e[i].layout[3] = {.name = "estop_seq", .type = PackedFieldType::u16, .unit = "count", .scale = 1.0f};
    ++i;

    // ---- 0x0004 "control-owner" — STATE, critical, on-change -------------
    // Matches Hub::buildControlOwnerPayload(): 4 × {source u8, owner u32}, in
    // ascending source order, 20 bytes total. Each pair is one arbiter source
    // (0 manual, 1 tcode, 2 pattern, 3 ossm) and the session id that owns it
    // (0 = unowned).  [20 B]
    e[i].id = slopsync::channels::control_owner; e[i].name = "control-owner";
    e[i].cls = ChannelClass::STATE; e[i].dir = Direction::h2c;
    e[i].access = AccessLevel::viewer; e[i].maxRateHz = 0.0f;
    e[i].defaultPriority = Priority::critical;
    e[i].fieldCount = 8;
    e[i].layout[0] = {.name = "src0",   .type = PackedFieldType::u8,  .unit = "", .scale = 1.0f};
    e[i].layout[1] = {.name = "owner0", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f};
    e[i].layout[2] = {.name = "src1",   .type = PackedFieldType::u8,  .unit = "", .scale = 1.0f};
    e[i].layout[3] = {.name = "owner1", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f};
    e[i].layout[4] = {.name = "src2",   .type = PackedFieldType::u8,  .unit = "", .scale = 1.0f};
    e[i].layout[5] = {.name = "owner2", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f};
    e[i].layout[6] = {.name = "src3",   .type = PackedFieldType::u8,  .unit = "", .scale = 1.0f};
    e[i].layout[7] = {.name = "owner3", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f};
    ++i;

    // ---- 0x0005 "safety-intents" — INTENT, critical, modest rate ----------
    // The client sends {1:"op"} where op is a safety_ops:: value (estop_clear=1
    // is hub-handled, stop/hold/pause/resume fall through to the delegate).
    e[i].id = slopsync::channels::safety_intents; e[i].name = "safety-intents";
    e[i].cls = ChannelClass::INTENT; e[i].dir = Direction::c2h;
    e[i].access = AccessLevel::controller; e[i].maxRateHz = 20.0f;
    e[i].defaultPriority = Priority::critical;
    e[i].fieldCount = 1;
    e[i].schema[0] = {.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = ""};
    ++i;

    // ---- 0x0006 "hub-status" — STATE, background, 1 Hz --------------------
    // Slow health telemetry.  [4+4+1+1 = 10 B]
    e[i].id = slopsync::channels::hub_status; e[i].name = "hub-status";
    e[i].cls = ChannelClass::STATE; e[i].dir = Direction::h2c;
    e[i].access = AccessLevel::viewer; e[i].maxRateHz = 1.0f;
    e[i].defaultPriority = Priority::background;
    e[i].fieldCount = 4;
    e[i].layout[0] = {.name = "heap_free", .type = PackedFieldType::u32, .unit = "B",     .scale = 1.0f};
    e[i].layout[1] = {.name = "uptime_s",  .type = PackedFieldType::u32, .unit = "s",     .scale = 1.0f};
    e[i].layout[2] = {.name = "rssi",      .type = PackedFieldType::i8,  .unit = "dBm",   .scale = 1.0f};
    e[i].layout[3] = {.name = "sessions",  .type = PackedFieldType::u8,  .unit = "count", .scale = 1.0f};
    ++i;

    // ---- 0x0007 "session-events" — EVENT, viewer -------------------------
    // Payload keys match Hub::emitTakeoverEvent(): {1:"source", 2:"session"}.
    e[i].id = slopsync::channels::session_events; e[i].name = "session-events";
    e[i].cls = ChannelClass::EVENT; e[i].dir = Direction::h2c;
    e[i].access = AccessLevel::viewer; e[i].maxRateHz = 0.0f;
    e[i].defaultPriority = Priority::normal;
    e[i].fieldCount = 2;
    e[i].schema[0] = {.key = 1, .name = "source",  .type = CborFieldType::uint_t, .unit = ""};
    e[i].schema[1] = {.key = 2, .name = "session", .type = CborFieldType::uint_t, .unit = ""};
    ++i;

    // ---- 0x0080 "motion" — STATE, elevated, 60 Hz ------------------------
    // The live carriage snapshot. scale 100 on positions = 10µm wire units;
    // scale 10 on speed = 0.1 mm/s wire units.  [2+2+2+1 = 7 B]
    e[i].id = ch::motion; e[i].name = "motion";
    e[i].cls = ChannelClass::STATE; e[i].dir = Direction::h2c;
    e[i].access = AccessLevel::viewer; e[i].maxRateHz = 60.0f;
    e[i].defaultPriority = Priority::elevated;
    e[i].fieldCount = 4;
    e[i].layout[0] = {.name = "pos_10um", .type = PackedFieldType::u16, .unit = "mm",   .scale = 100.0f};
    e[i].layout[1] = {.name = "tgt_10um", .type = PackedFieldType::u16, .unit = "mm",   .scale = 100.0f};
    e[i].layout[2] = {.name = "speed",    .type = PackedFieldType::i16, .unit = "mm/s", .scale = 10.0f};
    e[i].layout[3] = {.name = "flags",    .type = PackedFieldType::bitfield8, .unit = "flag", .scale = 1.0f,
                      .bits = {"homed", "homing", "gen_running", "paused", "override", "estop", "stream", ""}};
    ++i;

    // ---- 0x0081 "machine-config" — STATE, normal, on-change ---------------
    // The full geometry + dual-limit-set snapshot in physical units (f32).
    // [7 × 4 = 28 B]
    e[i].id = ch::machine_config; e[i].name = "machine-config";
    e[i].cls = ChannelClass::STATE; e[i].dir = Direction::h2c;
    e[i].access = AccessLevel::viewer; e[i].maxRateHz = 0.0f;
    e[i].defaultPriority = Priority::normal;
    e[i].fieldCount = 7;
    e[i].layout[0] = {.name = "window_min",  .type = PackedFieldType::f32, .unit = "mm",    .scale = 1.0f};
    e[i].layout[1] = {.name = "window_max",  .type = PackedFieldType::f32, .unit = "mm",    .scale = 1.0f};
    e[i].layout[2] = {.name = "user_speed",  .type = PackedFieldType::f32, .unit = "mm/s",  .scale = 1.0f};
    e[i].layout[3] = {.name = "user_accel",  .type = PackedFieldType::f32, .unit = "mm/s2", .scale = 1.0f};
    e[i].layout[4] = {.name = "input_speed", .type = PackedFieldType::f32, .unit = "mm/s",  .scale = 1.0f};
    e[i].layout[5] = {.name = "input_accel", .type = PackedFieldType::f32, .unit = "mm/s2", .scale = 1.0f};
    e[i].layout[6] = {.name = "max_rail",    .type = PackedFieldType::f32, .unit = "mm",    .scale = 1.0f};
    ++i;

    // ---- 0x0082 "pattern-state" — STATE, normal, on-change ----------------
    // PatternEngine live snapshot.  [1+1+4+4+4+4 = 18 B]
    e[i].id = ch::pattern_state; e[i].name = "pattern-state";
    e[i].cls = ChannelClass::STATE; e[i].dir = Direction::h2c;
    e[i].access = AccessLevel::viewer; e[i].maxRateHz = 0.0f;
    e[i].defaultPriority = Priority::normal;
    e[i].fieldCount = 6;
    e[i].layout[0] = {.name = "running",   .type = PackedFieldType::u8,  .unit = "",  .scale = 1.0f};
    e[i].layout[1] = {.name = "pattern",   .type = PackedFieldType::u8,  .unit = "",  .scale = 1.0f};
    e[i].layout[2] = {.name = "speed",     .type = PackedFieldType::f32, .unit = "%", .scale = 1.0f};
    e[i].layout[3] = {.name = "depth",     .type = PackedFieldType::f32, .unit = "%", .scale = 1.0f};
    e[i].layout[4] = {.name = "stroke",    .type = PackedFieldType::f32, .unit = "%", .scale = 1.0f};
    e[i].layout[5] = {.name = "sensation", .type = PackedFieldType::f32, .unit = "",  .scale = 1.0f};
    ++i;

    // ---- 0x0083 "odometer" — STATE, background, 1 Hz ---------------------
    // Session totals.  [4+4+4 = 12 B]
    e[i].id = ch::odometer; e[i].name = "odometer";
    e[i].cls = ChannelClass::STATE; e[i].dir = Direction::h2c;
    e[i].access = AccessLevel::viewer; e[i].maxRateHz = 1.0f;
    e[i].defaultPriority = Priority::background;
    e[i].fieldCount = 3;
    e[i].layout[0] = {.name = "strokes",    .type = PackedFieldType::u32, .unit = "",     .scale = 1.0f};
    e[i].layout[1] = {.name = "distance_m", .type = PackedFieldType::f32, .unit = "m",    .scale = 1.0f};
    e[i].layout[2] = {.name = "peak_mm_s",  .type = PackedFieldType::f32, .unit = "mm/s", .scale = 1.0f};
    ++i;

    // ---- 0x0084 "motion-input" — STREAM, c2h, controller, ≤333 Hz ---------
    // The SlopSync-native TCode successor: continuous stroke-window targets
    // + optional signed handoff velocity, decoded straight off BundleView by
    // the hub delegate's onStreamBundle() into the SlopMotion pacing ring
    // (maps to arbiter source 1 / MotionSource::TCODE_STREAM — the same
    // source id legacy TCode uses, since this IS that source, just arriving
    // over SlopSync instead of a text transport). scale 10000 on target =
    // 1e-4 resolution over the 0..1 stroke window; scale 1000 on vel = 1e-3
    // resolution, i16 signed (0 = no handoff velocity). NOTE: SPEC Appendix D
    // sketches 0x0081 as "motion-input" — this firmware already spent 0x0081
    // on machine-config, so 0x0084 is this device's actual allocation; the
    // catalog is self-describing and authoritative per Appendix D's own
    // disclaimer.  [2+2 = 4 B]
    e[i].id = ch::motion_input; e[i].name = "motion-input";
    e[i].cls = ChannelClass::STREAM; e[i].dir = Direction::c2h;
    e[i].access = AccessLevel::controller; e[i].maxRateHz = 333.0f;
    e[i].defaultPriority = Priority::elevated;
    e[i].fieldCount = 2;
    e[i].layout[0] = {.name = "target_norm", .type = PackedFieldType::u16, .unit = "norm",   .scale = 10000.0f};
    e[i].layout[1] = {.name = "vel_norm",    .type = PackedFieldType::i16, .unit = "norm/s", .scale = 1000.0f};
    ++i;

    // ---- 0x0100 "move" — INTENT, controller, 20 Hz, critical --------------
    // {1:"position" f32 mm, 2:"bypass" bool}. This channel maps to arbiter
    // source 0 (MANUAL) in the delegate.
    e[i].id = ch::move; e[i].name = "move";
    e[i].cls = ChannelClass::INTENT; e[i].dir = Direction::c2h;
    e[i].access = AccessLevel::controller; e[i].maxRateHz = 20.0f;
    e[i].defaultPriority = Priority::critical;
    e[i].fieldCount = 2;
    e[i].schema[0] = {.key = 1, .name = "position", .type = CborFieldType::f32_t, .unit = "mm",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 2000.0f};
    e[i].schema[1] = {.key = 2, .name = "bypass", .type = CborFieldType::bool_t, .unit = ""};
    ++i;

    // ---- 0x0101 "config-set" — INTENT, controller, 10 Hz ------------------
    // Every field optional; present keys are applied. cfg_gen bumps on success.
    e[i].id = ch::config_set; e[i].name = "config-set";
    e[i].cls = ChannelClass::INTENT; e[i].dir = Direction::c2h;
    e[i].access = AccessLevel::controller; e[i].maxRateHz = 10.0f;
    e[i].defaultPriority = Priority::normal;
    e[i].fieldCount = 6;
    e[i].schema[0] = {.key = 1, .name = "window_min",  .type = CborFieldType::f32_t, .unit = "mm"};
    e[i].schema[1] = {.key = 2, .name = "window_max",  .type = CborFieldType::f32_t, .unit = "mm"};
    e[i].schema[2] = {.key = 3, .name = "user_speed",  .type = CborFieldType::f32_t, .unit = "mm/s"};
    e[i].schema[3] = {.key = 4, .name = "user_accel",  .type = CborFieldType::f32_t, .unit = "mm/s2"};
    e[i].schema[4] = {.key = 5, .name = "input_speed", .type = CborFieldType::f32_t, .unit = "mm/s"};
    e[i].schema[5] = {.key = 6, .name = "input_accel", .type = CborFieldType::f32_t, .unit = "mm/s2"};
    ++i;

    // ---- 0x0102 "pattern-cmd" — INTENT, controller, 20 Hz -----------------
    // Session-volatile (cfg_gen does NOT bump). Maps to arbiter source 2
    // (PATTERN) via the delegate; running drives start/stop.
    e[i].id = ch::pattern_cmd; e[i].name = "pattern-cmd";
    e[i].cls = ChannelClass::INTENT; e[i].dir = Direction::c2h;
    e[i].access = AccessLevel::controller; e[i].maxRateHz = 20.0f;
    e[i].defaultPriority = Priority::normal;
    e[i].fieldCount = 6;
    e[i].schema[0] = {.key = 1, .name = "running",   .type = CborFieldType::bool_t, .unit = ""};
    e[i].schema[1] = {.key = 2, .name = "pattern",   .type = CborFieldType::uint_t, .unit = ""};
    e[i].schema[2] = {.key = 3, .name = "speed",     .type = CborFieldType::f32_t,  .unit = "%"};
    e[i].schema[3] = {.key = 4, .name = "depth",     .type = CborFieldType::f32_t,  .unit = "%"};
    e[i].schema[4] = {.key = 5, .name = "stroke",    .type = CborFieldType::f32_t,  .unit = "%"};
    e[i].schema[5] = {.key = 6, .name = "sensation", .type = CborFieldType::f32_t,  .unit = ""};
    ++i;

    // ---- 0x0103 "home" — INTENT, controller ------------------------------
    // {1:"op"} — op==1 starts sensorless homing. Other ops NACK UNSUPPORTED_OP.
    e[i].id = ch::home; e[i].name = "home";
    e[i].cls = ChannelClass::INTENT; e[i].dir = Direction::c2h;
    e[i].access = AccessLevel::controller; e[i].maxRateHz = 5.0f;
    e[i].defaultPriority = Priority::normal;
    e[i].fieldCount = 1;
    e[i].schema[0] = {.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = ""};
    ++i;

    return c;
}

}  // namespace slopdrive
