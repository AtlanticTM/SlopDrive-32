// slopsync-core — BEACON, SPEC §13.7.
//
// Raw payload, 13 bytes: `[boot_id:u32][etag:8B][pairing_open:u8]`.
// Broadcast by the hub (or its relay) every 500 ms on ESP-NOW, ONLY while
// the pairing window is open — new peers use it to discover the segment
// and confirm identity (boot_id) + catalog compatibility (etag) before
// running PAIR_REQ over unicast.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/util/byte_io.hpp"

namespace slopsync {

inline constexpr size_t kBeaconFrameBytes = 4 + size_t(limits::etag_bytes) + 1;

struct BeaconFrame {
    uint32_t boot_id = 0;
    std::array<std::byte, limits::etag_bytes> etag{};
    bool pairing_open = false;
};

inline size_t encodeBeacon(const BeaconFrame& b, std::span<std::byte> out) {
    if (out.size() < kBeaconFrameBytes) return 0;
    putU32(out.subspan(0, 4), b.boot_id);
    std::memcpy(out.data() + 4, b.etag.data(), b.etag.size());
    putU8(out.subspan(4 + b.etag.size(), 1), b.pairing_open ? 1 : 0);
    return kBeaconFrameBytes;
}

inline Result<BeaconFrame, DecodeError> decodeBeacon(std::span<const std::byte> in) {
    using Ret = Result<BeaconFrame, DecodeError>;
    if (in.size() < kBeaconFrameBytes) return Ret::err(DecodeError::Truncated);
    BeaconFrame b;
    b.boot_id = getU32(in.subspan(0, 4));
    std::memcpy(b.etag.data(), in.data() + 4, b.etag.size());
    b.pairing_open = getU8(in.subspan(4 + b.etag.size(), 1)) != 0;
    return Ret::ok(b);
}

}  // namespace slopsync
