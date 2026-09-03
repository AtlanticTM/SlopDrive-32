#pragma once

// PacingRing -- the Core-0 hand-off buffer between the SlopSync motion STREAM
// channels and the motion link.
// Constraints:
// - Hardware-free and FreeRTOS-free by design: producer (onStreamBundle) and
//   consumer (drainMotionStream) are the SAME hub task, so a lock here would
//   guard a race that structurally cannot happen. Native-testable
//   (test/native/test_pacing_ring).
// - Entries are pushed in non-decreasing due_us order and leave in ARRIVAL
//   order; the ring never reorders and never drops for age.
// - RELEASE DEPTH IS ONE. The engine on the RP parks exactly one scheduled
//   plan (slopmotion _next, last commit wins), so a second future-anchored
//   segment released before the first promotes EVICTS it. Releasing the head
//   only once the previously released anchor has passed is what keeps a
//   client's lookahead from erasing its own stream.
// See: include/comms/SlopSyncHubService.h, docs/rp-motion-port.md.

#include <array>
#include <cstddef>
#include <cstdint>

namespace slopdrive {

struct PacingEntry {
    uint64_t due_us = 0;   // device us, esp_timer_get_time() domain (unwrapped 64-bit)
    float    target = 0.0f;
    float    vel    = 0.0f;
    // WAVEFORM (0x0085 motion-segment) carries a commanded duration + an
    // EXPLICIT end-velocity presence flag; CHASE (0x0084 motion-input) leaves
    // has_duration false and derives has_end_vel from vel != 0 at push time.
    // Both channels share this ring, so the two paths differ ONLY at ingress.
    uint32_t duration_us  = 0;
    bool     has_duration = false;
    bool     has_end_vel  = false;
    // RFC-030: the EFFECTIVE curve family of the grant that produced this
    // entry (registry curve_families; 0 = unspecified), stamped per-entry at
    // ingress because entries from different sessions can interleave here.
    uint8_t  curve_family = 0;
};

class PacingRing {
public:
    static constexpr size_t kCapacity = 64;

    // Pushes one entry. Returns true if the ring was already full and the
    // oldest entry was overwritten to make room (newest wins); the caller
    // counts that as a drop.
    bool push(const PacingEntry& e) {
        bool overwrote = false;
        if (_count == kCapacity) {
            _tail = (_tail + 1) % kCapacity;  // evict oldest
            overwrote = true;
        } else {
            ++_count;
        }
        _buf[_head] = e;
        _head = (_head + 1) % kCapacity;
        return overwrote;
    }

    // Releases the oldest entry, with its own anchor intact, as soon as the
    // receiver's single scheduled-plan slot is free. The slot frees when the
    // previously released entry's anchor passes, so a segment leaves here one
    // segment BEFORE it has to start, never at its due time. Callers loop this
    // until it returns false.
    bool popReleased(uint64_t now_us, PacingEntry& out) {
        if (_count == 0 || now_us < _released_due_us) return false;
        out = _buf[_tail];
        _tail = (_tail + 1) % kCapacity;
        --_count;
        _released_due_us = out.due_us;
        return true;
    }

    // The oldest entry still in the ring, WITHOUT regard to its due time --
    // nullptr when empty. This is the RFC-008 one-segment LOOKAHEAD: call it
    // straight after popReleased() and it hands back the segment that FOLLOWS
    // the one just released, which is what the handoff sanity guard needs to
    // bound the released segment's end velocity.
    //
    // Read-only and non-consuming by design. Nothing about the guard may
    // change what the ring delivers or when.
    const PacingEntry* peekOldest() const {
        return _count == 0 ? nullptr : &_buf[_tail];
    }

    size_t size() const { return _count; }

private:
    std::array<PacingEntry, kCapacity> _buf{};
    size_t   _head = 0, _tail = 0, _count = 0;
    uint64_t _released_due_us = 0;
};

}  // namespace slopdrive
