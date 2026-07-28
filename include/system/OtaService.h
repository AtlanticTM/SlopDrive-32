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
//   2. HTTP endpoints on the shared SlopHttpServer —
//      POST /api/ota     (app image  → U_FLASH)
//      POST /api/ota/fs  (LittleFS   → U_SPIFFS)
//      the curl-from-anywhere fallback, token-authenticated.
//
//      THIS IS THE OPERATOR'S ONLY WORKING DEPLOYMENT PATH (espota is broken
//      on their host), so it is ported to the PsychicHttp backend with the
//      byte pump FACTORED, not forked: otaBeginWrite/otaWriteChunk/
//      otaEndWrite/otaAbortWrite are the single Update.begin/write/end/abort
//      state machine, and sendUploadResult() is the single final-response
//      policy. Only the ~15 lines that shovel chunks INTO that pump differ per
//      backend, because the two frameworks hand over chunks differently
//      (WebServer's HTTPUpload status enum vs Psychic's upload callback).
//      Auth, the safety gate, the constant-time compare, and the
//      arm-reboot-then-finish ordering are shared code, reached identically.
//
// SAFETY GATE (prepareForOta(), .clinerules §2 real-time safety):
//   Before ANY flash write begins we (1) stop the pattern engine and hard-stop
//   the motor via the existing arbiter stop semantics, (2) [no-op since M5c —
//   the :81 telemetry sender this used to suspend is deleted; the SlopSync hub
//   task is deliberately left running, see OtaService.cpp], and (3) raise
//   SystemState::ota_active so ConfigStore::save() (the only NVS flash writer
//   reachable while gated) defers instead of writing.
//   A failed OTA never resumes motion by itself — finishOta(false) leaves the
//   machine stopped until the user acts.
//
// Lifecycle hooks (.clinerules §4): begin() / handle() / (implicit stop via
// the safety gate). Placement is Core 0 only — never the motion-critical core.

class SlopHttpServer;
class PsychicRequest;
class MotionArbiter;
class PatternEngine;
struct SystemState;

class OtaService {
public:
    OtaService(SystemState& state,
               MotionArbiter& arbiter,
               PatternEngine& pattern);

    // ---- Lifecycle ----------------------------------------------------------
    // Configure + start ArduinoOTA. Call once after WiFi is up. hostname reuses
    // the existing mDNS name; password is the shared OTA secret.
    void begin(const char* hostname, const char* password);

    // Service ArduinoOTA + the deferred post-success reboot. MUST be called from
    // a Core-0 low-priority loop (commsTask) — never the motion path.
    void handle();

    // Register POST /api/ota and /api/ota/fs on the already-running HTTP server.
    void registerHttpRoutes(SlopHttpServer* server);

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
    // The same check against an already-extracted header value (the Psychic
    // path reads headers straight off the request). NULL/absent => refuse.
    bool checkAuthTokenValue(const char* token);
    static bool constantTimeEquals(const char* a, const char* b);

    // ---- SHARED OTA byte pump (backend-neutral) -----------------------------
    // Exactly one Update.begin/write/end/abort state machine. Both the
    // WebServer HTTPUpload pump and the PsychicHttp upload callback funnel
    // through these; nothing else in the class touches Update.
    void otaBeginWrite(int command);              // first chunk: gate + Update.begin
    void otaWriteChunk(const uint8_t* data, size_t len);
    void otaEndWrite(size_t total);               // last chunk: Update.end(true)
    void otaAbortWrite(const char* why);          // transfer died: Update.abort()

    // Shared final-response policy (401 / 400 / 200 + arm reboot + finishOta).
    // Speaks through SlopHttpServer::send(), which both backends implement.
    void sendUploadResult(int command);

    // Sync-WebServer chunked upload pump. command = U_FLASH or U_SPIFFS.
    void handleUpload(int command);

#if defined(USE_PSYCHIC_HTTP)
    // PsychicHttp chunked upload pump. Returns ESP_OK ALWAYS so the body is
    // fully drained even on a rejected token — that is what lets the final
    // response reach curl as a clean 401 instead of a broken pipe, exactly as
    // the WebServer path behaves today.
    int  psychicUploadChunk(int command, uint64_t index,
                            uint8_t* data, size_t len, bool final);
    // Called when the framework abandoned the transfer before the final chunk
    // (socket error, malformed multipart). Idempotent.
    void psychicUploadSalvage(int command);
#endif

    SystemState&    _state;
    MotionArbiter&  _arbiter;
    PatternEngine&  _pattern;
    SlopHttpServer* _server = nullptr;

    String         _password;
    std::atomic<bool> _active{false};

    // Per-HTTP-upload scratch (single in-flight, so plain members are fine).
    bool    _uploadAuthOk  = false;
    bool    _uploadBegun   = false;
    bool    _uploadStarted = false;   // otaBeginWrite() has run for this request
    bool    _uploadFinished = false;  // otaEndWrite() has run for this request
    String  _uploadError;

    // Deferred reboot after a successful HTTP flash so the JSON response flushes.
    DeferredReboot _reboot;
};