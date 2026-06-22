// Transport Manager — SlopDrive-32
//
// Owns "exactly one transport live" logic, WiFi bring-up, and mDNS.
// Takes a SystemState& reference to read/write transport mode.
//
// applyTransport(TransportMode) brings up the chosen path and tears down
// the others.  It also installs the correct response hooks on the TCodeParser
// so D0/D1/D2 replies go to the right transport.
//
// WiFi setup (setupWiFi) is co-located here because it's a prerequisite for
// the WS transport.

#ifndef TRANSPORT_MANAGER_H
#define TRANSPORT_MANAGER_H

#include <Arduino.h>
#include "config_api.h"
#include "SystemState.h"

// Forward declarations
class TCodeParser;
class SerialTransport;
class WebSocketTransport;
class BleTransport;
class DongleTransport;

class TransportManager {
public:
    TransportManager(SystemState&        state,
                     TCodeParser&        parser,
                     SerialTransport&    serial,
                     WebSocketTransport& ws,
                     BleTransport&       ble,
                     DongleTransport&    dongle);

    /// Bring up WiFi in STA mode + mDNS.  Safe to call once at boot.
    /// Returns true if connected.
    bool setupWiFi();

    /// Apply the selected transport mode: bring up the chosen path and tear
    /// down the others so exactly one is live.  Safe to call repeatedly.
    /// D0/D1/D2 response hooks are installed on the active transport.
    void applyTransport(TransportMode mode);

    /// Short tag string for the status chip / API ("WS" / "SER" / "BT" / "DONGLE").
    static const char* transportName(TransportMode m);

    /// True when the DONGLE UART has received a valid TCode frame in the last 2s.
    /// Used by /api/status so the WebUI indicator can show Hz instead of "DONGLE". :3
    bool isDongleActive() const;

private:
    SystemState&        _state;
    TCodeParser&        _parser;
    SerialTransport&    _serial;
    WebSocketTransport& _ws;
    BleTransport&       _ble;
    DongleTransport&    _dongle;
};

#endif // TRANSPORT_MANAGER_H