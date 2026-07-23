// slopsync-core — the in-process binding, SPEC §13.6: "a first-class
// conformance instrument". It connects a hub role and a client role inside
// one process (desktop simulator, unit tests) with configurable MTU,
// injected loss/reorder/duplication, injected latency/jitter, and a
// deterministic seeded mode (seeded XorShift32 + ManualClock ⇒ bit-identical
// delivery sequences across runs, §13.6/§17.2). An implementation without
// this fault injection cannot claim conformance testing.
//
// Boundary this file draws deliberately, per §5.6: this binding does NOT
// fragment. MTU here is an admission gate only — write() of a frame larger
// than the direction's MTU is refused (false), exactly like a real
// binding's hard packet-size ceiling (ESP-NOW's 250 bytes, BLE's negotiated
// ATT MTU, ...). Fragmentation is the layer ABOVE the binding
// (wire/fragmentation.hpp); it calls write() once per already-MTU-sized
// fragment. Mixing the two responsibilities here would hide the very MTU
// pressure §5.6 exists to test against.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "slopsync/core/clock.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/transport/transport.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/estop_frame.hpp"
#include "slopsync/wire/frame_buffer.hpp"

namespace slopsync {

// Per-direction fault schedule. Two independent instances live inside every
// InProcessLink (A→B and B→A) — real half-duplex-asymmetric links (e.g. a
// noisy uplink, a clean downlink) are common enough to be worth modeling
// separately rather than sharing one profile both ways.
struct FaultProfile {
    uint16_t mtu = 250;          // whole-frame bytes admitted by write() (§13.1's mtu meaning)
    uint8_t loss_pct = 0;        // 0..100: probability a written frame is dropped
    uint8_t dup_pct = 0;         // 0..100: probability a written frame is enqueued twice
    uint8_t reorder_pct = 0;     // 0..100: probability this frame swaps delivery order with the previous one
    uint32_t latency_us = 0;     // base one-way delay added at write time
    uint32_t jitter_us = 0;      // ± uniform spread added to latency_us
};

namespace detail {

// Fixed-capacity, no-heap frame queue with delivery timestamps. Supports
// push_front (the ESTOP fast path, §10.4/§11.2/§14.2 — jump the queue) and
// push_back (everything else), plus a swap-last-two primitive used to inject
// reordering. 16 slots: generous for a unit-test/sim harness; a full ring
// means write() returns false, same as any real binding's egress queue.
class FrameQueue {
public:
    static constexpr size_t kCapacity = 16;

    bool push_back(const FrameBuffer& fb, uint32_t deliverAtUs) {
        if (_count >= kCapacity) return false;
        _items[_count] = Entry{fb, deliverAtUs};
        ++_count;
        return true;
    }

    bool push_front(const FrameBuffer& fb, uint32_t deliverAtUs) {
        if (_count >= kCapacity) return false;
        for (size_t i = _count; i > 0; --i) _items[i] = _items[i - 1];
        _items[0] = Entry{fb, deliverAtUs};
        ++_count;
        return true;
    }

    // Swaps the just-queued frame (back of the queue) with the one queued
    // immediately before it — the documented reorder mechanism (§13.6).
    void swapLastTwo() {
        if (_count >= 2) std::swap(_items[_count - 1], _items[_count - 2]);
    }

    // Delivers the HEAD frame only, and only once the clock has reached its
    // deliver_at (wrap-safe timeReached — no raw comparisons on hub time,
    // per util/serial_arithmetic.hpp). This is intentionally strict
    // head-of-line: a later-queued frame whose deliver_at has *also* elapsed
    // still waits behind the head, which is exactly what "ordered=false"
    // warns callers about.
    std::optional<FrameBuffer> popFront(uint32_t nowUs) {
        if (_count == 0) return std::nullopt;
        if (!timeReached(nowUs, _items[0].deliverAtUs)) return std::nullopt;
        FrameBuffer fb = _items[0].frame;
        for (size_t i = 1; i < _count; ++i) _items[i - 1] = _items[i];
        --_count;
        return fb;
    }

    size_t size() const { return _count; }

private:
    struct Entry {
        FrameBuffer frame{};
        uint32_t deliverAtUs = 0;
    };
    std::array<Entry, kCapacity> _items{};
    size_t _count = 0;
};

inline bool looksLikeEstop(std::span<const std::byte> frame) {
    return frame.size() == kEstopFrameBytes && frame[0] == kEstopMagicByte && frame[1] == kEstopMagicByte &&
           frame[2] == kEstopMagicByte && frame[3] == kEstopMagicByte;
}

// One direction of the link: a FaultProfile, a queue, and the write()/read()
// mechanics that apply the faults. Shares the link's single IClock and
// single IRandom (both directions draw from the SAME rng instance — §13.6's
// determinism guarantee is about the whole link's operation sequence, not
// per-direction independence).
class Direction {
public:
    Direction(IClock& clock, IRandom& rng) : _clock(clock), _rng(rng) {}

    FaultProfile& profile() { return _profile; }
    const FaultProfile& profile() const { return _profile; }

    // Draw order (fixed, documented — this is what makes hand-derived
    // transcripts and the determinism proof possible): loss roll, then
    // jitter roll (only if jitter_us > 0 — skipping the draw when there's no
    // jitter to apply keeps a zero-jitter profile's rng consumption identical
    // to "no roll happened", which is the least surprising behavior), then
    // reorder roll, then dup roll. Every non-early-return write() consumes
    // rolls in this order regardless of the outcomes.
    bool write(std::span<const std::byte> frame) {
        if (frame.size() > _profile.mtu) return false;  // admission gate only — see file header

        const bool isEstop = looksLikeEstop(frame);

        const uint32_t lossRoll = _rng.nextU32() % 100;
        const bool lost = lossRoll < _profile.loss_pct;

        int32_t jitter = 0;
        if (_profile.jitter_us > 0) {
            const uint32_t span = 2u * _profile.jitter_us + 1u;
            const uint32_t roll = _rng.nextU32() % span;
            jitter = int32_t(roll) - int32_t(_profile.jitter_us);
        }

        const uint32_t reorderRoll = _rng.nextU32() % 100;
        const bool reorder = reorderRoll < _profile.reorder_pct;

        const uint32_t dupRoll = _rng.nextU32() % 100;
        const bool dup = dupRoll < _profile.dup_pct;

        if (lost) return true;  // accepted for "transmission"; the simulated network ate it — write()
                                 // reports admission, not delivery, same honesty rule as every real binding (§10.3).

        const uint32_t deliverAt = _clock.nowUs() + _profile.latency_us + uint32_t(jitter);
        FrameBuffer fb = FrameBuffer::from(frame);

        const bool queued = isEstop ? _queue.push_front(fb, deliverAt) : _queue.push_back(fb, deliverAt);
        if (!queued) return false;  // ring full

        if (!isEstop && reorder) _queue.swapLastTwo();

        if (dup) {
            // Best-effort: if the ring is full for the duplicate, it's
            // silently dropped — the ORIGINAL already succeeded, and that's
            // what write()'s return value reported.
            if (isEstop) {
                _queue.push_front(fb, deliverAt);
            } else {
                _queue.push_back(fb, deliverAt);
            }
        }

        return true;
    }

    std::optional<FrameBuffer> read() { return _queue.popFront(_clock.nowUs()); }

    TransportProperties properties() const {
        TransportProperties p;
        p.mtu = _profile.mtu;
        p.ordered = !(_profile.reorder_pct > 0 || _profile.jitter_us > 0);
        p.reliable = (_profile.loss_pct == 0 && _profile.dup_pct == 0);
        p.congestion = CongestionSignal::Simulated;
        return p;
    }

private:
    IClock& _clock;
    IRandom& _rng;
    FaultProfile _profile{};
    FrameQueue _queue{};
};

}  // namespace detail

// InProcessLink: owns both directions of a point-to-point pipe and hands out
// the two ITransport endpoints that ride them. All randomness and all time
// come from the caller-injected IRandom/IClock (SPEC §13.6, §17.2) — a
// XorShift32 seed plus a ManualClock's advance script is the whole
// determinism story; this class adds no randomness or timekeeping of its
// own.
//
// Non-copyable/non-movable: the Endpoint members hold references into this
// object's Direction members, so relocating an InProcessLink would leave
// those references pointing at the old (possibly destroyed) storage. It's
// a link, not a value — construct it once where it lives.
class InProcessLink {
public:
    InProcessLink(IClock& clock, IRandom& rng) : _aToB(clock, rng), _bToA(clock, rng) {}

    InProcessLink(const InProcessLink&) = delete;
    InProcessLink& operator=(const InProcessLink&) = delete;
    InProcessLink(InProcessLink&&) = delete;
    InProcessLink& operator=(InProcessLink&&) = delete;

    ITransport& endpointA() { return _endpointA; }
    ITransport& endpointB() { return _endpointB; }

    // The fault profile governing traffic FROM the named endpoint TO the
    // other one (i.e. profileA() shapes what B observes arriving from A).
    FaultProfile& profileA() { return _aToB.profile(); }
    FaultProfile& profileB() { return _bToA.profile(); }

private:
    class Endpoint final : public ITransport {
    public:
        Endpoint(detail::Direction& outbound, detail::Direction& inbound) : _out(outbound), _in(inbound) {}

        bool open() override {
            _open = true;
            return true;
        }
        void close() override { _open = false; }
        bool write(std::span<const std::byte> frame) override { return _open && _out.write(frame); }
        std::optional<FrameBuffer> read() override { return _open ? _in.read() : std::nullopt; }
        TransportProperties properties() const override { return _out.properties(); }

    private:
        bool _open = false;
        detail::Direction& _out;
        detail::Direction& _in;
    };

    detail::Direction _aToB;
    detail::Direction _bToA;
    Endpoint _endpointA{_aToB, _bToA};
    Endpoint _endpointB{_bToA, _aToB};
};

}  // namespace slopsync
