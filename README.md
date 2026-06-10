# SlopDrive-32

An ESP32-S3 firmware for a belt-driven linear stroke machine, controlled over
[Intiface](https://intiface.com/) / [buttplug.io](https://buttplug.io) with a
small web UI for trimming the machine's physical stroke range.

It maps incoming normalized position commands (`0.0`–`1.0`) from the
buttplug/TCode ecosystem onto a user-configurable slice of the machine's
physical travel, so you can move and resize the active stroke region anywhere
across the full mechanical range without touching the firmware.

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

## Features

- **Buttplug / Intiface control** via TCode (`tcode-v03` stroker profile).
- **Serial control mode** — USB Serial is dedicated to Intiface's serial comm
  manager, bypassing WiFi for the lowest latency/jitter.
- **WiFi web UI** for live configuration:
  - **Stroke Range** — set min/max physical positions; move and resize the
    active stroke region anywhere across the 240 mm of travel.
  - **Max Speed** and **Acceleration** caps.
  - **Inertia / predictive smoothing** to keep motion fluid between discrete
    position updates.
  - **Auto-duration** — sizes each move to the *measured* command cadence
    instead of trusting the app's (often fixed/bogus) duration value.
  - **Motor tab** — live TMC2160 tuning (run/hold current, StealthChop vs
    SpreadCycle, thresholds, chopper settings) with plain-English tooltips.
  - **Serial / Log** viewer (since the USB serial port is busy with TCode).
- **Settings persist** to NVS (flash) and reload on boot.
- **Web UI served from LittleFS** (in `data/`), not compiled into the firmware.
- **Push-to-home** — establish the home position by simply pushing the shaft
  into the endstop; no separate homing button required.

---

## Hardware

- **MCU:** ESP32-S3 (DevKitC-1, 8 MB flash)
- **Driver:** TMC2160 (SPI), `R_SENSE = 0.15 Ω`
- **Motor:** NEMA stepper, 200 steps/rev, 16 microsteps
- **Mechanics:** 20-tooth, 2 mm-pitch belt → 40 mm/rev → **80 steps/mm**
- **Travel:** 240 mm physical limit
- **Endstop:** active-LOW limit switch
- **Status LED:** single NeoPixel

### Default pin map (ESP32-S3)

| Function        | GPIO |
|-----------------|------|
| STEP            | 21   |
| DIR             | 20   |
| ENABLE (act-low)| 19   |
| ENDSTOP         | 47   |
| TMC CS          | 11   |
| TMC SCLK        | 12   |
| TMC MOSI        | 13   |
| TMC MISO        | 10   |
| NeoPixel        | 48   |

All hardware constants live in [`include/config.h`](include/config.h).

---

## Getting started

This is a [PlatformIO](https://platformio.org/) project.

### 1. Configure secrets

WiFi credentials and the Intiface host are kept out of git. Copy the template
and edit it:

```sh
cp include/secrets.example.h include/secrets.h
```

Then set your `SECRET_WIFI_SSID`, `SECRET_WIFI_PASSWORD`, and (if using the
WiFi/WSDM path) `SECRET_INTIFACE_HOST` / `SECRET_INTIFACE_PORT`.

> `include/secrets.h` is git-ignored. If it's missing, the build falls back to
> harmless placeholders and emits a warning so a fresh clone still compiles.

### 2. Build & flash the firmware

```sh
pio run -e esp32-s3-devkitc-1            # build
pio run -e esp32-s3-devkitc-1 -t upload  # flash firmware
```

### 3. Upload the web UI (LittleFS)

The web interface lives in `data/` and is flashed as a **separate** filesystem
image. Do this once (and again whenever you edit `data/index.html`):

```sh
pio run -t uploadfs
```

> If you skip this step, the device still boots and serves a small fallback page
> telling you to upload the filesystem.

### 4. Open the web UI

Connect to the device's IP (shown in the log) or `http://slopdrive32.local`.

---

## Control modes

- **Serial control mode** (`SERIAL_CONTROL_MODE = 1`, default): add a *Serial*
  device in Intiface pointing at the ESP32's COM port (115200 baud). USB Serial
  carries TCode; debug output goes to the web UI's **Serial / Log** panel.
- **WiFi / WSDM client:** with `SERIAL_CONTROL_MODE = 0`, the device connects
  out to Intiface's "Device WebSocket Server" as a client.
- **Direct WebSocket server:** the device also runs its own TCode WebSocket
  server (e.g. for MultiFunPlayer) on port `55555`.

See [`include/config.h`](include/config.h) for the relevant toggles and the
detailed comments explaining each path.

---

## How range mapping works

Incoming commands are normalized `0.0`–`1.0`. The firmware maps that onto the
**trimmed** range you set in the web UI:

```
physical_mm = min_position_mm + normalized * (max_position_mm - min_position_mm)
```

So if your machine can travel 240 mm but you set the range to `60 → 180`, the
full `0.0`–`1.0` input stroke is remapped into that 120 mm window — and you can
slide that window anywhere along the rail or trim either end, all from the UI.

---

## Project layout

```
include/   config.h, hardware/driver headers, secrets.example.h
src/        main.cpp, motor, buttplug/TCode, range mapper, app log
data/       index.html  (web UI, flashed to LittleFS)
platformio.ini
```

---

## Credits

- Concepts and wiring originally derived from **StrokeEngine** by the OSSM
  community.
- Built on [TMCStepper](https://github.com/teemuatlut/TMCStepper),
  [FastAccelStepper](https://github.com/gin66/FastAccelStepper),
  [ArduinoJson](https://arduinojson.org/),
  [arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets), and
  the Espressif Arduino core.
- The rest: vibes. 🤖

## License

No license is specified yet. Until one is added, treat this as
**all rights reserved** / personal-use reference code. Provided **as-is, with no
warranty**.
