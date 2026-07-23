// slopsync-core — control-plane fragmentation and reassembly, SPEC §5.6.
//
// Fragmentation exists ONLY for control-plane frames that cannot fit the
// binding's MTU (catalog-transfer fallback, large ECHOs on ESP-NOW). It is
// spec-forbidden for data-plane frames: STATE fits the 242-byte ESP-NOW floor
// by catalog design (§9.1) and STREAM bundles size themselves to the MTU
// (§5.4) — "a bundle never fragments". This header does not know or care
// which frame types are control vs data; callers gate that decision (they
// only ever hand this an oversized CONTROL frame).
//
// The ESTOP frame (§5.5, 12 raw bytes, 4×0xE5 magic) is NOT header-shaped and
// MUST pass through completely unfragmented and unharmed. In practice it
// always does: 12 bytes is far under every binding's MTU floor (242, §13.1),
// so it always takes the untouched passthrough branch below. The assert in
// fragmentFrame() documents/defends that invariant for adversarial
// maxFrameBytes values; it is not reachable in any conformant configuration.
#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/util/byte_io.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/estop_frame.hpp"
#include "slopsync/wire/frame_buffer.hpp"
#include "slopsync/wire/frame_header.hpp"

namespace slopsync {

// Each fragment payload is prefixed with a 2-byte frag_index (§5.6).
inline constexpr size_t kFragIndexBytes = 2;

// ============================================================================
// Fragmenter — splits ONE oversized control frame (header + payload) into
// fragments that each fit `maxFrameBytes`, and emits them via `emit`.
//
// Flags per fragment (§5.6): first = FRAG_START|FRAG_MORE, middle =
// FRAG_MORE, last = neither, single-fragment = FRAG_START alone. These four
// patterns fall out of one uniform rule applied per fragment index i of N
// total: START on i==0, MORE on every i < N-1. (N==1 therefore yields START
// alone — the "single" case — with no special-casing needed.)
//
// Frames that already fit `maxFrameBytes` pass through completely untouched
// (original bytes, original flags — no frag flags added): SPEC's "bindings
// whose MTU exceeds every control message never emit fragments".
//
// `emit` is called once per output fragment (or once, with the original
// bytes, for the passthrough case) with a std::span<const std::byte> of that
// fragment's complete wire bytes (header + [frag_index][slice]). No heap: the
// working buffer is a fixed kFrameBufferCapacity-sized stack array reused
// across calls to `emit` — Callback must fully consume/copy the span before
// returning if it needs to keep it (matches every other span-in callback in
// this library, e.g. ITransport::write).
//
// Returns false (no fragments emitted) if: `wholeFrame` is too short to carry
// a header, `maxFrameBytes` is too small to fit even one fragment's header +
// frag_index prefix, or `maxFrameBytes` exceeds kFrameBufferCapacity (the
// scratch buffer's size — no conformant binding's MTU does).
template <typename Callback>
inline bool fragmentFrame(std::span<const std::byte> wholeFrame, uint16_t maxFrameBytes, Callback&& emit) {
    if (maxFrameBytes > kFrameBufferCapacity) return false;

    // Passthrough: untouched, no frag flags — checked by SIZE alone, before
    // any header decode, so a 12-byte ESTOP frame always takes this branch
    // (its bytes are opaque to us here; it just happens to always fit).
    if (wholeFrame.size() <= maxFrameBytes) {
        emit(std::span<const std::byte>(wholeFrame));
        return true;
    }

    // Past this point we ARE about to fragment — defend the ESTOP invariant.
    bool looksLikeEstop = wholeFrame.size() == kEstopFrameBytes &&
                           wholeFrame[0] == kEstopMagicByte && wholeFrame[1] == kEstopMagicByte &&
                           wholeFrame[2] == kEstopMagicByte && wholeFrame[3] == kEstopMagicByte;
    assert(!looksLikeEstop && "ESTOP frames must never be fragmented (SPEC §5.5, §5.6)");
    if (looksLikeEstop) return false;  // release-build safety net for the assert above

    auto header = decodeFrameHeader(wholeFrame);
    if (!header) return false;  // too short to even carry an 8-byte header

    if (maxFrameBytes <= kHeaderBytes + kFragIndexBytes) return false;  // no room for any fragment

    std::span<const std::byte> payload = wholeFrame.subspan(kHeaderBytes);
    const size_t budget = size_t(maxFrameBytes) - kHeaderBytes - kFragIndexBytes;
    const size_t total = payload.size();
    const size_t numFragments = (total + budget - 1) / budget;  // ceil

    std::array<std::byte, kFrameBufferCapacity> scratch{};

    for (size_t i = 0; i < numFragments; ++i) {
        const size_t offset = i * budget;
        const size_t len = std::min(budget, total - offset);

        FrameHeader fh = *header;
        fh.flags = 0;
        if (i == 0) fh.flags |= flags::FRAG_START;
        if (i + 1 < numFragments) fh.flags |= flags::FRAG_MORE;
        fh.len = uint16_t(kFragIndexBytes + len);

        std::span<std::byte> out(scratch);
        size_t pos = encodeFrameHeader(fh, out);
        pos += putU16(out.subspan(pos), uint16_t(i));
        std::memcpy(out.data() + pos, payload.data() + offset, len);
        pos += len;

        emit(std::span<const std::byte>(scratch.data(), pos));
    }
    return true;
}

// ============================================================================
// Reassembler — the receive side. Fixed slots (SPEC §5.6:
// limits::frag_max_concurrent_per_session = 2 concurrent reassemblies),
// keyed by (type, seq). Handles out-of-order arrival, idempotent duplicates,
// 5 s timeout (limits::frag_reassembly_timeout_ms), and slot exhaustion by
// evicting the OLDEST slot ("excess: discard oldest").
//
// accept() returns Result<std::optional<FrameBuffer>, DecodeError> rather
// than the bare std::optional<FrameBuffer> the brief sketches — a deliberate,
// documented deviation (see file-level test/report notes): it reuses the
// library's existing Result<T,E> idiom and the existing DecodeError enum
// (core/result.hpp already has CapacityExceeded) to satisfy "report error via
// enum result" without inventing a parallel error type. Reading it:
//   Ok(nullopt)   — fragment accepted, reassembly not yet complete (this
//                    covers idempotent duplicates too: nothing changed).
//   Ok(FrameBuffer) — this fragment was the last piece; frame is complete.
//   Err(CapacityExceeded) — placing this fragment would overflow the
//                    reassembled-frame capacity; the slot is discarded.
class Reassembler {
public:
    struct Stats {
        uint32_t reassemblies_completed = 0;
        uint32_t timeouts = 0;
        uint32_t evictions = 0;
    };

    Result<std::optional<FrameBuffer>, DecodeError> accept(const FrameHeader& header,
                                                            std::span<const std::byte> fragPayload,
                                                            uint32_t nowMs) {
        using R = Result<std::optional<FrameBuffer>, DecodeError>;

        if (fragPayload.size() < kFragIndexBytes) {
            return R::ok(std::nullopt);  // malformed/short fragment: silently dropped, non-crashing
        }
        const uint16_t index = getU16(fragPayload.subspan(0, 2));
        if (index >= kMaxFragmentsPerMessage) {
            return R::err(DecodeError::CapacityExceeded);
        }
        std::span<const std::byte> slice = fragPayload.subspan(kFragIndexBytes);

        // Opportunistic lazy expiry: a slot for this (type,seq) that has
        // already timed out is treated as absent (freed) even if the caller
        // hasn't pumped expireStale() since. Callers SHOULD still call
        // expireStale() periodically (it's the only way stale slots get
        // reclaimed with no further traffic on that key at all).
        expireOne(header.type, header.seq, nowMs);

        Slot* slot = findSlot(header.type, header.seq);
        if (!slot) {
            slot = allocSlot(header.type, header.seq, nowMs);
            if (!slot) return R::ok(std::nullopt);  // unreachable: allocSlot always evicts to make room
        }
        slot->channel = header.channel;

        const bool isStart = (header.flags & flags::FRAG_START) != 0;
        const bool isMore = (header.flags & flags::FRAG_MORE) != 0;

        bool overflowed = false;

        if (isStart && !isMore) {
            // Single-fragment message: the whole payload in one piece.
            slot->totalFragments = 1;
            slot->lastFragLen = uint16_t(slice.size());
            if (!placeFragment(*slot, 0, slice)) overflowed = true;
        } else if (isMore) {
            // Full-size fragment (first-of-many or middle) — its length IS
            // the per-fragment unit size, discovered from whichever one of
            // these arrives first regardless of index order.
            if (slot->unitLen == 0) slot->unitLen = uint16_t(slice.size());
            if (!placeFragment(*slot, index, slice)) overflowed = true;
            if (!overflowed && slot->havePendingLast && slot->unitLen != 0) {
                if (!placeFragment(*slot, slot->pendingLastIndex,
                                    std::span<const std::byte>(slot->pendingLastBytes.data(), slot->pendingLastLen))) {
                    overflowed = true;
                }
                slot->havePendingLast = false;
            }
        } else {
            // Last fragment of a multi-fragment message (flags==0): its
            // index reveals the total fragment count immediately.
            slot->totalFragments = uint16_t(index + 1);
            slot->lastFragLen = uint16_t(slice.size());
            if (slot->unitLen != 0) {
                if (!placeFragment(*slot, index, slice)) overflowed = true;
            } else {
                // Unit size not known yet (no full fragment seen so far) —
                // buffer this one piece until it is.
                std::memcpy(slot->pendingLastBytes.data(), slice.data(), slice.size());
                slot->pendingLastLen = uint16_t(slice.size());
                slot->pendingLastIndex = index;
                slot->havePendingLast = true;
            }
        }

        if (overflowed) {
            *slot = Slot{};
            return R::err(DecodeError::CapacityExceeded);
        }

        return checkComplete(*slot);
    }

    // Sweep every active slot; deactivate (and count as a timeout) any whose
    // deadline (start + frag_reassembly_timeout_ms) has passed. Call this
    // periodically from an update() loop, per the brief.
    void expireStale(uint32_t nowMs) {
        for (auto& s : _slots) {
            if (!s.active) continue;
            expireOne(s.type, s.seq, nowMs);
        }
    }

    const Stats& stats() const { return _stats; }

private:
    // Practical cap on fragments-per-message: bounded by kFrameBufferCapacity
    // (512) divided by the smallest budget any real binding would ever use
    // (the ESP-NOW floor gives a budget of 232 — 3 fragments worst case for a
    // full-capacity frame). 32 leaves generous headroom for smaller
    // hypothetical MTUs without needing a heap-allocated bitmask.
    static constexpr size_t kMaxFragmentsPerMessage = 32;
    static constexpr size_t kMaxSlotPayload = kFrameBufferCapacity - kHeaderBytes;

    struct Slot {
        bool active = false;
        uint8_t type = 0;
        uint16_t channel = 0;
        uint16_t seq = 0;
        uint32_t startMs = 0;

        uint16_t unitLen = 0;         // full-fragment size, 0 = not yet known
        uint16_t totalFragments = 0;  // N, 0 = not yet known
        uint16_t lastFragLen = 0;
        uint32_t receivedMask = 0;    // bit i set = fragment i placed
        uint16_t receivedCount = 0;

        bool havePendingLast = false;
        uint16_t pendingLastIndex = 0;
        uint16_t pendingLastLen = 0;
        std::array<std::byte, kMaxSlotPayload> pendingLastBytes{};

        std::array<std::byte, kMaxSlotPayload> data{};
    };

    static constexpr size_t kMaxSlots = limits::frag_max_concurrent_per_session;
    std::array<Slot, kMaxSlots> _slots{};
    Stats _stats{};

    Slot* findSlot(uint8_t type, uint16_t seq) {
        for (auto& s : _slots) {
            if (s.active && s.type == type && s.seq == seq) return &s;
        }
        return nullptr;
    }

    // Deactivates the (type,seq) slot in-place if its deadline has passed.
    void expireOne(uint8_t type, uint16_t seq, uint32_t nowMs) {
        Slot* s = findSlot(type, seq);
        if (!s) return;
        uint32_t deadline = s->startMs + limits::frag_reassembly_timeout_ms;
        if (timeReached(nowMs, deadline)) {
            *s = Slot{};
            _stats.timeouts++;
        }
    }

    Slot* allocSlot(uint8_t type, uint16_t seq, uint32_t nowMs) {
        for (auto& s : _slots) {
            if (!s.active) {
                s = Slot{};
                s.active = true;
                s.type = type;
                s.seq = seq;
                s.startMs = nowMs;
                return &s;
            }
        }
        // All slots busy: evict the OLDEST (largest elapsed age via
        // wrap-safe timeDelta — never a raw `<` on a timestamp, per
        // util/serial_arithmetic.hpp's rule).
        Slot* oldest = &_slots[0];
        int32_t oldestAge = timeDelta(nowMs, oldest->startMs);
        for (auto& s : _slots) {
            int32_t age = timeDelta(nowMs, s.startMs);
            if (age > oldestAge) {
                oldest = &s;
                oldestAge = age;
            }
        }
        *oldest = Slot{};
        oldest->active = true;
        oldest->type = type;
        oldest->seq = seq;
        oldest->startMs = nowMs;
        _stats.evictions++;
        return oldest;
    }

    // Places `bytes` at fragment `index`'s offset. Idempotent: a duplicate
    // (bit already set) is a no-op success. Returns false on capacity
    // overflow (caller discards the whole slot and reports the error).
    bool placeFragment(Slot& slot, uint16_t index, std::span<const std::byte> bytes) {
        if (slot.receivedMask & (1u << index)) return true;  // duplicate: idempotent no-op
        const size_t offset = size_t(index) * slot.unitLen;
        if (offset + bytes.size() > slot.data.size()) return false;
        std::memcpy(slot.data.data() + offset, bytes.data(), bytes.size());
        slot.receivedMask |= (1u << index);
        slot.receivedCount++;
        return true;
    }

    Result<std::optional<FrameBuffer>, DecodeError> checkComplete(Slot& slot) {
        using R = Result<std::optional<FrameBuffer>, DecodeError>;
        if (slot.totalFragments == 0 || slot.receivedCount != slot.totalFragments) {
            return R::ok(std::nullopt);
        }

        const size_t totalLen = size_t(slot.totalFragments - 1) * slot.unitLen + slot.lastFragLen;
        if (totalLen > kFrameBufferCapacity - kHeaderBytes) {
            slot = Slot{};
            return R::err(DecodeError::CapacityExceeded);
        }

        FrameHeader outHeader;
        outHeader.type = slot.type;
        outHeader.flags = 0;
        outHeader.channel = slot.channel;
        outHeader.seq = slot.seq;
        outHeader.len = uint16_t(totalLen);

        FrameBuffer fb;
        auto out = fb.writable();
        size_t pos = encodeFrameHeader(outHeader, out);
        std::memcpy(out.data() + pos, slot.data.data(), totalLen);
        fb.setSize(pos + totalLen);

        slot = Slot{};
        _stats.reassemblies_completed++;
        return R::ok(std::optional<FrameBuffer>(fb));
    }
};

}  // namespace slopsync
