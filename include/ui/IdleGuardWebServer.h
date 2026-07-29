#pragma once

// This exists ONLY for the synchronous WebServer's single-serve-slot stall.
// Prefer including ui/SlopHttpServer.h — that is the name call sites use.
#include <WebServer.h>

// ============================================================================
// IdleGuardWebServer — sync WebServer + speculative-socket idle guard.
//
// The core WebServer serves ONE connection at a time and, in HC_WAIT_READ,
// waits HTTP_MAX_DATA_WAIT (5000 ms, a bare #define with no override guard)
// for the connected client to send its request bytes — serving nobody else
// meanwhile. Chromium browsers routinely open SPECULATIVE pool sockets that
// sit idle before (ever) carrying a request, so each one deafens the server
// for a full 5 s. Measured live 2026-07-24 with a single open UI tab:
// time-to-first-byte quantized at 4.8–5.0 s (one capture) and 9.7 s (two
// captures back to back), TCP connect instant, radio clean, [STALL] silent —
// the wait loop yields normally, so no task-level watchdog can see it.
//
// The guard: between handleClient() calls, drop a connected-but-SILENT client
// after a short grace period. A real request's bytes arrive within
// milliseconds of the TCP handshake — only speculative/idle sockets stay
// silent this long — and a dropped speculative socket costs the browser
// nothing (it opens a fresh connection on demand).
//
// _currentClient/_currentStatus/_statusChange are protected in the core
// class, which is why this is a subclass rather than a wrapper.
//
// NOT transitional. The PsychicHttp A/B that was going to retire this whole
// starvation class was retired instead (2026-07-29), so the sync WebServer is
// the HTTP plane and this guard is load-bearing until something replaces it.
// ============================================================================
class IdleGuardWebServer : public WebServer {
public:
    using WebServer::WebServer;

    // Call once per httpTask loop, right after handleClient(). Never blocks.
    void dropIdleCapture(uint32_t maxSilentMs = 300) {
        if (_currentStatus == HC_WAIT_READ && _currentClient &&
            !_currentClient.available() &&
            (millis() - _statusChange) > maxSilentMs) {
            // handleClient() sees !connected() next call and frees the slot.
            _currentClient.stop();
        }
    }
};
