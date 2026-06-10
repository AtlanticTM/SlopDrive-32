#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <LittleFS.h>


#include "config.h"
#include "motor.h"
#include "buttplug.h"
#include "range_mapper.h"
#include "applog.h"

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

// ============================================================================
// Device Configuration (runtime state)
// ============================================================================

static DeviceConfig g_config;

// Live TMC driver settings (tunable from the Motor tab). Held in RAM so live
// "Apply" changes persist across page reloads even before they're saved.
static DriverConfig g_driver;

// Loading states

static bool g_homed = false;
static bool g_homing_in_progress = false;
static bool g_wifi_ready = false;

// Auto-duration: measure the real inter-command interval from the app and use
// it as the move duration, overriding the (often bogus/fixed) "I" value many
// apps hardcode (e.g. always "I100"). When commands truly arrive every ~30ms
// but each claims 100ms, the motor keeps planning slow moves it never finishes
// -> sluggish. Sizing each move to the measured cadence fixes that.
static bool     g_auto_duration = true;     // toggled from the web UI
static uint32_t g_last_cmd_ms = 0;          // arrival time of previous command
static float    g_measured_interval_ms = 0; // smoothed measured cadence (for auto-duration)

// Stable measured rate for the WEB UI, computed as a windowed frame count
// (frames per elapsed second) rather than instantaneous interval math, which
// is far less jittery. Updated once per second from the raw rx frame counter.
static volatile uint16_t g_measured_hz = 0;



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

        g_wifi_ready = true;
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
    prefs.putUShort("max_speed", (uint16_t)g_config.max_speed_mm_s);
    prefs.putUShort("accel", (uint16_t)g_config.acceleration_mm_s2);
    prefs.putUShort("lookahead", motor.getLookaheadMs());
    prefs.putUShort("overshoot", (uint16_t)motor.getMaxOvershootMm());
    prefs.putBool("auto_dur", g_auto_duration);

    // TMC driver tunables (from the Motor tab)
    prefs.putUShort("tmc_run", g_driver.run_current_ma);
    prefs.putUChar("tmc_hold", g_driver.hold_current_pct);
    prefs.putUChar("tmc_sc", g_driver.stealthchop);
    prefs.putUInt("tmc_tpwm", g_driver.tpwm_thrs);
    prefs.putUChar("tmc_toff", g_driver.toff);
    prefs.putUChar("tmc_tbl", g_driver.tbl);
    prefs.putChar("tmc_hs", g_driver.hstart);
    prefs.putChar("tmc_he", g_driver.hend);

    prefs.end();
    APPLOG("Config saved to NVS");
}

static void loadConfig() {
    // Always start with defaults
    g_config = getDefaultConfig();
    g_driver = DriverConfig();   // default-constructed = config.h defaults
    mapper.setRange(g_config.min_position_mm, g_config.max_position_mm);

    // Try to load saved values (read-only mode)
    if (prefs.begin("strokeengine", true)) {
        float rmin = prefs.getFloat("range_min", g_config.min_position_mm);
        float rmax = prefs.getFloat("range_max", g_config.max_position_mm);
        uint16_t spd = prefs.getUShort("max_speed", (uint16_t)g_config.max_speed_mm_s);
        uint16_t acc = prefs.getUShort("accel", (uint16_t)g_config.acceleration_mm_s2);
        uint16_t look = prefs.getUShort("lookahead", 20);
        uint16_t over = prefs.getUShort("overshoot", 8);
        g_auto_duration = prefs.getBool("auto_dur", true);

        // TMC tunables
        g_driver.run_current_ma   = prefs.getUShort("tmc_run", g_driver.run_current_ma);
        g_driver.hold_current_pct = prefs.getUChar("tmc_hold", g_driver.hold_current_pct);
        g_driver.stealthchop      = prefs.getUChar("tmc_sc", g_driver.stealthchop);
        g_driver.tpwm_thrs        = prefs.getUInt("tmc_tpwm", g_driver.tpwm_thrs);
        g_driver.toff             = prefs.getUChar("tmc_toff", g_driver.toff);
        g_driver.tbl              = prefs.getUChar("tmc_tbl", g_driver.tbl);
        g_driver.hstart           = prefs.getChar("tmc_hs", g_driver.hstart);
        g_driver.hend             = prefs.getChar("tmc_he", g_driver.hend);
        prefs.end();

        // Validate: reject 0 or out-of-range values
        if (rmin < 0.0f || rmin > PHYSICAL_MAX_TRAVEL_MM) rmin = g_config.min_position_mm;
        if (rmax <= 0.0f || rmax > PHYSICAL_MAX_TRAVEL_MM) rmax = g_config.max_position_mm;
        if (rmin >= rmax) { rmin = g_config.min_position_mm; rmax = g_config.max_position_mm; }
        if (spd == 0 || spd > (uint16_t)MAX_SPEED_MM_S) spd = (uint16_t)g_config.max_speed_mm_s;
        if (acc == 0 || acc > 5000) acc = (uint16_t)g_config.acceleration_mm_s2;
        if (g_driver.run_current_ma < 250 || g_driver.run_current_ma > 3000)
            g_driver.run_current_ma = TMC_RUN_CURRENT_MA;
        if (g_driver.toff < 1 || g_driver.toff > 15) g_driver.toff = TMC_TOFF;

        mapper.setRange(rmin, rmax);
        g_config.max_speed_mm_s = (float)spd;
        g_config.acceleration_mm_s2 = (float)acc;
        g_config.run_current_ma = g_driver.run_current_ma;

        // Apply persisted inertia/predictive-smoothing settings to the motor.
        if (look > 200) look = 20;
        if (over > 50) over = 8;
        motor.setLookaheadMs(look);
        motor.setMaxOvershootMm((float)over);

        APPLOGF("Config: range=[%.1f, %.1f] speed=%.0f accel=%.0f run=%umA look=%u over=%u",
                rmin, rmax, (float)spd, (float)acc, g_driver.run_current_ma, look, over);
    } else {
        prefs.end();
        APPLOG("No saved config, using defaults");
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

// ----------------------------------------------------------------------------
// Old inline HTML (kept disabled). The UI was moved to data/index.html on
// LittleFS; this block is preserved only as reference and is compiled out.
// ----------------------------------------------------------------------------
#if 0
const char* htmlIndexPage = R"RAWHTML(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SlopDrive-32</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: #1a1a2e; color: #eee; min-height: 100vh;
      display: flex; justify-content: center; align-items: flex-start;
      padding: 20px;
    }
    .container { max-width: 500px; width: 100%; }
    h1 { text-align: center; font-size: 1.8em; margin-bottom: 6px; color: #e94560; }
    .subtitle { text-align: center; color: #888; font-size: 0.85em; margin-bottom: 20px; }

    /* Tabs */
    .tabs { display: flex; gap: 4px; margin-bottom: 16px; }
    .tab-btn {
      flex: 1; padding: 10px; border: none; background: #16213e; color: #aaa;
      font-size: 0.95em; cursor: pointer; border-radius: 8px 8px 0 0; transition: all 0.2s;
    }
    .tab-btn.active { background: #0f3460; color: #fff; }
    .tab-content { display: none; }
    .tab-content.active { display: block; }

    /* Cards */
    .card {
      background: #16213e; border-radius: 12px; padding: 20px; margin-bottom: 16px;
      box-shadow: 0 4px 15px rgba(0,0,0,0.3);
    }
    .card h2 { font-size: 1.1em; margin-bottom: 16px; color: #e94560; }

    /* Form Controls */
    label { display: block; font-size: 0.85em; color: #aaa; margin-bottom: 6px; }
    .slider-group { margin-bottom: 20px; }
    input[type="range"] {
      width: 100%; height: 8px; -webkit-appearance: none; appearance: none;
      background: #0f3460; border-radius: 4px; outline: none;
    }
    input[type="range"]::-webkit-slider-thumb {
      -webkit-appearance: none; width: 22px; height: 22px; border-radius: 50%;
      background: #e94560; cursor: pointer; border: 3px solid #fff;
    }
    .value-display {
      text-align: center; font-size: 1.4em; font-weight: bold; color: #e94560; margin-top: 4px;
    }

    /* Buttons */
    button.action-btn {
      width: 100%; padding: 12px; border: none; border-radius: 8px;
      font-size: 1em; font-weight: bold; cursor: pointer; transition: all 0.2s; margin-bottom: 8px;
    }
    .btn-primary { background: #e94560; color: #fff; }
    .btn-primary:hover { background: #c73e54; }
    .btn-secondary { background: #0f3460; color: #fff; }
    .btn-secondary:hover { background: #1a4a8a; }
    .btn-danger { background: #ff6b6b; color: #fff; }
    .btn-danger:hover { background: #ee5a5a; }

    /* Status */
    .status-bar {
      display: flex; justify-content: space-between; align-items: center;
      padding: 10px 16px; border-radius: 8px; margin-bottom: 12px; font-size: 0.85em;
    }
    .status-connected { background: #0a3d2e; color: #4ade80; }
    .status-disconnected { background: #3d1f1f; color: #ff6b6b; }
    .status-homed { background: #0a3d2e; color: #4ade80; }
    .status-not-homed { background: #3d351f; color: #fbbf24; }

    /* Manual Control */
    .manual-controls { display: flex; gap: 8px; margin-bottom: 12px; }
    .manual-controls button { flex: 1; padding: 16px; font-size: 1.2em; }
    .pos-display {
      text-align: center; font-size: 2em; font-weight: bold; color: #4ade80;
      padding: 16px; background: #0f3460; border-radius: 8px; margin-bottom: 12px;
    }

    /* Save indicator */
    .save-indicator {
      text-align: center; color: #4ade80; font-size: 0.85em; height: 20px; opacity: 0;
      transition: opacity 0.3s;
    }
    .save-indicator.show { opacity: 1; }

    /* Range visual */
    .range-visual {
      position: relative; height: 40px; background: #0f3460; border-radius: 8px;
      margin: 12px 0; overflow: hidden;
    }
    .range-fill {
      position: absolute; top: 0; bottom: 0; background: linear-gradient(90deg, #e94560, #ff6b6b);
      border-radius: 8px; transition: left 0.3s, width 0.3s;
    }
    .range-marker {
      position: absolute; top: -4px; bottom: -4px; width: 4px; background: #fff;
      border-radius: 2px; cursor: pointer; transition: left 0.3s;
    }
    .range-label {
      display: flex; justify-content: space-between; font-size: 0.75em; color: #888; margin-top: 4px;
    }
  </style>
</head>
<body>
<div class="container">
  <h1>&#x1F680; SlopDrive-32</h1>
  <div class="subtitle">StrokeEngine Controller v2.0</div>

  <!-- Status Bar -->
  <div id="wifiStatus" class="status-bar status-disconnected">
    <span>WiFi</span><span id="wifiText">Connecting...</span>
  </div>
  <div id="homedStatus" class="status-bar status-not-homed">
    <span>Motor</span><span id="homedText">Not Homed</span>
  </div>

  <!-- Tabs -->
  <div class="tabs">
    <button class="tab-btn active" onclick="showTab('settings')">Settings</button>
    <button class="tab-btn" onclick="showTab('manual')">Manual Control</button>
    <button class="tab-btn" onclick="showTab('logs')">Serial / Log</button>
  </div>

  <!-- Settings Tab -->
  <div id="settings" class="tab-content active">
    <div class="card">
      <h2>&#x1F4CF; Stroke Range</h2>

      <label>Minimum Position (inserted)</label>
      <div class="slider-group">
        <input type="range" id="rangeMin" min="0" max="240" value="0" step="1"
               oninput="updateRangeVisual()">
        <div class="value-display"><span id="rangeMinVal">0</span> mm</div>
      </div>

      <label>Maximum Position (extended)</label>
      <div class="slider-group">
        <input type="range" id="rangeMax" min="0" max="240" value="240" step="1"
               oninput="updateRangeVisual()">
        <div class="value-display"><span id="rangeMaxVal">240</span> mm</div>
      </div>

      <!-- Visual Range Indicator -->
      <label>Usable Stroke Range</label>
      <div class="range-visual">
        <div class="range-fill" id="rangeFill"></div>
      </div>
      <div class="range-label"><span>Home (0mm)</span><span>Full Extent (240mm)</span></div>
      <div style="text-align:center;margin-top:8px;">
        Stroke Depth: <strong id="strokeDepth">240</strong> mm
      </div>
    </div>

    <div class="card">
      <h2>&#x1F6A7; Max Speed</h2>
      <label>Maximum Movement Speed</label>
      <div class="slider-group">
        <input type="range" id="maxSpeed" min="50" max="3000" value="800" step="10"
               oninput="updateSpeedDisplay()">
        <div class="value-display"><span id="speedVal">800</span> mm/s</div>
      </div>
    </div>

    <div class="card">
      <h2>&#x1F300; Motion Smoothness</h2>
      <label>Acceleration (higher = snappier, lower = smoother/softer)</label>
      <div class="slider-group">
        <input type="range" id="accel" min="200" max="5000" value="1500" step="50"
               oninput="updateAccelDisplay()">
        <div class="value-display"><span id="accelVal">1500</span> mm/s&sup2;</div>
      </div>
      <div style="font-size:0.78em;color:#888;line-height:1.4;">
        Low values feel gentle and fluid but may lag fast strokes. High values
        track quick motion tightly but can feel harsh. ~1500 is a good start.
      </div>
    </div>

    <div class="card">
      <h2>&#x1F30A; Inertia (Predictive Smoothing)</h2>
      <label>Lookahead (how far past each point it coasts, in time)</label>
      <div class="slider-group">
        <input type="range" id="lookahead" min="0" max="60" value="20" step="1"
               oninput="updateInertiaDisplay()">
        <div class="value-display"><span id="lookaheadVal">20</span> ms</div>
      </div>
      <label>Max Overshoot (hard cap on coast distance)</label>
      <div class="slider-group">
        <input type="range" id="overshoot" min="0" max="30" value="8" step="1"
               oninput="updateInertiaDisplay()">
        <div class="value-display"><span id="overshootVal">8</span> mm</div>
      </div>
      <div style="font-size:0.78em;color:#888;line-height:1.4;">
        Between Intiface's position updates the motor keeps gliding in the same
        direction for the Lookahead time, so motion stays continuous instead of
        stuttering at each point. Overshoot caps how far past a point it can
        coast. Set both to 0 to disable. Try 20ms / 8mm.
      </div>
    </div>

    <div class="card">
      <h2>&#x23F1; Polling / Timing</h2>
      <label style="display:flex;align-items:center;gap:10px;cursor:pointer;">
        <input type="checkbox" id="autoDuration" style="width:20px;height:20px;">
        Auto Duration (use measured rate instead of app's value)
      </label>
      <div style="text-align:center;margin:14px 0;padding:12px;background:#0f3460;border-radius:8px;">
        Measured app rate:
        <strong id="measuredHz" style="color:#4ade80;">--</strong> Hz
        (<span id="measuredMs">--</span> ms)
      </div>
      <div style="font-size:0.78em;color:#888;line-height:1.4;">
        Many apps send a fixed move-duration (e.g. always 100ms) even when they
        actually update faster, which makes the motor plan slow moves it never
        finishes and feel sluggish. When ON, each move is sized to the REAL
        interval measured between incoming commands. Watch the measured rate
        above while the app drives it; if it reads ~30-60 Hz but motion felt
        slow with this OFF, leaving it ON should feel much snappier.
      </div>
    </div>

    <button class="action-btn btn-primary" onclick="saveSettings()">&#x1F4BE; Save Settings</button>
    <div class="save-indicator" id="saveIndicator">&#x2705; Settings saved!</div>
  </div>

  <!-- Manual Control Tab -->
  <div id="manual" class="tab-content">
    <div class="card">
      <h2>&#x1F4CD; Current Position</h2>
      <div class="pos-display"><span id="currentPos">---</span> mm</div>

      <label>Move to Position</label>
      <div class="slider-group">
        <input type="range" id="manualPos" min="0" max="240" value="0" step="1">
        <div class="value-display"><span id="manualPosVal">0</span> mm</div>
      </div>

      <div class="manual-controls">
        <button class="action-btn btn-secondary" onclick="moveManual(-1)">&#x276F; In</button>
        <button class="action-btn btn-primary" onclick="goToPosition()">&#x1F4A9; Go</button>
        <button class="action-btn btn-secondary" onclick="moveManual(1)">Out &#x276F;</button>
      </div>

      <button class="action-btn btn-secondary" onclick="moveToHome()">&#x1F3E0; Home</button>
      <button class="action-btn btn-danger" onclick="stopMotor()">&#x1F6D1; Stop</button>
    </div>
  </div>

  <!-- Logs Tab -->
  <div id="logs" class="tab-content">
    <div class="card">
      <h2>&#x1F4DC; Device Log</h2>
      <div id="serialModeBar" class="status-bar status-connected" style="display:none;">
        <span>Serial Control</span><span id="serialModeText">Active</span>
      </div>
      <div style="font-size:0.78em;color:#888;line-height:1.4;margin-bottom:10px;">
        The USB serial port is used by Intiface for commands, so boot/debug
        messages appear here instead of the serial monitor. Auto-refreshes.
      </div>
      <pre id="logBox" style="background:#0f3460;border-radius:8px;padding:12px;
           font-size:0.72em;line-height:1.4;color:#cfe;white-space:pre-wrap;
           max-height:340px;overflow-y:auto;margin-bottom:10px;">Loading...</pre>
      <button class="action-btn btn-secondary" onclick="refreshLog()">&#x1F501; Refresh</button>
      <label style="display:flex;align-items:center;gap:10px;cursor:pointer;margin-top:6px;">
        <input type="checkbox" id="autoLog" checked style="width:18px;height:18px;">
        Auto-refresh (2s)
      </label>
    </div>
  </div>
</div>

<script>
const API = '';

// Tab switching
function showTab(name) {
  document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
  document.getElementById(name).classList.add('active');
  event.target.classList.add('active');
}

// Range visual update
function updateRangeVisual() {
  const min = parseInt(document.getElementById('rangeMin').value);
  const max = parseInt(document.getElementById('rangeMax').value);
  document.getElementById('rangeMinVal').textContent = min;
  document.getElementById('rangeMaxVal').textContent = max;

  const depth = Math.max(max - min, 0);
  document.getElementById('strokeDepth').textContent = depth;

  // Update visual bar (240mm total range)
  const pctLeft = (min / 240) * 100;
  const pctWidth = (depth / 240) * 100;
  document.getElementById('rangeFill').style.left = pctLeft + '%';
  document.getElementById('rangeFill').style.width = pctWidth + '%';
}

// Speed display update
function updateSpeedDisplay() {
  document.getElementById('speedVal').textContent = document.getElementById('maxSpeed').value;
}

// Acceleration display update
function updateAccelDisplay() {
  document.getElementById('accelVal').textContent = document.getElementById('accel').value;
}

// Inertia (lookahead + overshoot) display update
function updateInertiaDisplay() {
  document.getElementById('lookaheadVal').textContent = document.getElementById('lookahead').value;
  document.getElementById('overshootVal').textContent = document.getElementById('overshoot').value;
}

// Manual position slider
document.getElementById('manualPos').addEventListener('input', function() {
  document.getElementById('manualPosVal').textContent = this.value;
});

// API calls
async function saveSettings() {
  const min = parseInt(document.getElementById('rangeMin').value);
  const max = parseInt(document.getElementById('rangeMax').value);
  const speed = parseInt(document.getElementById('maxSpeed').value);
  const accel = parseInt(document.getElementById('accel').value);
  const lookahead = parseInt(document.getElementById('lookahead').value);
  const overshoot = parseInt(document.getElementById('overshoot').value);
  const autoDuration = document.getElementById('autoDuration').checked;

  // Ensure valid range
  if (min >= max) {
    alert('Minimum must be less than Maximum');
    return;
  }

  try {
    const resp = await fetch('/api/settings', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({range_min: min, range_max: max, max_speed: speed,
                            accel: accel, lookahead: lookahead, overshoot: overshoot,
                            auto_duration: autoDuration})
    });
    const data = await resp.json();

    if (data.ok) {
      const ind = document.getElementById('saveIndicator');
      ind.classList.add('show');
      setTimeout(() => ind.classList.remove('show'), 2000);
    } else {
      alert('Error: ' + data.error);
    }
  } catch(e) {
    alert('Failed to save settings');
  }
}

async function goToPosition() {
  const pos = parseInt(document.getElementById('manualPos').value);
  await fetch('/api/move', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({position: pos})
  });
}

async function moveManual(dir) {
  const current = parseInt(document.getElementById('currentPos').textContent) || 0;
  const newPos = Math.max(0, Math.min(240, current + dir * 10));
  document.getElementById('manualPos').value = newPos;
  document.getElementById('manualPosVal').textContent = newPos;
  await fetch('/api/move', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({position: newPos})
  });
}

async function moveToHome() {
  await fetch('/api/home', {method: 'POST'});
}

async function stopMotor() {
  await fetch('/api/stop', {method: 'POST'});
}

// Status polling
async function pollStatus() {
  try {
    const resp = await fetch('/api/status');
    const data = await resp.json();

    // WiFi status
    if (data.wifi_connected) {
      document.getElementById('wifiStatus').className = 'status-bar status-connected';
      document.getElementById('wifiText').textContent = data.ip || 'Connected';
    } else {
      document.getElementById('wifiStatus').className = 'status-bar status-disconnected';
      document.getElementById('wifiText').textContent = 'Disconnected';
    }

    // Homed status
    if (data.homed) {
      document.getElementById('homedStatus').className = 'status-bar status-homed';
      document.getElementById('homedText').textContent = 'Homed - Buttplug: ' +
        (data.buttplug_connected ? 'Connected' : 'Disconnected');
    } else {
      document.getElementById('homedStatus').className = 'status-bar status-not-homed';
      document.getElementById('homedText').textContent = data.homing ? 'Homing...' : 'Not Homed';
    }

    // Current position
    if (data.position !== undefined) {
      document.getElementById('currentPos').textContent = data.position.toFixed(1);
    }

    // Serial control mode indicator (on the Logs tab)
    const smBar = document.getElementById('serialModeBar');
    if (smBar) {
      if (data.serial_mode) {
        smBar.style.display = 'flex';
        smBar.className = 'status-bar ' + (data.serial_active ? 'status-connected' : 'status-not-homed');
        document.getElementById('serialModeText').textContent =
          data.serial_active ? 'Active (receiving TCode)' : 'Waiting for Intiface...';
      } else {
        smBar.style.display = 'none';
      }
    }

    // Live measured polling rate from the controlling app
    if (data.measured_hz !== undefined) {
      document.getElementById('measuredHz').textContent =
        data.measured_hz > 0 ? data.measured_hz : '--';
      document.getElementById('measuredMs').textContent =
        data.measured_interval_ms > 0 ? data.measured_interval_ms : '--';
    }
  } catch(e) {}
}

// Load settings on page load
async function loadSettings() {
  try {
    const resp = await fetch('/api/settings');
    const data = await resp.json();
    if (data.range_min !== undefined) {
      document.getElementById('rangeMin').value = data.range_min;
      document.getElementById('rangeMax').value = data.range_max;
      document.getElementById('maxSpeed').value = data.max_speed || 800;
      document.getElementById('accel').value = data.accel || 1500;
      document.getElementById('lookahead').value = (data.lookahead !== undefined) ? data.lookahead : 20;
      document.getElementById('overshoot').value = (data.overshoot !== undefined) ? data.overshoot : 8;
      document.getElementById('autoDuration').checked = (data.auto_duration !== undefined) ? data.auto_duration : true;
      updateRangeVisual();
      updateSpeedDisplay();
      updateAccelDisplay();
      updateInertiaDisplay();
    }
  } catch(e) {}
}

// Device log viewer
async function refreshLog() {
  try {
    const resp = await fetch('/api/log');
    const txt = await resp.text();
    const box = document.getElementById('logBox');
    const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 30;
    box.textContent = txt || '(no log output yet)';
    if (atBottom) box.scrollTop = box.scrollHeight;
  } catch(e) {}
}

// Auto-refresh the log while the Logs tab is visible and the checkbox is on.
setInterval(() => {
  const logsTab = document.getElementById('logs');
  const auto = document.getElementById('autoLog');
  if (logsTab && logsTab.classList.contains('active') && auto && auto.checked) {
    refreshLog();
  }
}, 2000);

// Poll every second
setInterval(pollStatus, 1000);
loadSettings();
pollStatus();
refreshLog();
</script>
</body>
</html>
)RAWHTML";
#endif  // 0  - end of disabled inline HTML reference block

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
    doc["wifi_connected"] = g_wifi_ready;
    doc["ip"] = WiFi.localIP().toString();
    doc["homed"] = g_homed;
    doc["homing"] = g_homing_in_progress;
    doc["buttplug_connected"] = buttplug.isConnected();
    doc["position"] = motor.getPosition();
    // Live measured command cadence from the controlling app. Use the STABLE
    // windowed frame count (frames per 1s window) rather than the jittery
    // instantaneous interval, so the UI reads steady instead of jumping around.
    uint16_t hz = g_measured_hz;
    doc["measured_hz"] = hz;
    doc["measured_interval_ms"] = (hz > 0) ? (uint16_t)(1000 / hz) : 0;
    doc["auto_duration"] = g_auto_duration;
    doc["serial_mode"] = (bool)SERIAL_CONTROL_MODE;
    doc["serial_active"] = buttplug.isSerialActive();

    String json;
    serializeJson(doc, json);
    httpServer.send(200, "application/json", json);
}

void handleApiSettings() {
    if (httpServer.method() == HTTP_GET) {
        JsonDocument doc;
        doc["range_min"] = mapper.getMinMm();
        doc["range_max"] = mapper.getMaxMm();
        doc["max_speed"] = (uint16_t)g_config.max_speed_mm_s;
        doc["accel"] = (uint16_t)g_config.acceleration_mm_s2;
        doc["lookahead"] = motor.getLookaheadMs();
        doc["overshoot"] = (uint16_t)motor.getMaxOvershootMm();
        doc["auto_duration"] = g_auto_duration;

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
        uint16_t speed = doc["max_speed"] | (uint16_t)g_config.max_speed_mm_s;
        uint16_t accel = doc["accel"] | (uint16_t)g_config.acceleration_mm_s2;

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
        g_auto_duration = doc["auto_duration"] | g_auto_duration;

        mapper.setRange(rmin, rmax);
        g_config.max_speed_mm_s = (float)speed;
        g_config.acceleration_mm_s2 = (float)accel;
        motor.setMaxSpeed(g_config.max_speed_mm_s);
        motor.setAcceleration(g_config.acceleration_mm_s2);  // applies live + future moves
        motor.setLookaheadMs(lookahead);                     // applies live
        motor.setMaxOvershootMm(overshoot);                  // applies live
        saveConfig();

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

        if (err || !g_homed) {
            httpServer.send(400, "application/json", "{\"error\":\"Invalid request or not homed\"}");
            return;
        }

        float pos = doc["position"] | 0.0f;
        motor.moveTo(pos);

        JsonDocument resp;
        resp["ok"] = true;
        String json;
        serializeJson(resp, json);
        httpServer.send(200, "application/json", json);
    }
}

void handleApiHome() {
    // Allow homing if not already homed or homing, OR if previously stopped (g_homed reset)
    if (!g_homing_in_progress) {
        g_homed = false;
        g_homing_in_progress = true;
        motor.home();
    }
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

void handleApiStop() {
    motor.stop();
    // Motor power is cut - position is unknown, require re-homing
    g_homed = false;
    g_homing_in_progress = false;
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

// Motor / TMC live-tuning endpoint.
//   GET  -> current TMC settings as JSON (for the Motor tab to load)
//   POST -> {run_current,hold_current,stealthchop,tpwm_thrs,toff,tbl,hstart,hend,save,reset}
//           Applies the values to the driver LIVE. If "save":true also persists
//           them to NVS. If "reset":true restores defaults (then applies/saves).
void handleApiTmc() {
    if (httpServer.method() == HTTP_GET) {
        JsonDocument doc;
        doc["run_current"]  = g_driver.run_current_ma;
        doc["hold_current"] = g_driver.hold_current_pct;
        doc["stealthchop"]  = g_driver.stealthchop;
        doc["tpwm_thrs"]    = g_driver.tpwm_thrs;
        doc["toff"]         = g_driver.toff;
        doc["tbl"]          = g_driver.tbl;
        doc["hstart"]       = g_driver.hstart;
        doc["hend"]         = g_driver.hend;
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
        g_driver = DriverConfig();   // back to config.h defaults
    } else {
        // Read with the current value as the default, then clamp to safe ranges.
        g_driver.run_current_ma   = constrain((int)(doc["run_current"]  | (int)g_driver.run_current_ma), 300, 3000);
        g_driver.hold_current_pct = constrain((int)(doc["hold_current"] | (int)g_driver.hold_current_pct), 0, 100);
        g_driver.stealthchop      = (doc["stealthchop"] | (int)g_driver.stealthchop) ? 1 : 0;
        g_driver.tpwm_thrs        = (uint32_t)(doc["tpwm_thrs"] | (long)g_driver.tpwm_thrs);
        g_driver.toff             = constrain((int)(doc["toff"]   | (int)g_driver.toff), 1, 15);
        g_driver.tbl              = constrain((int)(doc["tbl"]    | (int)g_driver.tbl), 0, 3);
        g_driver.hstart           = constrain((int)(doc["hstart"] | (int)g_driver.hstart), 0, 7);
        g_driver.hend             = constrain((int)(doc["hend"]   | (int)g_driver.hend), -3, 12);
    }

    // Keep g_config.run_current_ma in sync (used elsewhere) and apply live.
    g_config.run_current_ma = g_driver.run_current_ma;
    motor.applyDriverConfig(g_driver);

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

// ============================================================================
// Buttplug Callbacks - Map Buttplug position (0.0-1.0) to physical mm
// ============================================================================

static void buttplugLinearCmd(float position, uint32_t duration_ms) {
    if (!g_homed) return;

    // --- Measure the REAL command cadence from the app ---
    // Many apps hardcode the "I" duration (e.g. always I100) regardless of how
    // often they actually send. If they send every ~30ms but each claims 100ms,
    // the motor keeps planning a slow 100ms move it never finishes -> sluggish,
    // forcing you to crank acceleration way down. We measure the true interval
    // between arrivals and, when Auto Duration is on, use THAT as the move time.
    uint32_t now = millis();
    if (g_last_cmd_ms != 0) {
        uint32_t gap = now - g_last_cmd_ms;
        if (gap > 0 && gap < 1000) {  // ignore pauses/restarts
            // Exponential smoothing to reject single-sample jitter.
            if (g_measured_interval_ms <= 0.0f) g_measured_interval_ms = (float)gap;
            else g_measured_interval_ms = 0.7f * g_measured_interval_ms + 0.3f * (float)gap;
        }
    }
    g_last_cmd_ms = now;

    // Override the duration with the measured cadence when enabled and valid.
    if (g_auto_duration && g_measured_interval_ms > 1.0f) {
        duration_ms = (uint32_t)(g_measured_interval_ms + 0.5f);
    }

    // Map Buttplug normalized position (0.0-1.0) to physical mm using range mapper
    // 0.0 = min_position_mm (retracted), 1.0 = max_position_mm (extended)
    float target_mm = mapper.intensityToPosition(position);

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
            speed_mm_s = constrain(speed_mm_s, 5.0f, g_config.max_speed_mm_s);
        } else {
            // Tiny/zero move - keep a gentle speed so we don't snap.
            speed_mm_s = g_config.max_speed_mm_s * 0.25f;
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
        if (g_homing_in_progress) {
            motor.runHomingStep();

            // Check if homing just completed
            if (!motor.isHoming()) {
                g_homing_in_progress = false;
                g_homed = motor.isHomed();
                if (g_homed) {
                    APPLOG("System is now homed and ready");
                }
            }
        } else if (!g_homed) {
            // Push-to-home: not homed and not running the active routine, so let
            // the user establish home by simply pushing the shaft into the
            // endstop (or by already resting on it at boot). No web UI needed.
            if (motor.checkPushToHome()) {
                g_homed = true;
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
        // feed it into the parser. This is the WiFi-free control path.
        buttplug.pollSerial();
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
            g_measured_hz = (uint16_t)per_sec;
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
    // This also populates g_driver with any saved TMC tunables.
    loadConfig();


    // Initialize motor system
    // Note: motor.begin() applies TMC2160 config from config.h defaults.
    // applyDriverConfig() is called after to apply the loaded/saved TMC tunables.
    motor.begin();

    // Apply the loaded TMC driver settings (from NVS, or defaults) live.
    motor.applyDriverConfig(g_driver);

    // Setup WiFi
    setupWiFi();

    // Setup HTTP server routes
    httpServer.on("/", handleRoot);
    httpServer.on("/api/status", HTTP_GET, handleApiStatus);
    httpServer.on("/api/settings", HTTP_GET, handleApiSettings);
    httpServer.on("/api/settings", HTTP_POST, handleApiSettings);
    httpServer.on("/api/move", HTTP_POST, handleApiMove);
    httpServer.on("/api/home", HTTP_POST, handleApiHome);
    httpServer.on("/api/stop", HTTP_POST, handleApiStop);
    httpServer.on("/api/tmc", HTTP_GET, handleApiTmc);
    httpServer.on("/api/tmc", HTTP_POST, handleApiTmc);
    httpServer.on("/api/log", HTTP_GET, handleApiLog);
    httpServer.begin();
    APPLOGF("HTTP server on port %d", HTTP_PORT);

    // Setup Buttplug WebSocket with callbacks
    buttplug.onLinearRampTo(buttplugLinearCmd);
    buttplug.onLinearStop(buttplugStop);
    buttplug.begin();

    // Connect OUT to Intiface's WSDM device server (if enabled). Intiface
    // listens for devices when its "Device WebSocket Server" toggle is on; the
    // ESP32 must connect to it as a client and send the JSON handshake.
    // Requires WiFi to be up so we can reach the PC running Intiface.
    // In serial-control mode the WiFi/WSDM path is intentionally NOT used for
    // control (that's the whole point - serial avoids WiFi latency). The web UI
    // still runs for configuration, but we skip the outbound WSDM client.
#if INTIFACE_ENABLED && !SERIAL_CONTROL_MODE
    if (g_wifi_ready) {
        buttplug.connectIntiface(INTIFACE_HOST, INTIFACE_PORT);
    }
#endif

    // Create FreeRTOS tasks
    xTaskCreatePinnedToCore(motorTask, "Motor", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(buttplugTask, "Buttplug", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(httpTask, "HTTP", 4096, NULL, 1, NULL, 0);

    APPLOG("System ready - push shaft into endstop to home, or use the web UI");
}

void loop() {
    // Main loop is idle - all work done in FreeRTOS tasks
    vTaskDelay(pdMS_TO_TICKS(100));
}