// slopsync-core — serial (wrap-around) comparison, SPEC §7.2–7.3.
// THE ONLY place in the library where sequence numbers or wrapping timestamps
// are compared. No inline `a > b` on a seq or hub-time exists anywhere else —
// that rule is how wraparound bugs are made structurally impossible.
#pragma once

#include <cstdint>

namespace slopsync {

// u16 sequence numbers (§7.3): `a` is newer than `b` iff
//   0 < (a - b) mod 2^16 < 2^15.
// Equal is NOT newer. Wrap (0xFFFF -> 0x0000) compares correctly.
inline bool seqIsNewer(uint16_t a, uint16_t b) {
    uint16_t d = uint16_t(a - b);
    return d != 0 && d < 0x8000u;
}

// Wrapping u32 timestamps (§7.2): interpret `t` in the ± half-range window
// around `reference` (µs: ±35.8 min; ms: ±24.8 days). Returns the signed
// delta t - reference within that window.
inline int32_t timeDelta(uint32_t t, uint32_t reference) {
    return int32_t(t - reference);  // two's-complement wrap does the windowing
}

// Convenience: `t` is at-or-after `reference` within the wrap window.
inline bool timeReached(uint32_t now, uint32_t deadline) {
    return timeDelta(now, deadline) >= 0;
}

// Millisecond bookkeeping over the wrapping µs clock (§7.2). `nowUs / 1000`
// does NOT wrap mod 2^32 — it jumps 4294967 -> 0 every ~71.6 min — which
// would strand every ms deadline ~71 min in the future at each wrap (STATE
// pushes stall, deadman windows go dormant: timeDelta needs a counter that
// wraps mod 2^32, and the quotient is not one). µs DELTAS are mod-2^32
// exact, so this accumulates them into a true mod-2^32 ms counter that
// timeReached() compares safely. Bit-identical to `nowUs / 1000` for any
// non-wrapping run (the first advance() seeds quotient + remainder).
// Precondition: advance() is called at least once per half wrap (~35 min) —
// the same window §7.2 already imposes on all timestamp interpretation.
class MonotonicMs {
public:
    uint32_t advance(uint32_t nowUs) {
        if (!_init) {
            _init = true;
            _lastUs = nowUs;
            _ms = nowUs / 1000u;
            _rem = uint16_t(nowUs % 1000u);
            return _ms;
        }
        uint32_t total = uint32_t(nowUs - _lastUs) + _rem;
        _lastUs = nowUs;
        _ms += total / 1000u;
        _rem = uint16_t(total % 1000u);
        return _ms;
    }

private:
    uint32_t _lastUs = 0;
    uint32_t _ms = 0;
    uint16_t _rem = 0;
    bool _init = false;
};

}  // namespace slopsync
