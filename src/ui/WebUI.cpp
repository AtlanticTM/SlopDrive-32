#include "WebUI.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_timer.h>

#if defined(FEATURE_RS485_MODBUS)
#include "ServoModbus.h"
#if defined(DRIVER_AIM_SERVO)
#include "EncoderValidator.h"
#endif
#endif

#include "AppLog.h"
#include "ConfigStore.h"
#include "MachineConfig.h"
#include "SlopGlowBoard.h"
#include "PatternEngine.h"
#include "MotorDriver.h"
#include "UiSocket.h"

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
    _httpServer->on("/api/settings",  HTTP_POST, [this]() { slopglowActivity(); handleApiSettings(); });
    _httpServer->on("/api/move",      HTTP_POST, [this]() { slopglowActivity(); handleApiMove(); });
    _httpServer->on("/api/home",      HTTP_POST, [this]() { slopglowActivity(); handleApiHome(); });
    _httpServer->on("/api/stop",      HTTP_POST, [this]() { slopglowActivity(); handleApiStop(); });
    _httpServer->on("/api/pause",     HTTP_POST, [this]() { slopglowActivity(); handleApiPause(); });
    _httpServer->on("/api/halt",      HTTP_POST, [this]() { slopglowActivity(); handleApiHalt(); });
    _httpServer->on("/api/override",  HTTP_POST, [this]() { slopglowActivity(); handleApiOverride(); });
    _httpServer->on("/api/tmc",       HTTP_GET,  [this]() { handleApiTmc(); });
    _httpServer->on("/api/tmc",       HTTP_POST, [this]() { slopglowActivity(); handleApiTmc(); });
    _httpServer->on("/api/servo",     HTTP_GET,  [this]() { handleApiServo(); });
    _httpServer->on("/api/servo",     HTTP_POST, [this]() { slopglowActivity(); handleApiServo(); });
    _httpServer->on("/api/clearfault",HTTP_POST, [this]() { slopglowActivity(); handleApiClearFault(); });
    _httpServer->on("/api/pattern",   HTTP_GET,  [this]() { handleApiPattern(); });
    _httpServer->on("/api/pattern",   HTTP_POST, [this]() { slopglowActivity(); handleApiPattern(); });
    _httpServer->on("/api/pattern/presets", HTTP_GET,  [this]() { handleApiPatternPresets(); });
    _httpServer->on("/api/pattern/presets", HTTP_POST, [this]() { slopglowActivity(); handleApiPatternPresets(); });
    _httpServer->on("/api/log",       HTTP_GET,  [this]() { handleApiLog(); });
    _httpServer->on("/api/mode",      HTTP_GET,  [this]() { handleApiMode(); });
    _httpServer->on("/api/mode",      HTTP_POST, [this]() { handleApiMode(); });
    _httpServer->on("/api/clients",   HTTP_GET,  [this]() { handleApiClients(); });
    _httpServer->on("/api/clients",   HTTP_POST, [this]() { slopglowActivity(); handleApiClients(); });
    _httpServer->on("/api/machine",        HTTP_GET,  [this]() { handleApiMachine(); });
    _httpServer->on("/api/machine/commit", HTTP_POST, [this]() { slopglowActivity(); handleApiMachineCommit(); });
    _httpServer->on("/api/machine/homeoverride", HTTP_POST, [this]() { slopglowActivity(); handleApiHomeOverride(); });

    _httpServer->begin();
    APPLOGF("HTTP server on port %d", HTTP_PORT);

    startTelemetrySampler();
}

// ============================================================================
// update()
// ============================================================================

void WebUI::update() {
    _httpServer->handleClient();

    // Deferred reboot for the machine-backend commit: the HTTP handler arms
    // this and returns immediately so its 200 response actually flushes to
    // the browser before the device goes down.
    _machineReboot.poll();
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
        _state.session_start_ms = millis();   // stamp the session odometer clock
        APPLOG("Telemetry sampler armed — sampling the shaft at 240Hz :3");
    } else {
        APPLOG("Telemetry sampler FAILED to arm — graph will be limp. :<");
    }
}

void WebUI::resetSessionStats() {
    _state.live_speed_mm_s.store(0.0f, std::memory_order_relaxed);
    _state.max_speed_mm_s.store(0.0f, std::memory_order_relaxed);
    _state.session_distance_mm.store(0.0f, std::memory_order_relaxed);
    _state.stroke_count.store(0, std::memory_order_relaxed);
    _state.session_start_ms = millis();
    _motor.resetPowerStats();   // zero the INA228 Wh accumulator + software peaks
    APPLOG("Session stats reset :3");
}

void WebUI::captureTelemetry(float position_mm, float target_mm, float raw_mm) {
    // ---- Session odometer stats (single-writer: this 240Hz timer task) --------
    // Derive live/peak speed, accumulate distance, and count strokes (direction
    // reversals) straight from the position stream. Cheap float math; publishes
    // to SystemState atomics that the 0x06 STATS frame + SESSION card read. :3
    {
        static float    last_pos_mm = position_mm;
        static uint32_t last_us     = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFu);
        static float    spd_ema     = 0.0f;
        static int8_t   last_dir    = 0;
        uint32_t now_us = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFu);
        float dt = (float)(now_us - last_us) * 1e-6f;      // seconds
        if (dt > 1e-4f && dt < 1.0f) {                     // ignore stalls/wraps
            float dpos  = position_mm - last_pos_mm;
            float adpos = fabsf(dpos);
            if (adpos > 0.002f) {                          // ignore sub-2µm jitter
                _state.session_distance_mm.store(
                    _state.session_distance_mm.load(std::memory_order_relaxed) + adpos,
                    std::memory_order_relaxed);
            }
            // Speed only on full-width ticks: when the esp_timer fires late and
            // then bursts, the catch-up callback arrives with a sub-ms dt while
            // dpos stays step-quantized (~0.049mm/step) — inst comes out 10%+
            // high and the session PEAK ratchets above the real dispatch
            // ceiling (a stat the UI must never overstate). Nominal tick is
            // 4.17ms; anything under ~2.5ms is a burst artifact, skip it. :3
            if (dt > 2.5e-3f) {
                float inst = adpos / dt;                    // instantaneous mm/s
                spd_ema += 0.25f * (inst - spd_ema);        // ~17ms time constant
                _state.live_speed_mm_s.store(spd_ema, std::memory_order_relaxed);
                if (spd_ema > _state.max_speed_mm_s.load(std::memory_order_relaxed))
                    _state.max_speed_mm_s.store(spd_ema, std::memory_order_relaxed);
                // A "stroke" = a direction reversal with meaningful travel.
                int8_t dir = (dpos > 0.05f) ? 1 : (dpos < -0.05f) ? -1 : last_dir;
                if (dir != 0 && last_dir != 0 && dir != last_dir)
                    _state.stroke_count.fetch_add(1, std::memory_order_relaxed);
                last_dir = dir;
            }
        }
        last_pos_mm = position_mm;
        last_us     = now_us;
    }

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
        doc["energy_wh"] = _motor.getBusEnergyWh();
    }

    // Session odometer stats (also carried in the 0x06 STATS WS frame).
    doc["live_speed_mm_s"]  = _state.live_speed_mm_s.load(std::memory_order_relaxed);
    doc["max_speed_mm_s"]   = _state.max_speed_mm_s.load(std::memory_order_relaxed);
    doc["distance_mm"]      = _state.session_distance_mm.load(std::memory_order_relaxed);
    doc["strokes"]          = _state.stroke_count.load(std::memory_order_relaxed);
    doc["session_ms"]       = (uint32_t)(millis() - _state.session_start_ms);

    uint16_t hz = _state.measured_hz;
    doc["measured_hz"] = hz;
    doc["measured_interval_ms"] = (hz > 0) ? (uint16_t)(1000 / hz) : 0;
    doc["auto_duration"] = _state.auto_duration;
    doc["measured_stroke_mm"] = (_state.test_stroke_override_mm > 0.0f)
                                ? _state.test_stroke_override_mm
                                : _motor.getMeasuredStrokeMm();
    doc["home_override"] = (_state.test_stroke_override_mm > 0.0f);
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
    doc["estopped"] = (bool)_state.estop_latched;   // latched e-stop state for the fallback poll

    // ---- Motion-generation diagnostics (D4: intent rate + plan dynamics) ----
    if (_arbiter) {
        PlanReport rpt = _arbiter->lastReport();
        doc["intent_count"] = _arbiter->totalIntents();
        doc["intent_rejected"] = _arbiter->rejectedIntents();
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

    // Driver-health block: NO live fault readback exists on either driver (TMC
    // DRV_STATUS polling was removed; the AIM drive exposes none over step/dir).
    // Report that honestly instead of a hardcoded all-clear (otpw/ot/s2g/faulted
    // all false) that would show "no fault" during a real overtemperature or
    // short-to-ground event. valid:false = this block carries no live data. :3
    JsonObject drv = doc["driver"].to<JsonObject>();
    drv["supported"] = false;
    drv["valid"]     = false;

    String json;
    serializeJson(doc, json);
    _httpServer->send(200, "application/json", json);
}

void WebUI::handleApiCapabilities() {
    JsonDocument doc;
    doc["fw_version"] = FIRMWARE_VERSION;   // OTA verification: prove which build is live
    // Rail-length agnostic: max_travel_mm is the user-configured max rail length
    // (the pre-homing scale + homing sweep bound), not a fixed geometry ceiling.
    doc["max_travel_mm"] = _state.config.max_rail_mm;
    doc["max_rail_mm"]   = _state.config.max_rail_mm;
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
    // Advanced pattern mode (fray-d port) — the UI builds the Advanced/Classic
    // pattern card split only when the firmware actually has the engine.
    feat["advanced_pattern"] = true;

    // SlopSync hub — ecosystem clients discover the sync plane from here:
    // binary WS on its own port, protocol id per docs/slopsync/SPEC.md.
    feat["slopsync"] = true;
    doc["slopsync_port"]  = (uint16_t)SLOPSYNC_WS_PORT;
    doc["slopsync_proto"] = "slopsync/1";

    // Phase 2 — runtime motion backend. _machine_backend mirrors whatever
    // main.cpp actually bound the MotorProxy to (Ground Truth: NOT re-read
    // from NVS here — this is the live-applied value, which for the FIRST
    // read after a commit is intentionally the pre-reboot value until the
    // device actually restarts). available_backends tells the UI whether the
    // toggle should even be offered. home_style is read live from NVS since
    // it's not reboot-gated (Phase 4 wires its actual effect). :3
    feat["motion_backend"] = (_machine_backend == 1) ? "modbus" : "fas";
    JsonArray backends = feat["available_backends"].to<JsonArray>();
    backends.add("fas");
#if defined(FEATURE_RS485_MODBUS)
    backends.add("modbus");
#endif
    feat["home_style"] = machineHomeStyleLoad();

    String json;
    serializeJson(doc, json);
    _httpServer->send(200, "application/json", json);
}

void WebUI::handleApiClearFault() {
    // No driver fault readback exists on this build — there is no fault state
    // to clear and no way to verify a clear took effect. Say so explicitly
    // (cleared:false) instead of an unqualified ok that implies a fault was
    // observed and cleared. :3
    APPLOG("Clear-fault requested — no driver fault readback on this build (nothing to clear/verify)");
    _httpServer->send(200, "application/json",
                      "{\"ok\":true,\"cleared\":false,\"reason\":\"no_fault_readback\"}");
}

// ============================================================================
// handleApiSettings (HTTP GET + POST) — delegates to applySettings for mutations
// ============================================================================

void WebUI::handleApiSettings() {
    if (_httpServer->method() == HTTP_GET) {
        JsonDocument doc;
        doc["range_min"] = _mapper.getMinMm();
        doc["range_max"] = _mapper.getMaxMm();
        // Ground truth: speed + accel read back from the DRIVER (post its
        // internal clamps), never the raw config request. :3
        doc["max_speed"] = (uint32_t)_motor.getMaxSpeed();
        doc["accel"] = (uint32_t)_motor.getAcceleration();
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
        // max_travel = pre-homing rail scale (= configured max rail length);
        // max_rail is the explicit setting the WebUI edits. :3
        doc["max_travel"] = _state.config.max_rail_mm;
        doc["max_rail"] = _state.config.max_rail_mm;
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
    // Session-odometer reset — the SESSION card's reset button posts this
    // (with no_persist). Handle it up front so a reset-only POST works. :3
    if (doc["reset_stats"] | false) resetSessionStats();

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

    // Stream speed-feed mode + overshoot clamp — accepted here too so the WS
    // ops (WS_OP_STREAM_MODE / WS_OP_OVERSHOOT) have a working HTTP-fallback
    // route. Session-only volatile state, same semantics as the WS ops. :3
    if (doc["stream_speed_mode"].is<int>()) {
        uint8_t m = (uint8_t)(doc["stream_speed_mode"] | (int)_state.stream_speed_mode);
        if (m > SystemState::SPEED_VELOCITY_MATCHED) m = SystemState::SPEED_VELOCITY_MATCHED;
        _state.stream_speed_mode = m;
    }
    if (doc["overshoot_clamp"].is<bool>()) {
        _state.interp_clamp_overshoot = (bool)(doc["overshoot_clamp"] | (bool)_state.interp_clamp_overshoot);
    }

    // Max rail length (mm) — rail-length-agnostic ceiling. Apply BEFORE the
    // window/default-range clamps below so they validate against the new rail.
    // Sanity-bound 10..2000mm. Pushed live to the motor + mapper. :3
    if (doc["max_rail"].is<float>() || doc["max_rail"].is<int>()) {
        float rail = doc["max_rail"] | _state.config.max_rail_mm;
        if (rail < 10.0f)   rail = 10.0f;
        if (rail > 2000.0f) rail = 2000.0f;
        _state.config.max_rail_mm = rail;
        _motor.setMaxRailMm(rail);
        _mapper.setMaxRailMm(rail);   // re-clamps the current window to the new rail
    }

    if (doc["default_range_min"].is<float>() || doc["default_range_max"].is<float>()) {
        float drmin = doc["default_range_min"] | _state.default_range_min;
        float drmax = doc["default_range_max"] | _state.default_range_max;
        if (drmin >= 0.0f && drmax <= _state.config.max_rail_mm && drmin < drmax) {
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

    // Echo post-clamp values — BOTH speed and accel read back from the driver.
    // The driver hard-clamps accel lower (20000) than config_api.h's ceiling
    // (100000); echoing the config value here reported a number the motor was
    // never going to run at. Ground Truth Doctrine: echo what was APPLIED. :3
    resp["ok"] = true;
    resp["range_min"] = _mapper.getMinMm();
    resp["range_max"] = _mapper.getMaxMm();
    resp["max_rail"] = _state.config.max_rail_mm;   // post-clamp rail length echo
    resp["max_speed"] = (uint32_t)_motor.getMaxSpeed();
    resp["accel"] = (uint32_t)_motor.getAcceleration();
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
    resp["stream_speed_mode"] = (uint8_t)_state.stream_speed_mode;
    resp["overshoot_clamp"] = (bool)_state.interp_clamp_overshoot;

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
    // Per-request bypass wins; otherwise honor the stored WS_OP_BYPASS state so
    // the toggle actually does what its echo claims. :3
    bool bypass = doc["bypass_limits"] | (bool)_state.bypass_limits;
    if (bypass) {
        // Bypass the window but NOT the machine: clamp to the effective physical
        // ceiling (measured stroke once homed, else configured max rail). :3
        pos = constrain(pos, 0.0f, _motor.effectiveCeilingMm());
    } else {
        pos = constrain(pos, _mapper.getMinMm(), _mapper.getMaxMm());
    }
    bool stream = doc["stream"] | true;

    // Sole-Caller doctrine (CLAUDE.md §2): the UI is an input source — it MUST
    // submit intents to the MotionArbiter, never call the driver directly. This
    // is also what makes a manual move honor the USER speed/accel limit set: a
    // MANUAL point move (deadline 0) plans AT the user ceiling instead of lunging
    // at the driver's raw max_speed. MANUAL bypasses the window inside the arbiter
    // (already clamped above per bypass_limits), and bypasses homed/pause gates.
    // Deferred push: applyMove runs on Core 0; the arbiter dispatches to FAS on
    // Core 1 (drained by processDeferred every ~1ms). :3
    if (_arbiter) {
        MotionIntent intent = {};
        intent.source          = MotionSource::MANUAL;
        intent.target_mm       = pos;     // already clamped to window / ceiling above
        intent.deadline_ms     = 0;       // point move → plan at USER ceilings
        intent.speed_hint_mm_s = 0.0f;
        intent.seq             = 0;
        _arbiter->submitDeferred(intent);
    } else {
        // No direct-driver fallback — the sole-caller rule is compile-enforced
        // now (MotorDriver motion methods are arbiter-only). An unwired arbiter
        // is a boot-order bug; refuse loudly instead of bypassing every gate. :3
        APPLOG("applyMove REFUSED: MotionArbiter not wired — no motion dispatched");
        resp["ok"] = false;
        resp["error"] = "Motion arbiter unavailable";
        return false;
    }

    // Seed the "where the shaft is" atomic the stream rising-edge reads
    // (main.cpp) so a stream started right after a manual move begins from the
    // manual endpoint, not a stale sample. The live posdot/readout reads
    // _motor.getPosition() directly, so this is only the stream-seed hint. :3
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
        _state.estop_latched = false;   // a fresh homing cycle exits the e-stopped state
        _state.resume_start_ms = millis();   // arm soft-start NOW so the first post-rehome move doesn't lunge (F-003)
    }
    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiStop() {
    _state.estop_requested.store(true);
    _state.estop_latched = true;   // latched for telemetry — UI fault banner rises from this
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
        if (_motor.isHomed() && _arbiter) _arbiter->hardStopMotion();
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
    if (_motor.isHomed() && _arbiter) _arbiter->hardStopMotion();
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
// handleApiClients (HTTP GET list / POST kick) — Health-tab client admin
// ============================================================================
//
// GET  /api/clients            → {clients:[{num, ip, idle_ms, streaming,
//                                            most_recent}], max, active_window_ms}
// POST /api/clients  {kick:N}   → force-disconnect client slot N (reclaim slot).
//
// "streaming" = passes the activity gate right now; "most_recent" = the single
// always-live last-active client. A muted (streaming:false) tab is the
// forgotten one costing you nothing — kick it only if you want its slot back.

void WebUI::handleApiClients() {
    if (!_uiSocket) {
        _httpServer->send(503, "application/json", "{\"error\":\"no UiSocket\"}");
        return;
    }

    // POST → kick
    if (_httpServer->method() == HTTP_POST) {
        JsonDocument doc;
        deserializeJson(doc, _httpServer->arg("plain"));
        if (!doc["kick"].is<int>()) {
            _httpServer->send(400, "application/json", "{\"error\":\"kick (client num) required\"}");
            return;
        }
        int num = doc["kick"].as<int>();
        bool ok = (num >= 0 && num < UiSocket::MAX_CLIENTS)
                    ? _uiSocket->kickClient((uint8_t)num) : false;
        String json; JsonDocument r; r["ok"] = ok; r["kicked"] = num;
        serializeJson(r, json);
        _httpServer->send(ok ? 200 : 404, "application/json", json);
        return;
    }

    // GET → list
    UiSocket::ClientInfo info[UiSocket::MAX_CLIENTS];
    uint8_t n = _uiSocket->enumerateClients(info, UiSocket::MAX_CLIENTS);

    JsonDocument r;
    r["max"] = UiSocket::MAX_CLIENTS;
    r["active_window_ms"] = UiSocket::CLIENT_ACTIVE_WINDOW_MS;
    JsonArray arr = r["clients"].to<JsonArray>();
    for (uint8_t i = 0; i < n; i++) {
        JsonObject c = arr.add<JsonObject>();
        c["num"]         = info[i].num;
        char ips[16];
        snprintf(ips, sizeof(ips), "%u.%u.%u.%u",
                 info[i].ip[0], info[i].ip[1], info[i].ip[2], info[i].ip[3]);
        c["ip"]          = ips;
        c["idle_ms"]     = info[i].idle_ms;
        c["streaming"]   = info[i].streaming;
        c["most_recent"] = info[i].most_recent;
    }

    String json;
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
// handleApiServo — AIM servo drive over RS485 Modbus (Configure pane + card)
// ============================================================================
//
// GET  → live telemetry snapshot + config-register mirror + runtime geometry.
// POST → {"scan":true}                    start async register scan
//        {"live":{"<reg>":val,...}}       live-tune whitelist writes (×2 each)
//        {"program":{...},"save":bool}    full gold-motor sequence (idle-only)
//        {"raw":{"reg":N,"val":V}}        single arbitrary write (expert)
//        {"save":true}                    persist drive params (reg 0x14)
//        {"output":bool}                  drive output enable toggle
//
// The program sequence mirrors the proven OSSM gold-motor tool: modbus-enable,
// output OFF, write settings ×3 (serial-reliability workaround), output
// restore, save-to-flash, then verify by rescan (Ground Truth — the UI adopts
// the mirror, never its own request). Writing steps/rev (reg 0x0B) recalcs the
// firmware's steps/mm LIVE and forces a re-home: the step<->mm meaning of the
// position reference is void across an electronic-gear change. No reboot. :3

#if defined(FEATURE_RS485_MODBUS)
// Live-tunable while running: speed/accel ceilings, loop gains, feed-forward,
// max output. These never move the motor and don't touch the gear train.
static bool servoIsLiveReg(uint16_t r) {
    return r == 0x02 || r == 0x03 || r == 0x05 || r == 0x06 || r == 0x07 ||
           r == 0x08 || r == 0x18;
}
// Programmable via the guarded sequence: live set + field-weakening, DIR
// polarity, e-gear pair, device address. NEVER 0x0C/0x0D (position) or 0x14
// (save flag — managed by the sequence itself).
static bool servoIsProgReg(uint16_t r) {
    return servoIsLiveReg(r) || r == 0x04 || r == 0x09 || r == 0x0A ||
           r == 0x0B || r == 0x15;
}
#endif

void WebUI::handleApiServo() {
#if !defined(FEATURE_RS485_MODBUS)
    _httpServer->send(404, "application/json", "{\"error\":\"no_rs485\"}");
#else
    if (!_servoModbus) {
        _httpServer->send(404, "application/json", "{\"error\":\"no_rs485\"}");
        return;
    }

    if (_httpServer->method() == HTTP_GET) {
        JsonDocument doc;
        doc["ready"] = _servoModbus->isReady();
        doc["addr"]  = _servoModbus->address();
        doc["queue"] = (uint32_t)_servoModbus->pendingWrites();

        ServoBusHealth bus = _servoModbus->getBusHealth();
        JsonObject busObj = doc["bus"].to<JsonObject>();
        busObj["baud"]           = bus.baud;
        busObj["sp_fail_streak"] = bus.sp_fail_streak;
        busObj["sp_sent"]        = bus.sp_sent;
        busObj["sp_ok"]          = bus.sp_ok;
        busObj["sp_fc"]          = _servoModbus->setpointFc();
        busObj["sp_le"]          = _servoModbus->setpointLe();
        busObj["sp_noecho"]      = _servoModbus->setpointNoEcho();

        ServoTelemetry t = _servoModbus->getTelemetry();
        JsonObject tele = doc["tele"].to<JsonObject>();
        tele["valid"]     = t.valid;
        tele["enabled"]   = t.enabled;
        tele["output_on"] = t.output_on;
        tele["alarm"]     = t.alarm;
        tele["current_a"] = t.current_a;
        tele["speed_rpm"] = t.speed_rpm;
        tele["voltage_v"] = t.voltage_v;
        tele["temp_c"]    = t.temp_c;
        tele["pwm_pct"]   = t.pwm_pct;

        ServoConfig c = _servoModbus->getConfig();
        JsonObject cfg = doc["cfg"].to<JsonObject>();
        cfg["valid"]    = c.valid;
        cfg["scanning"] = c.scanning;
        cfg["age_ms"]   = c.valid ? (uint32_t)(millis() - c.stamp_ms) : 0;
        cfg["known"]    = c.known;
        JsonArray regs = cfg["regs"].to<JsonArray>();
        for (size_t i = 0; i < ServoModbus::CFG_REG_COUNT; i++) regs.add(c.regs[i]);

        JsonObject g = doc["geom"].to<JsonObject>();
#if defined(DRIVER_AIM_SERVO)
        g["motor_steps_per_rev"] = aimMotorStepsPerRev();
        g["steps_per_mm"]        = aimStepsPerMm();
#endif
        g["homed"] = _state.homed;

#if defined(DRIVER_AIM_SERVO)
        // FAS-vs-encoder cross-check (report-only). dev_mm is noisy while
        // moving (Modbus timing skew); steady/max/warn are standstill-scored.
        if (_encValidator) {
            const EncoderValidation& ev = _encValidator->get();
            JsonObject enc = doc["enc"].to<JsonObject>();
            enc["valid"]      = t.enc_valid;
            enc["counts"]     = t.enc_counts;
            enc["age_ms"]     = t.enc_valid ? (uint32_t)(millis() - t.enc_stamp_ms) : 0;
            enc["state"]      = ev.state;      // 0 idle · 1 sign-detect · 2 tracking
            enc["have_dev"]   = ev.have_dev;
            enc["dev_mm"]     = ev.dev_mm;
            enc["dev_steady_mm"] = ev.dev_steady_mm;
            enc["max_steady_mm"] = ev.max_steady_mm;
            enc["sign"]       = ev.sign;
            enc["cpmm_meas"]  = ev.cpmm_meas;
            enc["cpmm_theory"] = (float)AIM_ENC_COUNTS_PER_MM;
            enc["warn"]       = ev.warn;
        }
#endif

        String json;
        serializeJson(doc, json);
        _httpServer->send(200, "application/json", json);
        return;
    }

    // ---- POST ----------------------------------------------------------------
    JsonDocument doc;
    if (deserializeJson(doc, _httpServer->arg("plain"))) {
        _httpServer->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    JsonDocument resp;
    resp["ok"] = true;
    bool rehome = false;

    if (doc["scan"] | false) _servoModbus->requestConfigScan();

    // ---- Setpoint-framing bench knobs (Modbus motion bring-up) ---------------
    // POST {"sp_fc": 0x78, "sp_le": false} — retunes the motion setpoint frame
    // LIVE (no reflash) so the bench can hunt this drive variant's real "write
    // target position" framing. The executor's keep-alive stream immediately
    // starts using the new shape; watch bus.sp_ok in the GET response. :3
    if (doc["sp_fc"].is<int>() || doc["sp_le"].is<bool>()) {
        uint8_t fc = doc["sp_fc"] | (int)_servoModbus->setpointFc();
        bool    le = doc["sp_le"] | _servoModbus->setpointLe();
        _servoModbus->setSetpointFraming(fc, le);
    }
    // {"sp_noecho": true} — BENCH ONLY: fire-and-forget setpoints, watchdog
    // blind. For discovering whether the drive executes position frames it
    // never echoes. Operator hand on the power switch. :3
    if (doc["sp_noecho"].is<bool>()) {
        _servoModbus->setSetpointNoEcho(doc["sp_noecho"].as<bool>());
    }
    // {"bench_pair":{"val":100,"low_first":true}} — BENCH ONLY: one atomic
    // FC 0x10 write of a 32-bit value to the position pair 0x0C/0x0D
    // (torn-half-protection hypothesis). Keep |val| tiny (~100 counts). :3
    if (doc["bench_pair"].is<JsonObject>()) {
        int32_t v  = doc["bench_pair"]["val"] | 0;
        bool    lf = doc["bench_pair"]["low_first"] | true;
        resp["bench_pair_queued"] = _servoModbus->queuePositionPair(v, lf);
    }
    // {"sp_period_ms": 8} — BENCH: live A/B of the delta-stream cadence
    // (clamped 4..50ms inside ServoModbus). :3
    if (doc["sp_period_ms"].is<int>()) {
        _servoModbus->setSpPeriodMs((uint8_t)doc["sp_period_ms"].as<int>());
        APPLOGF("ServoBench: sp_period_ms -> %u", (unsigned)_servoModbus->spPeriodMs());
    }

    if (doc["output"].is<bool>()) {
        _servoModbus->queueWrite(0x00, 1, 1);
        _servoModbus->queueWrite(0x01, doc["output"].as<bool>() ? 1 : 0, 1);
    }

    if (doc["live"].is<JsonObject>()) {
        JsonObject live = doc["live"];
        _servoModbus->queueWrite(0x00, 1, 1);   // writes need Modbus enabled
        for (JsonPair kv : live) {
            uint16_t reg = (uint16_t)strtoul(kv.key().c_str(), nullptr, 0);
            if (!servoIsLiveReg(reg)) {
                resp["ok"] = false;
                resp["error"] = "reg_not_live";
                resp["reg"] = reg;
                continue;
            }
            uint16_t val = (uint16_t)(kv.value().as<uint32_t>() & 0xFFFF);
            _servoModbus->queueWrite(reg, val, 2);
        }
        _servoModbus->requestConfigScan();      // verify by readback
    }

    if (doc["raw"].is<JsonObject>()) {
        uint16_t reg = doc["raw"]["reg"] | 0xFFFF;
        uint16_t val = doc["raw"]["val"] | 0;
        // Position regs never; structural regs only via "program" (sequence +
        // geometry recalc + forced re-home) so a raw poke can't silently
        // desync steps/mm or the bus address.
        // BENCH EXCEPTION: {"bench_pos":true} alongside "raw" unblocks the
        // position pair 0x0C/0x0D ONLY — for the operator-present trigger-
        // discovery experiment (which register write fires an incremental
        // move on this drive variant, since 0x7B/0x78 proved absent). Keep
        // experimental deltas tiny (±100 counts ≈ 0.12mm). :3
        bool bench_pos = doc["bench_pos"] | false;
        bool pos_reg   = (reg == 0x0C || reg == 0x0D);
        if (reg >= ServoModbus::CFG_REG_COUNT || (pos_reg && !bench_pos) ||
            reg == 0x09 || reg == 0x0A || reg == 0x0B || reg == 0x15) {
            resp["ok"] = false;
            resp["error"] = "raw_reg_blocked";
        } else {
            _servoModbus->queueWrite(0x00, 1, 1);
            _servoModbus->queueWrite(reg, val, 1);
            _servoModbus->requestConfigScan();
        }
    }

    if (doc["program"].is<JsonObject>()) {
        bool busy = _state.pattern_running || _state.homing_in_progress || _motor.isMoving();
        if (busy) {
            resp["ok"] = false;
            resp["error"] = "machine_busy";
        } else {
            JsonObject prog = doc["program"];
            // Validate the WHOLE batch first — rejecting mid-sequence would
            // leave the drive half-programmed with output disabled.
            bool valid = true;
            uint16_t bad_reg = 0;
            for (JsonPair kv : prog) {
                uint16_t reg = (uint16_t)strtoul(kv.key().c_str(), nullptr, 0);
                uint32_t val = kv.value().as<uint32_t>();
                if (!servoIsProgReg(reg) || val > 0xFFFF) { valid = false; bad_reg = reg; break; }
                // Steps/rev sanity band — mirrors MotionGeometry's clamp, but
                // reject instead of silently clamping (drive + firmware MUST
                // agree exactly, or every commanded mm is a lie).
                if (reg == 0x0B && (val < 50 || val > 32767)) { valid = false; bad_reg = reg; break; }
            }
            if (!valid) {
                resp["ok"] = false;
                resp["error"] = "bad_program_reg";
                resp["reg"] = bad_reg;
            } else {
                ServoConfig c = _servoModbus->getConfig();
                // Restore whatever output-enable value the drive had (gold
                // register doc says some variants use 7 = max, not 1).
                uint16_t restore_output =
                    (c.valid && (c.known & (1UL << 1)) && c.regs[1] != 0) ? c.regs[1] : 1;
                bool save = doc["save"] | true;   // structural changes default to persist

                _servoModbus->queueWrite(0x00, 1, 1);           // modbus enable
                _servoModbus->queueWrite(0x01, 0, 2);           // output OFF during config
                for (JsonPair kv : prog) {
                    uint16_t reg = (uint16_t)strtoul(kv.key().c_str(), nullptr, 0);
                    uint16_t val = (uint16_t)(kv.value().as<uint32_t>() & 0xFFFF);
                    _servoModbus->queueWrite(reg, val, 3);      // gold tool writes ×3
                    if (reg == 0x0B) {
#if defined(DRIVER_AIM_SERVO)
                        // LIVE steps/mm recalc + NVS persist — the whole point.
                        aimSetMotorStepsPerRev(val, true);
#endif
                        rehome = true;
                    }
                    if (reg == 0x09) rehome = true;             // direction flip → reference void
                }
                _servoModbus->queueWrite(0x01, restore_output, 2);
                if (save) _servoModbus->queueWrite(0x14, 1, 1); // persist in drive EEPROM
                _servoModbus->requestConfigScan();              // verify by readback

                if (rehome) {
                    _state.homed = false;
                    _motor.forceHomeState(false);
                    APPLOG("ServoProgram: structural change applied — machine UNHOMED, re-home before motion");
                }
            }
        }
    }

    if ((doc["save"] | false) && !doc["program"].is<JsonObject>()) {
        _servoModbus->queueWrite(0x00, 1, 1);
        _servoModbus->queueWrite(0x14, 1, 1);
    }

    resp["queued"] = (uint32_t)_servoModbus->pendingWrites();
    resp["rehome_required"] = rehome;
#if defined(DRIVER_AIM_SERVO)
    resp["motor_steps_per_rev"] = aimMotorStepsPerRev();
    resp["steps_per_mm"]        = aimStepsPerMm();
#endif
    String json;
    serializeJson(resp, json);
    _httpServer->send(200, "application/json", json);
#endif // FEATURE_RS485_MODBUS
}

// ============================================================================
// handleApiPattern (HTTP GET + POST) — delegates to applyPattern
// ============================================================================

// Advanced-mode readback (post-clamp device truth — Ground Truth Doctrine).
// Base values always; the six modifier blocks only when include_mods (the GET
// page-load adoption path — echoes stay lean and only carry the touched one).
static void apStateToJson(const PatternEngine& pe, JsonDocument& doc, bool include_mods) {
    doc["ap_mode"]      = pe.isAdvancedMode();
    doc["ap_speed"]     = pe.getApMaster();
    doc["ap_max_depth"] = pe.getApBase(advpat::DEPTH_MAX);
    doc["ap_min_depth"] = pe.getApBase(advpat::DEPTH_MIN);
    doc["ap_in_speed"]  = pe.getApBase(advpat::SPEED_IN);
    doc["ap_out_speed"] = pe.getApBase(advpat::SPEED_OUT);
    doc["ap_in_accel"]  = pe.getApBase(advpat::ACCEL_IN);
    doc["ap_out_accel"] = pe.getApBase(advpat::ACCEL_OUT);
    if (include_mods) {
        JsonArray mods = doc["ap_mods"].to<JsonArray>();
        for (uint8_t id = 0; id < advpat::BASE_COUNT; id++) {
            const advpat::BaseControl* c = pe.apSettings().byId(id);
            if (!c) continue;
            JsonObject m = mods.add<JsonObject>();
            m["ctrl"]      = id;
            m["amplitude"] = (int)c->modifier.amplitude;
            m["in_step"]   = (int)c->modifier.in_step;
            m["in_wait"]   = (int)c->modifier.in_wait;
            m["out_step"]  = (int)c->modifier.out_step;
            m["out_wait"]  = (int)c->modifier.out_wait;
            m["offset"]    = (int)c->modifier.offset;
        }
    }
}

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
        apStateToJson(_patternEngine, doc, true);
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
// handleApiPatternPresets — NVS-backed user-preset store (fray-d port)
// ============================================================================
//
// One NVS string key "list" in namespace "advpreset" holds a JSON array of
// {name, def} objects. `def` is the opaque advanced-mode snapshot the UI
// assembled (in/out speed, in/out accel, and the six modifier blocks — never
// depths or master speed, matching the factory-preset model). The firmware
// stores/echoes it verbatim; the UI is the sole interpreter, so the preset
// schema can evolve without a firmware change. Bounded so a runaway client
// can't exhaust NVS.

static constexpr size_t AP_PRESET_STORE_BUDGET = 3600;  // bytes of serialized JSON
static constexpr int    AP_PRESET_MAX_COUNT    = 24;

void WebUI::handleApiPatternPresets() {
    Preferences prefs;

    if (_httpServer->method() == HTTP_GET) {
        String stored;
        if (prefs.begin("advpreset", true)) {          // read-only
            stored = prefs.getString("list", "[]");
            prefs.end();
        } else {
            stored = "[]";
        }
        JsonDocument doc;
        JsonObject root = doc.to<JsonObject>();
        JsonDocument listDoc;
        if (deserializeJson(listDoc, stored) || !listDoc.is<JsonArray>())
            listDoc.to<JsonArray>();                    // corrupt/empty → []
        root["presets"] = listDoc.as<JsonArray>();
        String json;
        serializeJson(doc, json);
        _httpServer->send(200, "application/json", json);
        return;
    }

    // ---- POST: save or delete -------------------------------------------------
    JsonDocument body;
    if (deserializeJson(body, _httpServer->arg("plain"))) {
        _httpServer->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
        return;
    }
    String name = body["name"] | "";
    name.trim();
    if (name.length() == 0 || name.length() > 40) {
        _httpServer->send(400, "application/json", "{\"ok\":false,\"error\":\"Name required (1-40 chars)\"}");
        return;
    }

    // Load current list (read-write session).
    if (!prefs.begin("advpreset", false)) {
        _httpServer->send(500, "application/json", "{\"ok\":false,\"error\":\"NVS unavailable\"}");
        return;
    }
    String stored = prefs.getString("list", "[]");
    JsonDocument listDoc;
    if (deserializeJson(listDoc, stored) || !listDoc.is<JsonArray>())
        listDoc.to<JsonArray>();
    JsonArray arr = listDoc.as<JsonArray>();

    // Remove any existing entry with this name (save = overwrite; delete = drop).
    for (int i = (int)arr.size() - 1; i >= 0; i--) {
        if (name == (const char*)(arr[i]["name"] | "")) arr.remove(i);
    }

    bool isDelete = body["delete"] | false;
    if (!isDelete) {
        if (!body["def"].is<JsonObject>()) {
            prefs.end();
            _httpServer->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing def\"}");
            return;
        }
        if ((int)arr.size() >= AP_PRESET_MAX_COUNT) {
            prefs.end();
            _httpServer->send(507, "application/json", "{\"ok\":false,\"error\":\"Preset store full\"}");
            return;
        }
        JsonObject e = arr.add<JsonObject>();
        e["name"] = name;
        e["def"]  = body["def"];                        // deep-copies the snapshot
    }

    String out;
    serializeJson(listDoc, out);
    if (!isDelete && out.length() > AP_PRESET_STORE_BUDGET) {
        prefs.end();
        _httpServer->send(507, "application/json",
                          "{\"ok\":false,\"error\":\"Preset store full (size)\"}");
        return;
    }
    size_t written = prefs.putString("list", out);
    prefs.end();
    if (written == 0 && out.length() > 2) {
        _httpServer->send(500, "application/json", "{\"ok\":false,\"error\":\"NVS write failed\"}");
        return;
    }

    APPLOGF("AdvPreset %s: \"%s\" (%u presets, %u bytes)",
            isDelete ? "delete" : "save", name.c_str(), (unsigned)arr.size(), (unsigned)out.length());

    JsonDocument resp;
    resp["ok"] = true;
    resp["presets"] = listDoc.as<JsonArray>();
    String json;
    serializeJson(resp, json);
    _httpServer->send(200, "application/json", json);
}

// ============================================================================
// applyPattern — shared mutation used by HTTP POST /api/pattern AND WS op
// ============================================================================

bool WebUI::applyPattern(JsonDocument& doc, JsonDocument& resp) {
    // gen_rate_tick_hz is the ONLY field this handler touches that ConfigStore
    // persists. The old code mutated it but never saved — so a changed generator
    // tick rate silently reverted on the next boot (part of the "NVS works for
    // some settings, not others" bug). We now persist it, but ONLY when it
    // actually changes: applyPattern is also the hot path for live speed/depth/
    // stroke slider streaming while a pattern runs, and blindly saving on every
    // call would pound NVS flash into an early grave. Guarding on a real change
    // means one write per rate-rung change, zero writes during live scrubbing. :3
    bool persistTick = false;
    if (doc["rate_tick"].is<int>()) {
        int r = doc["rate_tick"];
        uint16_t newTick = (r >= 375) ? 500 : (r >= 175) ? 250 : (r >= 75) ? 100 : (r >= 35) ? 50 : 20;
        if (newTick != _state.gen_rate_tick_hz) {
            _state.gen_rate_tick_hz = newTick;
            persistTick = true;
        }
    }

    if (doc["speed"].is<float>())     _patternEngine.setSpeed(doc["speed"]);
    if (doc["depth"].is<float>())     _patternEngine.setDepth(doc["depth"]);
    if (doc["stroke"].is<float>())    _patternEngine.setStroke(doc["stroke"]);
    if (doc["sensation"].is<float>()) _patternEngine.setSensation(doc["sensation"]);
    if (doc["pattern"].is<int>())     _patternEngine.setPattern(doc["pattern"]);

    // ---- Advanced mode (fray-d port) — all fields optional/additive ---------
    // ap_reset FIRST: preset application layers deltas on the fray-d reset
    // baseline in a single atomic request ({ap_reset:true, <deltas>, ap_mods}).
    if (doc["ap_reset"].is<bool>() && doc["ap_reset"].as<bool>())
        _patternEngine.resetAdvanced();

    if (doc["ap_mode"].is<bool>())      _patternEngine.setAdvancedMode(doc["ap_mode"]);
    if (doc["ap_speed"].is<int>())      _patternEngine.setApMaster(doc["ap_speed"]);
    if (doc["ap_max_depth"].is<int>())  _patternEngine.setApBase(advpat::DEPTH_MAX, doc["ap_max_depth"]);
    if (doc["ap_min_depth"].is<int>())  _patternEngine.setApBase(advpat::DEPTH_MIN, doc["ap_min_depth"]);
    if (doc["ap_in_speed"].is<int>())   _patternEngine.setApBase(advpat::SPEED_IN,  doc["ap_in_speed"]);
    if (doc["ap_out_speed"].is<int>())  _patternEngine.setApBase(advpat::SPEED_OUT, doc["ap_out_speed"]);
    if (doc["ap_in_accel"].is<int>())   _patternEngine.setApBase(advpat::ACCEL_IN,  doc["ap_in_accel"]);
    if (doc["ap_out_accel"].is<int>())  _patternEngine.setApBase(advpat::ACCEL_OUT, doc["ap_out_accel"]);

    // Modifier blocks: ap_mod (one control) or ap_mods (array — preset apply).
    // Missing fields keep their current value (read back from the engine —
    // single Core-0 writer, no race).
    auto applyModObject = [this](JsonObject m) -> int {
        if (!m["ctrl"].is<int>()) return -1;
        int ctrl = m["ctrl"];
        const advpat::BaseControl* c = _patternEngine.apSettings().byId((uint8_t)ctrl);
        if (!c) return -1;
        int amp = m["amplitude"].is<int>() ? m["amplitude"].as<int>() : (int)c->modifier.amplitude;
        int is_ = m["in_step"].is<int>()   ? m["in_step"].as<int>()   : (int)c->modifier.in_step;
        int iw  = m["in_wait"].is<int>()   ? m["in_wait"].as<int>()   : (int)c->modifier.in_wait;
        int os_ = m["out_step"].is<int>()  ? m["out_step"].as<int>()  : (int)c->modifier.out_step;
        int ow  = m["out_wait"].is<int>()  ? m["out_wait"].as<int>()  : (int)c->modifier.out_wait;
        int off = m["offset"].is<int>()    ? m["offset"].as<int>()    : (int)c->modifier.offset;
        _patternEngine.setApModifier((uint8_t)ctrl, amp, is_, iw, os_, ow, off);
        return ctrl;
    };
    int ap_mod_ctrl = -1;
    if (doc["ap_mod"].is<JsonObject>())
        ap_mod_ctrl = applyModObject(doc["ap_mod"].as<JsonObject>());
    if (doc["ap_mods"].is<JsonArray>())
        for (JsonObject m : doc["ap_mods"].as<JsonArray>()) applyModObject(m);

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

    // Persist only when the generator tick rung actually moved (see the guard
    // note above) — the live speed/depth/stroke stream must never touch flash.
    if (persistTick) ConfigStore::save(_state, _mapper, _motor);

    resp["ok"] = true;
    resp["running"]   = _patternEngine.isRunning();
    resp["speed"]     = (int)roundf(_patternEngine.getSpeedPercent());
    resp["depth"]     = (int)roundf(_patternEngine.getDepthPercent());
    resp["stroke"]    = (int)roundf(_patternEngine.getStrokePercent());
    resp["sensation"] = (int)roundf(_patternEngine.getSensationPercent());
    resp["pattern"]   = _patternEngine.getPatternIdx();
    resp["rate_tick"] = _state.gen_rate_tick_hz;
    apStateToJson(_patternEngine, resp, false);
    if (ap_mod_ctrl >= 0) {
        // Echo the APPLIED (post-clamp) modifier block for the touched control.
        const advpat::BaseControl* c = _patternEngine.apSettings().byId((uint8_t)ap_mod_ctrl);
        if (c) {
            JsonObject m = resp["ap_mod"].to<JsonObject>();
            m["ctrl"]      = ap_mod_ctrl;
            m["amplitude"] = (int)c->modifier.amplitude;
            m["in_step"]   = (int)c->modifier.in_step;
            m["in_wait"]   = (int)c->modifier.in_wait;
            m["out_step"]  = (int)c->modifier.out_step;
            m["out_wait"]  = (int)c->modifier.out_wait;
            m["offset"]    = (int)c->modifier.offset;
        }
    }

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
// handleApiMachine (GET) / handleApiMachineCommit (POST) — Phase 2
// ============================================================================
//
// GET  /api/machine         → {backend_active, backend_code, home_style,
//                               bus:{...}}   (bus only when Modbus is compiled
//                               in AND a live ServoModbus is wired)
// POST /api/machine/commit  {backend:0|1}   → THE ONLY WRITER of the
//   persisted "machcfg"/backend NVS key. Reboot-to-apply contract: NVS is
//   written ONLY here, on an explicit commit — the UI's confirmation dialog
//   writes nothing on Cancel/backdrop/Esc, and a dismissed dialog must never
//   change what boots next. On a genuine change we respond first, THEN
//   schedule ESP.restart() ~500ms later (WebUI::update(), same deferred
//   pattern OtaService uses for its post-response reboot) so the 200 actually
//   reaches the browser before the device drops off the network. :3

void WebUI::handleApiMachine() {
    JsonDocument doc;
    doc["backend_active"] = (_machine_backend == 1) ? "modbus" : "fas";
    doc["backend_code"]   = _machine_backend;
    doc["home_style"]     = machineHomeStyleLoad();

#if defined(FEATURE_RS485_MODBUS)
    if (_servoModbus) {
        ServoBusHealth bus = _servoModbus->getBusHealth();
        JsonObject busObj = doc["bus"].to<JsonObject>();
        busObj["baud"]           = bus.baud;
        busObj["sp_fail_streak"] = bus.sp_fail_streak;
        busObj["sp_sent"]        = bus.sp_sent;
        busObj["sp_ok"]          = bus.sp_ok;
        busObj["sp_fc"]          = _servoModbus->setpointFc();
        busObj["sp_le"]          = _servoModbus->setpointLe();
        busObj["sp_noecho"]      = _servoModbus->setpointNoEcho();
    }
#endif

    String json;
    serializeJson(doc, json);
    _httpServer->send(200, "application/json", json);
}

void WebUI::handleApiMachineCommit() {
    JsonDocument doc;
    if (deserializeJson(doc, _httpServer->arg("plain")) || !doc["backend"].is<int>()) {
        _httpServer->send(400, "application/json", "{\"ok\":false,\"error\":\"backend (0|1) required\"}");
        return;
    }
    int backend = doc["backend"].as<int>();

    if (backend < 0 || backend > 1) {
        _httpServer->send(400, "application/json", "{\"ok\":false,\"error\":\"backend out of range\"}");
        return;
    }
#if !defined(FEATURE_RS485_MODBUS)
    if (backend == 1) {
        _httpServer->send(400, "application/json", "{\"ok\":false,\"error\":\"modbus backend not compiled into this build\"}");
        return;
    }
#endif

    if ((uint8_t)backend == _machine_backend) {
        // No-op commit — nothing to persist, nothing to reboot for.
        _httpServer->send(200, "application/json", "{\"ok\":true,\"rebooting\":false,\"unchanged\":true}");
        return;
    }

#if defined(FEATURE_RS485_MODBUS)
    // Modbus -> FAS: best-effort factory-restore the drive's RUNTIME baud
    // back to 19200 BEFORE the reboot. Why: FAS mode's own dual-baud probe
    // would still happily find the drive at 115200 (Phase 1 plumbing), so
    // this isn't required for FAS to work — it's here so the FAS boot path
    // looks byte-identical to pre-Phase-3 behavior (telemetry lands on the
    // first probe attempt instead of the fallback one) and so power-cycling
    // the drive later doesn't matter either way (factory 19200 is already
    // where we left it). BEST-EFFORT + accepted race: reprogramBaud() is
    // normally init-context-only (it bypasses ServoModbus's update() state
    // machine and touches the port directly), but servoBusTask is still
    // alive here — we're one commit away from ESP.restart() torching all of
    // this state anyway, so a garbled frame or two on the way out is a
    // non-issue. A failed restore just means the NEXT boot's probe finds
    // 115200 and moves on — not a bricked link either way. :3
    if (_machine_backend == 1 && backend == 0 && _servoModbus) {
        bool ok = _servoModbus->reprogramBaud(19200);
        APPLOGF("[MACHINE] backend commit: best-effort baud restore to 19200 %s",
                ok ? "OK" : "FAILED (harmless — next boot's probe finds whatever it finds)");
    }
#endif

    machineBackendStore((uint8_t)backend);
    APPLOGF("[MACHINE] backend commit: %u -> %d — rebooting to apply", _machine_backend, backend);
    _httpServer->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");

    _machineReboot.arm(500, "motion-backend change commit");
}

// ============================================================================
// handleApiHomeOverride (POST) — HTTP twin of WS_OP_HOME_OVERRIDE
// ============================================================================
// {"on":true,"stroke":250} / {"on":false}. Added during Modbus bench bring-up:
// the WS route required console-spawned WebSocket clients, and every one of
// those leaked a socket that went half-open on the next device reboot —
// feeding the exact ws-send-blocks-HTTP-mutex wedge the [STALL] watchdog
// caught at 158s. Plain HTTP request/response leaks nothing. Same state
// transitions as the WS case, byte for byte. :3

void WebUI::handleApiHomeOverride() {
    JsonDocument doc;
    deserializeJson(doc, _httpServer->arg("plain"));   // empty body -> defaults
    bool on = doc["on"] | true;

    JsonDocument resp;
    if (on) {
        float stroke = doc["stroke"] | 250.0f;
        if (stroke < 1.0f) stroke = 250.0f;
        _state.estop_latched = false;      // bench-home exits the e-stopped state
        _state.test_stroke_override_mm = stroke;
        _state.homing_in_progress = false;
        _state.homed = true;
        _state.resume_start_ms = millis(); // soft-start guard like a real home
        _motor.forceHomeState(true);       // driver-side flag + (Modbus) wire re-anchor
        APPLOG("HTTP Home-Override: faking homed for bench test :3");
        resp["measured_stroke"] = stroke;
    } else {
        _state.test_stroke_override_mm = 0.0f;
        _state.homed = false;
        _motor.forceHomeState(false);
        APPLOG("HTTP Home-Override: cleared — back to real homing.");
        resp["measured_stroke"] = _motor.getMeasuredStrokeMm();
    }
    resp["ok"] = true;
    resp["home_override"] = on;
    resp["homed"] = _state.homed;
    _bumpGen();

    String json;
    serializeJson(resp, json);
    _httpServer->send(200, "application/json", json);
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
        // No driver fault readback exists on this build — nothing to clear or
        // verify. Re-apply the current driver config (on the TMC this rewrites
        // the SPI registers with readback verification, a genuine recovery from
        // a transient glitch) but say honestly that no fault was cleared. :3
        {
            JsonDocument dummy;
            dummy["reset"] = false;  // don't reset, just re-apply current config
            applyDriverConfig(dummy, payload_out);
        }
        payload_out["cleared"] = false;
        payload_out["reason"] = "no_fault_readback";
        APPLOG("WS clear-fault: driver config re-applied — no fault readback exists to clear/verify");
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
            _state.estop_latched = false;   // a fresh homing cycle exits the e-stopped state
        }
        payload_out["ok"] = true;
        _bumpGen();
        return true;

    case WS_OP_HOME_OVERRIDE: {
        // TEST/bench: pretend the machine is homed without a motor so the WebUI
        // (rail, window, telemetry) populates for debugging. on:false clears it.
        bool on = payload_in["on"] | true;
        if (on) _state.estop_latched = false;   // bench-home exits the e-stopped state
        if (on) {
            float stroke = payload_in["stroke"] | 250.0f;   // generic bench value
            if (stroke < 1.0f) stroke = 250.0f;
            _state.test_stroke_override_mm = stroke;
            _state.homing_in_progress = false;
            _state.homed = true;
            _state.resume_start_ms = millis();   // soft-start guard like a real home
            // CRITICAL: flip the DRIVER'S own _homed flag too — setting
            // _state.homed alone only opens the MotionArbiter gate; the driver's
            // moveTo()/streamTo()/streamToSteps() all bail on `if (!_homed)`, so
            // no pulses ever leave the board. forceHomeState() energizes the FAS
            // outputs and zeroes position so a bench move genuinely drives step/
            // dir out to a (possibly disconnected) motor. :3
            _motor.forceHomeState(true);
            APPLOG("WS Home-Override: faking homed for bench test — no motor required :3");
            payload_out["measured_stroke"] = stroke;
        } else {
            _state.test_stroke_override_mm = 0.0f;
            _state.homed = false;
            _motor.forceHomeState(false);   // clear the driver flag too
            APPLOG("WS Home-Override: cleared — back to real homing.");
            payload_out["measured_stroke"] = _motor.getMeasuredStrokeMm();
        }
        payload_out["ok"] = true;
        payload_out["home_override"] = on;
        payload_out["homed"] = _state.homed;
        _bumpGen();
        return true;
    }

    case WS_OP_HALT:
        if (_motor.isHomed() && _arbiter) _arbiter->hardStopMotion();
        APPLOG("WS Halt: motor stopped — still homed and ready~ :3");
        payload_out["ok"] = true;
        _bumpGen();
        return true;

    case WS_OP_ESTOP:
        // Same code path as /api/stop (safety parity)
        _state.estop_requested.store(true);
        _state.estop_latched = true;   // latched for telemetry — UI fault banner rises from this
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
            if (_motor.isHomed() && _arbiter) _arbiter->hardStopMotion();
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
        // Bypass limits toggle — STORED in SystemState (previously this echoed
        // the requested value without storing anything, so the echo was a lie
        // and there was no truth to resync on reconnect). applyMove honors it
        // when a move doesn't carry a per-request bypass field; GET_CFG exposes
        // it. Session-only, never persisted. :3
        bool val = payload_in["on"] | false;
        _state.bypass_limits = val;
        APPLOGF("WS bypass-limits: %s", val ? "ON (window clamp bypassed, physical ceiling still enforced)" : "OFF");
        payload_out["ok"] = true;
        payload_out["bypass_limits"] = (bool)_state.bypass_limits;
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
        // Ground truth: read back from the driver (post-internal-clamp), same
        // as the applySettings echo. :3
        payload_out["max_speed"] = (uint32_t)_motor.getMaxSpeed();
        payload_out["accel"] = (uint32_t)_motor.getAcceleration();
        payload_out["blend_mode"] = _motor.getBlendMode();
        payload_out["auto_duration"] = _state.auto_duration;
        payload_out["intiface_compat"] = _state.intiface_compat;
        payload_out["default_range_min"] = _state.default_range_min;
        payload_out["default_range_max"] = _state.default_range_max;
        payload_out["expert_mode"] = _state.expert_mode;
        payload_out["max_travel"] = _state.config.max_rail_mm;
        payload_out["max_rail"] = _state.config.max_rail_mm;
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
        payload_out["bypass_limits"] = (bool)_state.bypass_limits;
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