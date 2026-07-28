#pragma once

// SlopSimCatalog — a DELIBERATELY DIFFERENT SlopSync catalog for slopsim.
// Constraints:
//   Proves the falsifiable claim the WebUI refactor rests on: "the client
//   renders a machine it has never met." This is slopsim's OWN device
//   catalog — a plausible, simpler/cheaper third-party machine ("benchrig")
//   that shares nothing with the real device except the SPEC-CORE channels
//   every SlopSync hub carries (0x0003-0x000E) and the library that encodes
//   them. Not included by the firmware build; wired in by
//   sim/slopsim/CMakeLists.txt instead of include/comms/SlopSyncCatalog.h.
//   See MachineSim.cpp's initCatalog().
// See: docs/slopdeck/DESIGN.md §5/§7 (sim-fidelity ruling)
//
// WHAT IS DIFFERENT FROM include/comms/SlopSyncCatalog.h, ON PURPOSE:
//   * Every DEVICE-range channel id is different (0x0090+ / 0x0110+ here vs.
//     the real device's RFC-047 grid — 0x1000+/0x1100+/0x1200+/0x2100+/
//     0x3000+/0x3100+/0x3200+/0x4100+/0x5200+ as of Phase C2) — the sharpest
//     part of the test: any client with a channel id hardcoded anywhere
//     breaks visibly against this catalog. This file's OWN ids are NOT part
//     of that renumber and never need to change to stay non-colliding with
//     it — the two catalogs were never in numeric correspondence.
//   * NO input (machine-driven) limit set and NO jerk setting — benchrig only
//     exposes the window and the user (manual) limit set. Its motion core
//     still needs internal speed/accel/jerk ceilings to plan at all (see
//     MachineSim.h _input_speed/_input_accel/_input_jerk), but those are
//     fixed firmware constants here, never wired to a setting or published —
//     exactly like a cheap machine with one hard-coded top gear.
//   * NO pattern/generator channel — this machine cannot run a pattern on its
//     own, so no pattern-state STATE / pattern-cmd INTENT pair is advertised.
//   * NO SlopMotion tuning surface (the real device's 0x008B/C/D + 0x0105) —
//     this machine has no jerk-limited planner introspection to expose.
//   * ONE setting the real device does not have: `warmup_mode`, a select, and
//     `device_label`, a str16 (RFC-026) free-text field — together they prove
//     (a) a brand-new setting renders with zero client changes and (b) the
//     str16 widget path, which nothing on the real device exercises.
//   * A device-defined category (>=128, with a `category_label`) — the real
//     device only ever uses the four registry-named categories, so a client
//     that hardcoded those names would fail to render this one's heading.
//   * Different bounds/defaults on every shared role (window, user limits) —
//     smaller rail, smaller speed/accel ceilings, different factory numbers.
//
// WHAT STAYS IDENTICAL: 0x0003 safety / 0x0004 control-owner / 0x0005
// safety-intents / 0x0006 hub-status / 0x0007 session-events (hand-authored,
// copied verbatim below because Hub internals hardcode these exact shapes —
// see the comment on each), plus 0x0008 log / 0x0009-0x000D trust / 0x000E
// safety-events, declared the same way the real catalog does: by calling the
// library's own builders. These are SPEC-CORE — every hub carries them
// unchanged, by protocol definition, not by choice.

#include <cstdint>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/channel/log_channel.hpp"
#include "slopsync/channel/safety_events_channel.hpp"
#include "slopsync/channel/trust_channels.hpp"

namespace benchrig {

// Device-catalog channel ids. Named here so buildDivergentCatalog() AND
// MachineSim.cpp reference ONE definition — same discipline as slopdrive::ch
// in the real catalog, so a literal in only one place can never silently
// diverge from the wire.
namespace ch {
inline constexpr uint16_t telemetry       = 0x0090;  // STATE: position + velocity
inline constexpr uint16_t limits          = 0x0091;  // STATE: window + user limit set
inline constexpr uint16_t device_settings = 0x0092;  // STATE: the device-only settings card
inline constexpr uint16_t motion_input    = 0x0095;  // STREAM c2h: dense point samples
inline constexpr uint16_t motion_segment  = 0x0096;  // STREAM c2h: timed segments
inline constexpr uint16_t limits_set      = 0x0110;  // INTENT: writes 0x0091
inline constexpr uint16_t device_set      = 0x0111;  // INTENT: writes 0x0092
inline constexpr uint16_t move            = 0x0112;  // INTENT: manual point move
inline constexpr uint16_t home            = 0x0113;  // INTENT: home / bench ops
}  // namespace ch

// A device-defined category (registry `ui_categories` -- RFC-047/048, Phase
// C2 -- registers 1..14; the vendor/device-defined range is 0x40..0x7E and
// REQUIRES a `category_label`, per catalog.hpp's CatalogEntry::categoryLabel
// comment). Proves a client renders a category heading it cannot possibly
// have a name for baked in. Was 128 under the retired setting_categories
// vocabulary's >=128 device-defined tail; 0x40 is the equivalent floor here.
namespace category {
inline constexpr uint8_t bench_extras = 0x40;
}

// ---- Factory DEFAULTS, advertised as RFC-009 `default` annotations ----------
// Deliberately DIFFERENT numbers from the real device's factory:: block
// (include/comms/SlopSyncCatalog.h) — smaller machine, smaller everything.
namespace factory {
inline constexpr float window_min   = 0.0f;
inline constexpr float window_max   = 300.0f;    // real device: 500.0f
inline constexpr float user_speed   = 40.0f;     // real device: 50.0f
inline constexpr float user_accel   = 150.0f;    // real device: 200.0f
inline constexpr uint8_t warmup_mode = 0;         // 0 = off
inline constexpr const char* device_label = "bench-rig";
}  // namespace factory

// ---- Hard ceilings advertised as `min`/`max` --------------------------------
// Also different from the real device: a cheaper rail, a lower top speed, no
// jerk ceiling at all (there is no jerk SETTING to bound).
namespace ceiling {
inline constexpr float rail_mm   = 800.0f;    // real device: 2000.0f
inline constexpr float speed_min = 1.0f;
inline constexpr float speed_max = 3000.0f;   // real device: 10000.0f
inline constexpr float accel_min = 10.0f;
inline constexpr float accel_max = 20000.0f;  // real device: 100000.0f
}  // namespace ceiling

// Fills `c` with benchrig's catalog. Out-param for the same reason the real
// builder is (Catalog32 is a multi-KiB pooled structure — never return one by
// value). Returns c.ok().
inline bool buildDivergentCatalog(slopsync::Catalog32& c) {
    using slopsync::AccessLevel;
    using slopsync::CborFieldType;
    using slopsync::ChannelClass;
    using slopsync::Direction;
    using slopsync::PackedFieldType;
    using slopsync::Priority;
    using slopsync::SettingDefault;
    namespace roles = slopsync::field_roles;

    c.clear();

    // ---- SPEC-CORE (0x0003-0x000E) ------------------------------------------
    // Verbatim copy of the pattern in include/comms/SlopSyncCatalog.h. Every
    // hub carries these unchanged; see that file's own comments for why each
    // shape is pinned to Hub internals. Copied rather than shared because the
    // library's stance is that hand-authored spec-core entries are authored
    // per hub anyway — only the log/trust/safety-events triplet comes from a
    // shared builder.

    // ---- 0x0003 "safety" — STATE, critical, on-change -----------------------
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

    // ---- 0x0004 "control-owner" — STATE, critical, on-change ----------------
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

    // ---- 0x0005 "safety-intents" — INTENT, critical, modest rate ------------
    c.addEntry({.id = slopsync::channels::safety_intents, .name = "safety-intents",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::watch, .maxRateHz = 20.0f,
                .defaultPriority = Priority::critical});
    c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = ""},
                           {"reserved", "estop_clear", "stop", "hold", "pause", "resume",
                            "estop", "override_on", "override_off", "bypass_on", "bypass_off"},
                           {AccessLevel::control,  // 0  (placeholder, never an op)
                            AccessLevel::control,  // 1  estop_clear
                            AccessLevel::watch,    // 2  stop          ROLE-EXEMPT
                            AccessLevel::control,  // 3  hold
                            AccessLevel::control,  // 4  pause
                            AccessLevel::control,  // 5  resume
                            AccessLevel::watch,    // 6  estop         ROLE-EXEMPT
                            AccessLevel::control,  // 7  override_on
                            AccessLevel::control,  // 8  override_off
                            AccessLevel::control,  // 9  bypass_on
                            AccessLevel::control});// 10 bypass_off

    // ---- 0x0006 "hub-status" — STATE, background, 1 Hz ----------------------
    c.addEntry({.id = slopsync::channels::hub_status, .name = "hub-status",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 1.0f,
                .defaultPriority = Priority::background});
    c.addLayoutField({.name = "heap_free", .type = PackedFieldType::u32, .unit = "B",     .scale = 1.0f});
    c.addLayoutField({.name = "uptime_s",  .type = PackedFieldType::u32, .unit = "s",     .scale = 1.0f});
    c.addLayoutField({.name = "rssi",      .type = PackedFieldType::i8,  .unit = "dBm",   .scale = 1.0f});
    c.addLayoutField({.name = "sessions",  .type = PackedFieldType::u8,  .unit = "count", .scale = 1.0f});
    c.addLayoutField({.name = "log_dropped", .type = PackedFieldType::u32, .unit = "count", .scale = 1.0f,
                      .desc = "Log lines dropped since boot (replay ring + cross-task bridge)."});

    // ---- 0x0007 "session-events" — EVENT, watch -----------------------------
    c.addEntry({.id = slopsync::channels::session_events, .name = "session-events",
                .cls = ChannelClass::EVENT, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::normal});
    c.addSchemaField({.key = 1, .name = "source",  .type = CborFieldType::uint_t, .unit = ""});
    c.addSchemaField({.key = 2, .name = "session", .type = CborFieldType::uint_t, .unit = ""});

    // ---- 0x0008 "log" — EVENT (library builder; SPEC-CORE) ------------------
    if (!slopsync::addLogChannel(c)) return false;

    // ---- 0x0009..0x000D — trust administration (library builder) ------------
    if (!slopsync::addTrustChannels(c)) return false;

    // ---- 0x000E "safety-events" — EVENT (library builder) -------------------
    if (!slopsync::addSafetyEventsChannel(c)) return false;

    // ---- DEVICE RANGE -------------------------------------------------------
    // benchrig's OWN allocation. Every id, every field set, every bound below
    // is deliberately different from the real device.

    // ---- 0x0090 "telemetry" — STATE, elevated, 30 Hz ------------------------
    // Just position + velocity + a two-bit status word. No raw/target split,
    // no plan-strip twin — this machine doesn't expose planner internals, only
    // where the carriage IS and how fast it's moving. Different wire shape
    // from the real device's 0x0080 too: plain f32 mm/mm-per-s rather than
    // scaled u16/i16 — proves a client isn't assuming a specific numeric
    // encoding, only the catalog's declared type + scale.
    c.addEntry({.id = ch::telemetry, .name = "telemetry",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 30.0f,
                .defaultPriority = Priority::elevated});
    c.addLayoutField({.name = "pos_mm", .type = PackedFieldType::f32, .unit = "mm", .scale = 1.0f,
                      .desc = "Where the carriage actually is.",
                      .role = roles::telemetry_position});
    c.addLayoutField({.name = "vel_mm_s", .type = PackedFieldType::f32, .unit = "mm/s", .scale = 1.0f,
                      .desc = "Live carriage speed; sign is the direction of travel.",
                      .role = roles::telemetry_velocity});
    c.addBitfieldField({.name = "flags", .type = PackedFieldType::bitfield8, .unit = "flag", .scale = 1.0f,
                        .desc = "Live machine status bits."},
                       {"homed", "moving"});

    // ---- 0x0091 "limits" — STATE, normal, on-change -------------------------
    // The window + the USER (manual) limit set only. DELIBERATELY NO input
    // (machine-driven) limit set and NO jerk field — this machine has no
    // separate machine-driven ceiling to tune and no jerk-limited planner to
    // expose one for (see the file header). `max_rail` is read-only derived
    // truth, same RFC-003 presence rule the real device uses: no setting_key,
    // never written back.
    c.addEntry({.id = ch::limits, .name = "limits",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::normal,
                .hasCategory = true, .category = slopsync::ui_categories::limits,
                .hasSettingChannel = true, .settingChannel = ch::limits_set});
    c.addLayoutField({.name = "window_min", .type = PackedFieldType::f32, .unit = "mm", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = ceiling::rail_mm,
                      .dflt = SettingDefault::ofFloat(factory::window_min),
                      .group = "Stroke window",
                      .desc = "Rearmost point of travel.",
                      .role = roles::window_min, .step = 1.0f,
                      .settingKey = 1, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "window_max", .type = PackedFieldType::f32, .unit = "mm", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = ceiling::rail_mm,
                      .dflt = SettingDefault::ofFloat(factory::window_max),
                      .group = "Stroke window",
                      .desc = "Frontmost point of travel. Must be greater than the rear limit.",
                      .role = roles::window_max, .step = 1.0f,
                      .settingKey = 2, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "user_speed", .type = PackedFieldType::f32, .unit = "mm/s", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = ceiling::speed_min, .max = ceiling::speed_max,
                      .dflt = SettingDefault::ofFloat(factory::user_speed),
                      .group = "Manual limits",
                      .desc = "Speed ceiling for moves driven by hand. A ceiling, not a target.",
                      .role = roles::limit_user_speed, .step = 1.0f,
                      .settingKey = 3, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "user_accel", .type = PackedFieldType::f32, .unit = "mm/s2", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = ceiling::accel_min, .max = ceiling::accel_max,
                      .dflt = SettingDefault::ofFloat(factory::user_accel),
                      .group = "Manual limits",
                      .desc = "How hard a hand-driven move is allowed to pick up speed.",
                      .role = roles::limit_user_accel, .step = 10.0f,
                      .settingKey = 4, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "max_rail", .type = PackedFieldType::f32, .unit = "mm", .scale = 1.0f,
                      .desc = "Total rail length this machine measured for itself. Read-only."});
    // Bit i gates the i-th SETTING-annotated field above, in layout order:
    //   0 window_min  1 window_max  2 user_speed  3 user_accel
    // (max_rail is skipped — no setting_key — same rule the real device uses.)
    c.addBitfieldField({.name = "enabled_mask", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f,
                        .desc = "Which of these settings the machine will accept right now.",
                        .role = roles::meta_enabled_mask},
                       {"window_min", "window_max", "user_speed", "user_accel"});

    // ---- 0x0092 "device-settings" — STATE, normal, on-change ----------------
    // THE channel the real device does not have. A DEVICE-DEFINED category
    // (>=128) with its own `category_label` — a client cannot possibly know
    // this heading's name in advance, which is exactly the point: it must
    // read `category_label` off the wire rather than assume a fixed set of
    // registry categories. `warmup_mode` proves a brand-new select renders
    // itself; `device_label` (str16) proves the free-text widget path, which
    // nothing on the real device exercises.
    c.addEntry({.id = ch::device_settings, .name = "device-settings",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::normal,
                .hasCategory = true, .category = category::bench_extras,
                .categoryLabel = "Bench Extras",
                .hasSettingChannel = true, .settingChannel = ch::device_set});
    c.addSelectField({.name = "warmup_mode", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 2.0f,
                      .dflt = SettingDefault::ofInt(factory::warmup_mode),
                      .group = "Comfort",
                      .desc = "How gently the machine ramps up when a session starts.",
                      .step = 1.0f, .settingKey = 1, .hasSettingKey = true, .hasStep = true},
                     {"off", "gentle", "full"});
    c.addLayoutField({.name = "device_label", .type = PackedFieldType::str16, .unit = "", .scale = 1.0f,
                      .dflt = SettingDefault::ofTstr(factory::device_label),
                      .group = "Comfort",
                      .desc = "A free-text name for this machine, up to 16 bytes.",
                      .role = roles::identity_name,
                      .settingKey = 2, .hasSettingKey = true});
    // Bit i gates the i-th setting-annotated field above: 0 warmup_mode, 1 device_label.
    c.addBitfieldField({.name = "enabled_mask", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f,
                        .desc = "Which of these settings the machine will accept right now.",
                        .role = roles::meta_enabled_mask},
                       {"warmup_mode", "device_label"});

    // ---- 0x0095 "motion-input" — STREAM, c2h, control, <=200 Hz -------------
    // Same wire shape as the real device's 0x0084 (dense point samples), a
    // different id and a lower rate ceiling — a cheaper machine, cheaper feed.
    c.addEntry({.id = ch::motion_input, .name = "motion-input",
                .cls = ChannelClass::STREAM, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 200.0f,
                .defaultPriority = Priority::elevated});
    c.addLayoutField({.name = "target_norm", .type = PackedFieldType::u16, .unit = "norm",   .scale = 10000.0f});
    c.addLayoutField({.name = "vel_norm",    .type = PackedFieldType::i16, .unit = "norm/s", .scale = 1000.0f});

    // ---- 0x0096 "motion-segment" — STREAM, c2h, control, <=30 Hz ------------
    c.addEntry({.id = ch::motion_segment, .name = "motion-segment",
                .cls = ChannelClass::STREAM, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 30.0f,
                .defaultPriority = Priority::elevated,
                .streamKind = slopsync::stream_kinds::segments});
    c.addLayoutField({.name = "target_norm",  .type = PackedFieldType::u16, .unit = "norm",   .scale = 10000.0f});
    c.addLayoutField({.name = "duration_ms",  .type = PackedFieldType::u16, .unit = "ms",     .scale = 1.0f});
    c.addLayoutField({.name = "end_vel_norm", .type = PackedFieldType::i16, .unit = "norm/s", .scale = 1000.0f});

    // ---- 0x0110 "limits-set" — INTENT, control, 10 Hz -----------------------
    c.addEntry({.id = ch::limits_set, .name = "limits-set",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 10.0f,
                .defaultPriority = Priority::normal});
    c.addSchemaField({.key = 1, .name = "window_min", .type = CborFieldType::f32_t, .unit = "mm",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = ceiling::rail_mm});
    c.addSchemaField({.key = 2, .name = "window_max", .type = CborFieldType::f32_t, .unit = "mm",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = ceiling::rail_mm});
    c.addSchemaField({.key = 3, .name = "user_speed", .type = CborFieldType::f32_t, .unit = "mm/s",
                      .hasMin = true, .hasMax = true, .min = ceiling::speed_min, .max = ceiling::speed_max});
    c.addSchemaField({.key = 4, .name = "user_accel", .type = CborFieldType::f32_t, .unit = "mm/s2",
                      .hasMin = true, .hasMax = true, .min = ceiling::accel_min, .max = ceiling::accel_max});

    // ---- 0x0111 "device-set" — INTENT, control, 5 Hz ------------------------
    c.addEntry({.id = ch::device_set, .name = "device-set",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 5.0f,
                .defaultPriority = Priority::normal});
    c.addSchemaField({.key = 1, .name = "warmup_mode", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 2.0f});
    c.addSchemaField({.key = 2, .name = "device_label", .type = CborFieldType::tstr_t, .unit = "",
                      .desc = "Truncated to 16 bytes on the wire (the str16 field it writes)."});

    // ---- 0x0112 "move" — INTENT, control, 20 Hz, critical -------------------
    c.addEntry({.id = ch::move, .name = "move",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 20.0f,
                .defaultPriority = Priority::critical});
    c.addSchemaField({.key = 1, .name = "position", .type = CborFieldType::f32_t, .unit = "mm",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = ceiling::rail_mm});
    c.addSchemaField({.key = 2, .name = "bypass", .type = CborFieldType::bool_t, .unit = ""});

    // ---- 0x0113 "home" — INTENT, control ------------------------------------
    // Same bench-op shape as the real device's 0x0103 (op 2 clears an e-stop
    // latch, same safety-reviewed rationale) — a different id, identical
    // semantics, because homing is protocol-shaped machinery this machine
    // still needs and nothing about "third-party" changes that.
    c.addEntry({.id = ch::home, .name = "home",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 5.0f,
                .defaultPriority = Priority::normal});
    c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = "",
                            .role = "action.home"},
                           {"reserved", "home", "force_home", "clear_override"},
                           {AccessLevel::control,   // 0 (placeholder, never an op)
                            AccessLevel::control,   // 1 home
                            AccessLevel::control,   // 2 force_home — CLEARS THE E-STOP LATCH
                            AccessLevel::control}); // 3 clear_override
    c.addSchemaField({.key = 2, .name = "stroke", .type = CborFieldType::f32_t, .unit = "mm",
                      .hasMin = true, .hasMax = true, .min = 1.0f, .max = 2000.0f});

    return c.ok();
}

}  // namespace benchrig
