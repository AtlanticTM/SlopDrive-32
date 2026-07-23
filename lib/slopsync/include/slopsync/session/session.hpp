// slopsync-core — session model: states (SPEC §2.2), identity (§6.1), and the
// hub-side per-session record. Pure data + tiny helpers; the behavior lives
// in hub/hub.hpp and client/client.hpp, which cite these states normatively.
#pragma once

#include <array>
#include <cstdint>

#include "slopsync/channel/event_channel.hpp"
#include "slopsync/channel/intent_registry.hpp"
#include "slopsync/channel/subscription.hpp"
#include "slopsync/generated/registry_constants.hpp"

namespace slopsync {

// Client-side session state machine (SPEC §2.2). A client MUST NOT act on
// user input needing hub state before LIVE, and MUST render SYNCING
// distinctly (stale-shown-as-fresh violates §1.2-1).
enum class ClientSessionState : uint8_t {
    CLOSED, CONNECTING, HELLO_SENT, SYNCING, LIVE
};

// Hub-side per-session state (SPEC §2.2): VALIDATING is bounded (2 s
// recommended); GRANTED = WELCOME sent, retained pushes streaming; LIVE
// after the client is presumed synced (hub-side this is bookkeeping only —
// no hub behavior gates on the client reaching LIVE).
enum class HubSessionState : uint8_t {
    FREE, VALIDATING, GRANTED, LIVE, CLOSED
};

struct ClientIdentity {
    std::array<std::byte, limits::instance_id_bytes> instance_id{};  // §6.1: durable, client-generated
    std::array<std::byte, limits::token_bytes> token{};              // §12.2; all-zero = no token (viewer)
    bool hasToken = false;
    const char* client_kind = "generic";   // §6.2 (tstr ≤16)
    const char* client_name = "unnamed";   // §6.2 (tstr ≤32)
};

// Hub-side record for one attached transport's session. Fixed-size, reused
// across sessions (reset() between occupants). One transport slot = at most
// one session (point-to-point bindings; multi-client = multiple transports).
struct HubSession {
    HubSessionState state = HubSessionState::FREE;
    uint32_t session_id = 0;                                   // §6.1: random non-zero per boot
    std::array<std::byte, limits::instance_id_bytes> instance_id{};
    AccessLevel role = AccessLevel::viewer;
    uint16_t helloSeenCfgGen = 0;
    bool clientEtagMatched = false;                            // §6.7: etag-skip decision

    SubscriptionTable<> subs;                                  // grants = truth (§10.2)
    IntentRing<> intentRing;                                   // §9.3 idempotency
    IngressRateLimiter intentLimiter;                          // §9.3 / §10.5
    EventQueue<> events;                                       // §9.4 bounded, drop-oldest

    uint32_t lastRxMs = 0;                                     // liveness (§6.5): ANY frame refreshes
    uint32_t lastTxMs = 0;                                     // idle-PING scheduling (§6.5)
    uint16_t retainedPending = 0;                              // remaining retained pushes after WELCOME

    void reset() { *this = HubSession(); }  // parens, not braces: IngressRateLimiter's ctor is explicit
    bool occupied() const { return state != HubSessionState::FREE && state != HubSessionState::CLOSED; }
};

}  // namespace slopsync
