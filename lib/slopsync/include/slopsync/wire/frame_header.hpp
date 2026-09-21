// slopsync-core — the 8-byte frame header, SPEC §5.1.
//   [type:u8][flags:u8][channel:u16][seq:u16][len:u16]   little-endian
// One frame = one transport datagram/message where the binding allows (§13);
// `len` is payload length excluding this header.
#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/util/byte_io.hpp"

namespace slopsync {

struct FrameHeader {
    uint8_t  type = 0;
    uint8_t  flags = 0;
    uint16_t channel = 0;  // 0x0000 = session-scoped (§5.1)
    uint16_t seq = 0;      // 0 where the class is unsequenced
    uint16_t len = 0;      // payload bytes after the header

    bool fragStart() const { return flags & flags::FRAG_START; }
    bool fragMore() const { return flags & flags::FRAG_MORE; }
};

// Encode into `out`; returns kHeaderBytes, or 0 if out is too small.
inline size_t encodeFrameHeader(const FrameHeader& h, std::span<std::byte> out) {
    if (out.size() < kHeaderBytes) return 0;
    putU8(out.subspan(0), h.type);
    putU8(out.subspan(1), h.flags);
    putU16(out.subspan(2), h.channel);
    putU16(out.subspan(4), h.seq);
    putU16(out.subspan(6), h.len);
    return kHeaderBytes;
}

// Decode from the front of `in`. nullopt iff in.size() < 8. Unknown type
// bytes decode fine — tolerance (§4.3) is the caller's dispatch concern.
// NOTE: an ESTOP frame (type 0xE5) is NOT header-shaped — callers MUST check
// for the 4×0xE5 magic (wire/estop_frame.hpp) BEFORE header-decoding (§5.5).
inline std::optional<FrameHeader> decodeFrameHeader(std::span<const std::byte> in) {
    if (in.size() < kHeaderBytes) return std::nullopt;
    FrameHeader h;
    h.type    = getU8(in.subspan(0));
    h.flags   = getU8(in.subspan(1));
    h.channel = getU16(in.subspan(2));
    h.seq     = getU16(in.subspan(4));
    h.len     = getU16(in.subspan(6));
    return h;
}

}  // namespace slopsync
