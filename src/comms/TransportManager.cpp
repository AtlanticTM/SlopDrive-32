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
    // ---- Reconnect/disconnect telemetry hook — register BEFORE WiFi.begin()
    // so we don't miss the very first (re)connect cycle. Runs on the WiFi
    // event task, which is Core 0 — same core as everything else touching
    // these SystemState fields, so plain scalar writes are safe. :3
    WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
        onWifiEvent(event, info);
    });

    WiFi.mode(WIFI_STA);
    // WiFi.setAutoConnect() was removed in arduino-esp32 3.x — setAutoReconnect()
    // is the surviving equivalent. The S3 reconnects automatically on drop. :3
    WiFi.setAutoReconnect(true);

    // ---- Modem-sleep latency fix ---------------------------------------------
    // WiFi.setSleep(false) disables the ESP32's default modem-sleep power-save
    // mode. Modem sleep periodically parks the radio between DTIM beacons to
    // save power, which injects tens-to-hundreds of ms of latency on inbound
    // packets — exactly the kind of stutter reported when close to (and
    // presumably power-save-friendly with) a nearby AP. Real-time TCode motion
    // wants the radio awake and listening continuously. :3
    WiFi.setSleep(false);
    // Push TX power to the ceiling — cheap insurance against marginal RSSI on
    // whichever AP band-steering lands us on. :3
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
        pollWifiLink();   // seed RSSI/channel/BSSID immediately, don't wait 1s
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
            // Every drop, expected or not, gets counted — this is the evidence
            // trail that proves whether the machine is actually roaming/
            // dropping near a "close" AP (band-steering / power-save flapping)
            // vs. just quietly reconnecting once at boot. :3
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
