#pragma once

// ============================================================================
// SlopMotion — jerk-limited dual-mode motion core (Ruckig-powered)
// ============================================================================
//
// PURPOSE
// -------
// The successor to MotionInterpolator's cubic Hermite engine: every incoming
// motion command becomes ONE Ruckig jerk-limited trajectory planned from the
// engine's ACTUAL current kinematic state (position + velocity + acceleration),
// and the existing ~1 kHz stream sampler simply evaluates that trajectory.
// Event-driven, never clocked — a plan is computed only when a command arrives
// (or when a stream starves mid-glide, see SETTLE below). This is the D4
// doctrine with a mathematically optimal planner in the plan step.
//
// THE TWO MODES (chosen per-command by what the command can know)
// ---------------------------------------------------------------
//   * ONE-SHOT (TCode v4 / any point carrying I<ms> interval data):
//     a complete move exists → plan the time-optimal jerk-limited profile and
//     STRETCH it to the commanded duration via Ruckig's minimum_duration.
//     If the deadline is physically infeasible under the limits, the profile
//     takes its physical minimum time instead (the deadline_late analog —
//     recorded as a DeadlineStretched anomaly).
//   * CHASE (TCode v3 bare high-rate points, no timing):
//     the future is unknown → chase the newest target under v/a/j ceilings.
//     Retarget-while-moving is the NORMAL case: each point replans from the
//     current sampled (p,v,a), so the chain is C2-continuous — no boundary
//     kink, no microstutter, the ossm-rs liquid feel. An optional feedforward
//     estimator differentiates the incoming point stream and hands Ruckig a
//     nonzero TARGET velocity so dense streams glide THROUGH points instead
//     of planning a decel-to-rest at every one.
//
// SETTLE (stream starvation — replaces the cubic's DecelOverrun machinery)
// ------------------------------------------------------------------------
// A trajectory that ends with nonzero end-velocity (G-slope handoff or chase
// feedforward) normally never reaches its end — the next point replans first.
// If the stream dies and the clock DOES run past the end, the engine plans a
// jerk-limited brake-to-rest from the end state using Ruckig's velocity
// control interface (target velocity 0), then holds the landing position.
// This is a one-time boundary event, not a clock loop.
//
// SAFETY / BOUNDS
// ---------------
// Ruckig Community has NO position limits, so the engine guards the window:
//   * command targets are clamped to [0,1] at commit;
//   * requested end velocities are clamped so the machine can brake before
//     the wall beyond the target (|vf|² ≤ amax·dist_to_wall, trapezoid bound
//     — conservative for a jerk-limited brake at our jmax/amax ratios, and
//     the sampler clamp below is the hard backstop), and to ≤ vmax (Ruckig
//     rejects target velocities above the ceiling as invalid input);
//   * sampled OUTPUT position is clamped to [0,1] — a transient excursion
//     (hot entry velocity braking past a wall-adjacent target) flattens at
//     the wall exactly like the cubic engine did. Downstream, the arbiter's
//     window clamp remains the hard physical backstop.
// Limits are CEILINGS handed to Ruckig, never targets — Ruckig derives the
// profile the move requires within them (the doctrine, verbatim).
//
// UNITS & TIME
// ------------
//   position : normalized 0..1 across the configured stroke window
//   velocity : normalized units per second (TCode G wire value / 1000)
//   time     : microseconds, uint64, injected by the caller (esp_timer on
//              target, synthetic in native tests — fully deterministic, the
//              slopsync conformance discipline)
// Public API is float (project style / S3 FPU); Ruckig computes in double
// internally. Planning is per-event so the S3's software-double cost is paid
// at plan time, never per 1 kHz sample (at_time evaluation is also double —
// measured on-target before firmware integration, see roadmap part 2).
//
// THREADING
// ---------
// Single-threaded by contract, same as MotionInterpolator: ALL of
// commit/positionAt/velocityAt/snapshot run on Core 1 (the sampler task).
// Cross-core command handoff stays OUTSIDE this class (FreeRTOS queue in the
// composition root). No locks in the eval path, no heap in steady state
// (Ruckig's waypoint vectors stay empty in community mode).
//
// Hardware-free: std headers + vendored lib/ruckig only. Native-tested in
// test/native/test_slopmotion. :3

#include <cstdint>
#include <cmath>
#include <optional>

#include <ruckig/ruckig.hpp>

namespace slopmotion {

inline constexpr const char* kVersion = "0.1.0";

// ---- Configuration ----------------------------------------------------------

// Kinematic ceilings in normalized units (per second^n). The firmware glue
// derives these from the mm-domain input limit set / stroke window length.
struct Limits {
    float vmax = 3.0f;     // units/s   (3 = three full strokes per second)
    float amax = 30.0f;    // units/s^2
    float jmax = 500.0f;   // units/s^3
};

struct Config {
    Limits limits{};

    // CHASE feedforward: finite-difference the incoming point stream and pass
    // a damped estimate as Ruckig's target velocity. 0 gain = every point
    // plans to rest (choppy at low rates); 1 = full raw estimate (overshoots
    // on noisy streams). The native scenario graphs are the tuning tool. :3
    bool     chase_feedforward = true;
    float    chase_ff_gain     = 0.7f;
    // Estimator resets after a stream gap this long (a fresh stream's first
    // point must not inherit a stale velocity estimate).
    uint32_t chase_stale_us    = 400000;
};

// ---- Command (POD, queue-safe — the InterpSegment successor) ---------------
struct Command {
    float    target       = 0.5f;   // normalized 0..1
    uint32_t duration_us  = 0;      // I<ms> * 1000 when present
    float    end_vel      = 0.0f;   // units/s — TCode G wire value / 1000
    bool     has_end_vel  = false;  // true → v4 gradient handoff velocity
    bool     has_duration = false;  // true → ONE-SHOT; false → CHASE
};

enum class Mode : uint8_t {
    Idle    = 0,   // holding position, no planned motion
    OneShot = 1,   // executing a deadline-stretched v4 profile
    Chase   = 2,   // tracking the newest bare point
    Settle  = 3    // braking to rest after stream starvation
};

// ---- Anomaly instrumentation (same drain pattern as the cubic engine) ------
enum class AnomalyType : uint8_t {
    None              = 0,
    PlanFailed        = 1,  // Ruckig returned an error; previous plan kept. detail = (float)Result
    SettleEngaged     = 2,  // stream starved mid-glide → brake plan. detail = end velocity
    EndVelClamped     = 3,  // requested vf cut by wall/vmax guard. detail = clamped vf
    DeadlineStretched = 4   // commanded duration infeasible; profile runs longer. detail = actual s
};

struct Anomaly {
    uint8_t  kind   = 0;      // AnomalyType
    uint16_t seq    = 0;      // rolling event id
    uint64_t t_us   = 0;      // engine time at record
    float    target = 0.0f;   // command target 0..1
    float    detail = 0.0f;   // kind-specific (see AnomalyType)
};

// ---- Telemetry snapshot (WebUI planned-path overlay feed) ------------------
struct Snapshot {
    float    pos        = 0.5f;
    float    vel        = 0.0f;   // units/s
    float    acc        = 0.0f;   // units/s^2
    float    target     = 0.5f;   // active trajectory's end position
    float    duration_s = 0.0f;   // active trajectory duration (0 = holding)
    float    elapsed_s  = 0.0f;
    uint8_t  mode       = 0;      // Mode
    uint32_t plans      = 0;      // successful plans since reset
    uint32_t failures   = 0;      // PlanFailed count since reset
};

// ============================================================================
// Engine
// ============================================================================
class Engine {
public:
    explicit Engine(const Config& cfg = {}, float start_pos = 0.5f)
        : _cfg(cfg) {
        resetAt(start_pos, 0);
    }

    // ---- Lifecycle ---------------------------------------------------------
    // Hard-reset to a static hold at `pos`. Used on home/estop/resume/stream
    // rising-edge (seed at the machine's actual position).
    void resetAt(float pos, uint64_t now_us) {
        _hold_pos   = clamp01(pos);
        _mode       = Mode::Idle;
        _have_traj  = false;
        _traj_start = now_us;
        _ff_valid   = false;
        _plans      = 0;
        _failures   = 0;
    }

    // Ceiling updates take effect at the NEXT plan (the in-flight trajectory
    // was legal under the limits it was planned with; retro-editing it is
    // impossible anyway — it is an immutable polynomial).
    void setLimits(const Limits& l) { _cfg.limits = l; }
    const Config& config() const { return _cfg; }
    void setChaseFeedforward(bool on, float gain) {
        _cfg.chase_feedforward = on;
        _cfg.chase_ff_gain     = gain;
    }

    // ---- Command entry (Core 1, after queue drain) -------------------------
    // Plan a new trajectory NOW from the current sampled state. Returns false
    // if Ruckig rejected the input (previous plan keeps executing).
    bool commit(const Command& cmd, uint64_t now_us) {
        // Input guard: a non-finite target/velocity is an upstream parser bug —
        // reject HERE, deterministically, instead of leaning on Ruckig's
        // per-field validation coverage. detail -99 marks the local guard.
        if (!std::isfinite(cmd.target) || !std::isfinite(cmd.end_vel)) {
            _failures++;
            recordAnomaly(AnomalyType::PlanFailed, 0.0f, -99.0f, now_us);
            return false;
        }

        double p, v, a;
        sampleRaw(now_us, p, v, a);

        const bool one_shot = cmd.has_duration && cmd.duration_us > 0;
        const double target = clamp01(cmd.target);

        // ---- Target (handoff) velocity selection ---------------------------
        double vf = 0.0;
        if (cmd.has_end_vel) {
            vf = cmd.end_vel;                       // v4 G-slope handoff
        } else if (!one_shot && _cfg.chase_feedforward) {
            vf = chaseFeedforward(cmd, now_us);     // v3 stream estimate
        }
        {
            const double vf_safe = boundSafeEndVel(vf, target);
            if (std::fabs(vf_safe - vf) > 1e-6) {
                recordAnomaly(AnomalyType::EndVelClamped, (float)target,
                              (float)vf_safe, now_us);
            }
            vf = vf_safe;
        }

        // ---- Build the Ruckig input ---------------------------------------
        ruckig::InputParameter<1> in;
        in.current_position[0]     = p;
        in.current_velocity[0]     = v;
        in.current_acceleration[0] = a;
        in.target_position[0]      = target;
        in.target_velocity[0]      = vf;
        in.target_acceleration[0]  = 0.0;
        in.max_velocity[0]         = _cfg.limits.vmax;
        in.max_acceleration[0]     = _cfg.limits.amax;
        in.max_jerk[0]             = _cfg.limits.jmax;
        if (one_shot) {
            in.minimum_duration = (double)cmd.duration_us * 1e-6;
        }

        ruckig::Trajectory<1> traj;
        const ruckig::Result res = _calc.calculate(in, traj);
        if ((int)res < 0) {
            _failures++;
            recordAnomaly(AnomalyType::PlanFailed, (float)target,
                          (float)(int)res, now_us);
            return false;
        }

        // Deadline verdict: minimum_duration is a MINIMUM — an infeasible
        // (too-short) deadline yields the physical-minimum profile instead.
        // 2% + 1 ms slack so float→double round-trips never false-positive.
        if (one_shot) {
            const double want = (double)cmd.duration_us * 1e-6;
            if (traj.get_duration() > want * 1.02 + 0.001) {
                recordAnomaly(AnomalyType::DeadlineStretched, (float)target,
                              (float)traj.get_duration(), now_us);
            }
        }

        _traj       = traj;
        _have_traj  = true;
        _traj_start = now_us;
        _mode       = one_shot ? Mode::OneShot : Mode::Chase;
        _plans++;
        return true;
    }

    // ---- Evaluation (Core 1, ~1 kHz hot path) ------------------------------
    // May engage the SETTLE transition when the clock runs past a trajectory
    // that ends moving (mirrors MotionInterpolator::handleTimeout).
    float positionAt(uint64_t now_us) {
        maybeSettle(now_us);
        double p, v, a;
        sampleRaw(now_us, p, v, a);
        return (float)clamp01(p);
    }

    float velocityAt(uint64_t now_us) {
        maybeSettle(now_us);
        double p, v, a;
        sampleRaw(now_us, p, v, a);
        return (float)v;
    }

    float accelerationAt(uint64_t now_us) {
        maybeSettle(now_us);
        double p, v, a;
        sampleRaw(now_us, p, v, a);
        return (float)a;
    }

    // Time-aware "does the plan still have motion left to render?" — the
    // sampler gates on this exactly as it did on the cubic's isBusy(). A
    // trajectory pending SETTLE still counts as busy (it is still moving).
    bool isBusy(uint64_t now_us) const {
        if (!_have_traj) return false;
        if (elapsedS(now_us) < _traj.get_duration()) return true;
        // Past the end: busy only if it ended moving (settle will engage on
        // the next sample call).
        double p, v, a;
        _traj.at_time(_traj.get_duration(), p, v, a);
        return std::fabs(v) > kRestVel;
    }

    Mode mode() const { return _mode; }
    uint64_t lastPlanUs() const { return _traj_start; }

    Snapshot snapshot(uint64_t now_us) {
        maybeSettle(now_us);
        Snapshot s;
        double p, v, a;
        sampleRaw(now_us, p, v, a);
        s.pos = (float)clamp01(p);
        s.vel = (float)v;
        s.acc = (float)a;
        if (_have_traj) {
            double pe, ve, ae;
            _traj.at_time(_traj.get_duration(), pe, ve, ae);
            s.target     = (float)clamp01(pe);
            s.duration_s = (float)_traj.get_duration();
            const double el = elapsedS(now_us);
            s.elapsed_s  = (float)(el < _traj.get_duration()
                                       ? el : _traj.get_duration());
        } else {
            s.target = (float)_hold_pos;
        }
        s.mode     = (uint8_t)_mode;
        s.plans    = _plans;
        s.failures = _failures;
        return s;
    }

    // ---- Anomaly drain (Core 1, single-threaded — no lock) -----------------
    bool popAnomaly(Anomaly& out) {
        if (_anom_count == 0) return false;
        const uint8_t read =
            (uint8_t)((_anom_write + kAnomalyDepth - _anom_count) % kAnomalyDepth);
        out = _anom_ring[read];
        _anom_count--;
        return true;
    }

private:
    static constexpr double  kRestVel      = 1e-4;  // units/s: "stopped"
    static constexpr uint8_t kAnomalyDepth = 16;

    static double clamp01(double x) {
        return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
    }

    double elapsedS(uint64_t now_us) const {
        return now_us <= _traj_start ? 0.0
                                     : (double)(now_us - _traj_start) * 1e-6;
    }

    // Raw kinematic state (UNCLAMPED position — planning continuity must see
    // the true polynomial state even during a transient wall excursion).
    void sampleRaw(uint64_t now_us, double& p, double& v, double& a) const {
        if (!_have_traj) {
            p = _hold_pos; v = 0.0; a = 0.0;
            return;
        }
        double t = elapsedS(now_us);
        const double dur = _traj.get_duration();
        if (t >= dur) t = dur;   // hold the end state; settle handles vf≠0
        _traj.at_time(t, p, v, a);
    }

    // Wall guard + ceiling for a requested end velocity: the machine must be
    // able to brake to rest inside the window beyond the target. Trapezoid
    // bound vf² ≤ amax·dist (i.e. brake at amax/2 average) — conservative
    // margin for the jerk-limited tail at sane jmax/amax ratios.
    double boundSafeEndVel(double vf, double target) const {
        const double vcap = _cfg.limits.vmax;
        if (vf >  vcap) vf =  vcap;
        if (vf < -vcap) vf = -vcap;
        const double dist = vf > 0.0 ? (1.0 - target) : target;
        const double vmax_wall = std::sqrt((double)_cfg.limits.amax * dist);
        if (std::fabs(vf) > vmax_wall) {
            vf = vf > 0.0 ? vmax_wall : -vmax_wall;
        }
        return vf;
    }

    // Finite-difference stream velocity estimator (CHASE feedforward).
    // EMA-damped; resets on stream gaps so a fresh stream starts clean.
    double chaseFeedforward(const Command& cmd, uint64_t now_us) {
        double vf = 0.0;
        if (_ff_valid && now_us > _ff_last_us) {
            const uint64_t gap = now_us - _ff_last_us;
            if (gap <= _cfg.chase_stale_us) {
                const double raw =
                    ((double)cmd.target - _ff_last_target) / ((double)gap * 1e-6);
                _ff_ema = _ff_valid_ema ? 0.5 * _ff_ema + 0.5 * raw : raw;
                _ff_valid_ema = true;
                vf = _ff_ema * (double)_cfg.chase_ff_gain;
            } else {
                _ff_valid_ema = false;   // stale stream → forget the EMA
            }
        }
        _ff_last_target = cmd.target;
        _ff_last_us     = now_us;
        _ff_valid       = true;
        return vf;
    }

    // Starve-settle: the clock ran past a trajectory that ends moving and no
    // fresh command replanned it → plan a jerk-limited brake-to-rest from the
    // end state (velocity control interface; it lands wherever braking lands,
    // clamped by the sampler at the walls). One-time boundary event.
    void maybeSettle(uint64_t now_us) {
        if (!_have_traj || _mode == Mode::Settle) {
            settleToIdle(now_us);
            return;
        }
        const double dur = _traj.get_duration();
        if (elapsedS(now_us) < dur) return;

        double p, v, a;
        _traj.at_time(dur, p, v, a);
        if (std::fabs(v) <= kRestVel) {
            // Ended at rest — collapse to a plain hold.
            _hold_pos  = clamp01(p);
            _have_traj = false;
            _mode      = Mode::Idle;
            return;
        }

        ruckig::InputParameter<1> in;
        in.control_interface       = ruckig::ControlInterface::Velocity;
        in.current_position[0]     = p;
        in.current_velocity[0]     = v;
        in.current_acceleration[0] = a;
        in.target_velocity[0]      = 0.0;
        in.target_acceleration[0]  = 0.0;
        in.max_velocity[0]         = _cfg.limits.vmax;
        in.max_acceleration[0]     = _cfg.limits.amax;
        in.max_jerk[0]             = _cfg.limits.jmax;

        ruckig::Trajectory<1> traj;
        const ruckig::Result res = _calc.calculate(in, traj);
        // Anchor the settle at the moment the starved trajectory ENDED, not
        // at this sample's clock — the brake follows the glide seamlessly.
        const uint64_t end_us =
            _traj_start + (uint64_t)(dur * 1e6 + 0.5);
        if ((int)res < 0) {
            // Brake plan failed (should be unreachable: a brake from a legal
            // state is always feasible) — hard-hold the end position.
            _failures++;
            recordAnomaly(AnomalyType::PlanFailed, (float)clamp01(p),
                          (float)(int)res, end_us);
            _hold_pos  = clamp01(p);
            _have_traj = false;
            _mode      = Mode::Idle;
            return;
        }
        recordAnomaly(AnomalyType::SettleEngaged, (float)clamp01(p),
                      (float)v, end_us);
        _traj       = traj;
        _traj_start = end_us;
        _mode       = Mode::Settle;
        _plans++;
    }

    // A finished SETTLE collapses to Idle hold at its landing position.
    void settleToIdle(uint64_t now_us) {
        if (!_have_traj || _mode != Mode::Settle) return;
        if (elapsedS(now_us) < _traj.get_duration()) return;
        double p, v, a;
        _traj.at_time(_traj.get_duration(), p, v, a);
        _hold_pos  = clamp01(p);
        _have_traj = false;
        _mode      = Mode::Idle;
    }

    void recordAnomaly(AnomalyType kind, float target, float detail,
                       uint64_t now_us) {
        Anomaly& slot = _anom_ring[_anom_write];
        slot.kind   = (uint8_t)kind;
        slot.seq    = _anom_seq++;
        slot.t_us   = now_us;
        slot.target = target;
        slot.detail = detail;
        _anom_write = (uint8_t)((_anom_write + 1) % kAnomalyDepth);
        if (_anom_count < kAnomalyDepth) _anom_count++;
    }

    // ---- State -------------------------------------------------------------
    Config                _cfg;
    ruckig::Ruckig<1>     _calc;        // offline calculate() only — no cycle time
    ruckig::Trajectory<1> _traj;
    bool                  _have_traj = false;
    uint64_t              _traj_start = 0;
    double                _hold_pos = 0.5;
    Mode                  _mode = Mode::Idle;

    // Chase feedforward estimator
    bool     _ff_valid = false;
    bool     _ff_valid_ema = false;
    double   _ff_ema = 0.0;
    double   _ff_last_target = 0.5;
    uint64_t _ff_last_us = 0;

    // Counters + anomaly ring
    uint32_t _plans = 0;
    uint32_t _failures = 0;
    Anomaly  _anom_ring[kAnomalyDepth];
    uint8_t  _anom_write = 0;
    uint8_t  _anom_count = 0;
    uint16_t _anom_seq = 0;
};

} // namespace slopmotion
