// Transport Manager — SlopDrive-32
//
// Owns "exactly one transport live" logic, WiFi bring-up, and mDNS.
// Takes a SystemState& reference to read/write transport mode.
//
// applyTransport(TransportMode) brings up the chosen path and tears down
// the others.  It also installs the correct response hooks on the TCodeParser
// so D0/D1/D2 replies go to the right transport.
//
// WiFi setup (setupWiFi) is co-located here because it's a prerequisite for
// the WS transport.

#ifndef TRANSPORT_MANAGER_H
#define TRANSPORT_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>   // arduino_event_id_t / arduino_event_info_t for the onEvent hook
#include "config_api.h"
#include "SystemState.h"

// Forward declarations
class TCodeParser;
class SerialTransport;
class WebSocketTransport;
class BleTransport;
class DongleTransport;
class OssmBleService;

class TransportManager {
public:
    TransportManager(SystemState&        state,
                     TCodeParser&        parser,
                     SerialTransport&    serial,
                     WebSocketTransport& ws,
                     BleTransport&       ble,
                     DongleTransport&    dongle,
                     OssmBleService&     ossm);

    /// Bring up WiFi in STA mode + mDNS.  Safe to call once at boot.
    /// Returns true if connected.
    bool setupWiFi();

    /// Apply the selected transport mode: bring up the chosen path and tear
    /// down the others so exactly one is live.  Safe to call repeatedly.
    /// D0/D1/D2 response hooks are installed on the active transport.
    void applyTransport(TransportMode mode);

    /// Short tag string for the status chip / API ("WS" / "SER" / "BT" / "DONGLE").
    static const char* transportName(TransportMode m);

    /// True when the DONGLE UART has received a valid TCode frame in the last 2s.
    /// Used by /api/status so the WebUI indicator can show Hz instead of "DONGLE". :3
    bool isDongleActive() const;

    /// Poll RSSI/channel/BSSID into SystemState. Cheap — call from a Core-0
    /// timer/task (e.g. once per second from commsTask or httpTask). Not an
    /// ISR/interrupt path, so no mutex needed — plain scalar writes, same
    /// core reads them back (WebUI::handleApiStatus). :3
    void pollWifiLink();

    /// Supervise the WiFi link on the reconnect-from-disconnected path. Call
    /// from the same Core-0 1s cadence as pollWifiLink(). No-op while connected
    /// (or while WiFi was never brought up). When the link is down it re-runs
    /// the full scan + strongest-AP pin (rate-limited to WIFI_RECONNECT_INTERVAL_MS).
    /// We own reconnection deliberately — WiFi.setAutoReconnect() would just
    /// re-associate with the last (possibly now-weakest/dead) pinned BSSID and
    /// never re-scan, which is the exact bug this feature fixes. :3
    void superviseWifi();

private:
    // ---- WiFi event handler (static — bridges to instance via _instance) ----
    // Espressif's onEvent() wants a free function or lambda without capture
    // for the raw NetworkEventCb variant; we use the std::function overload
    // instead so we can bind a member function directly. Hooked in setupWiFi().
    void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info);

    // Attempt a single SSID/password with a bounded, boot-only blocking wait.
    // Returns true on WL_CONNECTED. Used by setupWiFi() for the NVS-secondary
    // credential stage (unpinned recovery for an unknown network). :3
    bool _connectWith(const char* ssid, const char* pass, uint32_t timeoutMs);

    // Full-scan the given SSID, applog every candidate AP, pin WiFi.begin() to
    // the strongest BSSID, and wait (bounded) for association. On WIFI_PIN_MAX_ATTEMPTS
    // consecutive pinned failures — or when no candidate is seen — falls back to
    // an unpinned begin(). Returns true on WL_CONNECTED. Used for the primary
    // creds at boot and by superviseWifi() on every reconnect cycle. :3
    bool _connectBest(const char* ssid, const char* pass, uint32_t timeoutMs);

    // Bounded blocking wait for WL_CONNECTED (500ms poll slices). Boot/reconnect
    // only — never on the real-time path. Shared by _connectWith/_connectBest. :3
    bool _waitConnected(uint32_t timeoutMs);

    // Reconnect supervisor state. _wifiEnabled gates superviseWifi() so we only
    // drive reconnection when WiFi actually came up at boot (not when we gave up
    // and fell to serial TCode). _pinFailStreak counts consecutive pinned-connect
    // failures for the rule-2 unpinned fallback. _nextReconnectMs rate-limits the
    // supervisor while the link is down. :3
    bool     _wifiEnabled    = false;
    uint8_t  _pinFailStreak  = 0;
    uint32_t _nextReconnectMs = 0;

    SystemState&        _state;
    TCodeParser&        _parser;
    SerialTransport&    _serial;
    WebSocketTransport& _ws;
    BleTransport&       _ble;
    DongleTransport&    _dongle;
    OssmBleService&     _ossm;
};

#endif // TRANSPORT_MANAGER_H