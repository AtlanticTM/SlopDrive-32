#include "SlopSyncWsTransport.h"

#include "sloplog/sloplog.h"

// ============================================================================
// SlopSyncWsTransport / SlopSyncWsPort — implementation.
//
// See the header for the ONE-TASK INVARIANT: no locking anywhere in this file
// is intentional and correct, because begin/loop/onEvent/read/write all run on
// the single SlopSync service task. If you add a caller from another task, this
// stops being safe.
// ============================================================================

namespace slopdrive {

// ---------------------------------------------------------------------------
// SlopSyncWsTransport
// ---------------------------------------------------------------------------

bool SlopSyncWsTransport::open() {
    _open = true;
    _rxHead = _rxTail = _rxCount = 0;
    _muted = false;
    _mutedSinceMs = 0;
    return true;
}

void SlopSyncWsTransport::close() {
    _open = false;
    _rxHead = _rxTail = _rxCount = 0;
    _muted = false;
    _mutedSinceMs = 0;
}

bool SlopSyncWsTransport::write(std::span<const std::byte> frame) {
    // §9 / §13.1: MUST NOT block. false = not accepted; the hub's class
    // semantics decide drop vs retry. Once muted we refuse instantly — the
    // wedged socket never gets another blocking sendBIN attempt from the hub.
    if (!_open || _ws == nullptr) return false;
    if (_muted) return false;
    if (frame.empty()) return true;

    bool ok = _ws->sendBIN(_num, reinterpret_cast<uint8_t*>(const_cast<std::byte*>(frame.data())),
                           frame.size());
    if (!ok) {
        _muted = true;
        _mutedSinceMs = millis();
        SLOGW_EVERY_MS(2000, "slopsync", "client#%u send stalled — muted (kept connected)", _num);
    }
    return ok;
}

std::optional<slopsync::FrameBuffer> SlopSyncWsTransport::read() {
    // Non-blocking poll: at most one complete frame per call (§13.1).
    if (_rxCount == 0) return std::nullopt;
    slopsync::FrameBuffer fb = _rx[_rxHead];
    _rxHead = uint8_t((_rxHead + 1) % kRxRingDepth);
    --_rxCount;
    return fb;
}

slopsync::TransportProperties SlopSyncWsTransport::properties() const {
    slopsync::TransportProperties p;
    p.mtu = uint16_t(slopsync::kFrameBufferCapacity);  // whole-frame bytes
    p.ordered = true;                                  // TCP-backed
    p.reliable = true;
    p.congestion = slopsync::CongestionSignal::QueueWatermark;
    return p;
}

void SlopSyncWsTransport::pushRx(const uint8_t* data, size_t len) {
    // Inbound traffic proves the client is draining: clear any send-stall mute
    // (it earns the stream back), exactly like UiSocket's inbound-frame clear.
    _muted = false;
    _mutedSinceMs = 0;

    // One WS message == one SlopSync frame. Oversize (> FrameBuffer capacity)
    // can never be a valid frame on this profile — drop + count it.
    if (len == 0 || len > slopsync::kFrameBufferCapacity) {
        ++_rxDrops;
        SLOGW_EVERY_MS(2000, "slopsync", "client#%u dropped RX frame (len=%u)", _num, unsigned(len));
        return;
    }
    if (_rxCount >= kRxRingDepth) {
        // Ring full: drop the NEW frame (an older buffered one is closer to
        // being consumed by the hub this same tick). Count it.
        ++_rxDrops;
        SLOGW_EVERY_MS(2000, "slopsync", "client#%u RX ring full — frame dropped", _num);
        return;
    }
    _rx[_rxTail] = slopsync::FrameBuffer::from(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), len));
    _rxTail = uint8_t((_rxTail + 1) % kRxRingDepth);
    ++_rxCount;
}

// ---------------------------------------------------------------------------
// SlopSyncWsPort
// ---------------------------------------------------------------------------

SlopSyncWsPort::SlopSyncWsPort()
    // Subprotocol pinned to the spec value (limits::ws_subprotocol) so a client
    // negotiating "slopsync.v1" is matched by the Links2004 server.
    : _ws(SLOPSYNC_WS_PORT, "", "slopsync.v1") {
    for (uint8_t n = 0; n < kSlots; ++n) _slots[n].bind(&_ws, n);
}

void SlopSyncWsPort::begin(slopsync::Hub* hub) {
    _hub = hub;
    _ws.begin();
    // Lenient heartbeat — same rationale as UiSocket: an aggressive one drops
    // slow-but-alive clients on Wi-Fi/Tailscale jitter. (ping every 15s, expect
    // pong within 5s, 3 misses to drop.)
    _ws.enableHeartbeat(15000, 5000, 3);
    _ws.onEvent([this](uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
        this->onWsEvent(num, type, payload, length);
    });
    SLOGI("slopsync", "hub WS port up on :%u (slopsync.v1)", unsigned(SLOPSYNC_WS_PORT));
}

void SlopSyncWsPort::loop() {
    // Service the WS library first: this is where onWsEvent fires (synchronous,
    // same task), inbound frames land in the RX rings, and heartbeats run.
    _ws.loop();

    // Stall sweep: any transport muted continuously for kStallEvictMs is a
    // wedged/half-open socket — disconnect it so it stops occupying a hub slot.
    // _ws.disconnect() will also surface a WStype_DISCONNECTED; detachSlot() is
    // idempotent so a double-detach here + on that event is harmless.
    uint32_t now = millis();
    for (uint8_t n = 0; n < kSlots; ++n) {
        if (!_attached[n]) continue;
        if (!_slots[n].muted()) continue;
        if ((now - _slots[n].mutedSinceMs()) < SlopSyncWsTransport::kStallEvictMs) continue;
        SLOGW("slopsync", "client#%u stalled >%ums — disconnecting", n,
              unsigned(SlopSyncWsTransport::kStallEvictMs));
        detachSlot(n);
        _ws.disconnect(n);
    }
}

void SlopSyncWsPort::detachSlot(uint8_t num) {
    if (num >= kSlots) return;
    if (!_attached[num]) return;
    if (_hub) _hub->detachTransport(_slots[num]);
    _slots[num].close();
    _attached[num] = false;
}

void SlopSyncWsPort::onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    // Runs synchronously inside _ws.loop() on the service task.
    switch (type) {
        case WStype_CONNECTED: {
            // The hub owns only kSlots physical slots; a client num beyond that
            // can't be tracked — refuse it rather than index out of range.
            if (num >= kSlots) {
                SLOGW("slopsync", "WS client#%u beyond slot capacity — rejecting", num);
                _ws.disconnect(num);
                return;
            }
            // Reconnect on a num whose previous session never got a clean
            // DISCONNECTED: detach the stale one first.
            if (_attached[num]) detachSlot(num);
            if (_hub && _hub->attachTransport(_slots[num])) {
                _attached[num] = true;
                SLOGI("slopsync", "WS client#%u connected — transport attached", num);
            } else {
                // Hub full (all session slots taken): nothing to attach to.
                SLOGW("slopsync", "WS client#%u connected but hub full — disconnecting", num);
                _ws.disconnect(num);
            }
            break;
        }
        case WStype_DISCONNECTED:
            if (num < kSlots) {
                SLOGI("slopsync", "WS client#%u disconnected", num);
                detachSlot(num);
            }
            break;
        case WStype_BIN:
            if (num < kSlots && _attached[num]) {
                _slots[num].pushRx(payload, length);
            }
            break;
        case WStype_ERROR:
            SLOGW("slopsync", "WS client#%u error (len=%u)", num, unsigned(length));
            break;
        default:
            // TEXT / PING / PONG / fragmented — not part of the binary
            // slopsync.v1 framing; ignore quietly.
            break;
    }
}

}  // namespace slopdrive
