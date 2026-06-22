# SlopDrive-32 Refactor Blueprint
## OSSM Kinematics Port + FreeRTOS Dual-Core Bridge

**Document purpose:** Self-contained implementation plan for an agent executing this refactor. All relevant context from the codebase audit is included inline. No prior conversation context is required.

---

## Project Context

**Hardware:** ESP32-S3 dual-core microcontroller running FreeRTOS.
**V2 Motor:** 57AIM30 Integrated Modbus Servo (same motor as the reference OSSM codebase).
**Motion library:** FastAccelStepper (FAS) — handles step pulse generation via hardware timer ISR on Core 1.
**TCode input:** WebSocket, Serial, or BLE — all parsed by `TCodeParser` on Core 0.
**Reference codebase:** OSSM (open-source sex machine firmware) — `C:\Users\Atlan\Downloads\OSSM-hardware-main`.

**Important open-loop note:** The 57AIM30 servo is operated open-loop. The firmware does NOT read encoder feedback. `stepper->getCurrentPosition()` returns the last *commanded* step count, not a measured position. This is identical to how OSSM operates. Drift is negligible on a properly tuned servo and is outside firmware control.

**Transport scope:** TCode over WebSocket, Serial, and BLE. No API masquerade or OSSM BLE spoofing required.

---

## Codebase Map (Files Relevant to This Refactor)

```
src/
  main.cpp                        — Composition root, FreeRTOS task creation, buttplugLinearCmd()
  motion/
    Kinematics.cpp/.h             — planTrapezoid() + generator math helpers
    TMC2160StepperDriver.cpp/.h   — Concrete MotorDriver (V1 stepper, build-guarded DRIVER_TMC2160)
    Generator.cpp/.h              — On-device waveform generator (Core 1 task)
  comms/
    TCodeParser.cpp/.h            — Transport-agnostic TCode v0.3 parser
  system/
    SystemState.h                 — Centralised runtime state struct (cross-core fields)
  ui/
    WebUI.cpp/.h                  — HTTP REST API + telemetry ring
include/
  motion/
    Kinematics.h                  — PlanResult struct + planTrapezoid() declaration
    TMC2160StepperDriver.h        — Driver class declaration
    MotorDriver.h                 — Abstract base class (interface)
  system/
    SystemState.h                 — SystemState struct
  config_api.h                    — All hardware constants (STEPS_PER_MM, MAX_SPEED_MM_S, etc.)
```

---

## Current Architecture — What Exists Today

### The Problem: Synchronous Cross-Core Dispatch

`TCodeParser::feedLine()` runs on **Core 0** (commsTask). For every parsed `L0<pos>I<duration>` token it fires a synchronous callback:

```cpp
// TCodeParser.cpp line 139
if (_onLinearCmd) _onLinearCmd(position, duration_ms);
```

The callback is `buttplugLinearCmd()` in `main.cpp` (lines 75–195). It:
1. Measures inter-command cadence (EMA filter for Hz display)
2. Maps TCode position to mm via `RangeMapper`
3. Calls `kinematics::planTrapezoid()` to compute speed/accel
4. Calls `motor.setAcceleration()` then `motor.streamTo()` — **which touches FastAccelStepper objects whose engine and ISR live on Core 1**

There is no mutex, queue, or atomic protecting this cross-core access. FAS's internal `moveTo()` command queue provides implicit safety for position commands, but `setAcceleration()` and `setSpeedInHz()` write directly to FAS's internal state from the wrong core. This is a latent crash.

### Current `planTrapezoid()` — What It Does Right and Wrong

**Correct:**
- Uses `v = 2.0f × dist / duration_s` (the triangle peak speed formula — identical to OSSM)
- Non-blocking `moveTo()` retarget (no force-stop between waypoints)

**Legacy bloat to remove:**
- Uses fixed `a = a_max` ceiling for every segment (no per-segment back-calculation)
- Distance clamping was removed (was correct for old hardware, needs re-evaluation for V2)
- Raise-only acceleration guard was removed (was causing stale accel lock-in on old hardware)
- S-curve (`setLinearAcceleration()`) baked in at `init()` — OSSM doesn't use this at all
- `_blend_mode` logic in `streamTo()` — replaced by direction gating in the new queue consumer
- Target-chaining (`s_last_target_mm`) — replaced by live `getCurrentPosition()` read

---

## The Three Changes

### Change 1 — FreeRTOS Queue Bridge (CRITICAL — fixes a correctness bug)

**What:** Core 0's TCode callback pushes a `PositionTime` struct into a FreeRTOS `xQueue`. A new task pinned to Core 1 pops from the queue and does all motor work. Core 0 never touches the stepper object again.

**Why critical:** The current cross-core `setAcceleration()`/`setSpeedInHz()` calls are undefined behavior on the ESP32-S3's dual-core architecture. This is a stability fix, not just a quality improvement.

**Drawback:** Adds ~1ms queue hop latency. At 100Hz TCode this is imperceptible. If the queue fills (8 slots), the newest waypoint is dropped — a 10ms gap at 100Hz, also imperceptible.

### Change 2 — Back-Calculated Acceleration (HIGH — core kinematic improvement)

**What:** Replace the fixed `a = a_max` ceiling with OSSM's per-segment back-calculation:
```
requiredSpeed = (2 × distance) / timeSeconds          // triangle peak
proportion    = max(-((2×dist - 2×vt) / vt), 0.01)   // trapezoid fraction
requiredAccel = requiredSpeed / (timeSeconds × proportion / 2)
```
The raise-only guard is re-enabled: while the motor is running, acceleration can only increase.

**Why:** Short fast segments get high acceleration. Long slow segments get lower, smoother acceleration. Motion self-tunes to the content rhythm instead of always slamming the ceiling.

**Drawback:** Back-calculated accel can be very high for short fast segments. The `accel_limit` ceiling in config must be set correctly for the 57AIM30's capabilities.

### Change 3 — Distance Clamping Re-enabled (MEDIUM — safety valve)

**What:** Re-enable OSSM's physics guard that shortens a stroke when the commanded move is physically impossible in the time budget:
```
maxDistance = accelLimit × (timeSeconds / 2)²
maxDistance = min(maxDistance, speedLimit × timeSeconds)
if (distance > maxDistance): shorten stroke, preserve direction
```

**Why it was removed:** On old hardware with `a_max = 1500 mm/s²` and 250ms intervals, the clamp fired at 23mm — collapsing every full-depth stroke to a 20mm twitch. That was a real bug.

**Why it's safe now:** With the 57AIM30's acceleration capability (8000+ mm/s²), the clamp fires at `8000 × (0.125)² = 125mm` for a 250ms interval — beyond any physical stroke. In normal operation it never fires. It only activates for genuinely impossible commands (e.g. 150mm stroke in 50ms).

**Open-loop note:** `currentPosition` in the clamp calculation is `stepper->getCurrentPosition()` — the last commanded step count, not a measured encoder value. This is identical to OSSM's behavior and is correct.

**Drawback:** None in practice. The clamp is a pure safety valve at V2 acceleration levels.

---

## Phase 0: Deletions — Do These First

### Delete from `src/motion/Kinematics.cpp` and `include/motion/Kinematics.h`

| What | Why |
|---|---|
| `planTrapezoid()` function body | Replaced entirely with OSSM pipeline |
| `PlanResult` struct | Fields change (now step-units, adds `distance_clamped`) |

**Keep:** `carrier()`, `modShape()`, `ease()`, `bufEase()` — these are used by `Generator.cpp` and are unrelated to the streaming pipeline.

### Delete from `src/motion/TMC2160StepperDriver.cpp` and `.h`

| What | Location | Why |
|---|---|---|
| `_linear_accel_steps` field | `.h` line 131 | S-curve baked at init — OSSM doesn't use it |
| `setLinearAcceleration()` call in `init()` | `.cpp` | Same |
| `_blend_mode` field | `.h` lines 136–141 | Replaced by direction gating in queue consumer |
| `setBlendMode()` / `getBlendMode()` | `.h` lines 93–94 | Same |
| All blend-mode logic in `streamTo()` | `.cpp` lines 569–618 | Same |

### Delete from `src/main.cpp`

| What | Location | Why |
|---|---|---|
| `buttplugLinearCmd()` body (lines 75–195) | `main.cpp` | Replaced by push-only version |
| `s_last_target_mm` static + target-chaining | Inside `buttplugLinearCmd()` | Replaced by live `getCurrentPosition()` |
| Velocity feed-forward / lookahead block | `main.cpp` lines 119–149 | OSSM explicitly doesn't use lookahead |
| Resume blend timer | `main.cpp` lines 151–161 | Replaced by queue consumer's direction gating |
| `#include "Kinematics.h"` | `main.cpp` line 23 | Kinematics now called from Core 1 consumer, not Core 0 callback |

### Delete from `src/ui/WebUI.cpp`

| What | Why |
|---|---|
| `blend_mode` field in `handleApiSettings()` GET/POST | Blend mode is gone |
| `_motor.setBlendMode()` call | Same |

---

## Phase 1: New Data Structures

### 1.1 — Create `include/motion/PositionTime.h`

```cpp
#pragma once
#include <cstdint>
#include <chrono>
#include <optional>

// Waypoint pushed by Core 0 (TCode parser) and consumed by Core 1 (motion task).
// Mirrors OSSM's PositionTime struct from queue.h — same fields, same semantics.
// position: 0–100 (TCode normalized, mapped to steps by the Core 1 consumer)
// inTime:   milliseconds (the TCode I-parameter)
// setTime:  optional RX timestamp for latency compensation
struct PositionTime {
    uint8_t  position;   // 0–100
    uint16_t inTime;     // ms
    std::optional<std::chrono::steady_clock::time_point> setTime;
};
```

### 1.2 — New `PlanResult` struct in `include/motion/Kinematics.h`

Replace the existing struct:

```cpp
struct PlanResult {
    int32_t  target_steps;      // final clamped target in native steps
    uint32_t speed_steps_s;     // peak speed in steps/s (2×dist/T triangle peak)
    uint32_t accel_steps_s2;    // back-calculated acceleration in steps/s²
    bool     distance_clamped;  // true if the move was shortened by the physics guard
};
```

**Why steps instead of mm?** OSSM works entirely in native steps. Doing the unit conversion once at the planner boundary (not scattered across `streamTo()`) is cleaner and matches OSSM exactly.

### 1.3 — Add `std::atomic<float>` to `SystemState.h`

Add to the `SystemState` struct (after the existing `commanded_target_mm` field):

```cpp
#include <atomic>

// Written by Core 1 motion consumer after each dispatch.
// Read by Core 0 telemetry timer (WebUI::telemetryTimerCb).
// std::atomic<float> gives the no-tear guarantee with zero overhead on S3.
// memory_order_relaxed is correct — telemetry is display-only, no ordering needed.
std::atomic<float> actual_position_mm{0.0f};
```

### 1.4 — FreeRTOS Queue Declaration in `main.cpp`

Add near the top of `main.cpp` (after includes):

```cpp
#include "PositionTime.h"
#include "freertos/queue.h"

// 8-slot waypoint queue: at 100Hz TCode = 80ms of buffer.
// Enough to absorb a WiFi jitter spike; not so deep that a stale burst
// plays out after the stream stops. Core 0 pushes, Core 1 pops. :3
static constexpr size_t WAYPOINT_QUEUE_DEPTH = 8;
static QueueHandle_t g_waypoint_queue = nullptr;
```

In `setup()`, before task creation:
```cpp
g_waypoint_queue = xQueueCreate(WAYPOINT_QUEUE_DEPTH, sizeof(PositionTime));
configASSERT(g_waypoint_queue != nullptr);
```

---

## Phase 2: New `planTrapezoid()` Implementation

Replace the entire function body in `src/motion/Kinematics.cpp`. The signature changes to accept and return native steps:

### New signature in `include/motion/Kinematics.h`

```cpp
// OSSM streaming pipeline, ported verbatim. All units are native stepper steps.
// current_steps : stepper->getCurrentPosition() — live commanded position
// target_steps_raw : TCode position mapped to native steps (pre-clamp)
// time_s        : command interval in seconds
// speed_limit   : max speed in steps/s
// accel_limit   : max accel in steps/s²
// min_steps     : stroke window minimum (native steps, may be negative)
// max_steps     : stroke window maximum (native steps, may be negative)
PlanResult planTrapezoid(
    int32_t  current_steps,
    int32_t  target_steps_raw,
    float    time_s,
    uint32_t speed_limit,
    uint32_t accel_limit,
    int32_t  min_steps,
    int32_t  max_steps
);
```

### New implementation in `src/motion/Kinematics.cpp`

```cpp
PlanResult planTrapezoid(int32_t current_steps, int32_t target_steps_raw,
                         float time_s, uint32_t speed_limit, uint32_t accel_limit,
                         int32_t min_steps, int32_t max_steps) {
    PlanResult r = {};

    // Step 1: Clamp target into stroke window
    r.target_steps = constrain(target_steps_raw, min_steps, max_steps);

    // Step 2: Live distance from actual (commanded) position
    int32_t distance = abs(r.target_steps - current_steps);

    // No cadence info or trivial move — gentle nudge
    if (time_s <= 0.01f || distance < 2) {
        r.speed_steps_s  = speed_limit / 4;
        r.accel_steps_s2 = accel_limit;
        return r;
    }

    // Step 3: Distance clamping — physics guard
    // Fires only when the commanded move is physically impossible in the time budget.
    // At V2 servo acceleration levels (8000+ mm/s²) this almost never fires in practice.
    // When it does fire, it shortens the stroke to preserve phase-lock with the content.
    int32_t maxDist = (int32_t)((float)accel_limit * (time_s / 2.0f) * (time_s / 2.0f));
    maxDist = min(maxDist, (int32_t)((float)speed_limit * time_s));
    if (distance > maxDist) {
        // 2-step safety margin (matches OSSM's 2mm margin)
        distance = maxDist - 2;
        if (distance < 1) distance = 1;
        if (r.target_steps > current_steps)
            r.target_steps = current_steps + distance;
        else
            r.target_steps = current_steps - distance;
        r.distance_clamped = true;
    }

    // Step 4: Triangle peak speed (2×dist/T)
    uint32_t requiredSpeed = (uint32_t)(2.0f * (float)distance / time_s);

    // Step 5: Speed clamping
    requiredSpeed = constrain(requiredSpeed, 100u, speed_limit);

    // Step 6: Trapezoid proportion
    float vt = (float)requiredSpeed * time_s;
    float proportion = fmaxf(-((2.0f * (float)distance - 2.0f * vt) / vt), 0.01f);

    // Step 7: Back-calculated acceleration
    uint32_t requiredAccel = (uint32_t)((float)requiredSpeed / (time_s * proportion / 2.0f));
    requiredAccel = constrain(requiredAccel, 100u, accel_limit);

    r.speed_steps_s  = requiredSpeed;
    r.accel_steps_s2 = requiredAccel;
    return r;
}
```

---

## Phase 3: New Core 0 Push Callback

Replace `buttplugLinearCmd()` in `main.cpp` with this push-only version:

```cpp
static void buttplugLinearCmd(float position, uint32_t duration_ms) {
    if (!g_state.homed) return;
    if (g_state.paused || g_state.manual_override) {
        g_state.resume_start_ms = millis();
        return;
    }

    g_state.last_intiface_ms = millis();

    // Cadence measurement — Core 0 only, feeds Hz display and auto-duration
    uint32_t now = millis();
    if (g_state.last_cmd_ms != 0) {
        uint32_t gap = now - g_state.last_cmd_ms;
        if (gap > 0 && gap < 1000) {
            g_state.measured_interval_ms = (g_state.measured_interval_ms <= 0.0f)
                ? (float)gap
                : 0.7f * g_state.measured_interval_ms + 0.3f * (float)gap;
        }
    }
    g_state.last_cmd_ms = now;

    // Auto-duration fallback (only when host sends no I-parameter)
    if (duration_ms == 0 && g_state.auto_duration && g_state.measured_interval_ms > 1.0f)
        duration_ms = (uint32_t)(g_state.measured_interval_ms + 0.5f);

    // Build waypoint and push to queue — non-blocking (0 timeout).
    // If the queue is full, the waypoint is dropped. At 100Hz a dropped
    // waypoint is a 10ms gap — imperceptible. Core 0 NEVER blocks for Core 1.
    PositionTime pt;
    pt.position = (uint8_t)constrain((int)(position * 100.0f), 0, 100);
    pt.inTime   = (uint16_t)constrain((int)duration_ms, 0, 65535);
    pt.setTime  = std::chrono::steady_clock::now();

    xQueueSend(g_waypoint_queue, &pt, 0);

    // Stash raw mapped demand for UI motion graph (Core 0 only)
    g_state.commanded_raw_mm = mapper.intensityToPosition(position);
}
```

---

## Phase 4: New Core 1 Motion Consumer Task

Add this function to `main.cpp` (or a new `src/motion/MotionConsumer.cpp`):

```cpp
static void motionConsumerTask(void* /*param*/) {
    // Absolute timeline anchor — self-correcting clock, OSSM-style.
    // Advances by inTime each cycle to stay synchronized with intended playback rate.
    auto best = std::chrono::steady_clock::now();

    PositionTime lastPt = {};
    lastPt.position = 50;
    lastPt.inTime   = 250;
    int8_t lastDir  = 0;   // direction of last dispatched move: -1, 0, +1

    while (true) {
        PositionTime pt;

        // Block waiting for a waypoint — up to 50ms.
        // If nothing arrives, loop back. The stream stall watchdog in
        // motorTask handles settle-at-last-position behavior.
        if (xQueueReceive(g_waypoint_queue, &pt, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }

        // ---- Direction gating (OSSM raise-only guard) -----------------------
        // If the new command reverses direction AND the stepper is still running
        // the previous stroke, put the waypoint back and wait 1ms.
        // This prevents commanding a reversal while the motor is mid-acceleration,
        // which would require a longer brake ramp than the remaining distance.
        int16_t delta  = (int16_t)pt.position - (int16_t)lastPt.position;
        int8_t  newDir = (delta > 0) ? 1 : (delta < 0) ? -1 : 0;
        bool    sameDir = (newDir == 0) || (lastDir == 0) || (newDir == lastDir);
        if (!sameDir && motor.isMoving()) {
            xQueueSendToFront(g_waypoint_queue, &pt, 0);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        float timeSeconds = pt.inTime / 1000.0f;

        // ---- Latency compensation (OSSM-style) ------------------------------
        // Uses the RX timestamp to measure actual transport lag and adjusts
        // timeSeconds up to 25% shorter to resynchronize if we're falling behind.
        if (pt.setTime.has_value()) {
            uint16_t bufTarget = 100; // ms — configurable, default 100ms buffer
            int16_t currentBuffer = (int16_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - best).count();
            int16_t lag = (int16_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    pt.setTime.value() - best).count();

            if (lag < 0 || lag > bufTarget * 10) {
                best = pt.setTime.value();
                lag  = 0;
            }
            best += std::chrono::milliseconds(pt.inTime);

            int16_t offset = bufTarget - currentBuffer;
            if (offset < 0) {
                offset = (int16_t)max((int)(-pt.inTime / 4), (int)offset);
            }
            timeSeconds += offset / 1000.0f;
        } else {
            best = std::chrono::steady_clock::now();
        }

        lastPt  = pt;
        if (newDir != 0) lastDir = newDir;

        if (timeSeconds <= 0.01f) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }

        // ---- Map TCode 0–100 to native steps --------------------------------
        // 0 = fully retracted (min_mm), 100 = fully inserted (max_mm)
        // Coordinate system: endstop = 0 steps, front = negative steps
        float   frac         = pt.position / 100.0f;
        float   target_mm    = mapper.getMinMm() + frac * (mapper.getMaxMm() - mapper.getMinMm());
        int32_t target_steps = -motor.mmToNative(target_mm);  // negative = toward front

        int32_t min_steps = -motor.mmToNative(mapper.getMaxMm());
        int32_t max_steps = -motor.mmToNative(mapper.getMinMm());

        // ---- OSSM kinematic pipeline ----------------------------------------
        // NOTE: getCurrentPosition() returns last COMMANDED step count (open-loop).
        // This is correct — identical to OSSM's behavior.
        int32_t current_steps = motor.getStepperCurrentPosition(); // see note below

        PlanResult plan = kinematics::planTrapezoid(
            current_steps,
            target_steps,
            timeSeconds,
            (uint32_t)(g_state.config.max_speed_mm_s    * STEPS_PER_MM),
            (uint32_t)(g_state.config.acceleration_mm_s2 * STEPS_PER_MM),
            min_steps,
            max_steps
        );

        // ---- Raise-only acceleration guard (OSSM Step 8) --------------------
        // Once moving, acceleration can only increase. Lowering it mid-flight
        // means FAS needs a longer brake ramp than the remaining distance — overshoot.
        uint32_t finalAccel = plan.accel_steps_s2;
        if (motor.isMoving()) {
            finalAccel = max(motor.getStepperAcceleration(), finalAccel);
        }

        // ---- Dispatch to FAS (non-blocking retarget) ------------------------
        // All three calls happen on Core 1 — same core as the FAS engine. Safe.
        motor.streamToSteps(plan.target_steps, plan.speed_steps_s, finalAccel);

        // ---- Telemetry (atomic write — no mutex needed on S3) ---------------
        g_state.actual_position_mm.store(
            motor.nativeToMm(-plan.target_steps),
            std::memory_order_relaxed);
        g_state.commanded_target_mm = motor.nativeToMm(-plan.target_steps);

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
```

**Register in `setup()` after queue creation:**
```cpp
// Priority 4 — above motorTask (3) but below FAS ISR
xTaskCreatePinnedToCore(motionConsumerTask, "MotionConsumer", 4096, nullptr, 4, nullptr, 1);
```

---

## Phase 5: Motor Driver Interface Changes

The consumer task needs two new methods on `MotorDriver` / `TMC2160StepperDriver`:

### 5.1 — `streamToSteps()` — dispatch in native steps (no unit conversion)

Add to `MotorDriver.h` (pure virtual) and implement in `TMC2160StepperDriver`:

```cpp
// Dispatch a pre-planned move directly in native steps.
// Called exclusively from Core 1 (motionConsumerTask).
// accel and speed are already in steps/s and steps/s².
virtual void streamToSteps(int32_t target_steps, uint32_t speed_steps_s, uint32_t accel_steps_s2) = 0;
```

Implementation in `TMC2160StepperDriver.cpp`:
```cpp
void TMC2160StepperDriver::streamToSteps(int32_t target_steps,
                                          uint32_t speed_steps_s,
                                          uint32_t accel_steps_s2) {
    if (!_homed || !_stepper) return;
    enable();

    // Arm stall watchdog
    _last_sample_steps = target_steps;
    _last_sample_ms    = millis();
    _have_last_sample  = true;

    _stepper->setAcceleration(accel_steps_s2);
    _stepper->setSpeedInHz(speed_steps_s);
    _stepper->moveTo(target_steps);  // non-blocking retarget
}
```

### 5.2 — `getStepperCurrentPosition()` and `getStepperAcceleration()` — expose FAS internals to consumer

Add to `MotorDriver.h` and implement in `TMC2160StepperDriver`:

```cpp
virtual int32_t  getStepperCurrentPosition() const = 0;
virtual uint32_t getStepperAcceleration()    const = 0;
```

```cpp
int32_t  TMC2160StepperDriver::getStepperCurrentPosition() const {
    return _stepper ? _stepper->getCurrentPosition() : 0;
}
uint32_t TMC2160StepperDriver::getStepperAcceleration() const {
    return _stepper ? _stepper->getAcceleration() : 0;
}
```

---

## Phase 6: Telemetry Fix

In `WebUI.cpp`, `telemetryTimerCb()` currently calls `_motor.getPosition()` from Core 0. Replace with the atomic read:

```cpp
void WebUI::telemetryTimerCb(void* arg) {
    WebUI* self = static_cast<WebUI*>(arg);
    // Read actual_position_mm atomically — written by Core 1 consumer, safe to read here
    float actual = self->_state.actual_position_mm.load(std::memory_order_relaxed);
    self->captureTelemetry(actual,
                           self->_state.commanded_target_mm,
                           self->_state.commanded_raw_mm);
}
```

---

## Thread Safety Summary

Every cross-core data path, evaluated individually:

| Data | Direction | Mechanism | Notes |
|---|---|---|---|
| `PositionTime` waypoints | Core 0 → Core 1 | `xQueueSend` / `xQueueReceive` | Multi-field struct — queue is the correct tool |
| `g_state.homed` | Core 0 write, Core 1 read | `volatile bool` (existing) | 32-bit aligned bool, hardware-atomic on S3 |
| `g_state.estop_requested` | Core 0 write, Core 1 clear | `volatile bool` (existing) | Same |
| `g_state.commanded_target_mm` | Core 1 write, Core 0 read | `volatile float` (existing) | 32-bit aligned float, hardware-atomic on S3 |
| `g_state.actual_position_mm` | Core 1 write, Core 0 read | `std::atomic<float>` (NEW) | Explicit atomic — telemetry display value |
| `g_state.config.max_speed_mm_s` | Core 0 write (WebUI), Core 1 read | `volatile float` (existing) | Single 32-bit float, hardware-atomic on S3 |
| `g_state.config.acceleration_mm_s2` | Core 0 write, Core 1 read | `volatile float` (existing) | Same |
| Generator config (`gen_mux`) | Core 0 write, Core 1 read | `portMUX_TYPE` spinlock (existing) | Multi-field struct — keep spinlock as-is |
| `motor.*` stepper calls | Core 1 only (after refactor) | No synchronization needed | Core 1 owns all FAS access exclusively |

**The golden rule after this refactor:** Core 0 never calls any method on `motor` that touches FAS internals. Core 0 only writes to `g_state` fields and pushes to `g_waypoint_queue`.

---

## Implementation Order

Execute in this exact sequence to keep the build green at every step:

1. ✅ **Create `include/motion/PositionTime.h`** — new struct, no behavior change
2. ✅ **Add `g_waypoint_queue` to `main.cpp`** — created in `setup()` before task creation
3. ✅ **Add `std::atomic<float> actual_position_mm` to `SystemState.h`**
4. ✅ **Replace `PlanResult` struct and `planTrapezoid()` signature** in `Kinematics.h`
5. ✅ **Implement new `planTrapezoid()` body** in `Kinematics.cpp` — full OSSM 7-step pipeline
6. ✅ **Add `streamToSteps()`** to `MotorDriver.h` and `TMC2160StepperDriver` (`.h` + `.cpp`)
7. ✅ **Write `motionConsumerTask()`** and register it in `setup()` — priority 4, Core 1
8. ✅ **Replace `buttplugLinearCmd()`** with push-only version — queue is now the only path
9. **Delete the bloat** (Phase 0 deletions) — build and verify
10. ✅ **Fix telemetry** in `WebUI.cpp` to use `actual_position_mm` atomic
11. **Test** with MultiFunPlayer over WebSocket, then Serial, then BLE

---

## Key Constants (from `config_api.h`)

These are referenced throughout the implementation. Do not hardcode them — always use the constants:

```cpp
STEPS_PER_MM          // steps per millimetre (compile-time, derived from MICROSTEPS)
PHYSICAL_MAX_TRAVEL_MM // maximum physical stroke in mm
MAX_SPEED_MM_S        // absolute speed ceiling in mm/s
DEFAULT_ACCEL_MM_S2   // default acceleration in mm/s²
MICROSTEPS            // microstepping setting (16)
```

---

## What Does NOT Change

- `TCodeParser` — untouched. It still fires `_onLinearCmd` callback. Only the callback body changes.
- `Generator.cpp` — untouched. It calls `motor.streamTo()` directly from Core 1 (its own task). No cross-core issue.
- `motorTask` — untouched. Still handles E-stop, homing, push-to-home, and the stream stall watchdog.
- `WebUI.cpp` REST API — mostly untouched except the telemetry read fix and removal of `blend_mode`.
- `ConfigStore` — untouched.
- All transport code (`SerialTransport`, `WebSocketTransport`, `BleTransport`) — untouched.
- Homing procedure — untouched.
- `carrier()`, `modShape()`, `ease()`, `bufEase()` in `Kinematics.cpp` — untouched.
