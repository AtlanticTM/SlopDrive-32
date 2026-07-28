// WifiLink — SlopDrive-32 WiFi STA bring-up, link telemetry, and the
// scan-and-pin reconnect supervisor. mDNS registration rides setupWiFi().
// SlopSync is the only control plane; this class owns only the radio link.

#ifndef WIFI_LINK_H
#define WIFI_LINK_H

#include <Arduino.h>
#include <WiFi.h>   // arduino_event_id_t / arduino_event_info_t for the onEvent hook
#include "config_api.h"
#include "SystemState.h"

class WifiLink {
public:
    explicit WifiLink(SystemState& state) : _state(state) {}

    /// Bring up WiFi in STA mode + mDNS.  Safe to call once at boot.
    /// Returns true if connected.
    bool setupWiFi();

    /// Poll RSSI/channel/BSSID into SystemState. Cheap — call from a Core-0
    /// 1 s cadence. Plain scalar writes, same core reads them back.
    void pollWifiLink();

    /// Supervise the link on the reconnect-from-disconnected path, same 1 s
    /// cadence. No-op while connected (or while WiFi never came up). When the
    /// link is down: full scan + strongest-AP pin, rate-limited to
    /// WIFI_RECONNECT_INTERVAL_MS. We own reconnection deliberately —
    /// WiFi.setAutoReconnect() would re-associate with the last (possibly
    /// now-weakest/dead) pinned BSSID and never re-scan.
    void superviseWifi();

private:
    // WiFi event handler — bound via the std::function onEvent overload.
    void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info);

    // Single SSID/password attempt with a bounded, boot-only blocking wait
    // (init-exception rule). Used for the NVS-secondary recovery creds.
    bool _connectWith(const char* ssid, const char* pass, uint32_t timeoutMs);

    // Full-scan the SSID, pin to the strongest BSSID, bounded wait. Falls
    // back to an unpinned begin() after WIFI_PIN_MAX_ATTEMPTS failures.
    bool _connectBest(const char* ssid, const char* pass, uint32_t timeoutMs);

    // Bounded blocking wait for WL_CONNECTED (500 ms slices; boot/reconnect
    // only, never the real-time path).
    bool _waitConnected(uint32_t timeoutMs);

    bool     _wifiEnabled     = false;  // gates superviseWifi()
    uint8_t  _pinFailStreak   = 0;      // consecutive pinned-connect failures
    uint32_t _nextReconnectMs = 0;      // supervisor rate limit

    SystemState& _state;
};

#endif // WIFI_LINK_H
