// slopsync-core — the CLIENT role (SPEC §2.2, §6). Runs on remotes, apps,
// the sim, and (via slopsync-js reimplementation) browsers. No threads:
// owner pumps update(). All truth flows FROM the hub: the client's shadow
// updates only from STATE/ECHO frames, never from its own requests (§1.2-1).
//
// Reconnect doctrine (§6.7) as API: on transport loss the client drops all
// pending intents and reports each via ClientDelegate::onPendingDropped —
// the APPLICATION reconciles (compare desire vs adopted snapshot, re-issue
// if still wanted); the client library never blind-retransmits.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "slopsync/channel/state_apply.hpp"
#include "slopsync/core/clock.hpp"
#include "slopsync/core/crypto.hpp"
#include "slopsync/core/result.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/session/safety.hpp"
#include "slopsync/session/session.hpp"
#include "slopsync/transport/transport.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/blob_chunks.hpp"
#include "slopsync/wire/frame_buffer.hpp"
#include "slopsync/wire/messages/auth.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/intent.hpp"
#include "slopsync/wire/messages/nack.hpp"
#include "slopsync/wire/messages/pair.hpp"
#include "slopsync/wire/messages/probe_report.hpp"
#include "slopsync/wire/messages/welcome.hpp"
#include "slopsync/wire/raw/probe.hpp"

namespace slopsync {

// ---- M4c (RFC-029 item 1): what this client believes about the MACHINE ------
//
// Read it as an answer to one question: "is the thing I am talking to the
// machine I paired with, or a clone that copied its name?" mDNS identity
// strings are claims and a clone copies them perfectly, so the only real answer
// is a signature over material the client itself contributed entropy to.
enum class HubAuthState : uint8_t {
    // This client never asked to be signed to. The overwhelmingly common case,
    // and NOT a warning: a potato paired by physical ceremony trusts the LAN
    // exactly as much as it trusts the socket, deliberately.
    NotRequested = 0,
    // Asked, but holds no pinned key — so there is nothing to check the answer
    // against. A client reaches this by requesting a signature before it has
    // ever completed a pairing ceremony (PAIR_GRANT is where the key arrives).
    // Not a failure: an unverifiable answer is not a wrong one.
    Unverifiable,
    // Asked, holds a key, waiting. Intents are NOT withheld here — a hub is
    // allowed to take tens of milliseconds to produce an ECDSA signature, and
    // withholding on Pending would make every signing hub feel broken.
    Pending,
    // A signature arrived and verified against the pinned key. This is the only
    // state that positively means "this is your machine".
    Verified,
    // A signature arrived and did NOT verify. THE EVIL-TWIN ANSWER. The
    // application MUST surface "this is not your machine"; the library withholds
    // intents by itself so that a UI which forgets to cannot drive a clone.
    Mismatch,
    // No signature arrived within limits::hub_sig_timeout_ms, from a hub whose
    // key this client holds — i.e. from a machine that demonstrably HAD a
    // keypair when it paired us. Treated exactly like Mismatch, because the
    // cheapest evil-twin strategy is silence.
    Timeout,
};

class ClientDelegate {
public:
    virtual ~ClientDelegate() = default;
    // Session lifecycle (§2.2). onLive fires after ALL retained STATE pushes
    // promised by WELCOME (limits.retained_pending) have been adopted.
    virtual void onStateChange(ClientSessionState s) = 0;
    // A STATE frame accepted by newest-wins (§7.3); payload = packed snapshot.
    virtual void onState(uint16_t channel_id, uint16_t seq, std::span<const std::byte> payload) = 0;
    // §9.3: the applied-truth echo for one of our intents.
    virtual void onEcho(uint16_t intent_id, const IntentValueMap& applied, uint16_t cfg_gen) = 0;
    virtual void onNack(const NackMsg& n) = 0;
    virtual void onEvent(uint16_t channel_id, std::span<const std::byte> encodedPayload) { (void)channel_id; (void)encodedPayload; }
    // §6.7: pending intent died with the session — reconcile at app level.
    virtual void onPendingDropped(uint16_t intent_id) = 0;

    // ---- M5 addition (§12.2), additive: default no-op keeps every existing
    // ClientDelegate valid. Fires when a PAIR_GRANT arrives for a PAIR_REQ
    // this client sent (see Client::sendPairReq) — `token` is the 16-byte
    // value to persist and present in a future HELLO's `token` field.
    virtual void onPairGrant(std::span<const std::byte> token, AccessLevel roles) { (void)token; (void)roles; }

    // ---- M4c additions (RFC-029), additive: default no-ops keep every
    // existing ClientDelegate valid.
    //
    // The hub-authenticity verdict changed. Mismatch/Timeout mean "this is not
    // your machine" and MUST be surfaced to the human; the library has already
    // stopped sending intents by the time this fires, but a UI that keeps
    // showing a live-looking machine is itself a ground-truth defect.
    virtual void onHubAuth(HubAuthState s) { (void)s; }
    // A PAIR_GRANT delivered the hub's SEC1 P-256 public key. The Client has
    // already pinned it for the rest of this process; the APPLICATION must
    // PERSIST it beside the token, because the pin is only worth anything if it
    // survives a restart — an identity re-learned on every boot is trust on
    // every use, which is not trust on first use.
    virtual void onHubPublicKey(std::span<const std::byte> sec1Pubkey) { (void)sec1Pubkey; }
    // The hub re-issued `roles` — today only in answer to a successful AUTH
    // (RFC-029 item 6's proof presentation). The client has already re-sent its
    // standing subscription wishes if the role went UP.
    virtual void onRolesChanged(AccessLevel roles) { (void)roles; }
};

class Client {
public:
    static constexpr size_t kMaxWishes = 16;

    // `crypto` (M4c addition, additive with a default) is the seam RFC-029's
    // verification rides: the library is std-headers-only and will never
    // contain P-256 math, so a client that wants to verify a hub supplies an
    // ICrypto whose verifyP256 is WebCrypto / System.Security / mbedtls. The
    // default null object answers "unsupported", which is exactly right for
    // every client that never asks to be signed to.
    Client(const ClientIdentity& id, ITransport& transport, IClock& clock,
           IRandom& rng, ClientDelegate& delegate, ICrypto& crypto = defaultCrypto());

    // Standing wish-list (§6.2): applies to the next connect() and every
    // reconnect. Returns false when kMaxWishes exceeded.
    bool addSubscriptionWish(uint16_t channel_id, float rate_hz, Priority prio);

    // Cache from a prior session (§6.7 etag-skip). All-zero = none.
    void setCachedEtag(std::span<const std::byte, limits::etag_bytes> etag);

    // Opens the transport and sends HELLO. State -> HELLO_SENT.
    bool connect();
    void disconnect();  // GOODBYE (best-effort) + close

    void update(uint32_t nowUs);

    // §9.3: send an INTENT (absolute values only — the API takes a value
    // map, never deltas). Returns the assigned intent_id (monotonic per
    // session) or nullopt when not LIVE / send failed. Pending until ECHO.
    // `takeover` (M5 addition, §11.4): forwarded as the intent's `takeover`
    // (32) flag for source-mapped channels — default false reproduces the M4
    // signature's behavior exactly (always sent explicitly false rather than
    // omitted; NackCode selection in the hub's intent pipeline only checks
    // its truth value, so "explicitly false" and "absent" are equivalent on
    // the wire for every purpose this library cares about).
    std::optional<uint16_t> sendIntent(uint16_t channel_id, const IntentValueMap& values,
                                       std::optional<uint16_t> preconditionCfgGen = std::nullopt,
                                       bool takeover = false);

    // §11.2: initiate ESTOP — sends the 12-byte frame now and re-sends every
    // limits::estop_repeat_interval_ms until the safety channel's latched
    // STATE (channel 0x0003) is observed with estop bit set and seq >= ours,
    // or limits::estop_repeat_max attempts exhaust (then estopSendFailed()).
    void initiateEstop(uint8_t cause);
    bool estopSendFailed() const;

    ClientSessionState state() const;
    uint32_t sessionId() const;
    uint16_t lastCfgGen() const;
    std::span<const std::byte> hubEtag() const;

    // ---- M4 test/observability additions (not part of the frozen sketch,
    // added because the behavioral suite needs a way to read them) ----------
    // Current known grant for `channel_id` (from WELCOME or a later GRANT),
    // nullopt if never granted. Drives S-09's "client complies" assertion.
    std::optional<float> grantedRateHz(uint16_t channel_id) const;
    AccessLevel roles() const;
    uint32_t bootId() const;
    // Count of BLOB_REQ frames this client has sent (ever). Drives S-02's
    // "no BLOB_REQ observed on a matching-etag reconnect" assertion.
    size_t catalogReqCount() const;

    // ---- M5 additions -------------------------------------------------------
    // §12.2: this session's WELCOME nonce (empty/all-zero before WELCOME) —
    // pair with wire/hmac_sha256.hpp's pairingPinProof(pin, nonce()) to build
    // the pin_proof sendPairReq() below expects.
    std::span<const std::byte> nonce() const;
    // Sends PAIR_REQ with this instance's identity + an already-computed
    // 16-byte pin_proof. Returns false on a malformed-size proof or send
    // failure. The resulting PAIR_GRANT/NACK surfaces via
    // ClientDelegate::onPairGrant / onNack.
    bool sendPairReq(std::span<const std::byte> pinProof);
    // ---- M4b (RFC-027 modes (a)/(c)): KNOCK ---------------------------------
    // A PAIR_REQ carrying NO proof. Additive; the frozen sendPairReq signature
    // above is untouched.
    //
    // THE WHOLE CEREMONY FOR A DEVICE WITH ONE BUTTON. Send this and wait: if a
    // push-to-pair window is open the grant arrives immediately, otherwise the
    // knock joins the hub's bounded pending list and an operator approves it
    // from ANY `configure` session (their phone, a CLI, the machine's own page
    // — the trusted surface is a tier, not an app). Either way the answer
    // arrives as ClientDelegate::onPairGrant.
    //
    // SILENCE IS NORMAL AND EXPECTED between the knock and the operator's
    // answer — there is no "pending" reply frame, because the device class this
    // exists for has nothing to render one on. A NACK means refused: BUSY (the
    // pending list is full, retry after `retry_after_ms`), PAIRING_REQUIRED
    // (this hub does not offer knock-and-approve) or PAIRING_DENIED.
    //
    // THE SESSION MUST STAY UP. A knock is bound to the session that made it
    // and dies with it, because a grant has to be DELIVERED and this is the
    // only channel to a client that owns no token yet. Do not knock and
    // disconnect.
    bool sendPairKnock();
    // §6.4: sends PROBE and starts measuring the hub's burst; a PROBE_REPORT
    // is sent automatically once probe_max_duration_ms has elapsed (from
    // update()). Returns false when not LIVE or the send fails.
    bool runProbe();
    // §9.1/§11.1 shadow reads of the safety channel (0x0003), extended
    // beyond M4's estop-only observation to the full bitfield word.
    std::optional<uint8_t> safetyWord() const;
    bool stopLatched() const;

    // ---- M4c additions (RFC-029) -------------------------------------------
    // Every one of these is OPT-IN with an inert default. A client that calls
    // none of them behaves byte-for-byte as it did before M4c: bearer token in
    // HELLO, no `trust` sub-map, zero crypto. THAT IS THE COVENANT — the
    // mandatory client floor does not rise.

    // HELLO `trust`.client_ver — RFC-029 item 2's change tripwire. Self-reported
    // by definition, so it catches an honest update and nothing else; the hub
    // treats it as a tripwire, never as attestation. `ver` must outlive every
    // connect() (a string literal is the intended shape).
    void setClientVersion(const char* ver);

    // RFC-029 item 1, the client half.
    //
    // `setHubPublicKey` PINS this machine's identity. The key comes from
    // PAIR_GRANT's `trust`.hub_pubkey — i.e. from the pairing ceremony, the one
    // moment physical presence was already proven — and the application
    // persists it beside the token. Pass an empty span to un-pin.
    void setHubPublicKey(std::span<const std::byte> sec1Pubkey);
    std::span<const std::byte> hubPublicKey() const;
    // Ask the hub to sign `client_nonce || session_id || boot_id`. ON REQUEST
    // because signing is expensive on parts without an ECC accelerator, so a
    // client that does not care must not make every other client's hub work.
    // Takes effect at the next connect().
    void requestHubSignature(bool on);
    HubAuthState hubAuthState() const;

    // RFC-029 item 6: how this client presents its token.
    //   presentation_modes::bearer (0, DEFAULT) — raw token in HELLO. One
    //     memcpy, zero crypto, one round trip. The potato floor.
    //   presentation_modes::proof (1) — the token stays home; HELLO carries no
    //     token at all, and after WELCOME the client sends AUTH with
    //     HMAC-SHA256(token, nonce) truncated to 16 B. Costs one extra round
    //     trip, during which the session is legitimately at `watch`.
    // Takes effect at the next connect(). Selecting proof mode without a token
    // is a no-op: there is nothing to prove.
    void setTokenPresentationMode(uint8_t mode);
    uint8_t tokenPresentationMode() const;

private:
    // CONTRACT NOTE (for the implementing pass): everything PUBLIC above,
    // including delegate interfaces and doc comments, is frozen API. This
    // private section is a starting sketch — the implementation owns its
    // final shape and adds members as needed, defining methods inline here
    // or in a companion client_impl.hpp included from slopsync.h.
    ClientIdentity _id;
    ITransport& _t;
    IClock& _clock;
    IRandom& _rng;
    ClientDelegate& _delegate;
    ICrypto& _crypto;
    MonotonicMs _monoMs;  // wrap-safe ms derivation for all deadline bookkeeping (§7.2)
    ClientSessionState _state = ClientSessionState::CLOSED;

    static constexpr size_t kMaxPendingIntents = 8;
    static constexpr size_t kMaxShadowSlots = 16;
    static constexpr size_t kMaxGrants = 16;  // mirrors wire/messages/welcome.hpp's kWelcomeMaxGrants

    // Standing wish-list (§6.2/§6.7): re-sent verbatim on every connect().
    struct Wish {
        uint16_t channel_id = 0;
        float rate_hz = 0.0f;
        Priority priority = Priority::normal;
    };
    std::array<Wish, kMaxWishes> _wishes{};
    size_t _wishCount = 0;

    std::array<std::byte, limits::etag_bytes> _cachedEtag{};   // all-zero = none (§6.7)
    std::array<std::byte, limits::etag_bytes> _hubEtag{};

    uint32_t _sessionId = 0;
    uint32_t _bootId = 0;
    uint16_t _cfgGen = 0;
    AccessLevel _roles = AccessLevel::watch;

    struct GrantEntry {
        uint16_t channel_id = 0;
        float rate_hz = 0.0f;
        uint8_t priority = 0;
        bool valid = false;
    };
    std::array<GrantEntry, kMaxGrants> _grants{};

    // §9.1/§7.3 shadow: raw payload + seq per subscribed STATE channel,
    // discarded and rebuilt from scratch on every WELCOME (§6.7).
    struct ShadowEntry {
        uint16_t channel_id = 0;
        bool used = false;
        ShadowSlot slot{};
    };
    std::array<ShadowEntry, kMaxShadowSlots> _shadows{};

    // SYNCING -> LIVE gate (§2.2/§6.3): retained_pending distinct STATE
    // channels adopted, AND (if the catalog needed fetching) verified.
    uint32_t _requiredRetained = 0;
    uint32_t _adoptedCount = 0;
    bool _catalogReady = true;

    // Catalog transfer (§8.4).
    ChunkReassembler<64> _chunkReassembler;
    uint16_t _catalogChunkCount = 0;
    size_t _catalogReqSentCount = 0;

    // ---- §8.4/RFC-015: CATALOG_READY, the client half of the readiness gate.
    // Sent once the assembled catalog has been hash-verified LOCALLY (zero
    // round trips — the etag already makes the transfer self-verifying), and
    // re-sent on the chunk-repair cadence until the first STATE arrives, since
    // on a lossy binding the declaration itself can be the thing that was
    // lost. A HELLO whose cached etag matched needs none of this: the match IS
    // the proof, and the hub was already serving us before this code ran.
    std::array<std::byte, limits::etag_bytes> _readyEtag{};  // what we declared we operate against
    bool _readyPending = false;      // declared, but no STATE seen yet -> keep re-declaring
    uint32_t _lastReadySendMs = 0;
    uint32_t _readyAttempts = 0;

    // Pending INTENT ids awaiting ECHO/NACK (§9.3, §6.7).
    std::array<uint16_t, kMaxPendingIntents> _pending{};
    size_t _pendingCount = 0;
    uint16_t _nextIntentId = 1;

    // Liveness (§6.5/§6.6, M4 minimal + the holding-control cadence switch).
    uint32_t _lastRxMs = 0;
    uint32_t _lastTxMs = 0;
    // §6.6: true once an INTENT this session sent has been ECHOed. A
    // conservative proxy for "may own an active motion source" — this class
    // has no catalog channel-class awareness to know precisely which channel
    // is source-mapped, so it errs toward the faster PING cadence rather than
    // risking the hub's 600 ms deadman firing during a real pause. Reset per
    // connect() (a new session never inherits ownership, §6.8).
    bool _holdingSource = false;

    // ESTOP repeat-until-latched (§11.2).
    bool _estopActive = false;
    bool _estopSendFailed = false;
    uint8_t _estopCause = 0;
    uint16_t _estopSentSeq = 0;
    uint16_t _estopNextSeq = 1;
    uint32_t _estopAttempts = 0;
    uint32_t _lastEstopSendMs = 0;

    // ---- M5: pairing nonce + probe state ------------------------------------
    std::array<std::byte, 8> _nonce{};  // §12.2, from the most recent WELCOME

    // ---- M4c: RFC-029 trust state ------------------------------------------
    const char* _clientVer = nullptr;
    // THE ANTI-REPLAY ENTROPY. Freshly drawn from _rng on every connect(), so
    // a signature captured from one session is material for THAT session only.
    // This is also what finally consumes `_rng` — the member clang has been
    // warning about since M4, which existed precisely for this.
    std::array<std::byte, kTrustClientNonceBytes> _clientNonce{};
    bool _sigRequested = false;
    std::array<std::byte, kTrustPubkeyMaxBytes> _hubPubkey{};
    uint8_t _hubPubkeyLen = 0;
    HubAuthState _hubAuth = HubAuthState::NotRequested;
    uint32_t _sigDeadlineMs = 0;
    uint8_t _presentationMode = 0;  // presentation_modes::bearer

    bool _probeActive = false;
    uint32_t _probeStartMs = 0;
    uint32_t _probeBytesReceived = 0;
    uint32_t _probeFramesReceived = 0;
    uint16_t _probeMaxIndexSeen = 0;
    bool _probeAnyIndexSeen = false;

    // ---- internal helpers, defined in client_impl.hpp (included below) ----
    bool sendFrame(FrameType type, uint16_t channel, std::span<const std::byte> payload);
    void flushPending();
    void handleFrame(const FrameBuffer& fb, uint32_t nowMs);
    void handleWelcome(std::span<const std::byte> payload, uint32_t nowMs);
    void handleState(uint16_t channel, uint16_t seq, std::span<const std::byte> payload, uint32_t nowMs);
    void handleEcho(std::span<const std::byte> payload);
    void handleNack(std::span<const std::byte> payload);
    void handleGrant(std::span<const std::byte> payload);
    void handleEvent(uint16_t channel, std::span<const std::byte> payload);
    void handleBlobChunk(std::span<const std::byte> payload, uint32_t nowMs);
    void handlePing(std::span<const std::byte> payload);
    void handlePairGrant(std::span<const std::byte> payload);
    // ---- M4c (RFC-029) ------------------------------------------------------
    void handleHubSig(std::span<const std::byte> payload);
    // ONE verifier, TWO arrival points (inline in WELCOME, deferred in HUB_SIG)
    // — which is what keeps the client state machine simple no matter which
    // strategy the hub picked.
    void adoptHubSignature(std::span<const std::byte> sig);
    void setHubAuth(HubAuthState s);
    void pumpHubSigTimeout(uint32_t nowMs);
    void sendAuthProof();                 // RFC-029 item 6
    void resendSubscriptionWishes();      // after a role UPGRADE
    void handleProbeFrame(std::span<const std::byte> payload);
    void pumpProbe(uint32_t nowMs);
    void sendBlobReq();
    void sendCatalogReady(std::span<const std::byte> etag);  // §8.4/RFC-015
    void pumpCatalogReady(uint32_t nowMs);                   // re-declare until STATE flows
    void checkLiveTransition();
    void pumpEstopRepeat(uint32_t nowMs);
    ShadowEntry* findOrCreateShadow(uint16_t channel_id);
    void setState(ClientSessionState s);
};

}  // namespace slopsync

#include "slopsync/client/client_impl.hpp"
