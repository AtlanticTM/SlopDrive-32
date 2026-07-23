// WebSocket TCode transport — implementation
//
// Extracted verbatim from ButtplugServer in buttplug.cpp (Step 8).

#include "WebSocketTransport.h"
#include "sloplog/sloplog.h"
#include "config_api.h"

// ---- Static pointer for response-hook thunk ---------------------------------
// The ResponseCallback is a plain C function pointer, so we stash the active
// WS transport instance here when installResponseHooks() is called.
static WebSocketTransport* s_active_ws = nullptr;

static void _wsTxResponse(const char* msg) {
    if (s_active_ws) s_active_ws->sendServerResponse(msg);
}

// ---- Constructor -----------------------------------------------------------

// IMPORTANT: The 3rd constructor arg (protocol) MUST be "" (empty).
// arduinoWebSockets defaults the server subprotocol to "arduino" and echoes it
// back in the handshake's Sec-WebSocket-Protocol response header. Strict
// RFC-6455 clients like Intiface (Rust tungstenite) REJECT a handshake that
// returns a Sec-WebSocket-Protocol they did not request — exactly why Intiface
// silently failed to connect while MultiFunPlayer (a lenient client) worked.
// Passing "" stops the server from sending that header.
WebSocketTransport::WebSocketTransport(TCodeParser& parser)
    : _parser(parser)
    , _ws(BUTTPLUG_WEBSOCKET_PORT, "", "") {}

void WebSocketTransport::begin(uint16_t port) {
    _ws.begin();
    _ws.onEvent([this](uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
        this->onWSEvent(num, type, payload, length);
    });
    SLOGI("wsdm", "WebSocket server on port %u", BUTTPLUG_WEBSOCKET_PORT);
    SLOGI("wsdm", "In Intiface: add Websocket device, protocol 'TCode v0.3'");
    SLOGI("wsdm", "Address: ws://<this-esp32-ip>:%u", BUTTPLUG_WEBSOCKET_PORT);
}

void WebSocketTransport::run() {
    _ws.loop();
    if (_intiface_enabled) {
        _client.loop();  // service the Intiface WSDM client connection
    }
}

// ---- Response hooks --------------------------------------------------------

void WebSocketTransport::installResponseHooks() {
    s_active_ws = this;
    _parser.onResponse(_wsTxResponse);
}

void WebSocketTransport::removeResponseHooks() {
    if (s_active_ws == this) s_active_ws = nullptr;
    _parser.onResponse(nullptr);
}

void WebSocketTransport::sendServerResponse(const char* msg) {
    if (_client_idx < 0) return;
    _ws.sendTXT((uint8_t)_client_idx, msg);
    SLOGD("wsdm", "TX: %s", msg);  // msg already ends with \n (sink strips it)
}

// ---- WS server event handler -----------------------------------------------

void WebSocketTransport::onWSEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            SLOGI("wsdm", "Client %u disconnected", num);
            if (_client_idx == (int8_t)num) {
                _client_idx = -1;
                _srv_connected = false;
            }
            break;

        case WStype_CONNECTED: {
            IPAddress ip = _ws.remoteIP(num);
            SLOGI("wsdm", "Intiface connected from %s", ip.toString().c_str());
            _client_idx = (int8_t)num;
            _srv_connected = true;
            break;
        }

        case WStype_TEXT:
            if (length > 0) {
                _parser.feedLine((const char*)payload, length);
            }
            break;

        case WStype_PING:
        case WStype_PONG:
            break;

        default:
            break;
    }
}

// ---- WSDM client -----------------------------------------------------------

void WebSocketTransport::connectIntiface(const char* host, uint16_t port) {
    _intiface_enabled = true;
    _intiface_connected = false;
    _intiface_handshaked = false;

    SLOGI("wsdm", "Connecting to Intiface device server at ws://%s:%u", host, port);

    _client.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
        this->onIntifaceEvent(type, payload, length);
    });
    _client.begin(host, port, "/");
    _client.setReconnectInterval(5000);
    // Heartbeat keeps the TCP path warm and forces the stack to flush small
    // frames promptly. Without periodic traffic, Windows delayed-ACK + Nagle on
    // the tiny (~10 byte) TCode frames can throttle effective throughput to a
    // bursty 5-15 Hz. Ping every 2s, expect pong within 1s, drop after 2 misses.
    _client.enableHeartbeat(2000, 1000, 2);
}

void WebSocketTransport::disconnectIntiface() {
    _intiface_enabled = false;
    _intiface_connected = false;
    _intiface_handshaked = false;
    _client.disconnect();
    SLOGI("wsdm", "Intiface client disconnected");
}

void WebSocketTransport::sendIntifaceHandshake() {
    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"identifier\":\"%s\",\"address\":\"%s\",\"version\":0}",
             INTIFACE_IDENTIFIER, INTIFACE_ADDRESS);
    _client.sendTXT(msg);
    _intiface_handshaked = true;
    SLOGI("wsdm", "TX handshake: %s", msg);
}

void WebSocketTransport::onIntifaceEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            SLOGI("wsdm", "Connected to Intiface - sending handshake");
            _intiface_connected = true;
            sendIntifaceHandshake();
            break;

        case WStype_DISCONNECTED:
            SLOGI("wsdm", "Disconnected from Intiface");
            _intiface_connected = false;
            _intiface_handshaked = false;
            break;

        case WStype_TEXT:
        case WStype_BIN:
            if (length > 0) {
                _parser.feedLine((const char*)payload, length);
            }
            break;

        default:
            break;
    }
}