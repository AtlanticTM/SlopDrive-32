// DongleTransport — hardware UART bridge to the T-Dongle C5
//
// The T-Dongle C5 sits between MFP and the S3: MFP talks to the dongle's USB
// CDC port, the dongle relays TCode over a physical UART wire to the S3's
// Serial2 (pins 8/9). This transport reads that UART and feeds the TCodeParser
// exactly like SerialTransport does for USB — same line-buffering, same null-
// byte filtering, same disconnect detection. The S3 stays on WiFi for the web
// UI while MFP gets a dedicated USB port on the dongle. Best of both holes. :3
//
// The dongle's display shows the live position bar and Hz readout so you can
// watch the data stream pump in and out while the machine pounds away. hehee :3

#ifndef DONGLE_TRANSPORT_H
#define DONGLE_TRANSPORT_H

#include <Arduino.h>
#include "TCodeParser.h"

// ============================================================================
// DongleTransport — Serial2 UART receiver for T-Dongle C5 relay
// ============================================================================

class DongleTransport {
public:
    explicit DongleTransport(TCodeParser& parser);

    // ---- Lifecycle -----------------------------------------------------------
    /// Open Serial2 on the configured pins and baud rate. Call once at boot
    /// when DONGLE mode is selected. Safe to call multiple times (idempotent). :3
    void begin();

    /// Close Serial2 and release the UART. Call when switching away from DONGLE
    /// mode so the pins are free for other use. :3
    void end();

    // ---- Runtime -------------------------------------------------------------
    /// Drain Serial2 RX, line-buffer, and feed complete lines to the parser.
    /// Call frequently from commsTask — same contract as SerialTransport::poll().
    void poll();

    // ---- Status --------------------------------------------------------------
    /// True if at least one valid TCode frame has been received since begin()
    /// and the stream has not gone quiet for >2 s. :3
    bool isActive() const { return _active; }

    /// True if the UART has been opened. :3
    bool isOpen() const { return _open; }

    // ---- Response hook -------------------------------------------------------
    /// Install this transport's response hook on the parser so D0/D1/D2 replies
    /// go back to the dongle (and from there to MFP). :3
    void installResponseHooks();
    void removeResponseHooks();

private:
    TCodeParser& _parser;

    // Line accumulation buffer — same size as SerialTransport. :3
    char    _buf[256];
    uint8_t _len      = 0;
    bool    _active   = false;
    bool    _open     = false;
    uint32_t _last_ms = 0;

    // Static TX callback — sends D0/D1/D2 replies back over Serial2. :3
    static void _txResponse(const char* msg);
};

#endif // DONGLE_TRANSPORT_H
