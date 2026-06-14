#pragma once

#include <Arduino.h>
#include "SystemState.h"

// Forward declarations — WebUI stores references to these, not values.
class MotorDriver;
class RangeMapper;
class Generator;
// NOTE: Interpolator removed — jitter-buffer interpolation is compiled out for
// the trapezoidal-only troubleshooting build. :3
class TransportManager;

class SerialTransport;
class WebSocketTransport;
class BleTransport;

// Forward-declared to avoid pulling <WebServer.h> into every translation unit
// that includes this header (works around a PlatformIO include-path quirk when
// framework headers are included from a subdirectory header).
class WebServer;

// ---- Batched telemetry sample ring ----------------------------------------
// A dedicated 10ms-cadence sampler (esp_timer on Core 0) stuffs one of these
// every 10ms — actual carriage position + the target it was TOLD to take. The
// browser polls every ~100ms, drains the ~10 NEW samples since its last visit
// (tracked by a monotonic seq counter), and replays them 10ms apart on its own
// local clock. No firmware-millis() dependency on the browser side, so an ESP32
// reboot can't desync the playback — the pup keeps a steady rhythm no matter
// what. :3
struct TelemetrySample {
    float    position_mm;   // where the shaft ACTUALLY is (motor.getPosition) — "took"
    float    target_mm;     // where the PLANNER told it to go (post-kinematics) — "told"
    float    raw_mm;        // rawest demand: TCode parser + RangeMapper, pre-planner — "asked"
};


// 25 slots × 10ms = 250ms of buffered playback before the oldest gets lapped.
// Tight on RAM, plenty of headroom for a poll that runs a little late. :3
#define TELEMETRY_RING_SIZE 25
#define TELEMETRY_SAMPLE_INTERVAL_MS 10


// ============================================================================
// WebUI — all HTTP API handlers + LittleFS root-page serving
// ============================================================================
//
// Owns the WebServer instance.  All route registration, page serving, and
// /api/* JSON handlers live here so main.cpp only does composition wiring.
//
// Handlers take references to every subsystem they query or mutate (injected
// via the constructor) — no global reach-through.
//
// Lifecycle:
//   init()          — registers every route + calls httpServer.begin()
//   update()        — httpServer.handleClient(); call frequently (was httpTask)
//
// Paired with Step 7 cleanup (Risk #2):  handleApiStatus emits a stub
// driver{} block (valid:false) and handleApiClearFault is a no-op.  The
// frontend can hide those cards dynamically via feature-advertised flags
// (JSON modularity per .clinerules §3).

class WebUI {
public:
    WebUI(SystemState&        state,
          MotorDriver&        motor,
          RangeMapper&        mapper,
          Generator&          generator,
          TransportManager&   transportMgr,

          SerialTransport&    serialTransport,
          WebSocketTransport& wsTransport,
          BleTransport&       bleTransport);

    ~WebUI();

    /// Register all HTTP routes and start the server.
    void init();

    /// Service the HTTP server (was httpTask body).  Call frequently.
    void update();

    // ---- Batched telemetry ring buffer (Core 0) ----------------------------
    // Filled at a strict 10ms cadence by a dedicated esp_timer callback so the
    // sample spacing is rock-solid regardless of how busy the HTTP loop is. The
    // status poll drains only the samples newer than the browser's last-seen
    // seq, so no sample is ever sent twice and none are silently skipped (unless
    // the ring genuinely overflowed past 250ms of un-polled backlog). :3
    //
    // _telemetry_seq is a monotonic write counter (never wraps in any practical
    // session — 32 bits @ 100Hz = ~497 days). The slot for seq N is
    // N % TELEMETRY_RING_SIZE. The spinlock keeps the multi-field write atomic
    // against the HTTP-thread drain. :3
    TelemetrySample _telemetry_ring[TELEMETRY_RING_SIZE];
    volatile uint32_t _telemetry_seq = 0;   // total samples ever written
    portMUX_TYPE      _telemetry_mux = portMUX_INITIALIZER_UNLOCKED;

    /// Append one sample to the ring. Called from the 10ms esp_timer callback.
    void captureTelemetry(float position_mm, float target_mm, float raw_mm);


    /// Bridge for the C-style esp_timer callback to reach the instance.
    static void telemetryTimerCb(void* arg);
    /// Start the dedicated 10ms telemetry sampler (called from init()).
    void startTelemetrySampler();


private:
    // ---- Injected dependencies ----
    SystemState&        _state;
    MotorDriver&        _motor;
    RangeMapper&        _mapper;
    Generator&          _generator;
    TransportManager&   _transportMgr;

    SerialTransport&    _serialTransport;
    WebSocketTransport& _wsTransport;
    BleTransport&       _bleTransport;

    // ---- Owned instance (pointer — allocated in constructor, freed in dtor) --
    WebServer*          _httpServer = nullptr;

    // ---- Handler methods (one per route) ------------------------------------
    void handleRoot();
    void handleApiStatus();
    void handleApiClearFault();
    void handleApiSettings();
    void handleApiMove();
    void handleApiHome();
    void handleApiStop();
    void handleApiPause();
    void handleApiHalt();
    void handleApiOverride();
    void handleApiTmc();
    void handleApiGen();
    void handleApiInterp();
    void handleApiLog();
    void handleApiMode();
};