// slopsync-core — the STATE shadow-apply function, SPEC §9.1/§7.3.
//
// STATE frames are idempotent full snapshots (§9.1): every frame carries the
// complete current value, so a shadow only ever needs to remember the LAST
// one it accepted. "Accepted" is decided purely by sequence number
// (§7.3's newest-wins rule), via util/serial_arithmetic.hpp's seqIsNewer —
// this file adds no inline seq comparison of its own.
//
// applyStateFrame() is deliberately dumb: it does not know or care what a
// channel's payload MEANS (that's wire/packed/layout_codec.hpp's job, or a
// compile-time struct's, downstream of the shadow). It only decides whether
// an incoming (seq, payload) pair should replace what's currently held —
// exactly the "conflation" behavior SPEC §9.1/§7.3 describe: an older or
// equal-seq frame is silently dropped, gaps are meaningless, and a frame
// that would overflow the fixed shadow buffer is rejected rather than
// truncated (silently accepting a truncated STATE snapshot would violate
// the "every STATE frame contains the complete current value" rule — better
// to keep the old, complete value than adopt a corrupt partial one).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/packed/layout_codec.hpp"

namespace slopsync {

// A subscriber's shadow of one STATE channel: the raw payload bytes as
// received (undecoded — decoding into physical values is a separate step,
// e.g. via decodeByLayout/appendOnlyRead below), plus the bookkeeping
// needed to apply newest-wins.
struct ShadowSlot {
    bool valid = false;
    uint16_t seq = 0;
    std::array<std::byte, limits::min_transport_payload> value{};
    uint16_t size = 0;
};

// Applies an incoming STATE frame (frameSeq, payload) to `slot`.
//
// Accepts (copies payload into slot.value, updates slot.seq/size, sets
// slot.valid, returns true) iff `!slot.valid` (first frame ever seen for
// this channel) or `seqIsNewer(frameSeq, slot.seq)` (§7.3). Otherwise the
// frame is silently dropped and this returns false — for an older/equal seq
// that is the NORMAL, expected outcome of conflation on a lossy/reordering
// transport, not an error.
//
// A payload longer than the fixed shadow buffer (limits::min_transport_payload,
// the same 242-byte MTU-fit floor §9.1 requires of every STATE layout at
// catalog-design time) is a distinct, genuinely exceptional rejection: it
// can only mean a non-conformant sender or transport corruption, since a
// conformant catalog's STATE layouts never produce a payload this large.
// `capacityExceeded`, if non-null, is set true in that case (and left
// untouched otherwise) so a caller that cares can distinguish "dropped as
// stale" from "rejected as oversized" without a second return channel.
inline bool applyStateFrame(uint16_t frameSeq, std::span<const std::byte> payload, ShadowSlot& slot,
                             bool* capacityExceeded = nullptr) {
    if (payload.size() > slot.value.size()) {
        if (capacityExceeded) *capacityExceeded = true;
        return false;
    }
    if (slot.valid && !seqIsNewer(frameSeq, slot.seq)) {
        return false;  // stale or duplicate seq (§7.3): silently dropped
    }

    std::memcpy(slot.value.data(), payload.data(), payload.size());
    slot.size = uint16_t(payload.size());
    slot.seq = frameSeq;
    slot.valid = true;
    return true;
}

// SPEC §5.4 append-only evolution rule, named for the STATE-shadow context:
// a client compiled against an OLDER/SHORTER `knownLayout` can always parse
// its known prefix out of a payload a newer hub has grown by APPENDING
// fields to the tail — the payload being longer than `knownLayout` describes
// is exactly what catalog evolution looks like, and is legal by
// construction (§4.4: released fields never move, resize, or get removed).
//
// This is not a new algorithm: decodeByLayout() already reads only
// knownLayout.fieldCount fields and never looks past them, so calling it
// with a prefix `knownLayout` against a longer `payload` (e.g. straight out
// of a ShadowSlot::value/size pair) IS the rule. This wrapper exists purely
// so call sites reaching for "read what I know out of whatever the hub
// actually sent" say so by name, rather than re-deriving the reasoning
// inline at every call site.
inline Result<size_t, DecodeError> appendOnlyRead(const CatalogEntry& knownLayout,
                                                   std::span<const std::byte> payload,
                                                   std::span<float> outPhysical) {
    return decodeByLayout(knownLayout, payload, outPhysical);
}

}  // namespace slopsync
