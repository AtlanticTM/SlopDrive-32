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
#include "slopsync/wire/catalog_chunks.hpp"
#include "slopsync/wire/catalog_codec.hpp"
#include "slopsync/wire/frame_buffer.hpp"
#include "slopsync/wire/frame_header.hpp"
#include "slopsync/wire/messages/catalog_req.hpp"
#include "slopsync/wire/messages/echo.hpp"
#include "slopsync/wire/messages/event.hpp"
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/pair.hpp"
#include "slopsync/wire/messages/probe_report.hpp"
#include "slopsync/wire/messages/subscribe.hpp"
#include "slopsync/wire/raw/clock_frame.hpp"
#include "slopsync/wire/raw/ping_pong.hpp"
#include "slopsync/wire/raw/probe.hpp"
#include "slopsync/wire/sha256.hpp"

namespace slopsync {

// NACK BUSY retry-after hint (registry limits::busy_retry_after_default_ms —
// promoted from a code-local M4 constant when the gap was flagged).
inline constexpr uint32_t kHubBusyRetryAfterMs = limits::busy_retry_after_default_ms;

// ============================================================================
// Construction / attach / detach
// ============================================================================

inline Hub::Hub(const Catalog32& catalog, IClock& clock, IRandom& rng, HubDelegate& delegate)
    : _catalog(catalog), _clock(clock), _rng(rng), _delegate(delegate) {
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
}

inline bool Hub::attachTransport(ITransport& t) {
    for (auto& slot : _slots) {
        if (slot.transport == nullptr) {
            if (!t.open()) return false;
            slot.transport = &t;
            return true;
        }
    }
    return false;
}

inline void Hub::detachTransport(ITransport& t) {
    for (auto& slot : _slots) {
        if (slot.transport == &t) {
            // Detaching physically removes the transport; whatever session (if
            // any) was riding it is no longer reachable. §6.8: a rude socket
            // death is handled identically to GOODBYE — tear the session down
            // so its source ownership is RELEASED (the bug this fixes left it
            // owned by a departed session_id forever). Teardown runs FIRST,
            // while slot.transport is still valid, so any §11.3 loss-policy
            // safety broadcast to the remaining sessions is unaffected and this
            // slot's own last frame simply drains into the closing link. No
            // nowMs is threaded to detach (the transport layer, not update(),
            // drives it), so read the injected clock — same as latchEstop().
            teardownSession(slot, _clock.nowMs());
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

// ============================================================================
// update() — the frame pump + STATE pacing walk
// ============================================================================

inline void Hub::update(uint32_t nowUs) {
    // NOT nowUs / 1000: the quotient of a wrapping counter is not itself a
    // mod-2^32 counter, and every ms deadline below relies on timeReached()'s
    // wrap window (see MonotonicMs in util/serial_arithmetic.hpp).
    uint32_t nowMs = _monoMs.advance(nowUs);
    for (auto& slot : _slots) {
        if (slot.transport == nullptr) continue;
        pumpSlot(slot, nowMs);
        if (slot.session.occupied()) {
            pumpDeadman(slot, nowMs);  // §11.3: may free this very slot — re-check occupied() below
        }
        if (slot.session.occupied()) {
            pumpStatePacing(slot, nowMs);
            // Liveness (§6.5, M4 minimal): reply is event-driven (PING->PONG,
            // handlePing); the hub does not itself originate PING.
        }
    }
}

// ============================================================================
// Frame pump for one slot: ESTOP magic BEFORE header decode (§5.5), then
// normal header dispatch.
// ============================================================================

inline void Hub::pumpSlot(Slot& slot, uint32_t nowMs) {
    while (auto fb = slot.transport->read()) {
        std::span<const std::byte> bytes = fb->bytes();

        if (bytes.size() == kEstopFrameBytes && bytes[0] == kEstopMagicByte && bytes[1] == kEstopMagicByte &&
            bytes[2] == kEstopMagicByte && bytes[3] == kEstopMagicByte) {
            auto decoded = decodeEstop(bytes);
            if (decoded) {
                if (slot.session.occupied()) slot.session.lastRxMs = nowMs;
                handleEstopFrame(decoded.value(), nowMs);
            }
            // BadCrc: silently drop (§5.5) — not a real ESTOP, never acted on.
            continue;
        }

        auto header = decodeFrameHeader(bytes);
        if (!header) continue;  // too short to be a frame at all: drop
        if (slot.session.occupied()) slot.session.lastRxMs = nowMs;  // §6.5: any rx is proof of life
        dispatchFrame(slot, *header, fb->payload(), nowMs);
    }
}

inline void Hub::dispatchFrame(Slot& slot, const FrameHeader& h, std::span<const std::byte> payload, uint32_t nowMs) {
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
        case FrameType::CATALOG_REQ:
            if (slot.session.occupied()) handleCatalogReq(slot, payload);
            break;
        case FrameType::PAIR_REQ:
            if (slot.session.occupied()) handlePairReq(slot, payload, nowMs);
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

// ============================================================================
// Small send helpers
// ============================================================================

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
    std::array<std::byte, 128> buf{};
    size_t len = encodeNack(n, std::span<std::byte>(buf));
    if (len == 0) return;
    sendFrameTo(t, FrameType::NACK, 0, std::span<const std::byte>(buf.data(), len));
}

// ============================================================================
// HELLO / WELCOME (§6.2, §6.3)
// ============================================================================

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

inline void Hub::handleHello(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs) {
    auto helloR = decodeHello(payload);
    if (!helloR) {
        NackMsg n;
        n.code = NackCode::MALFORMED;
        sendNack(*slot.transport, n);
        return;
    }
    const HelloMsg& h = helloR.value();

    // §6.3 duplicate identity: evict ANY live session (anywhere) bearing this
    // instance_id, honor the new HELLO in its place.
    std::span<const std::byte> instanceSpan(h.instance_id);
    if (Slot* dup = findSlotByInstance(instanceSpan, &slot)) {
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
        NackMsg n;
        n.code = NackCode::BUSY;
        n.has_retry_after_ms = true;
        n.retry_after_ms = kHubBusyRetryAfterMs;
        sendNack(*slot.transport, n);
        return;
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
    // viewer", never the reverse (the pairing store's grant is never
    // downgraded by falling through to the delegate).
    AccessLevel role = h.has_token
                            ? _pairing.validate(instanceSpan, std::span<const std::byte>(h.token))
                            : AccessLevel::viewer;
    if (role == AccessLevel::viewer) {
        role = _delegate.validateToken(instanceSpan, std::span<const std::byte>(h.token), h.has_token);
    }
    slot.session.role = role;
    slot.session.helloSeenCfgGen = _cfgGen;
    slot.session.clientEtagMatched =
        h.has_catalog_etag && std::equal(h.catalog_etag.begin(), h.catalog_etag.end(), _etag.begin());
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
        if (entry->cls == ChannelClass::INTENT) continue;  // c2h-only, never subscribable
        if (uint8_t(slot.session.role) < uint8_t(entry->access)) continue;

        float grantedRate = (entry->maxRateHz <= 0.0f) ? 0.0f : std::min(wish.rate_hz, entry->maxRateHz);
        if (grantedRate < 0.0f) grantedRate = 0.0f;

        if (!slot.session.subs.upsert(wish.channel_id, grantedRate, entry->defaultPriority)) continue;  // table full

        if (w.grants_count < kWelcomeMaxGrants) {
            Grant g;
            g.channel_id = wish.channel_id;
            g.granted_rate_hz = grantedRate;
            g.priority = uint8_t(entry->defaultPriority);
            w.grants[w.grants_count++] = g;
        }
    }

    // §6.2/§10.5 publishes grants: inbound-STREAM (c2h motion-input) wishes.
    // A wish is granted iff the channel exists, is class STREAM, is direction
    // c2h, is within the session's role, and clamps to a positive rate. Any
    // failure -> omit (no NACK, §6.2). Grants become truth on the session
    // (publishGrants) and echo in WELCOME under granted_publishes (key 36).
    for (uint32_t i = 0; i < h.publishes_count; ++i) {
        const PublishWish& wish = h.publishes[i];
        const CatalogEntry* entry = _catalog.find(wish.channel_id);
        if (!entry) continue;                                  // unknown -> absent
        if (entry->cls != ChannelClass::STREAM) continue;      // wrong class -> absent
        if (entry->dir != Direction::c2h) continue;            // wrong direction -> absent
        if (uint8_t(slot.session.role) < uint8_t(entry->access)) continue;  // access -> absent

        float grantedRate = (entry->maxRateHz <= 0.0f) ? 0.0f : std::min(wish.rate_hz, entry->maxRateHz);
        if (grantedRate <= 0.0f) continue;                     // not a rate-bearing publish -> absent

        if (!slot.session.addPublishGrant(wish.channel_id, grantedRate, nowMs)) continue;  // table full

        if (w.granted_publishes_count < kWelcomeMaxGrantedPublishes) {
            GrantedPublish gp;
            gp.channel_id = wish.channel_id;
            gp.granted_rate_hz = grantedRate;
            w.granted_publishes[w.granted_publishes_count++] = gp;
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
    w.limits_info.max_frame = uint32_t(kFrameBufferCapacity);
    w.limits_info.max_subscriptions = uint32_t(limits::max_subscriptions_per_session);
    w.limits_info.retained_pending = retainedPending;
    w.roles = uint8_t(slot.session.role);
    w.deadman_ms = limits::deadman_default_ms;
    w.deadman_policy = 0;  // M4: informational only, policy dispatch is M5
    _rng.fill(std::span<std::byte>(w.nonce));
    slot.nonce = w.nonce;  // §12.2: remembered so a later PAIR_REQ on this session can be verified

    std::array<std::byte, 700> wbuf{};
    size_t wlen = encodeWelcome(w, std::span<std::byte>(wbuf));
    if (wlen == 0) return;  // catalog-conformance/encode bug; nothing sane to do
    if (!sendFrameTo(*slot.transport, FrameType::WELCOME, 0, std::span<const std::byte>(wbuf.data(), wlen))) return;

    slot.session.state = HubSessionState::GRANTED;
    // Retained STATE pushes happen via the normal pacing walk later in this
    // same update() call (each fresh grant's everPushed==false forces
    // dueForPush() to fire immediately — see subscription.hpp's design
    // note) — no separate immediate-push code path is needed here.
    slot.session.state = HubSessionState::LIVE;  // hub-side bookkeeping only, §2.2
    _delegate.onSessionJoined(slot.session.session_id);
}

// ============================================================================
// SUBSCRIBE / UNSUBSCRIBE (§6.6)
// ============================================================================

inline void Hub::handleSubscribe(Slot& slot, std::span<const std::byte> payload, uint32_t nowMs) {
    (void)nowMs;
    auto res = decodeSubscribe(payload);
    if (!res) return;
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
        if (entry->cls == ChannelClass::INTENT) {
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

// ============================================================================
// INTENT / ECHO / NACK (§9.3, exact order)
// ============================================================================

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

    // 4) Access: controller-level channel + viewer role -> NOT_CONTROLLER;
    // any other shortfall (admin-level) -> ACCESS_DENIED.
    if (uint8_t(slot.session.role) < uint8_t(entry->access)) {
        NackMsg n;
        n.code = (entry->access == AccessLevel::controller) ? NackCode::NOT_CONTROLLER : NackCode::ACCESS_DENIED;
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

    // 5b) §11.2 ESTOP_CLEAR op on the safety-intents channel (0x0005),
    // hub-handled, not delegated. Op encoding is registry-governed now:
    // value field key 1 == safety_ops::estop_clear (safety_intent_ops
    // section, allocated when this gap was flagged). The other ops
    // (STOP/HOLD/PAUSE/RESUME) have registry codes but no hub handling in
    // this M5 pass — they fall through to the delegate like any other
    // INTENT channel.
    if (m.channel_id == channels::safety_intents) {
        bool isEstopClear = false;
        for (uint32_t i = 0; i < m.value_count; ++i) {
            if (m.value[i].key == 1 && m.value[i].value.kind == IntentValue::Kind::U64 &&
                m.value[i].value.u64_val == safety_ops::estop_clear) {
                isEstopClear = true;
                break;
            }
        }
        if (isEstopClear) {
            // Belt-and-braces role gate (§11.2: "requires controller+ role")
            // independent of whatever access level a given catalog declares
            // for this channel — catalog access already gates it too (step 4
            // above), this is a second guard specific to the safety op.
            if (uint8_t(slot.session.role) < uint8_t(AccessLevel::controller)) {
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

    // §11.1: a source-mapped intent succeeding clears a STOP latch (deadman)
    // regardless of which authorized session owns/sent it — "clears by any
    // new motion intent".
    if (mappedSource && (_safetyWord & safety_bits::STOP)) {
        _safetyWord &= ~safety_bits::STOP;
        _safetyOwnerSession = 0;
        publishSafetySnapshot();
        broadcastSafetyNow(nowMs);
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

// ============================================================================
// STREAM ingress (§9.2 c2h motion input, §10.5 rate, §11.3/§11.4 source)
// ============================================================================

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

inline void Hub::handleStream(Slot& slot, const FrameHeader& h, std::span<const std::byte> payload, uint32_t nowMs) {
    const uint16_t channel_id = h.channel;

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
    const size_t sampleSize = entry->layoutWireSize();

    // 3) Parse + re-validate §5.4 caps against the payload (n≤32, span≤20ms,
    // exact total size / truncation). BundleView::parse enforces those; a
    // violation drops the bundle WHOLE.
    auto parsed = BundleView::parse(payload, sampleSize);
    if (!parsed) {
        ++slot.session.streamBundlesDropped;
        return;
    }
    const BundleView& bundle = parsed.value();

    // 3b) n==0 is malformed (an empty bundle carries no samples — BundleWriter
    // never emits one). t_off monotonicity + t_off[0]==0 are NOT checked by
    // BundleView::parse (it only bounds the span), so re-validate them here:
    // recover t_off[i] = sampleTimeUs(i) − t_base (wrap-safe unsigned subtract,
    // exact because t_off < 2^16), require strictly increasing with first == 0.
    const uint8_t n = bundle.sampleCount();
    if (n == 0) {
        ++slot.session.streamBundlesDropped;
        return;
    }
    uint32_t prevOff = 0;
    for (uint8_t i = 0; i < n; ++i) {
        const uint32_t off = bundle.sampleTimeUs(i) - bundle.tBase();  // = t_off[i]
        if (i == 0) {
            if (off != 0) { ++slot.session.streamBundlesDropped; return; }
        } else if (off <= prevOff) {
            ++slot.session.streamBundlesDropped;
            return;  // not strictly increasing
        }
        prevOff = off;
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
            return;  // another session owns it; silent (no per-bundle NACK, §9.2)
        }
        if (acq == SourceOwnershipTable::AcquireResult::Acquired) {
            _delegate.onSourceOwnership(source, slot.session.session_id, /*reason=*/0);
            publishControlOwnerStateIfPresent();
        }
        // AlreadyOwner: idempotent refresh, nothing to notify.
    }

    // 6) Deliver. Post-clamp / arbiter application is the delegate's job (sole-
    // caller doctrine §3.1) — the hub has done all gating.
    ++slot.session.streamBundlesAccepted;

    // §11.1: an ACCEPTED bundle on a source-mapped channel is "new motion" from
    // the owning source and clears a latched STOP the SAME way a source-mapped
    // INTENT does (see handleIntent) — otherwise, after a deadman STOP, a
    // resumed stream drives the arbiter (soft-start) while the safety STATE
    // still lies "STOP", a Ground-Truth violation. The bundle was ownership-
    // gated above (a non-owner Conflict-drops before here), so this only fires
    // for the source's live owner.
    if (mappedSource && (_safetyWord & safety_bits::STOP)) {
        _safetyWord &= ~safety_bits::STOP;
        _safetyOwnerSession = 0;
        publishSafetySnapshot();
        broadcastSafetyNow(nowMs);
    }

    _delegate.onStreamBundle(channel_id, slot.session.session_id, bundle);
}

// ============================================================================
// PING/PONG, GOODBYE, CATALOG_REQ
// ============================================================================

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

inline void Hub::handleCatalogReq(Slot& slot, std::span<const std::byte> payload) {
    auto res = decodeCatalogReq(payload);
    if (!res) return;
    const CatalogReqMsg& m = res.value();

    std::span<const std::byte> encoded(_catalogEncoded.data(), _catalogEncodedLen);
    size_t cc = chunkCount(encoded.size());

    auto sendChunk = [&](uint16_t idx) {
        std::array<std::byte, 4 + limits::catalog_chunk_payload> cbuf{};
        size_t n = fillChunk(encoded, idx, std::span<std::byte>(cbuf));
        if (n == 0) return;
        sendFrameTo(*slot.transport, FrameType::CATALOG_CHUNK, channels::catalog,
                    std::span<const std::byte>(cbuf.data(), n));
    };

    if (m.full) {
        for (uint16_t i = 0; i < cc; ++i) sendChunk(i);
    } else {
        for (uint32_t i = 0; i < m.chunks_count; ++i) sendChunk(m.chunks[i]);
    }
}

// ============================================================================
// STATE pacing (§9.1) — conflated push using RetainedStore + SubscriptionTable
// ============================================================================

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

        ShedDecision decision =
            firstPushSinceGrant ? ShedDecision::Send : shedDecision(sub.priority, entry->cls, slot.congestionLevel);

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

// ============================================================================
// Publication API
// ============================================================================

inline bool Hub::publishState(uint16_t channel_id, std::span<const std::byte> payload) {
    return _retained.publish(channel_id, payload).has_value();
}

inline bool Hub::publishEvent(uint16_t channel_id, std::span<const std::byte> encodedEventPayload) {
    bool any = false;
    for (auto& slot : _slots) {
        if (!slot.session.occupied()) continue;
        if (!slot.session.subs.find(channel_id)) continue;
        slot.session.events.push(encodedEventPayload);
        any = true;
    }
    if (any) {
        for (auto& slot : _slots) {
            if (!slot.session.occupied()) continue;
            if (!slot.session.subs.find(channel_id)) continue;
            while (auto ev = slot.session.events.pop()) {
                sendFrameTo(*slot.transport, FrameType::EVENT, channel_id, *ev);
            }
        }
    }
    return any;
}

// ============================================================================
// Safety: ESTOP/STOP latch + critical-priority broadcast (§11.1, §11.2, §11.3)
// ============================================================================

inline std::array<std::byte, 8> Hub::buildSafetyPayload() const {
    std::array<std::byte, 8> buf{};
    std::span<std::byte> s(buf);
    putU8(s.subspan(0, 1), _safetyWord);
    putU8(s.subspan(1, 1), _safetyCause);
    putU32(s.subspan(2, 4), _safetyOwnerSession);
    putU16(s.subspan(6, 2), _estopSeq);
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
}

inline void Hub::latchEstop(uint8_t cause, uint8_t origin, uint16_t estop_seq) {
    if (!(_safetyWord & safety_bits::ESTOP)) {
        _delegate.onEstop(cause, origin);
        _safetyWord |= safety_bits::ESTOP;
        _safetyCause = cause;
        _estopSeq = estop_seq;
        publishSafetySnapshot();
    }
    broadcastSafetyNow(_clock.nowMs());
}

inline bool Hub::estopLatched() const { return (_safetyWord & safety_bits::ESTOP) != 0; }

inline bool Hub::stopLatched() const { return (_safetyWord & safety_bits::STOP) != 0; }

inline uint8_t Hub::safetyWord() const { return _safetyWord; }

inline bool Hub::clearEstop() {
    // §11.2: "the hub MUST refuse (CLEAR_REFUSED) unless (a) the latched
    // cause is resolved ... (b) motion is at zero velocity, and (c) no other
    // stop level is pending escalation." (a)/(b)/(c) are machine-domain
    // conditions this library cannot see — delegate.canClearEstop() is the
    // hook; this method only enforces "must be latched" and "clearing never
    // restarts motion" (it never calls into any motion path).
    if (!(_safetyWord & safety_bits::ESTOP)) return false;
    if (!_delegate.canClearEstop()) return false;

    _safetyWord &= ~safety_bits::ESTOP;
    publishSafetySnapshot();
    broadcastSafetyNow(_clock.nowMs());
    return true;
}

// ============================================================================
// M5: pairing (§12.2)
// ============================================================================

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

    std::array<std::byte, limits::token_bytes> tokenBuf{};
    auto outcome = _pairing.handlePairReq(std::span<const std::byte>(m.instance_id),
                                          std::span<const std::byte>(m.pin_proof), std::span<const std::byte>(slot.nonce),
                                          _rng, nowMs, AccessLevel::controller, std::span<std::byte>(tokenBuf));

    switch (outcome) {
        case PairingManager::PairOutcome::Granted: {
            PairGrantMsg g{};
            g.token = tokenBuf;
            g.roles = uint8_t(AccessLevel::controller);
            std::array<std::byte, 64> buf{};
            size_t n = encodePairGrant(g, std::span<std::byte>(buf));
            if (n > 0) {
                sendFrameTo(*slot.transport, FrameType::PAIR_GRANT, 0, std::span<const std::byte>(buf.data(), n));
            }
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

// ============================================================================
// M5: network probe (§6.4) — hub side: answer PROBE with a timed burst,
// receive PROBE_REPORT and surface its counters.
// ============================================================================

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

// ============================================================================
// M5: congestion input (§10.3) + slow-consumer eviction (§10.4 step 4)
// ============================================================================

inline void Hub::setCongestionLevel(size_t slotIdx, uint8_t level) {
    if (slotIdx >= _slots.size()) return;
    _slots[slotIdx].congestionLevel = level;
    if (level < 2) _slots[slotIdx].criticalStalling = false;
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

inline bool Hub::sendFrameToTracked(Slot& slot, FrameType type, uint16_t channel, std::span<const std::byte> payload,
                                     uint32_t nowMs, uint16_t seq) {
    bool ok = sendFrameTo(*slot.transport, type, channel, payload, seq);
    trackCriticalSend(slot, ok, nowMs);
    return ok;
}

inline void Hub::sendNackTracked(Slot& slot, const NackMsg& n, uint32_t nowMs) {
    std::array<std::byte, 128> buf{};
    size_t len = encodeNack(n, std::span<std::byte>(buf));
    bool ok = len > 0 && sendFrameTo(*slot.transport, FrameType::NACK, 0, std::span<const std::byte>(buf.data(), len));
    trackCriticalSend(slot, ok, nowMs);
}

// ============================================================================
// M5: source ownership plumbing shared by the intent pipeline + deadman
// (§11.4 control-owner STATE, session-events takeover EVENT)
// ============================================================================

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
    ev.has_payload = true;
    ev.payload_count = 2;
    ev.payload[0] = IntentValueField{1, IntentValue::ofU64(source_id)};
    ev.payload[1] = IntentValueField{2, IntentValue::ofU64(newOwnerSession)};

    std::array<std::byte, 64> buf{};
    size_t n = encodeEvent(ev, std::span<std::byte>(buf));
    if (n > 0) publishEvent(channels::session_events, std::span<const std::byte>(buf.data(), n));
}

// ============================================================================
// M5: deadman (§11.3) — evaluated once per occupied session per update()
// ============================================================================

inline void Hub::pumpDeadman(Slot& slot, uint32_t nowMs) {
    // Scope note (documented clarification of a spec/task tension — see the
    // M5 report): §11.3's deadman window binds to the ACTIVE SOURCE, not to
    // sessions in general ("Every session that owns an active source has a
    // deadman window"). A pure viewer/controller session owning nothing
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

    if (!timeReached(nowMs, slot.session.lastRxMs + limits::deadman_default_ms)) return;

    // §6.5/§11.3: the session itself dies with its lost source(s) in this M5
    // pass (see the scoping note above) — GOODBYE best-effort FIRST (the slot
    // is reset below), then the shared teardown releases every owned source
    // with reason=3 (deadman-release), running each source's §11.3 loss policy
    // (Stop latches STOP + stops motion; Continue releases only). Registry code
    // DEADMAN_TIMEOUT (0x0108, allocated when this gap was flagged).
    GoodbyeMsg gb;
    gb.code = NackCode::DEADMAN_TIMEOUT;
    std::array<std::byte, 64> buf{};
    size_t n = encodeGoodbye(gb, std::span<std::byte>(buf));
    if (n > 0 && slot.transport != nullptr) {
        sendFrameTo(*slot.transport, FrameType::GOODBYE, 0, std::span<const std::byte>(buf.data(), n));
    }
    teardownSession(slot, nowMs, /*reason=*/3 /*deadman-release*/);
}

// ============================================================================
// Shared session teardown (§6.8, §11.3, §11.4) — the single choke point every
// slot-ending path funnels through, so source ownership is released the SAME
// way no matter how the session departed.
// ============================================================================

inline void Hub::releaseSessionSources(uint32_t sessionId, uint8_t reason, uint32_t nowMs) {
    bool releasedAny = false;
    _ownership.releaseAllOf(sessionId, [&](uint8_t source) {
        releasedAny = true;
        SourceLossPolicy pol = _delegate.sourcePolicy(source);
        if (pol == SourceLossPolicy::Stop) {
            _delegate.onDeadmanStop(source);  // §11.3: stop motion BEFORE the latch publishes
            _safetyWord |= safety_bits::STOP;
            _safetyCause = 1;  // deadman/owner-loss (§11.1 cause taxonomy has no distinct "session-loss")
            _safetyOwnerSession = sessionId;  // §11.1: "owning session_id where applicable"
            publishSafetySnapshot();
            broadcastSafetyNow(nowMs);  // bypass pacing, like the ESTOP latch does
        }
        // Continue-policy sources: release only, no STOP latch.
        _delegate.onSourceOwnership(source, 0, reason);
    });
    if (releasedAny) publishControlOwnerStateIfPresent();
}

inline void Hub::teardownSession(Slot& slot, uint32_t nowMs, uint8_t reason) {
    if (slot.session.occupied()) {
        releaseSessionSources(slot.session.session_id, reason, nowMs);
        _delegate.onSessionLeft(slot.session.session_id);
    }
    slot.session.reset();
    slot.pushRecords.fill(PushRecord{});
}

// ============================================================================
// Accessors
// ============================================================================

inline uint16_t Hub::cfgGen() const { return _cfgGen; }
inline uint32_t Hub::bootId() const { return _bootId; }

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

// ============================================================================
// M4 test-only unsolicited GRANT hook (§10.2; real policy is M5)
// ============================================================================

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
