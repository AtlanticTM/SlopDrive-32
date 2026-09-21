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
#include "slopsync/util/bounded_string.hpp"

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
// no hub behavior gates on the client reaching LIVE). STALE (RFC-042,
// library-internal — never itself wire-visible) sits between LIVE and CLOSED:
// silence past the deadman/idle-reap window marks a session STALE instead of
// tearing it down. A STALE session's slot, session_id, subs, publishGrants,
// intent ring, and readiness are all RETAINED (occupied() below is still
// true) so the same client resumes without a full HELLO/WELCOME/catalog
// cycle — see Hub::pumpDeadman/pumpIdleReap/handleHello's reattach branch.
enum class HubSessionState : uint8_t {
    FREE, VALIDATING, GRANTED, LIVE, STALE, CLOSED
};

struct ClientIdentity {
    std::array<std::byte, limits::instance_id_bytes> instance_id{};  // §6.1: durable, client-generated
    std::array<std::byte, limits::token_bytes> token{};              // §12.2; all-zero = no token (watch)
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
    AccessLevel role = AccessLevel::watch;
    uint16_t helloSeenCfgGen = 0;
    bool clientEtagMatched = false;                            // §6.7: etag-skip decision

    // ---- M4b (RFC-027/029): the identity a pairing decision is made ABOUT ---
    // HELLO's strings are views into the frame buffer and die with the
    // dispatch, but a pending knock (0x000A) and a ledger entry both have to
    // NAME the device long after that — an operator approving "something with
    // instance id 3f9a..." is being asked to authorize a hex blob. So the
    // session keeps its own truncated copies, at the ledger's caps so nothing
    // appears to change between the roster and the prompt.
    BoundedString<size_t(limits::trust_ledger_kind_max_bytes)> clientKind{};
    BoundedString<size_t(limits::trust_ledger_name_max_bytes)> clientName{};
    BoundedString<size_t(limits::client_ver_max_bytes)> clientVer{};
    bool hasClientVer = false;
    // trust_keys.presentation_mode as the client declared it. Recorded, not
    // enforced: token presentation stays BEARER-only until M4c, and the value
    // exists so the roster can show posture (RFC-029.6) the moment it does.
    uint8_t presentationMode = 0;

    // ---- §8.4/RFC-015 catalog readiness: the dual-plane gate ----------------
    // `ready` is the whole mechanism — ONE flag, zero RAM, never blocks.
    // While false the hub emits NO data-plane frame to this session (no
    // retained push, no STATE, no STREAM) and refuses its INTENTs with
    // NOT_READY; nothing is queued or buffered anywhere, because retained
    // values already live exactly once in the hub's channel table. Set at
    // HELLO when the client's etag MATCHES (proof of possession — the 99%
    // reconnect case keeps its zero-added-latency push), otherwise by a
    // CATALOG_READY (0x19) frame.
    bool ready = false;
    // The client declared a DIFFERENT etag than the hub's (§8.5 degraded
    // operation): still ready — it told us what it operates against, and
    // append-only layouts make its prefix-parse safe — but recorded so a hub
    // can log/expose the session as degraded.
    bool readyEtagMismatch = false;
    // §6.3 grant instant, in hub-ms: the start of the catalog_ready_timeout_ms
    // window. A client that PINGs forever but never READYs is invisible to
    // liveness reaping (it IS alive), so without this it would hold a session
    // slot indefinitely with both planes gated shut.
    uint32_t grantedAtMs = 0;

    SubscriptionTable<> subs;                                  // grants = truth (§10.2)
    IntentRing<> intentRing;                                   // §9.3 idempotency
    IngressRateLimiter intentLimiter;                          // §9.3 / §10.5
    EventQueue<> events;                                       // §9.4 bounded, drop-oldest

    // ---- §6.2/§9.2/§10.5: granted inbound-STREAM (c2h motion input) publishes.
    // A session may only send STREAM bundles on channels granted here — at
    // HELLO/WELCOME, or mid-session via PUBLISH (0x18, RFC-013), which ADDS or
    // REPLACES entries under the same §6.2 validation rules. Grants are truth
    // exactly like `subs` is for h2c pushes. Each entry carries its own sample
    // token bucket (§10.5) whose CAPACITY is the granted burst and whose REFILL
    // RATE is the granted sample rate (RFC-013 decoupled the two).
    static constexpr size_t kMaxPublishGrants = 8;  // matches kHelloMaxPublishWishes (wire/messages/hello.hpp)
    struct PublishGrant {
        bool used = false;
        uint16_t channel_id = 0;
        float granted_rate_hz = 0.0f;         // ceiling, samples/s (§6.2 clamp)
        float granted_burst = 0.0f;           // applied bucket capacity, samples (§10.5/RFC-013)
        bool burstRequested = false;          // the wish asked for one -> echo it back
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
        // RFC-030: the EFFECTIVE curve family granted to this publish (post
        // delegate override), 0 = unspecified. Read back at drain time via
        // Hub::publishCurveFamily() so the segment consumer honors the
        // sender's declared smoothness class.
        uint8_t curveFamily = 0;
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
    // limiter). `granted_burst` is the APPLIED (already hub-clamped) bucket
    // capacity in samples; pass <= 0 for "capacity = rate", the default.
    // `burstRequested` records whether the wish asked at all, which is what
    // decides whether the grant echoes a `burst` key back. Returns false
    // (unmodified) only when the fixed table is full AND `channel_id` is new —
    // the caller then simply omits the grant.
    bool addPublishGrant(uint16_t channel_id, float granted_rate_hz, uint32_t nowMs,
                         float granted_burst = 0.0f, bool burstRequested = false,
                         uint8_t curveFamily = 0) {
        uint32_t ratePerSec = uint32_t(granted_rate_hz < 1.0f ? 1.0f : granted_rate_hz);
        float capacity = granted_burst > 0.0f ? granted_burst : float(ratePerSec);
        PublishGrant* pg = publishGrantFor(channel_id);
        if (pg == nullptr) {
            for (auto& candidate : publishGrants) {
                if (candidate.used) continue;
                pg = &candidate;
                break;
            }
            if (pg == nullptr) return false;  // table full and this is a new channel
            pg->used = true;
            pg->channel_id = channel_id;
        }
        pg->granted_rate_hz = granted_rate_hz;
        pg->granted_burst = capacity;
        pg->burstRequested = burstRequested;
        pg->limiter = IngressRateLimiter(ratePerSec, nowMs, capacity);
        pg->everNackedOverage = false;
        pg->lastOverageNackMs = 0;
        pg->curveFamily = curveFamily;
        return true;
    }

    uint32_t lastRxMs = 0;                                     // liveness (§6.5): ANY frame refreshes
    // RFC-038: the APPLIED per-session deadman window — the HELLO wish clamped
    // into [deadman_min_ms, deadman_max_ms], or the default when no wish.
    // WELCOME key 24 echoes exactly this value; pumpDeadman() enforces it.
    uint32_t deadmanMs = limits::deadman_default_ms;
    uint32_t lastTxMs = 0;                                     // idle-PING scheduling (§6.5)
    uint16_t retainedPending = 0;                              // remaining retained pushes after WELCOME
    // RFC-042: hub-ms this session entered STALE, 0 while not stale. Backs the
    // slot-pressure eviction tie-break (lowest access tier first, then longest
    // continuously stale) — findEvictableStale() in hub_impl.hpp.
    uint32_t staleSinceMs = 0;

    // In-place destroy + reconstruct — never whole-object reassignment:
    // this struct is ~8 KB and the assignment form puts a full temporary on
    // the caller's task stack. Mechanism: SlopDrive-32's TRAPS.md T1.
    void reset() {
        this->~HubSession();
        new (this) HubSession();
    }
    bool occupied() const { return state != HubSessionState::FREE && state != HubSessionState::CLOSED; }
};

}  // namespace slopsync
