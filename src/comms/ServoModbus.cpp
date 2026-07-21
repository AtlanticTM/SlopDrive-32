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

bool ServoModbus::readRegisterBlocking(uint16_t reg, uint16_t& out, uint32_t timeout_ms) {
    if (!_ready) return false;
    // Two attempts, same as the init() probe — RS485 first-contact flakiness.
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t expected = sendReadRequest(reg, 1);
        unsigned long start = millis();
        while (_port.available() < (int)expected && (millis() - start) < timeout_ms) {
            delay(1);   // acceptable: init context, no RTOS tasks running yet
        }
        if (tryReadResponse(&out, 1)) return true;
    }
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

    // ---- Ready: writes > config scan > telemetry poll -----------------------
    // One register per transaction — the gold-motor tool's access pattern.
    // Writes preempt reads but only launch from IDLE, so a write echo can
    // never masquerade as a poll response (the next read's flush eats it). :3
    switch (_rx_state) {
    case RxState::IDLE:
        // 1. Drain the write queue first (user-facing config writes).
        if (_wq_count > 0) {
            if (now - _last_write_ms < WRITE_SPACING_MS) return;
            WriteOp& op = _wq[_wq_head];
            sendWriteCommand(op.reg, op.val);
            _last_write_ms = now;
            if (op.rep > 1) {
                op.rep--;
            } else {
                // Follow a device-address change so the bus keeps talking to
                // the drive it just renamed. :3
                if (op.reg == 0x15 && op.val >= 1 && op.val <= 247) {
                    _addr = (uint8_t)op.val;
                    APPLOGF("ServoModbus: following device-address change -> %u", _addr);
                }
                _wq_head = (_wq_head + 1) % WRITE_Q_LEN;
                _wq_count--;
            }
            return;
        }

        // 2. Config scan (on demand, faster cadence than telemetry).
        if (_scan_active) {
            if (now - _last_poll_ms < SCAN_INTERVAL_MS) return;
            _last_poll_ms = now;
            _rx_expected  = sendReadRequest((uint16_t)_scan_idx, 1);
            _rx_start_ms  = now;
            _pending_kind = PendingKind::SCAN;
            _rx_state     = RxState::WAITING;
            return;
        }

        // 3. Telemetry poll cycle, rate-limited to POLL_INTERVAL_MS.
        //    Slots 0..7 are the single telemetry registers; slot 8 is the
        //    encoder-position pair (one count=2 read, or LO/HI singles in
        //    fallback mode with slot 9 as the HI half). :3
        if (now - _last_poll_ms < POLL_INTERVAL_MS) return;
        _last_poll_ms = now;
        if (_reg_idx < POLL_REG_COUNT) {
            _rx_expected  = sendReadRequest(POLL_REGS[_reg_idx], 1);
            _pending_kind = PendingKind::TELE;
        } else if (!_enc_single_mode) {
            _rx_expected  = sendReadRequest(REG_ENC_LO, 2);
            _pending_kind = PendingKind::ENC_PAIR;
        } else if (_reg_idx == POLL_REG_COUNT) {
            _rx_expected  = sendReadRequest(REG_ENC_LO, 1);
            _pending_kind = PendingKind::ENC_LO;
        } else {
            _rx_expected  = sendReadRequest(REG_ENC_HI, 1);
            _pending_kind = PendingKind::ENC_HI;
        }
        _rx_start_ms  = now;
        _rx_state     = RxState::WAITING;
        break;

    case RxState::WAITING: {
        // Check if enough bytes have arrived yet
        if (_port.available() < (int)_rx_expected) {
            // Give it up to 80ms — far longer than the ~4ms the UART needs.
            if (now - _rx_start_ms > 80) {
                if (_pending_kind == PendingKind::SCAN) {
                    // A register that times out 3× is marked unknown and
                    // skipped — some drive variants stop at 0x14. :3
                    if (++_scan_retries >= 3) {
                        _scan_retries = 0;
                        _cfg_staged[_scan_idx] = 0;
                        _scanAdvance(now);
                    }
                } else if (_pending_kind == PendingKind::ENC_PAIR) {
                    // A drive that rejects count=2 answers with a 5-byte
                    // exception frame (< the 9 we expect) — lands here too.
                    // 3 strikes latches the single-read fallback for good. :3
                    if (++_enc_pair_fails >= 3 && !_enc_single_mode) {
                        _enc_single_mode = true;
                        APPLOG("ServoModbus: drive ignores 2-reg reads — encoder falls back to single-register LO/HI");
                    }
                    _reg_idx = 0;   // skip the encoder this cycle
                } else if (_pending_kind == PendingKind::ENC_LO ||
                           _pending_kind == PendingKind::ENC_HI) {
                    _reg_idx = 0;   // abort — never pair a stale LO with a late HI
                }
                _rx_state = RxState::IDLE;
            }
            return;
        }

        // Encoder pair (count=2) responses carry two registers — handle first.
        if (_pending_kind == PendingKind::ENC_PAIR) {
            uint16_t pair[2] = {0, 0};
            if (tryReadResponse(pair, 2)) {
                _enc_pair_fails = 0;
                _commitEncoder((int32_t)(((uint32_t)pair[1] << 16) | pair[0]), now);
            }
            // Garbled frame: just skip — the next cycle retries in ~25ms.
            _reg_idx  = 0;
            _rx_state = RxState::IDLE;
            break;
        }

        uint16_t val = 0;
        bool ok = tryReadResponse(&val, 1);

        if (_pending_kind == PendingKind::ENC_LO) {
            // Stage LO and go straight for HI; a garble aborts the pair so a
            // fresh LO is always matched with its own HI. :3
            _reg_idx  = ok ? (POLL_REG_COUNT + 1) : 0;
            if (ok) _enc_lo_staged = val;
            _rx_state = RxState::IDLE;
            break;
        }
        if (_pending_kind == PendingKind::ENC_HI) {
            if (ok) _commitEncoder((int32_t)(((uint32_t)val << 16) | _enc_lo_staged), now);
            _reg_idx  = 0;
            _rx_state = RxState::IDLE;
            break;
        }

        if (_pending_kind == PendingKind::SCAN) {
            if (ok) {
                _cfg_staged[_scan_idx] = val;
                _scan_known |= (1UL << _scan_idx);
                _scan_retries = 0;
                _scanAdvance(now);
            }
            // Garbled: retry the same register next cycle (retries counted on
            // timeout only — a CRC-fail retry is free).
            _rx_state = RxState::IDLE;
            break;
        }

        if (ok) {
            _staged[_reg_idx] = val;
            _reg_idx++;
            if (_reg_idx >= POLL_REG_COUNT) {
                // Full cycle complete — commit the snapshot under the spinlock
                // so getTelemetry() never sees a torn read. _reg_idx parks at
                // POLL_REG_COUNT: the encoder-pair slot runs next, then wraps.
                // enc_* fields are untouched here — they commit on their own
                // cadence in _commitEncoder(). :3
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

// Advance the config scan to the next register; commit the mirror as one
// consistent snapshot when the last register lands. :3
void ServoModbus::_scanAdvance(uint32_t now) {
    _scan_idx++;
    if (_scan_idx < CFG_REG_COUNT) return;
    portENTER_CRITICAL(&_mux);
    _cfg.valid    = true;
    _cfg.stamp_ms = now;
    _cfg.known    = _scan_known;
    memcpy(_cfg.regs, _cfg_staged, sizeof(_cfg.regs));
    portEXIT_CRITICAL(&_mux);
    _scan_active = false;
    APPLOGF("ServoModbus: config scan complete (known=0x%08lX) :3", (unsigned long)_scan_known);
}

// Commit a freshly assembled encoder sample. Separate from the 8-reg snapshot
// commit so the position is as fresh as the wire allows (~3.7 Hz) and stamped
// — the EncoderValidator keys off enc_stamp_ms to spot NEW samples. :3
void ServoModbus::_commitEncoder(int32_t counts, uint32_t now) {
    portENTER_CRITICAL(&_mux);
    _telemetry.enc_valid    = true;
    _telemetry.enc_counts   = counts;
    _telemetry.enc_stamp_ms = now;
    portEXIT_CRITICAL(&_mux);
}

void ServoModbus::emergencyStop() {
    if (!_ready) return;
    // Never keep programming through an e-stop — drop everything queued. :3
    _wq_count = 0;
    _scan_active = false;
    // Disable the drive output — reg 0x01 = 0. Fire-and-forget. :3
    sendWriteCommand(0x01, 0);
    APPLOG("ServoModbus: drive output disabled (emergency stop)");
}

// ============================================================================
// Configure-pane plumbing — config mirror, scan, write queue
// ============================================================================

ServoConfig ServoModbus::getConfig() const {
    ServoConfig snap;
    portENTER_CRITICAL(&_mux);
    snap = _cfg;
    portEXIT_CRITICAL(&_mux);
    snap.scanning = _scan_active;
    return snap;
}

void ServoModbus::requestConfigScan() {
    if (!_ready || _scan_active) return;
    _scan_active  = true;
    _scan_idx     = 0;
    _scan_retries = 0;
    _scan_known   = 0;
    memset(_cfg_staged, 0, sizeof(_cfg_staged));
}

bool ServoModbus::queueWrite(uint16_t reg, uint16_t value, uint8_t repeat) {
    if (!_ready) return false;
    if (repeat < 1) repeat = 1;
    if (_wq_count >= WRITE_Q_LEN) {
        APPLOG("ServoModbus: write queue FULL — write dropped");
        return false;
    }
    size_t tail = (_wq_head + _wq_count) % WRITE_Q_LEN;
    _wq[tail] = { reg, value, repeat };
    _wq_count++;
    return true;
}

size_t ServoModbus::pendingWrites() const {
    // Count each remaining repeat as one pending wire-write so the UI's
    // progress hint drains linearly. :3
    size_t n = 0;
    for (size_t i = 0; i < _wq_count; i++)
        n += _wq[(_wq_head + i) % WRITE_Q_LEN].rep;
    return n;
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