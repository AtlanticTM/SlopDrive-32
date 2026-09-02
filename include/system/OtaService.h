#pragma once

#include <Arduino.h>
#include <atomic>

#include "DeferredReboot.h"
#include "RpFlashLink.h"
#include "comms/BridgeProtocol.h"

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
//      on their host). The byte pump is FACTORED, not inlined into the route:
//      otaBeginWrite/otaWriteChunk/otaEndWrite/otaAbortWrite are the single
//      Update.begin/write/end/abort state machine, and sendUploadResult() is
//      the single final-response policy. Auth, the safety gate, the
//      constant-time compare, and the arm-reboot-then-finish ordering all live
//      in that shared code.
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

    // ---- Serial OTA -- the C5 bridge control channel (RFC-057) --------------
    // The C5 authenticates; the S3 trusts the link. Funnels into the SAME
    // Update state machine as HTTP. Returns the state to report back.
    bridge::OtaState otaSerialBegin(uint8_t target, uint32_t declared_size);
    bridge::OtaState otaSerialData(uint16_t seq, const uint8_t* data, size_t len);
    bridge::OtaState otaSerialEnd(uint32_t crc);
    void             otaSerialAbort(uint8_t reason);
    uint8_t  otaSerialAbortReason() const { return _serialAbort; }
    uint16_t otaSerialNextSeq() const { return _serialSeq; }
    bool     otaSerialInFlight() const { return _serialLastMs != 0; }
    // True while the SPI link belongs to an RP2350 image write. motorTask
    // reads this to stand off the link, so exactly one owner drives the bus.
    bool     rpFlashActive() const { return _rp.active(); }
    // The coprocessor's firmware string, for a C-8 stamp on the RP image.
    // Empty until the RP has answered kOpFlashVersion at least once.
    const char* rpVersion() const { return _rp.version(); }
    bool        readRpVersion() { return _rp.readVersion(); }
    // MUST be pumped. A sender that dies mid-transfer otherwise leaves the OTA
    // gate latched forever: NVS blocked, motion stopped, no further OTA.
    void otaSerialTick();

private:
    // Shared safety gate — stops motion + suspends telemetry + blocks NVS.
    // Returns false if an update is already in flight (concurrent refusal).
    bool prepareForOta(const char* source);

    // Resume telemetry, clear the in-flight flag. Motion stays stopped on
    // failure (never auto-resumes). On success the device reboots regardless.
    void finishOta(bool success, const char* what);

    // Constant-time X-OTA-Token header check against the shared secret.
    bool checkAuthToken();
    // The same check against an already-extracted header value.
    // NULL/absent => refuse.
    bool checkAuthTokenValue(const char* token);
    static bool constantTimeEquals(const char* a, const char* b);

    // ---- OTA byte pump ------------------------------------------------------
    // Exactly one Update.begin/write/end/abort state machine. The WebServer
    // HTTPUpload pump funnels through these; nothing else in the class touches
    // Update.
    // declared_size bounds the erase; UPDATE_SIZE_UNKNOWN only where the
    // transport cannot know it up front (chunked HTTP).
    void otaBeginWrite(int command, size_t declared_size, const char* source);
    void otaWriteChunk(const uint8_t* data, size_t len);
    void otaEndWrite(size_t total);               // last chunk: Update.end(true)
    void otaAbortWrite(const char* why);          // transfer died: Update.abort()

    // Final-response policy (401 / 400 / 200 + arm reboot + finishOta).
    // Speaks through SlopHttpServer::send().
    void sendUploadResult(int command);

    // Sync-WebServer chunked upload pump. command = U_FLASH or U_SPIFFS.
    void handleUpload(int command);

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

    // Success on the RP path does NOT reboot this device, so it needs its own
    // exit from the gate: clear the in-flight flags, leave motion stopped.
    void finishRpOta(bool success);

    // The RP2350 image path (sd-4k1.3): a THIRD target inside this class, never
    // a second service, so prepareForOta() stays the one gate.
    RpFlashLink _rp;
    bool        _serialToRp = false;

    // Serial-OTA scratch. Single in-flight, same as the HTTP path.
    int      _serialCommand  = 0;
    uint32_t _serialDeclared = 0;
    uint32_t _serialWritten  = 0;
    uint16_t _serialSeq      = 0;   // next chunk index expected
    uint32_t _serialCrcState = 0;   // streaming crc32 over the plaintext image
    uint8_t  _serialAbort    = 0;   // bridge::OtaAbortReason of the last failure
    uint32_t _serialLastMs   = 0;   // 0 = no serial transfer in flight
    // Off-seq census, reset at begin. Dups mean the ACK direction is lossy or
    // the sender resends early; holes mean chunks are dying C5->S3 (sd-6kz.1).
    uint32_t _serialDups     = 0;
    uint32_t _serialHoles    = 0;
    // Well above a sector erase (~40 ms) and any credit-window wait, well below
    // the point where a stuck gate looks like a dead machine.
    static constexpr uint32_t kSerialIdleAbortMs = 5000;

    // Deferred reboot after a successful HTTP flash so the JSON response flushes.
    DeferredReboot _reboot;
};