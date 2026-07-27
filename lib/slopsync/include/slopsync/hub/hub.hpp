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
#include <string_view>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/channel/log_channel.hpp"
#include "slopsync/channel/safety_events_channel.hpp"
#include "slopsync/channel/trust_channels.hpp"
#include "slopsync/channel/retained_store.hpp"
#include "slopsync/core/clock.hpp"
#include "slopsync/core/crypto.hpp"
#include "slopsync/core/result.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/session/pairing.hpp"
#include "slopsync/session/safety.hpp"
#include "slopsync/session/session.hpp"
#include "slopsync/session/shedding.hpp"
#include "slopsync/transport/transport.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/estop_frame.hpp"
#include "slopsync/wire/stream_bundle.hpp"
#include "slopsync/wire/messages/auth.hpp"      // AUTH / HUB_SIG + hubSigMaterial()
#include "slopsync/wire/messages/blob_req.hpp"  // BlobId — the store-backend seam
#include "slopsync/wire/messages/goodbye.hpp"
#include "slopsync/wire/messages/grant.hpp"
#include "slopsync/wire/messages/intent.hpp"
#include "slopsync/wire/messages/nack.hpp"
#include "slopsync/wire/messages/pair.hpp"
#include "slopsync/wire/messages/probe_report.hpp"
#include "slopsync/wire/messages/publish.hpp"  // PublishMsg + PublishWish (via hello.hpp)
#include "slopsync/wire/messages/welcome.hpp"

namespace slopsync {

inline constexpr size_t kHubMaxSessions = 4;  // conformance floor (§6.3, §17.1)

// What the application (firmware / sim model) provides to the hub.
class HubDelegate {
public:
    virtual ~HubDelegate() = default;

    // §12.2: map (instance_id, token) -> role. Return watch when no/invalid
    // token. M4 default-watch implementations are fine (pairing lands M5).
    virtual AccessLevel validateToken(std::span<const std::byte> instance_id,
                                      std::span<const std::byte> token, bool hasToken) {
        (void)instance_id; (void)token; (void)hasToken;
        return AccessLevel::watch;
    }

    // §9.3: apply an intent. Return the APPLIED (post-clamp) values to echo,
    // or a NackCode refusal. The hub handles idempotency, rate limiting,
    // role gating (catalog access level) and cfg_gen BEFORE calling this;
    // the delegate only applies + clamps.
    //
    // RFC-002 (TIGHTENED for v1.0): set `cfgChanged` iff at least one applied
    // configuration value actually CHANGED — NOT merely because the write was
    // accepted. A value-identical accepted config-set still returns its
    // post-clamp applied map (the ECHO is ground truth and is unaffected) but
    // leaves `cfgChanged` false, so `cfg_gen` does not move and no on-change
    // STATE republish is armed. The old "accepted therefore bumped" reading is
    // what let the wire amplify the 2026-07-24 dual-plane config storm. The
    // hub-side mirror of this rule is Hub::bumpConfigGeneration().
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
    //
    // Stream-ingress extension (additive, §11.4): the SAME mapping governs
    // c2h STREAM (motion-input) channels — a mapped STREAM channel's first
    // accepted bundle acquires the source and each subsequent bundle refreshes
    // its deadman (§11.3). Return the same source id for the INTENT channel and
    // the STREAM channel that drive one physical source so a stream and a jog
    // intent can't own it simultaneously. Semantics for INTENT callers are
    // unchanged.
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

    // §9.2/§10.5: a validated inbound STREAM bundle (client→hub motion input)
    // arrived on `channel_id` from `session_id`. Called ONLY after the hub has
    // confirmed the channel is a granted c2h STREAM publish for this session,
    // re-validated the §5.4 caps against its own catalog (n in 1..32, span
    // ≤ 20 ms, strictly-increasing t_off with t_off[0] == 0, exact size),
    // passed the granted-rate token bucket (§10.5), and acquired/refreshed
    // source ownership (§11.4) + deadman (§11.3). The delegate applies the
    // samples via the MotionArbiter (sole-caller doctrine, §3.1) — it never
    // re-checks ownership, role, or rate. `bundle` is a read-only view over
    // the frame payload, valid only for the duration of this call. Default
    // no-op keeps stream-free delegates valid.
    virtual void onStreamBundle(uint16_t channel_id, uint32_t session_id, const BundleView& bundle) {
        (void)channel_id; (void)session_id; (void)bundle;
    }

    // ---- RFC-021: the BLOB STORE BACKEND SEAM -------------------------------
    // The hub owns TRANSPORT (chunk framing, repair, identity, caps, access);
    // the delegate owns STORAGE. That split is the whole reason BLOB_* is one
    // verb rather than a per-feature clone of the chunking machinery: a store
    // backend implements exactly this one method and inherits selective repair,
    // total_bytes pre-sizing, CHUNK_UNAVAILABLE, and the §8.4 timeouts for free.
    //
    // Called ONLY for namespaces the hub does not serve itself. Namespace 0
    // (blob_ns::catalog) NEVER reaches the delegate — the hub already holds the
    // deterministic catalog encoding it computed at construction, and routing it
    // through a delegate would let an application make the catalog and its etag
    // disagree.
    //
    // Return the item's opaque bytes plus the roster generation they are
    // consistent with, or nullopt for "no such namespace / store / slot" (the
    // hub then NACKs CHUNK_UNAVAILABLE). The returned span MUST stay valid
    // until the next call into the hub — the hub copies out of it chunk by
    // chunk during this one dispatch and never retains it. A flash/NVS-backed
    // store therefore reads into its own scratch buffer and returns a view of
    // that; it never has to know how many chunks the hub will cut it into.
    //
    // Milestone 5 (device-side NVS presets + CRUD intents) plugs in HERE and
    // needs no library change: `ns` selects the space, `store_id` the store
    // declared by a STORE-class catalog entry, `slot` the item.
    struct BlobView {
        std::span<const std::byte> bytes{};
        uint16_t generation = 0;
    };
    virtual std::optional<BlobView> readBlob(uint8_t ns, uint8_t store_id, uint8_t slot) {
        (void)ns; (void)store_id; (void)slot;
        return std::nullopt;
    }
};

// The hub. Construction wires the catalog (client-invariant, §8.6), clock,
// rng (session ids, §6.1), and delegate. Transports attach 1:1 with session
// slots. Owner loop: hub.update(clock.nowUs()) — pumps every transport,
// advances every session, paces retained STATE pushes.
class Hub {
public:
    // `crypto` (RFC-028 obligation 3) is ADDITIVE with a null-object default,
    // so every existing call site compiles untouched — hub.hpp's public API is
    // frozen and extends only this way. The default is the library's own
    // software implementation (core/crypto.hpp); a host substitutes an
    // accelerated one, it never has to reimplement HMAC to get a Hub.
    Hub(const Catalog32& catalog, IClock& clock, IRandom& rng, HubDelegate& delegate,
        ICrypto& crypto = defaultCrypto());

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

    // ---- RFC-017: the SlopLog -> SlopSync BRIDGE SEAM -----------------------
    // THE seam a firmware log bridge calls, and the whole of it. Milestone 5
    // implements a `sloplog::ISink` whose write() is:
    //
    //     hub.publishLog(uint8_t(entry.level), entry.tag, entry.formatted);
    //
    // registered in applogBegin() alongside the existing web/serial sinks. The
    // library stays hardware-free because that is the entire contract: an
    // already-formatted line, a level from `log_levels`, a tag. This function
    // stamps hub-ms from the injected IClock, encodes ONE `log_events::entry`
    // EVENT with its fields in the scoped `body` (40) sub-map, files it in the
    // bounded replay ring, and fans it to every current subscriber.
    //
    // NEVER ALLOCATES, NEVER BLOCKS, NEVER FORMATS. Over-length `tag` and
    // `message` are TRUNCATED, not rejected (RFC-022.5's senders-truncate rule
    // — a diagnostic that vanishes because it was too long is the worst
    // possible failure mode for a log). Returns false only when the catalog
    // does not declare channel 0x0008 at all, i.e. this hub has no log channel.
    //
    // A drop-in caveat worth knowing before wiring the bridge: sinks must not
    // be able to recurse. A hub log line emitted from inside publishLog's own
    // transport writes would re-enter here; the firmware bridge therefore
    // publishes from the drain task exactly like SerialSink does, never from
    // inside SlopSync's own send path.
    bool publishLog(uint8_t level, std::string_view tag, std::string_view message);
    // §9.4's visible, never-silent drop counter for the log ring.
    uint32_t logDropped() const;

    // ---- Safety (M4 minimal: the latch that E-04 observes) -----------------
    // Latches the ESTOP bit into the safety STATE channel (0x0003), calls
    // delegate.onEstop FIRST (§11.2: stop motion before bookkeeping), then
    // publishes the latched snapshot at critical priority.
    void latchEstop(uint8_t cause, uint8_t origin, uint16_t estop_seq);
    bool estopLatched() const;

    // ---- Catalog encoding health (M5a, additive) ---------------------------
    // Bytes the constructor's encodeCatalog() actually produced, and the
    // compile-time buffer it had to fit in. ZERO IS A FAILURE, and it is the
    // one failure this class cannot report any other way: the constructor has
    // no return value, the library has no exceptions, and a hub whose catalog
    // did not fit still starts, still answers HELLO, and still serves an etag
    // — one computed over zero bytes, with an empty catalog behind it. Every
    // host SHOULD check this once at startup and shout; the firmware and the
    // sim both do. Purely observational, no behaviour attached.
    size_t catalogEncodedBytes() const { return _catalogEncodedLen; }
    static constexpr size_t catalogScratchCapacity() { return kCatalogScratchBytes; }

    uint16_t cfgGen() const;
    uint32_t bootId() const;
    size_t sessionCount() const;

    // ---- RFC-002 + RFC-011: the ONE cfg_gen rule ----------------------------
    // `cfg_gen` advances IF AND ONLY IF at least one applied configuration
    // value actually CHANGED — regardless of who changed it.
    //
    // Two halves, and both were broken:
    //   * CLIENT-driven (RFC-002): an accepted config-set used to bump even
    //     when every applied value was identical, re-arming on-change
    //     publications and legacy resync storms. The hub only bumps when the
    //     delegate reports `cfgChanged`, and applyIntent's contract now says
    //     that flag means CHANGED, not ACCEPTED. A value-identical accepted
    //     intent still gets its post-clamp ECHO (ground truth is unaffected)
    //     but MUST NOT bump nor trigger an on-change STATE republish.
    //   * HUB-driven (RFC-011): a machine-side config change — a physical
    //     control, boot adoption, an internal recalculation — had NO way to
    //     advance the generation, so a client's `precondition` CAS passed
    //     against config that had already moved. THIS is that missing half.
    //
    // Call it AFTER the change is applied and its STATE republished, once per
    // change (not once per field). It is idempotent in the sense that matters:
    // extra bumps are safe-but-wasteful (clients resync), missing bumps are
    // UNSAFE (a stale CAS silently succeeds).
    void bumpConfigGeneration();

    // ---- M5: pairing (§12.2) -----------------------------------------------
    // `pinAscii` must outlive the window (it's also what the app displays).
    void openPairingWindow(std::span<const char> pinAscii);
    void closePairingWindow();
    PairingManager& pairing();  // revocation UI / NVS persistence adapter
    const PairingManager& pairing() const;
    ICrypto& crypto();

    // ---- M4c: RFC-029 item 1, HUB AUTHENTICITY (the anti-evil-twin path) ---
    //
    // THE PROBLEM THIS SOLVES: nothing stops a laptop from BEING "SlopDrive" on
    // mDNS. Identity strings are claims, and a clone copies them perfectly. A
    // signature over material the CLIENT contributed entropy to cannot be
    // copied, replayed, or guessed by anything that does not hold the machine's
    // private key.
    //
    // THE LATENCY PROBLEM, AND WHY THIS IS AN API AT ALL: on the S3 there is no
    // ECC accelerator (that peripheral is C3/C6/H2), so one mbedtls software
    // ECDSA sign is ~30-80 ms in ONE uninterruptible call. update() runs on a
    // 5 ms Core-0 tick that also paces STATE, runs the deadman, and drains the
    // motion-input ring — an inline sign would stall all of that for 6-16 ticks
    // EVERY time a client connects, i.e. one client's handshake would put a
    // visible hole in another client's motion stream. That is not a latency
    // nit; it is a defect.
    //
    // The library is threadless by invariant, so it cannot own the deferral —
    // it exposes the WORK instead, and the application runs it on whatever task
    // it likes:
    //
    //     SignJob job;
    //     while (hub.takePendingSignJob(job)) {          // on a LOW-PRIORITY task
    //         std::array<std::byte, 72> sig{};
    //         size_t n = mySign(job.message, sig);       // ~30-80 ms, off the tick
    //         hub.submitSignature(job.session_id, {sig.data(), n});
    //     }
    //
    // The hub emits HUB_SIG (0x1D) when the signature arrives. A session that
    // died in the meantime is dropped silently by submitSignature() — the
    // session id is re-checked, so a slow signer can never deliver one client's
    // signature into another client's session.
    struct SignJob {
        uint32_t session_id = 0;
        std::array<std::byte, kHubSigMaterialBytes> message{};  // client_nonce || session_id || boot_id
    };
    // Pops one outstanding job. False when there is nothing to sign. Safe to
    // call from any task that is not concurrently inside update() (this library
    // has no locks — the application owns the serialization, exactly as it does
    // for every other Hub method).
    bool takePendingSignJob(SignJob& out);
    size_t pendingSignJobs() const;
    // Delivers a finished signature. Returns false when the session no longer
    // exists, already got one, or the signature is empty/oversize.
    bool submitSignature(uint32_t session_id, std::span<const std::byte> sig);
    // CONVENIENCE, AND A FOOTGUN WHERE SIGNING IS SLOW: drains up to `maxJobs`
    // through the injected ICrypto synchronously. Correct for a host/sim hub
    // and for tests; on the S3 this is exactly the stall the deferral exists to
    // prevent, so the firmware calls takePendingSignJob()/submitSignature()
    // from its own worker instead. Returns how many it signed.
    size_t signPendingNow(size_t maxJobs = 4);
    // Sign INLINE inside handleHello and put `welcome_sig` straight into
    // WELCOME, skipping HUB_SIG entirely. Legal, wire-alternative, identical
    // material — and appropriate ONLY where a sign is cheap (a host, the sim, a
    // part with the ECC peripheral). Off by default: the default must be the
    // one that cannot wreck a real-time tick.
    void setInlineSigning(bool on);
    bool inlineSigning() const;

    // ---- M4b: RFC-027(c) PUSH-TO-PAIR (the physical-presence window) --------
    // Opens a SHORT, SINGLE-GRANT association window: the first knock in it is
    // granted with no approval, and consumes it.
    //
    // THE LIBRARY PROVIDES THE WINDOW, NOT THE GESTURE. RFC-027 requires a
    // PHYSICAL-PRESENCE PROOF, not a GPIO, and the bare-minimum hardware is
    // NONE — the power cord is the button. Deciding that presence was proven is
    // the application's job and only the application's: firmware milestone 5
    // implements the NVS boot-counter gesture (N consecutive boots with uptime
    // < ~10 s) and calls this. A hub with a real button MAY bind it instead —
    // a UX upgrade, never a requirement. FACTORY RESET MUST BE A HARDER GESTURE
    // than opening this window; that too is the application's rule to keep.
    //
    // FACTORY-FRESH: with ZERO configure tokens in the ledger the window grants
    // `configure`, because physical possession is root and nobody has claimed
    // the machine yet (Matter/Chromecast commissioning semantics). Once a
    // configure token exists it grants the configured default (`control`).
    //
    // OBSERVABILITY: opening publishes a `window_opened` EVENT on 0x000B
    // (`watch` access), sets the window bit in the 0x000A snapshot, and adds
    // push_to_pair to `trust`.pairing_modes in every subsequent WELCOME — three
    // in-band signals, because a hub may have no LED and RFC-027 requires any
    // watch session to be able to see this.
    void openPresenceWindow();
    void closePresenceWindow();
    bool presenceWindowOpen() const;
    // What the window grants once a configure token DOES exist. Default control.
    void setPresenceDefaultRole(AccessLevel r);
    // Whether bare knocks are accepted at all (RFC-027 mode (a)). Default true.
    void setKnockApproveEnabled(bool on);

    // ---- M4b: RFC-029 item 2, the CLIENT-CHANGE TRIPWIRE policy -------------
    // A paired device whose granted role is AT OR BELOW this level auto-rekeeps
    // when its reported `client_ver` changes; anything above it is SUSPENDED to
    // `watch` pending re-approval. Default `watch` — the RFC's own default.
    //
    // HONESTY CLAUSE, NORMATIVE, repeated here because this setter is where an
    // integrator meets the feature: the version is SELF-REPORTED. This is a
    // TRIPWIRE, NOT ATTESTATION. It catches an honest update and nothing else;
    // a deliberately malicious update reports whatever version it likes and
    // keeps its token. The real bounds on a hostile client are role scoping,
    // instant revocation, roster visibility, and the role-exempt safety ops. Do
    // not let a UI built on this imply a stronger guarantee than exists.
    void setTrustChangeAutoKeepMax(AccessLevel r);

    // The trust ledger's `first_seen`/`last_seen` are UNIX epoch seconds, and
    // the library cannot invent them: protocol time is hub-boot-relative and
    // wraps in ~71 minutes (§7.2). An application with a wall clock (SNTP)
    // pushes it here; without one the fields stay 0, which is the honest
    // answer and is explicitly legal.
    void setWallClockSeconds(uint32_t epochSeconds);

    // ---- M5: safety (§11) --------------------------------------------------
    // §11.2 clearing: hub-side conditions (latched, no pending escalation) +
    // delegate.canClearEstop(). Clearing never restarts motion. Returns false
    // (and the intent path NACKs CLEAR_REFUSED) when refused.
    bool clearEstop();
    bool stopLatched() const;
    uint8_t safetyWord() const;

    // ---- RFC-025c: the safety channel's APPENDED `modes` byte ---------------
    // manual_override / bypass_limits as SAFETY-domain state (safety_mode_bits).
    // Clients WRITE them through safety_ops override_on/off + bypass_on/off on
    // 0x0005 (control role), which the hub latches on delegate acceptance —
    // exactly like HOLD/PAUSE. This setter is the OTHER direction: the machine
    // changing its own mind (a physical control, a legacy UI plane, an e-stop
    // that drops override as a side effect) pushing ground truth back into the
    // snapshot. Same posture as the firmware's existing estop reconciliation:
    // whatever the machine actually is, the retained snapshot says. Publishes +
    // broadcasts at critical priority only when a bit actually changed.
    void setSafetyModes(bool manualOverride, bool bypassLimits);
    uint8_t safetyModes() const;

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

    // §16.2 STREAM-ingress telemetry (read-only): per-session count of bundles
    // delivered to the delegate vs. dropped (ungranted / malformed / over-rate
    // / source-conflict). {0,0} for an out-of-range or unoccupied slot.
    struct StreamIngressCounters {
        uint32_t accepted = 0;
        uint32_t dropped = 0;
    };
    StreamIngressCounters streamIngressCounters(size_t slotIdx) const;

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
        std::array<std::byte, 8> nonce{};   // this session's WELCOME nonce (§6.3 key 29), needed to verify PAIR_REQ and AUTH proofs

        // ---- M4c (RFC-029 items 1 + 6): hub authenticity + token proofs -----
        // The client's own entropy from HELLO's `trust`.client_nonce, and
        // whether it asked to be signed to. Both live on the SLOT rather than
        // in HubSession because HubSession is a frozen building block and this
        // is per-transport handshake bookkeeping, exactly like `nonce` above.
        std::array<std::byte, kTrustClientNonceBytes> clientNonce{};
        bool hasClientNonce = false;
        bool sigRequested = false;
        // THE DEFERRED-SIGNING QUEUE, all of it. One session can have at most
        // one outstanding signature, so the "queue" is a flag per slot and
        // takePendingSignJob() is a scan — no ring buffer, no allocation, and
        // nothing to leak when a session dies mid-sign (teardown clears the
        // slot, and submitSignature() re-checks the session id before it emits).
        bool signPending = false;
        bool signDelivered = false;
        // RFC-029 item 6: failed AUTH proofs on this session. Mirrors §12.2's
        // three-strikes PIN rule rather than inventing a second number.
        uint8_t authFailures = 0;
        uint8_t congestionLevel = 0;        // §10.3: 0 clear, 1 congested, 2 stalled — injected via setCongestionLevel()
        bool criticalStalling = false;      // §10.4 step 4: a never-shed send has been failing since criticalStallSinceMs
        uint32_t criticalStallSinceMs = 0;
        bool hasProbeReport = false;        // §6.4
        ProbeResult lastProbeReport{};

        // ---- RFC-012: SOURCE_CONFLICT NACK throttle, per (session, SOURCE) --
        // Keyed by arbiter source, not by channel, because that is the thing
        // that is actually conflicted: this device maps BOTH 0x0084 and 0x0085
        // to source 1, and a producer that fails over between them is one dead
        // producer, not two. Cleared by teardownSession() with the rest of the
        // slot, so a re-connecting client gets its first NACK immediately.
        struct ConflictNack {
            bool used = false;
            uint8_t source = 0;
            uint32_t lastMs = 0;
        };
        std::array<ConflictNack, SourceOwnershipTable::kMaxSources> conflictNacks{};

        // ---- §8.4/§13.1: the RESUMABLE BLOB TRANSFER CURSOR ------------------
        // A BLOB_REQ used to be answered by blasting every chunk in ONE
        // synchronous loop with the transport's return value DISCARDED. §13.1 is
        // explicit that write() == false means "not accepted right now; caller
        // decides retry vs drop" — and the caller was silently choosing DROP, so
        // any blob longer than the link's egress queue lost its tail. MEASURED on
        // hardware: the 57-chunk device catalog delivered 47 chunks against a
        // 32-deep TX queue and the transfer failed. Growing the queue "fixed" it
        // at the cost of tripling per-client buffering and driving heap
        // low-water to 208 bytes under three clients — correctness bought with
        // headroom, which is the wrong trade. This cursor is the retry half: the
        // request is REMEMBERED, and drained a bounded number of chunks per
        // update() tick, pausing exactly when the transport pushes back.
        //
        // It holds an IDENTITY, never a span, and that is load-bearing rather
        // than fastidious. The bytes a transfer walks are BORROWED: the catalog
        // encoding is stable, but a trust-ledger item lives in ONE hub-wide
        // scratch buffer (_ledgerItemScratch) and a delegate's bytes are only
        // promised valid "until the next call into the hub" (HubDelegate::
        // readBlob). Re-resolving from the identity on every resume is what lets
        // two sessions transfer two different store items concurrently without
        // shredding each other's bytes.
        struct PendingBlob {
            bool active = false;
            BlobId id{};              // re-resolved every resume; never a retained span
            uint32_t totalBytes = 0;  // size at arm time — re-checked on every resume
            uint16_t generation = 0;  // ditto: a moved generation is a different document
            uint16_t reqSeq = 0;      // RFC-001: the BLOB_REQ that asked, for late NACK correlation
            bool hasReqSeq = false;
            bool full = true;         // true: walk 0..chunkCount; false: walk `chunks`
            uint16_t nextIndex = 0;   // full transfer: next chunk index to emit
            uint16_t cursor = 0;      // selective repair: position within `chunks`
            uint16_t count = 0;       // selective repair: how many indices were named
            std::array<uint16_t, kBlobReqMaxChunks> chunks{};
        };
        PendingBlob blob{};
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
    // BLOB_CHUNK transfer both read from it). Sized generously for a
    // Catalog32; a hub with a denser catalog sizes its own instantiation
    // accordingly (this is this library's default, same posture as
    // RetainedStore<>'s/SubscriptionTable<>'s own default-capacity comments).
    //
    // 8192 -> 16384 at M5a, and the reason is worth recording because it bit
    // for real: RFC-009 annotations (desc/group/options/defaults) are FLASH on
    // the hub and travel once, but they land in THIS buffer, and the RFC's own
    // sizing note projects 15–25 KB for a lavish catalog. The SlopDrive device
    // catalog crossed 8192 the moment its settings surface was annotated, and
    // the failure mode was SILENT AND TOTAL: encodeCatalog returns 0, the etag
    // is then a hash of ZERO BYTES, and the hub serves an empty catalog to
    // every client — a machine that advertises nothing while looking healthy.
    // catalogEncodedBytes() below exists so a host can NOTICE that instead of
    // shipping it.
    // MEASURED, twice, and the second measurement is why this moved. M5b's
    // 0x008A left the catalog at 12026 B of 16384. M5c's three slopmotion-*
    // tuning cards plus their shared writer take it to 15817 B — 96.5% full,
    // which is not headroom, it is a cliff one channel away.
    //
    // (This constant was briefly grown at M5b on a WRONG diagnosis — an
    // out-of-order entry list reads as a sizing failure, see below — and put
    // back when the measurement disagreed. It is growing now because the
    // measurement AGREES. Same number, opposite epistemic status.)
    //
    // A WARNING ABOUT DIAGNOSING THIS BUFFER, learned the expensive way at M5b:
    // encodeCatalog() returns 0 for TWO unrelated reasons — the buffer being
    // too small, and the entry list not being in strictly ascending id order
    // (its very first loop). A size-shaped log line for an ordering bug sends
    // you straight to this constant, and growing it does nothing. If encode
    // returns 0, check the ORDER first; it is the cheaper hypothesis and it is
    // the one that does not cost a flash cycle to disprove.
    //
    // 24576 -> 32768 for the advanced-pattern channel set (SlopSyncCatalog.h
    // 0x008E..0x0094 + 0x0107, 8 base controls + 6-per-control cyclic
    // Modifier). MEASURED (a scratch instrumented build, same method as the
    // M5a/M5b entries): 16073 B before, 23282 B after (94.7% of the OLD
    // 24576 B cap — past the device-catalog test's 80%-headroom floor, and
    // exactly the "cliff one channel away" the M5c comment above already
    // named). This is a RAM knob, not a wire constant — it lives in PSRAM
    // (the whole SlopSyncHubService is placement-new'd there; see
    // docs/http-plane-retirement.md and the firmware's own comments on why
    // that placement is load-bearing), and 8 KB more of an 8 MB budget is
    // noise. 32768 leaves 26214 B of 80%-headroom against a measured 23282 B
    // — real margin, not landing exactly on the new line a third time.
    static constexpr size_t kCatalogScratchBytes = 32768;

    // ---- §8.4: per-update() chunk budget for a resumable blob transfer -------
    // NOT a wire number, so no registry entry: a receiver cannot observe this
    // except as pacing, and two conformant hubs may pick differently. What
    // BOUNDS it, though, is wire-visible at both ends, which is where the value
    // comes from:
    //
    //   FLOOR — limits::catalog_chunk_gap_timeout_ms (500) is how long a
    //     receiver waits between chunks before it starts demanding repairs, and
    //     limits::frag_reassembly_timeout_ms (5000) is when it abandons. A
    //     transfer must stay obviously inside both.
    //   CEILING — a tick's chunks share the link (and the same per-client TX
    //     queue) with the safety plane, STATE pacing and EVENT drain, and one
    //     update() must not do dramatically more work than another.
    //
    // 8 chunks per tick on the firmware's 5 ms hub task is 1600 chunks/s
    // (~330 KB/s of BLOB_CHUNK) — far more throughput than any catalog needs.
    // The 57-chunk SlopDrive device catalog finishes in 8 ticks, ~40 ms: an
    // eighth of the gap timeout, two orders inside the abandon timeout, and
    // imperceptible next to the handshake it follows. Per tick that is
    // 8 x 206 B of payload (~1.7 KB framed), a quarter of the 32-deep TX queue
    // this bug was found against, so a transfer paces itself well below the
    // queue it shares instead of trying to own it.
    // 8 -> 2 (M5c). MEASURED, not tuned by feel: a bisect showed every SlopSync
    // session transiently borrows ~45-60 KB of heap, on a session whose steady
    // state costs about 1 KB, and that the WS plane owns it outright (450 HTTP
    // polls moved the watermark by exactly zero). The catalog is 11,659 B, so
    // the amplification is the queue, not the payload: each chunk this burst
    // emits becomes an AsyncWebSocket heap copy AND an AsyncTCP pbuf copy, and
    // bursting 8 at once puts all of them in flight simultaneously.
    //
    // Lower is slower, not broken -- §8.4 transfers are resumable BY DESIGN and
    // the hub already stops the instant the transport pushes back, so this
    // trades a few extra 5 ms ticks of catalog transfer for a much shallower
    // peak. A first-connect that takes 60 ms instead of 15 ms is invisible; a
    // 60 KB heap spike on a device with ~70 KB free is not.
    static constexpr size_t kBlobChunksPerTick = 2;

    const Catalog32& _catalog;
    IClock& _clock;
    IRandom& _rng;
    HubDelegate& _delegate;
    ICrypto& _crypto;
    RetainedStore<> _retained;
    std::array<Slot, kSlotCapacity> _slots;  // Slot self-initializes; no braces (explicit ctor member)
    std::array<std::byte, limits::etag_bytes> _etag{};
    std::array<std::byte, kCatalogScratchBytes> _catalogEncoded{};
    size_t _catalogEncodedLen = 0;
    uint32_t _bootId = 0;
    uint16_t _cfgGen = 1;
    MonotonicMs _monoMs;  // wrap-safe ms derivation for all deadline bookkeeping (§7.2)

    // ---- RFC-001: NACK ↔ inbound-frame correlation --------------------------
    // The frame-header seq of the frame currently being dispatched, valid ONLY
    // for the duration of that dispatch. Every NACK the hub emits inside a
    // dispatch is by definition "provoked by a specific inbound frame" and is
    // stamped with it (sendNack/sendNackTracked do it centrally, so no call
    // site can forget); a NACK emitted outside dispatch — there are none today,
    // but the deadman/eviction paths are right there — carries no seq, which is
    // the honest answer. A caller that sets `has_intent_seq` itself wins.
    uint16_t _dispatchSeq = 0;
    bool _dispatchSeqValid = false;

    // ---- Safety word state (§11.1): bit0 ESTOP, bit1 STOP, bit2 HOLD,
    // bit3 PAUSE — the exact bitfield the `safety` (0x0003) STATE layout
    // publishes. M4 only ever set bit0; M5 adds STOP (deadman) + the general
    // cause/owner bookkeeping the table describes.
    uint8_t _safetyWord = 0;
    uint8_t _safetyCause = 0;       // a `safety_causes` value (§11.1) — registry-owned, never a local enum
    uint32_t _safetyOwnerSession = 0;
    uint16_t _estopSeq = 0;
    // RFC-025c: the APPENDED `modes` byte of the 0x0003 snapshot
    // (safety_mode_bits). Appending it is legal layout evolution — every
    // existing field keeps its offset, an old client parsing the first 8 bytes
    // is still correct, and the etag moves, which IS the re-fetch mechanism.
    uint8_t _safetyModes = 0;

    // ---- M5: pairing (§12.2) + source ownership (§11.4) ---------------------
    PairingManager _pairing;
    SourceOwnershipTable _ownership;

    // ---- M4b: the trust ledger's BLOB-store identity ------------------------
    // Discovered from the catalog at construction, never legislated: if the
    // catalog declares a STORE entry at channels::paired_devices, its
    // descriptor's storeId is the number BLOB_REQ addresses it by. The catalog
    // is self-describing, so the id is agreed by being PUBLISHED. A hub with no
    // such entry has no trust store, `_hasPairedStore` stays false, and every
    // ledger request falls through to the delegate exactly as before.
    uint8_t _pairedStoreId = 0;
    bool _hasPairedStore = false;
    // Edge detector for the presence window. The window itself expires on a
    // time comparison inside PairingManager, so SOMETHING has to notice the
    // moment it stopped being open in order to emit the closing EVENT — a
    // watch session that saw `window_opened` must always see its close, even
    // when nobody ever knocked.
    bool _presenceWasOpen = false;
    // M4c: see setInlineSigning(). Default false — the safe answer for the part
    // this firmware actually runs on.
    bool _inlineSigning = false;
    // Scratch for ONE encoded ledger item, the unit a BLOB_REQ serves. Sized to
    // the store descriptor's per_item_max (trust_ledger_max_bytes /
    // paired_devices_max = 237) rounded up. Never the whole ledger: the wire
    // form is per-item so a client can fetch or re-fetch one slot.
    std::array<std::byte, 256> _ledgerItemScratch{};

    // ---- RFC-017: the log channel's bounded replay ring ---------------------
    // ONE per hub (not per session): the log is hub state, and every joining
    // subscriber replays the same tail. Depth = limits::log_replay_depth_default
    // (32); a hub that wants a shallower ring re-instantiates this member.
    EventReplayRing<> _logRing;

    // ---- internal helpers, defined in hub_impl.hpp (included below) --------
    size_t occupiedCount(const Slot* exclude) const;
    Slot* findSlotByInstance(std::span<const std::byte> instanceId, const Slot* exclude);
    Slot* attachedSlotFor(ITransport& t);
    void pumpSlot(Slot& slot, uint32_t nowMs);
    void dispatchFrame(Slot& slot, const FrameHeader& h, std::span<const std::byte> payload, uint32_t nowMs);
    void handleEstopFrame(const EstopFrame& f, uint32_t nowMs);
    void handleHello(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs);
    // ---- M4c (RFC-029) ------------------------------------------------------
    void handleAuth(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs);
    // The RFC-029 item-2 tripwire, shared VERBATIM by the HELLO bearer path and
    // the AUTH proof path. Extracted because a proof-mode client carries no
    // token in HELLO, so leaving the tripwire inline in handleHello would have
    // meant proof-mode clients — the ones with the BETTER security posture —
    // silently never tripping it. Returns the effective role.
    AccessLevel applyTrustObservation(Slot& slot, uint32_t nowMs);
    void emitHubSig(Slot& slot, std::span<const std::byte> sig);
    void handleSubscribe(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs);
    void handleUnsubscribe(Slot& slot, std::span<const std::byte> payload);
    // §6.6/RFC-013: mid-session publish renegotiation. Adds/replaces publish
    // grants and answers with a GRANT carrying `granted_publishes`.
    void handlePublish(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs);
    // The ONE place §6.2's publish-wish validation + rate/burst clamping
    // lives, shared verbatim by HELLO and PUBLISH (they must never drift).
    // Returns the grant to echo, or nullopt when the wish is to be omitted.
    std::optional<GrantedPublish> grantPublishWish(Slot& slot, const PublishWish& wish, uint32_t nowMs);
    // §8.4/RFC-015: flip this session's readiness bit (idempotent).
    void handleCatalogReady(Slot& slot, std::span<const std::byte> payload);
    // RFC-015: GOODBYE READY_TIMEOUT a session that never finished adopting
    // the catalog. Returns true when the slot was torn down.
    bool pumpReadyTimeout(Slot& slot, uint32_t nowMs);
    void handleIntent(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs);
    // §9.2/§10.5 inbound STREAM ingress. Needs the frame HEADER (channel id
    // rides the header, not the packed payload), unlike the CBOR handlers.
    void handleStream(Slot& slot, const FrameHeader& h, std::span<const std::byte> payload, uint32_t nowMs);
    void sendStreamOverageNack(Slot& slot, HubSession::PublishGrant& pg, uint16_t channel_id, uint32_t nowMs);
    // RFC-012: the §9.2 "bundles are never NACKed" carve-out for ownership.
    void sendSourceConflictNack(Slot& slot, uint8_t source, uint16_t channel_id, uint32_t nowMs);
    void handlePing(Slot& slot, std::span<const std::byte> payload);
    void handleClock(Slot& slot, std::span<const std::byte> payload);  // §7.1 hub-time exchange
    void handleGoodbye(Slot& slot, uint32_t nowMs);
    void handleBlobReq(Slot& slot, std::span<const std::byte> payload);
    // §8.4/RFC-021: the namespace-aware half of a transfer — WHICH bytes, and
    // may this session have them. Split out of handleBlobReq because a resumable
    // transfer re-runs it on every tick it resumes on (see Slot::PendingBlob).
    // Returns false with `nack` filled in for the caller to send; on success
    // stamps `id.generation` and points `encoded` at the borrowed bytes.
    bool resolveBlobBytes(Slot& slot, BlobId& id, std::span<const std::byte>& encoded, NackMsg& nack);
    // §8.4/§13.1: emit up to kBlobChunksPerTick chunks of this slot's pending
    // transfer, stopping early and keeping its position the moment the transport
    // refuses a write. Called once per slot per update().
    void pumpBlobTransfer(Slot& slot);
    void pumpStatePacing(Slot& slot, uint32_t nowMs);
    PushRecord* findOrCreatePushRecord(Slot& slot, uint16_t channel_id);
    bool sendFrameTo(ITransport& t, FrameType type, uint16_t channel, std::span<const std::byte> payload,
                     uint16_t seq = 0) const;
    void sendNack(ITransport& t, const NackMsg& n) const;
    // 9 bytes as of RFC-025c (was 8): word, cause, owner_session, estop_seq,
    // modes. Must stay byte-identical to the 0x0003 catalog layout.
    std::array<std::byte, 9> buildSafetyPayload() const;
    void broadcastSafetyNow(uint32_t nowMs);
    // RFC-010/025a/025c: the ONE place a 0x0005 op mutates the latched safety
    // state, run AFTER the delegate accepted it. Returns true if anything
    // changed (caller publishes + broadcasts).
    bool applySafetyOpLatch(uint8_t op, uint32_t sessionId);
    // RFC-025/010 per-op access: the minimum role `m`'s value map demands on
    // `entry`, starting from the entry's own access FLOOR and raised by any
    // schema-field `access` (key 16) or per-option `option_access` (key 17).
    AccessLevel requiredAccessFor(const CatalogEntry& entry, const IntentMsg& m) const;

    // ---- M5 helpers, defined in hub_impl.hpp --------------------------------
    void publishSafetySnapshot();                       // republish 0x0003 from _safetyWord/_safetyCause/...
    void publishControlOwnerStateIfPresent();            // republish 0x0004 iff the catalog declares it (§11.4)
    std::array<std::byte, 20> buildControlOwnerPayload() const;
    void emitTakeoverEvent(uint8_t source_id, uint32_t newOwnerSession, uint32_t nowMs);  // §11.4, session-events 0x0007
    bool anySubscribed(uint16_t channel_id) const;

    void pumpDeadman(Slot& slot, uint32_t nowMs);         // §11.3
    // RFC-024: §6.5 idle reaping for sessions that own NO source. Returns true
    // when the slot was torn down.
    bool pumpIdleReap(Slot& slot, uint32_t nowMs);
    // §9.4: drain this session's bounded EVENT queue. Split out of publishEvent
    // because RFC-015's READY gate makes queued-but-not-yet-sendable a real
    // state (and RFC-017 replay deliberately fills the queue before the session
    // can receive anything).
    void pumpEventDrain(Slot& slot);
    // RFC-017: on a fresh grant of a channel whose catalog entry declares a
    // replay_depth, seed the subscriber's queue with the ring tail.
    void replayEventsOnGrant(Slot& slot, uint16_t channel_id);

    // §6.8/§11.3/§11.4: release EVERY source a session owns, running each
    // source's §11.3 loss policy (Stop -> onDeadmanStop + latch STOP +
    // publish/broadcast; Continue -> release only), then republish
    // control-owner STATE once if anything was released. `reason` is the §11.4
    // delegate ownership-release reason (3 = deadman-release, 4 =
    // session-loss-release). Shared by pumpDeadman() and teardownSession() so
    // "owner departs" is ONE behavior regardless of how the departure happened.
    void releaseSessionSources(uint32_t sessionId, uint8_t reason, uint32_t nowMs);
    // Centralized teardown for one session slot: releases source ownership
    // (above) with the given `reason` (default session-loss), notifies the
    // roster (onSessionLeft), and frees the slot (session.reset() +
    // pushRecords). EVERY path that ends or recycles an occupied slot — GOODBYE,
    // transport detach, slow-consumer/duplicate-instance eviction, same-slot
    // re-HELLO, deadman fire — funnels through here so source ownership is
    // never orphaned to a departed session_id (§6.8; the field bug this closes
    // was a dead streamer's session_id owning motion-input forever, Conflict-
    // dropping every later client's bundles until reboot). Safe on a FREE slot
    // (no-op release, no onSessionLeft). Any GOODBYE frame a path wants to send
    // must be sent BEFORE calling this (the slot is reset afterward).
    void teardownSession(Slot& slot, uint32_t nowMs, uint8_t reason = 4);

    // §12.2 pairing wire handling; §6.4 probe.
    void handlePairReq(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs);

    // ---- M4b helpers, defined in hub_impl.hpp -------------------------------
    // §9.4 duality: emit the 0x000E EVENT twin for whatever moved between
    // `beforeWord` and the current `_safetyWord`. ONE function, called at every
    // site that mutates the safety word, so "an edge happened" can never be
    // decided differently in two places — the same reasoning that made
    // teardownSession() one function.
    void emitSafetyEdgeEvents(uint8_t beforeWord, uint32_t nowMs);
    // 0x000B pairing-events. No-op when the catalog does not declare it.
    void emitPairingEvent(uint8_t kind, std::span<const std::byte> instance_id, std::string_view name,
                          uint8_t mode, AccessLevel role, std::string_view version, uint32_t nowMs);
    // Republish 0x000A / 0x000D when the catalog declares them.
    void publishPendingPairingState(uint32_t nowMs);
    void publishPairedRosterState();
    // Knock-window expiry (RFC-027(a)) + presence-window close. Runs once per
    // update(), like every other timed pump.
    void pumpPairing(uint32_t nowMs);
    // RFC-027(a)/(c): a PAIR_REQ carrying no proof. Returns nothing — every
    // branch answers on the wire itself.
    void handleKnock(Slot& slot, const PairReqMsg& m, uint32_t nowMs);
    // Issue PAIR_GRANT to `slot` for an approved instance. Returns false when
    // the ledger is full or the grant could not be encoded.
    bool issuePairGrant(Slot& slot, std::span<const std::byte> instance_id, AccessLevel role, uint8_t mode,
                        uint32_t nowMs);
    // RFC-018/027/029 admin verbs on 0x0009. Returns true when it handled the
    // intent (echoing or NACKing itself).
    bool handleAdminIntent(Slot& slot, const IntentMsg& m, uint32_t nowMs);
    Slot* findSlotBySession(uint32_t session_id);
    void handleProbeRequest(Slot& slot, uint32_t nowMs);
    void handleProbeReportFrame(Slot& slot, std::span<const std::byte> payload);

    // §10.4 step 4: never-shed slow-consumer tracking + eviction.
    void trackCriticalSend(Slot& slot, bool sendOk, uint32_t nowMs);
    void evictSlot(Slot& slot, NackCode code, uint32_t nowMs);
    bool sendFrameToTracked(Slot& slot, FrameType type, uint16_t channel, std::span<const std::byte> payload,
                            uint32_t nowMs, uint16_t seq = 0);
    void sendNackTracked(Slot& slot, const NackMsg& n, uint32_t nowMs);
};

}  // namespace slopsync

#include "slopsync/hub/hub_impl.hpp"
