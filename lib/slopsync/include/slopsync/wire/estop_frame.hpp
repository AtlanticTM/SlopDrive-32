// slopsync-core — the ESTOP frame, SPEC §5.5. 12 bytes, deliberately outside
// the normal 8-byte header discipline (§5.1) so it can be recognized WITHOUT
// deframing, on a raw byte stream, by a serial ISR or relay hot path:
//
//   E5 E5 E5 E5 | cause:u8 origin:u8 seq:u16 | crc32:u32      (all LE)
//
// crc32 is CRC-32 IEEE (wire/crc32.hpp) over the first 8 bytes (the 4-byte
// magic + cause + origin + seq). False-trigger probability with the CRC
// gate is 2^-32 (§5.5).
//
// scanForEstop() is the byte-serial hot path referenced in §5.5 and the
// relay fast path in §14.2: no deframing, no queueing, no allocation — safe
// to run on an ISR stack over whatever bytes have arrived so far.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/util/byte_io.hpp"
#include "slopsync/wire/crc32.hpp"

namespace slopsync {

inline constexpr size_t kEstopFrameBytes = 12;
inline constexpr size_t kEstopCrcOffset = 8;   // crc32 covers bytes [0, 8)
inline constexpr std::byte kEstopMagicByte{0xE5};

// SPEC §5.5: cause of an ESTOP initiation event.
enum class EstopCause : uint8_t {
    user = 0,      // operator-initiated (physical button, UI)
    deadman = 1,   // deadman timeout expired (§11.3)
    fault = 2,     // hub/driver-detected fault
    relay = 3,     // relay-originated (segment-local safety event)
};

struct EstopFrame {
    uint8_t  cause = 0;   // EstopCause
    uint8_t  origin = 0;  // AccessLevel of the initiator
    uint16_t seq = 0;     // increments per initiation event (§5.5)
};

// Encodes exactly kEstopFrameBytes (12) into `out`: the 4x0xE5 magic, cause,
// origin, seq (LE), then crc32 (LE) over bytes [0,8). Returns 0 if `out` is
// too small — encoders can't otherwise fail (core/result.hpp).
inline size_t encodeEstop(const EstopFrame& frame, std::span<std::byte> out) {
    if (out.size() < kEstopFrameBytes) return 0;

    out[0] = kEstopMagicByte;
    out[1] = kEstopMagicByte;
    out[2] = kEstopMagicByte;
    out[3] = kEstopMagicByte;
    putU8(out.subspan(4), frame.cause);
    putU8(out.subspan(5), frame.origin);
    putU16(out.subspan(6), frame.seq);

    uint32_t crc = crc32(out.subspan(0, kEstopCrcOffset));
    putU32(out.subspan(kEstopCrcOffset), crc);
    return kEstopFrameBytes;
}

// Decodes a 12-byte ESTOP frame starting at in[0] — the caller has already
// located the 4x0xE5 magic (see scanForEstop for the byte-serial search over
// an unaligned stream). Truncated if fewer than 12 bytes are available;
// Malformed if the leading magic isn't actually 4x0xE5; BadCrc if the
// trailing crc32 doesn't validate against bytes [0,8) (§5.5).
inline Result<EstopFrame, DecodeError> decodeEstop(std::span<const std::byte> in) {
    if (in.size() < kEstopFrameBytes) {
        return Result<EstopFrame, DecodeError>::err(DecodeError::Truncated);
    }
    for (size_t i = 0; i < 4; ++i) {
        if (in[i] != kEstopMagicByte) {
            return Result<EstopFrame, DecodeError>::err(DecodeError::Malformed);
        }
    }
    uint32_t crc = getU32(in.subspan(kEstopCrcOffset, 4));
    if (crc32(in.subspan(0, kEstopCrcOffset)) != crc) {
        return Result<EstopFrame, DecodeError>::err(DecodeError::BadCrc);
    }

    EstopFrame frame;
    frame.cause  = getU8(in.subspan(4));
    frame.origin = getU8(in.subspan(5));
    frame.seq    = getU16(in.subspan(6));
    return Result<EstopFrame, DecodeError>::ok(frame);
}

struct EstopScanResult {
    bool found = false;
    size_t offset = 0;   // byte offset of the magic's first 0xE5 within `stream`
    EstopFrame frame{};
};

// Byte-serial magic scanner (SPEC §5.5, relay fast path §14.2): finds the
// FIRST occurrence of 4 consecutive 0xE5 bytes at ANY offset in `stream`,
// then attempts to decode+CRC-validate the 8 bytes that follow. On a CRC
// failure the scan CONTINUES from the very next byte (not past the whole
// candidate) — a coincidental or corrupt 0xE5 run must not blind the
// scanner to a real frame overlapping or following it. If a magic run is
// found with fewer than 8 trailing bytes available, scanning stops there
// with found=false and performs no out-of-bounds read (every subsequent
// offset would have even less trailing data, by construction).
inline EstopScanResult scanForEstop(std::span<const std::byte> stream) {
    EstopScanResult result;
    if (stream.size() < 4) return result;

    for (size_t i = 0; i + 4 <= stream.size(); ++i) {
        if (stream[i] != kEstopMagicByte || stream[i + 1] != kEstopMagicByte ||
            stream[i + 2] != kEstopMagicByte || stream[i + 3] != kEstopMagicByte) {
            continue;
        }
        if (i + kEstopFrameBytes > stream.size()) {
            // Not enough trailing bytes to form a candidate here, and none
            // of the remaining offsets (all further right) can have more.
            return result;
        }
        auto candidate = decodeEstop(stream.subspan(i, kEstopFrameBytes));
        if (candidate) {
            result.found = true;
            result.offset = i;
            result.frame = candidate.value();
            return result;
        }
        // BadCrc: keep scanning from i+1 (not i+4) — a 4x0xE5 run can
        // legitimately overlap with the start of the next one.
    }
    return result;
}

}  // namespace slopsync
