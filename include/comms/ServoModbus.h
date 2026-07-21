#pragma once

// ============================================================================
// ServoModbus — RS485 / Modbus RTU telemetry & config for AIM servo drive
// ============================================================================
//
// Datasheet: Modbus RTU 19200 8N1, FC 0x03 (read), 0x06 (write), 0x78 (write
// target position), 0x7A (change address). Readable telemetry:
//   0x0E alarm, 0x0F current (÷2000 A), 0x10 speed (÷10 r/min),
//   0x11 voltage (÷327 V), 0x12 temp °C, 0x13 PWM (±100%).
// Writable config:
//   0x00 enable, 0x01 output enable, 0x02 speed, 0x03 accel,
//   0x05/0x06/0x07 gains, 0x09 DIR polarity, 0x0A/0x0B e-gear, 0x14 save flag.
//
// Build-guarded behind FEATURE_RS485_MODBUS so the code compiles to nothing
// until the RS485 transceiver is confirmed wired and the flag is set in
// platformio.ini build_flags.
//
// Motion stays pure step/dir on Core 1 — Modbus is telemetry/config only. :3
//
// Per the AIM servo reference (AIM_servo_modbus_reference.md):
//   - XY-G485 auto-direction module handles DE/RE from TX — no explicit pin
//   - All values are 16-bit two's-complement for signed fields
//   - CRC16 Modbus RTU polynomial 0xA001 (reflected)

#if defined(FEATURE_RS485_MODBUS)

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class HardwareSerial;

// ---- Servo telemetry snapshot (updated by update(), read by /api/status) ----
struct ServoTelemetry {
    bool     valid      = false;   // true when we've had at least one good poll
    bool     enabled    = false;   // Modbus enable flag (reg 0x00)
    bool     output_on  = false;   // driver output enable (reg 0x01)
    uint16_t alarm      = 0;      // raw alarm register bitfield (reg 0x0E)
    float    current_a  = 0.0f;   // motor current in AMPS (reg 0x0F ÷ 2000)
    float    speed_rpm  = 0.0f;   // motor speed in RPM (reg 0x10 ÷ 10, signed)
    float    voltage_v  = 0.0f;   // system voltage in VOLTS (reg 0x11 ÷ 327)
    float    temp_c     = 0.0f;   // drive temperature in °C (reg 0x12, signed)
    float    pwm_pct    = 0.0f;   // PWM duty cycle in % (reg 0x13 ÷ 100, signed)

    // ---- Absolute encoder position (regs 0x16 LO / 0x17 HI) ----------------
    // Raw signed 32-bit encoder counts, 32768/motor-rev, zero = wherever the
    // shaft sat at drive power-on. Confirmed live on the 57AIM30: the pair
    // answers and tracks the shaft; 0x0C/0x0D is the (stale) move target, NOT
    // feedback. Committed the moment the pair lands — fresher than the rest of
    // the snapshot, stamped so consumers can tell a new sample from a re-read.
    bool     enc_valid    = false;
    int32_t  enc_counts   = 0;
    uint32_t enc_stamp_ms = 0;    // millis() when this sample committed
};

// ---- Full config-register mirror (regs 0x00..0x19) -------------------------
// Populated by an on-demand async scan (requestConfigScan) — one register per
// transaction, same access pattern as telemetry. `known` is a bitmask of which
// registers actually answered (a drive variant may not implement 0x15+). :3
struct ServoConfig {
    bool     valid    = false;   // at least one full scan has completed
    bool     scanning = false;   // a scan is in flight right now
    uint32_t stamp_ms = 0;       // millis() when the last scan committed
    uint32_t known    = 0;       // bit N set = regs[N] answered on the last scan
    uint16_t regs[0x1A] = {0};   // raw register values 0x00..0x19
};

// ---- Alarm bitfield values (register 0x0E) per datasheet -------------------
//   0x10 = battery power-loss alarm (indication only — motor does not stop)
//   0x12 = overcurrent alarm (motor stops)
//   0x14 = stall / locked-rotor alarm (motor stops)
//   0x15 = overvoltage alarm (motor stops — regen from large inertial loads)
#define SERVO_ALARM_BATT          0x0010
#define SERVO_ALARM_OVERCURRENT   0x0012
#define SERVO_ALARM_STALL         0x0014
#define SERVO_ALARM_OVERVOLTAGE   0x0015

class ServoModbus {
public:
    /** @param port   HardwareSerial reference (e.g. Serial1) — must be set up
     *                 by the caller with the right baud/pins/config BEFORE init()
     *  @param addr   Modbus slave address (default 1 per datasheet) */
    ServoModbus(HardwareSerial& port, uint8_t addr = 1);

    ~ServoModbus();

    // ---- Lifecycle -----------------------------------------------------------
    /// Probe the slave address. Returns true if the drive answers.
    /// Caller MUST have already called port.begin(19200, SERIAL_8N1, rx, tx).
    bool init();

    /// Non-blocking poll. Call from Core 0 (httpTask / commsTask) at any rate;
    /// internally rate-limits to POLL_INTERVAL_MS. Thread-safe telemetry update.
    void update();

    /// Blocking single-register read — INIT CONTEXT ONLY (setup(), before the
    /// RTOS tasks are live; same exception as init()'s probe). Used by boot
    /// geometry adoption to read the drive's e-gear register. Returns true and
    /// fills `out` on a CRC-valid response.
    bool readRegisterBlocking(uint16_t reg, uint16_t& out, uint32_t timeout_ms = 80);

    /// Disable the drive output via Modbus write (FC 0x06, reg 0x01 = 0).
    void emergencyStop();

    // ---- Status --------------------------------------------------------------
    bool isReady() const { return _ready; }

    /// Thread-safe telemetry snapshot for the WebUI status handler.
    ServoTelemetry getTelemetry() const;

    // ---- Config writes (fire-and-forget — no readback validation) -------------
    void setEnable(bool on);       // reg 0x00
    void setOutput(bool on);       // reg 0x01
    void setSpeed(uint16_t rpm);   // reg 0x02
    void setAccel(uint16_t rpm_s); // reg 0x03
    void setDirPolarity(bool invert); // reg 0x09 (0=active-low, 1=active-high)
    void saveToFlash();            // reg 0x14

    // ---- Configure-pane plumbing (async, drained by update()) -----------------
    static constexpr size_t CFG_REG_COUNT = 0x1A;   // regs 0x00..0x19

    /// Snapshot of the config-register mirror (thread-safe copy).
    ServoConfig getConfig() const;

    /// Kick off an async scan of regs 0x00..0x19. One register per transaction
    /// at SCAN_INTERVAL_MS spacing (~0.7s total). No-op while already scanning.
    void requestConfigScan();

    /// Queue an FC 0x06 write, sent `repeat` times WRITE_SPACING_MS apart (the
    /// OSSM gold-motor tool writes each setting 3× as a serial-reliability
    /// workaround — same idea). Returns false if the queue is full or the
    /// drive never answered a probe. Writing reg 0x15 retargets _addr so the
    /// bus follows a device-address change. Verification is by rescan, never
    /// by trusting the echo — Ground Truth Doctrine applies to drives too. :3
    bool queueWrite(uint16_t reg, uint16_t value, uint8_t repeat = 1);

    /// Total writes (incl. repeats) still waiting to go out on the wire.
    size_t pendingWrites() const;

    uint8_t address() const { return _addr; }

private:
    HardwareSerial& _port;
    uint8_t         _addr;
    bool            _ready = false;

    // Telemetry cache — updated under mutex by update(), read by getTelemetry().
    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    ServoTelemetry       _telemetry;

    uint32_t _last_poll_ms = 0;
    // Per-register poll spacing. We cycle through the register list one read
    // at a time (gold-motor access pattern); 25ms spacing — the same pacing
    // the config scan has run reliably at since it shipped — gives a full
    // telemetry refresh every ~0.3s (8 singles + the encoder pair slot). :3
    static constexpr uint32_t POLL_INTERVAL_MS = 25;

    // When the boot-time probe failed (drive off, wiring wrong, no power on the
    // 36V rail yet), keep re-probing every few seconds instead of going silent
    // forever. This also makes the RS485 module's TX LED blink periodically,
    // which is exactly what you need to debug wiring — no TX light at all
    // means the frames never leave the ESP32. :3
    static constexpr uint32_t REPROBE_INTERVAL_MS = 3000;

    // ---- State machine for non-blocking multi-register reads ------------------
    // A single update() cycle does NOT block: we send the request, and on the
    // next call we read the response if enough bytes have arrived. The 2 Hz
    // cadence gives the UART ~500ms to fill the RX buffer — it takes ~4ms at
    // 19200 baud for a 6-register read, so there's zero risk of timeout. :3
    enum class RxState : uint8_t {
        IDLE,       // nothing in flight
        WAITING,    // sent request, waiting for response bytes
        DONE        // have bytes, process next update()
    };
    RxState _rx_state = RxState::IDLE;
    uint32_t _rx_start_ms = 0;
    static constexpr size_t MAX_RSP_LEN = 32;  // addr + func + bytecount + 6*2 + crc
    uint8_t  _rx_buf[MAX_RSP_LEN];
    size_t   _rx_expected = 0;

    // Single-register poll cycle (matches the gold-motor tool's access pattern):
    // 0x00 enable, 0x01 output, 0x0E alarm, 0x0F current, 0x10 speed,
    // 0x11 voltage, 0x12 temp, 0x13 PWM. Values land in _staged[] and commit
    // to _telemetry under the spinlock once a full cycle completes. After the
    // 8 singles, _reg_idx == POLL_REG_COUNT is the encoder-pair slot (and
    // POLL_REG_COUNT+1 the HI half in single-read fallback mode). :3
    static constexpr size_t POLL_REG_COUNT = 8;
    static const uint16_t POLL_REGS[POLL_REG_COUNT];
    size_t   _reg_idx = 0;
    uint16_t _staged[POLL_REG_COUNT] = {0};

    // Which kind of read is in flight — a WAITING response routes to the
    // telemetry stager, the config-scan stager, or the encoder assembler. :3
    enum class PendingKind : uint8_t { TELE, SCAN, ENC_PAIR, ENC_LO, ENC_HI };
    PendingKind _pending_kind = PendingKind::TELE;

    // ---- Encoder position read (regs 0x16/0x17) ------------------------------
    // Preferred path is ONE FC 0x03 read of count=2 so the LO/HI words are a
    // consistent snapshot (a pair torn across two transactions mid-motion can
    // be off by 65536 counts = 2 motor revs). The 57AIM30 on the bench answers
    // multi-register reads; if a drive variant doesn't (the gold tool never
    // batches), 3 consecutive pair timeouts latch _enc_single_mode and we fall
    // back to two back-to-back single reads — tear-prone only while moving,
    // and the validator gates its verdicts on standstill anyway. :3
    static constexpr uint16_t REG_ENC_LO = 0x16;
    static constexpr uint16_t REG_ENC_HI = 0x17;
    bool     _enc_single_mode = false;
    uint8_t  _enc_pair_fails  = 0;
    uint16_t _enc_lo_staged   = 0;

    /// Commit a freshly assembled encoder sample under the spinlock.
    void _commitEncoder(int32_t counts, uint32_t now);

    // ---- Config scan state (regs 0x00..0x19, on demand) -----------------------
    // Faster spacing than telemetry (a scan is a user-facing "Read registers"
    // action): 26 regs × 25ms ≈ 0.7s. A register that times out 3× is marked
    // unknown (bit cleared in `known`) and skipped — some drive variants stop
    // at 0x14. Committed as one snapshot under the spinlock. :3
    static constexpr uint32_t SCAN_INTERVAL_MS = 25;
    bool     _scan_active  = false;
    size_t   _scan_idx     = 0;
    uint8_t  _scan_retries = 0;
    uint32_t _scan_known   = 0;
    uint16_t _cfg_staged[CFG_REG_COUNT] = {0};
    ServoConfig _cfg;              // committed mirror — guarded by _mux

    // ---- Write queue (drained by update(), WRITE_SPACING_MS apart) ------------
    // Fire-and-forget FC 0x06 frames; the drive's echo is discarded (flushed by
    // the next request). Writes preempt reads but only launch from IDLE so an
    // echo can never masquerade as a poll response. emergencyStop() clears the
    // queue — never keep programming through an e-stop. :3
    struct WriteOp { uint16_t reg; uint16_t val; uint8_t rep; };
    static constexpr size_t   WRITE_Q_LEN     = 64;
    static constexpr uint32_t WRITE_SPACING_MS = 20;
    WriteOp  _wq[WRITE_Q_LEN];
    size_t   _wq_head = 0, _wq_count = 0;
    uint32_t _last_write_ms = 0;

    // ---- Modbus primitives ---------------------------------------------------
    uint16_t crc16(const uint8_t* buf, size_t len) const;

    /// Send FC 0x03 read-holding-registers frame; returns expected response length.
    /// Response is picked up by tryReadResponse() on the next update() cycle.
    size_t sendReadRequest(uint16_t reg, size_t count);

    /// Check if a complete response has arrived; if so, parse it into out[].
    /// Returns true when a valid response was decoded.
    bool tryReadResponse(uint16_t* out, size_t count);

    /// Fire-and-forget FC 0x06 write — we don't wait for the echo. :3
    void sendWriteCommand(uint16_t reg, uint16_t value);

    /// Advance the config scan; commits the mirror when the last reg lands.
    void _scanAdvance(uint32_t now);
};

#endif // defined(FEATURE_RS485_MODBUS)