// DeferredReboot — the one audited "reboot soon, not now" helper.
// HTTP/WS handlers must never call ESP.restart() inline (the response would
// die mid-flight and the client hangs); they arm() this and let their task's
// update loop poll() it. Promoted from two identical hand-rolled copies in
// WebUI and OtaService.
#pragma once

#include <Arduino.h>

#include "AppLog.h"

class DeferredReboot {
public:
    // Arm (or re-arm) a reboot `delay_ms` from now. `why` must be a string
    // literal / static string — it is logged at fire time, not copied.
    void arm(uint32_t delay_ms, const char* why) {
        _why = why;
        _at_ms = millis() + delay_ms;
        _flushing = false;
        _pending = true;
    }

    void cancel() { _pending = false; }
    bool pending() const { return _pending; }

    // Call from the owning task's loop. Fires at most once per arm().
    // Runtime path (httpTask via OtaService::handle), so it NEVER blocks: the
    // post-drain settling window is a second deadline, never a delay().
    void poll() {
        if (!_pending || (int32_t)(millis() - _at_ms) < 0) return;
        if (!_flushing) {
            SLOGW("reboot", "deferred restart: %s", _why ? _why : "?");
            applogDrain();  // give the web ring/serial one last flush
            // Sink writes need wall-clock to land before the core resets.
            _at_ms = millis() + kSinkSettleMs;
            _flushing = true;
            return;
        }
        _pending = false;
        ESP.restart();
    }

private:
    static constexpr uint32_t kSinkSettleMs = 50;

    const char* _why = nullptr;
    uint32_t _at_ms = 0;
    bool _pending = false;
    bool _flushing = false;
};
