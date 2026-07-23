#pragma once

// ============================================================================
// SlopSyncWsTransport / SlopSyncWsPort — the firmware WebSocket binding for the
// SlopSync hub (SPEC §13.1: real transport adapters live in firmware, NEVER in
// lib/slopsync). One binary WS server on SLOPSYNC_WS_PORT (82), subprotocol
// "slopsync.v1"; each WS client num maps 1:1 onto one SlopSyncWsTransport slot,
// which the hub attaches to a session slot.
//
// ─── THE ONE-TASK INVARIANT (READ BEFORE TOUCHING _ws) ──────────────────────
// EVERY access to the owned WebSocketsServer `_ws` — begin(), loop(), sendBIN()
// (via ITransport::write), the onEvent dispatch, and disconnect() — happens on
// the SINGLE SlopSync service task (SlopSyncHubService's "SlopSyncHub" task,
// Core 0). Nothing else ever touches `_ws`.  BECAUSE of that, there is NO mutex
// here and none is needed: read()/write() are called by hub.update() on that
// task; the onEvent callback fires synchronously from inside _ws.loop() on that
// SAME task; the stall sweep runs there too. This is deliberately UNLIKE
// UiSocket, whose recursive _wsMutex exists only because httpTask AND senderTask
// both poke its _ws. Anyone who calls into this port from a second task
// reintroduces exactly that complexity — don't. The RX ring is filled and
// drained on one task, so it needs no locking either.
//
// Backpressure doctrine (§9, §10.4): write() must NEVER block the hub. sendBIN
// returns false on a wedged/backed-up socket; that mutes the client (like
// UiSocket's _sendStalled) so every subsequent write() returns false instantly.
// A client stalled continuously for ~2 s is disconnected by the port's sweep.
// The mute clears the instant the client sends us anything (proof it drained).
// ============================================================================

#include <Arduino.h>
#include <WebSocketsServer.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "config_api.h"
#include "slopsync/hub/hub.hpp"
#include "slopsync/transport/transport.hpp"
#include "slopsync/wire/frame_buffer.hpp"

namespace slopdrive {

// One WS client's endpoint of the frame pipe. Bound to a fixed client num for
// the lifetime of the port; open()/close() just gate + reset it.
class SlopSyncWsTransport final : public slopsync::ITransport {
public:
    static constexpr uint8_t kRxRingDepth = 4;   // buffered inbound frames before drop
    static constexpr uint32_t kStallEvictMs = 2000;  // §10.4-style: wedged this long → disconnect

    SlopSyncWsTransport() = default;

    // Wired once by the port at construction (server + this slot's client num).
    void bind(WebSocketsServer* ws, uint8_t num) { _ws = ws; _num = num; }

    // ---- ITransport -------------------------------------------------------
    bool open() override;
    void close() override;
    bool write(std::span<const std::byte> frame) override;
    std::optional<slopsync::FrameBuffer> read() override;
    slopsync::TransportProperties properties() const override;

    // ---- Driven by the port (all on the ONE service task) -----------------
    // Copy one inbound WS BIN message into the RX ring (one WS msg = one frame).
    void pushRx(const uint8_t* data, size_t len);
    // Stall bookkeeping for the port's disconnect sweep.
    bool muted() const { return _muted; }
    uint32_t mutedSinceMs() const { return _mutedSinceMs; }
    uint32_t dropCount() const { return _rxDrops; }

private:
    WebSocketsServer* _ws = nullptr;
    uint8_t _num = 0;
    bool _open = false;

    // RX ring (single producer = onEvent, single consumer = read(), same task).
    slopsync::FrameBuffer _rx[kRxRingDepth]{};
    uint8_t _rxHead = 0;   // next pop
    uint8_t _rxTail = 0;   // next push
    uint8_t _rxCount = 0;
    uint32_t _rxDrops = 0;

    // Send-stall mute (a wedged client must never block the hub).
    bool _muted = false;
    uint32_t _mutedSinceMs = 0;
};

// The server + its N transport slots (N = hub slot capacity). Owns the WS
// listener; drives it from the service loop; bridges connect/disconnect to
// hub.attach/detachTransport.
class SlopSyncWsPort {
public:
    // One transport per hub session slot + the one spare admission slot the hub
    // keeps (kHubMaxSessions + 1). WS client nums at or above this are refused.
    static constexpr uint8_t kSlots = slopsync::kHubMaxSessions + 1;

    SlopSyncWsPort();

    // Wire the hub, start the WS listener + heartbeat, install onEvent.
    void begin(slopsync::Hub* hub);
    // Pump: service the WS library (accept/read/heartbeat) then sweep stalls.
    // Called every service tick, on the ONE service task.
    void loop();

private:
    void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
    void detachSlot(uint8_t num);   // idempotent: hub detach + slot reset

    WebSocketsServer _ws;
    slopsync::Hub* _hub = nullptr;
    SlopSyncWsTransport _slots[kSlots];
    bool _attached[kSlots] = {};   // whether _slots[n] is currently attached to _hub
};

}  // namespace slopdrive
