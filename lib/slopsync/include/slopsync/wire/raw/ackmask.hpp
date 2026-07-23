// slopsync-core — ACKMASK, SPEC §13.3.
//
// Raw payload, 6 bytes: `[base_seq:u16][mask:u32]`. Emitted every 10 ms by
// an ESP-NOW receiver, acking header-seq values `base_seq .. base_seq+31`
// (bit i of `mask` <=> seq `base_seq+i` was received). Senders read the mask
// as their §10.3 congestion signal (loss rate) and, for control-plane
// frames only, as the key for stop-and-wait retransmission — STATE/STREAM
// are never retransmitted (§13.3: "the classes don't need it").
//
// AckTracker is the receiver-side helper that turns a stream of arriving
// seqs into the next ACKMASK snapshot to send.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/util/byte_io.hpp"
#include "slopsync/util/serial_arithmetic.hpp"

namespace slopsync {

struct AckMask {
    uint16_t base_seq = 0;
    uint32_t mask = 0;
};

inline constexpr size_t kAckMaskBytes = 6;
inline constexpr uint32_t kAckMaskWindow = 32;

inline size_t encodeAckMask(const AckMask& m, std::span<std::byte> out) {
    if (out.size() < kAckMaskBytes) return 0;
    putU16(out.subspan(0, 2), m.base_seq);
    putU32(out.subspan(2, 4), m.mask);
    return kAckMaskBytes;
}

inline Result<AckMask, DecodeError> decodeAckMask(std::span<const std::byte> in) {
    using Ret = Result<AckMask, DecodeError>;
    if (in.size() < kAckMaskBytes) return Ret::err(DecodeError::Truncated);
    AckMask m;
    m.base_seq = getU16(in.subspan(0, 2));
    m.mask = getU32(in.subspan(2, 4));
    return Ret::ok(m);
}

// Invokes `callback(seq)` for every bit set in `mask`, seq = base+i for each
// set bit i in [0,32) (u16 wrap is intentional and harmless: the window is
// only 32 wide, far short of a full seq cycle). Pure/free-standing per the
// brief — no AckTracker state needed to interpret an already-received mask.
template <typename Fn>
inline void unpackAckMask(uint16_t base, uint32_t mask, Fn&& callback) {
    for (uint32_t i = 0; i < kAckMaskWindow; ++i) {
        if (mask & (uint32_t{1} << i)) callback(uint16_t(base + i));
    }
}

// Receiver-side accumulator: feed it every seq that arrives, read back the
// ACKMASK to send next.
class AckTracker {
public:
    explicit AckTracker(uint16_t base = 0) : _base(base) {}

    // Marks `seq` received.
    //
    // - Older-or-equal than `_base` (per seqIsNewer — the ONLY "is this
    //   older/newer" decision this class makes, and it goes through the
    //   library's one sanctioned comparison primitive): a stale/duplicate
    //   ack, dropped. The window only ever moves forward (§13.3).
    // - Within [_base, _base+31]: sets the corresponding bit, no shift.
    // - At or beyond `_base + 32`: the window SLIDES forward so `seq`
    //   becomes its newest (top) bit, discarding ack state for anything
    //   that fell off the trailing edge. This is required — otherwise
    //   `_base` would never advance on a live, ever-incrementing stream,
    //   which is exactly the failure mode a fixed non-sliding window would
    //   have. Mirrors the proven ESP-NOW ancestor's bitmask behavior.
    void record(uint16_t seq) {
        if (seq != _base && !seqIsNewer(seq, _base)) return;  // stale: drop

        // Bit-POSITION arithmetic, not a newer/older decision (that check
        // already happened above via seqIsNewer) — this is the u16 analog
        // of serial_arithmetic.hpp's timeDelta, sized for this 32-wide
        // window. Kept local rather than added to serial_arithmetic.hpp
        // because it answers "how far", not "which is newer".
        int32_t offset = int32_t(int16_t(seq - _base));

        if (offset >= int32_t(kAckMaskWindow)) {
            const uint16_t shift = uint16_t(offset - int32_t(kAckMaskWindow) + 1);
            _base = uint16_t(_base + shift);
            _mask = (shift >= kAckMaskWindow) ? 0u : (_mask >> shift);
            offset = int32_t(kAckMaskWindow) - 1;
        }
        _mask |= (uint32_t{1} << offset);
    }

    AckMask snapshot() const { return AckMask{_base, _mask}; }
    void reset(uint16_t base) {
        _base = base;
        _mask = 0;
    }

    uint16_t base() const { return _base; }
    uint32_t mask() const { return _mask; }

private:
    uint16_t _base;
    uint32_t _mask = 0;
};

}  // namespace slopsync
