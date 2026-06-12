#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <ArduinoJson.h>

#include <LittleFS.h>


#include "config_api.h"
#if defined(DRIVER_TMC2160)
#include "TMC2160StepperDriver.h"
#endif
#include "range_mapper.h"
#include "AppLog.h"
#include "Kinematics.h"
#include "SystemState.h"
#include "ConfigStore.h"
#include "Generator.h"
#include "Interpolator.h"

// ---- Comms (Step 8: split from ButtplugServer) -------------------------------
#include "TCodeParser.h"
#include "SerialTransport.h"
#include "WebSocketTransport.h"
#include "BleTransport.h"
#include "TransportManager.h"


// In serial-control mode the USB Serial port is dedicated to Intiface TCode, so
// status/debug must go to the web log (applog), NOT Serial. APPLOG()/APPLOGF()
// route accordingly so the same source works in both modes.
#if SERIAL_CONTROL_MODE
  #define APPLOG(s)      applog(s)
  #define APPLOGF(...)   applogf(__VA_ARGS__)
#else
  #define APPLOG(s)      Serial.println(s)
  #define APPLOGF(...)   Serial.printf(__VA_ARGS__)
#endif


// ============================================================================
// Driver selection — compile-time via build flag (platformio.ini)
// ============================================================================
#if defined(DRIVER_TMC2160)
  TMC2160StepperDriver motor;
#else
  #error "No motor driver selected! Define DRIVER_TMC2160 (or future driver flags) in platformio.ini build_flags."
#endif

// ============================================================================
// Global Instances
// ============================================================================

RangeMapper mapper;

// Centralised, thread-safe runtime state (replaces ~30 file-scope globals).
static SystemState g_state;

Generator generator(g_state, mapper, motor);
Interpolator interpolator(g_state, mapper, motor);
WebServer httpServer(HTTP_PORT);

// ---- Comms modules (Step 8) -------------------------------------------------
TCodeParser tcodeParser;
SerialTransport    serialTransport(tcodeParser);
WebSocketTransport wsTransport(tcodeParser);
BleTransport       bleTransport(tcodeParser);
TransportManager   transportMgr(g_state, tcodeParser, serialTransport, wsTransport, bleTransport);


// ============================================================================
// Control gating blend constant (unchanged).
// ============================================================================
static const uint32_t RESUME_BLEND_MS = 800;


// ============================================================================
// HTTP Server - Web UI Pages
// ============================================================================


// Fallback page shown when LittleFS /index.html is missing.
const char* htmlFallbackPage = R"RAWHTML(
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
// HTTP Route Handlers
// ============================================================================

void handleRoot() {
    if (LittleFS.exists("/index.html")) {
        File f = LittleFS.open("/index.html", "r");
        if (f) {
            httpServer.streamFile(f, "text/html");
            f.close();
            return;
        }
    }
    httpServer.send(200, "text/html", htmlFallbackPage);
}

void handleApiStatus() {
    JsonDocument doc;
    doc["wifi_connected"] = g_state.wifi_ready;
    doc["ip"] = WiFi.localIP().toString();
    doc["homed"] = g_state.homed;
    doc["homing"] = g_state.homing_in_progress;
    doc["buttplug_connected"] = wsTransport.isConnected();
    doc["position"] = motor.getPosition();
    uint16_t hz = g_state.measured_hz;
    doc["measured_hz"] = hz;
    doc["measured_interval_ms"] = (hz > 0) ? (uint16_t)(1000 / hz) : 0;
    doc["auto_duration"] = g_state.auto_duration;
    doc["serial_mode"] = (bool)SERIAL_CONTROL_MODE;
    doc["serial_active"] = serialTransport.isActive();
    doc["serial_linked"] = serialTransport.isLinked();

    // Active transport + BLE link state
    doc["transport"] = TransportManager::transportName(g_state.getTransport());
    doc["ble_running"]   = bleTransport.isRunning();
    doc["ble_connected"] = bleTransport.isConnected();
    doc["ble_linked"]    = bleTransport.isLinked();

    // Control-gating state
    doc["paused"] = g_state.paused;
    doc["manual_override"] = g_state.manual_override;

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
    httpServer.send(200, "application/json", json);
}

void handleApiClearFault() {
    APPLOG("Driver fault clear requested (no-op — readback removed)");
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

void handleApiSettings() {
    if (httpServer.method() == HTTP_GET) {
        JsonDocument doc;
        doc["range_min"] = mapper.getMinMm();
        doc["range_max"] = mapper.getMaxMm();
        doc["max_speed"] = (uint16_t)g_state.config.max_speed_mm_s;
        doc["accel"] = (uint16_t)g_state.config.acceleration_mm_s2;
        doc["lookahead"] = motor.getLookaheadMs();
        doc["overshoot"] = (uint16_t)motor.getMaxOvershootMm();
        doc["auto_duration"] = g_state.auto_duration;
        doc["default_range_min"] = g_state.default_range_min;
        doc["default_range_max"] = g_state.default_range_max;
        doc["expert_mode"] = g_state.expert_mode;

        String json;
        serializeJson(doc, json);
        httpServer.send(200, "application/json", json);
    } else if (httpServer.method() == HTTP_POST) {

        String body = httpServer.arg("plain");
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);

        if (err) {
            httpServer.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }

        float rmin = doc["range_min"] | mapper.getMinMm();
        float rmax = doc["range_max"] | mapper.getMaxMm();
        uint16_t speed = doc["max_speed"] | (uint16_t)g_state.config.max_speed_mm_s;
        uint16_t accel = doc["accel"] | (uint16_t)g_state.config.acceleration_mm_s2;

        if (rmin >= rmax) {
            httpServer.send(400, "application/json", "{\"error\":\"Min must be less than Max\"}");
            return;
        }

        if (accel < 200) accel = 200;
        if (accel > 5000) accel = 5000;

        uint16_t lookahead = doc["lookahead"] | (uint16_t)motor.getLookaheadMs();
        float overshoot = doc["overshoot"] | motor.getMaxOvershootMm();
        if (lookahead > 200) lookahead = 200;
        if (overshoot < 0.0f) overshoot = 0.0f;
        if (overshoot > 50.0f) overshoot = 50.0f;

        g_state.auto_duration = doc["auto_duration"] | g_state.auto_duration;

        g_state.expert_mode = doc["expert_mode"] | g_state.expert_mode;
        if (doc["default_range_min"].is<float>() || doc["default_range_max"].is<float>()) {
            float drmin = doc["default_range_min"] | g_state.default_range_min;
            float drmax = doc["default_range_max"] | g_state.default_range_max;
            if (drmin >= 0.0f && drmax <= PHYSICAL_MAX_TRAVEL_MM && drmin < drmax) {
                g_state.default_range_min = drmin;
                g_state.default_range_max = drmax;
            }
        }

        mapper.setRange(rmin, rmax);

        g_state.config.max_speed_mm_s = (float)speed;
        g_state.config.acceleration_mm_s2 = (float)accel;
        motor.setMaxSpeed(g_state.config.max_speed_mm_s);
        motor.setAcceleration(g_state.config.acceleration_mm_s2);
        motor.setLookaheadMs(lookahead);
        motor.setMaxOvershootMm(overshoot);
        bool no_persist = doc["no_persist"] | false;
        if (!no_persist) ConfigStore::save(g_state, mapper, motor);


        JsonDocument resp;
        resp["ok"] = true;
        String json;
        serializeJson(resp, json);
        httpServer.send(200, "application/json", json);
    }
}

void handleApiMove() {
    if (httpServer.method() == HTTP_POST) {
        String body = httpServer.arg("plain");
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);

        if (err || !g_state.homed) {
            httpServer.send(400, "application/json", "{\"error\":\"Invalid request or not homed\"}");
            return;
        }

        float pos = doc["position"] | 0.0f;
        bool bypass = doc["bypass_limits"] | false;
        if (bypass) {
            pos = constrain(pos, 0.0f, PHYSICAL_MAX_TRAVEL_MM);
        } else {
            pos = constrain(pos, mapper.getMinMm(), mapper.getMaxMm());
        }
        bool stream = doc["stream"] | true;
        float speed = doc["speed"] | 0.0f;
        if (stream) {
            motor.streamTo(pos, speed);
        } else {
            motor.moveTo(pos);
        }

        JsonDocument resp;
        resp["ok"] = true;
        String json;
        serializeJson(resp, json);
        httpServer.send(200, "application/json", json);
    }
}

void handleApiHome() {
    if (!g_state.homing_in_progress) {
        g_state.homed = false;
        g_state.homing_in_progress = true;
        motor.home();
    }
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

void handleApiStop() {
    motor.stop();
    g_state.homed = false;
    g_state.homing_in_progress = false;
    g_state.paused = false;
    g_state.manual_override = false;
    g_state.resume_start_ms = 0;
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

void handleApiPause() {
    JsonDocument doc;
    deserializeJson(doc, httpServer.arg("plain"));
    bool was = g_state.paused;
    g_state.paused = doc["paused"] | (!g_state.paused);
    if (g_state.paused && !was) {
        if (motor.isHomed()) motor.hardStop();
        APPLOG("Paused: ignoring Intiface input");
    } else if (!g_state.paused && was) {
        g_state.resume_start_ms = millis();
        APPLOG("Unpaused: resuming Intiface input");
    }
    String json; JsonDocument r; r["ok"] = true; r["paused"] = g_state.paused;
    serializeJson(r, json);
    httpServer.send(200, "application/json", json);
}

void handleApiHalt() {
    if (motor.isHomed()) motor.hardStop();
    APPLOG("Halt: motor stopped (still homed/powered)");
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

void handleApiOverride() {
    JsonDocument doc;
    deserializeJson(doc, httpServer.arg("plain"));
    bool was = g_state.manual_override;
    g_state.manual_override = doc["override"] | (!g_state.manual_override);
    if (g_state.manual_override && !was) {
        APPLOG("Manual override ON: Intiface input ignored");
    } else if (!g_state.manual_override && was) {
        g_state.resume_start_ms = millis();
        APPLOG("Manual override OFF: easing back to Intiface");
    }
    String json; JsonDocument r; r["ok"] = true; r["manual_override"] = g_state.manual_override;
    serializeJson(r, json);
    httpServer.send(200, "application/json", json);
}

void handleApiTmc() {
    if (httpServer.method() == HTTP_GET) {
        JsonDocument doc;
        doc["run_current"]  = g_state.driver.run_current_ma;
        doc["hold_current"] = g_state.driver.hold_current_pct;
        doc["stealthchop"]  = g_state.driver.stealthchop;
        doc["tpwm_thrs"]    = g_state.driver.tpwm_thrs;
        doc["toff"]         = g_state.driver.toff;
        doc["tbl"]          = g_state.driver.tbl;
        doc["hstart"]       = g_state.driver.hstart;
        doc["hend"]         = g_state.driver.hend;
        String json;
        serializeJson(doc, json);
        httpServer.send(200, "application/json", json);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, httpServer.arg("plain"))) {
        httpServer.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    if (doc["reset"] | false) {
        g_state.driver = DriverConfig();
    } else {
        g_state.driver.run_current_ma   = constrain((int)(doc["run_current"]  | (int)g_state.driver.run_current_ma), 300, 3000);
        g_state.driver.hold_current_pct = constrain((int)(doc["hold_current"] | (int)g_state.driver.hold_current_pct), 0, 100);
        g_state.driver.stealthchop      = (doc["stealthchop"] | (int)g_state.driver.stealthchop) ? 1 : 0;
        g_state.driver.tpwm_thrs        = (uint32_t)(doc["tpwm_thrs"] | (long)g_state.driver.tpwm_thrs);
        g_state.driver.toff             = constrain((int)(doc["toff"]   | (int)g_state.driver.toff), 1, 15);
        g_state.driver.tbl              = constrain((int)(doc["tbl"]    | (int)g_state.driver.tbl), 0, 3);
        g_state.driver.hstart           = constrain((int)(doc["hstart"] | (int)g_state.driver.hstart), 0, 7);
        g_state.driver.hend             = constrain((int)(doc["hend"]   | (int)g_state.driver.hend), -3, 12);
    }

    g_state.config.run_current_ma = g_state.driver.run_current_ma;
    motor.applyDriverConfig(g_state.driver);

    bool save = doc["save"] | false;
    if (save) ConfigStore::save(g_state, mapper, motor);

    JsonDocument resp;
    resp["ok"] = true;
    String json;
    serializeJson(resp, json);
    httpServer.send(200, "application/json", json);
}

void handleApiLog() {
    String out;
    out.reserve(2048);
    applogDump(out);
    httpServer.send(200, "text/plain", out);
}

void handleApiMode() {
    if (httpServer.method() == HTTP_GET) {
        JsonDocument doc;
        doc["mode"]          = TransportManager::transportName(g_state.getTransport());
        doc["ble_running"]   = bleTransport.isRunning();
        doc["ble_connected"] = bleTransport.isConnected();
        String json;
        serializeJson(doc, json);
        httpServer.send(200, "application/json", json);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, httpServer.arg("plain"))) {
        httpServer.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    const char* m = doc["mode"] | "";
    TransportMode mode = g_state.getTransport();
    if      (strcasecmp(m, "WS")  == 0) mode = TransportMode::WS;
    else if (strcasecmp(m, "SER") == 0) mode = TransportMode::SER;
    else if (strcasecmp(m, "BT")  == 0) mode = TransportMode::BT;
    else { httpServer.send(400, "application/json", "{\"error\":\"mode must be WS|SER|BT\"}"); return; }

    transportMgr.applyTransport(mode);
    ConfigStore::save(g_state, mapper, motor);

    JsonDocument resp;
    resp["ok"]   = true;
    resp["mode"] = TransportManager::transportName(g_state.getTransport());
    String json;
    serializeJson(resp, json);
    httpServer.send(200, "application/json", json);
}


// ============================================================================
// On-device motion generator
// ============================================================================

void handleApiGen() {
    if (httpServer.method() == HTTP_GET) {
        JsonDocument doc;
        doc["running"]  = g_state.gen.running;
        doc["active"]   = g_state.gen_active;
        doc["wave"]     = g_state.gen.wave;
        doc["rate"]     = g_state.gen.rate_hz;
        doc["depth"]    = (int)roundf(g_state.gen.depth * 100.0f);
        doc["offset"]   = (int)roundf(g_state.gen.offset * 100.0f);
        doc["ease"]     = (int)roundf(g_state.gen.ease * 100.0f);
        doc["mod"]      = g_state.gen.mod;
        doc["mod_wave"] = g_state.gen.mod_wave;
        doc["mod_rate"] = g_state.gen.mod_rate;
        doc["mod_amp"]  = g_state.gen.mod_amp;
        doc["rate_tick"] = g_state.gen_rate_tick_hz;
        String json;
        serializeJson(doc, json);
        httpServer.send(200, "application/json", json);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, httpServer.arg("plain"))) {
        httpServer.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    APPLOGF("/api/gen POST: %s", httpServer.arg("plain").c_str());

    if (doc["rate_tick"].is<int>()) {
        int r = doc["rate_tick"];
        g_state.gen_rate_tick_hz = (r >= 75) ? 100 : (r >= 35) ? 50 : 20;
    }

    g_state.gen.wave     = constrain((int)(doc["wave"]     | (int)g_state.gen.wave), 0, 3);
    g_state.gen.rate_hz  = constrain((float)(doc["rate"]   | g_state.gen.rate_hz), 0.05f, 50.0f);
    if (doc["depth"].is<float>())  g_state.gen.depth  = constrain((float)doc["depth"]  / 100.0f, 0.02f, 1.0f);
    if (doc["offset"].is<float>()) g_state.gen.offset = constrain((float)doc["offset"] / 100.0f, 0.0f, 1.0f);
    if (doc["ease"].is<float>())   g_state.gen.ease   = constrain((float)doc["ease"]   / 100.0f, 0.0f, 1.0f);
    g_state.gen.mod      = constrain((int)(doc["mod"]      | (int)g_state.gen.mod), 0, 2);
    g_state.gen.mod_wave = constrain((int)(doc["mod_wave"] | (int)g_state.gen.mod_wave), 0, 2);
    g_state.gen.mod_rate = constrain((float)(doc["mod_rate"] | g_state.gen.mod_rate), 0.01f, 5.0f);
    g_state.gen.mod_amp  = constrain((float)(doc["mod_amp"] | g_state.gen.mod_amp), 0.0f, 10.0f);

    if (doc["running"].is<bool>()) {
        bool want = doc["running"];
        if (want && !g_state.gen.running) {
            g_state.gen_phase = 0.0f;
            g_state.gen_mod_clock = 0.0f;
            g_state.gen_last_us = micros();
            APPLOG("Generator started");
        } else if (!want && g_state.gen.running) {
            APPLOG("Generator stopped");
        }
        g_state.gen.running = want;
    }

    httpServer.send(200, "application/json", "{\"ok\":true}");
}

void handleApiInterp() {
    if (httpServer.method() == HTTP_GET) {
        JsonDocument doc;
        doc["mode"]      = (g_state.getInputMode() == InputMode::BUFFERED) ? "buffered" : "extrapolate";
        doc["easing"]    = g_state.buf_easing;
        doc["depth"]     = g_state.buf_depth;
        doc["tick"]      = g_state.buf_tick_hz;
        doc["active"]    = interpolator.isActive();
        doc["buffered"]  = g_state.buf_count;
        String json;
        serializeJson(doc, json);
        httpServer.send(200, "application/json", json);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, httpServer.arg("plain"))) {
        httpServer.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    if (doc["mode"].is<const char*>()) {
        const char* m = doc["mode"];
        g_state.setInputMode((strcasecmp(m, "buffered") == 0) ? InputMode::BUFFERED
                                                               : InputMode::EXTRAPOLATE);
    }
    if (doc["easing"].is<int>()) g_state.buf_easing = constrain((int)doc["easing"], 0, 4);
    if (doc["depth"].is<int>())  g_state.buf_depth  = constrain((int)doc["depth"], 1, 5);
    if (doc["tick"].is<int>()) {
        int t = doc["tick"];
        g_state.buf_tick_hz = (t >= 75) ? 100 : (t >= 35) ? 50 : 20;
    }

    bool save = doc["save"] | false;
    if (save) ConfigStore::save(g_state, mapper, motor);

    APPLOGF("Interp: mode=%s easing=%u depth=%u tick=%uHz",
            (g_state.getInputMode() == InputMode::BUFFERED) ? "buffered" : "extrapolate",
            g_state.buf_easing, g_state.buf_depth, g_state.buf_tick_hz);

    httpServer.send(200, "application/json", "{\"ok\":true}");
}


// ============================================================================
// TCode callbacks (glue between parser and motor)
// ============================================================================

static void buttplugLinearCmd(float position, uint32_t duration_ms) {

    if (!g_state.homed) return;
    if (g_state.paused || g_state.manual_override) { g_state.resume_start_ms = millis(); return; }

    g_state.last_intiface_ms = millis();

    uint32_t now = millis();
    if (g_state.last_cmd_ms != 0) {
        uint32_t gap = now - g_state.last_cmd_ms;
        if (gap > 0 && gap < 1000) {
            if (g_state.measured_interval_ms <= 0.0f) g_state.measured_interval_ms = (float)gap;
            else g_state.measured_interval_ms = 0.7f * g_state.measured_interval_ms + 0.3f * (float)gap;
        }
    }
    g_state.last_cmd_ms = now;

    if (g_state.auto_duration && g_state.measured_interval_ms > 1.0f) {
        duration_ms = (uint32_t)(g_state.measured_interval_ms + 0.5f);
    }

    float target_mm = mapper.intensityToPosition(position);

    // BUFFERED mode
    if (g_state.getInputMode() == InputMode::BUFFERED) {
        if (g_state.resume_start_ms != 0) {
            uint32_t elapsed = millis() - g_state.resume_start_ms;
            if (elapsed >= RESUME_BLEND_MS) {
                g_state.resume_start_ms = 0;
            } else {
                float blend = (float)elapsed / (float)RESUME_BLEND_MS;
                float cur = motor.getPosition();
                target_mm = cur + blend * (target_mm - cur);
            }
        }
        interpolator.pushSample(target_mm);
        return;
    }

    // EXTRAPOLATE mode: slow resume blend
    if (g_state.resume_start_ms != 0) {
        uint32_t elapsed = millis() - g_state.resume_start_ms;
        if (elapsed >= RESUME_BLEND_MS) {
            g_state.resume_start_ms = 0;
        } else {
            float blend = (float)elapsed / (float)RESUME_BLEND_MS;
            float cur = motor.getPosition();
            target_mm = cur + blend * (target_mm - cur);
        }
    }

    float speed_mm_s = 0.0f;
    if (duration_ms > 0) {
        float current_mm = motor.getPosition();
        float distance_mm = fabsf(target_mm - current_mm);
        if (distance_mm > 0.05f) {
            speed_mm_s = (distance_mm / (float)duration_ms) * 1000.0f;
            speed_mm_s = constrain(speed_mm_s, 5.0f, g_state.config.max_speed_mm_s);
        } else {
            speed_mm_s = g_state.config.max_speed_mm_s * 0.25f;
        }
    }

    motor.streamExtrapolated(target_mm, speed_mm_s);
}

static void buttplugStop() {
    if (motor.isHomed()) {
        motor.hardStop();
    }
}


// ============================================================================
// FreeRTOS Tasks
// ============================================================================

void motorTask(void* parameter) {
    while (true) {
        if (g_state.homing_in_progress) {
            motor.runHomingStep();

            if (!motor.isHoming()) {
                g_state.homing_in_progress = false;
                g_state.homed = motor.isHomed();
                if (g_state.homed) {
                    APPLOG("System is now homed and ready");
                }
            }
        } else if (!g_state.homed) {
            if (motor.checkPushToHome()) {
                g_state.homed = true;
                APPLOG("System homed via push-to-home and ready");
            }
        }

        motor.update();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/// Comms task: services all active transports and reports inbound rate.
/// Renamed from buttplugTask (Step 8).
void commsTask(void* parameter) {
    uint32_t last_report_ms = 0;
    uint32_t last_frame_count = 0;
    while (true) {
        // Always service the WS server + (if enabled) WSDM client.
        wsTransport.run();

        // Poll USB Serial when SER is the active transport.
        if (g_state.getTransport() == TransportMode::SER) {
            serialTransport.poll();
        }

        // Once-per-second rate diagnostic: raw inbound WebSocket + Serial frame
        // rate from the parser (counted before any motor work).
        uint32_t now = millis();
        if (now - last_report_ms >= 1000) {
            uint32_t frames = tcodeParser.rxFrameCount;
            uint32_t per_sec = frames - last_frame_count;
            last_frame_count = frames;
            last_report_ms = now;
            g_state.measured_hz = (uint16_t)per_sec;
            if (wsTransport.isConnected() || serialTransport.isActive()) {
                APPLOGF("[RATE] rx=%u frames/s", per_sec);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void httpTask(void* parameter) {
    while (true) {
        httpServer.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


// ============================================================================
// Setup & Loop
// ============================================================================

void setup() {
    Serial.begin(SERIAL_CONTROL_BAUD);
    applogBegin();
    APPLOG("=== SlopDrive-32 StrokeEngine v2.0 ===");
#if SERIAL_CONTROL_MODE
    applog("Serial control mode ON: USB Serial is dedicated to Intiface TCode.");
    applog("Add a 'Serial' device in Intiface pointing at this COM port.");
#endif

    if (LittleFS.begin(true)) {
        APPLOG("LittleFS mounted");
    } else {
        APPLOG("LittleFS mount FAILED - upload filesystem image (pio run -t uploadfs)");
    }

    ConfigStore::load(g_state, mapper, motor);

    motor.init();
    motor.applyDriverConfig(g_state.driver);

    // ---- Comms: WiFi + WS server + parser callbacks ----
    transportMgr.setupWiFi();

    httpServer.on("/", handleRoot);
    httpServer.on("/api/status", HTTP_GET, handleApiStatus);
    httpServer.on("/api/settings", HTTP_GET, handleApiSettings);
    httpServer.on("/api/settings", HTTP_POST, handleApiSettings);
    httpServer.on("/api/move", HTTP_POST, handleApiMove);
    httpServer.on("/api/home", HTTP_POST, handleApiHome);
    httpServer.on("/api/stop", HTTP_POST, handleApiStop);
    httpServer.on("/api/pause", HTTP_POST, handleApiPause);
    httpServer.on("/api/halt", HTTP_POST, handleApiHalt);
    httpServer.on("/api/override", HTTP_POST, handleApiOverride);
    httpServer.on("/api/tmc", HTTP_GET, handleApiTmc);
    httpServer.on("/api/tmc", HTTP_POST, handleApiTmc);
    httpServer.on("/api/clearfault", HTTP_POST, handleApiClearFault);
    httpServer.on("/api/gen", HTTP_GET, handleApiGen);
    httpServer.on("/api/gen", HTTP_POST, handleApiGen);
    httpServer.on("/api/interp", HTTP_GET, handleApiInterp);
    httpServer.on("/api/interp", HTTP_POST, handleApiInterp);
    httpServer.on("/api/log", HTTP_GET, handleApiLog);
    httpServer.on("/api/mode", HTTP_GET, handleApiMode);
    httpServer.on("/api/mode", HTTP_POST, handleApiMode);

    httpServer.begin();
    APPLOGF("HTTP server on port %d", HTTP_PORT);

    // Register TCode parser callbacks.
    tcodeParser.onLinearRampTo(buttplugLinearCmd);
    tcodeParser.onLinearStop(buttplugStop);

    // Start the WS TCode server (always up — MultiFunPlayer can connect).
    wsTransport.begin();

    // Bring up the saved transport (WS / SER / BT).
    transportMgr.applyTransport(g_state.getTransport());

    // Create FreeRTOS tasks
    xTaskCreatePinnedToCore(motorTask, "Motor", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(commsTask, "Comms", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(httpTask, "HTTP", 4096, NULL, 1, NULL, 0);

    generator.init();
    interpolator.init();

    APPLOG("System ready - push shaft into endstop to home, or use the web UI");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}