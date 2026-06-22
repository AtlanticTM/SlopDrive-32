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
    // DISCONNECT CRASH FIX (the "MFP yanks the cord" bug):
    //
    // When MFP closes the COM port, the ESP32-S3 USB-CDC layer does one of two
    // things depending on the host OS and driver:
    //   a) It floods the RX FIFO with 0x00 bytes (null storm) — these are not
    //      newlines, so they fill _buf until the overrun guard fires, reset _len
    //      to 0, then the next 0x00 fills it again. This tight loop starves the
    //      FreeRTOS watchdog and triggers a task WDT reset. owo
    //   b) It delivers a partial frame (no trailing \n) — _buf holds a half-eaten
    //      TCode command that never gets dispatched, but _len stays > 0. On the
    //      next reconnect the stale partial gets prepended to the first real frame,
    //      producing garbage that crashes feedLine(). uhoh.
    //
    // Fix: detect DTR drop (Serial.available() returns -1 or Serial is not
    // connected) and flush _buf. Also: cap the per-poll byte budget so a null
    // storm can't spin here for more than MAX_BYTES_PER_POLL bytes before
    // yielding back to the scheduler. And: skip 0x00 bytes entirely — they are
    // never valid TCode and only appear during disconnect/reconnect glitches. :3
    //
    // DTR detection: Serial.available() returns -1 when the USB-CDC host has
    // disconnected on ESP32 Arduino. We use this as the disconnect signal.
    // On reconnect it returns >= 0 again. :3
    if (!Serial) {
        // Host disconnected — flush any partial frame so the next reconnect
        // starts clean. Issue a DSTOP so the motor doesn't keep running. :3
        if (_len > 0 || _active) {
            _len    = 0;
            _active = false;
            _linked = false;
            _parser.triggerStop();
        }
        return;
    }

    // Cap bytes drained per poll() call. At 115200 baud that's ~11520 bytes/s;
    // at 2ms poll cadence we expect at most ~23 bytes. 256 is a generous ceiling
    // that handles burst catch-up without spinning forever on a null storm. :3
    static const int MAX_BYTES_PER_POLL = 256;
    int drained = 0;

    while (Serial.available() > 0 && drained < MAX_BYTES_PER_POLL) {
        int raw = Serial.read();
        drained++;

        // Skip null bytes — they are never valid TCode and only appear during
        // USB-CDC disconnect/reconnect glitches. Letting them through fills _buf
        // with garbage that crashes feedLine(). :3
        if (raw <= 0) continue;
        char c = (char)raw;

        if (c == '\n' || c == '\r') {
            if (_len > 0) {
                _buf[_len] = '\0';
                _active = true;
                _linked = true;
                _last_ms = millis();
                _parser.feedLine(_buf, _len);
                _len = 0;
            }
        } else if (c >= 0x20 && c < 0x7F) {
            // Only accept printable ASCII — TCode is pure ASCII. Any byte outside
            // this range during a live stream is line noise or a disconnect glitch.
            // Silently drop it; if it's mid-frame, the overrun guard below will
            // resync on the next newline. :3
            if (_len < sizeof(_buf) - 1) {
                _buf[_len++] = c;
            } else {
                // Buffer overrun — the frame is longer than any valid TCode line.
                // Flush and resync. The next newline starts a fresh frame. :3
                _len = 0;
            }
        } else {
            // Non-printable, non-newline byte mid-frame = line noise / glitch.
            // Flush the partial frame so we don't feed garbage to feedLine(). :3
            _len = 0;
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


