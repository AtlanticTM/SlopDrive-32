#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <ArduinoJson.h>

#include <LittleFS.h>


#include "config_api.h"
#include "motor.h"
#include "buttplug.h"
#include "range_mapper.h"
#include "AppLog.h"
#include "Kinematics.h"
#include "SystemState.h"

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
// Global Instances
// ============================================================================

MotorController motor;
ButtplugServer buttplug;
RangeMapper mapper;
WebServer httpServer(HTTP_PORT);
Preferences prefs;

// Centralised, thread-safe runtime state (replaces ~30 file-scope globals).
// All cross-core scalars are volatile (32-bit aligned, hardware-atomic on
// ESP32-S3).  The ring buffer is protected by its own portMUX.
static SystemState g_state;


// ============================================================================
// Device Configuration (runtime state)
// ============================================================================

// NOTE: All previous file-scope globals (g_config, g_driver, g_homed, etc.)
// are now fields of g_state.  The struct definitions (GeneratorConfig,
// InputMode, BufSample) live in include/system/SystemState.h.


// Forward declarations (definitions live further down).
static void applyTransport(TransportMode mode);
static const char* transportName(TransportMode m);




// Control gating blend constant (unchanged).
static const uint32_t RESUME_BLEND_MS = 800;



// ============================================================================
// WiFi Configuration
// ============================================================================

static void setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoConnect(true);
    WiFi.setAutoReconnect(true);

    APPLOGF("Connecting to WiFi: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        APPLOGF("WiFi connected! IP: %s", WiFi.localIP().toString().c_str());

        // Setup mDNS
        if (MDNS.begin(MDNSServiceName)) {
            MDNS.addService("http", "tcp", HTTP_PORT);
            MDNS.addService("ws", "tcp", BUTTPLUG_WEBSOCKET_PORT);
            APPLOGF("mDNS: http://%s.local:%d", MDNSServiceName, HTTP_PORT);
        }

        g_state.wifi_ready = true;
    } else {
        APPLOG("WiFi connection failed!");
    }
}

// ============================================================================
// Configuration Persistence (Preferences/NVS)
// ============================================================================

// Persist all settings to NVS. PREVIOUS BUG: this wrote with prefs.* without
// ever opening the namespace for WRITING - loadConfig() had opened it read-only
// and then prefs.end()'d it, so every putX() here silently no-op'd and nothing
// was ever saved. We now open the namespace read-write for the duration of the
// save and close it afterward.
static void saveConfig() {
    if (!prefs.begin("strokeengine", false)) {   // false = read-WRITE
        APPLOG("saveConfig: failed to open NVS for write!");
        return;
    }
    prefs.putFloat("range_min", mapper.getMinMm());
    prefs.putFloat("range_max", mapper.getMaxMm());
    prefs.putUShort("max_speed", (uint16_t)g_state.config.max_speed_mm_s);
    prefs.putUShort("accel", (uint16_t)g_state.config.acceleration_mm_s2);
    prefs.putUShort("lookahead", motor.getLookaheadMs());
    prefs.putUShort("overshoot", (uint16_t)motor.getMaxOvershootMm());
    prefs.putBool("auto_dur", g_state.auto_duration);
    prefs.putFloat("def_rmin", g_state.default_range_min);
    prefs.putFloat("def_rmax", g_state.default_range_max);
    prefs.putBool("expert", g_state.expert_mode);
    prefs.putUChar("transport", (uint8_t)g_state.getTransport());

    // Input mode + buffered interpolation + generator tick rate
    prefs.putUChar("input_mode", (uint8_t)g_state.getInputMode());
    prefs.putUChar("buf_easing", g_state.buf_easing);
    prefs.putUChar("buf_depth", g_state.buf_depth);
    prefs.putUShort("buf_tick", g_state.buf_tick_hz);
    prefs.putUShort("gen_tick", g_state.gen_rate_tick_hz);




    // TMC driver tunables (from the Motor tab)
    prefs.putUShort("tmc_run", g_state.driver.run_current_ma);
    prefs.putUChar("tmc_hold", g_state.driver.hold_current_pct);
    prefs.putUChar("tmc_sc", g_state.driver.stealthchop);
    prefs.putUInt("tmc_tpwm", g_state.driver.tpwm_thrs);
    prefs.putUChar("tmc_toff", g_state.driver.toff);
    prefs.putUChar("tmc_tbl", g_state.driver.tbl);
    prefs.putChar("tmc_hs", g_state.driver.hstart);
    prefs.putChar("tmc_he", g_state.driver.hend);

    prefs.end();
    APPLOG("Config saved to NVS");
}

static void loadConfig() {
    // Always start with defaults
    g_state.config = getDefaultConfig();
    g_state.driver = DriverConfig();   // default-constructed = config.h defaults
    mapper.setRange(g_state.config.min_position_mm, g_state.config.max_position_mm);

    // Try to load saved values (read-only mode)
    if (prefs.begin("strokeengine", true)) {
        float rmin = prefs.getFloat("range_min", g_state.config.min_position_mm);
        float rmax = prefs.getFloat("range_max", g_state.config.max_position_mm);
        uint16_t spd = prefs.getUShort("max_speed", (uint16_t)g_state.config.max_speed_mm_s);
        uint16_t acc = prefs.getUShort("accel", (uint16_t)g_state.config.acceleration_mm_s2);
        uint16_t look = prefs.getUShort("lookahead", 20);
        uint16_t over = prefs.getUShort("overshoot", 8);
        g_state.auto_duration = prefs.getBool("auto_dur", true);
        g_state.default_range_min = prefs.getFloat("def_rmin", 0.0f);
        g_state.default_range_max = prefs.getFloat("def_rmax", PHYSICAL_MAX_TRAVEL_MM);
        g_state.expert_mode = prefs.getBool("expert", false);
        {
            TransportMode t = (TransportMode)prefs.getUChar("transport", (uint8_t)DEFAULT_TRANSPORT_MODE);
            if ((uint8_t)t > (uint8_t)TransportMode::BT) t = DEFAULT_TRANSPORT_MODE;
            g_state.setTransport(t);
        }

        // Input mode + buffered interpolation + generator tick rate
        {
            InputMode im = (InputMode)prefs.getUChar("input_mode", (uint8_t)InputMode::BUFFERED);
            if ((uint8_t)im > (uint8_t)InputMode::BUFFERED) im = InputMode::EXTRAPOLATE;
            g_state.setInputMode(im);
        }
        g_state.buf_easing = constrain((int)prefs.getUChar("buf_easing", g_state.buf_easing), 0, 4);
        g_state.buf_depth  = constrain((int)prefs.getUChar("buf_depth", g_state.buf_depth), 1, 5);
        { uint16_t bt = prefs.getUShort("buf_tick", g_state.buf_tick_hz);
          g_state.buf_tick_hz = (bt >= 75) ? 100 : (bt >= 35) ? 50 : 20; }
        { uint16_t gt = prefs.getUShort("gen_tick", g_state.gen_rate_tick_hz);
          g_state.gen_rate_tick_hz = (gt >= 75) ? 100 : (gt >= 35) ? 50 : 20; }

        // Validate startup defaults; fall back to full travel if nonsensical.

        if (g_state.default_range_min < 0.0f || g_state.default_range_min > PHYSICAL_MAX_TRAVEL_MM ||
            g_state.default_range_max <= 0.0f || g_state.default_range_max > PHYSICAL_MAX_TRAVEL_MM ||
            g_state.default_range_min >= g_state.default_range_max) {
            g_state.default_range_min = 0.0f;
            g_state.default_range_max = PHYSICAL_MAX_TRAVEL_MM;
        }


        // TMC tunables
        g_state.driver.run_current_ma   = prefs.getUShort("tmc_run", g_state.driver.run_current_ma);
        g_state.driver.hold_current_pct = prefs.getUChar("tmc_hold", g_state.driver.hold_current_pct);
        g_state.driver.stealthchop      = prefs.getUChar("tmc_sc", g_state.driver.stealthchop);
        g_state.driver.tpwm_thrs        = prefs.getUInt("tmc_tpwm", g_state.driver.tpwm_thrs);
        g_state.driver.toff             = prefs.getUChar("tmc_toff", g_state.driver.toff);
        g_state.driver.tbl              = prefs.getUChar("tmc_tbl", g_state.driver.tbl);
        g_state.driver.hstart           = prefs.getChar("tmc_hs", g_state.driver.hstart);
        g_state.driver.hend             = prefs.getChar("tmc_he", g_state.driver.hend);
        prefs.end();

        // Validate: reject 0 or out-of-range values
        if (rmin < 0.0f || rmin > PHYSICAL_MAX_TRAVEL_MM) rmin = g_state.config.min_position_mm;
        if (rmax <= 0.0f || rmax > PHYSICAL_MAX_TRAVEL_MM) rmax = g_state.config.max_position_mm;
        if (rmin >= rmax) { rmin = g_state.config.min_position_mm; rmax = g_state.config.max_position_mm; }
        if (spd == 0 || spd > (uint16_t)MAX_SPEED_MM_S) spd = (uint16_t)g_state.config.max_speed_mm_s;
        if (acc == 0 || acc > 5000) acc = (uint16_t)g_state.config.acceleration_mm_s2;
        if (g_state.driver.run_current_ma < 250 || g_state.driver.run_current_ma > 3000)
            g_state.driver.run_current_ma = TMC_RUN_CURRENT_MA;
        if (g_state.driver.toff < 1 || g_state.driver.toff > 15) g_state.driver.toff = TMC_TOFF;

        mapper.setRange(rmin, rmax);
        g_state.config.max_speed_mm_s = (float)spd;
        g_state.config.acceleration_mm_s2 = (float)acc;
        g_state.config.run_current_ma = g_state.driver.run_current_ma;

        // Apply persisted inertia/predictive-smoothing settings to the motor.
        if (look > 200) look = 20;
        if (over > 50) over = 8;
        motor.setLookaheadMs(look);
        motor.setMaxOvershootMm((float)over);

        APPLOGF("Config: range=[%.1f, %.1f] speed=%.0f accel=%.0f run=%umA look=%u over=%u",
                rmin, rmax, (float)spd, (float)acc, g_state.driver.run_current_ma, look, over);
    } else {
        prefs.end();
        APPLOG("No saved config, using defaults");
    }
}

// ============================================================================
// Transport mode management
// ============================================================================

// Apply the active transport: bring up the chosen path and tear down the others
// so exactly one transport is live. Safe to call repeatedly.
//   WS  -> WebSocket server + (if WiFi) Intiface WSDM client; BLE off.
//   SER -> USB Serial TCode (polled in buttplugTask); BLE off, WSDM off.
//   BT  -> BLE Nordic-UART service; WSDM client off.
// Note: the WebSocket SERVER always stays up (it's how MultiFunPlayer connects
// and is cheap), but we only start the outbound Intiface client in WS mode.
static void applyTransport(TransportMode mode) {
    g_state.setTransport(mode);

    if (mode == TransportMode::BT) {
        buttplug.beginBLE();
        buttplug.disconnectIntiface();
    } else {
        if (buttplug.isBleRunning()) buttplug.stopBLE();
        if (mode == TransportMode::WS) {
#if INTIFACE_ENABLED
            if (g_state.wifi_ready) buttplug.connectIntiface(INTIFACE_HOST, INTIFACE_PORT);
#endif
        } else {  // SER
            buttplug.disconnectIntiface();
        }
    }

    const char* name = (mode == TransportMode::WS) ? "WS"
                     : (mode == TransportMode::SER) ? "SER" : "BT";
    APPLOGF("Transport mode: %s", name);
}

// Short tag string for the status chip / API.
static const char* transportName(TransportMode m) {
    switch (m) {
        case TransportMode::SER: return "SER";
        case TransportMode::BT:  return "BT";
        default:                 return "WS";
    }
}

// ============================================================================
// HTTP Server - Web UI Pages
// ============================================================================


// The full web UI now lives in data/index.html and is served from the LittleFS
// filesystem partition (flashed separately via "Upload Filesystem Image").
// This tiny fallback page is only shown if the filesystem wasn't uploaded, so
// the device is still reachable and tells the user what to do.
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

// Serve the web UI from LittleFS. The full UI is in data/index.html (uploaded
// as a filesystem image). If the file is missing, fall back to a short page
// that tells the user to upload the filesystem.
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
    doc["buttplug_connected"] = buttplug.isConnected();
    doc["position"] = motor.getPosition();
    // Live measured command cadence from the controlling app. Use the STABLE
    // windowed frame count (frames per 1s window) rather than the jittery
    // instantaneous interval, so the UI reads steady instead of jumping around.
    uint16_t hz = g_state.measured_hz;
    doc["measured_hz"] = hz;
    doc["measured_interval_ms"] = (hz > 0) ? (uint16_t)(1000 / hz) : 0;
    doc["auto_duration"] = g_state.auto_duration;
    doc["serial_mode"] = (bool)SERIAL_CONTROL_MODE;
    doc["serial_active"] = buttplug.isSerialActive();
    doc["serial_linked"] = buttplug.isSerialLinked();

    // Active transport (WS / SER / BT) + BLE link state for the status chip.
    doc["transport"] = transportName(g_state.getTransport());
    doc["ble_running"]   = buttplug.isBleRunning();
    doc["ble_connected"] = buttplug.isBleConnected();
    doc["ble_linked"]    = buttplug.isBleLinked();


    // Control-gating state for the web UI's sticky bar / banners.
    doc["paused"] = g_state.paused;
    doc["manual_override"] = g_state.manual_override;


    // --- Live TMC2160 driver health (from the background diag poll task) ---
    // These come from the cached DRV_STATUS read (no SPI access in this handler).
    DriverStatus ds = motor.getLastDriverStatus();
    JsonObject drv = doc["driver"].to<JsonObject>();
    drv["valid"]     = ds.valid;
    drv["otpw"]      = ds.otpw;          // overtemp pre-warning
    drv["ot"]        = ds.ot;            // overtemp shutdown
    drv["s2ga"]      = ds.s2ga;          // short-to-ground coil A
    drv["s2gb"]      = ds.s2gb;          // short-to-ground coil B
    drv["ola"]       = ds.ola;           // open load coil A
    drv["olb"]       = ds.olb;           // open load coil B
    drv["stst"]      = ds.stst;          // standstill
    drv["sg_result"] = ds.sg_result;     // raw StallGuard 0..1023
    drv["cs_actual"] = ds.cs_actual;     // actual current scale 0..31
    drv["load_pct"]  = motor.getLoadPercent();   // 0..100% mechanical load
    drv["faulted"]   = motor.isDriverFaulted();  // latched safety trip

    String json;
    serializeJson(doc, json);
    httpServer.send(200, "application/json", json);
}

// Clears the latched short-to-ground safety trip. The user must physically fix
// the wiring fault first; this just re-arms the firmware so motion is allowed
// again (a re-home is still required since power was cut).
void handleApiClearFault() {
    motor.clearDriverFault();
    APPLOG("Driver fault cleared by user (re-home required)");
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

        // Clamp accel to the safe band the motor controller accepts (10..5000).
        if (accel < 200) accel = 200;
        if (accel > 5000) accel = 5000;

        // Inertia / predictive smoothing (optional fields).
        uint16_t lookahead = doc["lookahead"] | (uint16_t)motor.getLookaheadMs();
        float overshoot = doc["overshoot"] | motor.getMaxOvershootMm();
        if (lookahead > 200) lookahead = 200;  // sane ceiling
        if (overshoot < 0.0f) overshoot = 0.0f;
        if (overshoot > 50.0f) overshoot = 50.0f;

        // Auto-duration toggle (override app's I value with measured cadence).
        g_state.auto_duration = doc["auto_duration"] | g_state.auto_duration;

        // Expert mode (unlocks unsafe slider ranges in the UI) + startup default
        // range. Validate the defaults before accepting them.
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
        motor.setAcceleration(g_state.config.acceleration_mm_s2);  // applies live + future moves
        motor.setLookaheadMs(lookahead);                     // applies live
        motor.setMaxOvershootMm(overshoot);                  // applies live
        // Live realtime overrides from the Control tab send no_persist=true so
        // we apply them to the motor immediately WITHOUT hammering NVS on every
        // slider tick. Only full "Save" posts (no_persist absent/false) persist.
        bool no_persist = doc["no_persist"] | false;
        if (!no_persist) saveConfig();


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
        // bypass_limits lets the user drive past the mapped working window
        // (e.g. to loosen up or test depth) — we still clamp to physical travel.
        bool bypass = doc["bypass_limits"] | false;
        if (bypass) {
            pos = constrain(pos, 0.0f, PHYSICAL_MAX_TRAVEL_MM);
        } else {
            pos = constrain(pos, mapper.getMinMm(), mapper.getMaxMm());
        }
        // Streamed slider drags send {"stream":true} (the default for the live
        // position slider). streamTo() NEVER force-stops + busy-waits, so rapid
        // consecutive requests re-plan smoothly instead of queuing up and then
        // snapping to the last one (the "drag lags then makes one big move at the
        // end" buffering symptom). A one-shot positioning move can pass
        // {"stream":false} to use the settling moveTo() instead. An optional
        // per-command "speed" (mm/s) lets the slider cap travel velocity.
        bool stream = doc["stream"] | true;
        float speed = doc["speed"] | 0.0f;   // 0 = use configured max speed
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
    // Allow homing if not already homed or homing, OR if previously stopped (g_state.homed reset)
    if (!g_state.homing_in_progress) {
        g_state.homed = false;
        g_state.homing_in_progress = true;
        motor.home();
    }
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

void handleApiStop() {
    motor.stop();
    // Motor power is cut - position is unknown, require re-homing
    g_state.homed = false;
    g_state.homing_in_progress = false;
    // E-Stop also clears any control gating - fresh start after re-home.
    g_state.paused = false;
    g_state.manual_override = false;
    g_state.resume_start_ms = 0;
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

// Pause: ignore Intiface input and stop motion (motor stays homed/powered).
// While paused the user can drive via the manual slider / waveform generator.
// POST {"paused": true|false}; omitting it toggles. On UN-pausing we start the
// slow-resume blend so Intiface eases the toy back rather than snapping.
void handleApiPause() {
    JsonDocument doc;
    deserializeJson(doc, httpServer.arg("plain"));
    bool was = g_state.paused;
    g_state.paused = doc["paused"] | (!g_state.paused);
    if (g_state.paused && !was) {
        if (motor.isHomed()) motor.hardStop();   // stop but keep position
        APPLOG("Paused: ignoring Intiface input");
    } else if (!g_state.paused && was) {
        g_state.resume_start_ms = millis();             // begin slow resume
        APPLOG("Unpaused: resuming Intiface input");
    }
    String json; JsonDocument r; r["ok"] = true; r["paused"] = g_state.paused;
    serializeJson(r, json);
    httpServer.send(200, "application/json", json);
}

// Halt: immediate motion stop, motor stays powered and homed. Does NOT change
// Intiface gating - the next command (manual or Intiface) simply moves again.
void handleApiHalt() {
    if (motor.isHomed()) motor.hardStop();
    APPLOG("Halt: motor stopped (still homed/powered)");
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

// Override: persistent manual takeover of Intiface (same gating as pause but a
// deliberate, sticky state). POST {"override": true|false}.
void handleApiOverride() {
    JsonDocument doc;
    deserializeJson(doc, httpServer.arg("plain"));
    bool was = g_state.manual_override;
    g_state.manual_override = doc["override"] | (!g_state.manual_override);
    if (g_state.manual_override && !was) {
        APPLOG("Manual override ON: Intiface input ignored");
    } else if (!g_state.manual_override && was) {
        g_state.resume_start_ms = millis();   // slow resume back into Intiface
        APPLOG("Manual override OFF: easing back to Intiface");
    }
    String json; JsonDocument r; r["ok"] = true; r["manual_override"] = g_state.manual_override;
    serializeJson(r, json);
    httpServer.send(200, "application/json", json);
}


// Motor / TMC live-tuning endpoint.
//   GET  -> current TMC settings as JSON (for the Motor tab to load)
//   POST -> {run_current,hold_current,stealthchop,tpwm_thrs,toff,tbl,hstart,hend,save,reset}
//           Applies the values to the driver LIVE. If "save":true also persists
//           them to NVS. If "reset":true restores defaults (then applies/saves).
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

    // POST
    JsonDocument doc;
    if (deserializeJson(doc, httpServer.arg("plain"))) {
        httpServer.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    if (doc["reset"] | false) {
        g_state.driver = DriverConfig();   // back to config.h defaults
    } else {
        // Read with the current value as the default, then clamp to safe ranges.
        g_state.driver.run_current_ma   = constrain((int)(doc["run_current"]  | (int)g_state.driver.run_current_ma), 300, 3000);
        g_state.driver.hold_current_pct = constrain((int)(doc["hold_current"] | (int)g_state.driver.hold_current_pct), 0, 100);
        g_state.driver.stealthchop      = (doc["stealthchop"] | (int)g_state.driver.stealthchop) ? 1 : 0;
        g_state.driver.tpwm_thrs        = (uint32_t)(doc["tpwm_thrs"] | (long)g_state.driver.tpwm_thrs);
        g_state.driver.toff             = constrain((int)(doc["toff"]   | (int)g_state.driver.toff), 1, 15);
        g_state.driver.tbl              = constrain((int)(doc["tbl"]    | (int)g_state.driver.tbl), 0, 3);
        g_state.driver.hstart           = constrain((int)(doc["hstart"] | (int)g_state.driver.hstart), 0, 7);
        g_state.driver.hend             = constrain((int)(doc["hend"]   | (int)g_state.driver.hend), -3, 12);
    }

    // Keep g_state.config.run_current_ma in sync (used elsewhere) and apply live.
    g_state.config.run_current_ma = g_state.driver.run_current_ma;
    motor.applyDriverConfig(g_state.driver);

    bool save = doc["save"] | false;
    if (save) saveConfig();

    JsonDocument resp;
    resp["ok"] = true;
    String json;
    serializeJson(resp, json);
    httpServer.send(200, "application/json", json);
}

// Serial / debug log viewer: returns the in-memory ring buffer as plain text so
// the web UI can show what would otherwise have gone to the serial monitor
// (which is now occupied by Intiface TCode in serial-control mode).
void handleApiLog() {
    String out;
    out.reserve(2048);
    applogDump(out);
    httpServer.send(200, "text/plain", out);
}

// Transport-mode endpoint.
//   GET  -> {"mode":"WS"|"SER"|"BT", "ble_running":bool, "ble_connected":bool}
//   POST -> {"mode":"WS"|"SER"|"BT"}  switch transport live + persist to NVS.
void handleApiMode() {
    if (httpServer.method() == HTTP_GET) {
        JsonDocument doc;
        doc["mode"]          = transportName(g_state.getTransport());
        doc["ble_running"]   = buttplug.isBleRunning();
        doc["ble_connected"] = buttplug.isBleConnected();
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

    applyTransport(mode);
    saveConfig();   // persist the selection

    JsonDocument resp;
    resp["ok"]   = true;
    resp["mode"] = transportName(g_state.getTransport());
    String json;
    serializeJson(resp, json);
    httpServer.send(200, "application/json", json);
}


// ============================================================================
// On-device motion generator
// ============================================================================
// Waveform/easing math lives in motion/Kinematics.h (namespace kinematics).

// GET  -> current generator parameters + live state
// POST -> any subset of {running,wave,rate,depth,offset,ease,mod,mod_wave,
//         mod_rate,mod_amp}. Applied live; held in RAM only (the generator is a
//         session/play feature, not a persisted setting).

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
        doc["rate_tick"] = g_state.gen_rate_tick_hz;   // local generation rate (Hz)
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

    // Local generation tick rate (20/50/100 Hz). Snap to the nearest allowed
    // bucket so the UI radio buttons and firmware always agree.
    if (doc["rate_tick"].is<int>()) {
        int r = doc["rate_tick"];
        g_state.gen_rate_tick_hz = (r >= 75) ? 100 : (r >= 35) ? 50 : 20;
    }


    // Parameters (percent-based fields arrive 0..100 and are stored 0..1).
    g_state.gen.wave     = constrain((int)(doc["wave"]     | (int)g_state.gen.wave), 0, 3);

    g_state.gen.rate_hz  = constrain((float)(doc["rate"]   | g_state.gen.rate_hz), 0.05f, 50.0f);
    if (doc["depth"].is<float>())  g_state.gen.depth  = constrain((float)doc["depth"]  / 100.0f, 0.02f, 1.0f);
    if (doc["offset"].is<float>()) g_state.gen.offset = constrain((float)doc["offset"] / 100.0f, 0.0f, 1.0f);
    if (doc["ease"].is<float>())   g_state.gen.ease   = constrain((float)doc["ease"]   / 100.0f, 0.0f, 1.0f);
    g_state.gen.mod      = constrain((int)(doc["mod"]      | (int)g_state.gen.mod), 0, 2);
    g_state.gen.mod_wave = constrain((int)(doc["mod_wave"] | (int)g_state.gen.mod_wave), 0, 2);
    g_state.gen.mod_rate = constrain((float)(doc["mod_rate"] | g_state.gen.mod_rate), 0.01f, 5.0f);
    // mod_amp is a modulation SWING in Hz (how far the rate is pushed for FM, or
    // the equivalent depth-reduction reference for AM).
    g_state.gen.mod_amp  = constrain((float)(doc["mod_amp"] | g_state.gen.mod_amp), 0.0f, 10.0f);


    // Start/stop. On a fresh start we reset the phase/clock so motion begins from
    // the bottom of the stroke rather than wherever the phase happened to be.
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

// Input-mode / buffered-interpolation endpoint.
//   GET  -> current mode + buffer settings + live state.
//   POST -> any subset of {mode,easing,depth,tick,save}. Applied live; if
//           "save":true also persisted to NVS.
// Fields:
//   mode   "extrapolate" | "buffered"
//   easing 0=linear 1=ease-in-out 2=ease-in 3=ease-out 4=ease-in-out-cubic
//   depth  1..5  (samples of look-behind before playback starts)
//   tick   20|50|100 (local interpolation rate, Hz)
void handleApiInterp() {
    if (httpServer.method() == HTTP_GET) {
        JsonDocument doc;
        doc["mode"]      = (g_state.getInputMode() == InputMode::BUFFERED) ? "buffered" : "extrapolate";
        doc["easing"]    = g_state.buf_easing;
        doc["depth"]     = g_state.buf_depth;
        doc["tick"]      = g_state.buf_tick_hz;
        doc["active"]    = g_state.buf_active;      // emitting interpolated motion now?
        doc["buffered"]  = g_state.buf_count;       // samples currently queued
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
        g_state.buf_tick_hz = (t >= 75) ? 100 : (t >= 35) ? 50 : 20;   // snap to bucket
    }

    bool save = doc["save"] | false;
    if (save) saveConfig();

    APPLOGF("Interp: mode=%s easing=%u depth=%u tick=%uHz",
            (g_state.getInputMode() == InputMode::BUFFERED) ? "buffered" : "extrapolate",
            g_state.buf_easing, g_state.buf_depth, g_state.buf_tick_hz);

    httpServer.send(200, "application/json", "{\"ok\":true}");
}

// Generator task: when running, computes the carrier waveform (with optional

// FM/AM modulation) and streams target positions to the motor at ~100 Hz. It
// auto-yields to Intiface — if real app commands arrived in the last 250 ms and
// the user hasn't paused/overridden, the generator goes idle so it doesn't fight
// the app. Motion is mapped into the live stroke window (or full travel while
// the window is bypassed is NOT used here — the generator always uses the
// configured working window).
void generatorTask(void* parameter) {
    uint32_t last_diag_ms = 0;
    while (true) {
        // Local generation rate is user-selectable (20/50/100 Hz). Higher =
        // smoother motion / more step planning; lower = lighter CPU. Re-read
        // each loop so live changes from /api/gen take effect immediately.
        uint16_t ghz = g_state.gen_rate_tick_hz; if (ghz < 5) ghz = 5; if (ghz > 200) ghz = 200;
        const TickType_t period = pdMS_TO_TICKS(1000 / ghz);

        bool intiface_recent = (g_state.last_intiface_ms != 0) &&
                               (millis() - g_state.last_intiface_ms < 250);
        bool user_has_control = g_state.paused || g_state.manual_override;
        bool emit = g_state.gen.running && g_state.homed &&
                    (user_has_control || !intiface_recent);
        g_state.gen_active = emit;

        // Diagnostics: while the generator is running, log once per second so the
        // Log tab reflects what it's doing — either why it's idle, or a heartbeat
        // with the live target while it's actually driving the motor.
        if (g_state.gen.running && (millis() - last_diag_ms > 1000)) {
            last_diag_ms = millis();
            if (!g_state.homed)
                APPLOG("Generator: waiting - motor not homed");
            else if (intiface_recent && !user_has_control)
                APPLOG("Generator: yielding to active Intiface (use Pause/Override to keep control)");
            else
            APPLOGF("Generator: running target=%.1fmm phase=%.2f window=[%.0f,%.0f]",
                    mapper.getMinMm() + g_state.gen.offset * (mapper.getMaxMm() - mapper.getMinMm())
                        + (kinematics::ease(kinematics::carrier(g_state.gen.wave, g_state.gen_phase), g_state.gen.ease) - 0.5f)
                          * g_state.gen.depth * (mapper.getMaxMm() - mapper.getMinMm()),
                        g_state.gen_phase, mapper.getMinMm(), mapper.getMaxMm());
        }



        if (emit) {
            uint32_t now_us = micros();
            float dt = (g_state.gen_last_us != 0) ? (now_us - g_state.gen_last_us) / 1e6f : 0.0f;
            g_state.gen_last_us = now_us;
            if (dt < 0.0f || dt > 0.5f) dt = 0.0f;   // guard against wrap/stall

            float rate  = g_state.gen.rate_hz;
            float depth = g_state.gen.depth;

            // Advance the modulator clock and apply FM (rate) or AM (depth).
            if (g_state.gen.mod != 0) {
                g_state.gen_mod_clock += dt * g_state.gen.mod_rate;
                if (g_state.gen_mod_clock > 1.0f) g_state.gen_mod_clock -= floorf(g_state.gen_mod_clock);
                float m = kinematics::modShape(g_state.gen.mod_wave, g_state.gen_mod_clock);
                if (g_state.gen.mod == 1) {        // rate FM: swing rate by +/- mod_amp Hz
                    rate = fmaxf(0.05f, rate + (m - 0.5f) * 2.0f * g_state.gen.mod_amp);
                } else {                     // depth AM: reduce depth, scaled by mod_amp (Hz) vs rate
                    float a = constrain(g_state.gen.mod_amp / fmaxf(g_state.gen.rate_hz, 0.1f), 0.0f, 1.0f);
                    depth = constrain(depth * (1.0f - 0.5f * m * a), 0.02f, 1.0f);
                }

            }

            // Advance carrier phase and map into the working window.
            g_state.gen_phase += dt * rate;
            if (g_state.gen_phase > 1.0f) g_state.gen_phase -= floorf(g_state.gen_phase);
            float c = kinematics::ease(kinematics::carrier(g_state.gen.wave, g_state.gen_phase), g_state.gen.ease);

            float lo = mapper.getMinMm(), hi = mapper.getMaxMm();
            float span = hi - lo;
            float center = lo + g_state.gen.offset * span;
            float pos = center + (c - 0.5f) * depth * span;
            pos = constrain(pos, lo, hi);

            // streamTo() re-targets without force-stopping; at 100 Hz the motor
            // glides smoothly through the waveform. speed 0 => configured max.
            motor.streamTo(pos, 0.0f);
        } else {
            g_state.gen_last_us = 0;   // reset dt so resume doesn't take a giant step
        }

        vTaskDelay(period);
    }
}

// ============================================================================
// Buffered interpolation (BUFFERED input mode)
// ============================================================================
// bufEase() lives in motion/Kinematics.h (namespace kinematics).

// Push a true Intiface sample (already range-mapped to mm) into the ring buffer.
// Called from buttplugLinearCmd() while in BUFFERED mode. The interpolatorTask
// consumes these by replaying them on a time-delayed playback clock.
//
// Timestamps are taken with esp_timer_get_time() (microseconds, monotonic) so
// the playback clock and the sample times share one wall-clock reference.
static void bufPushSample(float pos_mm) {
    portENTER_CRITICAL(&g_state.buf_mux);
    g_state.buf[g_state.buf_head] = { pos_mm, (uint32_t)(esp_timer_get_time() / 1000ULL) };
    g_state.buf_head = (g_state.buf_head + 1) % SystemState::BUF_CAP;
    if (g_state.buf_count < SystemState::BUF_CAP) g_state.buf_count++;
    portEXIT_CRITICAL(&g_state.buf_mux);
}

// Interpolator task — TIME-DELAYED JITTER BUFFER.
//
// Why the old "advance a cursor by the assumed tick period" approach jittered:
//   1. vTaskDelay() is NOT isochronous. With WiFi+BLE+web-server running, this
//      task is preempted; a "20ms" tick can land anywhere from 18 to 45ms. The
//      old code advanced the cursor by a FIXED 1000/hz ms regardless, so late
//      ticks under-advanced and the carriage stuttered.
//   2. It used each sample's raw BLE arrival delta (b.t_ms - a.t_ms) as the
//      segment duration. BLE delivers in bursts (connection-interval batching),
//      so those deltas are themselves jittery — feeding transport jitter
//      straight into motor velocity.
//   3. `depth` was only a look-behind COUNT, giving no real time cushion, so any
//      BLE gap bigger than the tiny backlog drained the ring → freeze → refill.
//
// The fix is the standard media jitter-buffer model: timestamp every sample on
// arrival, then PLAY BACK on a clock that runs a fixed delay (PLAY_DELAY_MS)
// behind real wall-clock time. Each tick we:
//   * read the REAL elapsed time (esp_timer, immune to tick jitter),
//   * compute play_time = now - PLAY_DELAY_MS,
//   * find the two buffered samples that bracket play_time,
//   * interpolate by the TRUE time fraction between them.
// Because we always render "the past", bursty/late arrivals are already in the
// buffer by the time we need them, and uneven tick spacing is corrected by
// using real time instead of an assumed step. Result: smooth motion even over
// jittery BLE.
void interpolatorTask(void* parameter) {
    // Playback clock, in the same ms timebase as the sample timestamps. 0 = not
    // yet started (we latch it on the first tick that has data).
    static uint32_t play_clock_ms = 0;
    static uint32_t last_real_us  = 0;

    while (true) {
        uint16_t hz = g_state.buf_tick_hz; if (hz < 5) hz = 5; if (hz > 200) hz = 200;
        TickType_t period = pdMS_TO_TICKS(1000 / hz);

        bool gated = g_state.paused || g_state.manual_override || !g_state.homed;
        bool run = (g_state.getInputMode() == InputMode::BUFFERED) && !gated;

        if (run) {
            uint32_t now_us = (uint32_t)esp_timer_get_time();
            uint32_t now_ms = now_us / 1000U;

            // The buffer delay (ms) the user dials in via `depth`. Each depth step
            // ~= 30ms of cushion (1->30ms .. 5->150ms). More delay = smoother ride
            // over BLE stalls at the cost of a little extra latency.
            uint8_t depth = g_state.buf_depth; if (depth < 1) depth = 1; if (depth > 5) depth = 5;
            uint32_t play_delay_ms = (uint32_t)depth * 30U;

            // Snapshot the ring.
            portENTER_CRITICAL(&g_state.buf_mux);
            uint8_t count = g_state.buf_count;
            uint8_t head  = g_state.buf_head;
            BufSample ring[SystemState::BUF_CAP];
            for (uint8_t i = 0; i < SystemState::BUF_CAP; i++) ring[i] = g_state.buf[i];
            portEXIT_CRITICAL(&g_state.buf_mux);

            if (count >= 2) {
                // Oldest valid sample index and the newest timestamp.
                uint8_t oldest_i = (uint8_t)((head + SystemState::BUF_CAP - count) % SystemState::BUF_CAP);
                uint8_t newest_i = (uint8_t)((head + SystemState::BUF_CAP - 1) % SystemState::BUF_CAP);
                uint32_t oldest_t = ring[oldest_i].t_ms;
                uint32_t newest_t = ring[newest_i].t_ms;

                // Advance the playback clock by REAL elapsed time (tick-jitter
                // immune). On the first primed tick, start it one buffer-delay
                // behind "now" so we render from the oldest available history.
                if (play_clock_ms == 0 || !g_state.buf_active) {
                    play_clock_ms = (now_ms > play_delay_ms) ? (now_ms - play_delay_ms) : oldest_t;
                    if (play_clock_ms < oldest_t) play_clock_ms = oldest_t;
                    last_real_us = now_us;
                    g_state.buf_active = true;
                } else {
                    uint32_t elapsed_ms = (now_us - last_real_us) / 1000U;
                    if (elapsed_ms > 0) {
                        play_clock_ms += elapsed_ms;
                        last_real_us = now_us;
                    }
                }

                // Clamp the playback clock into the available window:
                //  * never run past the newest sample we have (would extrapolate
                //    blindly); hold at newest if the buffer starved.
                //  * never fall so far behind that we'd render ancient history.
                uint32_t target_lag = newest_t - play_delay_ms;   // ideal play point
                if ((int32_t)(play_clock_ms - newest_t) > 0) {
                    play_clock_ms = newest_t;                      // starved: hold newest
                }
                // Gentle resync if we've drifted too far from the ideal lag
                // (keeps latency bounded without snapping).
                if (newest_t > play_delay_ms &&
                    (int32_t)(target_lag - play_clock_ms) > (int32_t)(play_delay_ms + 50)) {
                    play_clock_ms = target_lag;                    // jumped behind: catch up
                }
                if (play_clock_ms < oldest_t) play_clock_ms = oldest_t;

                // Find the pair of samples bracketing play_clock_ms.
                BufSample a = ring[oldest_i];
                BufSample b = ring[newest_i];
                for (uint8_t k = 0; k < count - 1; k++) {
                    uint8_t i0 = (uint8_t)((oldest_i + k) % SystemState::BUF_CAP);
                    uint8_t i1 = (uint8_t)((i0 + 1) % SystemState::BUF_CAP);
                    if ((int32_t)(play_clock_ms - ring[i0].t_ms) >= 0 &&
                        (int32_t)(ring[i1].t_ms - play_clock_ms) >= 0) {
                        a = ring[i0]; b = ring[i1];
                        break;
                    }
                }

                // True time fraction between the bracketing samples.
                float seg_ms = (float)(b.t_ms - a.t_ms);
                float t = (seg_ms > 0.5f)
                              ? ((float)((int32_t)(play_clock_ms - a.t_ms)) / seg_ms)
                              : 1.0f;
                t = constrain(t, 0.0f, 1.0f);

                float e = kinematics::bufEase(g_state.buf_easing, t);
                float pos = a.pos_mm + (b.pos_mm - a.pos_mm) * e;
                pos = constrain(pos, mapper.getMinMm(), mapper.getMaxMm());

                // Velocity to reach the next sample over its real remaining time,
                // so motion stays continuous (no per-tick speed spikes).
                float remain_ms = (float)((int32_t)(b.t_ms - play_clock_ms));
                if (remain_ms < 1.0f) remain_ms = seg_ms > 0.5f ? seg_ms : 20.0f;
                float dist = fabsf(b.pos_mm - pos);
                float spd  = (dist / remain_ms) * 1000.0f;
                spd = constrain(spd, 0.0f, g_state.config.max_speed_mm_s);
                motor.streamTo(pos, spd);

                // Retire samples strictly older than what we still need to
                // interpolate (keep the one just before play_clock_ms as `a`).
                portENTER_CRITICAL(&g_state.buf_mux);
                while (g_state.buf_count > 2) {
                    uint8_t o  = (uint8_t)((g_state.buf_head + SystemState::BUF_CAP - g_state.buf_count) % SystemState::BUF_CAP);
                    uint8_t o1 = (uint8_t)((o + 1) % SystemState::BUF_CAP);
                    // Drop the oldest only if the playback clock has already passed
                    // its successor (so `a/b` bracketing never loses its left edge).
                    if ((int32_t)(play_clock_ms - g_state.buf[o1].t_ms) > 0) g_state.buf_count--;
                    else break;
                }
                portEXIT_CRITICAL(&g_state.buf_mux);
            } else {
                // Fewer than 2 samples: not enough to interpolate yet. Don't
                // reset g_state.buf_active for a brief gap, but do pause the clock so it
                // doesn't run away while starved.
                last_real_us = now_us;
            }
        } else {
            // Not in buffered mode (or gated): clear ring/clock so a later switch
            // back into buffered mode starts fresh.
            g_state.buf_active = false;
            play_clock_ms = 0;
            last_real_us = 0;
            portENTER_CRITICAL(&g_state.buf_mux);
            g_state.buf_count = 0; g_state.buf_head = 0;
            portEXIT_CRITICAL(&g_state.buf_mux);
        }

        vTaskDelay(period);
    }
}


// ============================================================================
// Buttplug Callbacks - Map Buttplug position (0.0-1.0) to physical mm
// ============================================================================


static void buttplugLinearCmd(float position, uint32_t duration_ms) {

    if (!g_state.homed) return;
    // Pause or manual override: ignore Intiface entirely. The user is driving
    // via the manual slider / waveform generator instead. We also reset the
    // resume-blend origin so the FIRST command after un-gating starts the ease.
    if (g_state.paused || g_state.manual_override) { g_state.resume_start_ms = millis(); return; }

    // Stamp Intiface activity so the on-device generator can auto-yield: if real
    // app commands are arriving, the generator stops emitting motion (unless the
    // user has paused/overridden, in which case we never reach here anyway).
    g_state.last_intiface_ms = millis();

    // --- Measure the REAL command cadence from the app ---

    // Many apps hardcode the "I" duration (e.g. always I100) regardless of how
    // often they actually send. If they send every ~30ms but each claims 100ms,
    // the motor keeps planning a slow 100ms move it never finishes -> sluggish,
    // forcing you to crank acceleration way down. We measure the true interval
    // between arrivals and, when Auto Duration is on, use THAT as the move time.
    uint32_t now = millis();
    if (g_state.last_cmd_ms != 0) {
        uint32_t gap = now - g_state.last_cmd_ms;
        if (gap > 0 && gap < 1000) {  // ignore pauses/restarts
            // Exponential smoothing to reject single-sample jitter.
            if (g_state.measured_interval_ms <= 0.0f) g_state.measured_interval_ms = (float)gap;
            else g_state.measured_interval_ms = 0.7f * g_state.measured_interval_ms + 0.3f * (float)gap;
        }
    }
    g_state.last_cmd_ms = now;

    // Override the duration with the measured cadence when enabled and valid.
    if (g_state.auto_duration && g_state.measured_interval_ms > 1.0f) {
        duration_ms = (uint32_t)(g_state.measured_interval_ms + 0.5f);
    }

    // Map Buttplug normalized position (0.0-1.0) to physical mm using range mapper
    // 0.0 = min_position_mm (retracted), 1.0 = max_position_mm (extended)
    float target_mm = mapper.intensityToPosition(position);

    // --- BUFFERED input mode -------------------------------------------------
    // Instead of driving the motor directly, push this true sample into the ring
    // and let interpolatorTask resample/smooth it at the local tick rate. This
    // is the path that fixes sporadic/bursty apps. We still honor the slow-resume
    // blend by offsetting the FIRST sample after un-gating toward current pos.
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
        bufPushSample(target_mm);
        return;   // motion is emitted by interpolatorTask, not here
    }

    // Slow resume: just after pause/override is lifted, ease back toward where

    // Intiface wants us instead of snapping. Blend the target from the current
    // position toward the commanded target over RESUME_BLEND_MS.
    if (g_state.resume_start_ms != 0) {
        uint32_t elapsed = millis() - g_state.resume_start_ms;
        if (elapsed >= RESUME_BLEND_MS) {
            g_state.resume_start_ms = 0;  // blend complete
        } else {
            float blend = (float)elapsed / (float)RESUME_BLEND_MS;
            float cur = motor.getPosition();
            target_mm = cur + blend * (target_mm - cur);
        }
    }


    // Derive the speed needed to reach target in 'duration_ms' (the time Intiface
    // wants this move to take). This is the heart of smooth motion: each command
    // says "be HERE in N ms", and we compute the velocity to honor that.
    float speed_mm_s = 0.0f;  // 0 => motor uses configured max
    if (duration_ms > 0) {
        float current_mm = motor.getPosition();
        float distance_mm = fabsf(target_mm - current_mm);
        if (distance_mm > 0.05f) {
            speed_mm_s = (distance_mm / (float)duration_ms) * 1000.0f;
            // Cap to the user's configured max; never let it crawl to a stall.
            speed_mm_s = constrain(speed_mm_s, 5.0f, g_state.config.max_speed_mm_s);
        } else {
            // Tiny/zero move - keep a gentle speed so we don't snap.
            speed_mm_s = g_state.config.max_speed_mm_s * 0.25f;
        }
    }

    // streamExtrapolated() re-targets on the fly without force-stopping AND
    // projects slightly ahead based on the input stream's velocity, so motion
    // stays continuous between Intiface's discrete samples (the "inertia"
    // smoothing). updateExtrapolation() in the motor task handles stall safety.
    motor.streamExtrapolated(target_mm, speed_mm_s);
}

static void buttplugStop() {
    // Buttplug stop = pause movement, but keep motor homed
    // (unlike web UI Stop which cuts power and requires re-homing)
    if (motor.isHomed()) {
        motor.hardStop();  // Stop without cutting power - position is preserved
    }
}

// ============================================================================
// FreeRTOS Tasks
// ============================================================================

void motorTask(void* parameter) {
    while (true) {
        // Poll homing - runHomingStep() checks endstop and completes homing
        // Poll at 1ms intervals for fast endstop response
        if (g_state.homing_in_progress) {
            motor.runHomingStep();

            // Check if homing just completed
            if (!motor.isHoming()) {
                g_state.homing_in_progress = false;
                g_state.homed = motor.isHomed();
                if (g_state.homed) {
                    APPLOG("System is now homed and ready");
                }
            }
        } else if (!g_state.homed) {
            // Push-to-home: not homed and not running the active routine, so let
            // the user establish home by simply pushing the shaft into the
            // endstop (or by already resting on it at boot). No web UI needed.
            if (motor.checkPushToHome()) {
                g_state.homed = true;
                APPLOG("System homed via push-to-home and ready");
            }
        }


        // Poll position monitor - stops motor when runForward/runBackward reaches target
        motor.runMotorStep();

        // Predictive-extrapolation stall safety: if the Intiface sample stream
        // pauses, settle the motor on the last true sample instead of coasting.
        motor.updateExtrapolation();

        vTaskDelay(pdMS_TO_TICKS(1));  // 1ms = 1000Hz endstop polling
    }
}

void buttplugTask(void* parameter) {
    uint32_t last_report_ms = 0;
    uint32_t last_frame_count = 0;
    while (true) {
        buttplug.run();

#if SERIAL_CONTROL_MODE
        // Pull any TCode that Intiface streamed over the USB serial port and
        // feed it into the parser. This is the WiFi-free control path. Only do
        // this when SER is the selected transport so a stray serial monitor
        // doesn't inject motion while the user is on WS/BT.
        if (g_state.getTransport() == TransportMode::SER) buttplug.pollSerial();
#endif


        // Once-per-second diagnostic: print the RAW inbound WebSocket frame rate
        // (counted before any parsing/motor work). This definitively shows
        // whether a low command rate is coming from the app/Intiface (raw rate
        // already low) or being throttled inside the firmware (raw rate high but
        // measured motor rate low). Compare this "[RATE] rx=N/s" to the app.
        uint32_t now = millis();
        if (now - last_report_ms >= 1000) {
            uint32_t frames = buttplug.rxFrameCount;
            uint32_t per_sec = frames - last_frame_count;
            last_frame_count = frames;
            last_report_ms = now;
            // Publish the stable windowed rate for the web UI (frames/sec).
            g_state.measured_hz = (uint16_t)per_sec;
            if (buttplug.isConnected() || buttplug.isSerialActive()) {
                APPLOGF("[RATE] rx=%u frames/s", per_sec);
            }
        }

        // 2ms tick (~500Hz) so received WebSocket frames are drained quickly.
        // At 5ms, if multiple frames queue in the library they get serviced one
        // per loop and effective throughput drops; 2ms keeps the pipe clear.
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void httpTask(void* parameter) {
    while (true) {
        httpServer.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Driver-health monitor. Polls the TMC2160 DRV_STATUS register ~5x/second over
// SPI (serialized against config writes by the motor's internal mutex) so the
// web UI always has fresh temperature/short/StallGuard data. readDriverStatus()
// itself latches the hard short-to-ground safety trip and cuts motor power, so
// this task is also the active safety watchdog. We additionally surface a
// one-shot log line on overtemp pre-warning so it shows up in the log viewer.
void diagTask(void* parameter) {
    bool last_otpw = false;
    while (true) {
        DriverStatus s = motor.readDriverStatus();

        if (s.valid && s.otpw && !last_otpw) {
            APPLOG("WARNING: TMC2160 overtemperature pre-warning (otpw) - driver is HOT");
        }
        last_otpw = s.valid && s.otpw;

        // ~5 Hz: frequent enough to catch a fault fast, light enough on the bus.
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ============================================================================
// Setup & Loop
// ============================================================================

void setup() {
    // In serial-control mode the USB Serial port carries Intiface TCode, so we
    // open it at the agreed baud and route all debug to the in-memory web log.
    Serial.begin(SERIAL_CONTROL_BAUD);
    applogBegin();
    APPLOG("=== SlopDrive-32 StrokeEngine v2.0 ===");
#if SERIAL_CONTROL_MODE
    applog("Serial control mode ON: USB Serial is dedicated to Intiface TCode.");
    applog("Add a 'Serial' device in Intiface pointing at this COM port.");
#endif

    // Mount the LittleFS filesystem that holds the web UI (data/index.html).
    // begin(true) formats it if it's blank/corrupt so the device still boots.
    if (LittleFS.begin(true)) {
        APPLOG("LittleFS mounted");
    } else {
        APPLOG("LittleFS mount FAILED - upload filesystem image (pio run -t uploadfs)");
    }

    // Load saved configuration (loadConfig handles prefs.begin/end internally).
    // This also populates g_state.driver with any saved TMC tunables.
    loadConfig();


    // Initialize motor system
    // Note: motor.begin() applies TMC2160 config from config.h defaults.
    // applyDriverConfig() is called after to apply the loaded/saved TMC tunables.
    motor.begin();

    // Apply the loaded TMC driver settings (from NVS, or defaults) live.
    motor.applyDriverConfig(g_state.driver);

    // Setup WiFi
    setupWiFi();

    // Setup HTTP server routes
    httpServer.on("/", handleRoot);
    // The web UI is fully self-contained in data/index.html (CSS + JS inline),
    // so there's no separate /app.js route to serve.
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

    // Setup Buttplug WebSocket with callbacks
    buttplug.onLinearRampTo(buttplugLinearCmd);
    buttplug.onLinearStop(buttplugStop);
    buttplug.begin();

    // Bring up the SAVED transport (WS / SER / BT). applyTransport() starts the
    // chosen path and tears down the others so exactly one is live:
    //   WS  -> connects out to Intiface's WSDM server (needs WiFi)
    //   SER -> nothing extra; buttplugTask polls the USB serial port
    //   BT  -> starts BLE advertising (Nordic UART service)
    // The WebSocket SERVER (for MultiFunPlayer) is always up via buttplug.begin().
    applyTransport(g_state.getTransport());


    // Create FreeRTOS tasks
    xTaskCreatePinnedToCore(motorTask, "Motor", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(buttplugTask, "Buttplug", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(httpTask, "HTTP", 4096, NULL, 1, NULL, 0);
    // Driver-health poll/safety watchdog on core 0 (off the motor/control core).
    xTaskCreatePinnedToCore(diagTask, "Diag", 3072, NULL, 1, NULL, 0);
    // On-device waveform generator. Same core as the motor/control tasks so its
    // streamTo() calls stay coherent with the motion pipeline; priority below
    // the motor task so step generation always wins.
    xTaskCreatePinnedToCore(generatorTask, "Generator", 4096, NULL, 2, NULL, 1);
    // Buffered-interpolation task: resamples sporadic Intiface input at a fixed
    // local rate (20/50/100 Hz) with easing. Same core/priority as the generator
    // (both feed streamTo() and are mutually exclusive with it in practice).
    xTaskCreatePinnedToCore(interpolatorTask, "Interp", 4096, NULL, 2, NULL, 1);


    APPLOG("System ready - push shaft into endstop to home, or use the web UI");

}

void loop() {
    // Main loop is idle - all work done in FreeRTOS tasks
    vTaskDelay(pdMS_TO_TICKS(100));
}