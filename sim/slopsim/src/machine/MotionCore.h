#pragma once

// MotionCore — the pieces of the motion path that BOTH the live sim and the
// offline replayer run.
// Constraints:
//   Everything here is a transcription of a firmware seam (named per item) and
//   is shared for ONE reason: the analyzer's replay tuner is a second driver of
//   the same engine, and a tuner that renders a different curve than the machine
//   is worse than no tuner. If a rule lives here, MachineSim must not keep a
//   private copy of it — see MotionReplay.h for the other caller.
//   Pure host math: no clock, no hub, no locks, no I/O. Callers own the time
//   base and the state.

#include <cmath>
#include <cstdint>

#include "slopmotion/slopmotion.hpp"

namespace slopsim {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Soft-start on the stream feed. Two DIFFERENT, opposite quantities, both from
// include/system/config_api.h (~409-410):
//   kSafeApproachSpeed — a FLOOR under the speed the sampler feeds FAS, so a
//                        gentle ceiling never collapses to a crawl.
//   kSafeResumeRampMs  — the window over which SystemState::safeSpeedCap ramps
//                        the CEILING up from kSafeApproachSpeed after a
//                        discontinuity (un-pause, new stream, homing done).
inline constexpr float    kSafeApproachSpeed = 100.0f;   // mm/s  SAFE_APPROACH_SPEED_MM_S
inline constexpr uint32_t kSafeResumeRampMs  = 1200;     // ms    SAFE_RESUME_RAMP_MS
// SystemState::StreamSpeedMode. 0 is the DEVICE DEFAULT.
inline constexpr uint8_t kSpeedCeilingPegged   = 0;
inline constexpr uint8_t kSpeedVelocityMatched = 1;
// RangeMapper::setRange minimum window span (src/motion/range_mapper.cpp:27).
inline constexpr float kMinWindowSpanMm = 5.0f;
// 0x0085's "no end velocity" sentinel (SlopSyncCatalog).
inline constexpr int16_t kSegNoEndVel = -32768;
// Floats per record in the analyzer's trace feed:
//   {t_s, pos_mm, tgt_mm, vel_mm_s, cmd_norm, raw_norm, plan_kind, eng_vel_norm}
// The LIVE ring (MachineSim) and the REPLAY output (MotionReplay) both emit this
// layout, and /api/trace.bin ships it in the header so the page lays out its
// columns from the wire rather than from a number two files have to agree on by
// memory. APPEND ONLY: the analyzer reads what it knows and ignores the rest, so
// an older page against a newer sim degrades instead of misreading every row.
//
// plan_kind / eng_vel_norm were appended for the window-breach investigation:
// "the carriage left the window" and "the PLAN was still driving outward when it
// got there" are different diagnoses, and only the second one indicts the engine.
inline constexpr uint32_t kTraceStride = 8;

// ---- PacingEntry / PacingRing -----------------------------------------------
// Verbatim host copy of the firmware's (SlopSyncHubService.h). Single-thread
// producer/consumer by contract in BOTH callers — the live sim pushes from
// onStreamBundle and pops in the substep loop, both on the sim thread; the
// replayer owns its ring outright — so it stays lock-free for the same
// structural reason the firmware's does.
struct PacingEntry {
    uint64_t due_us = 0;
    float    target = 0.0f;
    float    vel    = 0.0f;
    uint32_t duration_us  = 0;
    bool     has_duration = false;
    bool     has_end_vel  = false;
    // RFC-030: the EFFECTIVE curve family of the grant that produced this entry
    // (registry curve_families; 0 = unspecified), stamped per-entry at ingress
    // because entries from different sessions could interleave in the ring — a
    // drain-time "current family" cache would mis-attribute them.
    uint8_t  curve_family = 0;
};

class PacingRing {
public:
    static constexpr size_t kCapacity = 64;

    bool push(const PacingEntry& e) {
        bool overwrote = false;
        if (_count == kCapacity) {
            _tail = (_tail + 1) % kCapacity;
            overwrote = true;
        } else {
            ++_count;
        }
        _buf[_head] = e;
        _head = (_head + 1) % kCapacity;
        return overwrote;
    }

    bool popDue(uint64_t now_us, PacingEntry& out) {
        if (_count == 0 || _buf[_tail].due_us > now_us) return false;
        out = _buf[_tail];
        _tail = (_tail + 1) % kCapacity;
        --_count;
        return true;
    }

    // RFC-008 one-segment lookahead — see the firmware's own peekOldest().
    const PacingEntry* peekOldest() const { return _count == 0 ? nullptr : &_buf[_tail]; }

private:
    PacingEntry _buf[kCapacity]{};
    size_t _head = 0, _tail = 0, _count = 0;
};

// ---- Wire decode ------------------------------------------------------------
// The 0x0084 / 0x0085 payload -> PacingEntry rule, in ONE place. The live path
// (MachineSim::onStreamBundle) reads the three raw fields straight off the
// bundle; the replayer reads the same three raw fields back out of a saved
// recording's CSV. Both then land here, so a recording renders through the
// identical scaling the wire got — a units bug cannot exist on one side only.
//
// `due_us` is NOT set here: it is a function of arrival time and the sender's
// timestamp, which only the caller knows.
//
// Returns false when the sample is REFUSED at decode (a 0 ms segment duration —
// the sender bug the wire recorder exists to catch). The entry is untouched in
// that case; the caller still records the row.
inline bool decodeWireSample(bool isSegment, uint16_t rawTarget, uint16_t rawDurMs,
                             int16_t rawEndVel, PacingEntry& e) {
    e.target = float(rawTarget) / 10000.0f;
    if (!isSegment) {
        e.vel = float(rawEndVel) / 1000.0f;
        // A 0x0084 point carries a gradient only when it is non-zero; zero means
        // "no gradient supplied", not "arrive stopped".
        e.has_end_vel = (e.vel != 0.0f);
        return true;
    }
    if (rawDurMs == 0) return false;
    e.has_duration = true;
    e.duration_us  = uint32_t(rawDurMs) * 1000u;
    if (rawEndVel == kSegNoEndVel) {
        e.has_end_vel = false;
    } else {
        e.vel = float(rawEndVel) / 1000.0f;
        e.has_end_vel = true;
    }
    return true;
}

// ---- SimStepper -------------------------------------------------------------
// FastAccelStepper modeled at the MotorDriver seam. The firmware's 1 kHz sampler
// path ends in streamToSteps(target, speed, accel) -> FAS re-ramps from CURRENT
// velocity toward the micro-target under those ceilings. FAS's ramp generator is
// a trapezoidal follower; this is that follower in the mm domain (step
// quantization is 1/AIM_STEPS_PER_MM = 1/20.372 = 0.0491 mm — below anything the
// UI shows or the operator feels, so steps are not modeled). NOT modeled: RMT
// jitter, driver electrical behavior — roadmap §6 explicitly scopes those out.
class SimStepper {
public:
    void reset(float pos_mm) { _pos = pos_mm; _vel = 0.0f; _target = pos_mm; }

    // streamToSteps()/moveTo() seam: retarget with ceilings. Non-blocking,
    // replans from current velocity — exactly FAS's moveTo contract.
    void command(float target_mm, float speed_mm_s, float accel_mm_s2) {
        _target = target_mm;
        _vmax = speed_mm_s > 1.0f ? speed_mm_s : 1.0f;
        _amax = accel_mm_s2 > 1.0f ? accel_mm_s2 : 1.0f;
    }

    void hardStop() { _vel = 0.0f; _target = _pos; }  // forceStop seam (e-stop)

    // One integration step (dt in seconds; both callers run 1 ms substeps like
    // the firmware sampler). Classic trapezoidal servo: decelerate when the
    // stopping distance reaches the remaining distance, else run at vmax.
    void step(float dt) {
        const float dist = _target - _pos;
        const float adist = dist < 0 ? -dist : dist;
        if (adist < 0.005f && _vel * _vel < 2.0f * _amax * 0.005f) {
            _pos = _target;
            _vel = 0.0f;
            return;
        }
        const float dir = dist < 0 ? -1.0f : 1.0f;
        const float stopDist = (_vel * _vel) / (2.0f * _amax);
        float a;
        if (_vel * dir > 0 && stopDist >= adist) {
            a = -dir * _amax;                       // braking into the target
        } else {
            a = dir * _amax;                        // accelerate toward target
        }
        // A LOWERED SPEED CEILING IS RAMPED INTO, NOT TELEPORTED TO. This used
        // to be a hard clamp (`if (_vel > _vmax) _vel = _vmax`), which let the
        // modeled carriage shed velocity instantaneously the moment its ceiling
        // dropped — FastAccelStepper cannot do that, because the ramp generator
        // is bound by the acceleration limit and the motor is physically
        // stepping. Two places lower the ceiling mid-motion and both were
        // affected: crossing OUTSIDE the stroke window swaps the stream set for
        // the gentle user set (1000 -> 50 mm/s on the operator's numbers), and
        // velocity-matched stream mode re-derives the ceiling every single tick.
        // Measured before the fix: 18 samples at 250 000 mm/s^2 against a
        // 50 000 mm/s^2 follower ceiling, every one of them immediately after a
        // window exit — a deceleration the hardware could not produce, reported
        // by the bench as if it were physics.
        //
        // ONE acceleration budget per step, spent once. The velocity is computed
        // from the trapezoid, clamped to the ceiling, and then the total CHANGE
        // is limited to amax*dt — rather than letting the trapezoid and the
        // ceiling ramp each take a full bite, which made a decelerating carriage
        // that was also over its ceiling shed 2x amax.
        const float dv = _amax * dt;
        float v_want = _vel + a * dt;
        if (v_want > _vmax) v_want = _vmax;
        if (v_want < -_vmax) v_want = -_vmax;
        if (v_want - _vel > dv) v_want = _vel + dv;
        if (v_want - _vel < -dv) v_want = _vel - dv;
        _vel = v_want;
        _pos += _vel * dt;
    }

    float positionMm() const { return _pos; }
    float velocityMmS() const { return _vel; }
    float targetMm() const { return _target; }

private:
    float _pos = 0.0f, _vel = 0.0f, _target = 0.0f;
    float _vmax = 100.0f, _amax = 1000.0f;
};

// ---- The arbiter seam -------------------------------------------------------
// MotionArbiter::submitStreamSample, transcribed: everything between "the engine
// says go to normalized p" and "FAS is handed a target with ceilings". This is
// the part of the motion path a tuner MUST run the same way as the machine —
// the window clamp, the soft-start ramp and the speed-feed mode each change the
// rendered curve on their own, independently of anything in slopmotion::Config.

// The machine geometry + limit set the clamp reads. Held as a struct because
// every field is one the replayer also lets the operator move.
struct ArbiterGeometry {
    float win_min_mm = 0.0f, win_max_mm = 0.0f;
    // MotorDriver::effectiveCeilingMm — the MEASURED stroke once homed, else the
    // configured rail. The hard machine envelope, applied after the window.
    float ceiling_mm = 500.0f;
    float input_speed = 1000.0f, input_accel = 50000.0f;   // the STREAM set
    // Jerk is here with the other two INPUT ceilings even though applyArbiter
    // never reads it: it is an input-set limit, and deriveLimits() below needs
    // all three to reproduce the machine's normalized ceilings.
    float input_jerk = 2000000.0f;
    float user_speed = 100.0f, user_accel = 2000.0f;       // the GENTLE set
    uint8_t stream_speed_mode = kSpeedCeilingPegged;
    // WINDOW-ENTRY GENTLENESS, split in two — because the original rule bundled
    // a feature with a hazard.
    //
    // The SPEED half is the feature: a carriage parked outside the window is
    // about to be dragged to the clamped edge, and doing that at the input
    // ceiling is the "shoot to the window" lunge. Gentle speed is what
    // "glide, don't lunge" actually needs, and it is KEPT unconditionally.
    //
    // The ACCEL half was the hazard. MotionArbiter.cpp reduced BOTH ceilings
    // keyed on POSITION ALONE, so the rule written for "parked outside, glide
    // in" also fired for "just overshot, must stop NOW" — withdrawing exactly
    // the authority required. Measured on GoogleCat: a carriage 0.4 mm from a
    // clean stop crossed the 0.5 mm slack, lost 250x of its braking authority
    // in one tick, and traveled 625 mm on a 500 mm rail.
    //
    // BRAKING AUTHORITY IS NEVER WITHDRAWN. `false` (the default) is the fixed
    // behavior; `true` restores the firmware's original rule for A/B only.
    bool gentle_accel_outside = false;
    // ---- SAFETY FILTER (bench prototype, `lab`) -----------------------------
    // The braking-distance invariant, enforced CONTINUOUSLY on the speed the
    // follower is permitted to use:
    //
    //     v  <=  sqrt(2 * a * d)        d = distance to the wall being approached
    //
    // Below that surface the carriage can always stop before the wall, so the
    // window becomes FORWARD INVARIANT — leaving it is structurally impossible
    // rather than usually avoided. The bound is exactly TIGHT: differentiating
    // v_safe = sqrt(2*a*d) along the motion gives dv_safe/dt = -a when v = v_safe,
    // which is precisely the rate the follower can shed speed. So enforcing it
    // every sample is necessary and sufficient; enforcing it only at plan time
    // is not, which is the entire bug.
    //
    // slopmotion ALREADY OWNS THIS INEQUALITY — `applyEndVelGuard` is
    // |vf|^2 <= amax * dist_to_wall. It is applied ONCE, to the commanded end
    // velocity, at plan time. This is the same rule made continuous and moved to
    // the state. If it holds up, the +-0.02 legality grace and the
    // `gentle_accel_outside` collapse above both become DELETABLE rather than
    // tunable, which is the real prize.
    //
    // ON BY DEFAULT (operator ruling 2026-07-30) after a 90-config sweep:
    // runaways 18 -> 0, worst excursion 306.78 -> 0.45 mm, worst-case fidelity
    // cost +0.002 mm, mean fidelity GAIN 5.1 mm. It is a strict improvement —
    // free when it is not needed, large when it is. The switch stays so the
    // bench can still A/B it.
    //
    // ACCELERATION-ONLY, deliberately: it uses `a` and ignores the jerk ceiling,
    // which makes it slightly OPTIMISTIC about how fast the machine can really
    // stop. The jerk-exact version is available for free from
    // lib/ruckig/src/ruckig/brake.cpp and is the follow-on if the numbers say the
    // approximation matters.
    bool safety_filter = true;

    float span() const { return win_max_mm > win_min_mm ? win_max_mm - win_min_mm : 1.0f; }
    float normToMm(float n) const { return win_min_mm + n * span(); }
    float mmToNorm(float mm) const { return (mm - win_min_mm) / span(); }
};

struct StepperCommand {
    float target_mm = 0.0f, speed_mm_s = 0.0f, accel_mm_s2 = 0.0f;
};

// ---- Normalized engine ceilings, derived from the geometry ------------------
// The firmware glue's rule, in ONE place: the engine plans in NORMALIZED units
// (window fractions per second), so each mm-domain INPUT ceiling is divided by
// the stroke-window span. An override wins when > 0, exactly as the firmware's
// `jovr`/sm-set keys 2/3 behave — 0 means "derive".
//
// THIS IS WHY IT MOVED HERE. The replayer used to inherit whatever normalized
// limits the LIVE sim happened to hold and then let the operator move the
// window independently, so a bench window of 100 mm still planned at the limits
// derived for the live machine's 500 mm one. The engine was capped at 2.0/s
// while the same recording on a real 100 mm window would have had 10.0/s —
// five times the authority. Every policy looked bad because all five were
// fighting a ceiling the machine would not have had, and no policy choice can
// out-run a velocity limit. See MotionReplay.h: a tuner that renders a
// different curve than the machine is worse than no tuner.
inline slopmotion::Limits deriveLimits(const ArbiterGeometry& g,
                                       float vmax_override = 0.0f,
                                       float amax_override = 0.0f,
                                       float jmax_override = 0.0f) {
    slopmotion::Limits l;
    const float span = g.span();
    l.vmax = vmax_override > 0.0f ? vmax_override : g.input_speed / span;
    l.amax = amax_override > 0.0f ? amax_override : g.input_accel / span;
    l.jmax = jmax_override > 0.0f ? jmax_override : g.input_jerk / span;
    return l;
}

// SystemState::safeSpeedCap, transcribed (SystemState.h ~403-412): the ceiling
// ramps SAFE_APPROACH_SPEED_MM_S -> configured_max over SAFE_RESUME_RAMP_MS from
// the last resume stamp. `resume_start_ms` of 0 means "no discontinuity yet".
inline float safeSpeedCap(float configured_max, uint32_t resume_start_ms, uint32_t now_ms) {
    if (resume_start_ms == 0) return configured_max;
    const uint32_t dt = now_ms - resume_start_ms;
    if (dt >= kSafeResumeRampMs) return configured_max;
    if (configured_max <= kSafeApproachSpeed) return configured_max;
    const float f = float(dt) / float(kSafeResumeRampMs);
    return kSafeApproachSpeed + f * (configured_max - kSafeApproachSpeed);
}

// MotionArbiter::_isOutsideWindow (MotionArbiter.cpp ~315) — 0.5 mm slack, so a
// carriage sitting exactly on an edge does not chatter between the two limit
// sets sample after sample.
inline bool isOutsideWindow(const ArbiterGeometry& g, float p0_mm) {
    const float eps = 0.5f;
    return (p0_mm < g.win_min_mm - eps) || (p0_mm > g.win_max_mm + eps);
}

// `norm`/`vel_norm` are the engine's sampled position and velocity;
// `carriage_mm` is where the stepper ACTUALLY is (the window-entry test reads
// the machine, not the plan).
inline StepperCommand applyArbiter(const ArbiterGeometry& g, float norm, float vel_norm,
                                   float carriage_mm, uint32_t resume_start_ms,
                                   uint32_t now_ms) {
    StepperCommand out;
    // Window clamp, then the HARD machine envelope.
    out.target_mm = clampf(g.normToMm(clampf(norm, 0.0f, 1.0f)), g.win_min_mm, g.win_max_mm);
    out.target_mm = clampf(out.target_mm, 0.0f, g.ceiling_mm);

    // Window-entry gentleness: a carriage OUTSIDE the window glides in on the
    // USER (gentle) set, both speed AND accel.
    const bool entering = isOutsideWindow(g, carriage_mm);
    float speed_ceiling = entering ? std::min(g.input_speed, g.user_speed) : g.input_speed;
    // Safe-approach soft start ramps the CEILING back up after a discontinuity —
    // applied BEFORE the floor clamp below.
    const float safe_cap = safeSpeedCap(speed_ceiling, resume_start_ms, now_ms);
    if (safe_cap < speed_ceiling) speed_ceiling = safe_cap;
    out.accel_mm_s2 = (entering && g.gentle_accel_outside)
                          ? std::min(g.input_accel, g.user_accel)
                          : g.input_accel;
    const float speed_floor = std::min(kSafeApproachSpeed, speed_ceiling);

    if (g.stream_speed_mode == kSpeedVelocityMatched) {
        // FAS coasts the curve's own instantaneous speed.
        out.speed_mm_s = std::fabs(vel_norm) * g.span();
        if (out.speed_mm_s > speed_ceiling) out.speed_mm_s = speed_ceiling;
        if (out.speed_mm_s < speed_floor) out.speed_mm_s = speed_floor;
    } else {
        // DEVICE DEFAULT — ceiling-pegged: constant speed, the 1 ms micro-target
        // deltas shape the velocity. The follower keeps full authority, so it
        // can actually recover lag.
        out.speed_mm_s = speed_ceiling < speed_floor ? speed_floor : speed_ceiling;
    }

    // ---- the safety filter, applied LAST ------------------------------------
    // It caps the speed the follower may use, so it must sit after every other
    // rule that sets that speed — including the safe-approach FLOOR, which this
    // deliberately overrides. A floor that forces motion the carriage cannot
    // stop from is not a soft-start, it is the bug wearing a different hat.
    if (g.safety_filter) {
        // Distance to the wall in the direction of travel. `carriage_mm` is the
        // machine's ACTUAL position, never the setpoint: the invariant is a
        // statement about where the carriage can stop, and the setpoint cannot
        // stop anything.
        const bool going_up = out.target_mm >= carriage_mm;
        const float d = going_up ? (g.win_max_mm - carriage_mm) : (carriage_mm - g.win_min_mm);
        if (d <= 0.0f) {
            // ALREADY outside and still heading further out. Nothing to
            // preserve — let the follower brake at whatever authority it has.
            out.speed_mm_s = 1.0f;
        } else {
            const float v_safe = std::sqrt(2.0f * out.accel_mm_s2 * d);
            if (out.speed_mm_s > v_safe) out.speed_mm_s = v_safe;
        }
    }
    return out;
}

// ---- SenderCurve ------------------------------------------------------------
// The analyzer's "raw" line: what the CLIENT described, rebuilt from the
// client's own boundary conditions.
//
// It is advanced entirely in the SENDER'S frame — each segment starts where the
// PREVIOUS SEGMENT'S CURVE ENDED, not where the machine got to. That is the
// whole point: if it chased the machine it would silently absorb the planner's
// shortfalls and the raw-vs-planned gap would always read zero. Anchored to the
// machine's position once, at the first segment, because the two frames have to
// agree somewhere.
class SenderCurve {
public:
    void reset() { *this = SenderCurve{}; }

    // False until the first timed segment has fixed the sender frame's origin.
    // Callers MUST test this before sampling the engine for `anchor_pos` below:
    // slopmotion::Engine::snapshot() runs maybeSettle() and can transition the
    // engine, so sampling it on every segment "just to pass the argument" would
    // manufacture SettleEngaged anomalies that the machine never had.
    bool anchored() const { return _p >= 0.0; }

    // `cubic` selects the reconstruction family (the engine's resolved
    // CurvePolicy); `anchor_pos` is read ONLY when !anchored().
    void note(const slopmotion::Command& cmd, uint64_t now_us, bool cubic, float anchor_pos) {
        // Chase points carry no span, so there is no curve to draw through them —
        // leave the previous one to finish rather than inventing a shape.
        if (!cmd.has_duration || cmd.duration_us == 0) return;
        const double T = double(cmd.duration_us) * 1e-6;
        const double target = double(clampf(cmd.target, 0.0f, 1.0f));
        if (_p < 0.0) {                       // anchor the sender frame once
            _p = double(anchor_pos);
            _v = 0.0;
        } else if (_T > 0.0) {
            // THE FRAME IS WHERE THE PREVIOUS CURVE REACHED, NOT THE ENDPOINT IT
            // WAS AIMED AT. A segment can be SUPERSEDED in flight — MFP's
            // seek/resume path emits a fresh segment with a zero offset, and
            // GoogleCat contains a pair of them 9 ms apart — and advancing the
            // frame to an endpoint the curve never reached teleports the
            // reference line. Measured: raw stepped 143.67 -> 110.00 mm in one
            // sample at t=2.878 s while the carriage was mid-stroke, and the
            // whole 33.95 mm `sender_max` on that take was this jump. The
            // machine was tracking its command correctly the entire time.
            //
            // A span that DID run to completion evaluates to its endpoint here,
            // so the normal chained case is unchanged — this only bites the
            // superseded one, which is exactly where the old rule was wrong.
            double rt = now_us >= _start_us ? double(now_us - _start_us) * 1e-6 : 0.0;
            if (rt > _T) rt = _T;
            double rp, rv, ra;
            slopmotion::Engine::evalCurve(_c, _T, rt / _T, rp, rv, ra);
            _p = rp;
            _v = rv;
        }
        const double vf = cmd.has_end_vel ? double(cmd.end_vel) : 0.0;
        // af mirrors the engine's OWN backward-difference estimator
        // (commitWaveform) so that raw and planned differ by FEASIBILITY only,
        // never because the two lines guessed the sender's curvature
        // differently. Ignored outright in C1, where a cubic takes no end
        // acceleration.
        double af = 0.0;
        if (cmd.has_end_vel && _prev_ok && now_us > _prev_us) {
            const double gap = double(now_us - _prev_us) * 1e-6;
            if (gap < 3.0 * T) af = (vf - _prev_vf) / gap;
        }
        // Start acceleration is 0 in the sender's frame: the wire carries no
        // such field, and a C1 cubic ignores it entirely. It only shades the C2
        // line.
        slopmotion::Engine::senderCurve(cubic, _p, _v, 0.0, target, vf, af, T, _c);
        _T        = T;
        _start_us = now_us;
        // _p/_v are NOT advanced here: they are the curve's own start, and the
        // next note() re-derives the frame from `_c` at that instant (above).
        _prev_vf  = vf;
        _prev_us  = now_us;
        _prev_ok  = cmd.has_end_vel;
    }

    // Advanced on the CALLER'S clock (not the machine's progress) and HELD at
    // its endpoint once the span expires, which is what makes an overrun visible
    // as raw-flat-while-planned-still-moving. -1 until the first timed segment.
    float sampleAt(uint64_t now_us) {
        if (_T > 0.0 && now_us >= _start_us) {
            double rt = double(now_us - _start_us) * 1e-6;
            if (rt > _T) rt = _T;
            double rp, rv, ra;
            slopmotion::Engine::evalCurve(_c, _T, rt / _T, rp, rv, ra);
            _norm = float(rp < 0.0 ? 0.0 : (rp > 1.0 ? 1.0 : rp));
        }
        return _norm;
    }

    float lastNorm() const { return _norm; }

private:
    double   _c[6] = {0, 0, 0, 0, 0, 0};
    double   _T = 0.0;
    uint64_t _start_us = 0;
    double   _p = -1.0;        // sender-frame position; <0 = not anchored yet
    double   _v = 0.0;         // sender-frame velocity at the last knot
    double   _prev_vf = 0.0;   // for the af backward difference
    uint64_t _prev_us = 0;
    bool     _prev_ok = false;
    float    _norm = -1.0f;
};

}  // namespace slopsim
