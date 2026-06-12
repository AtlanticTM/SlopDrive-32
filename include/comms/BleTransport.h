// Bluetooth LE TCode transport — SlopDrive-32
//
// Exposes a Nordic-UART-style (NUS) BLE service.  A BLE central writes raw
// TCode bytes to the RX characteristic; we assemble complete newline-terminated
// lines and feed them into a TCodeParser.  TCode replies (D0/D1/D2) are sent
// back as notifications on the TX characteristic.
//
// Guard the entire implementation behind BLE_ENABLED in the .cpp (and put
// empty stubs for non-BLE builds) so this file can be included unconditionally
// but NimBLE is only linked when requested.

#ifndef BLE_TRANSPORT_H
#define BLE_TRANSPORT_H

#include <Arduino.h>
#include "TCodeParser.h"

// Forward declarations for NimBLE types so the header stays light.
class NimBLEServer;
class NimBLECharacteristic;

class BleTransport {
public:
    explicit BleTransport(TCodeParser& parser);

    /// Start advertising the Nordic-UART BLE service.  No-op if already running
    /// or if BLE is compiled out.
    void begin();

    /// Stop advertising and shut down the BLE stack.  Safe to call any time.
    void stop();

    bool isRunning()   const { return _running; }
    bool isConnected() const { return _connected; }
    /// Sticky link latch (first TCode byte ever seen over BLE).
    bool isLinked()    const { return _linked; }

    /// Register this transport's response hook on the parser so D0/D1/D2
    /// replies go out as BLE TX notifications.  Call when BT becomes active.
    void installResponseHooks();

    /// Remove the response hook.
    void removeResponseHooks();

    /// Send a D0/D1/D2 reply as a BLE TX notification.  Public so the static
    /// response-hook thunk in the .cpp can call it.
    void _sendResponse(const char* msg);

    // ---- Internals exposed for NimBLE callback shims (see .cpp) ------------
    // These are called from the shim classes; do NOT call from application code.
    void _onConnect();
    void _onDisconnect();
    void _feedBytes(const uint8_t* data, size_t len);

private:
    TCodeParser& _parser;

    bool _running   = false;
    bool _connected = false;
    bool _linked    = false;

    NimBLEServer*         _server   = nullptr;
    NimBLECharacteristic* _tx_char  = nullptr;  // notify (device -> host)

    char    _buf[128];
    uint8_t _len = 0;
};

#endif // BLE_TRANSPORT_H