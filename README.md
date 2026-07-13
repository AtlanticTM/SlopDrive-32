# SlopDrive-32

An open-source ESP32-S3 / ESP32-C5 multi-node firmware for a belt-driven linear
stroke machine, controlled over [Intiface](https://intiface.com/) /
[buttplug.io](https://buttplug.io) with a rich web UI for configuration and
real-time telemetry. Now spanning a three-device network architecture with a
main controller, a receiver node, and a USB dongle with live display.

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
case — so it was reworked around Intiface/buttplug control. Use at your own risk,
review before flashing, and don't trust it with anything you care about. **No
warranty, express or implied. You are responsible for your own hardware and
safety.**

---

## Multi-Node Network Architecture

SlopDrive-32 now spans **three ESP32 devices** chained together in a
distributed control network — main controller, receiver node, and USB
dongle with live display.

| Node | Device | Role | Description |
|------|--------|------|-------------|
| **Main Controller** | ESP32-S3 (DevKitC-1) | Brain + Muscle | Runs the web UI, WiFi, mDNS, WebSocket server, LittleFS asset serving, BLE transport, TCode parsing, kinematics planner, and the 57AIM30 servo step pulse generation on Core 1. The central command post — every command flows through here. |
| **Node A (Receiver)** | ESP32-C5 (Waveshare Full-Size DevKit) | Remote Receiver | Dedicated receiver node listening for motion commands over ESP-NOW / WiFi. Sits near the actuator and relays the position stream to the motion pipeline. |
| **Node B (Transmitter)** | ESP32-C5 (LilyGO T-Dongle C5) | Display + Bridge | USB dongle that sits between the host (MultiFunPlayer / Intiface) and the S3. MFP talks to the dongle's USB-CDC port; the dongle relays TCode over a physical UART wire to the S3's Serial2. The dongle's ST7735 160×80 LCD display shows live position and command frequency. Also handles ESP-NOW broadcast of motion state. |
| **Node C (Planned)** | ESP32-C5 (TBD) | Remote Controller | A handheld wireless remote — physical knobs, buttons, maybe a small display — for direct manual stroke control. Still in the design/planning phase. Will speak SharedProtocol over ESP-NOW for low-latency wireless control. |

### SharedProtocol — The Common Tongue

All three nodes communicate via **SharedProtocol** (`lib/SharedProtocol/`), a
hardware-agnostic binary packet framing library that defines:

- **`NodeRole`** enum — `MAIN_CONTROLLER` (0), `RECEIVER` (1), `TRANSMITTER` (2)
- **`MessageType`** enum — `HEARTBEAT`, `MOTION_COMMAND`, `MOTION_STATE`,
  `CONFIG_UPDATE`, `EMERGENCY_STOP`
- **`PacketHeader`** — `{'S','D'}` magic bytes, protocol version, message type,
  sender role, payload length, and checksum for data integrity.
- **`MotionCommand`** / **`MotionState`** — compact binary structs for
  normalized position, speed, acceleration, homing/fault flags, and timestamps.
  No JSON overhead on the wire — raw binary, minimal latency.

The protocol is compiled into every node from the same shared library, so all
three devices communicate seamlessly regardless of whether they're on
ESP32-S3 or ESP32-C5 silicon.

---

## Architecture

SlopDrive-32 is built on a **dual-core, modular architecture** targeting the
ESP32-S3's twin Xtensa LX7 cores (and single-core operation on the ESP32-C5
nodes):

| Core | Responsibility |
|------|---------------|
| **Core 0** (System & Comms) | WiFi (AP+STA), mDNS, HTTP server, WebSocket server, LittleFS asset serving, JSON REST API, BLE transport, predictive lookahead calculations, asynchronous system monitoring |
| **Core 1** (Motion Real-Time) | High-frequency hardware step pulse generation, real-time motion execution, homing cycles, endstop monitoring, e-stop handling |

### Thread-Safe Data Sharing

All shared state between Core 0 and Core 1 is protected by FreeRTOS primitives —
atomic variables for flags (e-stop, homing state), `portMUX_TYPE` spinlocks for
generator configuration, and `xQueue` for ring buffers. No volatile hacks, no
naked shared access.

### Hardware Abstraction

All physical hardware interactions are abstracted behind clean C++ interface
classes (Abstract Base Classes with pure virtual functions):

- **`MotorDriver`** — abstracts stepper/servo communication. Currently
  implemented by `Ai57AIMServoDriver` for the 57AIM30 closed-loop servo
  (step/direction pulse generation via FastAccelStepper on the S3's MCPWM/RMT
  peripherals). The interface is structured for community extensions — Modbus
  servos, CANopen drives, external pulse generators, whatever hardware you
  strap to the rail.
- **`Transport`** — abstracts communication transport (Serial, BLE, WebSocket,
  Dongle UART relay). `TransportManager` routes data between the active
  transport and the TCode parser.
- Build-flags (`-DDRIVER_57AIM_SERVO`, `-DBLE_ENABLED`) gate driver code
  inclusion; unused drivers are not compiled into the binary.

---

## Features

### Motion Engine

- **57AIM30 Closed-Loop Servo Driver** — the current production motor driver is
  the `Ai57AIMServoDriver`, a "dumb" step/direction pulse generator for the
  57AIM30 closed-loop servo drive. No SPI, no Modbus, no register config — the
  57AIM30 handles all the closed-loop magic internally via its own DSP. We just
  send PUL (pulse) and DIR (direction) signals via FastAccelStepper and let the
  drive do its thing — 1600 steps/rev, 80mm belt travel per revolution, 20
  steps/mm.

- **Continuous Motion Blending** — `streamTo()` never softens a committed brake
  ramp between waypoints. It handles same-direction continuation vs. reversal
  through a selectable blend mode (let-it-land / allow-reversal / hybrid). No
  stop-and-go stutter between position samples — motion stays smooth through
  every waypoint with configurable blend strategies for direction changes.

- **Predictive Interpolation** — between discrete position updates, the
  kinematics planner generates continuous step schedules with configurable
  acceleration and max-speed caps. No stuttering between commands — smooth,
  mathematically continuous motion through every target.

- **Auto-Duration Timing** — the firmware measures the *actual* inter-command
  cadence from the host and sizes each move to fit, rather than trusting an
  app's often-fixed or bogus duration value. This prevents timing drift and
  keeps the stroke rhythm locked to the content.

- **Configurable Tick Rate** — the motion generator runs at a user-configurable
  tick rate (default 100 Hz, supportable up to 200 Hz) for smooth fine-grained
  step pulse generation.

- **Deceleration Guard** — when the commanded target moves faster than the
  hardware can physically accelerate, the planner softens the approach rather
  than slamming into the limit. The web UI's motion graph makes this visible in
  real time — target vs. actual divergence shows the struggle on screen.

- **Range Mapping** — incoming normalized `0.0`–`1.0` commands are remapped
  onto a user-trimmable window of the physical rail. The full input stroke is
  scaled and positioned anywhere across the available travel, configurable via
  the web UI in real time.

```text
physical_mm = min_position_mm + normalized × (max_position_mm − min_position_mm)
```

- **Homing** — a self-contained FreeRTOS task handles homing. On power-up the
  machine does not move or engage the motor; homing must be explicitly triggered
  from the web UI or by physically pushing the shaft into the endstop
  (push-to-home).

- **E-Stop** — an emergency-stop flag immediately halts all motion on Core 1,
  clears the step queue, and reports status to Core 0. The flag is an atomic
  variable checked at every stage of the motion pipeline.

- **`PositionTime`** queue — waypoint structs flow from Core 0 (TCode parser
  callbacks) into `g_waypoint_queue` (FreeRTOS queue). Core 1's
  `motionConsumerTask` pops them, runs the OSSM kinematic pipeline, and
  dispatches to FastAccelStepper. Includes `has_set_time` / `setTime` fields
  so the consumer can apply OSSM-style latency compensation (shorten
  `timeSeconds` up to 25% to resync when transport lag is detected).

### Communication & Control

- **Buttplug / Intiface control** via TCode (`tcode-v03` stroker profile). The
  TCode parser correctly implements the v0.3 implicit-decimal-fraction spec,
  handling arbitrary digit lengths without clipping or overflow.

| Mode | Description |
|------|-------------|
| **Serial Control** (default) | USB Serial dedicated to Intiface's serial comm manager — lowest latency/jitter path. The ESP32-S3's hardware USB-Serial/JTAG bridge keeps the port stable across host open/close cycles (no CDC teardown panics). |
| **BLE** | NimBLE-based GATT server advertising a TCode characteristic. Enables wireless control from mobile apps or BLE-capable hosts. |
| **WebSocket Server** | Direct TCode WebSocket server on port `55555` — compatible with MultiFunPlayer and other WebSocket-based controllers. |
| **WiFi / WSDM Client** | The device connects outward to Intiface's Device WebSocket Server as a client. |
| **Dongle Transport** | UART relay from the T-Dongle C5 (Node B). MFP talks USB-CDC to the dongle; the dongle forwards TCode over a physical UART wire to the S3's `Serial2` (pins 8/9). `DongleTransport` reads that UART and feeds the parser exactly like `SerialTransport` does for USB — same line-buffering, same null-byte filtering, same disconnect detection. D0/D1/D2 TCode replies are echoed back to the dongle so MFP sees responses. The S3 stays on WiFi for the web UI while MFP gets a dedicated USB port on the dongle. |

- **USB Crash Fix** — the firmware deliberately avoids `ARDUINO_USB_MODE=1` /
  `ARDUINO_USB_CDC_ON_BOOT=1` build flags on the S3, which would switch
  `Serial` to the native USB-OTG TinyUSB CDC stack. That stack tears down and
  re-enumerates on every DTR toggle (i.e., every time a host app opens/closes
  the port), causing panics and phantom disconnects. Sticking with the hardware
  USB-Serial/JTAG bridge keeps the connection solid across host sessions. On
  the T-Dongle C5 (which has no physical UART pins), the flags ARE enabled and
  the DTR-triggered reset is disabled in firmware via
  `chip_rst.usb_uart_chip_rst_dis=1` before `app_main()`.

### Web UI

- **Served from LittleFS** — the web interface is an independent front-end
  project (Vite/vanilla JS) processed at compile time through a PlatformIO
  extra-scripting hook. It is minified, single-file inlined, gzipped, and packed
  into a LittleFS partition flashed alongside the firmware. No web assets are
  embedded as C++ strings.

- **Real-Time Motion Graph** — a rolling canvas chart showing three traces
  overlaid on the last 10 seconds of motion:
  - **"Asked" (blue dotted)** — raw TCode demand, straight from the parser and
    range mapper before the planner touches it.
  - **"Told" (amber dashed)** — what the planner commanded the motor to target.
  - **"Took" (green solid)** — where the shaft actually ended up.
  The graph uses sequence-number deduplication and fixed-cadence local-clock
  playback, so ESP32 reboots and poll jitter don't freeze or corrupt the trace.

- **Configuration Panels** — live-adjustable settings with plain-English
  tooltips:
  - **Stroke Range** — set min/max physical positions (0–260 mm); resize and
    slide the active stroke window anywhere along the rail.
  - **Max Speed** and **Acceleration** caps.
  - **Inertia / Predictive Smoothing** — controls how aggressively the planner
    anticipates the next target.
  - **Continuous Blend Mode** — select let-it-land, allow-reversal, or hybrid
    blending for streamed position commands.
  - **Serial / Log Viewer** — since the USB serial port is busy with TCode,
    debug output is streamed to the web UI via WebSocket.
  - **Telemetry Settings** — configure the sampling rate and poll interval for
    the motion telemetry pipeline.

- **Dynamic Modularity** — the UI discovers active features at runtime via the
  JSON `/api/status` endpoint and programmatically shows, hides, or builds
  settings cards depending on what the firmware reports. The web assets never
  need modification when hardware configuration changes.

### Dongle Display (Node B — LilyGO T-Dongle C5)

- **ST7735 160×80 TFT LCD** — driven by the bundled `lib/lcd_st7735` library
  (pure Arduino SPI transactions, no chip-specific register poking). TFT_eSPI
  is explicitly NOT used because it accesses SPI registers (`VSPI_HOST`,
  `SPI_MOSI_DLEN_REG`) that don't exist on the ESP32-C5's SPI peripheral. The
  bundled library just works — clean SPI, no drama.
- **Live Position Bar** — horizontal bar showing the current stroke position
  in real time on a tiny glowing screen.
- **Command Frequency Readout** — Hz counter showing how fast the host is
  feeding position commands.
- **ESP-NOW State Broadcast** — the dongle beams `MotionState` packets back
  to the main controller over ESP-NOW so the web UI always knows the current
  position when the dongle is the active transport path.

### Persistence

- All configuration is saved to NVS (non-volatile storage) via the ESP32's
  Preferences library and restored on boot. Range limits, speed caps, motor
  tuning, and telemetry settings survive power cycles.

---

## Hardware

### Main Controller (ESP32-S3)

| Component | Specification |
|-----------|---------------|
| **MCU** | ESP32-S3 (DevKitC-1, 8 MB flash, 2 MB PSRAM) |
| **Driver** | 57AIM30 closed-loop servo drive (step/direction) |
| **Motor** | 57AIM30 integrated servo, 1600 steps/rev (DIP-switch configured) |
| **Mechanics** | HTD 5M belt, 16-tooth pulley → 80 mm/rev → **20 steps/mm** |
| **Travel** | 260 mm physical limit |
| **Endstop** | Active-LOW limit switch (GPIO 12, via optocoupler) |
| **LED** | Single NeoPixel (WS2812) for status indication |

### S3 Default Pin Map (57AIM30 Servo)

| Function | GPIO |
|----------|------|
| PUL (STEP) | 5 |
| DIR | 4 |
| ENDSTOP | 12 |
| NeoPixel | 48 |
| Serial2 RX (dongle UART) | 8 |
| Serial2 TX (dongle UART) | 9 |

### Node A (ESP32-C5 Waveshare Full-Size DevKit)

| Component | Specification |
|-----------|---------------|
| **MCU** | ESP32-C5 (Waveshare full-size devkit, 16 MB flash, PSRAM) |
| **Radio** | WiFi 6 + Bluetooth 5 (LE) + ESP-NOW |
| **Role** | Dedicated receiver node — listens for SharedProtocol motion commands |

### Node B (ESP32-C5 LilyGO T-Dongle C5)

| Component | Specification |
|-----------|---------------|
| **MCU** | ESP32-C5 (LilyGO T-Dongle C5, 16 MB flash, PSRAM) |
| **Display** | ST7735 160×80 TFT LCD (SPI) |
| **USB** | USB-CDC via HWCDCSerial (USB-Serial/JTAG peripheral) |
| **Radio** | WiFi 6 + Bluetooth 5 (LE) + ESP-NOW |
| **Role** | USB dongle — relays TCode from host to S3 over UART, displays live telemetry |

All hardware constants and pin assignments live in
[`include/system/config_api.h`](include/system/config_api.h). No pins are
hardcoded inside functional classes — everything is constructor-injected or
gated through the configuration header.

---

## Getting Started

This is a [PlatformIO](https://platformio.org/) project. The `pio` executable
is expected at `%USERPROFILE%\.platformio\penv\Scripts\platformio.exe`.

### Build Environments

The project contains **five build environments** spanning the three-node
architecture (plus legacy and template aliases):

| Environment | Target | Platform | Notes |
|-------------|--------|----------|-------|
| `s3_main` | ESP32-S3 main controller | `espressif32` (stable) | Production build — 57AIM servo, WiFi, WebUI, BLE, all transports |
| `esp32-s3-devkitc-1` | S3 legacy build | `espressif32` (stable) | Original single-node firmware using `src/main.cpp` and shared modules |
| `c5_waveshare` | Node A — C5 receiver | `pioarduino` fork | ESP32-C5 Waveshare devkit — lightweight comms node, no steppers |
| `c5_tdongle` | Node B — C5 dongle | `pioarduino` fork | LilyGO T-Dongle C5 — USB-CDC relay + ST7735 display |
| `esp32-c5-dev1` | Alias for `c5_waveshare` | `pioarduino` fork | Template alias for task specs |
| `esp32-c5-dev2` | Alias for `c5_tdongle` | `pioarduino` fork | Template alias for task specs |

#### Why `pioarduino` for C5?

The official `platformio/platform-espressif32` lags months behind on new
silicon support. The `pioarduino` community fork tracks arduino-esp32 releases
closely and ships ESP32-C5 support in its stable release ZIPs (release
55.03.39 = Arduino ESP32 v3.3.9 / ESP-IDF v5.5.4). The S3 environments stay
on the stable production `espressif32` platform — no dev packages, no
surprises.

#### Build Isolation

Each environment uses `build_src_filter` to compile ONLY its own source
folder (`src/s3_main/`, `src/c5_waveshare/`, `src/c5_tdongle/`) while
sharing the `lib/SharedProtocol/` library. S3 environments include
FastAccelStepper, NimBLE, WebSockets, and NeoPixel; C5 environments only
include ArduinoJson (lightweight packet parsing) — FastAccelStepper
explicitly throws `#error` on unsupported MCU derivatives (ESP32-C5 is not
yet supported).

### 1. Configure Secrets

WiFi credentials and the Intiface host address are kept out of git. Copy the
template and edit it:

```sh
copy include\secrets.example.h include\secrets.h
```

Then set `SECRET_WIFI_SSID`, `SECRET_WIFI_PASSWORD`, and (if using the
WiFi/WSDM path) `SECRET_INTIFACE_HOST` / `SECRET_INTIFACE_PORT`.

> `include/secrets.h` is git-ignored. If missing, the build falls back to
> harmless placeholders and emits a compile warning so a fresh clone still
> compiles.

### 2. Build & Flash

```sh
# Main controller (ESP32-S3)
pio run -e s3_main                           # build firmware
pio run -e s3_main -t uploadfs               # upload LittleFS (web UI)
pio run -e s3_main -t upload                 # upload firmware

# Node A — C5 Waveshare receiver
pio run -e c5_waveshare -t upload

# Node B — C5 T-Dongle transmitter/display
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
Vite build automatically, inlines all assets into a single HTML file, gzips
it, and places `index.html` + `index.html.gz` into `data/` for LittleFS
packaging.

Build flags from `platformio.ini` (e.g., `-DDRIVER_57AIM_SERVO`,
`-DBLE_ENABLED`) are parsed and written to `webui/.env.local` so Vite can
tree-shake UI features accordingly.

### 4. Open the Web UI

Connect to the device's IP (printed to Serial on boot) or
`http://slopdrive32.local` (via mDNS).

---

## Intiface Device Configuration

The [`intiface/`](intiface/) directory contains the device-config JSON for
registering SlopDrive-32 as a `tcode-v03` stroker in Intiface. The config
declares a standard positional stroker with a `[0, 999]` value range and
`"L0"`/`"L"` TCode axis commands.

The firmware itself uses implicit-decimal-fraction TCode parsing (any digit
length is valid), but the config JSON keeps the historical `[0, 999]` range
for Intiface compatibility.

---

## Project Layout

```
SlopDrive-32/
├── include/                               # Public headers
│   ├── secrets.example.h                  # WiFi / Intiface credentials template
│   ├── comms/                             # Transport layer interfaces
│   │   ├── BleTransport.h
│   │   ├── DongleTransport.h              # UART relay from T-Dongle C5 → S3
│   │   ├── SerialTransport.h
│   │   ├── TCodeParser.h
│   │   ├── TransportManager.h
│   │   └── WebSocketTransport.h
│   ├── motion/                            # Motion engine interfaces
│   │   ├── 57AIMServoDriver.h             # 57AIM30 closed-loop servo (step/dir)
│   │   ├── Generator.h                    # Step pulse generator (Core 1)
│   │   ├── Kinematics.h                   # Predictive planner & interpolation
│   │   ├── MotorDriver.h                  # Abstract stepper/servo driver interface
│   │   ├── PositionTime.h                 # Core 0→Core 1 waypoint queue struct
│   │   ├── range_mapper.h                 # Stroke window mapping
│   │   └── TMC2160StepperDriver.h         # TMC2160 SPI implementation (legacy)
│   ├── system/                            # System & configuration
│   │   ├── AppLog.h                       # Ring-buffer log (streamed to Web UI)
│   │   ├── config_api.h                   # All tunable defaults & pin maps
│   │   ├── ConfigStore.h                  # NVS persistence
│   │   └── SystemState.h                  # Global state with atomic e-stop flag
│   └── ui/
│       └── WebUI.h                        # HTTP server & WebSocket handler
├── src/                                   # Implementation
│   ├── main.cpp                           # Legacy S3 composition root
│   ├── s3_main/main.cpp                   # S3 main controller entry point
│   ├── c5_waveshare/main.cpp              # Node A — C5 Waveshare receiver
│   ├── c5_tdongle/main.cpp                # Node B — C5 T-Dongle transmitter/display
│   ├── comms/
│   │   ├── BleTransport.cpp
│   │   ├── DongleTransport.cpp            # UART-to-TCodeParser bridge
│   │   ├── SerialTransport.cpp
│   │   ├── TCodeParser.cpp
│   │   ├── TransportManager.cpp
│   │   └── WebSocketTransport.cpp
│   ├── motion/
│   │   ├── 57AIMServoDriver.cpp           # 57AIM30 pulse generation via FAS
│   │   ├── Generator.cpp
│   │   ├── Kinematics.cpp
│   │   ├── range_mapper.cpp
│   │   └── TMC2160StepperDriver.cpp
│   ├── system/
│   │   ├── AppLog.cpp
│   │   └── ConfigStore.cpp
│   └── ui/
│       └── WebUI.cpp
├── lib/                                   # Bundled libraries
│   ├── SharedProtocol/
│   │   └── SharedProtocol.h               # Multi-node binary packet protocol
│   └── lcd_st7735/                        # ST7735 driver for T-Dongle C5 display
│       ├── st7735.h
│       └── st7735.cpp
├── boards/                                # Custom board definitions
│   ├── esp32-c5-waveshare.json            # Waveshare ESP32-C5 full-size devkit
│   └── lilygo-t-dongle-c5.json            # LilyGO T-Dongle C5
├── webui/                                 # Independent Vite front-end project
│   ├── src/
│   │   ├── main.js                        # App entry, API client, WebSocket
│   │   ├── style.css                      # Full UI stylesheet
│   │   ├── core/
│   │   │   ├── range.js                   # Stroke-window slider & marker
│   │   │   └── ui.js                      # DOM helpers, $(), clamp()
│   │   └── features/
│   │       ├── generator.js               # Generator config panel
│   │       ├── motiongraph.js             # Real-time canvas telemetry graph
│   │       └── settings.js                # Settings & blend mode tab builder
│   ├── index.html
│   ├── vite.config.js
│   └── package.json
├── data/                                  # LittleFS image source (auto-generated)
│   ├── index.html
│   └── index.html.gz
├── intiface/                              # Intiface device-config JSON
│   └── slopdrive32-device-config.json
├── build_webui.py                         # PlatformIO extra-scripting hook
├── platformio.ini
├── LICENSE
├── THIRD_PARTY_LICENSES.md
└── README.md
```

---

## Credits

- Concepts and wiring originally derived from **StrokeEngine** by the OSSM
  community.
- Built on [FastAccelStepper](https://github.com/gin66/FastAccelStepper),
  [ArduinoJson](https://arduinojson.org/),
  [arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets),
  [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino),
  [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel),
  and the Espressif Arduino core.
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