// slopsync-core — safety coordination, SPEC §11.
//
// The library is machine-agnostic: "control sources" are application-defined
// small ids (the firmware maps them to MotionArbiter sources MANUAL/TCODE/
// PATTERN/OSSM). The hub owns WHO controls each source (§11.4 exclusive
// ownership + takeover) and WHEN silence kills it (§11.3 deadman, policy per
// source type); the application owns WHAT stopping means (delegate hooks).
//
// Stop taxonomy (§11.1) on the wire: the safety channel's (0x0003-shaped)
// word bitfield — bit0 ESTOP, bit1 STOP, bit2 HOLD, bit3 PAUSE. The hub
// latches ESTOP (§11.2) and STOP-on-deadman itself; HOLD/PAUSE are
// application-published via the normal publishState path.
#pragma once

#include <array>
#include <cstdint>

#include "slopsync/generated/registry_constants.hpp"

namespace slopsync {

// §11.3: what happens to a source when its owning session goes silent/away.
enum class SourceLossPolicy : uint8_t {
    Stop = 0,      // initiator-bound (streams, live control): motion stops
    Continue = 1,  // hub-autonomous (pattern): keeps running, ownership released
};

// Safety word bits (mirrors the safety channel layout's `word` bitfield).
namespace safety_bits {
inline constexpr uint8_t ESTOP = 1u << 0;
inline constexpr uint8_t STOP  = 1u << 1;
inline constexpr uint8_t HOLD  = 1u << 2;
inline constexpr uint8_t PAUSE = 1u << 3;
}  // namespace safety_bits

// §11.4: exclusive per-source ownership with role-gated takeover. Pure state
// machine — no I/O; the Hub drives it and publishes control-owner STATE +
// takeover EVENTs on transitions.
class SourceOwnershipTable {
public:
    static constexpr size_t kMaxSources = 4;

    enum class AcquireResult : uint8_t { Acquired, AlreadyOwner, Conflict, TakenOver };

    // First authorized session to activate owns it (§11.4). `takeover` set:
    // transfer succeeds iff requesterRole >= ownerRole. Roles recorded at
    // acquisition time.
    AcquireResult acquire(uint8_t source_id, uint32_t session_id, AccessLevel role, bool takeover);

    // Release on GOODBYE/eviction/deadman/explicit intent (§11.4). Returns
    // true if the session owned it.
    bool release(uint8_t source_id, uint32_t session_id);
    // Release EVERYTHING a dying session owns; invokes cb(source_id) per hit.
    template <typename Cb> void releaseAllOf(uint32_t session_id, Cb&& cb);

    uint32_t ownerOf(uint8_t source_id) const;      // 0 = unowned
    AccessLevel ownerRoleOf(uint8_t source_id) const;

private:
    struct Entry { uint8_t source_id = 0; uint32_t owner = 0; AccessLevel role = AccessLevel::viewer; bool used = false; };
    std::array<Entry, kMaxSources> _entries{};
    Entry* find(uint8_t source_id);
    const Entry* find(uint8_t source_id) const;
    Entry* findOrCreate(uint8_t source_id);
};

// ============================================================================
// SourceOwnershipTable — method bodies (§11.4). Pure state machine, no I/O;
// see the class doc comment above for the exact contract each method
// implements (frozen). Defined inline here (no companion _impl file exists
// for this header, unlike Hub/Client).
// ============================================================================

inline SourceOwnershipTable::Entry* SourceOwnershipTable::find(uint8_t source_id) {
    for (auto& e : _entries) {
        if (e.used && e.source_id == source_id) return &e;
    }
    return nullptr;
}

inline const SourceOwnershipTable::Entry* SourceOwnershipTable::find(uint8_t source_id) const {
    for (const auto& e : _entries) {
        if (e.used && e.source_id == source_id) return &e;
    }
    return nullptr;
}

inline SourceOwnershipTable::Entry* SourceOwnershipTable::findOrCreate(uint8_t source_id) {
    if (Entry* e = find(source_id)) return e;
    for (auto& e : _entries) {
        if (!e.used) {
            e = Entry{};
            e.used = true;
            e.source_id = source_id;
            return &e;
        }
    }
    return nullptr;  // kMaxSources distinct source ids already in use — an app/catalog design limit, not a runtime fault
}

inline SourceOwnershipTable::AcquireResult SourceOwnershipTable::acquire(uint8_t source_id, uint32_t session_id,
                                                                          AccessLevel role, bool takeover) {
    Entry* e = findOrCreate(source_id);
    if (!e) return AcquireResult::Conflict;  // table exhausted: nothing sane to hand out, refuse like a real conflict

    if (e->owner == 0) {
        e->owner = session_id;
        e->role = role;
        return AcquireResult::Acquired;
    }
    if (e->owner == session_id) {
        return AcquireResult::AlreadyOwner;  // idempotent re-activation by the same session
    }
    if (takeover && uint8_t(role) >= uint8_t(e->role)) {
        e->owner = session_id;
        e->role = role;
        return AcquireResult::TakenOver;
    }
    return AcquireResult::Conflict;  // no takeover requested, or requester's role is below the owner's
}

inline bool SourceOwnershipTable::release(uint8_t source_id, uint32_t session_id) {
    Entry* e = find(source_id);
    if (!e || e->owner != session_id) return false;
    e->owner = 0;
    e->role = AccessLevel::viewer;
    return true;
}

template <typename Cb>
inline void SourceOwnershipTable::releaseAllOf(uint32_t session_id, Cb&& cb) {
    for (auto& e : _entries) {
        if (e.used && e.owner == session_id) {
            e.owner = 0;
            e.role = AccessLevel::viewer;
            cb(e.source_id);
        }
    }
}

inline uint32_t SourceOwnershipTable::ownerOf(uint8_t source_id) const {
    const Entry* e = find(source_id);
    return e ? e->owner : 0;
}

inline AccessLevel SourceOwnershipTable::ownerRoleOf(uint8_t source_id) const {
    const Entry* e = find(source_id);
    return e ? e->role : AccessLevel::viewer;
}

}  // namespace slopsync
