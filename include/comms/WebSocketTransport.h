// WebSocket TCode transport — SlopDrive-32
//
// Two roles in one class:
//   1. WS SERVER (port 55555) — MultiFunPlayer / generic clients connect TO us
//      and stream raw TCode.  We parse every TEXT frame.
//   2. WSDM CLIENT — we connect OUT to Intiface's Device WebSocket Server,
//      send a JSON identification handshake, then exchange TCode over the
//      resulting binary (or text) frames.
//
// The server is always listening (cheap).  The WSDM client is started/stopped
// by connectIntiface() / disconnectIntiface().  Both feed a TCodeParser.
//
// D0/D1/D2 responses are sent to the WS server client.  In WSDM mode the
// Intiface side doesn't typically query; if it does, the response hook can
// also be pointed at the _client.

#ifndef WEBSOCKET_TRANSPORT_H
#define WEBSOCKET_TRANSPORT_H

#include <Arduino.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>
#include "TCodeParser.h"

class WebSocketTransport {
public:
    explicit WebSocketTransport(TCodeParser& parser);

    /// Start the local WS server on the given port.  Call once at boot.
    void begin(uint16_t port = 55555);

    /// Service both the WS server and (if connected) the WSDM client.
    /// Call frequently from the comms task.
    void run();

    // ---- Server status ----
    /// True if at least one client (MFP) is connected to the local WS server.
    bool isServerConnected() const { return _srv_connected; }

    // ---- WSDM client ----
    void connectIntiface(const char* host, uint16_t port);
    void disconnectIntiface();
    bool isIntifaceConnected() const { return _intiface_connected; }

    /// Combined: any WebSocket transport is live.
    bool isConnected() const { return _srv_connected || _intiface_connected; }

    /// Register this transport's response hook on the parser so D0/D1/D2 go
    /// back to the MFP / server client.  Call when WS becomes active transport.
    void installResponseHooks();

    /// Remove the response hook.
    void removeResponseHooks();

    /// Send a D0/D1/D2 reply to the MFP / server client.  Public so the
    /// static response-hook thunk in the .cpp can call it.
    void sendServerResponse(const char* msg);

    // ---- Raw frame counter (maintained externally by TCodeParser) ----
    // Read from parser.rxFrameCount for the combined inbound rate.

private:
    TCodeParser& _parser;

    WebSocketsServer _ws;
    bool _srv_connected = false;
    int8_t _client_idx  = -1;

    WebSocketsClient _client;
    bool _intiface_enabled    = false;
    bool _intiface_connected  = false;
    bool _intiface_handshaked = false;

    void onWSEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
    void onIntifaceEvent(WStype_t type, uint8_t* payload, size_t length);
    void sendIntifaceHandshake();
};

#endif // WEBSOCKET_TRANSPORT_H