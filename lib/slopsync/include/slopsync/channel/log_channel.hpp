// slopsync-core — the spec-core LOG channel (0x0008), RFC-017.
//
// WHY IT EXISTS: the sole-surface doctrine says the machine's ONLY
// communication plane is SlopSync (HTTP keeps static assets, OTA and
// /uitoken, nothing else, ever). `/api/log` was carrying a protocol duty —
// "show me what the machine just did" — and it left every non-browser client
// blind. So the log moves in-band, as an ordinary EVENT channel.
//
// THE ONE SANCTIONED EXCEPTION TO §9.4: events are edges and are never
// replayed on reconnect... except where a channel's catalog entry declares a
// `replay_depth` (entry key 13). The log declares one, because a client that
// connects AFTER a fault must still be able to see what happened — a log you
// can only read if you were already watching is not a log.
//
// SHAPE. `log_events::entry` is the only kind; its fields ride the scoped
// `body` (40) sub-map with the integer keys below, which are this channel's
// OWN catalog schema keys (that is the v1.0 EVENT grammar: a device-authored
// EVENT channel names its own fields and needs no registry PR to do it). The
// keys here are the reference hub's declaration of the spec-core channel, so
// hub and client agree the same way they agree on `log_events` kinds.
//
// HARDWARE-FREE: this header knows nothing about SlopLog, FreeRTOS or serial.
// The firmware bridge (milestone 5) is one call — Hub::publishLog() — made
// from an application-side sloplog::ISink. The library never allocates, never
// blocks and never formats: the bridge hands over an already-formatted line.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/generated/registry_constants.hpp"

namespace slopsync {

// The 0x0008 catalog SCHEMA keys = the `body` sub-map keys of a log EVENT.
namespace log_body {
inline constexpr uint8_t level = 1;    // registry `log_levels` (0 trace .. 5 fatal)
inline constexpr uint8_t tag = 2;      // tstr, the SlopLog-style subsystem tag
inline constexpr uint8_t hub_ms = 3;   // uint, hub-ms when the line was emitted (§7.2)
inline constexpr uint8_t message = 4;  // tstr, <= kLogMessageMaxBytes
}  // namespace log_body

inline constexpr size_t kLogTagMaxBytes = 16;
inline constexpr size_t kLogMessageMaxBytes = 128;  // registry: "message <=128 B"

// Declares the spec-core log channel on `cat`, in the canonical shape above.
// Call it in catalog-id order like any other entry (0x0008 sorts before a
// device's own >=0x0080 allocations). `replayDepth` defaults to the registry's
// own limits::log_replay_depth_default; pass a smaller number on a hub that
// wants a shallower ring, or 0 for a log channel that never replays (legal —
// it just makes the channel §9.4-ordinary).
inline bool addLogChannel(Catalog32& cat, uint16_t replayDepth = uint16_t(limits::log_replay_depth_default)) {
    CatalogEntry* e = cat.addEntry({.id = channels::log,
                                    .name = "log",
                                    .cls = ChannelClass::EVENT,
                                    .dir = Direction::h2c,
                                    // RFC-027 tiers: `watch` — reading the machine's
                                    // log needs no control authority, and a viewer
                                    // that cannot see errors cannot help.
                                    .access = AccessLevel::watch,
                                    .maxRateHz = 0.0f,  // EVENT: edge-driven, no pacing
                                    .defaultPriority = Priority::background,
                                    .hasReplayDepth = replayDepth > 0,
                                    .replayDepth = replayDepth});
    if (e == nullptr) return false;
    cat.addSchemaField({.key = log_body::level, .name = "level", .type = CborFieldType::uint_t, .unit = ""});
    cat.addSchemaField({.key = log_body::tag, .name = "tag", .type = CborFieldType::tstr_t, .unit = ""});
    cat.addSchemaField({.key = log_body::hub_ms, .name = "hub_ms", .type = CborFieldType::uint_t, .unit = "ms"});
    cat.addSchemaField({.key = log_body::message, .name = "message", .type = CborFieldType::tstr_t, .unit = ""});
    return !cat.overflow;
}

// ---------------------------------------------------------------------------
// The replay ring (§9.4's drop-oldest + visible drop counter, plus the
// replay_depth tail).
//
// Stores ALREADY-ENCODED EVENT payloads, exactly like EventQueue does, for two
// reasons: replay is then a memcpy per entry with no re-encode (and no clock
// read that would rewrite a past line's timestamp), and the ring inherits
// EventQueue's slot-size contract for free.
// ---------------------------------------------------------------------------
template <size_t Depth = size_t(limits::log_replay_depth_default), size_t SlotCapacity = 192>
class EventReplayRing {
public:
    static constexpr size_t kDepth = Depth;
    static constexpr size_t kSlotCapacity = SlotCapacity;

    // Appends one encoded EVENT payload. Over-capacity payloads are REJECTED
    // (false, ring unmodified) — a catalog-conformance error, not overflow.
    // A full ring drops its OLDEST entry and counts it (§9.4: visible, never
    // silent) — the normal steady state of any bounded log.
    bool push(std::span<const std::byte> bytes) {
        if (bytes.size() > SlotCapacity) return false;
        if (_count == Depth) {
            _tail = (_tail + 1) % Depth;
            --_count;
            ++_dropped;
        }
        Slot& s = _slots[_head];
        std::memcpy(s.bytes.data(), bytes.data(), bytes.size());
        s.size = uint16_t(bytes.size());
        _head = (_head + 1) % Depth;
        ++_count;
        return true;
    }

    size_t size() const { return _count; }
    // §9.4's visible drop counter — the value a hub wires into hub_status.
    uint32_t dropped() const { return _dropped; }

    // NON-DESTRUCTIVE oldest-first read of the newest `want` entries, which is
    // what replay is: `fn(std::span<const std::byte>)` is invoked in emission
    // order so a joining client sees the tail as it happened, not backwards.
    template <typename Fn>
    size_t forEachNewest(size_t want, Fn&& fn) const {
        size_t n = want < _count ? want : _count;
        size_t start = (_tail + (_count - n)) % Depth;
        for (size_t i = 0; i < n; ++i) {
            const Slot& s = _slots[(start + i) % Depth];
            fn(std::span<const std::byte>(s.bytes.data(), s.size));
        }
        return n;
    }

private:
    struct Slot {
        std::array<std::byte, SlotCapacity> bytes{};
        uint16_t size = 0;
    };
    std::array<Slot, Depth> _slots{};
    size_t _head = 0;
    size_t _tail = 0;
    size_t _count = 0;
    uint32_t _dropped = 0;
};

}  // namespace slopsync
