#pragma once

// ============================================================================
// SlopSyncUdpDiscovery — the WS-side discovery responder (SPEC §13.8,
// RFC-046 item 5): a LAN client with no BLE broadcasts DISCOVER_PROBE
// (0x1E) on registry `udp_discovery.port` (21328); this answers with a
// unicast DISCOVER_REPLY (0x1F), rate-limited per source address.
//
// ─── RAW LWIP SOCKET, POLLED FROM THE HUB TASK — NOT AsyncUDP ──────────────
// AsyncUDP.h is bundled with the arduino-esp32 core (framework-
// arduinoespressif32/libraries/AsyncUDP) but is NOT resolved by this
// project's LDF chain-mode scan the way WiFi/FS/Preferences are (confirmed:
// `pio run -e sd32-ota` fails "AsyncUDP.h: No such file or directory" even
// though the header exists on disk under the framework package — the LDF's
// own "check our library registry" message means it genuinely never found a
// local library providing it, not a mere include-path gap). Rather than
// wire in another framework-package -I/-lib_deps special case, this uses the
// POSIX/lwIP socket API directly (sys/socket.h et al.) — always linked into
// any WiFi-capable build in this codebase already, zero new build_flags.
//
// This also SIMPLIFIES the threading story versus the originally-planned
// AsyncUDP callback: poll() runs ONCE PER HUB-TASK TICK (called from
// SlopSyncHubService::taskLoop(), a non-blocking recvfrom() bounded to a
// handful of datagrams per call, exactly like this file's sibling drain*()
// functions), so there is no foreign-task callback here at all — TRAPS T5's
// "callbacks run on the library's own task" simply does not apply, because
// there is no callback. The identity snapshot is still write-once at
// begin() and the one live field (pairing_window_open/ws_available) is still
// a plain member updated by the SAME task that reads it in poll() — no
// cross-task synchronization needed either way.
// ============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "SlopSyncDiscoveryWire.h"

namespace slopdrive {

// A minimal fixed-capacity "one reply per source IP per second" limiter
// (§13.8: "so a probe storm cannot load the hub"). No heap: a small ring of
// (ip, lastReplyMs) slots, linear-scanned — N is tiny (a handful of
// concurrent discoverers on one LAN), so this is not a hot path.
class DiscoveryRateLimiter {
public:
    static constexpr size_t kCapacity = 8;

    // Returns true iff `ip` (host-order u32) may be answered right now, and
    // records that it was. False = still inside the window from a prior
    // reply; the caller drops the probe silently (§13.8 gives probes no
    // NACK/error path — throttling is just not replying).
    bool allow(uint32_t ip, uint32_t nowMs, uint32_t windowMs) {
        for (auto& e : _entries) {
            if (e.used && e.ip == ip) {
                if (uint32_t(nowMs - e.lastMs) < windowMs) return false;
                e.lastMs = nowMs;
                return true;
            }
        }
        for (auto& e : _entries) {
            if (!e.used) {
                e.used = true;
                e.ip = ip;
                e.lastMs = nowMs;
                return true;
            }
        }
        Entry& victim = _entries[_nextEvict];
        _nextEvict = (_nextEvict + 1) % kCapacity;
        victim.ip = ip;
        victim.lastMs = nowMs;
        victim.used = true;
        return true;
    }

private:
    struct Entry {
        uint32_t ip = 0;
        uint32_t lastMs = 0;
        bool used = false;
    };
    std::array<Entry, kCapacity> _entries{};
    size_t _nextEvict = 0;
};

class SlopSyncUdpDiscovery {
public:
    // Stores the (write-once) identity snapshot and binds the UDP socket on
    // discovery::kPort. Call once from setup(), after the WS port and the
    // hub's catalog/identity are already up. `hubName`/`fwVersion` follow
    // slopsync::Hub::setIdentity's own contract (rodata/static storage,
    // since only a pointer is kept — NOT copied).
    bool begin(const char* hubName, uint64_t hubInstanceId, uint16_t wsPort, const char* fwVersion,
               std::span<const std::byte> catalogEtag);

    // Non-blocking: drains at most a few pending probes and replies to each.
    // Call every hub-task tick (SlopSyncHubService::taskLoop()) — costs one
    // recvfrom() syscall (EAGAIN, immediate return) when nothing is pending.
    void poll();

    // The two fields that change live. Same-task writer/reader (see the
    // class doc) — no synchronization needed, but kept as the obvious single
    // setter pair for symmetry with SlopSyncBlePort::updateAdvertising.
    void setPairingWindowOpen(bool open) { _pairingOpen = open; }
    void setWsAvailable(bool available) { _wsAvailable = available; }

    uint32_t repliesSent() const { return _repliesSent; }
    uint32_t probesRejected() const { return _probesRejected; }
    uint32_t probesThrottled() const { return _probesThrottled; }

private:
    int _sock = -1;  // POSIX/lwIP socket fd, -1 = not bound
    DiscoveryRateLimiter _rateLimiter;

    // ---- write-once identity snapshot (see class doc) ----------------------
    const char* _hubName = "";
    uint64_t _hubInstanceId = 0;
    uint16_t _wsPort = 0;
    const char* _fwVersion = "";
    std::array<std::byte, discovery::kEtagBytes> _catalogEtag{};

    // ---- the two live fields -------------------------------------------------
    bool _pairingOpen = false;
    bool _wsAvailable = true;

    // ---- diagnostics ---------------------------------------------------------
    uint32_t _repliesSent = 0;
    uint32_t _probesRejected = 0;
    uint32_t _probesThrottled = 0;
};

}  // namespace slopdrive
