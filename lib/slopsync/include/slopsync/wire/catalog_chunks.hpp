// slopsync-core — catalog transfer chunking, SPEC §8.4:
//   CATALOG_CHUNK (raw): chunk_index:u16, chunk_count:u16, payload <=192B
//   (limits::catalog_chunk_payload) — a byte-range of the deterministic
//   catalog encoding (catalog_codec.hpp). 192 fits every binding's MTU
//   (§13.1's 242-byte floor minus the frame header and this 4-byte prefix)
//   unfragmented; WS MAY carry multiple chunks back-to-back in one message.
//
// Receiver-side reassembly (§8.4): reassemble by index; request missing
// indices after a gap timeout (`catalog_chunk_gap_timeout_ms`, 500ms
// default); abandon after `frag_reassembly_timeout_ms` (5s) total, then
// either retry from scratch or fall back to the static profile (§8.5,
// static_profile.hpp). This file only measures elapsed time against those
// two thresholds — it does not itself send CATALOG_REQ or drive a clock;
// callers own the transport and the actual timer (hence plain uint32_t
// `nowMs` parameters throughout, not an IClock&: this header has nothing to
// poll on its own, so there is nothing purity gains by injecting the whole
// clock interface just to read one value each call).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/util/byte_io.hpp"
#include "slopsync/util/serial_arithmetic.hpp"

namespace slopsync {

// Number of CATALOG_CHUNK frames needed to carry `totalBytes` of encoded
// catalog, at limits::catalog_chunk_payload bytes per chunk. 0 iff
// totalBytes == 0 (nothing to send).
inline constexpr size_t chunkCount(size_t totalBytes) {
    if (totalBytes == 0) return 0;
    return (totalBytes + limits::catalog_chunk_payload - 1) / limits::catalog_chunk_payload;
}

// Fills `out` with one CATALOG_CHUNK raw payload — [chunk_index:u16 LE]
// [chunk_count:u16 LE][payload bytes] — for `index` into `encoded` (the full
// deterministic catalog encoding, e.g. encodeCatalog's output). Returns
// total bytes written (4 + this chunk's payload length), or 0 if `index` is
// out of range for `encoded`'s size or `out` is too small to hold it.
inline size_t fillChunk(std::span<const std::byte> encoded, uint16_t index, std::span<std::byte> out) {
    size_t cc = chunkCount(encoded.size());
    if (cc == 0 || index >= cc) return 0;

    size_t offset = size_t(index) * limits::catalog_chunk_payload;
    size_t payloadLen = encoded.size() - offset;
    if (payloadLen > limits::catalog_chunk_payload) payloadLen = limits::catalog_chunk_payload;

    size_t total = 4 + payloadLen;
    if (out.size() < total) return 0;

    putU16(out.subspan(0, 2), index);
    putU16(out.subspan(2, 2), uint16_t(cc));
    std::memcpy(out.data() + 4, encoded.data() + offset, payloadLen);
    return total;
}

// Fixed-capacity receiver-side reassembler. MaxChunks bounds both the bitmap
// and the backing buffer (MaxChunks * catalog_chunk_payload bytes); callers
// with larger catalogs pick a bigger MaxChunks. No heap either way.
template <size_t MaxChunks = 64>
class ChunkReassembler {
public:
    static constexpr size_t kMaxTotalBytes = MaxChunks * limits::catalog_chunk_payload;

    // Starts (or restarts) a reassembly for a transfer that will span
    // `chunkCount` chunks totaling `totalBytes` of encoded catalog.
    void begin(uint16_t chunkCount, size_t totalBytes, uint32_t nowMs) {
        _chunkCount = chunkCount;
        _totalBytes = totalBytes;
        _receivedCount = 0;
        _received.fill(false);
        _startMs = nowMs;
        _lastProgressMs = nowMs;
        _active = (chunkCount > 0) && (chunkCount <= MaxChunks) && (totalBytes <= kMaxTotalBytes);
    }

    bool active() const { return _active; }

    // Accepts one CATALOG_CHUNK raw payload (as produced by fillChunk).
    // Returns true iff it was structurally valid, consistent with this
    // transfer's chunk_count, and copied in (duplicates are accepted
    // idempotently and still return true). False on any mismatch/overflow —
    // the caller decides whether that's worth logging; it is never fatal.
    bool insert(std::span<const std::byte> chunkPayload, uint32_t nowMs) {
        if (!_active || chunkPayload.size() < 4) return false;

        uint16_t idx = getU16(chunkPayload.subspan(0, 2));
        uint16_t cc = getU16(chunkPayload.subspan(2, 2));
        if (cc != _chunkCount || idx >= _chunkCount) return false;

        size_t payloadLen = chunkPayload.size() - 4;
        size_t offset = size_t(idx) * limits::catalog_chunk_payload;
        if (offset + payloadLen > _buf.size()) return false;

        std::memcpy(_buf.data() + offset, chunkPayload.data() + 4, payloadLen);
        if (!_received[idx]) {
            _received[idx] = true;
            ++_receivedCount;
        }
        _lastProgressMs = nowMs;
        return true;
    }

    bool complete() const { return _active && _receivedCount == _chunkCount; }

    // Writes the ascending list of not-yet-received chunk indices into
    // `out` (capped at out.size()), returning how many were written — the
    // exact shape CATALOG_REQ{chunks: [...]} needs for selective repair.
    size_t missingIndices(std::span<uint16_t> out) const {
        size_t n = 0;
        for (uint16_t i = 0; i < _chunkCount && n < out.size(); ++i) {
            if (!_received[i]) out[n++] = i;
        }
        return n;
    }

    // §8.4: total-abandon threshold since the transfer began — callers
    // check this and, on true, give up and either restart from scratch or
    // fall back to the static profile (§8.5).
    bool timedOut(uint32_t nowMs) const {
        return _active && timeReached(nowMs, _startMs + limits::frag_reassembly_timeout_ms);
    }

    // §8.4: gap threshold since the last accepted chunk, while still
    // incomplete — callers check this and, on true, emit
    // CATALOG_REQ{chunks: missingIndices()} to request the holes.
    bool gapElapsed(uint32_t nowMs) const {
        return _active && !complete() &&
               timeReached(nowMs, _lastProgressMs + limits::catalog_chunk_gap_timeout_ms);
    }

    // Valid once complete(); the reassembled bytes are exactly `totalBytes`
    // from begin(), regardless of MaxChunks' larger backing capacity.
    std::span<const std::byte> assembled() const {
        return std::span<const std::byte>(_buf.data(), _totalBytes);
    }

private:
    std::array<std::byte, kMaxTotalBytes> _buf{};
    std::array<bool, MaxChunks> _received{};
    uint16_t _chunkCount = 0;
    uint32_t _receivedCount = 0;
    size_t _totalBytes = 0;
    uint32_t _startMs = 0;
    uint32_t _lastProgressMs = 0;
    bool _active = false;
};

}  // namespace slopsync
