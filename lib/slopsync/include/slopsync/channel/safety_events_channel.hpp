// slopsync-core — the spec-core SAFETY-EVENTS channel (0x000E).
//
// WHY IT EXISTS. §9.4's event/state duality rule names e-stop as THE canonical
// pair — "EVENT for the edge, `safety` 0x0003 for the latch" — and §5.5/§11.2
// both require the hub to "emit the EVENT twin". Until M4b there was nowhere to
// emit it: no EVENT channel and no kind existed anywhere in the registry, so
// handleEstopFrame() had never emitted one and neither had the RFC-010 client
// e-stop op that M4a deliberately routed through it rather than inventing a
// number. This channel is that missing half.
//
// WHY ITS OWN CHANNEL, and not kinds bolted onto session-events (0x0007):
//   * 0x0007 is a SESSION-LIFECYCLE vocabulary (join / leave / takeover) at
//     `normal` priority. Safety edges are a different domain with a different
//     urgency, and merging them would force the whole channel to `critical` —
//     making every join notification never-shed, which is exactly backwards.
//   * A client must be able to subscribe to safety edges WITHOUT also taking
//     every session churn event, and vice versa. One channel cannot express
//     two subscription decisions.
//   * The duality rule reads better when it is literally true: 0x0003 is the
//     latch, 0x000E is its edge, same domain, same `critical` priority, same
//     `watch` access. Nobody may be denied a safety edge and nobody's safety
//     edge may be shed.
//
// EMITTED ON TRANSITIONS ONLY. A repeated ESTOP frame re-broadcasts the STATE —
// that is §11.2's only loss-recovery mechanism and it must keep working — but
// it does NOT re-emit the edge. An edge that did not happen is a lie, and a
// client counting initiations would count wrong.
//
// Every event carries `seq_of_state` (34) naming the 0x0003 frame it
// corresponds to, which is what lets a client that missed the edge reconcile
// against the latch it DID receive.
#pragma once

#include <cstdint>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/generated/registry_constants.hpp"

namespace slopsync {

// The 0x000E catalog SCHEMA keys = the `body` (40) sub-map keys of a
// safety EVENT. These are the CHANNEL'S OWN keys, not the global cbor_keys
// space — that is the v1.0 EVENT grammar.
namespace safety_body {
inline constexpr uint8_t word = 1;           // the safety word AFTER the edge (§11.1 bits)
inline constexpr uint8_t cause = 2;          // a `safety_causes` value
inline constexpr uint8_t owner_session = 3;  // session that caused it; 0 = the hub itself
inline constexpr uint8_t estop_seq = 4;      // §5.5 per-INITIATION sequence
inline constexpr uint8_t level = 5;          // stop_latched/stop_cleared: bitmask of bits that MOVED
}  // namespace safety_body

// Declares the spec-core safety-events channel on `cat`, in the canonical
// shape above. Call it in catalog-id order like any other entry (0x000E sorts
// after the other spec-core ids and before a device's own >=0x0080
// allocations).
//
// A hub that does not declare this entry simply emits no safety edges; the
// 0x0003 latch is unaffected and every §11.2 guarantee still holds, because the
// latch was always the load-bearing half. That is why this is a builder and not
// a hub requirement.
inline bool addSafetyEventsChannel(Catalog32& cat) {
    CatalogEntry* e = cat.addEntry({.id = channels::safety_events,
                                    .name = "safety-events",
                                    .cls = ChannelClass::EVENT,
                                    .dir = Direction::h2c,
                                    // Matches its STATE twin exactly: `watch`,
                                    // because the person in the room learning
                                    // the machine just latched needs no
                                    // authority to be told.
                                    .access = AccessLevel::watch,
                                    .maxRateHz = 0.0f,  // EVENT: edge-driven, no pacing
                                    .defaultPriority = Priority::critical});
    if (e == nullptr) return false;
    cat.addSchemaField({.key = safety_body::word, .name = "word", .type = CborFieldType::uint_t, .unit = "flag"});
    cat.addSchemaField({.key = safety_body::cause, .name = "cause", .type = CborFieldType::uint_t, .unit = ""});
    cat.addSchemaField({.key = safety_body::owner_session, .name = "owner_session",
                        .type = CborFieldType::uint_t, .unit = ""});
    cat.addSchemaField({.key = safety_body::estop_seq, .name = "estop_seq", .type = CborFieldType::uint_t,
                        .unit = "count"});
    cat.addSchemaField({.key = safety_body::level, .name = "level", .type = CborFieldType::uint_t, .unit = "flag"});
    return !cat.overflow;
}

}  // namespace slopsync
