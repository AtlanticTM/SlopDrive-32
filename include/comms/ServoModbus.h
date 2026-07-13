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

private:
    HardwareSerial& _port;
    uint8_t         _addr;
    bool            _ready = false;

    // Telemetry cache — updated under mutex by update(), read by getTelemetry().
    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    ServoTelemetry       _telemetry;

    uint32_t _last_poll_ms = 0;
    // Per-register poll spacing. The known-working OSSM "gold motor" tool only
    // ever reads ONE register per transaction — these drives commonly ignore
    // multi-register FC 0x03 requests entirely. So we cycle through the
    // register list one read at a time; 100ms spacing → full telemetry
    // refresh every ~0.8s. :3
    static constexpr uint32_t POLL_INTERVAL_MS = 100;

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
    // to _telemetry under the spinlock once a full cycle completes. :3
    static constexpr size_t POLL_REG_COUNT = 8;
    static const uint16_t POLL_REGS[POLL_REG_COUNT];
    size_t   _reg_idx = 0;
    uint16_t _staged[POLL_REG_COUNT] = {0};

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
};

#endif // defined(FEATURE_RS485_MODBUS)