// WifiLink — WiFi STA bring-up, link telemetry, scan-and-pin reconnect.
//
// Constraints:
// - delay() here is permitted only under the init-exception (DOCTRINE.md §2):
//   boot setup() or a supervised reconnect cycle while the link is down,
//   never the real-time motion path.
// - Reconnection is driven manually (WiFi.setAutoReconnect(false)) so every
//   cycle re-scans and re-pins the strongest AP; Arduino's own auto-reconnect
//   would re-associate to the last pinned BSSID without ever re-scanning.

#include "WifiLink.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <string.h>   // memcpy for BSSID pin

#include "config_api.h"
#include "sloplog/sloplog.h"
#include "ConfigStore.h"
#include "slopsync/generated/registry_constants.hpp"

// ---- WiFi + mDNS ------------------------------------------------------------

// Bounded blocking wait for association (see file-header constraint). Split
// into 500ms poll slices.
bool WifiLink::_waitConnected(uint32_t timeoutMs) {
    uint32_t waited = 0;
    while (WiFi.status() != WL_CONNECTED && waited < timeoutMs) {
        delay(500);
        waited += 500;
    }
    return WiFi.status() == WL_CONNECTED;
}

// Attempt a single SSID/password unpinned (Arduino fast-scan). Used for the
// NVS-secondary recovery creds — the target network is unknown, so pinning a
// scanned BSSID buys nothing there.
bool WifiLink::_connectWith(const char* ssid, const char* pass, uint32_t timeoutMs) {
    if (!ssid || ssid[0] == '\0') return false;
    SLOGI("transport", "Connecting to WiFi: %s", ssid);
    WiFi.begin(ssid, pass);
    return _waitConnected(timeoutMs);
}

// Full scan → strongest-BSSID pin. The ESP32 fast-scan latches the
// first-heard AP and never roams, so this enumerates EVERY AP for our SSID,
// applogs each candidate, and pins WiFi.begin() to the one with the best
// RSSI. Fallback: after WIFI_PIN_MAX_ATTEMPTS consecutive pinned failures (or
// if no candidate is heard at all) it drops to an unpinned begin() so a dead
// pinned AP can't strand the rig; the next cycle re-scans and re-pins.
// _pinFailStreak persists across bring-up cycles.
bool WifiLink::_connectBest(const char* ssid, const char* pass, uint32_t timeoutMs) {
    if (!ssid || ssid[0] == '\0') return false;

#if WIFI_SCAN_PIN_ENABLED
    // Synchronous full scan (all channels, no hidden). ~2-3s accepted boot
    // cost; the duration is applogged so it's visible.
    uint32_t scanStart = millis();
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
    uint32_t scanMs = millis() - scanStart;
    SLOGI("transport", "WiFi scan for '%s': %d net(s) seen in %lums", ssid, n, (unsigned long)scanMs);

    int     bestIdx  = -1;
    int32_t bestRssi = -128;
    int     candidates = 0;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) != ssid) continue;
        candidates++;
        int32_t  rssi = WiFi.RSSI(i);
        uint8_t* b    = WiFi.BSSID(i);
        char bs[18];
        snprintf(bs, sizeof(bs), "%02X:%02X:%02X:%02X:%02X:%02X",
                 b[0], b[1], b[2], b[3], b[4], b[5]);
        // Log every candidate whose RSSI clears WIFI_MIN_RSSI_LOG_DBM.
        if (rssi >= WIFI_MIN_RSSI_LOG_DBM)
            SLOGD("transport", "  AP %s ch%d %ddBm", bs, (int)WiFi.channel(i), (int)rssi);
        if (rssi > bestRssi) { bestRssi = rssi; bestIdx = i; }
    }

    bool pin = (bestIdx >= 0) && (_pinFailStreak < WIFI_PIN_MAX_ATTEMPTS);
    if (pin) {
        uint8_t bss[6];
        memcpy(bss, WiFi.BSSID(bestIdx), sizeof(bss));
        int32_t ch = WiFi.channel(bestIdx);
        char bs[18];
        snprintf(bs, sizeof(bs), "%02X:%02X:%02X:%02X:%02X:%02X",
                 bss[0], bss[1], bss[2], bss[3], bss[4], bss[5]);
        SLOGI("transport", "Pinning WiFi to strongest AP %s ch%d %ddBm (best of %d candidate%s)",
              bs, (int)ch, (int)bestRssi, candidates, candidates == 1 ? "" : "s");
        WiFi.scanDelete();
        WiFi.begin(ssid, pass, ch, bss);
        if (_waitConnected(timeoutMs)) {
            _pinFailStreak = 0;
            return true;
        }
        _pinFailStreak++;
        SLOGW("transport", "Pinned connect to %s failed (streak %u/%u)",
              bs, _pinFailStreak, (unsigned)WIFI_PIN_MAX_ATTEMPTS);
        return false;
    }

    // Fallback branch: no candidate heard, or the pin streak is exhausted.
    WiFi.scanDelete();
    if (bestIdx < 0)
        SLOGW("transport", "WiFi scan saw no '%s' AP — unpinned begin() fallback", ssid);
    else
        SLOGW("transport", "Pin streak hit %u — unpinned begin() fallback so a dead AP can't strand us",
              (unsigned)WIFI_PIN_MAX_ATTEMPTS);
#endif  // WIFI_SCAN_PIN_ENABLED

    WiFi.begin(ssid, pass);
    bool ok = _waitConnected(timeoutMs);
    if (ok) _pinFailStreak = 0;   // link restored — clear the streak for next time
    return ok;
}

bool WifiLink::setupWiFi() {
    WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
        onWifiEvent(event, info);
    });

    WiFi.mode(WIFI_STA);
    // See file-header constraint: manual reconnection (superviseWifi) so every
    // cycle re-scans and re-pins, instead of Arduino auto-reconnect silently
    // re-associating to a possibly weak or dead pinned BSSID.
    WiFi.setAutoReconnect(false);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    // Stage 1 — compile-time primary creds (secrets.h): full scan + strongest-AP
    // pin. 10s associate window on top of the ~2-3s scan.
    bool connected = _connectBest(WIFI_SSID, WIFI_PASSWORD, WIFI_CONNECT_TIMEOUT_MS);

    // Stage 2 — serial-settable secondary creds from NVS. Only if the primary
    // failed. This is the recovery path for a rig on an unknown network: set
    // creds over USB with `WIFI <ssid> <pass>` and reboot. 10s window.
    if (!connected) {
        char ssid2[33], pass2[65];
        if (ConfigStore::loadWifiCreds(ssid2, sizeof(ssid2), pass2, sizeof(pass2))) {
            SLOGW("transport", "Primary WiFi failed — trying NVS secondary creds");
            WiFi.disconnect(true, true);
            delay(100);
            connected = _connectWith(ssid2, pass2, WIFI_CONNECT_TIMEOUT_MS);
        }
    }

    if (connected) {
        SLOGI("transport", "WiFi connected! IP: %s", WiFi.localIP().toString().c_str());

        if (MDNS.begin(MDNSServiceName)) {
            MDNS.addService("http", "tcp", HTTP_PORT);
            // Advertising a port that refuses connections is worse than
            // advertising nothing — a discovering client would find it, dial
            // it, and fail, with the device itself as the source of the bad
            // address.
            MDNS.addService("slopsync", "tcp", SLOPSYNC_WS_PORT);
            MDNS.addServiceTxt("slopsync", "tcp", "proto", slopsync::limits::ws_subprotocol.data());
            MDNS.addServiceTxt("slopsync", "tcp", "fw", FIRMWARE_VERSION);
            SLOGI("transport", "mDNS: http://%s.local:%d, slopsync ws :%d",
                  MDNSServiceName, HTTP_PORT, SLOPSYNC_WS_PORT);
        }

        _state.wifi_ready = true;
        _wifiEnabled = true;   // arm the reconnect supervisor
        pollWifiLink();
        return true;
    } else {
        // Both credential sets failed. Stop the STA radio so it isn't burning
        // cycles endlessly retrying a network that isn't there. There is NO
        // fallback control plane: serial is boot-log/rescue-OTA only
        // (config_api.h), so the device runs headless until WiFi returns.
        SLOGW("transport", "WiFi connection failed (primary + secondary) — no control plane until WiFi returns");
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        _state.wifi_ready = false;
        return false;
    }
}

// ---- WiFi link telemetry ----------------------------------------------------

void WifiLink::onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            _state.wifi_reconnects++;
            _state.wifi_last_disconnect_reason = info.wifi_sta_disconnected.reason;
            _state.wifi_last_disconnect_ms = millis();
            _state.wifi_ready = false;
            SLOGW("transport", "WiFi disconnected (reason=%u, total drops=%lu)",
                  _state.wifi_last_disconnect_reason,
                    (unsigned long)_state.wifi_reconnects);
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            _state.wifi_ready = true;
            pollWifiLink();
            break;
        default:
            break;
    }
}

void WifiLink::pollWifiLink() {
    if (WiFi.status() != WL_CONNECTED) return;
    _state.wifi_rssi    = (int8_t)WiFi.RSSI();
    _state.wifi_channel = (uint8_t)WiFi.channel();
    uint8_t* bssid = WiFi.BSSID();
    if (bssid) {
        snprintf(_state.wifi_bssid, sizeof(_state.wifi_bssid),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    }
}

// ---- WiFi reconnect supervisor ----------------------------------------------

void WifiLink::superviseWifi() {
    if (!_wifiEnabled) return;                      // WiFi never came up at boot
    if (WiFi.status() == WL_CONNECTED) return;      // link healthy — nothing to do

    uint32_t now = millis();
    if (now < _nextReconnectMs) return;             // rate-limit while down
    _nextReconnectMs = now + WIFI_RECONNECT_INTERVAL_MS;

    SLOGW("transport", "WiFi link down — re-scanning to re-pin strongest AP");
    if (_connectBest(WIFI_SSID, WIFI_PASSWORD, WIFI_CONNECT_TIMEOUT_MS)) {
        SLOGI("transport", "WiFi reconnected! IP: %s", WiFi.localIP().toString().c_str());
        _state.wifi_ready = true;
        pollWifiLink();
    }
}
