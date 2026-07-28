#pragma once

// SlopSimWsTransport / SlopSimWsPort — host WebSocket binding for the
// slopsim hub.
// Constraints:
//   Modeled on the firmware's SlopSyncWsTransport/SlopSyncWsPort
//   (include/comms/SlopSyncWsTransport.h) with one structural difference:
//   IXWebSocket runs ONE THREAD PER CONNECTION and fires its callbacks
//   there, while the hub is strictly single-threaded (the sim thread pumps
//   hub.update()). The firmware's one-task invariant becomes a MARSHALING
//   rule here: connection threads only ever (a) push inbound frames into a
//   per-slot mutex-guarded RX ring and (b) flip open/close event flags; the
//   sim thread consumes both in loop(). Nothing but the sim thread ever
//   calls hub.attach/detachTransport or ITransport::read()/write().
//   The one true cross-thread hazard is ITransport::write() (sim thread)
//   racing connection teardown (conn thread destroys the ix::WebSocket
//   after its final callback returns). The per-slot mutex closes it:
//   write() holds the mutex across sendBinary(), and the CLOSE callback
//   takes the same mutex before nulling _ws — so teardown cannot complete
//   while a send is in flight, and no send can start after the pointer is
//   nulled.
//   Backpressure doctrine is the firmware's, verbatim: write() never
//   blocks; bufferedAmount() over threshold (or a failed send) MUTES the
//   client so every later write() returns false instantly; mute clears on
//   any inbound frame; muted continuously > kStallEvictMs -> the sweep
//   disconnects the client.
//   KNOWN GAP: IXWebSocket's server handshake does NOT echo
//   Sec-WebSocket-Protocol. Python/websocket-client and the C# plugin
//   tolerate that; BROWSERS hard-fail it. Before pointing slopsync-js at
//   the sim, this port either gets a patched handshake or is swapped for a
//   minimal single-threaded RFC6455 server (which would also make the
//   marshaling above unnecessary — firmware-identical structure).

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

#include <ixwebsocket/IXWebSocketServer.h>

#include "slopsync/hub/hub.hpp"
#include "slopsync/transport/transport.hpp"
#include "slopsync/wire/frame_buffer.hpp"

#include "common/SessionLog.h"

namespace slopsim {

class SlopSimWsTransport final : public slopsync::ITransport {
public:
    static constexpr uint8_t kRxRingDepth = 8;        // host RAM is free; firmware uses 4
    static constexpr uint32_t kStallEvictMs = 2000;   // firmware doctrine
    static constexpr size_t kMuteBufferedBytes = 64 * 1024;

    // ---- ITransport (sim thread only) ---------------------------------------
    bool open() override { return true; }
    void close() override;
    bool write(std::span<const std::byte> frame) override;
    std::optional<slopsync::FrameBuffer> read() override;
    slopsync::TransportProperties properties() const override {
        slopsync::TransportProperties p;
        p.mtu = uint16_t(slopsync::kFrameBufferCapacity);
        p.ordered = true;
        p.reliable = true;
        p.congestion = slopsync::CongestionSignal::QueueWatermark;
        return p;
    }

    // ---- Connection-thread side (marshaling producers) ----------------------
    void onOpen(ix::WebSocket* ws, const std::string& peer);
    void onClose();
    void pushRx(const void* data, size_t len);

    // ---- Sim-thread bookkeeping ---------------------------------------------
    bool consumeOpenEvent();      // true once per connection, after which attach
    bool consumeCloseEvent();     // true once per teardown, after which detach
    bool inUse() const;           // slot holds a live or not-yet-reaped connection
    void reap();                  // free the slot after detach
    void requestClose();          // sweep/kick: close the socket (evict path)

    bool muted() const { return _muted; }
    uint32_t mutedSinceMs() const { return _mutedSinceMs; }
    uint32_t rxDrops() const { return _rxDrops; }
    std::string peer() const;
    void setNowMsSource(const uint32_t* nowMs) { _nowMs = nowMs; }

private:
    mutable std::mutex _m;             // guards _ws, ring, event flags
    ix::WebSocket* _ws = nullptr;
    std::string _peer;
    bool _inUse = false;
    bool _openEvt = false;
    bool _closeEvt = false;

    slopsync::FrameBuffer _rx[kRxRingDepth]{};
    uint8_t _rxHead = 0, _rxTail = 0, _rxCount = 0;
    uint32_t _rxDrops = 0;

    bool _muted = false;
    uint32_t _mutedSinceMs = 0;
    const uint32_t* _nowMs = nullptr;  // sim-owned ms clock for mute stamps
};

class SlopSimWsPort {
public:
    static constexpr uint8_t kSlots = slopsync::kHubMaxSessions + 1;  // firmware parity

    // Starts the listener. Returns false (with reason logged) on bind failure.
    bool begin(slopsync::Hub* hub, uint16_t port, SessionLog* log);
    void stop();

    // Sim-thread pump: consume open/close events (attach/detach), sweep stalls.
    void loop(uint32_t nowMs);

    // TUI/fault-injection surface.
    struct SlotInfo {
        bool inUse = false;
        std::string peer;
        bool muted = false;
        uint32_t rxDrops = 0;
    };
    SlotInfo slotInfo(uint8_t i) const;
    void kick(uint8_t i);  // fault injection: hard-close one client

private:
    void onMessage(std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws,
                   const ix::WebSocketMessagePtr& msg);

    std::unique_ptr<ix::WebSocketServer> _server;
    slopsync::Hub* _hub = nullptr;
    SessionLog* _log = nullptr;
    SlopSimWsTransport _slots[kSlots];
    bool _attached[kSlots] = {};
    uint32_t _nowMs = 0;

    std::mutex _mapM;  // conn-id -> slot assignment (connection threads race here)
    std::unordered_map<std::string, uint8_t> _slotById;
};

}  // namespace slopsim
