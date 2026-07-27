#pragma once

// ============================================================================
// SlopSyncUiToken — GET /uitoken, RFC-029 §4.
//
// HTTP ESCAPEE #2, and the reason is not convenience. The sole-surface doctrine
// says SlopSync is the machine's only control plane and HTTP keeps only static
// assets and OTA. This endpoint is the second sanctioned exception because its
// ENTIRE security property IS the browser's same-origin policy, and that
// property cannot exist in-band: a WebSocket has no same-origin rule to lean on,
// so the same mechanism expressed over SlopSync would be no mechanism at all.
//
// THE MECHANISM, in one sentence: an evil page on another origin can SEND this
// request, but the browser will not let it READ the response — so the token
// reaches a same-origin WebUI and nothing else. That is why this handler MUST
// NOT set a single CORS header. The absence is the feature. Adding
// `Access-Control-Allow-Origin: *` here would silently hand control of the
// machine to every page on the internet; there is no softer way to say it.
//
// THE HONEST LIMIT, stated in code because a doc nobody reads is not a
// disclosure: a NATIVE process on the LAN can simply curl this and get a
// control-tier token. That is accepted. It is no worse than the status quo —
// today's LAN-trust posture already grants control to anything that can open a
// socket — and the class this closes is the browser-borne one (a random page
// you visit driving the machine in the background), which is the class that can
// actually reach a user who never opened the UI.
//
// PROPERTIES: single-use, short TTL (~60 s), rate-limited, control tier and
// NEVER configure (a browser-borne credential must not be able to re-key the
// machine's trust ledger), minted at request time and never templated into the
// static gzip bundle (a token baked into a cached asset is a permanent token).
//
// SPEED MATTERS HERE: on the default (sync WebServer) backend IdleGuardWebServer
// serves ONE connection at a time, so a slow handler is a machine-wide stall.
// Minting is an HMAC-SHA256 over a counter — microseconds. An ECDSA sign (tens
// of ms) in this handler would be a defect, which is exactly why the hub's own
// signing is deferred to a task. (Under -DUSE_PSYCHIC_HTTP the server
// multiplexes sockets so one slow handler no longer deafens the machine — but
// it still occupies the single httpd task, so the rule stands unchanged.)
//
// THREADING: mint() runs on httpTask, consume() runs on the Core-0 SlopSyncHub
// task. Neither touches slopsync::Hub, so the one-task invariant is untouched;
// the tiny slot table is protected by a portMUX spinlock held for a few
// microseconds, the same shape AppLog's web ring uses.
//
// WHERE THE SPINLOCK LIVES, AND WHY IT IS NOT A MEMBER: this object is a member
// of SlopSyncHubService, which is placement-new'd into PSRAM. A portMUX_TYPE
// MUST live in internal RAM — the spinlock is taken with a compare-and-set that
// is only defined on internal memory, and it would additionally be unreachable
// during a flash-cache-disabled window. So the mutex is a file-scope static in
// the .cpp (there is exactly one minter) while the slot table stays here in
// PSRAM, where it costs the internal heap nothing.
// ============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

class SlopHttpServer;

namespace slopdrive {

class SlopSyncUiTokenMinter {
public:
    // Seeds the per-boot HMAC secret from the hardware TRNG. Tokens therefore
    // never survive a reboot, which is correct: a reboot is a trust boundary.
    void begin();

    // Registers GET /uitoken on the shared server. Safe to skip entirely.
    void attachRoutes(SlopHttpServer* server);

    // Runtime kill switch for shared-space deployments (a machine on an open
    // network, a hackspace, a party). Off means /uitoken answers 403 and the
    // only way to control the device is an actual paired token. Persisted in
    // NVS so a deliberate lockdown survives a power cycle.
    void setEnabled(bool on);
    bool enabled() const { return _enabled; }

    // Hub-task side: is this the bytes of a live, unexpired, unused token? A
    // true return CONSUMES it (single-use is not advisory).
    bool consume(std::span<const std::byte> token);

    uint32_t minted() const { return _minted; }
    uint32_t consumed() const { return _consumed; }
    uint32_t refused() const { return _refused; }

private:
    static constexpr size_t kTokenBytes = 16;   // = slopsync limits::token_bytes
    static constexpr size_t kSlots = 4;         // a few tabs' worth, no more
    static constexpr uint32_t kTtlMs = 60000;   // RFC-029 §4: short
    static constexpr uint32_t kMinIntervalMs = 250;  // rate limit, per device

    struct Slot {
        std::array<std::byte, kTokenBytes> token{};
        uint32_t expiresMs = 0;
        bool used = true;   // an unminted slot is "already used"
    };

    void handleGet(SlopHttpServer* server);

    std::array<Slot, kSlots> _slots{};
    std::array<std::byte, 32> _secret{};
    uint32_t _counter = 0;
    uint32_t _lastMintMs = 0;
    uint32_t _minted = 0, _consumed = 0, _refused = 0;
    bool _enabled = true;
};

}  // namespace slopdrive
