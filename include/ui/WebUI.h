#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "SystemState.h"
#include "UiProtocol.h"

// Forward declarations — WebUI stores references to these, not values.
class MotorDriver;
class RangeMapper;
class PatternEngine;
class MotionArbiter;

#if defined(SD32_MODBUS_TOOLS)
class ServoModbus;
#endif

#if defined(SD32_MODBUS_TOOLS) && defined(DRIVER_AIM_SERVO)
class EncoderValidator;
#endif

// Forward-declared to avoid pulling the HTTP backend's headers into every
// translation unit that includes this one (works around a PlatformIO
// include-path quirk when framework headers are included from a subdirectory
// header). SlopHttpServer is the HTTP backend — the sync Arduino WebServer
// plus the speculative-socket idle guard. See include/ui/SlopHttpServer.h.
class SlopHttpServer;

// ---- Batched telemetry sample ring ------------------------------------------
// A dedicated 10ms-cadence sampler (esp_timer on Core 0) stuffs one of these
// every 10ms — actual carriage position + the target it was TOLD to take. The
// browser polls every ~100ms, drains the ~10 NEW samples since its last visit
// (tracked by a monotonic seq counter), and replays them 10ms apart on its own
// local clock. No firmware-millis() dependency on the browser side, so an
// ESP32 reboot can't desync the playback.
struct TelemetrySample {
    float    position_mm;   // what the driver was COMMANDED to (getPosition) -- "took"
    float    target_mm;     // where the PLANNER told it to go, post-kinematics -- "told"
    float    raw_mm;        // rawest demand, pre-planner -- "asked"
    // Where the shaft PHYSICALLY is, from the drive's own encoder. Equals
    // position_mm when the backend has no feedback, so a chart never has to
    // special-case a missing series.
    float    encoder_mm;
    uint32_t t_dev_us;      // esp_timer_get_time() truncated u32, device clock
};

// 64 slots × 4166µs = 266ms of buffered history at 240Hz. Sized to feed the
// WS 0x01 batching without wrap-around collisions at 40–50Hz drain rates (6
// samples per batch at 240Hz × 50Hz = enough headroom). LittleFS BSS overhead
// is the real flash hog — 64 samples (24 bytes each = 1.5KB) is negligible
// against the 320KB heap.
#define TELEMETRY_RING_SIZE 64
#define TELEMETRY_SAMPLE_INTERVAL_MS 4   // 240Hz = ~4166µs, rounded to 4ms (250Hz)


// WebUI — all HTTP API handlers + LittleFS root-page serving
//
// Constraints:
//   Owns the WebServer instance. All route registration, page serving, and
//   /api/* JSON handlers live here so main.cpp only does composition wiring.
//   Handlers take references to every subsystem they query or mutate
//   (injected via the constructor) — no global reach-through.
//
//   Lifecycle: init() registers every route and calls httpServer.begin();
//   update() calls httpServer.handleClient() (was httpTask) — call frequently.

class WebUI {
public:
    WebUI(SystemState&        state,
          MotorDriver&        motor,
          RangeMapper&        mapper,
          PatternEngine&      patternEngine);

    ~WebUI();

    // Register all HTTP routes and start the server.
    void init();

    // Service the HTTP server (was httpTask body).  Call frequently.
    void update();

    // Expose the owned HTTP server so OtaService can register its POST
    // /api/ota routes — and SlopSyncUiTokenMinter its GET /uitoken — on the
    // SAME server instance (no second listener on port 80).
    SlopHttpServer* server() { return _httpServer; }



    // ---- Batched telemetry ring buffer (Core 0) -----------------------------
    TelemetrySample _telemetry_ring[TELEMETRY_RING_SIZE];
    volatile uint32_t _telemetry_seq = 0;   // total samples ever written
    portMUX_TYPE      _telemetry_mux = portMUX_INITIALIZER_UNLOCKED;

    // Append one sample to the ring. Called from the 10ms esp_timer callback.
    void captureTelemetry(float position_mm, float target_mm, float raw_mm,
                          float encoder_mm);

    // Bridge for the C-style esp_timer callback to reach the instance.
    static void telemetryTimerCb(void* arg);
    // Start the dedicated 10ms telemetry sampler (called from init()).
    void startTelemetrySampler();

    // Zero the session odometer stats (distance/max/strokes) + the INA228 Wh
    // accumulator, and restamp the session clock. Called by the reset-session
    // control (POST /api/settings {reset_stats:true}).
    void resetSessionStats();

#if defined(SD32_MODBUS_TOOLS)
    // Set the ServoModbus reference after construction.
    void setServoModbus(ServoModbus& modbus) { _servoModbus = &modbus; }

    // Drive register 0x03 override, (r/min)/s, 0 = auto. Safe to call while
    // moving. Returns false only when there is no drive attached.
    bool setServoAccelReg(uint16_t value);

    // Copy the drive's OWN 0x03 and armed state into SystemState for the
    // readback. Called from update(); self-rate-limited.
    void refreshServoReadback();
    uint32_t _servo_readback_ms = 0;
#endif

#if defined(SD32_MODBUS_TOOLS) && defined(DRIVER_AIM_SERVO)
    // Set the FAS-vs-encoder validator reference after construction.
    void setEncoderValidator(EncoderValidator& v) { _encValidator = &v; }
#endif

    // ---- 0x10 CMD dispatch --------------------------------------------------
    // Parse a 0x10 CMD WS frame op, apply the mutation, return {ok, response_json}.
    // Callers wrap the result in their own echo (SlopSync's post-clamp ECHO).
    // Returns true on success, false on failure (but still sets payload_out["ok"]=false).
    bool handleCommand(uint8_t op, JsonDocument& payload_in,
                       JsonDocument& payload_out);

    // ---- Shared apply functions (used by both HTTP handlers and WS ops) -----
    // Every apply path bumps _state.cfg_gen via _bumpGen() — called at the end
    // of each mutation.  Returns the post-apply response JSON doc.

    // Apply a settings change (window, speed, accel, blend, auto_dur, expert, default_range).
    // payload_in: {range_min?, range_max?, max_speed?, accel?, blend_mode?, no_persist?, auto_duration?, expert_mode?, default_range_min?, default_range_max?}
    bool applySettings(JsonDocument& payload_in, JsonDocument& payload_out);

    // Apply a manual move command.  payload_in: {position, stream?, bypass_limits?, speed?}
    bool applyMove(JsonDocument& payload_in, JsonDocument& payload_out);

    // Apply a pattern/generator config change.  payload_in: {speed?, depth?, stroke?, sensation?, pattern?, rate_tick?, running?}
    bool applyPattern(JsonDocument& payload_in, JsonDocument& payload_out);

    // Apply driver config change.  payload_in: {run_current?, hold_current?, stealthchop?, tpwm_thrs?, toff?, tbl?, hstart?, hend?, reset?, save?}
    bool applyDriverConfig(JsonDocument& payload_in, JsonDocument& payload_out);

private:
    // ---- Injected dependencies ----------------------------------------------
    SystemState&        _state;
    MotorDriver&        _motor;
    RangeMapper&        _mapper;
    PatternEngine&      _patternEngine;

    // ---- Owned instance (pointer — allocated in constructor, freed in dtor) --
    SlopHttpServer*     _httpServer = nullptr;


#if defined(SD32_MODBUS_TOOLS)
    ServoModbus*        _servoModbus = nullptr;
#endif

#if defined(SD32_MODBUS_TOOLS) && defined(DRIVER_AIM_SERVO)
    EncoderValidator*   _encValidator = nullptr;
#endif

    // ---- cfg_gen helper -----------------------------------------------------
    void _bumpGen() { _state.cfg_gen.store(_state.cfg_gen.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }

    // ---- Arbiter reference (set once from setup(), used by applySettings) ---
    MotionArbiter* _arbiter = nullptr;
public:
    void setArbiter(MotionArbiter* arb) { _arbiter = arb; }
private:


    // ---- HTTP handler methods (one per route) -------------------------------
    void handleRoot();
    void handleApiStatus();
    void handleApiCapabilities();
    void handleApiSettings();
    // AIM servo drive over RS485 Modbus — GET: telemetry + config-register
    // mirror + runtime geometry; POST ops: scan / live-tune / program (full
    // gold-motor sequence, structural regs, forces re-home) / raw / save.
    void handleApiServo();
    void handleApiPattern();
    // User-preset store for Advanced mode (fray-d port). GET → list all saved
    // presets {name, def}; POST {name, def} → save/overwrite; POST {name,
    // delete:true} → remove. Persisted in NVS (namespace "advpreset") so they
    // survive reboots AND web-UI (LittleFS) reflashes — the UI mirrors them to
    // localStorage and can import/export for sharing.
    void handleApiPatternPresets();
    void handleApiLog();
    // Streams the whole diag archive. `tag` empty = every record. Owns
    // httpTask for the duration; that is the point of a one-shot dump.
    void handleApiDiag(const String& tag);
    void handleApiCrash();
    void handleApiCoredump();
    void handleApiTasks();
    // SlopMotion live-tuning rough-in (GET state+bench / POST knobs). No
    // persistence, no UI card yet — curl-driven until the WebUI refactor.
    void handleApiSlopMotion();
};