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
    _ws.enableHeartbeat(15000, 5000, 3);

    _ws.onEvent([this](uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
        this->_handleEvent(num, type, payload, length);
    });

    // Spawn sender task pinned to Core 0, priority 3 (above httpTask=1)
    xTaskCreatePinnedToCore(
        uiStreamTask, "UiStream", 4096, this, 3, &_senderHandle, 0);

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
    _ws.sendBIN(num, frame, HELLO_FRAME_SIZE);
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
    { WsLock lock(_wsMutex); sent = _ws.sendBIN(num, buf, off); }
    if (buf != stack_buf) delete[] buf;
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
        // Reset idempotency ring for new client
        _idemWrite[num] = 0;
        _idemCount[num] = 0;
        sendHello(num);
        break;
    }
    case WStype_DISCONNECTED:
        APPLOGF("[UiSocket] DISCONNECTED client#%u", num);
        onClientDisconnect();
        break;
    case WStype_BIN: {
        if (length < 1) return;
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
            _ws.sendBIN(num, r, CLOCK_REPLY_SIZE);  // already under WsLock from top of _handleEvent

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
        }
        break;
    }
    default: break;
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
    if (_state.estop_requested.load())  flags |= (1 << 3);
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

    bool ok = _ws.sendBIN(num, frame, off);
    if (!ok) {
        _tx_drops++;
    }
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
                                   : (uint32_t)(MACHINE_MAX_TRAVEL_MM * 100.0f + 0.5f);
    } else {
        stroke_10um = (uint32_t)(MACHINE_MAX_TRAVEL_MM * 100.0f + 0.5f);
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

    _ws.sendBIN(num, frame, STATUS_FRAME_SIZE);
}

// ============================================================================
// broadcastTelemetry() / broadcastStatus()
// ============================================================================

void UiSocket::broadcastTelemetry() {
    WsLock lock(_wsMutex);
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (_ws.clientIsConnected(i)) sendTelemetry(i);
    }
}

void UiSocket::broadcastStatus() {
    WsLock lock(_wsMutex);
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (_ws.clientIsConnected(i)) sendStatus(i);
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

        self->broadcastTelemetry();

        uint32_t now = millis();
        if (now - self->_lastStatusMs >= 500) {
            self->_lastStatusMs = now;
            self->broadcastStatus();
        }
    }
}