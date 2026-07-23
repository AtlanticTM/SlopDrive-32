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

}  // namespace slopsync
