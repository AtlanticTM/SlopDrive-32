#include "SlopSyncUiToken.h"

#include <Arduino.h>
#include <Preferences.h>
#include "ui/SlopHttpServer.h"
#include <esp_random.h>

#include <cstring>

#include "sloplog/sloplog.h"
#include "slopsync/core/crypto.hpp"      // constantTimeEqual via SoftwareCrypto
#include "slopsync/wire/hmac_sha256.hpp"

namespace slopdrive {

namespace {

constexpr const char* kNvsNamespace = "slopsync";
constexpr const char* kNvsKeyEnabled = "uitok_en";

// THE SPINLOCK LIVES HERE, NOT IN THE OBJECT — see the header. The minter is a
// member of SlopSyncHubService, which is placement-new'd into PSRAM, and a
// portMUX_TYPE in external RAM is not merely wasteful but WRONG: the
// compare-and-set the spinlock is taken with is defined on internal memory only,
// and external RAM additionally goes unreachable inside a flash-cache-disabled
// window. There is exactly one minter, so a file-scope mux is the honest shape.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// The comparison a token check MUST use (RFC-028.3, protocol-wide). Kept as a
// file-local instance so this module never has to reach into the hub's ICrypto
// from httpTask — SoftwareCrypto's methods are pure, so a second instance costs
// nothing and shares no state with the signing path.
//
// NAMESPACE SCOPE, NOT A FUNCTION-LOCAL STATIC — AND THAT IS LOAD-BEARING.
// This used to be `static SoftwareCrypto c;` inside an accessor, and consume()
// calls it from INSIDE portENTER_CRITICAL. A function-local static of a type
// with a non-trivial destructor is constructed on first use, and that
// construction registers the destructor through __cxa_atexit, which takes a
// recursive newlib lock, which calls abort() when interrupts are disabled.
// Result: the FIRST HELLO ever to present a live, unexpired /uitoken panicked
// the device — and only that one, because every other path through the loop
// (all-slots-stale after the reboot) skipped the call and looked healthy.
// Decoded from a real backtrace, not deduced:
//   consume():171 -> __cxa_atexit -> __register_exitproc
//                 -> __retarget_lock_acquire_recursive -> abort()
// At namespace scope the object is built during static init, before main, on a
// task with interrupts enabled — so there is nothing left to initialise lazily
// and the critical section below does pure arithmetic.
//
// THE GENERAL RULE, since this will not be the last spinlock in this codebase:
// NOTHING lazily-initialised may be touched inside portENTER_CRITICAL. That
// includes function-local statics, first-use singletons, and anything that
// might allocate, log, or take a lock. If you need one, construct it in
// begin().
slopsync::SoftwareCrypto s_cmp;

slopsync::SoftwareCrypto& cmp() { return s_cmp; }

void hexEncode(std::span<const std::byte> in, char* out) {
    static const char* kHex = "0123456789abcdef";
    size_t o = 0;
    for (std::byte b : in) {
        out[o++] = kHex[(uint8_t(b) >> 4) & 0x0F];
        out[o++] = kHex[uint8_t(b) & 0x0F];
    }
    out[o] = '\0';
}

}  // namespace

void SlopSyncUiTokenMinter::begin() {
    uint8_t seed[32] = {};
    esp_fill_random(seed, sizeof(seed));
    for (size_t i = 0; i < _secret.size(); ++i) _secret[i] = std::byte(seed[i]);
    std::memset(seed, 0, sizeof(seed));

    Preferences prefs;
    // Read-write open: see loadPairing()'s note on the first-boot NOT_FOUND
    // E-line operators misread as a boot failure.
    if (prefs.begin(kNvsNamespace, false)) {
        _enabled = prefs.getBool(kNvsKeyEnabled, true);
        prefs.end();
    }
    SLOGI("slopsync", "/uitoken %s (single-use, %u ms TTL, control tier)",
          _enabled ? "ENABLED" : "DISABLED", unsigned(kTtlMs));
}

void SlopSyncUiTokenMinter::setEnabled(bool on) {
    if (on == _enabled) return;
    _enabled = on;
    Preferences prefs;
    if (prefs.begin(kNvsNamespace, false)) {
        prefs.putBool(kNvsKeyEnabled, on);
        prefs.end();
    }
    SLOGW("slopsync", "/uitoken %s by operator", on ? "ENABLED" : "DISABLED");
}

void SlopSyncUiTokenMinter::attachRoutes(SlopHttpServer* server) {
    if (server == nullptr) return;
    server->on("/uitoken", HTTP_GET, [this, server]() { handleGet(server); });
    SLOGI("slopsync", "HTTP route: GET /uitoken (no CORS headers, by design)");
}

void SlopSyncUiTokenMinter::handleGet(SlopHttpServer* server) {
    // ---- DO NOT ADD CORS HEADERS BELOW THIS LINE ---------------------------
    // Their absence IS the security mechanism (see the header). A cross-origin
    // page may send this request; the browser must refuse to let it read the
    // answer. There is no legitimate reason for this endpoint to be readable
    // cross-origin, and no user-visible bug that adding a header would fix.
    // ------------------------------------------------------------------------
    if (!_enabled) {
        ++_refused;
        server->send(403, "application/json", "{\"ok\":false,\"error\":\"uitoken_disabled\"}");
        return;
    }

    const uint32_t now = millis();

    // ---- Pass 1 (locked, ~1 µs): rate limit + claim a counter --------------
    // Rate limit: one mint per kMinIntervalMs, device-wide. A page needs exactly
    // one token per session, so this is generous for real use and flattens the
    // "spray requests and grab whichever lands" pattern.
    uint32_t counter = 0;
    portENTER_CRITICAL(&s_mux);
    if (_lastMintMs != 0 && (now - _lastMintMs) < kMinIntervalMs) {
        portEXIT_CRITICAL(&s_mux);
        ++_refused;
        server->send(429, "application/json", "{\"ok\":false,\"error\":\"rate_limited\"}");
        return;
    }
    _lastMintMs = now;
    counter = ++_counter;
    portEXIT_CRITICAL(&s_mux);

    // ---- The HMAC runs UNLOCKED, deliberately ------------------------------
    // token = HMAC(boot secret, counter || now)[0..15]. The HMAC is what makes
    // it unguessable; the slot table is what makes it single-use and expiring.
    // Both halves are needed — a stateless token cannot be revoked on use.
    //
    // It sits outside the critical section because portENTER_CRITICAL disables
    // interrupts, and four SHA-256 block compressions is tens of microseconds —
    // long enough to be rude to the motion plane for a job that shares nothing.
    // The claimed counter makes every mint's material unique regardless of
    // interleaving, so there is nothing left to protect here.
    std::array<std::byte, 8> material{};
    for (size_t i = 0; i < 4; ++i) material[i] = std::byte((counter >> (8 * i)) & 0xFF);
    for (size_t i = 0; i < 4; ++i) material[4 + i] = std::byte((now >> (8 * i)) & 0xFF);
    auto mac = slopsync::hmacSha256(std::span<const std::byte>(_secret),
                                    std::span<const std::byte>(material));
    std::array<std::byte, kTokenBytes> tok{};
    for (size_t i = 0; i < kTokenBytes; ++i) tok[i] = mac[i];

    // ---- Pass 2 (locked, ~1 µs): install ------------------------------------
    // Oldest-expiring slot loses. Four slots is deliberately small: this is a
    // handshake credential, not a session store.
    portENTER_CRITICAL(&s_mux);
    size_t victim = 0;
    for (size_t i = 1; i < kSlots; ++i) {
        if (_slots[i].used && !_slots[victim].used) { victim = i; continue; }
        if (_slots[i].used == _slots[victim].used && _slots[i].expiresMs < _slots[victim].expiresMs)
            victim = i;
    }
    _slots[victim].token = tok;
    _slots[victim].expiresMs = now + kTtlMs;
    _slots[victim].used = false;
    ++_minted;
    portEXIT_CRITICAL(&s_mux);

    char hex[kTokenBytes * 2 + 1];
    hexEncode(std::span<const std::byte>(tok), hex);
    char body[128];
    // "tier" is stated explicitly so a client never has to guess what it got,
    // and so the ceiling is documented at the point of issue.
    snprintf(body, sizeof(body), "{\"ok\":true,\"token\":\"%s\",\"ttl_ms\":%u,\"tier\":\"control\"}",
             hex, unsigned(kTtlMs));
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", body);
}

bool SlopSyncUiTokenMinter::consume(std::span<const std::byte> token) {
    if (!_enabled || token.size() != kTokenBytes) return false;
    const uint32_t now = millis();
    bool hit = false;
    portENTER_CRITICAL(&s_mux);
    for (auto& s : _slots) {
        if (s.used) continue;
        if (int32_t(now - s.expiresMs) >= 0) {  // wrap-safe expiry compare
            s.used = true;
            continue;
        }
        if (cmp().constantTimeEqual(std::span<const std::byte>(s.token), token)) {
            s.used = true;   // single-use: consumed whether or not anything follows
            hit = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (hit) ++_consumed;
    return hit;
}

}  // namespace slopdrive
