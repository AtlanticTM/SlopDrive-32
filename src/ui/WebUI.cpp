#include "WebUI.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "AppLog.h"
#include "ConfigStore.h"
#include "Generator.h"
#include "Interpolator.h"
#include "MotorDriver.h"
#include "TransportManager.h"
#include "SerialTransport.h"
#include "WebSocketTransport.h"
#include "BleTransport.h"
#include "config_api.h"
#include "range_mapper.h"

// ---- Logging macros (identical to the ones in main.cpp) --------------------
// In serial-control mode the USB Serial port is dedicated to Intiface TCode,
// so status/debug must go to the web log (applog), NOT Serial.
#if SERIAL_CONTROL_MODE
  #define APPLOG(s)      applog(s)
  #define APPLOGF(...)   applogf(__VA_ARGS__)
#else
  #define APPLOG(s)      Serial.println(s)
  #define APPLOGF(...)   Serial.printf(__VA_ARGS__)
#endif

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
             Generator&          generator,
             Interpolator&       interpolator,
             TransportManager&   transportMgr,
             SerialTransport&    serialTransport,
             WebSocketTransport& wsTransport,
             BleTransport&       bleTransport)
    : _state(state)
    , _motor(motor)
    , _mapper(mapper)
    , _generator(generator)
    , _interpolator(interpolator)
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
    _httpServer->on("/api/settings",  HTTP_GET,  [this]() { handleApiSettings(); });
    _httpServer->on("/api/settings",  HTTP_POST, [this]() { handleApiSettings(); });
    _httpServer->on("/api/move",      HTTP_POST, [this]() { handleApiMove(); });
    _httpServer->on("/api/home",      HTTP_POST, [this]() { handleApiHome(); });
    _httpServer->on("/api/stop",      HTTP_POST, [this]() { handleApiStop(); });
    _httpServer->on("/api/pause",     HTTP_POST, [this]() { handleApiPause(); });
    _httpServer->on("/api/halt",      HTTP_POST, [this]() { handleApiHalt(); });
    _httpServer->on("/api/override",  HTTP_POST, [this]() { handleApiOverride(); });
    _httpServer->on("/api/tmc",       HTTP_GET,  [this]() { handleApiTmc(); });
    _httpServer->on("/api/tmc",       HTTP_POST, [this]() { handleApiTmc(); });
    _httpServer->on("/api/clearfault",HTTP_POST, [this]() { handleApiClearFault(); });
    _httpServer->on("/api/gen",       HTTP_GET,  [this]() { handleApiGen(); });
    _httpServer->on("/api/gen",       HTTP_POST, [this]() { handleApiGen(); });
    _httpServer->on("/api/interp",    HTTP_GET,  [this]() { handleApiInterp(); });
    _httpServer->on("/api/interp",    HTTP_POST, [this]() { handleApiInterp(); });
    _httpServer->on("/api/log",       HTTP_GET,  [this]() { handleApiLog(); });
    _httpServer->on("/api/mode",      HTTP_GET,  [this]() { handleApiMode(); });
    _httpServer->on("/api/mode",      HTTP_POST, [this]() { handleApiMode(); });

    _httpServer->begin();
    APPLOGF("HTTP server on port %d", HTTP_PORT);
}

// ============================================================================
// update() — service the HTTP server (was httpTask body)
// ============================================================================

void WebUI::update() {
    _httpServer->handleClient();
}

// ============================================================================
// Route handlers
// ============================================================================

void WebUI::handleRoot() {
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
    doc["wifi_connected"] = _state.wifi_ready;
    doc["ip"] = WiFi.localIP().toString();
    doc["homed"] = _state.homed;
    doc["homing"] = _state.homing_in_progress;
    doc["buttplug_connected"] = _wsTransport.isConnected();
    doc["position"] = _motor.getPosition();
    uint16_t hz = _state.measured_hz;
    doc["measured_hz"] = hz;
    doc["measured_interval_ms"] = (hz > 0) ? (uint16_t)(1000 / hz) : 0;
    doc["auto_duration"] = _state.auto_duration;
    doc["serial_mode"] = (bool)SERIAL_CONTROL_MODE;
    doc["serial_active"] = _serialTransport.isActive();
    doc["serial_linked"] = _serialTransport.isLinked();

    // Active transport + BLE link state
    doc["transport"] = TransportManager::transportName(_state.getTransport());
    doc["ble_running"]   = _bleTransport.isRunning();
    doc["ble_connected"] = _bleTransport.isConnected();
    doc["ble_linked"]    = _bleTransport.isLinked();

    // Control-gating state
    doc["paused"] = _state.paused;
    doc["manual_override"] = _state.manual_override;

    // --- Driver health (legacy TMC2160 readback REMOVED) ---
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

void WebUI::handleApiClearFault() {
    APPLOG("Driver fault clear requested (no-op — readback removed)");
    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiSettings() {
    if (_httpServer->method() == HTTP_GET) {
        JsonDocument doc;
        doc["range_min"] = _mapper.getMinMm();
        doc["range_max"] = _mapper.getMaxMm();
        doc["max_speed"] = (uint16_t)_state.config.max_speed_mm_s;
        doc["accel"] = (uint16_t)_state.config.acceleration_mm_s2;
        doc["lookahead"] = _motor.getLookaheadMs();
        doc["overshoot"] = (uint16_t)_motor.getMaxOvershootMm();
        doc["auto_duration"] = _state.auto_duration;
        doc["default_range_min"] = _state.default_range_min;
        doc["default_range_max"] = _state.default_range_max;
        doc["expert_mode"] = _state.expert_mode;

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

        float rmin = doc["range_min"] | _mapper.getMinMm();
        float rmax = doc["range_max"] | _mapper.getMaxMm();
        uint16_t speed = doc["max_speed"] | (uint16_t)_state.config.max_speed_mm_s;
        uint16_t accel = doc["accel"] | (uint16_t)_state.config.acceleration_mm_s2;

        if (rmin >= rmax) {
            _httpServer->send(400, "application/json", "{\"error\":\"Min must be less than Max\"}");
            return;
        }

        if (accel < 200) accel = 200;
        if (accel > 5000) accel = 5000;

        uint16_t lookahead = doc["lookahead"] | (uint16_t)_motor.getLookaheadMs();
        float overshoot = doc["overshoot"] | _motor.getMaxOvershootMm();
        if (lookahead > 200) lookahead = 200;
        if (overshoot < 0.0f) overshoot = 0.0f;
        if (overshoot > 50.0f) overshoot = 50.0f;

        _state.auto_duration = doc["auto_duration"] | _state.auto_duration;

        _state.expert_mode = doc["expert_mode"] | _state.expert_mode;
        if (doc["default_range_min"].is<float>() || doc["default_range_max"].is<float>()) {
            float drmin = doc["default_range_min"] | _state.default_range_min;
            float drmax = doc["default_range_max"] | _state.default_range_max;
            if (drmin >= 0.0f && drmax <= PHYSICAL_MAX_TRAVEL_MM && drmin < drmax) {
                _state.default_range_min = drmin;
                _state.default_range_max = drmax;
            }
        }

        _mapper.setRange(rmin, rmax);

        _state.config.max_speed_mm_s = (float)speed;
        _state.config.acceleration_mm_s2 = (float)accel;
        _motor.setMaxSpeed(_state.config.max_speed_mm_s);
        _motor.setAcceleration(_state.config.acceleration_mm_s2);
        _motor.setLookaheadMs(lookahead);
        _motor.setMaxOvershootMm(overshoot);
        bool no_persist = doc["no_persist"] | false;
        if (!no_persist) ConfigStore::save(_state, _mapper, _motor);

        JsonDocument resp;
        resp["ok"] = true;
        String json;
        serializeJson(resp, json);
        _httpServer->send(200, "application/json", json);
    }
}

void WebUI::handleApiMove() {
    if (_httpServer->method() == HTTP_POST) {
        String body = _httpServer->arg("plain");
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);

        if (err || !_state.homed) {
            _httpServer->send(400, "application/json", "{\"error\":\"Invalid request or not homed\"}");
            return;
        }

        float pos = doc["position"] | 0.0f;
        bool bypass = doc["bypass_limits"] | false;
        if (bypass) {
            pos = constrain(pos, 0.0f, PHYSICAL_MAX_TRAVEL_MM);
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

        JsonDocument resp;
        resp["ok"] = true;
        String json;
        serializeJson(resp, json);
        _httpServer->send(200, "application/json", json);
    }
}

void WebUI::handleApiHome() {
    if (!_state.homing_in_progress) {
        _state.homed = false;
        _state.homing_in_progress = true;
        _motor.home();
    }
    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiStop() {
    _motor.stop();
    _state.homed = false;
    _state.homing_in_progress = false;
    _state.paused = false;
    _state.manual_override = false;
    _state.resume_start_ms = 0;
    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiPause() {
    JsonDocument doc;
    deserializeJson(doc, _httpServer->arg("plain"));
    bool was = _state.paused;
    _state.paused = doc["paused"] | (!_state.paused);
    if (_state.paused && !was) {
        if (_motor.isHomed()) _motor.hardStop();
        APPLOG("Paused: ignoring Intiface input");
    } else if (!_state.paused && was) {
        _state.resume_start_ms = millis();
        APPLOG("Unpaused: resuming Intiface input");
    }
    String json; JsonDocument r; r["ok"] = true; r["paused"] = _state.paused;
    serializeJson(r, json);
    _httpServer->send(200, "application/json", json);
}

void WebUI::handleApiHalt() {
    if (_motor.isHomed()) _motor.hardStop();
    APPLOG("Halt: motor stopped (still homed/powered)");
    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiOverride() {
    JsonDocument doc;
    deserializeJson(doc, _httpServer->arg("plain"));
    bool was = _state.manual_override;
    _state.manual_override = doc["override"] | (!_state.manual_override);
    if (_state.manual_override && !was) {
        APPLOG("Manual override ON: Intiface input ignored");
    } else if (!_state.manual_override && was) {
        _state.resume_start_ms = millis();
        APPLOG("Manual override OFF: easing back to Intiface");
    }
    String json; JsonDocument r; r["ok"] = true; r["manual_override"] = _state.manual_override;
    serializeJson(r, json);
    _httpServer->send(200, "application/json", json);
}

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

    JsonDocument resp;
    resp["ok"] = true;
    String json;
    serializeJson(resp, json);
    _httpServer->send(200, "application/json", json);
}

void WebUI::handleApiGen() {
    if (_httpServer->method() == HTTP_GET) {
        JsonDocument doc;
        doc["running"]  = _state.gen.running;
        doc["active"]   = _state.gen_active;
        doc["wave"]     = _state.gen.wave;
        doc["rate"]     = _state.gen.rate_hz;
        doc["depth"]    = (int)roundf(_state.gen.depth * 100.0f);
        doc["offset"]   = (int)roundf(_state.gen.offset * 100.0f);
        doc["ease"]     = (int)roundf(_state.gen.ease * 100.0f);
        doc["mod"]      = _state.gen.mod;
        doc["mod_wave"] = _state.gen.mod_wave;
        doc["mod_rate"] = _state.gen.mod_rate;
        doc["mod_amp"]  = _state.gen.mod_amp;
        doc["rate_tick"] = _state.gen_rate_tick_hz;
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

    APPLOGF("/api/gen POST: %s", _httpServer->arg("plain").c_str());

    if (doc["rate_tick"].is<int>()) {
        int r = doc["rate_tick"];
        _state.gen_rate_tick_hz = (r >= 75) ? 100 : (r >= 35) ? 50 : 20;
    }

    _state.gen.wave     = constrain((int)(doc["wave"]     | (int)_state.gen.wave), 0, 3);
    _state.gen.rate_hz  = constrain((float)(doc["rate"]   | _state.gen.rate_hz), 0.05f, 50.0f);
    if (doc["depth"].is<float>())  _state.gen.depth  = constrain((float)doc["depth"]  / 100.0f, 0.02f, 1.0f);
    if (doc["offset"].is<float>()) _state.gen.offset = constrain((float)doc["offset"] / 100.0f, 0.0f, 1.0f);
    if (doc["ease"].is<float>())   _state.gen.ease   = constrain((float)doc["ease"]   / 100.0f, 0.0f, 1.0f);
    _state.gen.mod      = constrain((int)(doc["mod"]      | (int)_state.gen.mod), 0, 2);
    _state.gen.mod_wave = constrain((int)(doc["mod_wave"] | (int)_state.gen.mod_wave), 0, 2);
    _state.gen.mod_rate = constrain((float)(doc["mod_rate"] | _state.gen.mod_rate), 0.01f, 5.0f);
    _state.gen.mod_amp  = constrain((float)(doc["mod_amp"] | _state.gen.mod_amp), 0.0f, 10.0f);

    if (doc["running"].is<bool>()) {
        bool want = doc["running"];
        if (want && !_state.gen.running) {
            _state.gen_phase = 0.0f;
            _state.gen_mod_clock = 0.0f;
            _state.gen_last_us = micros();
            APPLOG("Generator started");
        } else if (!want && _state.gen.running) {
            APPLOG("Generator stopped");
        }
        _state.gen.running = want;
    }

    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiInterp() {
    if (_httpServer->method() == HTTP_GET) {
        JsonDocument doc;
        doc["mode"]      = (_state.getInputMode() == InputMode::BUFFERED) ? "buffered" : "extrapolate";
        doc["easing"]    = _state.buf_easing;
        doc["depth"]     = _state.buf_depth;
        doc["tick"]      = _state.buf_tick_hz;
        doc["active"]    = _interpolator.isActive();
        doc["buffered"]  = _state.buf_count;
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

    if (doc["mode"].is<const char*>()) {
        const char* m = doc["mode"];
        _state.setInputMode((strcasecmp(m, "buffered") == 0) ? InputMode::BUFFERED
                                                               : InputMode::EXTRAPOLATE);
    }
    if (doc["easing"].is<int>()) _state.buf_easing = constrain((int)doc["easing"], 0, 4);
    if (doc["depth"].is<int>())  _state.buf_depth  = constrain((int)doc["depth"], 1, 5);
    if (doc["tick"].is<int>()) {
        int t = doc["tick"];
        _state.buf_tick_hz = (t >= 75) ? 100 : (t >= 35) ? 50 : 20;
    }

    bool save = doc["save"] | false;
    if (save) ConfigStore::save(_state, _mapper, _motor);

    APPLOGF("Interp: mode=%s easing=%u depth=%u tick=%uHz",
            (_state.getInputMode() == InputMode::BUFFERED) ? "buffered" : "extrapolate",
            _state.buf_easing, _state.buf_depth, _state.buf_tick_hz);

    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiLog() {
    String out;
    out.reserve(2048);
    applogDump(out);
    _httpServer->send(200, "text/plain", out);
}

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

    const char* m = doc["mode"] | "";
    TransportMode mode = _state.getTransport();
    if      (strcasecmp(m, "WS")  == 0) mode = TransportMode::WS;
    else if (strcasecmp(m, "SER") == 0) mode = TransportMode::SER;
    else if (strcasecmp(m, "BT")  == 0) mode = TransportMode::BT;
    else { _httpServer->send(400, "application/json", "{\"error\":\"mode must be WS|SER|BT\"}"); return; }

    _transportMgr.applyTransport(mode);
    ConfigStore::save(_state, _mapper, _motor);

    JsonDocument resp;
    resp["ok"]   = true;
    resp["mode"] = TransportManager::transportName(_state.getTransport());
    String json;
    serializeJson(resp, json);
    _httpServer->send(200, "application/json", json);
}
