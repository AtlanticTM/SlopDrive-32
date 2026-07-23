// slopsync-core — per-session subscription/grant table, SPEC §10.2 (grants
// as truth) and §9.1 (STATE push pacing/conflation).
//
// A SubscriptionTable is a session's set of ACTIVE grants: what the hub told
// this client it would deliver, at what ceiling rate, at what priority. It
// is the thing SUBSCRIBE/GRANT (wire/messages/subscribe.hpp, grant.hpp)
// negotiate INTO — this file has no wire dependency, no knowledge of the
// session engine that will call it, and does not send anything itself.
//
// ---- Conflation with zero queues (design note, ties together with
// retained_store.hpp) --------------------------------------------------------
//
// SPEC §9.1 describes conflation as "at most a depth-1 queue per (channel,
// subscriber) — a newer snapshot replaces a queued unsent one". This file
// does NOT implement a queue at all, depth-1 or otherwise, and does not
// store any channel VALUE — only the pacing decision. The value lives
// exactly once, hub-side, in retained_store.hpp's RetainedStore (one entry
// per STATE channel, not per subscriber). A subscriber "not yet due" simply
// means: when it next becomes due, it reads whatever RetainedStore::get()
// currently holds — which, by construction, is always the LATEST published
// value (RetainedStore has no history either). So "a newer snapshot
// replaces a queued unsent one" is true by the absence of a queue to be
// stale in, rather than by an explicit replace-if-present operation: there
// is nothing else it COULD read. retained store (single latest value) +
// per-subscriber pacing (this file) together give you depth-1 conflation
// with no queue anywhere in the picture.
//
// dueForPush()'s "changePending" input is owned by the caller (e.g. "does
// RetainedStore's seq for this channel differ from the seq I last pushed to
// this subscriber" — a comparison this file deliberately doesn't make itself
// since it doesn't track per-subscriber last-seen seq; that bookkeeping is
// the session engine's, one layer up).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/util/serial_arithmetic.hpp"

namespace slopsync {

// One channel's active grant + STATE push pacing state.
struct SubscriptionEntry {
    uint16_t channel_id = 0;
    float granted_rate_hz = 0.0f;   // ceiling, §10.2; 0 = on-change only (§9.1)
    Priority priority = Priority::normal;

    // Pacing state. `everPushed` false means "granted but nothing has been
    // pushed to it yet" — which dueForPush() treats as unconditionally due,
    // implementing §9.1's "the hub ... MUST push [the retained value]
    // immediately upon grant (connect, re-subscribe, reconnect)": every
    // fresh upsert() (new subscription OR a re-SUBSCRIBE replacing an
    // existing one, §6.6) resets this to false specifically so the very
    // next dueForPush() check fires regardless of `changePending`.
    bool everPushed = false;
    uint32_t lastPushMs = 0;  // valid only when everPushed

    // §9.1 rate-ceiling + on-change semantics:
    //   - not yet pushed since grant -> due now (push-on-grant, see above).
    //   - no change pending -> never due (nothing new to conflate/send).
    //   - rate_hz == 0 (on-change only) -> due immediately once changed.
    //   - rate_hz > 0 -> due at most once per (1000/rate_hz) ms: periodic
    //     channels push at min(grant, change rate) per §9.1.
    // Time comparison goes through util/serial_arithmetic.hpp's timeReached
    // (wrap-safe hub-ms, §7.2) — no inline `now >= deadline` here.
    bool dueForPush(uint32_t nowMs, bool changePending) const {
        if (!everPushed) return true;
        if (!changePending) return false;
        if (granted_rate_hz <= 0.0f) return true;
        uint32_t periodMs = uint32_t(1000.0f / granted_rate_hz);
        return timeReached(nowMs, lastPushMs + periodMs);
    }

    // Caller calls this immediately after actually sending a push (whether
    // that push was the on-grant retained value or a periodic due push).
    void markPushed(uint32_t nowMs) {
        everPushed = true;
        lastPushMs = nowMs;
    }
};

// Fixed-capacity per-session grant table (§10.2), capacity
// limits::max_subscriptions_per_session (64, §6.6's session-wide ceiling —
// distinct from any single SUBSCRIBE frame's own wish-count cap,
// wire/messages/subscribe.hpp's kSubscribeMaxWishes).
template <size_t Capacity = limits::max_subscriptions_per_session>
class SubscriptionTable {
public:
    static constexpr size_t kCapacity = Capacity;

    // Re-SUBSCRIBE replaces (§6.6): if `channel_id` already has an entry,
    // its rate/priority are overwritten in place (table size unchanged) and
    // its pacing is reset to "due now" (see SubscriptionEntry::everPushed).
    // A brand-new channel_id is appended. Returns false (table unmodified)
    // iff this would be a new entry and the table is already at capacity —
    // the caller's cue to NACK SUB_LIMIT (§16.1's NackCode::SUB_LIMIT).
    bool upsert(uint16_t channel_id, float granted_rate_hz, Priority priority) {
        if (SubscriptionEntry* e = find(channel_id)) {
            e->granted_rate_hz = granted_rate_hz;
            e->priority = priority;
            e->everPushed = false;  // force push-on-(re)grant, §9.1
            e->lastPushMs = 0;
            return true;
        }
        if (_count >= Capacity) return false;
        SubscriptionEntry& e = _entries[_count++];
        e = SubscriptionEntry{};
        e.channel_id = channel_id;
        e.granted_rate_hz = granted_rate_hz;
        e.priority = priority;
        return true;
    }

    // Removes `channel_id` if present (UNSUBSCRIBE, §6.6). Returns whether
    // an entry was removed. Compacts the array (shifts later entries down)
    // so size()/iteration always cover exactly [0, size()).
    bool remove(uint16_t channel_id) {
        for (size_t i = 0; i < _count; ++i) {
            if (_entries[i].channel_id == channel_id) {
                for (size_t j = i + 1; j < _count; ++j) _entries[j - 1] = _entries[j];
                --_count;
                return true;
            }
        }
        return false;
    }

    SubscriptionEntry* find(uint16_t channel_id) {
        for (size_t i = 0; i < _count; ++i) {
            if (_entries[i].channel_id == channel_id) return &_entries[i];
        }
        return nullptr;
    }
    const SubscriptionEntry* find(uint16_t channel_id) const {
        for (size_t i = 0; i < _count; ++i) {
            if (_entries[i].channel_id == channel_id) return &_entries[i];
        }
        return nullptr;
    }

    size_t size() const { return _count; }

    // Iteration support (SPEC-driven consumers: e.g. shedding per §10.4 walks
    // every entry ordered by priority; that ordering is the caller's job,
    // this table just exposes the live [0, size()) span).
    SubscriptionEntry* begin() { return _entries.data(); }
    SubscriptionEntry* end() { return _entries.data() + _count; }
    const SubscriptionEntry* begin() const { return _entries.data(); }
    const SubscriptionEntry* end() const { return _entries.data() + _count; }

private:
    std::array<SubscriptionEntry, Capacity> _entries{};
    size_t _count = 0;
};

}  // namespace slopsync
