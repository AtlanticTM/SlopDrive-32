#include "WebUI.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_timer.h>


#include "AppLog.h"
#include "ConfigStore.h"
#include "Generator.h"
#include "MotorDriver.h"

#include "TransportManager.h"
#include "SerialTransport.h"
#include "WebSocketTransport.h"
#include "BleTransport.h"
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
             Generator&          generator,
             TransportManager&   transportMgr,
             SerialTransport&    serialTransport,
             WebSocketTransport& wsTransport,
             BleTransport&       bleTransport)
    : _state(state)
    , _motor(motor)
    , _mapper(mapper)
    , _generator(generator)
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
    // /api/interp is still wired so the UI config card renders, but the
    // actual jitter-buffer engine got yanked out. The route returns the
    // stored params and lets the UI tweak them — just nobody's home to
    // actually do the thrusting. Dormant but polite about it. yippie! :3
    _httpServer->on("/api/interp",    HTTP_GET,  [this]() { handleApiInterp(); });
    _httpServer->on("/api/interp",    HTTP_POST, [this]() { handleApiInterp(); });
    _httpServer->on("/api/log",       HTTP_GET,  [this]() { handleApiLog(); });
    _httpServer->on("/api/mode",      HTTP_GET,  [this]() { handleApiMode(); });
    _httpServer->on("/api/mode",      HTTP_POST, [this]() { handleApiMode(); });

    _httpServer->begin();
    APPLOGF("HTTP server on port %d", HTTP_PORT);

    // Arm the dedicated 10ms telemetry sampler so the UI gets a steady, even
    // stream of position/target samples to replay — independent of how busy
    // the HTTP loop gets. The shaft's rhythm, captured faithfully. :3
    startTelemetrySampler();
}


// ============================================================================
// update() — service the HTTP server (was httpTask body)
// ============================================================================

void WebUI::update() {
    _httpServer->handleClient();
    // Telemetry sampling no longer rides on this loop — it's driven by a
    // dedicated 10ms esp_timer (startTelemetrySampler) so the sample cadence
    // is rock-solid even when handleClient() blocks on a fat page transfer. :3
}

// ---- Dedicated 10ms telemetry sampler --------------------------------------
// A periodic esp_timer fires every TELEMETRY_SAMPLE_INTERVAL_MS and snapshots
// {actual position, commanded target} into the ring at a strict, even cadence.
// The browser drains the new ones each poll and replays them 10ms apart, so the
// rhythm it sees on-screen exactly mirrors the rhythm the shaft was driven at. :3
void WebUI::telemetryTimerCb(void* arg) {
    WebUI* self = static_cast<WebUI*>(arg);
    // Read actual_position_mm atomically — written by Core 1 motionConsumerTask,
    // safe to read here on Core 0 with no mutex. memory_order_relaxed is correct
    // for display-only telemetry — no ordering dependency with other variables. :3
    self->captureTelemetry(self->_state.actual_position_mm.load(std::memory_order_relaxed),
                           self->_state.commanded_target_mm,
                           self->_state.commanded_raw_mm);

}

void WebUI::startTelemetrySampler() {
    static esp_timer_handle_t handle = nullptr;
    if (handle) return;  // already pounding away
    esp_timer_create_args_t args = {};
    args.callback        = &WebUI::telemetryTimerCb;
    args.arg             = this;
    args.dispatch_method = ESP_TIMER_TASK;   // runs in the esp_timer task (Core 0)
    args.name            = "tele10ms";
    if (esp_timer_create(&args, &handle) == ESP_OK) {
        esp_timer_start_periodic(handle, TELEMETRY_SAMPLE_INTERVAL_MS * 1000ULL);
        APPLOG("Telemetry sampler armed — sampling the shaft every 10ms :3");
    } else {
        APPLOG("Telemetry sampler FAILED to arm — graph will be limp. :<");
    }
}

// ---- Batched telemetry ring capture ---------------------------------------
// Writes one sample at slot (seq % SIZE), then bumps the monotonic seq. The
// spinlock makes the {pos, tgt, seq} update atomic against the HTTP-thread
// drain in handleApiStatus — no torn reads, no half-written sample. seq lets
// the browser ask for "everything newer than what I last saw" so we never
// resend or skip a thrust. :3
void WebUI::captureTelemetry(float position_mm, float target_mm, float raw_mm) {
    portENTER_CRITICAL(&_telemetry_mux);
    uint32_t seq = _telemetry_seq;
    size_t idx = seq % TELEMETRY_RING_SIZE;
    _telemetry_ring[idx].position_mm = position_mm;
    _telemetry_ring[idx].target_mm   = target_mm;
    _telemetry_ring[idx].raw_mm      = raw_mm;
    _telemetry_seq = seq + 1;
    portEXIT_CRITICAL(&_telemetry_mux);
}



// ============================================================================
// Route handlers
// ============================================================================

void WebUI::handleRoot() {
    // Serve the gzipped bundle first — it's the production build from the
    // Vite pipeline, smaller and faster over-the-wire. :3
    //
    // IMPORTANT: streamFile() on ESP32 Arduino auto-detects ".gz" by filename
    // extension and adds Content-Encoding: gzip itself. We must NOT also call
    // sendHeader("Content-Encoding","gzip") or the browser gets a double header
    // and chokes with ERR_CONTENT_DECODING_FAILED. Let streamFile do its thing. :3
    if (LittleFS.exists("/index.html.gz")) {
        File f = LittleFS.open("/index.html.gz", "r");
        if (f) {
            _httpServer->streamFile(f, "text/html");
            f.close();
            return;
        }
    }
    // Fallback: uncompressed index.html (dev convenience, manual upload)
    if (LittleFS.exists("/index.html")) {
        File f = LittleFS.open("/index.html", "r");
        if (f) {
            _httpServer->streamFile(f, "text/html");
            f.close();
            return;
        }
    }
    // Last resort: the built-in error page (filesystem not uploaded yet)
    _httpServer->send(200, "text/html", htmlFallbackPage);
}

void WebUI::handleApiStatus() {
    JsonDocument doc;
    // Monotonic uptime in milliseconds so the browser JS can detect an ESP32
    // reboot (uptime resets to zero) and discard any stale cached state like
    // homed=true from before the reset. No more confused post-reboot WebUI. :3
    doc["uptime_ms"] = (uint32_t)millis();
    doc["wifi_connected"] = _state.wifi_ready;
    doc["ip"] = WiFi.localIP().toString();
    doc["homed"] = _state.homed;
    doc["homing"] = _state.homing_in_progress;
    doc["buttplug_connected"] = _wsTransport.isServerConnected();
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

    // ---- Batched telemetry samples — drain only what's NEW since the browser
    //      last polled (it sends ?since=<seq>). The firmware stuffs one sample
    //      every 10ms; the browser replays them 10ms apart on its own clock so
    //      the position markers + graph glide at the exact rhythm we drove. :3
    //
    //      We send each sample as just [actual_pos_mm, commanded_target_mm] —
    //      no per-sample timestamp needed, because the spacing is a fixed 10ms
    //      and the browser schedules playback locally. We also send the current
    //      tele_seq so the browser knows what to ask for next time, plus
    //      tele_dt so it knows the playback spacing without hardcoding it. :3 ----
    uint32_t since = 0;
    if (_httpServer->hasArg("since")) since = (uint32_t)strtoul(_httpServer->arg("since").c_str(), nullptr, 10);

    // Snapshot the ring under the spinlock into a tiny local buffer so the
    // 10ms timer can't write mid-serialize. 25 samples max — cheap on stack. :3
    TelemetrySample snap[TELEMETRY_RING_SIZE];
    uint32_t        snap_first_seq = 0;
    size_t          snap_n = 0;
    portENTER_CRITICAL(&_telemetry_mux);
    uint32_t head = _telemetry_seq;   // next seq to be written = total written
    // Oldest seq still resident in the ring.
    uint32_t oldest = (head > TELEMETRY_RING_SIZE) ? (head - TELEMETRY_RING_SIZE) : 0;
    // Start at whichever is newer: what the browser hasn't seen, or the oldest
    // sample we still hold (if the browser fell behind > 250ms we skip the gap).
    uint32_t from = (since > oldest) ? since : oldest;
    for (uint32_t s = from; s < head && snap_n < TELEMETRY_RING_SIZE; s++) {
        snap[snap_n++] = _telemetry_ring[s % TELEMETRY_RING_SIZE];
    }
    snap_first_seq = from;
    portEXIT_CRITICAL(&_telemetry_mux);

    doc["tele_seq"] = head;                              // ask for this next time
    doc["tele_dt"]  = TELEMETRY_SAMPLE_INTERVAL_MS;      // playback spacing (ms)
    doc["tele_from"] = snap_first_seq;                   // seq of samples[0]
    JsonArray samples = doc["samples"].to<JsonArray>();
    for (size_t i = 0; i < snap_n; i++) {
        // [actual_pos_mm, planned_target_mm, raw_mapped_mm] — the full pipeline:
        // "took" (where the shaft got) vs "told" (planner output) vs "asked"
        // (raw TCode+RangeMapper, pre-planner). Three traces, three stages, so
        // we can see exactly where the motion path gets mangled. :3
        JsonArray s = samples.add<JsonArray>();
        s.add(snap[i].position_mm);
        s.add(snap[i].target_mm);
        s.add(snap[i].raw_mm);
    }



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
        // Continuous-blend policy: 1=let-it-land, 2=allow-reversal, 3=hybrid.
        // Replaces the old predictive lookahead/overshoot — we keep velocity
        // through the stream now instead of guessing ahead of it. :3
        doc["blend_mode"] = _motor.getBlendMode();

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

        // Clamp raised to match setAcceleration()'s ceiling of 20000 mm/s².
        // The old 5000 cap was silently rejecting every value above 5000 —
        // so "8000" in the UI was actually running at 5000 the whole time. owo
        // Floor at 10 so a zero/garbage value doesn't brick the planner. :3
        if (accel < 10)    accel = 10;
        if (accel > 20000) accel = 20000;


        // Continuous-blend policy (1=let-it-land, 2=allow-reversal, 3=hybrid).
        // Default to the driver's current mode if the field is absent so a
        // partial settings POST doesn't clobber it. Clamped 1..3. :3
        uint8_t blend_mode = doc["blend_mode"] | (uint8_t)_motor.getBlendMode();
        if (blend_mode < 1) blend_mode = 1;
        if (blend_mode > 3) blend_mode = 3;

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
        _motor.setBlendMode(blend_mode);
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
        // Don't call _motor.home() here from the handler core — that's a
        // cross-core hookup with no protection. The dom core (Core 1 motorTask)
        // owns the motor; let it see the flag and mount home() on its own terms.
        // We just issued the order — he knows what to do. :3
    }
    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiStop() {
    // Flag-only from the handler core — no _motor.stop() cross-core call.
    // Core 0 (us) sets the flags, Core 1 (motorTask) sees estop_requested
    // and calls motor.stop() cleanly from the core that owns the hardware.
    // Nobody crosses the streams, nobody gets ghosted. :3
    _state.estop_requested = true;
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
        APPLOG("Paused: hands off the puppers — Intiface input edged out :3");
    } else if (!_state.paused && was) {
        _state.resume_start_ms = millis();
        APPLOG("Unpaused: easing back in, letting Intiface take the reins again~ :3");
    }
    String json; JsonDocument r; r["ok"] = true; r["paused"] = _state.paused;
    serializeJson(r, json);
    _httpServer->send(200, "application/json", json);
}

void WebUI::handleApiHalt() {
    if (_motor.isHomed()) _motor.hardStop();
    APPLOG("Halt: motor stopped — still homed and ready for round two~ :3");
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
        // Map the raw Hertz value to the nearest valid tick rate. We keep the
        // rungs spaced out (20/50/100/200) so the UI buttons map cleanly and
        // nobody accidentally picks 137 Hz. If the value isn't close to any
        // rung, snap to the nearest one — we're a good boy, not a brat. :3
        _state.gen_rate_tick_hz = (r >= 150) ? 200 : (r >= 75) ? 100 : (r >= 35) ? 50 : 20;
    }

    // ---- Generator config — lock the whole struct so the Core-1 generator -----
    // ---- task sees a consistent snapshot (no torn writes mid-update). ----------
    portENTER_CRITICAL(&_state.gen_mux);

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

    portEXIT_CRITICAL(&_state.gen_mux);

    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiInterp() {
    if (_httpServer->method() == HTTP_GET) {
        JsonDocument doc;
        // Interpolation/extrapolation are compiled out of this build — motion
        // is trapezoidal-planner only now. We still answer this endpoint so the
        // WebUI doesn't choke, but we advertise the real state honestly. :3
        doc["mode"]      = "trapezoidal";
        doc["easing"]    = _state.buf_easing;
        doc["depth"]     = _state.buf_depth;
        doc["tick"]      = _state.buf_tick_hz;
        doc["active"]    = false;   // no interpolator task in this build
        doc["buffered"]  = 0;
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
        // Same rung logic as the generator tick — 20/50/100/200, snapped clean.
        // The interpolator's inner loop already clamps 5..200 on its side, so
        // all we do here is map the UI value to the nearest valid rung. :3
        _state.buf_tick_hz = (t >= 150) ? 200 : (t >= 75) ? 100 : (t >= 35) ? 50 : 20;
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