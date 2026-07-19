// Transport Manager — implementation
//
// Extracted verbatim from main.cpp: setupWiFi(), applyTransport(),
// transportName() (Step 8).

#include "TransportManager.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <string.h>   // memcpy for BSSID pin

#include "config_api.h"
#include "AppLog.h"
#include "ConfigStore.h"
#include "TCodeParser.h"
#include "SerialTransport.h"
#include "WebSocketTransport.h"
#include "BleTransport.h"
#include "DongleTransport.h"
#include "OssmBleService.h"

TransportManager::TransportManager(SystemState&        state,
                                   TCodeParser&        parser,
                                   SerialTransport&    serial,
                                   WebSocketTransport& ws,
                                   BleTransport&       ble,
                                   DongleTransport&    dongle,
                                   OssmBleService&     ossm)
    : _state(state), _parser(parser), _serial(serial), _ws(ws), _ble(ble), _dongle(dongle), _ossm(ossm) {}

// ---- WiFi + mDNS -----------------------------------------------------------

// Bounded blocking wait for association. The delay() here is permitted under
// the init-exception rule — this runs only during boot setup() or a supervised
// reconnect cycle (when the link is already down and there's nothing to service
// on this task anyway), never on the real-time motion path. Split into 500ms
// poll slices. :3
bool TransportManager::_waitConnected(uint32_t timeoutMs) {
    uint32_t waited = 0;
    while (WiFi.status() != WL_CONNECTED && waited < timeoutMs) {
        delay(500);
        waited += 500;
    }
    return WiFi.status() == WL_CONNECTED;
}

// Attempt a single SSID/password unpinned (Arduino fast-scan). Used for the
// NVS-secondary recovery creds — the target network is unknown, so pinning a
// scanned BSSID buys nothing there. :3
bool TransportManager::_connectWith(const char* ssid, const char* pass, uint32_t timeoutMs) {
    if (!ssid || ssid[0] == '\0') return false;
    APPLOGF("Connecting to WiFi: %s", ssid);
    WiFi.begin(ssid, pass);
    return _waitConnected(timeoutMs);
}

// Full scan → strongest-BSSID pin. This is the crux of the boot-time selection
// fix: the ESP32 fast-scan latches the first-heard AP and never roams, so we
// enumerate EVERY AP for our SSID, applog each candidate, and pin WiFi.begin()
// to the one with the best RSSI. Rule-2 fallback: after WIFI_PIN_MAX_ATTEMPTS
// consecutive pinned failures (or if no candidate is heard at all) we drop to
// an unpinned begin() so a dead pinned AP can't strand the rig; the next cycle
// re-scans and re-pins. _pinFailStreak persists across bring-up cycles. :3
bool TransportManager::_connectBest(const char* ssid, const char* pass, uint32_t timeoutMs) {
    if (!ssid || ssid[0] == '\0') return false;

#if WIFI_SCAN_PIN_ENABLED
    // Synchronous full scan (all channels, no hidden). ~2-3s — accepted boot
    // cost per the task; we applog the duration so it's visible. :3
    uint32_t scanStart = millis();
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
    uint32_t scanMs = millis() - scanStart;
    APPLOGF("WiFi scan for '%s': %d net(s) seen in %lums", ssid, n, (unsigned long)scanMs);

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
        // Log every candidate strong enough to matter — this is the V1 evidence.
        if (rssi >= WIFI_MIN_RSSI_LOG_DBM)
            APPLOGF("  AP %s ch%d %ddBm", bs, (int)WiFi.channel(i), (int)rssi);
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
        APPLOGF("Pinning WiFi to strongest AP %s ch%d %ddBm (best of %d candidate%s)",
                bs, (int)ch, (int)bestRssi, candidates, candidates == 1 ? "" : "s");
        WiFi.scanDelete();
        WiFi.begin(ssid, pass, ch, bss);
        if (_waitConnected(timeoutMs)) {
            _pinFailStreak = 0;
            return true;
        }
        _pinFailStreak++;
        APPLOGF("Pinned connect to %s failed (streak %u/%u)",
                bs, _pinFailStreak, (unsigned)WIFI_PIN_MAX_ATTEMPTS);
        return false;
    }

    // Fallback branch: no candidate heard, or the pin streak is exhausted.
    WiFi.scanDelete();
    if (bestIdx < 0)
        APPLOGF("WiFi scan saw no '%s' AP — unpinned begin() fallback", ssid);
    else
        APPLOGF("Pin streak hit %u — unpinned begin() fallback so a dead AP can't strand us",
                (unsigned)WIFI_PIN_MAX_ATTEMPTS);
#endif  // WIFI_SCAN_PIN_ENABLED

    WiFi.begin(ssid, pass);
    bool ok = _waitConnected(timeoutMs);
    if (ok) _pinFailStreak = 0;   // link restored — clear the streak for next time
    return ok;
}

bool TransportManager::setupWiFi() {
    WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
        onWifiEvent(event, info);
    });

    WiFi.mode(WIFI_STA);
    // We drive reconnection ourselves (superviseWifi) so every reconnect cycle
    // re-scans and re-pins the strongest AP. The Arduino auto-reconnect would
    // just re-associate with the last pinned BSSID (possibly now the weakest, or
    // dead) and never re-scan — the exact bug this feature fixes. :3
    WiFi.setAutoReconnect(false);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    // Stage 1 — compile-time primary creds (secrets.h): full scan + strongest-AP
    // pin. 10s associate window on top of the ~2-3s scan. :3
    bool connected = _connectBest(WIFI_SSID, WIFI_PASSWORD, WIFI_CONNECT_TIMEOUT_MS);

    // Stage 2 — serial-settable secondary creds from NVS. Only if the primary
    // failed. This is the recovery path for a rig on an unknown network: set
    // creds over USB with `WIFI <ssid> <pass>` and reboot. 10s window. :3
    if (!connected) {
        char ssid2[33], pass2[65];
        if (ConfigStore::loadWifiCreds(ssid2, sizeof(ssid2), pass2, sizeof(pass2))) {
            APPLOG("Primary WiFi failed — trying NVS secondary creds");
            WiFi.disconnect(true, true);
            delay(100);
            connected = _connectWith(ssid2, pass2, WIFI_CONNECT_TIMEOUT_MS);
        }
    }

    if (connected) {
        APPLOGF("WiFi connected! IP: %s", WiFi.localIP().toString().c_str());

        if (MDNS.begin(MDNSServiceName)) {
            MDNS.addService("http", "tcp", HTTP_PORT);
            MDNS.addService("ws", "tcp", BUTTPLUG_WEBSOCKET_PORT);
            APPLOGF("mDNS: http://%s.local:%d", MDNSServiceName, HTTP_PORT);
        }

        _state.wifi_ready = true;
        _wifiEnabled = true;   // arm the reconnect supervisor
        pollWifiLink();
        return true;
    } else {
        // Both credential sets failed. Stop the STA radio so it isn't burning
        // cycles endlessly retrying a network that isn't there — the caller
        // (main.cpp) drops us to serial TCode control so the rig still runs. :3
        APPLOG("WiFi connection failed (primary + secondary) — falling back to serial TCode");
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        _state.wifi_ready = false;
        return false;
    }
}

// ---- WiFi link telemetry ---------------------------------------------------

void TransportManager::onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            _state.wifi_reconnects++;
            _state.wifi_last_disconnect_reason = info.wifi_sta_disconnected.reason;
            _state.wifi_last_disconnect_ms = millis();
            _state.wifi_ready = false;
            APPLOGF("WiFi disconnected (reason=%u, total drops=%lu)",
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

void TransportManager::pollWifiLink() {
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

// ---- WiFi reconnect supervisor ---------------------------------------------

void TransportManager::superviseWifi() {
    if (!_wifiEnabled) return;                      // WiFi never came up at boot
    if (WiFi.status() == WL_CONNECTED) return;      // link healthy — nothing to do

    uint32_t now = millis();
    if (now < _nextReconnectMs) return;             // rate-limit while down
    _nextReconnectMs = now + WIFI_RECONNECT_INTERVAL_MS;

    APPLOG("WiFi link down — re-scanning to re-pin strongest AP");
    if (_connectBest(WIFI_SSID, WIFI_PASSWORD, WIFI_CONNECT_TIMEOUT_MS)) {
        APPLOGF("WiFi reconnected! IP: %s", WiFi.localIP().toString().c_str());
        _state.wifi_ready = true;
        pollWifiLink();
    }
}

// ---- Transport selection ---------------------------------------------------

void TransportManager::applyTransport(TransportMode mode) {
    // Remove all response hooks — clean palate before new hose
    _serial.removeResponseHooks();
    _ws.removeResponseHooks();
    _ble.removeResponseHooks();
    _dongle.removeResponseHooks();

    // Close dongle UART if switching away from DONGLE
    if (mode != TransportMode::DONGLE && _dongle.isOpen()) {
        _dongle.end();
    }

    _state.setTransport(mode);

    if (mode == TransportMode::OSSM_BLE) {
        // OSSM masquerade: stop native NUS BLE, start OSSM GATT service
        if (_ble.isRunning()) _ble.stop();
        _ws.disconnectIntiface();
        _ossm.start();
    } else {
        // Stop OSSM service if switching away
        if (_ossm.isRunning()) _ossm.stop();

        if (mode == TransportMode::BT) {
            _ble.begin();
            _ws.disconnectIntiface();
            _ble.installResponseHooks();
        } else if (mode == TransportMode::DONGLE) {
            if (_ble.isRunning()) _ble.stop();
            _ws.disconnectIntiface();
            _dongle.begin();
            _dongle.installResponseHooks();
        } else {
            if (_ble.isRunning()) _ble.stop();
            if (mode == TransportMode::WS) {
#if INTIFACE_ENABLED
                if (_state.wifi_ready) _ws.connectIntiface(INTIFACE_HOST, INTIFACE_PORT);
#endif
                _ws.installResponseHooks();
            } else {  // SER
                _ws.disconnectIntiface();
                _serial.installResponseHooks();
            }
        }
    }

    const char* name = transportName(mode);
    APPLOGF("Transport mode: %s", name);
}

// static
const char* TransportManager::transportName(TransportMode m) {
    switch (m) {
        case TransportMode::SER:     return "SER";
        case TransportMode::BT:      return "BT";
        case TransportMode::DONGLE:  return "DONGLE";
        case TransportMode::OSSM_BLE: return "OSSM";
        default:                     return "WS";
    }
}

bool TransportManager::isDongleActive() const {
    return _dongle.isActive();
}