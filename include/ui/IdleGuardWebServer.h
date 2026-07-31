#pragma once

// This exists ONLY for the synchronous WebServer's single-serve-slot stall.
// Prefer including ui/SlopHttpServer.h — that is the name call sites use.
#include <WebServer.h>
// shutdown()/SHUT_RDWR for abortBlockedClient(). lwIP's BSD socket header is
// the ESP-IDF-blessed source for these (same choice SlopSyncUdpDiscovery makes).
#include <lwip/sockets.h>

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

    // ---- Partial-request reap: THE OTHER HALF OF THE SAME STARVATION -------
    // dropIdleCapture() above only reaps clients that sent NOTHING
    // (!available()). A client that sends SOME bytes and never finishes the
    // request takes the opposite branch in handleClient(): available() is
    // true, so it calls _parseRequest(), which blocks in
    // readStringUntil() for HTTP_MAX_SEND_WAIT (5000 ms). A between-calls
    // guard structurally cannot catch that — httpTask never returns to run it.
    // Reproduced 2026-07-31: five half-open connections -> TASK_WDT reboot,
    // heap_min 40 619 B (not a memory failure). See LEDGER ACTIVE TASK 1.
    //
    // CALLED FROM A DIFFERENT TASK (commsTask, 2 ms cadence) while httpTask is
    // blocked inside handleClient(). That is the whole point: the only task
    // that can cancel the stall is one the stall does not block.
    //
    // WHY shutdown() AND NOT stop()/close(): close() frees the fd NUMBER while
    // another task is blocked reading it, so a concurrent accept can reuse
    // that number and the blocked reader wakes onto someone else's socket.
    // shutdown() wakes the reader immediately and leaves the fd allocated;
    // httpTask then fails its parse and closes the client through its own
    // normal path, which is the only path that touches _currentClient.
    //
    // WHY READING _currentClient FROM ANOTHER TASK IS SAFE HERE: only ever
    // called once the caller has confirmed httpTask has been inside
    // handleClient() for longer than the reap threshold. Its owner is
    // therefore parked in a read on this object and cannot be reassigning it.
    // Do NOT call this speculatively.
    int abortBlockedClient() {
        const int fd = _currentClient.fd();
        if (fd < 0) return -1;
        ::shutdown(fd, SHUT_RDWR);
        return fd;
    }
};
