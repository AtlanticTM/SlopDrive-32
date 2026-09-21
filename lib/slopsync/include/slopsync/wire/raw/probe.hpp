// slopsync-core — network PROBE, SPEC §6.4.
//
// Two distinct payload shapes share the PROBE frame type, distinguished by
// direction, not by any on-wire discriminator:
//   - client -> hub (the probe REQUEST): raw, EMPTY payload (0 bytes) —
//     there is nothing to encode/decode; the frame itself is the request.
//   - hub -> client (the probe BURST, one frame per member of the timed
//     burst): raw payload `[probe_index:u16][padding...]` — probe_index
//     lets the client detect loss/reorder within the burst; the padding
//     exists only to consume bytes toward `probe_default_bytes` and its
//     content is never inspected (§4.3: unknown/don't-care bytes, ignored).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/util/byte_io.hpp"

namespace slopsync {

inline constexpr size_t kProbeRequestBytes = 0;  // documentation constant: the request IS empty
inline constexpr size_t kProbeIndexBytes = 2;

// Encodes one burst frame: probe_index at [0,2), the rest of `out` zeroed
// padding. Returns out.size() (the whole frame is "used", padding included)
// or 0 if out can't even hold the 2-byte index.
inline size_t encodeProbeFrame(uint16_t probeIndex, std::span<std::byte> out) {
    if (out.size() < kProbeIndexBytes) return 0;
    putU16(out.subspan(0, 2), probeIndex);
    for (size_t i = kProbeIndexBytes; i < out.size(); ++i) out[i] = std::byte{0};
    return out.size();
}

// Reads just probe_index; any padding bytes beyond it are ignored per §4.3
// (a longer/shorter padding tail than expected is not this decoder's
// business).
inline Result<uint16_t, DecodeError> decodeProbeFrame(std::span<const std::byte> in) {
    using Ret = Result<uint16_t, DecodeError>;
    if (in.size() < kProbeIndexBytes) return Ret::err(DecodeError::Truncated);
    return Ret::ok(getU16(in.subspan(0, 2)));
}

}  // namespace slopsync
