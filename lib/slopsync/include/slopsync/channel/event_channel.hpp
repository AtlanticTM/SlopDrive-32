// slopsync-core — per-subscriber bounded EVENT queue, SPEC §9.4.
//
// "Best-effort: events are conflated/bounded like everything else and are
// NOT replayed on reconnect." ... "Overflow: per-subscriber event queues are
// bounded (Appendix G); overflow drops oldest and sets an `events_dropped`
// counter in the hub-status channel — visible, never silent."
//
// EventQueue stores already-ENCODED EVENT frame payloads (bytes, as
// wire/messages/event.hpp's encodeEvent() produces them) — it has no
// opinion on their meaning, same "dumb by design" posture as
// channel/state_apply.hpp's ShadowSlot. Wiring `eventsDropped()` into the
// hub_status STATE channel (registry channels::hub_status) is a later
// milestone's job; this class only counts.
//
// Slot capacity: kSlotCapacity = 192 bytes. Rationale, same shape of
// argument as IntentRing's 96B (intent_registry.hpp): an encoded EventMsg is
// `mapHeader(<=5) + channel_id + timestamp + event_kind + [seq_of_state]
// + [body: <=8 IntentValueFields]`. The four/five mandatory scalar keys (all
// u16/u32) cost at most ~20 bytes; the optional body map is the same
// "handful of small scalars" shape ECHO's `applied` is (§9.4's example
// events — anomaly detected, session joined, takeover happened — carry a
// couple of small fields, not a pile of 9-byte u64s or string blobs).
//
// M3b RAISED IT 128 -> 192 for the RFC-017 log channel: a log EVENT carries a
// tstr `message` the registry caps at 128 B, and the mandatory framing plus
// tag/level/hub-ms costs ~40 B on top, so a registry-legal log line does not
// fit 128 and would have been rejected outright by push(). The cost is exactly
// 64 B x Depth(16) = 1 KiB per SESSION (~5 KiB on a 5-slot reference hub) —
// paid where the hub already lives in PSRAM. The alternative was truncating
// log messages to ~90 B, i.e. lying about a registered cap.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "slopsync/generated/registry_constants.hpp"

namespace slopsync {

template <size_t Depth = limits::event_queue_depth_per_subscriber, size_t SlotCapacity = 192>
class EventQueue {
public:
    static constexpr size_t kDepth = Depth;
    static constexpr size_t kSlotCapacity = SlotCapacity;

    // A queued event and the channel it belongs to. The channel_id is stored
    // per entry (not assumed by the drainer) because ONE queue serves ALL of a
    // session's subscribed EVENT channels: a drain that stamped every dequeued
    // event with whichever channel happened to trigger the drain would ship
    // session-events bytes under the log channel's id, and the receiving client
    // would decode them against the wrong catalog schema.
    struct Dequeued {
        uint16_t channel_id = 0;
        std::span<const std::byte> bytes{};
    };

    // Enqueues an encoded EVENT frame. Two distinct failure/adaptation modes:
    //   - `bytes.size() > kSlotCapacity`: rejected outright (returns false,
    //     queue unmodified) — a catalog-conformance violation, not overflow
    //     (see file header).
    //   - queue already holds `Depth` events: the OLDEST queued event is
    //     dropped to make room (§9.4 "overflow drops oldest"), eventsDropped()
    //     increments, and the new event is enqueued (returns true) — this is
    //     the NORMAL, expected outcome of a slow/absent subscriber, not an
    //     error.
    bool push(uint16_t channel_id, std::span<const std::byte> bytes) {
        if (bytes.size() > SlotCapacity) return false;

        if (_count == Depth) {
            _tail = (_tail + 1) % Depth;  // drop oldest
            --_count;
            ++_eventsDropped;
        }

        Slot& s = _slots[_head];
        std::memcpy(s.bytes.data(), bytes.data(), bytes.size());
        s.size = uint16_t(bytes.size());
        s.channel_id = channel_id;
        _head = (_head + 1) % Depth;
        ++_count;
        return true;
    }

    // FIFO pop: returns (and removes) the oldest queued event, nullopt if
    // empty. The returned span views this queue's own slot storage and stays
    // valid until the next push()/pop() call on this queue — consume it
    // before that (e.g. hand straight to a transport write), same convention
    // as IntentRing::lookup().
    std::optional<Dequeued> pop() {
        if (_count == 0) return std::nullopt;
        Slot& s = _slots[_tail];
        Dequeued out{s.channel_id, std::span<const std::byte>(s.bytes.data(), s.size)};
        _tail = (_tail + 1) % Depth;
        --_count;
        return out;
    }

    size_t size() const { return _count; }
    bool empty() const { return _count == 0; }
    bool full() const { return _count == Depth; }

    // Visible, never-silent overflow counter (§9.4) — the value a caller
    // wires into hub_status's `events_dropped` field.
    uint32_t eventsDropped() const { return _eventsDropped; }

private:
    struct Slot {
        std::array<std::byte, SlotCapacity> bytes{};
        uint16_t size = 0;
        uint16_t channel_id = 0;
    };

    std::array<Slot, Depth> _slots{};
    size_t _head = 0;
    size_t _tail = 0;
    size_t _count = 0;
    uint32_t _eventsDropped = 0;
};

}  // namespace slopsync
