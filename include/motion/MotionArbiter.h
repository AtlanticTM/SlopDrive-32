#pragma once

// MotionArbiter — event-driven motion planner and sole caller of MotorDriver.
//
// Constraints:
// - Motion doctrine (DOCTRINE.md §2): ONE intent -> ONE plan -> FAS executes.
//   No clocked motion tick, no chase loop. Every intent is planned ONCE, at
//   arrival, from the machine's ACTUAL current state (FAS position + live
//   velocity). Speed/accel are DERIVED from what the intent requires
//   (distance, deadline, ramp shape) and CLAMPED at the source's limit-set
//   ceilings — ceilings are never targets.
// - Retarget-while-moving is the normal case, not an edge case: a new intent
//   for the same source replans from live (p, v) via FAS's velocity-continuous
//   moveTo() retarget. This is what lets a 100-333 Hz host stream dense points
//   without a chase loop — each point becomes a retarget intent whose
//   deadline is the measured inter-command interval.
// - Depends on this much of FastAccelStepper's contract: getCurrentPosition()
//   is the open-loop commanded position; moveTo() is non-blocking and
//   retargets velocity-continuously from the current state; there is no
//   native asymmetric entry/exit accel. Ramp shaping is therefore a single
//   symmetric accel scaled by min(entryRamp, exitRamp) — true asymmetric
//   ramps would need two-segment dispatch with FAS completion awareness, not
//   implemented.
// - Blend/reversal policy: only "allow" is implemented (FAS retarget handles
//   reversals natively). "let-it-land" and "hybrid" are accepted but aliased
//   to "allow", logged as deprecated.
// - Sole-caller enforcement is compile-time: MotorDriver's motion methods
//   (moveTo/streamTo/streamToSteps/stop/hardStop) are protected with
//   `friend class MotionArbiter` (MotorDriver.h), so only this class can call
//   them; every other caller submits an intent via submit()/submitDeferred().
// - submit() runs from Core 0 transport callbacks AND Core 1 tasks; dispatch
//   is serialized under one portMUX_TYPE spinlock (sub-microsecond float math
//   only — no heap alloc, no ISR context). FAS itself is only ever called
//   from Core 1 (same core as the FAS engine): Core 0 callers enqueue via
//   submitDeferred() into a DEFER_QUEUE_DEPTH-slot FreeRTOS queue, drained in
//   full, in arrival order, by processDeferred() on motorTask (Core 1) every
//   tick.

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "MotorDriver.h"
#include "SystemState.h"
#include "config_api.h"

class RangeMapper;

// ---- MotionSource -----------------------------------------------------------
// Tags the origin of an intent so the arbiter can pick the right limit set
// and gating (MANUAL always wins the safety gates).
enum class MotionSource : uint8_t {
    MANUAL      = 0,   // WebUI rail tap, nudge, slider, move-to point
    TCODE_STREAM = 1,  // TCode L0 commands from any transport (Serial/WS/BLE/Dongle)
    PATTERN      = 2,  // Internal PatternEngine stroke segments
    OSSM_STREAM  = 3   // OSSM BLE streaming position commands
};

// ---- MotionIntent -----------------------------------------------------------
// The single entry point into the motion system.
struct MotionIntent {
    MotionSource source;
    float        target_mm;         // post window-mapping, pre-clamp
    uint32_t     deadline_ms;       // 0 = none (point move: plan at ceilings)
    float        speed_hint_mm_s;   // from S-extension when present, else 0
    // Pattern-derived accel demand (Advanced pattern mode). When BOTH hints are
    // present the planner takes them as the derived dynamics verbatim — the
    // pattern already derived them from its own stroke geometry — and the
    // ceiling clamps still apply. 0 = absent: accel is derived from distance +
    // deadline as before.
    float        accel_hint_mm_s2 = 0.0f;
    uint16_t     seq;               // per-source monotonic, telemetry attribution
};

// ---- PlanReport -------------------------------------------------------------
// Telemetry emitted by the planner after each dispatch.
struct PlanReport {
    float    derived_speed_mm_s;   // what the planner computed (before clamp)
    float    derived_accel_mm_s2;  // what the planner computed (before clamp)
    float    clamped_speed_mm_s;   // speed after applying limit set
    float    clamped_accel_mm_s2;  // accel after applying limit set
    int32_t  dispatched_steps;     // the final FAS target in native steps
    bool     deadline_feasible;    // true if the plan met the deadline
    bool     deadline_late;        // true if deadline was infeasible — arrived at ceilings
    uint32_t plan_us;              // microseconds spent in the planner (diagnostic)
};

// ---- MotionArbiter ----------------------------------------------------------
// Sole caller of MotorDriver for positioning.
class MotionArbiter {
public:
    MotionArbiter(SystemState& state, RangeMapper& mapper, MotorDriver& motor);

    // ---- Initialization (call after motor.init()) ---------------------------
    void init();

    // ---- Core 0 → Core 1 deferral (ring buffer queue) -----------------------
    // Pushes the intent into a FreeRTOS queue (non-blocking — drops if full,
    // correct for retarget semantics where the latest command wins at high Hz).
    // Replaces the old single-slot atomic which dropped frames at >100Hz.
    void submitDeferred(const MotionIntent& intent);

    // ---- Core 1 direct dispatch (called from Core 1 tasks only) -------------
    // PatternEngine and this class's own processDeferred() call this directly.
    // Plans and dispatches to FAS immediately. All FAS interaction stays on
    // Core 1. Returns the plan report for telemetry.
    PlanReport submit(const MotionIntent& intent);

    // ---- Core 1 deferred-intent consumer ------------------------------------
    // Called periodically from motorTask (Core 1). Drains the defer queue in
    // full, planning each intent via submit() in arrival order.
    void processDeferred();

    // ---- Core 1 stream-sample fast path (streamSamplerTask's Engine) --------
    // Called at ~1kHz by streamSamplerTask with a point sampled from its
    // slopmotion::Engine (DOCTRINE.md §8). This is NOT the trapezoid planner —
    // the Engine already shaped the curve. This path only runs the safety
    // gates (estop/homed/paused/override), maps the normalized position into the
    // stroke window, enforces the hard physical step bounds, and feeds FAS
    // directly via streamToSteps(). Accel is the constant input ceiling (kept
    // constant so the driver grit-cache stays quiet); speed depends on
    // SystemState::stream_speed_mode (ceiling-pegged vs velocity-matched).
    //   norm_pos        : 0..1 position within the configured stroke window
    //   norm_vel_per_s  : signed normalized units/second (VELOCITY_MATCHED only)
    // Returns true if a sample was dispatched, false if gated off.
    bool submitStreamSample(float norm_pos, float norm_vel_per_s);

    // ---- Emergency / gate helpers (Core 0 or Core 1) ------------------------
    void emergencyStop();
    void stopMotion();     // full stop: halts pulse train, cuts power, clears homed (MotorDriver::stop())
    void hardStopMotion(); // immediate stop, motor stays powered (MotorDriver::hardStop())
    void pause();
    void resume();

    // ---- Source gating (Core 1 read, Core 0 write via SystemState) ----------
    // Pause/override flags are read from SystemState on submit().

    // ---- Limit sets — updated by ConfigStore/API ----------------------------
    // USER set: manual moves, UI controls
    void setUserSpeedLimit(float mm_s);
    void setUserAccelLimit(float mm_s2);
    // INPUT set: TCode, PatternEngine, OSSM
    void setInputSpeedLimit(float mm_s);
    void setInputAccelLimit(float mm_s2);

    // ---- Telemetry — last plan report (atomic, any core) --------------------
    PlanReport lastReport() const;
    uint32_t   totalIntents() const { return _intent_count; }
    // Intents rejected by a gate (not-homed / e-stop / paused / override /
    // Intiface-recency). Surfaced in /api/status so a gated-off stream is
    // distinguishable from "no commands arrived" in the diagnostics.
    uint32_t   rejectedIntents() const { return _rejected_count; }

    // ---- Blend/reversal policy (stored, but currently all alias to "allow") --
    void setBlendMode(uint8_t mode);   // 1=let-it-land 2=allow 3=hybrid
    uint8_t getBlendMode() const { return _blend_mode; }

private:
    SystemState&  _state;
    RangeMapper&  _mapper;
    MotorDriver&  _motor;

    // ---- Limit sets ---------------------------------------------------------
    float _user_speed_limit_mm_s  = DEFAULT_USER_MAX_SPEED_MM_S;   // gentle (50)
    float _user_accel_limit_mm_s2 = DEFAULT_USER_ACCEL_MM_S2;      // gentle (200)
    float _input_speed_limit_mm_s  = DEFAULT_MAX_SPEED_MM_S;
    float _input_accel_limit_mm_s2 = DEFAULT_ACCEL_MM_S2;

    // ---- Dispatch lock (microcritical — protects FAS calls on Core 1) -------
    mutable portMUX_TYPE _dispatch_mux = portMUX_INITIALIZER_UNLOCKED;

    // ---- Core 0 → Core 1 deferral queue (DEFER_QUEUE_DEPTH slots, non-blocking
    // push) — see DEFER_QUEUE_DEPTH below for the actual depth. Handles 333Hz
    // streams: Core 0 pushes intents at 3ms intervals, Core 1 drains the entire
    // queue each loop tick. Drop-if-full. Created by init().
    QueueHandle_t     _defer_queue = nullptr;
    static constexpr uint8_t DEFER_QUEUE_DEPTH = 16;

    // ---- Blend policy -------------------------------------------------------
    uint8_t _blend_mode = 2;   // "allow" — FAS retarget handles reversals

    // ---- Telemetry ----------------------------------------------------------
    PlanReport           _last_report = {};
    volatile uint32_t    _intent_count = 0;
    volatile uint32_t    _rejected_count = 0;
    mutable portMUX_TYPE _telemetry_mux = portMUX_INITIALIZER_UNLOCKED;

    // ---- Core planner (the heart — D4) --------------------------------------
    // Executed under _dispatch_mux on Core 1. Reads actual machine state from
    // FAS, derives the trapezoidal profile, clamps at the source's limit set,
    // dispatches to FAS.
    PlanReport _planAndDispatch(const MotionIntent& intent, bool locked);

    // ---- Gate evaluation ----------------------------------------------------
    // Returns true if the intent should proceed. MANUAL bypasses all gates
    // except E-stop; stream/pattern sources honor homed/paused/override/window.
    bool _gatesPass(const MotionIntent& intent);

    // ---- Window clamping ----------------------------------------------------
    float _clampToWindow(float mm, MotionSource source);

    // ---- Window-entry detection ---------------------------------------------
    // True when p0_mm is currently OUTSIDE the configured stroke window (with a
    // small epsilon). Machine-driven sources (stream/pattern/OSSM) honor the
    // gentle USER limits on the move that carries the carriage from outside the
    // window into it, so it glides in instead of lunging to the edge at the
    // input ceiling. Once inside, the normal INPUT set resumes.
    bool _isOutsideWindow(float p0_mm) const;
};
