// slopsync-core — session model: states (SPEC §2.2), identity (§6.1), and the
// hub-side per-session record. Pure data + tiny helpers; the behavior lives
// in hub/hub.hpp and client/client.hpp, which cite these states normatively.
#pragma once

#include <array>
#include <cstdint>
#include <new>  // placement new (HubSession::reset)

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

    // ---- §6.2/§9.2/§10.5: granted inbound-STREAM (c2h motion input) publishes.
    // A session may only send STREAM bundles on channels granted here (at
    // HELLO/WELCOME). Grants are truth exactly like `subs` is for h2c pushes;
    // there is no mid-session PUBLISH frame in v1 (a producer that wants a new
    // publish reconnects — same posture as subscriptions before SUBSCRIBE
    // existed). Each entry carries its own sample-rate token bucket (§10.5).
    static constexpr size_t kMaxPublishGrants = 8;  // matches kHelloMaxPublishWishes (wire/messages/hello.hpp)
    struct PublishGrant {
        bool used = false;
        uint16_t channel_id = 0;
        float granted_rate_hz = 0.0f;         // ceiling, samples/s (§6.2 clamp)
        // Direct-list default member initializer (NOT bare `IngressRateLimiter
        // limiter;`): PublishGrant is an aggregate, so `publishGrants{}` copy-
        // initializes each element from `{}`, which would copy-initialize this
        // member — and IngressRateLimiter's ctor is `explicit`. A default member
        // initializer supplies the value directly (explicit is fine in direct-
        // init). The rate here is a placeholder; addPublishGrant() overwrites
        // the whole limiter with the granted sample rate before any bundle.
        IngressRateLimiter limiter{limits::intent_ingress_default_per_s};  // token bucket on SAMPLES/s (§10.5)
        bool everNackedOverage = false;       // throttle state for RATE_LIMITED (§10.5)
        uint32_t lastOverageNackMs = 0;
    };
    std::array<PublishGrant, kMaxPublishGrants> publishGrants{};
    uint32_t streamBundlesAccepted = 0;                        // §16.2 ingress telemetry
    uint32_t streamBundlesDropped = 0;                         // ungranted/malformed/over-rate/conflict

    // Live granted publish record for `channel_id`, or nullptr (never granted).
    PublishGrant* publishGrantFor(uint16_t channel_id) {
        for (auto& pg : publishGrants) {
            if (pg.used && pg.channel_id == channel_id) return &pg;
        }
        return nullptr;
    }
    // Records (or overwrites) a granted publish. `nowMs` seeds the bucket so a
    // fresh grant is burst-ready from its first bundle (like the intent
    // limiter). Returns false (unmodified) only when the fixed table is full
    // AND `channel_id` is new — the caller then simply omits the grant.
    bool addPublishGrant(uint16_t channel_id, float granted_rate_hz, uint32_t nowMs) {
        uint32_t ratePerSec = uint32_t(granted_rate_hz < 1.0f ? 1.0f : granted_rate_hz);
        if (PublishGrant* existing = publishGrantFor(channel_id)) {
            existing->granted_rate_hz = granted_rate_hz;
            existing->limiter = IngressRateLimiter(ratePerSec, nowMs);
            existing->everNackedOverage = false;
            existing->lastOverageNackMs = 0;
            return true;
        }
        for (auto& pg : publishGrants) {
            if (pg.used) continue;
            pg.used = true;
            pg.channel_id = channel_id;
            pg.granted_rate_hz = granted_rate_hz;
            pg.limiter = IngressRateLimiter(ratePerSec, nowMs);
            pg.everNackedOverage = false;
            pg.lastOverageNackMs = 0;
            return true;
        }
        return false;
    }

    uint32_t lastRxMs = 0;                                     // liveness (§6.5): ANY frame refreshes
    uint32_t lastTxMs = 0;                                     // idle-PING scheduling (§6.5)
    uint16_t retainedPending = 0;                              // remaining retained pushes after WELCOME

    // In-place destroy + reconstruct, NOT `*this = HubSession()`: the
    // assignment form materializes a whole-object TEMPORARY on the stack —
    // ~9 KB for this struct — which blew the hub task's stack canary on
    // target (panic decoded to exactly this line). Host tests never noticed
    // (megabyte stacks). Same semantics, zero stack cost.
    void reset() {
        this->~HubSession();
        new (this) HubSession();
    }
    bool occupied() const { return state != HubSessionState::FREE && state != HubSessionState::CLOSED; }
};

}  // namespace slopsync
