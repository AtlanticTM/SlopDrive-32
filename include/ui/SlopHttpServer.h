#pragma once

// ============================================================================
// SlopHttpServer — the HTTP backend A/B seam (CLAUDE.md §1: conditional
// compilation isolating driver implementations, no hardcoded coupling).
//
// ONE name, TWO implementations, selected at build time by -DUSE_PSYCHIC_HTTP:
//
//   (default)            SlopHttpServer : IdleGuardWebServer : WebServer
//                        The synchronous Arduino core WebServer plus the
//                        speculative-socket idle guard. Byte-for-byte the
//                        behaviour the device has shipped with. Envs: sd32,
//                        sd32-ota, s3_main.
//
//   -DUSE_PSYCHIC_HTTP   SlopHttpServer wraps PsychicHttpServer (hoeken's
//                        Arduino wrapper over the ESP-IDF esp_http_server).
//                        One httpd task select()-multiplexes N sockets, so a
//                        Chromium speculative socket that connects and stays
//                        silent simply never becomes readable — the measured
//                        4.8-5.0 s / 9.7 s HTTP_MAX_DATA_WAIT captures cannot
//                        happen. IdleGuardWebServer is ABSENT from this build
//                        (its header #errors if pulled in). Envs:
//                        sd32-psychic, sd32-psychic-ota.
//
// WHY THIS SHAPE AND NOT TWO COPIES OF THE HANDLERS:
//   src/ui/WebUI.cpp is ~2200 lines holding ~30 route handlers and a large
//   amount of hard-won behaviour. Duplicating it per backend would guarantee
//   the two copies drift. So the handler BODIES are untouched: they keep
//   calling _httpServer->send()/arg()/method()/sendHeader()/streamFile(), and
//   only this file changes what those calls mean. The Psychic side is a thin
//   request-scoped adapter that presents the exact WebServer-shaped surface
//   the handlers already use.
//
// THE ONE INVARIANT THE PSYCHIC ADAPTER RESTS ON (read before you change it):
//   esp_http_server runs ALL request handlers on a SINGLE task — it is a
//   select() loop, not a thread pool. That is why this adapter can park the
//   current PsychicRequest*/PsychicResponse* in members while a handler runs.
//   Enabling PsychicHttp's ENABLE_ASYNC (async worker threads) would break
//   that invariant and silently cross-wire two concurrent requests, so the
//   build #errors if ENABLE_ASYNC is defined. Do not define it.
//   (The scope guard below is still save/restore-based, so a nested dispatch
//   — e.g. the 404 path re-entering — cannot orphan the pointers.)
// ============================================================================

#if !defined(USE_PSYCHIC_HTTP)

// ---------------------------------------------------------------------------
// A-SIDE — synchronous Arduino WebServer (the shipping path, unchanged)
// ---------------------------------------------------------------------------
#include "ui/IdleGuardWebServer.h"

/// Zero-member subclass: exists only so both backends share one type name that
/// can be forward-declared (WebUI.h/OtaService.h do exactly that). No members,
/// no overrides, inherited constructors — code generation and static RAM are
/// identical to using IdleGuardWebServer directly.
class SlopHttpServer : public IdleGuardWebServer {
public:
    using IdleGuardWebServer::IdleGuardWebServer;
};

#else

// ---------------------------------------------------------------------------
// B-SIDE — PsychicHttp / esp_http_server
// ---------------------------------------------------------------------------
#if defined(ENABLE_ASYNC)
#error "SlopHttpServer's request-scoped adapter requires the single-task esp_http_server model. ENABLE_ASYNC breaks it."
#endif

#include <Arduino.h>
#include <FS.h>
#include <PsychicHttp.h>

#include <functional>

class SlopHttpServer {
public:
    /// A route handler, shaped exactly like the WebServer lambdas WebUI.cpp
    /// already registers: takes nothing, reaches back through the server for
    /// request data and to send the response.
    using Handler = std::function<void()>;

    explicit SlopHttpServer(uint16_t port);

    // ---- Registration / lifecycle (WebServer-shaped) -----------------------

    /// No-op. WebServer only surfaces headers it was told to keep; esp_http_server
    /// reads any header on demand, so there is nothing to pre-declare. Kept so
    /// callers do not need a #if around their collectHeaders() call.
    void collectHeaders(const char** headers, size_t count);

    /// Register for ANY method (WebServer's 2-arg on() default).
    void on(const char* uri, Handler fn);
    /// Register for one method. `method` is an http_method (HTTP_GET/HTTP_POST/...)
    /// — the SAME enum arduino-esp32's WebServer uses (HTTPMethod is a typedef of
    /// http_method in HTTP_Method.h), so call sites are literally identical.
    void on(const char* uri, int method, Handler fn);

    /// Start (or arm) the server. Unlike WebServer, esp_http_server refuses to
    /// start with no netif carrying an IP, so a "not yet" here is not fatal:
    /// handleClient() retries. Returns true if it started immediately.
    bool begin();

    /// NOT a request pump — esp_http_server has its own task. This is the
    /// start/retry pump for the boot-with-WiFi-down case, plus the place any
    /// future periodic HTTP-side bookkeeping belongs. Cheap; call from httpTask.
    void handleClient();

    bool isRunning() const { return _running; }

    // ---- Per-request accessors — VALID ONLY INSIDE A HANDLER ---------------
    // Outside a handler _req/_res are null and every one of these is a safe
    // no-op / empty answer rather than a crash.

    http_method method() const;
    bool   hasArg(const char* name) const;
    String arg(const char* name) const;          ///< "plain" == the raw request body
    bool   hasHeader(const char* name) const;
    String header(const char* name) const;

    void sendHeader(const String& name, const String& value, bool first = false);
    void send(int code, const char* contentType, const String& content);
    void send(int code, const char* contentType, const char* content);
    /// Streams an open file, chunked. Replicates WebServer::streamFile's gzip
    /// rule: a file whose name ends in ".gz" gets Content-Encoding: gzip unless
    /// the caller explicitly asked for a gzip content type.
    void streamFile(fs::File& file, const String& contentType);

    // ---- Escape hatch ------------------------------------------------------
    /// The real server, for code that must speak Psychic natively (OtaService's
    /// upload handler). Only exists on the B side — always behind a #if.
    PsychicHttpServer& psychic() { return _server; }

    /// Run `fn` with this request/response installed as the adapter's current
    /// scope, so backend-neutral code (OtaService::sendUploadResult) can keep
    /// calling send() the way it does on the sync backend. Needed because a
    /// natively-registered Psychic handler does not go through on()'s
    /// trampoline. Returns ESP_OK once a response has gone out.
    esp_err_t runInRequestScope(PsychicRequest* request, PsychicResponse* response,
                                const Handler& fn);

private:
    /// RAII save/restore of the request scope. Constructed by the trampoline
    /// around each handler call so a nested dispatch can never orphan pointers.
    struct Scope {
        SlopHttpServer*  self;
        PsychicRequest*  prevReq;
        PsychicResponse* prevRes;
        bool             prevSent;
        Scope(SlopHttpServer* s, PsychicRequest* rq, PsychicResponse* rs)
            : self(s), prevReq(s->_req), prevRes(s->_res), prevSent(s->_sent) {
            s->_req = rq; s->_res = rs; s->_sent = false;
        }
        ~Scope() { self->_req = prevReq; self->_res = prevRes; self->_sent = prevSent; }
    };

    PsychicHttpServer _server;
    PsychicRequest*   _req  = nullptr;
    PsychicResponse*  _res  = nullptr;
    bool              _sent = false;     ///< did the handler produce a response?
    bool              _running = false;
    bool              _wantRunning = false;
    uint32_t          _lastStartTryMs = 0;
};

#endif  // USE_PSYCHIC_HTTP
