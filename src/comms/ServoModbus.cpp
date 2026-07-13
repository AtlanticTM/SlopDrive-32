/**
 * ServoModbus — RS485 / Modbus RTU telemetry & config for AIM servo drive. :3
 *
 * Non-blocking state-machine design: a single update() call sends the read
 * request on the first cycle, then checks for the response on subsequent
 * cycles — no delay(), no blocking. The 2 Hz poll cadence gives the UART
 * ~500ms to fill the RX buffer; the actual transaction takes ~4ms at 19200
 * baud, so there's zero risk of timeout.
 *
 * Motion stays pure step/dir on Core 1. This module is telemetry +
 * config-only — the real-time path is never touched by Modbus traffic.
 *
 * Per AIM_servo_modbus_reference.md: 19200 8N1, slave addr 1, all values
 * 16-bit two's-complement for signed fields, CRC16 polynomial 0xA001.
 */

#include "ServoModbus.h"

#if defined(FEATURE_RS485_MODBUS)

#include <Arduino.h>
#include <HardwareSerial.h>
#include <algorithm>

#include "AppLog.h"

// ============================================================================
// Constructor / destructor
// ============================================================================

// Poll cycle — one register per transaction, exactly like the known-working
// OSSM gold-motor tool. These drives commonly ignore multi-register FC 0x03
// requests, so we never batch. Order: enable, output, alarm, current, speed,
// voltage, temp, PWM. :3
const uint16_t ServoModbus::POLL_REGS[ServoModbus::POLL_REG_COUNT] =
    { 0x00, 0x01, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13 };

ServoModbus::ServoModbus(HardwareSerial& port, uint8_t addr)
    : _port(port)
    , _addr(addr)
{
}

ServoModbus::~ServoModbus() {
    // port is a reference — no ownership. :3
}

// ============================================================================
// CRC16 — Modbus RTU polynomial 0xA001 (reflected)
// ============================================================================

uint16_t ServoModbus::crc16(const uint8_t* buf, size_t len) const {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x0001)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

// ============================================================================
// FC 0x03 — Read Holding Registers (non-blocking, split across cycles)
// ============================================================================
//
// sendReadRequest() builds + sends the frame and returns the expected response
// length. On the next update() call, tryReadResponse() checks if enough bytes
// have accumulated in the RX buffer, validates CRC, and decodes the data.

size_t ServoModbus::sendReadRequest(uint16_t reg, size_t count) {
    // Flush stale bytes before sending
    while (_port.available()) _port.read();

    // Build: addr(1) + func(1) + start_reg(2) + count(2) + crc(2) = 8 bytes
    uint8_t req[8];
    req[0] = _addr;
    req[1] = 0x03;                         // FC 0x03
    req[2] = (reg >> 8) & 0xFF;
    req[3] = reg & 0xFF;
    req[4] = 0x00;
    req[5] = (uint8_t)count;
    uint16_t crc = crc16(req, 6);
    req[6] = crc & 0xFF;                   // CRC LSB first
    req[7] = (crc >> 8) & 0xFF;

    _port.write(req, 8);
    _port.flush();

    // Expected response: addr(1) + func(1) + bytecount(1) + data(2*count) + crc(2)
    return 5 + 2 * count;
}

bool ServoModbus::tryReadResponse(uint16_t* out, size_t count) {
    size_t expected = 5 + 2 * count;
    if (expected > MAX_RSP_LEN) return false;

    // Check if enough bytes have arrived
    if (_port.available() < (int)expected) return false;

    // Read into our local buffer
    size_t n = _port.readBytes(_rx_buf, expected);
    if (n < expected) return false;

    // Validate CRC
    uint16_t rspCrc = _rx_buf[expected - 2] | (_rx_buf[expected - 1] << 8);
    if (crc16(_rx_buf, expected - 2) != rspCrc) return false;

    // Validate address + function code
    if (_rx_buf[0] != _addr || _rx_buf[1] != 0x03) return false;

    // Validate byte count
    size_t byteCount = _rx_buf[2];
    if (byteCount != 2 * count) return false;

    // Extract data (MSB first per register)
    for (size_t i = 0; i < count; i++) {
        out[i] = (_rx_buf[3 + 2 * i] << 8) | _rx_buf[3 + 2 * i + 1];
    }
    return true;
}

// ============================================================================
// FC 0x06 — Write Single Register (fire-and-forget)
// ============================================================================

void ServoModbus::sendWriteCommand(uint16_t reg, uint16_t value) {
    while (_port.available()) _port.read();

    // Build: addr(1) + func(1) + reg(2) + value(2) + crc(2) = 8 bytes
    uint8_t req[8];
    req[0] = _addr;
    req[1] = 0x06;                         // FC 0x06
    req[2] = (reg >> 8) & 0xFF;
    req[3] = reg & 0xFF;
    req[4] = (value >> 8) & 0xFF;
    req[5] = value & 0xFF;
    uint16_t crc = crc16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = (crc >> 8) & 0xFF;

    _port.write(req, 8);
    _port.flush();
    // Fire-and-forget — the drive echoes back but we don't wait for it.
    // A stale echo byte will be flushed by the next sendReadRequest() flush. :3
}

// ============================================================================
// Lifecycle
// ============================================================================

bool ServoModbus::init() {
    // Caller already opened the port — we just probe the slave.
    APPLOGF("ServoModbus: probing drive @ addr %d...", _addr);

    // Try to read alarm register (0x0E) as a quick health check.
    // Two attempts — RS485 can be temperamental on first contact. :3
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t expected = sendReadRequest(0x0E, 1);
        _rx_expected = expected;

        // Wait for response (non-blocking poll loop — only during init, which
        // runs in setup() context before any RTOS tasks are live)
        unsigned long start = millis();
        while (_port.available() < (int)expected && (millis() - start) < 80) {
            delay(1);   // acceptable: init context, no RTOS tasks running yet
        }

        uint16_t val = 0;
        if (tryReadResponse(&val, 1)) {
            _ready = true;
            _telemetry.valid = true;
            _telemetry.alarm = val;
            APPLOGF("ServoModbus: drive @ addr %d answers (alarm=0x%04X) — RS485 is live :3",
                    _addr, val);
            return true;
        }
    }

    APPLOGF("ServoModbus: drive @ addr %d did NOT answer — RS485 probe failed.", _addr);
    _ready = false;
    return false;
}

void ServoModbus::update() {
    uint32_t now = millis();

    // ---- Not ready? Keep knocking. -----------------------------------------
    // The boot probe can fail for boring reasons (36V rail off at flash time,
    // drive still booting, A/B swapped). Instead of going silent forever, we
    // re-send the probe frame every REPROBE_INTERVAL_MS. Bonus: the RS485
    // module's TX LED pulses on every attempt — if that LED NEVER lights, the
    // frames aren't leaving the ESP32 (wrong TX pin / dead module). If TX
    // lights but RX never does, the drive isn't answering (A/B swapped, wrong
    // baud/addr, drive unpowered). Cheap built-in wiring diagnostic. :3
    if (!_ready) {
        switch (_rx_state) {
        case RxState::IDLE:
            if (now - _last_poll_ms < REPROBE_INTERVAL_MS) return;
            _last_poll_ms = now;
            _rx_expected = sendReadRequest(0x0E, 1);
            _rx_start_ms = now;
            _rx_state = RxState::WAITING;
            break;
        case RxState::WAITING: {
            if (_port.available() < (int)_rx_expected) {
                if (now - _rx_start_ms > 80) _rx_state = RxState::IDLE;
                return;
            }
            uint16_t val = 0;
            if (tryReadResponse(&val, 1)) {
                portENTER_CRITICAL(&_mux);
                _telemetry.valid = true;
                _telemetry.alarm = val;
                portEXIT_CRITICAL(&_mux);
                _ready = true;
                APPLOGF("ServoModbus: drive @ addr %d answered on re-probe (alarm=0x%04X) — RS485 is live :3",
                        _addr, val);
            }
            _rx_state = RxState::IDLE;
            break;
        }
        default:
            _rx_state = RxState::IDLE;
            break;
        }
        return;
    }

    // ---- Ready: single-register poll cycle ----------------------------------
    // One register per transaction — the gold-motor tool's access pattern.
    // Values stage into _staged[] and commit to _telemetry as one consistent
    // snapshot when the cycle wraps. :3
    switch (_rx_state) {
    case RxState::IDLE:
        // Rate-limit per-register polls to POLL_INTERVAL_MS
        if (now - _last_poll_ms < POLL_INTERVAL_MS) return;
        _last_poll_ms = now;
        _rx_expected = sendReadRequest(POLL_REGS[_reg_idx], 1);
        _rx_start_ms = now;
        _rx_state = RxState::WAITING;
        break;

    case RxState::WAITING: {
        // Check if enough bytes have arrived yet
        if (_port.available() < (int)_rx_expected) {
            // Give it up to 80ms — far longer than the ~4ms the UART needs.
            // If we time out, reset to IDLE and retry the SAME register.
            if (now - _rx_start_ms > 80) {
                _rx_state = RxState::IDLE;
            }
            return;
        }

        uint16_t val = 0;
        if (tryReadResponse(&val, 1)) {
            _staged[_reg_idx] = val;
            _reg_idx++;
            if (_reg_idx >= POLL_REG_COUNT) {
                _reg_idx = 0;
                // Full cycle complete — commit the snapshot under the spinlock
                // so getTelemetry() never sees a torn read. :3
                portENTER_CRITICAL(&_mux);
                _telemetry.valid     = true;
                _telemetry.enabled   = (_staged[0] != 0);            // 0x00
                _telemetry.output_on = (_staged[1] != 0);            // 0x01
                _telemetry.alarm     = _staged[2];                   // 0x0E
                _telemetry.current_a = (int16_t)_staged[3] / 2000.0f; // 0x0F ÷2000 A
                _telemetry.speed_rpm = (int16_t)_staged[4] / 10.0f;   // 0x10 ÷10 rpm
                _telemetry.voltage_v = _staged[5] / 327.0f;           // 0x11 ÷327 V
                _telemetry.temp_c    = (int16_t)_staged[6];           // 0x12 °C
                _telemetry.pwm_pct   = (int16_t)_staged[7] / 100.0f;  // 0x13 ÷100 %
                portEXIT_CRITICAL(&_mux);
            }
        }
        // Successful or garbled — next poll starts fresh (same reg on failure)
        _rx_state = RxState::IDLE;
        break;
    }

    default:
        _rx_state = RxState::IDLE;
        break;
    } // switch
}

void ServoModbus::emergencyStop() {
    if (!_ready) return;
    // Disable the drive output — reg 0x01 = 0. Fire-and-forget. :3
    sendWriteCommand(0x01, 0);
    APPLOG("ServoModbus: drive output disabled (emergency stop)");
}

// ============================================================================
// Thread-safe telemetry accessor
// ============================================================================

ServoTelemetry ServoModbus::getTelemetry() const {
    ServoTelemetry snap;
    portENTER_CRITICAL(&_mux);
    snap = _telemetry;
    portEXIT_CRITICAL(&_mux);
    return snap;
}

// ============================================================================
// Config writes (fire-and-forget — we don't wait for the echo)
// ============================================================================

void ServoModbus::setEnable(bool on) {
    if (!_ready) return;
    sendWriteCommand(0x00, on ? 1 : 0);
}

void ServoModbus::setOutput(bool on) {
    if (!_ready) return;
    sendWriteCommand(0x01, on ? 1 : 0);
}

void ServoModbus::setSpeed(uint16_t rpm) {
    if (!_ready) return;
    sendWriteCommand(0x02, rpm);
}

void ServoModbus::setAccel(uint16_t rpm_s) {
    if (!_ready) return;
    sendWriteCommand(0x03, rpm_s);
}

void ServoModbus::setDirPolarity(bool invert) {
    if (!_ready) return;
    sendWriteCommand(0x09, invert ? 1 : 0);
}

void ServoModbus::saveToFlash() {
    if (!_ready) return;
    sendWriteCommand(0x14, 1);
    APPLOG("ServoModbus: saved config to drive flash — will survive a power cycle. :3");
}

#endif // defined(FEATURE_RS485_MODBUS)