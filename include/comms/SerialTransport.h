// USB Serial TCode transport — SlopDrive-32
//
// Reads TCode bytes from the USB Serial port, assembles complete newline-
// terminated lines, and feeds them into a TCodeParser.  D0/D1/D2 replies
// are sent back over USB Serial when SERIAL_CONTROL_MODE is active.
//
// Active/receiving indicators mirror the original ButtplugServer::pollSerial():
//   isActive()  — true for ~2s after the last TCode line; good for live UI.
//   isLinked()  — sticky latch: true once ANY TCode line is seen all session.

#ifndef SERIAL_TRANSPORT_H
#define SERIAL_TRANSPORT_H

#include <Arduino.h>
#include "TCodeParser.h"

class SerialTransport {
public:
    explicit SerialTransport(TCodeParser& parser);

    /// Drain the USB serial port and assemble TCode lines. Call frequently
    /// (~every tick of the comms task).  Lines are dispatched via parser.
    void poll();

    /// True only while TCode has arrived within the last ~2 s.
    bool isActive() const { return _active; }

    /// Sticky: true once ANY TCode line has been seen this session.
    bool isLinked() const { return _linked; }

    /// Register this transport's response hook on the parser so D0/D1/D2
    /// replies are sent to USB Serial (in serial-control mode).  Call this
    /// when this transport becomes active.
    void installResponseHooks();

    /// Remove the response hooks so this transport stops answering.
    void removeResponseHooks();

private:
    TCodeParser& _parser;
    bool     _active         = false;
    bool     _linked         = false;
    uint32_t _last_ms        = 0;
    char     _buf[128];
    uint8_t  _len            = 0;


    static void _txResponse(const char* msg);
};

#endif // SERIAL_TRANSPORT_H