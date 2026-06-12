// TCode v0.3 WebSocket Server Implementation - SlopDrive-32
//
// Intiface Central connects here as a WebSocket client using its built-in
// "TCode v0.3" device protocol and streams raw TCode strings. We parse them
// and drive the motor via the registered callbacks. No Buttplug JSON handshake
// is involved - TCode v0.3 is a plain-text line protocol.
//
// TCode v0.3 linear command:  L<channel><magnitude>[I<ms> | S<rate>]
//   Magnitude is VARIABLE length and means the fractional part after "0.":
//     L277        -> 0.77        (2 digits)
//     L0500       -> 0.500       (3 digits)
//     L09999      -> 0.9999      (4 digits)
//   I = ramp to position over N milliseconds   (L0500I1000)
//   S = ramp at N units per 100ms              (L020S10)
//
// Device commands (D...) are INFO QUERIES, except DSTOP:
//   D0    -> identify device & firmware (we reply "D0 SlopDrive-32 1.0")
//   D1    -> identify TCode version     (we reply "D1 TCode v0.3")
//   D2    -> list axes                  (we reply "D2 L0 0 9999 Up")
//   DSTOP -> stop motion


#include "buttplug.h"
#include "AppLog.h"
#include <NimBLEDevice.h>



// IMPORTANT: The 3rd constructor arg (protocol) MUST be "" (empty).
//
// arduinoWebSockets defaults the server subprotocol to "arduino" and echoes it
// back in the handshake's Sec-WebSocket-Protocol response header. Strict RFC-6455
// clients like Intiface (Rust tungstenite) REJECT a handshake that returns a
// Sec-WebSocket-Protocol they did not request, closing with error 1006 - which is
// exactly why Intiface silently failed to connect while MultiFunPlayer (a lenient
// client) connected fine. Passing "" stops the server from sending that header.
ButtplugServer::ButtplugServer() : _ws(BUTTPLUG_WEBSOCKET_PORT, "", "") {}

void ButtplugServer::begin(uint16_t port) {
    _ws.begin();
    _ws.onEvent([this](uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
        this->onWebSocketEvent(num, type, payload, length);
    });
    applogf("[TCode] WebSocket server on port %u", BUTTPLUG_WEBSOCKET_PORT);
    applog("[TCode] In Intiface: add Websocket device, protocol 'TCode v0.3'");
    applogf("[TCode] Address: ws://<this-esp32-ip>:%u", BUTTPLUG_WEBSOCKET_PORT);
}

void ButtplugServer::run() {
    _ws.loop();
    if (_intiface_enabled) {
        _client.loop();  // service the Intiface WSDM client connection
    }
}

// Serial control mode: read TCode from the USB serial port (Intiface's
// "serialport" comm manager) and feed complete lines into the same parser used
// for WebSocket. This bypasses WiFi entirely - no network latency/jitter, so
// the achievable command rate is limited only by the serial baud and Intiface's
// message_gap, not the wireless link. Lines are terminated by \n (and/or \r).
void ButtplugServer::pollSerial() {
    while (Serial.available() > 0) {
        char c = (char)Serial.read();

        if (c == '\n' || c == '\r') {
            if (_serial_len > 0) {
                _serial_buf[_serial_len] = '\0';
                rxFrameCount++;            // count for the rate diagnostic
                _serial_active = true;
                _serial_linked = true;     // sticky latch for the green light/toast
                _serial_last_ms = millis();

                parseTCode(_serial_buf, _serial_len);
                _serial_len = 0;
            }
        } else {
            // Accumulate into the line buffer (drop overflow defensively).
            if (_serial_len < sizeof(_serial_buf) - 1) {
                _serial_buf[_serial_len++] = c;
            } else {
                _serial_len = 0;  // overrun: resync on next newline
            }
        }
    }

    // Clear the "active" flag if the serial stream has gone quiet.
    if (_serial_active && (millis() - _serial_last_ms > 2000)) {
        _serial_active = false;
    }
}

// ---------------------------------------------------------------------------
// Intiface WSDM client
// ---------------------------------------------------------------------------
//
// Intiface's "Device WebSocket Server" (enabled via the toggle in its UI)
// LISTENS for devices. We connect to it as a CLIENT, then immediately send the
// JSON identification handshake:
//   {"identifier":"tcode-v03","address":"<unique>","version":0}
// After Intiface accepts, it streams the same TCode strings we already parse.
void ButtplugServer::connectIntiface(const char* host, uint16_t port) {
    _intiface_enabled = true;
    _intiface_connected = false;
    _intiface_handshaked = false;

    Serial.printf("[WSDM] Connecting to Intiface device server at ws://%s:%u\n", host, port);

    _client.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
        this->onIntifaceEvent(type, payload, length);
    });
    // Connect to the WSDM server root path.
    _client.begin(host, port, "/");
    // Auto-reconnect every 5s if the link drops or Intiface restarts.
    _client.setReconnectInterval(5000);
    // Heartbeat keeps the TCP path warm and forces the stack to flush small
    // frames promptly. Without periodic traffic, Windows delayed-ACK + Nagle on
    // the tiny (~10 byte) TCode frames can throttle effective throughput to a
    // bursty 5-15 Hz. Ping every 2s, expect pong within 1s, drop after 2 misses.
    _client.enableHeartbeat(2000, 1000, 2);
}

void ButtplugServer::disconnectIntiface() {
    _intiface_enabled = false;
    _intiface_connected = false;
    _intiface_handshaked = false;
    _client.disconnect();
    Serial.println("[WSDM] Intiface client disconnected");
}

void ButtplugServer::sendIntifaceHandshake() {
    // First message after the websocket opens MUST be the identification packet.
    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"identifier\":\"%s\",\"address\":\"%s\",\"version\":0}",
             INTIFACE_IDENTIFIER, INTIFACE_ADDRESS);
    _client.sendTXT(msg);
    _intiface_handshaked = true;
    Serial.printf("[WSDM] TX handshake: %s\n", msg);
}

void ButtplugServer::onIntifaceEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            Serial.println("[WSDM] Connected to Intiface - sending handshake");
            _intiface_connected = true;
            sendIntifaceHandshake();
            break;

        case WStype_DISCONNECTED:
            Serial.println("[WSDM] Disconnected from Intiface");
            _intiface_connected = false;
            _intiface_handshaked = false;
            break;

        case WStype_TEXT:
        case WStype_BIN:
            // After the handshake, Intiface's WSDM sends TCode over the socket as
            // BINARY frames (the protocol's raw bytes). For tcode-v03 those bytes
            // are simply the ASCII TCode string (e.g. "L0500I0100\n"), so we feed
            // both TEXT and BIN frames through the same parser.
            if (length > 0) {
                // Count the raw frame BEFORE parsing so we measure the true
                // inbound WebSocket rate, independent of motor processing.
                rxFrameCount++;
                // NOTE: do NOT log every frame here. Serial.printf blocks until
                // the UART drains (~2ms/line at 115200), and at streaming rates
                // that throttles the receive loop to ~10-15 Hz - exactly the
                // "low polling rate" symptom. Logging is rate-limited inside
                // parseTCode() instead so the hot path stays fast.
                parseTCode((const char*)payload, length);
            }
            break;

        default:
            break;
    }
}

// Send a plain-text TCode response back to the connected client (Intiface).
// In serial-control mode the reply must go out over the USB Serial port (that's
// where Intiface is listening); otherwise it goes to the WebSocket client.
void ButtplugServer::sendResponse(const char* msg) {
#if SERIAL_CONTROL_MODE
    if (_serial_active) {
        Serial.print(msg);  // msg already ends with \n - reply to Intiface serial
        return;
    }
#endif
    if (_client_idx < 0) return;
    _ws.sendTXT((uint8_t)_client_idx, msg);
    applogf("[TCode] TX: %s", msg);  // msg already ends with \n
}

void ButtplugServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            applogf("[TCode] Client %u disconnected", num);
            if (_client_idx == (int8_t)num) {
                _client_idx = -1;
                _connected = false;
            }
            break;

        case WStype_CONNECTED: {
            IPAddress ip = _ws.remoteIP(num);
            applogf("[TCode] Intiface connected from %s", ip.toString().c_str());
            _client_idx = (int8_t)num;
            _connected = true;
            break;
        }

        case WStype_TEXT:
            if (length > 0) {
                rxFrameCount++;  // raw inbound rate, pre-parse
                // No per-frame logging here (blocks the receive loop). See note
                // in onIntifaceEvent and the rate-limited log in parseTCode().
                parseTCode((const char*)payload, length);
            }
            break;

        case WStype_PING:
        case WStype_PONG:
            break;

        default:
            break;
    }
}

// ============================================================================
// TCode v0.3 Parser
// ============================================================================
//
// Parses one or more space/newline separated TCode commands. We only act on
// L0 (linear axis 0 = our stroke axis) and stop commands.

void ButtplugServer::parseTCode(const char* str, size_t len) {
    // Null-terminated working copy (TCode lines are short)
    char buf[256];
    size_t copy_len = min(len, sizeof(buf) - 1);
    memcpy(buf, str, copy_len);
    buf[copy_len] = '\0';

    char* token = strtok(buf, " \t\r\n");
    while (token != nullptr) {
        size_t tlen = strlen(token);
        if (tlen == 0) {
            token = strtok(nullptr, " \t\r\n");
            continue;
        }

        char axis = toupper(token[0]);

        // ---- Device commands (D...) ----
        // CRITICAL: In TCode v0.3, D0/D1/D2 are device-INFO QUERIES, not stop!
        //   D0    -> identify device & firmware  (reply with device id)
        //   D1    -> identify TCode version      (reply with TCode version)
        //   D2    -> list axes & ranges          (reply with axis list)
        //   DSTOP -> the actual stop command
        // Intiface sends D0/D1 on connect to recognize the device, so we MUST
        // answer these (and must NOT treat them as a stop).
        if (axis == 'D') {
            if (strncasecmp(token, "DSTOP", 5) == 0) {
                applog("[TCode] DSTOP - stop motion");
                if (_onStop) _onStop();
            } else if (strncasecmp(token, "D0", 2) == 0) {
                // Identify device & firmware version
                sendResponse("D0 SlopDrive-32 1.0\n");
            } else if (strncasecmp(token, "D1", 2) == 0) {
                // Identify TCode version
                sendResponse("D1 TCode v0.3\n");
            } else if (strncasecmp(token, "D2", 2) == 0) {
                // List available axes: L0 (linear), range 0..9999, enabled
                sendResponse("D2 L0 0 9999 Up\n");
            }
            token = strtok(nullptr, " \t\r\n");
            continue;
        }

        // ---- Linear / Rotation / Vibration axes (we only use L0) ----
        if (axis == 'L' || axis == 'R' || axis == 'V') {
            if (tlen < 2) {
                token = strtok(nullptr, " \t\r\n");
                continue;
            }

            uint8_t channel = token[1] - '0';

            // Only handle linear axis 0
            if (axis != 'L' || channel != 0) {
                token = strtok(nullptr, " \t\r\n");
                continue;
            }

            // --- Decode magnitude (FIXED scale, NOT digit-count based) ---
            // Intiface scales position 0.0-1.0 to the feature's value range
            // [0, TCODE_MAGNITUDE_MAX] and emits the integer WITHOUT guaranteed
            // zero-padding. e.g. position 0.086 is sent as "L086" (not "L0086").
            //
            // The old "fraction after 0." logic divided by 10^(digit count),
            // which misread unpadded values badly: "L086" became 0.86 (10x too
            // big) and the top of travel was unreachable. Dividing by a FIXED
            // maximum makes parsing independent of digit count:
            //   "L086"  -> 86  / 999 = 0.086   (correct)
            //   "L0117" -> 117 / 999 = 0.117   (correct)
            //   "L0999" -> 999 / 999 = 1.000   (full travel reachable)
            const char* p = token + 2;
            uint32_t mag_value = 0;
            int mag_digits = 0;
            while (*p && isdigit((unsigned char)*p)) {
                mag_value = mag_value * 10 + (uint32_t)(*p - '0');
                mag_digits++;
                p++;
            }

            if (mag_digits == 0) {
                // No digits (e.g. bare "L0") - ignore
                token = strtok(nullptr, " \t\r\n");
                continue;
            }

            float position = (float)mag_value / TCODE_MAGNITUDE_MAX;
            position = constrain(position, 0.0f, 1.0f);

            // --- Optional modifier: I (interval ms) or S (speed units/100ms) ---
            uint32_t duration_ms = 0;

            if (*p == 'I' || *p == 'i') {
                p++;
                duration_ms = (uint32_t)atoi(p);
            } else if (*p == 'S' || *p == 's') {
                // Speed = magnitude units per 100ms. Convert to a duration for
                // the move distance. We don't know current position here, so we
                // approximate the duration for a full-range move; the callback
                // caps actual speed to the user's configured max.
                p++;
                long rate = atol(p);
                if (rate > 0) {
                    // Full scale (1.0) at "rate" per 100ms -> ms for full range.
                    // value is in the same 0.$$ fractional units as position.
                    // duration for full 1.0 move = (1.0 / (rate/divisor)) * 100
                    // Simplify using a 0..1 scale: assume rate given as 0.$$ too.
                    // Practical: treat rate as %/100ms of full travel.
                    duration_ms = (uint32_t)(100000L / rate);  // rough estimate
                }
            }

            // Rate-limit this log to ~2/sec. Logging EVERY command blocks on the
            // UART (~2ms/line at 115200) and was throttling the receive loop to
            // ~10-15 Hz. The callback below now runs without per-command serial.
            static uint32_t s_last_log_ms = 0;
            uint32_t now_log = millis();
            if (now_log - s_last_log_ms >= 500) {
                s_last_log_ms = now_log;
                applogf("[TCode] L0: pos=%.4f dur=%ums (digits=%d)",
                        position, duration_ms, mag_digits);
            }

            if (_onLinearCmd) {
                _onLinearCmd(position, duration_ms);
            }
        }

        token = strtok(nullptr, " \t\r\n");
    }
}

// ============================================================================
// Bluetooth LE transport (Nordic UART Service)
// ============================================================================
//
// We expose a Nordic-UART-style BLE service. A BLE central (phone app, or a BLE
// host) writes raw TCode bytes to the RX characteristic; we assemble complete
// newline-terminated lines and feed them into the SAME parseTCode() pipeline as
// the WebSocket and Serial transports. TCode replies (D0/D1/D2) are sent back as
// notifications on the TX characteristic.
//
// NimBLE-Arduino is used instead of the stock Bluedroid stack because it's far
// smaller in flash/RAM, which matters alongside WiFi + the web server.

// ---- Callback shims -------------------------------------------------------
// NimBLE delivers events to subclasses of its callback interfaces. These thin
// shims just forward into the owning ButtplugServer instance.
class BleServerCallbacks : public NimBLEServerCallbacks {
public:
    explicit BleServerCallbacks(ButtplugServer* owner) : _owner(owner) {}
    void onConnect(NimBLEServer* /*srv*/) override {
        _owner->_onBleConnect();
    }
    void onDisconnect(NimBLEServer* srv) override {
        _owner->_onBleDisconnect();
        // Resume advertising so the next host can find us again.
        NimBLEDevice::startAdvertising();
    }
private:
    ButtplugServer* _owner;
};

class BleRxCallbacks : public NimBLECharacteristicCallbacks {
public:
    explicit BleRxCallbacks(ButtplugServer* owner) : _owner(owner) {}
    void onWrite(NimBLECharacteristic* chr) override {
        const std::string& v = chr->getValue();
        if (!v.empty()) {
            _owner->feedBleBytes((const uint8_t*)v.data(), v.size());
        }
    }
private:
    ButtplugServer* _owner;
};

void ButtplugServer::beginBLE() {
    if (_ble_running) return;

    NimBLEDevice::init(BLE_DEVICE_NAME);
    // Boost TX power a little for more reliable range.
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    _ble_server = NimBLEDevice::createServer();
    _ble_server->setCallbacks(new BleServerCallbacks(this));

    NimBLEService* svc = _ble_server->createService(BLE_NUS_SERVICE_UUID);

    // RX: host -> device (TCode in). WRITE + WRITE_NR (no response) for speed.
    NimBLECharacteristic* rx = svc->createCharacteristic(
        BLE_NUS_RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new BleRxCallbacks(this));

    // TX: device -> host (TCode replies). NOTIFY.
    _ble_tx_char = svc->createCharacteristic(
        BLE_NUS_TX_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_NUS_SERVICE_UUID);
    adv->setName(BLE_DEVICE_NAME);
    adv->setScanResponse(true);
    NimBLEDevice::startAdvertising();

    _ble_running = true;
    applogf("[BLE] Advertising '%s' (Nordic UART service)", BLE_DEVICE_NAME);
    applog("[BLE] Connect a BLE host and write TCode to the RX characteristic.");
}

void ButtplugServer::stopBLE() {
    if (!_ble_running) return;
    NimBLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);
    _ble_server = nullptr;
    _ble_tx_char = nullptr;
    _ble_running = false;
    _ble_connected = false;
    applog("[BLE] Stopped");
}

// Assemble newline-terminated TCode lines from raw BLE bytes and parse them.
// Mirrors pollSerial() but for the BLE RX characteristic. Some hosts send each
// TCode command without a trailing newline as a single write; to support that,
// we also parse on write boundaries if no newline arrived but bytes are present
// and look complete (start with a known axis/command letter).
void ButtplugServer::feedBleBytes(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (_ble_len > 0) {
                _ble_buf[_ble_len] = '\0';
                rxFrameCount++;
                _ble_linked = true;
                parseTCode(_ble_buf, _ble_len);
                _ble_len = 0;
            }
        } else {
            if (_ble_len < sizeof(_ble_buf) - 1) {
                _ble_buf[_ble_len++] = c;
            } else {
                _ble_len = 0;  // overrun: resync
            }
        }
    }

    // Newline-less host: a single write that didn't end in a terminator is
    // treated as one complete command (the common case for TCode-over-BLE).
    if (_ble_len > 0 && len > 0) {
        char last = (char)data[len - 1];
        if (last != '\n' && last != '\r') {
            _ble_buf[_ble_len] = '\0';
            rxFrameCount++;
            _ble_linked = true;
            parseTCode(_ble_buf, _ble_len);
            _ble_len = 0;
        }
    }
}


