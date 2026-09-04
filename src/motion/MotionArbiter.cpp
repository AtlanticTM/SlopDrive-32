// MotionArbiter -- the command gate; arbitrates MANUAL / PATTERN / stream
// sources into ONE forwarded LinkCommand per accepted intent.
// Constraints:
//   submit(), submitSegment() and processDeferred() run on Core 1 only;
//   submitDeferred()/submitSegmentDeferred() are the Core 0 handoff.
//   E-stop and the hard physical envelope are absolute: no source, MANUAL
//   included, may bypass them. MANUAL bypasses only the stroke WINDOW.
//   Never write actual_position_mm from here -- the telemetry sampler owns
//   that atomic; the arbiter publishes INTENT only.
// See: include/motion/MotionArbiter.h, docs/rp-motion-port.md.
#include "MotionArbiter.h"
#include "range_mapper.h"
#include "EngineConfigMap.h"
#include "sloplog/sloplog.h"
#include "config_api.h"
#include <esp_timer.h>
#include <math.h>

using namespace motionlink;

// ---- Dispatch guard ---------------------------------------------------------
// Bounded RAII take. Rationale and what it deliberately does NOT cover:
// MotionArbiter.h, _dispatch_lock. 2 ms is ~1000x the expected hold.
namespace {
constexpr TickType_t kDispatchLockTicks = pdMS_TO_TICKS(2);

class DispatchGuard {
public:
    explicit DispatchGuard(SemaphoreHandle_t h)
        : _h(h), _held(h != nullptr && xSemaphoreTake(h, kDispatchLockTicks) == pdTRUE) {}
    ~DispatchGuard() { if (_held) xSemaphoreGive(_h); }
    DispatchGuard(const DispatchGuard&) = delete;
    DispatchGuard& operator=(const DispatchGuard&) = delete;
    // Safe to touch the driver: holding the lock, or there is no lock to hold
    // (init() never ran -- a boot-order bug, not a reason to refuse motion).
    bool mayDispatch() const { return _held || _h == nullptr; }

private:
    SemaphoreHandle_t _h;
    bool              _held;
};
}  // namespace

MotionArbiter::MotionArbiter(SystemState& state, RangeMapper& mapper, MotorDriver& motor)
    : _state(state), _mapper(mapper), _motor(motor)
{}

void MotionArbiter::init() {
    _defer_queue = xQueueCreate(DEFER_QUEUE_DEPTH, sizeof(MotionIntent));
    configASSERT(_defer_queue != nullptr);
    _segment_queue = xQueueCreate(SEGMENT_QUEUE_DEPTH, sizeof(SegmentIntent));
    configASSERT(_segment_queue != nullptr);
    _dispatch_lock = xSemaphoreCreateMutex();   // priority-inheriting, task level
    configASSERT(_dispatch_lock != nullptr);
    SLOGI("arbiter", "MotionArbiter: command gate up, %u-slot point + %u-slot "
          "segment defer queues", DEFER_QUEUE_DEPTH, SEGMENT_QUEUE_DEPTH);
}

// ---- Core 0 -> Core 1 handoff -----------------------------------------------

void MotionArbiter::setConsumerTask(TaskHandle_t t) {
    _consumer.store(t, std::memory_order_release);
}

// Wakes the consumer. A notification that arrives while it is already running
// is not lost: the count survives to the next take, so the queue is drained
// again rather than one command late.
void MotionArbiter::_wakeConsumer() {
    TaskHandle_t t = _consumer.load(std::memory_order_acquire);
    if (t != nullptr) xTaskNotifyGive(t);
}

void MotionArbiter::submitDeferred(const MotionIntent& intent) {
    if (xQueueSend(_defer_queue, &intent, 0) != pdTRUE) {
        SLOGW_EVERY_MS(2000, "arbiter",
                       "DROP: point defer queue full -- Core 1 consumer stalled");
        return;
    }
    _wakeConsumer();
}

void MotionArbiter::submitSegmentDeferred(const SegmentIntent& seg) {
    if (xQueueSend(_segment_queue, &seg, 0) != pdTRUE) {
        SLOGW_EVERY_MS(2000, "arbiter",
                       "DROP: segment defer queue full -- Core 1 consumer stalled");
        return;
    }
    _wakeConsumer();
}

void MotionArbiter::processDeferred() {
    // Drain both queues in full on every wake. Segments first: a stream carries
    // anchors and a point move does not, so a segment held one tick behind a
    // burst of point moves is the one that renders late.
    SegmentIntent seg;
    while (xQueueReceive(_segment_queue, &seg, 0) == pdTRUE) submitSegment(seg);

    MotionIntent intent;
    uint8_t drained = 0;
    while (xQueueReceive(_defer_queue, &intent, 0) == pdTRUE) {
        submit(intent);
        drained++;
    }
    static uint8_t peak_drain = 0;
    if (drained > peak_drain) {
        peak_drain = drained;
        SLOGD("arbiter", "new point defer-drain peak %u/tick (total=%lu)",
              (unsigned)peak_drain, _intent_count);
    }

    const uint32_t now = millis();
    if (now - _policy_ms >= kPolicyPushMs) {
        _policy_ms = now;
        _pushPolicy();
    }
}

// ---- Policy push ------------------------------------------------------------
// The slave holds BOTH ceiling sets, the window, the gate bits and the engine
// tuning, because it must be self-sufficient for the maneuvers it starts on
// its own (MotionLinkProtocol.h, ConfigTag SCOPE RULE). The S3 still owns the
// CHOICE, which rides each command's limit_set byte.

void MotionArbiter::_pushPolicy() {
    Window win = _mapper.effectiveWindow();
    if (!_state.homed) {
        // Before zeroing, the persisted window is a number in a frame that
        // does not exist yet, and the engine clamps every target to it: a
        // homing sweep stops at its edge and the rear wall is unreachable
        // (first live homing on the port, 2026-09-03). Unhomed, the window is
        // the whole rail both ways around wherever the carriage booted; the
        // real one ships on the tick after `homed` rises.
        // Centered on where the carriage IS when the link first answers, not
        // on the processor's count zero: after a failed ritual the counter can
        // sit hundreds of mm from the rail, and an S3 reboot does not reset
        // it. Latched per boot so the window never moves under a sweep.
        static bool  s_centered = false;
        static float s_center_mm = 0.0f;
        if (!s_centered && _motor.isLinkUp()) {
            s_centered = true;
            s_center_mm = _motor.getPosition();
        }
        const float r = _mapper.getMaxRailMm();
        win.min_mm = s_center_mm - 1.5f * r;
        win.max_mm = s_center_mm + 1.5f * r;
    }
    const float span_mm = win.max_mm - win.min_mm;

    slopdrive::EngineTuning tune;
    tune.span_mm         = span_mm;
    tune.input_max_speed = _input_speed_limit_mm_s;
    tune.input_max_accel = _input_accel_limit_mm_s2;
    tune.input_max_jerk  = _state.config.input_max_jerk_mm_s3;
    tune.user_max_speed  = _user_speed_limit_mm_s;
    tune.user_max_accel  = _user_accel_limit_mm_s2;
    tune.vmax_ovr        = _state.sm_tune_vmax_ovr;
    tune.amax_ovr        = _state.sm_tune_amax_ovr;
    tune.jmax_ovr        = _state.sm_tune_jmax_ovr;
    tune.chase_ff        = _state.sm_tune_chase_ff;
    tune.chase_aff       = _state.sm_tune_chase_aff;
    tune.aim_extrap      = _state.sm_tune_aim_extrap;
    tune.chase_gain      = _state.sm_tune_chase_gain;
    tune.chase_look      = _state.sm_tune_chase_look;
    tune.dense_us        = _state.sm_tune_dense_us;
    tune.infeas_policy   = _state.sm_tune_infeas_policy;
    tune.infeas_blend    = _state.sm_tune_infeas_blend;
    tune.smooth_budget   = _state.sm_tune_smooth_budget;
    tune.amp_budget      = _state.sm_tune_amp_budget;
    tune.blend_steps     = _state.sm_tune_blend_steps;
    tune.curve_policy    = _state.sm_tune_curve_policy;
    tune.handoff_k       = _state.sm_tune_handoff_k;
    tune.settle_grace_us = _state.sm_tune_settle_grace_us;

    // A stored ordinal from a policy deleted 2026-09-02 runs as Blend
    // (EngineConfigMap.h). Said ONCE: a silent remap is the kind of thing
    // that gets rediscovered on hardware.
    static bool s_retired_said = false;
    if (!s_retired_said && tune.infeas_policy >= 2) {
        s_retired_said = true;
        SLOGW("arbiter", "infeasible_policy %u is retired; running blend",
              (unsigned)tune.infeas_policy);
    }

    for (const ConfigField& f : slopdrive::buildConfigTags(tune))
        _motor.pushConfig(f.tag, f.raw);

    const slopdrive::NormalizedLimits lim = slopdrive::normalizedLimits(tune);
    _state.sm_eff_vmax = lim.vmax;
    _state.sm_eff_amax = lim.amax;
    _state.sm_eff_jmax = lim.jmax;

    // Window in the slave's native counts. The native frame is NEGATED vs mm
    // (endstop 0, front negative), so "min" is the count normalized 0 maps to
    // and "max" the one normalized 1 maps to -- the wire's own definition,
    // and the pair is monotone in normalized units either way.
    _motor.pushConfig(kCfgWindowMinCounts,
                      f32Bits(float(-_motor.mmToNative(win.min_mm))));
    _motor.pushConfig(kCfgWindowMaxCounts,
                      f32Bits(float(-_motor.mmToNative(win.max_mm))));

    // Gates. ONE tag: the slave enforces ONE predicate and the bits exist so
    // telemetry can name WHICH gate is closed. E-stop is NOT here -- it keeps
    // kOpEstop so it punches through in one 50 us tick.
    uint32_t gates = 0;
    if (_state.homed) gates |= kGateHomed;
    if (_state.paused || _state.manual_override) gates |= kGatePaused;

    // Soft start stays S3 POLICY: the ramp runs here, the slave applies the
    // value as a transient min() over the selected set's vmax. SPEED ONLY --
    // soft accel cannot reach the already-soft speed cap, which renders as
    // freeze-then-jump. 0 = no cap.
    float soft_cap = 0.0f;
    if (span_mm > 1.0f) {
        const float cap_mm_s = _state.safeSpeedCap(_input_speed_limit_mm_s, millis());
        if (cap_mm_s < _input_speed_limit_mm_s) {
            soft_cap = cap_mm_s / span_mm;
            gates |= kGateSoftStart;
        }
    }
    _motor.pushConfig(kCfgSoftStartCap, f32Bits(soft_cap));
    _motor.pushConfig(kCfgGates, gates);
}

// ---- Gates ------------------------------------------------------------------

bool MotionArbiter::_absoluteGatesPass(MotionSource source) {
    // E-stop is the one gate no source bypasses. If the red button was
    // slapped, nothing moves.
    if (_state.estop_requested.load(std::memory_order_relaxed)) {
        _rejected_count = _rejected_count + 1;
        SLOGW_EVERY_MS(2000, "arbiter", "REJECT: e-stop (src=%u)", (unsigned)source);
        return false;
    }
    if (!_state.homed && source != MotionSource::MANUAL) {
        // Manual moves bypass the homed check (push-to-home scenario).
        _rejected_count = _rejected_count + 1;
        SLOGW_EVERY_MS(2000, "arbiter", "REJECT: not-homed (src=%u)", (unsigned)source);
        return false;
    }
    if (!_gatesPass(source)) {
        _rejected_count = _rejected_count + 1;
        SLOGW_EVERY_MS(2000, "arbiter",
                       "REJECT: gates (paused/override/intiface-recency) (src=%u)",
                       (unsigned)source);
        return false;
    }
    return true;
}

bool MotionArbiter::_gatesPass(MotionSource source) {
    if (source == MotionSource::MANUAL) return true;
    if (!_state.homed) return false;
    if (_state.paused) return false;
    if (_state.manual_override) return false;

    // Intiface recency: yield on stream MOTION, not packets, so a keep-alive
    // -only host cannot pin the pattern off.
    if (source == MotionSource::PATTERN) {
        const bool intiface_driving = (_state.last_intiface_move_ms != 0) &&
                                      (millis() - _state.last_intiface_move_ms < 1500);
        if (intiface_driving) return false;
    }
    return true;
}

// ---- Window -----------------------------------------------------------------

float MotionArbiter::_clampToWindow(float mm, MotionSource source) {
    if (source == MotionSource::MANUAL) {
        // Manual moves reach anywhere inside the effective physical ceiling
        // (measured stroke once homed, else the configured max rail).
        return constrain(mm, 0.0f, _motor.effectiveCeilingMm());
    }
    // The pair is read as ONE value: half of an in-flight window edit would
    // clamp against a min and a max that never coexisted (sd-tki.5).
    const Window win = _mapper.effectiveWindow();
    return constrain(mm, win.min_mm, win.max_mm);
}

bool MotionArbiter::_isOutsideWindow(float p0_mm) const {
    const float eps = 0.5f;   // sub-safety-zone slack, avoids edge chatter
    const Window win = _mapper.effectiveWindow();
    return (p0_mm < win.min_mm - eps) || (p0_mm > win.max_mm + eps);
}

// ---- The one door out -------------------------------------------------------

bool MotionArbiter::_dispatchCommand(LinkCommand& cmd, MotionSource source) {
    // Source-to-set selection. It rides the COMMAND rather than config so a
    // manual jog and a stream point can be in flight together without an
    // ordering dependency on which config push landed last.
    cmd.limit_set = (source == MotionSource::MANUAL) ? kLimitUser : kLimitInput;
    cmd.axis = 0;

    DispatchGuard guard(_dispatch_lock);
    if (!guard.mayDispatch()) {
        _rejected_count = _rejected_count + 1;
        SLOGW_EVERY_MS(2000, "arbiter",
                       "DROP: dispatch lock timeout (src=%u): a Core-1 motion "
                       "task is wedged", (unsigned)source);
        return false;
    }
    _motor.sendCommand(cmd);
    return true;
}

// ---- submitSegment ----------------------------------------------------------
// A waveform span already in normalized window units: the gates, the clamp
// and the limit set are the whole of the work here.

bool MotionArbiter::submitSegment(const SegmentIntent& seg) {
    const uint32_t start_us = micros();
    if (!_absoluteGatesPass(seg.source)) return false;

    // Clamp on the NORMALIZED target through the window, in mm, so one clamp
    // rule serves both entry points.
    const Window win = _mapper.effectiveWindow();
    const float span_mm = win.max_mm - win.min_mm;
    float target_mm = win.min_mm + seg.target * span_mm;
    target_mm = _clampToWindow(target_mm, seg.source);
    const float norm = (span_mm > 0.01f) ? (target_mm - win.min_mm) / span_mm : 0.5f;

    LinkCommand cmd;
    cmd.kind = seg.duration_us > 0 ? kCmdWaveform : kCmdPoint;
    cmd.curve_family = seg.curve_family;
    cmd.target = norm;
    cmd.duration_us = seg.duration_us;
    cmd.end_vel = seg.end_vel;
    cmd.next_chord = seg.next_chord;
    cmd.anchor_us = seg.anchor_us;
    if (seg.has_end_vel)    cmd.flags |= kCmdHasEndVel;
    if (seg.has_next_chord) cmd.flags |= kCmdHasNextChord;
    if (seg.has_anchor)     cmd.flags |= kCmdHasAnchor;

    if (!_dispatchCommand(cmd, seg.source)) return false;

    PlanReport report = {};
    const bool manual = seg.source == MotionSource::MANUAL;
    report.clamped_speed_mm_s  = manual ? _user_speed_limit_mm_s : _input_speed_limit_mm_s;
    report.clamped_accel_mm_s2 = manual ? _user_accel_limit_mm_s2 : _input_accel_limit_mm_s2;
    report.derived_speed_mm_s  = report.clamped_speed_mm_s;
    report.derived_accel_mm_s2 = report.clamped_accel_mm_s2;
    report.dispatched_steps    = -_motor.mmToNative(target_mm);
    report.deadline_feasible   = true;
    report.plan_us             = micros() - start_us;

    portENTER_CRITICAL(&_telemetry_mux);
    _last_report = report;
    _intent_count++;
    portEXIT_CRITICAL(&_telemetry_mux);

    _state.commanded_target_mm = target_mm;
    return true;
}

// ---- submit -----------------------------------------------------------------
// A point move in millimeters. deadline_ms > 0 becomes a waveform span that
// ARRIVES AT REST (has_end_vel true with end_vel 0, which is a different
// command from the no-handoff sentinel); no deadline is a bare point that the
// slave plans at the selected set's ceilings.

PlanReport MotionArbiter::submit(const MotionIntent& intent) {
    const uint32_t start_us = micros();
    PlanReport report = {};
    if (!_absoluteGatesPass(intent.source)) return report;

    float target_mm = _clampToWindow(intent.target_mm, intent.source);
    if (intent.source != MotionSource::MANUAL && target_mm != intent.target_mm) {
        const Window w = _mapper.effectiveWindow();
        SLOGW_EVERY_MS(2000, "arbiter",
                       "WINDOW CLAMP: intent=%.1f -> %.1f window=[%.1f,%.1f] src=%u",
                       intent.target_mm, target_mm, w.min_mm, w.max_mm,
                       (unsigned)intent.source);
    }

    // Ceilings in force, for the report only: the slave derives the profile.
    const bool manual = intent.source == MotionSource::MANUAL;
    float speed_ceiling = manual ? _user_speed_limit_mm_s : _input_speed_limit_mm_s;
    const float accel_ceiling = manual ? _user_accel_limit_mm_s2 : _input_accel_limit_mm_s2;
    // Outside the window a machine-driven source glides in at the gentle USER
    // speed. SPEED ONLY -- this test is keyed on POSITION ALONE, so it cannot
    // tell "parked outside at rest" from "just overshot and must stop now",
    // and removing accel authority in the second case is what turns an
    // overshoot into a runaway (54-config bench sweep: 11/54 -> 0/54).
    if (!manual && _isOutsideWindow(_motor.getPosition()))
        speed_ceiling = fminf(speed_ceiling, _user_speed_limit_mm_s);

    const Window win = _mapper.effectiveWindow();
    const float span_mm = win.max_mm - win.min_mm;
    // MANUAL reaches outside the window on purpose, so its normalized target
    // legitimately leaves 0..1. That is a real position outside the window,
    // never clamped on the wire (docs/rp-motion-port.md, Units).
    const float norm = (span_mm > 0.01f) ? (target_mm - win.min_mm) / span_mm : 0.5f;

    LinkCommand cmd;
    cmd.target = norm;
    cmd.curve_family = 0;
    if (intent.deadline_ms > 0) {
        cmd.kind = kCmdWaveform;
        cmd.duration_us = intent.deadline_ms * 1000u;
        cmd.end_vel = 0.0f;
        cmd.flags |= kCmdHasEndVel;   // explicit arrive-at-rest, not the sentinel
    } else {
        cmd.kind = kCmdPoint;
    }
    // Point moves carry no anchor: they are due on arrival by definition.

    if (!_dispatchCommand(cmd, intent.source)) {
        report.deadline_feasible = false;
        report.deadline_late     = true;
        report.plan_us           = micros() - start_us;
        return report;
    }

    report.derived_speed_mm_s  = intent.speed_hint_mm_s > 0.0f
                                     ? intent.speed_hint_mm_s : speed_ceiling;
    report.derived_accel_mm_s2 = intent.accel_hint_mm_s2 > 0.0f
                                     ? intent.accel_hint_mm_s2 : accel_ceiling;
    report.clamped_speed_mm_s  = fminf(report.derived_speed_mm_s, speed_ceiling);
    report.clamped_accel_mm_s2 = fminf(report.derived_accel_mm_s2, accel_ceiling);
    report.deadline_feasible   = report.clamped_speed_mm_s >= report.derived_speed_mm_s &&
                                 report.clamped_accel_mm_s2 >= report.derived_accel_mm_s2;
    report.deadline_late       = !report.deadline_feasible;
    report.dispatched_steps    = -_motor.mmToNative(target_mm);
    report.plan_us             = micros() - start_us;

    portENTER_CRITICAL(&_telemetry_mux);
    _last_report = report;
    _intent_count++;
    portEXIT_CRITICAL(&_telemetry_mux);

    _state.commanded_target_mm = target_mm;
    return report;
}

// ---- Emergency / gate helpers -----------------------------------------------

void MotionArbiter::emergencyStop() {
    _motor.emergencyStop();
    // Drain both defer queues so no stale intent fires after e-stop.
    MotionIntent dummy;
    while (xQueueReceive(_defer_queue, &dummy, 0) == pdTRUE) {}
    SegmentIntent seg;
    while (xQueueReceive(_segment_queue, &seg, 0) == pdTRUE) {}
}

void MotionArbiter::stopMotion() {
    // Full stop with power cut. Deliberately DIFFERENT from hardStopMotion(),
    // which halts but keeps the motor powered and homed.
    _motor.stop();
}

void MotionArbiter::hardStopMotion() {
    _motor.hardStop();
}

void MotionArbiter::pause() {
    _state.paused = true;
}

void MotionArbiter::resume() {
    _state.paused = false;
    _state.resume_start_ms = millis();  // stamp for safeSpeedCap soft-start
}

// ---- Limit set setters ------------------------------------------------------
// Storage only; _pushPolicy() ships the change on the next Core-1 tick, which
// keeps the wire off whatever task edited a setting.

void MotionArbiter::setUserSpeedLimit(float mm_s) {
    _user_speed_limit_mm_s = constrain(mm_s, 1.0f, MAX_SPEED_MM_S);
}

void MotionArbiter::setUserAccelLimit(float mm_s2) {
    _user_accel_limit_mm_s2 = constrain(mm_s2, 10.0f, MAX_ACCEL_MM_S2);
}

void MotionArbiter::setInputSpeedLimit(float mm_s) {
    _input_speed_limit_mm_s = constrain(mm_s, 1.0f, MAX_SPEED_MM_S);
}

void MotionArbiter::setInputAccelLimit(float mm_s2) {
    _input_accel_limit_mm_s2 = constrain(mm_s2, 10.0f, MAX_ACCEL_MM_S2);
}

// ---- Telemetry --------------------------------------------------------------

PlanReport MotionArbiter::lastReport() const {
    PlanReport rpt;
    portENTER_CRITICAL(&_telemetry_mux);
    rpt = _last_report;
    portEXIT_CRITICAL(&_telemetry_mux);
    return rpt;
}
