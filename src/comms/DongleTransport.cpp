// DongleTransport — hardware UART bridge to the T-Dongle C5
//
// The dongle is basically a slutty little USB-to-UART bridge with a pretty
// display. MFP pumps TCode into its USB hole, and it squirts it straight out
// its TX pin into our RX. We drain it greedily every poll() tick, line-buffer
// it, and feed complete frames to the TCodeParser. Same logic as SerialTransport
// but on Serial2 instead of the USB CDC. The S3 stays on WiFi, the dongle gets
// the USB port, everyone gets what they want. yippie! :3

#include "DongleTransport.h"
#include "config_api.h"
#include "AppLog.h"

DongleTransport::DongleTransport(TCodeParser& parser)
    : _parser(parser) {}

void DongleTransport::begin() {
    if (_open) return;  // idempotent — don't double-open. :3
    // Serial2 on the ESP32-S3: TX=pin8, RX=pin9 (or swap the wires if wrong).
    // The dongle's UART output is 3.3V logic — direct connection, no level
    // shifting needed. Just a wire from dongle TX to S3 RX and vice versa. :3
    Serial2.begin(DONGLE_UART_BAUD, SERIAL_8N1, DONGLE_UART_RX_PIN, DONGLE_UART_TX_PIN);
    _open   = true;
    _active = false;
    _len    = 0;
    APPLOG("[Dongle] Serial2 open — waiting for TCode from the dongle. :3");
}

void DongleTransport::end() {
    if (!_open) return;
    Serial2.end();
    _open   = false;
    _active = false;
    _len    = 0;
    APPLOG("[Dongle] Serial2 closed. :3");
}

void DongleTransport::poll() {
    if (!_open) return;

    // Cap bytes drained per poll() — same ceiling as SerialTransport.
    // At 115200 baud and 2ms poll cadence we expect ~23 bytes max; 256 handles
    // burst catch-up without spinning forever on line noise. :3
    static const int MAX_BYTES_PER_POLL = 256;
    int drained = 0;

    while (Serial2.available() > 0 && drained < MAX_BYTES_PER_POLL) {
        int raw = Serial2.read();
        drained++;

        // Skip null bytes — line noise / glitch artifact. :3
        if (raw <= 0) continue;
        char c = (char)raw;

        if (c == '\n' || c == '\r') {
            if (_len > 0) {
                _buf[_len] = '\0';
                _active  = true;
                _last_ms = millis();
                _parser.feedLine(_buf, _len);
                _len = 0;
            }
        } else if (c >= 0x20 && c < 0x7F) {
            // Only printable ASCII — TCode is pure ASCII. :3
            if (_len < (uint8_t)(sizeof(_buf) - 1)) {
                _buf[_len++] = c;
            } else {
                // Buffer overrun — flush and resync on next newline. :3
                _len = 0;
            }
        } else {
            // Non-printable mid-frame = line noise — flush partial frame. :3
            _len = 0;
        }
    }

    // Clear "active" flag if the stream has gone quiet for >2 s.
    if (_active && (millis() - _last_ms > 2000)) {
        _active = false;
        _parser.triggerStop();  // stream went quiet — stop the motor. :3
    }
}

void DongleTransport::installResponseHooks() {
    _parser.onResponse(_txResponse);
}

void DongleTransport::removeResponseHooks() {
    _parser.onResponse(nullptr);
}

// static
void DongleTransport::_txResponse(const char* msg) {
    // Squirt D0/D1/D2 replies back over Serial2 to the dongle, which forwards
    // them to MFP over USB CDC. The dongle is just a transparent relay. :3
    Serial2.print(msg);
}
