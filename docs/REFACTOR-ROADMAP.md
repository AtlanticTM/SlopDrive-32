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
| **SlopMotion** (Ruckig motion core, §1) | PART 1 — lib/slopmotion + vendored Ruckig v0.19.4, 11 native suites green, trace bench + graphs; firmware wiring is part 2 |
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

**Re-evaluation shortlist (recorded 2026-07-23, so we never re-shop this):**
- **PsychicHttp (esp_http_server) is the candidate** for the static+OTA end
  state: handlers run in its OWN task (blocking stays contained — NOT the
  async_tcp/LwIP context), IDF-native/Espressif-maintained underneath,
  built-in LRU socket purge + per-socket timeouts (our zombie-client
  defenses, first-class), single-port URI-routed WS possible. Its own
  benchmark: no crashes under load, best-in-class file serving.
- **ESPAsyncWebServer: DISQUALIFIED** — architecturally (all handlers move
  into the LwIP context) AND empirically (PsychicHttp's published loadtests:
  crashes under heavy load in every 60 s test; ~2 yrs abandoned upstream).
- **WS caveat — bench gate before trusting PsychicHttp's websockets:** its
  own numbers show 38 rps/connection round-trip (~26 ms/msg). Our pattern is
  push-heavy small-binary (SlopSync STREAM ≤50 bundles/s in + STATE ≥20 Hz
  out per client; TCode 100–333 Hz until absorbed) — if that overhead is
  per-message, it's disqualifying for the sync socket; if it's their
  echo-path plumbing, fine. MUST bench our exact pattern first. Escape
  hatch either way: ITransport keeps the WS layer swappable — PsychicHttp
  for HTTP/static/OTA + links2004 (proven at 333 Hz today) or raw
  esp_http_server WS for the sync socket. The halves need not migrate
  together (costs the single-port prize, nothing else).

## 5. slopsync-js — parked until motion + library refactor land

Phasing when it wakes: A read-only STATE cards (dual-plane with UiSocket) →
B intents (echo-confirmed lifecycle) → hub-side STREAM pacing → C retire
overlapping UiSocket frames.

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
NOW  ──► Ruckig motion core (v3 cyclic tracker + v4 one-shot planner)
     ──► SlopSync inbound motion: STREAM c2h handler + TCode pass-through channel
     ──► MFP plugin (C# SlopSync client) — first external implementation,
         rock-solid link is the milestone gate
     ──► merge feat/cpp20-slopsync → main + pairing rough-in (model C)
     ──► widen: slopsync-js A/B, SlopSim v1 ∥ board traits
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
