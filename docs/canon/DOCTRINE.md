# DOCTRINE — SlopDrive-32 engineering rules

The single home ([CANON C-1](CANON.md)) for the project's technical doctrine: architecture
constraints, build/deploy procedure, and the per-subsystem NON-NEGOTIABLE
rules. Governance meta-law lives in [`CANON.md`](CANON.md); volatile status in
[`LEDGER.md`](LEDGER.md); field-bug mechanisms in [`TRAPS.md`](TRAPS.md). Operator preferences live in
`CLAUDE.md` (repo root, gitignored).

The product: an extensible, high-performance modular linear motion control
platform — a sex machine usable with penetrables, dildos, etc. — on the
ESP32-S3 ecosystem. Hardware-agnostic, community-extensible.

## 1. Hardware-Agnostic Modularity
* **Driver polymorphism:** all physical hardware interactions (motors,
  sensors, inputs) sit behind C++ interface classes (ABCs with pure virtuals).
* **Build configuration:** conditional-compilation flags (`#if defined(...)`)
  isolate hardware driver implementations; unused driver objects are not
  compiled.
* **Pin allocation:** no hardware pins inside functional classes. Pins,
  serial ports, and timer channels are constructor-injected or mapped in
  `include/system/config_api.h`.

* **Module boundary doctrine (operator-ratified 2026-07-27):** functional
  cores are LIFTABLE — hardware-free, dependency-injected (clock, randomness,
  transport, motor handed IN, never grabbed), native-testable, stitchable
  into another codebase (the slop-libs are the proof pattern). Glue is the
  opposite and proudly so: the composition root (main.cpp), delegates, and
  board wiring are deliberately machine-specific, small, and honest. The
  smell to hunt is a core-shaped thing living inside glue. New functionality
  starts by deciding which of the two it is.

## 2. Core Performance & Safety (NON-NEGOTIABLE)
* **Non-blocking runtime:** operational loops and real-time motion paths never
  block. `delay()` is prohibited during regular runtime; short blocking delays
  are permitted ONLY in boot/init, module hardware setup, and isolated
  slow-speed calibration/homing cycles.
* **Motion doctrine — event-driven, never clocked:** ONE COMMAND → ONE PLAN →
  the motion engine executes. Plans are computed at intent arrival from the
  machine's ACTUAL state (live position + velocity); speed/accel are DERIVED
  from the intent and CLAMPED at ceilings. Ceilings are never targets
  (exception: deadline-less manual point moves plan at user ceilings). A loop
  computing positions on a clock is rebuilding a disease this project already
  cured. Segmentation follows commands and waveform structure only.
* **MotionArbiter sole-caller rule:** the MotionArbiter is the ONLY component
  that commands the motor driver for positioning. Input sources (manual UI,
  TCode transports, PatternEngine, SlopSync) never call the driver —
  they submit intents; the arbiter owns arbitration, limit-set selection (user
  set for manual, input set for machine-driven), and every safety gate (homed,
  paused, e-stop, window clamping, soft-start).
* **Dual-core separation:** Core 0 = system & comms (networking, LittleFS,
  WebSockets, transport/TCode parsing, monitoring). Core 1 = motion real-time
  (arbiter dispatch, plan submission, step timing).
* **Cross-core data:** anything shared between cores uses FreeRTOS primitives
  (atomics, mutexes, `xQueue`). Async-library callbacks run on the library's
  own task — enqueue, never mutate owner state ([TRAPS.md](TRAPS.md) T5).

## 3. WebUI Integration & Build Chain
* **Compile-time asset bundling:** web assets are an independent front-end
  project (Vite) built via PlatformIO extra scripts — never C++ strings.
  Outputs are minified + gzipped into the LittleFS image.
* **API-driven modularity:** the UI loads capability/config data dynamically
  and builds settings cards from what the firmware advertises.
* **Ground Truth Doctrine (NON-NEGOTIABLE):** the UI must NEVER display
  machine state that differs from the device's, in either direction. Page load
  ADOPTS device state. Echoes report APPLIED (post-clamp) values. Optimistic
  UI state is prohibited (shadow desired/reported, pending → echo-confirmed).
  Every new/modified control is verified end-to-end against the live device
  before it is done — a control that renders but drives nothing is a defect; a
  UI that lies about machine state is a safety defect on this product.

## 4. Coding Style & Extensibility
* Strict OOP; `.h`/`.cpp` isolation; lifecycle hooks (`init()`, `update()`,
  `emergencyStop()`) on functional modules.
* `float` over `double` (S3 hardware FPU) — double math only at plan-time
  events, never per-sample.
* Comments are constraints, not stories ([CANON C-12](CANON.md)).
* **Comment style law (operator-stamped 2026-07-28, applied retroactively
  by the ruling-5 pass):** scope src/, include/, lib/, sim/, test/ C++.
  * FILE HEADER: every .h/.hpp/.cpp opens with `// <Name> — <one-line job>`,
    then `// Constraints:` lines holding only load-bearing rules (threading,
    ownership, units, never-do's), then `// See:` pointers if real ones
    exist. No history, no authorship, no dates, no feature lists.
  * SECTION BANNERS: `// ---- <section name> ----` dash-padded to column
    80 (the tree's dominant pre-law width; amended from 76 on 2026-07-28
    to normalize to reality instead of repadding against it). Name only —
    no hex ids, no numbering, no box art. RFC-nnn/T-nn references in a
    banner name are POINTERS (allowed), not numbering. Files over ~150
    lines are divided into their natural sections this way.
  * STYLE: `//` everywhere; `/* */` only in license headers. Multi-line =
    consecutive `//` lines indented with the code they bind to.
  * CONTENT (C-12 mechanized): a comment is exactly one of — a CONSTRAINT,
    an INVARIANT the code cannot show, a POINTER (TRAPS/SPEC/docs), or
    `// TODO(<ledger item or RFC-nnn>): <change>`. A TODO without a home
    reference is a finding. Narration, history, restated code, and diff
    justification are deleted; a story worth keeping moves to docs/ with a
    pointer left behind.
  * VOICE: American English, fragments fine, no first person, no emoji.
* **Naming doctrine:** invented ecosystem-level things (protocols, subsystems,
  tools) get zero-collision, SEO-unique names ("SlopSync", never
  "SyncManager"). Ordinary classes/variables keep plain descriptive names.

## 5. Building & Testing
* pio: `%USERPROFILE%\.platformio\penv\Scripts\platformio.exe`; host Windows 11.
* Native tests: `pio test -e native` with WinLibs MinGW-w64 on PATH
  (`/c/Users/Atlan/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin`).
  Trust exit codes, not PIO's doctest summary ([TRAPS.md](TRAPS.md) T10).
* Cross-compile proof each milestone: xtensa `-fsyntax-only` on library TUs
  AND `pio run -e sd32-ota` green.
* `python tools/canon_lint.py` gates every substantive change ([CANON](CANON.md) §5).

## 6. Deployment (OTA)
* **Scope:** OTA is the S3 main controller ONLY (`sd32`/`sd32-ota` extend
  `s3_main`). The C5 nodes (`c5_waveshare`, `c5_tdongle`) have no web
  server/UI image and flash over USB-JTAG serial only.
* **OTA is the default path.** No USB hunting, no serial fallback unless OTA
  is confirmed unavailable. Device's usual address: `192.168.1.229` (works via
  Tailscale).
* **Commands:**
  * Firmware: `platformio.exe run -e sd32-ota -t upload` (firmware.bin only —
    the web bundle does NOT ship with this).
  * Web UI: `platformio.exe run -e sd32-ota -t uploadfs` (no reboot).
  * Both changed → BOTH commands, `upload` then `uploadfs`; never report a
    combined change deployed after one.
  * curl fallbacks: `curl -H "X-OTA-Token: <secret>" -F
    "image=@.pio/build/sd32-ota/firmware.bin" http://192.168.1.229/api/ota`
    (firmware) / `...littlefs.bin ... /api/ota/fs` (UI).
  * Secret: `SECRET_OTA_PASSWORD` in git-ignored `include/secrets.h`
    (template `include/secrets.example.h`). Never inline it anywhere durable.
* **If unreachable:** do NOT guess IPs, port-scan, or seize serial — stop and
  ask the operator for the device's IP and machine state.
* **Before flashing:** machine idle, operator aware. **After flashing:**
  verify `fw_version` via `/api/capabilities` (source of truth:
  `FIRMWARE_VERSION` in `config_api.h`) — upload completed ≠ deployed (C-8).
  After `uploadfs`: hard refresh, verify the change visibly.
* **Serial/USB is rescue-only** (bootloop, WiFi-breaking change, partition
  work) — a bench act the operator performs.

## 7. SlopLog & SlopGlow (NON-NEGOTIABLE usage)
Self-contained ecosystem modules: hardware-free core + thin Arduino glue,
vendorable to C5 nodes; header-only via explicit `-I lib/<name>/include`.
* **Logging goes through SlopLog. Only.** `SLOGT/D/I/W/E/F("tag", fmt, ...)`
  from any task, either core (bounded format + spinlock slot copy; never
  blocks/allocates; NOT ISR-safe). Throttle with `SLOGx_EVERY_MS`. Compile
  floor `SLOPLOG_COMPILE_LEVEL`. One drain point: `applogDrain()` in httpTask;
  sinks implement `sloplog::ISink`, registered in `applogBegin()`
  (`src/system/AppLog.cpp` is ONLY the sink/bridge). No `Serial.print` debug
  output, no new log macros (WebUI JS exempt). Sinks never block ([TRAPS.md](TRAPS.md) T6).
* **Boot lifecycle:** `applogBegin()` immediate-drains during single-task
  setup(); main.cpp disables that before task creation; first `/api/log` serve
  demotes serial to Warn+.
* **LEDs go through SlopGlow. Only.** Callers speak semantics
  (`slopglowEngine().raise/clear/set(GlowState::X)`); board wiring lives in
  `src/system/SlopGlowBoard.cpp`. Never `digitalWrite`/`ledcWrite` an LED
  elsewhere.
* **The LED liveness gate is a safety feature:** animation advances only while
  every registered heartbeat pulses (motorTask Core 1, commsTask Core 0; pump
  on httpTask). Never defeat it; frozen LEDs are a diagnostic ([TRAPS.md](TRAPS.md) T7).

## 8. SlopMotion (`lib/slopmotion/`)
Every command becomes ONE trajectory planned from the engine's actual
(p, v, a); the 1 kHz sampler evaluates it. Event-driven, never clocked.
* **Map:** header-only, hardware-free `slopmotion::Engine` wrapping vendored
  `lib/ruckig/` (BYTE-IDENTICAL to upstream — wrap, never patch;
  `lib/ruckig/VENDORED.md`). Tests `test/native/test_slopmotion`; scenario
  harness `examples/slopmotion_traces/` (includes the retired cubic as bench
  baseline — not firmware).
* **Division of labor (MEASURED — re-run the bench before re-litigating):**
  Ruckig Community is a point-to-point planner, not a waveform interpolator.
  WAVEFORM (duration-carrying segments) = C2 quintic Hermite over exactly the
  commanded duration, ceiling+window scanned, illegal shapes → Ruckig guard.
  CHASE (dense bare points) = Ruckig replan-per-point with predictive aim.
  SETTLE = brake-to-rest when a plan ends still-moving with no fresh command.
* **Safety:** Ruckig Community has NO position limits and quintics can bulge —
  the Engine owns the window: targets clamped, end velocities bound-safe,
  quintics legality-scanned, sampled output clamped. Exceptions never
  instantiated; non-finite inputs rejected at `commit()`.
* **Sampler task stack is 16 KB** — `commit()` nests KB-scale Ruckig
  temporaries. Never shrink it ([TRAPS.md](TRAPS.md) T1 class).

## 9. SlopSync (protocol + library — NON-NEGOTIABLE)
The ecosystem sync protocol (device-shadow + capability negotiation) is
developed in its OWN repo, **SlopSync** (sibling checkout `../SlopSync`,
pinned at this repo's root by `slopsync.pin`) — this machine repo CONSUMES
it, never edits it. The spec (SPEC.md, RENDERING.md, registry.yaml, its
codegen, its own library invariants, its own frozen-artifact list, its
`lib/slopsync/` layering and tests) is that repo's doctrine now, documented
there — restating it here would violate CANON C-1. What follows is this
machine's half of the boundary.
* **Consumption mechanics:** `platformio.ini` pulls `lib/slopsync` via
  `symlink://../SlopSync/lib/slopsync` in `common_s3_libs.lib_deps`, and an
  explicit `-I../SlopSync/lib/slopsync/include` for `env:native` (the two
  surviving native suites, `test_slopsync_devicecatalog` +
  `test_slopsync_discovery`, are this machine's own headers exercising the
  sibling's library). `tools/gen_channel_map.py` and `tools/gen_channel_grid.py`
  read the sibling's `spec/registry/registry.yaml` to render THIS device's
  `docs/slopsync/CHANNEL-MAP.md`. `tools/catalog_lint.py` reads the sibling's
  generated `registry_constants.hpp`. `tools/canon_lint.py`'s pin rule FAILs
  if `../SlopSync`'s HEAD doesn't match `slopsync.pin`, and cross-checks the
  sibling's frozen conformance artifacts (`mini_catalog.hpp`,
  `mini-catalog.yaml`) against the same hashes SlopSync's own
  `tools/slopsync_lint.py` pins — belt and suspenders across the repo
  boundary.
* **Spec-gap ritual (cross-repo order):** need a number/rule the spec lacks →
  fix it in the SlopSync repo FIRST (registry.yaml/SPEC.md, regenerated,
  committed there), bump `slopsync.pin` to the new sha, THEN code against the
  constant here. Never a code-local magic number for anything wire-visible,
  and never a spec change made from this repo.
* **Frozen ([CANON C-6](CANON.md)):** the conformance artifacts and the
  `hub.hpp`/`client.hpp` public API freeze are SlopSync's own frozen list now
  (enforced by its `tools/slopsync_lint.py`); this repo's belt-and-suspenders
  half is the sha256 cross-check in `tools/canon_lint.py` described above.
* **Firmware shape:** `SlopSyncHubService` (composition root, own Core-0
  task, single-task hub — [TRAPS.md](TRAPS.md) T5) + `SlopSyncAsyncWsTransport`
  (AsyncWebSocket on `SLOPSYNC_WS_PORT`, subprotocol `slopsync.v1`) +
  `SlopSyncCatalog.h`. The service lives in PSRAM via placement-new from
  main.cpp ([TRAPS.md](TRAPS.md) T2) — never move it back to BSS. Session teardown funnels
  through one path ([TRAPS.md](TRAPS.md) T3); **back-to-back sessions without a reboot is
  mandatory verification for any session-lifecycle change.**
* **Auth:** `validateToken` = `/uitoken` → trust ledger → `watch`. Tokenless
  clients can watch and e-stop (stop/estop role-EXEMPT) but not command
  motion. While `/uitoken` is enabled, LAN HTTP = control; lockdown posture
  buys a chokepoint, not LAN secrecy.
* **SlopSync is the ONLY input/output plane** (operator ruling 2026-07-26):
  motion input, telemetry, anomaly events, and settings ride SlopSync
  channels; HTTP remains for fallback polling and bootstrap only
  ([`docs/http-plane-retirement.md`](../http-plane-retirement.md)).
* **Transport doctrine (operator rulings 2026-07-27, calibrated):** SlopSync
  is the only protocol; transport-agnostic (SlopSync SPEC §13, SlopSync RFC-043 profiles).
  Hardware hubs: **BLE GATT is the conformance floor** (infrastructure-free
  control, discovery, future WiFi provisioning); **WebSocket is the
  preferred high-throughput path**, expected on ESP32-class silicon; clients
  auto-upgrade BLE→WS. **ESP-NOW** is the ESP32-peer/remote binding:
  supported and deliberately trivial to enable, NOT actively developed or
  tested. UI-serving is a hub capability, never a requirement (WROOM-D /
  OSSM-reference-PCB hubs are first-class and serve nothing).
* **Intake doctrine (operator ruling 2026-07-27):** on THIS machine the only
  way in and out is SlopSync. Other firmwares are never forced — SlopSync
  competes via the CLIENT ONRAMP (SlopSync RFC-044): TCode passthrough (criminally
  easy — clients feed the TCode they already generate through a SlopSync
  session) → native segments (0x2101, better) → native samples (0x2100,
  dense). First-party client support in MFP/Intiface/etc. is maintained and
  encouraged. Legacy raw-TCode transports (SER/BT/DONGLE) were REMOVED
  2026-07-27 (operator ruling executed: SlopSync is the only plane; TCode
  integration is a CLIENT-SIDE adapter per SlopSync RFC-044, never a hub-side
  stream). This firmware carries a BLE GATT `ITransport` (deployment state:
  [`LEDGER.md`](LEDGER.md), never here); the `OssmBleService` masquerade was
  REMOVED 2026-07-27 (SlopSync-over-BLE replaces it, ledger has the
  receipt).
* **Clients:** the MFP plugin (SlopSync.cs + SlopSync.xaml, dev-only harnesses
  never ship) and the verifier (`tools/slopsync_probe.py --ip <ip> --port 82`)
  both live in the SlopSync repo's `clients/mfp/` and `tools/` now.
  `LiveWireTest` refuses to run homed; run it TWICE back-to-back
  ([TRAPS.md](TRAPS.md) T3 check).
* **Branch/milestone status:** [`docs/canon/LEDGER.md`](LEDGER.md) — never here.
