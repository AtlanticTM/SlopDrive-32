# SlopDrive-32 — Refactor & Library Roadmap (CONSENSUS v1)

*Stamped 2026-07-23 after SlopSync went live-verified (probe 8/8). This is the
agreed map; change it deliberately, not by drift.*

---

## The North Star (stamped, do not gloss)

**SlopSync is end-to-end.** An application implements a SlopSync client
library and talks directly to the machine — discovery, capability
negotiation, telemetry, control, AND motion streaming. The transport zoo
(WSDM server, BLE UART, dongle bridge, TransportManager's exactly-one-live
arbitration) is scheduled for demolition as SlopSync absorbs each role.
Compat is layered, not middleware:

1. **Native motion streaming** — STREAM channel, client→hub, timestamped
   sample bundles (the wire format already supports this by design: ≤32
   samples / ≤20 ms span, c2h direction). Streaming clients parse TCode /
   funscript / anything on THEIR side and ship native samples.
2. **TCode pass-through channel** — raw TCode lines wrapped in a SlopSync
   channel for dumb-compat clients; hub feeds the existing parser. Bounded
   compat, never a second control plane.
3. **Legacy survivor** — raw serial TCode v3 @333 Hz (Intiface-over-USB)
   keeps its dedicated path indefinitely.

"More compat more better, but not to the point it gets convoluted."

---

## Where we stand

| Module | Status |
|---|---|
| **SlopSync** (protocol + lib + firmware hub) | LIVE — verified on hardware end-to-end, probe 8/8 |
| **SlopMotion** (Ruckig motion core, §1) | LIVE — lib/slopmotion + vendored Ruckig v0.19.4, 11 native suites green, trace bench + graphs, AND firmware wiring landed (see §10 below, CLAUDE.md §7.6) |
| **SlopLog** | LIVE — all legacy sites migrated, boot narration, serial handoff |
| **SlopGlow** | LIVE — liveness gate field-proven on day one |
| TMC2160 | 🪦 nuked (fw 2.1.38) |

---

## 1. Ruckig — STAMPED, top priority, both modes

Gold-standard jerk-limited motion calculation, replacing the cubic planner
entirely. Mode split falls out of what each input can know:

- **TCode v4 / one-command-per-move (interp data included): ONE-SHOT.**
  A complete move exists → compute the perfect jerk-limited profile from
  ACTUAL machine state (pos/vel/acc) to target within the commanded
  duration, execute it faithfully. "A planned move that is perfect" — this
  is the D4 doctrine with a better planner in the plan step.
- **TCode v3 / dense point streams (timing sometimes absent): CYCLIC
  TRACKING.** No move to plan — the future is unknown. Ruckig chases the
  newest target under v/a/j limits. Replaces the cubic interpolator; the
  ossm-rs liquid feel.
- Manual point moves stay FAS trapezoids (fine feel, simple fast path).
- Execution for both Ruckig modes rides the existing 1 kHz sampler →
  `submitStreamSample` arbiter path. No new clocked anything; the arbiter
  doctrine is untouched.

## 2. Boost SML — STAMPED with a scope cut

- **Homing FSM: yes** (hairiest, most safety-adjacent state logic).
- **OTA lifecycle: later, opportunistic.**
- **TransportManager: NO** — it is scheduled for demolition under the North
  Star; we don't renovate the gallows. Each SlopSync absorption step
  removes transport-manager surface instead.

## 3. ETL — demoted (final)

Own fixed structures + std cover us; String-churn paths die with
slopsync-js; no retrofit crusade. Revisit only on concrete need.

## 4. Async web server — parked (final); candidate pre-selected

Handlers-in-network-task is the failure class we just spent a day
exorcising. Sync WebServer + isolated WS tasks until HTTP is static-files +
OTA only, then re-evaluate whether it matters at all. (Interim mitigation
landed fw 2.1.40: ETag revalidation — reloads 304 in ~40 ms; only the
first-load ~600 ms stall remains.)

**Re-evaluation shortlist (researched 2026-07-23 — web-verified, so we
never re-shop this):**
- **PsychicHttp v3.x is the candidate for BOTH HTTP and websockets.** MIT,
  weekly releases through mid-2026, native ESP-IDF 5.5 support (v3.0.0),
  thin wrapper over esp_http_server: handlers/WS run in the server's OWN
  task (blocking contained — NOT the async_tcp/LwIP context), LRU socket
  purge + per-socket send timeouts built in. Its WS layer ships OUR
  failure-mode engineering as first-class features:
  `PSYCHIC_WS_MAX_PENDING_FRAMES` (=8) caps per-client queued frames (a
  stalled client is heap-bounded), static RX buffer (no per-frame alloc —
  multi-day-uptime fragmentation), optional PSRAM payloads, and `sendAll()`
  keeps serving healthy clients while one is wedged. Single-port URI-routed
  WS (retire :81/:82/:55555). The old "38 rps/conn" README number predates
  the v2/v3 rewrite AND measured echo round-trips, not push streaming.
- **links2004/arduinoWebSockets (incumbent): a NAMED, LIVE upstream defect,
  not a neutral status quo.** Issue #911 (open since 2024-10, unresolved):
  sendTXT/sendBIN block INDEFINITELY on slow/wedged connections, no send
  timeout — the exact `ws-send-blocks-http-mutex` incident we hand-patched
  (activity gates, stall mute, reaper, 500 ms TCP cap). Still maintained
  (v2.7.x through 2025-12) and contained by our defenses, but every new WS
  surface built on it inherits the defect.
- **ESP32Async/ESPAsyncWebServer (maintained fork): rehabilitated but not
  chosen.** The fork is active (monthly releases, 2026) and FIXED the old
  crash class: `WS_MAX_QUEUED_MESSAGES` bounded queues, discard-on-full
  default, `cleanupClients()` reaping. Still moves all handlers into its
  own AsyncTCP/LwIP event loop and replaces the HTTP server wholesale — a
  bigger architectural commitment than the workload needs. (The me-no-dev
  original remains abandoned/disqualified.)
- **Raw esp_http_server WS**: sound, bounded (`httpd_queue_work` fails
  closed), but choosing it = reimplementing PsychicHttp's per-client
  backpressure policy by hand for no gain.
- **Mongoose: license-blocked** (GPLv2/commercial dual) for a
  community-extensible firmware.
- **BENCH GATE stands:** no public push-streaming (not echo) benchmark
  exists for ANY candidate. Before migration: bench PsychicHttp v3 at our
  real pattern — small binary frames, 50–100 msg/s/client both directions,
  1–5 clients, one deliberately wedged. ITransport keeps the WS layer
  swappable regardless; HTTP and WS halves need not migrate together.

## 5. slopsync-js — ACTIVE (the WebUI refactor). Brief for the next agent.

**Operator-reported state (2026-07-24, fw 2.1.45):** the WebUI is slow,
laggy, takes a long time to reflect device state, and the stroke-window
control is broken. Expected — we are mid-refactor: the browser still speaks
the legacy UiSocket plane while every capability it needs now exists,
verified, on the SlopSync plane. Your job is to move it over, not to patch
the old plane. Read CLAUDE.md §3 (Ground Truth doctrine) and §8 (SlopSync
rules) before touching anything.

### 5.1 What the browser does TODAY (measured map, not guesses)
- Vanilla JS + Vite single-file bundle (`webui/src/`, entry `main.js`;
  `core/{link,wire,cmd,shadow,range,telebuf,api}.js` is the wire/state
  layer, `features/*.js` the tabs). ONE WebSocket: `ws://<ip>:81/ws/ui`,
  custom binary frames (Appendix C, `include/ui/UiProtocol.h`): 0x01
  telemetry ~45 Hz, 0x02 status 2 Hz, 0x04 interp ~45 Hz, 0x05 anomaly,
  0x06 stats, 0x10 CMD (JSON payload!) → 0x11 ECHO. Firmware side:
  `src/ui/UiSocket.cpp` senderTask Core 0 @22 ms + `src/ui/WebUI.cpp`
  (92 KB, sync WebServer :80, 28 routes, the §4 [STALL] class).
- The shadow lifecycle (`core/shadow.js`) is ALREADY doctrine-correct:
  controls render from `reported`, desired is a pending overlay, echoes
  carry post-clamp applied values, cfg_gen bumps force config resync.
  KEEP THIS MODEL — it survives the migration; only the wire under it
  changes.
- Known lag/breakage suspects to diagnose FIRST (don't assume — verify
  against the live device): staleness gates (`>150 ms` no telemetry →
  `body.stale`, `>1 s` → `body.suspended` which DISABLES all controls and
  blocks sends — a stalling telemetry feed makes the whole UI feel dead
  and may BE the "window doesn't work" symptom); cmd retry ladder (300 ms
  ×3) + overdue escalation masking lost echoes; cfg_gen resync clobber
  guards (`shadow.js:236-284`, `settingsAuthoritative` in `range.js`);
  the 100 ms HTTP-fallback poll hammering the sync WebServer when the WS
  degrades; links2004 WS server defects (§4 — sendTXT/sendBIN can block).
  Diagnose and note root causes in your report even for symptoms the
  migration will delete — we want to know WHAT was broken.

### 5.2 What SlopSync already offers the browser (all live-verified on hw)
- Hub WS `:82`, subprotocol `slopsync.v1`, 8-byte LE header + deterministic
  CBOR payloads. HELLO→WELCOME (session, roles, deadman, grants), CATALOG
  (self-describing channel list + packed layouts — BUILD THE UI FROM IT,
  §3 dynamic-modularity doctrine), CLOCK 0x05 (NTP-style, the UiSocket
  clock sync's replacement), SUBSCRIBE→GRANT at min(wish, catalog rate),
  retained safety on grant, NACK codes, slow-consumer eviction (never
  wedges on a stalled tab — the hub sheds it; this deletes the UiSocket
  activity-gate/reaper machinery wholesale).
- STATE channels (h2c): 0x0003 safety (retained), 0x0004 control-owner,
  0x0006 hub-status 1 Hz, 0x1100 motion ≤60 Hz (pos/tgt/speed/flags —
  replaces 0x01 telemetry), 0x1000 machine-config on-change (replaces
  config fetch), 0x1200 pattern-state, 0x1002 odometer (replaces 0x06),
  0x0007 session-events.
- INTENT channels (c2h) with post-clamp applied-value ECHO 0x0E — the
  ground-truth confirm the shadow layer needs: 0x3100 move, 0x3000
  config-set (window_min/window_max/user & input limit sets — THE
  stroke-window path), 0x3200 pattern-cmd, 0x3101 home; safety-intents
  0x0005 (stop/hold/pause/resume/estop_clear).
- Reference implementations for the wire, in order of usefulness:
  `clients/mfp-slopsync/SlopSync.cs` (complete C# client incl. CBOR codec
  — port its shape to JS), `tools/slopsync_probe.py` (Python, golden
  bytes), `test_slopsync_*` native suites. Browser CBOR: hand-roll the
  same minimal subset (ints/bstr/tstr/arrays/maps/f32); golden-byte-test
  it against the probe's builders like WireSelfTest.cs does. slopsync-js
  becomes the THIRD client implementation — same discipline: mirror the
  probe, never invent bytes.

### 5.3 Phasing (dual-plane, no big-bang cutover)
- **A — read plane:** slopsync-js core (connect/HELLO/CLOCK/subscribe/
  CBOR) + catalog-driven read-only cards; motion/safety/config/pattern/
  odometer STATE feeds the existing renderers alongside UiSocket. Ship,
  verify visually against the live device, measure staleness.
- **B — write plane:** intents with the existing shadow lifecycle wired
  to ECHO 0x0E applied values (stroke window FIRST — it's the reported
  defect; verify end-to-end: drag → 0x3000 → echo → band renders the
  device's clamped truth). Then move/pattern/mode/home/safety controls.
  Roles: viewer sessions render read-only (§9 trust model, enforcement
  flip comes later — build the UI assuming it).
- **C — demolition:** retire UiSocket frames one-for-one as their
  SlopSync replacement is verified (0x01→0x1100, 0x06→0x1002, 0x02→
  0x0006+0x1000, CMD/ECHO→intents/0x0E, clock→CLOCK). Delete senderTask
  + UiSocket when empty; port :81 dies (a §10 transport-demolition step).
  HTTP keeps only: static bundle, OTA, /api/log, /api/capabilities
  (bootstrap pointer to :82), and the sync-WebServer question then folds
  into §4's PsychicHttp decision — do NOT migrate the HTTP server in this
  refactor.
- SlopMotion plumbing debts ride along (CLAUDE.md §7.6): the 0x05 anomaly
  feed (currently deliberately silent — SLopLog only), the inert
  `interp_clamp_overshoot` toggle, and a /api/slopmotion tuning card.
  Anomalies want a proper SlopSync EVENT channel (new device channel id,
  catalog entry — follow the 0x2101 authoring pattern), not a UiSocket
  frame revival.

### 5.4 Constraints & verification (non-negotiable)
- Firmware-side: hub service is PSRAM-resident, ONE-TASK WS invariant,
  16 KB task stacks for a reason (see CLAUDE.md §8 field bugs 1-3);
  new STATE publishers follow the existing SlopSyncHubService publisher
  pattern; MotionArbiter sole-caller via the delegate, always. Catalog
  edits bump the etag — fine; the FROZEN conformance mini-catalog is
  untouchable. Registry discipline for anything wire-visible.
- Every migrated control: end-to-end verified against the LIVE device
  (payload sent + device state change + echo adopted) before its legacy
  path is deleted — a control that renders but drives nothing is a
  defect; optimistic UI is prohibited (§3). Page load ADOPTS device
  state. Back-to-back sessions without reboot is a mandatory regression
  pattern (field bug #3). Version-bump + OTA deploy per CLAUDE.md §6;
  `uploadfs` for UI-only changes (no reboot), verify with hard refresh.
- Perf acceptance: first meaningful state < 1 s after page load on LAN;
  motion card latency ≤ 1 frame at granted rate; dragging the stroke
  window must never freeze mid-drag on a healthy link. Measure before/
  after (the browser perf overlay in diag.js, plus hub-status).

## 6. SlopSim — STAMPED (name approved)

As close to the actual machine as makes sense: **motion + planning
validation at the `MotorDriver` seam** — a sim driver modeling the
kinematics FAS would execute. Explicitly NOT emulated: power electronics,
RS485/motor comms, FAS internals. Real Hub + real delegate + in-process
fault-injection transport + deterministic replay. v2: host-side WS
transport so the probe/UI/MFP connect to the sim as if it were hardware.

## 7. Board capability traits — STAMPED

Header-per-board (`boards/<name>.h`) defining one constexpr/macro surface,
zero templates. Built for WILD featureset variance — this is not a
single-linear-actuator-only idea. **Target single axis until rock solid**;
expandability is the design constraint, multi-axis is the future reward.
Board headers feed /api/capabilities + the SlopSync catalog so boards
advertise what they truly have.

## 8. Pairing UX — rough-in only (hardware still in flux)

Plumbing exists (PairingManager, NVS store, SlopGlow Pairing state).
WebUI card + PIN display when hardware settles.

## 9. Trust model — STAMPED: (C) viewer-default

Unpaired client = viewer (watch, never drive). Pairing grants controller.
**Security rider (NON-NEGOTIABLE): OTA rights are NEVER derivable from
SlopSync roles.** A paired controller can move the machine within limits;
it can NEVER flash code. OTA stays on its own token plane (HTTP +
X-OTA-Token, constant-time compare). Hardening backlog: per-boot nonce /
challenge-response so a sniffed token can't replay; keep admin distinct
from controller.

## 10. Sequencing — STAMPED

```
DONE ──► SlopMotion motion core (quintic waveform + Ruckig chase/guard —
         fw 2.1.39+, live-tuned via /api/slopmotion)
DONE ──► SlopSync inbound motion: STREAM c2h (motion-input 0x0084, publish
         grants, CLOCK responder — fw 2.1.42, probe 11/0: 250 bundles
         @50 Hz, zero wire loss, gate-verified)
         [TCode pass-through channel DEFERRED to post-MFP: would race
         TCodeParser across commsTask/hub task for zero MFP value —
         bounded compat, not a milestone gate]
DONE ──► MFP plugin (C# SlopSync client) — clients/mfp-slopsync, single-file
         Roslyn plugin, live-verified 8/8 vs fw 2.1.44 (mDNS discovery of the
         new _slopsync._tcp advert, 50 Hz granted, 250/250 zero wire loss).
         The milestone paid for itself: its LiveWireTest found the hub
         source-ownership teardown leak (dead session owned motion-input
         until reboot) — fixed across all six teardown paths, SPEC §6.8
         tightened, SI-11/12/13 added.
DONE ──► segment streaming (fw 2.1.45): channel 0x0085 motion-segment
         ({target u16/1e4, duration_ms u16, end_vel i16/1e3 | INT16_MIN
         sentinel}) feeds SlopMotion's quintic waveform path — one command
         per funscript action (~2–4 pkt/s vs 50), deadman held by §6.5
         PING keepalive (no protocol change). Plugin v0.2.0 Segments mode:
         keyframe-cursor emitter (MFP's own KeyframeCollection helpers),
         ScriptScale/Invert replicated, divergence watchdog warns when
         motion-provider/SmartLimit bends the axis. Bonus ground-truth fix:
         accepted STREAM bundles now clear a latched STOP like intents do
         (SI-15). Outputs-panel verdict: plugin injection impossible by
         design (entry-assembly scan + internal interfaces) — native
         Outputs entry = small upstream PR (~400 lines, template:
         WebSocketOutputTarget); shortcut actions SlopSync::Connection::*
         registered as the native-feel bridge.
NOW  ──► slopsync-js / WebUI refactor (operator call 2026-07-24: UI is the
         pain point — slow, laggy, stroke window broken. Full brief in §5)
     ──► merge feat/cpp20-slopsync → main + pairing rough-in (model C)
     ──► widen: SlopSim v1 ∥ board traits
     ──► C5 nodes (ESP-NOW transport — spec pre-fitted: min_transport_payload
         242 = ESP-NOW 250 minus our 8-byte header)
     ──► TransportManager demolition as absorption completes
     ──► SML homing FSM (opportunistic)
```

---

## Refactor backlog (unchanged order)

1. Pairing enforcement flip (model C) — after UX exists.
2. SeqRing<T,N> promotion (telemetry + anomaly rings).
3. RAII CritSection guard (~30 raw portENTER/EXIT sites).
4. LE byte writers for UiSocket — batch with slopsync-js Phase C.
5. Hub-side STREAM pacing (slopsync-core, additive).
6. Deferred deletions: SystemState dormant buf[] + gen_rate_tick_hz
   (config-migration story), legacy esp32-s3-devkitc-1 env at merge.
   ServoModbus::sendSetpoint stays (deliberate future API).
7. SLOPLOG_COMPILE_LEVEL=2 release profile when debugging calms.
8. Motor-tab dead config (stepper-era DriverConfig fields drive nothing on
   the servo) — batch with #6, needs config migration.
9. OTA hardening: per-boot nonce / challenge-response (see §9 rider).
