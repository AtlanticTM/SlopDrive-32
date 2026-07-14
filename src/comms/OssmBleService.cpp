#include "OssmBleService.h"
#include "PatternEngine.h"
#include "SystemState.h"
#include "range_mapper.h"
#include "AppLog.h"
#include "config_api.h"

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLECharacteristic.h>
#include <NimBLEAdvertising.h>
#include <math.h>
#include <chrono>
#include "PositionTime.h"

// ============================================================================
// OSSM BLE Protocol constants (from GLOBAL CONTEXT + reference/ossm/BLE_PROTOCOL.md)
// ============================================================================

#define OSSM_SVC_UUID       "522b443a-4f53-534d-0001-420badbabe69"
#define OSSM_CHAR_CMD_UUID  "522b443a-4f53-534d-1000-420badbabe69"
#define OSSM_CHAR_KNOB_UUID "522b443a-4f53-534d-1010-420badbabe69"
#define OSSM_CHAR_LAT_UUID  "522b443a-4f53-534d-1030-420badbabe69"
#define OSSM_CHAR_STATE_UUID "522b443a-4f53-534d-2000-420badbabe69"
#define OSSM_CHAR_LIST_UUID "522b443a-4f53-534d-3000-420badbabe69"
#define OSSM_CHAR_DESC_UUID "522b443a-4f53-534d-3010-420badbabe69"
#define OSSM_ADV_NAME       "OSSM"

#define DISCO_GRACE_MS    1000u
#define DISCO_RAMP_MS     2000u
#define STATE_NOTIFY_MS   1000u

// Pattern descriptions — verbatim from reference/ossm/BLE_PROTOCOL.md
static const char* kDescSimpleStroke = "Acceleration, coasting, deceleration equally split; no sensation";
static const char* kDescTeasingPound = "Speed shifts with sensation; balances faster strokes";
static const char* kDescRoboStroke   = "Sensation varies acceleration; from robotic to gradual";
static const char* kDescHalfnHalf    = "Full and half depth strokes alternate; sensation affects speed";
static const char* kDescDeeper       = "Stroke depth increases per cycle; sensation sets count";
static const char* kDescStopNGo      = "Pauses between strokes; sensation adjusts length";
static const char* kDescInsist       = "Modifies length, maintains speed; sensation influences direction";

static const char* kStateNames[] = {
    "menu.idle",
    "strokeEngine.idle",
    "strokeEngine",
    "simplePenetration.idle",
    "streaming",
    "homing",
    "error.idle"
};

// ============================================================================
// Static instance pointer for NimBLE callback trampolines
// ============================================================================

static OssmBleService* g_ossm = nullptr;

// ============================================================================
// NimBLE callback classes — no `override` (NimBLE 1.4.x uses positional args)
// ============================================================================

class OssmServerCB : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* srv) {
        if (g_ossm) g_ossm->_onConnect(0);
    }
    void onDisconnect(NimBLEServer* srv) {
        if (g_ossm) g_ossm->_onDisconnect(0, 0);
    }
    // NimBLE 1.4.x may pass connInfo as second param
    void onConnect(NimBLEServer* srv, ble_gap_conn_desc* desc) {
        if (g_ossm) g_ossm->_onConnect(desc ? desc->conn_handle : 0);
    }
    void onDisconnect(NimBLEServer* srv, ble_gap_conn_desc* desc) {
        if (g_ossm) g_ossm->_onDisconnect(desc ? desc->conn_handle : 0, 0);
    }
};

class OssmCmdCB : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* chr) {
        if (g_ossm) g_ossm->_onCommandWrite(chr);
    }
    void onRead(NimBLECharacteristic* chr) {
        // Last response already stored in char value by _respond()
    }
};

class OssmKnobCB : public NimBLECharacteristicCallbacks {
public:
    void onRead(NimBLECharacteristic* chr) {
        chr->setValue(g_ossm ? (g_ossm->getKnobLimit() ? "true" : "false") : "false");
    }
    void onWrite(NimBLECharacteristic* chr) {
        if (!g_ossm) return;
        std::string raw = chr->getValue();
        std::string lower;
        for (char c : raw) lower += (char)tolower(c);
        if (lower == "true" || lower == "1" || lower == "t") {
            g_ossm->setKnobLimit(true);
            chr->setValue("true");
        } else if (lower == "false" || lower == "0" || lower == "f") {
            g_ossm->setKnobLimit(false);
            chr->setValue("false");
        } else {
            chr->setValue("error:invalid_value");
        }
    }
};

class OssmLatCB : public NimBLECharacteristicCallbacks {
public:
    void onRead(NimBLECharacteristic* chr) {
        chr->setValue(g_ossm ? (g_ossm->getLatencyComp() ? "true" : "false") : "false");
    }
    void onWrite(NimBLECharacteristic* chr) {
        if (!g_ossm) return;
        std::string raw = chr->getValue();
        std::string lower;
        for (char c : raw) lower += (char)tolower(c);
        if (lower == "true" || lower == "1" || lower == "t") {
            g_ossm->setLatencyComp(true);
            chr->setValue("true");
        } else if (lower == "false" || lower == "0" || lower == "f") {
            g_ossm->setLatencyComp(false);
            chr->setValue("false");
        } else {
            chr->setValue("error:invalid_value");
        }
    }
};

class OssmStateCB : public NimBLECharacteristicCallbacks {
public:
    void onRead(NimBLECharacteristic* chr) {
        if (g_ossm) chr->setValue(g_ossm->buildStateJson().c_str());
    }
};

class OssmListCB : public NimBLECharacteristicCallbacks {
public:
    void onRead(NimBLECharacteristic* chr) {
        int count = PatternEngine::patternCount();
        String json = "[";
        for (int i = 0; i < count; i++) {
            if (i > 0) json += ",";
            json += "{\"name\":\"";
            json += PatternEngine::patternName(i);
            json += "\",\"idx\":";
            json += String(i);
            json += "}";
        }
        json += "]";
        chr->setValue(json.c_str());
    }
};

class OssmDescCB : public NimBLECharacteristicCallbacks {
public:
    void onRead(NimBLECharacteristic* chr) {
        if (g_ossm) {
            int idx = g_ossm->getDescCacheIdx();
            if (idx < 0) idx = 0;
            chr->setValue(OssmBleService::descriptionForIndex(idx));
        }
    }
    void onWrite(NimBLECharacteristic* chr) {
        if (!g_ossm) return;
        std::string raw = chr->getValue();
        int idx = 0;
        for (char c : raw) {
            if (c >= '0' && c <= '9') idx = idx * 10 + (c - '0');
            else break;
        }
        if (idx < 0) idx = 0;
        if (idx >= PatternEngine::patternCount())
            idx = PatternEngine::patternCount() - 1;
        g_ossm->setDescCacheIdx(idx);
    }
};

// ============================================================================
// Constructor / destructor
// ============================================================================

OssmBleService::OssmBleService(SystemState& state, PatternEngine& patternEngine,
                                 RangeMapper& mapper, QueueHandle_t waypointQueue)
    : _state(state), _patternEngine(patternEngine), _mapper(mapper), _waypointQueue(waypointQueue)
{}

OssmBleService::~OssmBleService() {
    stop();
}

// ============================================================================
// Lifecycle
// ============================================================================

void OssmBleService::init() {
    g_ossm = this;
    APPLOG("OssmBleService: init done");
}

void OssmBleService::start() {
    if (_running) return;

    APPLOG("OssmBleService: starting NimBLE OSSM service...");

    // CRITICAL: deinit any prior NimBLE stack before re-initializing.
    // Calling NimBLEDevice::init() twice without an intervening deinit(true)
    // leaves the NimBLE host task with stale internal state, causing
    // LoadProhibited crashes when it dereferences freed heap pointers.
    // deinit(true) blocks until the host task is fully torn down.
    //
    // BUT: NimBLEDevice::deinit() has NO internal guard against being called
    // when the stack was never initialized (unlike init(), which checks its
    // `initialized` flag before doing any work). Calling deinit() when NimBLE
    // has never been brought up (e.g. switching straight to OSSM mode from
    // SER/WS, where neither BleTransport nor OssmBleService has run yet)
    // makes nimble_port_stop() pend on a FreeRTOS mutex that was never
    // created — hits `assert(mu->handle)` in npl_freertos_mutex_pend() and
    // reboots the board. Only deinit if NimBLE is actually initialized. :3
    if (NimBLEDevice::getInitialized()) {
        NimBLEDevice::deinit(true);
    }

    NimBLEDevice::init(OSSM_ADV_NAME);

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(new OssmServerCB());

    _svc = _server->createService(OSSM_SVC_UUID);

    // 1000: Primary Command (READ, WRITE)
    _charCmd = _svc->createCharacteristic(OSSM_CHAR_CMD_UUID,
                                          NIMBLE_PROPERTY::READ |
                                          NIMBLE_PROPERTY::WRITE);
    _charCmd->setCallbacks(new OssmCmdCB());

    // 1010: Speed knob limit (READ, WRITE)
    _charKnob = _svc->createCharacteristic(OSSM_CHAR_KNOB_UUID,
                                           NIMBLE_PROPERTY::READ |
                                           NIMBLE_PROPERTY::WRITE);
    _charKnob->setCallbacks(new OssmKnobCB());
    _charKnob->setValue("true");

    // 1030: Latency compensation (READ, WRITE)
    _charLat = _svc->createCharacteristic(OSSM_CHAR_LAT_UUID,
                                          NIMBLE_PROPERTY::READ |
                                          NIMBLE_PROPERTY::WRITE);
    _charLat->setCallbacks(new OssmLatCB());
    _charLat->setValue("false");

    // 2000: State JSON (READ, NOTIFY)
    _charState = _svc->createCharacteristic(OSSM_CHAR_STATE_UUID,
                                           NIMBLE_PROPERTY::READ |
                                           NIMBLE_PROPERTY::NOTIFY);
    _charState->setCallbacks(new OssmStateCB());
    _charState->setValue(_buildStateJson().c_str());

    // 3000: Pattern list JSON (READ)
    _charList = _svc->createCharacteristic(OSSM_CHAR_LIST_UUID,
                                           NIMBLE_PROPERTY::READ);
    _charList->setCallbacks(new OssmListCB());

    // 3010: Pattern description (READ, WRITE)
    _charDesc = _svc->createCharacteristic(OSSM_CHAR_DESC_UUID,
                                           NIMBLE_PROPERTY::READ |
                                           NIMBLE_PROPERTY::WRITE);
    _charDesc->setCallbacks(new OssmDescCB());
    _descCacheIdx = 0;
    _charDesc->setValue(_descriptionForIndex(0));

    _svc->start();

    // Advertising
    _adv = NimBLEDevice::getAdvertising();
    _adv->addServiceUUID(OSSM_SVC_UUID);
    _adv->addServiceUUID(NimBLEUUID((uint16_t)0x180A));
    _adv->setScanResponse(true);
    _adv->setMinPreferred(6);
    _adv->setMaxPreferred(12);

    NimBLEAdvertisementData advData;
    advData.setName(OSSM_ADV_NAME);
    advData.setCompleteServices(NimBLEUUID(OSSM_SVC_UUID));
    _adv->setAdvertisementData(advData);

    _adv->start();
    _running = true;
    _lastNotifyMs = millis();
    _clientConnected = false;
    _discoStartMs = 0;
    _ramping = false;

    APPLOG("OssmBleService: advertising as \"OSSM\"");
}

void OssmBleService::stop() {
    if (!_running) return;

    APPLOG("OssmBleService: stopping NimBLE...");

    if (_adv) { _adv->stop(); _adv = nullptr; }

    NimBLEDevice::deinit(true);

    _server      = nullptr;
    _svc         = nullptr;
    _charCmd     = nullptr;
    _charKnob    = nullptr;
    _charLat     = nullptr;
    _charState   = nullptr;
    _charList    = nullptr;
    _charDesc    = nullptr;
    _running      = false;
    _clientConnected = false;
    _ramping      = false;
    _discoStartMs = 0;

    APPLOG("OssmBleService: stopped");
}

void OssmBleService::emergencyStop() {
    _patternEngine.emergencyStop();
    _ossmState = OssmState::MENU_IDLE;
}

// ============================================================================
// update() — heartbeat notify + disconnect safety ramp (Core 0)
// ============================================================================

void OssmBleService::update() {
    if (!_running) return;

    uint32_t now = millis();

    // ---- Disconnect safety ramp -------------------------------------------
    if (_ramping) {
        float factor = _rampFactor(now);
        if (factor <= 0.01f) {
            _ramping = false;
            _patternEngine.stop();
            APPLOG("OssmBleService: disconnect ramp complete — motion stopped");
        } else {
            float speed = _rampStartSpeed * factor;
            _patternEngine.setSpeed(speed);
        }
    }

    // ---- Heartbeat state notify (every 1000ms) ------------------------------
    if (_clientConnected && (now - _lastNotifyMs >= STATE_NOTIFY_MS)) {
        _lastNotifyMs = now;
        if (_charState) {
            _charState->setValue(_buildStateJson().c_str());
            _charState->notify();
        }
    }
}

// ============================================================================
// Connect / Disconnect callbacks
// ============================================================================

void OssmBleService::_onConnect(uint16_t connHandle) {
    _lastConnHandle = connHandle;
    _clientConnected = true;
    _ramping = false;
    _discoStartMs = 0;
    APPLOGF("OssmBleService: client connected (handle=%u)", connHandle);
}

void OssmBleService::_onDisconnect(uint16_t connHandle, int reason) {
    _clientConnected = false;

    if (_ossmState == OssmState::STROKEENGINE_RUNNING ||
        _ossmState == OssmState::STREAMING ||
        _ossmState == OssmState::SIMPLEPENETRATION_IDLE) {

        float currentSpeed = _patternEngine.getSpeedPercent();
        if (currentSpeed > 0.0f) {
            _discoStartMs = millis();
            _rampStartSpeed = currentSpeed;
            APPLOGF("OssmBleService: disconnect while %.0f%% speed — 1s grace before ramp",
                    currentSpeed);
        }
    }

    APPLOGF("OssmBleService: client disconnected (handle=%u, reason=%d)",
            connHandle, reason);

    if (_adv && _running) {
        _adv->start();
    }
}

// ============================================================================
// Command write handler — NimBLE host task (Core 0)
// ============================================================================

void OssmBleService::_onCommandWrite(NimBLECharacteristic* chr) {
    std::string raw = chr->getValue();
    if (raw.empty()) { _respond("fail:"); return; }

    size_t len = raw.length();
    if (len > 127) len = 127;
    char buf[128];
    memcpy(buf, raw.data(), len);
    buf[len] = '\0';

    APPLOGF("OssmBleService: cmd rx: \"%s\"", buf);

    if (strncmp(buf, "set:", 4) == 0) {
        _handleSet(buf + 4);
    } else if (strncmp(buf, "go:", 3) == 0) {
        _handleGo(buf + 3);
    } else if (strncmp(buf, "stream:", 7) == 0) {
        _handleStream(buf + 7);
    } else {
        char resp[140];
        snprintf(resp, sizeof(resp), "fail:%s", buf);
        if (_charCmd) _charCmd->setValue(resp);
    }
}

// ============================================================================
// set: handler
// ============================================================================

void OssmBleService::_handleSet(const char* cmd) {
    const char* colon = strchr(cmd, ':');
    if (!colon) { _respond("fail:set:"); return; }

    size_t plen = colon - cmd;
    char param[32] = {0};
    if (plen > 31) plen = 31;
    strncpy(param, cmd, plen);

    float value = atof(colon + 1);

    if (strcmp(param, "speed") == 0) {
        if (value < 0) value = 0; if (value > 100) value = 100;
        _patternEngine.setSpeed(value);
        _respond("ok:set:speed:");
    } else if (strcmp(param, "stroke") == 0) {
        if (value < 0) value = 0; if (value > 100) value = 100;
        _patternEngine.setStroke(value);
        _respond("ok:set:stroke:");
    } else if (strcmp(param, "depth") == 0) {
        if (value < 0) value = 0; if (value > 100) value = 100;
        _patternEngine.setDepth(value);
        _respond("ok:set:depth:");
    } else if (strcmp(param, "sensation") == 0) {
        if (value < 0) value = 0; if (value > 100) value = 100;
        _patternEngine.setSensation(value);
        _respond("ok:set:sensation:");
    } else if (strcmp(param, "pattern") == 0) {
        int idx = (int)value;
        if (idx < 0) idx = 0;
        if (idx >= PatternEngine::patternCount()) idx = PatternEngine::patternCount() - 1;
        _patternEngine.setPattern(idx);
        _respond("ok:set:pattern:");
    } else {
        char resp[64];
        snprintf(resp, sizeof(resp), "fail:set:%s:", param);
        if (_charCmd) _charCmd->setValue(resp);
    }
}

// ============================================================================
// go: handler
// ============================================================================

void OssmBleService::_handleGo(const char* cmd) {
    if (strcmp(cmd, "strokeEngine") == 0) {
        if (!_state.homed) { _respond("fail:go:strokeEngine"); return; }
        _state.resume_start_ms = millis();
        _patternEngine.start();
        _ossmState = OssmState::STROKEENGINE_RUNNING;
        _respond("ok:go:strokeEngine");
    } else if (strcmp(cmd, "menu") == 0) {
        _patternEngine.stop();
        _ramping = false;
        _ossmState = OssmState::MENU_IDLE;
        _respond("ok:go:menu");
    } else if (strcmp(cmd, "streaming") == 0) {
        _patternEngine.stop();
        _ossmState = OssmState::STREAMING;
        _respond("ok:go:streaming");
    } else if (strcmp(cmd, "simplePenetration") == 0) {
        if (!_state.homed) { _respond("fail:go:simplePenetration"); return; }
        _patternEngine.setPattern(0);
        _state.resume_start_ms = millis();
        _patternEngine.start();
        _ossmState = OssmState::SIMPLEPENETRATION_IDLE;
        _respond("ok:go:simplePenetration");
    } else {
        _respond("fail:go:");
    }
}

// ============================================================================
// stream: handler — validate only (Phase 4 bridges to waypoint queue)
// ============================================================================

void OssmBleService::_handleStream(const char* cmd) {
    if (_ossmState != OssmState::STREAMING) {
        _respond("fail:stream:not_in_streaming_state");
        return;
    }
    const char* colon = strchr(cmd, ':');
    if (!colon) { _respond("fail:stream:"); return; }
    float pos = atof(cmd);
    float timeMs = atof(colon + 1);
    if (pos < 0) pos = 0; if (pos > 100) pos = 100;
    if (timeMs < 1) timeMs = 1; if (timeMs > 65535) timeMs = 65535;

    // Bridge to the existing waypoint queue — identical to buttplugLinearCmd
    float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
    float span = hi - lo;
    float pos_mm = lo + (pos / 100.0f) * span;
    float position01 = span > 0.01f ? (pos_mm - lo) / span : 0.0f;
    if (position01 < 0.0f) position01 = 0.0f;
    if (position01 > 1.0f) position01 = 1.0f;

    PositionTime pt;
    pt.position     = (uint8_t)(int)(position01 * 100.0f);
    pt.inTime       = (uint16_t)(int)timeMs;
    pt.has_set_time = true;
    pt.setTime      = std::chrono::steady_clock::now();

    if (_waypointQueue) {
        xQueueSend(_waypointQueue, &pt, 0);
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "ok:stream:%d:%d", (int)pos, (int)timeMs);
    if (_charCmd) _charCmd->setValue(resp);
}

// ============================================================================
// Response helper
// ============================================================================

void OssmBleService::_respond(const char* response) {
    if (_charCmd) _charCmd->setValue(response);
}

// ============================================================================
// State JSON builder
// ============================================================================

String OssmBleService::_buildStateJson() {
    return buildStateJson();
}

String OssmBleService::buildStateJson() {
    const char* stateStr  = kStateNames[(int)_ossmState];
    int         speed     = (int)roundf(_patternEngine.getSpeedPercent());
    int         stroke    = (int)roundf(_patternEngine.getStrokePercent());
    int         depth     = (int)roundf(_patternEngine.getDepthPercent());
    int         sensation = (int)roundf(_patternEngine.getSensationPercent());
    int         patIdx    = _patternEngine.getPatternIdx();

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"speed\":%d,\"stroke\":%d,\"sensation\":%d,\"depth\":%d,\"pattern\":%d}",
             stateStr, speed, stroke, sensation, depth, patIdx);
    return String(buf);
}

// ============================================================================
// Disconnect safety ramp — easeInOutSine
// ============================================================================

float OssmBleService::_rampFactor(uint32_t nowMs) {
    if (_discoStartMs == 0) return 1.0f;
    uint32_t elapsed = nowMs - _discoStartMs;
    if (elapsed < DISCO_GRACE_MS) return 1.0f;
    elapsed -= DISCO_GRACE_MS;
    if (elapsed >= DISCO_RAMP_MS) return 0.0f;
    float t = (float)elapsed / (float)DISCO_RAMP_MS;
    return 0.5f * (1.0f + cosf(PI * t));
}

// ============================================================================
// Pattern description lookup
// ============================================================================

const char* OssmBleService::_descriptionForIndex(int idx) {
    return descriptionForIndex(idx);
}

const char* OssmBleService::descriptionForIndex(int idx) {
    switch (idx) {
        case 0:  return kDescSimpleStroke;
        case 1:  return kDescTeasingPound;
        case 2:  return kDescRoboStroke;
        case 3:  return kDescHalfnHalf;
        case 4:  return kDescDeeper;
        case 5:  return kDescStopNGo;
        case 6:  return kDescInsist;
        default: return "Unknown pattern";
    }
}