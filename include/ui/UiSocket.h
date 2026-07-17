#pragma once

#include <Arduino.h>
#include <functional>
#include <WebSocketsServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "SystemState.h"
#include "config_api.h"
#include "WebUI.h"          // for TelemetrySample + TELEMETRY_RING_SIZE (shared ring)
#include "UiProtocol.h"     // WS_OP_*, WS_IDEMPOTENCY_RING, WS_FRAME_ECHO

// Forward
class MotorDriver;

// ============================================================================
// UiSocket — binary WebSocket UI control plane + telemetry stream
// ============================================================================
//
// Owns a WebSocketsServer on UI_WS_PORT (81).  Now carries both the handshake
// plane (0x00 HELLO, 0x03 CLOCK), the stream plane (0x01 TELEMETRY,
// 0x02 STATUS), and the control plane (0x10 CMD forward → WebUI applyX,
// 0x11 ECHO response with idempotency per-client).

class UiSocket {
public:
    static constexpr size_t HELLO_FRAME_SIZE  = 4;
    static constexpr size_t CLOCK_REPLY_SIZE  = 13;

    // 0x01 header: type(1)+flags(1)+seq(2)+t_dev_us(4)+n(1)+dt_100us(1) = 10
    // plus n*6 per sample (pos_10um:2, tgt_10um:2, raw_10um:2) + i_bus_mA(2)
    static constexpr uint8_t TELE_HEADER_BYTES = 10;
    static constexpr uint8_t TELE_SAMPLE_BYTES = 6;   // 3×u16
    static constexpr uint8_t TELE_TRAILER_BYTES = 2;  // i_bus_mA
    static constexpr uint8_t TELE_MAX_N = 32;
    static constexpr size_t  TELE_MAX_FRAME = TELE_HEADER_BYTES + TELE_MAX_N * TELE_SAMPLE_BYTES + TELE_TRAILER_BYTES;

    static constexpr size_t STATUS_FRAME_SIZE = 28;

    // Per-client read position in the shared telemetry ring.
    static constexpr uint8_t MAX_CLIENTS = 5;

    // ---- Idempotency ring entry (per-client, per-command-id) ----------------
    struct IdemEntry {
        uint16_t id;
        uint8_t  ok;
        uint16_t cfg_gen;
        String   payload;      // JSON of last echo (copied, small — ops are low-rate)
    };

    using FrameHandler = std::function<void(uint8_t num, uint8_t* payload, size_t length)>;

    explicit UiSocket(SystemState& state, uint16_t port = UI_WS_PORT);

    // ---- Lifecycle hooks (.clinerules §4) ------------------------------------
    void init();
    void update();
    void emergencyStop();

    // ---- Wiring — called after construction, before tasks launch -------------
    void setTelemetryRing(TelemetrySample* ring, volatile uint32_t* seq, portMUX_TYPE* mux);
    void setMotorDriver(MotorDriver* motor) { _motor = motor; }
    /// Set the WebUI pointer for 0x10 command dispatch (set AFTER both are constructed).
    void setWebUI(WebUI* webui) { _webui = webui; }

    void sendHello(uint8_t num);

    int connectedClients() { return _ws.connectedClients(); }
    bool clientIsConnected(uint8_t num) { return _ws.clientIsConnected(num); }
    void onFrame(FrameHandler handler) { _frameHandler = handler; }

    void onClientConnect();
    void onClientDisconnect();

    // ---- Sender task ---------------------------------------------------------
    static void senderTask(void* arg);

    // ---- 0x01 / 0x02 / 0x04 frame builders ----------------------------------
    void sendTelemetry(uint8_t num);
    void sendStatus(uint8_t num);
    void sendInterp(uint8_t num);       // 0x04 INTERP — interpolator debug snapshot
    void broadcastTelemetry();
    void broadcastStatus();
    void broadcastInterp();

    // ---- 0x05 ANOMALY — drain the cross-core anomaly ring, send one frame per
    // event to a single client. Event-driven: only emits when new anomalies are
    // present. broadcastAnomaly() drains ONCE and fans the same events to all
    // connected clients so an event isn't consumed by only the first client.
    void broadcastAnomaly();

    // ---- 0x11 ECHO + idempotency (control plane) ------------------------------
    /// Send a 0x11 ECHO frame. Returns true if sent successfully.
    bool sendEcho(uint8_t num, uint16_t id, uint8_t ok, const String& json_payload);
    /// Check idempotency ring for dup; if found, re-send last echo and return true.
    /// Otherwise insert this (id, ok, payload) into the ring and return false.
    bool checkOrInsertIdem(uint8_t num, uint16_t id, uint8_t ok, const String& payload);

private:
    SystemState&  _state;
    uint16_t      _port;
    WebSocketsServer _ws;
    FrameHandler  _frameHandler = nullptr;

    // Shared telemetry ring (owned by WebUI — we drain, never write)
    TelemetrySample*   _teleRing = nullptr;
    volatile uint32_t* _teleSeq = nullptr;
    portMUX_TYPE*      _teleMux = nullptr;

    // Motor driver for i_bus_mA + measured_stroke (nullable)
    MotorDriver* _motor = nullptr;

    // WebUI pointer for 0x10 CMD dispatch (set in main.cpp after construction)
    WebUI*       _webui = nullptr;

    // Per-client read head
    uint32_t _clientHead[MAX_CLIENTS];

    // Sender task state
    TaskHandle_t _senderHandle = nullptr;
    uint32_t     _tx_drops = 0;
    uint32_t     _lastStatusMs = 0;
    uint16_t     _teleFrameSeq = 0;

    // ---- Idempotency ring (per-client) — ring of last 32 seen ids -----------
    IdemEntry _idemRing[MAX_CLIENTS][WS_IDEMPOTENCY_RING];
    uint8_t   _idemWrite[MAX_CLIENTS];   // next write index per client
    uint8_t   _idemCount[MAX_CLIENTS];   // how many entries currently valid (0..32)

    // ---- Thread safety --------------------------------------------------------
    // WebSocketsServer (Links2004/arduinoWebSockets) is NOT thread-safe. Two
    // FreeRTOS tasks touch _ws concurrently:
    //   - httpTask (Core 0, prio 1) via update() -> _ws.loop()  (handles
    //     incoming frames, dispatches CMD, sends HELLO/ECHO synchronously)
    //   - senderTask (Core 0, prio 3 — HIGHER, can preempt httpTask mid-loop())
    //     via broadcastTelemetry()/broadcastStatus() -> _ws.sendBIN()
    // Without a lock, senderTask can preempt httpTask mid-send/mid-parse,
    // corrupting the library's internal per-client TCP framing state — this
    // was the root cause of the random disconnects/reconnects, flickering
    // home/homed flag, and intermittent transport-mode/HELLO handshake
    // failures. A RECURSIVE mutex is required because _handleEvent() (called
    // from inside _ws.loop(), i.e. already holding the lock) itself calls
    // sendHello()/sendEcho(), which must re-acquire the same lock. :3
    SemaphoreHandle_t _wsMutex = nullptr;

    // RAII helper — lock/unlock the recursive mutex around any _ws access.
    struct WsLock {
        SemaphoreHandle_t m;
        explicit WsLock(SemaphoreHandle_t mtx) : m(mtx) { if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY); }
        ~WsLock() { if (m) xSemaphoreGiveRecursive(m); }
        WsLock(const WsLock&) = delete;
        WsLock& operator=(const WsLock&) = delete;
    };

    void _handleEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
};
