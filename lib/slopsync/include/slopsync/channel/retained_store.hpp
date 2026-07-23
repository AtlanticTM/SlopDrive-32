// slopsync-core — hub-side retained STATE values, SPEC §9.1 (the device
// shadow's source of truth).
//
// "The hub keeps the latest value of every STATE channel and MUST push it
// immediately upon grant (connect, re-subscribe, reconnect). This is the
// device-shadow primitive; it is what 'page load adopts device state'
// compiles to."
//
// RetainedStore holds exactly ONE entry per STATE channel — never per
// subscriber — because the retained value is a property of the CHANNEL
// (what is currently true), not of who is watching it. See
// channel/subscription.hpp's design note for how this pairs with
// SubscriptionTable's per-subscriber pacing to produce §9.1's depth-1
// conflation without either side ever queueing a value: the store holds
// only the latest snapshot (a `publish()` overwrites, it never appends),
// and a subscriber's dueForPush() decides only WHEN to read it, never
// WHICH one.
//
// Payload cap: limits::min_transport_payload (242 bytes) — the same §9.1
// "STATE payload MUST fit min_transport_payload unfragmented" MTU rule
// channel/catalog.hpp's CatalogEntry::layoutWireSize() checks at
// catalog-design time. publish() enforces it again here as the runtime
// backstop: a conformant catalog's STATE layouts never produce a payload
// this large, so hitting this rejection at runtime means a non-conformant
// catalog or a caller bug, not a normal operating condition.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "slopsync/generated/registry_constants.hpp"

namespace slopsync {

// One STATE channel's retained value, as read via RetainedStore::get().
struct RetainedView {
    uint16_t seq;
    std::span<const std::byte> payload;
};

// Fixed-capacity retained-value map, up to 32 STATE channels (a hub with
// more distinct STATE channels than that sizes its own RetainedStore<N>
// instance accordingly — 32 is this library's default, not a hard wire
// limit).
template <size_t Capacity = 32>
class RetainedStore {
public:
    static constexpr size_t kCapacity = Capacity;

    // Publishes a new full-snapshot value for `channel_id` (§9.1
    // full-snapshot rule: `payload` is the COMPLETE current value, never a
    // delta). Bumps and returns the channel's seq (hub-incremented per
    // publish, starts at 1 on a channel's first publish — §7.3's seq
    // comparisons treat 0 as just another valid predecessor, so there is no
    // reserved "unpublished" seq value to collide with; `valid`/get()'s
    // nullopt is what distinguishes "never published" from "published with
    // seq 0", not the seq value itself... except this store starts new
    // channels at seq 0 and increments on every publish including the
    // first, so the first published seq is 1, never 0).
    //
    // Returns nullopt (store unmodified) iff `payload.size() >
    // limits::min_transport_payload` (§9.1 MTU rule), or `channel_id` is new
    // and the store is already at Capacity.
    std::optional<uint16_t> publish(uint16_t channel_id, std::span<const std::byte> payload) {
        if (payload.size() > limits::min_transport_payload) return std::nullopt;

        Entry* e = find(channel_id);
        if (!e) {
            if (_count >= Capacity) return std::nullopt;
            e = &_entries[_count++];
            *e = Entry{};
            e->channel_id = channel_id;
        }

        e->seq = uint16_t(e->seq + 1);  // wraps at 0xFFFF -> 0x0000 intentionally, §7.3
        std::memcpy(e->payload.data(), payload.data(), payload.size());
        e->size = uint16_t(payload.size());
        e->valid = true;
        return e->seq;
    }

    // The current retained value, nullopt if `channel_id` has never been
    // published (distinct from "published once with an empty payload",
    // which returns a valid View with payload.size() == 0).
    std::optional<RetainedView> get(uint16_t channel_id) const {
        const Entry* e = find(channel_id);
        if (!e || !e->valid) return std::nullopt;
        return RetainedView{e->seq, std::span<const std::byte>(e->payload.data(), e->size)};
    }

    size_t size() const { return _count; }

private:
    struct Entry {
        uint16_t channel_id = 0;
        bool valid = false;
        uint16_t seq = 0;
        std::array<std::byte, limits::min_transport_payload> payload{};
        uint16_t size = 0;
    };

    Entry* find(uint16_t channel_id) {
        for (size_t i = 0; i < _count; ++i) {
            if (_entries[i].channel_id == channel_id) return &_entries[i];
        }
        return nullptr;
    }
    const Entry* find(uint16_t channel_id) const {
        for (size_t i = 0; i < _count; ++i) {
            if (_entries[i].channel_id == channel_id) return &_entries[i];
        }
        return nullptr;
    }

    std::array<Entry, Capacity> _entries{};
    size_t _count = 0;
};

}  // namespace slopsync
