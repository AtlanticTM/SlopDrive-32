#include "WebUI.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
// The HTTP backend A/B seam. Pulls in either the sync Arduino WebServer (+
// IdleGuardWebServer) or the PsychicHttp adapter, depending on
// -DUSE_PSYCHIC_HTTP. Every handler below is written against the surface both
// sides present, so nothing past init()/update() knows which one is live.
#include "ui/SlopHttpServer.h"
#include <WiFi.h>
#include <esp_timer.h>

#if defined(FEATURE_RS485_MODBUS)
#include "ServoModbus.h"
#if defined(DRIVER_AIM_SERVO)
#include "EncoderValidator.h"
#endif
#endif

#include "AppLog.h"          // bridge only: applogDump/applogSerialQuiet (/api/log)
#include "sloplog/sloplog.h"
#include "ConfigStore.h"
#include "MachineConfig.h"
#include "SlopGlowBoard.h"
#include "PatternEngine.h"
#include "MotorDriver.h"

#include "TransportManager.h"
#include "TCodeParser.h"
#include "SerialTransport.h"
#include "BleTransport.h"
#include "MotionArbiter.h"
#include "config_api.h"
#include "range_mapper.h"
#include "slopsync/generated/registry_constants.hpp"  // limits::ws_subprotocol (single source of the proto id)

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
             BleTransport&       bleTransport)
    : _state(state)
    , _motor(motor)
    , _mapper(mapper)
    , _patternEngine(patternEngine)
    , _transportMgr(transportMgr)

    , _serialTransport(serialTransport)
    , _bleTransport(bleTransport)
{
    // The build-flag-selected HTTP backend (include/ui/SlopHttpServer.h):
    //   default            -> IdleGuardWebServer (sync WebServer + the
    //                         speculative-socket idle guard that kills the
    //                         measured 5 s HTTP_MAX_DATA_WAIT captures)
    //   -DUSE_PSYCHIC_HTTP -> PsychicHttp / esp_http_server adapter
    _httpServer = new SlopHttpServer(HTTP_PORT);
}

WebUI::~WebUI() {
    delete _httpServer;
}

// ============================================================================
// init() — register every route and start the server
// ============================================================================

void WebUI::init() {
    // WebServer only exposes request headers that were explicitly collected —
    // without this, header("If-None-Match") is always empty and the ETag
    // revalidation in handleRoot() silently never fires. (Under PsychicHttp
    // this is a no-op: esp_http_server can read any header on demand.)
    static const char* kCollectHeaders[] = { "If-None-Match" };
    _httpServer->collectHeaders(kCollectHeaders, 1);

    _httpServer->on("/",          [this]() { handleRoot(); });
    _httpServer->on("/api/status",    HTTP_GET,  [this]() { handleApiStatus(); });
    _httpServer->on("/api/capabilities", HTTP_GET, [this]() { handleApiCapabilities(); });
    _httpServer->on("/api/settings",  HTTP_GET,  [this]() { handleApiSettings(); });
    _httpServer->on("/api/settings", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\",\"use\":\"0x0101 config-set\"}");
    });
    // ---- HTTP CONTROL IS RETIRED (M5c) ---------------------------------
    // "No controls outside SlopSync, HTTP is read only." Every route below
    // has an exact SlopSync twin and now answers 410 with a pointer to it.
    // GET siblings survive as read-only diagnostics; OTA and /uitoken are
    // permanent sidebands (they flash and they authorize, they do not move
    // the machine).
    _httpServer->on("/api/move", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\",\"use\":\"0x0100 move\"}");
    });
    _httpServer->on("/api/home", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\",\"use\":\"0x0103 home\"}");
    });
    _httpServer->on("/api/stop", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\",\"use\":\"0x0005 safety op=stop\"}");
    });
    _httpServer->on("/api/pause", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\",\"use\":\"0x0005 safety op=pause/resume\"}");
    });
    _httpServer->on("/api/halt", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\",\"use\":\"0x0005 safety op=hold\"}");
    });
    _httpServer->on("/api/override", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\",\"use\":\"0x0005 safety op=override_on/off\"}");
    });
    _httpServer->on("/api/servo",     HTTP_GET,  [this]() { handleApiServo(); });
    // POST /api/servo is RETIRED (M5c) — THE LAST HTTP WRITER. "No controls
    // outside SlopSync, HTTP is read only."
    //
    // Retired rather than ported, deliberately: unlike the other writers this
    // one took an arbitrary register->value map, which is not a fixed INTENT
    // schema, and the operator does not currently use servo tuning ("it was
    // always broken"). Designing its protocol shape under time pressure for a
    // feature with no user is how you get a bad shape you then live with.
    // RFC-031 records the intended split for when it returns: the `live`
    // whitelist becomes annotated settings, `program` becomes an RFC-021 blob,
    // and `scan` is already 0x0106 machine-admin op 3.
    //
    // GET /api/servo survives as a read-only diagnostic, per the same rule that
    // keeps /api/log and /api/capabilities.
    _httpServer->on("/api/servo", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\","
                          "\"see\":\"RFC-031\","
                          "\"use\":\"slopsync 0x0106 machine-admin op=servo_scan (scan only)\"}");
    });
    // POST /api/clearfault is RETIRED (M5c) — it is now machine-admin op 1 on
    // SlopSync 0x0106. No controls outside SlopSync.
    _httpServer->on("/api/clearfault", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\","
                          "\"use\":\"slopsync 0x0106 machine-admin op=clear_fault\"}");
    });
    _httpServer->on("/api/pattern",   HTTP_GET,  [this]() { handleApiPattern(); });
    _httpServer->on("/api/pattern", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\",\"use\":\"0x0102 pattern-cmd\"}");
    });
    _httpServer->on("/api/pattern/presets", HTTP_GET,  [this]() { handleApiPatternPresets(); });
    // POST /api/pattern/presets is RETIRED (M5) — THE LAST HTTP WRITER. "No
    // controls outside SlopSync, HTTP is read only." save/load/delete/rename
    // are now SlopSync 0x0108 pattern-presets-cmd (RFC-021 store 0x0095 +
    // roster 0x0096, SlopSyncHubService.cpp). GET stays: the legacy NVS
    // ("advpreset") list is still a useful read-only diagnostic, and that key
    // is left in place — the new store migrates from it read-only, once, at
    // boot (SlopSyncHubService::loadPresets), never deletes it.
    _httpServer->on("/api/pattern/presets", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\",\"use\":\"slopsync 0x0108 pattern-presets-cmd\"}");
    });
    _httpServer->on("/api/log",       HTTP_GET,  [this]() { handleApiLog(); });
    _httpServer->on("/api/mode",      HTTP_GET,  [this]() { handleApiMode(); });
    _httpServer->on("/api/mode", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\",\"use\":\"retired with the transport selector (M5c)\"}");
    });
    _httpServer->on("/api/slopmotion", HTTP_GET,  [this]() { handleApiSlopMotion(); });
    // POST /api/slopmotion is RETIRED (M5c). "No controls outside SlopSync,
    // HTTP is read only" — the 20 live-tune knobs are channels 0x008B/0x008C/
    // 0x008D written through 0x0105 slopmotion-set, which ALSO persists them to
    // NVS (this endpoint never did). GET stays: a read-only view of the tuning
    // state is a diagnostic, and one that keeps working when the SlopSync plane
    // is the thing being debugged.
    _httpServer->on("/api/slopmotion", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\","
                          "\"use\":\"slopsync 0x0105 slopmotion-set\"}");
    });
    _httpServer->on("/api/machine",        HTTP_GET,  [this]() { handleApiMachine(); });
    _httpServer->on("/api/machine/commit", HTTP_POST, [this]() { slopglowActivity(); handleApiMachineCommit(); });
    _httpServer->on("/api/machine/homeoverride", HTTP_POST, [this]() {
        _httpServer->send(410, "application/json",
                          "{\"ok\":false,\"error\":\"retired\",\"use\":\"0x0103 home op=2 force_home / op=3 clear_override\"}");
    });

    _httpServer->begin();
    SLOGI("ui", "HTTP server on port %d", HTTP_PORT);

    startTelemetrySampler();
}

// ============================================================================
// update()
// ============================================================================

void WebUI::update() {
    // Sync backend: this IS the request pump.
    // Psychic backend: esp_http_server serves on its own task, so this is only
    // the deferred-start retry (Psychic refuses to start with no IP, which the
    // sync WebServer never did). Either way it must keep being called.
    _httpServer->handleClient();
#if !defined(USE_PSYCHIC_HTTP)
    // Drop speculative browser sockets that hold the single serve slot while
    // sending nothing — otherwise each one deafens HTTP for 5 s (measured).
    // Structurally unnecessary under Psychic: a silent socket simply never
    // becomes readable in select(), so it costs the server nothing.
    _httpServer->dropIdleCapture();
#endif

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
    //
    // ONE read of FAS, TWO consumers: the graph AND the shared atomic. Before
    // fw 2.1.48 this callback only fed the graph, so actual_position_mm had a
    // single writer (applyMove) and sat frozen at the last manual endpoint —
    // which meant SlopSync's 0x0080 `pos` field lied through every stream,
    // pattern and homing cycle, and main.cpp's stream rising-edge re-seeded
    // SlopMotion from a stale position. This sampler is the writer now.
    // Do NOT call getPosition() twice — one sample, both uses. :3
    const float actual_mm = self->_motor.getPosition();
    self->_state.actual_position_mm.store(actual_mm, std::memory_order_relaxed);
    self->captureTelemetry(actual_mm,
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
        SLOGI("ui", "Telemetry sampler armed — sampling the shaft at 240Hz :3");
    } else {
        SLOGE("ui", "Telemetry sampler FAILED to arm — graph will be limp. :<");
    }
}

void WebUI::resetSessionStats() {
    _state.live_speed_mm_s.store(0.0f, std::memory_order_relaxed);
    _state.max_speed_mm_s.store(0.0f, std::memory_order_relaxed);
    _state.session_distance_mm.store(0.0f, std::memory_order_relaxed);
    _state.stroke_count.store(0, std::memory_order_relaxed);
    _state.session_start_ms = millis();
    _motor.resetPowerStats();   // zero the INA228 Wh accumulator + software peaks
    SLOGI("ui", "Session stats reset :3");
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
    // Streaming the 115 KB bundle out of LittleFS blocks httpTask ~0.5-1 s
    // per load (sync WebServer — the known roadmap-§4 class; [STALL]
    // http:ui.update names it). Until that rework: ETag + Cache-Control
    // no-cache. Every load still REVALIDATES (ground truth — an uploadfs is
    // picked up immediately because size/mtime change the tag), but an
    // unchanged bundle answers 304 in ~10 ms instead of restreaming. :3
    for (const char* path : { "/index.html.gz", "/index.html" }) {
        if (!LittleFS.exists(path)) continue;
        File f = LittleFS.open(path, "r");
        if (!f) continue;
        String etag = "\"" + String((unsigned long)f.size()) + "-" +
                      String((unsigned long)f.getLastWrite()) + "\"";
        if (_httpServer->header("If-None-Match") == etag) {
            f.close();
            _httpServer->sendHeader("ETag", etag);
            _httpServer->sendHeader("Cache-Control", "no-cache");
            _httpServer->send(304, "text/html", "");
            return;
        }
        _httpServer->sendHeader("ETag", etag);
        _httpServer->sendHeader("Cache-Control", "no-cache");
        _httpServer->streamFile(f, "text/html");
        f.close();
        return;
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
    // M5c: the Intiface/TCode :55555 WebSocket is DELETED — SlopSync is the
    // only input and output now, and Intiface is planned to speak SlopSync
    // natively rather than us speaking its protocol. Reported as a constant
    // false rather than dropped from the payload, so an older cached page reads
    // "not connected" instead of "undefined". The key goes when the HTTP
    // control surface does.
    doc["buttplug_connected"] = false;
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
    {
        // Item 4 (fw 2.1.76): a pre-home carryover (ConfigStore::load()
        // restores a PRIOR boot's measurement into the motor regardless of
        // _state.homed) must never overstate the configured ceiling. The
        // override branch is exempt — WS_OP_HOME_OVERRIDE always sets
        // _state.homed=true alongside it, so it is never "unhomed".
        float ms = (_state.test_stroke_override_mm > 0.0f)
                   ? _state.test_stroke_override_mm
                   : _motor.getMeasuredStrokeMm();
        if (!_state.homed && ms > _state.config.max_rail_mm) ms = _state.config.max_rail_mm;
        doc["measured_stroke_mm"] = ms;
    }
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

    // Driver-health block: NO live fault readback exists on the AIM drive — it
    // exposes no fault status over the step/dir interface. Report that honestly
    // instead of a hardcoded all-clear (otpw/ot/s2g/faulted
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
    {
        // Item 4 (fw 2.1.76): same pre-home clamp as handleApiStatus() above —
        // see that comment for why. Kept in step deliberately; these are the
        // HTTP twin of the SlopSync 0x0081 `measured_stroke` field.
        float ms = _motor.getMeasuredStrokeMm();
        if (!_state.homed && ms > _state.config.max_rail_mm) ms = _state.config.max_rail_mm;
        doc["measured_stroke_mm"] = ms;
    }

    JsonObject speed = doc["speed_ceiling_mm_s"].to<JsonObject>();
    speed["normal"] = (uint32_t)NORMAL_MAX_SPEED_MM_S;
    speed["expert"] = (uint32_t)EXPERT_MAX_SPEED_MM_S;

    JsonObject accel = doc["accel_ceiling_mm_s2"].to<JsonObject>();
    accel["normal"] = (uint32_t)NORMAL_MAX_ACCEL_MM_S2;
    accel["expert"] = (uint32_t)EXPERT_MAX_ACCEL_MM_S2;

    // Jerk joined the limit family in fw 2.1.47 — advertised the same way so the
    // UI derives its slider max from the API instead of hardcoding a literal. :3
    JsonObject jerk = doc["jerk_ceiling_mm_s3"].to<JsonObject>();
    jerk["normal"] = (uint32_t)NORMAL_MAX_JERK_MM_S3;
    jerk["expert"] = (uint32_t)EXPERT_MAX_JERK_MM_S3;

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
    // Single source of truth: the registry constant that also names the WS
    // subprotocol + the mDNS TXT `proto` record (was a stale "slopsync/1").
    doc["slopsync_proto"] = slopsync::limits::ws_subprotocol.data();

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
    SLOGI("ui", "Clear-fault requested — no driver fault readback on this build (nothing to clear/verify)");
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
        doc["input_max_jerk"]  = (uint32_t)_state.config.input_max_jerk_mm_s3;
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
    // INPUT set — speed/accel go to the arbiter AND (via config) to SlopMotion's
    // derived ceilings; jerk is planner-only (the arbiter has no jerk concept),
    // so it just lands in config and main.cpp's per-tick push picks it up within
    // ~1 ms. Clamped to the same HARD firmware ceiling its siblings use — the
    // NORMAL/EXPERT split is a UI guardrail advertised via /api/capabilities,
    // not something the firmware enforces here. :3
    if (doc["input_max_speed"].is<uint32_t>() || doc["input_max_accel"].is<uint32_t>() ||
        doc["input_max_jerk"].is<uint32_t>()) {
        float is = doc["input_max_speed"] | _state.config.input_max_speed_mm_s;
        float ia = doc["input_max_accel"] | _state.config.input_max_accel_mm_s2;
        float ij = doc["input_max_jerk"]  | _state.config.input_max_jerk_mm_s3;
        if (is < 1.0f) is = 1.0f; if (is > MAX_SPEED_MM_S) is = MAX_SPEED_MM_S;
        if (ia < 10.0f) ia = 10.0f; if (ia > MAX_ACCEL_MM_S2) ia = MAX_ACCEL_MM_S2;
        if (ij < 1000.0f) ij = 1000.0f; if (ij > MAX_JERK_MM_S3) ij = MAX_JERK_MM_S3;
        _state.config.input_max_speed_mm_s = is;
        _state.config.input_max_accel_mm_s2 = ia;
        _state.config.input_max_jerk_mm_s3 = ij;
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
    // Mirror the mapper's post-clamp range back into state.config. The mapper
    // is the ONE live source of truth for the window (every real motion
    // consumer — MotionArbiter, PatternEngine, main.cpp's telemetry — reads
    // _mapper directly, and it always did apply a live window edit correctly).
    // state.config.min/max_position_mm is a SEPARATE copy that only
    // ConfigStore::load() used to keep in sync (see its own "silent
    // divergence from the mapper's real range" comment) — this call path
    // never did, which starved two things that read state.config instead of
    // the mapper: SlopSyncHubService's 0x0081 machine-config STATE broadcast
    // (so the UI's ground-truth rail band always redisplayed the stale
    // boot-time window after a live edit — CLAUDE.md 3's Ground Truth
    // Doctrine was doing exactly its job, faithfully reporting a firmware
    // value that was itself wrong) and pumpConfigGeneration()'s change
    // detector (so the SlopSync protocol cfg_gen never advanced for a window
    // edit either). The physical machine was never the bug; its own STATE
    // channel lying about itself was. :3
    _state.config.min_position_mm = _mapper.getMinMm();
    _state.config.max_position_mm = _mapper.getMaxMm();

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
    resp["input_max_jerk"]  = (uint32_t)_state.config.input_max_jerk_mm_s3;
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
        SLOGW("ui", "applyMove REFUSED: MotionArbiter not wired — no motion dispatched");
        resp["ok"] = false;
        resp["error"] = "Motion arbiter unavailable";
        return false;
    }

    // Immediate seed of the "where the shaft is" atomic that the stream
    // rising-edge reads (main.cpp). The 240 Hz telemetry sampler maintains this
    // atomic continuously from _motor.getPosition(), so this store is only a
    // head start: it publishes the manual ENDPOINT the instant the intent is
    // submitted, before the shaft has actually travelled there. A stream started
    // in the same breath as a manual move therefore plans toward the endpoint
    // rather than the mid-flight sample. The sampler overwrites it within one
    // 4.2 ms tick either way. The live posdot/readout reads _motor.getPosition()
    // directly. :3
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
        SLOGI("ui", "Paused: hands off the puppers — Intiface input edged out :3");
    } else if (!_state.paused && was) {
        _state.resume_start_ms = millis();
        SLOGI("ui", "Unpaused: easing back in, letting Intiface take the reins again~ :3");
    }
    _bumpGen();
    String json; JsonDocument r; r["ok"] = true; r["paused"] = _state.paused;
    serializeJson(r, json);
    _httpServer->send(200, "application/json", json);
}

void WebUI::handleApiHalt() {
    if (_motor.isHomed() && _arbiter) _arbiter->hardStopMotion();
    SLOGI("ui", "Halt: motor stopped — still homed and ready for round two~ :3");
    _bumpGen();
    _httpServer->send(200, "application/json", "{\"ok\":true}");
}

void WebUI::handleApiOverride() {
    JsonDocument doc;
    deserializeJson(doc, _httpServer->arg("plain"));
    bool was = _state.manual_override;
    _state.manual_override = doc["override"] | (!_state.manual_override);
    if (_state.manual_override && !was) {
        SLOGI("ui", "Manual override ON: you're topping now — Intiface can watch but can't touch :3");
    } else if (!_state.manual_override && was) {
        _state.resume_start_ms = millis();
        SLOGI("ui", "Manual override OFF: handing the leash back to Intiface~ :3");
    }
    _bumpGen();
    String json; JsonDocument r; r["ok"] = true; r["manual_override"] = _state.manual_override;
    serializeJson(r, json);
    _httpServer->send(200, "application/json", json);
}

// ============================================================================
// ============================================================================
//

// ============================================================================
// applyDriverConfig — shared driver-config mutation (used by the WS op)
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
        SLOGI("ui", "ServoBench: sp_period_ms -> %u", (unsigned)_servoModbus->spPeriodMs());
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
                    SLOGW("ui", "ServoProgram: structural change applied — machine UNHOMED, re-home before motion");
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

    SLOGD("ui", "/api/pattern POST: %s", _httpServer->arg("plain").c_str());

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

    SLOGI("ui", "AdvPreset %s: \"%s\" (%u presets, %u bytes)",
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
    // Touched-control tracking (SlopSync 0x0107 needs this): the singular
    // ap_mod echo below only ever reported the LAST touched control, which is
    // silently wrong for a request that lands modifier edits on more than one
    // base control at once (a preset apply, or a client batching several
    // sub-fields from several controls into one wire frame — legal under
    // kIntentMaxValueFields=8, 6 sub-keys per control). `touched` records
    // EVERY control this call actually wrote, in ctrl-id order, so the caller
    // can build a ground-truth echo for all of them, not just the last.
    bool touched[advpat::BASE_COUNT] = {};
    int ap_mod_ctrl = -1;
    if (doc["ap_mod"].is<JsonObject>()) {
        ap_mod_ctrl = applyModObject(doc["ap_mod"].as<JsonObject>());
        if (ap_mod_ctrl >= 0 && ap_mod_ctrl < advpat::BASE_COUNT) touched[ap_mod_ctrl] = true;
    }
    if (doc["ap_mods"].is<JsonArray>()) {
        for (JsonObject m : doc["ap_mods"].as<JsonArray>()) {
            int ctrl = applyModObject(m);
            if (ctrl >= 0 && ctrl < advpat::BASE_COUNT) touched[ctrl] = true;
        }
    }

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
            SLOGI("ui", "PatternEngine started");
        } else if (!want && _patternEngine.isRunning()) {
            _patternEngine.stop();
            SLOGI("ui", "PatternEngine stopped");
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
        // Kept for whatever still reads the singular shape; superseded (not
        // replaced) by the ap_mods array below for the general case.
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
    // Ground Truth for EVERY control this call actually touched, not just the
    // last one — see the `touched` comment above. SlopSync's 0x0107 delegate
    // case reads this array to build its per-key ECHO; nothing else currently
    // reads it, so adding it costs existing callers nothing.
    {
        JsonArray mods = resp["ap_mods"].to<JsonArray>();
        for (uint8_t id = 0; id < advpat::BASE_COUNT; id++) {
            if (!touched[id]) continue;
            const advpat::BaseControl* c = _patternEngine.apSettings().byId(id);
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

    _bumpGen();
    return true;
}

void WebUI::handleApiLog() {
    String out;
    out.reserve(2048);
    applogDump(out);
    _httpServer->send(200, "text/plain", out);
    // The UI has provably received the log stream — serial's job is done.
    // From here serial carries Warn+ only; the web ring is primary.
    applogSerialQuiet();
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
        SLOGI("ui", "backend commit: best-effort baud restore to 19200 %s",
              ok ? "OK" : "FAILED (harmless — next boot's probe finds whatever it finds)");
    }
#endif

    machineBackendStore((uint8_t)backend);
    SLOGI("ui", "backend commit: %u -> %d — rebooting to apply", _machine_backend, backend);
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
        SLOGI("ui", "HTTP Home-Override: faking homed for bench test :3");
        resp["measured_stroke"] = stroke;
    } else {
        _state.test_stroke_override_mm = 0.0f;
        _state.homed = false;
        _motor.forceHomeState(false);
        SLOGI("ui", "HTTP Home-Override: cleared — back to real homing.");
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
// handleApiSlopMotion (HTTP GET + POST) — SlopMotion live-tuning ROUGH-IN
// ============================================================================
// Curl-driven bench tuning until the WebUI refactor grows a proper card.
// POST writes the sm_tune_* fields in SystemState (Core 0 single-writer);
// the Core-1 sampler pushes them into the engine every tick, so a change is
// live within ~1 ms. Values are clamped HERE and the response echoes the
// APPLIED values (ground-truth doctrine — never echo the raw request).
// Nothing persists: a reboot restores compile-time defaults, which is the
// desired behavior for a tuning session (no way to brick the feel in NVS).
void WebUI::handleApiSlopMotion() {
    // Canonical wire names for slopmotion::InfeasiblePolicy, indexed by the
    // stored ordinal. ONE table for the POST log line and the GET echo so the
    // two can never disagree about what is in force. Names match the sim's
    // /api/slopmotion echo exactly, so an operator can diff the two responses.
    //
    // EVERY index into this table is guarded with a real `<` bounds check, and
    // NEVER with `%` or `&`. A modulo does not bound an out-of-range ordinal,
    // it ALIASES it onto a valid name — which is how `plan % 3` once reported
    // PlanKind::Cubic (=3) as "no active plan" mid-stroke. An out-of-range
    // value must LOOK wrong, so it falls back to index 0 and the operator sees
    // a policy that plainly is not the one they set.
    static const char* kInfeasPolicyNames[] = {
        "stretch", "scale", "reshape", "prio-amplitude", "prio-smooth"
    };
    static constexpr uint8_t kInfeasPolicyCount =
        uint8_t(sizeof(kInfeasPolicyNames) / sizeof(kInfeasPolicyNames[0]));
    // Canonical wire names for slopmotion::CurvePolicy — same table-and-bound
    // discipline, same sim parity. "follow" is FollowClient: honour the
    // sender's declared family, which with no wire signalling yet resolves to
    // C2, i.e. pre-0.8.0 behaviour byte for byte.
    static const char* kCurvePolicyNames[] = { "follow", "c1", "c2" };
    static constexpr uint8_t kCurvePolicyCount =
        uint8_t(sizeof(kCurvePolicyNames) / sizeof(kCurvePolicyNames[0]));
    if (_httpServer->method() == HTTP_POST) {
        JsonDocument doc;
        if (deserializeJson(doc, _httpServer->arg("plain"))) {
            _httpServer->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }
        auto clampf = [](float v, float lo, float hi) {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        // jmax is a mm-domain PERSISTED limit as of fw 2.1.47 (settings key
        // input_max_jerk) — what lives here is only the normalized bench
        // OVERRIDE, exactly like vmax_ovr/amax_ovr. Legacy key "jmax" is still
        // accepted as an alias so bench scripts written against the rough-in
        // keep working; both mean "0 = derive from the mm limit / window span".
        if (doc["jmax_ovr"].is<float>())          // 0 = derive from mm limits
            _state.sm_tune_jmax_ovr = clampf(doc["jmax_ovr"], 0.0f, 2000000.0f);
        else if (doc["jmax"].is<float>())         // deprecated alias
            _state.sm_tune_jmax_ovr = clampf(doc["jmax"], 0.0f, 2000000.0f);
        if (doc["vmax_ovr"].is<float>())          // 0 = derive from mm limits
            _state.sm_tune_vmax_ovr = clampf(doc["vmax_ovr"], 0.0f, 20.0f);
        if (doc["amax_ovr"].is<float>())          // 0 = derive from mm limits
            _state.sm_tune_amax_ovr = clampf(doc["amax_ovr"], 0.0f, 500.0f);
        if (doc["chase_ff"].is<bool>())
            _state.sm_tune_chase_ff = doc["chase_ff"].as<bool>();
        if (doc["chase_accel_ff"].is<bool>())
            _state.sm_tune_chase_aff = doc["chase_accel_ff"].as<bool>();
        if (doc["chase_gain"].is<float>())
            _state.sm_tune_chase_gain = clampf(doc["chase_gain"], 0.0f, 1.5f);
        if (doc["chase_lookahead"].is<float>())
            _state.sm_tune_chase_look = clampf(doc["chase_lookahead"], 0.0f, 8.0f);
        if (doc["chase_dense_ms"].is<float>())
            _state.sm_tune_dense_us =
                (uint32_t)(clampf(doc["chase_dense_ms"], 10.0f, 500.0f) * 1000.0f);
        // Infeasible-segment policy — what gives when a commanded stroke cannot
        // physically happen in its commanded duration:
        //   "stretch" (0) range-first:   keep the stroke, overrun the deadline
        //   "scale"   (1) timing-first + shape-first: keep the deadline, shrink
        //                 the stroke until the quintic is legal
        //   "reshape" (2) timing-first + machine-first (ENGINE DEFAULT): keep
        //                 the deadline and as much range as physics allows,
        //                 giving up the SPLINE SHAPE instead of the range
        //   "prio-amplitude" (3) budgeted: spend SMOOTHNESS first (flatten the
        //                 span's end handle toward its chord, up to
        //                 smooth_budget), then start spending amplitude
        //   "prio-smooth"    (4) budgeted: spend AMPLITUDE first (up to
        //                 amplitude_budget), then start spending smoothness
        // The two budgeted policies stay inside the QUINTIC family the whole
        // way down, so the sender's curve degrades CONTINUOUSLY instead of
        // snapping to a Ruckig chord the moment the legality scan fails — which
        // is the "static interpolation" artifact reshape produces on hardware.
        // Both are identical to no policy at all until a segment is infeasible.
        // Accepts either canonical string (case-insensitive) or the ordinal;
        // anything else leaves the applied value alone, and the echo below
        // tells the truth about what is in force (ground-truth doctrine).
        if (doc["infeasible_policy"].is<const char*>()) {
            const char* p = doc["infeasible_policy"];
            if      (strcasecmp(p, "stretch") == 0) _state.sm_tune_infeas_policy = 0;
            else if (strcasecmp(p, "scale")   == 0) _state.sm_tune_infeas_policy = 1;
            else if (strcasecmp(p, "reshape") == 0) _state.sm_tune_infeas_policy = 2;
            else if (strcasecmp(p, "prio-amplitude") == 0) _state.sm_tune_infeas_policy = 3;
            else if (strcasecmp(p, "prio-smooth")    == 0) _state.sm_tune_infeas_policy = 4;
        } else if (doc["infeasible_policy"].is<int>()) {
            const int p = doc["infeasible_policy"].as<int>();
            if (p >= 0 && p < (int)kInfeasPolicyCount)
                _state.sm_tune_infeas_policy = (uint8_t)p;
        }
        // Curve family for waveform-segment reconstruction. A funscript
        // rendered through Pchip/Makima IS a C1 cubic Hermite spline, and
        // {target, duration, end_vel} on 0x0085 is a COMPLETE encoding of one —
        // so "c1" reproduces the sender's own span exactly, where the quintic
        // necessarily rounds off the acceleration step the script has at each
        // knot. "follow" (engine default) honours a declared family; no wire
        // signalling exists yet, so today it resolves to C2 — pre-0.8.0
        // behaviour byte for byte. Strings only, mirroring the sim's
        // /api/slopmotion vocabulary so the two responses diff directly.
        if (doc["curve_policy"].is<const char*>()) {
            const char* p = doc["curve_policy"];
            if      (strcasecmp(p, "follow") == 0) _state.sm_tune_curve_policy = 0;
            else if (strcasecmp(p, "c1")     == 0) _state.sm_tune_curve_policy = 1;
            else if (strcasecmp(p, "c2")     == 0) _state.sm_tune_curve_policy = 2;
        }
        // Budgeted-policy spend limits. Fractions in [0, 1], clamped here so
        // the GET echo is what Core 1 will actually push (the engine clamps
        // them again — a config push is not a trusted input on either side).
        if (doc["smooth_budget"].is<float>())
            _state.sm_tune_smooth_budget = clampf(doc["smooth_budget"], 0.0f, 1.0f);
        if (doc["amplitude_budget"].is<float>())
            _state.sm_tune_amp_budget = clampf(doc["amplitude_budget"], 0.0f, 1.0f);
        // Alpha-search bisection depth. One quintic build + one legality scan
        // per step (no Ruckig call), clamped to the engine's own [1, 10].
        if (doc["blend_steps"].is<int>())
            _state.sm_tune_blend_steps =
                (uint8_t)(int)clampf((float)doc["blend_steps"].as<int>(), 1.0f, 10.0f);
        if (doc["infeasible_scale_margin"].is<float>())
            _state.sm_tune_infeas_margin =
                clampf(doc["infeasible_scale_margin"], 0.50f, 1.00f);
        // RESHAPE bisection depth — a plan-time BUDGET dial (one Ruckig
        // calculate() per step), clamped to the engine's own [0, 8].
        if (doc["reshape_steps"].is<int>())
            _state.sm_tune_reshape_steps =
                (uint8_t)(int)clampf((float)doc["reshape_steps"].as<int>(), 0.0f, 8.0f);
        // Settle grace. The ENGINE field is MICROSECONDS; this API talks
        // MILLISECONDS because that is the unit an operator thinks in (same
        // convention as chase_dense_ms above) — convert at the boundary, here,
        // and nowhere else. Clamped [0, 200] ms: 0 = pre-0.4 brake-on-expiry,
        // 200 ms is already far past any sane stream interval.
        if (doc["settle_grace_ms"].is<float>())
            _state.sm_tune_settle_grace_us =
                (uint32_t)(clampf(doc["settle_grace_ms"], 0.0f, 200.0f) * 1000.0f);
        if (doc["chase_aim_accel_extrap"].is<bool>())
            _state.sm_tune_aim_extrap = doc["chase_aim_accel_extrap"].as<bool>();
        // DC centring of a degraded band: keep the achieved stroke symmetric
        // about the COMMANDED midpoint when the machine cannot deliver the full
        // amplitude on the clock. ON is the engine default; OFF restores the
        // slopmotion 0.4.0 contract. The gain is a feel dial (0..1, and NOT
        // monotone — see SystemState) clamped to the engine's own range here so
        // the GET echo below is the value Core 1 will actually push.
        if (doc["wave_centering"].is<bool>())
            _state.sm_tune_centring = doc["wave_centering"].as<bool>();
        if (doc["wave_centering_gain"].is<float>())
            _state.sm_tune_centring_gain =
                clampf(doc["wave_centering_gain"], 0.0f, 1.0f);
        // RFC-008 handoff sanity guard — the Fritsch-Carlson chord factor k
        // used to bound an inbound segment's end velocity against the FOLLOWING
        // segment's chord. 1.5 is the shape-preserving bound (engine default);
        // 0 turns the guard OFF, which is the machine half of the M5d A/B
        // against the MFP plugin's own limiter. Clamped to the engine's [0, 8]
        // here too, so the GET echo is what Core 1 will actually push.
        if (doc["handoff_k"].is<float>())
            _state.sm_tune_handoff_k = clampf(doc["handoff_k"], 0.0f, 8.0f);
        if (doc["reset_stats"].as<bool>()) {
            _state.sm_plan_us_max  = 0;
            _state.sm_plan_us_avg  = 0.0f;
            _state.sm_anomalies    = 0;
            // The per-kind breakdown resets WITH the total — a bench session
            // that zeroes one and not the other would publish a histogram that
            // sums to more than its own total.
            for (uint8_t k = 0; k < SystemState::SM_ANOM_KINDS; ++k)
                _state.sm_anom_kind[k] = 0;
            // RFC-019: the reset must be OBSERVABLE to every subscriber of
            // 0x0088 slopmotion-diag, not just to whoever POSTed. Bump last,
            // after the counters are actually zero, so a snapshot that carries
            // the new generation can never still carry the old totals.
            _state.sm_reset_gen = (uint16_t)(_state.sm_reset_gen + 1);
        }
        SLOGI("ui", "slopmotion tuning: jmax_ovr=%.0f gain=%.2f look=%.2f ff=%d aff=%d "
                    "policy=%s margin=%.2f steps=%u grace=%.0fms aimx=%d "
                    "centring=%d@%.2f handoff_k=%.2f curve=%s "
                    "smooth_bud=%.2f amp_bud=%.2f blend=%u",
              (double)_state.sm_tune_jmax_ovr, (double)_state.sm_tune_chase_gain,
              (double)_state.sm_tune_chase_look,
              (int)_state.sm_tune_chase_ff, (int)_state.sm_tune_chase_aff,
              kInfeasPolicyNames[_state.sm_tune_infeas_policy < kInfeasPolicyCount
                                     ? _state.sm_tune_infeas_policy : 0],
              (double)_state.sm_tune_infeas_margin,
              (unsigned)_state.sm_tune_reshape_steps,
              (double)_state.sm_tune_settle_grace_us / 1000.0,
              (int)_state.sm_tune_aim_extrap,
              (int)_state.sm_tune_centring,
              (double)_state.sm_tune_centring_gain,
              (double)_state.sm_tune_handoff_k,
              kCurvePolicyNames[_state.sm_tune_curve_policy < kCurvePolicyCount
                                    ? _state.sm_tune_curve_policy : 0],
              (double)_state.sm_tune_smooth_budget,
              (double)_state.sm_tune_amp_budget,
              (unsigned)_state.sm_tune_blend_steps);
    }

    // GET and POST both answer with the full applied state.
    static const char* kModeNames[] = { "idle", "waveform", "chase", "settle" };
    static const char* kKindNames[] = { "none", "quintic", "ruckig", "cubic" };
    JsonDocument resp;
    JsonObject tuning = resp["tuning"].to<JsonObject>();
    tuning["jmax_ovr"]        = _state.sm_tune_jmax_ovr;
    tuning["vmax_ovr"]        = _state.sm_tune_vmax_ovr;
    tuning["amax_ovr"]        = _state.sm_tune_amax_ovr;
    tuning["chase_ff"]        = (bool)_state.sm_tune_chase_ff;
    tuning["chase_accel_ff"]  = (bool)_state.sm_tune_chase_aff;
    tuning["chase_gain"]      = _state.sm_tune_chase_gain;
    tuning["chase_lookahead"] = _state.sm_tune_chase_look;
    tuning["chase_dense_ms"]  = _state.sm_tune_dense_us / 1000.0f;
    // Canonical string echo — the wire name, never the raw enum ordinal.
    // Bounded with `<`, never `%`: aliasing an out-of-range ordinal onto a
    // valid name would report a policy the machine is not running.
    tuning["infeasible_policy"]       = kInfeasPolicyNames[
        _state.sm_tune_infeas_policy < kInfeasPolicyCount
            ? _state.sm_tune_infeas_policy : 0];
    tuning["infeasible_scale_margin"] = _state.sm_tune_infeas_margin;
    tuning["reshape_steps"]           = _state.sm_tune_reshape_steps;
    // Curve family, same string vocabulary as the sim's echo ("follow"/"c1"/"c2").
    tuning["curve_policy"]            = kCurvePolicyNames[
        _state.sm_tune_curve_policy < kCurvePolicyCount
            ? _state.sm_tune_curve_policy : 0];
    // Budgeted-policy spend limits + the alpha-search depth. APPLIED values,
    // post-clamp — inert unless infeasible_policy is one of the two budgeted
    // ones, but always echoed so the operator can set them up before switching.
    tuning["smooth_budget"]           = _state.sm_tune_smooth_budget;
    tuning["amplitude_budget"]        = _state.sm_tune_amp_budget;
    tuning["blend_steps"]             = _state.sm_tune_blend_steps;
    // MILLISECONDS on the wire, microseconds in the engine (see the POST side).
    tuning["settle_grace_ms"]         = _state.sm_tune_settle_grace_us / 1000.0f;
    tuning["chase_aim_accel_extrap"]  = (bool)_state.sm_tune_aim_extrap;
    // Centring: the APPLIED pair (post-clamp), i.e. exactly what the per-tick
    // Core-1 push writes into slopmotion::Config.
    tuning["wave_centering"]          = (bool)_state.sm_tune_centring;
    tuning["wave_centering_gain"]     = _state.sm_tune_centring_gain;
    // RFC-008 handoff guard strength (0 = off). APPLIED value, post-clamp.
    tuning["handoff_k"]               = _state.sm_tune_handoff_k;
    // "effective" is what Core 1 ACTUALLY pushed into the engine last tick —
    // post-derivation, post-override, post-clamp. All three are read back from
    // the sm_eff_* back-channel, never recomputed here, so this block cannot
    // lie about the machine's real ceilings (ground-truth doctrine). :3
    JsonObject eff = resp["effective"].to<JsonObject>();
    eff["vmax"] = _state.sm_eff_vmax;   // normalized units/s (window span = 1)
    eff["amax"] = _state.sm_eff_amax;
    eff["jmax"] = _state.sm_eff_jmax;
    // The mm-domain source of the derived jerk ceiling, so a bench session can
    // see WHY eff.jmax is what it is without a second /api/settings round-trip.
    eff["jmax_mm_s3"] = _state.config.input_max_jerk_mm_s3;
    JsonObject stats = resp["stats"].to<JsonObject>();
    stats["active"]       = (bool)_state.interp_active;
    stats["mode"]         = kModeNames[_state.sm_mode < 4 ? _state.sm_mode : 0];
    stats["plan_kind"]    = kKindNames[_state.sm_plan_kind < 4 ? _state.sm_plan_kind : 0];
    stats["plans"]        = _state.sm_plans;
    stats["failures"]     = _state.sm_failures;
    stats["anomalies"]    = _state.sm_anomalies;
    // Per-kind breakdown. The scalar total above is unchanged (back-compat);
    // this is what makes the anomaly feed actually diagnosable from outside —
    // "anomalies: 42" could be 42 benign settles or 42 scaled strokes, and
    // an investigation could not tell the two apart from the API alone.
    // Keys come from kSmAnomalyNames (SystemState.h), the SAME table the
    // Core-1 drain log formats with, so a key here always matches the log text
    // an operator saw. "none" (index 0) is emitted too: a nonzero count there
    // means the engine recorded a kind-0 event, which is itself a defect worth
    // seeing rather than hiding.
    JsonObject byKind = stats["anomalies_by_kind"].to<JsonObject>();
    for (uint8_t k = 0; k < kSmAnomalyNameCount; ++k)
        byKind[kSmAnomalyNames[k]] = _state.sm_anom_kind[k];
    stats["plan_us_last"] = _state.sm_plan_us_last;
    stats["plan_us_max"]  = _state.sm_plan_us_max;
    stats["plan_us_avg"]  = _state.sm_plan_us_avg;
    // RFC-019 reset generation — the same value 0x0088 publishes, so the HTTP
    // and SlopSync views of "have these counters been reset?" cannot disagree.
    stats["reset_gen"]    = _state.sm_reset_gen;
    JsonObject sync = resp["sync"].to<JsonObject>();
    sync["bundles"]     = _state.sm_sync_bundles;
    sync["seg_bundles"] = _state.sm_sync_seg_bundles;  // subset on 0x0085 motion-segment (waveform)
    sync["samples"]     = _state.sm_sync_samples;
    sync["enqueued"]    = _state.sm_sync_enqueued;
    sync["dropped"]     = _state.sm_sync_dropped;
    resp["persist"] = false;   // reboot = defaults, by design (rough-in)

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
// Called for each command frame by whichever plane received it. The caller
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
        // verify. Re-apply the current driver config (on the AIM step/dir drive
        // this is effectively a no-op refresh, not a register rewrite) but say
        // honestly that no fault was cleared. :3
        {
            JsonDocument dummy;
            dummy["reset"] = false;  // don't reset, just re-apply current config
            applyDriverConfig(dummy, payload_out);
        }
        payload_out["cleared"] = false;
        payload_out["reason"] = "no_fault_readback";
        SLOGI("ui", "WS clear-fault: driver config re-applied — no fault readback exists to clear/verify");
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
            SLOGI("ui", "WS Home-Override: faking homed for bench test — no motor required :3");
            payload_out["measured_stroke"] = stroke;
        } else {
            _state.test_stroke_override_mm = 0.0f;
            _state.homed = false;
            _motor.forceHomeState(false);   // clear the driver flag too
            SLOGI("ui", "WS Home-Override: cleared — back to real homing.");
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
        SLOGI("ui", "WS Halt: motor stopped — still homed and ready~ :3");
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
        SLOGW("ui", "WS bypass-limits: %s", val ? "ON (window clamp bypassed, physical ceiling still enforced)" : "OFF");
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