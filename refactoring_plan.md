# SlopDrive-32 — Modular Refactoring Plan

> Goal: Convert the monolithic `main.cpp` (1601 lines) into a clean, category-organized,
> OOP architecture so that adding new hardware drivers (V2 brushless servo) later is seamless.
> **This pass refactors V1 only.** No V2 code is written here.
>
> Each step is **independently compilable**. After every step the firmware must still build
> and behave identically. Work top-to-bottom; do not skip ahead. Check boxes as you go.

---

## 0. Ground Rules (read before starting)

- [ ] **Behavior must not change.** This is a pure structural refactor. If something behaves
      differently after a step, the step is wrong — revert and re-do.
- [ ] **One module per step.** Extract, compile, verify, commit. Never extract two modules at once.
- [ ] **Single-precision floats only** (`float`, not `double`) per `.clinerules` §4 — keep using the FPU.
- [ ] **No `delay()` in runtime paths.** `delay()` is permitted ONLY in `init()`/boot/homing
      (already the case in `motor.cpp::begin()`/`home()` — preserve that, don't add new ones).
- [ ] **Cross-core data must be protected** (atomics / mutex / `xQueue`) per `.clinerules` §2.
- [ ] **`.h`/`.cpp` isolation** per `.clinerules` §4. Every module exposes lifecycle hooks where
      it makes sense: `init()`, `update()`, `emergencyStop()`.
- [ ] Commit after each completed step with a message like `refactor: extract Kinematics`.

---

## Target File Tree (category folders)

```
include/  &  src/   (mirror each other)
├── system/
│   ├── config_api.h          # pins, geometry, build flags, structs, enums  (was config.h)
│   ├── AppLog.h/.cpp          # RAM ring-buffer logger                       (was applog)
│   ├── SystemState.h/.cpp     # shared, mutex/atomic-protected runtime state (the globals)
│   └── ConfigStore.h/.cpp     # NVS (Preferences) save/load
│
├── motion/
│   ├── MotorDriver.h          # ABSTRACT base class (pure virtual lifecycle + motion)
│   ├── TMC2160StepperDriver.h/.cpp  # legacy V1 impl, build-flag guarded, NO driver readback
│   ├── Kinematics.h/.cpp      # pure math: carrier/mod/ease/bufEase (FPU float)
│   ├── RangeMapper.h/.cpp     # intensity <-> mm
│   ├── Generator.h/.cpp       # GeneratorConfig + generatorTask (self-contained bundle)
│   └── Interpolator.h/.cpp    # jitter buffer + interpolatorTask (self-contained bundle)
│
├── comms/
│   ├── TCodeParser.h/.cpp     # pure TCode v0.3 parser + callbacks (transport-agnostic)
│   ├── WebSocketTransport.h/.cpp  # WS server (MFP) + Intiface WSDM client
│   ├── SerialTransport.h/.cpp # USB serial TCode line assembly
│   ├── BleTransport.h/.cpp    # NimBLE Nordic-UART service
│   └── TransportManager.h/.cpp# applyTransport() + WiFi/mDNS bringup
│
├── ui/
│   └── WebUI.h/.cpp           # all HTTP /api/* handlers + route registration
│
└── main.cpp                   # thin: setup() wires modules, loop() idles
```

### ⚠️ Build-system caveat (handle in Step 1)
PlatformIO compiles `src/**` recursively, but headers moved into subfolders of `include/`
need a working include path. Either:
- **(A)** add `build_flags = -Iinclude/system -Iinclude/motion -Iinclude/comms -Iinclude/ui`
  to `platformio.ini`, **or**
- **(B)** use relative includes (`#include "../system/config_api.h"`).
Choose **(A)** — it keeps `#include "Kinematics.h"` clean everywhere. Verify a clean build
*before* extracting any logic.

---

## Risk Register (flagged during analysis — keep these in mind throughout)

1. **Shared global state is the real monolith.** ~30 file-scope `static` globals in `main.cpp`
   are touched by 5 tasks across both cores; only the sample ring is mutex-protected. Moving
   *functions* is easy — moving *state* safely is the hard part. → Addressed early (Step 3).
2. **Dropping TMC driver readback touches the WebUI + frontend.** `/api/status` emits a
   `driver{}` object and `/api/clearfault` exists; `data/index.html` renders them. We feature-flag
   these OFF and keep the endpoints responding (empty/false) so the frontend never breaks.
3. **`SERIAL_CONTROL_MODE` is woven through logging + `sendResponse()` + serial polling.**
   When splitting transports, make it a per-transport concern, not scattered `#if`s.
4. **`STEPS_PER_MM` is a compile-time constant** baked into motor + main math. A V2 servo has
   different units. The `MotorDriver` base must own mm<->native conversion so callers stay
   unit-agnostic.
5. **`FastAccelStepper` is a hard requirement for ALL configs** (per project owner). Keep it in
   a shared location reachable by every driver — do NOT bury it inside the TMC-only `#if` block.

---

## Step 1 — Scaffolding & build path (no logic moves) — LOWEST RISK

- [ ] Create the empty category folders under `include/` and `src/`.
- [ ] Add include paths to `platformio.ini` (`build_flags = -O2 -Iinclude/system -Iinclude/motion -Iinclude/comms -Iinclude/ui`).
- [ ] **Rename** `config.h` → `system/config_api.h`; `applog.*` → `system/AppLog.*`. Update includes.
- [ ] **Move** existing modules unchanged into folders: `motor.*`, `range_mapper.*`, `buttplug.*`.
- [ ] **Verify:** `pio run` builds cleanly with zero behavior change. Flash & smoke-test homing + a stroke.
- [ ] Commit: `refactor: scaffold category folders + include paths`.

---

## Step 2 — Extract `Kinematics` (pure math) — SAFEST EXTRACTION

> Pure, stateless, zero hardware/RTOS dependencies. Perfect first real extraction.

- [ ] Create `motion/Kinematics.h/.cpp`.
- [ ] Move from `main.cpp` verbatim (keep `float` / FPU): `genCarrier()`, `genModShape()`,
      `genEase()`, and `bufEase()`.
- [ ] Expose them as free functions in a `kinematics` namespace (e.g. `kinematics::carrier(wave, p)`).
- [ ] Replace the originals in `main.cpp` with calls into the new header.
- [ ] **Verify:** build + run; generator and buffered modes behave identically.
- [ ] Commit: `refactor: extract Kinematics math`.

---

## Step 3 — Introduce `SystemState` (the shared-state container) — HIGH VALUE

> This is the keystone. Collect the loose globals into ONE owned, thread-safe struct.
> Do this BEFORE extracting tasks/handlers so they can take a `SystemState&` instead of
> reaching for file-scope globals.

- [ ] Create `system/SystemState.h/.cpp` with a `SystemState` struct/class holding:
  - [ ] Loading/flow: `homed`, `homing_in_progress`, `wifi_ready` (use `std::atomic` / `volatile`).
  - [ ] Config snapshots: `DeviceConfig config`, `DriverConfig driver`.
  - [ ] Control gating: `paused`, `manual_override`, `resume_start_ms`, `expert_mode`.
  - [ ] Transport: `transport` (`TransportMode`).
  - [ ] Cadence/auto-duration: `auto_duration`, `last_cmd_ms`, `measured_interval_ms`, `measured_hz`.
  - [ ] Default range: `default_range_min/max`.
  - [ ] Generator bundle state: `GeneratorConfig gen`, phase/clock fields, `gen_active`, `last_intiface_ms`, `gen_rate_tick_hz`.
  - [ ] Input/interp: `input_mode`, `buf_easing`, `buf_depth`, `buf_tick_hz`, the sample ring + its `portMUX`.
- [ ] Provide thread-safe accessors for anything crossing Core0<->Core1 (atomics for scalars,
      keep the existing `portMUX` critical sections for the ring buffer).
- [ ] Move `GeneratorConfig`, `InputMode`, `BufSample` type definitions into `SystemState.h`.
- [ ] In `main.cpp`, replace the ~30 globals with a single `SystemState g_state;` and point all
      existing code at `g_state.<field>`. (Mechanical find/replace; no logic change yet.)
- [ ] **Verify:** build + run; full behavior identical. This is the riskiest step — test thoroughly
      (home, stroke, pause/override, generator, buffered mode, transport switch).
- [ ] Commit: `refactor: centralize runtime state in SystemState`.

---

## Step 4 — Extract `ConfigStore` (NVS persistence)

- [ ] Create `system/ConfigStore.h/.cpp` owning its own `Preferences` instance.
- [ ] Move `saveConfig()` and `loadConfig()` in; change signatures to operate on
      `SystemState&` (+ `RangeMapper&`, `MotorController&` as needed) instead of globals.
- [ ] Expose `ConfigStore::save(state, mapper, motor)` and `ConfigStore::load(state, mapper, motor)`.
- [ ] Update `setup()` and all handlers that call save/load.
- [ ] **Verify:** settings persist across reboot exactly as before (range, speed, TMC tunables, transport).
- [ ] Commit: `refactor: extract ConfigStore (NVS)`.

---

## Step 5 — Extract `Generator` bundle

> Self-contained feature bundle: config + task + helpers.

- [ ] Create `motion/Generator.h/.cpp`.
- [ ] Move `generatorTask()` in. It depends on `Kinematics`, `SystemState`, `RangeMapper`, `MotorController`.
- [ ] Wrap in a small `Generator` class with lifecycle hooks: `init()` (creates task),
      `update()`/internal loop, and respects `emergencyStop()` (idles on pause/override/unhomed).
- [ ] Pass dependencies by reference in the constructor (no global reach-through).
- [ ] Update `setup()` task creation to call `generator.init()`.
- [ ] **Verify:** waveform generation (all shapes + FM/AM mod) identical; auto-yield to Intiface works.
- [ ] Commit: `refactor: extract Generator bundle`.

---

## Step 6 — Extract `Interpolator` bundle

- [ ] Create `motion/Interpolator.h/.cpp`.
- [ ] Move `interpolatorTask()` and `bufPushSample()` in. Depends on `Kinematics::bufEase`,
      `SystemState` (ring buffer + mux), `RangeMapper`, `MotorController`.
- [ ] Wrap in an `Interpolator` class with `init()`/`update()`/`emergencyStop()` and a
      `pushSample(float pos_mm)` method (replaces global `bufPushSample`).
- [ ] Keep the `portMUX` jitter-buffer protection EXACTLY as-is (`.clinerules` §2 thread-safety).
- [ ] Update `buttplugLinearCmd()` (still in main for now) to call `interpolator.pushSample()`.
- [ ] **Verify:** BUFFERED mode smoothness identical over WS/Serial/BLE.
- [ ] Commit: `refactor: extract Interpolator bundle`.

---

## Step 7 — Introduce `MotorDriver` abstract base + refactor TMC into `TMC2160StepperDriver`

> Future-proofs V2 (brushless servo). FastAccelStepper stays shared for ALL drivers (Risk #5).
> TMC2160 is legacy — DROP driver readback per project owner.

- [ ] Create `motion/MotorDriver.h` — pure abstract base class (pure virtual functions):
  - [ ] Lifecycle: `virtual void init() = 0; virtual void update() = 0; virtual void emergencyStop() = 0;`
  - [ ] Motion: `home()`, `moveTo(mm)`, `streamTo(mm, speed)`, `streamExtrapolated(...)`,
        `updateExtrapolation()`, `stop()`, `hardStop()`, `enable()`, `disable()`.
  - [ ] Status: `isHomed()`, `isMoving()`, `getPosition()`, `setMaxSpeed()`, `setAcceleration()`.
  - [ ] **Unit conversion owned by the driver** (Risk #4): `mmToNative()`, `nativeToMm()`.
  - [ ] FastAccelStepper engine/handle lives in the base or a shared helper (Risk #5), NOT inside
        a TMC-only `#if`.
- [ ] Rename `MotorController` → `TMC2160StepperDriver : public MotorDriver` in `motion/`.
- [ ] Wrap the entire TMC2160-specific implementation in a build flag, e.g.
      `#if defined(DRIVER_TMC2160)` in `platformio.ini` (`.clinerules` §1 conditional compilation).
- [ ] **DELETE legacy driver-readback** per project owner:
  - [ ] Remove `DriverStatus` struct, `readDriverStatus()`, `getLoadPercent()`,
        `getLastDriverStatus()`, `_last_status`, `_driver_faulted`, `clearDriverFault()`,
        `isDriverFaulted()`, and the `_spi_mutex` SPI serialization that only guarded readback.
        (Keep config WRITES; only readback/diagnostics are dropped.)
  - [ ] Delete `diagTask()` from `main.cpp` and its `xTaskCreatePinnedToCore` call.
- [ ] **Verify:** motor init, homing, push-to-home, streaming, generator all work. No crashes from
      removed SPI reads. (See Step 8 for the WebUI/API cleanup that pairs with this.)
- [ ] Commit: `refactor: add MotorDriver base, TMC2160StepperDriver, drop legacy readback`.

---

## Step 8 — Split `buttplug` into `comms/` (TCodeParser + 3 transports + WiFi)

> The current `ButtplugServer` does everything. Split the pure parser from the transports
> so each input path is its own file and a new transport can be added independently.

- [ ] **`comms/TCodeParser.h/.cpp`** — transport-agnostic core:
  - [ ] Move `parseTCode()`, magnitude decode, `D0/D1/D2/DSTOP` handling, and the
        `LinearCmdCallback`/`StopCallback` typedefs + registration.
  - [ ] Expose `feedLine(const char* str, size_t len)` and a `sendResponse` hook (injected per
        transport) so the parser never touches Serial/WS directly (resolves Risk #3).
- [ ] **`comms/SerialTransport.h/.cpp`** — move `pollSerial()` + serial line buffer + active/linked
      latches. Feeds bytes into `TCodeParser`. Owns `SERIAL_CONTROL_MODE` reply path.
- [ ] **`comms/WebSocketTransport.h/.cpp`** — move `_ws` server + `_client` WSDM client,
      `onWebSocketEvent`, `onIntifaceEvent`, `sendIntifaceHandshake`, `connectIntiface`,
      `disconnectIntiface`, `rxFrameCount`. Feeds `TCodeParser`.
- [ ] **`comms/BleTransport.h/.cpp`** — move `beginBLE/stopBLE/feedBleBytes`, the NimBLE callback
      shims (`BleServerCallbacks`, `BleRxCallbacks`), BLE state/latches. Guard NimBLE behind a
      build flag (`.clinerules` §1) so it can be compiled out. Feeds `TCodeParser`.
- [ ] **`comms/TransportManager.h/.cpp`** — move `applyTransport()`, `transportName()`, AND
      `setupWiFi()`/mDNS. Owns "exactly one transport live" logic; takes `SystemState&`.
- [ ] Update `buttplugTask()` (rename → `commsTask`) to drive whichever transports are active.
- [ ] **Verify:** all three transports (WS, SER, BT) still receive TCode and move the motor; mode
      switching + persistence intact; D0/D1/D2 replies still answer Intiface on each transport.
- [ ] Commit: `refactor: split comms into TCodeParser + WS/Serial/BLE transports + TransportManager`.

---

## Step 9 — Extract `WebUI` (all HTTP handlers)

> The bulk of `main.cpp`'s remaining size.

- [ ] Create `ui/WebUI.h/.cpp` owning the `WebServer httpServer` instance.
- [ ] Move ALL handlers in: `handleRoot`, `handleApiStatus`, `handleApiSettings`, `handleApiMove`,
      `handleApiHome`, `handleApiStop`, `handleApiPause`, `handleApiHalt`, `handleApiOverride`,
      `handleApiTmc`, `handleApiGen`, `handleApiInterp`, `handleApiLog`, `handleApiMode`,
      plus the `htmlFallbackPage` and the route registration block from `setup()`.
- [ ] Handlers take references to `SystemState`, `MotorDriver`, `RangeMapper`, `ConfigStore`,
      `Generator`, `Interpolator`, `TransportManager` — no global reach-through.
- [ ] **Pair with Step 7 cleanup (Risk #2):** in `handleApiStatus`, feature-flag the `driver{}`
      block (emit `valid:false` / omit) and make `handleApiClearFault` a no-op stub OR remove its
      route. The UI advertises active features so `data/index.html` can hide the driver-health and
      TMC-readback cards dynamically (`.clinerules` §3 dynamic modularity). Confirm the frontend
      doesn't error when those fields are absent.
- [ ] Expose `WebUI::init()` (registers routes + `begin()`) and `WebUI::update()` (the
      `handleClient()` loop body, was `httpTask`).
- [ ] **Verify:** every page tab + API endpoint works; settings save/load; no JS console errors
      from the removed driver fields.
- [ ] Commit: `refactor: extract WebUI handlers`.

---

## Step 10 — Re-wire `main.cpp` (final integration) — LAST STEP

> By now `main.cpp` should be nearly empty of logic. Make it a thin composition root.

- [ ] `main.cpp` declares the module instances (or a small `App` aggregator): `SystemState`,
      `TMC2160StepperDriver`/`MotorDriver&`, `RangeMapper`, `ConfigStore`, `Generator`,
      `Interpolator`, `TCodeParser`, the transports, `TransportManager`, `WebUI`.
- [ ] `setup()` becomes ordered wiring only: `AppLog::begin()` → mount LittleFS →
      `ConfigStore::load()` → `motor.init()` → `motor.applyDriverConfig()` → `TransportManager`
      WiFi → `WebUI::init()` → register comms callbacks → `applyTransport()` → create tasks.
- [ ] Keep the remaining glue callbacks (`buttplugLinearCmd`, `buttplugStop`) — either as thin
      free functions wired to the parser, or fold into an `InputRouter`. Route to
      `Interpolator::pushSample()` / `motor.streamExtrapolated()` as today.
- [ ] Re-create the FreeRTOS tasks with correct core pinning (`.clinerules` §2):
      - [ ] **Core 1 (real-time):** `motorTask` (1ms), `Generator`, `Interpolator`.
      - [ ] **Core 0 (system/math):** `commsTask`, `WebUI::update` task.
      - [ ] `diagTask` is GONE (removed in Step 7).
- [ ] `loop()` stays idle (`vTaskDelay`).
- [ ] **Verify (full regression):** boot → home (button + push-to-home) → stroke via WS, Serial,
      BLE → pause/override/resume blend → generator (all waveforms + mod) → buffered mode →
      settings save/load across reboot → transport switching. Confirm identical behavior to V1.
- [ ] Commit: `refactor: thin main.cpp composition root — modularization complete`.

---

## Done — Definition of Complete

- [ ] `main.cpp` contains only composition/wiring (target: < ~150 lines).
- [ ] Every module is `.h`/`.cpp` isolated and lives under `system/`, `motion/`, `comms/`, or `ui/`.
- [ ] `MotorDriver` is a pure abstract base; `TMC2160StepperDriver` is the only impl, build-flag guarded.
- [ ] FastAccelStepper is shared and reachable by any future driver.
- [ ] Legacy TMC driver readback / `diagTask` fully removed; WebUI hides the obsolete cards.
- [ ] Comms is split: one pure `TCodeParser` + independent WS/Serial/BLE transports.
- [ ] Generator and Interpolator are self-contained bundles.
- [ ] Shared state lives in `SystemState` with thread-safe cross-core access.
- [ ] Behavior is byte-for-byte identical to the original V1 firmware.
- [ ] Adding the V2 brushless-servo driver requires only a new `motion/ServoDriver.*` subclassing
      `MotorDriver` + a build flag — zero changes to comms, UI, generator, or interpolator.
