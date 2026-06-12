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

// ---- APPLOG macro (same as main.cpp) ---------------------------------------
#if SERIAL_CONTROL_MODE
  #define APPLOG(s)      applog(s)
  #define APPLOGF(...)   applogf(__VA_ARGS__)
#else
  #define APPLOG(s)      Serial.println(s)
  #define APPLOGF(...)   Serial.printf(__VA_ARGS__)
#endif

TransportManager::TransportManager(SystemState&        state,
                                   TCodeParser&        parser,
                                   SerialTransport&    serial,
                                   WebSocketTransport& ws,
                                   BleTransport&       ble)
    : _state(state), _parser(parser), _serial(serial), _ws(ws), _ble(ble) {}

// ---- WiFi + mDNS -----------------------------------------------------------

bool TransportManager::setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoConnect(true);
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
    // Remove old response hooks.
    _serial.removeResponseHooks();
    _ws.removeResponseHooks();
    _ble.removeResponseHooks();

    _state.setTransport(mode);

    if (mode == TransportMode::BT) {
        _ble.begin();
        _ws.disconnectIntiface();
        _ble.installResponseHooks();
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
        case TransportMode::SER: return "SER";
        case TransportMode::BT:  return "BT";
        default:                 return "WS";
    }
}