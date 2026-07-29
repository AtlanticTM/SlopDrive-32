// OtaService — shared WiFi OTA path (ArduinoOTA + HTTP), one safety gate
// Constraints:
//   ArduinoOTA and the HTTP upload path share one _active flag
//   (prepareForOta): concurrent-update refusal and "refuse if ArduinoOTA is
//   already active" both fall out of that single CAS -- never add a second
//   in-flight flag.
//   prepareForOta() MUST complete before the first flash write on either
//   path: it stops all motion, then raises the OTA-active guard so
//   ConfigStore::save() defers any NVS write for the duration (a concurrent
//   write can reset the chip mid-flash).
//   A failed OTA never resumes motion by itself -- the operator clears the
//   fault / re-homes.

#include "OtaService.h"

#include "ui/SlopHttpServer.h"
#include <ArduinoOTA.h>
#include <Update.h>
#include <string.h>

#include "sloplog/sloplog.h"
#include "SystemState.h"
#include "MotionArbiter.h"
#include "PatternEngine.h"

OtaService::OtaService(SystemState& state,
                       MotionArbiter& arbiter,
                       PatternEngine& pattern)
    : _state(state), _arbiter(arbiter), _pattern(pattern) {}

// ---- begin() -- configure + start ArduinoOTA (call once, after WiFi is up) --

void OtaService::begin(const char* hostname, const char* password) {
    _password = password ? password : "";

    ArduinoOTA.setHostname(hostname);
    if (_password.length()) {
        ArduinoOTA.setPassword(_password.c_str());
    } else {
        SLOGW("ota", "WARNING: no OTA password set — ArduinoOTA is UNAUTHENTICATED");
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
        SLOGE("ota", "ArduinoOTA error [%u]", (unsigned)error);
        // Failed OTA NEVER resumes motion by itself — motion stays held.
        finishOta(false, "ArduinoOTA");
    });

    ArduinoOTA.begin();
    SLOGI("ota", "ArduinoOTA ready — hostname='%s' (espota)", hostname);
}

// ---- handle() -- service ArduinoOTA + deferred HTTP reboot (Core-0 low-prio only) --

void OtaService::handle() {
    ArduinoOTA.handle();

    _reboot.poll();
}

// ---- prepareForOta() -- shared safety gate ----------------------------------
// Runs BEFORE the first flash write on BOTH paths. Single in-flight flag gives
// the concurrent-refusal + "refuse if ArduinoOTA active" guarantee for free
// (both paths compete for the same _active CAS).
// See: .clinerules §2, OTA §2/§3.

bool OtaService::prepareForOta(const char* source) {
    bool expected = false;
    if (!_active.compare_exchange_strong(expected, true)) {
        SLOGW("ota", "refused (%s) — an update is already in flight", source);
        return false;
    }

    SLOGI("ota", "start (%s) — stopping motion BEFORE flash write", source);

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
    // M5c: the :81 sender task this used to suspend no longer exists. The
    // SlopSync hub task is deliberately NOT suspended in its place — it is a
    // 5 ms cooperative loop over bounded queues that neither blocks nor
    // allocates in steady state, and it is what still answers a client asking
    // "is the machine safe?" while a flash is in progress.

    // (3) Raise the NVS-write guard so ConfigStore::save() defers if anything
    //     tries to persist config during the write window.
    _state.ota_active.store(true);

    return true;
}

// ---- finishOta() -- clear the in-flight flag on failure ---------------------
// Success leaves the machine gated through the reboot; failure leaves motion
// held, never auto-resumed.

void OtaService::finishOta(bool success, const char* what) {
    if (success) {
        SLOGI("ota", "%s complete — device will reboot; motion stays stopped until it comes back", what);
        // Intentionally do NOT resume telemetry or clear the gate: the device
        // reboots (ArduinoOTA auto, or HTTP scheduled) and boots fresh.
        return;
    }

    SLOGE("ota", "%s FAILED — old image kept, MOTION STAYS STOPPED", what);

    _state.ota_active.store(false);
    _active.store(false);
    // estop_requested stays latched: a failed OTA never resumes motion by itself.
    // The user re-arms via the normal clear-fault / home flow.
}

// ---- Auth -- constant-time X-OTA-Token check --------------------------------

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

bool OtaService::checkAuthTokenValue(const char* token) {
    // Same policy as checkAuthToken(), for callers that already hold the header
    // value. No configured password → hard-refuse (never silently open).
    if (_password.length() == 0) return false;
    if (token == nullptr) return false;
    return constantTimeEquals(token, _password.c_str());
}

// ---- OTA byte pump -- the ONLY place Update.* is called ---------------------
// Both backends' chunk pumps funnel through these four. Keeping them together
// (rather than one copy per backend) means the operator's only working
// deployment path never has two flash state machines that can drift apart.

void OtaService::otaBeginWrite(int command) {
    _uploadStarted = true;
    // Safety gate — also enforces single-in-flight / refuse-if-ArduinoOTA.
    if (!prepareForOta(command == U_FLASH ? "HTTP(app)" : "HTTP(fs)")) {
        _uploadError = "busy";
        return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, command)) {
        _uploadError = String("begin failed: ") + Update.errorString();
        SLOGE("ota", "Update.begin failed: %s", Update.errorString());
        // Roll the gate back — nothing was written.
        finishOta(false, command == U_FLASH ? "HTTP app" : "HTTP fs");
        return;
    }
    _uploadBegun = true;
    SLOGI("ota", "HTTP flash begun (%s)", command == U_FLASH ? "app" : "fs");
}

void OtaService::otaWriteChunk(const uint8_t* data, size_t len) {
    if (!(_uploadAuthOk && _uploadBegun && _uploadError.length() == 0)) return;
    if (len == 0) return;
    if (Update.write(const_cast<uint8_t*>(data), len) != len) {
        _uploadError = String("write failed: ") + Update.errorString();
        SLOGE("ota", "Update.write failed: %s", Update.errorString());
    }
}

void OtaService::otaEndWrite(size_t total) {
    _uploadFinished = true;
    if (_uploadAuthOk && _uploadBegun && _uploadError.length() == 0) {
        if (!Update.end(true)) {   // true = set the size to what was written
            _uploadError = String("end failed: ") + Update.errorString();
            SLOGE("ota", "Update.end failed: %s", Update.errorString());
        } else {
            SLOGI("ota", "HTTP flash finalized — %u bytes", (unsigned)total);
        }
    } else if (_uploadBegun) {
        Update.abort();
    }
}

void OtaService::otaAbortWrite(const char* why) {
    if (_uploadBegun && !_uploadFinished) Update.abort();
    if (_uploadError.length() == 0) _uploadError = "aborted";
    SLOGW("ota", "HTTP upload %s — old image intact", why ? why : "aborted");
}

// ---- sendUploadResult() -- shared final-response policy ---------------------
// Speaks only through SlopHttpServer::send(), which both backends implement,
// so this is the same code on both sides. Ordering is load-bearing: send the
// 200 FIRST, then arm the deferred reboot (so the response actually flushes
// to curl), then finishOta(true), which deliberately leaves the machine
// gated through the reboot.

void OtaService::sendUploadResult(int command) {
    const bool isApp = (command == U_FLASH);
    const char* what = isApp ? "HTTP app" : "HTTP fs";

    if (!_uploadAuthOk) {
        _server->send(401, "application/json",
                      "{\"ok\":false,\"error\":\"unauthorized\"}");
        return;
    }
    // A transfer that never reached its final chunk must NEVER be reported as
    // a flashed image. (WebServer surfaces this as UPLOAD_FILE_ABORTED.)
    if (_uploadError.length() == 0 && !_uploadFinished) {
        _uploadError = "incomplete upload";
        if (_uploadBegun) Update.abort();
    }
    if (_uploadError.length()) {
        _server->send(400, "application/json",
                      String("{\"ok\":false,\"error\":\"") + _uploadError + "\"}");
        finishOta(false, what);
        return;
    }
    _server->send(200, "application/json",
                  isApp ? "{\"ok\":true,\"target\":\"app\",\"reboot_ms\":500}"
                        : "{\"ok\":true,\"target\":\"fs\",\"reboot_ms\":500}");
    _reboot.arm(500, isApp ? "OTA app image flashed"
                           : "OTA LittleFS bundle flashed");   // let the response flush first
    finishOta(true, what);
}

// ---- HTTP routes -- POST /api/ota (U_FLASH) + POST /api/ota/fs (U_SPIFFS) ---


void OtaService::registerHttpRoutes(SlopHttpServer* server) {
    _server = server;

    // WebServer only retains headers we explicitly ask it to keep.
    static const char* kOtaHeaders[] = { "X-OTA-Token" };
    server->collectHeaders(kOtaHeaders, 1);

    // ---- POST /api/ota  (application image → U_FLASH) -----------------------
    server->on("/api/ota", HTTP_POST,
        [this]() { sendUploadResult(U_FLASH); },   // final response (after the upload cb)
        [this]() { handleUpload(U_FLASH); });

    // ---- POST /api/ota/fs  (LittleFS bundle → U_SPIFFS) ---------------------
    server->on("/api/ota/fs", HTTP_POST,
        [this]() { sendUploadResult(U_SPIFFS); },
        [this]() { handleUpload(U_SPIFFS); });

    SLOGI("ota", "HTTP routes: POST /api/ota (app), POST /api/ota/fs (LittleFS) — X-OTA-Token auth");
}

// ---- handleUpload() -- sync-WebServer chunked upload pump for both endpoints --

void OtaService::handleUpload(int command) {
    HTTPUpload& up = _server->upload();

    switch (up.status) {
    case UPLOAD_FILE_START: {
        _uploadError    = "";
        _uploadBegun    = false;
        _uploadStarted  = false;
        _uploadFinished = false;
        // Auth check on the FIRST chunk — the earliest the framework lets us see
        // headers. A bad token means we never call Update.begin(), so nothing is
        // ever written to flash; the final handler answers 401.
        _uploadAuthOk = checkAuthToken();
        if (!_uploadAuthOk) {
            SLOGW("ota", "HTTP upload REJECTED (bad/missing X-OTA-Token) file=%s", up.filename.c_str());
            return;
        }
        otaBeginWrite(command);
        break;
    }

    case UPLOAD_FILE_WRITE:
        otaWriteChunk(up.buf, up.currentSize);
        break;

    case UPLOAD_FILE_END:
        otaEndWrite(up.totalSize);
        break;

    case UPLOAD_FILE_ABORTED:
        otaAbortWrite("aborted mid-transfer");
        break;

    default:
        break;
    }
}

