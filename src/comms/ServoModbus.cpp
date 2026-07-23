/**
 * ServoModbus — RS485 / Modbus RTU telemetry & config for AIM servo drive. :3
 *
 * Non-blocking state-machine design: a single update() call sends the read
 * request on the first cycle, then checks for the response on subsequent
 * cycles — no delay(), no blocking. The 2 Hz poll cadence gives the UART
 * ~500ms to fill the RX buffer; the actual transaction takes ~4ms at 19200
 * baud (faster still at 115200), so there's zero risk of timeout.
 *
 * Dual-baud: the drive is factory-19200, but may have been reprogrammed to
 * 115200 (OSSM-RS style). init() probes both at boot; the not-ready reprobe
 * loop in update() keeps alternating between them so a drive that shows up
 * later at either baud still gets found. baud() reports which one landed.
 *
 * Motion stays pure step/dir on Core 1. This module is telemetry +
 * config-only — the real-time path is never touched by Modbus traffic. The
 * proprietary 0x7B setpoint frame (sendSetpoint()) is plumbed in but unused —
 * a future streamed-motion executor is the only intended caller.
 *
 * Per AIM_servo_modbus_reference.md: 8N1 @ 19200 or 115200, slave addr 1, all
 * values 16-bit two's-complement for signed fields, CRC16 polynomial 0xA001.
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
// FC 0x10 — Write Multiple Registers (bench: paired 0x0C/0x0D position write)
// ============================================================================
// BENCH HYPOTHESIS (fw 2.1.25 findings): single-register FC 0x06 writes to the
// position pair 0x0C/0x0D are REJECTED (read back unchanged) — classic
// torn-half protection — while multi-register FC 0x03 READS work fine, so
// this firmware is more standard-Modbus than its translated datasheet admits.
// A 32-bit position command may only be accepted as ONE atomic FC 0x10
// transaction covering both words. Frame:
//   [addr][0x10][startHi][startLo][cntHi][cntLo][byteCount=4]
//   [w1Hi][w1Lo][w2Hi][w2Lo][crcLo][crcHi]  = 13 bytes
// Normal reply: [addr][0x10][start][count][crc] = 8 bytes — fire-and-forget
// here (bench), flushed by the next transaction like the FC 0x06 echoes. :3

void ServoModbus::sendWriteMulti(uint16_t start_reg, uint16_t w1, uint16_t w2) {
    while (_port.available()) _port.read();

    uint8_t req[13];
    req[0]  = _addr;
    req[1]  = 0x10;                        // FC 0x10 Write Multiple Registers
    req[2]  = (start_reg >> 8) & 0xFF;
    req[3]  = start_reg & 0xFF;
    req[4]  = 0x00;                        // register count = 2
    req[5]  = 0x02;
    req[6]  = 0x04;                        // byte count = 4
    req[7]  = (w1 >> 8) & 0xFF;
    req[8]  = w1 & 0xFF;
    req[9]  = (w2 >> 8) & 0xFF;
    req[10] = w2 & 0xFF;
    uint16_t crc = crc16(req, 11);
    req[11] = crc & 0xFF;                  // CRC LSB first
    req[12] = (crc >> 8) & 0xFF;

    _port.write(req, 13);
    _port.flush();
}

// ============================================================================
// Absolute-position setpoint — FC + byte order RUNTIME-TUNABLE (bench)
// ============================================================================
// Frame: [addr][FC][pos32][CRC16-LE], 8 bytes total. Our drive's datasheet
// documents FC 0x78 "Write Target Position" (default); OSSM-RS's 57AIMxx
// generation uses proprietary 0x7B with a big-endian payload — SAME shape,
// different code. Bench finding (fw 2.1.21, 285 frames): this drive never
// answers 0x7B at all, so the FC and payload byte order are runtime knobs
// (setSetpointFraming(), driven from POST /api/servo {"sp_fc":..,"sp_le":..})
// to let the bench find the variant's true framing without a reflash per
// guess. Unlike the fire-and-forget FC 0x06 write, we DO validate the echo —
// this IS the motion path, so a garbled or missing echo has to be visible
// (see the WAITING/SETPOINT handling in update()). :3

void ServoModbus::setSetpointFraming(uint8_t fc, bool le) {
    _sp_fc = fc;
    _sp_le = le;
    _sp_exception_logged = false;   // fresh knob, fresh one-shot diagnostic
    APPLOGF("ServoModbus: setpoint framing -> FC 0x%02X, payload %s-endian (bench knob)",
            fc, le ? "little" : "big");
}

void ServoModbus::setSetpointNoEcho(bool noecho) {
    _sp_noecho = noecho;
    if (noecho) _sp_fail_streak = 0;   // clear any latched streak so the watchdog re-arms clean later
    APPLOGF("ServoModbus: setpoint no-echo mode %s %s", noecho ? "ON" : "OFF",
            noecho ? "— BENCH ONLY, watchdog blind to setpoint loss!" : "— echo validation restored");
}

bool ServoModbus::sendSetpoint(int32_t pos_counts) {
    if (!_ready || _rx_state != RxState::IDLE) return false;

    while (_port.available()) _port.read();

    // Build: addr(1) + func(1) + pos32(4) + crc(2) = 8 bytes
    uint8_t req[8];
    req[0] = _addr;
    req[1] = _sp_fc;
    uint32_t p = (uint32_t)pos_counts;
    if (_sp_le) {
        req[2] = (uint8_t)(p);
        req[3] = (uint8_t)(p >> 8);
        req[4] = (uint8_t)(p >> 16);
        req[5] = (uint8_t)(p >> 24);
    } else {
        req[2] = (uint8_t)(p >> 24);
        req[3] = (uint8_t)(p >> 16);
        req[4] = (uint8_t)(p >> 8);
        req[5] = (uint8_t)(p);
    }
    uint16_t crc = crc16(req, 6);
    req[6] = crc & 0xFF;                   // CRC LSB first
    req[7] = (crc >> 8) & 0xFF;

    _port.write(req, 8);
    _port.flush();

    _sp_sent++;
    _sp_last_fc = _sp_fc;

    if (_sp_noecho) {
        // BENCH mode: fire-and-forget like FC 0x06 — no echo wait, bus goes
        // straight back to IDLE (any stray reply bytes get flushed by the next
        // transaction's pre-flush). Watchdog is blind in this mode. :3
        return true;
    }

    _rx_expected  = 8;      // assume the drive echoes the frame back
    _pending_kind = PendingKind::SETPOINT;
    _rx_start_ms  = millis();
    _rx_state     = RxState::WAITING;
    return true;
}

bool ServoModbus::sendPositionDelta(int32_t delta_counts) {
    if (!_ready || _rx_state != RxState::IDLE) return false;

    while (_port.available()) _port.read();

    // FC 0x10, start 0x000C, 2 registers, 4 data bytes: low word then high
    // word (bench-proven order, fw 2.1.26: +100 -> +103 counts moved). :3
    uint32_t d  = (uint32_t)delta_counts;
    uint16_t lw = (uint16_t)(d & 0xFFFF);
    uint16_t hw = (uint16_t)((d >> 16) & 0xFFFF);

    uint8_t req[13];
    req[0]  = _addr;
    req[1]  = 0x10;
    req[2]  = 0x00; req[3]  = 0x0C;        // start reg
    req[4]  = 0x00; req[5]  = 0x02;        // count 2
    req[6]  = 0x04;                        // byte count
    req[7]  = (lw >> 8) & 0xFF; req[8]  = lw & 0xFF;
    req[9]  = (hw >> 8) & 0xFF; req[10] = hw & 0xFF;
    uint16_t crc = crc16(req, 11);
    req[11] = crc & 0xFF;
    req[12] = (crc >> 8) & 0xFF;

    _port.write(req, 13);
    _port.flush();

    _sp_sent++;
    _sp_last_fc = 0x10;

    if (_sp_noecho) return true;           // bench knob still honored

    // Standard FC 0x10 reply: [addr][0x10][startHi][startLo][cntHi][cntLo][crc]
    _rx_expected  = 8;
    _pending_kind = PendingKind::SETPOINT;
    _rx_start_ms  = millis();
    _rx_state     = RxState::WAITING;
    return true;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool ServoModbus::init() {
    // Caller already opened the port at 19200 (factory default) — probe there
    // first. If nobody answers, the drive may have been reprogrammed OSSM-RS
    // style to 115200; rebaud the port and probe again. Whichever baud
    // answers wins and _baud latches to it for every subsequent transaction
    // (including the not-ready reprobe in update()). :3
    APPLOGF("ServoModbus: probing drive @ addr %d (19200)...", _addr);

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
            _baud = 19200;
            _telemetry.valid = true;
            _telemetry.alarm = val;
            APPLOGF("ServoModbus: drive @ addr %d answers @19200 (alarm=0x%04X) — RS485 is live :3",
                    _addr, val);
            return true;
        }
    }

    APPLOGF("ServoModbus: drive @ addr %d did NOT answer @19200 — trying 115200 (OSSM-RS-style reprogrammed drive)...",
            _addr);
    _port.updateBaudRate(115200);

    for (int attempt = 0; attempt < 2; attempt++) {
        size_t expected = sendReadRequest(0x0E, 1);
        _rx_expected = expected;

        unsigned long start = millis();
        while (_port.available() < (int)expected && (millis() - start) < 80) {
            delay(1);   // acceptable: init context, no RTOS tasks running yet
        }

        uint16_t val = 0;
        if (tryReadResponse(&val, 1)) {
            _ready = true;
            _baud = 115200;
            _telemetry.valid = true;
            _telemetry.alarm = val;
            APPLOGF("ServoModbus: drive @ addr %d answers @115200 (alarm=0x%04X) — RS485 is live :3",
                    _addr, val);
            return true;
        }
    }

    // Neither baud answered — restore 19200 so the port and _baud are back in
    // a known state for the not-ready reprobe loop in update() to alternate
    // from. :3
    _port.updateBaudRate(19200);
    _baud = 19200;
    APPLOGF("ServoModbus: drive @ addr %d did NOT answer at either baud — RS485 probe failed.", _addr);
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
            // Alternate baud on every reprobe attempt — a drive that later
            // shows up reprogrammed to 115200 (or power-cycled back to
            // factory 19200) gets found either way. Same TX-LED wiring
            // diagnostic as before: no TX blink means the frames never left
            // the ESP32 regardless of which baud we're trying. :3
            _baud = (_baud == 19200) ? 115200 : 19200;
            _port.updateBaudRate(_baud);
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
                APPLOGF("ServoModbus: drive @ addr %d answered on re-probe @%lu baud (alarm=0x%04X) — RS485 is live :3",
                        _addr, (unsigned long)_baud, val);
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
    case RxState::IDLE: {
        // 1. Drain the write queue first (user-facing config writes). Peek
        //    the count under the mux; if there's work AND spacing allows,
        //    snapshot the head op + advance/pop under the mux, THEN do the
        //    wire write outside it — never hold the spinlock across UART. :3
        portENTER_CRITICAL(&_mux);
        size_t wq_count = _wq_count;
        portEXIT_CRITICAL(&_mux);
        if (wq_count > 0) {
            if (now - _last_write_ms < WRITE_SPACING_MS) return;
            portENTER_CRITICAL(&_mux);
            WriteOp op = _wq[_wq_head];
            if (op.rep > 1) {
                _wq[_wq_head].rep--;
            } else {
                _wq_head = (_wq_head + 1) % WRITE_Q_LEN;
                _wq_count--;
            }
            portEXIT_CRITICAL(&_mux);
            if (op.kind == 1) {
                sendWriteMulti(op.reg, op.val, op.val2);   // FC 0x10 pair (bench)
            } else {
                sendWriteCommand(op.reg, op.val);
            }
            _last_write_ms = now;
            if (op.rep <= 1) {
                // Follow a device-address change so the bus keeps talking to
                // the drive it just renamed. :3
                if (op.reg == 0x15 && op.val >= 1 && op.val <= 247) {
                    _addr = (uint8_t)op.val;
                    APPLOGF("ServoModbus: following device-address change -> %u", _addr);
                }
            }
            return;
        }

        // 2. Config scan (on demand, faster cadence than telemetry).
        portENTER_CRITICAL(&_mux);
        bool scanning = _scan_active;
        portEXIT_CRITICAL(&_mux);
        if (scanning) {
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
    }

    case RxState::WAITING: {
        // Check if enough bytes have arrived yet
        if (_port.available() < (int)_rx_expected) {
            // Setpoint frames get their own short timeout — this is a future
            // motion-stream frame, not telemetry, so a stall here must fail
            // fast rather than sit on the 80ms telemetry budget. :3
            uint32_t timeout_ms = (_pending_kind == PendingKind::SETPOINT) ? SETPOINT_TIMEOUT_MS : 80;
            if (now - _rx_start_ms > timeout_ms) {
                if (_pending_kind == PendingKind::SETPOINT) {
                    // Before writing this off as silence: a drive that DOESN'T
                    // support the setpoint FC answers with a 5-byte Modbus
                    // exception ([addr][FC|0x80][code][crc]) — shorter than
                    // the 8-byte echo we wait for, so it lands HERE, not in
                    // the success path. Log it ONCE: "exception code N" tells
                    // the bench operator to try the next framing knob, where
                    // pure silence says the FC isn't even parsed. :3
                    int avail = _port.available();
                    if (avail >= 5 && !_sp_exception_logged) {
                        size_t n = _port.readBytes(_rx_buf, (avail < (int)MAX_RSP_LEN) ? avail : MAX_RSP_LEN);
                        if (n >= 5 && _rx_buf[0] == _addr && _rx_buf[1] == (uint8_t)(_sp_last_fc | 0x80)) {
                            _sp_exception_logged = true;
                            APPLOGF("ServoModbus: drive REJECTED setpoint FC 0x%02X — Modbus exception code %u",
                                    _sp_last_fc, _rx_buf[2]);
                        }
                    }
                    _sp_fail_streak++;
                } else if (_pending_kind == PendingKind::SCAN) {
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

        // Setpoint echo — its own framing (addr + FC 0x7B + 6 bytes), not a
        // 1- or 2-register FC 0x03 response, so validate it before any of the
        // read-response paths below touch the buffer. :3
        if (_pending_kind == PendingKind::SETPOINT) {
            bool ok = false;
            if (_rx_expected <= MAX_RSP_LEN) {
                size_t n = _port.readBytes(_rx_buf, _rx_expected);
                if (n >= _rx_expected) {
                    uint16_t rspCrc = _rx_buf[_rx_expected - 2] | (_rx_buf[_rx_expected - 1] << 8);
                    ok = (crc16(_rx_buf, _rx_expected - 2) == rspCrc) &&
                         (_rx_buf[0] == _addr) && (_rx_buf[1] == _sp_last_fc);
                }
            }
            if (ok) {
                _sp_ok++;
                _sp_fail_streak = 0;
            } else {
                _sp_fail_streak++;
            }
            _rx_state = RxState::IDLE;
            break;
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
    _scan_active  = false;
    portEXIT_CRITICAL(&_mux);
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

// ============================================================================
// reprogramBaud() — OSSM-RS magic sequence, blocking, init-context ONLY
// ============================================================================
// Same "no RTOS tasks yet" exception as init()/readRegisterBlocking(): this
// touches the port and the low-level send/receive helpers directly, bypassing
// the update() state machine entirely, so it must never run once
// servoBusTask/httpTask are alive and polling. main.cpp calls it right after
// servoModbus.init() succeeds, well before any task is created. :3
bool ServoModbus::reprogramBaud(uint32_t target_baud) {
    if (!_ready) return false;

    uint16_t baud_code;
    if (target_baud == 115200)      baud_code = 803;
    else if (target_baud == 19200)  baud_code = 801;
    else {
        APPLOGF("ServoModbus: reprogramBaud(%lu) refused — only 19200/115200 are known baud codes.",
                (unsigned long)target_baud);
        return false;
    }

    uint32_t previous_baud = _baud;
    if (previous_baud == target_baud) return true;   // already there, nothing to do

    APPLOGF("ServoModbus: reprogramBaud() — %lu -> %lu via OSSM-RS magic sequence...",
            (unsigned long)previous_baud, (unsigned long)target_baud);

    // OSSM-RS magic sequence — fire-and-forget FC 0x06 writes, ~30ms gaps.
    // NEVER reg 0x14 (the EEPROM save flag) — this is a deliberately VOLATILE
    // runtime change so a drive power-cycle always recovers factory 19200; a
    // botched rebaud can never permanently brick the link. Reg 0x03 doubles
    // as the magic sequence's baud-code carrier here — it's normally the
    // drive's ACCEL register, so whatever real accel value lived there may
    // need rewriting afterward (the Configure pane / handleApiServo owns
    // that, not this function). :3
    sendWriteCommand(0x00, 1);
    delay(30);
    sendWriteCommand(0x03, baud_code);
    delay(30);
    sendWriteCommand(0x04, 129);
    delay(30);
    sendWriteCommand(0x00, 506);
    delay(100);   // settle before we rebaud our own port

    _port.updateBaudRate(target_baud);

    // Probe to confirm — two attempts, same pattern as init(). :3
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t expected = sendReadRequest(0x0E, 1);
        _rx_expected = expected;
        unsigned long start = millis();
        while (_port.available() < (int)expected && (millis() - start) < 80) {
            delay(1);   // acceptable: init-context, no RTOS tasks running yet
        }
        uint16_t val = 0;
        if (tryReadResponse(&val, 1)) {
            _baud = target_baud;
            APPLOGF("ServoModbus: reprogramBaud() CONFIRMED @%lu (alarm=0x%04X) :3",
                    (unsigned long)target_baud, val);
            return true;
        }
    }

    // Didn't answer at the new baud — restore the PREVIOUS baud and re-probe
    // so the link is left in a KNOWN state, never stranded between two
    // possible speeds. :3
    APPLOGF("ServoModbus: reprogramBaud() to %lu did NOT confirm — restoring %lu...",
            (unsigned long)target_baud, (unsigned long)previous_baud);
    _port.updateBaudRate(previous_baud);
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t expected = sendReadRequest(0x0E, 1);
        _rx_expected = expected;
        unsigned long start = millis();
        while (_port.available() < (int)expected && (millis() - start) < 80) {
            delay(1);
        }
        uint16_t val = 0;
        if (tryReadResponse(&val, 1)) {
            _baud = previous_baud;
            APPLOGF("ServoModbus: restored @%lu (alarm=0x%04X) after failed reprogram.",
                    (unsigned long)previous_baud, val);
            return false;
        }
    }
    // Neither the new nor the old baud answered — link is likely down for an
    // unrelated reason. Leave _baud at the best-known previous value; the
    // not-ready reprobe loop in update() will keep alternating and find the
    // drive again once it's back. :3
    APPLOGF("ServoModbus: reprogramBaud() — restore probe at %lu ALSO failed. Link may be down; "
            "the reprobe loop will keep looking.", (unsigned long)previous_baud);
    _baud = previous_baud;
    return false;
}

void ServoModbus::emergencyStop() {
    if (!_ready) return;
    // Never keep programming through an e-stop — drop everything queued. :3
    portENTER_CRITICAL(&_mux);
    _wq_count = 0;
    _scan_active = false;
    portEXIT_CRITICAL(&_mux);
    // Disable the drive output — reg 0x01 = 0. Fire-and-forget. This wire
    // write itself can race a poll in flight (update() may be mid-WAITING on
    // an unrelated read when this fires from another context) — acceptable
    // for e-stop: worst case is one garbled frame on either side. What has to
    // stay consistent is the drain state above (queue now empty), not this
    // specific transaction. :3
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
    snap.scanning = _scan_active;
    portEXIT_CRITICAL(&_mux);
    return snap;
}

void ServoModbus::requestConfigScan() {
    if (!_ready) return;
    // Reset the scan cursor/staging BEFORE raising _scan_active: the flag is
    // the cross-core handshake, and once update() (Core 1 in Modbus mode) sees
    // it, the cursor must already be at zero — raise-then-reset would let a
    // scan launch mid-reset with a stale index. The staging fields themselves
    // are only ever touched by update() while _scan_active is up, so resetting
    // them outside the mux here is safe when the flag is down. :3
    portENTER_CRITICAL(&_mux);
    bool already = _scan_active;
    portEXIT_CRITICAL(&_mux);
    if (already) return;
    _scan_idx     = 0;
    _scan_retries = 0;
    _scan_known   = 0;
    memset(_cfg_staged, 0, sizeof(_cfg_staged));
    portENTER_CRITICAL(&_mux);
    _scan_active = true;
    portEXIT_CRITICAL(&_mux);
}

bool ServoModbus::queueWrite(uint16_t reg, uint16_t value, uint8_t repeat) {
    if (!_ready) return false;
    if (repeat < 1) repeat = 1;
    portENTER_CRITICAL(&_mux);
    bool full = _wq_count >= WRITE_Q_LEN;
    if (!full) {
        size_t tail = (_wq_head + _wq_count) % WRITE_Q_LEN;
        _wq[tail] = { reg, value, repeat, /*kind=*/0, /*val2=*/0 };
        _wq_count++;
    }
    portEXIT_CRITICAL(&_mux);
    if (full) {
        APPLOG("ServoModbus: write queue FULL — write dropped");
        return false;
    }
    return true;
}

bool ServoModbus::queuePositionPair(int32_t counts, bool low_first) {
    if (!_ready) return false;
    uint16_t lw = (uint16_t)((uint32_t)counts & 0xFFFF);
    uint16_t hw = (uint16_t)(((uint32_t)counts >> 16) & 0xFFFF);
    uint16_t w1 = low_first ? lw : hw;
    uint16_t w2 = low_first ? hw : lw;
    portENTER_CRITICAL(&_mux);
    bool full = _wq_count >= WRITE_Q_LEN;
    if (!full) {
        size_t tail = (_wq_head + _wq_count) % WRITE_Q_LEN;
        _wq[tail] = { /*reg=*/0x0C, w1, /*rep=*/1, /*kind=*/1, w2 };
        _wq_count++;
    }
    portEXIT_CRITICAL(&_mux);
    if (full) {
        APPLOG("ServoModbus: write queue FULL — position pair dropped");
        return false;
    }
    APPLOGF("ServoModbus: queued FC 0x10 position pair 0x0C/0x0D = %ld (%s-word first)",
            (long)counts, low_first ? "low" : "high");
    return true;
}

size_t ServoModbus::pendingWrites() const {
    // Count each remaining repeat as one pending wire-write so the UI's
    // progress hint drains linearly. :3
    portENTER_CRITICAL(&_mux);
    size_t n = 0;
    for (size_t i = 0; i < _wq_count; i++)
        n += _wq[(_wq_head + i) % WRITE_Q_LEN].rep;
    portEXIT_CRITICAL(&_mux);
    return n;
}

// ---- Bus health snapshot ---------------------------------------------------
ServoBusHealth ServoModbus::getBusHealth() const {
    ServoBusHealth h;
    h.baud  = _baud;
    h.ready = _ready;
    portENTER_CRITICAL(&_mux);
    h.sp_fail_streak = _sp_fail_streak;
    h.sp_sent        = _sp_sent;
    h.sp_ok          = _sp_ok;
    portEXIT_CRITICAL(&_mux);
    return h;
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