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

    // ---- Activity gate ------------------------------------------------------
    // A client is streamed telemetry/interp/anomaly/status ONLY if it has sent
    // an inbound application frame (clock ping / command) within this window,
    // OR it is the single most-recently-active connected client (which always
    // stays live — see _shouldStream()). A foregrounded tab clock-pings every
    // ~2s; a backgrounded tab's setInterval is throttled to ≥~1/min or paused,
    // so it falls out of the window and is auto-muted — no fan-out cost, no
    // head-of-line stall behind its backed-up socket. It resumes within one
    // tick the moment it's refocused (a ping re-stamps it). Requires ZERO
    // cooperation from the client, so it evicts an already-open forgotten tab
    // running an old bundle. :3
    static constexpr uint32_t CLIENT_ACTIVE_WINDOW_MS = 10000;

    // Public snapshot for the Health-tab client panel (/api/clients).
    struct ClientInfo {
        uint8_t  num;
        bool     connected;
        bool     streaming;    // passes the activity gate this instant
        bool     most_recent;  // the always-live last-active client
        uint32_t idle_ms;      // ms since last inbound application frame
        uint8_t  ip[4];
    };

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

    // ---- Client admin (Health tab) ------------------------------------------
    /// Fill `out` with one ClientInfo per connected client (up to maxOut).
    /// Returns the number written. Thread-safe (takes _wsMutex).
    uint8_t enumerateClients(ClientInfo* out, uint8_t maxOut);
    /// Force-disconnect one client (frees its slot). Returns true if it was
    /// connected. Note: the WebUI reconnects, so this is a deliberate slot
    /// reclaim, not a permanent ban.
    bool kickClient(uint8_t num);

    void onClientConnect();
    void onClientDisconnect();

    // ---- Sender task ---------------------------------------------------------
    static void senderTask(void* arg);

    // ---- OTA safety gate (Core 0) --------------------------------------------
    // Suspend/resume the telemetry+status broadcast task. During an OTA flash
    // write window the sender must be quiesced: it broadcasts from flash-resident
    // code and touches the WS library every ~20ms; a flash-cache stall mid-write
    // causes resets (.clinerules §2 / OTA §2). suspendSender() blocks until the
    // task is actually parked. resumeSender() is a no-op if never suspended. :3
    void suspendSender();
    void resumeSender();

    // ---- 0x01 / 0x02 / 0x04 frame builders ----------------------------------
    void sendTelemetry(uint8_t num);
    void sendStatus(uint8_t num);
    void sendStats(uint8_t num);        // 0x06 STATS — session odometer totals
    void sendInterp(uint8_t num);       // 0x04 INTERP — interpolator debug snapshot
    void broadcastTelemetry();
    void broadcastStatus();
    void broadcastStats();
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

    // Per-client last inbound application-frame timestamp (millis()). 0 = never.
    // Stamped on WStype_CONNECTED and every inbound WStype_BIN — NOT on
    // WStype_PONG (a backgrounded browser auto-pongs at the protocol layer
    // without waking its JS, which is exactly why heartbeat can't reap it and
    // why we must gate on real application traffic instead). :3
    uint32_t _lastInboundMs[MAX_CLIENTS];

    // Per-client send-stall mute. A backed-up socket makes sendBIN return false
    // (after the capped TCP timeout). Rather than DISCONNECT the client (which
    // turned slow-but-alive tabs into a drop→reconnect loop), we just drop it
    // FROM THE STREAM: set _sendStalled and stop sending. The connection stays
    // up. It's un-muted the moment the client sends an inbound frame (its clock
    // ping / a command) — proof it's draining and alive — so a transient blip
    // self-heals within one ping cycle and a genuinely wedged/backgrounded tab
    // stays muted (costing nothing) instead of being dropped. :3
    bool     _sendStalled[MAX_CLIENTS];
    uint32_t _sendStalledSince[MAX_CLIENTS];   // millis() when the stall began

    // A client that has been send-stalled continuously for this long WITHOUT
    // sending any inbound frame is a dead half-open socket (tab closed / network
    // dropped with no FIN/RST). It MUST be reaped: _ws.loop() (in the httpTask)
    // keeps servicing that socket — heartbeat pings + reads block on it for
    // seconds and starve HTTP for every other client. A healthy tab clears its
    // stall via a clock ping every ~2s, well under this, so it's never reaped;
    // a backgrounded tab still drains at the TCP layer so it never stalls at
    // all (just muted by the activity gate). Only true zombies hit this. :3
    static constexpr uint32_t STALL_REAP_MS = 4000;

    // Reap dead half-open clients (see STALL_REAP_MS). Called each sender tick.
    void _reapDeadClients();

    // Guarded sendBIN — the ONE way any frame leaves this class. Checks the
    // result and applies the send-stall mute (the "WS send blocks HTTP mutex"
    // fix) on failure, so a backed-up socket can't re-block the shared mutex on
    // HELLO/STATUS/STATS/INTERP/ANOMALY/CLOCK/ECHO the way it once could on
    // telemetry. Caller MUST hold _wsMutex. Returns sendBIN's verdict. :3
    bool _sendGuarded(uint8_t num, const uint8_t* frame, size_t len, const char* what);

    // Activity gate: true if client `num` should receive the telemetry stream.
    // Recently-active OR the single most-recently-active connected client.
    // Caller MUST hold _wsMutex (touches _ws.clientIsConnected).
    bool _shouldStream(uint8_t num);

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
