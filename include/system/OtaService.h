#pragma once

#include <Arduino.h>
#include <atomic>

#include "DeferredReboot.h"

// ============================================================================
// OtaService — WiFi OTA update path (firmware + LittleFS web bundle)
// ============================================================================
//
// One class owns BOTH over-the-air update routes so the safety gate is written
// exactly once and shared:
//
//   1. ArduinoOTA (PlatformIO-native `espota`) — serviced from commsTask on
//      Core 0. This is the routine `pio run -e sd32-ota -t upload/-t uploadfs`
//      path.
//   2. HTTP endpoints on the existing (synchronous) WebServer —
//      POST /api/ota     (app image  → U_FLASH)
//      POST /api/ota/fs  (LittleFS   → U_SPIFFS)
//      the curl-from-anywhere fallback, token-authenticated.
//
// SAFETY GATE (prepareForOta(), .clinerules §2 real-time safety):
//   Before ANY flash write begins we (1) stop the pattern engine and hard-stop
//   the motor via the existing arbiter stop semantics, (2) suspend the WS
//   telemetry sender task (flash-cache access during a write window causes
//   resets), and (3) raise SystemState::ota_active so ConfigStore::save() (the
//   only NVS flash writer reachable while gated) defers instead of writing.
//   A failed OTA never resumes motion by itself — finishOta(false) resumes
//   only telemetry, leaving the machine stopped until the user acts.
//
// Lifecycle hooks (.clinerules §4): begin() / handle() / (implicit stop via
// the safety gate). Placement is Core 0 only — never the motion-critical core.

class WebServer;
class MotionArbiter;
class PatternEngine;
class UiSocket;
struct SystemState;

class OtaService {
public:
    OtaService(SystemState& state,
               MotionArbiter& arbiter,
               PatternEngine& pattern,
               UiSocket& uiSocket);

    // ---- Lifecycle ----------------------------------------------------------
    // Configure + start ArduinoOTA. Call once after WiFi is up. hostname reuses
    // the existing mDNS name; password is the shared OTA secret.
    void begin(const char* hostname, const char* password);

    // Service ArduinoOTA + the deferred post-success reboot. MUST be called from
    // a Core-0 low-priority loop (commsTask) — never the motion path.
    void handle();

    // Register POST /api/ota and /api/ota/fs on the already-running WebServer.
    void registerHttpRoutes(WebServer* server);

    // True while any OTA session (ArduinoOTA or HTTP) is in flight.
    bool isActive() const { return _active.load(); }

private:
    // Shared safety gate — stops motion + suspends telemetry + blocks NVS.
    // Returns false if an update is already in flight (concurrent refusal).
    bool prepareForOta(const char* source);

    // Resume telemetry, clear the in-flight flag. Motion stays stopped on
    // failure (never auto-resumes). On success the device reboots regardless.
    void finishOta(bool success, const char* what);

    // Constant-time X-OTA-Token header check against the shared secret.
    bool checkAuthToken();
    static bool constantTimeEquals(const char* a, const char* b);

    // Sync-WebServer chunked upload pump. command = U_FLASH or U_SPIFFS.
    void handleUpload(int command);

    SystemState&   _state;
    MotionArbiter& _arbiter;
    PatternEngine& _pattern;
    UiSocket&      _uiSocket;
    WebServer*     _server = nullptr;

    String         _password;
    std::atomic<bool> _active{false};

    // Per-HTTP-upload scratch (single in-flight, so plain members are fine).
    bool    _uploadAuthOk = false;
    bool    _uploadBegun  = false;
    String  _uploadError;

    // Deferred reboot after a successful HTTP flash so the JSON response flushes.
    DeferredReboot _reboot;
};