// USB Serial TCode transport — implementation

#include "SerialTransport.h"
#include "config_api.h"

SerialTransport::SerialTransport(TCodeParser& parser)
    : _parser(parser) {}

void SerialTransport::poll() {
    // The USB-Serial/JTAG bridge is a golden hose — the host (MultiFunPlayer /
    // Intiface) pumps a thick stream of TCode straight down the pipe into our
    // parser's waiting mouth. We drain it greedily every tick. :3
    //
    // HARD-WON LESSON (the "cleanup" broke this twice): do NOT gate this read on
    // any liveness check. A prior version gated on Serial.availableForWrite()>0,
    // but that reports TX BUFFER SPACE, not host presence — with the device
    // sitting quiet (normal: the host does all the talking) it reads 0 even while
    // the host is balls-deep pumping commands at us, so poll() bailed and NEVER
    // drained RX. No connection ever detected, whole thing went limp. uhoh.
    //
    // We don't need ANY gate: draining the RX FIFO is always safe whether a host
    // is attached or not (no bytes → the loop simply doesn't run). On the stable
    // USB-Serial/JTAG endpoint there's no DTR teardown to race, so we just read
    // unconditionally, exactly like the rock-solid main branch. Clean and greedy.
    while (Serial.available() > 0) {
        char c = (char)Serial.read();

        if (c == '\n' || c == '\r') {
            if (_len > 0) {
                _buf[_len] = '\0';
                _active = true;
                _linked = true;
                _last_ms = millis();
                _parser.feedLine(_buf, _len);
                _len = 0;
            }
        } else {
            if (_len < sizeof(_buf) - 1) {
                _buf[_len++] = c;
            } else {
                _len = 0;  // overrun: resync on next newline
            }
        }
    }

    // Clear "active" flag if the stream has gone quiet for >2 s.
    if (_active && (millis() - _last_ms > 2000)) {
        _active = false;
    }
}


void SerialTransport::installResponseHooks() {
    _parser.onResponse(_txResponse);
}

void SerialTransport::removeResponseHooks() {
    _parser.onResponse(nullptr);
}

// static
void SerialTransport::_txResponse(const char* msg) {
    // We're on the USB-Serial/JTAG bridge (see platformio.ini — no CDC flags),
    // which is stable across host open/close, so we can squirt the D0/D1/D2
    // reply straight back down the hose unconditionally, exactly like the
    // rock-solid main branch. No fragile liveness gymnastics needed here. :3
    Serial.print(msg);  // msg already ends with \n
}


