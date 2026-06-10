// TCode v0.3 WebSocket Server - SlopDrive-32
//
// Intiface Central connects to THIS WebSocket server (ESP32 is the server) using
// its built-in "TCode v0.3" device protocol. In the Intiface UI you add a
// websocket device, choose protocol "TCode v0.3", and point it at this ESP32:
//   ws://<esp32-ip>:55555
//
// Intiface then streams raw TCode v0.3 strings over the socket - there is NO
// Buttplug JSON handshake. We just parse the TCode and drive the motor.
//
// TCode v0.3 linear format (see github.com/multiaxis/TCode-Specification):
//   L<ch><mag>          Linear, channel, magnitude 0.<mag> (variable digits!)
//                         L277        -> 0.77
//                         L0500       -> 0.500
//   L<ch><mag>I<ms>     ...ramp to 0.<mag> over <ms> milliseconds
//                         L0500I1000  -> ramp to 0.500 over 1000ms
//   L<ch><mag>S<rate>   ...ramp to 0.<mag> at <rate> units per 100ms
//   DSTOP               Stop all axes
//
// We ALSO connect out to Intiface's WSDM device server as a client (see config).

//
// Multiple commands may be space-separated on one line.
#ifndef BUTTPLUG_H
#define BUTTPLUG_H

#include <Arduino.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>
#include "config.h"


// Callback: called when a valid L0 (linear axis 0) command is received
// position: 0.0 - 1.0 (normalized)
// duration_ms: time to reach position in ms (0 = use speed setting)
typedef void (*LinearCmdCallback)(float position, uint32_t duration_ms);
typedef void (*StopCallback)();

class ButtplugServer {
public:
    ButtplugServer();

    void begin(uint16_t port = BUTTPLUG_WEBSOCKET_PORT);
    void run();

    // Serial control mode: Intiface connects to the ESP32 over USB serial (its
    // "serialport" comm manager) and streams TCode just like the WebSocket
    // transport, but WITHOUT WiFi latency/jitter. Call pollSerial() frequently;
    // it assembles complete newline-terminated TCode lines from `Serial` and
    // feeds them into the same parser. The USB Serial port is therefore
    // dedicated to TCode - debug output goes to the in-memory log (applog).
    void pollSerial();
    bool isSerialActive() const { return _serial_active; }


    // True if EITHER the local server (MFP) OR the Intiface WSDM client is up.
    bool isConnected() const { return _connected || _intiface_connected; }
    bool isIntifaceConnected() const { return _intiface_connected; }

    // Raw WebSocket frame counter (incremented BEFORE any parsing/motor work).
    // Lets us measure the true inbound rate independent of motor processing,
    // to prove whether a low command rate is app-side or firmware-side.
    volatile uint32_t rxFrameCount = 0;


    // ---- Intiface WSDM client ----
    // Connect out to Intiface's Device WebSocket Server. host/port come from
    // the Intiface log line "Listening on: 0.0.0.0:<port>".
    void connectIntiface(const char* host, uint16_t port);
    void disconnectIntiface();
    bool intifaceEnabled() const { return _intiface_enabled; }

    // Register callbacks
    void onLinearCmd(LinearCmdCallback cb)  { _onLinearCmd = cb; }
    void onStop(StopCallback cb)            { _onStop = cb; }

    // Legacy compatibility aliases
    void onLinearRampTo(LinearCmdCallback cb) { _onLinearCmd = cb; }
    void onLinearStop(StopCallback cb)        { _onStop = cb; }

private:
    // Local server: MultiFunPlayer etc. connect TO us and stream raw TCode.
    WebSocketsServer _ws;
    bool _connected = false;
    int8_t _client_idx = -1;

    // Intiface WSDM client: WE connect TO Intiface and send the JSON handshake.
    WebSocketsClient _client;
    bool _intiface_enabled = false;
    bool _intiface_connected = false;   // websocket up
    bool _intiface_handshaked = false;  // JSON identification sent

    // Serial control mode state.
    bool     _serial_active = false;       // a TCode line was seen recently
    uint32_t _serial_last_ms = 0;          // last serial TCode arrival
    char     _serial_buf[128];             // line assembly buffer
    uint8_t  _serial_len = 0;              // bytes currently in _serial_buf

    LinearCmdCallback _onLinearCmd = nullptr;
    StopCallback      _onStop      = nullptr;

    void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
    void onIntifaceEvent(WStype_t type, uint8_t* payload, size_t length);
    void sendIntifaceHandshake();
    void parseTCode(const char* str, size_t len);
    void sendResponse(const char* msg);  // reply to a server client (MFP)
};

#endif // BUTTPLUG_H
