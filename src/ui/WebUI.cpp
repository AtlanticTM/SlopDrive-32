#include "WebUI.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_timer.h>

#if defined(FEATURE_RS485_MODBUS)
#include "ServoModbus.h"
#endif

#include "AppLog.h"
#include "ConfigStore.h"
#include "StatusLeds.h"
#include "PatternEngine.h"
#include "MotorDriver.h"

#include "TransportManager.h"
#include "TCodeParser.h"
#include "SerialTransport.h"
#include "WebSocketTransport.h"
#include "BleTransport.h"
#include "MotionArbiter.h"
#include "config_api.h"
#include "range_mapper.h"

// ---- Fallback HTML page (shown when LittleFS /index.html is missing) -------
static const char* htmlFallbackPage = R"RAWHTML(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SlopDrive-32 - Filesystem Missing</title>
  <style>body{font-family:sans-serif;background:#1a1a2e;color:#eee;padding:30px;line-height:1.5}
  code{background:#0f3460;padding:2px 6px;border-radius:4px}</style>
</head>
<body>
  <h1>&#x1F680; SlopDrive-32</h1>
  <p><strong>Web UI files not found on the device.</strong></p>
  <p>The interface is served from the LittleFS filesystem image. Upload it with
  the PlatformIO task <code>Upload Filesystem Image</code> (or run
  <code>pio run -t uploadfs</code>), then reload this page.</p>
</body>
</html>
)RAWHTML";

// ============================================================================
// Constructor — capture references to all subsystems
// ============================================================================

WebUI::WebUI(SystemState&        state,
             MotorDriver&        motor,
             RangeMapper&        mapper,
             PatternEngine&      patternEngine,
             TransportManager&   transportMgr,
             SerialTransport&    serialTransport,
             WebSocketTransport& wsTransport,
             BleTransport&       bleTransport)
    : _state(state)
    , _motor(motor)
    , _mapper(mapper)
    , _patternEngine(patternEngine)
    , _transportMgr(transportMgr)

    , _serialTransport(serialTransport)
    , _wsTransport(wsTransport)
    , _bleTransport(bleTransport)
{
    _httpServer = new WebServer(HTTP_PORT);
}

WebUI::~WebUI() {
    delete _httpServer;
}

// ============================================================================
// init() — register every route and start the server
// ============================================================================

void WebUI::init() {
    _httpServer->on("/",          [this]() { handleRoot(); });
    _httpServer->on("/api/status",    HTTP_GET,  [this]() { handleApiStatus(); });
    _httpServer->on("/api/capabilities", HTTP_GET, [this]() { handleApiCapabilities(); });
    _httpServer->on("/api/settings",  HTTP_GET,  [this]() { handleApiSettings(); });
    _httpServer->on("/api/settings",  HTTP_POST, [this]() { statusLedsActivity(); handleApiSettings(); });
    _httpServer->on("/api/move",      HTTP_POST, [this]() { statusLedsActivity(); handleApiMove(); });
    _httpServer->on("/api/home",      HTTP_POST, [this]() { statusLedsActivity(); handleApiHome(); });
    _httpServer->on("/api/stop",      HTTP_POST, [this]() { statusLedsActivity(); handleApiStop(); });
    _httpServer->on("/api/pause",     HTTP_POST, [this]() { statusLedsActivity(); handleApiPause(); });
    _httpServer->on("/api/halt",      HTTP_POST, [this]() { statusLedsActivity(); handleApiHalt(); });
    _httpServer->on("/api/override",  HTTP_POST, [this]() { statusLedsActivity(); handleApiOverride(); });
    _httpServer->on("/api/tmc",       HTTP_GET,  [this]() { handleApiTmc(); });
    _httpServer->on("/api/tmc",       HTTP_POST, [this]() { statusLedsActivity(); handleApiTmc(); });
    _httpServer->on("/api/clearfault",HTTP_POST, [this]() { statusLedsActivity(); handleApiClearFault(); });
    _httpServer->on("/api/pattern",   HTTP_GET,  [this]() { handleApiPattern(); });
    _httpServer->on("/api/pattern",   HTTP_POST, [this]() { statusLedsActivity(); handleApiPattern(); });
    _httpServer->on("/api/log",       HTTP_GET,  [this]() { handleApiLog(); });
    _httpServer->on("/api/mode",      HTTP_GET,  [this]() { handleApiMode(); });
    _httpServer->on("/api/mode",      HTTP_POST, [this]() { handleApiMode(); });

    _httpServer->begin();
    APPLOGF("HTTP server on port %d", HTTP_PORT);

    startTelemetrySampler();
}

// ============================================================================
// update()
// ============================================================================

void WebUI::update() {
    _httpServer->handleClient();
}

// ---- Dedicated telemetry sampler -------------------------------------------
void WebUI::telemetryTimerCb(void* arg) {
    WebUI* self = static_cast<WebUI*>(arg);
    // D4: actual = stepper.getCurrentPosition() ONLY — FAS truth, never the planner's inbox.
    // The position graph draws three lines:
    //   took (actual)  = motor position NOW           → reality blue
    //   told (target)  = planner output after clamping → intent purple
    //   asked (raw)    = TCode parser + mapper demand  → dotted asked
    // When the planner derives a slower profile (gentle command), the gap
    // between "told" and "took" shows exactly how much the planner backed off.
    self->captureTelemetry(self->_motor.getPosition(),
                           self->_state.commanded_target_mm,
                           self->_state.commanded_raw_mm);
}

void WebUI::startTelemetrySampler() {
    static esp_timer_handle_t handle = nullptr;
    if (handle) return;
    esp_timer_create_args_t args = {};
    args.callback        = &WebUI::telemetryTimerCb;
    args.arg             = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name            = "tele10ms";
    if (esp_timer_create(&args, &handle) == ESP_OK) {
        esp_timer_start_periodic(handle, 4166ULL);
        APPLOG("Telemetry sampler armed — sampling the shaft at 240Hz :3");
    } else {
        APPLOG("Telemetry sampler FAILED to arm — graph will be limp. :<");
    }
}

void WebUI::captureTelemetry(float position_mm, float target_mm, float raw_mm) {
    portENTER_CRITICAL_ISR(&_telemetry_mux);
    uint32_t seq = _telemetry_seq;
    size_t idx = seq % TELEMETRY_RING_SIZE;
    _telemetry_ring[idx].position_mm = position_mm;
    _telemetry_ring[idx].target_mm   = target_mm;
    _telemetry_ring[idx].raw_mm      = raw_mm;
    _telemetry_ring[idx].t_dev_us    = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFu);
    _telemetry_seq = seq + 1;
    portEXIT_CRITICAL_ISR(&_telemetry_mux);
}

// ============================================================================
// Route handlers
// ============================================================================

void WebUI::handleRoot() {
    if (LittleFS.exists("/index.html.gz")) {
        File f = LittleFS.open("/index.html.gz", "r");
        if (f) {
            _httpServer->streamFile(f, "text/html");
            f.close();
            return;
        }
    }
    if (LittleFS.exists("/index.html")) {
        File f = LittleFS.open("/index.html", "r");
        if (f) {
            _httpServer->streamFile(f, "text/html");
            f.close();
            return;
        }
    }
    _httpServer->send(200, "text/html", htmlFallbackPage);
}

void WebUI::handleApiStatus() {
    JsonDocument doc;
    doc["uptime_ms"] = (uint32_t)millis();
    doc["wifi_connected"] = _state.wifi_ready;
    doc["ip"] = WiFi.localIP().toString();

    doc["rssi"] = _state.wifi_rssi;
    doc["wifi_channel"] = _state.wifi_channel;
    doc["wifi_bssid"] = _state.wifi_bssid;
    doc["wifi_reconnects"] = _state.wifi_reconnects;
    doc["wifi_last_disconnect_reason"] = _state.wifi_last_disconnect_reason;

    doc["homed"] = _state.homed;
    doc["homing"] = _state.homing_in_progress;
    doc["buttplug_connected"] = _wsTransport.isServerConnected();
    doc["position"] = _motor.getPosition();

    doc["has_current_sensor"] = _motor.hasCurrentSensor();
    if (_motor.hasCurrentSensor()) {
        doc["bus_current_a"] = _motor.getBusCurrentA();
        doc["bus_voltage_v"] = _motor.getBusVoltageV();
    }

    doc["has_power_monitor"] = _motor.hasPowerMonitor();
    if (_motor.hasPowerMonitor()) {
        doc["bus_power_w"] = _motor.getBusPowerW();
        doc["die_temp_c"] = _motor.getDieTempC();
        doc["peak_current_a"] = _motor.getPeakBusCurrentA();
    }

    uint16_t hz = _state.measured_hz;
    doc["measured_hz"] = hz;
    doc["measured_interval_ms"] = (hz > 0) ? (uint16_t)(1000 / hz) : 0;
    doc["auto_duration"] = _state.auto_duration;
    doc["measured_stroke_mm"] = _motor.getMeasuredStrokeMm();
    doc["serial_mode"] = (bool)SERIAL_CONTROL_MODE;
    doc["serial_active"] = _serialTransport.isActive();
    doc["serial_linked"] = _serialTransport.isLinked();

    doc["transport"] = TransportManager::transportName(_state.getTransport());
    doc["ble_running"]   = _bleTransport.isRunning();
    doc["ble_connected"] = _bleTransport.isConnected();
    doc["ble_linked"]    = _bleTransport.isLinked();
    doc["dongle_active"] = _transportMgr.isDongleActive();

    doc["paused"] = _state.paused;
    doc["manual_override"] = _state.manual_override;

    // ---- Motion-generation diagnostics (D4: intent rate + plan dynamics) ----
    if (_arbiter) {
        PlanReport rpt = _arbiter->lastReport();
        doc["intent_count"] = _arbiter->totalIntents();
        doc["plan_derived_spd"] = (uint32_t)rpt.derived_speed_mm_s;
        doc["plan_clamped_spd"] = (uint32_t)rpt.clamped_speed_mm_s;
        doc["plan_derived_acc"] = (uint32_t)rpt.derived_accel_mm_s2;
        doc["plan_clamped_acc"] = (uint32_t)rpt.clamped_accel_mm_s2;
        doc["plan_feasible"] = rpt.deadline_feasible;
        doc["plan_late"] = rpt.deadline_late;
    }

    uint32_t since = 0;
    if (_httpServer->hasArg("since")) since = (uint32_t)strtoul(_httpServer->arg("since").c_str(), nullptr, 10);

    TelemetrySample snap[TELEMETRY_RING_SIZE];
    uint32_t        snap_first_seq = 0;
    size_t          snap_n = 0;
    portENTER_CRITICAL(&_telemetry_mux);
    uint32_t head = _telemetry_seq;
    uint32_t oldest = (head > TELEMETRY_RING_SIZE) ? (head - TELEMETRY_RING_SIZE) : 0;
    uint32_t from = (since > oldest) ? since : oldest;
    for (uint32_t s = from; s < head && snap_n < TELEMETRY_RING_SIZE; s++) {
        snap[snap_n++] = _telemetry_ring[s % TELEMETRY_RING_SIZE];
    }
    snap_first_seq = from;
    portEXIT_CRITICAL(&_telemetry_mux);

    doc["tele_seq"] = head;
    doc["tele_dt"]  = TELEMETRY_SAMPLE_INTERVAL_MS;
    doc["tele_from"] = snap_first_seq;
    JsonArray samples = doc["samples"].to<JsonArray>();
    for (size_t i = 0; i < snap_n; i++) {
        JsonArray s = samples.add<JsonArray>();
        s.add(snap[i].position_mm);
        s.add(snap[i].target_mm);
        s.add(snap[i].raw_mm);
    }

    JsonObject drv = doc["driver"].to<JsonObject>();
    drv["valid"]     = false;
    drv["otpw"]      = false;
    drv["ot"]        = false;
    drv["s2ga"]      = false;
    drv["s2gb"]      = false;
    drv["ola"]       = false;
    drv["olb"]       = false;
    drv["stst"]      = false;
    drv["sg_result"] = 0;
    drv["cs_actual"] = 0;
    drv["load_pct"]  = 0;
    drv["faulted"]   = false;

    String json;
    serializeJson(doc, json);
    _httpServer->send(200, "application/json", json);
}

void WebUI::handleApiCapabilities() {
    JsonDocument doc;
    doc["max_travel_mm"] = (float)MACHINE_MAX_TRAVEL_MM;
    doc["measured_stroke_mm"] = _motor.getMeasuredStrokeMm();

    JsonObject speed = doc["speed_ceiling_mm_s"].to<JsonObject>();
    speed["normal"] = (uint32_t)NORMAL_MAX_SPEED_MM_S;
    speed["expert"] = (uint32_t)EXPERT_MAX_SPEED_MM_S;

    JsonObject accel = doc["accel_ceiling_mm_s2"].to<JsonObject>();
    accel["normal"] = (uint32_t)NORMAL_MAX_ACCEL_MM_S2;
    accel["expert"] = (uint32_t)EXPERT_MAX_ACCEL_MM_S2;

    JsonObject feat = doc["features"].to<JsonObject>();
    feat["has_current_sensor"] = _motor.hasCurrentSensor();
    feat["has_power_monitor"]  = _motor.hasPowerMonitor();
#if defined(FEATURE_RS485_MODBUS)
    feat["has_rs485"] = true;
#else
    feat["has_rs485"] = false;
#endif
#if defined(BLE_ENABLED)
    feat["has_ble"] = true;
#else
    feat["has_ble"] = false;
#endif
    feat["has_dongle"] = true;
    feat["blend_mode"] = _motor.getBlendMode();
    feat["expert_ceilings"] = _state.expert_mode;

    String json;
    serializeJson(doc, json);
    _httpServer->send(200, "application/json", json);
}

void WebUI::handleApiClearFault() {
    APPLOG("Driver fault clear requested (no-op — readback removed)");
    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

// ============================================================================
// handleApiSettings (HTTP GET + POST) — delegates to applySettings for mutations
// ============================================================================

void WebUI::handleApiSettings() {
    if (_httpServer->method() == HTTP_GET) {
        JsonDocument doc;
        doc["range_min"] = _mapper.getMinMm();
        doc["range_max"] = _mapper.getMaxMm();
        doc["max_speed"] = (uint32_t)_state.config.max_speed_mm_s;
        doc["accel"] = (uint32_t)_state.config.acceleration_mm_s2;
        // Dual limit sets (v0.4 / D4 Phase 3) — same shape as the WS echo
        doc["user_max_speed"] = (uint32_t)_state.config.user_max_speed_mm_s;
        doc["user_max_accel"] = (uint32_t)_state.config.user_max_accel_mm_s2;
        doc["input_max_speed"] = (uint32_t)_state.config.input_max_speed_mm_s;
        doc["input_max_accel"] = (uint32_t)_state.config.input_max_accel_mm_s2;
        doc["blend_mode"] = _motor.getBlendMode();
        doc["auto_duration"] = _state.auto_duration;
        doc["intiface_compat"] = _state.intiface_compat;
        doc["default_range_min"] = _state.default_range_min;
        doc["default_range_max"] = _state.default_range_max;
        doc["expert_mode"] = _state.expert_mode;
        doc["stream_speed_mode"] = (uint8_t)_state.stream_speed_mode;
        doc["max_travel"] = (float)MACHINE_MAX_TRAVEL_MM;
        doc["measured_stroke"] = _motor.getMeasuredStrokeMm();

        String json;
        serializeJson(doc, json);
        _httpServer->send(200, "application/json", json);
    } else if (_httpServer->method() == HTTP_POST) {
        String body = _httpServer->arg("plain");
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);

        if (err) {
            _httpServer->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }

        JsonDocument resp;
        if (!applySettings(doc, resp)) {
            String json;
            serializeJson(resp, json);
            _httpServer->send(400, "application/json", json);
            return;
        }

        String json;
        serializeJson(resp, json);
        _httpServer->send(200, "application/json", json);
    }
}

// ============================================================================
// applySettings — shared mutation used by HTTP POST /api/settings AND WS op
// ============================================================================

bool WebUI::applySettings(JsonDocument& doc, JsonDocument& resp) {
    float rmin = doc["range_min"] | _mapper.getMinMm();
    float rmax = doc["range_max"] | _mapper.getMaxMm();
    uint32_t speed = doc["max_speed"] | (uint32_t)_state.config.max_speed_mm_s;
    uint32_t accel = doc["accel"] | (uint32_t)_state.config.acceleration_mm_s2;

    // Dual limit sets — accept from WebUI, seed into arbiter + config
    if (doc["user_max_speed"].is<uint32_t>() || doc["user_max_accel"].is<uint32_t>()) {
        float us = doc["user_max_speed"] | _state.config.user_max_speed_mm_s;
        float ua = doc["user_max_accel"] | _state.config.user_max_accel_mm_s2;
        if (us < 1.0f) us = 1.0f; if (us > MAX_SPEED_MM_S) us = MAX_SPEED_MM_S;
        if (ua < 10.0f) ua = 10.0f; if (ua > MAX_ACCEL_MM_S2) ua = MAX_ACCEL_MM_S2;
        _state.config.user_max_speed_mm_s = us;
        _state.config.user_max_accel_mm_s2 = ua;
        if (_arbiter) { _arbiter->setUserSpeedLimit(us); _arbiter->setUserAccelLimit(ua); }
    }
    if (doc["input_max_speed"].is<uint32_t>() || doc["input_max_accel"].is<uint32_t>()) {
        float is = doc["input_max_speed"] | _state.config.input_max_speed_mm_s;
        float ia = doc["input_max_accel"] | _state.config.input_max_accel_mm_s2;
        if (is < 1.0f) is = 1.0f; if (is > MAX_SPEED_MM_S) is = MAX_SPEED_MM_S;
        if (ia < 10.0f) ia = 10.0f; if (ia > MAX_ACCEL_MM_S2) ia = MAX_ACCEL_MM_S2;
        _state.config.input_max_speed_mm_s = is;
        _state.config.input_max_accel_mm_s2 = ia;
        if (_arbiter) { _arbiter->setInputSpeedLimit(is); _arbiter->setInputAccelLimit(ia); }
    }

    if (rmin >= rmax) {
        resp["ok"] = false;
        resp["error"] = "Min must be less than Max";
        return false;
    }

    if (speed < 1)                          speed = 1;
    if (speed > (uint32_t)MAX_SPEED_MM_S)   speed = (uint32_t)MAX_SPEED_MM_S;
    if (accel < 10)                          accel = 10;
    if (accel > (uint32_t)MAX_ACCEL_MM_S2)   accel = (uint32_t)MAX_ACCEL_MM_S2;

    uint8_t blend_mode = doc["blend_mode"] | (uint8_t)_motor.getBlendMode();
    if (blend_mode < 1) blend_mode = 1;
    if (blend_mode > 3) blend_mode = 3;

    _state.auto_duration = doc["auto_duration"] | _state.auto_duration;
    _state.intiface_compat = doc["intiface_compat"] | _state.intiface_compat;
    TCodeParser::intifaceCompat = _state.intiface_compat;

    _state.expert_mode = doc["expert_mode"] | _state.expert_mode;
    if (doc["default_range_min"].is<float>() || doc["default_range_max"].is<float>()) {
        float drmin = doc["default_range_min"] | _state.default_range_min;
        float drmax = doc["default_range_max"] | _state.default_range_max;
        if (drmin >= 0.0f && drmax <= MACHINE_MAX_TRAVEL_MM && drmin < drmax) {
            _state.default_range_min = drmin;
            _state.default_range_max = drmax;
        }
    }

    _mapper.setRange(rmin, rmax);

    _state.config.max_speed_mm_s = (float)speed;
    _state.config.acceleration_mm_s2 = (float)accel;
    _motor.setMaxSpeed(_state.config.max_speed_mm_s);
    _motor.setAcceleration(_state.config.acceleration_mm_s2);
    _motor.setBlendMode(blend_mode);
    bool no_persist = doc["no_persist"] | false;

    if (!no_persist) ConfigStore::save(_state, _mapper, _motor);

    // Echo post-clamp values — speed from driver (it caps), accel from config
    // (the driver may cap lower than config_api.h ceiling; align in Phase 3 §4)
    resp["ok"] = true;
    resp["range_min"] = _mapper.getMinMm();
    resp["range_max"] = _mapper.getMaxMm();
    resp["max_speed"] = (uint32_t)_motor.getMaxSpeed();
    resp["accel"] = (uint32_t)_state.config.acceleration_mm_s2;
    resp["user_max_speed"] = (uint32_t)_state.config.user_max_speed_mm_s;
    resp["user_max_accel"] = (uint32_t)_state.config.user_max_accel_mm_s2;
    resp["input_max_speed"] = (uint32_t)_state.config.input_max_speed_mm_s;
    resp["input_max_accel"] = (uint32_t)_state.config.input_max_accel_mm_s2;
    resp["blend_mode"] = _motor.getBlendMode();
    resp["auto_duration"] = _state.auto_duration;
    resp["intiface_compat"] = _state.intiface_compat;
    resp["expert_mode"] = _state.expert_mode;
    resp["default_range_min"] = _state.default_range_min;
    resp["default_range_max"] = _state.default_range_max;

    _bumpGen();
    return true;
}

// ============================================================================
// handleApiMove (HTTP POST) — delegates to applyMove
// ============================================================================

void WebUI::handleApiMove() {
    if (_httpServer->method() == HTTP_POST) {
        String body = _httpServer->arg("plain");
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);

        if (err || !doc["position"].is<float>()) {
            _httpServer->send(400, "application/json", "{\"error\":\"position required\"}");
            return;
        }
        if (!_state.homed) {
            _httpServer->send(400, "application/json", "{\"error\":\"Invalid request or not homed\"}");
            return;
        }

        JsonDocument resp;
        applyMove(doc, resp);

        String json;
        serializeJson(resp, json);
        _httpServer->send(200, "application/json", json);
    }
}

// ============================================================================
// applyMove — shared mutation used by HTTP POST /api/move AND WS op
// ============================================================================

bool WebUI::applyMove(JsonDocument& doc, JsonDocument& resp) {
    if (!_state.homed) {
        resp["ok"] = false;
        resp["error"] = "Not homed";
        return false;
    }

    float pos = doc["position"] | 0.0f;
    bool bypass = doc["bypass_limits"] | false;
    if (bypass) {
        pos = constrain(pos, 0.0f, MACHINE_MAX_TRAVEL_MM);
    } else {
        pos = constrain(pos, _mapper.getMinMm(), _mapper.getMaxMm());
    }
    bool stream = doc["stream"] | true;
    float speed = doc["speed"] | 0.0f;
    if (stream) {
        _motor.streamTo(pos, speed);
    } else {
        _motor.moveTo(pos);
    }

    // Store the commanded position so the telemetry sampler captures live manual moves
    // (the TCode path does this in motionConsumerTask; manual moves bypass it entirely).
    _state.actual_position_mm.store(pos, std::memory_order_relaxed);
    _state.commanded_target_mm = pos;

    resp["ok"] = true;
    resp["position"] = pos;
    resp["bypass_limits"] = bypass;
    resp["stream"] = stream;

    _bumpGen();
    return true;
}

void WebUI::handleApiHome() {
    if (!_state.homing_in_progress) {
        _state.homed = false;
        _state.homing_in_progress = true;
        _state.resume_start_ms = millis();   // arm soft-start NOW so the first post-rehome move doesn't lunge (F-003)
    }
    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiStop() {
    _state.estop_requested.store(true);
    _state.homed = false;
    _state.homing_in_progress = false;
    _state.paused = false;
    _state.manual_override = false;
    _state.resume_start_ms = 0;
    _bumpGen();
    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiPause() {
    JsonDocument doc;
    deserializeJson(doc, _httpServer->arg("plain"));
    bool was = _state.paused;
    _state.paused = doc["paused"] | (!_state.paused);
    if (_state.paused && !was) {
        if (_motor.isHomed()) _motor.hardStop();
        APPLOG("Paused: hands off the puppers — Intiface input edged out :3");
    } else if (!_state.paused && was) {
        _state.resume_start_ms = millis();
        APPLOG("Unpaused: easing back in, letting Intiface take the reins again~ :3");
    }
    _bumpGen();
    String json; JsonDocument r; r["ok"] = true; r["paused"] = _state.paused;
    serializeJson(r, json);
    _httpServer->send(200, "application/json", json);
}

void WebUI::handleApiHalt() {
    if (_motor.isHomed()) _motor.hardStop();
    APPLOG("Halt: motor stopped — still homed and ready for round two~ :3");
    _bumpGen();
    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiOverride() {
    JsonDocument doc;
    deserializeJson(doc, _httpServer->arg("plain"));
    bool was = _state.manual_override;
    _state.manual_override = doc["override"] | (!_state.manual_override);
    if (_state.manual_override && !was) {
        APPLOG("Manual override ON: you're topping now — Intiface can watch but can't touch :3");
    } else if (!_state.manual_override && was) {
        _state.resume_start_ms = millis();
        APPLOG("Manual override OFF: handing the leash back to Intiface~ :3");
    }
    _bumpGen();
    String json; JsonDocument r; r["ok"] = true; r["manual_override"] = _state.manual_override;
    serializeJson(r, json);
    _httpServer->send(200, "application/json", json);
}

// ============================================================================
// handleApiTmc (HTTP GET + POST) — delegates to applyDriverConfig
// ============================================================================

void WebUI::handleApiTmc() {
    if (_httpServer->method() == HTTP_GET) {
        JsonDocument doc;
        doc["run_current"]  = _state.driver.run_current_ma;
        doc["hold_current"] = _state.driver.hold_current_pct;
        doc["stealthchop"]  = _state.driver.stealthchop;
        doc["tpwm_thrs"]    = _state.driver.tpwm_thrs;
        doc["toff"]         = _state.driver.toff;
        doc["tbl"]          = _state.driver.tbl;
        doc["hstart"]       = _state.driver.hstart;
        doc["hend"]         = _state.driver.hend;
        String json;
        serializeJson(doc, json);
        _httpServer->send(200, "application/json", json);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, _httpServer->arg("plain"))) {
        _httpServer->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    JsonDocument resp;
    applyDriverConfig(doc, resp);

    String json;
    serializeJson(resp, json);
    _httpServer->send(200, "application/json", json);
}

// ============================================================================
// applyDriverConfig — shared mutation used by HTTP POST /api/tmc AND WS op
// ============================================================================

bool WebUI::applyDriverConfig(JsonDocument& doc, JsonDocument& resp) {
    if (doc["reset"] | false) {
        _state.driver = DriverConfig();
    } else {
        _state.driver.run_current_ma   = constrain((int)(doc["run_current"]  | (int)_state.driver.run_current_ma), 300, 3000);
        _state.driver.hold_current_pct = constrain((int)(doc["hold_current"] | (int)_state.driver.hold_current_pct), 0, 100);
        _state.driver.stealthchop      = (doc["stealthchop"] | (int)_state.driver.stealthchop) ? 1 : 0;
        _state.driver.tpwm_thrs        = (uint32_t)(doc["tpwm_thrs"] | (long)_state.driver.tpwm_thrs);
        _state.driver.toff             = constrain((int)(doc["toff"]   | (int)_state.driver.toff), 1, 15);
        _state.driver.tbl              = constrain((int)(doc["tbl"]    | (int)_state.driver.tbl), 0, 3);
        _state.driver.hstart           = constrain((int)(doc["hstart"] | (int)_state.driver.hstart), 0, 7);
        _state.driver.hend             = constrain((int)(doc["hend"]   | (int)_state.driver.hend), -3, 12);
    }

    _state.config.run_current_ma = _state.driver.run_current_ma;
    _motor.applyDriverConfig(_state.driver);

    bool save = doc["save"] | false;
    if (save) ConfigStore::save(_state, _mapper, _motor);

    resp["ok"] = true;
    resp["run_current"]  = _state.driver.run_current_ma;
    resp["hold_current"] = _state.driver.hold_current_pct;
    resp["stealthchop"]  = _state.driver.stealthchop;
    resp["tpwm_thrs"]    = _state.driver.tpwm_thrs;
    resp["toff"]         = _state.driver.toff;
    resp["tbl"]          = _state.driver.tbl;
    resp["hstart"]       = _state.driver.hstart;
    resp["hend"]         = _state.driver.hend;

    _bumpGen();
    return true;
}

// ============================================================================
// handleApiPattern (HTTP GET + POST) — delegates to applyPattern
// ============================================================================

void WebUI::handleApiPattern() {
    if (_httpServer->method() == HTTP_GET) {
        JsonDocument doc;
        doc["running"]    = _patternEngine.isRunning();
        doc["active"]     = _state.gen_active;
        doc["speed"]      = (int)roundf(_patternEngine.getSpeedPercent());
        doc["depth"]      = (int)roundf(_patternEngine.getDepthPercent());
        doc["stroke"]     = (int)roundf(_patternEngine.getStrokePercent());
        doc["sensation"]  = (int)roundf(_patternEngine.getSensationPercent());
        doc["pattern"]    = _patternEngine.getPatternIdx();
        doc["rate_tick"]  = _state.gen_rate_tick_hz;
        String json;
        serializeJson(doc, json);
        _httpServer->send(200, "application/json", json);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, _httpServer->arg("plain"))) {
        _httpServer->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    APPLOGF("/api/pattern POST: %s", _httpServer->arg("plain").c_str());

    JsonDocument resp;
    if (!applyPattern(doc, resp)) {
        String json;
        serializeJson(resp, json);
        _httpServer->send(400, "application/json", json);
        return;
    }

    String json;
    serializeJson(resp, json);
    _httpServer->send(200, "application/json", json);
}

// ============================================================================
// applyPattern — shared mutation used by HTTP POST /api/pattern AND WS op
// ============================================================================

bool WebUI::applyPattern(JsonDocument& doc, JsonDocument& resp) {
    if (doc["rate_tick"].is<int>()) {
        int r = doc["rate_tick"];
        _state.gen_rate_tick_hz = (r >= 375) ? 500 : (r >= 175) ? 250 : (r >= 75) ? 100 : (r >= 35) ? 50 : 20;
    }

    if (doc["speed"].is<float>())     _patternEngine.setSpeed(doc["speed"]);
    if (doc["depth"].is<float>())     _patternEngine.setDepth(doc["depth"]);
    if (doc["stroke"].is<float>())    _patternEngine.setStroke(doc["stroke"]);
    if (doc["sensation"].is<float>()) _patternEngine.setSensation(doc["sensation"]);
    if (doc["pattern"].is<int>())     _patternEngine.setPattern(doc["pattern"]);

    if (doc["running"].is<bool>()) {
        bool want = doc["running"];
        if (want && !_patternEngine.isRunning()) {
            if (!_state.homed) {
                resp["ok"] = false;
                resp["error"] = "Not homed";
                return false;
            }
            _state.resume_start_ms = millis();
            _patternEngine.start();
            APPLOG("PatternEngine started");
        } else if (!want && _patternEngine.isRunning()) {
            _patternEngine.stop();
            APPLOG("PatternEngine stopped");
        }
    }

    resp["ok"] = true;
    resp["running"]   = _patternEngine.isRunning();
    resp["speed"]     = (int)roundf(_patternEngine.getSpeedPercent());
    resp["depth"]     = (int)roundf(_patternEngine.getDepthPercent());
    resp["stroke"]    = (int)roundf(_patternEngine.getStrokePercent());
    resp["sensation"] = (int)roundf(_patternEngine.getSensationPercent());
    resp["pattern"]   = _patternEngine.getPatternIdx();
    resp["rate_tick"] = _state.gen_rate_tick_hz;

    _bumpGen();
    return true;
}

void WebUI::handleApiLog() {
    String out;
    out.reserve(2048);
    applogDump(out);
    _httpServer->send(200, "text/plain", out);
}

// ============================================================================
// handleApiMode (HTTP GET + POST) — delegates to applyMode
// ============================================================================

void WebUI::handleApiMode() {
    if (_httpServer->method() == HTTP_GET) {
        JsonDocument doc;
        doc["mode"]          = TransportManager::transportName(_state.getTransport());
        doc["ble_running"]   = _bleTransport.isRunning();
        doc["ble_connected"] = _bleTransport.isConnected();
        String json;
        serializeJson(doc, json);
        _httpServer->send(200, "application/json", json);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, _httpServer->arg("plain"))) {
        _httpServer->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    JsonDocument resp;
    if (!applyMode(doc, resp)) {
        String json;
        serializeJson(resp, json);
        _httpServer->send(400, "application/json", json);
        return;
    }

    String json;
    serializeJson(resp, json);
    _httpServer->send(200, "application/json", json);
}

// ============================================================================
// applyMode — shared mutation used by HTTP POST /api/mode AND WS op
// ============================================================================

bool WebUI::applyMode(JsonDocument& doc, JsonDocument& resp) {
    const char* m = doc["mode"] | "";
    TransportMode mode = _state.getTransport();
    if      (strcasecmp(m, "WS")     == 0) mode = TransportMode::WS;
    else if (strcasecmp(m, "SER")    == 0) mode = TransportMode::SER;
    else if (strcasecmp(m, "BT")     == 0) mode = TransportMode::BT;
    else if (strcasecmp(m, "DONGLE") == 0) mode = TransportMode::DONGLE;
    else if (strcasecmp(m, "OSSM") == 0) mode = TransportMode::OSSM_BLE;
    else {
        resp["ok"] = false;
        resp["error"] = "mode must be WS|SER|BT|DONGLE|OSSM";
        return false;
    }

    _transportMgr.applyTransport(mode);
    ConfigStore::save(_state, _mapper, _motor);

    resp["ok"]   = true;
    resp["mode"] = TransportManager::transportName(_state.getTransport());

    _bumpGen();
    return true;
}

// ============================================================================
// handleCommand — dispatch 0x10 CMD ops from WS control plane
// ============================================================================
// This is called by UiSocket's _handleEvent for each 0x10 frame. The caller
// handles JSON parsing of the payload, then passes the parsed doc here.
// Returns true on success; payload_out always gets "ok" set.

bool WebUI::handleCommand(uint8_t op, JsonDocument& payload_in,
                           JsonDocument& payload_out) {
    switch (op) {
    // ---- Config mutations ------------------------------------------------
    case WS_OP_SET_WINDOW:
    case WS_OP_SET_SPEED:
    case WS_OP_SET_ACCEL:
    case WS_OP_BLEND:
        // These ops all route through applySettings which handles all of them
        return applySettings(payload_in, payload_out);

    case WS_OP_GEN_CFG:
    case WS_OP_GEN_RUN:
        return applyPattern(payload_in, payload_out);

    case WS_OP_MODE:
        return applyMode(payload_in, payload_out);

    // ---- Driver config ---------------------------------------------------
    case WS_OP_CLEAR_FAULT:
        // "clear_fault" same as /api/tmc with save=false and no new params — no-op driver reset
        {
            JsonDocument dummy;
            dummy["reset"] = false;  // don't reset, just apply current (no-op)
            applyDriverConfig(dummy, payload_out);
        }
        return true;

    case WS_OP_SAVE: {
        ConfigStore::save(_state, _mapper, _motor);
        payload_out["ok"] = true;
        _bumpGen();
        return true;
    }

    // ---- Motion commands -------------------------------------------------
    case WS_OP_MOVE:
        return applyMove(payload_in, payload_out);

    case WS_OP_HOME:
        if (!_state.homing_in_progress) {
            _state.homed = false;
            _state.homing_in_progress = true;
        }
        payload_out["ok"] = true;
        _bumpGen();
        return true;

    case WS_OP_HALT:
        if (_motor.isHomed()) _motor.hardStop();
        APPLOG("WS Halt: motor stopped — still homed and ready~ :3");
        payload_out["ok"] = true;
        _bumpGen();
        return true;

    case WS_OP_ESTOP:
        // Same code path as /api/stop (safety parity)
        _state.estop_requested.store(true);
        _state.homed = false;
        _state.homing_in_progress = false;
        _state.paused = false;
        _state.manual_override = false;
        _state.resume_start_ms = 0;
        payload_out["ok"] = true;
        _bumpGen();
        return true;

    case WS_OP_PAUSE: {
        bool wanted = payload_in["paused"] | (!_state.paused);
        bool was = _state.paused;
        _state.paused = wanted;
        if (_state.paused && !was) {
            if (_motor.isHomed()) _motor.hardStop();
        } else if (!_state.paused && was) {
            _state.resume_start_ms = millis();
        }
        payload_out["ok"] = true;
        payload_out["paused"] = _state.paused;
        _bumpGen();
        return true;
    }

    case WS_OP_OVERRIDE: {
        bool wanted = payload_in["on"] | (!_state.manual_override);
        bool was = _state.manual_override;
        _state.manual_override = wanted;
        if (!_state.manual_override && was) {
            _state.resume_start_ms = millis();
        }
        payload_out["ok"] = true;
        payload_out["manual_override"] = _state.manual_override;
        _bumpGen();
        return true;
    }

    case WS_OP_BYPASS: {
        // Bypass limits toggle — stored in payload_out["on"], applied by subsequent moves
        bool val = payload_in["on"] | false;
        payload_out["ok"] = true;
        payload_out["bypass_limits"] = val;
        _bumpGen();
        return true;
    }

    case WS_OP_STREAM_MODE: {
        // v0.4 stream speed-feed A/B: 0=ceiling-pegged, 1=velocity-matched.
        // Core 1's streamSamplerTask reads _state.stream_speed_mode each cruise
        // feed; a plain volatile write is sufficient (single producer here).
        uint8_t m = (uint8_t)(payload_in["mode"] | (int)_state.stream_speed_mode);
        if (m > SystemState::SPEED_VELOCITY_MATCHED) m = SystemState::SPEED_VELOCITY_MATCHED;
        _state.stream_speed_mode = m;
        payload_out["ok"] = true;
        payload_out["stream_speed_mode"] = m;
        _bumpGen();
        return true;
    }

    case WS_OP_OVERSHOOT: {
        // Monotone (Fritsch–Carlson) tangent clamp on the v4 gradient cubic.
        // Core 1's streamSamplerTask pushes _state.interp_clamp_overshoot into
        // the interpolator each tick; a plain volatile write is sufficient.
        bool on = payload_in["on"] | (bool)_state.interp_clamp_overshoot;
        _state.interp_clamp_overshoot = on;
        payload_out["ok"] = true;
        payload_out["overshoot_clamp"] = on;
        _bumpGen();
        return true;
    }

    // ---- Read-only: get_cfg snapshot -------------------------------------
    case WS_OP_GET_CFG: {
        // Full config snapshot — same shape as /api/settings GET
        payload_out["range_min"] = _mapper.getMinMm();
        payload_out["range_max"] = _mapper.getMaxMm();
        payload_out["max_speed"] = (uint32_t)_state.config.max_speed_mm_s;
        payload_out["accel"] = (uint32_t)_state.config.acceleration_mm_s2;
        payload_out["blend_mode"] = _motor.getBlendMode();
        payload_out["auto_duration"] = _state.auto_duration;
        payload_out["intiface_compat"] = _state.intiface_compat;
        payload_out["default_range_min"] = _state.default_range_min;
        payload_out["default_range_max"] = _state.default_range_max;
        payload_out["expert_mode"] = _state.expert_mode;
        payload_out["max_travel"] = (float)MACHINE_MAX_TRAVEL_MM;
        payload_out["measured_stroke"] = _motor.getMeasuredStrokeMm();

        payload_out["speed"]     = (int)roundf(_patternEngine.getSpeedPercent());
        payload_out["depth"]     = (int)roundf(_patternEngine.getDepthPercent());
        payload_out["stroke"]    = (int)roundf(_patternEngine.getStrokePercent());
        payload_out["sensation"] = (int)roundf(_patternEngine.getSensationPercent());
        payload_out["pattern"]   = _patternEngine.getPatternIdx();
        payload_out["rate_tick"] = _state.gen_rate_tick_hz;
        payload_out["running"]   = _patternEngine.isRunning();

        payload_out["paused"] = _state.paused;
        payload_out["manual_override"] = _state.manual_override;
        payload_out["transport"] = TransportManager::transportName(_state.getTransport());
        payload_out["homed"] = _state.homed;
        payload_out["stream_speed_mode"] = (uint8_t)_state.stream_speed_mode;
        payload_out["overshoot_clamp"] = (bool)_state.interp_clamp_overshoot;
        payload_out["ok"] = true;
        // No cfg_gen bump for reads
        return true;
    }

    default:
        payload_out["ok"] = false;
        payload_out["error"] = "Unknown op";
        return false;
    }
}