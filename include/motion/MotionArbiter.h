#pragma once

// MotionArbiter -- the COMMAND GATE and sole caller of MotorDriver.
//
// Constraints:
// - The RP2350 holds the plan (docs/rp-motion-port.md). This class does not
//   plan: it arbitrates sources, runs every safety gate, clamps to the stroke
//   window, picks the ceiling SET, and forwards ONE LinkCommand per accepted
//   intent. Ceilings, window, gates, soft-start value and engine tuning go
//   out as kOpConfig tags, on change.
// - ONE DOOR OUT: every accepted intent leaves through _dispatchCommand().
//   A denied intent is counted in rejectedIntents() and never reaches the
//   driver.
// - Commands are forwarded ON ARRIVAL, never on a tick (architecture.md
//   section 2). processDeferred() exists only to cross the core boundary.
// - submit()/submitSegment() run on Core 1 ONLY; Core 0 enqueues through
//   submitDeferred()/submitSegmentDeferred(), drained in arrival order by
//   processDeferred() on motorTask.
// - Anchors leave here in S3 esp_timer microseconds; the DRIVER converts to
//   slave time, because it owns the clock estimate.

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "MotorDriver.h"
#include "SystemState.h"
#include "config_api.h"
#include "MotionLinkProtocol.h"

class RangeMapper;

// ---- MotionSource -----------------------------------------------------------
// Origin of an intent: picks the limit set and the gating.
enum class MotionSource : uint8_t {
    MANUAL       = 0,  // WebUI rail tap, nudge, slider, move-to point
    TCODE_STREAM = 1,  // TCode L0 from any transport
    PATTERN      = 2,  // internal PatternEngine stroke segments
    OSSM_STREAM  = 3   // OSSM BLE streaming position commands
};

// ---- MotionIntent -----------------------------------------------------------
// A POINT MOVE in machine millimeters: manual taps, pattern strokes, TCode.
struct MotionIntent {
    MotionSource source;
    float        target_mm;         // post window-mapping, pre-clamp
    uint32_t     deadline_ms;       // 0 = none: kCmdPoint at the set's ceilings
    float        speed_hint_mm_s;
    float        accel_hint_mm_s2 = 0.0f;
    uint16_t     seq;               // per-source monotonic, telemetry attribution
};

// ---- SegmentIntent ----------------------------------------------------------
// A WAVEFORM SPAN in normalized window units, what the SlopSync motion-stream
// channels carry. Fields map one for one onto motionlink::LinkCommand.
struct SegmentIntent {
    MotionSource source = MotionSource::TCODE_STREAM;
    float    target = 0.5f;         // normalized 0..1 over the stroke window
    uint32_t duration_us = 0;       // 0 = bare point, rendered as kCmdPoint
    float    end_vel = 0.0f;        // normalized units/s
    // FALSE is the sentinel "no handoff given"; TRUE with 0 is arrive-at-rest.
    bool     has_end_vel = false;
    float    next_chord = 0.0f;     // magnitude, normalized units/s
    bool     has_next_chord = false;
    uint32_t anchor_us = 0;         // S3 esp_timer microseconds
    bool     has_anchor = false;
    uint8_t  curve_family = 0;      // RFC-030 declared family
};

// ---- PlanReport -------------------------------------------------------------
// What the last accepted command runs UNDER. The derivation lives on the
// slave, so this reports ceilings and the dispatched target, never a profile.
struct PlanReport {
    float    derived_speed_mm_s;
    float    derived_accel_mm_s2;
    float    clamped_speed_mm_s;   // selected set's speed, post soft-start
    float    clamped_accel_mm_s2;  // selected set's accel
    int32_t  dispatched_steps;
    bool     deadline_feasible;
    bool     deadline_late;
    uint32_t plan_us;              // microseconds spent in the gate
};

// ---- MotionArbiter ----------------------------------------------------------
class MotionArbiter {
public:
    MotionArbiter(SystemState& state, RangeMapper& mapper, MotorDriver& motor);

    void init();

    // ---- Core 0 -> Core 1 deferral ------------------------------------------
    // Non-blocking, drops if full: the latest command wins at high Hz.
    void submitDeferred(const MotionIntent& intent);
    void submitSegmentDeferred(const SegmentIntent& seg);

    // ---- Core 1 direct dispatch ---------------------------------------------
    PlanReport submit(const MotionIntent& intent);
    bool submitSegment(const SegmentIntent& seg);

    // ---- Core 1 deferred consumer plus policy push --------------------------
    // Every motorTask tick: drains both queues in arrival order, then
    // refreshes pushed policy. The driver's change detection turns that into
    // one frame per actual change.
    void processDeferred();

    // ---- Emergency / gate helpers (either core) -----------------------------
    void emergencyStop();
    void stopMotion();     // cuts power, clears homed (MotorDriver::stop())
    void hardStopMotion(); // immediate stop, motor stays powered
    void pause();
    void resume();

    // ---- Limit sets, updated by ConfigStore/API -----------------------------
    // USER set: manual moves, UI controls, and every RP-initiated maneuver
    // (settle, recovery, window entry), which are recovery and never content.
    void setUserSpeedLimit(float mm_s);
    void setUserAccelLimit(float mm_s2);
    // INPUT set: TCode, PatternEngine, OSSM.
    void setInputSpeedLimit(float mm_s);
    void setInputAccelLimit(float mm_s2);

    // ---- Telemetry ----------------------------------------------------------
    PlanReport lastReport() const;
    uint32_t   totalIntents() const { return _intent_count; }
    // Denied by a gate. Distinguishes a gated-off stream from silence.
    uint32_t   rejectedIntents() const { return _rejected_count; }

private:
    SystemState&  _state;
    RangeMapper&  _mapper;
    MotorDriver&  _motor;

    float _user_speed_limit_mm_s  = DEFAULT_USER_MAX_SPEED_MM_S;
    float _user_accel_limit_mm_s2 = DEFAULT_USER_ACCEL_MM_S2;
    float _input_speed_limit_mm_s  = DEFAULT_MAX_SPEED_MM_S;
    float _input_accel_limit_mm_s2 = DEFAULT_ACCEL_MM_S2;

    // ---- Driver dispatch lock (Core 1, task level) --------------------------
    // Serializes the DRIVER CALL only. processDeferred() dispatches from
    // motorTask while PatternEngine dispatches from its own Core-1 task, and
    // the driver's cross-task post ring is single-producer by contract.
    // NOT a portMUX critical section: dispatch reaches the wire path, which
    // suspends the scheduler for a ~40 us frame; interrupts off across that
    // breaks the slave's 20 kHz link (T27 class).
    // Bounded take: a timeout means a real-time task is already wedged, so
    // count and drop rather than stall one.
    // NOT taken by the stop paths: a stop never waits on a lock.
    // TODO(sd-tki.4): pending the operator ruling cpp-safety.md requires.
    SemaphoreHandle_t _dispatch_lock = nullptr;

    QueueHandle_t     _defer_queue = nullptr;
    QueueHandle_t     _segment_queue = nullptr;
    static constexpr uint8_t DEFER_QUEUE_DEPTH = 16;
    static constexpr uint8_t SEGMENT_QUEUE_DEPTH = 16;

    PlanReport           _last_report = {};
    volatile uint32_t    _intent_count = 0;
    volatile uint32_t    _rejected_count = 0;
    mutable portMUX_TYPE _telemetry_mux = portMUX_INITIALIZER_UNLOCKED;

    // Policy refresh cadence. The window glide is the only fast-moving input
    // and the driver drops unchanged tags, so this costs compares, not frames.
    static constexpr uint32_t kPolicyPushMs = 20;
    uint32_t _policy_ms = 0;

    // The one door out: picks the ceiling set, takes the lock, forwards.
    // False = lock timeout (counted, never awaited).
    bool _dispatchCommand(motionlink::LinkCommand& cmd, MotionSource source);

    void _pushPolicy();

    // MANUAL bypasses every source gate except e-stop.
    bool _gatesPass(MotionSource source);
    // Absolute gates, e-stop included; counts its own rejection.
    bool _absoluteGatesPass(MotionSource source);

    float _clampToWindow(float mm, MotionSource source);

    // Outside the window a machine-driven source honors the gentle USER
    // speed so it glides in instead of lunging to the edge.
    bool _isOutsideWindow(float p0_mm) const;
};
