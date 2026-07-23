#pragma once

// ============================================================================
// MotionArbiter — Event-Driven Motion Planner + Sole Caller of MotorDriver
// ============================================================================
//
// D4 doctrine: ONE COMMAND → ONE PLAN → FAS EXECUTES. No motion tick. No
// chase loop. Every position intent is translated ONCE, at arrival, into a FAS
// motion plan computed FROM THE MACHINE'S ACTUAL CURRENT STATE (position +
// live velocity from FAS), with velocity and acceleration DERIVED from what the
// intent requires — distance, deadline, ramp shape — and merely CLAMPED at the
// ceilings. Max speed/accel are CEILINGS, never targets.
//
// Retarget-while-moving IS the normal case — a new intent for the same source
// replans from live (p, v) via FAS's velocity-continuous retarget (moveTo()).
// This fully supports v3 hosts dumping 100–333Hz points: each point becomes a
// retarget intent whose deadline is the measured inter-command interval.
//
// PatternEngine is refactored to emit ONE intent per stroke segment (endpoint
// + segment duration + ease from the pattern's shape) instead of clock-ticks.
//
// FAS API verified against vendored FastAccelStepper:
//   - getCurrentPosition() — int32_t native steps, open-loop commanded position
//   - isRunning() — bool trajectory completion query
//   - getAcceleration() — uint32_t active accel setting in steps/s²
//   - moveTo(int32_t) — non-blocking, retargets velocity-continuously from
//     current state (CONFIRMED: "FAS re-plans from current velocity")
//   - setSpeedInHz(uint32_t), setAcceleration(uint32_t) — symmetric accel only
//   - No native asymmetric entry/exit accel support
//
// Ramp shaping: single symmetric accel → min(entryRamp, exitRamp) multiplier
// applied to the derived accel. Fidelity note: true asymmetric entry/exit would
// require two-segment dispatch with FAS completion awareness; not warranted at
// this stage — single-symmetric with conservative multiplier is indistinguishable
// for v0.4 ramp commands at the speeds SlopDrive-32 operates.
//
// Blend/reversal: "allow" only (FAS retarget handles reversals natively).
// "let-it-land" and "hybrid" accepted-but-aliased to "allow" with deprecation
// log. Per D4: "If honoring them cleanly conflicts with D4, implement 'allow'
// only, stub the others as accepted-but-aliased, and flag for retirement."
//
// Sole-caller enforcement: compile-time via friend declaration. MotorDriver
// motion methods (moveTo/streamTo/streamToSteps/stop/hardStop) become private
// with `friend class MotionArbiter`. All other callers route through submit().
//
// DECIDE: submit() runs on transport callbacks (Core 0 tasks) and Core 1 tasks.
// Single portMUX_TYPE spinlock on dispatch — sub-microsecond float math,
// microcritical. No heap alloc. No ISR contexts. FAS calls only from Core 1
// (task-context, same core as FAS engine). For Core 0 callers: intent is
// enqueued via a single-slot atomic deferral consumed by motionConsumerTask
// (or FAS dispatch task) on Core 1. DECIDE: single-slot atomic deferral
// (Core 0 sets intent, Core 1 consumer checks and applies). Justification:
// avoids mutex contention on the FAS call path and keeps ALL FAS interaction
// on Core 1. The single-slot design means a rapid burst of Core 0 submits
// only keeps the latest intent — correct for TCode retarget semantics where
// only the freshest command matters.

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "MotorDriver.h"
#include "SystemState.h"
#include "config_api.h"

class RangeMapper;

// ============================================================================
// RampShape — per-intent acceleration multipliers from TCode AxisRampData
// ============================================================================
// MIT attribution: struct shape derived from jcfain/TCodeESP32 v0.4 AxisRampData
struct RampShape {
    float entryMultiplier = 1.0f;   // 1.0 = full derived accel (disabled)
    float exitMultiplier  = 1.0f;   // 1.0 = full derived accel (disabled)
};

// ============================================================================
// MotionSource — tags the origin of every intent so the arbiter can select the
// correct limit set and apply the right gating (MANUAL always wins, etc.).
// ============================================================================
enum class MotionSource : uint8_t {
    MANUAL      = 0,   // WebUI rail tap, nudge, slider, move-to point
    TCODE_STREAM = 1,  // TCode L0 commands from any transport (Serial/WS/BLE/Dongle)
    PATTERN      = 2,  // Internal PatternEngine stroke segments
    OSSM_STREAM  = 3   // OSSM BLE streaming position commands
};

// ============================================================================
// MotionIntent — the single entry point to the motion system
// ============================================================================
struct MotionIntent {
    MotionSource source;
    float        target_mm;         // post window-mapping, pre-clamp
    uint32_t     deadline_ms;       // 0 = none (point move: plan at ceilings)
    float        speed_hint_mm_s;   // from S-extension when present, else 0
    // Pattern-derived accel demand (Advanced pattern mode). When BOTH hints are
    // present the planner takes them as the derived dynamics verbatim — the
    // pattern already derived them from its own stroke geometry (D4: derived
    // from what the intent requires) — and the ceiling clamps still apply.
    // 0 = absent: accel is derived from distance + deadline as before.
    float        accel_hint_mm_s2 = 0.0f;
    RampShape    rampIn;            // entry accel multiplier (1.0 = disabled)
    RampShape    rampOut;           // exit accel multiplier (1.0 = disabled)
    uint16_t     seq;               // per-source monotonic, telemetry attribution
};

// ============================================================================
// PlanReport — telemetry from the planner after each dispatch
// ============================================================================
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

// ============================================================================
// MotionArbiter — sole caller of MotorDriver for positioning
// ============================================================================
class MotionArbiter {
public:
    MotionArbiter(SystemState& state, RangeMapper& mapper, MotorDriver& motor);

    // ---- Initialization (call after motor.init()) -----------------------------
    void init();

    // ---- Core 0 → Core 1 deferral (ring buffer queue) -----------------------
    // Pushes the intent into a FreeRTOS queue (non-blocking — drops if full,
    // correct for retarget semantics where the latest command wins at high Hz).
    // Replaces the old single-slot atomic which dropped frames at >100Hz.
    void submitDeferred(const MotionIntent& intent);

    // ---- Core 1 direct dispatch (called from Core 1 tasks only) ---------------
    // PatternEngine, motionConsumerTask, and the deferred-intent consumer call
    // this directly. Plans and dispatches to FAS immediately. All FAS
    // interaction stays on Core 1.
    // Returns the plan report for telemetry.
    PlanReport submit(const MotionIntent& intent);

    // ---- Core 1 deferred-intent consumer --------------------------------------
    // Called periodically from the Core 1 motion task. Checks the atomic
    // deferral slot and applies any pending intent from Core 0.
    void processDeferred();

    // ---- Core 1 stream-sample fast path (MotionInterpolator sampler) ----------
    // Called at ~1kHz by streamSamplerTask with a pre-planned point sampled from
    // the MotionInterpolator's cubic. This is NOT the trapezoid planner — the
    // interpolator already shaped the curve. This path only runs the safety
    // gates (estop/homed/paused/override), maps the normalized position into the
    // stroke window, enforces the hard physical step bounds, and feeds FAS
    // directly via streamToSteps(). Accel is the constant input ceiling (kept
    // constant so the driver grit-cache stays quiet); speed depends on
    // SystemState::stream_speed_mode (ceiling-pegged vs velocity-matched).
    //   norm_pos        : 0..1 position within the configured stroke window
    //   norm_vel_per_s  : signed normalized units/second (VELOCITY_MATCHED only)
    // Returns true if a sample was dispatched, false if gated off.
    bool submitStreamSample(float norm_pos, float norm_vel_per_s);

    // ---- Emergency / gate helpers (Core 0 or Core 1) --------------------------
    void emergencyStop();
    void stopMotion();     // full stop: halts pulse train, cuts power, clears homed (MotorDriver::stop())
    void hardStopMotion(); // immediate stop, motor stays powered (MotorDriver::hardStop())
    void pause();
    void resume();

    // ---- Source gating (Core 1 read, Core 0 write via SystemState) ------------
    // Pause/override flags are read from SystemState on submit().

    // ---- Limit sets — updated by ConfigStore/API ------------------------------
    // USER set: manual moves, UI controls
    void setUserSpeedLimit(float mm_s);
    void setUserAccelLimit(float mm_s2);
    // INPUT set: TCode, PatternEngine, OSSM
    void setInputSpeedLimit(float mm_s);
    void setInputAccelLimit(float mm_s2);

    // ---- Telemetry — last plan report (atomic, any core) ----------------------
    PlanReport lastReport() const;
    uint32_t   totalIntents() const { return _intent_count; }
    // Intents rejected by a gate (not-homed / e-stop / paused / override /
    // Intiface-recency). Surfaced in /api/status so a gated-off stream is
    // distinguishable from "no commands arrived" in the diagnostics. :3
    uint32_t   rejectedIntents() const { return _rejected_count; }

    // ---- Blend/reversal policy (stored, but currently all alias to "allow") ---
    void setBlendMode(uint8_t mode);   // 1=let-it-land 2=allow 3=hybrid
    uint8_t getBlendMode() const { return _blend_mode; }

private:
    SystemState&  _state;
    RangeMapper&  _mapper;
    MotorDriver&  _motor;

    // ---- Limit sets -----------------------------------------------------------
    float _user_speed_limit_mm_s  = DEFAULT_USER_MAX_SPEED_MM_S;   // gentle (50)
    float _user_accel_limit_mm_s2 = DEFAULT_USER_ACCEL_MM_S2;      // gentle (200)
    float _input_speed_limit_mm_s  = DEFAULT_MAX_SPEED_MM_S;
    float _input_accel_limit_mm_s2 = DEFAULT_ACCEL_MM_S2;

    // ---- Dispatch lock (microcritical — protects FAS calls on Core 1) ---------
    mutable portMUX_TYPE _dispatch_mux = portMUX_INITIALIZER_UNLOCKED;

    // ---- Core 0 → Core 1 deferral queue (8 slots, non-blocking push) ---------
    // Handles 333Hz streams: Core 0 pushes intents at 3ms intervals, Core 1
    // drains the entire queue each loop tick. Drop-if-full — at >100Hz the
    // latest intent always arrives within 1 queue depth. Created by init().
    QueueHandle_t     _defer_queue = nullptr;
    static constexpr uint8_t DEFER_QUEUE_DEPTH = 16;

    // ---- Blend policy ---------------------------------------------------------
    uint8_t _blend_mode = 2;   // "allow" — FAS retarget handles reversals

    // ---- Telemetry ------------------------------------------------------------
    PlanReport           _last_report = {};
    volatile uint32_t    _intent_count = 0;
    volatile uint32_t    _rejected_count = 0;
    mutable portMUX_TYPE _telemetry_mux = portMUX_INITIALIZER_UNLOCKED;

    // ---- Per-source sequence counters -----------------------------------------
    uint16_t _seq_manual      = 0;
    uint16_t _seq_tcode       = 0;
    uint16_t _seq_pattern     = 0;
    uint16_t _seq_osssm       = 0;

    // ---- Core planner (the heart — D4) ----------------------------------------
    // Executed under _dispatch_mux on Core 1. Reads actual machine state from
    // FAS, derives the trapezoidal profile, clamps at the source's limit set,
    // dispatches to FAS.
    PlanReport _planAndDispatch(const MotionIntent& intent, bool locked);

    // ---- Gate evaluation ------------------------------------------------------
    // Returns true if the intent should proceed. MANUAL bypasses all gates
    // except E-stop; stream/pattern sources honour homed/paused/override/window.
    bool _gatesPass(const MotionIntent& intent);

    // ---- Window clamping ------------------------------------------------------
    float _clampToWindow(float mm, MotionSource source);

    // ---- Window-entry detection -----------------------------------------------
    // True when p0_mm is currently OUTSIDE the configured stroke window (with a
    // small epsilon). Machine-driven sources (stream/pattern/OSSM) honor the
    // gentle USER limits on the move that carries the carriage from outside the
    // window into it, so it glides in instead of lunging to the edge at the
    // input ceiling. Once inside, the normal INPUT set resumes. :3
    bool _isOutsideWindow(float p0_mm) const;
};

// ============================================================================
// Sole-caller enforcement
// ============================================================================
// MotorDriver declares MotionArbiter as friend in MotorDriver.h:
//   friend class MotionArbiter;
// MotorDriver motion methods become protected/private. The lock is compile-time.