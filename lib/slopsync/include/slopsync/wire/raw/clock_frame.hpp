// slopsync-core — CLOCK, SPEC §7.1.
//
// §7.1's prose describes CLOCK as "unchanged from the port-81 ancestor: 13
// bytes, `0x05` + t0:u32` (request) / `0x05` + t0 + t1 + t2` (reply)" — that
// 0x05 is the legacy protocol's own leading type byte, from an era with no
// separate frame header. In slopsync framing (§5.1) the type byte already
// lives in the 8-byte header (`type = FrameType::CLOCK = 0x05`, conveniently
// the SAME numeric value, which is why the spec text can describe it as
// "unchanged"), so the CLOCK PAYLOAD carried after that header is:
//
//   request (client -> hub):  [t0:u32]                   4 bytes
//   reply   (hub -> client):  [t0:u32][t1:u32][t2:u32]   12 bytes
//
// i.e. exactly the legacy 13-byte shapes minus their leading type byte,
// which the frame header now supplies. t0 is the client's own clock (echoed
// back unchanged by the hub, hence appearing in both the request and the
// reply); t1/t2 are hub-time at receipt and at send. All four values live in
// the SAME 32-bit-wrapping hub-µs / client-µs domains as everywhere else in
// the protocol (§7.2) — client and hub clocks are NOT required to agree,
// that disagreement is exactly what this exchange measures.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/util/byte_io.hpp"
#include "slopsync/util/serial_arithmetic.hpp"

namespace slopsync {

inline constexpr size_t kClockRequestBytes = 4;
inline constexpr size_t kClockReplyBytes = 12;

struct ClockRequest {
    uint32_t t0 = 0;  // client-µs at send
};

struct ClockReply {
    uint32_t t0 = 0;  // echoed back unchanged from the request
    uint32_t t1 = 0;  // hub-µs at receipt
    uint32_t t2 = 0;  // hub-µs at send
};

inline size_t encodeClockRequest(uint32_t t0, std::span<std::byte> out) {
    if (out.size() < kClockRequestBytes) return 0;
    return putU32(out, t0);
}

inline Result<ClockRequest, DecodeError> decodeClockRequest(std::span<const std::byte> in) {
    using Ret = Result<ClockRequest, DecodeError>;
    if (in.size() < kClockRequestBytes) return Ret::err(DecodeError::Truncated);
    return Ret::ok(ClockRequest{getU32(in.subspan(0, 4))});
}

inline size_t encodeClockReply(uint32_t t0, uint32_t t1, uint32_t t2, std::span<std::byte> out) {
    if (out.size() < kClockReplyBytes) return 0;
    putU32(out.subspan(0, 4), t0);
    putU32(out.subspan(4, 4), t1);
    putU32(out.subspan(8, 4), t2);
    return kClockReplyBytes;
}

inline Result<ClockReply, DecodeError> decodeClockReply(std::span<const std::byte> in) {
    using Ret = Result<ClockReply, DecodeError>;
    if (in.size() < kClockReplyBytes) return Ret::err(DecodeError::Truncated);
    ClockReply r;
    r.t0 = getU32(in.subspan(0, 4));
    r.t1 = getU32(in.subspan(4, 4));
    r.t2 = getU32(in.subspan(8, 4));
    return Ret::ok(r);
}

// The client-side result of one CLOCK exchange (SPEC §7.1):
//   offset = ((t1 - t0) + (t2 - t3)) / 2     (hub-time minus client-time)
//   rtt    = (t3 - t0) - (t2 - t1)
// where t3 is client-µs at reply RECEIPT (not carried on the wire — the
// client already knows its own clock reading the instant the reply arrives).
struct ClockSync {
    int32_t offsetUs = 0;
    uint32_t rttUs = 0;
};

// Every subtraction above crosses two DIFFERENT clock domains (client vs.
// hub) and every value independently wraps its own u32 range (§7.2) — a
// plain `a - b` would be correct arithmetic only by accident near either
// wrap boundary. util/serial_arithmetic.hpp's timeDelta() is the library's
// one sanctioned wrap-safe "signed difference of two u32 timestamps"
// primitive; every subtraction the formulas need goes through it, computed
// in int64 so the two half-sums can't overflow int32 before the final
// narrow. This is the only place these four values are combined — no
// inline `t1 - t0` exists elsewhere in the library.
inline ClockSync computeClockSync(uint32_t t0, uint32_t t1, uint32_t t2, uint32_t t3) {
    const int64_t d1 = int64_t(timeDelta(t1, t0));  // t1 - t0, windowed
    const int64_t d2 = int64_t(timeDelta(t2, t3));  // t2 - t3, windowed
    const int64_t offset = (d1 + d2) / 2;

    const int64_t dRttObserved = int64_t(timeDelta(t3, t0));  // t3 - t0, windowed
    const int64_t dHubHold = int64_t(timeDelta(t2, t1));      // t2 - t1, windowed
    const int64_t rtt = dRttObserved - dHubHold;

    ClockSync s;
    s.offsetUs = int32_t(offset);
    s.rttUs = uint32_t(rtt);
    return s;
}

}  // namespace slopsync
