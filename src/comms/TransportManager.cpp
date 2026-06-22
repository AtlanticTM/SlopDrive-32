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

TransportManager::TransportManager(SystemState&        state,
                                   TCodeParser&        parser,
                                   SerialTransport&    serial,
                                   WebSocketTransport& ws,
                                   BleTransport&       ble,
                                   DongleTransport&    dongle)
    : _state(state), _parser(parser), _serial(serial), _ws(ws), _ble(ble), _dongle(dongle) {}

// ---- WiFi + mDNS -----------------------------------------------------------

bool TransportManager::setupWiFi() {
    WiFi.mode(WIFI_STA);
    // WiFi.setAutoConnect() was removed in arduino-esp32 3.x — setAutoReconnect()
    // is the surviving equivalent. The S3 reconnects automatically on drop. :3
    WiFi.setAutoReconnect(true);

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
        return true;
    } else {
        APPLOG("WiFi connection failed!");
        _state.wifi_ready = false;
        return false;
    }
}

// ---- Transport selection ---------------------------------------------------

void TransportManager::applyTransport(TransportMode mode) {
    // Rip out the old transport's tongue from the parser's mouth — we're about
    // to plug a different pipe in. Each transport slobbers its own response hook
    // into the parser, and two at once means crossed streams and garbled D0/D1/D2
    // replies. Clean the palate, then let the new hose fill it. :3
    _serial.removeResponseHooks();
    _ws.removeResponseHooks();
    _ble.removeResponseHooks();
    _dongle.removeResponseHooks();

    // Close dongle UART if we're switching away from DONGLE mode — free the
    // pins so they can be used for other things. :3
    if (mode != TransportMode::DONGLE && _dongle.isOpen()) {
        _dongle.end();
    }

    _state.setTransport(mode);

    if (mode == TransportMode::BT) {
        _ble.begin();
        _ws.disconnectIntiface();
        _ble.installResponseHooks();
    } else if (mode == TransportMode::DONGLE) {
        // DONGLE: open Serial2 and listen for TCode relayed from the T-Dongle C5.
        // WiFi stays up for the web UI — best of both holes. :3
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

    const char* name = transportName(mode);
    APPLOGF("Transport mode: %s", name);
}

// static
const char* TransportManager::transportName(TransportMode m) {
    switch (m) {
        case TransportMode::SER:    return "SER";
        case TransportMode::BT:     return "BT";
        case TransportMode::DONGLE: return "DONGLE";
        default:                    return "WS";
    }
}

// Delegate straight to DongleTransport::isActive() — true when the UART has
// received at least one valid TCode frame in the last 2s. The WebUI uses this
// to show Hz in the indicator instead of just "DONGLE". :3
bool TransportManager::isDongleActive() const {
    return _dongle.isActive();
}
