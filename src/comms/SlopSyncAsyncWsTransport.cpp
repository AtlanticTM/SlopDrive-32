// The WHOLE file compiles to nothing unless -DSLOPSYNC_WS_ASYNC=1 is set, the
// same pattern src/ui/SlopHttpServer.cpp uses for the PsychicHttp backend.
//
// This guard is NOT optional tidiness. Everything under src/ is compiled by
// every environment whether or not that environment wants it, so without it the
// links2004 builds fail on `ESPAsyncWebServer.h: No such file or directory` --
// in an env that never references the async transport at all. Trying to fix
// that from platformio.ini (lib_ignore juggling) treats the symptom; the file
// excluding itself is the actual fix.
#if defined(SLOPSYNC_WS_ASYNC) && SLOPSYNC_WS_ASYNC

#include "SlopSyncAsyncWsTransport.h"

#include "sloplog/sloplog.h"

namespace slopdrive {

// ---------------------------------------------------------------------------
// SlopSyncAsyncWsTransport
// ---------------------------------------------------------------------------

bool SlopSyncAsyncWsTransport::isDroppable(std::span<const std::byte> frame) {
    // Byte 0 of every frame is the type (wire/frame_header.hpp), so this costs
    // one load and no parsing. The registry itself classifies each type as
    // control / data / raw — only `data` may be shed, and that is exactly
    // STATE and STREAM. Everything else, EVENT included, is a promise to the
    // client that something will arrive.
    if (frame.size() < 1) return false;
    const auto t = static_cast<slopsync::FrameType>(frame[0]);
    return t == slopsync::FrameType::STATE || t == slopsync::FrameType::STREAM;
}

bool SlopSyncAsyncWsTransport::open() {
    // DO NOT RESET THE RING HERE. It used to, with the note "safe because
    // open() runs on the hub task with no client attached yet, so there is no
    // concurrent producer" -- and that stopped being true the moment attach was
    // deferred onto the hub task (field bug #5). The new order is:
    //
    //   AsyncTCP: WS_EVT_CONNECT -> attachClient() [resets the ring]
    //   AsyncTCP: WS_EVT_DATA    -> pushRx(HELLO)  [ring now holds the HELLO]
    //   hub task: loop()         -> attachTransport() -> open()
    //
    // so a reset here discards the client's HELLO and every session dies at its
    // handshake timeout. attachClient() already resets the ring at the correct
    // boundary -- the arrival of a NEW client, on the producer side, when there
    // is provably no other producer -- so this only ever duplicated it.
    _open.store(true, std::memory_order_release);
    return true;
}

void SlopSyncAsyncWsTransport::close() {
    _open.store(false, std::memory_order_release);
    _rxHead.store(0, std::memory_order_relaxed);
    _rxTail.store(0, std::memory_order_relaxed);
}

bool SlopSyncAsyncWsTransport::write(std::span<const std::byte> frame) {
    // §9 / §13.1: MUST NOT block. Nothing on this path can — AsyncWebSocket
    // queues and returns. false = not accepted; the hub's class semantics
    // decide drop vs retry.
    if (!_open.load(std::memory_order_acquire) || _ws == nullptr) return false;
    if (frame.empty()) return true;

    const uint32_t id = _clientId.load(std::memory_order_acquire);
    if (id == 0) return false;

    // Everything below addresses the client BY ID. Never cache the pointer:
    // the AsyncTCP task may destroy the client between a lookup and its use,
    // and the id-taking API re-resolves under _ws_clients_lock every call.
    const bool droppable = isDroppable(frame);

    if (_policy != SlopSyncTxPolicy::CloseAlways && droppable) {
        // Shed telemetry EARLY, before the queue is full, so control frames
        // still have room behind it. If data is allowed to fill the queue then
        // classifying was pointless: the next NACK is the one that fails.
        AsyncWebSocketClient* c = _ws->client(id);
        if (c != nullptr && c->queueLen() >= kDataQueueHighWater) {
            _txDataDrops.fetch_add(1, std::memory_order_relaxed);
            SLOGD_EVERY_MS(5000, "slopsync",
                           "client#%u shedding data frames (queue %u/%u)",
                           unsigned(id), unsigned(c->queueLen()),
                           unsigned(WS_MAX_QUEUED_MESSAGES));
            return false;   // honest refusal; the hub conflates and moves on
        }
    }

    const bool ok = _ws->binary(id,
                                reinterpret_cast<const uint8_t*>(frame.data()),
                                frame.size());
    if (ok) {
        if (!droppable) _ctrlStallSinceMs = 0;   // control is flowing again
        return true;
    }

    // ---- The queue refused it. THIS IS USUALLY NOT AN ERROR. ---------------
    // ITransport::write returning false already MEANS "not accepted right now;
    // the caller's class semantics decide retry vs drop" (§13.1). A burst that
    // outruns the queue is ordinary flow control: the catalog BLOB transfer
    // pushes 57 chunks into a 32-deep queue and fills it EVERY time, and the
    // hub simply retries on the next tick.
    //
    // An earlier version of this function tore the session down on the first
    // refused control frame. That killed every session at BLOB chunk ~16 --
    // caught on hardware, by this file's own log line. Do not reintroduce it:
    // "queue full once" and "client is not draining" are different facts.
    if (droppable) {
        _txDataDrops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    _txCtrlFails.fetch_add(1, std::memory_order_relaxed);

    // Control frames get a STALL TIMER rather than an immediate verdict. The
    // hub will retry; what we are watching for is a queue that never drains,
    // which is the one case where the client really is stranded waiting on a
    // reply. Unlike the links2004 mute this cannot be reset by the client
    // sending us traffic -- only by a control frame actually going out -- so a
    // chatty-but-wedged client cannot hold the session open forever.
    const uint32_t now = millis();
    if (_ctrlStallSinceMs == 0) {
        _ctrlStallSinceMs = now;
        return false;
    }
    if (_policy == SlopSyncTxPolicy::DropAlways) return false;   // measurement only
    if (uint32_t(now - _ctrlStallSinceMs) < kCtrlStallMs) return false;

    SLOGW("slopsync",
          "client#%u control frame (type 0x%02X) unsendable for %ums — closing session",
          unsigned(id), unsigned(frame[0]), unsigned(now - _ctrlStallSinceMs));
    _ctrlStallSinceMs = 0;
    _ws->close(id);
    return false;
}

std::optional<slopsync::FrameBuffer> SlopSyncAsyncWsTransport::read() {
    // Consumer side of the SPSC ring (hub task). ACQUIRE on the tail pairs with
    // the producer's RELEASE store in pushRx: it is what guarantees the frame's
    // BYTES are visible here and not just the index. Without that pairing a
    // weakly-ordered core could publish the index first and hand us a
    // half-written buffer.
    const uint8_t head = _rxHead.load(std::memory_order_relaxed);
    const uint8_t tail = _rxTail.load(std::memory_order_acquire);
    if (head == tail) return std::nullopt;   // empty

    slopsync::FrameBuffer fb = _rx[head];
    // RELEASE so the producer cannot observe the freed slot before we have
    // finished copying out of it.
    _rxHead.store(uint8_t((head + 1) % kRxRingDepth), std::memory_order_release);
    return fb;
}

slopsync::TransportProperties SlopSyncAsyncWsTransport::properties() const {
    slopsync::TransportProperties p;
    p.mtu = uint16_t(slopsync::kFrameBufferCapacity);   // whole-frame bytes
    p.ordered = true;                                   // TCP-backed
    p.reliable = true;
    p.congestion = slopsync::CongestionSignal::QueueWatermark;
    return p;
}

void SlopSyncAsyncWsTransport::attachClient(uint32_t id) {
    // AsyncTCP task. Publish the id with RELEASE so a hub-task write() that
    // sees it also sees a ring that has been reset.
    _rxHead.store(0, std::memory_order_relaxed);
    _rxTail.store(0, std::memory_order_relaxed);
    _ctrlStallSinceMs = 0;
    _rxDrops.store(0, std::memory_order_relaxed);
    _txDataDrops.store(0, std::memory_order_relaxed);
    _txCtrlFails.store(0, std::memory_order_relaxed);
    _clientId.store(id, std::memory_order_release);
}

void SlopSyncAsyncWsTransport::detachClient() {
    // AsyncTCP task. Clearing the id is what makes every subsequent write()
    // refuse instantly — no pointer to dangle, nothing to race.
    _clientId.store(0, std::memory_order_release);
}

void SlopSyncAsyncWsTransport::pushRx(const uint8_t* data, size_t len) {
    // Producer side (AsyncTCP task). One WS message == one SlopSync frame.
    if (len == 0 || len > slopsync::kFrameBufferCapacity) {
        _rxDrops.fetch_add(1, std::memory_order_relaxed);
        SLOGW_EVERY_MS(2000, "slopsync", "RX frame dropped (len=%u)", unsigned(len));
        return;
    }

    const uint8_t tail = _rxTail.load(std::memory_order_relaxed);
    const uint8_t next = uint8_t((tail + 1) % kRxRingDepth);
    if (next == _rxHead.load(std::memory_order_acquire)) {
        // Full. Drop the NEW frame: an older buffered one is closer to being
        // consumed by the hub this same tick, so discarding it would be
        // strictly worse. Same choice the links2004 port made.
        _rxDrops.fetch_add(1, std::memory_order_relaxed);
        SLOGW_EVERY_MS(2000, "slopsync", "RX ring full — frame dropped");
        return;
    }

    _rx[tail] = slopsync::FrameBuffer::from(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), len));
    // RELEASE: everything written to the slot above must be visible to the
    // consumer BEFORE it can observe the advanced tail. See read().
    _rxTail.store(next, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// SlopSyncAsyncWsPort
// ---------------------------------------------------------------------------

SlopSyncAsyncWsPort::SlopSyncAsyncWsPort()
    : _http(SLOPSYNC_WS_PORT), _ws("/") {}

void SlopSyncAsyncWsPort::setPolicy(SlopSyncTxPolicy p) {
    for (auto& s : _slots) s.setPolicy(p);
}

void SlopSyncAsyncWsPort::begin(slopsync::Hub* hub) {
    _hub = hub;
    for (auto& s : _slots) s.bind(&_ws);
    for (auto& a : _attached) a.store(false, std::memory_order_relaxed);

    // THE handshake contract. Our vendored patch makes this a real RFC 6455
    // selection: a client that does not offer exactly this gets no
    // Sec-WebSocket-Protocol header back and MUST fail the connection.
    // Upstream would have echoed whatever was asked for. See VENDORED.md.
    _ws.setSubprotocol(SLOPSYNC_WS_SUBPROTOCOL);

    _ws.onEvent([this](AsyncWebSocket* server, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
        onEvent(server, client, type, arg, data, len);
    });

    _http.addHandler(&_ws);
    _http.begin();
    SLOGI("slopsync", "async WS port up on :%u (subprotocol %s, %u slots)",
          unsigned(SLOPSYNC_WS_PORT), SLOPSYNC_WS_SUBPROTOCOL, unsigned(kSlots));
}

int SlopSyncAsyncWsPort::slotForClient(uint32_t id) const {
    if (id == 0) return -1;
    for (int i = 0; i < int(kSlots); ++i) {
        if (_slots[i].clientId() == id) return i;
    }
    return -1;
}

void SlopSyncAsyncWsPort::detachSlot(int slot) {
    // HUB TASK ONLY. This calls into the hub, so the ONLY caller may be
    // loop()'s reap sweep. The disconnect EVENT no longer comes here -- it sets
    // _wantDetach and loop() does the work. See field bug #5 in the header.
    if (slot < 0 || slot >= int(kSlots)) return;
    // Idempotent: both the reap sweep and a deferred detach may reach here.
    if (_attached[slot].exchange(false, std::memory_order_acq_rel)) {
        if (_hub) _hub->detachTransport(_slots[slot]);
    }
    _slots[slot].detachClient();
}

void SlopSyncAsyncWsPort::onEvent(AsyncWebSocket* /*server*/, AsyncWebSocketClient* client,
                                  AwsEventType type, void* arg, uint8_t* data, size_t len) {
    // ***** THIS RUNS ON THE ASYNCTCP TASK, NOT THE HUB TASK. *****
    // Everything touched here is either atomic or owned by AsyncWebSocket's own
    // locks. Do not add non-atomic shared state to this path.
    if (client == nullptr) return;
    const uint32_t id = client->id();

    switch (type) {
        case WS_EVT_CONNECT: {
            int slot = -1;
            for (int i = 0; i < int(kSlots); ++i) {
                if (_slots[i].clientId() == 0) { slot = i; break; }
            }
            if (slot < 0) {
                // Hub is full. Refuse cleanly rather than accepting a client we
                // cannot serve — an accepted-then-silent socket is exactly the
                // "connected but nothing happens" failure users report as a bug.
                SLOGW("slopsync", "no free slot for WS client#%u — closing", unsigned(id));
                client->close();
                return;
            }
            // Claim the slot HERE (so a second connect cannot take it) but do
            // NOT call the hub from this task -- see the flags' note in the
            // header. loop() completes the attach on the hub task.
            _slots[slot].attachClient(id);
            _wantAttach[slot].store(true, std::memory_order_release);
            SLOGI("slopsync", "WS client#%u claimed slot %d (attach deferred to hub task)",
                  unsigned(id), slot);
            break;
        }

        case WS_EVT_DISCONNECT:
        case WS_EVT_ERROR: {
            const int slot = slotForClient(id);
            if (slot >= 0) {
                SLOGI("slopsync", "WS client#%u gone (slot %d) — detach deferred", unsigned(id), slot);
                // Flag only. The slot's clientId is deliberately NOT cleared
                // here: leaving it occupied until the hub task has finished the
                // teardown is what stops a fresh connect from reusing a slot
                // the hub still believes is live.
                _wantDetach[slot].store(true, std::memory_order_release);
            }
            break;
        }

        case WS_EVT_DATA: {
            const AwsFrameInfo* info = static_cast<AwsFrameInfo*>(arg);
            if (info == nullptr) return;
            // slopsync.v1 is BINARY and one message == one frame. Anything
            // fragmented or text is not our framing; ignore it quietly rather
            // than trying to reassemble a protocol that never fragments.
            if (!info->final || info->index != 0 || info->len != len) return;
            if (info->opcode != WS_BINARY) return;

            const int slot = slotForClient(id);
            if (slot >= 0) _slots[slot].pushRx(data, len);
            break;
        }

        default:
            // PONG and friends — AsyncWebSocket handles keepalive itself.
            break;
    }
}

void SlopSyncAsyncWsPort::loop() {
    // AsyncTCP needs no pumping, so unlike the links2004 port there is no
    // service call here and no stall sweep: a wedged client can no longer stall
    // anything, because the send path never blocks. What IS still worth doing
    // is reaping clients AsyncWebSocket has already given up on, so hub slots
    // do not leak if a disconnect event was ever missed.
    _ws.cleanupClients(kSlots);

    // ---- Deferred attach/detach, ON THE HUB TASK (field bug #5) ------------
    // DETACH IS PROCESSED FIRST, and the ordering is load-bearing: a client
    // that connected and vanished between two ticks has BOTH flags set, and
    // the honest resolution is "nothing was ever attached, so there is nothing
    // to tear down" -- handling attach first would hand the hub a session that
    // is already gone and immediately tear it down again.
    for (int i = 0; i < int(kSlots); ++i) {
        if (_wantDetach[i].exchange(false, std::memory_order_acq_rel)) {
            _wantAttach[i].store(false, std::memory_order_release);
            if (_attached[i].exchange(false, std::memory_order_acq_rel)) {
                if (_hub) _hub->detachTransport(_slots[i]);
            }
            _slots[i].detachClient();
            continue;
        }
        if (_wantAttach[i].exchange(false, std::memory_order_acq_rel)) {
            const uint32_t id = _slots[i].clientId();
            if (_hub && _hub->attachTransport(_slots[i])) {
                _attached[i].store(true, std::memory_order_release);
                SLOGI("slopsync", "WS client#%u attached to slot %d", unsigned(id), i);
            } else {
                SLOGW("slopsync", "hub refused WS client#%u — closing", unsigned(id));
                _slots[i].detachClient();
                _ws.close(id);
            }
        }
    }

    for (int i = 0; i < int(kSlots); ++i) {
        const uint32_t id = _slots[i].clientId();
        if (id == 0) continue;
        if (!_ws.hasClient(id)) {
            SLOGW("slopsync", "slot %d client#%u vanished without an event — reaping",
                  i, unsigned(id));
            detachSlot(i);
        }
    }
}

}  // namespace slopdrive

#endif  // SLOPSYNC_WS_ASYNC
