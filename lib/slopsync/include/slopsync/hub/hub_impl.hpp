// slopsync-core — Hub method definitions (SPEC §2.2, §6, §9, §10, §11.2).
// Included from the bottom of hub/hub.hpp so a single
// `#include "slopsync/hub/hub.hpp"` is a complete, working Hub — this file
// is never included on its own.
//
// M4 scope (see hub.hpp's file-level note): full session engine — HELLO/
// WELCOME/duplicate-instance/BUSY admission (§6.3), mid-session SUBSCRIBE/
// UNSUBSCRIBE (§6.6), retained-STATE push + ongoing pacing (§9.1), INTENT/
// ECHO/NACK in the exact §9.3 order, catalog transfer (§8.4), PING/PONG
// liveness (§6.5), and the ESTOP latch + critical-priority broadcast
// (§11.2). Deadman policy dispatch, takeover, congestion-driven re-grant,
// and pairing are M5.
#pragma once

#include <algorithm>
#include <cstring>

#include "slopsync/util/byte_io.hpp"
#include "slopsync/util/serial_arithmetic.hpp"
#include "slopsync/wire/blob_chunks.hpp"
#include "slopsync/wire/catalog_codec.hpp"
#include "slopsync/wire/frame_buffer.hpp"
#include "slopsync/wire/frame_header.hpp"
#include "slopsync/wire/messages/blob_req.hpp"
#include "slopsync/wire/messages/echo.hpp"
#include "slopsync/wire/messages/event.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/pair.hpp"
#include "slopsync/wire/messages/probe_report.hpp"
#include "slopsync/wire/messages/publish.hpp"
#include "slopsync/wire/messages/subscribe.hpp"
#include "slopsync/wire/raw/catalog_ready.hpp"
#include "slopsync/wire/raw/clock_frame.hpp"
#include "slopsync/wire/raw/ping_pong.hpp"
#include "slopsync/wire/raw/probe.hpp"
#include "slopsync/wire/sha256.hpp"

namespace slopsync {

// NACK BUSY retry-after hint (registry limits::busy_retry_after_default_ms —
// promoted from a code-local M4 constant when the gap was flagged).
inline constexpr uint32_t kHubBusyRetryAfterMs = limits::busy_retry_after_default_ms;

// ---- Construction / attach / detach -----------------------------------------

inline Hub::Hub(const Catalog32& catalog, IClock& clock, IRandom& rng, HubDelegate& delegate, ICrypto& crypto)
    : _catalog(catalog), _clock(clock), _rng(rng), _delegate(delegate), _crypto(crypto) {
    _catalogEncodedLen = encodeCatalog(_catalog, std::span<std::byte>(_catalogEncoded));
    auto digest = Sha256::hash(std::span<const std::byte>(_catalogEncoded.data(), _catalogEncodedLen));
    for (size_t i = 0; i < _etag.size(); ++i) _etag[i] = digest[i];

    // boot_id: random non-zero (§6.1).
    do {
        _bootId = _rng.nextU32();
    } while (_bootId == 0);

    // Seed the retained SAFETY snapshot (all-clear) when the catalog declares
    // the channel. Safety is hub-owned and otherwise edge-driven (published
    // on latch/clear), so without this a fresh boot held NO retained value
    // for 0x0003 and a subscriber's §9.1 "retained value immediately upon
    // grant" push never happened — caught live by the conformance probe on
    // the first real hardware session.
    if (_catalog.find(channels::safety) != nullptr) {
        auto snapshot = buildSafetyPayload();
        _retained.publish(channels::safety, std::span<const std::byte>(snapshot));
    }

    // M4b: learn the trust ledger's store id FROM THE CATALOG rather than from
    // a registry constant. The catalog is self-describing; a store number is
    // agreed by being published, not legislated. A hub that declares no
    // paired-devices entry simply has no trust store and every non-catalog blob
    // request keeps falling through to the delegate exactly as before.
    if (const CatalogEntry* pd = _catalog.find(channels::paired_devices)) {
        if (const StoreDescriptor* sd = _catalog.storeDescriptor(*pd)) {
            _pairedStoreId = sd->storeId;
            _hasPairedStore = true;
        }
    }

    // Seed the retained pending-pairing / roster snapshots for the same reason
    // the safety snapshot is seeded: they are edge-driven state, so without a
    // seed a subscriber's §9.1 "retained value immediately upon grant" push
    // would never happen on a fresh boot and a configure client would show an
    // EMPTY approval list it had never actually been told was empty. That
    // distinction — "nothing pending" versus "no idea" — is exactly the
    // ground-truth doctrine.
    publishPendingPairingState(0);
    publishPairedRosterState();
}

inline bool Hub::attachTransport(ITransport& t) {
    // A genuinely-free slot only: no session at all. A STALE session's
    // transport is null by definition (RFC-042's silence/detach triggers) so
    // it LOOKS exactly like a free slot on `transport == nullptr` alone — that
    // was the bug: a brand-new, identity-unrelated connection would win the
    // race against handleHello()'s own identity-matched reattach path
    // (findSlotByInstance()/handleReattach()), which never gets a chance to
    // run because the STALE slot is gone by the time HELLO arrives. STALE
    // slots stay `occupied()` on purpose (session.hpp) precisely so this loop
    // can tell the two apart.
    for (auto& slot : _slots) {
        if (slot.transport == nullptr && !slot.session.occupied()) {
            if (!t.open()) return false;
            slot.transport = &t;
            return true;
        }
    }
    // No free slot: fall back to the SAME oldest-parked eviction policy
    // RFC-042 item 5 already uses under HELLO slot-pressure
    // (findEvictableStale()) — reusing it rather than inventing a second
    // reclaim rule for the identical "slots full, something STALE must yield"
    // situation.
    if (Slot* victim = findEvictableStale(nullptr)) {
        // A STALE session's transport is USUALLY already null (the
        // out-of-band-detach trigger clears it), but a deadman/idle-reap
        // staleness can leave one still attached and never formally detached
        // — sever it first, same as handleReattach()'s path-B severing of
        // `stale.transport`, so it isn't silently overwritten and leaked.
        if (victim->transport != nullptr) {
            victim->transport->close();
            victim->transport = nullptr;
        }
        teardownSession(*victim, _clock.nowMs());
        if (!t.open()) return false;
        victim->transport = &t;
        return true;
    }
    return false;
}

inline void Hub::detachTransport(ITransport& t) {
    for (auto& slot : _slots) {
        if (slot.transport == &t) {
            // RFC-042's third staleness trigger: "transport reports closed/
            // errored out of band" — the case that matters most for a genuine
            // WiFi blip, and unlike silence it is DETECTED, not timed out.
            // Ownership is released (RFC-045: no stop latch, exactly like the
            // silence triggers) but the slot — session_id, grants, intent ring
            // — is RETAINED, not freed: a reconnecting client reattaches via
            // handleHello()'s §6.3 migration path instead of a full
            // HELLO/WELCOME/catalog cycle. Skip the already-STALE case (a
            // formal detach arriving for a slot idle-reaped/deadmanned
            // earlier) so a late transport-layer cleanup doesn't reset
            // staleSinceMs and unfairly un-age it for RFC-042 item 5's
            // eviction tie-break. No nowMs is threaded to detach (the
            // transport layer, not update(), drives it), so read the injected
            // clock — same as latchEstop().
            if (slot.session.occupied() && slot.session.state != HubSessionState::STALE) {
                markStale(slot, _clock.nowMs(), /*reason=*/4 /*session-loss-release*/);
                // The transport is CONFIRMED gone here — unlike the silence
                // triggers (where it might still be attached), RFC-042's
                // "kept while stale" table scopes pending-knock/AUTH/blob
                // state to "if the transport itself is still attached". Reset
                // it, exactly like handleReattach()'s path-B reset: it was
                // mid-flight against a socket that no longer exists.
                bool droppedAny = false;
                _pairing.pending().dropBySession(slot.session.session_id, [&](const PendingKnock& k) {
                    droppedAny = true;
                    emitPairingEvent(pairing_events::expired, std::span<const std::byte>(k.instance_id),
                                     k.name.view(), k.mode, AccessLevel::watch, {}, _clock.nowMs());
                });
                if (droppedAny) publishPendingPairingState(_clock.nowMs());
                slot.hasClientNonce = false;
                slot.clientNonce.fill(std::byte{0});
                slot.sigRequested = false;
                slot.signPending = false;
                slot.signDelivered = false;
                slot.authFailures = 0;
                slot.blob = typename Slot::PendingBlob{};
            }
            t.close();
            slot.transport = nullptr;
            return;
        }
    }
}

inline Hub::Slot* Hub::attachedSlotFor(ITransport& t) {
    for (auto& slot : _slots) {
        if (slot.transport == &t) return &slot;
    }
    return nullptr;
}

// ---- update() ---------------------------------------------------------------
// the frame pump + STATE pacing walk

inline void Hub::update(uint32_t nowUs) {
    // NOT nowUs / 1000: the quotient of a wrapping counter is not itself a
    // mod-2^32 counter, and every ms deadline below relies on timeReached()'s
    // wrap window (see MonotonicMs in util/serial_arithmetic.hpp).
    uint32_t nowMs = _monoMs.advance(nowUs);
    // M4b: knock windows and the push-to-pair window are hub-wide, not
    // per-slot, so they are pumped ONCE here rather than inside the slot walk.
    // Before the walk, so an expiry that frees a pending slot is visible to a
    // knock arriving in this very update().
    pumpPairing(nowMs);
    for (auto& slot : _slots) {
        if (slot.transport == nullptr) continue;
        pumpSlot(slot, nowMs);
        if (slot.session.occupied()) {
            pumpDeadman(slot, nowMs);  // §11.3: may free this very slot — re-check occupied() below
        }
        if (slot.session.occupied()) {
            pumpReadyTimeout(slot, nowMs);  // RFC-015: may also free this slot
        }
        if (slot.session.occupied()) {
            pumpIdleReap(slot, nowMs);  // RFC-024: may also free this slot
        }
        // RE-CHECK THE TRANSPORT, not just occupancy (field bug #5). The
        // pumps above can free a slot, and an APPLICATION can legitimately
        // detach a transport from another task between the top-of-loop check
        // and here -- pumpStatePacing dereferences slot.transport several calls
        // deep (sendFrameTo takes ITransport&), so a null there is a
        // LoadProhibited panic rather than a missed frame.
        //
        // The firmware's own fix is to defer attach/detach onto the hub task,
        // and that is the right fix for the RACE. This check is the library
        // refusing to be crashable by a transport that gets it wrong: the cost
        // is one load per slot per tick, and the alternative is a panic in
        // somebody else's integration that looks like a hub bug.
        if (slot.session.occupied() && slot.transport != nullptr) {
            pumpStatePacing(slot, nowMs);
            pumpEventDrain(slot);
            // §8.4/RFC-021: the BULK plane goes LAST, and deliberately. A blob
            // transfer is the only unbounded-length thing the hub emits, so it
            // drains after safety/STATE/EVENT have taken their pick of this
            // tick's link capacity — and it stops the instant the transport
            // pushes back. NOT gated on RFC-015 readiness: the catalog transfer
            // is what a session does BEFORE it can be ready.
            pumpBlobTransfer(slot);
            // Liveness (§6.5, M4 minimal): reply is event-driven (PING->PONG,
            // handlePing); the hub does not itself originate PING.
        }
    }
}

// ---- Frame pump for one slot ------------------------------------------------
// ESTOP magic checked BEFORE header decode (§5.5), then normal header dispatch.

inline void Hub::pumpSlot(Slot& slot, uint32_t nowMs) {
    while (auto fb = slot.transport->read()) {
        std::span<const std::byte> bytes = fb->bytes();

        if (bytes.size() == kEstopFrameBytes && bytes[0] == kEstopMagicByte && bytes[1] == kEstopMagicByte &&
            bytes[2] == kEstopMagicByte && bytes[3] == kEstopMagicByte) {
            auto decoded = decodeEstop(bytes);
            if (decoded) {
                if (slot.session.occupied()) slot.session.lastRxMs = nowMs;
                reviveIfStale(slot, nowMs);  // RFC-042 path A: any frame is proof of life
                handleEstopFrame(decoded.value(), nowMs);
            }
            // BadCrc: silently drop (§5.5) — not a real ESTOP, never acted on.
            continue;
        }

        auto header = decodeFrameHeader(bytes);
        if (!header) continue;  // too short to be a frame at all: drop
        if (slot.session.occupied()) slot.session.lastRxMs = nowMs;  // §6.5: any rx is proof of life
        // RFC-042 path A: a STALE session's own transport reviving on ANY
        // frame (a PING is enough) — the dominant resumption case, since
        // backgrounding/locking a screen throttles JS timers without closing
        // the socket. Before dispatch, so a HELLO arriving here (a client
        // choosing to fully reconnect anyway) still sees a coherent state.
        reviveIfStale(slot, nowMs);
        dispatchFrame(slot, *header, fb->payload(), nowMs);
    }
}

inline void Hub::dispatchFrame(Slot& slot, const FrameHeader& h, std::span<const std::byte> payload, uint32_t nowMs) {
    // RFC-001: every NACK emitted below is provoked by THIS frame, so stamp
    // its seq centrally (sendNack/sendNackTracked read it) rather than
    // threading a seq argument through nine handlers and hoping none forgets.
    _dispatchSeq = h.seq;
    _dispatchSeqValid = true;
    struct SeqScope {
        bool& flag;
        ~SeqScope() { flag = false; }
    } seqScope{_dispatchSeqValid};

    switch (FrameType(h.type)) {
        case FrameType::HELLO:
            handleHello(slot, payload, nowMs);
            break;
        case FrameType::SUBSCRIBE:
            if (slot.session.occupied()) handleSubscribe(slot, payload, nowMs);
            break;
        case FrameType::UNSUBSCRIBE:
            if (slot.session.occupied()) handleUnsubscribe(slot, payload);
            break;
        case FrameType::PUBLISH:
            // §6.6/RFC-013: SUBSCRIBE's c2h counterpart for publish wishes.
            if (slot.session.occupied()) handlePublish(slot, payload, nowMs);
            break;
        case FrameType::CATALOG_READY:
            // §8.4/RFC-015: raw plane, idempotent, never NACKed.
            if (slot.session.occupied()) handleCatalogReady(slot, payload);
            break;
        case FrameType::INTENT:
            if (slot.session.occupied()) handleIntent(slot, payload, nowMs);
            break;
        case FrameType::STREAM:
            // §9.2: inbound (c2h) motion-input bundles. Header carries the
            // channel id. Never ACKed — validation failures drop silently.
            if (slot.session.occupied()) handleStream(slot, h, payload, nowMs);
            break;
        case FrameType::PING:
            if (slot.session.occupied()) handlePing(slot, payload);
            break;
        case FrameType::CLOCK:
            // §7.1 hub-time exchange. Header-framed like PING (the legacy
            // "0x05" byte IS this frame's header type — see clock_frame.hpp);
            // the 8-byte header already passed decode in pumpSlot(), so this
            // never needed pre-header special-casing the way ESTOP does.
            if (slot.session.occupied()) handleClock(slot, payload);
            break;
        case FrameType::GOODBYE:
            if (slot.session.occupied()) handleGoodbye(slot, nowMs);
            break;
        case FrameType::BLOB_REQ:
            if (slot.session.occupied()) handleBlobReq(slot, payload);
            break;
        case FrameType::PAIR_REQ:
            if (slot.session.occupied()) handlePairReq(slot, payload, nowMs);
            break;
        case FrameType::AUTH:
            // RFC-029 item 6. NOT gated on READY (RFC-015 gates the DATA
            // planes): authenticating is session grammar, and making a client
            // finish adopting a catalog before it may prove who it is would
            // leave it at `watch` for the whole of SYNCING for no security gain.
            if (slot.session.occupied()) handleAuth(slot, payload, nowMs);
            break;
        case FrameType::PROBE:
            if (slot.session.occupied()) handleProbeRequest(slot, nowMs);
            break;
        case FrameType::PROBE_REPORT:
            if (slot.session.occupied()) handleProbeReportFrame(slot, payload);
            break;
        case FrameType::PONG:
        default:
            // §4.3: unknown/unhandled frame types are silently ignored. PONG
            // carries no further action beyond the liveness stamp already
            // applied in pumpSlot().
            break;
    }
}

// ---- Small send helpers -----------------------------------------------------

inline bool Hub::sendFrameTo(ITransport& t, FrameType type, uint16_t channel, std::span<const std::byte> payload,
                              uint16_t seq) const {
    std::array<std::byte, kFrameBufferCapacity> buf{};
    FrameHeader h;
    h.type = uint8_t(type);
    h.flags = 0;
    h.channel = channel;
    h.seq = seq;
    h.len = uint16_t(payload.size());
    size_t pos = encodeFrameHeader(h, std::span<std::byte>(buf));
    if (pos == 0) return false;
    if (payload.size() > buf.size() - pos) return false;
    if (!payload.empty()) std::memcpy(buf.data() + pos, payload.data(), payload.size());
    return t.write(std::span<const std::byte>(buf.data(), pos + payload.size()));
}

inline void Hub::sendNack(ITransport& t, const NackMsg& n) const {
    // RFC-001: correlate to the inbound frame being refused (see _dispatchSeq).
    NackMsg stamped = n;
    if (!stamped.has_intent_seq && _dispatchSeqValid) {
        stamped.has_intent_seq = true;
        stamped.intent_seq = _dispatchSeq;
    }
    std::array<std::byte, 128> buf{};
    size_t len = encodeNack(stamped, std::span<std::byte>(buf));
    if (len == 0) return;
    sendFrameTo(t, FrameType::NACK, 0, std::span<const std::byte>(buf.data(), len));
}

// ---- HELLO / WELCOME (§6.2, §6.3) -------------------------------------------

inline size_t Hub::occupiedCount(const Slot* exclude) const {
    size_t n = 0;
    for (const auto& s : _slots) {
        if (&s == exclude) continue;
        if (s.session.occupied()) ++n;
    }
    return n;
}

inline Hub::Slot* Hub::findSlotByInstance(std::span<const std::byte> instanceId, const Slot* exclude) {
    for (auto& s : _slots) {
        if (&s == exclude) continue;
        if (!s.session.occupied()) continue;
        if (instanceId.size() == s.session.instance_id.size() &&
            std::equal(instanceId.begin(), instanceId.end(), s.session.instance_id.begin())) {
            return &s;
        }
    }
    return nullptr;
}

// ---- M4c (RFC-029 item 1) / RFC-042: the hub authenticity signature, shared
// verbatim by handleHello() and handleReattach() (a reattach WELCOME is a real
// WELCOME and gets the identical signing treatment). See registry.yaml's
// HUB_SIG note for the full latency argument for why this is deferred by
// default rather than signed inline. Mutates `w` (may set `trust`/
// `welcome_sig` for inline signing) and `slot` (signDelivered on the inline
// path). Returns true iff the caller must arm slot.signPending AFTER the
// WELCOME actually sends successfully.
inline bool Hub::armWelcomeSignature(Slot& slot, WelcomeMsg& w) {
    if (!(slot.sigRequested && slot.hasClientNonce)) return false;
    const auto material = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(slot.clientNonce),
                                         slot.session.session_id, _bootId);
    if (!_inlineSigning) return true;  // armed, not computed — caller arms after the send succeeds
    std::array<std::byte, kTrustSigMaxBytes> sig{};
    const size_t sigLen = _crypto.signP256(std::span<const std::byte>(material), std::span<std::byte>(sig));
    if (sigLen > 0 && sigLen <= sig.size()) {
        w.has_trust = true;
        w.trust_map.has_welcome_sig = true;
        w.trust_map.welcome_sig_len = uint8_t(sigLen);
        w.trust_map.welcome_sig = sig;
        slot.signDelivered = true;
    }
    return false;
}

inline void Hub::handleHello(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs) {
    auto helloR = decodeHello(payload);
    if (!helloR) {
        NackMsg n;
        n.code = NackCode::MALFORMED;
        sendNack(*slot.transport, n);
        return;
    }
    const HelloMsg& h = helloR.value();

    // §6.3 duplicate identity: a LIVE session evicts as before. A STALE one
    // REATTACHES (RFC-042 §6.3 migration path) — this is a resumption, not a
    // competing claimant, so it skips eviction, BUSY pressure, and grant
    // renegotiation entirely; handleReattach() answers with its own WELCOME.
    std::span<const std::byte> instanceSpan(h.instance_id);
    if (Slot* dup = findSlotByInstance(instanceSpan, &slot)) {
        if (dup->session.state == HubSessionState::STALE) {
            handleReattach(slot, *dup, h, nowMs);
            return;
        }
        GoodbyeMsg gb;
        gb.code = NackCode::DUPLICATE_INSTANCE;
        std::array<std::byte, 64> gbuf{};
        size_t glen = encodeGoodbye(gb, std::span<std::byte>(gbuf));
        if (glen > 0 && dup->transport != nullptr) {
            sendFrameTo(*dup->transport, FrameType::GOODBYE, 0, std::span<const std::byte>(gbuf.data(), glen));
        }
        // §6.3 + §6.8: the evicted duplicate's session ends here — release its
        // source ownership too (GOODBYE frame already sent above).
        teardownSession(*dup, nowMs);
    }

    // §6.3 admission: BUSY once kHubMaxSessions SESSIONS (not physical slots)
    // are occupied. `slot` itself doesn't count against its own admission —
    // a HELLO replacing this very slot's own (already-occupied, e.g. a
    // retried) session isn't new capacity pressure.
    if (occupiedCount(&slot) >= kHubMaxSessions) {
        // RFC-042 item 5: a STALE session yields its slot under pressure
        // before a genuinely new identity is refused — lowest access tier
        // first, tie-break longest continuously stale. A LIVE session is
        // NEVER evicted for pressure (only a duplicate-instance_id HELLO,
        // above, ever displaces one).
        if (Slot* victim = findEvictableStale(&slot)) {
            GoodbyeMsg gb;
            gb.code = NackCode::SLOT_RECLAIMED;
            std::array<std::byte, 64> gbuf{};
            size_t glen = encodeGoodbye(gb, std::span<std::byte>(gbuf));
            if (glen > 0 && victim->transport != nullptr) {
                // Best-effort: the reclaimed session was stale for a reason
                // and this GOODBYE may never arrive.
                sendFrameTo(*victim->transport, FrameType::GOODBYE, 0, std::span<const std::byte>(gbuf.data(), glen));
            }
            teardownSession(*victim, nowMs);
        } else {
            NackMsg n;
            n.code = NackCode::BUSY;
            n.has_retry_after_ms = true;
            n.retry_after_ms = kHubBusyRetryAfterMs;
            sendNack(*slot.transport, n);
            return;
        }
    }

    // Fresh session in this slot. A HELLO can land on a slot that is STILL
    // occupied by a prior session (a client re-HELLOing on a live transport
    // without a GOODBYE — a reconnect that reuses the socket). Tear that
    // outgoing session down first so its source ownership is released (§6.8);
    // otherwise it orphans exactly like the GOODBYE/detach bug and the fresh
    // session can never acquire the source. On a FREE slot this is a cheap
    // no-op (no onSessionLeft, plain reset).
    teardownSession(slot, nowMs);
    slot.session.state = HubSessionState::VALIDATING;
    do {
        slot.session.session_id = _rng.nextU32();
    } while (slot.session.session_id == 0);
    std::memcpy(slot.session.instance_id.data(), h.instance_id.data(), slot.session.instance_id.size());
    // §12.2: the hub's own pairing store is consulted FIRST (instance_id +
    // token -> role); a delegate is still free to grant a role for tokens it
    // recognizes by its own mechanism (e.g. a pre-provisioned/legacy token)
    // when the pairing store doesn't know this pair — "override-if-still-
    // watch", never the reverse (the pairing store's grant is never
    // downgraded by falling through to the delegate).
    AccessLevel role = h.has_token
                            ? _pairing.validate(instanceSpan, std::span<const std::byte>(h.token), _crypto)
                            : AccessLevel::watch;
    if (role == AccessLevel::watch) {
        role = _delegate.validateToken(instanceSpan, std::span<const std::byte>(h.token), h.has_token);
    }

    // ---- M4b: remember WHO this is, in hub-owned bytes ----------------------
    // HELLO's strings are views into the frame buffer and die with this
    // dispatch; a pending knock and a ledger entry both have to name the device
    // long afterwards.
    slot.session.clientKind.assign(h.client_kind);
    slot.session.clientName.assign(h.client_name);
    slot.session.hasClientVer = h.has_trust && h.trust_map.has_client_ver;
    if (slot.session.hasClientVer) slot.session.clientVer.assign(h.trust_map.client_ver);
    slot.session.presentationMode =
        (h.has_trust && h.trust_map.has_presentation_mode) ? h.trust_map.presentation_mode : 0;

    // ---- M4c (RFC-029 item 1): the client's own entropy + its ask -----------
    // Recorded BEFORE any signing decision, and never invented: a hub does not
    // sign a nonce the client did not send, because the entire point of the
    // client nonce is that the CLIENT chose it. (Signing a hub-chosen value is
    // exactly the replayable design the feasibility pass killed.)
    slot.hasClientNonce = h.has_trust && h.trust_map.has_client_nonce;
    if (slot.hasClientNonce) slot.clientNonce = h.trust_map.client_nonce;
    slot.sigRequested = h.has_trust && h.trust_map.has_sig_request && h.trust_map.sig_request;

    // ---- RFC-029 item 2: THE CLIENT-CHANGE TRIPWIRE -------------------------
    // Runs only for a device the ledger actually knows (observeHello returns
    // `known == false` otherwise, and an unknown device has nothing to compare
    // against). A version change on a device granted more than the auto-keep
    // ceiling SUSPENDS its role to `watch` and queues a re-approval that any
    // configure session sees on 0x000A.
    //
    // HONESTY CLAUSE, NORMATIVE — stated here because this is where a reader
    // meets it: `client_ver` is SELF-REPORTED. This catches an HONEST update
    // and nothing else. A deliberately malicious update reports whatever
    // version it likes and keeps its token; this is a tripwire, not
    // attestation. What actually bounds a hostile client is role scoping,
    // instant revocation, roster visibility, and the role-exempt safety ops.
    //
    // M4c NOTE: the body moved to applyTrustObservation() so the AUTH
    // (proof-mode) path runs the IDENTICAL wire. A proof-mode client sends no
    // token in HELLO, so leaving this inline here would have meant the clients
    // with the BETTER security posture were the only ones that never tripped
    // the tripwire — a silent hole created by an unrelated feature.
    if (h.has_token) {
        slot.session.role = role;  // applyTrustObservation reads the pre-observation role
        role = applyTrustObservation(slot, nowMs);
    }
    slot.session.role = role;
    slot.session.helloSeenCfgGen = _cfgGen;
    slot.session.clientEtagMatched =
        h.has_catalog_etag && std::equal(h.catalog_etag.begin(), h.catalog_etag.end(), _etag.begin());
    // §8.4/RFC-015: a MATCHING etag in HELLO is proof the client already holds
    // this exact catalog — it can decode packed payloads right now, so it is
    // READY immediately and the 99% reconnect case keeps today's zero-added-
    // latency retained push. Absent or mismatched -> gated until CATALOG_READY.
    slot.session.ready = slot.session.clientEtagMatched;
    slot.session.readyEtagMismatch = false;
    slot.session.grantedAtMs = nowMs;
    slot.session.lastRxMs = nowMs;
    slot.session.lastTxMs = nowMs;

    // Build grants from HELLO's subscription wishes (§6.2, §6.3, §10.2):
    // unknown channel -> omit; access above role -> omit; class not
    // subscribable (INTENT is c2h-only) -> omit; else clamp rate to the
    // catalog ceiling and grant at the catalog's own default priority.
    WelcomeMsg w{};
    for (uint32_t i = 0; i < h.subscriptions_count; ++i) {
        const SubscriptionWish& wish = h.subscriptions[i];
        const CatalogEntry* entry = _catalog.find(wish.channel_id);
        if (!entry) continue;
        // Not subscribable: INTENT is c2h-only, and STORE (RFC-021) has no push
        // plane at all — its items move over BLOB_REQ and its dynamic side is a
        // separate tiny STATE channel.
        if (entry->cls == ChannelClass::INTENT || entry->cls == ChannelClass::STORE) continue;
        if (uint8_t(slot.session.role) < uint8_t(entry->access)) continue;

        float grantedRate = (entry->maxRateHz <= 0.0f) ? 0.0f : std::min(wish.rate_hz, entry->maxRateHz);
        if (grantedRate < 0.0f) grantedRate = 0.0f;

        if (!slot.session.subs.upsert(wish.channel_id, grantedRate, entry->defaultPriority)) continue;  // table full

        replayEventsOnGrant(slot, wish.channel_id);  // RFC-017

        if (w.grants_count < kWelcomeMaxGrants) {
            Grant g;
            g.channel_id = wish.channel_id;
            g.granted_rate_hz = grantedRate;
            g.priority = uint8_t(entry->defaultPriority);
            w.grants[w.grants_count++] = g;
        }
    }

    // §6.2/§10.5 publishes grants: inbound-STREAM (c2h motion-input) wishes.
    // Validation/clamping is grantPublishWish() — shared verbatim with the
    // mid-session PUBLISH frame (RFC-013) so the two can never drift.
    for (uint32_t i = 0; i < h.publishes_count; ++i) {
        auto gp = grantPublishWish(slot, h.publishes[i], nowMs);
        if (!gp) continue;  // §6.2: a failing wish is absent from the grants, never a NACK
        if (w.granted_publishes_count < kWelcomeMaxGrantedPublishes) {
            w.granted_publishes[w.granted_publishes_count++] = *gp;
        }
    }

    // retained_pending: STATE-class granted channels that currently hold a
    // retained value (§9.1, §6.3) — the count WELCOME advertises, and the
    // exact gate the client's SYNCING->LIVE transition (§2.2) counts against.
    uint32_t retainedPending = 0;
    for (uint32_t i = 0; i < w.grants_count; ++i) {
        const CatalogEntry* entry = _catalog.find(w.grants[i].channel_id);
        if (entry && entry->cls == ChannelClass::STATE && _retained.get(w.grants[i].channel_id)) {
            ++retainedPending;
        }
    }

    w.proto_ver = kProtocolVersion;
    w.session_id = slot.session.session_id;
    w.boot_id = _bootId;
    w.catalog_etag = _etag;
    w.cfg_gen = _cfgGen;
    // §13.1/§13.4: a hub MAY advertise a smaller max_frame than kFrameBufferCapacity
    // permits (a binding's OWN declared MTU, e.g. BLE's negotiated ATT MTU-3) but
    // MUST NOT advertise a larger one — so this is the transport's honest
    // properties().mtu, capped at the buffer capacity, not a flat constant. WS's
    // own properties().mtu already equals kFrameBufferCapacity, so this is a no-op
    // there; only a small-MTU binding (BLE pre-negotiation) sees a smaller number.
    w.limits_info.max_frame =
        (slot.transport != nullptr)
            ? uint32_t(std::min<uint16_t>(slot.transport->properties().mtu, uint16_t(kFrameBufferCapacity)))
            : uint32_t(kFrameBufferCapacity);
    w.limits_info.max_subscriptions = uint32_t(limits::max_subscriptions_per_session);
    w.limits_info.retained_pending = retainedPending;
    w.roles = uint8_t(slot.session.role);
    // RFC-038: a client that KNOWS its liveness cadence is coarse (throttled
    // browser tab, slow BLE connection interval) may wish a deadman window;
    // clamp into the registry bounds, apply per session, echo the APPLIED
    // value on the key that was already the echo. No wish = default = today.
    uint32_t appliedDeadman = limits::deadman_default_ms;
    if (h.deadman_wish_ms > 0) {
        appliedDeadman = h.deadman_wish_ms;
        if (appliedDeadman < limits::deadman_min_ms) appliedDeadman = limits::deadman_min_ms;
        if (appliedDeadman > limits::deadman_max_ms) appliedDeadman = limits::deadman_max_ms;
    }
    slot.session.deadmanMs = appliedDeadman;
    w.deadman_ms = appliedDeadman;
    w.deadman_policy = 0;  // M4: informational only, policy dispatch is M5
    // RFC-033.3: the per-frame wish bound is advertised, never binary-searched.
    static_assert(kSubscribeMaxWishes == limits::max_subscriptions_per_frame,
                  "registry max_subscriptions_per_frame documents the reference decoder cap");
    w.limits_info.max_subscriptions_per_frame = uint32_t(kSubscribeMaxWishes);
    // RFC-016(a): identity travels when the application declared one.
    if (!_idProduct.empty() || !_idFwVersion.empty() || !_idHubName.empty()) {
        w.has_identity = true;
        w.identity.product = _idProduct;
        w.identity.fw_version = _idFwVersion;
        w.identity.hub_name = _idHubName;
    }
    // RFC-048: durable identity rides the SAME identity map, independently of
    // product/fw_version/hub_name above — a hub with a durable id but no other
    // identity strings still gets the map (IdentityInfo::any() covers it).
    if (_hubInstanceId != 0) {
        w.has_identity = true;
        w.identity.has_hub_instance_id = true;
        w.identity.hub_instance_id = _hubInstanceId;
    }
    // RFC-046: the hub's own WS endpoint, 0/0 = absent (omitted on the wire).
    w.ws_port = _wsPort;
    w.ipv4 = _ipv4;
    _rng.fill(std::span<std::byte>(w.nonce));
    slot.nonce = w.nonce;  // §12.2: remembered so a later PAIR_REQ on this session can be verified

    // RFC-027.3 / trust_keys.pairing_modes: advertise which association
    // ceremonies this hub is offering RIGHT NOW. Re-evaluated per session
    // rather than fixed at boot, which is the point: a push-to-pair window is
    // advertised exactly while it is open, so a client connecting during one
    // can see it without an out-of-band hint. Omitted entirely when the hub
    // offers nothing, keeping the byte-for-byte potato WELCOME intact.
    const uint8_t offered = _pairing.offeredModes(nowMs);
    if (offered != 0) {
        w.has_trust = true;
        w.trust_map.has_pairing_modes = true;
        w.trust_map.pairing_modes_mask = offered;
    }

    // ---- M4c (RFC-029 item 1): the hub authenticity signature ---------------
    // TWO DELIVERY POINTS, ONE MEANING (see registry.yaml's HUB_SIG note for
    // the full latency argument):
    //
    //   INLINE, here, only when the application has declared signing cheap. A
    //   host, the sim, or a part with the ECC peripheral. This is the shortest
    //   path and it costs the connecting client zero extra round trips.
    //
    //   DEFERRED, by arming slot.signPending and letting the application's own
    //   worker produce it. This is the S3's path and the DEFAULT, because one
    //   software ECDSA is ~30-80 ms in a single uninterruptible call and
    //   update() runs on a 5 ms tick that also paces STATE, runs the deadman and
    //   drains motion input. Signing inline there would put a 6-16 tick hole in
    //   one client's motion stream every time a DIFFERENT client connected.
    //
    // Either way the client sees the same signature over the same 16 bytes and
    // runs the same verification. A hub that can do neither arms nothing, sends
    // neither, and is conformant — signing is ON REQUEST and a request is not a
    // promise.
    bool armSignJob = armWelcomeSignature(slot, w);

    std::array<std::byte, 700> wbuf{};
    size_t wlen = encodeWelcome(w, std::span<std::byte>(wbuf));
    if (wlen == 0) return;  // catalog-conformance/encode bug; nothing sane to do
    if (!sendFrameTo(*slot.transport, FrameType::WELCOME, 0, std::span<const std::byte>(wbuf.data(), wlen))) return;
    // A signature is only worth computing for a session that got its WELCOME:
    // arming before the send would leave an application's signer grinding
    // ~30-80 ms of ECDSA for a handshake that never happened.
    if (armSignJob) slot.signPending = true;

    slot.session.state = HubSessionState::GRANTED;
    // Retained STATE pushes happen via the normal pacing walk later in this
    // same update() call (each fresh grant's everPushed==false forces
    // dueForPush() to fire immediately — see subscription.hpp's design
    // note) — no separate immediate-push code path is needed here.
    slot.session.state = HubSessionState::LIVE;  // hub-side bookkeeping only, §2.2
    _delegate.onSessionJoined(slot.session.session_id);
}

// ---- RFC-042 path B — reattach ----------------------------------------------
// A fresh HELLO names a STALE session's instance_id on a NEW transport. This is
// §6.3's migration path applied to a resumption rather than a live-duplicate
// hop: SAME session_id, SAME grants (subs and publishGrants are carried over
// verbatim, not renegotiated from this HELLO's wishes — RFC-042's own design
// table), role RE-DERIVED from the presented token exactly as any HELLO does.
// No BUSY pressure is spent (not new capacity) and no teardown/loss-policy runs
// on `stale` (a migration is not a session loss, §6.3) — its slot is simply
// vacated once its state has moved to `slot`.

inline void Hub::handleReattach(Slot& slot, Slot& stale, const HelloMsg& h, uint32_t nowMs) {
    // `slot` may itself already hold an unrelated session (a re-HELLO on a
    // transport that was previously talking to a DIFFERENT identity) —
    // release that first, exactly like a fresh HELLO does.
    teardownSession(slot, nowMs);

    // Migrate identity + grants verbatim. This is a plain member-wise copy
    // from an EXISTING object — NOT the whole-object `T{}`-reassignment reset
    // pattern TRAPS T1 forbids: there is no temporary construction of a fresh
    // HubSession, just a field-by-field copy into a slot that already exists.
    slot.session = stale.session;
    slot.pushRecords = stale.pushRecords;
    slot.conflictNacks = stale.conflictNacks;

    // AUTH/pending-blob/sign state is retained ONLY while the transport itself
    // is still attached (RFC-042's kept-table, path A) — path B always resets
    // it, because it was mid-flight against a socket that no longer exists.
    slot.hasClientNonce = false;
    slot.clientNonce.fill(std::byte{0});
    slot.sigRequested = false;
    slot.signPending = false;
    slot.signDelivered = false;
    slot.authFailures = 0;
    slot.congestionLevel = 0;
    slot.criticalStalling = false;
    slot.hasProbeReport = false;
    slot.blob = typename Slot::PendingBlob{};

    // Sever the stale slot's own transport if one is still attached (a
    // genuine WiFi blip can leave a zombie socket behind that never formally
    // detached) — this identity now lives on `slot`'s transport only.
    if (stale.transport != nullptr) {
        stale.transport->close();
        stale.transport = nullptr;
    }
    // Vacate the old physical slot WITHOUT teardownSession's ownership-release/
    // onSessionLeft: this session is CONTINUING, not ending (§6.3: "a
    // migration is not a session loss"). reset() is T1-safe in-place
    // destroy+placement-new.
    stale.session.reset();
    stale.pushRecords.fill(PushRecord{});
    stale.conflictNacks.fill(typename Slot::ConflictNack{});
    stale.clientNonce.fill(std::byte{0});
    stale.hasClientNonce = false;
    stale.blob = typename Slot::PendingBlob{};

    // ---- Refresh identity fields from THIS HELLO — it is a real HELLO -------
    slot.session.clientKind.assign(h.client_kind);
    slot.session.clientName.assign(h.client_name);
    slot.session.hasClientVer = h.has_trust && h.trust_map.has_client_ver;
    if (slot.session.hasClientVer) slot.session.clientVer.assign(h.trust_map.client_ver);
    slot.session.presentationMode =
        (h.has_trust && h.trust_map.has_presentation_mode) ? h.trust_map.presentation_mode : 0;
    slot.hasClientNonce = h.has_trust && h.trust_map.has_client_nonce;
    if (slot.hasClientNonce) slot.clientNonce = h.trust_map.client_nonce;
    slot.sigRequested = h.has_trust && h.trust_map.has_sig_request && h.trust_map.sig_request;

    // ---- Role RE-DERIVED from the presented token, exactly as any HELLO -----
    // (§6.3's migration text): a revoked credential downgrades correctly; an
    // unrevoked one cheaply reproduces the identical role it already had.
    std::span<const std::byte> instanceSpan(h.instance_id);
    AccessLevel role = h.has_token
                            ? _pairing.validate(instanceSpan, std::span<const std::byte>(h.token), _crypto)
                            : AccessLevel::watch;
    if (role == AccessLevel::watch) {
        role = _delegate.validateToken(instanceSpan, std::span<const std::byte>(h.token), h.has_token);
    }
    if (h.has_token) {
        slot.session.role = role;  // applyTrustObservation reads the pre-observation role
        role = applyTrustObservation(slot, nowMs);
    }
    slot.session.role = role;
    slot.session.helloSeenCfgGen = _cfgGen;
    slot.session.clientEtagMatched =
        h.has_catalog_etag && std::equal(h.catalog_etag.begin(), h.catalog_etag.end(), _etag.begin());
    // Readiness is RETAINED from before staleness UNLESS the freshly-declared
    // etag now disagrees — a hub whose catalog changed WHILE the session was
    // away must not let a reattaching client believe it is still ready
    // against a shape that no longer exists. Matching keeps `ready` exactly as
    // it was (RFC-042's kept-table: "no re-SYNC").
    if (!slot.session.clientEtagMatched) {
        slot.session.ready = false;
        slot.session.readyEtagMismatch = h.has_catalog_etag;
        slot.session.grantedAtMs = nowMs;  // re-arm RFC-015 READY_TIMEOUT fresh
    }
    slot.session.staleSinceMs = 0;
    slot.session.lastRxMs = nowMs;
    slot.session.lastTxMs = nowMs;

    // "Fresh grant for PUSH purposes only" (RFC-042 §4): every existing STATE
    // subscription re-arms its first-push-after-grant treatment, so a resumed
    // session's very first frames back are a full resync (cfg_gen, a latched
    // safety word, anything it may have missed while away) — reused machinery
    // (SubscriptionEntry::everPushed), not new machinery.
    for (auto& e : slot.session.subs) e.everPushed = false;

    // Build a normal WELCOME, but from the RETAINED grants — SAME session_id,
    // SAME grants, this is a reattach, not a renegotiation.
    WelcomeMsg w{};
    for (const auto& e : slot.session.subs) {
        if (w.grants_count >= kWelcomeMaxGrants) break;
        Grant g;
        g.channel_id = e.channel_id;
        g.granted_rate_hz = e.granted_rate_hz;
        g.priority = uint8_t(e.priority);
        w.grants[w.grants_count++] = g;
    }
    for (const auto& pg : slot.session.publishGrants) {
        if (!pg.used) continue;
        if (w.granted_publishes_count >= kWelcomeMaxGrantedPublishes) break;
        GrantedPublish gp;
        gp.channel_id = pg.channel_id;
        gp.granted_rate_hz = pg.granted_rate_hz;
        gp.has_burst = pg.burstRequested;
        gp.burst = pg.granted_burst;
        gp.has_curve_family = pg.curveFamily != 0;
        gp.curve_family = pg.curveFamily;
        w.granted_publishes[w.granted_publishes_count++] = gp;
    }

    uint32_t retainedPending = 0;
    for (uint32_t i = 0; i < w.grants_count; ++i) {
        const CatalogEntry* entry = _catalog.find(w.grants[i].channel_id);
        if (entry && entry->cls == ChannelClass::STATE && _retained.get(w.grants[i].channel_id)) {
            ++retainedPending;
        }
    }

    w.proto_ver = kProtocolVersion;
    w.session_id = slot.session.session_id;  // UNCHANGED — same identity, RFC-042
    w.boot_id = _bootId;
    w.catalog_etag = _etag;
    w.cfg_gen = _cfgGen;
    // See the identical rationale in handleHello's WELCOME build above.
    w.limits_info.max_frame =
        (slot.transport != nullptr)
            ? uint32_t(std::min<uint16_t>(slot.transport->properties().mtu, uint16_t(kFrameBufferCapacity)))
            : uint32_t(kFrameBufferCapacity);
    w.limits_info.max_subscriptions = uint32_t(limits::max_subscriptions_per_session);
    w.limits_info.retained_pending = retainedPending;
    w.roles = uint8_t(slot.session.role);
    w.deadman_ms = slot.session.deadmanMs;  // RFC-038 window kept, not renegotiated
    w.deadman_policy = 0;
    w.limits_info.max_subscriptions_per_frame = uint32_t(kSubscribeMaxWishes);
    if (!_idProduct.empty() || !_idFwVersion.empty() || !_idHubName.empty()) {
        w.has_identity = true;
        w.identity.product = _idProduct;
        w.identity.fw_version = _idFwVersion;
        w.identity.hub_name = _idHubName;
    }
    // RFC-048/RFC-046 — see the identical block in handleHello's WELCOME build.
    if (_hubInstanceId != 0) {
        w.has_identity = true;
        w.identity.has_hub_instance_id = true;
        w.identity.hub_instance_id = _hubInstanceId;
    }
    w.ws_port = _wsPort;
    w.ipv4 = _ipv4;
    _rng.fill(std::span<std::byte>(w.nonce));
    slot.nonce = w.nonce;

    const uint8_t offered = _pairing.offeredModes(nowMs);
    if (offered != 0) {
        w.has_trust = true;
        w.trust_map.has_pairing_modes = true;
        w.trust_map.pairing_modes_mask = offered;
    }

    bool armSignJob = armWelcomeSignature(slot, w);

    std::array<std::byte, 700> wbuf{};
    size_t wlen = encodeWelcome(w, std::span<std::byte>(wbuf));
    if (wlen == 0) return;  // catalog-conformance/encode bug; nothing sane to do
    if (!sendFrameTo(*slot.transport, FrameType::WELCOME, 0, std::span<const std::byte>(wbuf.data(), wlen))) return;
    if (armSignJob) slot.signPending = true;

    slot.session.state = HubSessionState::LIVE;
    emitSessionEvent(session_events::session_resumed, slot.session.session_id, nowMs);
    // Deliberately NOT _delegate.onSessionJoined(): this is a resumption, not
    // a new join (session_events::session_joined's own registry note).
}

// ---- M4c (RFC-029) ----------------------------------------------------------
// the trust tripwire, shared; AUTH; and the signing queue.

// The RFC-029 item-2 tripwire. Reads the pre-observation role from
// slot.session.role and returns the effective one. Called from HELLO (bearer
// presentation) and from a successful AUTH (proof presentation) — one wire, two
// entry points, because a hole in either is a hole.
//
// HONESTY CLAUSE, NORMATIVE: `client_ver` is SELF-REPORTED. This catches an
// HONEST update and nothing else. A deliberately malicious update reports
// whatever version it likes and keeps its token; this is a tripwire, not
// attestation. What actually bounds a hostile client is role scoping, instant
// revocation, roster visibility, and the role-exempt safety ops.
inline AccessLevel Hub::applyTrustObservation(Slot& slot, uint32_t nowMs) {
    std::span<const std::byte> instanceSpan(slot.session.instance_id);
    auto obs = _pairing.observeHello(instanceSpan, slot.session.clientKind.view(), slot.session.clientName.view(),
                                     slot.session.clientVer.view(), slot.session.hasClientVer,
                                     slot.session.presentationMode);
    if (!obs.known) return slot.session.role;  // unknown device: nothing to compare against

    if (obs.suspended) {
        // Queue it as a pending RE-approval, so the operator's answer is the
        // same verb (`pair_approve`) with a different sentence attached, and so
        // a configure client that connects mid-window still sees it in the
        // retained 0x000A snapshot.
        if (PendingKnock* k = _pairing.pending().add(instanceSpan, slot.session.session_id,
                                                     nowMs + limits::pairing_window_default_s * 1000u,
                                                     pairing_modes::knock_approve)) {
            k->reapproval = true;
            k->kind.assign(slot.session.clientKind.view());
            k->name.assign(slot.session.clientName.view());
            k->version.assign(slot.session.clientVer.view());
        }
        emitPairingEvent(pairing_events::recognized_pending, instanceSpan, slot.session.clientName.view(),
                         pairing_modes::knock_approve, obs.suspendedRole, slot.session.clientVer.view(), nowMs);
        publishPendingPairingState(nowMs);
        publishPairedRosterState();
    }
    return obs.effectiveRole;
}

// ---- AUTH (0x1C) — RFC-029 item 6's proof presentation ----------------------
//
// WHAT THIS BUYS: v1 transports are cleartext `ws://`. A bearer token in HELLO
// is therefore readable by any passive LAN observer, forever. A proof is
// HMAC-SHA256(token, THIS session's WELCOME nonce) — the sniffer captures
// something that is already spent by the time they read it, and the credential
// itself never leaves either end. §12.1 excludes the passive observer from the
// v1 threat model; this closes the hole anyway because it costs one HMAC.
//
// WHAT IT DOES NOT BUY: anything against an ACTIVE attacker. A MITM on a
// cleartext binding does not need your token — it has your session. Proof mode
// is a passive-theft plug, and calling it more than that would be the kind of
// security theatre this project's honesty clauses exist to prevent.
inline void Hub::handleAuth(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs) {
    // Strikes are spent BEFORE any work: an exhausted session gets silence, so
    // a peer that keeps hammering after eviction cannot keep buying HMACs.
    if (slot.authFailures >= limits::auth_attempts_max) return;

    auto res = decodeAuth(payload);
    if (!res || !res.value().trust_map.has_token_proof) {
        NackMsg n;
        n.code = NackCode::MALFORMED;
        sendNack(*slot.transport, n);
        return;
    }
    const TrustMap& t = res.value().trust_map;

    // A device that presents a proof IS a proof-mode device — record it whether
    // or not it bothered to say so, because the roster's job is to show the
    // posture that was actually used, not the one that was claimed. RFC-029.6:
    // "the roster records which mode a device uses (visible security posture)".
    slot.session.presentationMode =
        t.has_presentation_mode ? t.presentation_mode : uint8_t(presentation_modes::proof);

    // `proved` is the role; `proofOk` is whether the HMAC verified. They differ
    // for exactly one device: a RECOGNIZED-PENDING one, which proves correctly
    // and still only gets `watch` because its role is suspended pending a
    // person's re-approval. That device authenticated fine and must not burn a
    // strike for it.
    bool proofOk = false;
    const AccessLevel proved = _pairing.validateProof(
        std::span<const std::byte>(slot.session.instance_id), std::span<const std::byte>(t.token_proof),
        std::span<const std::byte>(slot.nonce), _crypto, &proofOk);

    if (!proofOk) {
        ++slot.authFailures;
        NackMsg n;
        n.code = NackCode::UNAUTHORIZED;
        sendNack(*slot.transport, n);
        if (slot.authFailures >= limits::auth_attempts_max) {
            // Same three-strikes shape as §12.2's PIN window, and for the same
            // reason: an unauthenticated peer does not get an unbounded budget
            // of the hub's crypto. GOODBYE first so the failure is EXPLICIT —
            // §6.8's teardown then runs the full loss policy, exactly as every
            // other session-end path does.
            GoodbyeMsg gb;
            gb.code = NackCode::UNAUTHORIZED;
            std::array<std::byte, 64> gbuf{};
            const size_t glen = encodeGoodbye(gb, std::span<std::byte>(gbuf));
            if (glen > 0) {
                sendFrameTo(*slot.transport, FrameType::GOODBYE, 0, std::span<const std::byte>(gbuf.data(), glen));
            }
            teardownSession(slot, nowMs);
        }
        return;
    }

    slot.authFailures = 0;
    slot.session.role = proved;
    // The tripwire runs HERE for proof-mode clients — same function, same
    // policy, same re-approval queue as the bearer path in handleHello.
    slot.session.role = applyTrustObservation(slot, nowMs);

    // The answer: a GRANT carrying `roles` (23). No new answer frame was needed
    // — GRANT already means "your access to things was re-evaluated", and
    // `roles` is the same key WELCOME uses for the same value. `grants` stays
    // empty on purpose: the wishes this client sent in HELLO were judged
    // against the role it had THEN, the hub does not hoard rejected wishes, and
    // a client whose role went up re-SUBSCRIBEs (which handleSubscribe
    // re-authorizes from scratch). Zero per-session memory, one existing verb.
    GrantMsg g{};
    g.has_roles = true;
    g.roles = uint8_t(slot.session.role);
    std::array<std::byte, 64> gbuf{};
    const size_t glen = encodeGrant(g, std::span<std::byte>(gbuf));
    if (glen > 0) sendFrameTo(*slot.transport, FrameType::GRANT, 0, std::span<const std::byte>(gbuf.data(), glen));
    publishPairedRosterState();  // presentation_mode moved: posture is roster-visible
}

// ---- the deferred-signing queue (RFC-029 item 1) ----------------------------

inline void Hub::setInlineSigning(bool on) { _inlineSigning = on; }
inline bool Hub::inlineSigning() const { return _inlineSigning; }

inline size_t Hub::pendingSignJobs() const {
    size_t n = 0;
    for (const auto& s : _slots) {
        if (s.signPending) ++n;
    }
    return n;
}

inline bool Hub::takePendingSignJob(SignJob& out) {
    for (auto& s : _slots) {
        if (!s.signPending) continue;
        out.session_id = s.session.session_id;
        out.message = hubSigMaterial(std::span<const std::byte, kTrustClientNonceBytes>(s.clientNonce),
                                     s.session.session_id, _bootId);
        // Taken, not yet delivered. Clearing the flag here (rather than in
        // submitSignature) is what makes the job AT-MOST-ONCE: a signer that
        // dies mid-sign costs this session its signature, which the client
        // reads as "unverified" and handles — strictly better than a job that
        // can be handed out twice and burn 80 ms twice.
        s.signPending = false;
        return true;
    }
    return false;
}

inline bool Hub::submitSignature(uint32_t session_id, std::span<const std::byte> sig) {
    if (session_id == 0 || sig.empty() || sig.size() > kTrustSigMaxBytes) return false;
    for (auto& s : _slots) {
        // The session-id re-check is the whole safety of the async contract: a
        // slow signer's answer can arrive after its session died and a NEW
        // client took the slot, and delivering one client's signature into
        // another client's session would be a verification failure the innocent
        // client could never explain.
        if (!s.session.occupied() || s.session.session_id != session_id) continue;
        if (s.signDelivered) return false;
        if (s.transport == nullptr) return false;
        emitHubSig(s, sig);
        s.signDelivered = true;
        return true;
    }
    return false;
}

inline size_t Hub::signPendingNow(size_t maxJobs) {
    size_t done = 0;
    SignJob job;
    while (done < maxJobs && takePendingSignJob(job)) {
        std::array<std::byte, kTrustSigMaxBytes> sig{};
        const size_t n = _crypto.signP256(std::span<const std::byte>(job.message), std::span<std::byte>(sig));
        // A hub with no keypair answers 0 and simply never signs. That is a
        // conformant answer, not an error — see crypto.hpp's null object.
        if (n == 0 || n > sig.size()) continue;
        if (submitSignature(job.session_id, std::span<const std::byte>(sig.data(), n))) ++done;
    }
    return done;
}

inline void Hub::emitHubSig(Slot& slot, std::span<const std::byte> sig) {
    HubSigMsg m{};
    m.trust_map.has_welcome_sig = true;
    m.trust_map.welcome_sig_len = uint8_t(sig.size());
    for (size_t i = 0; i < sig.size(); ++i) m.trust_map.welcome_sig[i] = sig[i];
    std::array<std::byte, 128> buf{};
    const size_t n = encodeHubSig(m, std::span<std::byte>(buf));
    if (n == 0) return;
    sendFrameTo(*slot.transport, FrameType::HUB_SIG, 0, std::span<const std::byte>(buf.data(), n));
}

// ---- SUBSCRIBE / UNSUBSCRIBE (§6.6) -----------------------------------------

inline void Hub::handleSubscribe(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs) {
    (void)nowMs;
    auto res = decodeSubscribe(payload);
    if (!res) {
        // RFC-033: a SUBSCRIBE the hub cannot process is ANSWERED, never
        // dropped. The silent `return` that stood here produced a healthy-
        // looking LIVE session with zero STATE — twice in one night (a frame
        // over the 16-wish decode cap, both times) — and presented as a client
        // rendering bug. The cap itself is now advertised in WELCOME limits.
        NackMsg n;
        n.code = NackCode::SUBSCRIBE_REJECTED;
        n.has_detail = true;
        n.detail = (res.error() == DecodeError::CapacityExceeded)
                       ? "too many wishes per frame"
                       : "undecodable subscribe";
        sendNack(*slot.transport, n);
        return;
    }
    const SubscribeMsg& m = res.value();

    GrantMsg batch{};
    for (uint32_t i = 0; i < m.subscriptions_count; ++i) {
        const SubscriptionWish& wish = m.subscriptions[i];
        const CatalogEntry* entry = _catalog.find(wish.channel_id);
        if (!entry) {
            NackMsg n;
            n.code = NackCode::UNKNOWN_CHANNEL;
            n.has_channel_id = true;
            n.channel_id = wish.channel_id;
            sendNack(*slot.transport, n);
            continue;
        }
        if (entry->cls == ChannelClass::INTENT || entry->cls == ChannelClass::STORE) {
            NackMsg n;
            n.code = NackCode::CLASS_MISMATCH;
            n.has_channel_id = true;
            n.channel_id = wish.channel_id;
            sendNack(*slot.transport, n);
            continue;
        }
        if (uint8_t(slot.session.role) < uint8_t(entry->access)) {
            NackMsg n;
            n.code = NackCode::ACCESS_DENIED;
            n.has_channel_id = true;
            n.channel_id = wish.channel_id;
            sendNack(*slot.transport, n);
            continue;
        }

        float grantedRate = (entry->maxRateHz <= 0.0f) ? 0.0f : std::min(wish.rate_hz, entry->maxRateHz);
        if (grantedRate < 0.0f) grantedRate = 0.0f;

        if (!slot.session.subs.upsert(wish.channel_id, grantedRate, entry->defaultPriority)) {
            NackMsg n;
            n.code = NackCode::SUB_LIMIT;
            n.has_channel_id = true;
            n.channel_id = wish.channel_id;
            sendNack(*slot.transport, n);
            continue;
        }

        // RFC-017: a channel declaring replay_depth backfills its ring tail to
        // the fresh subscriber (§9.4's one sanctioned exception).
        replayEventsOnGrant(slot, wish.channel_id);

        if (batch.grants_count < kGrantMsgMaxGrants) {
            Grant g;
            g.channel_id = wish.channel_id;
            g.granted_rate_hz = grantedRate;
            g.priority = uint8_t(entry->defaultPriority);
            batch.grants[batch.grants_count++] = g;
        }
    }

    if (batch.grants_count > 0) {
        std::array<std::byte, 400> gbuf{};
        size_t glen = encodeGrant(batch, std::span<std::byte>(gbuf));
        if (glen > 0) sendFrameTo(*slot.transport, FrameType::GRANT, 0, std::span<const std::byte>(gbuf.data(), glen));
    }
    // Retained pushes for newly (re-)granted STATE channels follow the same
    // everPushed==false -> pumpStatePacing() path as HELLO's grants.
}

inline void Hub::handleUnsubscribe(Slot& slot, std::span<const std::byte> payload) {
    auto res = decodeUnsubscribe(payload);
    if (!res) return;
    const UnsubscribeMsg& m = res.value();
    for (uint32_t i = 0; i < m.channel_count; ++i) {
        slot.session.subs.remove(m.channel_ids[i]);
    }
}

// ---- PUBLISH (§6.6 / RFC-013) -----------------------------------------------
// mid-session publish renegotiation, plus the ONE implementation of §6.2's publish-wish validation both entry points share.

inline std::optional<GrantedPublish> Hub::grantPublishWish(Slot& slot, const PublishWish& wish, uint32_t nowMs) {
    // A wish is granted iff the channel exists, is class STREAM, is direction
    // c2h, is within the session's role, and clamps to a positive rate. Any
    // failure -> omit (no NACK, §6.2) — identical in HELLO and PUBLISH.
    const CatalogEntry* entry = _catalog.find(wish.channel_id);
    if (!entry) return std::nullopt;                                  // unknown -> absent
    if (entry->cls != ChannelClass::STREAM) return std::nullopt;      // wrong class -> absent
    if (entry->dir != Direction::c2h) return std::nullopt;            // wrong direction -> absent
    if (uint8_t(slot.session.role) < uint8_t(entry->access)) return std::nullopt;  // access -> absent

    float grantedRate = (entry->maxRateHz <= 0.0f) ? 0.0f : std::min(wish.rate_hz, entry->maxRateHz);
    if (grantedRate <= 0.0f) return std::nullopt;                     // not a rate-bearing publish -> absent

    // §10.5 + RFC-013 burst: the token bucket's CAPACITY, decoupled from its
    // refill rate. Default (no wish) = the granted rate, i.e. exactly the
    // pre-RFC-013 behavior. A requested burst is clamped into
    // [rate, rate x max_burst_multiple] and ECHOED post-clamp (ground-truth
    // doctrine): an unbounded client-declared burst would reintroduce the very
    // flood the limiter exists to stop, and a burst BELOW the rate would be a
    // bucket that cannot even hold one second of the rate it was granted.
    float grantedBurst = grantedRate;
    if (wish.has_burst) {
        const float ceiling = grantedRate * float(limits::max_burst_multiple);
        grantedBurst = wish.burst;
        if (!(grantedBurst > 0.0f)) grantedBurst = grantedRate;  // also catches NaN
        if (grantedBurst < grantedRate) grantedBurst = grantedRate;
        if (grantedBurst > ceiling) grantedBurst = ceiling;
    }

    // RFC-030: the declared curve family, passed through the application's
    // curve policy so the echo is the EFFECTIVE family. An unknown (future)
    // family value is treated as unspecified rather than parroted — the hub
    // must never claim to honor a smoothness class it cannot name.
    uint8_t effectiveFamily = 0;
    if (wish.has_curve_family) {
        uint8_t fam = (wish.curve_family <= curve_families::step) ? wish.curve_family
                                                                  : curve_families::unspecified;
        effectiveFamily = _delegate.effectiveCurveFamily(wish.channel_id, fam);
    }

    if (!slot.session.addPublishGrant(wish.channel_id, grantedRate, nowMs, grantedBurst, wish.has_burst,
                                      effectiveFamily)) {
        return std::nullopt;  // table full and this is a new channel
    }

    GrantedPublish gp;
    gp.channel_id = wish.channel_id;
    gp.granted_rate_hz = grantedRate;
    gp.has_burst = wish.has_burst;  // echo a burst only to a client that asked for one
    gp.burst = grantedBurst;
    gp.has_curve_family = wish.has_curve_family;  // echo a family only to a client that declared one
    gp.curve_family = effectiveFamily;
    // RFC-049b: echo the ORIGINAL wish verbatim alongside the effective value
    // so a downgrade (curve_policy overrode it) is two present keys a client
    // compares, not an inference from what it remembers sending.
    gp.has_requested_curve_family = wish.has_curve_family;
    gp.requested_curve_family = wish.curve_family;
    return gp;
}

// RFC-030: what family is a live publish operating under? 0 = unspecified —
// no such session, no such grant, or no declaration. Segment consumers read
// this at drain time.
inline uint8_t Hub::publishCurveFamily(uint32_t session_id, uint16_t channel_id) const {
    for (const Slot& slot : _slots) {
        if (!slot.session.occupied() || slot.session.session_id != session_id) continue;
        for (const auto& pg : slot.session.publishGrants) {
            if (pg.used && pg.channel_id == channel_id) return pg.curveFamily;
        }
        return 0;
    }
    return 0;
}

inline void Hub::handlePublish(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs) {
    auto res = decodePublish(payload);
    if (!res) {
        NackMsg n;
        n.code = NackCode::MALFORMED;
        sendNackTracked(slot, n, nowMs);
        return;
    }
    const PublishMsg& m = res.value();

    // Answered like SUBSCRIBE: a GRANT frame carrying the applied results —
    // same key, same entry shape WELCOME uses, so a client needs exactly one
    // publish-grant decoder for the handshake and every renegotiation.
    GrantMsg batch{};
    for (uint32_t i = 0; i < m.publishes_count; ++i) {
        auto gp = grantPublishWish(slot, m.publishes[i], nowMs);
        if (!gp) continue;  // §6.2: silently omitted, exactly as in HELLO
        if (batch.granted_publishes_count < kWelcomeMaxGrantedPublishes) {
            batch.granted_publishes[batch.granted_publishes_count++] = *gp;
        }
    }

    // A GRANT is emitted even when nothing was granted: the empty
    // `granted_publishes` IS the answer ("none of your wishes survived
    // validation"), and a client that got no frame at all could not tell that
    // apart from a lost PUBLISH.
    std::array<std::byte, 400> gbuf{};
    size_t glen = encodeGrant(batch, std::span<std::byte>(gbuf));
    if (glen > 0) {
        sendFrameToTracked(slot, FrameType::GRANT, 0, std::span<const std::byte>(gbuf.data(), glen), nowMs);
    }
}

// ---- CATALOG_READY (§8.4 / RFC-015) -----------------------------------------
// the dual-plane readiness gate's one input

inline void Hub::handleCatalogReady(Slot& slot, std::span<const std::byte> payload) {
    auto etag = decodeCatalogReady(payload);
    if (!etag) return;  // raw plane: a wrong-size payload drops, never NACKs

    // Idempotent by design: a client on a lossy binding re-sends this until the
    // first retained STATE arrives, so duplicates are harmless flag-sets.
    slot.session.ready = true;
    // §8.5 degraded operation: a client declaring a DIFFERENT etag still
    // becomes ready — it has told us what it operates against, and append-only
    // layouts make its prefix-parse safe — but the divergence is recorded so a
    // hub can surface the session as degraded.
    slot.session.readyEtagMismatch = !std::equal(etag->begin(), etag->end(), _etag.begin());
    // Retained pushes flow from the pacing walk later in this same update()
    // call — every grant's everPushed==false is still pending, so nothing had
    // to be queued while the gate was shut.
}

// ---- INTENT / ECHO / NACK (§9.3, exact order) -------------------------------

// RFC-025/010 per-op access, resolved GENERICALLY from the catalog — no
// channel-id special case anywhere in this function, deliberately: a generic
// client reads exactly these annotations to decide which ops to offer, so the
// hub must enforce exactly what the catalog advertises or gray-never-hide
// becomes a lie (a client would gray a control it was actually allowed, or
// offer one it wasn't and discover the truth only by NACK).
//
// The entry's own `access` is the FLOOR. Two annotations may RAISE it:
//   * schema-field `access` (key 16) — "this whole field needs more";
//   * schema-field `option_access` (key 17) — index-aligned with the field's
//     option labels, i.e. the WIRE VALUE indexes it. This is the one that
//     makes 0x0005 work: the channel sits at `watch` so ANY session may send
//     `estop`/`stop` (§11.2's "safety outranks authorization" generalized —
//     the failure mode of getting this wrong is "the person in the room
//     cannot stop the machine"), while `hold`/`pause`/`resume`/`estop_clear`/
//     override/bypass carry `control` per-option.
//
// An option value PAST the end of the vector raises to the STRICTEST declared
// option, not the floor. That is the safe direction: an unknown op on a
// safety-shaped channel must not be cheaper to reach than every known one.
// (It still ends in the delegate's UNSUPPORTED_OP — this only decides who is
// allowed to find that out.)
inline AccessLevel Hub::requiredAccessFor(const CatalogEntry& entry, const IntentMsg& m) const {
    AccessLevel need = entry.access;
    if (!entry.usesSchema()) return need;
    auto raise = [&need](AccessLevel a) {
        if (uint8_t(a) > uint8_t(need)) need = a;
    };
    const auto fields = _catalog.schemaFields(entry);
    for (uint32_t i = 0; i < m.value_count; ++i) {
        const SchemaField* sf = nullptr;
        for (const SchemaField& f : fields) {
            if (f.key == m.value[i].key) { sf = &f; break; }
        }
        if (sf == nullptr) continue;  // §4.3: an unknown key is tolerated, never gated
        if (sf->hasAccess) raise(sf->access);
        if (!sf->hasOptionAccess) continue;
        if (m.value[i].value.kind != IntentValue::Kind::U64) continue;  // not an enum value
        const auto oa = _catalog.optionAccess(*sf);
        const uint64_t idx = m.value[i].value.u64_val;
        if (idx < oa.size()) {
            raise(oa[idx]);
        } else {
            for (AccessLevel a : oa) raise(a);  // unknown option: strictest declared
        }
    }
    return need;
}

inline void Hub::handleIntent(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs) {
    auto res = decodeIntent(payload);
    if (!res) {
        NackMsg n;
        n.code = NackCode::MALFORMED;
        sendNack(*slot.transport, n);
        return;
    }
    const IntentMsg& m = res.value();

    // 1) Rate limiter (§9.3, §10.5).
    if (!slot.session.intentLimiter.allow(nowMs)) {
        NackMsg n;
        n.code = NackCode::RATE_LIMITED;
        n.has_intent_id = true;
        n.intent_id = m.intent_id;
        sendNack(*slot.transport, n);
        return;
    }

    // 1b) §11.5(2)/RFC-015: READY gates the CONTROL plane too. A session that
    // has not adopted the catalog has, by construction, never received the
    // retained safety latch (0x0003) — it is gated shut — so letting it ACT
    // would mean acting blind to a live e-stop. Refused, never queued; the
    // client retries after its CATALOG_READY lands. Deliberately AFTER the
    // rate limiter so a pre-ready flood is throttled like any other.
    if (!slot.session.ready) {
        NackMsg n;
        n.code = NackCode::NOT_READY;
        n.has_channel_id = true;
        n.channel_id = m.channel_id;
        n.has_intent_id = true;
        n.intent_id = m.intent_id;
        sendNack(*slot.transport, n);
        return;
    }

    // 2) Idempotency ring: exact-id duplicate re-emits the stored ECHO,
    // never re-applies (§9.3).
    if (auto cached = slot.session.intentRing.lookup(m.intent_id)) {
        sendFrameToTracked(slot, FrameType::ECHO, m.channel_id, *cached, nowMs);
        return;
    }

    // 3) Catalog: channel exists + is INTENT class.
    const CatalogEntry* entry = _catalog.find(m.channel_id);
    if (!entry) {
        NackMsg n;
        n.code = NackCode::UNKNOWN_CHANNEL;
        n.has_channel_id = true;
        n.channel_id = m.channel_id;
        n.has_intent_id = true;
        n.intent_id = m.intent_id;
        sendNack(*slot.transport, n);
        return;
    }
    if (entry->cls != ChannelClass::INTENT) {
        NackMsg n;
        n.code = NackCode::CLASS_MISMATCH;
        n.has_channel_id = true;
        n.channel_id = m.channel_id;
        n.has_intent_id = true;
        n.intent_id = m.intent_id;
        sendNack(*slot.transport, n);
        return;
    }

    // 4) Access: the entry's floor RAISED by any per-field/per-option
    // annotation the value map actually invokes (RFC-025/010 — see
    // requiredAccessFor). control-level shortfall -> NOT_CONTROLLER; any other
    // (configure-level) -> ACCESS_DENIED.
    const AccessLevel needAccess = requiredAccessFor(*entry, m);
    if (uint8_t(slot.session.role) < uint8_t(needAccess)) {
        NackMsg n;
        n.code = (needAccess == AccessLevel::control) ? NackCode::NOT_CONTROLLER : NackCode::ACCESS_DENIED;
        n.has_channel_id = true;
        n.channel_id = m.channel_id;
        n.has_intent_id = true;
        n.intent_id = m.intent_id;
        sendNack(*slot.transport, n);
        return;
    }

    // 5) Precondition CAS vs cfg_gen (§9.3).
    if (m.has_precondition && m.precondition != _cfgGen) {
        NackMsg n;
        n.code = NackCode::CONFLICT;
        n.has_intent_id = true;
        n.intent_id = m.intent_id;
        sendNack(*slot.transport, n);
        return;
    }

    // 5b) The two safety-intents ops the hub handles ITSELF, before the
    // delegate is ever consulted: ESTOP_CLEAR (§11.2) and ESTOP (RFC-010).
    // Everything else on 0x0005 falls through to the delegate like any other
    // INTENT and is LATCHED afterwards by applySafetyOpLatch (RFC-025a).
    //
    // Op encoding is registry-governed: value field key 1 == a `safety_ops`
    // value.
    uint64_t safetyOp = 0;
    bool hasSafetyOp = false;
    if (m.channel_id == channels::safety_intents) {
        for (uint32_t i = 0; i < m.value_count; ++i) {
            if (m.value[i].key == 1 && m.value[i].value.kind == IntentValue::Kind::U64) {
                safetyOp = m.value[i].value.u64_val;
                hasSafetyOp = true;
                break;
            }
        }
    }

    if (hasSafetyOp && safetyOp == safety_ops::estop_clear) {
        // Belt-and-braces role gate (§11.2: "requires control+ role")
        // independent of whatever access level a given catalog declares for
        // this channel or this op — step 4 already gates it, this is a second
        // guard specific to the one op that UN-latches a safety state. A
        // catalog that forgets estop_clear's `option_access` must not thereby
        // hand the clear to a viewer.
        if (uint8_t(slot.session.role) < uint8_t(AccessLevel::control)) {
            NackMsg n;
            n.code = NackCode::NOT_CONTROLLER;
            n.has_intent_id = true;
            n.intent_id = m.intent_id;
            sendNackTracked(slot, n, nowMs);
            return;
        }
        if (!clearEstop()) {
            NackMsg n;
            n.code = NackCode::CLEAR_REFUSED;
            n.has_intent_id = true;
            n.intent_id = m.intent_id;
            sendNackTracked(slot, n, nowMs);
            return;
        }
        EchoMsg echo;
        echo.intent_id = m.intent_id;
        echo.cfg_gen = _cfgGen;
        echo.applied_count = 0;
        std::array<std::byte, 32> ebuf{};
        size_t elen = encodeEcho(echo, std::span<std::byte>(ebuf));
        if (elen > 0) {
            slot.session.intentRing.store(m.intent_id, std::span<const std::byte>(ebuf.data(), elen));
            sendFrameToTracked(slot, FrameType::ECHO, m.channel_id, std::span<const std::byte>(ebuf.data(), elen),
                               nowMs);
        }
        return;
    }

    // RFC-010 — CLIENT-ASSERTABLE E-STOP. `estop` is treated EXACTLY as a valid
    // 0xE5 ESTOP frame would be: latchEstop() calls delegate.onEstop() FIRST
    // (motion stops before any protocol bookkeeping, §11.2), then latches,
    // publishes 0x0003 and broadcasts it at critical priority to every
    // subscriber, bypassing pacing. The raw 0xE5 frame remains the
    // deframed/relay guarantee for transports that need to recognize a stop
    // without a session; this op is the trivially-implementable client path,
    // and its absence is why a UI's red button silently degraded to `stop` —
    // which maps to a decel HALT, NOT an e-stop latch.
    //
    // ROLE-EXEMPT (RFC-025b): no role check here, on purpose. Step 4's
    // per-option access is what declares that exemption to clients, and the
    // §9.3 rate limiter (step 1, already passed) is what bounds a viewer
    // looping it — a named, limited, accepted risk in §12.1. "Anyone in the
    // room may stop the machine" outranks it.
    //
    // Repeats are meaningful (§11.2: repeat-until-latch is the client's only
    // loss-recovery mechanism) so an estop while ALREADY latched is not an
    // error: latchEstop re-broadcasts, and the hub still ECHOes.
    // THE EVENT TWIN, honestly: §5.5/§11.2's "emit the EVENT twin" has NO
    // registry home yet — there is no safety EVENT channel and no
    // `session_event_kinds` value for a latch — so the raw 0xE5 path
    // (handleEstopFrame) has never emitted one either. Rather than invent a
    // wire number, this op is dispatched THROUGH handleEstopFrame: whatever
    // "exactly as a valid 0xE5 frame" means today it means here too, and when
    // the EVENT twin is registered and added there, this path inherits it for
    // free with no second implementation to keep in sync. That equivalence is
    // the actual requirement; a private event kind would have broken it.
    if (hasSafetyOp && safetyOp == safety_ops::estop) {
        EstopFrame f;
        f.cause = safety_causes::user;                  // an operator asked for it (§5.5)
        f.origin = uint8_t(slot.session.role);          // AccessLevel of the initiator
        // §5.5: seq increments per INITIATION event, so a repeat of an
        // already-latched estop keeps the seq of the initiation it is
        // repeating — otherwise subscribers see an initiation that never
        // happened. handleEstopFrame only adopts f.seq on the transition, so
        // this matches the raw-frame semantics exactly.
        f.seq = (_safetyWord & safety_bits::ESTOP) ? _estopSeq : uint16_t(_estopSeq + 1);
        handleEstopFrame(f, nowMs);

        EchoMsg echo;
        echo.intent_id = m.intent_id;
        echo.cfg_gen = _cfgGen;
        echo.applied_count = 1;
        echo.applied[0] = {1, IntentValue::ofU64(safety_ops::estop)};
        std::array<std::byte, 64> ebuf{};
        size_t elen = encodeEcho(echo, std::span<std::byte>(ebuf));
        if (elen > 0) {
            slot.session.intentRing.store(m.intent_id, std::span<const std::byte>(ebuf.data(), elen));
            sendFrameToTracked(slot, FrameType::ECHO, m.channel_id, std::span<const std::byte>(ebuf.data(), elen),
                               nowMs);
        }
        return;
    }

    // 5c) M4b: session-admin (0x0009). Handled by the hub itself for the same
    // reason ESTOP_CLEAR is — these ops mutate HUB state (the session table,
    // the trust ledger), not application state, so there is nothing sane for a
    // delegate to apply and no way for one to get it right. Everything above
    // this line has already run: rate limiting, catalog lookup, class check,
    // readiness, per-op access (the channel floor is `configure`) and the
    // cfg_gen CAS. Pairing administration inherits the whole write plane's
    // protections instead of growing its own half of each.
    if (m.channel_id == channels::session_admin) {
        handleAdminIntent(slot, m, nowMs);
        return;
    }

    // 6) Source ownership (§11.4): a channel the delegate maps to an arbiter
    // source acquires exclusive ownership BEFORE applyIntent — Conflict/
    // TakenOver are decided here, never inside the delegate.
    std::optional<uint8_t> mappedSource = _delegate.sourceForChannel(m.channel_id);
    if (mappedSource) {
        uint8_t source = *mappedSource;
        bool takeoverFlag = m.has_takeover && m.takeover;
        auto acq = _ownership.acquire(source, slot.session.session_id, slot.session.role, takeoverFlag);

        if (acq == SourceOwnershipTable::AcquireResult::Conflict) {
            // §11.4/registry: no takeover flag sent -> hint TAKEOVER_REQUIRED
            // ("retry with takeover"); takeover flag sent but role
            // insufficient -> SOURCE_CONFLICT.
            NackMsg n;
            n.code = takeoverFlag ? NackCode::SOURCE_CONFLICT : NackCode::TAKEOVER_REQUIRED;
            n.has_intent_id = true;
            n.intent_id = m.intent_id;
            sendNackTracked(slot, n, nowMs);
            return;
        }
        if (acq == SourceOwnershipTable::AcquireResult::Acquired) {
            _delegate.onSourceOwnership(source, slot.session.session_id, /*reason=*/0);
            publishControlOwnerStateIfPresent();
        } else if (acq == SourceOwnershipTable::AcquireResult::TakenOver) {
            _delegate.onSourceOwnership(source, slot.session.session_id, /*reason=*/1);
            publishControlOwnerStateIfPresent();
            emitTakeoverEvent(source, slot.session.session_id, nowMs);
        }
        // AlreadyOwner: idempotent re-activation, nothing to notify.
    }

    // 7) Delegate applies + clamps.
    IntentValueMap requested{m.value_count, m.value};
    bool cfgChanged = false;
    auto applied = _delegate.applyIntent(m.channel_id, requested, slot.session.role, cfgChanged);

    if (!applied) {
        NackMsg n;
        n.code = applied.error();
        n.has_intent_id = true;
        n.intent_id = m.intent_id;
        sendNackTracked(slot, n, nowMs);
        return;
    }

    if (cfgChanged) ++_cfgGen;

    // RFC-025a: THE HUB LATCHES ALL FOUR LEVELS (and the two RFC-025c mode
    // bits), triggered by DELEGATE ACCEPTANCE. Reaching this line IS the
    // acceptance: a delegate that does not implement HOLD returned
    // UNSUPPORTED_OP above and nothing was latched, which is the discoverable,
    // honest answer the pre-v1.0 "codes exist, nobody latches" state could not
    // give. Ordering matters — the latch happens BEFORE the ECHO, so a client
    // that reacts to its own echo cannot observe the machine mid-update.
    const uint8_t safetyWordBeforeLatch = _safetyWord;
    if (hasSafetyOp && applySafetyOpLatch(uint8_t(safetyOp), slot.session.session_id)) {
        publishSafetySnapshot();
        broadcastSafetyNow(nowMs);
        emitSafetyEdgeEvents(safetyWordBeforeLatch, nowMs);
    }

    // §11.1: a source-mapped intent succeeding clears a STOP latch (deadman)
    // regardless of which authorized session owns/sent it — "clears by any
    // new motion intent".
    if (mappedSource && (_safetyWord & safety_bits::STOP)) {
        const uint8_t before = _safetyWord;
        _safetyWord &= ~safety_bits::STOP;
        _safetyOwnerSession = 0;
        publishSafetySnapshot();
        broadcastSafetyNow(nowMs);
        emitSafetyEdgeEvents(before, nowMs);
    }

    EchoMsg echo;
    echo.intent_id = m.intent_id;
    echo.cfg_gen = _cfgGen;
    echo.applied_count = applied.value().count;
    echo.applied = applied.value().fields;

    std::array<std::byte, 256> ebuf{};
    size_t elen = encodeEcho(echo, std::span<std::byte>(ebuf));
    if (elen == 0) return;  // catalog-conformance bug (applied map too large); nothing sane to send

    slot.session.intentRing.store(m.intent_id, std::span<const std::byte>(ebuf.data(), elen));
    sendFrameToTracked(slot, FrameType::ECHO, m.channel_id, std::span<const std::byte>(ebuf.data(), elen), nowMs);
}

// ---- STREAM ingress (§9.2 c2h motion input, §10.5 rate, §11.3/§11.4 source) --

inline void Hub::sendStreamOverageNack(Slot& slot, HubSession::PublishGrant& pg, uint16_t channel_id, uint32_t nowMs) {
    // Throttle to limits::stream_ingress_overage_nack_per_s per channel: the
    // NACK is back-pressure feedback, not a per-drop echo — unthrottled it
    // would mirror the very flood it reports (§10.5). First overage always
    // NACKs; then at most one per interval.
    constexpr uint32_t kIntervalMs = 1000u / limits::stream_ingress_overage_nack_per_s;
    if (pg.everNackedOverage && !timeReached(nowMs, pg.lastOverageNackMs + kIntervalMs)) return;
    pg.everNackedOverage = true;
    pg.lastOverageNackMs = nowMs;

    NackMsg n;
    n.code = NackCode::RATE_LIMITED;
    n.has_channel_id = true;
    n.channel_id = channel_id;
    sendNackTracked(slot, n, nowMs);
}

inline void Hub::sendSourceConflictNack(Slot& slot, uint8_t source, uint16_t channel_id, uint32_t nowMs) {
    // RFC-012 — an EXPLICIT carve-out in §9.2's "STREAM bundles are never
    // NACKed", carrying exactly the precedent §10.5's RATE_LIMITED NACK set.
    //
    // The hole it fills: a producer whose arbiter source is owned by another
    // LIVE session had every bundle dropped with ZERO wire signal. Data-plane
    // bundles carry no takeover flag (§11.4, deliberate — takeover is an INTENT
    // act, never something a motion sample can do), so takeover=false is the
    // only possibility and Conflict is the only refusal. The producer was dead
    // and could not tell. Now it is told, once per (session, source), then at
    // most limits::stream_ingress_overage_nack_per_s — a NACK per dropped
    // bundle would mirror the very flood the ownership rule is protecting.
    //
    // Producers SHOULD subscribe control-owner (0x0004) for the full picture;
    // this NACK is the minimum signal that makes the failure DIAGNOSABLE.
    constexpr uint32_t kIntervalMs = 1000u / limits::stream_ingress_overage_nack_per_s;
    Slot::ConflictNack* rec = nullptr;
    for (auto& c : slot.conflictNacks) {
        if (c.used && c.source == source) { rec = &c; break; }
    }
    if (rec == nullptr) {
        for (auto& c : slot.conflictNacks) {
            if (!c.used) { c.used = true; c.source = source; rec = &c; break; }
        }
        if (rec == nullptr) return;  // table full: stay silent rather than spam
    } else if (!timeReached(nowMs, rec->lastMs + kIntervalMs)) {
        return;
    }
    rec->lastMs = nowMs;

    NackMsg n;
    n.code = NackCode::SOURCE_CONFLICT;
    n.has_channel_id = true;
    n.channel_id = channel_id;
    sendNackTracked(slot, n, nowMs);
}

inline void Hub::handleStream(Slot& slot, const FrameHeader& h, std::span<const std::byte> payload, uint32_t nowMs) {
    const uint16_t channel_id = h.channel;

    // 0) §11.5(2)/RFC-015: same reasoning as the INTENT gate — a pre-READY
    // session has never received the retained safety latch, so its motion must
    // not reach the arbiter. Dropped + counted, never NACKed (§9.2's rule for
    // the data plane; the client learns from the absent effect and from the
    // NOT_READY it gets on the control plane).
    if (!slot.session.ready) {
        ++slot.session.streamBundlesDropped;
        return;
    }

    // 1) Grant gate (§6.2/§9.2): only a channel this session was granted as a
    // publish is eligible. Unknown / ungranted / wrong-class / wrong-dir all
    // resolve to "no grant record" here (the HELLO grant loop rejected them),
    // so a single lookup covers every silent-drop case. STREAM is never
    // NACKed for these (§9.2) — drop + count.
    HubSession::PublishGrant* pg = slot.session.publishGrantFor(channel_id);
    if (!pg) {
        ++slot.session.streamBundlesDropped;
        return;
    }

    // 2) Re-derive sample size from the hub's OWN catalog and re-confirm the
    // class/direction (defensive: the grant proves it was valid at HELLO, and
    // the catalog is client-invariant §8.6, but the RX side owns its truth).
    const CatalogEntry* entry = _catalog.find(channel_id);
    if (!entry || entry->cls != ChannelClass::STREAM || entry->dir != Direction::c2h) {
        ++slot.session.streamBundlesDropped;
        return;
    }
    const size_t sampleSize = _catalog.layoutWireSize(*entry);

    // 3) Parse + re-validate §5.4 caps against the payload (n≤32, span≤20ms,
    // strictly-increasing t_off with t_off[0]==0, exact total size /
    // truncation). BundleView::parse enforces ALL of those; a violation drops
    // the bundle WHOLE.
    auto parsed = BundleView::parse(payload, sampleSize);
    if (!parsed) {
        ++slot.session.streamBundlesDropped;
        return;
    }
    const BundleView& bundle = parsed.value();

    // 3b) n==0 is malformed AT THE INGRESS LAYER (an empty bundle carries no
    // samples — BundleWriter never emits one, and delivering one to the
    // delegate would burn a rate-limiter token and a source acquisition for
    // zero motion). It is NOT a parse error: a zero-sample bundle is
    // structurally well-formed, and BundleView stays a pure structural parser.
    //
    // The t_off walk that USED to live here (strictly increasing, t_off[0]==0)
    // is gone: BundleView::parse now performs it, so re-deriving it from
    // sampleTimeUs() would be a second O(n) pass over the same 32 u16s on the
    // motion hot path for an identical verdict. Moving it into the parser is
    // what makes a CLIENT decoding an h2c bundle from a hostile hub as safe as
    // this hub is (RFC-028's symmetric obligation) — the check did not get
    // weaker here, it got applied in one more place.
    const uint8_t n = bundle.sampleCount();
    if (n == 0) {
        ++slot.session.streamBundlesDropped;
        return;
    }

    // 3c) RFC-014 SCHEDULING CONTRACT (segment-class channels only). For a
    // segment stream, `t_base + t_off[i]` IS the intended EXECUTION START of
    // sample i — not a sample timestamp — resolved through §7.2's nearest-window
    // rule. That is what the signed difference below computes: hub time is a
    // wrapping u32 of microseconds, so "how far ahead" is the nearest-window
    // interpretation of the difference, never a naive unsigned compare.
    //
    // A schedule further ahead than limits::max_future_schedule_ms (250) is
    // REJECTED whole. This replaces an unregistered 250 ms folklore constant
    // that shipped fw 2.1.45 enforced and the MFP plugin was guessing against
    // with a private SegLookaheadMs = 120 — interop by folklore, now by number.
    // Recommended client lookahead is <= half the limit.
    //
    // Only the FIRST sample is tested: §5.4 caps a bundle's whole span at 20 ms,
    // so if t_off[0] is legal every later sample is legal by construction.
    // PAST schedules are NOT rejected — a late bundle is the normal
    // consequence of jitter and the engine resolves it by playing it now.
    if (_catalog.isSegmentClass(*entry)) {
        const int32_t aheadUs = int32_t(bundle.tBase() - _clock.nowUs());
        if (aheadUs > int32_t(limits::max_future_schedule_ms) * 1000) {
            ++slot.session.streamBundlesDropped;
            return;
        }
    }

    // 4) Granted-rate token bucket on SAMPLES (§10.5). Overdraw -> drop whole +
    // throttled RATE_LIMITED NACK, but keep servicing later legal bundles.
    if (!pg->limiter.allowN(nowMs, uint32_t(n))) {
        ++slot.session.streamBundlesDropped;
        sendStreamOverageNack(slot, *pg, channel_id, nowMs);
        return;
    }

    // 5) Source ownership (§11.4): a channel the delegate maps to an arbiter
    // source acquires on the FIRST accepted bundle; later bundles are
    // AlreadyOwner (refresh only — lastRxMs was already stamped in pumpSlot(),
    // which is what pumpDeadman() reads, so the deadman window is refreshed by
    // arrival alone, §11.3/§6.5). A bundle from a non-owner while the source is
    // owned is dropped: data-plane bundles carry no takeover flag (§11.4), so
    // takeover=false is the only option and Conflict is the only refusal.
    std::optional<uint8_t> mappedSource = _delegate.sourceForChannel(channel_id);
    if (mappedSource) {
        uint8_t source = *mappedSource;
        auto acq = _ownership.acquire(source, slot.session.session_id, slot.session.role, /*takeover=*/false);
        if (acq == SourceOwnershipTable::AcquireResult::Conflict) {
            ++slot.session.streamBundlesDropped;
            // RFC-012: NOT silent any more. The bundle is still DROPPED (§9.2's
            // data-plane rule is intact — nothing is queued or retried); what
            // changes is that the producer finally learns why.
            sendSourceConflictNack(slot, source, channel_id, nowMs);
            return;
        }
        if (acq == SourceOwnershipTable::AcquireResult::Acquired) {
            _delegate.onSourceOwnership(source, slot.session.session_id, /*reason=*/0);
            publishControlOwnerStateIfPresent();
            // A producer that WINS the source has its conflict throttle cleared,
            // so a later loss of the same source NACKs immediately instead of
            // waiting out a stale interval.
            for (auto& c : slot.conflictNacks) {
                if (c.used && c.source == source) c.used = false;
            }
        }
        // AlreadyOwner: idempotent refresh, nothing to notify.
    }

    // 6) Deliver. Post-clamp / arbiter application is the delegate's job (sole-
    // caller doctrine §3.1) — the hub has done all gating.
    ++slot.session.streamBundlesAccepted;

    // RFC-045 REMOVED the STOP-clearing block that used to live here: an
    // accepted STREAM bundle silently clearing a latched STOP was a workaround
    // for the deadman's OWN forced-STOP latch (SI-15) — un-wedging a reconnect
    // after a deadman fire that, post-RFC-045, no longer latches anything in
    // the first place. It is moot, not merely obsolete: there is no longer a
    // deadman-born STOP for a resumed stream to clear. An EXPLICIT stop/estop
    // (0x0005) is still a command and still requires an explicit resume/clear
    // to lift (§11.1's INTENT-side clear in handleIntent is separate and
    // unaffected — a genuine operator move-intent still clears STOP the way it
    // always has; a raw STREAM sample no longer does).
    _delegate.onStreamBundle(channel_id, slot.session.session_id, bundle);
}

// ---- PING/PONG, GOODBYE, BLOB_REQ -------------------------------------------

inline void Hub::handlePing(Slot& slot, std::span<const std::byte> payload) {
    std::array<std::byte, 32> buf{};
    size_t n = encodePong(payload, std::span<std::byte>(buf));
    sendFrameTo(*slot.transport, FrameType::PONG, 0, std::span<const std::byte>(buf.data(), n));
}

inline void Hub::handleClock(Slot& slot, std::span<const std::byte> payload) {
    // §7.1: one CLOCK exchange. The request payload (after the frame header,
    // which carried the legacy 0x05 type byte — see clock_frame.hpp) is just
    // t0:u32 (client-µs); echo it back with t1 = hub-µs at receipt and t2 =
    // hub-µs at send. All hub-time reads go through the injected IClock so the
    // reply is deterministic under a ManualClock (§17.2). A truncated request
    // (< 4 bytes) is silently dropped — CLOCK is raw plane, never NACKed, same
    // drop-on-bad-frame rule as PING/ESTOP.
    auto req = decodeClockRequest(payload);
    if (!req) return;
    const uint32_t t1 = _clock.nowUs();  // hub-µs at receipt
    const uint32_t t2 = _clock.nowUs();  // hub-µs at send (≥ t1; equal under a ManualClock)
    std::array<std::byte, kClockReplyBytes> buf{};
    size_t n = encodeClockReply(req.value().t0, t1, t2, std::span<std::byte>(buf));
    if (n == 0) return;
    sendFrameTo(*slot.transport, FrameType::CLOCK, 0, std::span<const std::byte>(buf.data(), n));
}

inline void Hub::handleGoodbye(Slot& slot, uint32_t nowMs) {
    // §6.8: after GOODBYE the hub frees the session AND releases any control
    // ownership per §11.4's loss rules (identical to deadman) — teardownSession
    // does both. A voluntary GOODBYE is still the source's author departing, so
    // a Stop-policy source stops motion here exactly as a rude drop or a
    // deadman timeout would (there is no unmonitored path to motion, §11.3).
    teardownSession(slot, nowMs);
}

// RFC-021: ONE namespace-agnostic transfer path. Resolving "which bytes" is the
// only namespace-aware step; everything after it — chunk count, identity header,
// selective repair, CHUNK_UNAVAILABLE — is identical for the catalog and for a
// preset store, which is the point of the verb.
//
// Every resume re-runs this (see Slot::PendingBlob): the bytes are borrowed, so
// the identity is the only thing safe to keep between ticks. Re-running it also
// re-evaluates the ACCESS GATE for free, and that is the behavior you want — a
// session demoted mid-transfer (RFC-029's tripwire suspends a changed client to
// `watch`) stops receiving the trust ledger at the next chunk boundary rather
// than at the end of the document.
inline bool Hub::resolveBlobBytes(Slot& slot, BlobId& id, std::span<const std::byte>& encoded, NackMsg& nack) {
    // ---- ACCESS GATE (M4b) --------------------------------------------------
    // BLOB_REQ had NO role check at all before this. That was survivable while
    // the only namespace was the catalog (which §8.6 makes client-invariant and
    // deliberately readable by anyone who can connect), and it stopped being
    // survivable the moment a STORE could hold something that is not public —
    // the trust ledger is `configure` access, and without this gate a `watch`
    // session could enumerate every paired device on the machine.
    //
    // The rule is the SAME one SUBSCRIBE uses: the declaring catalog entry's
    // `access` is the floor. Generic, not special-cased for the ledger, so
    // every future store inherits it — and a store nobody declared is
    // CHUNK_UNAVAILABLE rather than ACCESS_DENIED, which is honest: refusing to
    // confirm the existence of something that does not exist tells an attacker
    // nothing either way.
    if (!id.isCatalog()) {
        const CatalogEntry* storeEntry = nullptr;
        for (uint16_t i = 0; i < _catalog.count; ++i) {
            const CatalogEntry& e = _catalog.entries[i];
            if (e.cls != ChannelClass::STORE) continue;
            const StoreDescriptor* sd = _catalog.storeDescriptor(e);
            if (sd != nullptr && id.has_store_id && sd->storeId == id.store_id) {
                storeEntry = &e;
                break;
            }
        }
        if (storeEntry == nullptr) {
            nack.code = NackCode::CHUNK_UNAVAILABLE;
            return false;
        }
        if (uint8_t(slot.session.role) < uint8_t(storeEntry->access)) {
            nack.code = NackCode::ACCESS_DENIED;
            nack.has_channel_id = true;
            nack.channel_id = storeEntry->id;
            return false;
        }
    }

    if (id.isCatalog()) {
        // Served by the hub itself, never by the delegate: the encoding here
        // is the same buffer the etag was computed over at construction, so
        // catalog and etag cannot drift. The transfer is byte-for-byte the
        // pre-RFC-021 catalog transfer apart from the generalized chunk header.
        encoded = std::span<const std::byte>(_catalogEncoded.data(), _catalogEncodedLen);
        id.generation = 0;
    } else if (_hasPairedStore && id.ns == blob_ns::store && id.has_store_id &&
               id.store_id == _pairedStoreId) {
        // THE TRUST LEDGER IS HUB-SERVED, not delegate-served — the same reason
        // the catalog is. The ledger IS hub state (PairingManager owns it), so
        // routing it through a delegate would let an application answer "who is
        // paired?" differently from how the hub actually decides roles. That is
        // precisely the class of divergence the catalog/etag rule exists to
        // prevent, applied to authorization instead of layout.
        const PairingManager::PairedEntry* e = _pairing.slot(id.has_slot ? id.slot : 0);
        if (e == nullptr) {
            nack.code = NackCode::CHUNK_UNAVAILABLE;  // empty slot: honest, enumerable
            return false;
        }
        // Re-encoded on EVERY resume, deliberately: _ledgerItemScratch is ONE
        // hub-wide buffer, so a transfer that cached a span into it would be
        // corrupted the moment another session asked for a different ledger
        // slot. Re-resolving per tick is what makes concurrent store transfers
        // safe without a scratch buffer per session.
        const size_t itemLen = _pairing.encodeEntry(*e, std::span<std::byte>(_ledgerItemScratch));
        if (itemLen == 0) {
            nack.code = NackCode::CHUNK_UNAVAILABLE;
            return false;
        }
        encoded = std::span<const std::byte>(_ledgerItemScratch.data(), itemLen);
        // The roster generation the caller must be consistent with. A bump
        // between two BLOB_REQs means "your enumeration went stale, start
        // over" — the store machinery already carries that for free, which is
        // the point of reusing it rather than cloning the chunker.
        id.generation = _pairing.generation();
    } else {
        auto view = _delegate.readBlob(id.ns, id.store_id, id.slot);
        if (!view) {
            // Registry CHUNK_UNAVAILABLE covers BOTH "index out of range" and
            // "the requested namespace/store/slot does not exist" (RFC-021
            // generalized its note); a hub with no store backend at all answers
            // every non-catalog request this way, which is honest and
            // discoverable rather than silent.
            nack.code = NackCode::CHUNK_UNAVAILABLE;
            return false;
        }
        encoded = view->bytes;
        id.generation = view->generation;
    }

    if (chunkCount(encoded.size()) == 0) {  // nothing to send is not a transfer
        nack.code = NackCode::CHUNK_UNAVAILABLE;
        return false;
    }
    return true;
}

inline void Hub::handleBlobReq(Slot& slot, std::span<const std::byte> payload) {
    auto res = decodeBlobReq(payload);
    if (!res) {
        NackMsg n;
        n.code = NackCode::MALFORMED;
        sendNack(*slot.transport, n);
        return;
    }
    const BlobReqMsg& m = res.value();

    BlobId id = m.blob;
    std::span<const std::byte> encoded{};
    NackMsg nack{};
    if (!resolveBlobBytes(slot, id, encoded, nack)) {
        sendNack(*slot.transport, nack);
        return;
    }
    const size_t cc = chunkCount(encoded.size());

    // §8.4 repair naming an index this blob does not have: ONE NACK for the
    // REQUEST, not one per bad index. Range-checked HERE, before anything is
    // queued, for a reason the old inline send loop got for free and a deferred
    // transfer would not: the refusal stays inside this frame's dispatch, so
    // RFC-001 stamps it with the BLOB_REQ's own seq. A client that pipelines two
    // repairs can still tell which one was refused.
    if (!m.full) {
        for (uint32_t i = 0; i < m.chunks_count; ++i) {
            if (size_t(m.chunks[i]) >= cc) {
                NackMsg n;
                n.code = NackCode::CHUNK_UNAVAILABLE;
                sendNack(*slot.transport, n);
                return;
            }
        }
    }

    // Arm the cursor, SUPERSEDING whatever this session still had in flight: the
    // newest request is the one the client is waiting on, and running both would
    // interleave two blobs' chunks down one link for no one's benefit. (A client
    // that wants the rest of the old one asks again — that is what selective
    // repair is.)
    Slot::PendingBlob& pb = slot.blob;
    pb = Slot::PendingBlob{};
    pb.active = true;
    pb.id = id;
    pb.totalBytes = uint32_t(encoded.size());
    pb.generation = id.generation;
    pb.hasReqSeq = _dispatchSeqValid;
    pb.reqSeq = _dispatchSeq;
    pb.full = m.full;
    if (!m.full) {
        pb.count = uint16_t(m.chunks_count);  // decodeBlobReq caps this at kBlobReqMaxChunks
        for (uint32_t i = 0; i < m.chunks_count; ++i) pb.chunks[i] = m.chunks[i];
    }

    // NOTHING IS SENT HERE. update()'s per-slot walk drains the cursor at the
    // end of this very tick, so a blob within one tick's budget still completes
    // in the tick it was asked for — but it does so under the SAME budget every
    // later tick uses, instead of the request tick quietly getting a double
    // helping on top of its own parse/resolve work.
}

// §8.4/§13.1: the drain half of a resumable transfer. Bounded work, and the
// only thing that pauses it is the transport saying "not now".
inline void Hub::pumpBlobTransfer(Slot& slot) {
    Slot::PendingBlob& pb = slot.blob;
    if (!pb.active) return;
    if (slot.transport == nullptr || !slot.session.occupied()) {
        // Belt-and-braces with teardownSession()'s clear: a cursor pointing at a
        // departed session is the exact shape of the source-ownership field bug
        // (§6.8), so it is checked here too rather than trusted to one caller.
        pb.active = false;
        return;
    }

    // RFC-001: a refusal decided ticks after the request is still PROVOKED by
    // that request. sendNack() can only stamp seqs during a dispatch and this is
    // not one, so carry the armed seq explicitly.
    auto refuse = [&](NackMsg n) {
        n.has_intent_seq = pb.hasReqSeq;
        n.intent_seq = pb.reqSeq;
        sendNack(*slot.transport, n);
        pb.active = false;
    };

    BlobId id = pb.id;
    std::span<const std::byte> encoded{};
    NackMsg gateNack{};
    if (!resolveBlobBytes(slot, id, encoded, gateNack)) {
        refuse(gateNack);  // vanished, or the session lost the role that had it
        return;
    }
    // ...and it must still be THE SAME document. Splicing the head of one
    // revision onto the tail of another yields a reassembly that never existed
    // on this hub; the receiver would either fail its etag/generation check
    // (best case) or adopt a Frankenstein as ground truth (the case this
    // product cannot afford). A moved blob ends the transfer; the client
    // re-requests and gets a coherent one.
    if (uint32_t(encoded.size()) != pb.totalBytes || id.generation != pb.generation) {
        NackMsg n;
        n.code = NackCode::CHUNK_UNAVAILABLE;
        refuse(n);
        return;
    }

    const size_t cc = chunkCount(encoded.size());
    std::array<std::byte, kBlobChunkHeaderBytes + limits::catalog_chunk_payload> cbuf{};
    for (size_t budget = 0; budget < kBlobChunksPerTick; ++budget) {
        uint16_t idx = 0;
        if (pb.full) {
            if (size_t(pb.nextIndex) >= cc) break;  // whole blob delivered
            idx = pb.nextIndex;
        } else {
            if (pb.cursor >= pb.count) break;  // every named index delivered
            idx = pb.chunks[pb.cursor];
        }

        const size_t n = fillBlobChunk(id, encoded, idx, std::span<std::byte>(cbuf));
        if (n == 0) {
            // Unreachable for a well-behaved blob: handleBlobReq range-checked
            // every repair index and the size check above catches a resize. Kept
            // because "unreachable" plus a raw index is how out-of-range reads
            // happen — and the answer is the same ONE CHUNK_UNAVAILABLE for the
            // request that the request-time path emits, mid-resume or not.
            NackMsg u;
            u.code = NackCode::CHUNK_UNAVAILABLE;
            refuse(u);
            return;
        }
        // Plain sendFrameTo, NOT sendFrameToTracked: §10.4's slow-consumer clock
        // is for NEVER-SHED critical traffic, and a blob is the opposite — bulk,
        // sheddable, resumable. Routing it through the eviction tracker would
        // turn "this client's queue is briefly full" into "evict this client".
        if (!sendFrameTo(*slot.transport, FrameType::BLOB_CHUNK, channels::catalog,
                         std::span<const std::byte>(cbuf.data(), n))) {
            // BACKPRESSURE, AND IT IS NOT AN ERROR (§13.1: write() == false means
            // "not accepted right now", and the caller decides retry vs drop —
            // this hub retries). The cursor does not advance, so this exact chunk
            // is the first thing tried next tick. No NACK, no teardown, no
            // warning-level anything: a full egress queue is the transport doing
            // its job, and it is the ONLY thing that pauses a transfer.
            return;
        }
        if (pb.full) {
            ++pb.nextIndex;
        } else {
            ++pb.cursor;
        }
    }

    // Completion check runs after the budget loop as well as inside it, so a
    // transfer that exactly fills its last budget retires now rather than
    // costing an extra tick's resolve to discover it has nothing left to do.
    const bool done = pb.full ? (size_t(pb.nextIndex) >= cc) : (pb.cursor >= pb.count);
    if (done) pb.active = false;
}

// ---- STATE pacing (§9.1) ----------------------------------------------------
// conflated push using RetainedStore + SubscriptionTable

inline Hub::PushRecord* Hub::findOrCreatePushRecord(Slot& slot, uint16_t channel_id) {
    for (auto& pr : slot.pushRecords) {
        if (pr.valid && pr.channel_id == channel_id) return &pr;
    }
    for (auto& pr : slot.pushRecords) {
        if (!pr.valid) {
            pr.channel_id = channel_id;
            pr.lastSeq = 0;
            // NOTE: `valid` here means "slot allocated", flipped true only
            // once an actual push has been recorded (see pumpStatePacing) —
            // that is what lets a brand-new record's changePending compute
            // as "always due" on its very first check.
            return &pr;
        }
    }
    return nullptr;  // capacity exhausted: never happens for a conformant catalog
}

inline void Hub::pumpStatePacing(Slot& slot, uint32_t nowMs) {
    // §8.4/RFC-015: the data-plane gate. Not ready = nothing emitted — no
    // retained push, no periodic STATE. This is a GATE, not a spool: nothing
    // is queued or buffered, because the retained value already lives exactly
    // once in _retained and each grant's everPushed==false keeps the push
    // pending until the session can actually decode it.
    if (!slot.session.ready) return;

    for (auto& sub : slot.session.subs) {
        const CatalogEntry* entry = _catalog.find(sub.channel_id);
        if (!entry || entry->cls != ChannelClass::STATE) continue;  // STREAM/EVENT pacing is out of M4/M5 scope
        auto retained = _retained.get(sub.channel_id);
        if (!retained) continue;  // nothing published for this channel yet

        PushRecord* pr = findOrCreatePushRecord(slot, sub.channel_id);
        bool changePending = (pr == nullptr) || !pr->valid || seqIsNewer(retained->seq, pr->lastSeq);

        // §9.1's push-on-grant guarantee (dueForPush()'s !everPushed case) is
        // never shed — the very first retained push after a fresh grant goes
        // out regardless of congestion, exactly like every other never-shed
        // send; §10.4's decimation applies only to the STEADY-STATE periodic/
        // on-change pushes that follow.
        bool firstPushSinceGrant = !sub.everPushed;
        if (!sub.dueForPush(nowMs, changePending)) continue;

        ShedDecision decision = firstPushSinceGrant
                                    ? ShedDecision::Send
                                    : shedDecision(sub.priority, entry->cls, slot.congestionLevel,
                                                   _catalog.isSegmentClass(*entry));  // RFC-014/023

        bool transmit = true;
        if (decision == ShedDecision::Drop) {
            transmit = false;
        } else if (decision == ShedDecision::Decimate2x || decision == ShedDecision::Decimate4x ||
                   decision == ShedDecision::ConflateHard) {
            // "decimation = skip N-1 of N due pushes per channel counter":
            // ConflateHard uses the same N=4 mechanic as Decimate4x — SPEC
            // §10.4's "stretch periodic pushes toward on-change-only" for
            // STATE and "halve the effective sample rate" for STREAM are the
            // same shape of throttle, just named per-class.
            uint32_t n = (decision == ShedDecision::Decimate2x) ? 2 : 4;
            if (pr) {
                ++pr->shedCounter;
                transmit = (pr->shedCounter % n == 0);
            }
        }

        if (transmit) {
            if (sendFrameTo(*slot.transport, FrameType::STATE, sub.channel_id, retained->payload, retained->seq)) {
                sub.markPushed(nowMs);
                if (pr) {
                    pr->lastSeq = retained->seq;
                    pr->valid = true;
                }
            }
        } else {
            // Advance the pacing clock even on a shed skip so a periodic
            // channel's next natural due-opportunity lands one grant-period
            // later, not on the very next update() tick (which would spam
            // the decimation counter instead of actually throttling anything
            // — see the M5 report's design note on this).
            sub.markPushed(nowMs);
        }
    }
}

// ---- Publication API --------------------------------------------------------

inline bool Hub::publishState(uint16_t channel_id, std::span<const std::byte> payload) {
    return _retained.publish(channel_id, payload).has_value();
}

inline bool Hub::publishEvent(uint16_t channel_id, std::span<const std::byte> encodedEventPayload) {
    bool any = false;
    for (auto& slot : _slots) {
        if (!slot.session.occupied()) continue;
        if (!slot.session.subs.find(channel_id)) continue;
        // The channel id is stored WITH the bytes, not inferred at drain time:
        // one queue carries every EVENT channel a session subscribes to, and a
        // drain that stamped the triggering channel onto whatever it popped
        // would ship (say) log bytes under session-events' id — undecodable
        // against the receiving client's catalog schema.
        slot.session.events.push(channel_id, encodedEventPayload);
        any = true;
    }
    if (any) {
        for (auto& slot : _slots) {
            if (!slot.session.occupied()) continue;
            pumpEventDrain(slot);
        }
    }
    return any;
}

inline void Hub::pumpEventDrain(Slot& slot) {
    // RFC-015: the READY gate covers the WHOLE data plane, EVENTs included — a
    // session that cannot decode a channel's catalog schema cannot decode its
    // event bodies either. Queued, never discarded: the bounded queue holds
    // them (drop-oldest, counted) and this drain fires the instant the session
    // becomes ready. That is also what makes RFC-017 replay work on a client
    // that is still transferring the catalog.
    if (!slot.session.ready || slot.transport == nullptr) return;
    while (auto ev = slot.session.events.pop()) {
        sendFrameTo(*slot.transport, FrameType::EVENT, ev->channel_id, ev->bytes);
    }
}

// ---- RFC-017: the log channel (0x0008) --------------------------------------
// publication + replay_depth backfill

inline void Hub::replayEventsOnGrant(Slot& slot, uint16_t channel_id) {
    // §9.4's no-replay rule gains exactly one exception: "except where a
    // channel's catalog entry declares a replay depth". Presence of key 13 IS
    // the opt-in, so this is catalog-driven rather than channel-hardcoded — a
    // hub that declares replay_depth on some other EVENT channel wires its own
    // ring here the same way.
    const CatalogEntry* entry = _catalog.find(channel_id);
    if (!entry || entry->cls != ChannelClass::EVENT) return;
    if (!entry->hasReplayDepth || entry->replayDepth == 0) return;
    if (channel_id != channels::log) return;  // the only ring this hub keeps

    _logRing.forEachNewest(size_t(entry->replayDepth), [&](std::span<const std::byte> bytes) {
        slot.session.events.push(channel_id, bytes);
    });
}

inline bool Hub::publishLog(uint8_t level, std::string_view tag, std::string_view message) {
    const CatalogEntry* entry = _catalog.find(channels::log);
    if (entry == nullptr) return false;  // this hub has no log channel

    // Senders truncate diagnostics (RFC-022.5). A log line that VANISHES for
    // being 3 bytes too long is the worst failure mode a log can have.
    if (tag.size() > kLogTagMaxBytes) tag = tag.substr(0, kLogTagMaxBytes);
    if (message.size() > kLogMessageMaxBytes) message = message.substr(0, kLogMessageMaxBytes);

    const uint32_t nowMs = _clock.nowMs();

    EventMsg ev{};
    ev.channel_id = channels::log;
    ev.timestamp = nowMs;
    ev.event_kind = log_events::entry;
    ev.has_body = true;  // scoped `body` (40): keys are 0x0008's OWN schema keys
    ev.body_count = 4;
    ev.body[0] = IntentValueField{log_body::level, IntentValue::ofU64(level)};
    ev.body[1] = IntentValueField{log_body::tag, IntentValue::ofTstr(tag)};
    ev.body[2] = IntentValueField{log_body::hub_ms, IntentValue::ofU64(nowMs)};
    ev.body[3] = IntentValueField{log_body::message, IntentValue::ofTstr(message)};

    std::array<std::byte, EventReplayRing<>::kSlotCapacity> buf{};
    size_t n = encodeEvent(ev, std::span<std::byte>(buf));
    if (n == 0) return false;
    std::span<const std::byte> encoded(buf.data(), n);

    // Ring FIRST: the ring is the record, and it must hold the line even when
    // nobody is subscribed right now — that is the entire point of replay.
    _logRing.push(encoded);
    publishEvent(channels::log, encoded);
    return true;
}

inline uint32_t Hub::logDropped() const { return _logRing.dropped(); }

// ---- Safety: ESTOP/STOP latch + critical-priority broadcast (§11.1, §11.2, §11.3) --

inline std::array<std::byte, 9> Hub::buildSafetyPayload() const {
    std::array<std::byte, 9> buf{};
    std::span<std::byte> s(buf);
    putU8(s.subspan(0, 1), _safetyWord);
    putU8(s.subspan(1, 1), _safetyCause);
    putU32(s.subspan(2, 4), _safetyOwnerSession);
    putU16(s.subspan(6, 2), _estopSeq);
    // RFC-025c APPENDED byte 8: `modes` (safety_mode_bits). Bytes 0..7 are
    // untouched, which is the whole point of append-only evolution — an old
    // client that decodes only the first 8 bytes still reads every field it
    // knew about at exactly the offset it knew about.
    putU8(s.subspan(8, 1), _safetyModes);
    return buf;
}

inline void Hub::publishSafetySnapshot() {
    _retained.publish(channels::safety, std::span<const std::byte>(buildSafetyPayload()));
}

inline void Hub::broadcastSafetyNow(uint32_t nowMs) {
    auto retained = _retained.get(channels::safety);
    if (!retained) return;
    for (auto& slot : _slots) {
        if (!slot.session.occupied()) continue;
        // A PARKED session keeps its slot, grants and subscriptions but has NO
        // transport (RFC-042; detachTransport() nulls it). sendFrameToTracked()
        // takes an ITransport&, so a null here is a panic, not a missed frame —
        // see TRAPS T13. Skipping loses nothing: the snapshot is retained and
        // §9.1 re-pushes it the instant the session reattaches. Deliberately
        // NOT routed through trackCriticalSend(): an unattached session is not
        // a congested link and must not be aged toward eviction as one.
        if (slot.transport == nullptr) continue;
        // RFC-015: even the never-shed critical broadcast respects the gate —
        // a session that cannot decode a packed safety snapshot gains nothing
        // from receiving one, and §11.5(2) is satisfied instead by the retained
        // push that fires the instant it becomes ready.
        if (!slot.session.ready) continue;
        if (!slot.session.subs.find(channels::safety)) continue;
        if (sendFrameToTracked(slot, FrameType::STATE, channels::safety, retained->payload, nowMs, retained->seq)) {
            slot.session.subs.find(channels::safety)->markPushed(nowMs);
            PushRecord* pr = findOrCreatePushRecord(slot, channels::safety);
            if (pr) {
                pr->lastSeq = retained->seq;
                pr->valid = true;
            }
        }
    }
}

inline void Hub::handleEstopFrame(const EstopFrame& f, uint32_t nowMs) {
    const uint8_t before = _safetyWord;
    if (!(_safetyWord & safety_bits::ESTOP)) {
        _delegate.onEstop(f.cause, f.origin);
        _safetyWord |= safety_bits::ESTOP;
        _safetyCause = f.cause;
        _estopSeq = f.seq;
        publishSafetySnapshot();
    }
    // Always re-broadcast NOW, bypassing normal pacing (§10.1 critical
    // priority, §10.4's ESTOP exemption) — repeats are the client's only
    // loss-recovery mechanism (§11.2), so a repeat must be able to trigger a
    // fresh delivery attempt even once already latched.
    broadcastSafetyNow(nowMs);
    // §9.4/§5.5/§11.2: EMIT THE EVENT TWIN. Only on the TRANSITION — the
    // re-broadcast above is loss recovery for a latch that already happened,
    // and announcing an edge that did not happen would make any client counting
    // initiations count wrong. emitSafetyEdgeEvents compares before/after and
    // is therefore silent on a repeat by construction, with no second
    // "did it change?" test to get out of sync with the one above.
    //
    // M4a routed the RFC-010 client `estop` op through this same function
    // precisely so this landing would fix BOTH the raw-frame path and the op
    // path at one call site. It does.
    emitSafetyEdgeEvents(before, nowMs);
}

inline void Hub::latchEstop(uint8_t cause, uint8_t origin, uint16_t estop_seq) {
    // Funnelled through handleEstopFrame rather than duplicating its body: the
    // two were byte-for-byte the same logic modulo where `nowMs` came from, and
    // a duplicated latch is exactly the kind of thing that acquires an EVENT
    // twin in one copy and not the other.
    EstopFrame f;
    f.cause = cause;
    f.origin = origin;
    f.seq = estop_seq;
    handleEstopFrame(f, _clock.nowMs());
}

inline bool Hub::estopLatched() const { return (_safetyWord & safety_bits::ESTOP) != 0; }

inline bool Hub::stopLatched() const { return (_safetyWord & safety_bits::STOP) != 0; }

inline uint8_t Hub::safetyWord() const { return _safetyWord; }

inline uint8_t Hub::safetyModes() const { return _safetyModes; }

inline void Hub::setSafetyModes(bool manualOverride, bool bypassLimits) {
    const uint8_t next = uint8_t((manualOverride ? safety_mode_bits::OVERRIDE : 0) |
                                 (bypassLimits ? safety_mode_bits::BYPASS : 0));
    if (next == _safetyModes) return;  // ground truth unchanged: no publish, no wake-up
    _safetyModes = next;
    publishSafetySnapshot();
    broadcastSafetyNow(_clock.nowMs());
}

// RFC-010 / RFC-025a / RFC-025c: THE HUB LATCHES ALL FOUR LEVELS (plus the two
// mode bits). Called only after the delegate ACCEPTED the op, so "the delegate
// does not implement HOLD" resolves to a NACK UNSUPPORTED_OP from the delegate
// and no latch ever happens — discoverable and honest, instead of the pre-v1.0
// behavior where HOLD/PAUSE had registry codes and wire bits but no rule about
// who set them, so a generic client could not know whether sending HOLD did
// anything at all on an arbitrary hub.
//
// ESTOP (op 6) and ESTOP_CLEAR (op 1) are NOT here: they are hub-handled before
// the delegate is ever consulted, because §11.2 requires motion to stop BEFORE
// protocol bookkeeping. Everything in this function is post-acceptance
// bookkeeping by definition.
inline bool Hub::applySafetyOpLatch(uint8_t op, uint32_t sessionId) {
    const uint8_t beforeWord = _safetyWord;
    const uint8_t beforeModes = _safetyModes;
    const uint8_t beforeCause = _safetyCause;
    const uint32_t beforeOwner = _safetyOwnerSession;

    switch (op) {
        case safety_ops::stop:
            _safetyWord |= safety_bits::STOP;
            _safetyCause = safety_causes::user;
            _safetyOwnerSession = sessionId;
            break;
        case safety_ops::hold:
            _safetyWord |= safety_bits::HOLD;
            _safetyCause = safety_causes::user;
            _safetyOwnerSession = sessionId;
            break;
        case safety_ops::pause:
            _safetyWord |= safety_bits::PAUSE;
            _safetyCause = safety_causes::user;
            _safetyOwnerSession = sessionId;
            break;
        case safety_ops::resume:
            // §11.1: resume lifts HOLD and PAUSE. It deliberately does NOT lift
            // STOP (which clears on new motion intent, see handleIntent) and
            // certainly not ESTOP (which needs estop_clear + its preconditions).
            _safetyWord &= uint8_t(~(safety_bits::HOLD | safety_bits::PAUSE));
            break;
        case safety_ops::override_on:  _safetyModes |= safety_mode_bits::OVERRIDE; break;
        case safety_ops::override_off: _safetyModes &= uint8_t(~safety_mode_bits::OVERRIDE); break;
        case safety_ops::bypass_on:    _safetyModes |= safety_mode_bits::BYPASS; break;
        case safety_ops::bypass_off:   _safetyModes &= uint8_t(~safety_mode_bits::BYPASS); break;
        default:
            return false;  // an op with no latched representation (or an unknown one)
    }
    return _safetyWord != beforeWord || _safetyModes != beforeModes ||
           _safetyCause != beforeCause || _safetyOwnerSession != beforeOwner;
}

inline bool Hub::clearEstop() {
    // §11.2: "the hub MUST refuse (CLEAR_REFUSED) unless (a) the latched
    // cause is resolved ... (b) motion is at zero velocity, and (c) no other
    // stop level is pending escalation." (a)/(b)/(c) are machine-domain
    // conditions this library cannot see — delegate.canClearEstop() is the
    // hook; this method only enforces "must be latched" and "clearing never
    // restarts motion" (it never calls into any motion path).
    if (!(_safetyWord & safety_bits::ESTOP)) return false;
    if (!_delegate.canClearEstop()) return false;

    const uint8_t before = _safetyWord;
    _safetyWord &= ~safety_bits::ESTOP;
    publishSafetySnapshot();
    const uint32_t nowMs = _clock.nowMs();
    broadcastSafetyNow(nowMs);
    emitSafetyEdgeEvents(before, nowMs);  // §9.4 duality: the release is an edge too
    return true;
}

// ---- M5: pairing (§12.2) ----------------------------------------------------

inline void Hub::openPairingWindow(std::span<const char> pinAscii) { _pairing.openWindow(pinAscii, _clock.nowMs()); }

inline void Hub::closePairingWindow() { _pairing.closeWindow(); }

inline PairingManager& Hub::pairing() { return _pairing; }

inline void Hub::handlePairReq(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs) {
    auto res = decodePairReq(payload);
    if (!res) {
        NackMsg n;
        n.code = NackCode::MALFORMED;
        sendNack(*slot.transport, n);
        return;
    }
    const PairReqMsg& m = res.value();

    // RFC-027: NO PROOF IS NOT AN ERROR — it is mode (a) or (c). A bare
    // PAIR_REQ is a KNOCK, and routing it here is what lets a device with one
    // button and no display pair at all. Only a request that DOES carry a proof
    // takes the §12.2 PIN path below.
    if (!m.has_pin_proof) {
        handleKnock(slot, m, nowMs);
        return;
    }

    std::array<std::byte, limits::token_bytes> tokenBuf{};
    auto outcome = _pairing.handlePairReq(std::span<const std::byte>(m.instance_id),
                                          std::span<const std::byte>(m.pin_proof), std::span<const std::byte>(slot.nonce),
                                          _rng, nowMs, AccessLevel::control, std::span<std::byte>(tokenBuf),
                                          _crypto);

    switch (outcome) {
        case PairingManager::PairOutcome::Granted: {
            PairGrantMsg g{};
            g.token = tokenBuf;
            g.roles = uint8_t(AccessLevel::control);
            // M4c seam, same as the knock path: the hub's durable identity is
            // delivered AT the ceremony when one exists.
            std::array<std::byte, kTrustPubkeyMaxBytes> pk{};
            const size_t pkLen = _crypto.publicKey(std::span<std::byte>(pk));
            if (pkLen > 0 && pkLen <= pk.size()) {
                g.has_trust = true;
                g.trust_map.has_hub_pubkey = true;
                g.trust_map.hub_pubkey_len = uint8_t(pkLen);
                g.trust_map.hub_pubkey = pk;
            }
            std::array<std::byte, 96> buf{};
            size_t n = encodePairGrant(g, std::span<std::byte>(buf));
            if (n > 0) {
                sendFrameTo(*slot.transport, FrameType::PAIR_GRANT, 0, std::span<const std::byte>(buf.data(), n));
            }
            // The ledger entry PairingManager just minted knows only the
            // instance id; give it the labels this session already told us, so
            // a PIN pairing shows up in the roster looking like a device rather
            // than a hex blob — identical treatment to the knock path.
            if (auto* e = _pairing.findByInstance(std::span<const std::byte>(m.instance_id))) {
                e->kind.assign(slot.session.clientKind.view());
                e->name.assign(slot.session.clientName.view());
                if (slot.session.hasClientVer) e->version.assign(slot.session.clientVer.view());
                e->presentationMode = slot.session.presentationMode;
            }
            slot.session.role = AccessLevel::control;  // upgrade in place, like every other mode
            emitPairingEvent(pairing_events::granted, std::span<const std::byte>(m.instance_id),
                             slot.session.clientName.view(), pairing_modes::pin_proof, AccessLevel::control,
                             slot.session.clientVer.view(), nowMs);
            publishPairedRosterState();
            break;
        }
        case PairingManager::PairOutcome::Denied: {
            NackMsg n;
            n.code = NackCode::PAIRING_DENIED;
            sendNack(*slot.transport, n);
            break;
        }
        case PairingManager::PairOutcome::WindowClosed: {
            NackMsg n;
            n.code = NackCode::PAIRING_REQUIRED;
            sendNack(*slot.transport, n);
            break;
        }
    }
}

// ---- M4b: the §9.4 EVENT TWIN of the safety latch (channel 0x000E) ----------
//
// ONE FUNCTION, called from every site that mutates `_safetyWord`, taking the
// word as it was BEFORE. That shape is deliberate: it makes "did an edge
// happen?" a single derivation from before/after rather than a judgement each
// call site makes for itself, so a repeat can never accidentally announce an
// initiation and a new latch can never be silently swallowed. Same reasoning
// that made teardownSession() one function — the bug class is identical, six
// call sites drifting apart.
//
// Silent by construction when the catalog does not declare 0x000E (a hub
// without the channel keeps every §11.2 guarantee, because the LATCH was always
// the load-bearing half), or when nothing actually moved.

inline void Hub::emitSafetyEdgeEvents(uint8_t beforeWord, uint32_t nowMs) {
    if (_catalog.find(channels::safety_events) == nullptr) return;
    const uint8_t after = _safetyWord;
    if (after == beforeWord) return;

    constexpr uint8_t kStopBits = uint8_t(safety_bits::STOP | safety_bits::HOLD | safety_bits::PAUSE);
    const uint8_t newlySet = uint8_t(after & uint8_t(~beforeWord));
    const uint8_t newlyClear = uint8_t(beforeWord & uint8_t(~after));

    auto emit = [&](uint8_t kind, uint8_t level) {
        EventMsg ev{};
        ev.channel_id = channels::safety_events;
        ev.timestamp = nowMs;
        ev.event_kind = kind;
        // §9.4: name the STATE frame this edge corresponds to. That is what
        // lets a client which missed the edge reconcile against the latch it
        // DID receive, instead of holding two unrelated facts.
        if (auto retained = _retained.get(channels::safety)) {
            ev.has_seq_of_state = true;
            ev.seq_of_state = retained->seq;
        }
        ev.has_body = true;
        ev.body_count = 0;
        ev.body[ev.body_count++] = {safety_body::word, IntentValue::ofU64(_safetyWord)};
        ev.body[ev.body_count++] = {safety_body::cause, IntentValue::ofU64(_safetyCause)};
        ev.body[ev.body_count++] = {safety_body::owner_session, IntentValue::ofU64(_safetyOwnerSession)};
        ev.body[ev.body_count++] = {safety_body::estop_seq, IntentValue::ofU64(_estopSeq)};
        if (level != 0) ev.body[ev.body_count++] = {safety_body::level, IntentValue::ofU64(level)};
        std::array<std::byte, 96> buf{};
        size_t n = encodeEvent(ev, std::span<std::byte>(buf));
        if (n > 0) publishEvent(channels::safety_events, std::span<const std::byte>(buf.data(), n));
    };

    if (newlySet & safety_bits::ESTOP) emit(safety_events::estop_latched, 0);
    if (newlyClear & safety_bits::ESTOP) emit(safety_events::estop_cleared, 0);
    // ONE edge per direction even when an action moved several bits: `level`
    // carries the mask, so "resume lifted HOLD and PAUSE" is one operator
    // action reported once, not two a UI has to re-correlate.
    if (const uint8_t s = uint8_t(newlySet & kStopBits)) emit(safety_events::stop_latched, s);
    if (const uint8_t c = uint8_t(newlyClear & kStopBits)) emit(safety_events::stop_cleared, c);
}

// ---- M4b: pairing + trust (RFC-027 modes (a)/(b)/(c), RFC-029 items 2 & 4) --

inline const PairingManager& Hub::pairing() const { return _pairing; }
inline ICrypto& Hub::crypto() { return _crypto; }

inline void Hub::setPresenceDefaultRole(AccessLevel r) { _pairing.setPresenceDefaultRole(r); }
inline void Hub::setKnockApproveEnabled(bool on) { _pairing.setKnockApproveEnabled(on); }
inline void Hub::setTrustChangeAutoKeepMax(AccessLevel r) { _pairing.setTrustChangeAutoKeepMax(r); }
inline void Hub::setWallClockSeconds(uint32_t epochSeconds) { _pairing.setWallClockSeconds(epochSeconds); }
inline bool Hub::presenceWindowOpen() const { return _pairing.presenceWindowOpen(_clock.nowMs()); }

inline void Hub::openPresenceWindow() {
    const uint32_t nowMs = _clock.nowMs();
    if (_pairing.presenceWindowOpen(nowMs)) return;  // already open: not an edge
    _pairing.openPresenceWindow(nowMs);
    _presenceWasOpen = true;
    // THREE in-band signals, because RFC-027(c) requires window state to be
    // observable by any watch session and a hub may have no LED: the EVENT
    // (here), the 0x000A snapshot bit (below), and `trust`.pairing_modes in
    // every WELCOME issued while it is open (handleHello).
    emitPairingEvent(pairing_events::window_opened, {}, {}, pairing_modes::push_to_pair,
                     _pairing.presenceGrantRole(), {}, nowMs);
    publishPendingPairingState(nowMs);
}

inline void Hub::closePresenceWindow() {
    const uint32_t nowMs = _clock.nowMs();
    if (!_pairing.presenceWindowOpen(nowMs)) return;
    _pairing.closePresenceWindow();
    _presenceWasOpen = false;
    emitPairingEvent(pairing_events::window_closed, {}, {}, pairing_modes::push_to_pair,
                     AccessLevel::watch, {}, nowMs);
    publishPendingPairingState(nowMs);
}

inline void Hub::emitPairingEvent(uint8_t kind, std::span<const std::byte> instance_id, std::string_view name,
                                  uint8_t mode, AccessLevel role, std::string_view version, uint32_t nowMs) {
    if (_catalog.find(channels::pairing_events) == nullptr) return;
    EventMsg ev{};
    ev.channel_id = channels::pairing_events;
    ev.timestamp = nowMs;
    ev.event_kind = kind;
    ev.has_body = true;
    ev.body_count = 0;
    // NOTE WHAT NEVER APPEARS: the token. This channel is `watch`-visible on
    // purpose (that is how window state stays observable without an LED), and a
    // token is the one field that would make that a mistake.
    if (!instance_id.empty()) {
        ev.body[ev.body_count++] = {pairing_body::instance_id, IntentValue::ofBstr(instance_id)};
    }
    if (!name.empty()) ev.body[ev.body_count++] = {pairing_body::name, IntentValue::ofTstr(name)};
    ev.body[ev.body_count++] = {pairing_body::mode, IntentValue::ofU64(mode)};
    ev.body[ev.body_count++] = {pairing_body::role, IntentValue::ofU64(uint8_t(role))};
    if (!version.empty()) ev.body[ev.body_count++] = {pairing_body::version, IntentValue::ofTstr(version)};
    std::array<std::byte, 160> buf{};
    size_t n = encodeEvent(ev, std::span<std::byte>(buf));
    if (n > 0) publishEvent(channels::pairing_events, std::span<const std::byte>(buf.data(), n));
}

inline void Hub::publishPendingPairingState(uint32_t nowMs) {
    if (_catalog.find(channels::pending_pairing) == nullptr) return;
    std::array<std::byte, kPendingPayloadBytes> buf{};
    std::span<std::byte> s(buf);
    const auto& pend = _pairing.pending();
    putU16(s.subspan(0, 2), _pairing.generation());
    putU8(s.subspan(2, 1), uint8_t(pend.count()));
    putU8(s.subspan(3, 1), _pairing.presenceWindowOpen(nowMs) ? pending_pairing_flags::window_open : uint8_t(0));
    size_t off = 4;
    for (size_t i = 0; i < kPendingSlots; ++i) {
        const PendingKnock* k = pend.at(i);
        if (k != nullptr && k->used) {
            // instance_id as two u32 LE halves — `packed_field_types` has no
            // u64, and the concatenated bytes are byte-identical to one. See
            // channel/trust_channels.hpp for why that beat adding a wire type
            // the whole protocol would then have to carry for one field.
            uint32_t lo = 0, hi = 0;
            for (size_t b = 0; b < 4; ++b) lo |= uint32_t(uint8_t(k->instance_id[b])) << (8 * b);
            for (size_t b = 0; b < 4; ++b) hi |= uint32_t(uint8_t(k->instance_id[4 + b])) << (8 * b);
            putU32(s.subspan(off, 4), lo);
            putU32(s.subspan(off + 4, 4), hi);
            putU8(s.subspan(off + 8, 1), k->mode);
            // SECONDS, not milliseconds: the 120 s window is 120000 ms and does
            // not fit the u16 the slot layout allocates. Found while
            // implementing; the registry note was corrected in the same commit
            // rather than the code quietly clamping to 65 s.
            const uint32_t remainMs = timeReached(nowMs, k->expiresAtMs) ? 0u : uint32_t(k->expiresAtMs - nowMs);
            uint32_t remainS = (remainMs + 999u) / 1000u;
            if (remainS > 0xFFFFu) remainS = 0xFFFFu;
            putU16(s.subspan(off + 9, 2), uint16_t(remainS));
            const std::string_view nm = k->name.view();
            for (size_t b = 0; b < 16; ++b) {
                s[off + 11 + b] = std::byte(b < nm.size() ? uint8_t(nm[b]) : 0);
            }
        }
        off += kPendingSlotBytes;
    }
    _retained.publish(channels::pending_pairing, std::span<const std::byte>(buf));
}

inline void Hub::publishPairedRosterState() {
    if (_catalog.find(channels::paired_devices_roster) == nullptr) return;
    std::array<std::byte, kPairedRosterBytes> buf{};
    std::span<std::byte> s(buf);
    putU16(s.subspan(0, 2), _pairing.generation());
    putU8(s.subspan(2, 1), uint8_t(_pairing.entryCount()));
    putU8(s.subspan(3, 1), uint8_t(PairingManager::kMaxPaired));
    _retained.publish(channels::paired_devices_roster, std::span<const std::byte>(buf));
}

inline void Hub::pumpPairing(uint32_t nowMs) {
    bool changed = false;
    // RFC-027(a): a knock's window elapsed unanswered.
    _pairing.pending().expire(nowMs, [&](const PendingKnock& k) {
        changed = true;
        emitPairingEvent(pairing_events::expired, std::span<const std::byte>(k.instance_id), k.name.view(),
                         k.mode, AccessLevel::watch, {}, nowMs);
    });
    // The presence window auto-expires inside PairingManager on a time
    // comparison, so noticing the transition HERE is what produces its closing
    // EDGE — a watch session that saw `window_opened` always sees the matching
    // close, even when nobody ever knocked.
    const bool openNow = _pairing.presenceWindowOpen(nowMs);
    if (_presenceWasOpen && !openNow) {
        _presenceWasOpen = false;
        emitPairingEvent(pairing_events::window_closed, {}, {}, pairing_modes::push_to_pair,
                         AccessLevel::watch, {}, nowMs);
        changed = true;
    } else if (!_presenceWasOpen && openNow) {
        _presenceWasOpen = true;
    }
    if (changed) publishPendingPairingState(nowMs);
}

inline Hub::Slot* Hub::findSlotBySession(uint32_t session_id) {
    if (session_id == 0) return nullptr;
    for (auto& s : _slots) {
        if (s.session.occupied() && s.session.session_id == session_id) return &s;
    }
    return nullptr;
}

inline bool Hub::issuePairGrant(Slot& slot, std::span<const std::byte> instance_id, AccessLevel role, uint8_t mode,
                                uint32_t nowMs) {
    std::array<std::byte, limits::token_bytes> tokenBuf{};
    PairingManager::PairedEntry* e =
        _pairing.grant(instance_id, role, mode, _rng, std::span<std::byte>(tokenBuf));
    if (e == nullptr) return false;
    // Carry over what the joiner's session already told us, so the ledger entry
    // names a DEVICE rather than a hex blob the moment it is created — an
    // operator approving "instance 3f9a..." is being asked to authorize
    // nothing they can recognize.
    e->kind.assign(slot.session.clientKind.view());
    e->name.assign(slot.session.clientName.view());
    if (slot.session.hasClientVer) e->version.assign(slot.session.clientVer.view());
    e->presentationMode = slot.session.presentationMode;

    PairGrantMsg g{};
    g.token = tokenBuf;
    g.roles = uint8_t(role);
    // M4c seam: if this hub ever HAS a durable identity, PAIR_GRANT is where it
    // is delivered — at the ceremony, the moment presence was proven, which is
    // what makes it trust-on-first-use at a VERIFIED moment rather than at an
    // arbitrary connection. The null ICrypto returns 0 and the key is simply
    // absent, which is the potato path and costs nothing.
    std::array<std::byte, kTrustPubkeyMaxBytes> pk{};
    const size_t pkLen = _crypto.publicKey(std::span<std::byte>(pk));
    if (pkLen > 0 && pkLen <= pk.size()) {
        g.has_trust = true;
        g.trust_map.has_hub_pubkey = true;
        g.trust_map.hub_pubkey_len = uint8_t(pkLen);
        g.trust_map.hub_pubkey = pk;
    }
    std::array<std::byte, 96> buf{};
    const size_t n = encodePairGrant(g, std::span<std::byte>(buf));
    if (n == 0) return false;
    sendFrameTo(*slot.transport, FrameType::PAIR_GRANT, 0, std::span<const std::byte>(buf.data(), n));

    // THE SESSION IS UPGRADED IN PLACE. Without this a joiner would have to
    // reconnect before using the role it was just granted — an extra ceremony
    // step on exactly the device class (one button, no display) this mode
    // exists to serve. Nothing is invented: the role is the one the ledger now
    // holds, and the next HELLO resolves to the same value.
    slot.session.role = role;

    _pairing.pending().remove(instance_id);
    emitPairingEvent(pairing_events::granted, instance_id, slot.session.clientName.view(), mode, role,
                     slot.session.clientVer.view(), nowMs);
    publishPendingPairingState(nowMs);
    publishPairedRosterState();
    return true;
}

// RFC-027 modes (a) and (c): a PAIR_REQ that carries NO proof.
inline void Hub::handleKnock(Slot& slot, const PairReqMsg& m, uint32_t nowMs) {
    std::span<const std::byte> inst(m.instance_id);

    // §6.1 sanity: a session may only knock for ITS OWN instance id. Without
    // this, any connected client could queue a knock in another device's name
    // and — since approval is BY instance_id — trick an operator into
    // authorizing a stranger under a familiar label. One comparison, whole
    // class of confusion gone.
    if (!std::equal(slot.session.instance_id.begin(), slot.session.instance_id.end(), m.instance_id.begin())) {
        NackMsg n;
        n.code = NackCode::PAIRING_DENIED;
        sendNackTracked(slot, n, nowMs);
        return;
    }

    // (c) PUSH-TO-PAIR first: a presence window is a stronger claim than a
    // queued approval, and it is SINGLE-GRANT — consuming it here is what makes
    // "the first knock in the window" literally true.
    if (_pairing.presenceWindowOpen(nowMs)) {
        const AccessLevel role = _pairing.presenceGrantRole();
        _pairing.closePresenceWindow();
        _presenceWasOpen = false;
        if (!issuePairGrant(slot, inst, role, pairing_modes::push_to_pair, nowMs)) {
            NackMsg n;
            n.code = NackCode::PAIRING_DENIED;  // ledger full: honest refusal
            sendNackTracked(slot, n, nowMs);
            return;
        }
        emitPairingEvent(pairing_events::window_closed, {}, {}, pairing_modes::push_to_pair,
                         AccessLevel::watch, {}, nowMs);
        publishPendingPairingState(nowMs);
        return;
    }

    // (a) KNOCK-AND-APPROVE.
    if (!_pairing.knockApproveEnabled()) {
        NackMsg n;
        n.code = NackCode::PAIRING_REQUIRED;
        sendNackTracked(slot, n, nowMs);
        return;
    }
    PendingKnock* k = _pairing.pending().add(inst, slot.session.session_id,
                                             nowMs + limits::pairing_window_default_s * 1000u,
                                             pairing_modes::knock_approve);
    if (k == nullptr) {
        // The bound did its job: pairing_pending_max strangers are already
        // waiting. BUSY, not DENIED — nothing was refused on the merits and
        // retrying later is the correct client behavior. `retry_after_ms` is
        // the knock window, because that is genuinely when a slot next frees.
        NackMsg n;
        n.code = NackCode::BUSY;
        n.has_retry_after_ms = true;
        n.retry_after_ms = limits::pairing_window_default_s * 1000u;
        sendNackTracked(slot, n, nowMs);
        return;
    }
    k->kind.assign(slot.session.clientKind.view());
    k->name.assign(slot.session.clientName.view());
    if (slot.session.hasClientVer) k->version.assign(slot.session.clientVer.view());

    // NO frame answers the knocker, and that is the design rather than an
    // omission: the answer is a PAIR_GRANT when an operator approves, a NACK if
    // denied, and silence in between — the joiner has one button and nothing to
    // render an interim status on. Everything an OPERATOR needs is published
    // instead, on 0x000A and 0x000B.
    emitPairingEvent(pairing_events::knocked, inst, k->name.view(), k->mode, AccessLevel::watch,
                     k->version.view(), nowMs);
    publishPendingPairingState(nowMs);
}

// ---- M4b: RFC-018/027/029 admin verbs on session-admin (0x0009) -------------
//
// Reached ONLY from handleIntent, AFTER the generic §9.3 pipeline has already
// done rate limiting, catalog lookup, class check, readiness, per-op access
// gating (the channel floor is `configure`) and the cfg_gen CAS. That ordering
// is the whole reason these are an ordinary INTENT channel: pairing
// administration inherits every protection the rest of the write plane has,
// instead of growing its own half of each.
//
// THE TRUSTED SURFACE IS A TIER, NOT AN APP. There is no check anywhere below
// asking WHO a session is — only what tier it holds.
inline bool Hub::handleAdminIntent(Slot& slot, const IntentMsg& m, uint32_t nowMs) {
    auto nack = [&](NackCode code) {
        NackMsg n;
        n.code = code;
        n.has_channel_id = true;
        n.channel_id = m.channel_id;
        n.has_intent_id = true;
        n.intent_id = m.intent_id;
        sendNackTracked(slot, n, nowMs);
    };

    uint64_t op = 0;
    bool hasOp = false;
    uint32_t targetSession = 0;
    std::span<const std::byte> targetInstance{};
    uint64_t wantRole = uint64_t(AccessLevel::control);
    bool hasRole = false;
    for (uint32_t i = 0; i < m.value_count; ++i) {
        const IntentValueField& f = m.value[i];
        switch (f.key) {
            case admin_value::op:
                if (f.value.kind == IntentValue::Kind::U64) { op = f.value.u64_val; hasOp = true; }
                break;
            case admin_value::session_id:
                if (f.value.kind == IntentValue::Kind::U64) targetSession = uint32_t(f.value.u64_val);
                break;
            case admin_value::instance_id:
                if (f.value.kind == IntentValue::Kind::Bstr) targetInstance = f.value.bstr_val;
                break;
            case admin_value::role:
                if (f.value.kind == IntentValue::Kind::U64) { wantRole = f.value.u64_val; hasRole = true; }
                break;
            default: break;  // §4.3
        }
    }
    if (!hasOp) { nack(NackCode::INVALID_VALUE); return true; }

    auto echoOk = [&]() {
        EchoMsg echo;
        echo.intent_id = m.intent_id;
        echo.cfg_gen = _cfgGen;
        echo.applied_count = 1;
        echo.applied[0] = {admin_value::op, IntentValue::ofU64(op)};
        std::array<std::byte, 64> ebuf{};
        const size_t elen = encodeEcho(echo, std::span<std::byte>(ebuf));
        if (elen == 0) return;
        slot.session.intentRing.store(m.intent_id, std::span<const std::byte>(ebuf.data(), elen));
        sendFrameToTracked(slot, FrameType::ECHO, m.channel_id, std::span<const std::byte>(ebuf.data(), elen),
                           nowMs);
    };

    switch (op) {
        case session_admin_ops::evict: {
            Slot* victim = findSlotBySession(targetSession);
            if (victim == nullptr) { nack(NackCode::INVALID_VALUE); return true; }
            // ECHO BEFORE the eviction, because evicting YOURSELF is legal and
            // teardown resets the slot (intent ring included). A self-evict
            // that silently swallowed its own confirmation is the kind of thing
            // found only by an operator wondering why the button did nothing.
            echoOk();
            GoodbyeMsg gb;
            gb.code = NackCode::SESSION_EVICTED;
            std::array<std::byte, 64> gbuf{};
            const size_t glen = encodeGoodbye(gb, std::span<std::byte>(gbuf));
            if (glen > 0 && victim->transport != nullptr) {
                sendFrameTo(*victim->transport, FrameType::GOODBYE, 0,
                            std::span<const std::byte>(gbuf.data(), glen));
            }
            // The full §6.8/RFC-005 teardown: an admin kick releases source
            // ownership under the source's own §11.3 loss policy exactly as a
            // crash would. "No unmonitored path to motion" gets no exception
            // for actions an operator meant to take.
            teardownSession(*victim, nowMs);
            return true;
        }
        case session_admin_ops::pair_approve: {
            if (targetInstance.size() != limits::instance_id_bytes) { nack(NackCode::INVALID_VALUE); return true; }
            if (wantRole > uint64_t(AccessLevel::configure)) { nack(NackCode::INVALID_VALUE); return true; }
            const AccessLevel role = hasRole ? AccessLevel(uint8_t(wantRole)) : AccessLevel::control;
            // CEILING: up to the approver's OWN tier, configure included
            // (operator ruling). Conventional admin behavior; the roster is
            // the audit trail rather than a hard ceiling that would leave the
            // first administrator unable to appoint a second. Asking for MORE
            // than you hold is REFUSED rather than silently clamped, because a
            // silent clamp hands an operator a device they believe has powers
            // it does not — a ground-truth violation wearing a convenience hat.
            if (uint8_t(role) > uint8_t(slot.session.role)) { nack(NackCode::ACCESS_DENIED); return true; }

            PendingKnock* k = _pairing.pending().find(targetInstance);
            if (k == nullptr) { nack(NackCode::INVALID_VALUE); return true; }
            Slot* joiner = findSlotBySession(k->session_id);
            if (joiner == nullptr || joiner->transport == nullptr) {
                // The knocker left. Its pending entry is stale; drop it rather
                // than mint a token nobody can be handed. See the design note
                // in session/pairing.hpp on why knocks are session-bound.
                const uint8_t mode = k->mode;
                std::array<std::byte, limits::instance_id_bytes> inst{};
                std::memcpy(inst.data(), k->instance_id.data(), inst.size());
                _pairing.pending().remove(targetInstance);
                emitPairingEvent(pairing_events::expired, std::span<const std::byte>(inst), {}, mode,
                                 AccessLevel::watch, {}, nowMs);
                publishPendingPairingState(nowMs);
                nack(NackCode::INVALID_VALUE);
                return true;
            }
            const uint8_t mode = k->mode;
            if (!issuePairGrant(*joiner, targetInstance, role, mode, nowMs)) {
                nack(NackCode::BUSY);  // ledger full
                return true;
            }
            echoOk();
            return true;
        }
        case session_admin_ops::pair_deny: {
            if (targetInstance.size() != limits::instance_id_bytes) { nack(NackCode::INVALID_VALUE); return true; }
            PendingKnock* k = _pairing.pending().find(targetInstance);
            if (k == nullptr) { nack(NackCode::INVALID_VALUE); return true; }
            const uint8_t mode = k->mode;
            std::array<std::byte, limits::instance_id_bytes> inst{};
            std::memcpy(inst.data(), k->instance_id.data(), inst.size());
            const bool wasReapproval = k->reapproval;
            _pairing.pending().remove(targetInstance);
            // Denying a RE-approval is a REVOKE in effect. Leaving a suspended
            // entry in the ledger after an operator said "no" would be a lie
            // the roster keeps telling forever.
            if (wasReapproval) {
                _pairing.revoke(std::span<const std::byte>(inst));
                emitPairingEvent(pairing_events::revoked, std::span<const std::byte>(inst), {}, mode,
                                 AccessLevel::watch, {}, nowMs);
                publishPairedRosterState();
            }
            emitPairingEvent(pairing_events::denied, std::span<const std::byte>(inst), {}, mode,
                             AccessLevel::watch, {}, nowMs);
            publishPendingPairingState(nowMs);
            echoOk();
            return true;
        }
        case session_admin_ops::revoke: {
            if (targetInstance.size() != limits::instance_id_bytes) { nack(NackCode::INVALID_VALUE); return true; }
            if (!_pairing.revoke(targetInstance)) { nack(NackCode::INVALID_VALUE); return true; }
            // AND DROP ANY PENDING ENTRY FOR IT. Found by the live pairing probe
            // against the sim: a device revoked while sitting in
            // RECOGNIZED-PENDING left its re-approval in the queue, so a later
            // `pair_approve` would have RESURRECTED a revoked device — minting a
            // fresh token with no new ceremony, from a queue entry an operator
            // had already answered by revoking. Revocation has to clear the
            // question as well as the answer.
            _pairing.pending().remove(targetInstance);
            // TAKES EFFECT AT THE NEXT HELLO, stated plainly: a session already
            // live keeps the role it was ADMITTED with, because roles are
            // resolved once at admission and re-resolving them per frame is a
            // cost the whole design avoids. An operator who wants the device
            // gone NOW follows with `evict` — two verbs, both present, neither
            // pretending to be the other.
            emitPairingEvent(pairing_events::revoked, targetInstance, {}, 0, AccessLevel::watch, {}, nowMs);
            publishPairedRosterState();
            publishPendingPairingState(nowMs);
            echoOk();
            return true;
        }
        default:
            nack(NackCode::UNSUPPORTED_OP);
            return true;
    }
}

// ---- M5: network probe (§6.4) -----------------------------------------------
// hub side: answer PROBE with a timed burst, receive PROBE_REPORT and surface its counters.

inline void Hub::handleProbeRequest(Slot& slot, uint32_t nowMs) {
    (void)nowMs;
    // M5 minimal: fire the whole burst in this call rather than spreading it
    // across probe_max_duration_ms of real time — the in-process link's own
    // FrameQueue capacity (16) already gives a congested/slow link something
    // real to push back against (write() returning false), which is the
    // property the burst exists to probe in the first place.
    uint16_t mtuPayload = uint16_t(slot.transport->properties().mtu > kHeaderBytes
                                        ? slot.transport->properties().mtu - kHeaderBytes
                                        : limits::min_transport_payload);
    if (mtuPayload == 0) return;
    uint32_t frameCount = (limits::probe_default_bytes + mtuPayload - 1) / mtuPayload;

    std::array<std::byte, limits::min_transport_payload> pbuf{};
    std::span<std::byte> pspan(pbuf.data(), std::min<size_t>(mtuPayload, pbuf.size()));
    for (uint32_t i = 0; i < frameCount; ++i) {
        size_t n = encodeProbeFrame(uint16_t(i), pspan);
        if (n == 0) break;
        sendFrameTo(*slot.transport, FrameType::PROBE, 0, std::span<const std::byte>(pspan.data(), n));
    }
}

inline void Hub::handleProbeReportFrame(Slot& slot, std::span<const std::byte> payload) {
    auto res = decodeProbeReport(payload);
    if (!res) return;
    slot.lastProbeReport = res.value().probe_result;
    slot.hasProbeReport = true;
    // §6.4 step 3 ("hub MAY raise grants accordingly") is a policy decision
    // left to a future milestone; M5 only surfaces the counters via
    // probeReportFor() (see hub.hpp's doc comment on that method).
}

inline std::optional<ProbeResult> Hub::probeReportFor(size_t slotIdx) const {
    if (slotIdx >= _slots.size() || !_slots[slotIdx].hasProbeReport) return std::nullopt;
    return _slots[slotIdx].lastProbeReport;
}

// ---- M5: congestion input (§10.3) + slow-consumer eviction (§10.4 step 4) ---

inline void Hub::setCongestionLevel(size_t slotIdx, uint8_t level) {
    if (slotIdx >= _slots.size()) return;
    _slots[slotIdx].congestionLevel = level;
    if (level < 2) _slots[slotIdx].criticalStalling = false;
}

inline void Hub::setCongestionLevel(ITransport& t, uint8_t level) {
    Slot* slot = attachedSlotFor(t);
    if (!slot) return;
    setCongestionLevel(size_t(slot - _slots.data()), level);
}

inline void Hub::trackCriticalSend(Slot& slot, bool sendOk, uint32_t nowMs) {
    if (slot.congestionLevel < 2) {
        slot.criticalStalling = false;
        return;
    }
    if (sendOk) {
        slot.criticalStalling = false;
        return;
    }
    if (!slot.criticalStalling) {
        slot.criticalStalling = true;
        slot.criticalStallSinceMs = nowMs;
        return;
    }
    if (timeReached(nowMs, slot.criticalStallSinceMs + limits::never_shed_stall_eviction_ms)) {
        evictSlot(slot, NackCode::SESSION_EVICTED, nowMs);
    }
}

inline void Hub::evictSlot(Slot& slot, NackCode code, uint32_t nowMs) {
    if (slot.session.occupied()) {
        GoodbyeMsg gb;
        gb.code = code;
        std::array<std::byte, 64> buf{};
        size_t n = encodeGoodbye(gb, std::span<std::byte>(buf));
        if (n > 0 && slot.transport != nullptr) {
            // Best-effort, always attempted regardless of the very
            // congestion that caused this eviction (§10.4: a link that can't
            // carry a few dozen bytes either way is simply gone).
            sendFrameTo(*slot.transport, FrameType::GOODBYE, 0, std::span<const std::byte>(buf.data(), n));
        }
    }
    // §6.8/§11.4: eviction is a session end — release its source ownership
    // (GOODBYE frame already sent above) via the shared teardown path.
    teardownSession(slot, nowMs);
    slot.congestionLevel = 0;
    slot.criticalStalling = false;
}

// Both *Tracked helpers refuse a detached slot BEFORE dereferencing, and
// without tracking the miss (TRAPS T13): a parked session has no link to be
// congested on, so arming the critical-stall timer would evict it for a
// failure that never happened. The `update()` walk guards its own slot; these
// two are also reachable from the fan-out senders, where the slot being
// written to is NOT the slot being pumped.
inline bool Hub::sendFrameToTracked(Slot& slot, FrameType type, uint16_t channel, std::span<const std::byte> payload,
                                     uint32_t nowMs, uint16_t seq) {
    if (slot.transport == nullptr) return false;
    bool ok = sendFrameTo(*slot.transport, type, channel, payload, seq);
    trackCriticalSend(slot, ok, nowMs);
    return ok;
}

inline void Hub::sendNackTracked(Slot& slot, const NackMsg& n, uint32_t nowMs) {
    if (slot.transport == nullptr) return;
    NackMsg stamped = n;  // RFC-001, same rule as sendNack()
    if (!stamped.has_intent_seq && _dispatchSeqValid) {
        stamped.has_intent_seq = true;
        stamped.intent_seq = _dispatchSeq;
    }
    std::array<std::byte, 128> buf{};
    size_t len = encodeNack(stamped, std::span<std::byte>(buf));
    bool ok = len > 0 && sendFrameTo(*slot.transport, FrameType::NACK, 0, std::span<const std::byte>(buf.data(), len));
    trackCriticalSend(slot, ok, nowMs);
}

// ---- M5: source ownership plumbing ------------------------------------------
// Shared by the intent pipeline and deadman (§11.4 control-owner STATE,
// session-events takeover EVENT).

inline std::array<std::byte, 20> Hub::buildControlOwnerPayload() const {
    std::array<std::byte, 20> buf{};
    std::span<std::byte> s(buf);
    for (uint8_t i = 0; i < SourceOwnershipTable::kMaxSources; ++i) {
        size_t off = size_t(i) * 5;
        putU8(s.subspan(off, 1), i);
        putU32(s.subspan(off + 1, 4), _ownership.ownerOf(i));
    }
    return buf;
}

inline void Hub::publishControlOwnerStateIfPresent() {
    if (!_catalog.find(channels::control_owner)) return;  // §11.4: catalog-optional in this M5 pass
    auto payload = buildControlOwnerPayload();
    _retained.publish(channels::control_owner, std::span<const std::byte>(payload));
}

inline bool Hub::anySubscribed(uint16_t channel_id) const {
    for (const auto& slot : _slots) {
        if (slot.session.occupied() && slot.session.subs.find(channel_id)) return true;
    }
    return false;
}

inline void Hub::emitTakeoverEvent(uint8_t source_id, uint32_t newOwnerSession, uint32_t nowMs) {
    // §9.4 best-effort: skip the encode entirely when nobody is subscribed
    // (session-events, 0x0007, isn't even in every catalog — see the M5
    // report for where this is and isn't exercised).
    if (!anySubscribed(channels::session_events)) return;

    EventMsg ev{};
    ev.channel_id = channels::session_events;
    ev.timestamp = nowMs;
    // Registry session_event_kinds (allocated when this gap was flagged).
    ev.event_kind = session_events::takeover;
    // Kind-specific fields ride the SCOPED `body` (40) sub-map; keys 1/2 are
    // the session-events channel's OWN catalog schema keys, not global ones.
    ev.has_body = true;
    ev.body_count = 2;
    ev.body[0] = IntentValueField{1, IntentValue::ofU64(source_id)};
    ev.body[1] = IntentValueField{2, IntentValue::ofU64(newOwnerSession)};

    std::array<std::byte, 64> buf{};
    size_t n = encodeEvent(ev, std::span<std::byte>(buf));
    if (n > 0) publishEvent(channels::session_events, std::span<const std::byte>(buf.data(), n));
}

// ---- RFC-042: session staleness ---------------------------------------------
// the shared "mark stale" path + the observability edges (session_stale/session_resumed, session_event_kinds 4/5).

// Best-effort EVENT on the spec-core session-events channel (0x0007), same
// shape as emitTakeoverEvent's single-id kinds: body key 1 = the affected
// session_id. Skips the encode entirely when nobody is subscribed (§9.4).
inline void Hub::emitSessionEvent(uint8_t kind, uint32_t session_id, uint32_t nowMs) {
    if (!anySubscribed(channels::session_events)) return;

    EventMsg ev{};
    ev.channel_id = channels::session_events;
    ev.timestamp = nowMs;
    ev.event_kind = kind;
    ev.has_body = true;
    ev.body_count = 1;
    ev.body[0] = IntentValueField{1, IntentValue::ofU64(session_id)};

    std::array<std::byte, 32> buf{};
    size_t n = encodeEvent(ev, std::span<std::byte>(buf));
    if (n > 0) publishEvent(channels::session_events, std::span<const std::byte>(buf.data(), n));
}

// The RFC-042 staleness transition, shared by pumpDeadman() and pumpIdleReap():
// releases every source the session owns (unconditionally, latching nothing —
// RFC-045) and marks the session STALE instead of tearing it down. Slot,
// session_id, subs, publishGrants, intent ring, and readiness are all left
// exactly as they were (session.hpp's RFC-042 doc comment enumerates the kept
// fields) — only `state` and `staleSinceMs` change. NO GOODBYE: staleness is
// not termination, and the client may never even notice.
inline void Hub::markStale(Slot& slot, uint32_t nowMs, uint8_t reason) {
    releaseSessionSources(slot.session.session_id, reason, nowMs);
    slot.session.state = HubSessionState::STALE;
    slot.session.staleSinceMs = nowMs;
    emitSessionEvent(session_events::session_stale, slot.session.session_id, nowMs);
}

// Path A resumption (§6.6 "any received frame is proof of life", RFC-042):
// called from pumpSlot() before dispatch, for every frame on an occupied
// slot. A STALE session that is still attached to its ORIGINAL transport —
// the dominant case, since backgrounding/locking a screen throttles JS timers
// without closing the socket — needs nothing more than any frame (a PING is
// enough) to flip straight back to LIVE: no HELLO, no re-SUBSCRIBE, no catalog
// fetch, because the grants never left. Path B (a fresh transport reattaching
// via HELLO) is handleReattach(), below.
inline void Hub::reviveIfStale(Slot& slot, uint32_t nowMs) {
    if (slot.session.state != HubSessionState::STALE) return;
    slot.session.state = HubSessionState::LIVE;
    slot.session.staleSinceMs = 0;
    emitSessionEvent(session_events::session_resumed, slot.session.session_id, nowMs);
}

// RFC-042 item 5: under slot pressure (a HELLO that would otherwise NACK BUSY)
// a STALE session yields its slot before a genuinely new identity is refused.
// Eligible: STALE only — a LIVE session is never evicted for pressure, full
// stop (only a duplicate-instance_id HELLO ever displaces one). Choice: lowest
// access tier first, tie-break longest continuously stale (earliest
// staleSinceMs loses first). Returns nullptr when nothing is eligible.
inline Hub::Slot* Hub::findEvictableStale(const Slot* exclude) {
    Slot* best = nullptr;
    for (auto& s : _slots) {
        if (&s == exclude) continue;
        if (s.session.state != HubSessionState::STALE) continue;
        if (best == nullptr) {
            best = &s;
            continue;
        }
        const bool lowerTier = uint8_t(s.session.role) < uint8_t(best->session.role);
        const bool sameTierStaler = uint8_t(s.session.role) == uint8_t(best->session.role) &&
                                    timeDelta(s.session.staleSinceMs, best->session.staleSinceMs) < 0;
        if (lowerTier || sameTierStaler) best = &s;
    }
    return best;
}

// ---- M5: deadman (§11.3) ----------------------------------------------------
// evaluated once per occupied session per update()

inline void Hub::pumpDeadman(Slot& slot, uint32_t nowMs) {
    // A session already STALE (or otherwise not LIVE — VALIDATING/GRANTED
    // never own a source) has nothing further for this pump to do; re-firing
    // on an already-stale session would be a harmless but pointless restate.
    if (slot.session.state != HubSessionState::LIVE) return;

    // Scope note (documented clarification of a spec/task tension — see the
    // M5 report): §11.3's deadman window binds to the ACTIVE SOURCE, not to
    // sessions in general ("Every session that owns an active source has a
    // deadman window"). A pure watch/control session owning nothing
    // keeps the separate, more lenient §6.5 idle-liveness policy (reaped
    // only after 3x its PING interval), which this M5 pass does not
    // implement as an active reaper — only source-owning sessions are
    // subject to the tighter deadman_ms window checked here.
    bool ownsAny = false;
    for (uint8_t src = 0; src < SourceOwnershipTable::kMaxSources; ++src) {
        if (_ownership.ownerOf(src) == slot.session.session_id) {
            ownsAny = true;
            break;
        }
    }
    if (!ownsAny) return;

    // RFC-038: per-session window (HELLO wish clamped at grant; default when
    // no wish). WELCOME key 24 echoed exactly this value.
    if (!timeReached(nowMs, slot.session.lastRxMs + slot.session.deadmanMs)) return;

    // RFC-042: silence past the deadman window means the SESSION is STALE, not
    // gone — the slot is RETAINED so the same client resumes without a full
    // HELLO/WELCOME/catalog cycle. RFC-045: this releases ownership but forces
    // no stop (see markStale()/releaseSessionSources()'s own comments); the
    // reference hub no longer emits DEADMAN_TIMEOUT for silence at all (the
    // code stays registered for a hub/policy combination that still wants to
    // terminate outright).
    markStale(slot, nowMs, /*reason=*/3 /*deadman-release*/);
}

// ---- RFC-024/RFC-042: idle reaping for sessions that own NO source ----------
//
// THE COMBINED LIVENESS MODEL (three regimes, one per failure it protects
// against; they are checked in this order and the first to fire wins):
//
//   1. SOURCE-OWNING sessions -> §11.3 DEADMAN, deadman_default_ms (600).
//      RFC-042: marked STALE, ownership released (RFC-045: nothing latched).
//
//   2. EVERY OTHER session -> §6.5 IDLE REAPING, this function:
//      idle_reap_multiplier (3) x ping_interval_idle_ms (1000) = 3000 ms of
//      total silence. RFC-042: also marked STALE, not torn down — a dark
//      viewer's slot is retained under the SAME staleness model as a
//      source-owner's, and only yields under RFC-042 item 5's slot-pressure
//      eviction, never merely for having gone quiet.
//
//   3. Sessions that are ALIVE but never adopted the catalog ->
//      RFC-015 READY TIMEOUT, catalog_ready_timeout_ms (15000) since GRANT.
//      Neither 1 nor 2 can ever fire on a client that PINGs happily (it IS
//      alive, and it owns nothing because both planes are gated), so without
//      this it would hold a slot forever. Measured from grant, not from last
//      rx, because that is the thing that is not progressing. UNAFFECTED by
//      RFC-042 on purpose: a session stuck mid-handshake has no partially-
//      adopted state worth preserving, so this remains a hard teardown.
//
// The three do not overlap: 1 and 2 are disjoint by definition (owns / does
// not own), and 3 keys off a different clock entirely. A session can only be
// marked stale by 1 or 2 once — markStale() changes `state` either way.

inline bool Hub::pumpIdleReap(Slot& slot, uint32_t nowMs) {
    if (slot.session.state != HubSessionState::LIVE) return false;

    // Source owners belong to regime 1 and are already handled, on a tighter
    // window.
    for (uint8_t src = 0; src < SourceOwnershipTable::kMaxSources; ++src) {
        if (_ownership.ownerOf(src) == slot.session.session_id) return false;
    }

    constexpr uint32_t kIdleReapMs = limits::idle_reap_multiplier * limits::ping_interval_idle_ms;
    if (!timeReached(nowMs, slot.session.lastRxMs + kIdleReapMs)) return false;

    // RFC-042: STALE, not torn down (see pumpDeadman's twin comment). No
    // GOODBYE — staleness is not an ending.
    markStale(slot, nowMs, /*reason=*/4 /*session-loss-release — owns nothing, forwarded for uniformity*/);
    return true;
}

// ---- RFC-015: catalog-readiness timeout -------------------------------------
// the one liveness hole READY opens

inline bool Hub::pumpReadyTimeout(Slot& slot, uint32_t nowMs) {
    if (slot.session.state == HubSessionState::STALE) return false;  // RFC-042: not this pump's business
    if (slot.session.ready) return false;
    if (!timeReached(nowMs, slot.session.grantedAtMs + limits::catalog_ready_timeout_ms)) return false;

    // A client that PINGs happily but never finishes adopting the catalog is
    // ALIVE, so neither the §11.3 deadman (it owns no source — both planes are
    // gated) nor §6.5 idle reaping would ever fire on it: it would hold a
    // session slot forever, useless to itself and denied to someone else.
    // GOODBYE with the registry's own code for exactly this (READY_TIMEOUT),
    // best-effort FIRST — teardownSession resets the slot right after.
    GoodbyeMsg gb;
    gb.code = NackCode::READY_TIMEOUT;
    std::array<std::byte, 64> buf{};
    size_t n = encodeGoodbye(gb, std::span<std::byte>(buf));
    if (n > 0 && slot.transport != nullptr) {
        sendFrameTo(*slot.transport, FrameType::GOODBYE, 0, std::span<const std::byte>(buf.data(), n));
    }
    teardownSession(slot, nowMs);
    return true;
}

// ---- Shared session teardown (§6.8, §11.3, §11.4) ---------------------------
// the single choke point every slot-ending path funnels through, so source ownership is released the SAME way no matter how the session departed.

inline void Hub::releaseSessionSources(uint32_t sessionId, uint8_t reason, uint32_t nowMs) {
    (void)nowMs;  // RFC-045: nothing here broadcasts a safety edge any more
    // RFC-045: source-loss is liveness bookkeeping, not a safety event. This
    // used to run a per-source Stop-vs-Continue POLICY dispatch here — latching
    // STOP + delegate.onDeadmanStop() for every "initiator-bound" source,
    // regardless of which of the six teardown doors (or, since RFC-042, a
    // plain staleness transition) triggered it. REMOVED: a command-driven
    // source has nothing left to execute once its owner is gone and settles on
    // its own by construction (§9.6's closed motion surface — every mode is a
    // continuously-fed stream or an individually time-bounded segment), so the
    // forced latch only ever converted a graceful settle into a spurious,
    // operator-visible STOP edge. The one case that is genuinely different — a
    // hub-autonomous generator whose owning session went away — is now the
    // FIRMWARE DELEGATE's call via the registered `source.background_run`
    // field role, decided entirely inside its OWN onSourceOwnership()
    // implementation; the library stays device-agnostic and only ever reports
    // the release, exactly as it always has for every source class.
    //
    // `SourceLossPolicy`/`HubDelegate::sourcePolicy()`/`onDeadmanStop()` remain
    // declared (frozen delegate interface) but are no longer called from here.
    bool releasedAny = false;
    _ownership.releaseAllOf(sessionId, [&](uint8_t source) {
        releasedAny = true;
        _delegate.onSourceOwnership(source, 0, reason);
    });
    if (releasedAny) publishControlOwnerStateIfPresent();
}

inline void Hub::teardownSession(Slot& slot, uint32_t nowMs, uint8_t reason) {
    if (slot.session.occupied()) {
        releaseSessionSources(slot.session.session_id, reason, nowMs);
        // M4b: a pending KNOCK belongs to the session that made it, and dies
        // with it. This is the same lesson as the source-ownership field bug,
        // applied before it could happen twice: session-scoped state that
        // outlives its session is state nobody can ever clear. Concretely, a
        // leaked knock would keep one of only four pending slots forever and
        // would let an operator "approve" a device that left minutes ago.
        bool droppedAny = false;
        _pairing.pending().dropBySession(slot.session.session_id, [&](const PendingKnock& k) {
            droppedAny = true;
            emitPairingEvent(pairing_events::expired, std::span<const std::byte>(k.instance_id), k.name.view(),
                             k.mode, AccessLevel::watch, {}, nowMs);
        });
        if (droppedAny) publishPendingPairingState(nowMs);
        _delegate.onSessionLeft(slot.session.session_id);
    }
    slot.session.reset();
    slot.pushRecords.fill(PushRecord{});
    slot.conflictNacks.fill(typename Slot::ConflictNack{});  // RFC-012 throttle is per-session
    // M4c: the signing job and the AUTH strike count are session-scoped too, so
    // they die on the SAME path — this is the third-field-bug lesson applied
    // pre-emptively. A leaked signPending would make a departed session's
    // signature job immortal work for the application's signer, and a leaked
    // authFailures would hand the next occupant of this slot somebody else's
    // strikes.
    slot.clientNonce.fill(std::byte{0});
    slot.hasClientNonce = false;
    slot.sigRequested = false;
    slot.signPending = false;
    slot.signDelivered = false;
    slot.authFailures = 0;
    // §8.4: and the blob cursor, for the SAME reason and by the same rule. A
    // resumable transfer is session-scoped state that outlives a single dispatch
    // — precisely the shape that produced the source-ownership field bug — so
    // it dies on the one path every session death funnels through. Left behind,
    // it would resume against whoever occupies this slot next, spraying a
    // departed client's ledger item at a stranger.
    slot.blob = typename Slot::PendingBlob{};
}

// ---- Accessors --------------------------------------------------------------

inline uint16_t Hub::cfgGen() const { return _cfgGen; }
inline uint32_t Hub::bootId() const { return _bootId; }

// RFC-011: the hub-side half of "cfg_gen advances iff an applied config value
// actually changed, REGARDLESS OF WHO CHANGED IT". Without this a machine-side
// change (physical control, boot adoption, internal recalculation) left the
// generation stale and a client's `precondition` CAS passed against config that
// had already moved. See hub.hpp for the full contract.
inline void Hub::bumpConfigGeneration() { ++_cfgGen; }

inline size_t Hub::sessionCount() const { return occupiedCount(nullptr); }

inline const HubSession* Hub::sessionBySlot(size_t i) const {
    if (i >= _slots.size()) return nullptr;
    return &_slots[i].session;
}

inline Hub::StreamIngressCounters Hub::streamIngressCounters(size_t slotIdx) const {
    if (slotIdx >= _slots.size() || !_slots[slotIdx].session.occupied()) return StreamIngressCounters{};
    return StreamIngressCounters{_slots[slotIdx].session.streamBundlesAccepted,
                                 _slots[slotIdx].session.streamBundlesDropped};
}

// ---- M4 test-only unsolicited GRANT hook (§10.2; real policy is M5) ---------

inline bool Hub::regrantForTest(size_t slotIdx, uint16_t channel_id, float new_rate) {
    if (slotIdx >= _slots.size()) return false;
    Slot& slot = _slots[slotIdx];
    if (!slot.session.occupied()) return false;
    SubscriptionEntry* existing = slot.session.subs.find(channel_id);
    if (!existing) return false;  // regrant only: must already hold a grant

    if (!slot.session.subs.upsert(channel_id, new_rate, existing->priority)) return false;

    GrantMsg batch{};
    Grant g;
    g.channel_id = channel_id;
    g.granted_rate_hz = new_rate;
    g.priority = uint8_t(existing->priority);
    batch.grants[0] = g;
    batch.grants_count = 1;

    std::array<std::byte, 64> gbuf{};
    size_t glen = encodeGrant(batch, std::span<std::byte>(gbuf));
    if (glen == 0) return false;
    return sendFrameTo(*slot.transport, FrameType::GRANT, 0, std::span<const std::byte>(gbuf.data(), glen));
}

}  // namespace slopsync
