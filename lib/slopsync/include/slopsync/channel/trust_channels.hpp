// slopsync-core — the spec-core TRUST ADMINISTRATION surface: session-admin
// (0x0009), pending-pairing (0x000A), pairing-events (0x000B), and the
// paired-devices store pair (0x000C STORE + 0x000D roster STATE).
//
// THE TRUSTED SURFACE IS A TIER, NOT AN APP. Nothing here knows what a WebUI
// is, and nothing anywhere may special-case one. Every op on 0x0009 is reachable
// by ANY `configure` session — phone, CLI, another machine, the served page —
// which is the entire point of RFC-027 striking §12.2's "admin granted only via
// the hub's own UI" sentence. If you ever find yourself writing a check that
// asks WHO a session is rather than WHAT TIER it holds, that is the bug.
//
// ONE ADMIN CHANNEL, NOT THREE. Eviction, pairing approval and revocation are
// all "an authorized operator changing who may do what", so they share one
// access floor, one §9.3 rate limiter, one idempotency ring and one place a
// generic RFC-009 renderer looks. Splitting them would have burned two more
// spec-core ids to re-implement the same dispatch and would have let a hub end
// up authorizing "approve a pairing" differently from "evict a session" — one
// decision wearing two hats.
//
// WHY THE LEDGER IS A BLOB STORE AND NOT A PACKED ROSTER (feasibility pass,
// binding): a ledger entry is {instance_id, kind, name, version, first_seen,
// last_seen, role, state} plus its posture fields — about 96 encoded bytes. A
// 242 B packed STATE snapshot therefore holds two of them. Two. So the ledger
// reuses RFC-021's store machinery verbatim (chunked transfer, selective
// repair, total_bytes pre-sizing, generation) and its dynamic side is the tiny
// {generation, count, capacity} STATE the same shape every store uses.
#pragma once

#include <cstdint>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/generated/registry_constants.hpp"

namespace slopsync {

// ---------------------------------------------------------------------------
// 0x0009 session-admin — the INTENT `value` map keys.
// ---------------------------------------------------------------------------
namespace admin_value {
inline constexpr uint8_t op = 1;           // a `session_admin_ops` value
inline constexpr uint8_t session_id = 2;   // uint u32 — `evict`
inline constexpr uint8_t instance_id = 3;  // bstr 8  — pair_approve / pair_deny / revoke
inline constexpr uint8_t role = 4;         // uint    — pair_approve's granted `access_levels`
}  // namespace admin_value

// ---------------------------------------------------------------------------
// 0x000B pairing-events — the EVENT `body` (40) sub-map keys.
//
// NOTE WHAT IS ABSENT: any token, ever. These events are `watch`-visible (see
// addPairingEventsChannel below for why) and a token is the one field that
// would make that a mistake.
// ---------------------------------------------------------------------------
namespace pairing_body {
inline constexpr uint8_t instance_id = 1;  // bstr 8 — who
inline constexpr uint8_t name = 2;         // tstr   — their client_name, for the approval prompt
inline constexpr uint8_t mode = 3;         // uint   — the `pairing_modes` BIT involved
inline constexpr uint8_t role = 4;         // uint   — granted/suspended `access_levels`
inline constexpr uint8_t version = 5;      // tstr   — recognized_pending: the NEWLY reported client_ver
}  // namespace pairing_body

// ---------------------------------------------------------------------------
// 0x000A pending-pairing — packed STATE.
//
// Header {generation u16, count u8, flags u8} then `pairing_pending_max` fixed
// slots. flags bit0 = a pairing association window is currently OPEN, which is
// how RFC-027(c)'s "window state must be observable in-band" holds for a
// session that connected BEFORE the window opened (WELCOME's trust.pairing_modes
// covers the ones that connect during it, and 0x000B covers the edge).
//
// THE u64 PROBLEM, and what was done about it. The registry describes each slot
// as {instance_id u64, kind u8, expires_s u16, name str16}, but `packed_field_
// types` has no u64 — the vocabulary stops at u32/i32/f32 because a packed
// STATE field is a scaled physical quantity and nothing physical needed 64
// bits. Rather than add a wire type the whole protocol would then have to carry
// for one field, the instance id is declared as TWO u32 fields holding its
// little-endian halves. The bytes on the wire are byte-identical to a u64 LE,
// the catalog describes them with types that exist, and a client reassembles
// them with one shift. Recorded rather than silently done, because "the
// registry says u64" and "the catalog says u32,u32" would otherwise look like a
// bug to the next reader.
// ---------------------------------------------------------------------------
namespace pending_pairing_flags {
inline constexpr uint8_t window_open = 1u << 0;
}  // namespace pending_pairing_flags

inline constexpr size_t kPendingSlots = size_t(limits::pairing_pending_max);
// {generation u16, count u8, flags u8} + slots {u32,u32,u8,u16,str16}
inline constexpr size_t kPendingSlotBytes = 4 + 4 + 1 + 2 + 16;
inline constexpr size_t kPendingPayloadBytes = 4 + kPendingSlots * kPendingSlotBytes;
static_assert(kPendingPayloadBytes <= limits::min_transport_payload,
              "0x000A must fit the 242 B floor every binding guarantees (§9.1)");

// {generation u16, count u8, capacity u8}
inline constexpr size_t kPairedRosterBytes = 4;

// ---------------------------------------------------------------------------
// Builders. Each is independent: a hub declares exactly the ones it implements,
// and a client discovers what exists by looking, which is the self-describing
// catalog doing its job. A hub with no trust ledger simply never declares
// 0x000C/0x000D and nothing else changes.
// ---------------------------------------------------------------------------

// 0x0009 session-admin (INTENT, `configure`).
//
// The op field carries index-aligned option labels so a generic client can name
// the verbs, and index-aligned `option_access` so it knows it may offer them.
// Every op sits at `configure` — including index 0, the placeholder that
// `session_admin_ops` starting at 1 leaves behind, kept at the STRICT side so
// wire value 0 is never the cheapest thing on this channel to reach.
inline bool addSessionAdminChannel(Catalog32& cat, float maxRateHz = 5.0f) {
    CatalogEntry* e = cat.addEntry({.id = channels::session_admin,
                                    .name = "session-admin",
                                    .cls = ChannelClass::INTENT,
                                    .dir = Direction::c2h,
                                    .access = AccessLevel::configure,
                                    .maxRateHz = maxRateHz,
                                    .defaultPriority = Priority::normal});
    if (e == nullptr) return false;
    cat.addSelectSchemaField({.key = admin_value::op, .name = "op", .type = CborFieldType::uint_t, .unit = ""},
                             {"reserved", "evict", "pair_approve", "pair_deny", "revoke"},
                             {AccessLevel::configure,   // 0 placeholder, never an op
                              AccessLevel::configure,   // 1 evict
                              AccessLevel::configure,   // 2 pair_approve
                              AccessLevel::configure,   // 3 pair_deny
                              AccessLevel::configure}); // 4 revoke
    cat.addSchemaField({.key = admin_value::session_id, .name = "session_id", .type = CborFieldType::uint_t,
                        .unit = ""});
    cat.addSchemaField({.key = admin_value::instance_id, .name = "instance_id", .type = CborFieldType::bstr_t,
                        .unit = ""});
    // `role` is what pair_approve grants. The operator ruling is that a
    // configure session may grant UP TO ITS OWN TIER, configure included — the
    // conventional admin behavior, with the paired-device roster as the audit
    // trail rather than a hard ceiling that would leave the first administrator
    // unable to appoint a second. min/max here describe the VALUE RANGE, not the
    // authorization; the hub still clamps to the requester's own tier.
    cat.addSchemaField({.key = admin_value::role, .name = "role", .type = CborFieldType::uint_t, .unit = "",
                        .hasMin = true, .hasMax = true,
                        .min = float(uint8_t(AccessLevel::watch)),
                        .max = float(uint8_t(AccessLevel::configure))});
    return !cat.overflow;
}

// 0x000A pending-pairing (STATE, `configure`).
//
// `configure` because this is the approval WORKLIST: it names who is asking and
// gives an operator the material to decide. The live-edge notifications that a
// viewer legitimately needs (a window opened; something is pending) ride 0x000B
// at `watch` instead.
inline bool addPendingPairingChannel(Catalog32& cat) {
    CatalogEntry* e = cat.addEntry({.id = channels::pending_pairing,
                                    .name = "pending-pairing",
                                    .cls = ChannelClass::STATE,
                                    .dir = Direction::h2c,
                                    .access = AccessLevel::configure,
                                    .maxRateHz = 0.0f,  // on-change
                                    .defaultPriority = Priority::normal});
    if (e == nullptr) return false;
    cat.addLayoutField({.name = "generation", .type = PackedFieldType::u16, .unit = "count", .scale = 1.0f});
    cat.addLayoutField({.name = "count", .type = PackedFieldType::u8, .unit = "count", .scale = 1.0f});
    cat.addBitfieldField({.name = "flags", .type = PackedFieldType::bitfield8, .unit = "flag", .scale = 1.0f},
                         {"window_open"});
    for (size_t i = 0; i < kPendingSlots; ++i) {
        // Names must be unique and <= 24 bytes; the slot index suffix is what
        // makes a flat layout addressable by a generic renderer.
        const char* lo[] = {"inst_lo0", "inst_lo1", "inst_lo2", "inst_lo3"};
        const char* hi[] = {"inst_hi0", "inst_hi1", "inst_hi2", "inst_hi3"};
        const char* kd[] = {"kind0", "kind1", "kind2", "kind3"};
        const char* ex[] = {"expires_s0", "expires_s1", "expires_s2", "expires_s3"};
        const char* nm[] = {"name0", "name1", "name2", "name3"};
        cat.addLayoutField({.name = lo[i], .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});
        cat.addLayoutField({.name = hi[i], .type = PackedFieldType::u32, .unit = "", .scale = 1.0f});
        cat.addLayoutField({.name = kd[i], .type = PackedFieldType::u8, .unit = "flag", .scale = 1.0f});
        cat.addLayoutField({.name = ex[i], .type = PackedFieldType::u16, .unit = "s", .scale = 1.0f});
        cat.addLayoutField({.name = nm[i], .type = PackedFieldType::str16, .unit = "", .scale = 1.0f});
    }
    return !cat.overflow;
}
static_assert(kPendingSlots == 4, "the slot-name tables above are sized for pairing_pending_max == 4");

// 0x000B pairing-events (EVENT, `watch`).
//
// `watch` IS THE DELIBERATE CHOICE, and it is what makes RFC-027(c)'s "window
// state must be observable in-band by any watch session" true on a hub with no
// LED. The material here is not secret: an instance_id and a client_name are on
// the wire in every HELLO on the same LAN, and no token or proof ever appears in
// a body. What a viewer gains is the ability to SEE that a pairing window is
// open and that something knocked — which is the property that makes an
// unnoticed pairing hard, i.e. the security posture improving, not degrading.
inline bool addPairingEventsChannel(Catalog32& cat) {
    CatalogEntry* e = cat.addEntry({.id = channels::pairing_events,
                                    .name = "pairing-events",
                                    .cls = ChannelClass::EVENT,
                                    .dir = Direction::h2c,
                                    .access = AccessLevel::watch,
                                    .maxRateHz = 0.0f,
                                    .defaultPriority = Priority::normal});
    if (e == nullptr) return false;
    cat.addSchemaField({.key = pairing_body::instance_id, .name = "instance_id", .type = CborFieldType::bstr_t,
                        .unit = ""});
    cat.addSchemaField({.key = pairing_body::name, .name = "name", .type = CborFieldType::tstr_t, .unit = ""});
    cat.addSchemaField({.key = pairing_body::mode, .name = "mode", .type = CborFieldType::uint_t, .unit = "flag"});
    cat.addSchemaField({.key = pairing_body::role, .name = "role", .type = CborFieldType::uint_t, .unit = ""});
    cat.addSchemaField({.key = pairing_body::version, .name = "version", .type = CborFieldType::tstr_t, .unit = ""});
    return !cat.overflow;
}

// 0x000C paired-devices (STORE, `configure`) + 0x000D its roster (STATE).
//
// They land together because a store without its roster is unusable: the roster
// is how a client learns there is anything to enumerate and how it notices its
// enumeration went stale mid-transfer. `storeId` is discovered by the hub from
// THIS descriptor rather than being a registry constant — the catalog is
// self-describing, so the number is agreed by being published, not by being
// legislated.
inline bool addPairedDevicesStore(Catalog32& cat, uint8_t storeId = 1) {
    CatalogEntry* e = cat.addEntry({.id = channels::paired_devices,
                                    .name = "paired-devices",
                                    .cls = ChannelClass::STORE,
                                    .dir = Direction::h2c,
                                    .access = AccessLevel::configure,
                                    .maxRateHz = 0.0f,
                                    .defaultPriority = Priority::background});
    if (e == nullptr) return false;
    cat.addStoreDescriptor({.storeId = storeId,
                            .kind = "trust.ledger",
                            .capacity = uint16_t(limits::paired_devices_max),
                            // The per-item budget that keeps the WHOLE ledger
                            // inside trust_ledger_max_bytes (1900 = one NVS
                            // page), which is the constraint the feasibility
                            // pass actually measured.
                            .perItemMax = uint32_t(limits::trust_ledger_max_bytes / limits::paired_devices_max),
                            .nameMax = uint16_t(limits::trust_ledger_name_max_bytes)});
    return !cat.overflow;
}

inline bool addPairedDevicesRoster(Catalog32& cat) {
    CatalogEntry* e = cat.addEntry({.id = channels::paired_devices_roster,
                                    .name = "paired-devices-roster",
                                    .cls = ChannelClass::STATE,
                                    .dir = Direction::h2c,
                                    .access = AccessLevel::configure,
                                    .maxRateHz = 0.0f,  // on-change, tiny
                                    .defaultPriority = Priority::background});
    if (e == nullptr) return false;
    cat.addLayoutField({.name = "generation", .type = PackedFieldType::u16, .unit = "count", .scale = 1.0f});
    cat.addLayoutField({.name = "count", .type = PackedFieldType::u8, .unit = "count", .scale = 1.0f});
    cat.addLayoutField({.name = "capacity", .type = PackedFieldType::u8, .unit = "count", .scale = 1.0f});
    return !cat.overflow;
}

// Convenience: the whole trust surface in catalog-id order. A hub that wants
// all of it calls this once; one that wants a subset calls the pieces.
inline bool addTrustChannels(Catalog32& cat, uint8_t storeId = 1) {
    if (!addSessionAdminChannel(cat)) return false;
    if (!addPendingPairingChannel(cat)) return false;
    if (!addPairingEventsChannel(cat)) return false;
    if (!addPairedDevicesStore(cat, storeId)) return false;
    if (!addPairedDevicesRoster(cat)) return false;
    return true;
}

}  // namespace slopsync
