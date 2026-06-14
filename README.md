# SlopDrive-32

An open-source ESP32-S3 firmware for a belt-driven linear stroke machine, controlled
over [Intiface](https://intiface.com/) / [buttplug.io](https://buttplug.io) with a
rich web UI for configuration and real-time telemetry.

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

## Architecture

SlopDrive-32 is built on a **dual-core, modular architecture** targeting the
ESP32-S3's twin Xtensa LX7 cores:

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

- **`MotorDriver`** — abstracts stepper/driver communication (currently
  implemented by `TMC2160StepperDriver`, but structured for community extensions
  like Modbus servos, closed-loop drivers, or external controllers).
- **`Transport`** — abstracts communication transport (Serial, BLE, WebSocket).
  `TransportManager` routes data between the active transport and the TCode
  parser.
- Build-flags (`-DDRIVER_TMC2160`, `-DBLE_ENABLED`) gate driver code inclusion;
  unused drivers are not compiled into the binary.

---

## Features

### Motion Engine

- **Predictive Interpolation** — between discrete position updates, the kinematics
  planner generates continuous step schedules with configurable acceleration and
  max-speed caps. No stuttering between commands — the shaft glides smoothly into
  every target, filling the gap between updates with mathematically continuous
  motion.

- **Auto-Duration Timing** — the firmware measures the *actual* inter-command
  cadence from the host and sizes each move to fit, rather than trusting an app's
  often-fixed or bogus duration value. This prevents timing drift and keeps the
  stroke rhythm locked to the content.

- **Configurable Tick Rate** — the motion generator runs at a user-configurable
  tick rate (default 100 Hz, supportable up to 200 Hz) for smooth fine-grained
  step pulse generation.

- **Deceleration Guard** — when the commanded target moves faster than the
  hardware can physically accelerate, the planner softens the approach rather
  than slamming into the limit. The web UI's motion graph makes this visible in
  real time — target vs. actual divergence literally draws the struggle on
  screen, two lines that spread wider the deeper you push, until the shaft is
  stretching to keep up with what it's being fed. yippie! :3

- **Range Mapping** — incoming normalized `0.0`–`1.0` commands are remapped
  onto a user-trimmable window of the physical rail. The full input stroke is
  scaled and positioned anywhere across the available travel, configurable via
  the web UI in real time.

```text
physical_mm = min_position_mm + normalized × (max_position_mm − min_position_mm)
```

- **Homing** — a self-contained FreeRTOS task handles homing. On power-up the
  machine does not move or engage the motor; homing must be explicitly triggered
  from the web UI or by physically pushing the shaft into the endstop (push-to-home).

- **E-Stop** — an emergency-stop flag immediately halts all motion on Core 1,
  clears the step queue, and reports status to Core 0. The flag is an atomic
  variable checked at every stage of the motion pipeline.

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

- **USB Crash Fix** — the firmware deliberately avoids `ARDUINO_USB_MODE=1` /
  `ARDUINO_USB_CDC_ON_BOOT=1` build flags, which would switch `Serial` to the
  native USB-OTG TinyUSB CDC stack. That stack tears down and re-enumerates on
  every DTR toggle (i.e., every time a host app opens/closes the port), causing
  panics and phantom disconnects. Sticking with the hardware USB-Serial/JTAG
  bridge keeps the connection solid across host sessions.

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

- **Configuration Panels** — live-adjustable settings with plain-English tooltips:
  - **Stroke Range** — set min/max physical positions (0–240 mm); resize and
    slide the active stroke window anywhere along the rail.
  - **Max Speed** and **Acceleration** caps.
  - **Inertia / Predictive Smoothing** — controls how aggressively the planner
    anticipates the next target.
  - **Motor Tab** — live TMC2160 tuning (run/hold current, StealthChop vs.
    SpreadCycle, chopper thresholds, blank time, hysteresis, and more).
  - **Serial / Log Viewer** — since the USB serial port is busy with TCode,
    debug output is streamed to the web UI via WebSocket.
  - **Telemetry Settings** — configure the sampling rate and poll interval for
    the motion telemetry pipeline.

- **Dynamic Modularity** — the UI discovers active features at runtime via the
  JSON `/api/status` endpoint and programmatically shows, hides, or builds
  settings cards depending on what the firmware reports. The web assets never
  need modification when hardware configuration changes.

### Persistence

- All configuration is saved to NVS (non-volatile storage) via the ESP32's
  Preferences library and restored on boot. Range limits, speed caps, motor
  tuning, and telemetry settings survive power cycles.

---

## Hardware

| Component | Specification |
|-----------|---------------|
| **MCU** | ESP32-S3 (DevKitC-1, 8 MB flash, 2 MB PSRAM) |
| **Driver** | TMC2160 (SPI), `R_SENSE = 0.15 Ω` |
| **Motor** | NEMA stepper, 200 steps/rev, 16 microsteps |
| **Mechanics** | 20-tooth pulley, 2 mm-pitch belt → 40 mm/rev → **80 steps/mm** |
| **Travel** | 240 mm physical limit |
| **Endstop** | Active-LOW limit switch |
| **LED** | Single NeoPixel (WS2812) for status indication |

### Default Pin Map (ESP32-S3)

| Function | GPIO |
|----------|------|
| STEP | 21 |
| DIR | 20 |
| ENABLE (active-low) | 19 |
| ENDSTOP | 47 |
| TMC CS | 11 |
| TMC SCLK | 12 |
| TMC MOSI | 13 |
| TMC MISO | 10 |
| NeoPixel | 48 |

All hardware constants and pin assignments live in
[`include/system/config_api.h`](include/system/config_api.h). No pins are
hardcoded inside functional classes — everything is constructor-injected or
gated through the configuration header.

---

## Getting Started

This is a [PlatformIO](https://platformio.org/) project. The `pio` executable
is expected at `%USERPROFILE%\.platformio\penv\Scripts\platformio.exe`.

### 1. Configure Secrets

WiFi credentials and the Intiface host address are kept out of git. Copy the
template and edit it:

```sh
copy include\secrets.example.h include\secrets.h
```

Then set `SECRET_WIFI_SSID`, `SECRET_WIFI_PASSWORD`, and (if using the WiFi/WSDM
path) `SECRET_INTIFACE_HOST` / `SECRET_INTIFACE_PORT`.

> `include/secrets.h` is git-ignored. If missing, the build falls back to
> harmless placeholders and emits a compile warning so a fresh clone still
> compiles.

### 2. Build & Flash

```sh
pio run -e esp32-s3-devkitc-1                # build (also builds web UI assets)
pio run -e esp32-s3-devkitc-1 -t uploadfs    # upload LittleFS (web UI)
pio run -e esp32-s3-devkitc-1 -t upload      # upload firmware
```

> The `platformio.ini` `targets = uploadfs, upload` line auto-uploads the
> filesystem image before firmware. Upload speed is set to 1.5 Mbaud for fast
> flashing.

### 3. Web UI Build Pipeline

The web UI source lives in `webui/` and is an independent Vite project:

```sh
cd webui
npm install
npm run build
```

At compile time, PlatformIO's `build_webui.py` extra-scripting hook runs the
Vite build automatically, inlines all assets into a single HTML file, gzips it,
and places `index.html` + `index.html.gz` into `data/` for LittleFS packaging.

Build flags from `platformio.ini` (e.g., `-DDRIVER_TMC2160`, `-DBLE_ENABLED`)
are parsed and written to `webui/.env.local` so Vite can tree-shake UI features
accordingly.

### 4. Open the Web UI

Connect to the device's IP (printed to Serial on boot) or `http://slopdrive32.local`
(via mDNS).

---

## Intiface Device Configuration

The [`intiface/`](intiface/) directory contains the device-config JSON for
registering SlopDrive-32 as a `tcode-v03` stroker in Intiface. The config
declares a positional stroker with a `[0, 999]` value range and the
`"L0"`/`"L"` TCode axis commands. It's a perfectly respectable JSON blob—right
up until it tells the machine to plunge in and not stop until the stomach
bulges. owo

The firmware itself uses implicit-decimal-fraction TCode parsing (any digit
length is valid), but the config JSON keeps the historical `[0, 999]` range for
Intiface compatibility.

---

## Project Layout

```
SlopDrive-32/
├── include/                          # Public headers
│   ├── secrets.example.h             # WiFi / Intiface credentials template
│   ├── comms/                        # Transport layer interfaces
│   │   ├── BleTransport.h
│   │   ├── SerialTransport.h
│   │   ├── TCodeParser.h
│   │   ├── TransportManager.h
│   │   └── WebSocketTransport.h
│   ├── motion/                       # Motion engine interfaces
│   │   ├── Generator.h               # Step pulse generator (Core 1)
│   │   ├── Kinematics.h              # Predictive planner & interpolation
│   │   ├── MotorDriver.h             # Abstract stepper driver interface
│   │   ├── range_mapper.h            # Stroke window mapping
│   │   └── TMC2160StepperDriver.h    # TMC2160 SPI implementation
│   ├── system/                       # System & configuration
│   │   ├── AppLog.h                  # Ring-buffer log (streamed to Web UI)
│   │   ├── config_api.h              # All tunable defaults & pin map
│   │   ├── ConfigStore.h             # NVS persistence
│   │   └── SystemState.h             # Global state with atomic e-stop flag
│   └── ui/
│       └── WebUI.h                   # HTTP server & WebSocket handler
├── src/                              # Implementation
│   ├── main.cpp                      # Composition root (thin entry point)
│   ├── comms/
│   │   ├── BleTransport.cpp
│   │   ├── SerialTransport.cpp
│   │   ├── TCodeParser.cpp
│   │   ├── TransportManager.cpp
│   │   └── WebSocketTransport.cpp
│   ├── motion/
│   │   ├── Generator.cpp
│   │   ├── Kinematics.cpp
│   │   ├── range_mapper.cpp
│   │   └── TMC2160StepperDriver.cpp
│   ├── system/
│   │   ├── AppLog.cpp
│   │   └── ConfigStore.cpp
│   └── ui/
│       └── WebUI.cpp
├── webui/                            # Independent Vite front-end project
│   ├── src/
│   │   ├── main.js                   # App entry, API client, WebSocket
│   │   ├── style.css                 # Full UI stylesheet
│   │   ├── core/
│   │   │   ├── range.js              # Stroke-window slider & marker
│   │   │   └── ui.js                 # DOM helpers, $(), clamp()
│   │   └── features/
│   │       ├── generator.js          # Generator config panel
│   │       ├── motiongraph.js        # Real-time canvas telemetry graph
│   │       └── settings.js           # Settings & motor tab builder
│   ├── index.html
│   ├── vite.config.js
│   └── package.json
├── data/                             # LittleFS image source (auto-generated)
│   ├── index.html
│   └── index.html.gz
├── intiface/                         # Intiface device-config JSON
│   └── slopdrive32-device-config.json
├── build_webui.py                    # PlatformIO extra-scripting hook
├── platformio.ini
├── LICENSE
├── THIRD_PARTY_LICENSES.md
└── README.md
```

---

## Credits

- Concepts and wiring originally derived from **StrokeEngine** by the OSSM
  community.
- Built on [TMCStepper](https://github.com/teemuatlut/TMCStepper),
  [FastAccelStepper](https://github.com/gin66/FastAccelStepper),
  [ArduinoJson](https://arduinojson.org/),
  [arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets),
  [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino),
  [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel),
  and the Espressif Arduino core.
- The rest: vibes. 🤖
- Also, if you've read this far into the README, you already know what kind of
  machine this is. Don't pretend you don't. :3

---

## License

SlopDrive-32 is licensed under the **MIT License** — see [`LICENSE`](LICENSE).
Provided **as-is, with no warranty**.

This project links against third-party Arduino libraries and the Espressif
Arduino core, each under its own license (MIT / LGPL / Apache-2.0). See
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) for full attribution and
license details.
