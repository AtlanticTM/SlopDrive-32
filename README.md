# SlopDrive-32

An open-source ESP32-S3 / ESP32-C5 firmware for a capstan-drum linear
stroke machine, controlled over [Intiface](https://intiface.com/) /
[buttplug.io](https://buttplug.io) with a rich web UI for configuration and
real-time telemetry. The custom v0.0 controller PCB carries a main ESP32-S3
brain plus an onboard ESP32-C5-Zero coprocessor, paired with an external
USB dongle (also an ESP32-C5) that bridges the host PC's TCode stream to the
machine wirelessly.

The firmware maps incoming normalized position commands (`0.0`–`1.0`) from the
buttplug/TCode ecosystem onto a user-configurable slice of the machine's physical
travel, so you can move and resize the active stroke region anywhere across the full
mechanical range without touching the firmware or recompiling.

---
---

## ⚠️ Disclaimer: this project was vibecoded

Full honesty: **this entire project was "vibecoded."** I am not smart enough to
have figured this stuff out on my own — it was put together iteratively with the
heavy assistance of AI, through a lot of "make it do the thing" and "why doesn't
the thing work." It builds and it runs, but:

- There may be bugs, rough edges, or subtly wrong assumptions.
- The code may not follow best practices in every place.
- I cannot promise I fully understand every line of it.

It started life as a fork of concepts from the **StrokeEngine** project (an open
source effort by the OSSM community), which worked but felt clunky for this use
case — so it was reworked around Intiface/buttplug control, then later grew a
StrokeEngine-derived pattern engine and an OSSM-compatible BLE layer of its own
(see [Communication & Control](#communication--control) below). Use at your own
risk, review before flashing, and don't trust it with anything you care about.
**No warranty, express or implied. You are responsible for your own hardware and
safety.**

---

## Hardware Overview

The machine is built around one custom PCB (**v0.0**) carrying two microcontrollers,
plus one separate external accessory:

| Device | MCU | Role |
|--------|-----|------|
| **Main Controller** | Arduino Nano ESP32 module (u-blox NORA-W106 / ESP32-S3, 16MB flash, 8MB PSRAM) | The brain. Runs WiFi, mDNS, the HTTP/WebSocket servers, LittleFS web UI, TCode parsing, BLE transports, OTA, and — on Core 1 — the motion arbiter and 57AIM30 servo step pulse generation. |
| **Onboard Coprocessor** | ESP32-C5-Zero, soldered to the same v0.0 PCB | Receives TCode wirelessly over ESP-NOW from the T-Dongle C5 and relays it to the main controller over a physical UART wire. Under **active development**; a small OLED + buttons/encoder for local control is planned but not yet built. |
| **T-Dongle C5** | ESP32-C5 (LilyGO T-Dongle C5), separate external USB stick | Plugs into the host PC. MultiFunPlayer/Intiface talk TCode to it over USB-CDC; it relays that stream over ESP-NOW (5GHz) to the onboard coprocessor above. Drives its own ST7735 display showing live position and command frequency. Under **active development**. |
| *(Planned)* **Handheld Remote** | ESP32-C5, board TBD | A wireless remote with physical knobs/buttons for direct manual control. **Not started** — no code, board definition, or firmware exists yet; this is a future idea, not in-progress work. |

The original bare ESP32-S3 DevKitC-1 build (belt+pulley drive, endstop switch,
NeoPixel status LED) was the **prototype** hardware and is no longer the target —
it's kept as a legacy PlatformIO build environment (`esp32-s3-devkitc-1`) for
reference, but the v0.0 PCB above is the real machine. See
[Hardware](#hardware) for full specs.

### SharedProtocol — a scaffold, not (yet) the wire format

`lib/SharedProtocol/SharedProtocol.h` defines a hardware-agnostic binary packet
protocol (`NodeRole` enum, `MessageType` enum, a `PacketHeader` with `{'S','D'}`
magic bytes + checksum, `MotionCommand`/`MotionState` structs) intended as the
eventual common tongue between all nodes. **It isn't actually used yet** — it's
`#include`d only by `src/s3_main/main.cpp`, an 18-line placeholder stub that is
deliberately excluded from every real build (`src/main.cpp` is the live main-
controller entry point; see [Project Layout](#project-layout)). The real
node-to-node links today are:

- **T-Dongle C5 → onboard C5-Zero**: raw ESP-NOW packets carrying ad-hoc structs
  (bundled TCode text fragments + a bitmask ACK scheme), on a configurable 5GHz
  channel (`SECRET_ESPNOW_CHANNEL`, `SECRET_ESPNOW_PEER_MAC` in `secrets.h`).
- **Onboard C5-Zero → main controller**: plain TCode text over a physical UART
  wire (`DongleTransport`, GPIO 43/44 at 460800 baud) — the C5 side looks just
  like a USB-serial TCode source to the main controller's parser.

SharedProtocol is real, compiled, and structurally sound — it's just not wired
into either link yet.

---

## Architecture

SlopDrive-32 is built on a **dual-core, modular architecture** targeting the
ESP32-S3's twin Xtensa LX7 cores (and single-core operation on the ESP32-C5
nodes):

| Core | Responsibility |
|------|---------------|
| **Core 0** (System & Comms) | WiFi (AP+STA), mDNS, HTTP server, both WebSocket servers (TCode + binary UI control plane), LittleFS asset serving, JSON REST API, OTA, BLE transports, TCode/OSSM parsing, asynchronous system monitoring |
| **Core 1** (Motion Real-Time) | `MotionArbiter` dispatch, `MotionInterpolator` sampling, `PatternEngine` playback, FastAccelStepper step pulse generation, homing, e-stop handling |

### Thread-Safe Data Sharing

Cross-core state uses a mix of primitives chosen per field: `std::atomic` for
values needing read-modify-write semantics or multi-field consistency (e-stop
flag, OTA-in-progress flag, live position), plain `volatile` for single 32-bit-
aligned scalars where the ESP32-S3's hardware guarantees atomic load/store
(homing flags, transport mode, generator-active flag), `portMUX_TYPE` spinlocks
for multi-field generator config, and `xQueue`/FreeRTOS queues for cross-core
work handoff (the interpolator's sample queue, `MotionArbiter`'s internal
deferred-intent queue for Core-0-originated commands). No naked unsynchronized
shared access.

### Hardware Abstraction

All physical hardware interactions are abstracted behind clean C++ interface
classes (Abstract Base Classes with pure virtual functions):

- **`MotorDriver`** — abstracts stepper/servo communication, including bus
  current/voltage/power telemetry getters. Implemented today by
  `Ai57AIMServoDriver` (the 57AIM30 closed-loop servo, current production
  driver) and `TMC2160StepperDriver` (the legacy belt-drive stepper build).
  **`MotionArbiter` is the only class permitted to call a driver's motion
  methods** (`moveTo`/`streamTo`/`streamToSteps`/`stop`/`hardStop`) — this is
  enforced at compile time via a `friend class MotionArbiter` declaration on
  `MotorDriver` itself, not just convention. The interface is structured for
  further community extensions — CANopen drives, external pulse generators,
  whatever hardware you strap to the rail.
- **`Transport`** — abstracts communication transport (Serial, BLE, WebSocket,
  Dongle UART relay, OSSM BLE). `TransportManager` routes data between the
  active transport and the TCode parser; exactly one transport is active at a
  time, selectable from the web UI.
- Build-flags (`-DDRIVER_57AIM_SERVO`, `-DBLE_ENABLED`, `-DFEATURE_RS485_MODBUS`)
  gate driver/feature code inclusion; unused drivers are not compiled into the
  binary.

---

## Features

### Motion Engine

Motion is **event-driven, never clocked** — "D4 doctrine": one command → one
plan → FastAccelStepper executes (its ISR is the sample rate). There is no
motion tick and no chase loop; every position intent is translated once, at
arrival, into a plan computed from the machine's actual live position and
velocity, with speed/accel derived from what the intent requires and clamped
at configured ceilings.

- **`Ai57AIMServoDriver`** — the production motor driver: a "dumb" step/direction
  pulse generator for the 57AIM30 closed-loop servo drive, via FastAccelStepper.
  No SPI, no Modbus, no register config for motion itself — the 57AIM30 handles
  closed-loop control internally via its own DSP; we just send PUL/DIR pulses.
  On the v0.0 board this drives a **capstan drum + Dyneema line**, not a belt:
  25mm drum diameter, 800 steps/rev at the motor shaft (DIP-switch configured)
  through a 2:1 reduction → 1600 steps per drum revolution → ~78.54mm travel per
  drum revolution → **~20.37 steps/mm** (kept as a float on purpose — truncating
  to a flat 20 would drift the carriage over a long stroke).

- **`MotionArbiter`** — the sole caller of the motor driver, arbitrating four
  intent sources (`MANUAL`, `TCODE_STREAM`, `PATTERN`, `OSSM_STREAM`) and owning
  every safety gate: homed, paused, e-stop (absolute, no source bypasses it),
  manual-override yield, window clamping, and a soft-start speed ramp on any
  motion discontinuity (new stream connection, un-pause, manual-override
  release, etc. — `SAFE_APPROACH_SPEED_MM_S` ramping up over
  `SAFE_RESUME_RAMP_MS`). It also selects between two independently-configurable
  limit sets: a **user set** (manual/UI moves) and an **input set** (TCode
  streams, patterns, OSSM) — each with its own max speed/accel. Manual moves may
  target the full physical travel; every other source is clamped to the user's
  configured stroke window. Pattern-sourced intents additionally yield to any
  TCode/Intiface activity within the last 250ms, so live streaming always wins
  over idle pattern playback.

- **`MotionInterpolator`** — handles the TCode streaming path specifically: a
  cubic Hermite curve generator (ported from TempestMAx's OSR2/SR6 `Axis`
  library, MIT) that builds C1-continuous curves between TCode points, sampled
  at ~1kHz on Core 1. Supports both bare v0.3-style points (with live-mode
  extrapolation from measured inter-command cadence) and v0.4 points carrying an
  explicit tangent slope (see TCode v0.4 below), with monotone tangent limiting
  to avoid overshoot. An anomaly ring buffer flags overshoot/dropped-point/decel-
  overrun/duration-fallback events, surfaced to the web UI. Manual moves and
  `PatternEngine` segments go through `MotionArbiter`'s own geometric trapezoid
  math instead — "predictive interpolation" is really two purpose-specific
  mechanisms depending on where the intent came from, not one unified planner.

- **`PatternEngine`** — a StrokeEngine-derived pattern player (vendored MIT
  pattern math from `theelims`/KinkyMakers OSSM-hardware), event-driven like
  everything else: it asks the active pattern for the next stroke segment,
  submits one `MotionIntent` to the arbiter, and sleeps for that segment's
  derived duration. Ships 7 core patterns matching upstream OSSM naming —
  **Simple Stroke, Teasing Pounding, Robo Stroke, Half'n'Half, Deeper, Stop'n'Go,
  Insist** — plus two optional extended patterns behind build flags. Controlled
  via speed/depth/stroke/sensation percentages, same as OSSM's own API shape.

- **Continuous Motion Blending** — `MotorDriver::streamTo()` supports three
  named blend modes (let-it-land / allow-reversal / hybrid), but at the
  `MotionArbiter` layer only **allow-reversal** is fully live: FastAccelStepper's
  native velocity-continuous retargeting handles reversals directly, so
  let-it-land and hybrid are accepted-but-aliased to allow with a deprecation
  log, and are slated for removal.

- **Sensorless Homing** — the v0.0 board has **no endstop switch**. A
  self-contained FreeRTOS task sweeps the carriage to both hard stops, detecting
  a stall via a sustained current spike on the INA228 current sensor (see
  below), backs off 10mm from the rear stop to zero, and records the actually-
  measured usable stroke (which can be shorter than the 260mm geometry ceiling).
  Homing must be explicitly triggered from the web UI — the machine never moves
  on power-up. The old endstop-switch push-to-home path still exists in code but
  is compiled out by default (`HOMING_USE_ENDSTOP` isn't defined in any current
  build) — it only applies to the legacy belt-drive/DevKitC-1 build.

- **Bus Current/Voltage/Power Telemetry** — a `CurrentSensor` wrapper around an
  INA228 20-bit high-side monitor on the 36V motor bus (behind an ISO1640
  isolator), the same sensor that makes sensorless homing possible. Live current,
  voltage, power, die temperature, peak current, and cumulative energy are
  exposed through `MotorDriver`'s telemetry getters and surfaced in the web UI's
  Health tab.

- **Deceleration Guard** — when a commanded target moves faster than the
  hardware can physically accelerate to, the arbiter's raise-only acceleration
  guard softens the approach rather than slamming into the limit. The web UI's
  diagnostics graph makes the resulting target-vs-actual divergence visible.

- **Range Mapping** — incoming normalized `0.0`–`1.0` commands are remapped
  onto a user-trimmable window of the physical rail:

  ```text
  physical_mm = min_position_mm + normalized × (max_position_mm − min_position_mm)
  ```

- **E-Stop** — an atomic emergency-stop flag immediately halts all motion on
  Core 1, hard-stops the driver, and drains any deferred intents queued from
  Core 0. No source — not even manual — can bypass it.

- **Legacy pipeline, retained but dormant** — the older tick-rate generator
  (`Kinematics.h`'s trapezoid planner, `PositionTime`/`g_waypoint_queue`/
  `motionConsumerTask`, and the configurable `gen_rate_tick_hz` setting with its
  OSSM-style up-to-25%-latency-compensation logic) is still present in the
  source tree but compiled out by default — it only activates under a
  `LEGACY_STREAM_PIPELINE` build flag that no current environment defines. The
  `gen_rate_tick_hz` field is still persisted/exposed in the web UI for
  diagnostics-cadence purposes only; it no longer governs any live motion loop.
  **Known gap:** the OSSM BLE transport's continuous `stream:` command (distinct
  from its pattern-playback commands, which work fine) currently pushes into
  this same dormant queue — so BLE position streaming from apps like XToys isn't
  wired to a live consumer in the default build yet. This is acknowledged WIP,
  not a documented-as-working feature.

### Communication & Control

- **TCode v0.4** (wire-compatible with v0.3) — the parser now supports a
  runtime **axis registry** (only the stroke axis, `L0`, is registered by
  default; other axes are opt-in), a **`D2`** command that dynamically
  enumerates whatever's actually registered instead of a hardcoded reply, and a
  **`G<slope>`** extension carrying MultiFunPlayer's v0.4 interpolation tangent
  (the "MFP slope"), which `MotionInterpolator` uses to shape a smooth Hermite
  curve through each point instead of just extrapolating. `D1` reports
  `"TCode v0.4"`. Plain unslowed v0.3 lines (`L0500`) still work exactly as
  before. Magnitude decoding is an **implicit decimal fraction** of the digit
  string (`L0500` → 0.500, `L050000` → 0.5 with more precision) rather than a
  fixed divisor, so arbitrary digit counts don't clip or overflow — truncated
  at 6 digits, which is far beyond any real-world magnitude precision.

  The device still **identifies itself to Intiface as `tcode-v03`** — this is
  deliberate, not a bug: Intiface Central has no v0.4 protocol entry, and v0.4
  is a strict wire-compatible superset, so identifying as v0.3 is how a v0.4
  device stays plug-and-play with the existing ecosystem.

- **Six transport modes**, exactly one active at a time (selected in the web UI,
  persisted to NVS):

  | Mode | Description |
  |------|-------------|
  | **Serial** | USB Serial dedicated to Intiface's serial comm manager — lowest latency/jitter path. |
  | **BLE** | NimBLE-based GATT server advertising a Nordic-UART-style service (separate write/notify characteristics) for a TCode RX/TX pair. |
  | **WebSocket Server** | Direct TCode WebSocket server on port `55555` — compatible with MultiFunPlayer and other WebSocket-based controllers. Always running alongside... |
  | **WiFi / WSDM Client** | ...an outbound connection to Intiface's Device WebSocket Server as a client. These two are two roles of the same always-resident WebSocket transport object, not independently selectable — picking "WS" mode turns both on together. |
  | **Dongle Transport** | UART relay from the onboard C5-Zero coprocessor (itself fed wirelessly by the external T-Dongle C5 over ESP-NOW). `DongleTransport` reads that UART (GPIO 43/44, 460800 baud) and feeds the parser exactly like `SerialTransport` does for USB. |
  | **OSSM BLE** | SlopDrive-32 advertises itself as a **stock KinkyMakers OSSM device** (BLE peripheral/server, not a client) so third-party OSSM-ecosystem apps (OSSM Possum, XToys) can control it directly — full command/state/pattern-list characteristic set, with a 1s-grace + 2s ease-out safety ramp on disconnect. See the Known Gap note above re: continuous position streaming. |

- **Binary WebSocket UI control plane** — a completely separate WebSocket
  server on port `81` (distinct from the TCode server on 55555) speaks a compact
  binary frame protocol for device configuration and telemetry: `HELLO`,
  batched `TELE` position/target/raw samples, `STATUS` (bus/thermal/WiFi/heap),
  `CLOCK` (RTT sync), `INTERP` (interpolator debug), `ANOMALY` (event-driven),
  `CMD` (client→device, ~20 operations covering window/speed/accel, transport
  switching, blend mode, pause/halt/e-stop/home, manual moves, and more), and
  `ECHO` (device→client ack). A monotonic `cfg_gen` counter rides on every
  frame; when a client sees it advance past what it has cached, it discards any
  pending optimistic UI state and re-fetches the full config — keeping multiple
  simultaneously-connected browser tabs (or a phone + a desktop) consistent
  without polling.

- **`WIFI <ssid> <password>` / `WIFI CLEAR` sideband command** — a rescue tool
  parsed out of the normal TCode stream (works over any transport that reaches
  the parser, though the confirmation reply is Serial-only by convention).
  Stores a *secondary* set of WiFi credentials to NVS without touching the
  primary compiled-in `secrets.h` values; a reboot is required to try them.
  Intended for "rig on an unfamiliar network, no easy way to re-flash" recovery,
  not live reconfiguration.

- **USB Crash Fix** — avoiding TinyUSB-OTG CDC re-enumeration panics on DTR
  toggle (every time a host app opens/closes the port) is a real, deliberate
  concern here, but the *fix* is board-specific and opposite between the two S3
  build environments: the legacy `esp32-s3-devkitc-1` env has real UART0 pins
  broken out, so it leaves `Serial` off native USB entirely
  (`ARDUINO_USB_MODE`/`ARDUINO_USB_CDC_ON_BOOT` both unset, keeping the stable
  hardware USB-Serial/JTAG bridge). The production `s3_main` env's Arduino Nano
  ESP32 board has **no broken-out UART0 pins at all**, so `Serial` must ride the
  onboard USB — there, both flags are explicitly set to `1`, which per
  Espressif's `boards.txt` selects the same stable USB-Serial/JTAG (HWCDC)
  peripheral rather than the crash-prone native USB-OTG stack. On the T-Dongle
  C5 (USB-only, no physical UART), the flags are enabled the same way and the
  DTR-triggered reset is additionally disabled in firmware via
  `chip_rst.usb_uart_chip_rst_dis=1`, forcing one clean restart on power-on so
  the reset storm happens before MFP ever opens the port.

### Web UI

- **Served from LittleFS** — the web interface is an independent front-end
  project (Vite/vanilla JS, no framework) processed at compile time through a
  PlatformIO extra-scripting hook (`build_webui.py`). It is minified,
  single-file inlined (fonts included as base64), gzipped, and packed into a
  LittleFS partition flashed alongside the firmware. No web assets are embedded
  as C++ strings. A handful of build flags round-trip into Vite's
  `webui/.env.local` for tree-shaking (currently only `BLE_ENABLED` is actually
  wired up this way — extending the same mechanism to the driver-selection flag
  is a known gap, not yet load-bearing for anything the UI currently branches on).

- **Diagnostics Graph** — a toggle-gated, full-width strip chart (`diag.js`)
  with three lanes over a rolling 10-second window: position (actual, commanded,
  and interpolator-reported, theme-colored), lag (`|actual − commanded|`), and
  bus power (voltage + wattage). Sampling rides the same clock-synced render
  timeline the rail control uses, so ESP32 reboots and poll jitter don't corrupt
  the trace.

- **Rail & Plan Strip** — the primary stroke-window editor: a draggable
  travel-rail control (`rail.js`) with a comet-trail position marker, window
  band, input command "tape," and hazard ribbons, backed by a canvas lane
  underneath (`planstrip.js`) visualizing the in-flight interpolator segment
  (from/to, sweep head, ghost history of recent segments).

- **Configuration Panels** — live-adjustable settings, discovered and
  ceiling-clamped at runtime from the device rather than hardcoded:
  - **Stroke Range** — drag/resize the active stroke window anywhere along the
    measured travel (not a fixed 0–260mm default; the true ceiling comes from
    sensorless homing).
  - **Dual limit sets** — independent max-speed/acceleration ceilings for
    manual (user) vs. TCode/pattern/OSSM (input) moves.
  - **Continuous Blend Mode** — let-it-land / allow-reversal / hybrid selector
    (see the Motion Engine deprecation note above).
  - **Interpolation mode, stream speed-feed, and overshoot-clamp toggles** —
    the modern evolution of what used to be described as "inertia/predictive
    smoothing."
  - **Pattern panel** — glyph tile grid for the built-in patterns plus
    speed/depth/stroke/sensation sliders and a live wave-scope canvas.
  - **Theme picker** — 9 built-in accent-pair themes plus a custom color
    picker, persisted locally.
  - **Health tab** — live power meters (current/voltage/power/temperature) fed
    by the INA228, a 60-second power sparkline, an optional RS485/Modbus
    telemetry card, and a WiFi link strip (RSSI/channel/BSSID/reconnect count).
  - **Serial/Log Viewer, Anomaly Log, Connected Clients** — streamed debug log,
    interpolator anomaly events, and a list of currently-connected UI clients
    with kick support.

- **Dynamic Modularity** — the UI discovers active features at runtime via the
  JSON **`/api/capabilities`** endpoint (travel limits, speed/accel ceilings,
  which optional features are compiled in, firmware version) and
  programmatically shows, hides, or builds settings cards accordingly. A
  separate `/api/status` endpoint exists purely as an HTTP-polling fallback for
  telemetry when the binary WebSocket link is down.

- **Ground Truth** — controls reflect confirmed device state via a
  desired/reported reconciliation state machine (pending → overdue → settled/
  faulted), not optimistic assumptions; a page load adopts whatever the device
  currently reports rather than pushing UI defaults onto a live session.

### OTA & Deployment

The main controller supports over-the-air firmware and web-UI updates —
this is the default deployment path for routine changes, no cable required:

- **Two OTA surfaces sharing one safety gate**: `ArduinoOTA`/espota (serviced
  from Core 0) and two HTTP endpoints, `POST /api/ota` (firmware) and
  `POST /api/ota/fs` (LittleFS/web UI bundle), gated by an `X-OTA-Token` header
  checked with a constant-time comparison against `SECRET_OTA_PASSWORD`.
- **Before any flash write**: the pattern engine stops, the motor hard-stops
  and latches e-stop, WebSocket telemetry is suspended, and NVS config writes
  are deferred until the flash window closes. A failed OTA resumes telemetry
  only — motion stays latched-stopped until manually cleared, never silently
  resuming.
- **`FIRMWARE_VERSION`** (currently `2.1.4`, in `config_api.h`) is the single
  source of truth for "which build is actually running," surfaced via
  `/api/capabilities` → `fw_version` and the boot log — the way to confirm an
  update actually landed.
- Dedicated PlatformIO environments: `sd32` (serial/USB bench + rescue path,
  identical to `s3_main` but on an OTA-capable partition table) and `sd32-ota`
  (the default *routine* path — same build, uploaded over WiFi via espota). See
  [Build Environments](#build-environments) below.
- An empty/missing `SECRET_OTA_PASSWORD` fails safe: HTTP OTA is hard-refused
  and ArduinoOTA runs unauthenticated with a loud boot warning, rather than
  silently exposing an open flash endpoint.

### Onboard Coprocessor & T-Dongle Display

- **ST7735 160×80 TFT LCD** on the T-Dongle C5, driven by the bundled
  `lib/lcd_st7735` library (pure Arduino SPI transactions, no chip-specific
  register poking — TFT_eSPI is explicitly not used because it touches SPI
  registers that don't exist on the ESP32-C5's peripheral).
- **Live Position Bar** and **Command Frequency Readout** — real-time stroke
  position and host command Hz on the dongle's screen.
- **ESP-NOW link** — the T-Dongle beams TCode to the onboard C5-Zero coprocessor
  over ESP-NOW on a configurable 5GHz channel; the coprocessor relays it to the
  main controller over UART. A planned future addition is a small OLED +
  buttons/encoder directly on the onboard coprocessor for local control without
  a host PC at all — not yet built.

### Persistence

- All configuration is saved to NVS (non-volatile storage) via the ESP32's
  Preferences library and restored on boot. Range limits, dual speed/accel
  limit sets, motor tuning, and telemetry settings survive power cycles.
  Writes are automatically deferred while an OTA flash is in progress, so a
  config change can't race a firmware update.

---

## Hardware

### Main Controller PCB (v0.0)

| Component | Specification |
|-----------|---------------|
| **MCU** | Arduino Nano ESP32 module (u-blox NORA-W106 / ESP32-S3), 16MB flash, 8MB PSRAM |
| **Driver** | 57AIM30 closed-loop servo drive (step/direction), buffered through an SN74AHCT125 → opto inputs |
| **Motor** | 57AIM30 integrated servo, 800 steps/rev at the motor shaft (DIP-switch configured), 2:1 reduction to the drum |
| **Mechanics** | Capstan drum + Dyneema line — 25mm drum diameter, ~78.54mm travel/drum-rev, **1600 steps/drum-rev, ~20.37 steps/mm** |
| **Travel** | 260mm geometry ceiling; actual usable stroke is measured at homing time and may be shorter |
| **Homing** | Sensorless — current-stall detection via the INA228, no endstop switch on this board |
| **Current/Power Sensing** | INA228 20-bit monitor on the 36V bus (behind an ISO1640 isolator), 5mΩ shunt, 32.768A full scale |
| **Status LEDs** | Three discrete active-low LEDs (not a NeoPixel) — R/G/B on GPIO 46/0/45 — plus a standalone active-low orange user LED and an active-high heartbeat LED |
| **RS485 (deferred)** | XY-G485 auto-direction module wired for the motor's internal encoder/temp/fault telemetry — wired but not yet driven |

### S3 Pin Map (57AIM30 build, v0.0 controller)

| Function | GPIO |
|----------|------|
| PUL (STEP) | 5 |
| DIR | 6 |
| I2C SDA (INA228, future AS5600) | 8 |
| I2C SCL | 9 |
| Legacy endstop (unused on this board) | 12 |
| RS485 TX (deferred) | 17 |
| RS485 RX (deferred) | 18 |
| Heartbeat LED (active-high) | 21 |
| Onboard coprocessor UART TX | 43 |
| Onboard coprocessor UART RX | 44 |
| Status LED — Blue | 45 |
| Status LED — Red | 46 |
| Status LED — Orange (user) | 48 |
| Status LED — Green | 0 *(strapping pin — initialized after boot settles)* |

> The legacy belt-drive/DevKitC-1 prototype used a different pinout entirely
> (belt+pulley mechanics, GPIO 4/5 step/dir, a GPIO 12 endstop switch, a
> NeoPixel on GPIO 48) — that build still exists as the `esp32-s3-devkitc-1`
> PlatformIO environment but is no longer the target hardware.

### Onboard Coprocessor (ESP32-C5-Zero)

| Component | Specification |
|-----------|---------------|
| **MCU** | ESP32-C5-Zero, soldered to the main v0.0 PCB |
| **Radio** | WiFi 6 + Bluetooth 5 (LE) + ESP-NOW |
| **Role** | Receives TCode over ESP-NOW from the T-Dongle C5, relays it to the main controller over UART. Planned: local OLED + buttons/encoder (not yet built). |

### T-Dongle C5 (external accessory)

| Component | Specification |
|-----------|---------------|
| **MCU** | ESP32-C5 (LilyGO T-Dongle C5) |
| **Display** | ST7735 160×80 TFT LCD (SPI) |
| **USB** | USB-CDC via the hardware USB-Serial/JTAG peripheral (HWCDC) |
| **Radio** | WiFi 6 + Bluetooth 5 (LE) + ESP-NOW |
| **Role** | Plugs into the host PC; bridges USB TCode to ESP-NOW, wireless to the onboard coprocessor |

All hardware constants and pin assignments live in
[`include/system/config_api.h`](include/system/config_api.h). No pins are
hardcoded inside functional classes — everything is constructor-injected or
gated through the configuration header.

---

## Getting Started

This is a [PlatformIO](https://platformio.org/) project. The `pio` executable
is expected at `%USERPROFILE%\.platformio\penv\Scripts\platformio.exe`.

### Build Environments

| Environment | Target | Platform | Notes |
|-------------|--------|----------|-------|
| `s3_main` | Main controller (v0.0 PCB) | `espressif32` (stable) | **Default env.** Arduino Nano ESP32 board, 16MB partitions, 57AIM servo + WiFi + WebUI + BLE + all transports. |
| `sd32` | Main controller | `espressif32` (stable) | Serial/USB bench + rescue path — identical to `s3_main`, but on the OTA-capable partition table. Use for the one cabled flash that installs the first OTA-able image, or to recover from a bootloop/WiFi-breaking change. |
| `sd32-ota` | Main controller | `espressif32` (stable) | **The default routine deployment path.** Same build as `sd32`, uploaded over WiFi via espota to the device's usual IP. See [OTA & Deployment](#ota--deployment). |
| `esp32-s3-devkitc-1` | Legacy/prototype main controller | `espressif32` (stable) | Original bare-DevKitC-1 build (belt+pulley, endstop switch, NeoPixel) — kept for reference, no longer the target hardware. |
| `c5_waveshare` | Onboard coprocessor (ESP32-C5-Zero) | `pioarduino` fork | Env name is a historical leftover; the board it actually targets is the C5-Zero soldered to the v0.0 PCB, not a standalone Waveshare devkit. |
| `c5_tdongle` | T-Dongle C5 | `pioarduino` fork | USB-CDC-to-ESP-NOW bridge + ST7735 display. |
| `esp32-c5-dev1` | Alias for `c5_waveshare` | `pioarduino` fork | Template alias for task specs. |
| `esp32-c5-dev2` | Alias for `c5_tdongle` | `pioarduino` fork | Template alias for task specs. |

#### Why `pioarduino` for C5?

The official `platformio/platform-espressif32` lags months behind on new
silicon support. The `pioarduino` community fork tracks arduino-esp32 releases
closely and ships ESP32-C5 support in its stable release ZIPs (release
55.03.39 = Arduino ESP32 v3.3.9 / ESP-IDF v5.5.4). The main-controller
environments stay on the stable production `espressif32` platform — no dev
packages, no surprises.

#### Build Isolation

Each environment uses `build_src_filter` to compile ONLY its own source
folder (`src/c5_waveshare/`, `src/c5_tdongle/`, or the shared main-controller
tree — `src/main.cpp` + `src/comms/`, `src/motion/`, `src/system/`, `src/ui/`)
while sharing the `lib/SharedProtocol/` library (currently unused, see
[Hardware Overview](#hardware-overview)). Main-controller environments include
FastAccelStepper, NimBLE, WebSockets, NeoPixel, and the INA228 current-sensor
library; C5 environments only include ArduinoJson (lightweight packet parsing)
— FastAccelStepper explicitly throws `#error` on unsupported MCU derivatives
(ESP32-C5 is not yet supported).

### 1. Configure Secrets

WiFi credentials, the OTA password, the Intiface host address, and ESP-NOW
pairing are kept out of git. Copy the template and edit it:

```sh
copy include\secrets.example.h include\secrets.h
```

Then set:

| Constant | Purpose |
|----------|---------|
| `SECRET_WIFI_SSID` / `SECRET_WIFI_PASSWORD` | Primary WiFi credentials |
| `SECRET_OTA_PASSWORD` | Shared secret guarding both OTA paths (espota `--auth` and the `X-OTA-Token` HTTP header) |
| `SECRET_INTIFACE_HOST` / `SECRET_INTIFACE_PORT` | Only used in WiFi/WSDM client mode |
| `SECRET_ESPNOW_CHANNEL` | 5GHz channel shared by the T-Dongle C5 and the onboard coprocessor (default 36 — UNII-1, no DFS, safest choice) |
| `SECRET_ESPNOW_PEER_MAC` | MAC address of the onboard coprocessor, read from its boot log |

> `include/secrets.h` is git-ignored. If missing, the build falls back to
> harmless placeholders and emits a compile warning so a fresh clone still
> compiles. A `secrets.h` predating a given feature (e.g. no
> `SECRET_OTA_PASSWORD`) fails safe rather than silently breaking.

### 2. Build & Flash

For the main controller, OTA (`sd32-ota`) is the default path once a device is
already running OTA-capable firmware — see [OTA & Deployment](#ota--deployment).
For a first flash or a cabled connection:

```sh
# Main controller — cabled (first flash, or rescue)
pio run -e sd32 -t uploadfs               # upload LittleFS (web UI)
pio run -e sd32 -t upload                  # upload firmware

# Main controller — over WiFi (routine updates)
pio run -e sd32-ota -t uploadfs
pio run -e sd32-ota -t upload

# Onboard coprocessor (ESP32-C5-Zero)
pio run -e c5_waveshare -t upload

# T-Dongle C5
pio run -e c5_tdongle -t upload
```

> The `platformio.ini` `targets = uploadfs, upload` line is commented out by
> default — use explicit `-t uploadfs -t upload` when the board is connected.
> Leaving it active causes `pio run` to fail with COM-not-found when building
> offline.

### 3. Web UI Build Pipeline

The web UI source lives in `webui/` and is an independent Vite project:

```sh
cd webui
npm install
npm run build
```

At compile time, PlatformIO's `build_webui.py` extra-scripting hook runs the
Vite build automatically, inlines all assets (including fonts, as base64) into
a single HTML file, gzips it, and places `index.html` + `index.html.gz` into
`data/` for LittleFS packaging.

A small map of build flags from `platformio.ini` gets written to
`webui/.env.local` so Vite can tree-shake UI features accordingly — currently
`BLE_ENABLED` is the flag that actually round-trips this way.

### 4. Open the Web UI

Connect to the device's IP (printed to Serial on boot) or
`http://slopdrive32.local` (via mDNS).

---

## Intiface Device Configuration

The [`intiface/`](intiface/) directory contains the device-config JSON for
registering SlopDrive-32 as a `tcode-v03` stroker in Intiface. The config
declares a standard positional stroker with a `[0, 999]` value range and
`"L0"`/`"L"` TCode axis commands. The firmware itself uses implicit-decimal-
fraction TCode parsing (any digit length is valid), but the config JSON keeps
the historical `[0, 999]` range for Intiface compatibility.

> The BLE service/characteristic UUIDs in
> `intiface/slopdrive32-device-config.json` are kept in sync with the
> firmware's `BLE_NUS_*` UUIDs in `config_api.h` — see
> [`intiface/README.md`](intiface/README.md) for the sync table if you ever
> change one side.

---

## Project Layout

```
SlopDrive-32/
├── include/                               # Public headers
│   ├── secrets.example.h                  # WiFi / OTA / Intiface / ESP-NOW credentials template
│   ├── comms/                             # Transport layer interfaces
│   │   ├── BleTransport.h
│   │   ├── DongleTransport.h              # UART relay from onboard C5-Zero → S3
│   │   ├── OssmBleService.h               # OSSM-compatible BLE peripheral (server)
│   │   ├── SerialTransport.h
│   │   ├── ServoModbus.h                  # RS485/Modbus telemetry (config-only, not a MotorDriver)
│   │   ├── TCodeAxisState.h               # Per-axis state for the TCode v0.4 axis registry
│   │   ├── TCodeParser.h
│   │   ├── TransportManager.h
│   │   └── WebSocketTransport.h
│   ├── motion/                            # Motion engine interfaces
│   │   ├── 57AIMServoDriver.h             # 57AIM30 closed-loop servo (step/dir), capstan-drum math
│   │   ├── CurrentSensor.h                # INA228 bus current/voltage/power wrapper
│   │   ├── Kinematics.h                   # Legacy trapezoid planner (dormant by default)
│   │   ├── MotionArbiter.h                # Sole caller of MotorDriver; arbitration + safety gates
│   │   ├── MotionInterpolator.h           # Cubic Hermite interpolation for TCode streaming
│   │   ├── MotorDriver.h                  # Abstract stepper/servo driver interface
│   │   ├── PatternEngine.h                # StrokeEngine-derived pattern playback
│   │   ├── PositionTime.h                 # Legacy Core 0→Core 1 waypoint struct
│   │   ├── range_mapper.h                 # Stroke window mapping
│   │   └── TMC2160StepperDriver.h         # TMC2160 SPI implementation (legacy belt-drive build)
│   ├── system/                            # System & configuration
│   │   ├── AppLog.h                       # Ring-buffer log (streamed to Web UI)
│   │   ├── config_api.h                   # All tunable defaults & pin maps
│   │   ├── ConfigStore.h                  # NVS persistence
│   │   ├── OtaService.h                   # ArduinoOTA + HTTP OTA endpoints, safety gate
│   │   ├── StatusLeds.h                   # Discrete status LED driver (Nano ESP32 — not a NeoPixel)
│   │   └── SystemState.h                  # Global state, atomic e-stop/OTA flags
│   └── ui/
│       ├── UiProtocol.h                   # Binary WebSocket frame/opcode definitions
│       ├── UiSocket.h                     # Binary WebSocket UI control plane (port 81)
│       └── WebUI.h                        # HTTP server & REST API
├── src/                                   # Implementation
│   ├── main.cpp                           # Live main-controller composition root
│   ├── s3_main/main.cpp                   # Dead placeholder stub — excluded from every build
│   ├── c5_waveshare/main.cpp              # Onboard ESP32-C5-Zero coprocessor
│   ├── c5_tdongle/main.cpp                # T-Dongle C5 — USB-CDC/ESP-NOW bridge + display
│   ├── comms/                             # (mirrors include/comms/)
│   ├── motion/                            # (mirrors include/motion/)
│   ├── system/                            # (mirrors include/system/)
│   └── ui/                                # (mirrors include/ui/)
├── lib/                                   # Bundled libraries
│   ├── SharedProtocol/
│   │   └── SharedProtocol.h               # Cross-node binary protocol (defined, not yet wired in)
│   ├── StrokeEnginePatterns/              # Vendored MIT pattern math backing PatternEngine
│   └── lcd_st7735/                        # ST7735 driver for the T-Dongle C5 display
├── boards/                                # Custom board definitions
│   ├── arduino-nano-esp32.json            # Main controller (v0.0 PCB)
│   ├── esp32-c5-zero.json                 # Onboard coprocessor
│   └── lilygo-t-dongle-c5.json            # T-Dongle C5
├── webui/                                 # Independent Vite front-end project
│   ├── src/
│   │   ├── main.js                        # App entry, boot sequence, top-level wiring
│   │   ├── style.css                      # Full UI stylesheet
│   │   ├── core/
│   │   │   ├── api.js                     # HTTP fetch wrappers (fallback path)
│   │   │   ├── capabilities.js            # /api/capabilities-driven feature discovery
│   │   │   ├── cmd.js                     # Binary control-plane client (cmd ids, resend, cfg_gen watch)
│   │   │   ├── link.js                    # WebSocket state machine, reconnect, clock sync
│   │   │   ├── meter.js                   # Reusable instrument widget (Health tab)
│   │   │   ├── range.js                   # Stroke-window state store
│   │   │   ├── shadow.js                  # Desired/reported reconciliation state machine
│   │   │   ├── telebuf.js                 # Clock-synced telemetry ring buffer
│   │   │   ├── theme.js                   # Accent theme system
│   │   │   ├── ui.js                      # DOM helpers, tabs, tooltips, toasts
│   │   │   └── wire.js                    # Binary frame codecs
│   │   ├── features/
│   │   │   ├── diag.js                    # Diagnostics strip chart (position/lag/power)
│   │   │   ├── pattern.js                 # Pattern Engine control panel
│   │   │   ├── planstrip.js               # Live interpolator segment visualization
│   │   │   ├── rail.js                    # Travel-rail control surface (stroke window editor)
│   │   │   └── settings.js                # Transport, limits, blend mode, expert-mode settings
│   │   └── fonts/                         # Self-hosted webfonts (inlined at build time)
│   ├── index.html
│   ├── vite.config.js
│   └── package.json
├── data/                                  # LittleFS image source (auto-generated)
│   ├── index.html
│   └── index.html.gz
├── intiface/                              # Intiface device-config JSON
│   ├── README.md
│   └── slopdrive32-device-config.json
├── build_webui.py                         # PlatformIO extra-scripting hook
├── tools/
│   └── ota_auth.py                        # Injects the OTA secret into espota at build time
├── platformio.ini
├── LICENSE
├── THIRD_PARTY_LICENSES.md
└── README.md
```

---

## Credits

- Concepts and wiring originally derived from **StrokeEngine** by the OSSM
  community (theelims / KinkyMakers, MIT-licensed) — both the original
  transport rework inspiration and, later, the vendored pattern math behind
  `PatternEngine`.
- Cubic-Hermite TCode streaming interpolation ported from TempestMAx's
  OSR2/SR6 `Axis` library (MIT).
- Built on [FastAccelStepper](https://github.com/gin66/FastAccelStepper),
  [ArduinoJson](https://arduinojson.org/),
  [arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets),
  [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino),
  [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel),
  [INA228 (RobTillaart)](https://github.com/RobTillaart/INA228), and the
  Espressif Arduino core.
- ST7735 LCD driver derived from [LilyGO's T-Dongle-C5 library](https://github.com/Xinyuan-LilyGO/T-Dongle-C5).
- ESP32-C5 platform support via the [pioarduino community fork](https://github.com/pioarduino/platform-espressif32).
- The rest: vibes. 🤖

---

## License

SlopDrive-32 is licensed under the **MIT License** — see [`LICENSE`](LICENSE).
Provided **as-is, with no warranty**.

This project links against third-party Arduino libraries and the Espressif
Arduino core, each under its own license (MIT / LGPL / Apache-2.0). See
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) for full attribution and
license details.
