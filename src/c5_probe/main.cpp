// ESP32-C5 comms bridge — WiFi 6 / 5 GHz front end for the SlopDrive-32 S3.
//
// Role (docs/c5-comms-offload.md): terminate WiFi and WebSocket here, relay
// whole SlopSync frames to the S3 over UART, and keep the hub itself on the S3
// where PSRAM is. The point is not to move the hub; it is to get WiFi, BLE and
// AsyncTCP OFF the motion MCU so its ISR latency stops corrupting pulse output.
//
// Framing is SPEC §13.5: COBS-encoded frame + 0x00 delimiter, using the
// CANONICAL codec (slopsync/wire/serial_cobs.hpp). Nothing here reimplements it.
//
// One UART carries up to kSlots independent sessions, so the encoded payload is
// [slot_id][slopsync frame] — the slot byte rides INSIDE the COBS envelope so
// the 0x00 delimiter discipline stays intact.
//
// STATUS: bench bring-up. The UART peer is not yet wired; TX is exercised and
// measured, RX is implemented but unproven against a real S3.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_http_server.h>
#include <HTTPClient.h>
#include <Update.h>
#include <unistd.h>   // close() — close_fn takes ownership of the fd
#include <lwip/sockets.h>
#include <lwip/tcp.h>

#include <span>

#include "comms/BridgeProtocol.h"          // ONE vocabulary, two ends (T20)
#include "slopsync/wire/crc32.hpp"
#include "slopsync/wire/serial_cobs.hpp"
#include "slopsync/wire/estop_frame.hpp"   // SPEC §13.5 raw ESTOP scan

#if __has_include(<secrets.h>)
  #include <secrets.h>
#else
  #include <secrets.example.h>
#endif

static constexpr uint8_t PIN_LED    = 27;
static constexpr uint8_t LED_BRIGHT = 40;
// PCB-verified link to the S3 (operator, 2026-08-01):
//   S3 TX (D1/GPIO43) -> C5 RX (IO12)
//   C5 TX (IO11)      -> S3 RX (D0/GPIO44)
// NOT 7/8 — those are src/c5_waveshare's PIN_RELAY_TX/RX for the ESP-NOW relay
// node, a different board and a different link. Using them measured zero raw
// bytes in both directions across two S3 pin orders.
static constexpr int8_t  PIN_S3_TX  = 11;
static constexpr int8_t  PIN_S3_RX  = 12;

// §13.1 serial row: max_frame 512.
// 4 Mbaud. The old 4-Mbaud decode-error verdict was the S3's dead RX ISR
// (transport.md T33), not the link. Matches kUartLinkBaud in
// SlopSyncUartTransport.h; both ends must agree or the link is down.
static constexpr uint32_t UART_BAUD   = 4000000;
static constexpr size_t   kMaxFrame   = 512;
static constexpr size_t   kSlots      = bridge::kSlots;
// Worst-case COBS expansion is 1 byte per 254 (serial_cobs.hpp:30-33), over
// [slot][frame], plus the delimiter.
static constexpr size_t   kCobsMax    = (1 + kMaxFrame) + (1 + kMaxFrame) / 254 + 1 + 1;

static httpd_handle_t g_srv    = nullptr;   // :82 — SlopSync WebSocket
static httpd_handle_t g_http80 = nullptr;   // :80 — /probe, /uitoken, OTA
static int  g_wsFd[kSlots];          // httpd session fds, -1 = free
static bool g_wsUsed[kSlots];

// The slot table is written by the httpd task (claim in wsHandler, free in
// onSockClose) and read by the Arduino loop task (pumpFromS3). Guard EVERY
// touch, and hold the guard ACROSS the send, not just across the lookup:
// onSockClose does `close(sockfd)`, which returns the fd number to the OS, and
// with lru_purge_enable a new connection reuses it immediately. A check-then-
// send without the lock writes a raw SlopSync frame into whatever socket
// inherited that number. The benign form of the same race just increments
// g_rxDrops; the malignant form corrupts an unrelated session.
// RECURSIVE because a failing httpd_ws_send_frame_async can trigger a session
// close on the calling task, which re-enters onSockClose.
static SemaphoreHandle_t g_slotMx = nullptr;
struct SlotLock {
    SlotLock()  { if (g_slotMx) xSemaphoreTakeRecursive(g_slotMx, portMAX_DELAY); }
    ~SlotLock() { if (g_slotMx) xSemaphoreGiveRecursive(g_slotMx); }
    SlotLock(const SlotLock&) = delete;
    SlotLock& operator=(const SlotLock&) = delete;
    SlotLock(SlotLock&&) = delete;
    SlotLock& operator=(SlotLock&&) = delete;
};

// ---- Diagnostics ------------------------------------------------------------
struct Stage { const char* name; uint32_t freeB, largestB; };
static Stage   g_stages[12];
static uint8_t g_stageCount = 0;
static void report(const char* s) {
    if (g_stageCount >= 12) return;
    g_stages[g_stageCount++] = {s, (uint32_t)ESP.getFreeHeap(), (uint32_t)ESP.getMaxAllocHeap()};
}

static volatile uint32_t g_framesToS3 = 0, g_framesFromS3 = 0;
static volatile uint32_t g_rxDrops    = 0, g_txDrops      = 0;
static volatile uint32_t g_bytesToS3  = 0;
static volatile uint32_t g_estopSeen  = 0;   // SPEC §13.5 raw-scan hits

// ---- UART TX ----------------------------------------------------------------
// MUST NOT block (ITransport §9/§13.1). HardwareSerial::write() blocks when the
// TX buffer is full, so free space is checked FIRST and the frame is refused
// rather than stalling the HTTP task.
static bool sendToS3(uint8_t slot, const uint8_t* frame, size_t len) {
    if (len == 0 || len > kMaxFrame) return false;

    // STATIC, not locals: this runs deep inside the httpd task's 4096-byte
    // stack, under wsHandler's own 512-byte frame buffer. ~1 KB of extra
    // locals down there is not free. Safe because esp_http_server serves from
    // a single task, so there is exactly one producer.
    static uint8_t src[1 + kMaxFrame];
    src[0] = slot;
    memcpy(src + 1, frame, len);

    static uint8_t enc[kCobsMax];
    size_t n = slopsync::cobsEncode(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(src), len + 1),
        std::span<std::byte>(reinterpret_cast<std::byte*>(enc), sizeof(enc)));
    if (n == 0) { g_txDrops++; return false; }   // dst too small
    enc[n++] = 0x00;                             // delimiter is the caller's job

    if ((size_t)Serial1.availableForWrite() < n) { g_txDrops++; return false; }
    Serial1.write(enc, n);
    g_framesToS3++;
    g_bytesToS3 += n;
    return true;
}

// ---- UART RX ----------------------------------------------------------------
// Delimiter-split here, COBS-decode in the library. Runs on the Arduino loop
// task (not an ISR), which keeps it clear of the PSRAM/cache-window trap.
static uint8_t g_rxAcc[kCobsMax];
static size_t  g_rxLen = 0;
// Set when an oversized run overflows the accumulator. Suppresses further
// accumulation until the NEXT delimiter, so the run resyncs cleanly instead of
// restarting mid-run and handing its own tail to cobsDecode as if it were a
// whole frame — COBS accepts many such tails, and the only remaining gates are
// a 2-byte minimum and the slot range, so garbage reaches a live session.
static bool    g_rxOverflow = false;

// ---- S3 OTA status, published by pumpFromS3, consumed by otaS3Handler -------
// Aligned 8/16/32-bit stores are atomic on the C5, and there is exactly one
// writer, so no lock. g_s3OtaStamp is the edge the waiter watches.
// The C5 is SINGLE CORE. While the OTA handler spins on the httpd task
// (priority 5) the Arduino loop task (priority 1) barely runs, so acks sat
// undrained and the sender saw ~7/s instead of hundreds. During a transfer the
// handler drains the link itself and loop() stands off -- exactly one caller of
// pumpFromS3 at any moment, so the accumulator stays single-threaded.
static volatile bool g_otaOwnsRx = false;
struct OtaRxOwner {
    OtaRxOwner()  { g_otaOwnsRx = true; }
    ~OtaRxOwner() { g_otaOwnsRx = false; }
    OtaRxOwner(const OtaRxOwner&) = delete;
    OtaRxOwner& operator=(const OtaRxOwner&) = delete;
};

static volatile uint8_t  g_s3OtaState  = bridge::kOtaIdle;
static volatile uint8_t  g_s3OtaReason = 0;
static volatile uint16_t g_s3OtaSeq    = 0;
static volatile uint32_t g_s3OtaStamp  = 0;

static void pumpFromS3() {
    // BULK read, not byte-at-a-time: read() takes the UART mutex per call, and
    // under an OTA stream this task fell far enough behind that its own acks
    // lagged, the sender read that as loss, and retransmits saturated the link
    // (26 MB for a 1.7 MB image, 2026-08-06). Same fix the S3 side already has.
    // Bounded per pass so a saturating peer cannot hold the loop task.
    static uint8_t rxChunk[512];
    size_t budget = 16384;
    while (budget != 0) {
        const int avail = Serial1.available();
        if (avail <= 0) break;
        size_t want = (size_t)avail > sizeof(rxChunk) ? sizeof(rxChunk) : (size_t)avail;
        if (want > budget) want = budget;
        const size_t got = Serial1.readBytes(rxChunk, want);
        if (got == 0) break;
        budget -= got;
        for (size_t ci = 0; ci < got; ++ci) {
        uint8_t b = rxChunk[ci];
        if (b != 0x00) {
            if (g_rxOverflow) continue;                  // wait for the delimiter
            if (g_rxLen < sizeof(g_rxAcc)) g_rxAcc[g_rxLen++] = b;
            else { g_rxOverflow = true; g_rxLen = 0; g_rxDrops++; }
            continue;
        }
        if (g_rxOverflow) { g_rxOverflow = false; g_rxLen = 0; continue; }
        if (g_rxLen == 0) continue;              // empty frame / resync

        // SPEC §13.5 MUST: run the raw four-0xE5 ESTOP scan on the bytes
        // between delimiters BEFORE (and regardless of) COBS decoding. 0xE5
        // survives COBS unchanged when the window holds no zero byte, and the
        // CRC validates any candidate — so a corrupt or unsynced stream still
        // surfaces an emergency stop. This is the one path that must survive
        // exactly the corruption that kills every other path.
        {
            auto es = slopsync::scanForEstop(
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(g_rxAcc), g_rxLen));
            if (es.found) {
                g_estopSeen++;
                SlotLock lk;   // held ACROSS the sends — see g_slotMx
                for (size_t i = 0; i < kSlots; i++) {
                    if (!g_wsUsed[i] || g_wsFd[i] < 0) continue;
                    httpd_ws_frame_t ef{};
                    ef.type    = HTTPD_WS_TYPE_BINARY;
                    ef.payload = g_rxAcc;
                    ef.len     = g_rxLen;
                    httpd_ws_send_frame_async(g_srv, g_wsFd[i], &ef);
                }
            }
        }

        uint8_t out[1 + kMaxFrame];
        auto r = slopsync::cobsDecode(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(g_rxAcc), g_rxLen),
            std::span<std::byte>(reinterpret_cast<std::byte*>(out), sizeof(out)));
        g_rxLen = 0;
        if (!r.isOk()) { g_rxDrops++; continue; }

        size_t n = r.value();
        if (n < 2) { g_rxDrops++; continue; }
        uint8_t slot = out[0];

        // Bridge control coming BACK from the S3. Only OTA status uses this
        // direction today. Cross-task: written here on the Arduino loop task,
        // read by the httpd task in otaS3Handler.
        if (slot == bridge::kSlot) {
            if (n >= 6 && out[1] == bridge::kOpOtaStatus) {
                g_s3OtaReason = out[3];
                g_s3OtaSeq    = uint16_t(out[4]) | uint16_t(uint16_t(out[5]) << 8);
                g_s3OtaState  = out[2];
                // Published LAST: the waiter spins on this changing, so every
                // field above must already be visible when it does.
                g_s3OtaStamp  = millis();
            }
            continue;
        }

        if (slot >= kSlots) { g_rxDrops++; continue; }

        {
            // Lock spans the validity check AND the send: without it
            // onSockClose can free the slot and close(fd) in between, and the
            // fd number is immediately reusable.
            SlotLock lk;
            if (!g_wsUsed[slot] || g_wsFd[slot] < 0) { g_rxDrops++; continue; }
            httpd_ws_frame_t f{};
            f.type    = HTTPD_WS_TYPE_BINARY;
            f.payload = out + 1;
            f.len     = n - 1;
            if (httpd_ws_send_frame_async(g_srv, g_wsFd[slot], &f) == ESP_OK) g_framesFromS3++;
            else g_rxDrops++;
        }
        }
    }
}

// ---- WiFi: scan and pin the STRONGEST BSSID ---------------------------------
// Port of WifiLink::_connectBest() from the S3, and it is here for the same
// reason: this is a multi-AP network sharing one SSID, and the default
// fast-scan latches the FIRST AP heard, then never roams. Measured 2026-08-01:
// the C5 sat at -72 dBm one meter from an AP because it had clung to a distant
// one, while the S3 — same room, same SSID, pinned — held -47 dBm. On the board
// that terminates every control connection, that is the difference between a
// stable WebSocket and one that keeps dropping.
//
// Keep the two implementations in step; if the S3's constants change, these
// should follow. They are duplicated rather than shared because the C5 build
// does not include config_api.h.
static constexpr uint8_t  kPinMaxAttempts      = 3;       // then unpinned fallback
static constexpr int32_t  kMinRssiLogDbm       = -90;
static constexpr uint32_t kReconnectIntervalMs = 5000;
static constexpr uint32_t kConnectTimeoutMs    = 15000;
static uint8_t g_pinFailStreak = 0;

static bool waitConnected(uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
        // Keep draining Serial1 while we wait. A bring-up can block for many
        // seconds and the S3 keeps transmitting; a stalled drain overflows the
        // 8 KB RX buffer and costs frames that have nothing to do with WiFi.
        pumpFromS3();
        delay(10);
    }
    return WiFi.status() == WL_CONNECTED;
}

static bool connectBest() {
    const char* ssid = SECRET_WIFI_SSID_5G;
    const char* pass = SECRET_WIFI_PASSWORD_5G;

    const uint32_t scanStart = millis();
    const int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
    Serial.printf("[c5_probe] WiFi scan for '%s': %d net(s) in %lums\n",
                  ssid, n, (unsigned long)(millis() - scanStart));

    int     bestIdx    = -1;
    int32_t bestRssi   = -128;
    int     candidates = 0;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) != ssid) continue;
        candidates++;
        const int32_t rssi = WiFi.RSSI(i);
        if (rssi >= kMinRssiLogDbm)
            Serial.printf("[c5_probe]   AP %s ch%d %ddBm\n",
                          WiFi.BSSIDstr(i).c_str(), (int)WiFi.channel(i), (int)rssi);
        if (rssi > bestRssi) { bestRssi = rssi; bestIdx = i; }
    }

    if (bestIdx >= 0 && g_pinFailStreak < kPinMaxAttempts) {
        uint8_t bss[6];
        memcpy(bss, WiFi.BSSID(bestIdx), sizeof(bss));
        const int32_t ch = WiFi.channel(bestIdx);
        Serial.printf("[c5_probe] pinning to %s ch%d %ddBm (best of %d candidate%s)\n",
                      WiFi.BSSIDstr(bestIdx).c_str(), (int)ch, (int)bestRssi,
                      candidates, candidates == 1 ? "" : "s");
        WiFi.scanDelete();
        WiFi.begin(ssid, pass, ch, bss);
        if (waitConnected(kConnectTimeoutMs)) { g_pinFailStreak = 0; return true; }
        g_pinFailStreak++;
        Serial.printf("[c5_probe] pinned connect failed (streak %u/%u)\n",
                      (unsigned)g_pinFailStreak, (unsigned)kPinMaxAttempts);
        return false;
    }

    WiFi.scanDelete();
    // No candidate heard, or the streak is spent: unpinned, so a pinned AP that
    // has died cannot strand the bridge forever. The next cycle re-scans.
    Serial.printf("[c5_probe] unpinned fallback (%s)\n",
                  bestIdx < 0 ? "no AP for our SSID" : "pin streak exhausted");
    WiFi.begin(ssid, pass);
    const bool ok = waitConnected(kConnectTimeoutMs);
    if (ok) g_pinFailStreak = 0;
    return ok;
}

// ---- WebSocket --------------------------------------------------------------
static int slotForFd(int fd) {
    for (size_t i = 0; i < kSlots; i++) if (g_wsUsed[i] && g_wsFd[i] == fd) return (int)i;
    return -1;
}

// Per-socket setup. TCP_NODELAY is NOT optional on a control link: without it
// Nagle holds a small frame until the peer's ACK arrives, and the peer's
// delayed-ACK timer is 40 ms. Measured 47 ms round trip flat across 8..504 B
// payloads before this — the tell was that size did not matter, so it was a
// fixed stall and not bandwidth.
static esp_err_t onSockOpen(httpd_handle_t hd, int sockfd) {
    int one = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // BOUND THE SEND. httpd_ws_send_frame_async() is a blocking socket write,
    // and pumpFromS3() calls it on the Arduino loop task once per relayed
    // frame. On a default (infinite-timeout) socket, ONE client whose receive
    // window has filled stalls that write — and with it the UART drain and
    // every OTHER client's telemetry. That is one slow peer wedging the whole
    // bridge, which is precisely the failure slopsoak's `wedge` scenario
    // exists to catch, reproduced here on the C5.
    //
    // With a bound, a wedged peer costs one timeout instead of the link: the
    // send fails, g_rxDrops counts it, and the bridge moves on. 50 ms is ~6
    // frames at the observed relay rate — long enough that a healthy client on
    // a busy channel is never cut short, short enough that a dead one cannot
    // hold the loop task.
    struct timeval snd = {.tv_sec = 0, .tv_usec = 50000};
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    (void)hd;
    return ESP_OK;
}

// Session teardown. Registered as httpd_config_t::close_fn — without it a slot
// is claimed forever and the fifth disconnect permanently wedges the bridge.
// esp_http_server does NOT notify a WS handler on close, so this is the only
// hook that exists.
static void onSockClose(httpd_handle_t hd, int sockfd) {
    int closed[kSlots];
    size_t nClosed = 0;
    {
        // The clear and the close(2) must both be inside the lock. Clearing
        // outside it would still let pumpFromS3 hold a validated fd that this
        // close() then returns to the OS for reuse.
        SlotLock lk;
        for (size_t i = 0; i < kSlots; i++) {
            if (g_wsUsed[i] && g_wsFd[i] == sockfd) {
                g_wsUsed[i] = false; g_wsFd[i] = -1;
                closed[nClosed++] = (int)i;
            }
        }
        close(sockfd);   // close_fn owns the fd once registered
    }
    // TELL THE S3, outside the lock. Without this the far side can only infer a
    // gone peer from RX SILENCE, and silence cannot distinguish "disconnected"
    // from "connected but idle" — which made it reap live sessions that SPEC
    // §11.3/RFC-042 says to RETAIN. This is the precise signal that heuristic
    // was standing in for.
    for (size_t i = 0; i < nClosed; i++) {
        const uint8_t msg[2] = {bridge::kOpSlotClosed, (uint8_t)closed[i]};
        sendToS3(bridge::kSlot, msg, sizeof(msg));
    }
    (void)hd;
}

static esp_err_t wsHandler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        // Handshake. Claim a slot; refuse AND CLOSE rather than accepting a
        // socket we cannot serve — returning ESP_FAIL alone leaves the peer
        // connected-but-silent, which is the failure users report as a bug.
        int fd = httpd_req_to_sockfd(req);
        {
            SlotLock lk;
            for (size_t i = 0; i < kSlots; i++) {
                if (!g_wsUsed[i]) { g_wsUsed[i] = true; g_wsFd[i] = fd; return ESP_OK; }
            }
        }
        httpd_sess_trigger_close(g_srv, fd);
        return ESP_FAIL;
    }

    // A FRAME WE CANNOT PARSE MUST NOT KILL THE SESSION. Returning non-OK from
    // a URI handler makes esp_http_server close the socket, and that is exactly
    // what used to happen here: .NET's ClientWebSocket sends an UNSOLICITED
    // PONG every KeepAliveInterval (default 30 s), httpd_ws_recv_frame errored
    // on it, the error propagated out of this handler, and the connection was
    // reset. Every .NET client — MultiFunPlayer included — reconnected on a
    // 30.0 s cycle forever. Measured 2026-08-02: one unsolicited PONG, instant
    // ConnectionResetError.
    //
    // RFC 6455 §5.5.3: "A Pong frame MAY be sent unsolicited. This serves as a
    // unidirectional heartbeat. A response to an unsolicited Pong frame is not
    // expected." Ignoring it is the conforming behavior; resetting never is.
    httpd_ws_frame_t f{};
    esp_err_t e = httpd_ws_recv_frame(req, &f, 0);
    if (e != ESP_OK) { g_rxDrops++; return ESP_OK; }

    // CONSUME THE PAYLOAD FOR EVERY TYPE, control frames included. A frame
    // whose bytes are not read stays in the TCP stream and desyncs the parser
    // for everything after it — which is why simply ignoring the unsolicited
    // PONG was not enough: it turned an immediate reset into a reset a dozen
    // frames later, which is harder to diagnose, not better.
    uint8_t buf[kMaxFrame];
    if (f.len > kMaxFrame) { g_rxDrops++; return ESP_OK; }
    if (f.len > 0) {
        f.payload = buf;
        e = httpd_ws_recv_frame(req, &f, f.len);
        if (e != ESP_OK) { g_rxDrops++; return ESP_OK; }
    }

    // Control frames are OURS now (handle_ws_control_frames is true on the URI
    // registration), so answer them per RFC 6455 §5.5 rather than letting an
    // unexpected one fall through as an error.
    switch (f.type) {
        case HTTPD_WS_TYPE_PONG:
            // §5.5.3: an unsolicited Pong is a unidirectional heartbeat and
            // expects no response. .NET's ClientWebSocket emits one every
            // KeepAliveInterval; discarding it is the whole conforming duty.
            return ESP_OK;
        case HTTPD_WS_TYPE_PING: {
            // §5.5.2: a Pong MUST carry the Ping's application data verbatim.
            httpd_ws_frame_t pong{};
            pong.type    = HTTPD_WS_TYPE_PONG;
            pong.payload = buf;
            pong.len     = f.len;
            httpd_ws_send_frame(req, &pong);
            return ESP_OK;
        }
        case HTTPD_WS_TYPE_CLOSE: {
            // §5.5.1: echo a Close to complete the handshake. close_fn does the
            // slot teardown and tells the S3.
            httpd_ws_frame_t bye{};
            bye.type = HTTPD_WS_TYPE_CLOSE;
            bye.len  = 0;
            httpd_ws_send_frame(req, &bye);
            return ESP_OK;
        }
        case HTTPD_WS_TYPE_BINARY:
            break;
        default:
            return ESP_OK;   // TEXT/CONTINUE are not part of this protocol
    }

    if (f.len == 0) { g_rxDrops++; return ESP_OK; }

    int slot = slotForFd(httpd_req_to_sockfd(req));
    if (slot < 0) return ESP_OK;

    // Echo mode: a frame whose first byte is 0xEE is bounced straight back.
    // Measures the WiFi round trip in isolation, without the UART peer that
    // does not exist yet. Real SlopSync type bytes never collide with 0xEE.
    if (buf[0] == 0xEE) {
        httpd_ws_frame_t e{};
        e.type = HTTPD_WS_TYPE_BINARY; e.payload = buf; e.len = f.len;
        return httpd_ws_send_frame(req, &e);
    }

    sendToS3((uint8_t)slot, buf, f.len);
    return ESP_OK;
}

// ---- Diagnostics endpoint ---------------------------------------------------
static esp_err_t probeHandler(httpd_req_t* req) {
    char j[1024];
    int used = 0;
    { SlotLock lk; for (size_t i = 0; i < kSlots; i++) if (g_wsUsed[i]) used++; }
    int p = snprintf(j, sizeof(j),
        "{\"chip\":\"%s\",\"cores\":%u,\"flash\":%u,\"psram\":%u,"
        "\"heap_total\":%u,\"free\":%u,\"min_free\":%u,\"largest\":%u,"
        "\"rssi\":%d,\"ch\":%u,\"bssid\":\"%s\",\"ws_slots_used\":%d,\"reset_reason\":%d,"
        "\"tx_frames\":%u,\"tx_bytes\":%u,\"tx_drops\":%u,"
        "\"rx_frames\":%u,\"rx_drops\":%u,\"uptime_ms\":%u,\"stages\":[",
        ESP.getChipModel(), (unsigned)ESP.getChipCores(),
        (unsigned)ESP.getFlashChipSize(), (unsigned)ESP.getPsramSize(),
        (unsigned)ESP.getHeapSize(), (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
        WiFi.RSSI(), (unsigned)WiFi.channel(), WiFi.BSSIDstr().c_str(),
        used, (int)esp_reset_reason(),
        (unsigned)g_framesToS3, (unsigned)g_bytesToS3, (unsigned)g_txDrops,
        (unsigned)g_framesFromS3, (unsigned)g_rxDrops, (unsigned)millis());
    for (uint8_t i = 0; i < g_stageCount && p < (int)sizeof(j) - 120; i++) {
        p += snprintf(j + p, sizeof(j) - p, "%s{\"n\":\"%s\",\"free\":%u,\"largest\":%u}",
                      i ? "," : "", g_stages[i].name,
                      (unsigned)g_stages[i].freeB, (unsigned)g_stages[i].largestB);
    }
    snprintf(j + p, sizeof(j) - p, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, j, HTTPD_RESP_USE_STRLEN);
}

// ---- /uitoken proxy ---------------------------------------------------------
// The bridge is the front door, so it has to serve the WHOLE front door — not
// just the socket. /uitoken is an HTTP mint on the S3 and SlopSync clients
// fetch it from the SAME host they open the WebSocket to (slopsync_probe.py
// calls mint_uitoken(args.ip); there is no separate flag). Without this a
// session through the bridge silently lands at WATCH tier and every control
// assertion correctly refuses — which reads like a transport bug and is not.
//
// SECURITY NOTE: this does not widen the trust model — /uitoken already mints
// to anything on the LAN that can reach it — but it does mean the C5 must know
// where the S3 lives. Static for now; discovery is the proper answer.
static const char* kS3Host = "192.168.1.229";

static esp_err_t uitokenHandler(httpd_req_t* req) {
    HTTPClient http;
    if (!http.begin(String("http://") + kS3Host + "/uitoken")) {
        return httpd_resp_send_500(req);
    }
    const int code = http.GET();
    if (code != 200) {
        http.end();
        httpd_resp_set_status(req, "502 Bad Gateway");
        return httpd_resp_send(req, "uitoken upstream failed", HTTPD_RESP_USE_STRLEN);
    }
    // 16 RAW bytes, not text — must pass through byte-exact.
    String body = http.getString();
    http.end();
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, body.c_str(), body.length());
}

// ---- OTA --------------------------------------------------------------------
static esp_err_t otaHandler(httpd_req_t* req) {
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) return httpd_resp_send_500(req);
    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
        if (r <= 0) { Update.abort(); return httpd_resp_send_500(req); }
        Update.write((uint8_t*)buf, r);
        remaining -= r;
    }
    if (!Update.end(true)) return httpd_resp_send_500(req);
    httpd_resp_sendstr(req, "OK");
    delay(500);
    ESP.restart();
    return ESP_OK;
}

// ---- Serial OTA forward: HTTP here -> bridge ops -> the S3's flash ----------
// RFC-057: OTA is a duty of the composite hub, not of the chip holding the
// flash. This is the ONLY auth on the path -- the S3 trusts the link and does
// not re-check, so a weak comparison here is the whole product's OTA security.

static bool otaTokenOk(httpd_req_t* req) {
    char tok[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", tok, sizeof(tok)) != ESP_OK) return false;
    // Constant time: length-independent accumulate, same as the S3's own check.
    const char* want = SECRET_OTA_PASSWORD;
    uint8_t diff = uint8_t(strlen(tok) ^ strlen(want));
    for (size_t i = 0; tok[i] && want[i]; ++i) diff |= uint8_t(tok[i] ^ want[i]);
    return diff == 0;
}

// Blocks until the S3 publishes a fresh status. Returns false on timeout.
// delay() is acceptable here: the C5 carries no motion path, and an OTA is an
// exclusive operation the operator deliberately started.
// Wait until the S3 reports state `a` or `b`, never "any status change":
// per-chunk acks queue behind the final status, so a stamp-change wait reads a
// stale kOtaWriting and misreports a VERIFIED flash as failed (2026-08-06,
// twice, both version-verified on the S3 afterward).
static bool waitS3State(uint8_t a, uint8_t b, uint32_t timeout_ms) {
    const uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        // loop() stands off for the whole handler, so this MUST drain or the
        // status it is waiting for can never arrive.
        if (g_otaOwnsRx) pumpFromS3();
        const uint8_t st = g_s3OtaState;
        if (st == a || st == b) return true;
        delay(2);
    }
    return false;
}

static esp_err_t otaS3Handler(httpd_req_t* req) {
    if (!otaTokenOk(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unauthorized\"}");
    }
    const bool isFs = (strstr(req->uri, "/fs") != nullptr);
    const uint32_t total = uint32_t(req->content_len);
    if (total == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no content-length\"}");
    }

    OtaRxOwner rxOwner;   // loop() stands off for the whole transfer
    g_s3OtaState = bridge::kOtaIdle;   // else last transfer's terminal state
                                       // satisfies the begin-wait instantly
    uint8_t begin[6] = {bridge::kOpOtaBegin,
                        uint8_t(isFs ? bridge::kOtaTargetFs : bridge::kOtaTargetApp),
                        uint8_t(total), uint8_t(total >> 8),
                        uint8_t(total >> 16), uint8_t(total >> 24)};
    sendToS3(bridge::kSlot, begin, sizeof(begin));

    // Wait for kOtaReady before streaming: nothing may be sent until the S3 has
    // accepted the transfer and reported the first chunk it wants.
    if (!waitS3State(bridge::kOtaReady, bridge::kOtaFailed, 30000) ||
        g_s3OtaState != bridge::kOtaReady) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"s3 refused begin\"}");
    }

    // RETRANSMIT RING. This link measures 68-74% on frames this size
    // (transport.md), so a 1.7 MB transfer WILL lose chunks and must survive
    // it. Holds every in-flight chunk so a gap the S3 reports can be resent
    // without rewinding the HTTP body, which is forward-only.
    // Sized 2x the window so the newest write can never clobber the oldest
    // unacked slot. STATIC: the httpd task has a 4096-byte stack.
    static uint8_t  ring[bridge::kOtaWindow * 2][bridge::kOtaChunkBytes + 3];
    static uint16_t ringLen[bridge::kOtaWindow * 2];
    const uint16_t kRing = bridge::kOtaWindow * 2;

    const uint32_t totalChunks =
        (total + bridge::kOtaChunkBytes - 1) / bridge::kOtaChunkBytes;
    uint32_t crc = slopsync::crc32Init();
    uint32_t nextNew = 0;                 // next chunk index to READ from HTTP
    uint32_t sent = 0;
    const uint32_t deadline = millis() + 300000;
    uint32_t resends = 0;
    // Above the S3's worst lazy sector erase (~80 ms), so "flash busy" is
    // never mistaken for loss -- at 15 ms every erase stall fired a full-span
    // resend (16,112 resends for 7,175 chunks, 2026-08-06). With go-back-N
    // recovery a real loss costs one interval and heals whole, so a long
    // timer is cheap; it was only ever short to fight the lockstep that
    // go-back-N removed.
    const uint32_t kResendAfterMs = 150;
    uint32_t lastProgressMs = millis();
    uint32_t lastWant = g_s3OtaSeq;

    while (g_s3OtaSeq < totalChunks) {
        if (millis() > deadline || g_s3OtaState == bridge::kOtaFailed) {
            const uint8_t ab[2] = {bridge::kOpOtaAbort, bridge::kOtaAbortHost};
            sendToS3(bridge::kSlot, ab, sizeof(ab));
            httpd_resp_set_status(req, "504 Gateway Timeout");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"s3 stalled mid-stream\"}");
        }

        // Retransmit is driven by LACK OF PROGRESS, never by "not yet acked".
        // The S3 acks on a window, so being ahead of its last ack is the normal
        // state; treating that as loss resent one chunk 109,421 times and
        // livelocked the link (2026-08-06).
        // GO-BACK-N, the WHOLE outstanding span. The S3 refuses out-of-order
        // chunks (a hole bricks the image), so it is a go-back-N receiver, and
        // a go-back-N receiver with a resend-one sender is a phase trap: one
        // lost burst locks every later new send to exactly window-ahead,
        // rejected forever, and the link degrades to one chunk per interval
        // (measured 2026-08-06, sd-6kz.1: 15.4 ms/chunk, holes at want+15).
        if (g_s3OtaSeq != lastWant) { lastWant = g_s3OtaSeq; lastProgressMs = millis(); }
        else if (nextNew > g_s3OtaSeq && millis() - lastProgressMs > kResendAfterMs) {
            for (uint32_t s = g_s3OtaSeq; s < nextNew; ++s) {
                const uint16_t idx = uint16_t(s % kRing);
                while (!sendToS3(bridge::kSlot, ring[idx], ringLen[idx])) delay(1);
                ++resends;
            }
            lastProgressMs = millis();   // one span per interval, never a flood
        }

        // Fill the window with new chunks.
        while (nextNew < totalChunks && (nextNew - g_s3OtaSeq) < bridge::kOtaWindow) {
            const uint16_t idx = uint16_t(nextNew % kRing);
            const uint32_t left = total - sent;
            const int want = int(left > bridge::kOtaChunkBytes ? bridge::kOtaChunkBytes : left);
            // FILL THE CHUNK. httpd_req_recv returns what the socket has, not
            // what was asked; a short read makes chunks smaller than
            // kOtaChunkBytes, so totalChunks under-counts and the tail of the
            // image is never sent.
            int r = 0;
            while (r < want) {
                const int got = httpd_req_recv(req, (char*)(ring[idx] + 3 + r), want - r);
                if (got <= 0) { r = got; break; }
                r += got;
            }
            if (r <= 0) {
                const uint8_t ab[2] = {bridge::kOpOtaAbort, bridge::kOtaAbortHost};
                sendToS3(bridge::kSlot, ab, sizeof(ab));
                httpd_resp_set_status(req, "400 Bad Request");
                return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"upload truncated\"}");
            }
            ring[idx][0] = bridge::kOpOtaData;
            ring[idx][1] = uint8_t(nextNew);
            ring[idx][2] = uint8_t(nextNew >> 8);
            ringLen[idx] = uint16_t(r + 3);
            // CRC covers each chunk ONCE, on read -- retransmits must not
            // re-fold the same bytes in.
            crc = slopsync::crc32Update(
                crc, std::span<const std::byte>(reinterpret_cast<const std::byte*>(ring[idx] + 3), size_t(r)));
            while (!sendToS3(bridge::kSlot, ring[idx], ringLen[idx])) delay(1);
            sent += uint32_t(r);
            ++nextNew;
        }
        pumpFromS3();   // WE own the link now; nobody else is draining it
        delay(1);
    }
    const uint32_t fin = slopsync::crc32Final(crc);
    uint8_t end[5] = {bridge::kOpOtaEnd, uint8_t(fin), uint8_t(fin >> 8),
                      uint8_t(fin >> 16), uint8_t(fin >> 24)};
    sendToS3(bridge::kSlot, end, sizeof(end));

    if (!waitS3State(bridge::kOtaDone, bridge::kOtaFailed, 30000) ||
        g_s3OtaState != bridge::kOtaDone) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"s3 flash failed\"}");
    }
    // resends is the link's loss story for this transfer: at parity with
    // chunks it means every first send was lost (see sd-6kz.1).
    char ok[96];
    snprintf(ok, sizeof(ok),
             "{\"ok\":true,\"target\":\"s3\",\"chunks\":%lu,\"resends\":%lu,\"reboot_ms\":500}",
             (unsigned long)totalChunks, (unsigned long)resends);
    return httpd_resp_sendstr(req, ok);
}

void setup() {
    Serial.begin(115200);
    delay(1500);
    // BEFORE the slot table is touched and long before either server starts —
    // SlotLock is a no-op while this is null, which would silently restore the
    // race it exists to close.
    g_slotMx = xSemaphoreCreateRecursiveMutex();
    for (size_t i = 0; i < kSlots; i++) { g_wsFd[i] = -1; g_wsUsed[i] = false; }
    // Why the last boot happened. A bridge that dies mid-stream reboots and
    // looks identical to one that was power-cycled; without this the only
    // evidence is an operator noticing the uptime went backwards.
    Serial.printf("[c5_probe] reset reason = %d\n", (int)esp_reset_reason());
    report("S0 boot");

    rgbLedWrite(PIN_LED, 0, LED_BRIGHT, 0);
    report("S1 +led");

    // Buffers MUST be sized BEFORE begin(). HardwareSerial refuses to resize a
    // running UART and only log_e()s about it, so calling these afterwards is a
    // silent no-op: TX stays at its 0 default, availableForWrite() then reports
    // the 128-byte hardware FIFO, and every frame encoding to more than that is
    // refused forever while small ones sail through. The RX default of 256 is
    // also smaller than one max encoded frame (517), so whole frames could not
    // be buffered either.
    // XTAL, not the default: at 5 Mbaud the divider's accuracy is what keeps
    // framing clean, and APB is not held stable across frequency scaling.
    Serial1.setClockSource(UART_CLK_SRC_XTAL);
    Serial1.setTxBufferSize(16384);
    Serial1.setRxBufferSize(16384);
    Serial1.begin(UART_BAUD, SERIAL_8N1, PIN_S3_RX, PIN_S3_TX);
    report("S2 +uart");

    WiFi.mode(WIFI_STA);
    // Latency, not battery — this is mains-powered. Modem sleep parks the radio
    // between beacons and adds tens of ms of jitter to a control link.
    WiFi.setSleep(false);
    // Full 11ax capability; do not let the driver negotiate down to 11n.
    esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    // OFF so every reconnect goes through connectBest() and RE-SCANS. Arduino's
    // auto-reconnect re-associates to whatever it last used, which on a
    // multi-AP SSID is exactly the wrong-AP latch the scan exists to break.
    WiFi.setAutoReconnect(false);
    report("S3 +wifi init");
    connectBest();
    report("S4 +wifi conn");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    // Port 82 and path "/" so the bridge is drop-in for the S3's own hub
    // endpoint: existing SlopSync clients (tools/slopsync_probe.py, the JS
    // client) point at a different IP and nothing else changes.
    cfg.server_port      = 82;
    cfg.ctrl_port        = 32769;   // must not collide with a default instance
    cfg.max_open_sockets = kSlots + 2;
    cfg.lru_purge_enable = true;
    cfg.close_fn         = onSockClose;   // frees the slot; see onSockClose
    cfg.open_fn          = onSockOpen;    // TCP_NODELAY; see onSockOpen
    if (httpd_start(&g_srv, &cfg) == ESP_OK) {
        // Subprotocol MUST be echoed — SlopSync clients open with
        // 'slopsync.v1' and a server that does not negotiate it is refused.
        // handle_ws_control_frames = TRUE (6th field). We answer PING/PONG/CLOSE
        // ourselves in wsHandler — see the RFC 6455 §5.5 switch there. Leaving
        // it false let an unsolicited PONG reach httpd_ws_recv_frame as an
        // error, and the error propagated out of the handler and reset the
        // socket, which is what broke every .NET client on a 30 s cycle.
        httpd_uri_t ws{"/", HTTP_GET, wsHandler, nullptr, true, true, "slopsync.v1"};
        httpd_register_uri_handler(g_srv, &ws);
    }

    // SECOND instance on port 80. The split is NOT cosmetic: the S3 serves its
    // hub socket on 82 and its HTTP surface on 80, and SlopSync clients mint
    // /uitoken over plain HTTP on port 80 regardless of the WS port
    // (slopsync_probe.py's mint_uitoken() builds "http://<ip>/uitoken" with no
    // port). Serving everything on 82 left port 80 dead, the mint silently
    // failed, and every session landed at WATCH tier — which reads as a
    // transport fault and is not. Mirror the S3's layout exactly.
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.server_port      = 80;
    http.ctrl_port        = 32770;   // distinct from the :82 instance
    http.max_open_sockets = 4;
    http.lru_purge_enable = true;
    if (httpd_start(&g_http80, &http) == ESP_OK) {
        httpd_uri_t pr{"/probe", HTTP_GET, probeHandler, nullptr, false, false, nullptr};
        httpd_register_uri_handler(g_http80, &pr);
        httpd_uri_t ut{"/uitoken", HTTP_GET, uitokenHandler, nullptr, false, false, nullptr};
        httpd_register_uri_handler(g_http80, &ut);
        httpd_uri_t ota{"/api/ota", HTTP_POST, otaHandler, nullptr, false, false, nullptr};
        httpd_register_uri_handler(g_http80, &ota);
        // Forwarders: the C5 authenticates, the S3 writes its own flash
        // (RFC-057). /api/ota above still flashes the C5 itself.
        httpd_uri_t s3a{"/api/ota/s3", HTTP_POST, otaS3Handler, nullptr, false, false, nullptr};
        httpd_register_uri_handler(g_http80, &s3a);
        httpd_uri_t s3f{"/api/ota/s3/fs", HTTP_POST, otaS3Handler, nullptr, false, false, nullptr};
        httpd_register_uri_handler(g_http80, &s3f);
    }
    report("S5 +httpd+ws");

    for (uint8_t i = 0; i < g_stageCount; i++) {
        Serial.printf("[%-14s] free=%7u largest=%7u\n",
                      g_stages[i].name, (unsigned)g_stages[i].freeB,
                      (unsigned)g_stages[i].largestB);
        Serial.flush(); delay(15);
    }
    if (WiFi.status() == WL_CONNECTED) {
        wifi_ap_record_t ap{};
        esp_wifi_sta_get_ap_info(&ap);
        Serial.printf("ip=%s ch=%u band=%s 11ax=%d rssi=%d\n",
                      WiFi.localIP().toString().c_str(), (unsigned)ap.primary,
                      ap.primary >= 36 ? "5GHz" : "2.4GHz", ap.phy_11ax, WiFi.RSSI());
    }
    Serial.flush();
}

void loop() {
    if (!g_otaOwnsRx) pumpFromS3();

    // Supervised reconnect. There was NO reconnect path here at all — the loop
    // only blinked an LED at the link state — so a dropped association was
    // left to Arduino's auto-reconnect, which is now off. Every cycle re-scans
    // and re-pins, so a roam lands on the strongest AP rather than the
    // remembered one.
    static uint32_t lastReconnect = 0;
    if (WiFi.status() != WL_CONNECTED &&
        millis() - lastReconnect >= kReconnectIntervalMs) {
        lastReconnect = millis();
        connectBest();
    }

    static uint32_t last = 0;
    if (millis() - last >= 2000) {
        last = millis();
        static bool on = false;
        on = !on;
        bool linked = (WiFi.status() == WL_CONNECTED);
        rgbLedWrite(PIN_LED, linked ? 0 : LED_BRIGHT,
                    (linked && on) ? LED_BRIGHT : 0, 0);
    }
    delay(1);
}
