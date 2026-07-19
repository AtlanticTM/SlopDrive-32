# SYSTEM ARCHITECTURE & MODULARITY CONSTRAINTS
You are writing firmware for an extensible, high-performance modular linear motion control platform — a sex machine usable with penetrables, dildos, etc. — running on the ESP32-S3 ecosystem. The architecture must be highly flexible, agnostic to specific hardware models, and built for community extensibility.

## 1. Hardware Agnostic Modularity
* **Driver Polymorphism:** All physical hardware interactions (motors, sensors, inputs) must be abstracted behind clean C++ Interface classes (Abstract Base Classes with pure virtual functions).
* **Build Configuration:** Use conditional compilation preprocessing flags (`#if defined(...)`) to isolate specific hardware driver implementations (e.g., separating TMC2160 stepper logic from Modbus servo logic). Avoid compiling unused driver objects.
* **Pin Allocation:** No hardware pins may be hardcoded inside functional classes. All pins, serial ports, and timer channels must be passed via class constructors or mapped in a single global configuration header (`config_api.h`).

## 2. Core Performance & Safety Rules (NON-NEGOTIABLE)
* **Execution Block Guardrails:** Operational loops and real-time motion generation paths must be strictly non-blocking. The use of `delay()` or blocking loops is explicitly prohibited during regular runtime use.
* **Initialization Exceptions:** Short, blocking `delay()` statements are permitted *only* during boot execution blocks, system initialization routines, module hardware setup sequences, and highly isolated, slow-speed protected calibration/homing cycles.
* **Motion Doctrine — Event-Driven, Never Clocked:** All motion is intent-based: ONE COMMAND → ONE PLAN → FastAccelStepper executes (its ISR is the sample rate). Plans are computed at intent arrival from the machine's ACTUAL state (live position + velocity), with speed/accel DERIVED from what the intent requires and merely CLAMPED at ceilings. Max speed/accel are CEILINGS, never targets (exception: deadline-less manual point moves legitimately plan at the user ceilings). If you find yourself writing a loop that computes positions on a clock, stop — you are rebuilding a disease this project already cured. Segmentation follows commands and waveform structure only.
* **MotionArbiter Sole-Caller Rule:** The MotionArbiter is the ONLY component that commands the motor driver for positioning. Input sources (manual UI, TCode transports, PatternEngine, OSSM BLE) never cross streams and never call the driver directly — they submit intents to the arbiter, which owns arbitration, limit-set selection (user set for manual, input set for everything machine-driven), and every safety gate (homed, paused, e-stop, window clamping, soft-start).
* **Dual-Core Task Separation:**
    * **Core 0 (System & Comms):** Handles high-level networking, LittleFS asset serving, WebSockets (prefer raw binary streams for motion telemetry), transport/TCode parsing, and asynchronous system monitoring.
    * **Core 1 (Motion Real-Time):** Dedicated strictly to motion execution — arbiter dispatch, FastAccelStepper plan submission, step timing, and real-time safe motion handling.
* **Thread-Safe Data Sharing:** Any data structure, state variable, or ring buffer passed between Core 0 and Core 1 must be strictly protected using FreeRTOS primitives (atomic variables, mutexes, or `xQueue`).

## 3. WebUI Integration & Build Chain
* **Compile-Time Asset Bundling:** Web assets (HTML, CSS, JavaScript) must not be written natively as C++ strings. Instead, the UI must be designed as an independent front-end project processed during compilation via a build pipeline toolchain (e.g., Node.js/Vite/Webpack scripts executing inside PlatformIO extra scripting hooks).
* **Target Filesystem Optimization:** The pipeline must minify, consolidate, and ideally Gzip the compiled build outputs before packing them into a production-ready **LittleFS** filesystem image to be flashed alongside the firmware.
* **Dynamic Modularity (API Driven):** To handle runtime setting adjustments and hardware variance without modifying the compiled web assets, the UI must load data dynamically via JSON REST APIs or WebSocket handshakes. The interface must use these configuration structures to programmatically hide, show, or build out UI settings cards on the fly depending on what features the firmware advertises as active.
* **Ground Truth Doctrine (NON-NEGOTIABLE):** The UI must NEVER display machine state that differs from the device's, in either direction. Page load ADOPTS device state — it never pushes defaults onto a live session. Echoes report APPLIED (post-clamp) values from the driver, never pre-clamp requests. Optimistic UI state is prohibited: controls reflect confirmed device state (shadow desired/reported with pending → echo-confirmed lifecycle). Any new or modified control must be verified end-to-end against the live device (payload sent + device state change observed) before it is considered done — a control that renders but drives nothing is a defect, and a UI that lies about machine state is a safety defect on this product.

## 4. Coding Style & Extensibility
* Implement strict Object-Oriented Programming (OOP) with distinct `.h` header and `.cpp` implementation file isolation.
* Ensure all functional modules expose structured lifecycle hooks (`init()`, `update()`, `emergencyStop()`).
* Write clean, mathematically performant single-precision floating-point code (`float` over `double`) to fully utilize the ESP32-S3's hardware FPU capability.

## 5. Building
* pio executable is located at %USERPROFILE%\.platformio\penv\Scripts\platformio.exe
* system we are building on is windows 11

## 6. Firmware & Web UI Deployment (OTA)
* **SCOPE — OTA is the S3 MAIN CONTROLLER ONLY.** `sd32`/`sd32-ota` extend `s3_main` (Arduino Nano ESP32 / ESP32-S3) and build ONLY `src/main.cpp` + `comms/motion/system/ui` — the sole node with WiFi + WebServer + the LittleFS web bundle. The two ESP32-C5 relay/display nodes (`c5_waveshare`, `c5_tdongle`) have NO web server or UI image and are NOT OTA-updatable — they stay on their own USB-JTAG serial flashing (`-e c5_waveshare` / `-e c5_tdongle`). Everything below refers to the main controller.
* **OTA IS THE DEFAULT DEPLOYMENT PATH.** Do not ask the user to plug in USB, do not hunt for COM ports, and do not touch the serial environments (`s3_main`, `sd32`) unless OTA has been *confirmed* unavailable. Routine firmware and UI updates go over WiFi to the device's IP (works via Tailscale).
* **The commands (proven live, then documented):**
    * Firmware flash (firmware.bin ONLY — the web bundle does NOT ship with this): `%USERPROFILE%\.platformio\penv\Scripts\platformio.exe run -e sd32-ota -t upload`
    * Web UI (after a Vite change — no firmware reboot): `%USERPROFILE%\.platformio\penv\Scripts\platformio.exe run -e sd32-ota -t uploadfs`
    * A change touching BOTH firmware and UI requires BOTH commands — `upload` then `uploadfs`. Never report a combined change as deployed after only one.
    * curl fallback (firmware): `curl -H "X-OTA-Token: <secret>" -F "image=@.pio/build/sd32-ota/firmware.bin" http://192.168.1.229/api/ota`
    * curl fallback (web UI): `curl -H "X-OTA-Token: <secret>" -F "image=@.pio/build/sd32-ota/littlefs.bin" http://192.168.1.229/api/ota/fs`
    * The OTA secret lives in `include/secrets.h` as `SECRET_OTA_PASSWORD` (git-ignored; template in `include/secrets.example.h`). NEVER inline the real value in a command you commit, a doc, or a log. The `sd32-ota` env feeds it to espota automatically via `tools/ota_auth.py`.
* **The device's usual address is `192.168.1.229`.** If it is unreachable or an upload fails: do NOT guess other IPs, do NOT port-scan the network, and do NOT fall back to serial on your own. STOP and ask the user: *what IP is the device currently on, and what state is the machine in (powered? on WiFi? mid-crash? cable-connected?)*. Resume only with their answer.
* **Before flashing:** confirm the machine is idle (no pattern running). The firmware's OTA safety gate WILL stop motion itself before the first flash byte, but do not fire an update mid-session without the user knowing.
* **After flashing:** verify the new build actually landed — `curl http://192.168.1.229/api/capabilities` and quote the `fw_version` (source of truth: `FIRMWARE_VERSION` in `config_api.h`), or read it from the boot log. A completed upload is NOT a verified deployment until the version is confirmed. After `uploadfs`, verify the UI change is visible (hard refresh) the same way.
* **UI-only changes never require a firmware upload** — `uploadfs` + a browser refresh is the whole cycle, and the firmware/`fw_version` is untouched. Firmware changes reboot the device (~10–15 s offline is normal); warn the user if they're mid-session.
* **Serial/USB is the RESCUE PATH ONLY** — a bootlooped device, a WiFi-breaking change, or partition work. It is a bench operation the *user* performs with the `sd32` (or `s3_main`) env. Recommend *when* to use it; never silently seize it as your first move.