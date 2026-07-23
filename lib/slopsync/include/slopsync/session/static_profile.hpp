// slopsync-core — the static-client (etag-pinned) profile decision, SPEC
// §8.5. A constrained client (C5 remote, minimal BLE device) MAY ship with a
// compiled-in catalog and pre-encoded CBOR templates instead of a CBOR
// stack; §8.5 requires it to compare its own etag against the hub's WELCOME
// etag and pick one of two declared behaviors on mismatch. This file is that
// decision, factored out as a pure function so both the C5 firmware and the
// desktop sim can call the identical logic.
//
// Why degrading is safe at all (§5.4's append-only evolution rule): a
// released packed layout may only GROW at the tail, never reorder/resize/
// remove a field. So a static client compiled against catalog etag E, faced
// with a hub whose catalog moved on to etag E' purely by APPENDING new
// trailing fields, still parses every field in its own known PREFIX
// correctly out of the hub's (longer) STATE/STREAM payloads — the etag
// mismatch means "you don't know about everything", not "what you do know
// is wrong". That is exactly the gap §8.5 lets a client paper over for
// passive functions (rendering telemetry it recognizes) while still
// requiring it to refuse blind trust in anything it can't re-verify: an
// INTENT schema it cannot re-derive from a stale compiled-in catalog is not
// covered by append-only safety (a NEW required field on an INTENT schema,
// or a changed min/max, is exactly the kind of change a stale client cannot
// detect), so control functions MUST be suppressed in degraded mode.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace slopsync {

enum class StaticProfilePolicy : uint8_t {
    DegradeGracefully,  // mismatch -> proceed, but caller MUST suppress control functions
    RefuseLoudly,       // mismatch -> refuse outright (caller shows an "update me" indication)
};

struct StaticProfileDecision {
    bool proceed = false;            // false => do not use this session for anything
    bool controlSuppressed = false;  // true => caller MUST NOT send INTENTs built from its compiled-in schema
};

// Pure decision table (SPEC §8.5):
//   etag match                       -> {proceed=true,  controlSuppressed=false}  (full speed ahead)
//   mismatch + DegradeGracefully      -> {proceed=true,  controlSuppressed=true}   (append-only prefix still parses)
//   mismatch + RefuseLoudly           -> {proceed=false, controlSuppressed=false}  (caller shows "update me")
inline StaticProfileDecision decideStaticProfile(std::span<const std::byte> myEtag,
                                                  std::span<const std::byte> hubEtag,
                                                  StaticProfilePolicy policy) {
    const bool match = myEtag.size() == hubEtag.size() &&
                        std::equal(myEtag.begin(), myEtag.end(), hubEtag.begin());
    if (match) return StaticProfileDecision{true, false};
    if (policy == StaticProfilePolicy::DegradeGracefully) return StaticProfileDecision{true, true};
    return StaticProfileDecision{false, false};
}

// Convenience overload for the common fixed-size 8-byte etag
// (limits::etag_bytes) so callers holding two std::array<std::byte,8>
// don't have to spell out the span conversion themselves.
inline StaticProfileDecision decideStaticProfile(const std::array<std::byte, 8>& myEtag,
                                                  const std::array<std::byte, 8>& hubEtag,
                                                  StaticProfilePolicy policy) {
    return decideStaticProfile(std::span<const std::byte>(myEtag), std::span<const std::byte>(hubEtag), policy);
}

}  // namespace slopsync
