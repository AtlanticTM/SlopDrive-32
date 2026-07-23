// slopsync-core — standard COBS (Consistent Overhead Byte Stuffing), the
// serial binding's framing layer (SPEC §13.5): "encode each SlopSync frame
// with COBS, append 0x00" as the delimiter.
//
// IMPORTANT: 0x00 delimiters are OUTSIDE these functions by design. Neither
// cobsEncode nor cobsDecode reads or writes the trailing 0x00 — the caller
// appends it after cobsEncode() and strips it (or splits on it) before
// calling cobsDecode(). This mirrors §13.5: "a receiver in unsynced/corrupt
// state MUST still run the 4x0xE5 scanner on raw bytes between delimiters" —
// delimiter-finding is the transport's job, COBS (de)framing is this file's.
//
// Standard COBS properties relied on elsewhere in the library (§13.5):
// a COBS-encoded region never contains a literal 0x00 byte, and a run of
// non-zero bytes (e.g. an ESTOP frame's 4x0xE5 magic) passes through
// byte-for-byte unchanged as long as no 0x00 fell inside that run in the
// original data — only actual zero bytes (and the 254-byte max-run cap)
// cause COBS to insert/consume anything.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "slopsync/core/result.hpp"

namespace slopsync {

// Encodes `src` (COBS body only, no delimiter) into `dst`. Returns the
// number of bytes written, or 0 if `dst` is too small to hold the result —
// encoders can't otherwise fail (core/result.hpp). Worst-case expansion is
// 1 extra byte per 254 bytes of source (the max-run code 0xFF), so a caller
// sizing `dst` up front needs `src.size() + src.size()/254 + 1`.
//
// Edge case worth documenting explicitly: if a maximal 254-byte non-zero run
// ends EXACTLY at the end of `src` with no trailing zero, no bytes remain to
// need a further code byte, so none is emitted (the encoded length for
// exactly 254 non-zero input bytes is 255, not 256: `FF` + the 254 bytes).
// This is a standard, decoder-transparent COBS convention — a code byte of
// 0xFF never implies a following zero, so the decoder never expects one
// either way; the choice only affects encoded length, not round-trip
// correctness.
inline size_t cobsEncode(std::span<const std::byte> src, std::span<std::byte> dst) {
    if (dst.empty()) return 0;  // dst[0] must exist to hold the first code byte

    size_t writeIndex = 1;
    size_t codeIndex = 0;
    uint8_t code = 1;
    // Whether a trailing code byte is still owed if the input ends right
    // now: true initially (an empty input still encodes to a single code
    // byte, `01`) and after every zero-triggered flush; false only right
    // after a max-run (0xFF) flush, where ending here needs nothing more.
    bool finalZero = true;

    for (size_t readIndex = 0; readIndex < src.size(); ++readIndex) {
        uint8_t b = uint8_t(src[readIndex]);
        if (b == 0) {
            dst[codeIndex] = std::byte{code};
            finalZero = true;
            code = 1;
            if (writeIndex >= dst.size()) return 0;
            codeIndex = writeIndex++;
        } else {
            if (writeIndex >= dst.size()) return 0;
            dst[writeIndex++] = std::byte{b};
            ++code;
            if (code == 0xFFu) {
                dst[codeIndex] = std::byte{code};
                finalZero = false;
                code = 1;
                if (writeIndex >= dst.size()) return 0;
                codeIndex = writeIndex++;
            }
        }
    }

    if (finalZero || code != 1) {
        dst[codeIndex] = std::byte{code};
    } else {
        // The run opened right after a max-run flush never received any
        // data before input ended — un-reserve its slot (see edge case above).
        --writeIndex;
    }
    return writeIndex;
}

// Decodes a COBS body (delimiter already stripped by the caller) from `src`
// into `dst`. Malformed if a code byte of 0x00 appears (0x00 never occurs
// inside a valid COBS-encoded region — it is reserved for the external
// delimiter, §13.5); Truncated if a code byte claims more data bytes than
// remain in `src`; CapacityExceeded if the decoded output would not fit
// `dst` (core/result.hpp).
inline Result<size_t, DecodeError> cobsDecode(std::span<const std::byte> src,
                                               std::span<std::byte> dst) {
    size_t readIndex = 0;
    size_t writeIndex = 0;

    while (readIndex < src.size()) {
        uint8_t code = uint8_t(src[readIndex]);
        if (code == 0) {
            return Result<size_t, DecodeError>::err(DecodeError::Malformed);
        }
        ++readIndex;

        for (uint8_t i = 1; i < code; ++i) {
            if (readIndex >= src.size()) {
                return Result<size_t, DecodeError>::err(DecodeError::Truncated);
            }
            if (writeIndex >= dst.size()) {
                return Result<size_t, DecodeError>::err(DecodeError::CapacityExceeded);
            }
            dst[writeIndex++] = src[readIndex++];
        }

        // A code < 0xFF implies a zero byte followed, UNLESS we've just
        // consumed the last code byte in this buffer (that trailing zero is
        // the frame delimiter, which lives outside this function).
        if (code != 0xFFu && readIndex < src.size()) {
            if (writeIndex >= dst.size()) {
                return Result<size_t, DecodeError>::err(DecodeError::CapacityExceeded);
            }
            dst[writeIndex++] = std::byte{0};
        }
    }
    return Result<size_t, DecodeError>::ok(writeIndex);
}

}  // namespace slopsync
