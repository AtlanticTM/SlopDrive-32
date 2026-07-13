#pragma once

// ============================================================================
// StatusLeds — onboard LED feedback for the custom v0.0 controller (Nano ESP32)
// ============================================================================
// Three jobs, three lamps:
//   * Heartbeat (GPIO28, ACTIVE-HIGH): smooth PWM "breathing" so a glance at
//     the board proves the S3 is alive and scheduling — a hard fault freezes
//     the breath mid-inhale, which is instantly obvious.
//   * Amber user LED (GPIO48, ACTIVE-LOW): short pulse every time the WebUI
//     issues a command (move/home/settings/gen/...) — visual RX confirmation.
//   * RGB discrete LEDs (GPIO46/0/45, ACTIVE-LOW): machine state at a glance:
//       RED solid    — not homed (or E-stopped) — don't trust position
//       BLUE blink   — homing sweep in progress
//       GREEN solid  — homed & idle, ready to play
//       CYAN solid   — homed & actively streaming motion (TCode or generator)
//       MAGENTA      — paused / manual override (host input gated)
//
// All functions are cheap and non-blocking (.clinerules §2). update() is meant
// to be called from a Core 0 housekeeping loop (~10ms cadence). :3

struct SystemState;

// Configure pins + LEDC channel. Call ONCE from setup(), after boot has
// settled — GPIO0 (green) is a strapping pin and must not be driven earlier.
void statusLedsInit();

// Advance the breathing animation and refresh the RGB/amber outputs from the
// current system state. Non-blocking; call at ~10ms-100ms cadence on Core 0.
void statusLedsUpdate(const SystemState& state);

// Kick the amber activity pulse — call from WebUI command handlers whenever
// an operator command is received. Safe from any core (32-bit store). :3
void statusLedsActivity();