// slopsync-core — the HUB role (SPEC §2.2, §3.1, §6, §9, §10). One per
// machine. No threads: the owner calls update() from its loop (firmware:
// a Core-0 task; sim: the test harness). The hub NEVER touches application
// machinery directly — everything application-specific goes through
// HubDelegate, and on the firmware that delegate submits to the
// MotionArbiter (sole-caller doctrine, §3.1: normative).
//
// M4 scope note: liveness bookkeeping and the safety channel's ESTOP latch
// are implemented here (E-04 needs the latch as the acknowledgement, §11.2).
// M5 additions (this file): deadman policy dispatch (§11.3), source
// ownership + takeover (§11.4), congestion-driven shedding + slow-consumer
// eviction (§10.4), pairing (§12.2), and the network probe (§6.4).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/channel/retained_store.hpp"
#include "slopsync/core/clock.hpp"
#include "slopsync/core/result.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/session/pairing.hpp"
#include "slopsync/session/safety.hpp"
#include "slopsync/session/session.hpp"
#include "slopsync/session/shedding.hpp"
#include "slopsync/transport/transport.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/estop_frame.hpp"
#include "slopsync/wire/messages/goodbye.hpp"
#include "slopsync/wire/messages/grant.hpp"
#include "slopsync/wire/messages/intent.hpp"
#include "slopsync/wire/messages/nack.hpp"
#include "slopsync/wire/messages/probe_report.hpp"
#include "slopsync/wire/messages/welcome.hpp"

namespace slopsync {

inline constexpr size_t kHubMaxSessions = 4;  // conformance floor (§6.3, §17.1)

// What the application (firmware / sim model) provides to the hub.
class HubDelegate {
public:
    virtual ~HubDelegate() = default;

    // §12.2: map (instance_id, token) -> role. Return viewer when no/invalid
    // token. M4 default-viewer implementations are fine (pairing lands M5).
    virtual AccessLevel validateToken(std::span<const std::byte> instance_id,
                                      std::span<const std::byte> token, bool hasToken) {
        (void)instance_id; (void)token; (void)hasToken;
        return AccessLevel::viewer;
    }

    // §9.3: apply an intent. Return the APPLIED (post-clamp) values to echo,
    // or a NackCode refusal. The hub handles idempotency, rate limiting,
    // role gating (catalog access level) and cfg_gen BEFORE calling this;
    // the delegate only applies + clamps. Setting `cfgChanged` bumps cfg_gen.
    virtual Result<IntentValueMap, NackCode> applyIntent(uint16_t channel_id,
                                                         const IntentValueMap& requested,
                                                         AccessLevel role,
                                                         bool& cfgChanged) = 0;

    // §11.2 (minimal M4 form): motion must STOP before protocol bookkeeping.
    virtual void onEstop(uint8_t cause, uint8_t origin) = 0;

    // Roster visibility (§16.2) — optional.
    virtual void onSessionJoined(uint32_t session_id) { (void)session_id; }
    virtual void onSessionLeft(uint32_t session_id) { (void)session_id; }

    // ---- M5 safety hooks (§11) — defaults keep source-free delegates valid.
    // Map an INTENT channel to a control source id (nullopt = no source
    // semantics). Intents on mapped channels acquire the source (§11.4:
    // exclusive ownership; SOURCE_CONFLICT / takeover) BEFORE applyIntent.
    virtual std::optional<uint8_t> sourceForChannel(uint16_t channel_id) {
        (void)channel_id; return std::nullopt;
    }
    // §11.3: loss policy per source. Initiator-bound (streams, live control)
    // default Stop; hub-autonomous (pattern) returns Continue.
    virtual SourceLossPolicy sourcePolicy(uint8_t source_id) {
        (void)source_id; return SourceLossPolicy::Stop;
    }
    // §11.3: deadman fired on a Stop-policy source — STOP MOTION NOW (the
    // hub latches STOP in the safety word AFTER this returns).
    virtual void onDeadmanStop(uint8_t source_id) { (void)source_id; }
    // §11.4: ownership transitions (reason: 0 acquire, 1 takeover, 2 release,
    // 3 deadman-release, 4 session-loss-release). owner_session 0 = released.
    virtual void onSourceOwnership(uint8_t source_id, uint32_t owner_session,
                                   uint8_t reason) {
        (void)source_id; (void)owner_session; (void)reason;
    }
    // §11.2: application-side clear precondition (velocity zero, fault gone —
    // machine domain the library can't see). false -> NACK CLEAR_REFUSED.
    virtual bool canClearEstop() { return true; }
};

// The hub. Construction wires the catalog (client-invariant, §8.6), clock,
// rng (session ids, §6.1), and delegate. Transports attach 1:1 with session
// slots. Owner loop: hub.update(clock.nowUs()) — pumps every transport,
// advances every session, paces retained STATE pushes.
class Hub {
public:
    Hub(const Catalog32& catalog, IClock& clock, IRandom& rng, HubDelegate& delegate);

    // Returns false when all kHubMaxSessions slots are taken (the transport
    // itself is then not serviced — distinct from session BUSY, which NACKs
    // a HELLO on an attached transport when slots race, §6.3).
    bool attachTransport(ITransport& t);
    void detachTransport(ITransport& t);

    void update(uint32_t nowUs);

    // ---- Application-facing publication API --------------------------------
    // §9.1: publish the new full snapshot of a STATE channel. Stored in the
    // retained store (seq bumped); each subscriber receives it per its grant
    // pacing (conflation by design: late subscribers get only the latest).
    bool publishState(uint16_t channel_id, std::span<const std::byte> payload);
    // §9.4: enqueue an EVENT to every subscriber of the channel (bounded,
    // drop-oldest per subscriber).
    bool publishEvent(uint16_t channel_id, std::span<const std::byte> encodedEventPayload);

    // ---- Safety (M4 minimal: the latch that E-04 observes) -----------------
    // Latches the ESTOP bit into the safety STATE channel (0x0003), calls
    // delegate.onEstop FIRST (§11.2: stop motion before bookkeeping), then
    // publishes the latched snapshot at critical priority.
    void latchEstop(uint8_t cause, uint8_t origin, uint16_t estop_seq);
    bool estopLatched() const;

    uint16_t cfgGen() const;
    uint32_t bootId() const;
    size_t sessionCount() const;

    // ---- M5: pairing (§12.2) -----------------------------------------------
    // `pinAscii` must outlive the window (it's also what the app displays).
    void openPairingWindow(std::span<const char> pinAscii);
    void closePairingWindow();
    PairingManager& pairing();  // revocation UI / NVS persistence adapter

    // ---- M5: safety (§11) --------------------------------------------------
    // §11.2 clearing: hub-side conditions (latched, no pending escalation) +
    // delegate.canClearEstop(). Clearing never restarts motion. Returns false
    // (and the intent path NACKs CLEAR_REFUSED) when refused.
    bool clearEstop();
    bool stopLatched() const;
    uint8_t safetyWord() const;

    // ---- M5: congestion input (§10.3) --------------------------------------
    // The in-process binding's congestion signal is Simulated (§13.1): the
    // test/sim injects it here per slot. Real bindings feed their native
    // signals through the same choke point. Levels: 0 clear, 1 congested
    // (shed per §10.4 steps 1–3), 2 stalled (never-shed can't drain —
    // §10.4 step 4 eviction clock runs).
    void setCongestionLevel(size_t slotIdx, uint8_t level);

    // ---- M5: network probe (§6.4) -------------------------------------------
    // The client's most recently received PROBE_REPORT for the session in
    // `slotIdx`, nullopt if none has arrived yet. M5 scope is deliberately
    // minimal per the milestone brief: this just surfaces the counters:
    // deciding to raise a grant off of them (§6.4 step 3, "hub MAY raise
    // grants accordingly") is a policy left to a future milestone/delegate
    // hook — not implemented here.
    std::optional<ProbeResult> probeReportFor(size_t slotIdx) const;

    // Test/observability access (read-only).
    const HubSession* sessionBySlot(size_t i) const;

    // ---- M4 test-only hook (documented deviation; real congestion/re-grant
    // policy is M5) -----------------------------------------------------------
    // Forces an unsolicited GRANT (§10.2) for one already-granted channel of
    // one session, driving S-09's "hub changed its mind, client complies"
    // scenario without a real congestion detector behind it. Returns false if
    // `slotIdx` is out of range or holds no occupied session, or the session
    // has no existing grant for `channel_id` (regrant, not first-grant).
    bool regrantForTest(size_t slotIdx, uint16_t channel_id, float new_rate);

private:
    struct PushRecord {
        uint16_t channel_id = 0;
        uint16_t lastSeq = 0;
        bool valid = false;
        uint32_t shedCounter = 0;  // §10.4: counts natural push opportunities for N:1 decimation
    };

    // One physical transport <-> at most one session. `pushRecords` is
    // Hub-private per-subscriber STATE pacing bookkeeping ("what seq did I
    // last actually SEND this subscriber for this channel") that
    // channel/subscription.hpp deliberately does not own (see that file's
    // design note) — it lives here, keyed by channel_id, rather than as an
    // addition to the frozen SubscriptionTable/HubSession building blocks.
    struct Slot {
        ITransport* transport = nullptr;
        HubSession session;
        std::array<PushRecord, limits::max_subscriptions_per_session> pushRecords{};

        // ---- M5 additions (§10.3/§10.4 congestion+shedding, §12.2 pairing) --
        std::array<std::byte, 8> nonce{};   // this session's WELCOME nonce (§6.3 key 29), needed to verify PAIR_REQ
        uint8_t congestionLevel = 0;        // §10.3: 0 clear, 1 congested, 2 stalled — injected via setCongestionLevel()
        bool criticalStalling = false;      // §10.4 step 4: a never-shed send has been failing since criticalStallSinceMs
        uint32_t criticalStallSinceMs = 0;
        bool hasProbeReport = false;        // §6.4
        ProbeResult lastProbeReport{};
    };

    // Physical transport-tracking capacity is deliberately ONE MORE than
    // kHubMaxSessions (the session/grant conformance floor, §6.3/§17.1): it
    // is what makes the admission RACE the spec calls out constructible at
    // all — "NACKs a HELLO on an attached transport when slots race" (§6.3)
    // requires a transport to have successfully attached (this method's
    // OTHER branch: "the transport itself is then not serviced" when there is
    // truly no room anywhere) yet still find every SESSION slot filled by the
    // time its HELLO is processed. kHubMaxSessions itself remains the one
    // hard cap on concurrent SESSIONS — enforced in handleHello() by counting
    // occupied() sessions, never by this array's size — so the effective
    // client-facing conformance floor is unchanged; this is one spare
    // physical slot for the BUSY admission path to be observable at all
    // (see hub_impl.hpp's HELLO handling, and the M4 report's Deviations).
    static constexpr size_t kSlotCapacity = kHubMaxSessions + 1;

    // Scratch capacity for the whole catalog's deterministic CBOR encoding
    // (computed once at construction and cached — §8.3's etag AND §8.4's
    // CATALOG_CHUNK transfer both read from it). Sized generously for a
    // Catalog32; a hub with a denser catalog sizes its own instantiation
    // accordingly (this is this library's default, same posture as
    // RetainedStore<>'s/SubscriptionTable<>'s own default-capacity comments).
    static constexpr size_t kCatalogScratchBytes = 8192;

    const Catalog32& _catalog;
    IClock& _clock;
    IRandom& _rng;
    HubDelegate& _delegate;
    RetainedStore<> _retained;
    std::array<Slot, kSlotCapacity> _slots;  // Slot self-initializes; no braces (explicit ctor member)
    std::array<std::byte, limits::etag_bytes> _etag{};
    std::array<std::byte, kCatalogScratchBytes> _catalogEncoded{};
    size_t _catalogEncodedLen = 0;
    uint32_t _bootId = 0;
    uint16_t _cfgGen = 1;
    MonotonicMs _monoMs;  // wrap-safe ms derivation for all deadline bookkeeping (§7.2)

    // ---- Safety word state (§11.1): bit0 ESTOP, bit1 STOP, bit2 HOLD,
    // bit3 PAUSE — the exact bitfield the `safety` (0x0003) STATE layout
    // publishes. M4 only ever set bit0; M5 adds STOP (deadman) + the general
    // cause/owner bookkeeping the table describes.
    uint8_t _safetyWord = 0;
    uint8_t _safetyCause = 0;       // 0 user, 1 deadman, 2 fault, 3 relay (§11.1)
    uint32_t _safetyOwnerSession = 0;
    uint16_t _estopSeq = 0;

    // ---- M5: pairing (§12.2) + source ownership (§11.4) ---------------------
    PairingManager _pairing;
    SourceOwnershipTable _ownership;

    // ---- internal helpers, defined in hub_impl.hpp (included below) --------
    size_t occupiedCount(const Slot* exclude) const;
    Slot* findSlotByInstance(std::span<const std::byte> instanceId, const Slot* exclude);
    Slot* attachedSlotFor(ITransport& t);
    void pumpSlot(Slot& slot, uint32_t nowMs);
    void dispatchFrame(Slot& slot, const FrameHeader& h, std::span<const std::byte> payload, uint32_t nowMs);
    void handleEstopFrame(const EstopFrame& f, uint32_t nowMs);
    void handleHello(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs);
    void handleSubscribe(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs);
    void handleUnsubscribe(Slot& slot, std::span<const std::byte> payload);
    void handleIntent(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs);
    void handlePing(Slot& slot, std::span<const std::byte> payload);
    void handleGoodbye(Slot& slot);
    void handleCatalogReq(Slot& slot, std::span<const std::byte> payload);
    void pumpStatePacing(Slot& slot, uint32_t nowMs);
    PushRecord* findOrCreatePushRecord(Slot& slot, uint16_t channel_id);
    bool sendFrameTo(ITransport& t, FrameType type, uint16_t channel, std::span<const std::byte> payload,
                     uint16_t seq = 0) const;
    void sendNack(ITransport& t, const NackMsg& n) const;
    std::array<std::byte, 8> buildSafetyPayload() const;
    void broadcastSafetyNow(uint32_t nowMs);

    // ---- M5 helpers, defined in hub_impl.hpp --------------------------------
    void publishSafetySnapshot();                       // republish 0x0003 from _safetyWord/_safetyCause/...
    void publishControlOwnerStateIfPresent();            // republish 0x0004 iff the catalog declares it (§11.4)
    std::array<std::byte, 20> buildControlOwnerPayload() const;
    void emitTakeoverEvent(uint8_t source_id, uint32_t newOwnerSession, uint32_t nowMs);  // §11.4, session-events 0x0007
    bool anySubscribed(uint16_t channel_id) const;

    void pumpDeadman(Slot& slot, uint32_t nowMs);         // §11.3

    // §12.2 pairing wire handling; §6.4 probe.
    void handlePairReq(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs);
    void handleProbeRequest(Slot& slot, uint32_t nowMs);
    void handleProbeReportFrame(Slot& slot, std::span<const std::byte> payload);

    // §10.4 step 4: never-shed slow-consumer tracking + eviction.
    void trackCriticalSend(Slot& slot, bool sendOk, uint32_t nowMs);
    void evictSlot(Slot& slot, NackCode code);
    bool sendFrameToTracked(Slot& slot, FrameType type, uint16_t channel, std::span<const std::byte> payload,
                            uint32_t nowMs, uint16_t seq = 0);
    void sendNackTracked(Slot& slot, const NackMsg& n, uint32_t nowMs);
};

}  // namespace slopsync

#include "slopsync/hub/hub_impl.hpp"
