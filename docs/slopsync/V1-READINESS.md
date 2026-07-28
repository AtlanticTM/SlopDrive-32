# SlopSync PUBLIC v1.0 — Readiness Ledger

> Channel ids herein are historical (pre-C4); current map:
> [CHANNEL-MAP.md](CHANNEL-MAP.md).

*The operator's yardstick (ruling 2026-07-25): SlopSync is ready when the
WebUI is fully migratable — proven by COVERAGE against the RFC queue, not by
migrating the UI now. This ledger maps every communication surface the
current WebUI (and bench tooling) uses to its v1.0 home. A surface with no
row here is a bug in this ledger.*

**Status:** the RFC-001..029 base pass this ledger tracks landed 2026-07-26
(see [RFC-QUEUE.md](RFC-QUEUE.md#v10-batch-disposition-2026-07-26)'s
disposition table). Read this file as the coverage proof that made landing
safe to call, not as an open tracking board.

**Dispositions:**
- **LIVE** — an existing catalog channel covers it today.
- **RFC-nnn** — covered once that queue entry lands in v1.0.
- **AUTHOR** — needs no spec change at all; just author a new device
  channel (the 0x2101 pattern) + catalog entry.
- **DEAD** — carted away by operator ruling; not migrated, deleted.
- **OTA** — permanent HTTP escapee #1 (own token plane, rides next to the
  PsychicHttp work).
- **UITOKEN** — permanent HTTP escapee #2 ([RFC-029](RFC-QUEUE.md#rfc-029--trust-lifecycle-hub-authenticity-change-tripwires-own-ui-trust) §4; ruled 2026-07-25).
  A sideband convenience for WebUI-hosting devices only: optional to
  implement, never a prerequisite for any client, and its security
  property (browser same-origin) cannot exist in-band.

---

## 1. Coverage matrix

| Surface (today) | Disposition | v1.0 home |
|---|---|---|
| `GET /` static bundle | — | HTTP forever (asset serving is not an API) |
| `POST /api/ota`, `/api/ota/fs`, ArduinoOTA :3232 | **OTA** | Operator ruling: firmware dev's problem; escapes AND should escape (rights never derivable from SlopSync roles) |
| `GET /uitoken` (new, [RFC-029](RFC-QUEUE.md#rfc-029--trust-lifecycle-hub-authenticity-change-tripwires-own-ui-trust) §4) | **UITOKEN** | Escapee #2 by ruling: same-origin-policy IS the security property, so it cannot move in-band. Optional per-device sideband — a hub with no WebUI simply omits it |
| `/api/status` — homed/homing/paused/estop/override/position | **LIVE** | 0x1100 motion flags + 0x0003 safety |
| `/api/status` — speeds, distance, strokes | **LIVE** | 0x1100 + 0x1002 odometer |
| `/api/status` — wifi bssid/ip/channel/reconnects/tx_drops | **[RFC-026](RFC-QUEUE.md#rfc-026--strings-on-the-wire-operator-ordered) + AUTHOR** | link-status STATE channel (needs `str<N>`); kills the WebUI's last 30 s HTTP poll. The transport-chip / Intiface-serial-BLE link state this row named at write time no longer exists to migrate — the 2026-07-27 transport-switch deletion removed those transports outright, it did not move them to SlopSync. Only the wifi fields remain live scope here |
| `/api/status` — plan_derived/clamped/feasible/late, intent counters | **AUTHOR** | arbiter-diagnostics STATE channel |
| `/api/capabilities` — fw_version, product, features | **[RFC-016](RFC-QUEUE.md#rfc-016--in-band-hub-identity-capabilities--catalog-introspection)** | identity in WELCOME; capabilities = catalog introspection |
| `/api/capabilities` — ceilings (speed/accel/jerk) | **LANDED (M5a)** | 0x1000 fields + `limit.*` role tags + min/max/default annotations |
| `/api/settings` GET/POST — window, user/input speed/accel/jerk | **LANDED (M5a)** | 0x1000 ↔ 0x3000, fully [RFC-009](RFC-QUEUE.md#rfc-009--settings-metamodel-per-field-catalog-annotations-for-generic-self-building-uis)-annotated (setting_key/default/min/max/step/group/desc/role) + `enabled_mask` |
| `/api/settings` — blend_mode, expert_mode, default_range, stream_speed_mode, overshoot_clamp, auto_duration | **[RFC-009](RFC-QUEUE.md#rfc-009--settings-metamodel-per-field-catalog-annotations-for-generic-self-building-uis)** | settings metamodel (typed, categorized) |
| `/api/settings {reset_stats}`, `{reset_peaks}` | **[RFC-019](RFC-QUEUE.md#rfc-019--action-intents--observable-resets)** | action intents + observable `reset_gen` |
| `/api/move` | **LIVE** | 0x3100 |
| `/api/home` | **LIVE** | 0x3101 op 1 |
| `/api/stop` (hard E-STOP latch) | **[RFC-010](RFC-QUEUE.md#rfc-010--client-assertable-e-stop-over-slopsync)** | `safety_ops: estop`; **gates :81 deletion** |
| `/api/pause`, `/api/halt` | **LIVE** | 0x0005 ops 4/5, 2 |
| `/api/override` (manual_override, bypass_limits) | **[RFC-025](RFC-QUEUE.md#rfc-025--safety-semantics-completion-incl-overridebypass-ruling)** | safety domain per ruling: 0x0003 appended byte + 0x0005 ops |
| `/api/servo` GET (26-reg mirror, bus health, encoder validation) | **AUTHOR** | register-mirror STATE (52 B fits) + servo-diag channel |
| `/api/servo` POST (program sequence, raw write, save) | **[RFC-020](RFC-QUEUE.md#rfc-020--procedures-long-running-guarded-operations--reboot-commit)** | procedure pattern: action intent + progress STATE + verify-by-readback |
| `/api/clearfault` | **DEAD** | stub returning `no_fault_readback`; its real effect (move home) already LIVE |
| `/api/pattern` — 6 classic fields, run/stop | **LANDED (M5a)** | 0x1200 ↔ 0x3200, annotated; `pattern` is a NAMED u8 select (7 core PatternEngine names) + a genuinely dynamic `enabled_mask` (drops on e-stop / not-homed). JS migration still pending |
| `/api/pattern` — `ap_*` baseline + 6 modifier blocks | **[RFC-009](RFC-QUEUE.md#rfc-009--settings-metamodel-per-field-catalog-annotations-for-generic-self-building-uis) + AUTHOR** | flattened packed layout (50 f32 + 7 mask = 207 B ✓) + keyed intent writes; `ap_reset` via [RFC-019](RFC-QUEUE.md#rfc-019--action-intents--observable-resets) (bumps cfg_gen AND reset_gen). **Feasibility pass:** needs PER-ENTRY field capacity, not a uniform `Catalog<48,50>` (= 320 KiB); and its fully-annotated catalog entry must respect the new `catalog_max_entry_bytes` 4096 cap — trim descs or split |
| `/api/pattern {rate_tick}` | **[RFC-009](RFC-QUEUE.md#rfc-009--settings-metamodel-per-field-catalog-annotations-for-generic-self-building-uis)** | ordinary tuning setting |
| `/api/pattern/presets` GET/POST | **[RFC-021](RFC-QUEUE.md#rfc-021--slopsync-presets-operator-ordered)** | preset stores (kind `pattern.frayd`) |
| `/api/log` | **[RFC-017](RFC-QUEUE.md#rfc-017--device-log-channel)** | spec-core log EVENT channel + ring-tail backfill; serial-quiet re-binds to first grant |
| `/api/mode` GET/POST | **DEAD** | operator ruling: transport-mode switching is obsolete — SlopSync is the replacement |
| `/api/clients` GET/POST | **[RFC-018](RFC-QUEUE.md#rfc-018--session-roster--admin-eviction)** | 0x0002 roster + session-admin evict |
| `/api/slopmotion` — 14 tuning knobs | **[RFC-009](RFC-QUEUE.md#rfc-009--settings-metamodel-per-field-catalog-annotations-for-generic-self-building-uis) + AUTHOR** | textbook typed settings (`flags: advanced`), device channel pair |
| `/api/slopmotion` — stats/anomaly counters, plan bench | **LANDED (M5a)** | 0x1102 slopmotion-diag STATE (84 B: plans/failures/9 per-kind counters/plan-time bench/5 ingress counters — the 9th, `anom_handoff_bounded`, arrived with M4d) + [RFC-019](RFC-QUEUE.md#rfc-019--action-intents--observable-resets) `reset_gen`, fed by the existing HTTP reset. The reset ACTION INTENT still wants [RFC-019](RFC-QUEUE.md#rfc-019--action-intents--observable-resets) proper |
| `/api/machine` GET | **AUTHOR** | backend/bus-health STATE channel |
| `/api/machine/commit` (backend switch + deferred reboot) | **[RFC-009](RFC-QUEUE.md#rfc-009--settings-metamodel-per-field-catalog-annotations-for-generic-self-building-uis) + [RFC-020](RFC-QUEUE.md#rfc-020--procedures-long-running-guarded-operations--reboot-commit)** | `restart_required` setting + reboot-commit handshake (`REBOOTING` GOODBYE) |
| `/api/machine/homeoverride` (bench fake-home) | **[RFC-025](RFC-QUEUE.md#rfc-025--safety-semantics-completion-incl-overridebypass-ruling)** | home 0x3101 ops 2/3 (safety-reviewed — op 2 clears an e-stop latch) |
| :81 0x00 HELLO / 0x03 CLOCK | **LIVE** | HELLO/WELCOME, CLOCK |
| :81 0x01 TELE pos/tgt | **LIVE** | 0x1100 |
| :81 0x01 TELE `raw` ("asked" line) + per-sample `i_bus_mA` | **LANDED (M5a)** | `raw_10um` APPENDED to 0x1100 (7 → 9 B): asked / planned / achieved in ONE frame, one seq, one timestamp. `i_bus_mA` went to 0x1001 instead — bus current is background diagnostics and did not belong on the 60 Hz channel |
| :81 0x02 STATUS bus_mV/die_c10/peak_mA | **LANDED (M5a)** | 0x1001 power STATE, ≤10 Hz, background. FEATURE-GATED on `hasCurrentSensor()`/`hasPowerMonitor()`: a machine without the hardware does not advertise the channel, and that absence IS the capability answer ([RFC-016](RFC-QUEUE.md#rfc-016--in-band-hub-identity-capabilities--catalog-introspection)) |
| :81 0x02 STATUS link fields | **[RFC-026](RFC-QUEUE.md#rfc-026--strings-on-the-wire-operator-ordered) + AUTHOR** | same link-status channel as above |
| :81 0x04 INTERP plan-strip (~45 Hz) | **LANDED (M5a)** | 0x1101 plan-strip STATE, 45 Hz, elevated, 18 B — flags/style/start/end/cur/vel/duration/elapsed, straight off slopmotion::Snapshot |
| :81 0x05 ANOMALY | **LANDED (M5a)** | 0x4100 motion-anomaly EVENT — the FIRST device-authored EVENT channel, and therefore the proof that the M3b `body` (40) sub-map grammar works: it named its own fields with no registry change. Core 1 hands edges to Core 0 through an SPSC ring; the dead legacy feed is replaced, not revived |
| :81 0x06 STATS energy_mwh + session_ms | **LANDED (M5a)** | `energy_wh` (f32 Wh, not the legacy fixed-point mWh — the wire is self-describing) + `session_ms` appended to 0x1002 (12 → 20 B) |
| :81 0x10/0x11 CMD/ECHO (20 ops) | **LIVE / [RFC-009](RFC-QUEUE.md#rfc-009--settings-metamodel-per-field-catalog-annotations-for-generic-self-building-uis) / [RFC-010](RFC-QUEUE.md#rfc-010--client-assertable-e-stop-over-slopsync) / [RFC-025](RFC-QUEUE.md#rfc-025--safety-semantics-completion-incl-overridebypass-ruling)** | per-op mapping follows the rows above |
| TCode WS :55555 / WSDM / BLE / dongle machine transports | — | machine-control transports, not WebUI surfaces; TCode passthrough remains [RFC-008](RFC-QUEUE.md#rfc-008--doctrine-the-machine-owns-motion-processing-not-the-client)'s named third mode |

## 2. The dead cart (operator: "bring out your dead")

Ruled dead or found dead; deleted during the WebUI refactor, never migrated:

1. **`/api/mode` + the transport segmented control** — obsolete by ruling;
   SlopSync replaces the old OSSM-tool control path.
2. **`/api/clearfault`** — firmware stub (`cleared:false` always); UI button
   keeps only its move-home side effect, which is already 0x3100/0x3101.
3. **`settings.js` `/api/interp` callers** — no firmware route; the panel's
   DOM elements don't even exist in index.html. Dead code walking.
4. **`#genTickSeg` → `/api/gen`** — SHIPPING BROKEN CONTROL: the element
   exists, clicks POST to a 404 (real path was `/api/pattern {rate_tick}`).
   Ground-truth doctrine violation, pre-existing.
5. **`OP_HOME_OVERRIDE` import in main.js** — imported, never used.
6. **Anomaly panel legacy wiring** — the ring it renders is written only by
   the superseded MotionInterpolator; SlopMotion's anomalies go to SlopLog.
   The panel shows dead gauges TODAY (doctrine violation). Rebuilt against
   the AUTHOR'd anomaly EVENT channel, not revived.
7. **`FALLBACK_LAYOUTS` in catalog.js** — deleted the day [RFC-015](RFC-QUEUE.md#rfc-015--syncing-order-catalog-completes-before-retained-state) lands.
8. **`session.js` `INTENT_SCHEMAS` hand-copy** — redundant with the catalog
   the JS already decodes, and stale (missing config_set key 7 input_jerk —
   live float-as-int encode bug). Delete; decode from catalog.
9. **The legacy :81 UiSocket plane entire** — protocol, senderTask, frame
   builders — once [RFC-010](RFC-QUEUE.md#rfc-010--client-assertable-e-stop-over-slopsync) lands (its last load-bearing duty is the e-stop).
10. **`IdleGuardWebServer`** — dies with the PsychicHttp migration, NOT with
    SlopSync (the speculative-socket stall is an HTTP-server problem; the
    SlopSync cutover removes the API surface, not the server).

## 2.5 Client work items (operator-specified, 2026-07-25)

Feature requirements for the M5b external-client pass. Recorded here because
two of them have DEPENDENCIES on the M5 device-authoring pile — those
channels now have a named consumer and should be prioritized accordingly.

### MFP plugin (`clients/mfp-slopsync/`)
- **Condense the UI.** SlopSync.xaml is doing too much; tighten to what an
  operator actually touches mid-session.
- **Add a Home control** (INTENT 0x3101 op 1 — already live).
- **Add a mini stroke-window control** (compact min/max editor writing
  config-set 0x3000 keys 1/2, adopting from 0x1000 per ground-truth).
- **Read back input speed / accel / jerk — FOR DISPLAY.** Locate the
  fields by their **`field_roles`** (`limit.input.speed|accel|jerk`,
  `window.min|max`) rather than hardcoding channel 0x1000, so the same
  code works against any conforming hub. First real proof of the role
  vocabulary earning its keep.
  **DOCTRINE CLAMP (operator ruling, 2026-07-25) — read before
  implementing:** this readback creates NO obligation on the plugin to
  map, clamp, scale, or otherwise respect those limits. MFP plays a
  funscript; it cannot make authored content more machine-compatible, and
  it must not try. The plugin's job is to ship the sender's INTENT as
  authored ([RFC-008.5](RFC-QUEUE.md#rfc-008--doctrine-the-machine-owns-motion-processing-not-the-client)); **the MACHINE's job is to play back whatever it
  is fed as well as it possibly can.** Limits are shown to the operator,
  nothing more. Per [RFC-006](RFC-QUEUE.md#rfc-006--motion-producing-clients-have-no-portable-way-to-learn-the-machines-kinematic-limits) the normative word is MAY, never SHOULD.
  The one sanctioned exception is where adaptation is FREE and the client
  owns the source anyway: for MFP's *generated* (non-scripted) axes,
  choosing a friendlier generation rate or parameter set is a legitimate
  client-side quality-of-implementation choice — because it is shaping
  its own generator, not second-guessing the machine.
  **Consequence:** the plugin's v0.2.3 Fritsch–Carlson handoff limiter is
  slated for DELETION once the hub-side handoff guard lands (see below).
  Do not extend it, do not add sibling limiters.

### Hub-side handoff sanity guard ([RFC-008](RFC-QUEUE.md#rfc-008--doctrine-the-machine-owns-motion-processing-not-the-client)'s "concrete first consequence")
**LANDED — milestone M4d (fw 2.1.53 / slopmotion 0.7.0).** "Plan for the
worst" is machine-side doctrine, so the guard lives in the motion engine,
not in any client.
- **The bound:** `slopmotion::boundHandoffVelocity(end_vel, chord_in,
  chord_out, k)` → `|end_vel| ≤ k·min(|chord_in|, |chord_out|)`, k = 1.5
  (the Fritsch–Carlson shape-preserving value). Sign preserved; in-bounds
  input returned bit-for-bit unchanged.
- **The lookahead:** `PacingRing::peekOldest()` →
  `Command::next_chord`/`has_next_chord`, filled by
  `SlopSyncHubService::drainMotionStream` at the last moment before the
  command crosses to Core 1 (the Core-1 queue holds 0–1 commands; the
  schedule-ahead knowledge lives in the RING and nowhere else).
  `chord_in` is computed IN the engine as `|target − p| / T` — the machine's
  real position, better ground truth than the sender's script geometry.
- **Observability:** `AnomalyType::HandoffBounded` (kind 8) → SlopLog
  `motion` tag through the existing Core-1 drain, `anom_handoff_bounded` on
  0x1102 slopmotion-diag (80 → 84 B) and in `GET /api/slopmotion`
  `anomalies_by_kind`, and an EVENT on 0x4100 motion-anomaly with the label
  `handoff_bounded` — so a client can see its content being reshaped, which
  is the whole point.
- **The knob:** `POST /api/slopmotion {"handoff_k": k}`, applied value
  echoed, clamped [0, 8]; **0 disables the guard**, which is the machine
  half of the M5d A/B against the plugin's own limiter.
- **The tail case, decided:** no successor in the ring → NO bound, accept as
  sent. Guessing a chord we do not have would trim well-behaved senders (a
  feel regression for every good client), and the segment is already DUE so
  deferring it would trade a shape problem for a deadline problem. The
  quintic legality scan + Ruckig guard remain the backstop.
- **Coverage caveat:** the guard can only fire when the successor is already
  in the ring, i.e. when the current segment is shorter than the client's
  scheduling lookahead (MFP ships 120 ms; the hub clamps a wire `t_off`
  beyond 250 ms). Structurally this correlates with the pathology — an
  oversized Akima tangent implies a steep current chord implies a short
  segment — but raising the client's lookahead is the direct widener.
- **The plugin limiter is NOT deleted** (that is M5d, and it needs a live
  A/B first). `SegHandoffLimiterEnabled` is now safe to flip to false for
  the comparison.

### Diagnostic CLI — motion-input vs planner graphing
Requirement: **graph the raw commanded input arriving over SlopSync,
scaled into the stroke window, against what the motion planner actually
did** — so planner behavior (quintic shaping, Ruckig guard fallbacks,
chase lag) is visually comparable to the sender's intent.

Needs three things, two of which are AUTHOR-pile items — **prioritize
these in M5**:
1. **The "asked" / raw demand value** — the pre-planning target, i.e. the
   legacy `:81 0x01 TELE raw` field. Already listed in §1 as AUTHOR
   (extend 0x1100 append-only, or a dedicated channel). Without it there
   is nothing to plot the planner against, and §1 already flags it as
   "the most likely thing to be silently lost" in the migration.
2. **The plan-strip feed** — the planner's current segment
   (`startPos/endPos/curPos/curVel/durationUs/elapsedUs`), i.e. the
   legacy `:81 0x04 INTERP` frame. AUTHOR-pile, ~45-60 Hz, elevated
   priority; §1 notes it was tracked NOWHERE before this ledger.
3. **Window bounds** from 0x1000 (`window.min|max` roles) to convert
   normalized 0..1 input into mm for a like-for-like plot.
Note a subtlety: inbound motion (0x2100/0x2101) is c2h, so an observer
CLI cannot see another client's stream directly — it observes the hub's
republished truth via (1) and (2). That is the correct design (ground
truth: you see what the machine actually did), not a limitation to work
around.

## 3. Scorecard

Every surface above lands in exactly one of LIVE / RFC / AUTHOR / DEAD /
OTA — **zero uncovered surfaces** once [RFC-001](RFC-QUEUE.md#rfc-001--nack-cannot-be-correlated-to-a-specific-in-flight-intent)…029 land. The AUTHOR pile
(power/thermal, link-status, plan-strip, anomaly events, arbiter-diag,
servo mirror, slopmotion-diag — ~7 new device channels) requires no spec
changes at all: it is catalog authoring on machinery that already exists,
gated on the catalog-capacity restructure ([RFC-009](RFC-QUEUE.md#rfc-009--settings-metamodel-per-field-catalog-annotations-for-generic-self-building-uis) action item, sized by
the 2026-07-25 feasibility pass) and — for the anomaly channel — on the
EVENT `body` sub-map grammar fix from that same pass, without which a
device-authored EVENT channel would need a registry PR for its own field
keys.

**M5a device-authoring pass (2026-07-26) — CLOSED 8 of the AUTHOR pile's
rows.** Four new device channels (0x1101 plan-strip, 0x1001 power, 0x1102
slopmotion-diag, 0x4100 motion-anomaly), four append-only layout extensions
(0x1100 `raw_10um`, 0x1000 `enabled_mask`, 0x1200 `enabled_mask`, 0x1002
`energy_wh`+`session_ms`), and the whole settings surface annotated per
[RFC-009](RFC-QUEUE.md#rfc-009--settings-metamodel-per-field-catalog-annotations-for-generic-self-building-uis). Device catalog 21 → 25 entries; its etag moved, as expected.
STILL AUTHOR-PILE, deliberately deferred to a later pass: **link/transport
status** (needs `str16` authoring for BSSID/IP and is entangled with WiFi
state), the **servo 26-register mirror** (large, and its programming sequence
wants [RFC-020](RFC-QUEUE.md#rfc-020--procedures-long-running-guarded-operations--reboot-commit)'s procedure pattern first), and **arbiter diagnostics**.

**One library change came out of it, and it was a real bug:** the Hub's
`kCatalogScratchBytes` was 8192, and an annotated catalog encodes to ~10.3 KB.
`encodeCatalog()` returns 0 when it does not fit, so the hub hashed ZERO BYTES
into its etag and served an EMPTY catalog while answering HELLO cheerfully —
silent and total. Raised to 16384 ([RFC-009](RFC-QUEUE.md#rfc-009--settings-metamodel-per-field-catalog-annotations-for-generic-self-building-uis)'s own sizing note projects 15–25 KB
for a lavish catalog), plus an additive `Hub::catalogEncodedBytes()` so a host
can notice instead of shipping it. Firmware, sim and the native suite all check
it now.

**Feasibility-pass verdict (2026-07-25):** two bounded audits (protocol
consistency: 38 findings / 10 blockers; on-target: crypto, RAM, NVS,
toolchain) produced ZERO architectural rejections — every blocker resolved
by amendment, all bound into the queue's own feasibility-pass section.
Its one open question (`/uitoken`) was ruled the same day: sanctioned as
escapee #2. **No open questions remain; the base pass is ready to land on
green light.**

Post-base-pass expectation (operator): RFCs become rare, small, flip-a-flag
affairs. If a future proposal requires rethinking core structure, the base
pass failed — treat that as the alarm it is.
