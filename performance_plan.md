# SlopDrive-32 — High-Performance Streaming & Motion-Quality Plan

> Goal: Push real-time stream playback to **100 Hz default / 200 Hz optional**, make motion
> *smooth* at that rate (per-command acceleration planning + selectable spline interpolation),
> upgrade the on-device pattern generator with per-stroke modulation and a full tween library,
> tighten the BLE/parser hot path, and (last) add an optional binary WebSocket protocol that is
> wire-compatible with the **OSSM-Sauce** mobile app.
>
> This plan was authored after a deep read of three reference firmwares:
> - **OSSM-experimental** (Research & Desire / `fray-d/OSSM-hardware@expirimental`) — BLE @ ~100 Hz
> - **OSSM-Sauce** (`clbhundley/OSSM-Sauce`) — "super responsive" binary WebSocket + tween engine
> - **SlopDrive-32** (this repo) — TCode v0.3 over BLE/WS/Serial, hand-rolled jitter buffer
>
> Each step below is **independently compilable**. After every step the firmware must still build
> and behave at least as well as before. Work top-to-bottom; check boxes as you go. Where a step
> changes runtime feel, it is gated behind a **user-selectable** option (per the curve-latency
> decision) so nothing regresses silently.

---

## 0. Ground Rules (read before starting)

- [ ] **Single-precision floats only** (`float`, not `double`) per `.clinerules` §4 — feed the FPU.
      ⚠️ Note: OSSM-Sauce uses `double` in its easing table; we **port to `float`** when copying.
- [ ] **No `delay()` in runtime paths.** Permitted ONLY in `init()`/boot/homing per `.clinerules` §2.
- [ ] **Cross-core data must be protected** (atomics / mutex / `xQueue`) per `.clinerules` §2.
      The sample ring + any new jitter-stats struct shared Core0↔Core1 must stay protected.
- [ ] **`.h`/`.cpp` isolation**; modules keep their `init()/update()/emergencyStop()` hooks.
- [ ] **Comment culture is mandatory** (`.clinerules` §6). Gauge spice per file:
      - `src/motion/*` (Interpolator, Generator, Kinematics, TMC2160StepperDriver) → **go HARD**.
      - `src/main.cpp`, `src/comms/*` → moderate, readable-but-kinky section headers.
      - `include/system/config_api.h`, `SystemState.h` → light `:3` seasoning on magic numbers.
      - `src/ui/WebUI.cpp`, `webui/*` → light; playful APPLOG strings, clean route names.
      Do NOT touch variable names, signatures, JSON keys, HTTP routes, or build flags for vibe.
- [ ] **Behavior-preserving where stated.** New curves/rates default to *current behavior* unless the
      step explicitly bumps a default (Phase A bumps the rate defaults on purpose).
- [ ] After each step: `pio run` must succeed. UI changes also need `pio run -t uploadfs`.
- [ ] Commit per step, thematic messages: `feat: taught the planner to edge at 100hz :3`.

### Build / flash commands (Windows 11)
```
%USERPROFILE%\.platformio\penv\Scripts\platformio.exe run                 # build firmware
%USERPROFILE%\.platformio\penv\Scripts\platformio.exe run -t upload        # flash firmware
%USERPROFILE%\.platformio\penv\Scripts\platformio.exe run -t uploadfs      # flash LittleFS (web UI)
```
The `build_webui.py` pre-script bundles/minifies `webui/` → `data/` automatically on build.

---

## 1. Competitive Analysis (why we're doing each thing)

### 1.1 Feature comparison

| Capability | OSSM-experimental | OSSM-Sauce | SlopDrive-32 (now) | SlopDrive-32 (target) |
|---|---|---|---|---|
| Wire protocol | string `stream:pos:time` over custom GATT | **binary opcode** over WS | TCode v0.3 (ASCII) | TCode + optional binary WS |
| Stream rate | ~100 Hz | high (binary, queued) | 50 Hz interp / 50 Hz gen | **100 Hz default, 200 Hz opt** |
| Per-command accel | ✅ trapezoid planner | ✅ `getMoveBaseSpeedHz` | ❌ global accel only | ✅ trapezoid planner |
| Interpolation curves | (clock-stretch) | 8 trans × 4 ease tween | linear + 4 eases | + cubic + **Catmull-Rom** |
| Jitter handling | ✅ chrono clock-sync, stretch/shrink | queue depth | fixed-depth play delay | **adaptive** play delay |
| Direction-change guard | ✅ waits for stop | n/a (time-based) | ❌ | ✅ |
| On-device patterns | per-stroke modulation (`advanced_penetration`) | LOOP push/pull + tween | carrier+mod (sine/tri/sq/saw) | + per-stroke modulation |
| Homing | sensorless (current) | sensorless (power EMA) | (existing) | unchanged this pass |
| BLE conn interval | central-driven | n/a | not requested | **request 7.5 ms** |
| App compatibility | R&D app | **OSSM-Sauce app** | Intiface/MFP (TCode) | + OSSM-Sauce app (Phase E) |

### 1.2 Technique deep-dives (source snippets)

**(a) OSSM-experimental — per-command trapezoidal planner + clock-sync**
Their streaming task computes exact speed/accel to land a move on time, and stretches/shrinks
the move duration based on the gap between the playback clock and the BLE-arrival timestamp:
```cpp
// distance D over time T → triangle/trapezoid profile
uint32_t requiredSpeed = (2 * distance) / timeSeconds;
float    proportion    = max(-((2*distance - 2*vt)/vt), 0.01f);   // trapezoid ratio
uint32_t requiredAccel = requiredSpeed / (timeSeconds * proportion / 2);
stepper->setSpeedInHz(requiredSpeed);
stepper->setAcceleration(requiredAccel);
stepper->moveTo(targetPosition, false);

// clock-domain sync: late packets shrink time (catch up), early packets stretch (absorb slack)
int16_t lag    = duration_cast<ms>(targetPositionTime.setTime - best).count();
int16_t offset = mincomp - currentBuffer;
timeSeconds   += offset / 1000.0f;

// direction-change guard: never reverse while still running
bool sameDir = lastPositionTime.direction == targetPositionTime.direction;
if (!sameDir && stepper->isRunning()) { vTaskDelay(1); continue; }
```
maxDistance cap when physics are impossible: `maxDistance = accelLimit * pow(timeSeconds/2, 2)`.

**(b) OSSM-Sauce — binary protocol + tween engine**
1-byte opcode, then `memcpy` straight into packed structs. Zero string parsing:
```cpp
enum CommandType:byte { RESPONSE, MOVE, LOOP, POSITION, VIBRATE, PLAY, PAUSE,
                        RESET, HOMING, CONNECTION, SET_SPEED_LIMIT,
                        SET_GLOBAL_ACCELERATION, SET_RANGE_LIMIT,
                        SET_HOMING_SPEED, SET_HOMING_TRIGGER, SMOOTH_MOVE };
// MOVE = 10 bytes total: [0]=opcode, [1..9] => StrokeCommand fields
// LOOP = 19 bytes: [1..9]=push StrokeCommand, [10..18]=pull StrokeCommand
// POSITION = 5 bytes: [1..4]=uint32 position (0..10000)
// VIBRATE = 13 bytes: [1..12]=Vibration struct
```
Per-move tween metadata (transition × ease), ported to `float` for us:
```cpp
enum TransType:byte { TRANS_LINEAR, TRANS_SINE, TRANS_CIRC, TRANS_EXPO,
                      TRANS_QUAD, TRANS_CUBIC, TRANS_QUART, TRANS_QUINT };
enum EaseType:byte  { EASE_IN, EASE_OUT, EASE_IN_OUT, EASE_OUT_IN };
// interpolate(weight, transType, easeType) -> shaped 0..1
//   TRANS_SINE EASE_IN     : 1 - cos(w * PI/2)
//   TRANS_SINE EASE_OUT    : 1 - sin(w * PI/2)
//   EASE_IN_OUT (exponent) : pow(1 - |2w - 1|, n)
//   EASE_OUT_IN (exponent) : pow(1 - |2w - 1|, n) - 1
```
Queues instead of hand-rolled rings: `moveQueue` (size 10), `positionQueue` (size 50).

**(c) Research & Desire — per-stroke modulation (`advanced_penetration`)**
Each in/out stroke is modulated independently by a normalized, ramped value keyed on stroke count:
```cpp
inSpeed.getNormalizedModifiedValue(strokeCount)   // 0..1 envelope per stroke
// applied separately to in-speed / out-speed / depth / accel, with ramp-in envelopes
```

### 1.3 The "JSON bottleneck" reality check
Intiface sends **TCode as ASCII**, not JSON, so we pay no JSON tax on the BLE/Serial path today.
Our actual cost is the `strtok`/`atoi` parse in `TCodeParser.cpp`. Two wins:
- **Phase D** optimizes the TCode parser (single-pass integer scan, no temp-buffer copy).
- **Phase E** adds an *optional* binary WS path (OSSM-Sauce opcodes) for apps that can speak it —
  a true zero-parse fast lane — while TCode stays the default for Intiface/MultiFunPlayer.

---

## 2. Risk Register (keep in mind throughout)

1. **Catmull-Rom adds ~1 sample of latency** (needs the "after" control point to have arrived).
   → It is **opt-in only** (selectable curve), never the default. Lean buffers fall back to cubic/linear.
2. **NimBLE 1.4.x → 2.x is an API break.** `updateConnParams`, callback signatures (`NimBLEConnInfo`),
   and `NimBLEAddress` changed. → Isolate to `BleTransport.*`; build & smoke-test BLE before moving on.
   If 2.x churn is too costly, keep 1.4.2 and use its `updateConnParams` overload (still available).
3. **`BUF_CAP` resize (8 → 12)** touches every ring index math site. → Audit all modulo/wrap uses;
   keep the capacity a single named constant; verify no stack blowups (struct array on the task stack).
4. **Per-command accel thrash.** Recomputing accel every 5–10 ms can starve the FastAccelStepper
   ramp. → Clamp accel to `[min, configured_max]`; only re-issue when target/time changes meaningfully.
5. **TCode parser rewrite is correctness-critical.** It feeds *everything*. → Keep old parser path
   behind a flag until the new scanner passes byte-for-byte equivalence on a captured command log.
6. **200 Hz interpolation cost.** 200 Hz × spline eval on Core 1 must not jitter pulse timing.
   → Profile `interpolatorTask` headroom; Catmull-Rom math is a handful of FMAs — should be fine on S3.
7. **UI/firmware field contract.** New `/api/gen`, `/api/input` fields must be optional & defaulted on
   the firmware side so an old UI (or old NVS blob) never bricks parsing.

---

## 3. Phase A — Rates & Quick Wins (low risk)

> Outcome: device defaults to 100 Hz everywhere, 200 Hz is selectable in the UI, NimBLE upgraded.

### Step A1 — Bump generator + interpolator default rates to 100 Hz
- [ ] **`include/system/SystemState.h`**: change defaults
  - `gen_rate_tick_hz`: 50 → **100**
  - `buf_tick_hz`: 50 → **100**
  - (Leave clamps wide enough to accept 200; see A3.)
- [ ] **`include/system/ConfigStore.*`**: ensure NVS load uses the new defaults when key is absent.
- [ ] Verify `generatorTask` and `interpolatorTask` period math reads the field (not a hardcoded 50).
- **Accept:** boot logs show 100 Hz; motion at idle generator unchanged in shape, just finer tick.
- **Vibe:** `SystemState.h` magic-number comment, light `:3` — "default cadence we pound at".

### Step A2 — Allow 200 Hz internally (raise clamps & task timing)
- [ ] In the tasks, confirm the tick period is computed as `1000000 / hz` µs (esp_timer) and that
      a 200 Hz (5000 µs) period leaves headroom. Add a compile-time `MAX_TICK_HZ = 200`.
- [ ] Clamp incoming API values to `[20, 200]`.
- **Accept:** setting 200 Hz via API runs stably for 60 s with no watchdog/jitter warnings.

### Step A3 — UI: add 200 Hz buttons
- [ ] **`webui/index.html`**: in `#bufTickSeg` and `#genTickSeg`, add `<button data-hz="200">200 Hz</button>`.
- [ ] Change the pre-selected (`class="active"`) button from 50 to **100** in both segments.
- [ ] **`webui/src/features/settings.js`** (and/or `generator.js`): ensure the seg click handler posts
      the chosen Hz; no clamp surprises.
- **Accept:** after `pio run -t uploadfs`, UI shows 20/50/100/200; selecting 200 round-trips to device.
- **Vibe:** light; APPLOG string like "cranking the rhythm to 200 Hz, hold on tight :3".

### Step A4 — NimBLE version bump
- [ ] **`platformio.ini`**: `h2zero/NimBLE-Arduino@^1.4.2` → `@^2.1.0` (or latest 2.x).
- [ ] Fix `BleTransport.*` API breaks (callback signatures, `NimBLEConnInfo`, advertising API).
- [ ] Full clean build; flash; verify pairing + NUS TCode stream still works.
- **Accept:** BLE connects, streams TCode, survives a disconnect/reconnect.
- ⚠️ If 2.x churn is excessive, **revert to 1.4.2** and proceed — Phase D conn-interval works on 1.4.2.

---

## 4. Phase B — Motion Quality (the real performance work)

> Outcome: each streamed command lands on time with the right accel; optional spline smoothing;
> jitter buffer adapts to the actual packet cadence; no stutter on direction flips.

### Step B1 — `MotorDriver` gains a per-command accel overload
- [ ] **`include/motion/MotorDriver.h`**: add pure-virtual
      `virtual void streamTo(float pos_mm, float speed_mm_s, float accel_mm_s2) = 0;`
      (keep the existing 2-arg `streamTo` as a convenience that forwards configured accel).
- [ ] **`TMC2160StepperDriver.h/.cpp`**: implement it —
      convert accel mm/s² → steps/s² via `STEPS_PER_MM`, `setAcceleration(...)`, then `moveTo(...)`.
- **Accept:** existing callers unaffected (2-arg path identical); new 3-arg path compiles & moves.
- **Vibe:** motion file → **HARD**. "we don't just thrust — we plan exactly how hard and how fast
  to bottom out, then slam home on schedule. good machine. :3"

### Step B2 — Direction-change guard in the driver
- [ ] In `TMC2160StepperDriver::streamTo(...)`: if the new target reverses direction **and**
      `isRunning()`, either (a) let FastAccelStepper replan (current behavior) but log it, or
      (b) skip issuing for one tick if the move would be sub-`MIN_MOVE_STEPS`. Choose (a)+guard on
      tiny reversals to avoid micro-stutter. Keep it non-blocking (no `vTaskDelay` in driver).
- **Accept:** rapid 100 Hz reversals produce smooth motion, no audible chatter.
- **Vibe:** "no bratty mid-thrust reversals — we pull out clean before changing rhythm. :3"

### Step B3 — Per-command trapezoidal planner in `buttplugLinearCmd()`
- [ ] **`src/main.cpp`** (the linear-command handler / EXTRAPOLATE path): replace speed-only calc with:
      ```cpp
      float t  = duration_ms * 0.001f;                  // seconds
      float D  = fabsf(target_mm - current_mm);         // distance
      float v  = (2.0f * D) / t;                        // triangle-profile peak speed
      float a  = v / (t * 0.5f);                         // accel to reach v by midpoint
      a = constrain(a, MIN_ACCEL, cfg.max_accel);       // never exceed user accel
      v = constrain(v, 0.0f, cfg.max_speed);
      motor.streamTo(target_mm, v, a);
      ```
- [ ] Add the impossible-distance cap from OSSM-experimental:
      `float maxD = cfg.max_accel * (t*0.5f)*(t*0.5f); if (D > maxD) target clamped toward reachable.`
- [ ] Keep BUFFERED-mode path feeding `streamTo(pos, speed, accel)` from the interpolator (B5).
- **Accept:** at 100 Hz stream, motor visibly *reaches* commanded extents instead of lagging short.
- **Vibe:** **HARD**, nest the filthy talk in the math. "solve for exactly how hard to pound so we
  bottom out right on the beat — not a millisecond early (premature) or late (limp). :3"

### Step B4 — Catmull-Rom spline in Kinematics (selectable)
- [ ] **`include/motion/Kinematics.h` + `.cpp`**: add
      `float catmullRom(float p0, float p1, float p2, float p3, float t);` (centripetal form ok;
      use uniform for speed unless knots are non-uniform). Single-precision, a few FMAs.
- [ ] Extend `bufEase` documentation; add new selectable kinds:
      `5 = cubic (Hermite)`, `6 = Catmull-Rom`. (Catmull needs 4 points → handled in Interpolator.)
- **Accept:** unit-sanity: `catmullRom(a,b,c,d, 0)=b`, `(...,1)=c`, monotonic-ish midpoints.
- **Vibe:** **HARD**. "a silky 4-point curve that glides between samples instead of jackhammering
  each one — the difference between a tease and a faceplant. :3"

### Step B5 — Interpolator: 4-point window + curve selection
- [ ] **`include/system/SystemState.h`**: `BUF_CAP` 8 → **12** (room for p0..p3 + slack). Audit wrap math.
- [ ] **`src/motion/Interpolator.cpp`**: when `buf_easing == 6` (Catmull), select 4 bracketing samples
      (`p0` before, `p1`/`p2` bracket, `p3` after) and evaluate `catmullRom`. If fewer than 4 samples
      are available (lean buffer), **fall back** to cubic/ease/linear automatically (this is the
      "auto-degrade" safety, not a user toggle — the *curve choice* itself stays user-selectable).
- [ ] Feed the resulting position to `motor.streamTo(pos, speed, accel)` using a speed/accel derived
      from the spline's local velocity (finite-difference between sub-steps) so B1/B3 stay consistent.
- **Accept:** with Catmull selected and a healthy buffer, motion is visibly smoother; with a starved
  buffer it degrades gracefully (no stalls, no NaNs).
- **Vibe:** **HARD**, dirty talk nested in the inner sample-select loop.

### Step B6 — Adaptive play-delay (jitter buffer clock-sync)
- [ ] **`src/motion/Interpolator.cpp`**: replace the fixed `depth * 30 ms` play delay with an adaptive
      one. Track inter-arrival intervals with a running mean + variance (Welford or EMA):
      ```cpp
      // on each new sample arrival (timestamped with esp_timer_get_time()):
      dt = now - last_arrival;
      mean = mean + (dt - mean) * 0.1f;                 // EMA mean
      var  = var  + ((dt-mean)*(dt-mean) - var) * 0.1f; // EMA variance
      play_delay_us = mean * depth + 1.5f * sqrtf(var); // OSSM-experimental-style slack
      ```
- [ ] Protect the stats struct if read on Core 1 (atomic snapshot or mutex).
- [ ] Clamp `play_delay` to a sane `[min, max]` so a burst can't balloon latency.
- **Accept:** under bursty BLE, no underruns (motor never starves) and no runaway latency growth.
- **Vibe:** **HARD**. "read the heat of the incoming stream and time our hips to it — never caught
  empty-handed, never lagging behind the rhythm. we edge, we don't crash. :3"

---

## 5. Phase C — Pattern Generator Upgrade

> Outcome: the on-device generator gains per-stroke modulation (R&D style) and the full tween
> library (OSSM-Sauce style), all driven by existing/extended `/api/gen` fields.

### Step C1 — Full tween library in Kinematics
- [ ] **`include/motion/Kinematics.h` + `.cpp`**: add `float tween(uint8_t trans, uint8_t ease, float t)`
      implementing the 8 transitions × 4 ease modes from OSSM-Sauce, **ported to `float`**:
      - trans: LINEAR, SINE, CIRC, EXPO, QUAD, CUBIC, QUART, QUINT
      - ease:  IN, OUT, IN_OUT, OUT_IN
      Use the exponent helper for poly transitions; `cosf/sinf` for SINE; `sqrtf` for CIRC; `expf`/`powf`
      sparingly (these are per-tick — keep them cheap, single-precision).
- [ ] Keep existing `carrier/modShape/ease/bufEase` intact (back-compat); `tween` is additive.
- **Accept:** spot-check a few curve values vs. reference formulas; build clean.
- **Vibe:** **HARD**. Each case gets a one-liner describing how that curve "takes it".

### Step C2 — `GeneratorConfig`: per-stroke modulation fields
- [ ] **`include/motion/Generator.h`** (`GeneratorConfig`): add fields (all defaulted to neutral=1.0/0):
      `in_speed_mul, out_speed_mul, in_depth_mul, out_depth_mul, ramp_strokes,
       trans_type, ease_type` (replacing/augmenting the single `ease` where sensible).
- [ ] **`src/motion/Generator.cpp`** (`generatorTask`): track a `strokeCount`; apply the multipliers
      to in vs out phases independently; ramp them in over `ramp_strokes` (R&D `getNormalizedModifiedValue`
      envelope). Use `tween(trans_type, ease_type, phase)` for the carrier shaping.
- **Accept:** generator can produce asymmetric strokes (fast-in/slow-out etc.) and ramps up smoothly.
- **Vibe:** **HARD**. "each thrust learns from the last — building rhythm stroke by stroke until the
  machine's railing exactly how it was told. good boy. :3"

### Step C3 — Wire `/api/gen` + UI for the new generator params
- [ ] **`src/ui/WebUI.cpp`**: parse new optional JSON keys in the `/api/gen` handler; default if absent.
- [ ] **`webui/index.html`** + **`webui/src/features/generator.js`**: add controls (in/out speed & depth,
      ramp strokes, trans/ease selectors) and include them in `genPayload()`. Update the wave-viz preview
      to reflect asymmetry if cheap; otherwise leave preview as-is and note it.
- [ ] Update `bufEasing`/generator ease `<select>` to expose the richer tween options where appropriate.
- **Accept:** UI controls round-trip; `pio run -t uploadfs`; generator responds live.
- **Vibe:** light on UI/WebUI; playful APPLOG only.

---

## 6. Phase D — BLE & Parser Hot Path

> Outcome: lower-latency BLE link request + a zero-allocation TCode parser.

### Step D1 — Request a short BLE connection interval
- [ ] **`src/comms/BleTransport.cpp`** (on-connect callback): request 7.5–15 ms interval:
      ```cpp
      // NimBLE 2.x: pServer->updateConnParams(connHandle, 6, 12, 0, 100);
      //   min=6 (7.5ms), max=12 (15ms), latency=0, supervision timeout=100 (1s)
      ```
      (NimBLE 1.4.2 has an equivalent `updateConnParams` overload if we stayed on 1.4.2 in A4.)
- [ ] The central must agree; log the negotiated interval if the API exposes it.
- **Accept:** with a central that honors it (phone/PC), 100 Hz TCode streams without packet starvation.
- **Vibe:** moderate. "beg the host for a tighter rhythm — 7.5 ms between thrusts or we're not interested :3"

### Step D2 — Single-pass TCode scanner
- [ ] **`src/comms/TCodeParser.cpp`** (`feedLine`/equivalent): replace `strtok`+`atoi`+temp-buffer with an
      in-place single-pass integer/axis scanner (no allocations, no copy). Preserve exact semantics:
      axis letter, channel index, value width/precision, optional interval/speed suffixes (`I`/`S`).
- [ ] Keep the old path compiled behind a temporary `LEGACY_TCODE_PARSER` flag until equivalence is proven.
- [ ] Validate against a captured command log (Intiface + MultiFunPlayer) — byte-for-byte same callbacks.
- **Accept:** identical motion vs. legacy parser on the captured log; measurable drop in per-line CPU.
- **Vibe:** moderate. "swallow the whole command in one pass — no fumbling, no spitting it back out :3"

---

## 7. Phase E — Binary WebSocket Protocol (LOW PRIORITY — DO LAST)

> Outcome: an **optional** second WS message path that is wire-compatible with the OSSM-Sauce app,
> giving a true zero-parse fast lane. TCode remains the default for Intiface/MFP. Implement only
> after A–D are stable.

### Step E1 — Protocol module scaffold
- [ ] **NEW `include/comms/BinaryWsProtocol.h` + `src/comms/BinaryWsProtocol.cpp`**: a transport-agnostic
      decoder that takes a binary frame and dispatches to the same motion callbacks TCode uses.
- [ ] Detect binary vs. text frames in `WebSocketTransport`: route binary → `BinaryWsProtocol`,
      text → TCode parser. No behavior change for existing text clients.

### Step E2 — Implement OSSM-Sauce opcodes (byte layouts — see Appendix A)
- [ ] `CommandType` enum (RESPONSE..SMOOTH_MOVE). `memcpy` into packed structs (mind ESP32 alignment;
      use `__attribute__((packed))` and field-by-field copies if needed — **do not** assume struct
      padding matches the sender; copy explicit byte offsets like OSSM-Sauce does).
- [ ] Implement at minimum: `POSITION` (5 B), `MOVE` (10 B), `LOOP` (19 B), `VIBRATE` (13 B),
      `PLAY/PAUSE/RESET`, `HOMING`, `SET_SPEED_LIMIT`, `SET_RANGE_LIMIT`. Map their 0..10000 position
      space to our mm range via `RangeMapper`.
- [ ] Reuse Phase B's trapezoidal planner + Phase C's `tween()` so binary moves get the same quality.
- [ ] Send `RESPONSE` acks where the app expects them.

### Step E3 — Verify against the OSSM-Sauce app
- [ ] Connect the OSSM-Sauce mobile app to the device's WS; verify homing, position, loop, vibrate,
      and playlist streaming all drive the machine correctly.
- **Accept:** OSSM-Sauce app fully controls SlopDrive-32; TCode clients still work unchanged.
- **Vibe:** moderate in `comms`, but the opcode-dispatch switch can get a little filthy per case. :3

---

## 8. Phase F — Verification & Flash Checklist

- [ ] `pio run` clean (no warnings introduced by our changes where avoidable).
- [ ] `pio run -t uploadfs` (web UI) + `pio run -t upload` (firmware).
- [ ] Smoke matrix:
  - [ ] Serial TCode stream @ 100 Hz — smooth, reaches extents.
  - [ ] BLE TCode stream @ 100 Hz (and 200 Hz where central allows) — no starvation.
  - [ ] WS TCode (Intiface/MFP) — unchanged behavior.
  - [ ] Generator: symmetric + asymmetric per-stroke, ramp-in, each tween curve.
  - [ ] Interpolator: linear / ease / cubic / Catmull-Rom selectable; Catmull degrades gracefully on
        a starved buffer.
  - [ ] Direction-flip torture test (square-wave target) — no chatter.
  - [ ] (If Phase E done) OSSM-Sauce app drives the machine.
- [ ] `emergencyStop()` still halts instantly from every path.
- [ ] 10-minute soak at 100 Hz — no watchdog resets, no latency creep.

---

## Appendix A — OSSM-Sauce Binary Protocol Reference (for Phase E)

```
Frame = [opcode:1 byte][payload...]

enum CommandType : byte {
  RESPONSE=0, MOVE=1, LOOP=2, POSITION=3, VIBRATE=4, PLAY=5, PAUSE=6,
  RESET=7, HOMING=8, CONNECTION=9, SET_SPEED_LIMIT=10, SET_GLOBAL_ACCELERATION=11,
  SET_RANGE_LIMIT=12, SET_HOMING_SPEED=13, SET_HOMING_TRIGGER=14, SMOOTH_MOVE=15
};

MOVE      : total 10 bytes — payload[1..9] = StrokeCommand wire fields
LOOP      : total 19 bytes — [1..9]=push StrokeCommand, [10..18]=pull StrokeCommand
POSITION  : total  5 bytes — [1..4]=uint32 position in 0..10000 (mapped to mm range)
VIBRATE   : total 13 bytes — [1..12]=Vibration struct
PLAY      : 2 or 6 bytes — [1]=MovementMode, optional [2..5]=uint32 playTimeMs
PAUSE/RESET/HOMING : 1 byte opcode (+ mode/trigger bytes as defined)

StrokeCommand (wire, packed — copy by explicit offset, DON'T trust C++ padding):
  endTimeMs   : uint32   (ms)
  depth       : short    (0..10000, mapped to user range)
  transType   : byte     (TransType)
  easeType    : byte     (EaseType)
  auxiliary   : byte
  // (targetPosition / durationReciprocal / baseSpeedHz are derived on-device, not on the wire)

Position space: 0..10000 → map(0..10000, rangeLimitUserMin, rangeLimitUserMax)
```

## Appendix B — Tween formulas (port to `float`, for C1)

```
LINEAR : t
SINE   IN  : 1 - cosf(t * PI * 0.5f)
SINE   OUT : sinf(t * PI * 0.5f)
SINE   IO  : -(cosf(PI*t) - 1) * 0.5f
QUAD   IN  : t*t            CUBIC IN : t^3   QUART IN : t^4   QUINT IN : t^5
EXPO   IN  : (t<=0)?0 : powf(2, 10*t - 10)
CIRC   IN  : 1 - sqrtf(1 - t*t)
*_OUT      : 1 - f_in(1 - t)
*_IN_OUT   : (t<0.5) ? f_in(2t)/2 : 1 - f_in(2(1-t))/2
EASE_OUT_IN: mirror of IN_OUT
exponent helper (OSSM-Sauce style, n=exponent):
  EASE_IN     : pow(w, n)
  EASE_OUT    : pow(w-1, n)
  EASE_IN_OUT : pow(1 - |2w-1|, n)
  EASE_OUT_IN : pow(1 - |2w-1|, n) - 1
```

---

## Suggested commit sequence (thematic, per `.clinerules` §6)

```
A1 feat: default cadence bumped to 100hz — we pound faster now :3
A3 feat: 200hz option in the UI for when 100 just isnt enough
A4 chore: nimble 2.x — newer, tighter, takes it better
B1 feat: per-command accel — we plan exactly how hard to slam home
B3 feat: trapezoidal planner lands every thrust on the beat :3
B4 feat: catmull-rom curve — silky glide instead of jackhammer
B6 feat: adaptive jitter buffer — we read the rhythm and never starve
C2 feat: per-stroke modulation — the machine learns the rhythm stroke by stroke
D1 fix: beg the host for a 7.5ms connection interval
D2 perf: single-pass tcode scanner — swallows commands whole :3
E2 feat: binary ws — now fully compatible with the OSSM-Sauce app
```
