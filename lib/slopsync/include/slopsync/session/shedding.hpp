// slopsync-core — the §10.4 shedding algorithm, expressed as one pure
// function. No I/O, no hub/session knowledge: shedDecision() only answers
// "given this subscription's priority and channel class, and the link's
// current congestion level, what happens to its next due push?" — wiring
// that decision into an actual pacing loop (decimation counters, ConflateHard
// stretching, Drop, slow-consumer eviction) is hub/hub_impl.hpp's job.
//
// The exact table implemented here (per the M5 milestone brief, which is
// more prescriptive than SPEC §10.4's prose — flagged as such in the M5
// report's deviations/clarifications):
//
//   level 0 (clear):      everything -> Send.
//   critical priority:    always -> Send, at every level (§10.1 never-shed set).
//   level 1 (congested):
//     background + STREAM -> Decimate4x
//     background + STATE  -> ConflateHard (stretch periodic pushes toward on-change-only)
//     normal     + STREAM -> Decimate2x
//     elevated             -> Send (untouched)
//     anything else (background/normal on INTENT/EVENT, normal+STATE) -> Send
//       (INTENT is client->hub and never paced here; EVENT already has its
//       own bounded drop-oldest queue independent of congestion level,
//       §9.4 — this function's decimation vocabulary just doesn't apply to
//       either class, so "no additional shed" is the correct answer, not an
//       omission)
//   level 2 (stalled — never-shed queue itself can't drain):
//     background -> Drop (regardless of class)
//     normal     -> Decimate4x (regardless of class)
//     elevated   -> Decimate2x (regardless of class)
//     critical   -> Send (never-shed, see above)
#pragma once

#include <cstdint>

#include "slopsync/generated/registry_constants.hpp"

namespace slopsync {

enum class ShedDecision : uint8_t { Send, Decimate2x, Decimate4x, ConflateHard, Drop };

inline ShedDecision shedDecision(Priority priority, ChannelClass cls, uint8_t congestionLevel) {
    if (congestionLevel == 0) return ShedDecision::Send;
    if (priority == Priority::critical) return ShedDecision::Send;  // never-shed set, §10.1 — every level

    if (congestionLevel == 1) {
        switch (priority) {
            case Priority::background:
                if (cls == ChannelClass::STREAM) return ShedDecision::Decimate4x;
                if (cls == ChannelClass::STATE) return ShedDecision::ConflateHard;
                return ShedDecision::Send;
            case Priority::normal:
                if (cls == ChannelClass::STREAM) return ShedDecision::Decimate2x;
                return ShedDecision::Send;
            case Priority::elevated:
            case Priority::critical:
            default:
                return ShedDecision::Send;
        }
    }

    // congestionLevel >= 2: the never-shed queue itself can't drain (§10.4
    // step 4's regime) — priority alone decides, uniformly across classes.
    switch (priority) {
        case Priority::background: return ShedDecision::Drop;
        case Priority::normal:     return ShedDecision::Decimate4x;
        case Priority::elevated:   return ShedDecision::Decimate2x;
        case Priority::critical:
        default:                  return ShedDecision::Send;
    }
}

}  // namespace slopsync
