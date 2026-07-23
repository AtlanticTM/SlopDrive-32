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
#include "slopsync/wire/messages/hello.hpp"
#include "slopsync/wire/messages/subscribe.hpp"
#include "slopsync/wire/raw/ping_pong.hpp"
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
            t.close();
            slot.transport = nullptr;
            // Detaching physically removes the transport; whatever session
            // (if any) was riding it is no longer reachable — free it too so
            // its slot is immediately reusable and doesn't linger occupied.
            if (slot.session.occupied()) {
                _delegate.onSessionLeft(slot.session.session_id);
            }
            slot.session.reset();
            slot.pushRecords.fill(PushRecord{});
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
    uint32_t nowMs = nowUs / 1000u;
    for (auto& slot : _slots) {
        if (slot.transport == nullptr) continue;
        pumpSlot(slot, nowMs);
        if (slot.session.occupied()) {
            pumpStatePacing(slot, nowMs);
            // Liveness (§6.5, M4 minimal): reply is event-driven (PING->PONG,
            // handlePing); the hub does not itself originate PING/deadman
            // policy in M4 (deadman dispatch is M5) — only lastRxMs tracking
            // happens on receipt, already done in pumpSlot.
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
        case FrameType::PING:
            if (slot.session.occupied()) handlePing(slot, payload);
            break;
        case FrameType::GOODBYE:
            if (slot.session.occupied()) handleGoodbye(slot);
            break;
        case FrameType::CATALOG_REQ:
            if (slot.session.occupied()) handleCatalogReq(slot, payload);
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
        _delegate.onSessionLeft(dup->session.session_id);
        dup->session.reset();
        dup->pushRecords.fill(PushRecord{});
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

    // Fresh session in this slot.
    slot.session.reset();
    slot.pushRecords.fill(PushRecord{});
    slot.session.state = HubSessionState::VALIDATING;
    do {
        slot.session.session_id = _rng.nextU32();
    } while (slot.session.session_id == 0);
    std::memcpy(slot.session.instance_id.data(), h.instance_id.data(), slot.session.instance_id.size());
    slot.session.role = _delegate.validateToken(instanceSpan, std::span<const std::byte>(h.token), h.has_token);
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
        sendFrameTo(*slot.transport, FrameType::ECHO, m.channel_id, *cached);
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

    // 6) Delegate applies + clamps.
    IntentValueMap requested{m.value_count, m.value};
    bool cfgChanged = false;
    auto applied = _delegate.applyIntent(m.channel_id, requested, slot.session.role, cfgChanged);

    if (!applied) {
        NackMsg n;
        n.code = applied.error();
        n.has_intent_id = true;
        n.intent_id = m.intent_id;
        sendNack(*slot.transport, n);
        return;
    }

    if (cfgChanged) ++_cfgGen;

    EchoMsg echo;
    echo.intent_id = m.intent_id;
    echo.cfg_gen = _cfgGen;
    echo.applied_count = applied.value().count;
    echo.applied = applied.value().fields;

    std::array<std::byte, 256> ebuf{};
    size_t elen = encodeEcho(echo, std::span<std::byte>(ebuf));
    if (elen == 0) return;  // catalog-conformance bug (applied map too large); nothing sane to send

    slot.session.intentRing.store(m.intent_id, std::span<const std::byte>(ebuf.data(), elen));
    sendFrameTo(*slot.transport, FrameType::ECHO, m.channel_id, std::span<const std::byte>(ebuf.data(), elen));
}

// ============================================================================
// PING/PONG, GOODBYE, CATALOG_REQ
// ============================================================================

inline void Hub::handlePing(Slot& slot, std::span<const std::byte> payload) {
    std::array<std::byte, 32> buf{};
    size_t n = encodePong(payload, std::span<std::byte>(buf));
    sendFrameTo(*slot.transport, FrameType::PONG, 0, std::span<const std::byte>(buf.data(), n));
}

inline void Hub::handleGoodbye(Slot& slot) {
    _delegate.onSessionLeft(slot.session.session_id);
    slot.session.reset();
    slot.pushRecords.fill(PushRecord{});
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
        if (!entry || entry->cls != ChannelClass::STATE) continue;  // STREAM/EVENT pacing is out of M4 scope
        auto retained = _retained.get(sub.channel_id);
        if (!retained) continue;  // nothing published for this channel yet

        PushRecord* pr = findOrCreatePushRecord(slot, sub.channel_id);
        bool changePending = (pr == nullptr) || !pr->valid || seqIsNewer(retained->seq, pr->lastSeq);

        if (sub.dueForPush(nowMs, changePending)) {
            if (sendFrameTo(*slot.transport, FrameType::STATE, sub.channel_id, retained->payload, retained->seq)) {
                sub.markPushed(nowMs);
                if (pr) {
                    pr->lastSeq = retained->seq;
                    pr->valid = true;
                }
            }
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
// Safety: ESTOP latch + critical-priority broadcast (§11.2)
// ============================================================================

inline std::array<std::byte, 8> Hub::buildSafetyPayload(uint8_t cause) const {
    std::array<std::byte, 8> buf{};
    std::span<std::byte> s(buf);
    putU8(s.subspan(0, 1), uint8_t(1));  // bitfield8 `word`: bit0 = estop
    putU8(s.subspan(1, 1), cause);
    putU32(s.subspan(2, 4), 0);  // owner_session: 0 for M4 (§11.1, "not applicable yet")
    putU16(s.subspan(6, 2), _estopSeq);
    return buf;
}

inline void Hub::broadcastSafetyNow(uint32_t nowMs) {
    auto retained = _retained.get(channels::safety);
    if (!retained) return;
    for (auto& slot : _slots) {
        if (!slot.session.occupied()) continue;
        if (!slot.session.subs.find(channels::safety)) continue;
        if (sendFrameTo(*slot.transport, FrameType::STATE, channels::safety, retained->payload, retained->seq)) {
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
    if (!_estopLatched) {
        _delegate.onEstop(f.cause, f.origin);
        _estopLatched = true;
        _estopSeq = f.seq;
        _retained.publish(channels::safety, std::span<const std::byte>(buildSafetyPayload(f.cause)));
    }
    // Always re-broadcast NOW, bypassing normal pacing (§10.1 critical
    // priority, §10.4's ESTOP exemption) — repeats are the client's only
    // loss-recovery mechanism (§11.2), so a repeat must be able to trigger a
    // fresh delivery attempt even once already latched.
    broadcastSafetyNow(nowMs);
}

inline void Hub::latchEstop(uint8_t cause, uint8_t origin, uint16_t estop_seq) {
    if (!_estopLatched) {
        _delegate.onEstop(cause, origin);
        _estopLatched = true;
        _estopSeq = estop_seq;
        _retained.publish(channels::safety, std::span<const std::byte>(buildSafetyPayload(cause)));
    }
    broadcastSafetyNow(_clock.nowMs());
}

inline bool Hub::estopLatched() const { return _estopLatched; }

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
