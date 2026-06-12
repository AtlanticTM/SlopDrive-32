// USB Serial TCode transport — implementation

#include "SerialTransport.h"
#include "config_api.h"

SerialTransport::SerialTransport(TCodeParser& parser)
    : _parser(parser) {}

void SerialTransport::poll() {
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
    Serial.print(msg);  // msg already ends with \n
}