#pragma once

#include <Arduino.h>

// =============================================================================
// Secrets (WiFi password, network addresses) — kept OUT of git like a good
// pup keeps its leash on. No accidental exposure here. :3
// =============================================================================
// Real values live in include/secrets.h (git-ignored). If that file is missing
// (e.g. a fresh clone before you copy secrets.example.h -> secrets.h), we fall
// back to harmless placeholders so the project still compiles.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #warning "include/secrets.h not found - copy secrets.example.h to secrets.h and edit it. Using placeholder WiFi values."
  #define SECRET_WIFI_SSID       "CHANGE_ME_SSID"
  #define SECRET_WIFI_PASSWORD   "CHANGE_ME_PASSWORD"
  #define SECRET_INTIFACE_HOST   "192.168.1.100"
  #define SECRET_INTIFACE_PORT   54817
#endif

// =============================================================================
// WiFi Configuration (values come from secrets.h)
// =============================================================================
#define WIFI_SSID      SECRET_WIFI_SSID
#define WIFI_PASSWORD  SECRET_WIFI_PASSWORD


// =============================================================================
// Device Geometry
// =============================================================================
// Physical travel limit in millimeters
#define PHYSICAL_MAX_TRAVEL_MM  240.0f

// Stepper + belt mechanics:
//   NEMA motor: 200 steps/rev
//   TMC microsteps: configured via driver (see MICROSTEPS below)
//   Pulley: 20 teeth, 2mm pitch → 40mm per motor revolution
#define MOTOR_STEPS_PER_REV     200
#define PULLEY_TEETH            20
#define BELT_PITCH_MM           2.0f
#define MM_PER_MOTOR_REV        (PULLEY_TEETH * BELT_PITCH_MM)  // 40mm

// =============================================================================
// Motor Driver Pins (ESP32-S3) - MATCH ORIGINAL STROKEENGINE WIRING
// =============================================================================
#define PIN_STEP                21	 // Step signal
#define PIN_DIR                 20	 // Direction signal
#define PIN_ENABLE              19	 // Enable signal (active low)
#define PIN_ENDSTOP             47	 // Endstop limit switch (active LOW, normally open to GND)

#define PIN_TMC_CS              11	 // TMC Chip Select
#define PIN_TMC_SCLK            12	 // TMC Clock
#define PIN_TMC_MOSI            13	 // TMC Master Out Slave In
#define PIN_TMC_MISO            10	 // TMC Master In Slave Out

// =============================================================================
// RGB Status LED (NeoPixel)
// =============================================================================
#define PIN_NEOPIXEL_PIN        48
#define NEOPIXEL_COUNT          1

// =============================================================================
// Motor Defaults
// =============================================================================
// Microsteps on the TMC2160 (set via driver config tab, saved in EEPROM)
#define MICROSTEPS              16

// Steps per mm = (MOTOR_STEPS_PER_REV * MICROSTEPS) / MM_PER_MOTOR_REV
//              = (200 * 16) / 40 = 80 steps/mm
#define STEPS_PER_MM            ((MOTOR_STEPS_PER_REV * MICROSTEPS) / MM_PER_MOTOR_REV)

// Maximum motor speed in mm/s
#define MAX_SPEED_MM_S              550.0f
#define DEFAULT_MAX_SPEED_MM_S      550.0f   // alias for config defaults

// Default acceleration mm/s^2
// Needs to be high enough to cover a full stroke in the shortest expected
// interval. For a 120mm stroke at 250ms: a = 4×dist/T² = 4×120/0.0625 = 7680.
// 8000 gives comfortable margin. The planner uses this as the cruise accel;
// the raise-only guard in streamTo() keeps it from softening mid-flight. :3
#define DEFAULT_ACCEL_MM_S2     8000.0f


// =============================================================================
// TMC2160 Driver Defaults
// =============================================================================
#define TMC_R_SENSE             0.15f   // Ohms (match original StrokeEngine board)
#define TMC_RUN_CURRENT_MA      2000     // mA (default run current for TMC2160)
#define TMC_STALLGUARD_DMA      -64
#define TMC_TOFF                3        // off-time regulation (match original)
#define TMC_TSTEP_REG           255
#define TMC_HOLD_CURRENT_PCT    50       // % of run current while idle
#define TMC_TBL                 2        // blank time code (1 = 24 clocks, typical)
#define TMC_STEALTHCHOP         0        // 0 = SpreadCycle (more torque), 1 = quiet
#define TMC_TPWM_THRS           0        // 0 = never auto-switch stealth<->spread
#define TMC_HSTART              5        // chopper hysteresis start
#define TMC_HEND                1        // chopper hysteresis end


// =============================================================================
// Serial Control Mode
// =============================================================================
// This flag no longer gates runtime input — the active transport (WS/SER/BT
// selected in the web UI and persisted to NVS) is the single source of truth.
// SERIAL_CONTROL_MODE is now used ONLY to:
//   1. Set the factory-default transport mode (1 → SER, 0 → WS).
//   2. Route debug output: when ON, debug goes to the in-memory web log
//      (applog) so it doesn't spray all over the USB TCode stream like an
//      overexcited pup. Keep that stream clean for Intiface. :3
//   3. Announce serial-control status via /api/status → serial_mode.
#define SERIAL_CONTROL_MODE     1            // 1 = USB Serial is default transport, 0 = WiFi
#define SERIAL_CONTROL_BAUD     115200       // must match Intiface's serial port

// =============================================================================
// Buttplug WebSocket Port
// =============================================================================
// Port the ESP32 runs its OWN WebSocket server on (for MultiFunPlayer, which
// connects TO the device and streams raw TCode).
#define BUTTPLUG_WEBSOCKET_PORT 55555

// =============================================================================
// Intiface WSDM (Websocket Device Manager) Client
// =============================================================================
// When Intiface's "Device WebSocket Server" toggle is ON, Intiface LISTENS for
// devices to connect to IT. The ESP32 connects out to this host:port as a
// WebSocket CLIENT, sends the identification handshake, then exchanges TCode.
//
// Set INTIFACE_HOST to the IP of the PC running Intiface, and INTIFACE_PORT to
// the port shown in Intiface's log ("Listening on: 0.0.0.0:<port>"). The port
// can change between Intiface launches - update it or set via the web UI.
#define INTIFACE_HOST          SECRET_INTIFACE_HOST  // <-- from secrets.h
#define INTIFACE_PORT          SECRET_INTIFACE_PORT  // <-- from secrets.h
#define INTIFACE_ENABLED       true             // enable WSDM client connection

// Identification handshake sent as the FIRST message after connecting.
// identifier: must match the protocol selected in Intiface's websocket device
//             dropdown. For TCode v0.3 stroker this is "tcode-v03".
// address:    arbitrary unique string to identify this device across sessions.
#define INTIFACE_IDENTIFIER    "tcode-v03"
#define INTIFACE_ADDRESS       "slopdrive32-0001"

// TCode magnitude scaling — DEPRECATED, NO LONGER USED IN THE DECODE PATH. :3
//
// Old assumption (WRONG): Intiface emits the magnitude against a fixed 0–999
// scale unpadded ("L086" = 86/999), so we divided by this constant. That broke
// the moment a sender used more (or fewer) digits — a 5-digit value like 50000
// got divided by 999 = 50.0, clamped to 1.0, and every fast high-precision
// stroke slammed into the wall.
//
// Correct TCode v0.3 (what we do now, in TCodeParser): the magnitude is an
// IMPLICIT DECIMAL FRACTION of arbitrary length — strip the axis+channel, then
// treat the entire remaining digit string as if prefixed with "0." So:
//   L0500    → 0.500       (leading zeros are decimal placeholders!)
//   L0086    → 0.086
//   V0010000 → 0.010000
// The divisor is 10^(digit count), never a fixed magic number, which is
// immune to Intiface's occasional extra-leading-zero padding glitch. Kept here
// only because docs / the Intiface device-config JSON still reference the
// historical [0,999] range. Don't wire it back into the parser. :3
#define TCODE_MAGNITUDE_MAX    999.0f


// Max fractional digits we KEEP when decoding a TCode v0.3 magnitude. The spec
// puts no ceiling on how many digits a sender can cram after the channel
// (L0500 = 0.5, L050000 = 0.5 too — just more precision), so we truncate
// anything past this. 6 digits = ~1 part in a million, comfortably finer than a
// float's ~7 significant figures and WAY finer than the stepper can physically
// resolve. Capping here also keeps mag_value safely inside uint32 no matter how
// long a greedy app makes the value. Take six, spit the rest — that's all this
// good boy can swallow without overflowing. :3
#define TCODE_MAGNITUDE_MAX_DIGITS  6



// =============================================================================
// Transport Mode — three ways to get dirty talk into this machine. :3
// =============================================================================
// The machine can receive TCode over three transports. Exactly ONE is active
// at a time; the user picks it in the web UI (Settings) and we remember it
// in NVS (we're loyal like that). The status chip shows WS / SER / BT.
//   WS  = WebSocket. The ESP32 runs a TCode WebSocket server (MultiFunPlayer
//         connects in) AND can connect out to Intiface's WSDM server. Needs WiFi.
//         The social butterfly of transports — always making new connections.
//   SER = USB Serial. Intiface's "serialport" comm manager streams TCode over
//         USB. Lowest latency, no WiFi needed. The direct, no-nonsense top.
//   BT  = Bluetooth LE. The ESP32 advertises a Nordic-UART-style BLE service;
//         a BLE host (phone app / Intiface BLE) writes TCode to the RX
//         characteristic. Wireless, personal, intimate. No WiFi needed.
enum class TransportMode : uint8_t {
    WS  = 0,
    SER = 1,
    BT  = 2,
};

// Default transport on a fresh device (before any saved selection). SER keeps
// backwards-compatible behavior with the old SERIAL_CONTROL_MODE build flag.
#ifndef DEFAULT_TRANSPORT_MODE
  #if SERIAL_CONTROL_MODE
    #define DEFAULT_TRANSPORT_MODE  TransportMode::SER
  #else
    #define DEFAULT_TRANSPORT_MODE  TransportMode::WS
  #endif
#endif

// =============================================================================
// Bluetooth LE (BLE) Transport
// =============================================================================
// Advertised device name (what shows up in a BLE scanner / Intiface).
#define BLE_DEVICE_NAME        "SlopDrive-32"
// Nordic UART Service (NUS) UUIDs - the de-facto "BLE serial" profile. A host
// writes TCode bytes to the RX characteristic; we notify TCode replies on TX.
#define BLE_NUS_SERVICE_UUID   "8a846175-ea22-4411-88f5-9a8afcc20671"
#define BLE_NUS_RX_CHAR_UUID   "8a846175-ea22-4411-88f5-9a8afcc20672"  // host -> device (write)
#define BLE_NUS_TX_CHAR_UUID   "8a846175-ea22-4411-88f5-9a8afcc20673"  // device -> host (notify)


// =============================================================================
// HTTP Server Port
// =============================================================================

#define HTTP_SERVER_PORT        80
#define HTTP_PORT               80           // alias used in main.cpp

// MDNS service name
#define MDNSServiceName         "slopdrive32"

// =============================================================================
// Control Mode
// =============================================================================
enum class ControlMode : uint8_t {
    MANUAL = 0,     // Web UI manual control
    BUTTPLUG = 1    // Buttplug/Intiface WebSocket control
};

// =============================================================================
// Device State for Configuration (persisted to EEPROM/NVS)
// =============================================================================
struct DeviceConfig {
    // Range mapping (mm) - where buttplug 0.0 and 1.0 map to physically
    float min_position_mm;     // default: 0mm (rearmost)
    float max_position_mm;     // default: 240mm (forwardmost)

    // Speed limit (mm/s) - caps all motion regardless of source
    float max_speed_mm_s;      // default: 550

    // Acceleration (mm/s^2)
    float acceleration_mm_s2;  // default: 1500

    // Control mode
    uint8_t control_mode;      // 0=Manual, 1=Buttplug

    // TMC Driver settings (live-tunable from the Motor tab)
    uint16_t microsteps;       // 1/2/4/8/16/32/64/128/256 - smoothness vs torque
    uint16_t run_current_ma;   // motor RMS current while moving (torque + heat)
    uint8_t  hold_current_pct; // % of run current when idle (holding torque/heat)
    int8_t   stallguard_dma;   // StallGuard threshold (sensorless load detect)
    uint8_t  toff;             // chopper off-time / driver enable (1-15, 0=off)
    uint8_t  tbl;              // chopper blank time code 0-3 (16/24/36/54 clocks)
    uint8_t  stealthchop;      // 1 = StealthChop (quiet), 0 = SpreadCycle (torque)
    uint32_t tpwm_thrs;        // TSTEP velocity threshold to flip stealth->spread
    int8_t   hstart;           // chopper hysteresis start (0-7)
    int8_t   hend;             // chopper hysteresis end (-3..12)

    // Manual mode settings
    float manual_depth;        // 0.0-1.0 position within trimmed range
    float manual_speed;        // 0.0-1.0 speed ratio

    // Checksum for validation (simple XOR of all bytes)
    uint32_t checksum;
};

// Default configuration values
inline DeviceConfig getDefaultConfig() {
    DeviceConfig cfg;
    cfg.min_position_mm = 0.0f;
    cfg.max_position_mm = PHYSICAL_MAX_TRAVEL_MM;
    cfg.max_speed_mm_s = MAX_SPEED_MM_S;
    cfg.acceleration_mm_s2 = DEFAULT_ACCEL_MM_S2;
    cfg.control_mode = (uint8_t)ControlMode::BUTTPLUG;
    cfg.microsteps = MICROSTEPS;
    cfg.run_current_ma = TMC_RUN_CURRENT_MA;
    cfg.hold_current_pct = TMC_HOLD_CURRENT_PCT;
    cfg.stallguard_dma = TMC_STALLGUARD_DMA;
    cfg.toff = TMC_TOFF;
    cfg.tbl = TMC_TBL;
    cfg.stealthchop = TMC_STEALTHCHOP;
    cfg.tpwm_thrs = TMC_TPWM_THRS;
    cfg.hstart = TMC_HSTART;
    cfg.hend = TMC_HEND;
    cfg.manual_depth = 0.5f;
    cfg.manual_speed = 0.3f;
    return cfg;
}

// Compute usable travel range in mm
inline float getUsableRange(const DeviceConfig& cfg) {
    return cfg.max_position_mm - cfg.min_position_mm;
}

// Map a normalized value (0.0-1.0) to physical position in mm
inline float mapToPosition(float normalized, const DeviceConfig& cfg) {
    normalized = constrain(normalized, 0.0f, 1.0f);
    return cfg.min_position_mm + normalized * getUsableRange(cfg);
}

// Map a physical position (mm) to normalized value (0.0-1.0)
inline float mapFromPosition(float pos_mm, const DeviceConfig& cfg) {
    float range = getUsableRange(cfg);
    if (range <= 0.0f) return 0.0f;
    return constrain((pos_mm - cfg.min_position_mm) / range, 0.0f, 1.0f);
}

// Map normalized speed to actual mm/s
inline float mapToSpeed(float normalized, const DeviceConfig& cfg) {
    normalized = constrain(normalized, 0.0f, 1.0f);
    return normalized * cfg.max_speed_mm_s;
}