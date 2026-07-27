// ============================================================================
// SlopHttpServer — B-side implementation (PsychicHttp / esp_http_server).
//
// The whole file compiles to nothing unless -DUSE_PSYCHIC_HTTP is set, so the
// shipping sd32/sd32-ota build is not affected in any way (no symbols, no
// static RAM, no lib_deps).
// ============================================================================

#if defined(USE_PSYCHIC_HTTP)

#include "ui/SlopHttpServer.h"

#include <LittleFS.h>
#include <string.h>

#include "sloplog/sloplog.h"

namespace {

// Chunk size for file streaming. Deliberately SMALLER than PsychicHttp's own
// FILE_CHUNK_SIZE (8 KB): this is a malloc on the internal heap on every root
// page load, and this project has already been bitten once by internal-heap
// starvation (the SlopSync service had to move to PSRAM). 4 KB is ~3 TCP
// segments per chunk — plenty of pipelining, half the transient footprint,
// and far friendlier to a fragmented maxblock. :3
constexpr size_t kStreamChunk = 4096;

bool endsWithNoCase(const char* s, const char* suffix) {
    if (!s || !suffix) return false;
    size_t ls = strlen(s), lp = strlen(suffix);
    return ls >= lp && strcasecmp(s + ls - lp, suffix) == 0;
}

}  // namespace

// ----------------------------------------------------------------------------
// Construction — the httpd config is the interesting part
// ----------------------------------------------------------------------------

SlopHttpServer::SlopHttpServer(uint16_t port) : _server(port) {
    // ---- Task placement (NON-NEGOTIABLE, CLAUDE.md §2 dual-core rule) -------
    // esp_http_server defaults to tskNO_AFFINITY, which would let the httpd
    // task land on CORE 1 — the motion real-time core. Pin it to Core 0 with
    // the rest of the system/comms work.
    _server.config.core_id = 0;

    // Priority 1 == httpTask's priority, i.e. exactly where HTTP serving runs
    // today. Deliberately BELOW commsTask (2) and the SlopSyncHub task (2):
    // the hub drains the 0x0084 inbound motion stream every 5 ms, and a 115 KB
    // page stream must never be able to preempt that. HTTP responsiveness does
    // not suffer — httpd spends its life blocked in select().
    _server.config.task_priority = 1;

    // Deep enough for the worst handler chain we actually have: LittleFS/VFS
    // file I/O, ArduinoJson serialization, and (the deep one) Update.write()
    // on the OTA path. Psychic's own default is 8192; httpTask — which runs
    // the same work today — is also 8192. We take the headroom on purpose:
    // this project has had two stack-overflow incidents and host tests never
    // see them (megabyte stacks). Task stacks come from the internal HEAP,
    // not BSS, so this does not move the static-RAM figure.
    _server.config.stack_size = 12288;

    // ---- Socket budget (the number that actually fixes the bug) ------------
    // The stall was never a lack of parallelism — it was that ONE synchronous
    // accept()ed socket blocked everyone for HTTP_MAX_DATA_WAIT. select() over
    // N sockets makes a silent speculative socket cost nothing at all.
    //
    // 4 concurrent sockets, LRU purge ON. Chromium opens up to 6 sockets per
    // origin; with purge enabled the 5th connection evicts the oldest IDLE one
    // instantly instead of queueing (a purged speculative socket costs the
    // browser nothing — it reconnects on demand).
    //
    // Why not more: lwip is built with CONFIG_LWIP_MAX_SOCKETS=16 and httpd
    // consumes max_open_sockets + 3 (listener + the ctrl UDP pair) = 7 here.
    // The rest of the budget is already spoken for — UiSocket's WebSocket
    // server (1 listener + up to 5 clients), the SlopSync WS transport on :82
    // (1 listener + clients), ArduinoOTA, mDNS. Raising this is a real risk of
    // socket exhaustion elsewhere, so raise it only with that budget in hand.
    _server.config.max_open_sockets = 4;
    _server.config.lru_purge_enable = true;

    // TCP keepalive so a half-open socket (WiFi roam, laptop lid, Tailscale
    // drop) is reaped by the stack instead of squatting one of the 4 slots.
    _server.config.keep_alive_enable   = true;
    _server.config.keep_alive_idle     = 30;   // s idle before probing
    _server.config.keep_alive_interval = 10;   // s between probes
    _server.config.keep_alive_count    = 3;    // probes before giving up

    // Upload ceiling. The two real payloads are firmware.bin (~2.0 MB, app
    // slot is 6.5 MB) and littlefs.bin (the spiffs partition is 0x360000 =
    // 3.375 MB). 7 MB is a sanity bound that cannot reject a legitimate image.
    _server.maxUploadSize = 7u * 1024u * 1024u;
    // Non-upload request bodies stay at Psychic's 16 KB default — every JSON
    // POST this firmware accepts is orders of magnitude smaller.
}

// ----------------------------------------------------------------------------
// Registration
// ----------------------------------------------------------------------------

void SlopHttpServer::collectHeaders(const char** /*headers*/, size_t /*count*/) {
    // Intentionally empty — see the header. esp_http_server keeps the raw
    // header block and httpd_req_get_hdr_value_str() reads any of it on demand.
}

void SlopHttpServer::on(const char* uri, Handler fn) {
    on(uri, HTTP_ANY, fn);
}

void SlopHttpServer::on(const char* uri, int method, Handler fn) {
    _server.on(uri, method,
               [this, fn](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
                   Scope scope(this, request, response);
                   fn();
                   if (!_sent) {
                       // A handler that returns without answering would leave
                       // the socket hanging until the client times out. Never
                       // observed, but a silent hang is exactly the failure
                       // class we are here to kill.
                       SLOGE("ui", "handler for %s sent no response", request->uriCStr());
                       response->send(500, "application/json",
                                      "{\"ok\":false,\"error\":\"no_response\"}");
                   }
                   return ESP_OK;
               });
}

esp_err_t SlopHttpServer::runInRequestScope(PsychicRequest* request,
                                            PsychicResponse* response,
                                            const Handler& fn) {
    Scope scope(this, request, response);
    fn();
    if (!_sent) {
        SLOGE("ui", "scoped handler for %s sent no response", request->uriCStr());
        response->send(500, "application/json",
                       "{\"ok\":false,\"error\":\"no_response\"}");
    }
    return ESP_OK;
}

bool SlopHttpServer::begin() {
    _wantRunning = true;
    _lastStartTryMs = millis();
    esp_err_t err = _server.begin();
    _running = (err == ESP_OK);
    if (!_running) {
        SLOGW("ui", "PsychicHttp not started yet (%s) — retrying from httpTask",
              esp_err_to_name(err));
    }
    return _running;
}

void SlopHttpServer::handleClient() {
    // esp_http_server serves requests on its own task; there is nothing to
    // pump here. The ONLY job is the deferred start: Psychic refuses to start
    // while no netif holds an IP, and WebUI::init() runs whether or not WiFi
    // came up at boot. Without this a WiFi-late boot would leave HTTP dead
    // forever, which the synchronous WebServer never did.
    if (!_wantRunning || _running) return;
    uint32_t now = millis();
    if (now - _lastStartTryMs < 2000) return;
    _lastStartTryMs = now;
    if (_server.begin() == ESP_OK) {
        _running = true;
        SLOGI("ui", "PsychicHttp started (deferred — network came up)");
    }
}

// ----------------------------------------------------------------------------
// Per-request accessors
// ----------------------------------------------------------------------------

http_method SlopHttpServer::method() const {
    return _req ? _req->method() : HTTP_GET;
}

bool SlopHttpServer::hasArg(const char* name) const {
    if (!_req || !name) return false;
    if (strcmp(name, "plain") == 0) return _req->contentLength() > 0;
    return _req->hasParam(name);
}

String SlopHttpServer::arg(const char* name) const {
    if (!_req || !name) return String();
    // WebServer's convention: arg("plain") is the raw request body. Every JSON
    // POST in this firmware goes through that name.
    if (strcmp(name, "plain") == 0) return _req->body();
    PsychicWebParameter* p = _req->getParam(name);
    return p ? p->value() : String();
}

bool SlopHttpServer::hasHeader(const char* name) const {
    return _req && name && _req->hasHeader(name);
}

String SlopHttpServer::header(const char* name) const {
    if (!_req || !name) return String();
    return _req->header(name);
}

void SlopHttpServer::sendHeader(const String& name, const String& value, bool /*first*/) {
    if (!_res) return;
    _res->addHeader(name.c_str(), value.c_str());
}

void SlopHttpServer::send(int code, const char* contentType, const String& content) {
    if (!_res) return;
    _sent = true;
    _res->send(code, contentType ? contentType : "text/plain", content.c_str());
}

void SlopHttpServer::send(int code, const char* contentType, const char* content) {
    if (!_res) return;
    _sent = true;
    _res->send(code, contentType ? contentType : "text/plain", content ? content : "");
}

void SlopHttpServer::streamFile(fs::File& file, const String& contentType) {
    if (!_res) return;
    _sent = true;

    // WebServer::_streamFileCore's gzip rule, replicated: a ".gz" file is sent
    // with Content-Encoding: gzip UNLESS the caller asked for a gzip-ish type
    // (i.e. actually wants the compressed bytes as the payload). handleRoot()
    // opens /index.html.gz and asks for "text/html", so this is the branch the
    // 115 KB web bundle takes on every cold load.
    const bool wantsRawGz = contentType.equalsIgnoreCase("application/x-gzip") ||
                            contentType.equalsIgnoreCase("application/octet-stream");
    if (!wantsRawGz &&
        (endsWithNoCase(file.name(), ".gz") || endsWithNoCase(file.path(), ".gz"))) {
        _res->addHeader("Content-Encoding", "gzip");
    }

    const size_t size = file.size();

    // Small file: one shot, so httpd emits a real Content-Length.
    if (size > 0 && size <= kStreamChunk) {
        uint8_t* buf = (uint8_t*)malloc(size);
        if (buf) {
            int n = file.read(buf, size);
            _res->send(200, contentType.c_str(), buf, n > 0 ? (size_t)n : 0);
            free(buf);
            return;
        }
        // fall through to the chunked path if that tiny malloc failed
    }

    char* chunk = (char*)malloc(kStreamChunk);
    if (!chunk) {
        SLOGE("ui", "streamFile: no %u B block for the send buffer", unsigned(kStreamChunk));
        _res->send(500, "text/plain", "out of memory");
        return;
    }

    _res->setCode(200);
    _res->setContentType(contentType.c_str());
    _res->sendHeaders();

    // read() returns int and CAN return -1. Assigning that to a size_t would
    // hand sendChunk() a ~4 GB length — keep it signed and bail on <= 0.
    for (;;) {
        int n = file.read((uint8_t*)chunk, kStreamChunk);
        if (n <= 0) break;                       // EOF or read error
        if (_res->sendChunk((uint8_t*)chunk, (size_t)n) != ESP_OK) {
            free(chunk);
            return;   // socket died mid-stream; sendChunk already aborted it
        }
        if ((size_t)n < kStreamChunk) break;     // short read == EOF
    }

    free(chunk);
    _res->finishChunking();
}

#endif  // USE_PSYCHIC_HTTP
