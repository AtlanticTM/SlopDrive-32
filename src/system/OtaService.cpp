#include "OtaService.h"

#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <string.h>

#include "AppLog.h"
#include "SystemState.h"
#include "MotionArbiter.h"
#include "PatternEngine.h"
#include "UiSocket.h"

// ============================================================================
// OtaService — shared WiFi OTA path (ArduinoOTA + HTTP), one safety gate
// ============================================================================

OtaService::OtaService(SystemState& state,
                       MotionArbiter& arbiter,
                       PatternEngine& pattern,
                       UiSocket& uiSocket)
    : _state(state), _arbiter(arbiter), _pattern(pattern), _uiSocket(uiSocket) {}

// ----------------------------------------------------------------------------
// begin() — configure + start ArduinoOTA (call once, after WiFi is up)
// ----------------------------------------------------------------------------

void OtaService::begin(const char* hostname, const char* password) {
    _password = password ? password : "";

    ArduinoOTA.setHostname(hostname);
    if (_password.length()) {
        ArduinoOTA.setPassword(_password.c_str());
    } else {
        APPLOG("[OTA] WARNING: no OTA password set — ArduinoOTA is UNAUTHENTICATED");
    }

    ArduinoOTA.onStart([this]() {
        // ArduinoOTA.getCommand() → U_FLASH (app) or U_SPIFFS (LittleFS bundle).
        int cmd = ArduinoOTA.getCommand();
        prepareForOta(cmd == U_FLASH ? "ArduinoOTA(app)" : "ArduinoOTA(fs)");
    });

    ArduinoOTA.onEnd([this]() {
        // Success — ArduinoOTA reboots the device itself right after this hook.
        // Leave the machine gated (motion stopped) through the reboot.
        finishOta(true, "ArduinoOTA");
    });

    ArduinoOTA.onError([this](ota_error_t error) {
        APPLOGF("[OTA] ArduinoOTA error [%u]", (unsigned)error);
        // Failed OTA NEVER resumes motion by itself — telemetry back, motion held.
        finishOta(false, "ArduinoOTA");
    });

    ArduinoOTA.begin();
    APPLOGF("[OTA] ArduinoOTA ready — hostname='%s' (espota)", hostname);
}

// ----------------------------------------------------------------------------
// handle() — service ArduinoOTA + deferred HTTP reboot. Core-0 low-prio only.
// ----------------------------------------------------------------------------

void OtaService::handle() {
    ArduinoOTA.handle();

    if (_rebootPending && (int32_t)(millis() - _rebootAtMs) >= 0) {
        _rebootPending = false;
        APPLOG("[OTA] rebooting into new image");
        delay(20);          // boot-adjacent: allow the TCP FIN/response to flush
        ESP.restart();
    }
}

// ----------------------------------------------------------------------------
// prepareForOta() — SHARED SAFETY GATE (.clinerules §2 / OTA §2/§3)
// ----------------------------------------------------------------------------
//
// Runs BEFORE the first flash write on BOTH paths. Single in-flight flag gives
// the concurrent-refusal + "refuse if ArduinoOTA active" guarantee for free
// (both paths compete for the same _active CAS).

bool OtaService::prepareForOta(const char* source) {
    bool expected = false;
    if (!_active.compare_exchange_strong(expected, true)) {
        APPLOGF("[OTA] refused (%s) — an update is already in flight", source);
        return false;
    }

    APPLOGF("[OTA] start (%s) — stopping motion + suspending telemetry BEFORE flash write", source);

    // (1) Refuse/stop all motion first. Stop the pattern engine, hard-stop the
    //     motor via the existing stop semantics, and latch the e-stop flag so
    //     every Core-1 motion gate (pattern / stream / generator) parks itself.
    _pattern.stop();               // user-facing gate: running=false
    _pattern.emergencyStop();      // task holds position immediately
    _arbiter.emergencyStop();      // disable/quiesce the motor stream source
    _arbiter.hardStopMotion();     // immediate stop (no decel ramp)
    _state.estop_requested.store(true);
    _state.paused          = false;
    _state.manual_override = false;
    _state.resume_start_ms = 0;

    // (2) Suspend the telemetry/WS broadcast task — flash writes stall the flash
    //     cache; a task touching flash-resident code/data mid-write resets us.
    _uiSocket.suspendSender();

    // (3) Raise the NVS-write guard so ConfigStore::save() defers if anything
    //     tries to persist config during the write window.
    _state.ota_active.store(true);

    return true;
}

// ----------------------------------------------------------------------------
// finishOta() — resume telemetry on failure; success leaves the machine gated
// (motion held, never auto-resumed) and reboots.
// ----------------------------------------------------------------------------

void OtaService::finishOta(bool success, const char* what) {
    if (success) {
        APPLOGF("[OTA] %s complete — device will reboot; motion stays stopped until it comes back", what);
        // Intentionally do NOT resume telemetry or clear the gate: the device
        // reboots (ArduinoOTA auto, or HTTP scheduled) and boots fresh.
        return;
    }

    APPLOGF("[OTA] %s FAILED — old image kept, telemetry resumed, MOTION STAYS STOPPED", what);
    _uiSocket.resumeSender();
    _state.ota_active.store(false);
    _active.store(false);
    // estop_requested stays latched: a failed OTA never resumes motion by itself.
    // The user re-arms via the normal clear-fault / home flow.
}

// ----------------------------------------------------------------------------
// Auth — constant-time X-OTA-Token check
// ----------------------------------------------------------------------------

bool OtaService::constantTimeEquals(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t la = strlen(a);
    size_t lb = strlen(b);
    // Fold the length difference into the accumulator so a mismatched length
    // still runs the full compare and can never early-out to a timing tell.
    uint8_t diff = (uint8_t)((la ^ lb) != 0);
    size_t n = (la > lb) ? la : lb;
    for (size_t i = 0; i < n; i++) {
        char ca = (i < la) ? a[i] : 0;
        char cb = (i < lb) ? b[i] : 0;
        diff |= (uint8_t)(ca ^ cb);
    }
    return diff == 0;
}

bool OtaService::checkAuthToken() {
    // No configured password → hard-refuse the HTTP path (never silently open).
    if (_password.length() == 0) return false;
    if (!_server || !_server->hasHeader("X-OTA-Token")) return false;
    String tok = _server->header("X-OTA-Token");
    return constantTimeEquals(tok.c_str(), _password.c_str());
}

// ----------------------------------------------------------------------------
// HTTP routes — POST /api/ota (U_FLASH) + POST /api/ota/fs (U_SPIFFS)
// ----------------------------------------------------------------------------

void OtaService::registerHttpRoutes(WebServer* server) {
    _server = server;

    // WebServer only retains headers we explicitly ask it to keep.
    static const char* kOtaHeaders[] = { "X-OTA-Token" };
    server->collectHeaders(kOtaHeaders, 1);

    // ---- POST /api/ota  (application image → U_FLASH) -----------------------
    server->on("/api/ota", HTTP_POST,
        [this]() {   // final response (fires after the upload callback completes)
            if (!_uploadAuthOk) {
                _server->send(401, "application/json",
                              "{\"ok\":false,\"error\":\"unauthorized\"}");
                return;
            }
            if (_uploadError.length()) {
                _server->send(400, "application/json",
                              String("{\"ok\":false,\"error\":\"") + _uploadError + "\"}");
                finishOta(false, "HTTP app");
                return;
            }
            _server->send(200, "application/json",
                          "{\"ok\":true,\"target\":\"app\",\"reboot_ms\":500}");
            _rebootPending = true;
            _rebootAtMs    = millis() + 500;   // let the response flush first
            finishOta(true, "HTTP app");
        },
        [this]() { handleUpload(U_FLASH); });

    // ---- POST /api/ota/fs  (LittleFS bundle → U_SPIFFS) ---------------------
    server->on("/api/ota/fs", HTTP_POST,
        [this]() {
            if (!_uploadAuthOk) {
                _server->send(401, "application/json",
                              "{\"ok\":false,\"error\":\"unauthorized\"}");
                return;
            }
            if (_uploadError.length()) {
                _server->send(400, "application/json",
                              String("{\"ok\":false,\"error\":\"") + _uploadError + "\"}");
                finishOta(false, "HTTP fs");
                return;
            }
            _server->send(200, "application/json",
                          "{\"ok\":true,\"target\":\"fs\",\"reboot_ms\":500}");
            _rebootPending = true;
            _rebootAtMs    = millis() + 500;
            finishOta(true, "HTTP fs");
        },
        [this]() { handleUpload(U_SPIFFS); });

    APPLOG("[OTA] HTTP routes: POST /api/ota (app), POST /api/ota/fs (LittleFS) — X-OTA-Token auth");
}

// ----------------------------------------------------------------------------
// handleUpload() — sync-WebServer chunked upload pump for both endpoints
// ----------------------------------------------------------------------------

void OtaService::handleUpload(int command) {
    HTTPUpload& up = _server->upload();

    switch (up.status) {
    case UPLOAD_FILE_START: {
        _uploadError = "";
        _uploadBegun = false;
        // Auth check on the FIRST chunk — the earliest the framework lets us see
        // headers. A bad token means we never call Update.begin(), so nothing is
        // ever written to flash; the final handler answers 401.
        _uploadAuthOk = checkAuthToken();
        if (!_uploadAuthOk) {
            APPLOGF("[OTA] HTTP upload REJECTED (bad/missing X-OTA-Token) file=%s", up.filename.c_str());
            return;
        }
        // Safety gate — also enforces single-in-flight / refuse-if-ArduinoOTA.
        if (!prepareForOta(command == U_FLASH ? "HTTP(app)" : "HTTP(fs)")) {
            _uploadError = "busy";
            return;
        }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, command)) {
            _uploadError = String("begin failed: ") + Update.errorString();
            APPLOGF("[OTA] Update.begin failed: %s", Update.errorString());
            // Roll the gate back — nothing was written.
            finishOta(false, command == U_FLASH ? "HTTP app" : "HTTP fs");
            return;
        }
        _uploadBegun = true;
        APPLOGF("[OTA] HTTP flash begun (%s)", command == U_FLASH ? "app" : "fs");
        break;
    }

    case UPLOAD_FILE_WRITE:
        if (_uploadAuthOk && _uploadBegun && _uploadError.length() == 0) {
            if (Update.write(up.buf, up.currentSize) != up.currentSize) {
                _uploadError = String("write failed: ") + Update.errorString();
                APPLOGF("[OTA] Update.write failed: %s", Update.errorString());
            }
        }
        break;

    case UPLOAD_FILE_END:
        if (_uploadAuthOk && _uploadBegun && _uploadError.length() == 0) {
            if (!Update.end(true)) {   // true = set the size to what was written
                _uploadError = String("end failed: ") + Update.errorString();
                APPLOGF("[OTA] Update.end failed: %s", Update.errorString());
            } else {
                APPLOGF("[OTA] HTTP flash finalized — %u bytes", (unsigned)up.totalSize);
            }
        } else if (_uploadBegun) {
            Update.abort();
        }
        break;

    case UPLOAD_FILE_ABORTED:
        if (_uploadBegun) Update.abort();
        _uploadError = "aborted";
        APPLOG("[OTA] HTTP upload aborted mid-transfer — old image intact");
        break;

    default:
        break;
    }
}