// WsServerPort — SlopSimWsTransport/SlopSimWsPort implementation (see
// WsServerPort.h for the marshaling contract).

#include "net/WsServerPort.h"

#include <cstring>

namespace slopsim {

// ---- SlopSimWsTransport -----------------------------------------------------

void SlopSimWsTransport::onOpen(ix::WebSocket* ws, const std::string& peer) {
    std::lock_guard<std::mutex> lk(_m);
    _ws = ws;
    _peer = peer;
    _inUse = true;
    _openEvt = true;
    _closeEvt = false;
    _muted = false;
    _rxHead = _rxTail = _rxCount = 0;
    _rxDrops = 0;
}

void SlopSimWsTransport::onClose() {
    std::lock_guard<std::mutex> lk(_m);
    _ws = nullptr;   // teardown may complete once we release the mutex
    _closeEvt = true;
}

void SlopSimWsTransport::pushRx(const void* data, size_t len) {
    if (len == 0 || len > slopsync::kFrameBufferCapacity) return;  // never a valid frame
    std::lock_guard<std::mutex> lk(_m);
    _muted = false;  // inbound traffic is proof the client drained (firmware doctrine)
    if (_rxCount == kRxRingDepth) {
        ++_rxDrops;  // ring full: drop the NEW frame (firmware doctrine)
        return;
    }
    auto& fb = _rx[_rxTail];
    std::memcpy(fb.writable().data(), data, len);
    fb.setSize(len);
    _rxTail = uint8_t((_rxTail + 1) % kRxRingDepth);
    ++_rxCount;
}

bool SlopSimWsTransport::write(std::span<const std::byte> frame) {
    std::lock_guard<std::mutex> lk(_m);
    if (!_ws || _muted) return false;
    if (_ws->bufferedAmount() > kMuteBufferedBytes) {
        _muted = true;
        _mutedSinceMs = _nowMs ? *_nowMs : 0;
        return false;
    }
    auto info = _ws->sendBinary(std::string(reinterpret_cast<const char*>(frame.data()), frame.size()));
    if (!info.success) {
        _muted = true;
        _mutedSinceMs = _nowMs ? *_nowMs : 0;
        return false;
    }
    return true;
}

std::optional<slopsync::FrameBuffer> SlopSimWsTransport::read() {
    std::lock_guard<std::mutex> lk(_m);
    if (_rxCount == 0) return std::nullopt;
    slopsync::FrameBuffer out = _rx[_rxHead];
    _rxHead = uint8_t((_rxHead + 1) % kRxRingDepth);
    --_rxCount;
    return out;
}

void SlopSimWsTransport::close() {
    requestClose();
}

void SlopSimWsTransport::requestClose() {
    std::lock_guard<std::mutex> lk(_m);
    if (_ws) _ws->close();
}

bool SlopSimWsTransport::consumeOpenEvent() {
    std::lock_guard<std::mutex> lk(_m);
    if (!_openEvt) return false;
    _openEvt = false;
    return true;
}

bool SlopSimWsTransport::consumeCloseEvent() {
    std::lock_guard<std::mutex> lk(_m);
    if (!_closeEvt) return false;
    _closeEvt = false;
    return true;
}

bool SlopSimWsTransport::inUse() const {
    std::lock_guard<std::mutex> lk(_m);
    return _inUse;
}

void SlopSimWsTransport::reap() {
    std::lock_guard<std::mutex> lk(_m);
    _inUse = false;
    _peer.clear();
    _rxHead = _rxTail = _rxCount = 0;
    _muted = false;
}

std::string SlopSimWsTransport::peer() const {
    std::lock_guard<std::mutex> lk(_m);
    return _peer;
}

// ---- SlopSimWsPort ----------------------------------------------------------

bool SlopSimWsPort::begin(slopsync::Hub* hub, uint16_t port, SessionLog* log) {
    _hub = hub;
    _log = log;
    for (auto& s : _slots) s.setNowMsSource(&_nowMs);

    _server = std::make_unique<ix::WebSocketServer>(port, "0.0.0.0");
    _server->setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws,
               const ix::WebSocketMessagePtr& msg) { onMessage(state, ws, msg); });

    auto res = _server->listen();
    if (!res.first) {
        if (_log) _log->logf('E', "ws: listen failed on :%u — %s", unsigned(port), res.second.c_str());
        return false;
    }
    _server->start();
    if (_log) _log->logf('I', "ws: listening on :%u (slopsync.v1)", unsigned(port));
    return true;
}

void SlopSimWsPort::stop() {
    if (_server) _server->stop();
}

// Runs on IXWebSocket connection threads — marshaling producer side only.
void SlopSimWsPort::onMessage(std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws,
                              const ix::WebSocketMessagePtr& msg) {
    const std::string id = state->getId();

    switch (msg->type) {
        case ix::WebSocketMessageType::Open: {
            std::lock_guard<std::mutex> lk(_mapM);
            uint8_t slot = 0xFF;
            for (uint8_t i = 0; i < kSlots; ++i) {
                if (!_slots[i].inUse()) { slot = i; break; }
            }
            if (slot == 0xFF) {
                if (_log) _log->logf('W', "ws: refusing connection %s — all %u slots busy",
                                     id.c_str(), unsigned(kSlots));
                ws.close(1013, "try again later");
                return;
            }
            _slotById[id] = slot;
            _slots[slot].onOpen(&ws, state->getRemoteIp());
            break;
        }
        case ix::WebSocketMessageType::Close: {
            std::lock_guard<std::mutex> lk(_mapM);
            auto it = _slotById.find(id);
            if (it != _slotById.end()) {
                _slots[it->second].onClose();
                _slotById.erase(it);
            }
            break;
        }
        case ix::WebSocketMessageType::Message: {
            if (!msg->binary) return;  // TEXT is not slopsync; ignore (firmware parity)
            uint8_t slot;
            {
                std::lock_guard<std::mutex> lk(_mapM);
                auto it = _slotById.find(id);
                if (it == _slotById.end()) return;
                slot = it->second;
            }
            _slots[slot].pushRx(msg->str.data(), msg->str.size());
            break;
        }
        default:
            break;  // Ping/Pong/Fragment/Error — transport-level, nothing to do
    }
}

// Sim thread: consume marshaled events, sweep stalls.
void SlopSimWsPort::loop(uint32_t nowMs) {
    _nowMs = nowMs;
    for (uint8_t i = 0; i < kSlots; ++i) {
        auto& s = _slots[i];

        if (s.consumeOpenEvent()) {
            if (_hub->attachTransport(s)) {
                _attached[i] = true;
                if (_log) _log->logf('I', "ws: client %s -> slot %u attached", s.peer().c_str(), i);
            } else {
                if (_log) _log->logf('W', "ws: hub refused transport (full) — closing slot %u", i);
                s.requestClose();
            }
        }

        if (s.consumeCloseEvent()) {
            if (_attached[i]) {
                _hub->detachTransport(s);
                _attached[i] = false;
            }
            if (_log) _log->logf('I', "ws: slot %u detached", i);
            s.reap();
        }

        // Stall sweep: a client muted continuously for kStallEvictMs is gone.
        if (_attached[i] && s.muted() && nowMs - s.mutedSinceMs() > SlopSimWsTransport::kStallEvictMs) {
            if (_log) _log->logf('W', "ws: slot %u stalled >%ums — disconnecting", i,
                                 unsigned(SlopSimWsTransport::kStallEvictMs));
            s.requestClose();
        }
    }
}

SlopSimWsPort::SlotInfo SlopSimWsPort::slotInfo(uint8_t i) const {
    SlotInfo info;
    if (i >= kSlots) return info;
    const auto& s = _slots[i];
    info.inUse = s.inUse();
    info.peer = s.peer();
    info.muted = s.muted();
    info.rxDrops = s.rxDrops();
    return info;
}

void SlopSimWsPort::kick(uint8_t i) {
    if (i < kSlots) _slots[i].requestClose();
}

}  // namespace slopsim
