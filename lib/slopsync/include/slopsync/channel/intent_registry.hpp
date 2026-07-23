// slopsync-core — per-session INTENT idempotency ring + ingress rate
// limiter, SPEC §9.3 ("Idempotency", "Rate limiting").
//
// Both pieces are session-scoped, pure, and hold no knowledge of the wire
// framing or the session engine that will own them (M4 scope: build the
// building blocks; Hub/Client wiring is a later milestone). Nothing here
// calls a transport, touches a catalog, or knows what an INTENT's `value`
// means — it only tracks "have I seen this id" and "how many per second".
//
// ---- IntentRing: exact-match idempotency (§9.3) ---------------------------
//
// "The hub keeps a ring of the last 32 (id -> ECHO) per session; a duplicate
// id re-emits the stored ECHO and MUST NOT re-apply." Client ids are
// session-scoped and client-assigned monotonically increasing (§9.3), so the
// ring never needs to know the current id — it just remembers the last
// `idempotency_ring_depth` (32) stores, oldest evicted first, and answers
// "do I have bytes for this exact id" by exact match. No hashing, no
// ordering assumptions beyond "store() is called at most once per id" (true
// by construction: a session only ever stores an id when it first computes
// and applies that intent — every subsequent sight of the same id is a
// duplicate that goes through lookup(), never store() again).
//
// Slot capacity: kSlotCapacity = 96 bytes. Rationale: an encoded EchoMsg
// (wire/messages/echo.hpp) is `mapHeader(3) + cfg_gen + intent_id + applied`.
// The fixed part (map head + two u16 keys/values) costs at most 1+1+3+1+3 =
// 9 bytes; `applied` is `key(1) + mapHeader(1) + up to kIntentMaxValueFields
// (8) IntentValueField entries`, each at most `key(<=2B, uint8 sub-key) +
// value`. 96 bytes sizes for the *realistic* shape of a control-plane
// intent's applied-value map — a handful (think: 1-4) of small scalars
// (u8/u16/f32-sized set-points, the "set speed 400" shape §9.3 mandates for
// every intent schema), NOT the pathological case of all 8 fields being
// maximum-width u64/i64 (9-byte CBOR payload each) or carrying bstr/tstr
// blobs. That pathological case does not fit in 96 bytes and is expected
// to: a catalog whose INTENT schemas produce ECHOes that large is not
// respecting the spirit of "absolute values only" (§9.3) any more than a
// STATE layout that didn't fit `min_transport_payload` would respect §9.1 —
// both are catalog-design-time constraints this library enforces mechanically
// at the boundary rather than silently accepting. store() below REJECTS
// (returns false, no mutation) an echo that doesn't fit; it is the caller's
// (hub's) job to treat that as a catalog-conformance bug, not something to
// paper over with truncation.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/util/serial_arithmetic.hpp"

namespace slopsync {

template <size_t Depth = limits::idempotency_ring_depth, size_t SlotCapacity = 96>
class IntentRing {
public:
    static constexpr size_t kDepth = Depth;
    static constexpr size_t kSlotCapacity = SlotCapacity;

    // Stores `echoBytes` under `intent_id`, always writing into the next
    // ring slot and evicting whatever id occupied it (the oldest store once
    // the ring is full — a plain circular buffer, §9.3's "ring of the last
    // 32"). Precondition callers must honor: store() is called at most once
    // per unique intent_id (see file header) — it does not search for an
    // existing slot to update in place.
    //
    // Returns false (ring unmodified) iff `echoBytes.size() > kSlotCapacity`
    // — see file header; this is a catalog-conformance rejection, not a
    // capacity-eviction.
    bool store(uint16_t intent_id, std::span<const std::byte> echoBytes) {
        if (echoBytes.size() > SlotCapacity) return false;

        Slot& s = _slots[_head];
        s.used = true;
        s.intent_id = intent_id;
        std::memcpy(s.bytes.data(), echoBytes.data(), echoBytes.size());
        s.size = uint16_t(echoBytes.size());

        _head = (_head + 1) % Depth;
        if (_count < Depth) ++_count;
        return true;
    }

    // Exact-id lookup (§9.3 "duplicate id"): the stored ECHO bytes if
    // `intent_id` is still resident in the ring, nullopt if it never was or
    // has since been evicted (32 newer ids stored after it — per the
    // client-monotonic-id assumption, that only happens to a genuinely old
    // id, never to one still worth re-emitting).
    std::optional<std::span<const std::byte>> lookup(uint16_t intent_id) const {
        for (size_t i = 0; i < _count; ++i) {
            const Slot& s = _slots[i];
            if (s.used && s.intent_id == intent_id) {
                return std::span<const std::byte>(s.bytes.data(), s.size);
            }
        }
        return std::nullopt;
    }

    // Session end (§6.7: "the ring dies with the session").
    void clear() {
        for (auto& s : _slots) s.used = false;
        _head = 0;
        _count = 0;
    }

    size_t size() const { return _count; }

private:
    struct Slot {
        bool used = false;
        uint16_t intent_id = 0;
        std::array<std::byte, SlotCapacity> bytes{};
        uint16_t size = 0;
    };

    std::array<Slot, Depth> _slots{};
    size_t _head = 0;   // next slot store() will (over)write
    size_t _count = 0;  // slots ever used, saturating at Depth
};

// ---- IngressRateLimiter: per-session intent ingress cap (§9.3, §10.5) -----
//
// "Rate limiting: hub-enforced per session (NACK RATE_LIMITED); Appendix G
// default 50 intents/s." Algorithm: continuous-refill TOKEN BUCKET, not a
// fixed 1-second window. Chosen over a sliding/fixed window because a fixed
// window has the classic edge-of-window doubling flaw (up to 2x the nominal
// rate can land across a window boundary); a token bucket has no such edge
// and its two knobs map directly onto the two things §9.3/§10.5 actually
// specify: sustained rate (tokens refilled per second == `ratePerSec`) and
// burst tolerance ("Appendix G default 50/s — generous for UIs" reads as
// "a UI may fire a burst of up to a second's worth at once", which is
// exactly bucket CAPACITY == ratePerSec, i.e. one second of headroom, no
// more).
//
// The bucket starts FULL (burst-ready from the first intent a session ever
// sends — a fresh session should not have to "wait a second" before its
// first burst is allowed) and refills continuously, capped at `ratePerSec`
// tokens, based on elapsed wall time between allow() calls. All elapsed-time
// math goes through util/serial_arithmetic.hpp's timeDelta (wrap-safe hub-ms
// per SPEC §7.2) — no inline `now - last` anywhere in this file.
class IngressRateLimiter {
public:
    explicit IngressRateLimiter(uint32_t ratePerSec = limits::intent_ingress_default_per_s,
                                 uint32_t nowMs = 0)
        : _ratePerSec(ratePerSec), _tokens(float(ratePerSec)), _lastMs(nowMs) {}

    // Call once per ingress intent, at its arrival time. Returns true (and
    // consumes one token) if under the rate; false (session should be
    // NACKed RATE_LIMITED, §10.5) if the bucket is empty.
    bool allow(uint32_t nowMs) {
        refill(nowMs);
        if (_tokens >= 1.0f) {
            _tokens -= 1.0f;
            return true;
        }
        return false;
    }

    float tokens() const { return _tokens; }
    uint32_t ratePerSec() const { return _ratePerSec; }

private:
    void refill(uint32_t nowMs) {
        int32_t deltaMs = timeDelta(nowMs, _lastMs);  // §7.2 wrap-safe
        if (deltaMs <= 0) return;  // no time elapsed (or clock went backward within the wrap window): no refill, no update
        _lastMs = nowMs;
        float capacity = float(_ratePerSec);
        float added = (float(deltaMs) / 1000.0f) * capacity;
        _tokens += added;
        if (_tokens > capacity) _tokens = capacity;  // burst cap: one second's worth (§9.3/§10.5)
    }

    uint32_t _ratePerSec;
    float _tokens;
    uint32_t _lastMs;
};

}  // namespace slopsync
