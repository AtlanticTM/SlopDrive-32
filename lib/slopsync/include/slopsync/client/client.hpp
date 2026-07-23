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
#include "slopsync/core/result.hpp"
#include "slopsync/core/rng.hpp"
#include "slopsync/session/safety.hpp"
#include "slopsync/session/session.hpp"
#include "slopsync/transport/transport.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/catalog_chunks.hpp"
#include "slopsync/wire/frame_buffer.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/intent.hpp"
#include "slopsync/wire/messages/nack.hpp"
#include "slopsync/wire/messages/pair.hpp"
#include "slopsync/wire/messages/probe_report.hpp"
#include "slopsync/wire/messages/welcome.hpp"
#include "slopsync/wire/raw/probe.hpp"

namespace slopsync {

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
};

class Client {
public:
    static constexpr size_t kMaxWishes = 16;

    Client(const ClientIdentity& id, ITransport& transport, IClock& clock,
           IRandom& rng, ClientDelegate& delegate);

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
    // Count of CATALOG_REQ frames this client has sent (ever). Drives S-02's
    // "no CATALOG_REQ observed on a matching-etag reconnect" assertion.
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
    // §6.4: sends PROBE and starts measuring the hub's burst; a PROBE_REPORT
    // is sent automatically once probe_max_duration_ms has elapsed (from
    // update()). Returns false when not LIVE or the send fails.
    bool runProbe();
    // §9.1/§11.1 shadow reads of the safety channel (0x0003), extended
    // beyond M4's estop-only observation to the full bitfield word.
    std::optional<uint8_t> safetyWord() const;
    bool stopLatched() const;

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
    AccessLevel _roles = AccessLevel::viewer;

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
    uint16_t _lastChunkLen = 0;
    size_t _catalogReqSentCount = 0;

    // Pending INTENT ids awaiting ECHO/NACK (§9.3, §6.7).
    std::array<uint16_t, kMaxPendingIntents> _pending{};
    size_t _pendingCount = 0;
    uint16_t _nextIntentId = 1;

    // Liveness (§6.5, M4 minimal).
    uint32_t _lastRxMs = 0;
    uint32_t _lastTxMs = 0;

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
    void handleCatalogChunk(std::span<const std::byte> payload, uint32_t nowMs);
    void handlePing(std::span<const std::byte> payload);
    void handlePairGrant(std::span<const std::byte> payload);
    void handleProbeFrame(std::span<const std::byte> payload);
    void pumpProbe(uint32_t nowMs);
    void sendCatalogReq();
    void checkLiveTransition();
    void pumpEstopRepeat(uint32_t nowMs);
    ShadowEntry* findOrCreateShadow(uint16_t channel_id);
    void setState(ClientSessionState s);
};

}  // namespace slopsync

#include "slopsync/client/client_impl.hpp"
