// slopsync-core — Client method definitions (SPEC §2.2, §6, §9.3, §11.2).
// Included from the bottom of client/client.hpp so a single
// `#include "slopsync/client/client.hpp"` is a complete, working Client —
// this file is never included on its own.
//
// M4 scope (see client.hpp's file-level note): HELLO/WELCOME adoption,
// catalog etag-skip or fetch+verify (§6.7, §8.4), SYNCING->LIVE on retained
// STATE adoption (§2.2), INTENT/ECHO/NACK client-side bookkeeping incl. the
// §6.7 "never blind-retransmit" reconnect doctrine, ESTOP repeat-until-latch
// (§11.2), and minimal PING liveness (§6.5). Deadman/takeover UX and pairing
// are out of scope (M5+).
#pragma once

#include <algorithm>
#include <cstring>

#include "slopsync/util/byte_io.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/estop_frame.hpp"
#include "slopsync/wire/frame_header.hpp"
#include "slopsync/wire/messages/blob_req.hpp"
#include "slopsync/wire/messages/echo.hpp"
#include "slopsync/wire/messages/goodbye.hpp"
#include "slopsync/wire/messages/grant.hpp"
#include "slopsync/wire/messages/pair.hpp"
#include "slopsync/wire/messages/probe_report.hpp"
#include "slopsync/wire/messages/subscribe.hpp"
#include "slopsync/wire/raw/catalog_ready.hpp"
#include "slopsync/wire/raw/ping_pong.hpp"
#include "slopsync/wire/raw/probe.hpp"
#include "slopsync/wire/sha256.hpp"

namespace slopsync {

namespace detail {
inline bool isAllZero(std::span<const std::byte> b) {
    for (auto x : b) {
        if (x != std::byte{0}) return false;
    }
    return true;
}
}  // namespace detail

// ---- Construction / identity / wishes ---------------------------------------

inline Client::Client(const ClientIdentity& id, ITransport& transport, IClock& clock, IRandom& rng,
                       ClientDelegate& delegate, ICrypto& crypto)
    : _id(id), _t(transport), _clock(clock), _rng(rng), _delegate(delegate), _crypto(crypto) {}

inline bool Client::addSubscriptionWish(uint16_t channel_id, float rate_hz, Priority prio) {
    if (_wishCount >= kMaxWishes) return false;
    _wishes[_wishCount++] = Wish{channel_id, rate_hz, prio};
    return true;
}

inline void Client::setCachedEtag(std::span<const std::byte, limits::etag_bytes> etag) {
    std::copy(etag.begin(), etag.end(), _cachedEtag.begin());
}

inline void Client::setState(ClientSessionState s) {
    _state = s;
    _delegate.onStateChange(s);
}

// ---- connect() / disconnect() -----------------------------------------------

inline bool Client::connect() {
    // §6.7: pending intents from whatever session existed before this call
    // are dead; the app reconciles, the library never blind-retransmits.
    flushPending();

    if (!_t.open()) return false;

    _sessionId = 0;
    _cfgGen = 0;
    for (auto& g : _grants) g = GrantEntry{};
    for (auto& s : _shadows) s = ShadowEntry{};
    _requiredRetained = 0;
    _adoptedCount = 0;
    _catalogReady = true;
    _chunkReassembler = ChunkReassembler<64>{};
    _catalogChunkCount = 0;
    _readyPending = false;
    _readyAttempts = 0;
    _estopActive = false;
    _estopSendFailed = false;
    _holdingSource = false;  // §6.8: a new session never inherits ownership

    // ---- M4c: the RFC-029 trust state, reset per session --------------------
    _hubAuth = HubAuthState::NotRequested;
    _sigDeadlineMs = 0;

    HelloMsg h{};
    h.proto_ver = kProtocolVersion;
    h.client_kind = _id.client_kind;
    h.client_name = _id.client_name;
    h.instance_id = _id.instance_id;
    // RFC-029 item 6. In PROOF mode the token DOES NOT GO ON THE WIRE at all —
    // that is the entire point, since v1 bindings are cleartext. The session
    // therefore starts at `watch` and is upgraded by AUTH after WELCOME, which
    // is the honest one-round-trip price. `bearer` (the default) is unchanged
    // and stays a memcpy: the potato floor does not move.
    const bool proofMode = (_presentationMode == presentation_modes::proof) && _id.hasToken;
    h.has_token = _id.hasToken && !proofMode;
    h.token = _id.token;
    h.has_catalog_etag = !detail::isAllZero(std::span<const std::byte>(_cachedEtag));
    h.catalog_etag = _cachedEtag;
    h.subscriptions_count = uint32_t(_wishCount);
    for (size_t i = 0; i < _wishCount; ++i) {
        h.subscriptions[i].channel_id = _wishes[i].channel_id;
        h.subscriptions[i].rate_hz = _wishes[i].rate_hz;
        h.subscriptions[i].priority = uint8_t(_wishes[i].priority);
    }

    // ---- M4c: the OPTIONAL `trust` sub-map ----------------------------------
    // Emitted only when this client actually opted into something. A client
    // that called none of the M4c setters produces a HELLO byte-identical to
    // its M4b one — no sub-map, no key 39, no extra bytes on the wire.
    if (_clientVer != nullptr) {
        h.has_trust = true;
        h.trust_map.has_client_ver = true;
        h.trust_map.client_ver = std::string_view(_clientVer);
    }
    if (_presentationMode != 0) {
        h.has_trust = true;
        h.trust_map.has_presentation_mode = true;
        h.trust_map.presentation_mode = _presentationMode;
    }
    if (_sigRequested) {
        // FRESH ENTROPY, EVERY CONNECT. This is the replay fix in one line: the
        // hub signs client_nonce || session_id || boot_id, so a signature
        // captured off a cleartext wire is bound to a nonce this client will
        // never present again. Drawing it here rather than once at construction
        // matters — a nonce reused across reconnects would make a captured
        // signature replayable at exactly the moment (a reconnect) an attacker
        // is most likely to be in position.
        _rng.fill(std::span<std::byte>(_clientNonce));
        h.has_trust = true;
        h.trust_map.has_client_nonce = true;
        h.trust_map.client_nonce = _clientNonce;
        h.trust_map.has_sig_request = true;
        h.trust_map.sig_request = true;
        // Pinned key or not decides whether silence is evidence. Without a key
        // there is nothing to verify against and no timeout is applied — a hub
        // that simply has no keypair is conformant, and treating its silence as
        // an attack would make honest hubs unusable.
        setHubAuth(_hubPubkeyLen > 0 ? HubAuthState::Pending : HubAuthState::Unverifiable);
    }

    std::array<std::byte, 700> buf{};
    size_t n = encodeHello(h, std::span<std::byte>(buf));
    if (n == 0) return false;
    if (!sendFrame(FrameType::HELLO, 0, std::span<const std::byte>(buf.data(), n))) return false;

    // The clock on "is this machine going to prove itself?" starts when the ask
    // leaves, not when WELCOME lands — a hub that never answers at all is the
    // case this is here to catch.
    if (_hubAuth == HubAuthState::Pending) _sigDeadlineMs = _clock.nowMs() + limits::hub_sig_timeout_ms;

    setState(ClientSessionState::HELLO_SENT);
    return true;
}

inline void Client::disconnect() {
    if (_state != ClientSessionState::CLOSED) {
        GoodbyeMsg gb{};
        gb.code = NackCode::NORMAL_CLOSURE;  // registry 0x0107 (allocated when this gap was flagged)
        std::array<std::byte, 64> buf{};
        size_t n = encodeGoodbye(gb, std::span<std::byte>(buf));
        if (n > 0) sendFrame(FrameType::GOODBYE, 0, std::span<const std::byte>(buf.data(), n));
    }
    flushPending();
    _t.close();
    setState(ClientSessionState::CLOSED);
}

// ---- update() ---------------------------------------------------------------
// frame pump + idle-PING liveness + ESTOP repeat

inline void Client::update(uint32_t nowUs) {
    // NOT nowUs / 1000 — see MonotonicMs in util/serial_arithmetic.hpp.
    uint32_t nowMs = _monoMs.advance(nowUs);
    while (auto fb = _t.read()) {
        handleFrame(*fb, nowMs);
    }

    if (_state != ClientSessionState::CLOSED) {
        // §6.6: tighten to ping_interval_holding_control_ms while _holdingSource
        // is set, so the client stays ahead of the hub's 600 ms deadman instead
        // of relying on ping_interval_idle_ms (1 s) the whole time.
        uint32_t pingIntervalMs =
            _holdingSource ? limits::ping_interval_holding_control_ms : limits::ping_interval_idle_ms;
        if (timeReached(nowMs, _lastTxMs + pingIntervalMs)) {
            std::array<std::byte, 8> buf{};
            size_t n = encodePing(std::span<const std::byte>(), std::span<std::byte>(buf));
            sendFrame(FrameType::PING, 0, std::span<const std::byte>(buf.data(), n));
        }
    }

    pumpEstopRepeat(nowMs);
    pumpCatalogReady(nowMs);
    pumpProbe(nowMs);
    pumpHubSigTimeout(nowMs);
}

// ---- Small send helper ------------------------------------------------------

inline bool Client::sendFrame(FrameType type, uint16_t channel, std::span<const std::byte> payload) {
    std::array<std::byte, kFrameBufferCapacity> buf{};
    FrameHeader h;
    h.type = uint8_t(type);
    h.flags = 0;
    h.channel = channel;
    h.seq = 0;
    h.len = uint16_t(payload.size());
    size_t pos = encodeFrameHeader(h, std::span<std::byte>(buf));
    if (pos == 0) return false;
    if (payload.size() > buf.size() - pos) return false;
    if (!payload.empty()) std::memcpy(buf.data() + pos, payload.data(), payload.size());
    bool ok = _t.write(std::span<const std::byte>(buf.data(), pos + payload.size()));
    if (ok) _lastTxMs = _clock.nowMs();
    return ok;
}

inline void Client::flushPending() {
    for (size_t i = 0; i < _pendingCount; ++i) _delegate.onPendingDropped(_pending[i]);
    _pendingCount = 0;
}

// ---- Frame dispatch ---------------------------------------------------------
// ESTOP magic checked BEFORE header decode (§5.5), then dispatch by type.

inline void Client::handleFrame(const FrameBuffer& fb, uint32_t nowMs) {
    std::span<const std::byte> bytes = fb.bytes();

    if (bytes.size() == kEstopFrameBytes && bytes[0] == kEstopMagicByte && bytes[1] == kEstopMagicByte &&
        bytes[2] == kEstopMagicByte && bytes[3] == kEstopMagicByte) {
        if (decodeEstop(bytes)) _lastRxMs = nowMs;  // proof of life only; the client acts on the safety STATE, not this
        return;
    }

    auto header = fb.header();
    if (!header) return;
    _lastRxMs = nowMs;
    std::span<const std::byte> payload = fb.payload();

    switch (FrameType(header->type)) {
        case FrameType::WELCOME:
            handleWelcome(payload, nowMs);
            break;
        case FrameType::STATE:
            handleState(header->channel, header->seq, payload, nowMs);
            break;
        case FrameType::ECHO:
            handleEcho(payload);
            break;
        case FrameType::NACK:
            handleNack(payload);
            break;
        case FrameType::GRANT:
            handleGrant(payload);
            break;
        case FrameType::EVENT:
            handleEvent(header->channel, payload);
            break;
        case FrameType::BLOB_CHUNK:
            handleBlobChunk(payload, nowMs);
            break;
        case FrameType::PING:
            handlePing(payload);
            break;
        case FrameType::PAIR_GRANT:
            handlePairGrant(payload);
            break;
        case FrameType::HUB_SIG:
            handleHubSig(payload);
            break;
        case FrameType::PROBE:
            handleProbeFrame(payload);
            break;
        case FrameType::GOODBYE:
            setState(ClientSessionState::CLOSED);
            flushPending();
            break;
        case FrameType::PONG:
        default:
            break;  // §4.3: unknown/unhandled frame types ignored; PONG is proof-of-life only
    }
}

// ---- WELCOME (§6.3, §6.7, §8.4) ---------------------------------------------

inline void Client::handleWelcome(std::span<const std::byte> payload, uint32_t nowMs) {
    if (_state != ClientSessionState::HELLO_SENT) return;  // tolerant: ignore stray/duplicate WELCOME
    auto res = decodeWelcome(payload);
    if (!res) return;
    const WelcomeMsg& w = res.value();

    _sessionId = w.session_id;
    _bootId = w.boot_id;
    _cfgGen = w.cfg_gen;
    _roles = AccessLevel(w.roles);
    _hubEtag = w.catalog_etag;
    _nonce = w.nonce;  // §12.2: needed for a subsequent PAIR_REQ's pin_proof

    for (auto& g : _grants) g = GrantEntry{};
    for (uint32_t i = 0; i < w.grants_count && i < kMaxGrants; ++i) {
        _grants[i] = GrantEntry{w.grants[i].channel_id, w.grants[i].granted_rate_hz, w.grants[i].priority, true};
    }

    // §6.7: snapshot adoption is mandatory — discard the shadow entirely and
    // rebuild it only from what follows this WELCOME.
    for (auto& s : _shadows) s = ShadowEntry{};
    _requiredRetained = w.limits_info.retained_pending;
    _adoptedCount = 0;

    bool matches = std::equal(_cachedEtag.begin(), _cachedEtag.end(), w.catalog_etag.begin());
    if (matches) {
        _catalogReady = true;
    } else {
        _catalogReady = false;
        _chunkReassembler = ChunkReassembler<64>{};
        _catalogChunkCount = 0;
        sendBlobReq();
    }

    // ---- M4c (RFC-029 item 1): the INLINE delivery point --------------------
    // A hub that can sign cheaply puts `welcome_sig` right here. Identical
    // material, identical verification, identical verdict as the deferred
    // HUB_SIG path — the client does not care which strategy the hub picked,
    // which is what keeps this state machine one enum wide.
    if (w.has_trust && w.trust_map.has_welcome_sig) {
        adoptHubSignature(std::span<const std::byte>(w.trust_map.welcome_sig.data(), w.trust_map.welcome_sig_len));
    }

    // ---- M4c (RFC-029 item 6): the PROOF presentation -----------------------
    // WELCOME's nonce is the message; the token (which never crossed the wire)
    // is the key. This is the extra round trip proof mode honestly costs.
    if (_presentationMode == presentation_modes::proof && _id.hasToken) sendAuthProof();

    setState(ClientSessionState::SYNCING);
    checkLiveTransition();
    (void)nowMs;
}

inline void Client::checkLiveTransition() {
    if (_state == ClientSessionState::SYNCING && _catalogReady && _adoptedCount >= _requiredRetained) {
        setState(ClientSessionState::LIVE);
    }
}

// ---- STATE shadow-apply (§9.1/§7.3) + the ESTOP repeat-until-latch observer --

inline Client::ShadowEntry* Client::findOrCreateShadow(uint16_t channel_id) {
    for (auto& e : _shadows) {
        if (e.used && e.channel_id == channel_id) return &e;
    }
    for (auto& e : _shadows) {
        if (!e.used) {
            e.used = true;
            e.channel_id = channel_id;
            e.slot = ShadowSlot{};
            return &e;
        }
    }
    return nullptr;  // capacity exhausted (16 channels) — drop silently
}

inline void Client::handleState(uint16_t channel, uint16_t seq, std::span<const std::byte> payload, uint32_t nowMs) {
    (void)nowMs;
    ShadowEntry* e = findOrCreateShadow(channel);
    if (!e) return;

    // §8.4/RFC-015: STATE arriving is proof the hub opened this client's data
    // plane, so the CATALOG_READY re-declaration loop stops here (even for a
    // frame this client then discards as stale — the gate is what it was
    // waiting on).
    _readyPending = false;

    bool wasValid = e->slot.valid;
    bool accepted = applyStateFrame(seq, payload, e->slot);
    if (!accepted) return;

    _delegate.onState(channel, seq, payload);

    if (_state == ClientSessionState::SYNCING && !wasValid) {
        ++_adoptedCount;
        checkLiveTransition();
    }

    // §11.2 repeat-until-latched: the safety channel's own estop_seq field
    // (payload offset 6, NOT this frame's transport-level `seq`) is the
    // acknowledgement initiateEstop() is waiting for.
    if (channel == channels::safety && _estopActive && payload.size() >= 8) {
        uint8_t word = uint8_t(payload[0]);
        bool estopBit = (word & 0x01u) != 0;
        uint16_t estopSeqField = getU16(payload.subspan(6, 2));
        bool reached = estopBit && (estopSeqField == _estopSentSeq || seqIsNewer(estopSeqField, _estopSentSeq));
        if (reached) {
            _estopActive = false;
            _estopSendFailed = false;
        }
    }
}

// ---- ECHO / NACK / GRANT / EVENT (§9.3, §10.2, §9.4) ------------------------

inline void Client::handleEcho(std::span<const std::byte> payload) {
    auto res = decodeEcho(payload);
    if (!res) return;
    const EchoMsg& m = res.value();

    bool found = false;
    for (size_t i = 0; i < _pendingCount; ++i) {
        if (_pending[i] == m.intent_id) {
            for (size_t j = i + 1; j < _pendingCount; ++j) _pending[j - 1] = _pending[j];
            --_pendingCount;
            found = true;
            break;
        }
    }
    if (!found) return;  // stray/duplicate ECHO for an untracked id: ignore (§4.3-style tolerance)

    _cfgGen = m.cfg_gen;
    _holdingSource = true;  // §6.6: an applied INTENT is the proxy for source ownership (see declaration)
    IntentValueMap applied{m.applied_count, m.applied};
    _delegate.onEcho(m.intent_id, applied, m.cfg_gen);
}

inline void Client::handleNack(std::span<const std::byte> payload) {
    auto res = decodeNack(payload);
    if (!res) return;
    const NackMsg& m = res.value();

    if (_state == ClientSessionState::HELLO_SENT) {
        // §2.2: HELLO_SENT -> (NACK) -> CLOSED.
        setState(ClientSessionState::CLOSED);
    }

    if (m.has_intent_id) {
        for (size_t i = 0; i < _pendingCount; ++i) {
            if (_pending[i] == m.intent_id) {
                for (size_t j = i + 1; j < _pendingCount; ++j) _pending[j - 1] = _pending[j];
                --_pendingCount;
                break;
            }
        }
    }

    _delegate.onNack(m);
}

inline void Client::handleGrant(std::span<const std::byte> payload) {
    auto res = decodeGrant(payload);
    if (!res) return;
    const GrantMsg& m = res.value();

    for (uint32_t i = 0; i < m.grants_count; ++i) {
        const Grant& g = m.grants[i];
        bool updated = false;
        for (auto& e : _grants) {
            if (e.valid && e.channel_id == g.channel_id) {
                e.rate_hz = g.granted_rate_hz;
                e.priority = g.priority;
                updated = true;
                break;
            }
        }
        if (!updated) {
            for (auto& e : _grants) {
                if (!e.valid) {
                    e = GrantEntry{g.channel_id, g.granted_rate_hz, g.priority, true};
                    break;
                }
            }
        }
    }

    // ---- M4c (RFC-029 item 6): the AUTH answer ------------------------------
    // The hub re-issued `roles`. On an UPGRADE the standing wish list is re-sent
    // as SUBSCRIBE, because the wishes in HELLO were judged against the role
    // this client had before it authenticated — the hub does not hoard rejected
    // wishes waiting for permission to show up, and SUBSCRIBE is exactly the
    // verb for asking again.
    if (m.has_roles) {
        const AccessLevel before = _roles;
        _roles = AccessLevel(m.roles);
        if (_roles != before) {
            if (uint8_t(_roles) > uint8_t(before)) resendSubscriptionWishes();
            _delegate.onRolesChanged(_roles);
        }
    }
}

inline void Client::handleEvent(uint16_t channel, std::span<const std::byte> payload) {
    _delegate.onEvent(channel, payload);
}

// ---- Catalog transfer (§8.4) ------------------------------------------------

inline void Client::sendBlobReq() {
    BlobReqMsg m{};
    m.full = true;
    std::array<std::byte, 16> buf{};
    size_t n = encodeBlobReq(m, std::span<std::byte>(buf));
    if (n > 0 && sendFrame(FrameType::BLOB_REQ, 0, std::span<const std::byte>(buf.data(), n))) {
        ++_catalogReqSentCount;
    }
}

inline void Client::handleBlobChunk(std::span<const std::byte> payload, uint32_t nowMs) {
    BlobChunkHeader h{};
    if (!getBlobChunkHeader(payload, h)) return;
    // This client transfers exactly one namespace: the catalog. A chunk from a
    // store (a preset the application asked for) belongs to that fetch's own
    // reassembler, not to this one — dropping it here is what keeps the two
    // from corrupting each other.
    if (!h.id.isCatalog()) return;
    if (h.chunk_count == 0 || h.chunk_index >= h.chunk_count) return;

    if (!_chunkReassembler.active() || _catalogChunkCount != h.chunk_count) {
        // total_bytes rides the header now (RFC-021/028: know the size before
        // you allocate), which also retires the old "remember the last chunk's
        // length and reconstruct the real total" hack — the sender simply says.
        _chunkReassembler.begin(h, nowMs);
        _catalogChunkCount = h.chunk_count;
    }
    _chunkReassembler.insert(payload, nowMs);

    if (_chunkReassembler.complete()) {
        auto bytes = _chunkReassembler.assembled();

        auto digest = Sha256::hash(bytes);
        bool match = true;
        for (size_t i = 0; i < _hubEtag.size(); ++i) {
            if (digest[i] != _hubEtag[i]) {
                match = false;
                break;
            }
        }
        // §8.5-adjacent M4 minimal policy: proceed either way (a hub that
        // refuses degraded operation is a policy this milestone doesn't
        // model) but only adopt the etag into the reconnect cache if the
        // reassembled bytes actually verified.
        if (match) _cachedEtag = _hubEtag;
        _catalogReady = true;

        // §8.4/RFC-015: the hash IS the acknowledgement — declare which
        // catalog this client now operates against so the hub opens its data
        // plane. On a verified transfer that is the hub's etag; on a transfer
        // that did NOT verify, declare what was actually assembled (the digest
        // of the received bytes), which is the honest §8.5 "degraded
        // operation" statement and is what lets the hub flag the session
        // rather than be misled.
        std::array<std::byte, limits::etag_bytes> declared{};
        for (size_t i = 0; i < declared.size(); ++i) declared[i] = match ? _hubEtag[i] : digest[i];
        sendCatalogReady(std::span<const std::byte>(declared));

        checkLiveTransition();
    }
}

// ---- CATALOG_READY (§8.4 / RFC-015) -----------------------------------------
// Declaring which catalog this client operates against.

inline void Client::sendCatalogReady(std::span<const std::byte> etag) {
    std::array<std::byte, kCatalogReadyBytes> buf{};
    size_t n = encodeCatalogReady(etag, std::span<std::byte>(buf));
    if (n == 0) return;
    std::copy(etag.begin(), etag.end(), _readyEtag.begin());
    if (sendFrame(FrameType::CATALOG_READY, 0, std::span<const std::byte>(buf.data(), n))) {
        // Pending until the hub demonstrably opened the data plane, i.e. until
        // the first STATE frame lands (handleState clears this).
        _readyPending = true;
        _lastReadySendMs = _clock.nowMs();
        ++_readyAttempts;
    }
}

inline void Client::pumpCatalogReady(uint32_t nowMs) {
    if (!_readyPending) return;
    // Bounded by the hub's own catalog_ready_timeout_ms: past that the hub has
    // already GOODBYE'd us (READY_TIMEOUT), so re-declaring is pure noise.
    constexpr uint32_t kMaxAttempts = limits::catalog_ready_timeout_ms / limits::catalog_chunk_gap_timeout_ms;
    if (_readyAttempts >= kMaxAttempts) {
        _readyPending = false;
        return;
    }
    if (!timeReached(nowMs, _lastReadySendMs + limits::catalog_chunk_gap_timeout_ms)) return;

    // Idempotent on the hub side by design — re-declaring is a flag-set, so a
    // lossy binding costs nothing but 16 bytes per repair interval.
    std::array<std::byte, kCatalogReadyBytes> buf{};
    size_t n = encodeCatalogReady(std::span<const std::byte>(_readyEtag), std::span<std::byte>(buf));
    if (n == 0) {
        _readyPending = false;
        return;
    }
    sendFrame(FrameType::CATALOG_READY, 0, std::span<const std::byte>(buf.data(), n));
    _lastReadySendMs = nowMs;
    ++_readyAttempts;
}

// ---- PING/PONG (§6.5) -------------------------------------------------------

inline void Client::handlePing(std::span<const std::byte> payload) {
    std::array<std::byte, 32> buf{};
    size_t n = encodePong(payload, std::span<std::byte>(buf));
    sendFrame(FrameType::PONG, 0, std::span<const std::byte>(buf.data(), n));
}

// ---- sendIntent (§9.3) ------------------------------------------------------

inline std::optional<uint16_t> Client::sendIntent(uint16_t channel_id, const IntentValueMap& values,
                                                   std::optional<uint16_t> preconditionCfgGen, bool takeover) {
    if (_state != ClientSessionState::LIVE) return std::nullopt;
    // RFC-029 item 1, NORMATIVE: "a clone fails the signature; the client MUST
    // surface 'this is not your machine' and withhold intents". WITHHELD HERE,
    // in the library, and not left to the application — a UI that forgets the
    // check would otherwise be driving a stranger's machine, and on THIS product
    // that is a physical-safety failure, not a data one. `Pending` is
    // deliberately NOT withheld: a hub is allowed to take tens of milliseconds
    // to sign, and a client that froze during that window would make every
    // signing hub feel broken.
    if (_hubAuth == HubAuthState::Mismatch || _hubAuth == HubAuthState::Timeout) return std::nullopt;
    if (_pendingCount >= kMaxPendingIntents) return std::nullopt;

    IntentMsg m{};
    m.channel_id = channel_id;
    m.intent_id = _nextIntentId;
    m.value_count = values.count;
    m.value = values.fields;
    if (preconditionCfgGen) {
        m.has_precondition = true;
        m.precondition = *preconditionCfgGen;
    }
    m.has_takeover = true;
    m.takeover = takeover;

    std::array<std::byte, 300> buf{};
    size_t n = encodeIntent(m, std::span<std::byte>(buf));
    if (n == 0) return std::nullopt;
    if (!sendFrame(FrameType::INTENT, channel_id, std::span<const std::byte>(buf.data(), n))) return std::nullopt;

    uint16_t id = _nextIntentId;
    _pending[_pendingCount++] = id;
    ++_nextIntentId;
    if (_nextIntentId == 0) _nextIntentId = 1;  // monotonic per session; avoid 0 on the (distant) wrap
    return id;
}

// ---- ESTOP initiate + repeat (§11.2) ----------------------------------------

inline void Client::initiateEstop(uint8_t cause) {
    _estopActive = true;
    _estopSendFailed = false;
    _estopCause = cause;
    _estopSentSeq = _estopNextSeq++;
    if (_estopNextSeq == 0) _estopNextSeq = 1;
    _estopAttempts = 0;

    EstopFrame f;
    f.cause = cause;
    f.origin = uint8_t(_roles);
    f.seq = _estopSentSeq;
    std::array<std::byte, kEstopFrameBytes> buf{};
    size_t n = encodeEstop(f, std::span<std::byte>(buf));
    if (n > 0) {
        _t.write(std::span<const std::byte>(buf.data(), n));
        _lastTxMs = _clock.nowMs();
    }
    _estopAttempts = 1;
    _lastEstopSendMs = _clock.nowMs();
}

inline void Client::pumpEstopRepeat(uint32_t nowMs) {
    if (!_estopActive) return;
    if (_estopAttempts >= limits::estop_repeat_max) {
        _estopActive = false;
        _estopSendFailed = true;
        return;
    }
    if (!timeReached(nowMs, _lastEstopSendMs + limits::estop_repeat_interval_ms)) return;

    EstopFrame f;
    f.cause = _estopCause;
    f.origin = uint8_t(_roles);
    f.seq = _estopSentSeq;
    std::array<std::byte, kEstopFrameBytes> buf{};
    size_t n = encodeEstop(f, std::span<std::byte>(buf));
    if (n > 0) {
        _t.write(std::span<const std::byte>(buf.data(), n));
        _lastTxMs = nowMs;
    }
    ++_estopAttempts;
    _lastEstopSendMs = nowMs;
}

inline bool Client::estopSendFailed() const { return _estopSendFailed; }

// ---- Accessors --------------------------------------------------------------

inline ClientSessionState Client::state() const { return _state; }
inline uint32_t Client::sessionId() const { return _sessionId; }
inline uint16_t Client::lastCfgGen() const { return _cfgGen; }
inline std::span<const std::byte> Client::hubEtag() const { return std::span<const std::byte>(_hubEtag); }
inline AccessLevel Client::roles() const { return _roles; }
inline uint32_t Client::bootId() const { return _bootId; }

inline std::optional<float> Client::grantedRateHz(uint16_t channel_id) const {
    for (const auto& e : _grants) {
        if (e.valid && e.channel_id == channel_id) return e.rate_hz;
    }
    return std::nullopt;
}

inline size_t Client::catalogReqCount() const { return _catalogReqSentCount; }

// ---- M5: pairing (§12.2) ----------------------------------------------------

inline std::span<const std::byte> Client::nonce() const { return std::span<const std::byte>(_nonce); }

inline bool Client::sendPairReq(std::span<const std::byte> pinProof) {
    if (pinProof.size() != kPinProofBytes) return false;

    PairReqMsg m{};
    m.instance_id = _id.instance_id;
    m.has_pin_proof = true;
    std::memcpy(m.pin_proof.data(), pinProof.data(), pinProof.size());

    std::array<std::byte, 64> buf{};
    size_t n = encodePairReq(m, std::span<std::byte>(buf));
    if (n == 0) return false;
    return sendFrame(FrameType::PAIR_REQ, 0, std::span<const std::byte>(buf.data(), n));
}

inline bool Client::sendPairKnock() {
    PairReqMsg m{};
    m.instance_id = _id.instance_id;  // has_pin_proof stays false: that IS the knock
    std::array<std::byte, 64> buf{};
    size_t n = encodePairReq(m, std::span<std::byte>(buf));
    if (n == 0) return false;
    return sendFrame(FrameType::PAIR_REQ, 0, std::span<const std::byte>(buf.data(), n));
}

inline void Client::handlePairGrant(std::span<const std::byte> payload) {
    auto res = decodePairGrant(payload);
    if (!res) return;
    const PairGrantMsg& m = res.value();
    // ---- M4c (RFC-029 item 1): TOFU AT A VERIFIED MOMENT --------------------
    // The hub's durable identity arrives HERE and nowhere else: the pairing
    // ceremony is the one moment physical presence was already proven, so it is
    // the only moment at which "whatever key is handed over is the right key" is a
    // defensible assumption. Pinning it at an arbitrary later connection would
    // be trust-on-first-CONNECT, which an evil twin satisfies trivially.
    if (m.has_trust && m.trust_map.has_hub_pubkey) {
        std::span<const std::byte> pk(m.trust_map.hub_pubkey.data(), m.trust_map.hub_pubkey_len);
        setHubPublicKey(pk);
        _delegate.onHubPublicKey(pk);
    }
    _delegate.onPairGrant(std::span<const std::byte>(m.token), AccessLevel(m.roles));
}

// ---- M4c: RFC-029 item 1 (hub authenticity) + item 6 (token proof presentation) --

inline void Client::setClientVersion(const char* ver) { _clientVer = ver; }

inline void Client::setHubPublicKey(std::span<const std::byte> sec1Pubkey) {
    if (sec1Pubkey.empty() || sec1Pubkey.size() > _hubPubkey.size()) {
        _hubPubkeyLen = 0;  // un-pin: an over-size key is not a key
        return;
    }
    for (size_t i = 0; i < sec1Pubkey.size(); ++i) _hubPubkey[i] = sec1Pubkey[i];
    _hubPubkeyLen = uint8_t(sec1Pubkey.size());
}

inline std::span<const std::byte> Client::hubPublicKey() const {
    return std::span<const std::byte>(_hubPubkey.data(), _hubPubkeyLen);
}

inline void Client::requestHubSignature(bool on) { _sigRequested = on; }
inline HubAuthState Client::hubAuthState() const { return _hubAuth; }

inline void Client::setTokenPresentationMode(uint8_t mode) { _presentationMode = mode; }
inline uint8_t Client::tokenPresentationMode() const { return _presentationMode; }

inline void Client::setHubAuth(HubAuthState s) {
    if (_hubAuth == s) return;
    _hubAuth = s;
    _delegate.onHubAuth(s);
}

// THE VERIFIER. One function, reached from WELCOME (inline delivery) and from
// HUB_SIG (deferred delivery), so the two hub strategies are literally
// indistinguishable to everything above this line.
inline void Client::adoptHubSignature(std::span<const std::byte> sig) {
    // First valid answer wins; a second is ignored rather than allowed to
    // downgrade a Verified session. Otherwise a hub (or an attacker who can
    // inject one frame) could flip an already-verified client to Mismatch by
    // sending a second, bogus HUB_SIG.
    if (_hubAuth == HubAuthState::Verified || _hubAuth == HubAuthState::Mismatch) return;
    if (!_sigRequested) return;  // unsolicited signature: nothing was asked, nothing is claimed
    if (_hubPubkeyLen == 0) {
        setHubAuth(HubAuthState::Unverifiable);
        return;
    }
    if (sig.empty()) return;

    const auto material =
        hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(_clientNonce), _sessionId, _bootId);
    // THE REPLAY FENCE IS THE MATERIAL, NOT THE CHECK. `material` contains this
    // client's fresh nonce AND this session's id AND this boot's id, so a
    // signature captured from any earlier session verifies against bytes that
    // no longer exist. The verify below is ordinary; what makes it unforgeable
    // by an evil twin is what it is asked about.
    const bool ok = _crypto.verifyP256(hubPublicKey(), std::span<const std::byte>(material), sig);
    setHubAuth(ok ? HubAuthState::Verified : HubAuthState::Mismatch);
}

inline void Client::handleHubSig(std::span<const std::byte> payload) {
    auto res = decodeHubSig(payload);
    if (!res || !res.value().trust_map.has_welcome_sig) return;
    const TrustMap& t = res.value().trust_map;
    adoptHubSignature(std::span<const std::byte>(t.welcome_sig.data(), t.welcome_sig_len));
}

inline void Client::pumpHubSigTimeout(uint32_t nowMs) {
    if (_hubAuth != HubAuthState::Pending) return;
    if (!timeReached(nowMs, _sigDeadlineMs)) return;
    // SILENCE FROM A HUB WHOSE KEY WE HOLD IS AN ANSWER. We only ever pinned a
    // key because that machine handed us one in PAIR_GRANT, which means it HAD
    // a keypair — so "asked, never answered" is either a clone that cannot
    // sign, or a machine that has lost its identity. Both mean: do not drive it.
    setHubAuth(HubAuthState::Timeout);
}

inline void Client::sendAuthProof() {
    if (!_id.hasToken) return;
    std::array<std::byte, 32> mac{};
    if (!_crypto.hmacSha256(std::span<const std::byte>(_id.token), std::span<const std::byte>(_nonce),
                            std::span<std::byte>(mac))) {
        return;
    }
    AuthMsg m{};
    m.trust_map.has_token_proof = true;
    for (size_t i = 0; i < kTrustTokenProofBytes; ++i) m.trust_map.token_proof[i] = mac[i];
    m.trust_map.has_presentation_mode = true;
    m.trust_map.presentation_mode = uint8_t(presentation_modes::proof);

    std::array<std::byte, 64> buf{};
    const size_t n = encodeAuth(m, std::span<std::byte>(buf));
    if (n == 0) return;
    sendFrame(FrameType::AUTH, 0, std::span<const std::byte>(buf.data(), n));
}

inline void Client::resendSubscriptionWishes() {
    if (_wishCount == 0) return;
    SubscribeMsg m{};
    m.subscriptions_count = uint32_t(_wishCount < kSubscribeMaxWishes ? _wishCount : kSubscribeMaxWishes);
    for (uint32_t i = 0; i < m.subscriptions_count; ++i) {
        m.subscriptions[i].channel_id = _wishes[i].channel_id;
        m.subscriptions[i].rate_hz = _wishes[i].rate_hz;
        m.subscriptions[i].priority = uint8_t(_wishes[i].priority);
    }
    std::array<std::byte, 400> buf{};
    const size_t n = encodeSubscribe(m, std::span<std::byte>(buf));
    if (n == 0) return;
    // Wishes already granted are re-granted identically (upsert), so re-sending
    // the WHOLE list is idempotent and needs no diff — the hub re-authorizes
    // every entry from scratch against the role it now sees.
    sendFrame(FrameType::SUBSCRIBE, 0, std::span<const std::byte>(buf.data(), n));
}

// ---- M5: network probe (§6.4) -----------------------------------------------
// client side: request the burst, measure it, report back.

inline bool Client::runProbe() {
    if (_state != ClientSessionState::LIVE) return false;
    if (!sendFrame(FrameType::PROBE, 0, std::span<const std::byte>())) return false;

    _probeActive = true;
    _probeStartMs = _clock.nowMs();
    _probeBytesReceived = 0;
    _probeFramesReceived = 0;
    _probeMaxIndexSeen = 0;
    _probeAnyIndexSeen = false;
    return true;
}

inline void Client::handleProbeFrame(std::span<const std::byte> payload) {
    if (!_probeActive) return;  // a stray/late burst frame after reporting already happened: ignore (§4.3-style tolerance)
    auto idx = decodeProbeFrame(payload);
    if (!idx) return;

    ++_probeFramesReceived;
    _probeBytesReceived += uint32_t(payload.size());
    if (!_probeAnyIndexSeen || idx.value() > _probeMaxIndexSeen) {
        _probeMaxIndexSeen = idx.value();
        _probeAnyIndexSeen = true;
    }
}

inline void Client::pumpProbe(uint32_t nowMs) {
    if (!_probeActive) return;
    if (!timeReached(nowMs, _probeStartMs + limits::probe_max_duration_ms)) return;

    // Finalize and report (§6.4 step 2): loss vs the highest index observed
    // — indices are 0-based and monotonic within one burst, so
    // (maxIndexSeen + 1) is how many frames the hub actually sent toward us.
    uint32_t expected = _probeAnyIndexSeen ? uint32_t(_probeMaxIndexSeen) + 1 : 0;
    uint32_t lost = (expected > _probeFramesReceived) ? (expected - _probeFramesReceived) : 0;

    ProbeReportMsg m{};
    m.probe_result.bytes_received = _probeBytesReceived;
    m.probe_result.span_ms = timeDelta(nowMs, _probeStartMs) > 0 ? uint32_t(timeDelta(nowMs, _probeStartMs)) : 0;
    m.probe_result.loss_pct_x100 = expected > 0 ? uint32_t((uint64_t(lost) * 10000u) / expected) : 0;
    m.probe_result.rtt_ms = 0;  // not measured by this minimal client-side path (see runProbe()'s doc comment)

    std::array<std::byte, 64> buf{};
    size_t n = encodeProbeReport(m, std::span<std::byte>(buf));
    if (n > 0) sendFrame(FrameType::PROBE_REPORT, 0, std::span<const std::byte>(buf.data(), n));

    _probeActive = false;
}

// ---- M5: safety shadow accessors (§9.1/§11.1), extended from M4's estop-only read --

inline std::optional<uint8_t> Client::safetyWord() const {
    for (const auto& e : _shadows) {
        if (e.used && e.channel_id == channels::safety && e.slot.valid && e.slot.size >= 1) {
            return uint8_t(e.slot.value[0]);
        }
    }
    return std::nullopt;
}

inline bool Client::stopLatched() const {
    auto w = safetyWord();
    return w.has_value() && (*w & safety_bits::STOP) != 0;
}

}  // namespace slopsync
