# SlopDrive-32 — Refactor & Library Roadmap

*Living document — iterate freely, strike things, reorder. Updated 2026-07-23
after SlopSync went live-verified (probe 8/8, fw 2.1.37).*

---

## Where we stand (context, not tasks)

The "mega modularization" now has three proven pillars, all following the same
pattern — hardware-free core + native doctest suite + thin ESP32 glue,
vendorable to other boards:

| Module | Status |
|---|---|
| **SlopSync** (protocol + lib + firmware hub) | LIVE — spec'd, 14 native suites, verified on hardware end-to-end |
| **SlopLog** (systemwide logging) | LIVE — all 165 legacy sites migrated, boot narration, serial handoff |
| **SlopGlow** (semantic LEDs + liveness gate) | LIVE — earned its keep on day one of existence |
| TMC2160 | 🪦 nuked (it never worked and it knows what it did) |

---

## 1. Libraries to adopt (the original shopping list)

### 1.1 Ruckig — jerk-limited motion planning ("peak tier")
The reason we migrated to C++20 in the first place. Community edition is
time-optimal jerk-limited OTG (online trajectory generation) — the thing that
makes ossm-rs streaming feel liquid.

- **Where it lands:** inside the MotionArbiter's planning step. Today a plan is
  trapezoidal via FastAccelStepper's own ramp; Ruckig would compute the
  jerk-limited profile and FAS (or the Modbus executor) tracks it. The
  D4 doctrine (ONE COMMAND → ONE PLAN → execute) is unchanged — Ruckig just
  makes the PLAN better. Also the natural upgrade path for
  `ServoMotionExecutor`'s hand-rolled jerk-limited tracker.
- **Watch out:** Ruckig wants a cyclic update loop for streaming mode — that
  collides with "event-driven, never clocked" if used naively. The correct
  marriage: Ruckig in *waypoint/one-shot* mode per intent, not a 1 kHz
  Ruckig tick. Needs a design pass before code.
- **Effort:** medium. **Risk:** medium (motion-path change = careful bench time).

### 1.2 Boost SML — compile-time state machines
Header-only, compile-time-checked transitions ("catches issues at compile
time, love it").

- **Candidates, in order of payoff:**
  1. **Homing FSM** (motorTask's hand-rolled homing sequence — the hairiest
     state logic in the firmware)
  2. Transport lifecycle in TransportManager (exactly-one-live + fallback)
  3. OTA lifecycle (idle → preparing → flashing → rebooting/failed)
- **Non-candidate:** SlopSync session states — the library is frozen-ish,
  deterministic, and already table-tested; don't churn it.
- **Effort:** small per-FSM. **Risk:** low (behavior-preserving rewrites with
  the compiler checking the transition table).

### 1.3 ETL (Embedded Template Library) — fixed containers
`etl::vector`, `etl::circular_buffer`, `etl::string` etc. — heap-free,
bounds-checked.

- **Honest reassessment:** the strongest original motivation (ad-hoc rings
  everywhere) has partly evaporated — SlopLog/SlopGlow/slopsync rolled their
  own fixed structures, and the house SeqRing idiom is small. ETL still earns
  its slot for *firmware-side* String elimination (ArduinoJson + WebUI String
  churn in HTTP handlers) and any new comms buffers.
- **Decision needed:** adopt broadly, adopt only for new code, or drop from
  the list. My lean: **new code only**, no retrofit crusade.

### 1.4 Async web server — *deliberately parked*
ESPAsyncWebServer's callback-context footguns (heap discipline, no blocking in
handlers) vs the now-tamed sync WebServer + isolated WS tasks. SlopSync over
WS is the real future control plane; the HTTP side is boot-strap + fallback.
**Recommendation: park until slopsync-js makes HTTP mostly static-file-only,
then re-evaluate whether it matters at all.**

---

## 2. Modules to build (Slop* pipeline)

### 2.1 slopsync-js — the WebUI becomes a SlopSync client
The big one. A browser-side SlopSync client (WS :82, CBOR decode, shadow
store) that progressively replaces the bespoke UiSocket binary protocol.

- Phase A: read-only — telemetry/status cards driven by SlopSync STATE
  subscriptions alongside the existing UiSocket (dual-plane, zero risk).
- Phase B: intents — controls send SlopSync INTENTs, echo-confirmed lifecycle
  maps 1:1 onto the existing Ground-Truth shadow pattern (`cfg_gen`,
  applied-values) the UI already implements.
- Phase C: retire overlapping UiSocket frames (keep the 240 Hz telemetry ring
  on UiSocket until SlopSync grows hub-side STREAM pacing — see 3.4).
- JS work — per working agreement, I own this end-to-end and only surface
  state-sync decisions.

### 2.2 Simulator — **SlopSim** *(name candidate — approve/veto)*
Desktop build of the machine: slopsync-core's in-process binding + a motion
model (mass/velocity/limits) + the real Hub + delegate compiled for host.
Answers the old question "is the firmware sim exactly the machine or not
even remotely" — with slopsync-core it can be *literally the same code*.
- Unlocks: UI development without hardware, protocol fuzzing, deterministic
  replay of field incidents (the fault-injection binding already exists).
- **Effort:** medium; most substrate already exists (that was the plan all
  along).

### 2.3 Board capability traits — the "feature table" for the ecosystem
Constexpr board-trait headers (pins, peripherals, features) so Reference PCB
(WROOM-32D), our S3, and the v2 C6 build from ONE codebase with per-board
`#if`-free module wiring. This is the OSSM-ecosystem compatibility play.
- Builds directly on what config_api.h already does, formalized per-board.
- Feeds `/api/capabilities` + the SlopSync catalog automatically — a board
  advertises what it truly has.

### 2.4 C5 node integration
The two ESP32-C5 nodes (relay + T-Dongle display) join the family:
- Vendored SlopLog (they have zero logging story) + SlopGlow (they have LEDs).
- **SlopSync-over-ESP-NOW transport** — second real ITransport binding
  (ACKMASK/BEACON frames + the AckLossRate congestion signal already exist in
  the wire layer for exactly this).
- The display node becomes a SlopSync *client* rendering machine state — the
  first non-browser ecosystem device. Good dogfood for the protocol.

---

## 3. Refactor backlog (smaller, ordered by value)

1. **Pairing enforcement flip** — `SlopDriveHubDelegate::validateToken` is
   controller-for-all (LAN-trust). Flip to token-gated once a pairing UX
   exists (PIN display in WebUI + pairing card). Deliberate decision, not a
   default.
2. **SeqRing<T,N> promotion** — telemetry ring (WebUI) + anomaly ring
   (SystemState) are the same idiom; unify. (Idempotency/applog rings are
   different shapes — leave them.)
3. **RAII CritSection guard** — replace raw portENTER/EXIT pairs (~30 sites);
   mechanical, prevents the forgotten-exit class of bug.
4. **LE byte writers for UiSocket** — ~40 hand-packed frame lines behind
   `putU16LE`-style helpers (slopsync's byte_io is the in-house prior art).
   Do together with 2.1 Phase C to avoid double-touching frames.
5. **Hub-side STREAM pacing** (slopsync-core, additive) — unlocks true
   timestamped telemetry bundles over SlopSync; prerequisite for retiring
   UiSocket's 0x01 telemetry frames.
6. **Deferred deletions** — SystemState dormant `buf[]` ring +
   `gen_rate_tick_hz` (needs a config-migration story), legacy
   `esp32-s3-devkitc-1` env (dies at merge). `ServoModbus::sendSetpoint`
   stays (deliberately-retained future API).
7. **Compile-floor release profile** — `SLOPLOG_COMPILE_LEVEL=2` (Info+) for
   release builds once debugging calms down; Debug stays for dev.

---

## 4. Endgame sequencing (proposal — argue with me)

```
now ──► slopsync-js Phase A/B          (UI on the protocol, dual-plane)
    ──► pairing enforcement + UX       (before anyone else's device connects)
    ──► merge feat/cpp20-slopsync → main   (branch has earned it)
    ──► SlopSim + board traits         (parallel tracks, both unblock ecosystem)
    ──► C5 nodes (ESP-NOW transport)   (second transport proves §13 for real)
    ──► Ruckig planning pass           (motion quality, careful bench cycle)
    ──► SML homing FSM                 (opportunistic, low risk)
```

Golden-vector `.bin` generation + the standalone conformance CLI slot in
whenever a third-party implementer materializes (the OSSM/R&D folks) — the
manifest + `catalog_check.hpp` engine already exist.

---

*Naming candidates on the table: **SlopSim** (simulator). Everything else
either has a name or is an internal class that keeps a plain one.*
