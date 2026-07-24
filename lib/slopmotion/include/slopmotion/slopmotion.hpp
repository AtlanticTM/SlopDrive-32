#pragma once

// ============================================================================
// SlopMotion — jerk-limited dual-mode motion core (quintic waveform + Ruckig)
// ============================================================================
//
// PURPOSE
// -------
// The successor to MotionInterpolator's cubic Hermite engine: every incoming
// motion command becomes ONE planned trajectory computed from the engine's
// ACTUAL current kinematic state (position + velocity + acceleration), and the
// existing ~1 kHz stream sampler simply evaluates it. Event-driven, never
// clocked — a plan is computed only when a command arrives (or when a stream
// starves mid-glide, see SETTLE below).
//
// THE DIVISION OF LABOR (measured, not assumed — see the part-1 bench)
// --------------------------------------------------------------------
// Ruckig Community is a superb POINT-TO-POINT planner and a poor waveform
// INTERPOLATOR: its minimum_duration-stretched profiles are bang-cruise-bang
// (no smoothness objective — following interior points is the Pro "waypoints"
// feature). Measured on the v4 sparse-sine bench: RMS deviation 0.090 for the
// stretched Ruckig profile vs 0.005 for a quintic Hermite, at 28 vs 8 peak
// accel. Handing Ruckig perfect boundary conditions (af = source) does not
// change the shape. Hence:
//
//   * WAVEFORM (TCode v4 / any point carrying I<ms> ≥ 50 ms): a QUINTIC
//     Hermite segment from the current (p,v,a) to (target, G-velocity,
//     af-estimate) over exactly the commanded duration. C2 where the old
//     cubic was C1 — the boundary accel spike class is gone — and it
//     reproduces the sender's spline faithfully (that was the cubic's one
//     virtue; we keep its soul, fix its sins). Every quintic is CEILING-
//     SCANNED at plan time; a segment that demands more than vmax/amax/jmax
//     (or leaves the window) falls back to the Ruckig guard, which stretches
//     to the physical minimum and flags the anomaly. An infeasible deadline
//     therefore arrives AS FAST AS THE CEILINGS ALLOW, never lagging below
//     them, and never trusting an absurd wire command verbatim.
//   * CHASE (TCode v3 bare high-rate points, or I < 50 ms): the future is
//     unknown → Ruckig chases the point stream under the ceilings, replanning
//     per point from the sampled state (C2-continuous, retarget-while-moving
//     is the normal case). For DENSE streams (mean interval ≤ 60 ms, the
//     legacy live-trigger idea) the engine aims one interval AHEAD of the
//     newest point with the estimated stream velocity as the arrival
//     velocity — chasing the newest stale point by construction lags ~4
//     intervals (measured); predictive aim is how the old live mode solved
//     this and how we solve it too. Sparse/isolated points get pure
//     point-chase (no invented velocity).
//   * SETTLE: a trajectory that ends still-moving with no fresh command gets
//     a one-time jerk-limited brake-to-rest via Ruckig's velocity interface
//     (replaces the cubic's parabolic DecelOverrun). A boundary event, not a
//     clock loop.
//
// SAFETY / BOUNDS
// ---------------
// Ruckig Community has NO position limits and quintics can bulge, so the
// Engine owns the window:
//   * command targets are clamped to [0,1] at commit;
//   * requested end velocities are clamped so the machine can brake before
//     the wall beyond the target (|vf|² ≤ amax·dist_to_wall, and ≤ vmax);
//   * quintic plans are legality-scanned (v/a/j ceilings AND window bounds)
//     before adoption — illegal shapes fall back to the Ruckig guard;
//   * sampled OUTPUT position is clamped to [0,1]; the arbiter's window clamp
//     remains the hard physical backstop downstream.
// Limits are CEILINGS, never targets (the doctrine, verbatim).
//
// UNITS & TIME
// ------------
//   position : normalized 0..1 across the configured stroke window
//   velocity : normalized units per second (TCode G wire value / 1000)
//   time     : microseconds, uint64, injected by the caller (esp_timer on
//              target, synthetic in native tests — fully deterministic)
// Public API is float; planning math is double (plan-time only — the 1 kHz
// sample path is a polynomial/profile evaluation).
//
// THREADING
// ---------
// Single-threaded by contract, same as MotionInterpolator: ALL of
// commit/positionAt/velocityAt/snapshot run on Core 1 (the sampler task).
// Cross-core command handoff stays OUTSIDE this class. No locks, no heap in
// steady state (Ruckig's waypoint vectors stay empty in community mode).
//
// Hardware-free: std headers + vendored lib/ruckig only. Native-tested in
// test/native/test_slopmotion; scenario bench in examples/slopmotion_traces. :3

#include <cstdint>
#include <cmath>
#include <optional>

#include <ruckig/ruckig.hpp>

namespace slopmotion {

inline constexpr const char* kVersion = "0.2.0";

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

    // CHASE feedforward + predictive aim (dense streams only, see gate
    // below): the engine differentiates the incoming point stream, aims
    // chase_lookahead intervals ahead of the newest point, and asks Ruckig to
    // arrive there AT the estimated stream velocity. Tuned on the bench
    // graphs — chasing the stale newest point with damped arrival velocity
    // measured ~4 intervals of lag.
    bool     chase_feedforward = true;
    float    chase_ff_gain     = 0.9f;    // damping on the velocity estimate
    float    chase_lookahead   = 3.0f;    // intervals of predictive aim (bench-swept)
    // Arrive matching the stream's estimated CURVATURE too (target accel).
    // With af forced to 0 every chase plan is a long flatten-out tail that a
    // curving stream preempts forever — chronic lag (bench-measured).
    bool     chase_accel_ff    = true;
    // Streams with mean interval above this are NOT dense: no feedforward, no
    // extrapolation — isolated points plan a plain point-chase to rest.
    // (The legacy LIVE_TRIGGER idea, relaxed to admit 20 Hz app streams.)
    uint32_t chase_dense_us    = 60000;
    // Estimator resets after a stream gap this long.
    uint32_t chase_stale_us    = 400000;
};

// ---- Command (POD, queue-safe — the InterpSegment successor) ---------------
struct Command {
    float    target       = 0.5f;   // normalized 0..1
    uint32_t duration_us  = 0;      // I<ms> * 1000 when present
    float    end_vel      = 0.0f;   // units/s — TCode G wire value / 1000
    bool     has_end_vel  = false;  // true → v4 gradient handoff velocity
    bool     has_duration = false;  // true + duration ≥ 50 ms → WAVEFORM
};

enum class Mode : uint8_t {
    Idle     = 0,   // holding position, no planned motion
    Waveform = 1,   // executing a quintic v4 segment (or its Ruckig fallback)
    Chase    = 2,   // tracking a bare point stream
    Settle   = 3    // braking to rest after stream starvation
};

enum class PlanKind : uint8_t { None = 0, Quintic = 1, Ruckig = 2 };

// ---- Anomaly instrumentation (same drain pattern as the cubic engine) ------
enum class AnomalyType : uint8_t {
    None              = 0,
    PlanFailed        = 1,  // plan rejected; previous plan kept. detail = (float)Result or -99 input guard
    SettleEngaged     = 2,  // stream starved mid-glide → brake plan. detail = end velocity
    EndVelClamped     = 3,  // requested vf cut by wall/vmax guard. detail = clamped vf
    DeadlineStretched = 4,  // commanded duration infeasible; guard profile runs longer. detail = actual s
    WaveformFallback  = 5   // quintic broke a ceiling/window → Ruckig guard took the segment. detail = worst ratio
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
    float    target     = 0.5f;   // active plan's end position
    float    duration_s = 0.0f;   // active plan duration (0 = holding)
    float    elapsed_s  = 0.0f;
    uint8_t  mode       = 0;      // Mode
    uint8_t  plan_kind  = 0;      // PlanKind of the active plan
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
        _kind       = PlanKind::None;
        _plan_start = now_us;
        _est_valid  = false;
        _est_ema_ok = false;
        _prev_vf_ok = false;
        _plans      = 0;
        _failures   = 0;
    }

    // Ceiling updates take effect at the NEXT plan (an in-flight trajectory
    // is an immutable polynomial planned under the limits of its time).
    void setLimits(const Limits& l) { _cfg.limits = l; }
    const Config& config() const { return _cfg; }
    void setChaseFeedforward(bool on, float gain) {
        _cfg.chase_feedforward = on;
        _cfg.chase_ff_gain     = gain;
    }

    // ---- Command entry (Core 1, after queue drain) -------------------------
    // Plan a new trajectory NOW from the current sampled state. Returns false
    // if the input was rejected (previous plan keeps executing).
    bool commit(const Command& cmd, uint64_t now_us) {
        // Input guard: a non-finite target/velocity is an upstream parser bug —
        // reject HERE, deterministically. detail -99 marks the local guard.
        if (!std::isfinite(cmd.target) || !std::isfinite(cmd.end_vel)) {
            _failures++;
            recordAnomaly(AnomalyType::PlanFailed, 0.0f, -99.0f, now_us);
            return false;
        }

        double p, v, a;
        sampleRaw(now_us, p, v, a);

        const double target = clamp01(cmd.target);
        // Stream estimator feeds BOTH modes (chase aim + waveform af/vf
        // estimates), so update it on every commit.
        updateEstimator(target, now_us);

        const bool waveform =
            cmd.has_duration && cmd.duration_us >= kShortMoveUs;

        bool ok;
        if (waveform) {
            ok = commitWaveform(cmd, p, v, a, target, now_us);
        } else {
            ok = commitChase(cmd, p, v, a, target, now_us);
        }
        if (ok) _plans++;
        return ok;
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
        if (_kind == PlanKind::None) return false;
        if (elapsedS(now_us) < planDuration()) return true;
        double p, v, a;
        planEndState(p, v, a);
        return std::fabs(v) > kRestVel;
    }

    Mode     mode() const { return _mode; }
    PlanKind planKind() const { return _kind; }
    uint64_t lastPlanUs() const { return _plan_start; }

    Snapshot snapshot(uint64_t now_us) {
        maybeSettle(now_us);
        Snapshot s;
        double p, v, a;
        sampleRaw(now_us, p, v, a);
        s.pos = (float)clamp01(p);
        s.vel = (float)v;
        s.acc = (float)a;
        if (_kind != PlanKind::None) {
            double pe, ve, ae;
            planEndState(pe, ve, ae);
            s.target     = (float)clamp01(pe);
            s.duration_s = (float)planDuration();
            const double el = elapsedS(now_us);
            s.elapsed_s  = (float)(el < planDuration() ? el : planDuration());
        } else {
            s.target = (float)_hold_pos;
        }
        s.mode      = (uint8_t)_mode;
        s.plan_kind = (uint8_t)_kind;
        s.plans     = _plans;
        s.failures  = _failures;
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
    static constexpr double   kRestVel      = 1e-4;   // units/s: "stopped"
    static constexpr uint8_t  kAnomalyDepth = 16;
    // I<ms> below this is a dense-stream point, not a plannable segment
    // (legacy SHORT_MOVE_INTERVAL).
    static constexpr uint32_t kShortMoveUs  = 50000;
    static constexpr int      kScanSteps    = 64;     // quintic legality grid
    static constexpr double   kAimCapS      = 0.060;  // predictive aim ceiling

    static double clamp01(double x) {
        return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
    }

    double elapsedS(uint64_t now_us) const {
        return now_us <= _plan_start ? 0.0
                                     : (double)(now_us - _plan_start) * 1e-6;
    }

    // ---- Active-plan evaluation --------------------------------------------
    double planDuration() const {
        return _kind == PlanKind::Quintic ? _q_T
             : _kind == PlanKind::Ruckig  ? _traj.get_duration()
             : 0.0;
    }

    void planEndState(double& p, double& v, double& a) const {
        if (_kind == PlanKind::Quintic)      quinticAt(1.0, p, v, a);
        else if (_kind == PlanKind::Ruckig)  _traj.at_time(_traj.get_duration(), p, v, a);
        else { p = _hold_pos; v = 0.0; a = 0.0; }
    }

    // Raw kinematic state (UNCLAMPED position — planning continuity must see
    // the true polynomial state even during a transient wall excursion).
    void sampleRaw(uint64_t now_us, double& p, double& v, double& a) const {
        if (_kind == PlanKind::None) {
            p = _hold_pos; v = 0.0; a = 0.0;
            return;
        }
        double t = elapsedS(now_us);
        const double dur = planDuration();
        if (t >= dur) t = dur;   // hold the end state; settle handles vf≠0
        if (_kind == PlanKind::Quintic) quinticAt(dur > 0 ? t / dur : 1.0, p, v, a);
        else                            _traj.at_time(t, p, v, a);
    }

    // Quintic evaluation at normalized tau ∈ [0,1] (real-time derivatives).
    void quinticAt(double tau, double& p, double& v, double& a) const {
        const double* c = _q_c;
        p = ((((c[5]*tau + c[4])*tau + c[3])*tau + c[2])*tau + c[1])*tau + c[0];
        v = ((((5*c[5]*tau + 4*c[4])*tau + 3*c[3])*tau + 2*c[2])*tau + c[1]) / _q_T;
        a = (((20*c[5]*tau + 12*c[4])*tau + 6*c[3])*tau + 2*c[2]) / (_q_T * _q_T);
    }

    // ---- WAVEFORM (v4 / timed segments): quintic + Ruckig guard ------------
    bool commitWaveform(const Command& cmd, double p, double v, double a,
                        double target, uint64_t now_us) {
        const double T = (double)cmd.duration_us * 1e-6;

        // End velocity: wire G when present, else the stream estimate (an
        // I-only stream shouldn't come to rest at every point).
        double vf = cmd.has_end_vel ? (double)cmd.end_vel
                  : (_cfg.chase_feedforward && _est_ema_ok && streamIsDense())
                        ? _est_v_ema * (double)_cfg.chase_ff_gain
                        : 0.0;
        vf = applyEndVelGuard(vf, target, now_us);

        // End acceleration: the wire carries no af, but consecutive G values
        // imply it (backward difference). Measured on the bench: af=source
        // cuts waveform RMS ~3x vs af=0; the backward difference approximates
        // that. Only trusted between consecutive G-bearing commands of a
        // dense-ish segment chain.
        double af = 0.0;
        if (cmd.has_end_vel && _prev_vf_ok && now_us > _prev_vf_us) {
            const double gap = (double)(now_us - _prev_vf_us) * 1e-6;
            if (gap < 3.0 * T) af = ((double)cmd.end_vel - _prev_vf) / gap;
        }
        if (cmd.has_end_vel) {
            _prev_vf = (double)cmd.end_vel;   // raw wire value, pre-guard
            _prev_vf_us = now_us;
            _prev_vf_ok = true;
        } else {
            _prev_vf_ok = false;
        }

        // Build the quintic in normalized tau; scaled boundary derivatives.
        const double V0 = v * T, A0 = a * T * T;
        const double VF = vf * T, AF = af * T * T;
        const double R1 = target - p - V0 - A0 / 2.0;
        const double R2 = VF - V0 - A0;
        const double R3 = AF - A0;
        double c[6];
        c[0] = p; c[1] = V0; c[2] = A0 / 2.0;
        c[3] =  10.0 * R1 - 4.0 * R2 + R3 / 2.0;
        c[4] = -15.0 * R1 + 7.0 * R2 - R3;
        c[5] =   6.0 * R1 - 3.0 * R2 + R3 / 2.0;

        double worst = quinticWorstRatio(c, T);
        if (worst <= 1.0) {
            for (int i = 0; i < 6; i++) _q_c[i] = c[i];
            _q_T        = T;
            _kind       = PlanKind::Quintic;
            _mode       = Mode::Waveform;
            _plan_start = now_us;
            return true;
        }

        // Quintic broke a ceiling or the window → the Ruckig guard takes the
        // segment: stretched to the deadline when feasible, physical minimum
        // (+ DeadlineStretched) when not. This is the "absurd wire command"
        // path the old cubic executed verbatim.
        recordAnomaly(AnomalyType::WaveformFallback, (float)target,
                      (float)worst, now_us);
        const bool ok = planRuckig(p, v, a, target, vf, 0.0, T, now_us);
        if (ok) {
            _mode = Mode::Waveform;
            if (_traj.get_duration() > T * 1.02 + 0.001) {
                recordAnomaly(AnomalyType::DeadlineStretched, (float)target,
                              (float)_traj.get_duration(), now_us);
            }
        }
        return ok;
    }

    // Worst (peak / ceiling) ratio across v/a/j ceilings AND window bounds,
    // scanned on a fixed tau grid. > 1.0 = illegal quintic.
    double quinticWorstRatio(const double* c, double T) const {
        const double vc = _cfg.limits.vmax, ac = _cfg.limits.amax,
                     jc = _cfg.limits.jmax;
        double worst = 0.0;
        for (int i = 0; i <= kScanSteps; i++) {
            const double tau = (double)i / kScanSteps;
            const double pp = ((((c[5]*tau + c[4])*tau + c[3])*tau + c[2])*tau + c[1])*tau + c[0];
            const double vv = ((((5*c[5]*tau + 4*c[4])*tau + 3*c[3])*tau + 2*c[2])*tau + c[1]) / T;
            const double aa = (((20*c[5]*tau + 12*c[4])*tau + 6*c[3])*tau + 2*c[2]) / (T*T);
            const double jj = ((60*c[5]*tau + 24*c[4])*tau + 6*c[3]) / (T*T*T);
            worst = std::fmax(worst, std::fabs(vv) / vc);
            worst = std::fmax(worst, std::fabs(aa) / ac);
            worst = std::fmax(worst, std::fabs(jj) / jc);
            // window with a small grace band — the sampler clamp flattens
            // tiny bulges; a real excursion goes to the guard
            if (pp < -0.02) worst = std::fmax(worst, 1.0 + (-0.02 - pp));
            if (pp >  1.02) worst = std::fmax(worst, 1.0 + (pp - 1.02));
        }
        return worst;
    }

    // ---- CHASE (bare / short-interval points) ------------------------------
    bool commitChase(const Command& cmd, double p, double v, double a,
                     double target, uint64_t now_us) {
        double aim = target;
        double vf  = 0.0;
        double af  = 0.0;
        if (_cfg.chase_feedforward && _est_ema_ok && streamIsDense()) {
            // Predictive aim: the newest point is already ~1 interval stale
            // and the plan needs time to get there — aim ahead along the
            // stream's motion, arrive AT its velocity and curvature. (The
            // legacy live-mode extrapolation, reborn with real dynamics.)
            const double look =
                std::fmin(_est_dt_ema * (double)_cfg.chase_lookahead, kAimCapS);
            const double v_est = _est_v_ema * (double)_cfg.chase_ff_gain;
            aim = clamp01(target + v_est * look);
            vf  = applyEndVelGuard(v_est, aim, now_us);
            if (_cfg.chase_accel_ff) {
                const double acap = 0.5 * (double)_cfg.limits.amax;
                af = _est_a_ema < -acap ? -acap
                   : _est_a_ema >  acap ?  acap : _est_a_ema;
            }
        } else if (cmd.has_end_vel) {
            vf = applyEndVelGuard((double)cmd.end_vel, aim, now_us);
        }
        const bool ok = planRuckig(p, v, a, aim, vf, af, 0.0, now_us);
        if (ok) _mode = Mode::Chase;
        return ok;
    }

    // ---- Shared Ruckig point-planner (chase, guard fallback) ---------------
    // min_dur 0 = time-optimal; > 0 = stretch toward the deadline.
    bool planRuckig(double p, double v, double a, double target, double vf,
                    double af, double min_dur, uint64_t now_us) {
        ruckig::InputParameter<1> in;
        in.current_position[0]     = p;
        in.current_velocity[0]     = v;
        in.current_acceleration[0] = a;
        in.target_position[0]      = target;
        in.target_velocity[0]      = vf;
        in.target_acceleration[0]  = af;
        in.max_velocity[0]         = _cfg.limits.vmax;
        in.max_acceleration[0]     = _cfg.limits.amax;
        in.max_jerk[0]             = _cfg.limits.jmax;
        if (min_dur > 0.0) in.minimum_duration = min_dur;

        ruckig::Trajectory<1> traj;
        const ruckig::Result res = _calc.calculate(in, traj);
        if ((int)res < 0) {
            _failures++;
            recordAnomaly(AnomalyType::PlanFailed, (float)target,
                          (float)(int)res, now_us);
            return false;
        }
        _traj       = traj;
        _kind       = PlanKind::Ruckig;
        _plan_start = now_us;
        return true;
    }

    // Wall guard + ceiling for a requested end velocity: the machine must be
    // able to brake to rest inside the window beyond the target. Trapezoid
    // bound vf² ≤ amax·dist — conservative margin for the jerk-limited tail
    // at sane jmax/amax ratios; the sampler clamp is the hard backstop.
    double applyEndVelGuard(double vf, double target, uint64_t now_us) {
        const double vin = vf;
        const double vcap = _cfg.limits.vmax;
        if (vf >  vcap) vf =  vcap;
        if (vf < -vcap) vf = -vcap;
        const double dist = vf > 0.0 ? (1.0 - target) : target;
        const double vmax_wall = std::sqrt((double)_cfg.limits.amax * dist);
        if (std::fabs(vf) > vmax_wall) {
            vf = vf > 0.0 ? vmax_wall : -vmax_wall;
        }
        if (std::fabs(vf - vin) > 1e-6) {
            recordAnomaly(AnomalyType::EndVelClamped, (float)target,
                          (float)vf, now_us);
        }
        return vf;
    }

    // ---- Stream estimator (velocity + cadence of the incoming points) ------
    // Fed by every commit; consumed by chase aim and waveform vf/af fill-ins.
    // EMAs are deliberately calm (the ±ms arrival jitter of real transports
    // otherwise buzzes straight into the acceleration trace — bench-measured).
    void updateEstimator(double target, uint64_t now_us) {
        if (_est_valid && now_us > _est_last_us) {
            const uint64_t gap = now_us - _est_last_us;
            if (gap <= _cfg.chase_stale_us) {
                const double dt  = (double)gap * 1e-6;
                const double raw = (target - _est_last_target) / dt;
                if (_est_ema_ok) {
                    const double v_prev = _est_v_ema;
                    _est_v_ema  += 0.35 * (raw - _est_v_ema);
                    _est_dt_ema += 0.30 * (dt - _est_dt_ema);
                    // Stream curvature: differentiate the (already smoothed)
                    // velocity EMA, then smooth again — accel estimates are
                    // second differences of a jittery signal, treat gently.
                    const double a_raw = (_est_v_ema - v_prev) / dt;
                    _est_a_ema += 0.25 * (a_raw - _est_a_ema);
                } else {
                    _est_v_ema  = raw;
                    _est_dt_ema = dt;
                    _est_a_ema  = 0.0;
                    _est_ema_ok = true;
                }
            } else {
                _est_ema_ok = false;   // stale stream → forget the dynamics
            }
        }
        _est_last_target = target;
        _est_last_us     = now_us;
        _est_valid       = true;
    }

    bool streamIsDense() const {
        return _est_ema_ok &&
               _est_dt_ema * 1e6 <= (double)_cfg.chase_dense_us;
    }

    // ---- Starve-settle -----------------------------------------------------
    // The clock ran past a plan that ends moving and no fresh command
    // replanned it → plan a jerk-limited brake-to-rest from the end state
    // (velocity control interface; lands wherever braking lands, clamped by
    // the sampler at the walls). One-time boundary event.
    void maybeSettle(uint64_t now_us) {
        if (_kind == PlanKind::None || _mode == Mode::Settle) {
            settleToIdle(now_us);
            return;
        }
        const double dur = planDuration();
        if (elapsedS(now_us) < dur) return;

        double p, v, a;
        planEndState(p, v, a);
        if (std::fabs(v) <= kRestVel) {
            // Ended at rest — collapse to a plain hold.
            _hold_pos = clamp01(p);
            _kind     = PlanKind::None;
            _mode     = Mode::Idle;
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
        // Anchor the settle at the moment the starved plan ENDED, not at this
        // sample's clock — the brake follows the glide seamlessly.
        const uint64_t end_us = _plan_start + (uint64_t)(dur * 1e6 + 0.5);
        if ((int)res < 0) {
            // Should be unreachable: a brake from a legal state is always
            // feasible — hard-hold the end position.
            _failures++;
            recordAnomaly(AnomalyType::PlanFailed, (float)clamp01(p),
                          (float)(int)res, end_us);
            _hold_pos = clamp01(p);
            _kind     = PlanKind::None;
            _mode     = Mode::Idle;
            return;
        }
        recordAnomaly(AnomalyType::SettleEngaged, (float)clamp01(p),
                      (float)v, end_us);
        _traj       = traj;
        _kind       = PlanKind::Ruckig;
        _plan_start = end_us;
        _mode       = Mode::Settle;
        _plans++;
    }

    // A finished SETTLE collapses to Idle hold at its landing position.
    void settleToIdle(uint64_t now_us) {
        if (_kind == PlanKind::None || _mode != Mode::Settle) return;
        if (elapsedS(now_us) < planDuration()) return;
        double p, v, a;
        planEndState(p, v, a);
        _hold_pos = clamp01(p);
        _kind     = PlanKind::None;
        _mode     = Mode::Idle;
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
    ruckig::Trajectory<1> _traj;        // active Ruckig plan (chase/guard/settle)
    double                _q_c[6] = {}; // active quintic (normalized tau)
    double                _q_T = 0.0;   // quintic duration, seconds
    PlanKind              _kind = PlanKind::None;
    uint64_t              _plan_start = 0;
    double                _hold_pos = 0.5;
    Mode                  _mode = Mode::Idle;

    // Stream estimator
    bool     _est_valid = false;
    bool     _est_ema_ok = false;
    double   _est_v_ema = 0.0;
    double   _est_a_ema = 0.0;
    double   _est_dt_ema = 0.0;
    double   _est_last_target = 0.5;
    uint64_t _est_last_us = 0;

    // Previous wire G (for the backward-difference af estimate)
    bool     _prev_vf_ok = false;
    double   _prev_vf = 0.0;
    uint64_t _prev_vf_us = 0;

    // Counters + anomaly ring
    uint32_t _plans = 0;
    uint32_t _failures = 0;
    Anomaly  _anom_ring[kAnomalyDepth];
    uint8_t  _anom_write = 0;
    uint8_t  _anom_count = 0;
    uint16_t _anom_seq = 0;
};

} // namespace slopmotion
