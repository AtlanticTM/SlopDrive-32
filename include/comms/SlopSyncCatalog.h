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
//     (242 B) unfragmented — the largest here is 0x0088 at 88 B. ✓
//   * names ≤32 B, field names ≤24 B, units ≤8 B (schema/catalog.cddl).
//   * a fully-annotated entry must fit limits::catalog_max_entry_bytes (4096) —
//     `desc` strings are the only unbounded cost, so they stay TIGHT. The
//     conformance checker enforces it; the device-catalog test asserts it.
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
#include "slopsync/channel/log_channel.hpp"
#include "slopsync/channel/safety_events_channel.hpp"
#include "slopsync/channel/trust_channels.hpp"

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
inline constexpr uint16_t motion_segment = 0x0085;
// ---- M5a: the telemetry channels the legacy :81 plane owned ---------------
inline constexpr uint16_t plan_strip     = 0x0086;
inline constexpr uint16_t power          = 0x0087;
inline constexpr uint16_t motion_diag    = 0x0088;
inline constexpr uint16_t motion_anomaly = 0x0089;
// ---- M5b: the MODE settings the legacy :81/HTTP plane owned ---------------
// A SECOND settings category, not more fields on 0x0081 — and the reason is
// structural, not stylistic. 0x0081's RFC-009 `enabled_mask` is a bitfield8
// whose bit i gates its i-th setting-annotated field, and seven of those eight
// bits are already spoken for. A fifth limit would fit; four MODE settings
// would not, and widening the mask would change an existing field's type,
// which is a protocol break rather than the append-only evolution the packed
// layouts promise. RFC-009's own answer is the one taken here: a settings
// category that outgrows its channel SPLITS into a new STATE+INTENT pair.
inline constexpr uint16_t machine_modes  = 0x008A;
// ---- M5c: SlopMotion live tuning, off HTTP and onto the protocol ----------
// THREE state cards, ONE shared writer (0x0105). `settingChannel` is per-entry
// and `setting_key` is a key WITHIN that writer, so several STATE channels may
// name the same INTENT channel as long as their keys do not collide. That is
// what lets 17 knobs -- more than any single channel's bitfield8 enabled_mask
// can gate -- stay one coherent write path instead of three.
inline constexpr uint16_t sm_limits      = 0x008B;
inline constexpr uint16_t sm_chase       = 0x008C;
inline constexpr uint16_t sm_waveform    = 0x008D;
// ---- Advanced pattern — off the dead /api/pattern HTTP surface, onto SlopSync
// THE SAME FLATTENED-ENTRY BUDGET SPLIT AS 0x008B/C/D. AdvancedPattern.h's real
// (firmware, not legacy-JS) parameter set is 8 base controls (advpat::Settings)
// plus a 6-field cyclic Modifier PER base control (advpat::BASE_COUNT = 6) — 44
// settings total, which is exactly the ~50-field case channel/catalog.hpp's own
// comment says CatalogEntry::kMaxFields was raised to 64 (8 -> 64) FOR. But
// registry.yaml's catalog_max_entry_bytes note is the other half of that
// story: a 50-field FULLY annotated entry encodes to ~8-10 KB, so "fits in one
// entry" and "affordable in one entry" are different questions — this device
// answers the second one by splitting, same as 0x008B/C/D. One channel per
// BASE CONTROL's modifier (6 fields, well under the 8-bit enabled_mask) keeps
// every group boundary a real conceptual one instead of an artifact of
// bit-packing, exactly like sm_limits/sm_chase/sm_waveform split by subsystem
// rather than by filling every last mask bit.
inline constexpr uint16_t pattern_advanced          = 0x008E;  // ap_mode + 7 base controls
inline constexpr uint16_t pattern_adv_mod_depth1    = 0x008F;  // advpat::DEPTH_MAX modifier
inline constexpr uint16_t pattern_adv_mod_depth2    = 0x0090;  // advpat::DEPTH_MIN modifier
inline constexpr uint16_t pattern_adv_mod_speedin   = 0x0091;  // advpat::SPEED_IN modifier
inline constexpr uint16_t pattern_adv_mod_speedout  = 0x0092;  // advpat::SPEED_OUT modifier
inline constexpr uint16_t pattern_adv_mod_accelin   = 0x0093;  // advpat::ACCEL_IN modifier
inline constexpr uint16_t pattern_adv_mod_accelout  = 0x0094;  // advpat::ACCEL_OUT modifier
inline constexpr uint16_t move           = 0x0100;
inline constexpr uint16_t config_set     = 0x0101;
inline constexpr uint16_t pattern_cmd    = 0x0102;
inline constexpr uint16_t home           = 0x0103;
inline constexpr uint16_t modes_set      = 0x0104;
inline constexpr uint16_t sm_set         = 0x0105;
inline constexpr uint16_t machine_admin  = 0x0106;
// Shared writer behind ALL SEVEN 0x008E..0x0094 advanced-pattern STATE
// channels — same "one settingChannel, many cards" pattern as 0x0105.
inline constexpr uint16_t pattern_advanced_cmd = 0x0107;
// RFC-021 `pattern.frayd` preset store — retires POST /api/pattern/presets.
// See PatternPresetStore.h for the backend and the STORE/roster entries below.
inline constexpr uint16_t pattern_presets        = 0x0095;  // STORE
inline constexpr uint16_t pattern_presets_roster = 0x0096;  // its roster STATE
inline constexpr uint16_t pattern_presets_cmd    = 0x0108;  // save/load/delete/rename INTENT
}  // namespace ch

// MIRROR of PatternPresetStore::{kCapacity,kNameMax,kPayloadBytes}
// (include/comms/PatternPresetStore.h), same forced-duplication rule as
// kApBaseCount above (this header stays library-only; PatternPresetStore.h is
// itself hardware-free but still a cross-module include this header has never
// taken). SlopSyncHubService.cpp carries a static_assert pinning these
// together, so drift fails the FIRMWARE build, not a silent wire mismatch.
inline constexpr uint8_t kPresetCapacity = 24;
inline constexpr uint8_t kPresetNameMax = 32;
inline constexpr uint8_t kPresetPayloadBytes = 40;

// MIRROR of advpat::BASE_COUNT (include/motion/AdvancedPattern.h), same forced-
// duplication rule as `factory`/`ceiling` below: this header must stay
// buildable with nothing but the library (native tests, the sim), and
// AdvancedPattern.h — though itself hardware-free — is still a cross-module
// dependency this header has never taken. SlopSyncHubService.cpp DOES include
// PatternEngine.h (and therefore AdvancedPattern.h) and carries a static_assert
// pinning this to advpat::BASE_COUNT, so drift fails the FIRMWARE build, not a
// silent wire mismatch.
inline constexpr uint8_t kApBaseCount = 6;

// ---- 0x0089 motion-anomaly EVENT: the `body` (40) sub-map keys -------------
// These are the CHANNEL'S OWN schema keys, exactly as slopsync::safety_body is
// for 0x000E — that is the v1.0 EVENT grammar (registry key 40's own note: with
// kind-specific fields at the TOP level, every device-authored EVENT channel
// would need a registry PR to name its own fields). This channel is the first
// DEVICE-authored EVENT channel in the ecosystem and therefore the proof that
// the grammar fix works: nothing below required a registry change.
namespace anom_body {
inline constexpr uint8_t kind   = 1;  // slopmotion::AnomalyType, MIRRORS event_kind (see the entry)
inline constexpr uint8_t seq    = 2;  // engine's rolling event id (wraps)
inline constexpr uint8_t target = 3;  // the command target that provoked it, 0..1 normalized
inline constexpr uint8_t detail = 4;  // KIND-SPECIFIC scalar — see the option labels
inline constexpr uint8_t t_us   = 5;  // engine time at record, µs (low 32 bits)
}  // namespace anom_body

// ---- Machine FEATURES that gate whether a channel is advertised AT ALL -----
// RFC-016 in practice: "capability discovery IS catalog introspection". A hub
// with no INA228 must not advertise a power channel that would publish zeros
// forever — a client cannot tell "0.0 A" from "no sensor", and a UI that shows
// a dead gauge is the same ground-truth violation as the WebUI's dead anomaly
// panel this milestone exists to kill. So the channel is ABSENT, and its
// absence IS the answer to "does this machine measure current?".
//
// Defaults are all-false so the HOST tests and any future non-motorized build
// get the minimal catalog unless they say otherwise; the firmware fills these
// in from MotorDriver::hasCurrentSensor()/hasPowerMonitor() at construction.
struct DeviceFeatures {
    bool has_current_sensor = false;  // MotorDriver::hasCurrentSensor()
    bool has_power_monitor  = false;  // MotorDriver::hasPowerMonitor() (die temp)
};

// ---- Factory DEFAULTS advertised as RFC-009 `default` annotations ----------
// MIRROR of getDefaultConfig() in include/system/config_api.h, which cannot be
// included here: it pulls in <Arduino.h>, and this header must stay hardware-
// free (the native test suite and the sim both build it with nothing but the
// library). Duplication is therefore forced — so the drift is caught instead of
// tolerated: SlopSyncHubService.cpp, which DOES include config_api.h, carries a
// static_assert per constant below. Change a factory default there and the
// FIRMWARE fails to compile until this table follows.
namespace factory {
inline constexpr float window_min  = 0.0f;        // getDefaultConfig().min_position_mm
inline constexpr float window_max  = 500.0f;      // DEFAULT_MAX_RAIL_MM
inline constexpr float user_speed  = 50.0f;       // DEFAULT_USER_MAX_SPEED_MM_S
inline constexpr float user_accel  = 200.0f;      // DEFAULT_USER_ACCEL_MM_S2
inline constexpr float input_speed = 950.0f;      // DEFAULT_MAX_SPEED_MM_S
inline constexpr float input_accel = 50000.0f;    // DEFAULT_ACCEL_MM_S2
inline constexpr float input_jerk  = 2000000.0f;  // DEFAULT_INPUT_MAX_JERK_MM_S3
// M5b mode defaults (0x008A). Same forced-duplication rule as above — each one
// is static_assert'd against its real source in SlopSyncHubService.cpp.
inline constexpr uint8_t blend_mode        = 1;   // AIMServoDriver::_blend_mode ctor value
inline constexpr uint8_t stream_speed_mode = 0;   // SystemState::SPEED_CEILING_PEGGED
inline constexpr uint8_t overshoot_clamp   = 0;   // SystemState::interp_clamp_overshoot = false
}  // namespace factory

// ---- Hard firmware ceilings advertised as `min`/`max` ----------------------
// The bounds WebUI::applySettings actually clamps to (src/ui/WebUI.cpp), NOT
// the NORMAL/EXPERT UI guardrails — those are a client-side affordance and a
// static catalog must advertise what the hub will really accept. Same
// static_assert treatment as `factory` above.
namespace ceiling {
inline constexpr float rail_mm    = 2000.0f;      // applySettings' max_rail sanity bound
inline constexpr float speed_min  = 1.0f;
inline constexpr float speed_max  = 10000.0f;     // MAX_SPEED_MM_S
inline constexpr float accel_min  = 10.0f;
inline constexpr float accel_max  = 100000.0f;    // MAX_ACCEL_MM_S2
inline constexpr float jerk_min   = 1000.0f;
inline constexpr float jerk_max   = 50000000.0f;  // MAX_JERK_MM_S3
}  // namespace ceiling

// Fills `c` with this device's catalog. OUT-PARAM, never a return value: a
// Catalog32 is ~22 KB of pooled field storage, so returning one by value
// would put that on the caller's stack — the bug class that has already blown
// a FreeRTOS task stack on this firmware once. Returns c.ok(): false means a
// capacity in Catalog32 was exceeded while building (entries or field pools),
// which is a build-time authoring error, not a runtime condition.
//
// `feat` gates the FEATURE-DEPENDENT entries (see DeviceFeatures). It defaults
// to all-false, so an existing call site keeps the minimal catalog.
inline bool buildSlopDriveCatalog(slopsync::Catalog32& c, DeviceFeatures feat = {}) {
    using slopsync::AccessLevel;
    using slopsync::CborFieldType;
    using slopsync::ChannelClass;
    using slopsync::Direction;
    using slopsync::PackedFieldType;
    using slopsync::Priority;
    using slopsync::SettingDefault;
    namespace roles = slopsync::field_roles;

    c.clear();

    // ---- 0x0003 "safety" — STATE, critical, on-change --------------------
    // VERBATIM copy of conformance/mini_catalog.hpp's safety entry: the hub's
    // buildSafetyPayload() hardcodes exactly this 9-byte layout (word
    // bitfield8, cause u8, owner_session u32, estop_seq u16, modes bitfield8).
    // Do NOT reshape it.  [9 B]
    //
    // RFC-025c APPENDED `modes` (manual_override + bypass_limits) at v1.0.
    // These are SAFETY-domain state by operator ruling — they render near the
    // rail in a UI, but they change what the machine does with a motion
    // command, so every surface needs them and they belong on the retained,
    // critical-priority snapshot rather than a legacy HTTP endpoint. Written
    // via 0x0005 ops override_on/off + bypass_on/off. Append-only: bytes 0..7
    // keep their meaning and offsets exactly.
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

    // ---- 0x0004 "control-owner" — STATE, critical, on-change -------------
    // Matches Hub::buildControlOwnerPayload(): 4 × {source u8, owner u32}, in
    // ascending source order, 20 bytes total. Each pair is one arbiter source
    // (0 manual, 1 tcode, 2 pattern, 3 ossm) and the session id that owns it
    // (0 = unowned).  [20 B]
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

    // ---- 0x0005 "safety-intents" — INTENT, critical, modest rate ----------
    // The client sends {1:"op"} where op is a safety_ops:: value (estop=6 and
    // estop_clear=1 are hub-handled; the rest reach the delegate and the hub
    // latches the result — RFC-025a).
    //
    // *** THE ACCESS FLOOR IS `watch`, AND THAT IS THE POINT (RFC-025b). ***
    // `estop` and `stop` are ROLE-EXEMPT: anyone connected — including a
    // watch-only session — may stop this machine. §11.2's "safety outranks
    // authorization", generalized. The failure mode of getting this backwards
    // is "the person standing in the room cannot stop the machine", which is
    // not a permissions bug, it is an injury. Loop-stop spam by a viewer is
    // bounded by the §9.3 intent rate limiter above (20 Hz here) and is a
    // named, accepted risk in §12.1.
    //
    // Everything else — hold/pause/resume/estop_clear/override/bypass —
    // requires `control`, expressed as index-aligned `option_access` (catalog
    // key 17) on the enum-valued `op` field rather than as hub-side code,
    // because a GENERIC client renders this channel from the catalog and must
    // know which ops it may offer. A client that honours key 17 greys the
    // rest correctly (RFC-009's grey-never-hide); one that ignores it
    // discovers the same truth by NACK. Encoding it only in hub code would
    // make the honest client impossible.
    //
    // Index alignment is LITERAL: element i of both lists describes wire value
    // i. safety_ops starts at 1, so index 0 is a PLACEHOLDER — and it carries
    // the label "reserved" rather than "", because an option label may never
    // be empty (an unnamed choice is unrenderable, and the decoder rejects
    // one outright). Its access is `control`, the strict side, so wire value 0
    // is never the cheapest thing on this channel to reach; it NACKs
    // UNSUPPORTED_OP at the delegate regardless.
    c.addEntry({.id = slopsync::channels::safety_intents, .name = "safety-intents",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::watch, .maxRateHz = 20.0f,
                .defaultPriority = Priority::critical});
    c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = "",
                            .role = "action.safety"},
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

    // ---- 0x0006 "hub-status" — STATE, background, 1 Hz --------------------
    // Slow health telemetry.  [4+4+1+1 = 10 B]
    c.addEntry({.id = slopsync::channels::hub_status, .name = "hub-status",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 1.0f,
                .defaultPriority = Priority::background});
    c.addLayoutField({.name = "heap_free", .type = PackedFieldType::u32, .unit = "B",     .scale = 1.0f});
    c.addLayoutField({.name = "uptime_s",  .type = PackedFieldType::u32, .unit = "s",     .scale = 1.0f,
                      .role = roles::telemetry_uptime});
    c.addLayoutField({.name = "rssi",      .type = PackedFieldType::i8,  .unit = "dBm",   .scale = 1.0f});
    c.addLayoutField({.name = "sessions",  .type = PackedFieldType::u8,  .unit = "count", .scale = 1.0f});
    // M5b APPENDED (10 -> 14 B): RFC-017 / §9.4's VISIBLE drop counter for the
    // log plane. A bounded log that silently eats lines under load is a log you
    // cannot reason about, so the number has to be reachable on the wire and not
    // just in a counter someone remembers to print. Sums both places a line can
    // be lost: the hub's replay ring (Hub::logDropped) and the firmware's
    // httpTask->hub hand-off ring. Append-only — bytes 0..9 keep their offsets.
    c.addLayoutField({.name = "log_dropped", .type = PackedFieldType::u32, .unit = "count", .scale = 1.0f,
                      .desc = "Log lines dropped since boot (replay ring + cross-task bridge)."});

    // ---- 0x0007 "session-events" — EVENT, watch ----------------------------
    // Payload keys match Hub::emitTakeoverEvent(): {1:"source", 2:"session"}.
    c.addEntry({.id = slopsync::channels::session_events, .name = "session-events",
                .cls = ChannelClass::EVENT, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::normal});
    c.addSchemaField({.key = 1, .name = "source",  .type = CborFieldType::uint_t, .unit = ""});
    c.addSchemaField({.key = 2, .name = "session", .type = CborFieldType::uint_t, .unit = ""});

    // ---- 0x0008 "log" — EVENT, watch, background, replay_depth 32 ----------
    // RFC-017 / M5b: the device log, in band. Declared by the library's own
    // builder for the same reason the trust channels are — it is a SPEC-CORE
    // channel whose shape hub and client cannot negotiate, so a hand-authored
    // near-copy would be quietly non-conforming.
    //
    // Declaring it is what makes the SlopLog bridge REACHABLE: publishLog()
    // returns false on a hub whose catalog has no 0x0008, so without this line
    // the whole bridge is a no-op. The replay depth is the registry default
    // (limits::log_replay_depth_default = 32) — a client that connects AFTER a
    // fault still sees what happened, which is the one sanctioned exception to
    // §9.4's no-replay rule.
    if (!slopsync::addLogChannel(c)) return false;

    // ---- 0x0009..0x000D — the TRUST ADMINISTRATION surface (M4b) -----------
    // session-admin, pending-pairing, pairing-events, and the paired-devices
    // store + its roster, all in the canonical shapes the library declares
    // (lib/slopsync/include/slopsync/channel/trust_channels.hpp). Declared as a
    // group and by the library's own builders rather than hand-authored here,
    // because these are SPEC-CORE channels: hub and client cannot negotiate
    // their shapes, so a device that re-authored them slightly differently
    // would be quietly non-conforming with a perfectly valid-looking catalog.
    //
    // Declaring them is what makes pairing REACHABLE on this machine: the
    // approval verb is an ordinary INTENT and the hub resolves it through the
    // catalog like any other, so an undeclared 0x0009 means an operator has no
    // way to approve anything. The trust ledger's store_id (1) is discovered by
    // the hub FROM this descriptor — the catalog is self-describing, and a
    // store number is agreed by being published rather than legislated.
    if (!slopsync::addTrustChannels(c)) return false;

    // ---- 0x000E "safety-events" — EVENT, critical, watch -------------------
    // The §9.4 EVENT TWIN of the safety latch (0x0003). §5.5/§11.2 have always
    // required the hub to emit it and until M4b there was no channel to emit it
    // ON. Same access and priority as its STATE twin: an edge nobody may be
    // denied and nobody's may be shed.
    if (!slopsync::addSafetyEventsChannel(c)) return false;

    // ---- 0x0080 "motion" — STATE, elevated, 60 Hz ------------------------
    // The live carriage snapshot. scale 100 on positions = 10µm wire units;
    // scale 10 on speed = 0.1 mm/s wire units.  [2+2+2+1+2 = 9 B]
    //
    // M5a APPENDED "raw_10um" as field 5 (7 -> 9 B) — the PRE-PLANNING demand,
    // i.e. the legacy :81 0x01 TELE `raw` line, which V1-READINESS §1 named
    // "the most likely thing to be silently lost" in the migration.
    //
    // WHY HERE AND NOT A NEW CHANNEL (the choice the brief asked me to make and
    // justify): raw / target / actual are ONE measurement of ONE quantity at
    // three pipeline stages — asked, planned, achieved. Splitting them across
    // channels would hand the diagnostic CLI two independently-paced,
    // independently-conflated STATE streams and make it re-correlate samples
    // that were simultaneous at the source; at 60 Hz with per-channel grant
    // pacing that reintroduces exactly the sampling skew the plot exists to
    // measure. Appending keeps ONE frame, ONE seq, ONE timestamp, and costs a
    // client that already subscribes to motion precisely nothing extra to
    // adopt. Append-only, so bytes 0..6 keep their offsets and an old client's
    // prefix parse stays correct; the etag moves, which is the re-fetch
    // mechanism working as designed.
    //
    // The three roles are the CLI's other half: with `window.min|max` from
    // 0x0081 it converts a normalized sender intent into mm and plots it
    // against telemetry.position without hardcoding a single channel id.
    c.addEntry({.id = ch::motion, .name = "motion",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 60.0f,
                .defaultPriority = Priority::elevated});
    c.addLayoutField({.name = "pos_10um", .type = PackedFieldType::u16, .unit = "mm",   .scale = 100.0f,
                      .desc = "Where the carriage actually is.",
                      .role = roles::telemetry_position});
    c.addLayoutField({.name = "tgt_10um", .type = PackedFieldType::u16, .unit = "mm",   .scale = 100.0f,
                      .desc = "Where the motion planner is currently driving to."});
    c.addLayoutField({.name = "speed",    .type = PackedFieldType::i16, .unit = "mm/s", .scale = 10.0f,
                      .desc = "Live carriage speed; sign is the direction of travel.",
                      .role = roles::telemetry_velocity});
    c.addBitfieldField({.name = "flags", .type = PackedFieldType::bitfield8, .unit = "flag", .scale = 1.0f,
                        .desc = "Live machine mode bits."},
                       {"homed", "homing", "gen_running", "paused", "override", "estop", "stream"});
    c.addLayoutField({.name = "raw_10um", .type = PackedFieldType::u16, .unit = "mm",   .scale = 100.0f,
                      .desc = "What the controlling input ASKED for, mapped into the stroke "
                              "window, before the planner shaped it."});

    // ---- 0x0081 "machine-config" — STATE, normal, on-change ---------------
    // The full geometry + dual-limit-set snapshot in physical units (f32).
    // [8 × 4 = 32 B]
    // fw 2.1.47 APPENDED "input_jerk" as field 7 (28 → 32 B). Append-only layout
    // evolution, which the library invariants permit: every existing field keeps
    // its offset, so an old client decoding the first 28 B is still correct. The
    // etag DOES change — that is the designed re-fetch mechanism, not a break.
    // NOTE: fields live in the catalog's shared layout POOL, and M2b raised
    // CatalogEntry::kMaxFields (the CODEC's per-entry bound) from 8 to 64 — so a
    // 9th field here is now merely an append-only layout evolution + etag bump,
    // no longer a library change. :3
    //
    // M5a (RFC-009) ANNOTATED the whole surface and APPENDED "enabled_mask" as
    // field 9 (32 -> 33 B). Every field below now carries the paired INTENT key
    // (`setting_key` -> 0x0101), a factory `default`, min/max/step, a `group`
    // card heading, a USER-FACING `desc`, and a registry `role`. The entry
    // declares `settingChannel` = 0x0101 and `category` = limits, which is what
    // makes "render me a settings page" answerable from the catalog alone.
    //
    // *** max_rail DELIBERATELY CARRIES NO setting_key. *** It is DERIVED
    // MACHINE TRUTH — the rail ceiling the driver reports after homing measures
    // it (or the configured sanity bound before that), and 0x0101 has no key
    // that writes it. RFC-003's stored-vs-effective distinction IS this
    // presence test and nothing else: absent = read-only, display it with its
    // unit and never write it back into a setting's shadow. A client that
    // stomped a stored value with this one is the exact slopsync-js bug
    // (adopting the EFFECTIVE window as stored config) that produced RFC-003.
    c.addEntry({.id = ch::machine_config, .name = "machine-config",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::normal,
                .hasCategory = true, .category = slopsync::setting_categories::limits,
                .hasSettingChannel = true, .settingChannel = ch::config_set});
    c.addLayoutField({.name = "window_min",  .type = PackedFieldType::f32, .unit = "mm",    .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = ceiling::rail_mm,
                      .dflt = SettingDefault::ofFloat(factory::window_min),
                      .group = "Stroke window",
                      .desc = "Rearmost point of travel. Everything the machine is told to do is "
                              "mapped into the window between this and the front limit.",
                      .role = roles::window_min, .step = 1.0f,
                      .settingKey = 1, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "window_max",  .type = PackedFieldType::f32, .unit = "mm",    .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = ceiling::rail_mm,
                      .dflt = SettingDefault::ofFloat(factory::window_max),
                      .group = "Stroke window",
                      .desc = "Frontmost point of travel. Must be greater than the rear limit; the "
                              "machine never moves past it.",
                      .role = roles::window_max, .step = 1.0f,
                      .settingKey = 2, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "user_speed",  .type = PackedFieldType::f32, .unit = "mm/s",  .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = ceiling::speed_min, .max = ceiling::speed_max,
                      .dflt = SettingDefault::ofFloat(factory::user_speed),
                      .group = "Manual limits",
                      .desc = "Speed ceiling for moves YOU drive by hand. Kept gentle by default: "
                              "it is a ceiling, not a target.",
                      .role = roles::limit_user_speed, .step = 1.0f,
                      .settingKey = 3, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "user_accel",  .type = PackedFieldType::f32, .unit = "mm/s2", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = ceiling::accel_min, .max = ceiling::accel_max,
                      .dflt = SettingDefault::ofFloat(factory::user_accel),
                      .group = "Manual limits",
                      .desc = "How hard a hand-driven move is allowed to pick up speed. Lower "
                              "feels softer at the start and end of every move.",
                      .role = roles::limit_user_accel, .step = 10.0f,
                      .settingKey = 4, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "input_speed", .type = PackedFieldType::f32, .unit = "mm/s",  .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = ceiling::speed_min, .max = ceiling::speed_max,
                      .dflt = SettingDefault::ofFloat(factory::input_speed),
                      .group = "Machine-driven limits",
                      .desc = "Speed ceiling for everything the machine drives itself: patterns, "
                              "scripts and live streams. This is your top-speed safety limit.",
                      .role = roles::limit_input_speed, .step = 10.0f,
                      .settingKey = 5, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "input_accel", .type = PackedFieldType::f32, .unit = "mm/s2", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = ceiling::accel_min, .max = ceiling::accel_max,
                      .dflt = SettingDefault::ofFloat(factory::input_accel),
                      .group = "Machine-driven limits",
                      .desc = "How hard patterns and scripts may change speed. Raise it for snappy "
                              "content, lower it if the machine feels harsh.",
                      .role = roles::limit_input_accel, .step = 100.0f,
                      .settingKey = 6, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "max_rail",    .type = PackedFieldType::f32, .unit = "mm",    .scale = 1.0f,
                      .desc = "Total rail length this machine measured for itself. Read-only: it "
                              "is what the hardware IS, not something you choose."});
    c.addLayoutField({.name = "input_jerk",  .type = PackedFieldType::f32, .unit = "mm/s3", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = ceiling::jerk_min, .max = ceiling::jerk_max,
                      .dflt = SettingDefault::ofFloat(factory::input_jerk),
                      .group = "Machine-driven limits",
                      .desc = "How abruptly machine-driven motion may change its acceleration. "
                              "Protects the mechanics; it is not a smoothing knob.",
                      .role = roles::limit_input_jerk, .step = 1000.0f,
                      .settingKey = 7, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true, .hasStep = true});
    // RFC-009 item 4 — DYNAMIC ENABLED STATE. Bit i gates the i-th
    // SETTING-ANNOTATED field of this layout, in layout order:
    //   0 window_min  1 window_max  2 user_speed  3 user_accel
    //   4 input_speed 5 input_accel 6 input_jerk
    // max_rail is skipped because it carries no setting_key, and so is this
    // field itself — "setting-annotated" is the only membership rule, which is
    // why it needs no second list to stay in step. Disabled means GREY, NEVER
    // HIDE. Bit labels name the field each bit gates so the mapping survives
    // encode/decode without a client re-deriving it.
    c.addBitfieldField({.name = "enabled_mask", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f,
                        .desc = "Which of these settings the machine will accept right now.",
                        .role = roles::meta_enabled_mask},
                       {"window_min", "window_max", "user_speed", "user_accel",
                        "input_speed", "input_accel", "input_jerk"});

    // ---- 0x0082 "pattern-state" — STATE, normal, on-change ----------------
    // PatternEngine live snapshot.  [1+1+4+4+4+4+1 = 19 B]
    //
    // M5a: annotated + "enabled_mask" APPENDED as field 7 (18 -> 19 B).
    // category = user (this is the everyday operating surface, not the safety
    // envelope); settingChannel = 0x0102 pattern-cmd.
    //
    // `pattern` is the worked example of RFC-009 gap 3: a u8-backed
    // single-select that a generic client could previously only render as a
    // raw number. The option labels below are PatternEngine::patternName()'s
    // own strings, index-aligned with the wire value exactly as
    // setPattern(idx) consumes it — so a client shows "Teasing Pounding", not
    // "1", without knowing anything about this machine.
    //
    // Fields carry `pattern.*` roles (registry field_roles, additive) so a
    // generic client can draw a proper generator card — running toggle,
    // pattern picker, speed/depth/stroke/sensation knobs — instead of six
    // unrelated sliders. Same doctrine as every other role: a hint a client
    // MAY upgrade to a bespoke widget on, never a requirement.
    //
    // THE COMPILE-GATED TAIL: PatternEngine's registry is
    // CORE_PATTERN_COUNT (7) plus up to two build-flagged extended patterns
    // (PATTERN_EXT_TESTPATTERN1/2). Only the seven CORE names are advertised,
    // because this header is hardware-free and cannot see those flags — and
    // advertising a choice the running firmware might clamp away would be a
    // lie in exactly the direction the ground-truth doctrine forbids. A build
    // that ships the extended patterns can append their labels here; the
    // labels are index-aligned and append-only, so that is an etag bump and
    // nothing more.
    c.addEntry({.id = ch::pattern_state, .name = "pattern-state",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::normal,
                .hasCategory = true, .category = slopsync::setting_categories::user,
                .hasSettingChannel = true, .settingChannel = ch::pattern_cmd});
    c.addLayoutField({.name = "running",   .type = PackedFieldType::u8,  .unit = "",  .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f,
                      .dflt = SettingDefault::ofBool(false),
                      .group = "Pattern",
                      .desc = "Whether the built-in pattern generator is currently driving the "
                              "machine.",
                      .role = roles::pattern_running,
                      .step = 1.0f, .settingKey = 1, .hasSettingKey = true, .hasStep = true});
    c.addSelectField({.name = "pattern",   .type = PackedFieldType::u8,  .unit = "",  .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 6.0f,
                      .dflt = SettingDefault::ofInt(0),
                      .group = "Pattern",
                      .desc = "Which stroke pattern the generator plays.",
                      .role = roles::pattern_select,
                      .step = 1.0f, .settingKey = 2, .hasSettingKey = true, .hasStep = true},
                     {"Simple Stroke", "Teasing Pounding", "Robo Stroke", "Half'n'Half",
                      "Deeper", "Stop'n'Go", "Insist"});
    c.addLayoutField({.name = "speed",     .type = PackedFieldType::f32, .unit = "%", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f,
                      .dflt = SettingDefault::ofFloat(0.0f),
                      .group = "Pattern",
                      .desc = "How fast the pattern strokes, as a percentage of its own range. "
                              "Bounded by the machine-driven speed limit.",
                      .role = roles::pattern_speed,
                      .step = 1.0f, .settingKey = 3, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "depth",     .type = PackedFieldType::f32, .unit = "%", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f,
                      .dflt = SettingDefault::ofFloat(0.0f),
                      .group = "Pattern",
                      .desc = "How far into the stroke window the pattern reaches.",
                      .role = roles::pattern_depth,
                      .step = 1.0f, .settingKey = 4, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "stroke",    .type = PackedFieldType::f32, .unit = "%", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f,
                      .dflt = SettingDefault::ofFloat(0.0f),
                      .group = "Pattern",
                      .desc = "Length of each stroke, as a percentage of the available depth.",
                      .role = roles::pattern_stroke,
                      .step = 1.0f, .settingKey = 5, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "sensation", .type = PackedFieldType::f32, .unit = "",  .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f,
                      .dflt = SettingDefault::ofFloat(50.0f),
                      .group = "Pattern",
                      .desc = "Pattern character knob. 50 is neutral; what it changes depends on "
                              "the pattern you picked.",
                      .role = roles::pattern_sensation,
                      .step = 1.0f, .settingKey = 6, .hasSettingKey = true, .hasStep = true});
    // RFC-009 item 4 — bit i gates the i-th setting-annotated field above:
    //   0 running  1 pattern  2 speed  3 depth  4 stroke  5 sensation
    // Genuinely DYNAMIC here, unlike 0x0081's: the delegate refuses 0x0102
    // outright while the e-stop is latched (ESTOP_ACTIVE) or before homing
    // (NOT_HOMED), so all six bits drop together in either state and a client
    // greys the whole pattern card from one ground truth instead of
    // discovering it one NACK at a time.
    c.addBitfieldField({.name = "enabled_mask", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f,
                        .desc = "Which pattern controls the machine will accept right now.",
                        .role = roles::meta_enabled_mask},
                       {"running", "pattern", "speed", "depth", "stroke", "sensation"});

    // ---- 0x0083 "odometer" — STATE, background, 1 Hz ---------------------
    // Session totals.  [4+4+4+4+4 = 20 B]
    // M5a APPENDED "energy_wh" + "session_ms" (fields 4/5, 12 -> 20 B) — the
    // legacy :81 0x06 STATS frame's two remaining fields, which had no
    // SlopSync home. Append-only: strokes/distance_m/peak_mm_s keep their
    // offsets. energy_wh is Wh as a float rather than the legacy frame's
    // milli-Wh u32: the wire is self-describing (unit + scale), so there is no
    // reason to carry a fixed-point encoding a client has to know about.
    // Reads 0.0 forever on a machine with no power monitor — honest, and the
    // capability question is answered by 0x0087's presence, not by this field.
    c.addEntry({.id = ch::odometer, .name = "odometer",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 1.0f,
                .defaultPriority = Priority::background,
                .hasCategory = true, .category = slopsync::setting_categories::diagnostics});
    c.addLayoutField({.name = "strokes",    .type = PackedFieldType::u32, .unit = "",     .scale = 1.0f,
                      .group = "Session", .desc = "Direction reversals counted this session."});
    c.addLayoutField({.name = "distance_m", .type = PackedFieldType::f32, .unit = "m",    .scale = 1.0f,
                      .group = "Session", .desc = "Total distance the carriage has travelled this session."});
    c.addLayoutField({.name = "peak_mm_s",  .type = PackedFieldType::f32, .unit = "mm/s", .scale = 1.0f,
                      .group = "Session", .desc = "Fastest the carriage moved this session."});
    c.addLayoutField({.name = "energy_wh",  .type = PackedFieldType::f32, .unit = "Wh",   .scale = 1.0f,
                      .group = "Session", .desc = "Electrical energy drawn this session. Zero if this "
                                                  "machine has no power monitor."});
    c.addLayoutField({.name = "session_ms", .type = PackedFieldType::u32, .unit = "ms",   .scale = 1.0f,
                      .group = "Session", .desc = "Time since boot, or since the session counters were "
                                                  "last reset."});

    // ---- 0x0084 "motion-input" — STREAM, c2h, control, ≤333 Hz -----------
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
    c.addEntry({.id = ch::motion_input, .name = "motion-input",
                .cls = ChannelClass::STREAM, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 333.0f,
                .defaultPriority = Priority::elevated});
    c.addLayoutField({.name = "target_norm", .type = PackedFieldType::u16, .unit = "norm",   .scale = 10000.0f});
    c.addLayoutField({.name = "vel_norm",    .type = PackedFieldType::i16, .unit = "norm/s", .scale = 1000.0f});

    // ---- 0x0085 "motion-segment" — STREAM, c2h, control, ≤50 Hz ----------
    // TIMED-SEGMENT motion streaming: the WAVEFORM-mode companion to 0x0084.
    // Where motion-input carries dense point samples the sender interpolates
    // (chase mode, ~50 Hz), THIS channel carries the sender's NATIVE segments —
    // ONE {target, duration, end_vel} per stroke leg — which the SlopMotion
    // engine renders as a C2 quintic over EXACTLY the commanded duration (the
    // same waveform path TCode v4 drives via buttplugLinearCmd). Funscript
    // players know their segments natively, so a segment stream is ~2–4
    // packets/s instead of 50, with strictly better motion. Decoded by FIXED
    // OFFSET in the delegate's onStreamBundle() (same convention as 0x0084),
    // enqueued into the SAME SlopMotion pacing ring, and mapped to arbiter
    // source 1 / TCODE_STREAM — a client uses 0x0084 OR 0x0085, both ARE "the
    // stream input".
    //   * duration_ms is the commanded segment duration and MUST be ≥1;
    //     durationless points belong on 0x0084 (a 0 here is skipped + counted
    //     dropped, never sent to the engine).
    //   * end_vel_norm == -32768 (INT16_MIN) is the "NO end velocity" SENTINEL:
    //     0 is a legitimate slope (a reversal ends AT rest), so 0 cannot mean
    //     "absent". On the sentinel the engine estimates the boundary accel/vel
    //     itself (backward-difference af + stream vf); otherwise it honors the
    //     wire handoff velocity verbatim.
    //   * bundle sample timestamps (§5.4 t_off) are the intended segment START
    //     in hub time, resolved through the same nearest-window pacing as 0x0084.
    // scale 10000 on target = 1e-4 over the 0..1 window; scale 1000 on end_vel =
    // 1e-3 units/s, i16 signed.  [2+2+2 = 6 B]
    // RFC-014/023: streamKind = segments — EACH SAMPLE HERE CARRIES ITS OWN
    // duration_ms, so it commands a time extent, not an instant. A dropped
    // segment is a permanently lost motion command (not a recoverable
    // interpolation gap like 0x0084's points), so the hub's shedding table
    // must never decimate this channel. 0x0084 stays at the stream_kind
    // DEFAULT (samples) deliberately — absent-means-samples is the rule, and
    // this catalog demonstrates it rather than marking it redundantly.
    c.addEntry({.id = ch::motion_segment, .name = "motion-segment",
                .cls = ChannelClass::STREAM, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 50.0f,
                .defaultPriority = Priority::elevated,
                .streamKind = slopsync::stream_kinds::segments});
    c.addLayoutField({.name = "target_norm",  .type = PackedFieldType::u16, .unit = "norm",   .scale = 10000.0f});
    c.addLayoutField({.name = "duration_ms",  .type = PackedFieldType::u16, .unit = "ms",     .scale = 1.0f});
    c.addLayoutField({.name = "end_vel_norm", .type = PackedFieldType::i16, .unit = "norm/s", .scale = 1000.0f});

    // ---- 0x0086 "plan-strip" — STATE, elevated, 45 Hz --------------------
    // THE PLANNER'S CURRENT SEGMENT: what SlopMotion is executing right now,
    // as a strip you can draw. The SlopSync home of the legacy :81 0x04 INTERP
    // frame (~45 Hz), which V1-READINESS §1 notes "was tracked NOWHERE before
    // this ledger" and which powers the WebUI's planstrip. Together with
    // 0x0080's raw/tgt/pos triple it is the whole input for the diagnostic
    // graphing CLI: raw demand in, planner shape out, carriage response.
    //
    // STATE, not STREAM: this is a SNAPSHOT of a thing that is continuously
    // true (the active plan), not a series of commands or timed samples, and
    // conflation is exactly the right loss behaviour — a subscriber that falls
    // behind wants the CURRENT segment, never a backlog of stale ones. It
    // therefore stays at the stream_kind DEFAULT and declares nothing:
    // `streamKind` is read only for STREAM-class entries (isSegmentClass), so
    // marking a STATE channel `samples` would encode nothing (0 is omitted per
    // §5.3) and imply a classification that does not apply.
    //
    // Normalized units, matching the engine's own domain (1.0 == the full
    // stroke window): positions scale 10000, velocity scale 1000, both exactly
    // as the legacy frame encoded them, so a port is a re-plumb and not a
    // re-derivation. durationUs/elapsedUs stay µs u32.
    //   [1+1+2+2+2+2+4+4 = 18 B]
    c.addEntry({.id = ch::plan_strip, .name = "plan-strip",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 45.0f,
                .defaultPriority = Priority::elevated,
                .hasCategory = true, .category = slopsync::setting_categories::diagnostics});
    c.addBitfieldField({.name = "flags", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f,
                        .group = "Active plan",
                        .desc = "Whether a plan is running, and which planner produced it."},
                       {"active", "live_mode", "grad_mode"});
    c.addSelectField({.name = "style", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .group = "Active plan",
                      .desc = "Which planning mode the motion core is in."},
                     {"idle", "waveform", "chase", "settle"});
    c.addLayoutField({.name = "start_norm", .type = PackedFieldType::u16, .unit = "norm",   .scale = 10000.0f,
                      .group = "Active plan", .desc = "Where the current plan started."});
    c.addLayoutField({.name = "end_norm",   .type = PackedFieldType::u16, .unit = "norm",   .scale = 10000.0f,
                      .group = "Active plan", .desc = "Where the current plan ends."});
    c.addLayoutField({.name = "cur_norm",   .type = PackedFieldType::u16, .unit = "norm",   .scale = 10000.0f,
                      .group = "Active plan", .desc = "The setpoint the plan is producing right now."});
    c.addLayoutField({.name = "cur_vel",    .type = PackedFieldType::i16, .unit = "norm/s", .scale = 1000.0f,
                      .group = "Active plan", .desc = "The plan's velocity right now, signed."});
    c.addLayoutField({.name = "duration_us", .type = PackedFieldType::u32, .unit = "us",    .scale = 1.0f,
                      .group = "Active plan", .desc = "How long the current plan runs in total."});
    c.addLayoutField({.name = "elapsed_us",  .type = PackedFieldType::u32, .unit = "us",    .scale = 1.0f,
                      .group = "Active plan", .desc = "How far into the current plan we are."});

    // ---- 0x0087 "power" — STATE, background, 10 Hz -----------------------
    // Bus voltage / current / die temperature: the legacy :81 0x02 STATUS
    // frame's power fields, which feed the BUS A/V and DIE °C meter tiles.
    //
    // *** DECLARED ONLY WHEN THE HARDWARE EXISTS. *** RFC-016's "capability
    // discovery IS catalog introspection", made concrete: a machine with no
    // INA228 does not advertise this channel at all, so its ABSENCE is the
    // answer to "can this hub measure power?". Publishing zeros instead would
    // be indistinguishable from an idle machine, which is the same class of
    // lie as the WebUI's dead anomaly gauges.
    //
    // The two flags gate different fields and are honoured separately —
    // hasCurrentSensor() gives bus V/A, hasPowerMonitor() adds die temp — so a
    // rig with a shunt but no thermal sensor advertises a 3-field entry rather
    // than a 4-field one with a permanently-zero column. Byte offsets differ
    // between those two builds; that is fine and is precisely why the catalog
    // is fetched per firmware and etag-keyed rather than assumed.
    //
    // i_bus_mA lives HERE and not on 0x0080, deliberately. The legacy plane
    // carried it per telemetry sample; bus current is a slow, background
    // diagnostic and putting it on the 60 Hz motion snapshot would have grown
    // the highest-rate channel on the machine to carry a value nothing on the
    // motion path reads. The named consumer (the graphing CLI) wants raw vs
    // planner, not amps.
    //   [2+2 = 4 B, or +2 = 6 B with a power monitor]
    if (feat.has_current_sensor) {
        c.addEntry({.id = ch::power, .name = "power",
                    .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 10.0f,
                    .defaultPriority = Priority::background,
                    .hasCategory = true, .category = slopsync::setting_categories::diagnostics});
        c.addLayoutField({.name = "bus_mV",  .type = PackedFieldType::u16, .unit = "V", .scale = 1000.0f,
                          .group = "Power", .desc = "DC bus voltage feeding the motor drive.",
                          .role = roles::telemetry_power_bus});
        c.addLayoutField({.name = "peak_mA", .type = PackedFieldType::u16, .unit = "A", .scale = 1000.0f,
                          .group = "Power",
                          .desc = "Largest bus current seen since the counters were last reset."});
        c.addLayoutField({.name = "i_bus_mA", .type = PackedFieldType::i16, .unit = "A", .scale = 1000.0f,
                          .group = "Power", .desc = "Bus current right now; sign follows the drive.",
                          .role = roles::telemetry_current});
        if (feat.has_power_monitor) {
            c.addLayoutField({.name = "die_c10", .type = PackedFieldType::i16, .unit = "C", .scale = 10.0f,
                              .group = "Power", .desc = "Power-monitor die temperature.",
                              .role = roles::telemetry_temp});
        }
    }

    // ---- 0x0088 "slopmotion-diag" — STATE, background, 1 Hz --------------
    // The `stats` + `sync` blocks of GET /api/slopmotion, in band. Plan
    // counts, the per-kind anomaly breakdown, the on-device plan-time bench,
    // and the SlopSync stream-ingress counters.
    //
    // The per-kind counters are ten NAMED fields rather than one array
    // because "42 anomalies" told an investigation nothing and "40
    // waveform_scaled + 2 endvel_clamped" tells it everything — and a generic
    // client renders named fields with no per-device knowledge. Their order is
    // slopmotion::AnomalyType's own, which is APPEND-ONLY upstream, so a new
    // engine kind appends a field to the END OF THIS BLOCK — which does shift
    // every offset after it (the M4d `handoff_bounded` growth moved plan_us_*,
    // sync_* and reset_gen by 4 B, 80 -> 84; slopmotion 0.8.0's
    // `waveform_smoothed` moved the same three blocks another 4 B, 84 -> 88).
    // The catalog's own layout is what a client decodes against and the etag
    // moves with it, so that is a resync, not a break — before the v1.0 tag.
    // After it, an eleventh kind wants its own channel rather than a
    // reshuffled 0x0088.
    //
    // reset_gen (RFC-019) is the observable-reset half: every applied counter
    // reset increments it, so EVERY subscriber sees that a reset happened
    // rather than only the session that asked for it. Without it a client that
    // was watching the counters simply sees them jump backwards and cannot
    // tell a reset from a reboot from a wrap.
    //   [3*4 + 10*4 + 12 + 5*4 + 2 + 1 + 1 = 88 B]
    c.addEntry({.id = ch::motion_diag, .name = "slopmotion-diag",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 1.0f,
                .defaultPriority = Priority::background,
                .hasCategory = true, .category = slopsync::setting_categories::diagnostics});
    c.addLayoutField({.name = "plans",    .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Planner", .desc = "Motion plans computed successfully."});
    c.addLayoutField({.name = "failures", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Planner", .desc = "Commands the planner rejected; the previous plan kept running."});
    c.addLayoutField({.name = "anomalies", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Planner", .desc = "Total planner anomalies of every kind."});
    c.addSelectField({.name = "mode",      .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .group = "Planner", .desc = "Which planning mode the motion core is in."},
                     {"idle", "waveform", "chase", "settle"});
    // Options are indexed by slopmotion::PlanKind and the enum is APPEND-ONLY.
    // "cubic" (=3) arrived with curve_policy/ForceC1: a C1 cubic and a C2 quintic
    // are different curves and the client must be able to tell them apart, so
    // this list grows rather than collapsing both into "hermite".
    c.addSelectField({.name = "plan_kind", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .group = "Planner", .desc = "Which curve the active plan is."},
                     {"none", "quintic", "ruckig", "cubic"});
    // Per-kind breakdown — names are slopmotion::AnomalyType's, index 0 is
    // the engine's own "none" placeholder and is never counted.
    c.addLayoutField({.name = "anom_none",        .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Anomalies", .desc = "Placeholder slot; never counts."});
    c.addLayoutField({.name = "anom_plan_failed", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Anomalies", .desc = "A command could not be planned at all."});
    c.addLayoutField({.name = "anom_settle",      .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Anomalies", .desc = "The stream went quiet mid-move; the machine braked to rest."});
    c.addLayoutField({.name = "anom_endvel_clamped", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Anomalies", .desc = "A handoff speed was cut back to stay inside the window."});
    c.addLayoutField({.name = "anom_deadline_stretched", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Anomalies", .desc = "A move needed longer than the time it was given."});
    c.addLayoutField({.name = "anom_waveform_fallback",  .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Anomalies", .desc = "The sender's curve broke a limit; the machine reshaped it."});
    c.addLayoutField({.name = "anom_waveform_scaled",    .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Anomalies", .desc = "A stroke was shortened to finish on time."});
    c.addLayoutField({.name = "anom_waveform_centred",   .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Anomalies", .desc = "A shortened stroke was re-centred on its midpoint."});
    c.addLayoutField({.name = "anom_handoff_bounded",    .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Anomalies",
                      .desc = "A sender asked to arrive at a speed the next segment could not "
                              "absorb; the machine bounded it."});
    c.addLayoutField({.name = "anom_waveform_smoothed", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Anomalies",
                      .desc = "A curve was flattened toward a straight line so the machine "
                              "could keep the timing without losing the stroke."});
    c.addLayoutField({.name = "plan_us_last", .type = PackedFieldType::u32, .unit = "us", .scale = 1.0f,
                      .group = "Plan time", .desc = "Time the most recent plan took to compute."});
    c.addLayoutField({.name = "plan_us_max",  .type = PackedFieldType::u32, .unit = "us", .scale = 1.0f,
                      .group = "Plan time", .desc = "Worst plan time since the counters were reset."});
    c.addLayoutField({.name = "plan_us_avg",  .type = PackedFieldType::f32, .unit = "us", .scale = 1.0f,
                      .group = "Plan time", .desc = "Smoothed average plan time."});
    c.addLayoutField({.name = "sync_bundles",  .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Stream ingress", .desc = "Motion bundles accepted over SlopSync."});
    c.addLayoutField({.name = "sync_samples",  .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Stream ingress", .desc = "Motion samples decoded from those bundles."});
    c.addLayoutField({.name = "sync_enqueued", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Stream ingress", .desc = "Samples that reached the motion core."});
    c.addLayoutField({.name = "sync_dropped",  .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Stream ingress",
                      .desc = "Samples discarded before the motion core: too late, unusable, or "
                              "refused because the machine was not ready."});
    c.addLayoutField({.name = "sync_seg_bundles", .type = PackedFieldType::u32, .unit = "", .scale = 1.0f,
                      .group = "Stream ingress", .desc = "How many of those bundles were timed segments."});
    c.addLayoutField({.name = "reset_gen", .type = PackedFieldType::u16, .unit = "", .scale = 1.0f,
                      .group = "Planner",
                      .desc = "Counts up every time these counters are reset, so every viewer "
                              "sees the reset and not just whoever asked for it.",
                      .role = roles::meta_reset_gen});

    // ---- 0x0089 "motion-anomaly" — EVENT, watch, normal ------------------
    // SlopMotion's anomaly feed, as EDGES. The roadmap already prescribed this
    // channel, and it is a live ground-truth REPAIR, not a new feature: the
    // legacy :81 0x05 ANOMALY ring is written only by the superseded
    // MotionInterpolator, so today the WebUI's anomaly panel renders dead
    // gauges while the real engine's anomalies go only to SlopLog. This is the
    // feed that panel gets rebuilt against.
    //
    // FIRST DEVICE-AUTHORED EVENT CHANNEL — and therefore the proof that the
    // M3b `body` (40) grammar fix works. Every field below is keyed by THIS
    // CHANNEL'S OWN schema (slopdrive::anom_body), so naming them cost no
    // registry PR; under the pre-fix grammar (kind-specific fields at the top
    // level) this channel could not have existed without one, which was the
    // precise coupling the self-describing catalog exists to prevent.
    //
    // `kind` appears BOTH as the frame's event_kind (33) — the protocol's own
    // discriminator, which is what a client switches on — and as body key 1
    // carrying the identical value. That is not redundancy for its own sake:
    // the catalog has no vocabulary for LABELLING event kinds (there is no
    // per-entry kind-label list), and `options` on a schema field is the one
    // registered mechanism for turning a number into a name. Mirroring it into
    // the body is what lets a generic client print "waveform_scaled" instead
    // of "6". Worth a future RFC; not worth inventing a key for here.
    //
    // NO replay depth: an anomaly is an edge, and §9.4's default (edges are
    // never replayed) is right for it. The counters on 0x0088 are the durable
    // record — that is the event/state duality doing its job.
    c.addEntry({.id = ch::motion_anomaly, .name = "motion-anomaly",
                .cls = ChannelClass::EVENT, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::normal,
                .hasCategory = true, .category = slopsync::setting_categories::diagnostics});
    c.addSelectSchemaField({.key = anom_body::kind, .name = "kind", .type = CborFieldType::uint_t,
                            .unit = "",
                            .desc = "What the motion core had to do differently, and why."},
                           {"none", "plan_failed", "settle", "endvel_clamped", "deadline_stretched",
                            "waveform_fallback", "waveform_scaled", "waveform_centred",
                            "handoff_bounded", "waveform_smoothed"});
    c.addSchemaField({.key = anom_body::seq, .name = "seq", .type = CborFieldType::uint_t, .unit = "",
                      .desc = "Rolling event id from the motion core; wraps."});
    c.addSchemaField({.key = anom_body::target, .name = "target", .type = CborFieldType::f32_t,
                      .unit = "norm",
                      .desc = "The commanded position that provoked it, 0..1 across the stroke window."});
    c.addSchemaField({.key = anom_body::detail, .name = "detail", .type = CborFieldType::f32_t, .unit = "",
                      .desc = "Kind-specific number: the clamped speed, the stretched duration, or "
                              "the fraction of the stroke actually achieved."});
    c.addSchemaField({.key = anom_body::t_us, .name = "t_us", .type = CborFieldType::uint_t, .unit = "us",
                      .desc = "Motion-core time when it happened."});

    // ---- 0x008A "machine-modes" — STATE, elevated, on-change -------------
    // M5b. The four MODE settings the legacy :81/HTTP plane owned outright:
    // WS_OP_BLEND (0x07), WS_OP_MODE (0x06), WS_OP_STREAM_MODE (0x12) and
    // WS_OP_OVERSHOOT (0x13). Until now a SlopSync-only client could not read
    // them at all, let alone set them — the WebUI got them from /api/settings
    // over HTTP, which is exactly the control surface being retired.
    //
    // They are MODES, not limits: each one changes what the machine DOES with a
    // command rather than how far or how fast it may go. That is why they are
    // their own category rather than more fields on 0x0081 (see ch::
    // machine_modes for the enabled_mask arithmetic that makes the split
    // structural rather than tidy-minded).
    //
    // Layout [1+1+1+1+1 = 5 B], all u8 — small enough that the on-change
    // cadence costs nothing, and every value is an enum the catalog names, so a
    // generic client renders four dropdowns without knowing this device exists.
    c.addEntry({.id = ch::machine_modes, .name = "machine-modes",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::elevated,
                .hasCategory = true, .category = slopsync::setting_categories::user,
                .hasSettingChannel = true, .settingChannel = ch::modes_set});
    // Blend is 1-BASED on this machine (the driver constrains to [1,3]), so the
    // options array carries a dead index 0. Naming it "—" rather than omitting
    // it keeps wire value == option index, which is the whole contract of a
    // select field; a client must never have to subtract one.
    c.addSelectField({.name = "blend_mode", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .dflt = SettingDefault::ofInt(factory::blend_mode),
                      .group = "Motion behaviour",
                      // <=128 B: the registry caps desc and the probe enforces
                      // it. The option LABELS carry the per-choice detail, so
                      // this only has to say what the setting is about.
                      .desc = "What the machine does when a new move arrives while one is "
                              "still running.",
                      .settingKey = 1, .hasSettingKey = true},
                     {"—", "let-it-land", "allow-reversal", "hybrid"});
    c.addSelectField({.name = "stream_speed_mode", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .dflt = SettingDefault::ofInt(factory::stream_speed_mode),
                      .group = "Motion behaviour",
                      .desc = "How a streamed point picks its speed: the machine's ceiling, "
                              "or the speed the sender asked for.",
                      .settingKey = 3, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true},
                     {"ceiling-pegged", "velocity-matched"});
    c.addSelectField({.name = "overshoot_clamp", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .dflt = SettingDefault::ofInt(factory::overshoot_clamp),
                      .group = "Motion behaviour",
                      .desc = "Stops a smoothed curve from bulging past the points it was given. "
                              "Costs a little smoothness to remove overshoot micromotion.",
                      .settingKey = 4, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true},
                     {"off", "on"});
    // Bit i gates the i-th setting-annotated field, same rule as 0x0081.
    // TRANSPORT IS THE ONE THAT ACTUALLY MOVES: switching input source while
    // the machine is being driven would yank control out from under a live
    // session, so the machine reports that bit low while a pattern or stream is
    // running. The other three are safe to change at any time — they take
    // effect on the NEXT move rather than reshaping the one in flight.
    c.addBitfieldField({.name = "enabled_mask", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f,
                        .desc = "Which of these the machine will accept right now.",
                        .role = roles::meta_enabled_mask},
                       {"blend_mode", "stream_speed_mode", "overshoot_clamp"});

    // ---- 0x008B/0x008C/0x008D "slopmotion-*" — STATE, tuning ---------------
    // M5c: the SlopMotion live-tune surface, off HTTP and onto the protocol.
    // POST /api/slopmotion retires with it — no controls outside SlopSync.
    //
    // THREE CHANNELS, ONE TAB. A settings channel is capped at 8 settings
    // because its RFC-009 enabled_mask is a bitfield8 and bit i gates the i-th
    // setting of ITS layout. That is a WIRE limit the user never sees: SPEC
    // §8.8 — "a category spans channels; two channels in the same category
    // merge into one tab" — so all three carry category = tuning and differ
    // only by `group`. 20 knobs, one Tuning tab, three cards, nothing dropped
    // to make it fit.
    //
    // ONE SHARED WRITER (0x0105). `settingChannel` is per-entry and
    // `setting_key` is a key WITHIN that writer, so several STATE channels may
    // name the same INTENT channel provided their keys never collide. Keys are
    // allocated 1..20 across the three cards and are never reused.
    //
    // PERSISTED to NVS (operator ruling 2026-07-27). /api/slopmotion was a
    // session-only surface that reset on reboot; these are real settings and
    // survive one. That is also why they carry `default` annotations — a
    // generic client needs to offer "reset to factory" for a value that sticks.
    c.addEntry({.id = ch::sm_limits, .name = "slopmotion-limits",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::background,
                .hasCategory = true, .category = slopsync::setting_categories::tuning,
                .hasSettingChannel = true, .settingChannel = ch::sm_set});
    c.addLayoutField({.name = "jmax_ovr", .type = PackedFieldType::f32, .unit = "1/s3", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 2000000.0f,
                      .dflt = SettingDefault::ofFloat(0.0f), .group = "Ceiling overrides",
                      .desc = "Jerk ceiling for the planner. 0 derives it from the machine limits.",
                      .role = "", .step = 1000.0f,
                      .settingKey = 1, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "vmax_ovr", .type = PackedFieldType::f32, .unit = "1/s", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 20.0f,
                      .dflt = SettingDefault::ofFloat(0.0f), .group = "Ceiling overrides",
                      .desc = "Speed ceiling override, normalised. 0 derives it from the mm limits.",
                      .settingKey = 2, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true});
    c.addLayoutField({.name = "amax_ovr", .type = PackedFieldType::f32, .unit = "1/s2", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 500.0f,
                      .dflt = SettingDefault::ofFloat(0.0f), .group = "Ceiling overrides",
                      .desc = "Acceleration ceiling override, normalised. 0 derives it from the mm limits.",
                      .settingKey = 3, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true});
    c.addSelectField({.name = "centring", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .dflt = SettingDefault::ofInt(1), .group = "Centring",
                      .desc = "Pulls a drifting waveform back toward the middle of the stroke window.",
                      .settingKey = 4, .hasSettingKey = true},
                     {"off", "on"});
    c.addLayoutField({.name = "centring_gain", .type = PackedFieldType::f32, .unit = "", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f,
                      .dflt = SettingDefault::ofFloat(1.0f), .group = "Centring",
                      .desc = "How hard centring pulls. Higher recentres faster and follows the script less.",
                      .step = 0.05f, .settingKey = 5, .hasSettingKey = true, .hasStep = true});
    c.addBitfieldField({.name = "enabled_mask", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f, .desc = "Which of these the machine will accept right now.",
                        .role = roles::meta_enabled_mask},
                       {"jmax_ovr", "vmax_ovr", "amax_ovr", "centring", "centring_gain"});

    // BOTH INPUT PATHS ARE LIVE AND IN USE. These knobs steer the CHASE path
    // (dense sample streams — MFP's Samples mode); the waveform path
    // (timed segments — MFP's Segments mode) has its own card below. This is a
    // per-path split, NOT a legacy one: neither path is deprecated and the
    // plugin ships both.
    //
    // All of them are wired — every one reaches the engine config on the
    // per-tick push — so the mask reports them ENABLED, which is the truth. A
    // knob that is accepted but whose path is not currently active is a
    // different statement from a knob the machine refuses, and greying it would
    // be exactly the lie enabled_mask exists to prevent.
    c.addEntry({.id = ch::sm_chase, .name = "slopmotion-chase",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::background,
                .hasCategory = true, .category = slopsync::setting_categories::tuning,
                .hasSettingChannel = true, .settingChannel = ch::sm_set});
    c.addSelectField({.name = "chase_ff", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .dflt = SettingDefault::ofInt(1), .group = "Sample streams",
                      .desc = "Aim at where the sender is heading, not just where it last was.",
                      .settingKey = 6, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true},
                     {"off", "on"});
    c.addSelectField({.name = "chase_accel_ff", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .dflt = SettingDefault::ofInt(1), .group = "Sample streams",
                      .desc = "Also match how the sender's speed is changing, not just its speed.",
                      .settingKey = 7, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true},
                     {"off", "on"});
    c.addLayoutField({.name = "chase_gain", .type = PackedFieldType::f32, .unit = "", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.5f,
                      .dflt = SettingDefault::ofFloat(0.9f), .group = "Sample streams",
                      .desc = "Damping on the speed estimate. Lower is steadier, higher is more responsive.",
                      .step = 0.05f, .settingKey = 8, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "chase_lookahead", .type = PackedFieldType::f32, .unit = "", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 8.0f,
                      .dflt = SettingDefault::ofFloat(3.0f), .group = "Sample streams",
                      .desc = "How far ahead to aim, in stream intervals. Too far overshoots at turns.",
                      .step = 0.5f, .settingKey = 9, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "chase_dense_ms", .type = PackedFieldType::u32, .unit = "ms", .scale = 1000.0f,
                      .hasMin = true, .hasMax = true, .min = 10.0f, .max = 500.0f,
                      .dflt = SettingDefault::ofFloat(60.0f), .group = "Sample streams",
                      .desc = "Streams faster than this count as dense and get predictive aiming.",
                      .settingKey = 10, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true});
    c.addSelectField({.name = "chase_aim_extrap", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .dflt = SettingDefault::ofInt(1), .group = "Sample streams",
                      .desc = "Second-order aiming. Sharper tracking, but can overshoot at turn points.",
                      .settingKey = 11, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true},
                     {"off", "on"});
    c.addLayoutField({.name = "handoff_k", .type = PackedFieldType::f32, .unit = "", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 8.0f,
                      .dflt = SettingDefault::ofFloat(1.5f), .group = "Sample streams",
                      .desc = "Bound on handoff speed between moves, as a multiple of the chord.",
                      .step = 0.1f, .settingKey = 12, .flags = slopsync::setting_flags::advanced,
                      .hasSettingKey = true, .hasStep = true});
    c.addBitfieldField({.name = "enabled_mask", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f, .desc = "Which of these the machine will accept right now.",
                        .role = roles::meta_enabled_mask},
                       {"chase_ff", "chase_accel_ff", "chase_gain", "chase_lookahead",
                        "chase_dense_ms", "chase_aim_extrap", "handoff_k"});

    // The WAVEFORM path (timed segments — MFP's Segments mode). Equally live.
    c.addEntry({.id = ch::sm_waveform, .name = "slopmotion-waveform",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::background,
                .hasCategory = true, .category = slopsync::setting_categories::tuning,
                .hasSettingChannel = true, .settingChannel = ch::sm_set});
    c.addSelectField({.name = "curve_policy", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .dflt = SettingDefault::ofInt(0), .group = "Curve",
                      .desc = "Rebuild the sender's curve as sent, or force one smoothness family.",
                      .settingKey = 13, .hasSettingKey = true},
                     {"follow client", "force C1", "force C2"});
    c.addSelectField({.name = "infeasible_policy", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .dflt = SettingDefault::ofInt(2), .group = "Infeasible moves",
                      .desc = "What to do when a move cannot be finished in the time it was given.",
                      .settingKey = 14, .hasSettingKey = true},
                     {"stretch", "scale", "reshape", "prioritise amplitude", "prioritise smooth"});
    c.addLayoutField({.name = "infeasible_margin", .type = PackedFieldType::f32, .unit = "", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.5f, .max = 1.0f,
                      .dflt = SettingDefault::ofFloat(0.92f), .group = "Infeasible moves",
                      .desc = "How much of the stroke a scaled-down move keeps.",
                      .step = 0.01f, .settingKey = 15, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "smooth_budget", .type = PackedFieldType::f32, .unit = "", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f,
                      .dflt = SettingDefault::ofFloat(0.5f), .group = "Infeasible moves",
                      .desc = "How much smoothness may be spent before amplitude is touched.",
                      .step = 0.05f, .settingKey = 16, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "amplitude_budget", .type = PackedFieldType::f32, .unit = "", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f,
                      .dflt = SettingDefault::ofFloat(0.5f), .group = "Infeasible moves",
                      .desc = "How much stroke length may be spent before smoothness is touched.",
                      .step = 0.05f, .settingKey = 17, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "blend_steps", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 1.0f, .max = 10.0f,
                      .dflt = SettingDefault::ofInt(6), .group = "Infeasible moves",
                      .desc = "How gradually a budget is spent. More steps is smoother, slower to settle.",
                      .settingKey = 18, .hasSettingKey = true});
    c.addLayoutField({.name = "reshape_steps", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 8.0f,
                      .dflt = SettingDefault::ofInt(6), .group = "Infeasible moves",
                      .desc = "Attempts allowed when reshaping a move to fit its deadline.",
                      .settingKey = 19, .hasSettingKey = true});
    c.addLayoutField({.name = "settle_grace_ms", .type = PackedFieldType::u32, .unit = "ms", .scale = 1000.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 200.0f,
                      .dflt = SettingDefault::ofFloat(30.0f), .group = "Settling",
                      .desc = "Quiet time after a stream stops before the machine brakes to rest.",
                      .settingKey = 20, .hasSettingKey = true});
    c.addBitfieldField({.name = "enabled_mask", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f, .desc = "Which of these the machine will accept right now.",
                        .role = roles::meta_enabled_mask},
                       {"curve_policy", "infeasible_policy", "infeasible_margin", "smooth_budget",
                        "amplitude_budget", "blend_steps", "reshape_steps", "settle_grace_ms"});

    // ---- 0x008E "pattern-advanced" — STATE, normal, on-change -------------
    // Advanced mode's 8 BASE controls (advpat::Settings, everything except the
    // per-control cyclic Modifier — see 0x008F..0x0094 for those). This is the
    // real fix the roadmap asked for: POST /api/pattern used to carry ap_mode/
    // ap_speed/ap_max_depth/ap_min_depth/ap_in_speed/ap_out_speed/ap_in_accel/
    // ap_out_accel as ad-hoc JSON keys a generic client could not discover; now
    // they are 8 RFC-009 settings a generic client renders without knowing this
    // firmware exists. That endpoint answers 410 today (M5c); this channel is
    // what makes Advanced mode reachable again at all.
    //
    // SAME CATEGORY AS 0x0082 (`user`), DIFFERENT settingChannel (0x0107, not
    // 0x0102): Advanced is a separate sub-mode of the SAME pattern generator,
    // not a seventh classic-pattern field, so it earns its own writer while
    // sharing the category so both render as ONE tab (SPEC §8.8 — "a category
    // spans channels").
    //   [1*8 fields + 1 mask = 9 B]
    c.addEntry({.id = ch::pattern_advanced, .name = "pattern-advanced",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::normal,
                .hasCategory = true, .category = slopsync::setting_categories::user,
                .hasSettingChannel = true, .settingChannel = ch::pattern_advanced_cmd});
    c.addLayoutField({.name = "ap_mode", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f,
                      .dflt = SettingDefault::ofBool(false),
                      .group = "Advanced pattern",
                      .desc = "Drive the generator with Advanced mode instead of the classic patterns.",
                      .step = 1.0f, .settingKey = 1, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "master", .type = PackedFieldType::u8, .unit = "%", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f,
                      .dflt = SettingDefault::ofInt(0),
                      .group = "Advanced pattern",
                      .desc = "Overall stroke speed. 0 holds position.",
                      .step = 1.0f, .settingKey = 2, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "max_depth", .type = PackedFieldType::u8, .unit = "%", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f,
                      .dflt = SettingDefault::ofInt(10),
                      .group = "Depth window",
                      .desc = "Deepest point of the stroke (the in-stroke target).",
                      .step = 1.0f, .settingKey = 3, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "min_depth", .type = PackedFieldType::u8, .unit = "%", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f,
                      .dflt = SettingDefault::ofInt(0),
                      .group = "Depth window",
                      .desc = "Shallowest point of the stroke (the out-stroke target).",
                      .step = 1.0f, .settingKey = 4, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "in_speed", .type = PackedFieldType::u8, .unit = "%", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 1.0f, .max = 100.0f,
                      .dflt = SettingDefault::ofInt(100),
                      .group = "Speed",
                      .desc = "In-stroke speed, as a percentage of master speed.",
                      .step = 1.0f, .settingKey = 5, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "out_speed", .type = PackedFieldType::u8, .unit = "%", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 1.0f, .max = 100.0f,
                      .dflt = SettingDefault::ofInt(100),
                      .group = "Speed",
                      .desc = "Out-stroke speed, as a percentage of master speed.",
                      .step = 1.0f, .settingKey = 6, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "in_accel", .type = PackedFieldType::u8, .unit = "%", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f,
                      .dflt = SettingDefault::ofInt(40),
                      .group = "Acceleration",
                      .desc = "How hard the in-stroke accelerates.",
                      .step = 1.0f, .settingKey = 7, .hasSettingKey = true, .hasStep = true});
    c.addLayoutField({.name = "out_accel", .type = PackedFieldType::u8, .unit = "%", .scale = 1.0f,
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f,
                      .dflt = SettingDefault::ofInt(40),
                      .group = "Acceleration",
                      .desc = "How hard the out-stroke accelerates.",
                      .step = 1.0f, .settingKey = 8, .hasSettingKey = true, .hasStep = true});
    // Bit i gates the i-th setting-annotated field above, same rule as 0x0082.
    // GENUINELY dynamic, and genuinely NARROWER than 0x0082's: unlike `running`
    // on 0x0102, none of these 8 setters is gated on `homed` (PatternEngine::
    // setAdvancedMode/setApMaster/setApBase have no homed check — only start()
    // does), so mirroring 0x0082's mask formula here would be a DISHONEST
    // refusal the delegate never actually makes. The mask therefore tracks
    // e-stop alone; the fields simply have no effect on the machine until it is
    // homed and running, same as dialling in a pattern before pressing start.
    c.addBitfieldField({.name = "enabled_mask", .type = PackedFieldType::bitfield8, .unit = "flag",
                        .scale = 1.0f,
                        .desc = "Which of these the machine will accept right now.",
                        .role = roles::meta_enabled_mask},
                       {"ap_mode", "master", "max_depth", "min_depth", "in_speed", "out_speed",
                        "in_accel", "out_accel"});

    // ---- 0x008F..0x0094 "pattern-adv-mod-*" — STATE, background -----------
    // The 6-field cyclic Modifier (advpat::Modifier) that rides EACH of the 6
    // base controls (advpat::BASE_COUNT) — the "modifier cycle" the roadmap
    // asked for: amplitude ramps a control's swing in over `in_step` strokes,
    // holds `in_wait`, ramps back over `out_step`, rests `out_wait`, and
    // `offset` phase-shifts the whole cycle. ONE CHANNEL PER BASE CONTROL
    // rather than binpacking 36 fields into the fewest possible 8-field
    // channels: each is a real, separate concept (fray-d lets you set a
    // completely different breathing pattern on depth vs. speed vs. accel),
    // and a channel boundary that means something is worth six channel ids
    // more than a denser one that doesn't — same judgement 0x008B/C/D already
    // made splitting by subsystem, not by bit-count.
    //
    // ALL SIX SHARE 0x0107 as settingChannel (see 0x008E) and `user` as
    // category, so all seven advanced-pattern cards merge into ONE tab.
    // setting_keys are allocated 9..44 across the six, 6 keys apiece, and
    // match the wire layout below exactly: keyBase+0 amplitude, +1 in_step,
    // +2 in_wait, +3 out_step, +4 out_wait, +5 offset — which is also
    // SlopSyncHubService's applyIntent(0x0107) grouping formula
    // (base = 9 + 6*advpat::BaseId), so the two can be eyeballed against each
    // other without cross-referencing a third table. `advanced`-flagged: this
    // is the deep-customization layer under the 8 base controls, not the
    // everyday knobs.
    //   [1*6 fields + 1 mask = 7 B, ×6 channels]
    auto addApModifierChannel = [&](uint16_t id, const char* wireName, const char* group,
                                    uint8_t keyBase) {
        c.addEntry({.id = id, .name = wireName,
                    .cls = ChannelClass::STATE, .dir = Direction::h2c,
                    .access = AccessLevel::watch, .maxRateHz = 0.0f,
                    .defaultPriority = Priority::background,
                    .hasCategory = true, .category = slopsync::setting_categories::user,
                    .hasSettingChannel = true, .settingChannel = ch::pattern_advanced_cmd});
        c.addLayoutField({.name = "amplitude", .type = PackedFieldType::u8, .unit = "%", .scale = 1.0f,
                          .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f,
                          .dflt = SettingDefault::ofInt(100), .group = group,
                          .desc = "Modulation strength; 100 = off.",
                          .step = 1.0f, .settingKey = uint8_t(keyBase + 0),
                          .flags = slopsync::setting_flags::advanced,
                          .hasSettingKey = true, .hasStep = true});
        c.addLayoutField({.name = "in_step", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                          .hasMin = true, .hasMax = true, .min = 1.0f, .max = 25.0f,
                          .dflt = SettingDefault::ofInt(1), .group = group,
                          .desc = "Strokes ramping into the modulation.",
                          .step = 1.0f, .settingKey = uint8_t(keyBase + 1),
                          .flags = slopsync::setting_flags::advanced,
                          .hasSettingKey = true, .hasStep = true});
        c.addLayoutField({.name = "in_wait", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                          .hasMin = true, .hasMax = true, .min = 0.0f, .max = 25.0f,
                          .dflt = SettingDefault::ofInt(0), .group = group,
                          .desc = "Strokes held at full modulation.",
                          .step = 1.0f, .settingKey = uint8_t(keyBase + 2),
                          .flags = slopsync::setting_flags::advanced,
                          .hasSettingKey = true, .hasStep = true});
        c.addLayoutField({.name = "out_step", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                          .hasMin = true, .hasMax = true, .min = 1.0f, .max = 25.0f,
                          .dflt = SettingDefault::ofInt(1), .group = group,
                          .desc = "Strokes ramping back out.",
                          .step = 1.0f, .settingKey = uint8_t(keyBase + 3),
                          .flags = slopsync::setting_flags::advanced,
                          .hasSettingKey = true, .hasStep = true});
        c.addLayoutField({.name = "out_wait", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                          .hasMin = true, .hasMax = true, .min = 0.0f, .max = 25.0f,
                          .dflt = SettingDefault::ofInt(0), .group = group,
                          .desc = "Strokes resting before the cycle repeats.",
                          .step = 1.0f, .settingKey = uint8_t(keyBase + 4),
                          .flags = slopsync::setting_flags::advanced,
                          .hasSettingKey = true, .hasStep = true});
        c.addLayoutField({.name = "offset", .type = PackedFieldType::u8, .unit = "", .scale = 1.0f,
                          .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f,
                          .dflt = SettingDefault::ofInt(0), .group = group,
                          .desc = "Phase shift of the cycle.",
                          .step = 1.0f, .settingKey = uint8_t(keyBase + 5),
                          .flags = slopsync::setting_flags::advanced,
                          .hasSettingKey = true, .hasStep = true});
        // Same honesty note as 0x008E: no setter here checks `homed` either
        // (setApModifier has no gate beyond the delegate's e-stop check), so
        // the mask tracks e-stop alone.
        c.addBitfieldField({.name = "enabled_mask", .type = PackedFieldType::bitfield8, .unit = "flag",
                            .scale = 1.0f,
                            .desc = "Which of these the machine will accept right now.",
                            .role = roles::meta_enabled_mask},
                           {"amplitude", "in_step", "in_wait", "out_step", "out_wait", "offset"});
    };
    addApModifierChannel(ch::pattern_adv_mod_depth1,   "pattern-adv-mod-depth1",   "Depth 1 modifier",   9);
    addApModifierChannel(ch::pattern_adv_mod_depth2,   "pattern-adv-mod-depth2",   "Depth 2 modifier",   15);
    addApModifierChannel(ch::pattern_adv_mod_speedin,  "pattern-adv-mod-speedin",  "Speed in modifier",  21);
    addApModifierChannel(ch::pattern_adv_mod_speedout, "pattern-adv-mod-speedout", "Speed out modifier", 27);
    addApModifierChannel(ch::pattern_adv_mod_accelin,  "pattern-adv-mod-accelin",  "Accel in modifier",  33);
    addApModifierChannel(ch::pattern_adv_mod_accelout, "pattern-adv-mod-accelout", "Accel out modifier", 39);

    // ---- 0x0095 "pattern-presets" — STORE, control -------------------------
    // RFC-021's `pattern.frayd` worked example, landed: retires the last HTTP
    // writer, POST /api/pattern/presets (NVS "advpreset", 24 x {name, def}
    // opaque JSON). `access = control` matches this store's CRUD writer
    // (0x0108) — same tier as every other pattern control, not `configure`
    // (unlike 0x000C paired-devices, this is not an admin/security surface).
    // storeId 2 (1 is the trust ledger, RFC-027/029) — self-describing, agreed
    // by being published here rather than legislated.
    //
    // perItemMax/nameMax are declared, not measured against the wire: the
    // payload is opaque device-defined bytes (in/out speed, in/out accel, six
    // modifier blocks — the SAME fields the retired handler's `def` carried,
    // "never depths or master speed"). See PatternPresetStore.h for the
    // 40-byte layout and SlopDriveHubDelegate::applyIntent's 0x0108 case for
    // the encode/decode.
    c.addEntry({.id = ch::pattern_presets, .name = "pattern-presets",
                .cls = ChannelClass::STORE, .dir = Direction::h2c,
                .access = AccessLevel::control, .maxRateHz = 0.0f,
                .defaultPriority = Priority::background});
    c.addStoreDescriptor({.storeId = 2, .kind = "pattern.frayd",
                          .capacity = kPresetCapacity,
                          .perItemMax = kPresetPayloadBytes,
                          .nameMax = kPresetNameMax});

    // ---- 0x0096 "pattern-presets-roster" — STATE, watch, on-change --------
    // {generation u16, count u8, capacity u8} — BARE, deliberately, same shape
    // as 0x000D paired-devices-roster. An embedded str16 name preview per slot
    // was the original plan (see PatternPresetStore.h's earlier revision) and
    // was cut for a real, measured reason, not a preference: Catalog32's
    // layout-field pool (channel/catalog.hpp, capacity 200) had only 11 free
    // slots left on this device before this channel existed (189/200 used),
    // and 3 header fields + 14 name fields needs 17. A client enumerates names
    // the same way it already does for the trust ledger: BLOB_REQ each slot
    // (kPayloadBytes is tiny — 40 B — so kPresetCapacity fetches is cheap) or
    // read the name back from a save/rename ECHO it sent itself. A generation
    // bump means "re-enumerate", exactly like 0x000D.
    c.addEntry({.id = ch::pattern_presets_roster, .name = "pattern-presets-roster",
                .cls = ChannelClass::STATE, .dir = Direction::h2c,
                .access = AccessLevel::watch, .maxRateHz = 0.0f,
                .defaultPriority = Priority::background,
                .hasCategory = true, .category = slopsync::setting_categories::user,
                .hasSettingChannel = true, .settingChannel = ch::pattern_presets_cmd});
    c.addLayoutField({.name = "generation", .type = PackedFieldType::u16, .unit = "count", .scale = 1.0f});
    c.addLayoutField({.name = "count",      .type = PackedFieldType::u8,  .unit = "count", .scale = 1.0f});
    c.addLayoutField({.name = "capacity",   .type = PackedFieldType::u8,  .unit = "count", .scale = 1.0f});

    // ---- 0x0100 "move" — INTENT, control, 20 Hz, critical ----------------
    // {1:"position" f32 mm, 2:"bypass" bool}. This channel maps to arbiter
    // source 0 (MANUAL) in the delegate.
    //
    // NO `action.*` ROLE HERE, DELIBERATELY. RFC-019's action roles mark a
    // schema field as a VERB ("do this") rather than a value; `position` is a
    // value (where to go), not a verb, and tagging it action.move would tell a
    // generic client to render a button where a slider belongs. This channel
    // is exactly what generic value-field rendering already handles.
    c.addEntry({.id = ch::move, .name = "move",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 20.0f,
                .defaultPriority = Priority::critical});
    c.addSchemaField({.key = 1, .name = "position", .type = CborFieldType::f32_t, .unit = "mm",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 2000.0f});
    c.addSchemaField({.key = 2, .name = "bypass", .type = CborFieldType::bool_t, .unit = ""});

    // ---- 0x0101 "config-set" — INTENT, control, 10 Hz --------------------
    // Every field optional; present keys are applied. cfg_gen bumps on success.
    // fw 2.1.47 APPENDED key 7 "input_jerk" — append-only (keys 1-6 keep their
    // meaning exactly), and released key numbers are never reused. :3
    c.addEntry({.id = ch::config_set, .name = "config-set",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 10.0f,
                .defaultPriority = Priority::normal});
    //
    // M5a: each key now advertises the SAME bounds its 0x0081 twin does, so a
    // client that validates before sending gets the same answer the hub would
    // NACK with. The user-facing text (desc/group/default/role) lives ONCE, on
    // the STATE side — RFC-009 renders settings from the snapshot and resolves
    // the write key through `settingChannel`, so duplicating 128-byte descs
    // here would double the flash cost of every tooltip for no new meaning.
    c.addSchemaField({.key = 1, .name = "window_min",  .type = CborFieldType::f32_t, .unit = "mm",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = ceiling::rail_mm});
    c.addSchemaField({.key = 2, .name = "window_max",  .type = CborFieldType::f32_t, .unit = "mm",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = ceiling::rail_mm});
    c.addSchemaField({.key = 3, .name = "user_speed",  .type = CborFieldType::f32_t, .unit = "mm/s",
                      .hasMin = true, .hasMax = true, .min = ceiling::speed_min, .max = ceiling::speed_max});
    c.addSchemaField({.key = 4, .name = "user_accel",  .type = CborFieldType::f32_t, .unit = "mm/s2",
                      .hasMin = true, .hasMax = true, .min = ceiling::accel_min, .max = ceiling::accel_max});
    c.addSchemaField({.key = 5, .name = "input_speed", .type = CborFieldType::f32_t, .unit = "mm/s",
                      .hasMin = true, .hasMax = true, .min = ceiling::speed_min, .max = ceiling::speed_max});
    c.addSchemaField({.key = 6, .name = "input_accel", .type = CborFieldType::f32_t, .unit = "mm/s2",
                      .hasMin = true, .hasMax = true, .min = ceiling::accel_min, .max = ceiling::accel_max});
    c.addSchemaField({.key = 7, .name = "input_jerk",  .type = CborFieldType::f32_t, .unit = "mm/s3",
                      .hasMin = true, .hasMax = true, .min = ceiling::jerk_min, .max = ceiling::jerk_max});

    // ---- 0x0102 "pattern-cmd" — INTENT, control, 20 Hz -------------------
    // Session-volatile (cfg_gen does NOT bump). Maps to arbiter source 2
    // (PATTERN) via the delegate; running drives start/stop.
    c.addEntry({.id = ch::pattern_cmd, .name = "pattern-cmd",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 20.0f,
                .defaultPriority = Priority::normal});
    // Bounds mirror the 0x0082 twin (see the config-set note above for why the
    // prose lives only on the STATE side).
    c.addSchemaField({.key = 1, .name = "running",   .type = CborFieldType::bool_t, .unit = ""});
    c.addSchemaField({.key = 2, .name = "pattern",   .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 6.0f});
    c.addSchemaField({.key = 3, .name = "speed",     .type = CborFieldType::f32_t,  .unit = "%",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f});
    c.addSchemaField({.key = 4, .name = "depth",     .type = CborFieldType::f32_t,  .unit = "%",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f});
    c.addSchemaField({.key = 5, .name = "stroke",    .type = CborFieldType::f32_t,  .unit = "%",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f});
    c.addSchemaField({.key = 6, .name = "sensation", .type = CborFieldType::f32_t,  .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f});

    // ---- 0x0103 "home" — INTENT, control --------------------------------
    // {1:"op", 2:"stroke"} — op 1 starts sensorless homing; ops 2/3 are the
    // BENCH ops (RFC-025, safety-reviewed) that make motorless dev work
    // possible at all: they are the in-band twin of the legacy
    // POST /api/machine/homeoverride that every bench session already used.
    //
    // *** OP 2 (force_home) CLEARS AN E-STOP LATCH. *** That is exactly why
    // RFC-025 placed these under safety review rather than in a convenience
    // bucket, and why they are `control` and rate-capped like every other op
    // here. Op 2 declares the machine homed WITHOUT a homing cycle, so the
    // stroke window it hands the arbiter is an ASSERTION, not a measurement —
    // on a machine with a motor attached that is a real collision hazard, and
    // the call site in SlopSyncHubService says so again.
    //
    // Op values are DEVICE-defined: 0x0103 is in this device's own >=0x0100
    // allocation, so unlike 0x0005's registry-governed `safety_ops` these
    // numbers live in this catalog and nowhere else — which is precisely why
    // they carry option labels, so a generic client can name them.
    c.addEntry({.id = ch::home, .name = "home",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 5.0f,
                .defaultPriority = Priority::normal});
    c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = "",
                            .role = "action.home"},
                           {"reserved", "home", "force_home", "clear_override"},
                           {AccessLevel::control,   // 0 (placeholder, never an op)
                            AccessLevel::control,   // 1 home
                            AccessLevel::control,   // 2 force_home  — CLEARS THE E-STOP LATCH
                            AccessLevel::control}); // 3 clear_override
    c.addSchemaField({.key = 2, .name = "stroke", .type = CborFieldType::f32_t, .unit = "mm",
                      .hasMin = true, .hasMax = true, .min = 1.0f, .max = 2000.0f});

    // ---- 0x0104 "modes-set" — INTENT, control, 5 Hz ----------------------
    // M5b: the write half of 0x008A. Every key optional; present keys applied,
    // and the ECHO carries the POST-CLAMP value the handler actually took.
    //
    // NOT cfg_gen-bumping and NOT persisted here — each op routes to the same
    // WebUI::handleCommand path the legacy plane used, which owns whatever
    // persistence each mode has. Routing them anywhere else would give
    // SlopSync a second, divergent idea of what "blend mode" means.
    //
    // 5 Hz because these are human dropdown changes, not a control loop. The
    // bounds are the enum ranges the catalog's own option arrays declare, so a
    // client that validates locally gets the same answer the hub would NACK.
    c.addEntry({.id = ch::modes_set, .name = "modes-set",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 5.0f,
                .defaultPriority = Priority::normal});
    c.addSchemaField({.key = 1, .name = "blend_mode", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 1.0f, .max = 3.0f});
    // KEY 2 IS DELIBERATELY UNUSED. It briefly held "transport" (the WS/SER/
    // BT/DONGLE/OSSM input-source selector) before that setting was retired:
    // SlopSync is now the only way in, the hub listens on WebSocket and BLE by
    // default, and OSSM-BLE is gone. The C5 dongle may return one day, but as a
    // transport the hub simply HAS, not a mode an operator picks.
    //
    // The number is skipped rather than recycled. This channel never left the
    // branch so reuse would technically be safe, but "released keys are never
    // reused" is only a reliable habit if it does not get relitigated per case,
    // and a gap costs nothing.
    c.addSchemaField({.key = 3, .name = "stream_speed_mode", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f});
    c.addSchemaField({.key = 4, .name = "overshoot_clamp", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f});

    // ---- 0x0105 "slopmotion-set" — INTENT, control, 5 Hz -------------------
    // The single writer behind all three slopmotion-* cards. Keys 1..20 are
    // allocated across those cards and never collide; every key optional, only
    // the keys PRESENT are applied, and each echoes the value the machine
    // actually took after its own clamp.
    //
    // Bounds mirror WebUI::applySlopMotion's clamps exactly, so a client that
    // validates locally gets the same answer the hub would NACK with. Times are
    // MILLISECONDS on the wire (the engine stores microseconds) — same
    // vocabulary /api/slopmotion used, so the two are diffable during the
    // transition.
    c.addEntry({.id = ch::sm_set, .name = "slopmotion-set",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 5.0f,
                .defaultPriority = Priority::normal});
    c.addSchemaField({.key = 1, .name = "jmax_ovr", .type = CborFieldType::f32_t, .unit = "1/s3",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 2000000.0f});
    c.addSchemaField({.key = 2, .name = "vmax_ovr", .type = CborFieldType::f32_t, .unit = "1/s",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 20.0f});
    c.addSchemaField({.key = 3, .name = "amax_ovr", .type = CborFieldType::f32_t, .unit = "1/s2",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 500.0f});
    c.addSchemaField({.key = 4, .name = "centring", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f});
    c.addSchemaField({.key = 5, .name = "centring_gain", .type = CborFieldType::f32_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f});
    c.addSchemaField({.key = 6, .name = "chase_ff", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f});
    c.addSchemaField({.key = 7, .name = "chase_accel_ff", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f});
    c.addSchemaField({.key = 8, .name = "chase_gain", .type = CborFieldType::f32_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.5f});
    c.addSchemaField({.key = 9, .name = "chase_lookahead", .type = CborFieldType::f32_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 8.0f});
    c.addSchemaField({.key = 10, .name = "chase_dense_ms", .type = CborFieldType::f32_t, .unit = "ms",
                      .hasMin = true, .hasMax = true, .min = 10.0f, .max = 500.0f});
    c.addSchemaField({.key = 11, .name = "chase_aim_extrap", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f});
    c.addSchemaField({.key = 12, .name = "handoff_k", .type = CborFieldType::f32_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 8.0f});
    c.addSchemaField({.key = 13, .name = "curve_policy", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 2.0f});
    c.addSchemaField({.key = 14, .name = "infeasible_policy", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 4.0f});
    c.addSchemaField({.key = 15, .name = "infeasible_margin", .type = CborFieldType::f32_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.5f, .max = 1.0f});
    c.addSchemaField({.key = 16, .name = "smooth_budget", .type = CborFieldType::f32_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f});
    c.addSchemaField({.key = 17, .name = "amplitude_budget", .type = CborFieldType::f32_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 1.0f});
    c.addSchemaField({.key = 18, .name = "blend_steps", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 1.0f, .max = 10.0f});
    c.addSchemaField({.key = 19, .name = "reshape_steps", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 8.0f});
    c.addSchemaField({.key = 20, .name = "settle_grace_ms", .type = CborFieldType::f32_t, .unit = "ms",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 200.0f});

    // ---- 0x0106 "machine-admin" — INTENT, control -------------------------
    // The device ACTIONS that are not settings and not motion: clear a driver
    // fault, persist config, kick off a servo register scan. They were HTTP
    // writers (/api/clearfault, WS_OP_SAVE, POST /api/servo {"scan":true});
    // "no controls outside SlopSync" retires all three.
    //
    // An op SELECT rather than one channel per verb, exactly like 0x0103 home:
    // these are rare, human-initiated, and share a shape. 2 Hz because a human
    // presses them; a client that needs to press one faster than twice a second
    // is doing something the machine should not help with.
    c.addEntry({.id = ch::machine_admin, .name = "machine-admin",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 2.0f,
                .defaultPriority = Priority::normal});
    c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = "",
                            .role = "action.admin"},
                           {"reserved", "clear_fault", "save_config", "servo_scan"},
                           {AccessLevel::control,   // 0 placeholder, never an op
                            AccessLevel::control,   // 1 clear_fault
                            AccessLevel::control,   // 2 save_config
                            AccessLevel::control}); // 3 servo_scan

    // ---- 0x0107 "pattern-advanced-cmd" — INTENT, control, 20 Hz -----------
    // The single writer behind ALL SEVEN 0x008E..0x0094 advanced-pattern
    // cards. Same lean-schema convention as every other settings writer in
    // this catalog (config_set, pattern_cmd, modes_set, sm_set): the
    // user-facing text (desc/group/default/role) lives ONCE, on the STATE
    // side, so this channel carries only what a client needs to validate
    // before sending — name, type, unit, bounds.
    //
    // Keys 1..8 mirror 0x008E's layout exactly. Keys 9..44 are 6-per-control
    // blocks, base = 9 + 6*id with id in advpat::BaseId order (DEPTH_MAX=0
    // .. ACCEL_OUT=5), matching 0x008F..0x0094 exactly — see
    // SlopSyncHubService's applyIntent(0x0107) for the same arithmetic run
    // in reverse to decode a wire frame back into a control + sub-field.
    //
    // Session-volatile, same as 0x0102 pattern-cmd: cfg_gen does not bump.
    c.addEntry({.id = ch::pattern_advanced_cmd, .name = "pattern-advanced-cmd",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 20.0f,
                .defaultPriority = Priority::normal});
    c.addSchemaField({.key = 1, .name = "ap_mode",   .type = CborFieldType::bool_t, .unit = ""});
    c.addSchemaField({.key = 2, .name = "master",    .type = CborFieldType::uint_t, .unit = "%",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f});
    c.addSchemaField({.key = 3, .name = "max_depth", .type = CborFieldType::uint_t, .unit = "%",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f});
    c.addSchemaField({.key = 4, .name = "min_depth", .type = CborFieldType::uint_t, .unit = "%",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f});
    c.addSchemaField({.key = 5, .name = "in_speed",  .type = CborFieldType::uint_t, .unit = "%",
                      .hasMin = true, .hasMax = true, .min = 1.0f, .max = 100.0f});
    c.addSchemaField({.key = 6, .name = "out_speed", .type = CborFieldType::uint_t, .unit = "%",
                      .hasMin = true, .hasMax = true, .min = 1.0f, .max = 100.0f});
    c.addSchemaField({.key = 7, .name = "in_accel",  .type = CborFieldType::uint_t, .unit = "%",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f});
    c.addSchemaField({.key = 8, .name = "out_accel", .type = CborFieldType::uint_t, .unit = "%",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f});
    // 6 keys per base control (advpat::BaseId order): amplitude, in_step,
    // in_wait, out_step, out_wait, offset — base = 9 + 6*id. Authoring order
    // doesn't need to be ascending here (the encoder sorts schema fields by
    // key before emitting), so a loop is safe where it would not be for the
    // addEntry() ordering above.
    for (uint8_t id = 0; id < kApBaseCount; ++id) {
        const uint8_t base = uint8_t(9 + 6 * id);
        c.addSchemaField({.key = uint8_t(base + 0), .name = "amplitude", .type = CborFieldType::uint_t,
                          .unit = "%", .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f});
        c.addSchemaField({.key = uint8_t(base + 1), .name = "in_step",   .type = CborFieldType::uint_t,
                          .unit = "", .hasMin = true, .hasMax = true, .min = 1.0f, .max = 25.0f});
        c.addSchemaField({.key = uint8_t(base + 2), .name = "in_wait",   .type = CborFieldType::uint_t,
                          .unit = "", .hasMin = true, .hasMax = true, .min = 0.0f, .max = 25.0f});
        c.addSchemaField({.key = uint8_t(base + 3), .name = "out_step",  .type = CborFieldType::uint_t,
                          .unit = "", .hasMin = true, .hasMax = true, .min = 1.0f, .max = 25.0f});
        c.addSchemaField({.key = uint8_t(base + 4), .name = "out_wait",  .type = CborFieldType::uint_t,
                          .unit = "", .hasMin = true, .hasMax = true, .min = 0.0f, .max = 25.0f});
        c.addSchemaField({.key = uint8_t(base + 5), .name = "offset",    .type = CborFieldType::uint_t,
                          .unit = "", .hasMin = true, .hasMax = true, .min = 0.0f, .max = 100.0f});
    }

    // ---- 0x0108 "pattern-presets-cmd" — INTENT, control -------------------
    // The CRUD writer behind the 0x0095 store / 0x0096 roster pair (RFC-021).
    // {1:"op", 2:"slot", 3:"name"}. `op` is RFC-019's OPEN `action.<name>`
    // convention (no registry change needed, same as 0x0005's action.safety):
    // index 0 is the mandatory non-empty placeholder, never a real op.
    //
    // `slot` addresses directly — the client picks it (normally the roster's
    // first free entry), there is no name-keyed dedup the way the retired
    // HTTP handler had. `name` is required for save/rename, ignored for
    // load/delete. See SlopDriveHubDelegate::applyIntent's 0x0108 case for
    // exactly what each op does and PatternPresetStore.h for the backend.
    c.addEntry({.id = ch::pattern_presets_cmd, .name = "pattern-presets-cmd",
                .cls = ChannelClass::INTENT, .dir = Direction::c2h,
                .access = AccessLevel::control, .maxRateHz = 5.0f,
                .defaultPriority = Priority::normal});
    c.addSelectSchemaField({.key = 1, .name = "op", .type = CborFieldType::uint_t, .unit = "",
                            .role = "action.preset"},
                           {"reserved", "save", "load", "delete", "rename"});
    c.addSchemaField({.key = 2, .name = "slot", .type = CborFieldType::uint_t, .unit = "",
                      .hasMin = true, .hasMax = true, .min = 0.0f, .max = float(kPresetCapacity - 1)});
    c.addSchemaField({.key = 3, .name = "name", .type = CborFieldType::tstr_t, .unit = ""});

    return c.ok();
}

}  // namespace slopdrive
