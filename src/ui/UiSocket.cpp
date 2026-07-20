#include "UiSocket.h"
#include "AppLog.h"
#include <ArduinoJson.h>
#include <esp_timer.h>
#include <WiFi.h>
#include "MotorDriver.h"

// 240Hz → dt in 100µs units = round(4166/100) = 42
#define TELE_DT_100US 42

// ============================================================================
// Constructor
// ============================================================================

UiSocket::UiSocket(SystemState& state, uint16_t port)
    : _state(state), _port(port), _ws(port)
{
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        _clientHead[i] = 0;
        _idemWrite[i] = 0;
        _idemCount[i] = 0;
        _lastInboundMs[i] = 0;
        _sendStalled[i] = false;
        _sendStalledSince[i] = 0;
    }
    // Recursive mutex: httpTask (update()->_ws.loop()) and senderTask
    // (broadcastTelemetry/Status) both touch the non-thread-safe
    // WebSocketsServer instance. Recursive because _handleEvent() runs
    // synchronously INSIDE _ws.loop() (already holding the lock) and itself
    // calls sendHello()/sendEcho(), which re-acquire it. :3
    _wsMutex = xSemaphoreCreateRecursiveMutex();
}

// ============================================================================
// Wiring
// ============================================================================

void UiSocket::setTelemetryRing(TelemetrySample* ring, volatile uint32_t* seq, portMUX_TYPE* mux) {
    _teleRing = ring;
    _teleSeq  = seq;
    _teleMux  = mux;
}

// ============================================================================
// init() — start WS server, spawn sender task
// ============================================================================

static void uiStreamTask(void* arg) { UiSocket::senderTask(arg); }

void UiSocket::init() {
    // Power-save is permanently disabled for this device (TransportManager::
    // setupWiFi() already calls WiFi.setSleep(false) at boot — it's plugged
    // into a wall brick, never battery, so there's zero reason to ever touch
    // WIFI_PS_* modes here). Previously this task toggled WIFI_PS_NONE on
    // client connect and restored the "saved" mode on disconnect — an
    // entirely pointless (and mildly risky, since PS-mode toggling can add
    // WiFi radio jitter/latency spikes) dance now removed. :3
    APPLOGF("[UiSocket] init on port %u", _port);

    WsLock lock(_wsMutex);
    _ws.begin();
    // Lenient heartbeat (original values). An aggressive heartbeat was dropping
    // slow-but-alive clients on WiFi/Tailscale jitter — the exact "connections
    // keep dropping" regression. Dead-socket reaping isn't urgent: a wedged or
    // backgrounded client is already muted by the activity gate / send-stall
    // mute (_shouldStream), so it costs nothing while it lingers. :3
    _ws.enableHeartbeat(15000, 5000, 3);

    _ws.onEvent([this](uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
        this->_handleEvent(num, type, payload, length);
    });

    // Spawn sender task pinned to Core 0, priority 3 (above httpTask=1).
    // Stack is 10240 (not the old 4096): this task now also runs _ws.loop() —
    // moved off httpTask so a wedged client can't freeze the heartbeat/HTTP —
    // which services inbound WS commands (512-byte JSON buffer + deserializeJson
    // + sendEcho). That handling used to live in httpTask's 8192 stack; on the
    // old 4096 sender stack it overflowed and crash-looped the device. :3
    xTaskCreatePinnedToCore(
        uiStreamTask, "UiStream", 10240, this, 3, &_senderHandle, 0);

    APPLOG("[UiSocket] ready — ws://<ip>:81/ + sender task on Core 0 prio 3");
}

// ============================================================================
// update() — service the WS server loop (non-blocking)
// ============================================================================

void UiSocket::update() {
    WsLock lock(_wsMutex);
    _ws.loop();
}

// ============================================================================
// emergencyStop()
// ============================================================================

void UiSocket::emergencyStop() {
    APPLOG("[UiSocket] emergencyStop — disconnecting all UI WS clients");
    WsLock lock(_wsMutex);
    _ws.disconnect();
}

// ============================================================================
// _sendGuarded — checked sendBIN + send-stall mute (caller holds _wsMutex)
// ============================================================================
//
// The "WS send blocks HTTP mutex" regression fix, applied to EVERY frame type.
// Previously only sendTelemetry checked its sendBIN result and muted the
// stalled client; every other frame (HELLO/STATUS/STATS/INTERP/ANOMALY/CLOCK/
// ECHO) ignored it, so a backed-up socket reproduced the exact mutex stall on
// those paths. One failed send → mute (kept connected) → the client earns the
// stream back with an inbound frame, or gets reaped by _reapDeadClients(). :3

bool UiSocket::_sendGuarded(uint8_t num, const uint8_t* frame, size_t len, const char* what) {
    bool ok = _ws.sendBIN(num, const_cast<uint8_t*>(frame), len);
    if (!ok && num < MAX_CLIENTS) {
        _tx_drops++;
        if (!_sendStalled[num]) {
            _sendStalled[num] = true;
            _sendStalledSince[num] = millis();
            APPLOGF("[UiSocket] client#%u %s send stalled — muting stream (kept connected)", num, what);
        }
    }
    return ok;
}

// ============================================================================
// HELLO
// ============================================================================

void UiSocket::sendHello(uint8_t num) {
    uint8_t frame[HELLO_FRAME_SIZE];
    frame[0] = 0x00;
    frame[1] = 1;  // proto_ver
    uint16_t gen = _state.cfg_gen.load(std::memory_order_relaxed);
    frame[2] = (uint8_t)(gen & 0xFF);
    frame[3] = (uint8_t)(gen >> 8);
    WsLock lock(_wsMutex);
    _sendGuarded(num, frame, HELLO_FRAME_SIZE, "HELLO");
}

// ============================================================================
// Power save
// ============================================================================

void UiSocket::onClientConnect() {
    int count;
    { WsLock lock(_wsMutex); count = _ws.connectedClients(); }
    APPLOGF("[UiSocket] client connected (total=%d)", count);
}

void UiSocket::onClientDisconnect() {
    int count;
    { WsLock lock(_wsMutex); count = _ws.connectedClients(); }
    APPLOGF("[UiSocket] client disconnected (total=%d)", count);
}

// ============================================================================
// sendEcho — construct and send a 0x11 ECHO frame
// ============================================================================
// Frame: u8 type=0x11, u16 id, u8 ok, u16 cfg_gen, payload=JSON (UTF-8)

bool UiSocket::sendEcho(uint8_t num, uint16_t id, uint8_t ok, const String& json_payload) {
    // Header: 1 + 2 + 1 + 2 = 6 bytes + JSON
    size_t payload_len = json_payload.length();
    size_t frame_len = 6 + payload_len;
    // Stack-allocate for small payloads (most echoes are < 256 bytes)
    uint8_t stack_buf[320];
    uint8_t* buf = (frame_len <= sizeof(stack_buf)) ? stack_buf : new uint8_t[frame_len];

    size_t off = 0;
    buf[off++] = WS_FRAME_ECHO;    // 0x11
    buf[off++] = (uint8_t)(id & 0xFF);
    buf[off++] = (uint8_t)(id >> 8);
    buf[off++] = ok;
    uint16_t gen = _state.cfg_gen.load(std::memory_order_relaxed);
    buf[off++] = (uint8_t)(gen & 0xFF);
    buf[off++] = (uint8_t)(gen >> 8);
    memcpy(buf + off, json_payload.c_str(), payload_len);
    off += payload_len;

    bool sent;
    { WsLock lock(_wsMutex); sent = _sendGuarded(num, buf, off, "ECHO"); }
    if (buf != stack_buf) delete[] buf;
    if (!sent) {
        // The command was already APPLIED by the time the echo goes out — an
        // ack lost in flight must be distinguishable from an ack delivered,
        // or the client's pending-state machine waits forever in silence. The
        // idempotency ring will re-echo if the client retries the same id. :3
        APPLOGF("[UiSocket] ECHO id=%u to client#%u FAILED to send — command applied, ack lost", id, num);
    }
    return sent;
}

// ============================================================================
// checkOrInsertIdem — idempotency ring per client
// ============================================================================
// Returns true if the id was a duplicate (re-echo sent). Returns false if
// this is a new id (we inserted the entry; caller must apply the command).

bool UiSocket::checkOrInsertIdem(uint8_t num, uint16_t id, uint8_t ok, const String& payload) {
    if (num >= MAX_CLIENTS) return false;

    uint8_t count = _idemCount[num];
    uint8_t write = _idemWrite[num];

    // Search for existing entry with this id
    for (uint8_t i = 0; i < count; i++) {
        uint8_t idx = (write - count + i) % WS_IDEMPOTENCY_RING;  // oldest→newest
        if (_idemRing[num][idx].id == id) {
            // Duplicate — re-send last echo, do NOT re-apply
            sendEcho(num, _idemRing[num][idx].id,
                     _idemRing[num][idx].ok,
                     _idemRing[num][idx].payload);
            APPLOGF("[UiSocket] idem HIT client#%u id=%u — re-echoed, NOT re-applied", num, id);
            return true;
        }
    }

    // New id — insert at write position, advance ring
    _idemRing[num][write].id = id;
    _idemRing[num][write].ok = ok;
    _idemRing[num][write].cfg_gen = _state.cfg_gen.load(std::memory_order_relaxed);
    _idemRing[num][write].payload = payload;
    _idemWrite[num] = (write + 1) % WS_IDEMPOTENCY_RING;
    if (count < WS_IDEMPOTENCY_RING) _idemCount[num] = count + 1;

    return false;
}

// ============================================================================
// _handleEvent
// ============================================================================

void UiSocket::_handleEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    // This callback fires synchronously from inside _ws.loop() (called by
    // update(), which already holds _wsMutex). Locking again here is a
    // cheap no-op recursion, but it also makes _handleEvent's own direct
    // _ws.sendBIN() (CLOCK reply below) and its calls into sendHello()/
    // sendEcho() safe to call independently of the caller's lock state.
    WsLock lock(_wsMutex);
    switch (type) {
    case WStype_CONNECTED: {
        IPAddress ip = _ws.remoteIP(num);
        APPLOGF("[UiSocket] CONNECTED client#%u from %s", num, ip.toString().c_str());
        onClientConnect();
        if (_teleSeq && num < MAX_CLIENTS) {
            _clientHead[num] = *_teleSeq;
        }
        // Do NOT stamp activity on connect. A freshly-(re)connected client
        // must EARN the stream by sending an inbound frame (its clock ping,
        // ~2s after open). This is the fix for the hang: after a reboot every
        // tab — including a slow/backgrounded/remote zombie — reconnects at
        // once; if connect counted as activity they'd all be streamed for the
        // window, and one wedged socket stalls the shared WS mutex → HTTP
        // hangs. A backgrounded tab won't ping, so it stays muted and never
        // enters the send loop. Cost: ~2s to first telemetry on a fresh open.
        if (num < MAX_CLIENTS) { _lastInboundMs[num] = 0; _sendStalled[num] = false; _sendStalledSince[num] = 0; }
        // Reset idempotency ring for new client
        _idemWrite[num] = 0;
        _idemCount[num] = 0;
        sendHello(num);
        break;
    }
    case WStype_DISCONNECTED:
        APPLOGF("[UiSocket] DISCONNECTED client#%u", num);
        if (num < MAX_CLIENTS) { _lastInboundMs[num] = 0; _sendStalled[num] = false; _sendStalledSince[num] = 0; }
        onClientDisconnect();
        break;
    case WStype_BIN: {
        if (length < 1) return;
        // Inbound application traffic = this client is live and draining. Stamp
        // activity (re-activates a refocused tab) AND clear any send-stall mute:
        // the client just proved it's alive, so give it the stream back (and
        // cancel any pending dead-client reap).
        if (num < MAX_CLIENTS) { _lastInboundMs[num] = millis(); _sendStalled[num] = false; _sendStalledSince[num] = 0; }
        uint8_t ft = payload[0];
        if (ft == 0x03 && length >= 5) {
            // CLOCK
            uint32_t t0 = (uint32_t)payload[1] | ((uint32_t)payload[2] << 8)
                        | ((uint32_t)payload[3] << 16) | ((uint32_t)payload[4] << 24);
            uint32_t t1 = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFu);
            uint8_t r[CLOCK_REPLY_SIZE];
            r[0] = 0x03;
            r[1] = (uint8_t)(t0 & 0xFF); r[2] = (uint8_t)((t0 >> 8) & 0xFF);
            r[3] = (uint8_t)((t0 >> 16) & 0xFF); r[4] = (uint8_t)((t0 >> 24) & 0xFF);
            r[5] = (uint8_t)(t1 & 0xFF); r[6] = (uint8_t)((t1 >> 8) & 0xFF);
            r[7] = (uint8_t)((t1 >> 16) & 0xFF); r[8] = (uint8_t)((t1 >> 24) & 0xFF);
            uint32_t t2 = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFu);
            r[9] = (uint8_t)(t2 & 0xFF); r[10] = (uint8_t)((t2 >> 8) & 0xFF);
            r[11] = (uint8_t)((t2 >> 16) & 0xFF); r[12] = (uint8_t)((t2 >> 24) & 0xFF);
            _sendGuarded(num, r, CLOCK_REPLY_SIZE, "CLOCK");  // already under WsLock from top of _handleEvent

        } else if (ft == WS_FRAME_CMD && _webui && length >= 4) {
            // 0x10 CMD: u8 type, u16 id (LE), u8 op, remaining = JSON payload (UTF-8)
            uint16_t cmd_id = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
            uint8_t  op     = payload[3];

            // Parse JSON payload (bytes 4..end as UTF-8 null-terminated string)
            size_t json_len = length - 4;
            char json_buf[512];
            if (json_len >= sizeof(json_buf)) json_len = sizeof(json_buf) - 1;
            memcpy(json_buf, payload + 4, json_len);
            json_buf[json_len] = '\0';

            JsonDocument payload_in;
            DeserializationError err = deserializeJson(payload_in, json_buf);

            if (err) {
                // Bad JSON — echo failure
                String fail_json = "{\"ok\":false,\"error\":\"Invalid JSON\"}";
                checkOrInsertIdem(num, cmd_id, 0, fail_json);
                sendEcho(num, cmd_id, 0, fail_json);
                APPLOGF("[UiSocket] CMD client#%u id=%u op=0x%02X — JSON parse FAILED", num, cmd_id, op);
                break;
            }

            // Dispatch to WebUI handleCommand (synchronous, non-blocking — all setters
            // are simple variable writes / FastAccelStepper setSpeed etc, same as HTTP).
            JsonDocument payload_out;
            bool ok = _webui->handleCommand(op, payload_in, payload_out);
            payload_out["ok"] = ok;  // ensure ok is always present

            String echo_json;
            serializeJson(payload_out, echo_json);

            uint8_t echo_ok = ok ? 1 : 0;

            // Check idempotency — if duplicate, just re-echo; else insert + send echo
            if (!checkOrInsertIdem(num, cmd_id, echo_ok, echo_json)) {
                // New command — already applied by handleCommand; send echo
                sendEcho(num, cmd_id, echo_ok, echo_json);
            }

            APPLOGF("[UiSocket] CMD client#%u id=%u op=0x%02X ok=%d", num, cmd_id, op, ok);

        } else if (ft == 0x10 && _frameHandler) {
            // Legacy fallback — if _webui not set and frameHandler is wired
            _frameHandler(num, payload, length);
        } else if (ft == WS_FRAME_CMD) {
            // CMD frame too short (< 4 bytes) or arrived before the WebUI
            // handler was wired — previously this fell through BOTH branches
            // with total silence. Tell the client (when the id is readable)
            // and leave a log trace either way. :3
            if (length >= 3) {
                uint16_t cmd_id = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
                sendEcho(num, cmd_id, 0, "{\"ok\":false,\"error\":\"Malformed CMD frame\"}");
            }
            APPLOGF("[UiSocket] CMD client#%u DROPPED: len=%u webui_wired=%d — malformed or too early",
                    num, (unsigned)length, _webui != nullptr);
        } else {
            APPLOGF("[UiSocket] unknown inbound frame 0x%02X len=%u from client#%u — dropped",
                    ft, (unsigned)length, num);
        }
        break;
    }
    case WStype_ERROR:
        // The WS library reported a socket-level error for this client — the
        // one event type that must never vanish without a trace. :3
        APPLOGF("[UiSocket] WS ERROR client#%u (payload len=%u)", num, (unsigned)length);
        break;
    case WStype_PING:
    case WStype_PONG:
        break;   // library heartbeat — expected, uninteresting
    default:
        APPLOGF("[UiSocket] unhandled WS event type=%d client#%u len=%u",
                (int)type, num, (unsigned)length);
        break;
    }
}

// ============================================================================
// Helper: clamp float mm to u16 0.01mm units (0..65535 → 0..655.35mm)
// ============================================================================

static inline uint16_t mmToU10(float mm) {
    if (mm <= 0.0f) return 0;
    if (mm >= 655.35f) return 65535;
    return (uint16_t)(mm * 100.0f + 0.5f);
}

// ============================================================================
// sendTelemetry(num) — drain ring for one client, send 0x01 frame
// ============================================================================

void UiSocket::sendTelemetry(uint8_t num) {
    if (!_teleRing || !_teleSeq || !_teleMux) return;
    if (num >= MAX_CLIENTS) return;
    WsLock lock(_wsMutex);
    if (!_ws.clientIsConnected(num)) return;

    uint32_t head, start;
    TelemetrySample batch[TELE_MAX_N];
    uint8_t n = 0;
    uint32_t last_t_dev = 0;

    portENTER_CRITICAL_ISR(_teleMux);
    head = *_teleSeq;
    start = _clientHead[num];
    if (head > start + TELEMETRY_RING_SIZE) start = head - TELEMETRY_RING_SIZE;
    for (uint32_t s = start; s < head && n < TELE_MAX_N; s++) {
        batch[n] = _teleRing[s % TELEMETRY_RING_SIZE];
        last_t_dev = batch[n].t_dev_us;
        n++;
    }
    _clientHead[num] = head;
    portEXIT_CRITICAL_ISR(_teleMux);

    if (n == 0) return;

    uint8_t frame[TELE_MAX_FRAME];
    size_t off = 0;

    frame[off++] = 0x01;

    uint8_t flags = 0;
    if (_state.homed)             flags |= (1 << 0);
    if (_state.gen_active)       flags |= (1 << 1);  // pattern/generator _running flag (PB-004)
    if (_state.gen_active)       flags |= (1 << 2);  // gen_active (emitting) — keep for compat
    // bit3: e-stopped. estop_requested is a transient request consumed by
    // motorTask within ~1ms — sampled at ~45Hz it would almost never be seen.
    // estop_latched is the durable "device is e-stopped" state (cleared when a
    // new homing cycle starts), so the UI fault banner can rise from truth. :3
    if (_state.estop_requested.load() || _state.estop_latched)  flags |= (1 << 3);
    if (_state.paused)           flags |= (1 << 4);
    if (_state.manual_override)  flags |= (1 << 5);
    // bit 6: FLAG_INTIFACE_ACTIVE — set when Intiface was recently active
    if (_state.last_intiface_ms != 0 && (millis() - _state.last_intiface_ms < 250))
        flags |= (1 << 6);
    frame[off++] = flags;

    frame[off++] = (uint8_t)(_teleFrameSeq & 0xFF);
    frame[off++] = (uint8_t)(_teleFrameSeq >> 8);
    _teleFrameSeq++;

    frame[off++] = (uint8_t)(last_t_dev & 0xFF);
    frame[off++] = (uint8_t)((last_t_dev >> 8) & 0xFF);
    frame[off++] = (uint8_t)((last_t_dev >> 16) & 0xFF);
    frame[off++] = (uint8_t)((last_t_dev >> 24) & 0xFF);

    frame[off++] = n;
    frame[off++] = TELE_DT_100US;

    for (uint8_t i = 0; i < n; i++) {
        uint16_t p = mmToU10(batch[i].position_mm);
        uint16_t t = mmToU10(batch[i].target_mm);
        uint16_t r = mmToU10(batch[i].raw_mm);
        frame[off++] = (uint8_t)(p & 0xFF); frame[off++] = (uint8_t)(p >> 8);
        frame[off++] = (uint8_t)(t & 0xFF); frame[off++] = (uint8_t)(t >> 8);
        frame[off++] = (uint8_t)(r & 0xFF); frame[off++] = (uint8_t)(r >> 8);
    }

    uint16_t i_mA = 0;
    if (_motor) {
        float a = _motor->getBusCurrentA();
        float ma = fabsf(a) * 1000.0f;
        i_mA = (ma > 65535.0f) ? 65535 : (uint16_t)ma;
    }
    frame[off++] = (uint8_t)(i_mA & 0xFF);
    frame[off++] = (uint8_t)(i_mA >> 8);

    // A backed-up socket returns false after the (capped) TCP timeout.
    // _sendGuarded mutes this client from the stream — does NOT disconnect it.
    // Muting stops the per-tick mutex stall immediately (one failed send, not a
    // loop), keeps the connection alive (no drop→reconnect churn), and lets the
    // client earn the stream back by sending an inbound frame once it has
    // drained (handled in _handleEvent), or be reaped if it never does
    // (_reapDeadClients). :3
    _sendGuarded(num, frame, off, "TELEMETRY");
}

// ============================================================================
// sendStatus(num) — 0x02 STATUS frame, Appendix C
// ============================================================================

void UiSocket::sendStatus(uint8_t num) {
    WsLock lock(_wsMutex);
    if (!_ws.clientIsConnected(num)) return;

    uint8_t frame[STATUS_FRAME_SIZE];
    size_t off = 0;
    frame[off++] = 0x02;

    uint16_t bus_mV = 0;
    if (_motor) {
        float v = _motor->getBusVoltageV();
        bus_mV = (uint16_t)(v * 1000.0f + 0.5f);
    }
    frame[off++] = (uint8_t)(bus_mV & 0xFF);
    frame[off++] = (uint8_t)(bus_mV >> 8);

    int16_t die_c10 = 0;
    if (_motor && _motor->hasPowerMonitor()) {
        die_c10 = (int16_t)(_motor->getDieTempC() * 10.0f + 0.5f);
    }
    frame[off++] = (uint8_t)(die_c10 & 0xFF);
    frame[off++] = (uint8_t)(die_c10 >> 8);

    uint16_t peak_mA = 0;
    if (_motor) {
        float pa = _motor->getPeakBusCurrentA();
        peak_mA = (uint16_t)(fabsf(pa) * 1000.0f + 0.5f);
    }
    frame[off++] = (uint8_t)(peak_mA & 0xFF);
    frame[off++] = (uint8_t)(peak_mA >> 8);

    frame[off++] = (int8_t)WiFi.RSSI();
    frame[off++] = (uint8_t)_state.wifi_channel;

    uint16_t reconn = (uint16_t)_state.wifi_reconnects;
    frame[off++] = (uint8_t)(reconn & 0xFF);
    frame[off++] = (uint8_t)(reconn >> 8);

    uint16_t drops = (uint16_t)_tx_drops;
    frame[off++] = (uint8_t)(drops & 0xFF);
    frame[off++] = (uint8_t)(drops >> 8);

    uint32_t heap = ESP.getFreeHeap();
    frame[off++] = (uint8_t)(heap & 0xFF);
    frame[off++] = (uint8_t)((heap >> 8) & 0xFF);
    frame[off++] = (uint8_t)((heap >> 16) & 0xFF);
    frame[off++] = (uint8_t)((heap >> 24) & 0xFF);

    uint16_t cfg = _state.cfg_gen.load(std::memory_order_relaxed);
    frame[off++] = (uint8_t)(cfg & 0xFF);
    frame[off++] = (uint8_t)(cfg >> 8);

    frame[off++] = (uint8_t)_state.transport;

    uint32_t stroke_10um = 0;
    if (_motor) {
        float ms = _motor->getMeasuredStrokeMm();
        stroke_10um = (ms > 0.0f) ? (uint32_t)(ms * 100.0f + 0.5f)
                                   : (uint32_t)(_motor->getMaxRailMm() * 100.0f + 0.5f);
    } else {
        stroke_10um = (uint32_t)(DEFAULT_MAX_RAIL_MM * 100.0f + 0.5f);
    }
    frame[off++] = (uint8_t)(stroke_10um & 0xFF);
    frame[off++] = (uint8_t)((stroke_10um >> 8) & 0xFF);
    frame[off++] = (uint8_t)((stroke_10um >> 16) & 0xFF);
    frame[off++] = (uint8_t)((stroke_10um >> 24) & 0xFF);

    IPAddress ip = WiFi.localIP();
    frame[off++] = ip[0];
    frame[off++] = ip[1];
    frame[off++] = ip[2];
    frame[off++] = ip[3];

    _sendGuarded(num, frame, STATUS_FRAME_SIZE, "STATUS");
}

// ============================================================================
// sendStats(num) — 0x06 STATS frame, session odometer totals (~2Hz)
// ============================================================================
// Layout in UiProtocol.h (STATS_FRAME_SIZE = 19). Slow-changing session
// totals for the dashboard SESSION card. Live current speed is NOT here — the
// hero numeral derives that from the telemetry stream for a snappier readout.

void UiSocket::sendStats(uint8_t num) {
    WsLock lock(_wsMutex);
    if (!_ws.clientIsConnected(num)) return;

    uint8_t frame[STATS_FRAME_SIZE];
    size_t off = 0;
    frame[off++] = WS_FRAME_STATS;   // 0x06

    auto putU16 = [&](uint32_t v) {
        if (v > 65535) v = 65535;
        frame[off++] = (uint8_t)(v & 0xFF); frame[off++] = (uint8_t)(v >> 8);
    };
    auto putU32 = [&](uint32_t v) {
        frame[off++] = (uint8_t)(v & 0xFF);        frame[off++] = (uint8_t)((v >> 8) & 0xFF);
        frame[off++] = (uint8_t)((v >> 16) & 0xFF); frame[off++] = (uint8_t)((v >> 24) & 0xFF);
    };

    float    wh         = _motor ? _motor->getBusEnergyWh() : 0.0f;
    uint16_t max_spd    = (uint16_t)(_state.max_speed_mm_s.load(std::memory_order_relaxed) + 0.5f);
    uint32_t dist_mm    = (uint32_t)(_state.session_distance_mm.load(std::memory_order_relaxed) + 0.5f);
    uint32_t energy_mwh = (wh > 0.0f) ? (uint32_t)(wh * 1000.0f + 0.5f) : 0;
    uint32_t strokes    = _state.stroke_count.load(std::memory_order_relaxed);
    uint32_t session_ms = (uint32_t)(millis() - _state.session_start_ms);

    putU16(max_spd);
    putU32(dist_mm);
    putU32(energy_mwh);
    putU32(strokes);
    putU32(session_ms);

    _sendGuarded(num, frame, STATS_FRAME_SIZE, "STATS");
}

// ============================================================================
// sendInterp(num) — 0x04 INTERP frame, v0.4 interpolator debug snapshot
// ============================================================================
//
// Layout defined in UiProtocol.h (INTERP_FRAME_SIZE = 19 bytes). All fields
// are read straight from SystemState (published by Core 1's streamSamplerTask).
// A torn read across the individual scalars is visually harmless at UI rates.

void UiSocket::sendInterp(uint8_t num) {
    WsLock lock(_wsMutex);
    if (!_ws.clientIsConnected(num)) return;

    uint8_t frame[INTERP_FRAME_SIZE];
    size_t off = 0;
    frame[off++] = WS_FRAME_INTERP;   // 0x04

    uint8_t flags = 0;
    if (_state.interp_active)    flags |= (1 << 0);
    if (_state.interp_live_mode) flags |= (1 << 1);
    if (_state.interp_grad_mode) flags |= (1 << 2);
    frame[off++] = flags;
    frame[off++] = _state.interp_style;

    auto putU16 = [&](float norm) {
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        uint16_t v = (uint16_t)(norm * 10000.0f + 0.5f);
        frame[off++] = (uint8_t)(v & 0xFF);
        frame[off++] = (uint8_t)(v >> 8);
    };
    putU16(_state.interp_start_pos);
    putU16(_state.interp_end_pos);
    putU16(_state.interp_cur_pos);

    // Signed velocity: units/second * 1000, clamped to i16 range.
    float velq = _state.interp_cur_vel * 1000.0f;
    if (velq >  32767.0f) velq =  32767.0f;
    if (velq < -32768.0f) velq = -32768.0f;
    int16_t vi = (int16_t)(velq >= 0.0f ? velq + 0.5f : velq - 0.5f);
    frame[off++] = (uint8_t)((uint16_t)vi & 0xFF);
    frame[off++] = (uint8_t)((uint16_t)vi >> 8);

    uint32_t dur = _state.interp_duration_us;
    frame[off++] = (uint8_t)(dur & 0xFF);
    frame[off++] = (uint8_t)((dur >> 8) & 0xFF);
    frame[off++] = (uint8_t)((dur >> 16) & 0xFF);
    frame[off++] = (uint8_t)((dur >> 24) & 0xFF);

    uint32_t ela = _state.interp_elapsed_us;
    frame[off++] = (uint8_t)(ela & 0xFF);
    frame[off++] = (uint8_t)((ela >> 8) & 0xFF);
    frame[off++] = (uint8_t)((ela >> 16) & 0xFF);
    frame[off++] = (uint8_t)((ela >> 24) & 0xFF);

    _sendGuarded(num, frame, INTERP_FRAME_SIZE, "INTERP");
}

// ============================================================================
// _shouldStream — the activity gate
// ============================================================================
//
// Returns true if client `num` should receive the telemetry stream this tick:
//   (a) it sent an inbound application frame within CLIENT_ACTIVE_WINDOW_MS, OR
//   (b) it is the single most-recently-active connected client — which ALWAYS
//       stays live, even when quiet, so the last tab you actually touched keeps
//       working in the background until a NEWER client takes over.
// Muted clients (neither) get zero sends, so a backed-up/forgotten socket can't
// stall the fan-out for everyone else. Caller MUST hold _wsMutex. :3

bool UiSocket::_shouldStream(uint8_t num) {
    if (num >= MAX_CLIENTS) return false;
    if (!_ws.clientIsConnected(num)) return false;

    // Send-stalled clients are muted (kept connected) until they prove they're
    // draining via an inbound frame — overrides even the most-recent rule so a
    // wedged last-active tab can't keep stalling the shared mutex.
    if (_sendStalled[num]) return false;

    uint32_t now = millis();

    // (a) recently active
    if (_lastInboundMs[num] != 0 && (now - _lastInboundMs[num]) < CLIENT_ACTIVE_WINDOW_MS)
        return true;

    // (b) the single most-recently-active connected client (wrap-safe compare)
    uint8_t  best  = 0xFF;
    uint32_t bestT = 0;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!_ws.clientIsConnected(i)) continue;
        if (_lastInboundMs[i] == 0) continue;
        if (best == 0xFF || (int32_t)(_lastInboundMs[i] - bestT) > 0) {
            best = i;
            bestT = _lastInboundMs[i];
        }
    }
    return (best == num);
}

// ============================================================================
// _reapDeadClients — kill half-open zombie sockets before they starve HTTP
// ============================================================================
//
// A client that has been send-stalled continuously for STALL_REAP_MS without a
// single inbound frame is a dead half-open TCP connection (tab closed / network
// dropped with no FIN/RST). If left connected, _ws.loop() — which runs in the
// httpTask — keeps servicing it: heartbeat pings and reads block on the dead
// socket for seconds every iteration, starving HTTP for EVERY other client (the
// "page takes forever to load" bug). We can't rely on the WS heartbeat to reap
// it, because the heartbeat runs inside that same blocked _ws.loop(). So the
// sender task reaps it directly. Healthy tabs clear their stall via a clock
// ping every ~2s (< STALL_REAP_MS) and are never reaped. :3

void UiSocket::_reapDeadClients() {
    WsLock lock(_wsMutex);
    uint32_t now = millis();
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!_ws.clientIsConnected(i)) continue;
        if (!_sendStalled[i]) continue;
        if ((now - _sendStalledSince[i]) < STALL_REAP_MS) continue;
        APPLOGF("[UiSocket] client#%u dead (stalled %ums, no inbound) — reaping half-open socket", i, now - _sendStalledSince[i]);
        _sendStalled[i] = false;
        _sendStalledSince[i] = 0;
        _lastInboundMs[i] = 0;
        _ws.disconnect(i);
    }
}

// ============================================================================
// enumerateClients / kickClient — Health-tab admin
// ============================================================================

uint8_t UiSocket::enumerateClients(ClientInfo* out, uint8_t maxOut) {
    if (!out || maxOut == 0) return 0;
    WsLock lock(_wsMutex);
    uint32_t now = millis();
    uint8_t written = 0;
    for (uint8_t i = 0; i < MAX_CLIENTS && written < maxOut; i++) {
        if (!_ws.clientIsConnected(i)) continue;
        ClientInfo& c = out[written];
        c.num         = i;
        c.connected   = true;
        c.streaming   = _shouldStream(i);
        c.idle_ms     = (_lastInboundMs[i] == 0) ? 0 : (now - _lastInboundMs[i]);
        IPAddress ip  = _ws.remoteIP(i);
        c.ip[0] = ip[0]; c.ip[1] = ip[1]; c.ip[2] = ip[2]; c.ip[3] = ip[3];
        c.most_recent = false;  // filled below
        written++;
    }
    // Mark the single most-recently-active as most_recent (for the UI badge).
    uint8_t  best = 0xFF; uint32_t bestT = 0;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!_ws.clientIsConnected(i) || _lastInboundMs[i] == 0) continue;
        if (best == 0xFF || (int32_t)(_lastInboundMs[i] - bestT) > 0) { best = i; bestT = _lastInboundMs[i]; }
    }
    for (uint8_t w = 0; w < written; w++) if (out[w].num == best) out[w].most_recent = true;
    return written;
}

bool UiSocket::kickClient(uint8_t num) {
    if (num >= MAX_CLIENTS) return false;
    WsLock lock(_wsMutex);
    if (!_ws.clientIsConnected(num)) return false;
    APPLOGF("[UiSocket] kickClient#%u — admin disconnect (slot reclaim)", num);
    _lastInboundMs[num] = 0;
    _ws.disconnect(num);
    return true;
}

// ============================================================================
// broadcastTelemetry() / broadcastStatus() / broadcastInterp()
// ============================================================================
//
// All gated by _shouldStream(): muted (quiet, non-most-recent) clients are
// skipped entirely so their backed-up socket can't stall the fan-out. :3

void UiSocket::broadcastTelemetry() {
    WsLock lock(_wsMutex);
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (_shouldStream(i)) sendTelemetry(i);
    }
}

void UiSocket::broadcastStatus() {
    WsLock lock(_wsMutex);
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (_shouldStream(i)) sendStatus(i);
    }
}

void UiSocket::broadcastStats() {
    WsLock lock(_wsMutex);
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (_shouldStream(i)) sendStats(i);
    }
}

void UiSocket::broadcastInterp() {
    WsLock lock(_wsMutex);
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (_shouldStream(i)) sendInterp(i);
    }
}

// ============================================================================
// broadcastAnomaly() — 0x05 ANOMALY, drain cross-core ring, fan to all clients
// ============================================================================
//
// Event-driven, not rate-driven: drains SystemState::anom_ring ONCE under its
// portMUX into a local batch, then packs each event and sends it to every
// connected client. Draining once (rather than per-client) guarantees an event
// isn't consumed by only the first client — the same batch fans to all. The
// producer (Core 1 sampler) laps-clamp is enforced here on the read side.

void UiSocket::broadcastAnomaly() {
    // ---- Drain the cross-core ring under its spinlock -----------------------
    InterpAnomaly batch[SystemState::ANOM_CAP];
    uint8_t n = 0;

    portENTER_CRITICAL(&_state.anom_mux);
    uint32_t w = _state.anom_write;
    uint32_t r = _state.anom_read;
    // Lap-clamp: if the producer got more than CAP ahead, the oldest unread
    // events were already overwritten in the ring — skip forward to the newest
    // CAP window so we never read a slot that's been clobbered mid-flight.
    if (w - r > SystemState::ANOM_CAP) r = w - SystemState::ANOM_CAP;
    while (r < w && n < SystemState::ANOM_CAP) {
        batch[n++] = _state.anom_ring[r % SystemState::ANOM_CAP];
        r++;
    }
    _state.anom_read = r;
    portEXIT_CRITICAL(&_state.anom_mux);

    if (n == 0) return;

    // ---- Pack + fan each event to all connected clients ---------------------
    WsLock lock(_wsMutex);
    for (uint8_t k = 0; k < n; k++) {
        const InterpAnomaly& a = batch[k];

        uint8_t frame[ANOMALY_FRAME_SIZE];
        size_t off = 0;
        frame[off++] = WS_FRAME_ANOMALY;   // 0x05
        frame[off++] = a.kind;

        frame[off++] = (uint8_t)(a.seq & 0xFF);
        frame[off++] = (uint8_t)(a.seq >> 8);

        frame[off++] = (uint8_t)(a.tDevUs & 0xFF);
        frame[off++] = (uint8_t)((a.tDevUs >> 8) & 0xFF);
        frame[off++] = (uint8_t)((a.tDevUs >> 16) & 0xFF);
        frame[off++] = (uint8_t)((a.tDevUs >> 24) & 0xFF);

        auto putU16n = [&](float norm) {
            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;
            uint16_t v = (uint16_t)(norm * 10000.0f + 0.5f);
            frame[off++] = (uint8_t)(v & 0xFF);
            frame[off++] = (uint8_t)(v >> 8);
        };
        putU16n(a.targetPos);
        putU16n(a.startPos);

        auto putF32 = [&](float f) {
            uint32_t bits;
            memcpy(&bits, &f, sizeof(bits));   // raw IEEE754 LE
            frame[off++] = (uint8_t)(bits & 0xFF);
            frame[off++] = (uint8_t)((bits >> 8) & 0xFF);
            frame[off++] = (uint8_t)((bits >> 16) & 0xFF);
            frame[off++] = (uint8_t)((bits >> 24) & 0xFF);
        };
        putF32(a.startVel);
        putF32(a.endSlope);

        frame[off++] = (uint8_t)(a.durationUs & 0xFF);
        frame[off++] = (uint8_t)((a.durationUs >> 8) & 0xFF);
        frame[off++] = (uint8_t)((a.durationUs >> 16) & 0xFF);
        frame[off++] = (uint8_t)((a.durationUs >> 24) & 0xFF);

        putF32(a.extra);

        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            if (_shouldStream(i)) _sendGuarded(i, frame, ANOMALY_FRAME_SIZE, "ANOMALY");
        }
    }
}

// ============================================================================
// senderTask — dedicated Core 0 task, 20ms tick
// ============================================================================

void UiSocket::senderTask(void* arg) {
    UiSocket* self = static_cast<UiSocket*>(arg);
    TickType_t lastWake = xTaskGetTickCount();
    constexpr TickType_t PERIOD = pdMS_TO_TICKS(22); // ~45Hz telemetry, ~22ms

    self->_lastStatusMs = millis();

    while (true) {
        vTaskDelayUntil(&lastWake, PERIOD);

        // WS server servicing lives HERE now (moved off httpTask). _ws.loop()
        // accepts new connections, reads inbound frames, and runs the library
        // heartbeat — all of which can block on a wedged/half-open client socket.
        // Keeping it on this dedicated task means such a block delays only
        // telemetry, never the Core-0 heartbeat/HTTP/OTA. Must run every tick
        // (even with zero clients) so new connections are still accepted. :3
        {
            uint32_t _s0 = millis();
            self->update();                       // _ws.loop() under _wsMutex
            uint32_t _dt = millis() - _s0;
            if (_dt > 120) APPLOGF("[STALL] sender:ws.loop blocked %lums", (unsigned long)_dt);
        }

        bool anyConnected = false;
        {
            WsLock lock(self->_wsMutex);
            for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
                if (self->_ws.clientIsConnected(i)) { anyConnected = true; break; }
            }
        }

        if (!anyConnected) {
            self->_teleFrameSeq = 0;
            continue;
        }

        // Reap dead half-open sockets first — a zombie left connected starves
        // _ws.loop() (and thus HTTP) for everyone else. See _reapDeadClients().
        self->_reapDeadClients();

        self->broadcastTelemetry();
        // 0x04 INTERP rides the same ~45Hz tick as telemetry so the WebUI's
        // planned-path overlay renders as smoothly as the position graph.
        self->broadcastInterp();
        // 0x05 ANOMALY drains the cross-core anomaly ring on the same tick.
        // It's event-driven (no-op when the ring is empty), so riding the 45Hz
        // sender costs nothing when motion is clean and surfaces events within
        // ~22ms when it isn't. :3
        self->broadcastAnomaly();

        uint32_t now = millis();
        if (now - self->_lastStatusMs >= 500) {
            self->_lastStatusMs = now;
            self->broadcastStatus();
            self->broadcastStats();     // 0x06 session odometer, same ~2Hz cadence
        }
    }
}

// ============================================================================
// suspendSender / resumeSender — OTA flash-write safety gate (.clinerules §2)
// ============================================================================
//
// The sender task broadcasts telemetry/status/interp/anomaly from flash-resident
// code every ~22ms and hammers the WS library. During an OTA flash write the
// flash cache is disabled for the active partition; any task executing XIP code
// or touching flash-resident data during that window faults/resets the chip.
// prepareForOta() parks this task BEFORE Update.begin() and finishOta() revives
// it. We suspend the task outright (not a flag) so not one WS byte is emitted
// during the write. The 22ms tick means the task is virtually always sitting in
// vTaskDelayUntil() when suspended, so no half-sent frame is left on the wire. :3

void UiSocket::suspendSender() {
    if (_senderHandle) {
        vTaskSuspend(_senderHandle);
    }
}

void UiSocket::resumeSender() {
    if (_senderHandle) {
        vTaskResume(_senderHandle);
    }
}
