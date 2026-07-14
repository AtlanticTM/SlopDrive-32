// Transport Manager — implementation
//
// Extracted verbatim from main.cpp: setupWiFi(), applyTransport(),
// transportName() (Step 8).

#include "TransportManager.h"

#include <WiFi.h>
#include <ESPmDNS.h>

#include "config_api.h"
#include "AppLog.h"
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

bool TransportManager::setupWiFi() {
    WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
        onWifiEvent(event, info);
    });

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    APPLOGF("Connecting to WiFi: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        APPLOGF("WiFi connected! IP: %s", WiFi.localIP().toString().c_str());

        if (MDNS.begin(MDNSServiceName)) {
            MDNS.addService("http", "tcp", HTTP_PORT);
            MDNS.addService("ws", "tcp", BUTTPLUG_WEBSOCKET_PORT);
            APPLOGF("mDNS: http://%s.local:%d", MDNSServiceName, HTTP_PORT);
        }

        _state.wifi_ready = true;
        pollWifiLink();
        return true;
    } else {
        APPLOG("WiFi connection failed!");
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