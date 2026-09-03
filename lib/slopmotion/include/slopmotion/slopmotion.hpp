// SlopMotion — jerk-limited dual-mode motion core (quintic waveform + Ruckig).
//
// PURPOSE
// -------
// The jerk-limited motion core: every incoming
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
//   * WAVEFORM (TCode v4 / any point carrying I<ms> ≥ 20 ms): a QUINTIC
//     Hermite segment from the current (p,v,a) to (target, G-velocity,
//     af-estimate) over exactly the commanded duration. C2 where the old
//     cubic was C1 — the boundary accel spike class is gone — and it
//     reproduces the sender's spline faithfully (that was the cubic's one
//     virtue; we keep its soul, fix its sins). Every quintic is CEILING-
//     SCANNED at plan time; a segment that demands more than vmax/amax/jmax
//     (or leaves the window) is handled by InfeasiblePolicy: RESHAPE
//     (default) keeps the deadline AND as much of the stroke as the machine
//     can physically deliver by giving up as little of the quintic's SHAPE as
//     the physics demands (see SHARPNESS BEFORE AMPLITUDE below), SCALE keeps
//     the deadline and shrinks the stroke to fit a min-jerk quintic, STRETCH
//     keeps the stroke and hands the segment to the Ruckig guard, which runs
//     it at the physical minimum and flags the anomaly. Either way an
//     infeasible deadline moves AS FAST AS THE CEILINGS ALLOW, never lagging
//     below them, and never trusting an absurd wire command verbatim. Which
//     fidelity to sacrifice depends on the sender — see InfeasiblePolicy.
//     Whichever of the two timing-first policies is armed, the shortened band
//     is then held on the COMMANDED MIDPOINT by the centering debt (Config::
//     wave_centering) — including, deliberately, by shortening strokes the
//     machine could have completed, because delivering one direction in full
//     while the other is clipped is exactly what walks the band off center.
//     Every such stroke reports a WaveformCentered anomaly.
//     Since 0.7.0 a waveform segment may also arrive with ONE-SEGMENT
//     LOOKAHEAD (Command::next_chord), which arms the RFC-008 handoff sanity
//     guard: the sender's end velocity is bounded to the Fritsch-Carlson knot
//     limit of the segment that FOLLOWS, so an infeasible handoff is caught
//     BEFORE it forces the shape/amplitude/deadline sacrifices above. See
//     boundHandoffVelocity.
//   * CHASE (TCode v3 bare high-rate points, or I < 20 ms): the future is
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
//     clock loop. Guarded by a GRACE WINDOW (Config::settle_grace_us): a
//     stream whose next command is merely a few ms late is not a starved
//     stream, and braking on ms-scale transport jitter is worse than holding
//     (see maybeSettle).
//
// SHARPNESS BEFORE AMPLITUDE (0.6.0 — the operator's hybrid)
// ----------------------------------------------------------
// JERK IS THE SHAPE PARAMETER. A min-jerk quintic is the smooth extreme of a
// timed move (peak velocity = 1.875 × mean); a pure trapezoid is the sharp
// extreme (peak → 1.0 × mean, i.e. a straight line held at velocity
// saturation); everything in between is a jerk-limited double-S, which is
// exactly what Ruckig generates. Low jerk ⇒ long accel ramps ⇒ round profile
// ⇒ a HIGHER peak velocity is needed to cover the same distance in the same
// time. High jerk ⇒ short ramps ⇒ flat top. The two are one continuous dial,
// and the dial is the jerk ceiling handed to the planner.
//
// Before 0.6.0 an infeasible timed segment made a BINARY choice: keep the
// smooth quintic and SHORTEN the stroke (Scale), or keep the amplitude and
// take Ruckig's profile planned at the FULL mechanical jmax — short ramps,
// long flat top, visibly a straight line (Reshape). The operator, watching
// this at 500 mm/s where nearly every segment crosses the threshold, asked
// for the continuous middle: "adjust the slope factor, so it stays smooth
// when close to max speed, and straightens out the further it is away."
//
// So RESHAPE now spends SHARPNESS before it spends AMPLITUDE:
//   1. Quintic fits at the real ceilings → smooth, full amplitude, on time.
//      Unchanged.
//   2. It does not fit, but the MACHINE can still make the whole stroke by the
//      deadline → keep the full amplitude and the exact deadline, and plan at
//      the SMALLEST jerk ceiling that still gets there in time (see
//      softestFeasibleJerk). Only as sharp as the physics demands.
//   3. Only when the true mechanical jmax is already spent and the stroke
//      STILL does not fit does the endpoint shorten (the existing bisection),
//      and the centering debt then decides how much of that reach this side of
//      the band gets.
// The mechanical jmax from Config::limits is a hard ceiling this feature
// spends UP TO and never past — softening only ever makes a plan gentler than
// the one 0.5.0 would have run, never more aggressive.
//
// MEASURED (operator's window 200 mm, amax 50000 mm/s², jerk 2e6 mm/s³, i.e.
// normalized amax 250 / jmax 1e4), full amplitude and deadline held in every
// row, "sat" = fraction of the move spent at velocity saturation (the straight
// line the operator can see), pk/mean = peak/mean velocity ratio (1.0 square,
// 1.875 min-jerk quintic, higher is rounder):
//     vmax  T      stroke   0.5.0 (jmax)              0.6.0 (softened)
//     5.0   167ms  0.50     j 10000  sat 57%  1.311   j 4455  sat 28%  1.670
//     5.0   250ms  0.70     j 10000  sat 74%  1.169   j 1653  sat 21%  1.786
//     2.5   250ms  0.35     j 10000  sat 82%  1.111   j  826  sat 21%  1.786
//     2.5   400ms  0.70     j 10000  sat 88%  1.074   j  694  sat 46%  1.429
// Peak ACCELERATION falls with the flat top (196→147, 180→91, 120→46 on those
// rows): the rounder profile is gentler on the mechanism as well as on the
// operator, and its only cost is a higher peak VELOCITY — bounded by vmax, by
// construction, because vmax hitting is precisely what stops the search.
//
// WHERE IT DOES NOT HELP, said plainly: when the stroke is so over-budget that
// even jmax cannot deliver it on time, step 3 runs and the endpoint it finds
// is one where jmax is MARGINAL — there is no sharpness left to give back,
// because maximum amplitude at a fixed deadline IS the flat profile. The
// operator's own headline chain (0.30↔1.00 in 167 ms = 140 mm) is in exactly
// that regime at both 500 and 1000 mm/s: at full amplitude it needs 838 mm/s
// mean against a 500 mm/s ceiling. Softening fires there only on the strokes
// the CENTERING debt pulls in (which have slack again by construction). If you
// want curve back in that regime you have to buy it with amplitude — that is
// InfeasiblePolicy::Scale, and it is a different intent, not a tuning of this
// one.
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
// Single-threaded by contract: ALL of
// commit/positionAt/velocityAt/snapshot run on Core 1 (the sampler task).
// Cross-core command handoff stays OUTSIDE this class. No locks, no heap in
// steady state (Ruckig's waypoint vectors stay empty in community mode).
//
// Hardware-free: std headers + vendored lib/ruckig only. Native-tested in
// test/native/test_slopmotion; scenario bench in examples/slopmotion_traces. :3
#pragma once

#include <cstdint>
#include <cmath>
#include <optional>

#include <ruckig/ruckig.hpp>

namespace slopmotion {

inline constexpr const char* kVersion = "0.8.0";

// ---- Configuration ----------------------------------------------------------

// Kinematic ceilings in normalized units (per second^n). The firmware glue
// derives these from the mm-domain input limit set / stroke window length.
struct Limits {
    float vmax = 3.0f;     // units/s   (3 = three full strokes per second)
    float amax = 30.0f;    // units/s^2
    float jmax = 500.0f;   // units/s^3
};

// What to sacrifice when the wire commands a move the machine physically
// cannot execute in the commanded duration. This is a statement of INTENT, not
// a tuning knob: both answers are correct, for different senders.
//
//   Stretch — range-first. Keep the whole stroke, overrun the deadline. Right
//             for a sender whose amplitude is the content (manual point moves,
//             "go here" commands). This is the pre-0.3 behavior.
//   Scale   — timing-first, SHAPE-first. Keep the deadline, keep the min-jerk
//             quintic, shrink the stroke around the current position until
//             the quintic fits. Right for a SCHEDULED sender (funscript
//             segments over SlopSync 0x0085): the next segment arrives on its
//             own clock regardless of whether we finished, so an overrun plan
//             is PREEMPTED mid-flight. Measured on the virtual machine (window
//             500 mm, vmax 1.1, amax 16): a 0→1 stroke chain at 400 ms/segment
//             under Stretch achieved only 36 % of the commanded amplitude
//             (181 mm of 500) AND ran phase-lagged — degenerate, it loses
//             range and timing. Scale loses only range, honestly and visibly
//             (WaveformScaled anomaly carries the achieved fraction).
//   Reshape — timing-first, MACHINE-first (DEFAULT). Keep the deadline and as
//             much of the stroke as the MACHINE can physically deliver in it,
//             paying with the quintic's shape instead of with amplitude.
//
//             WHY THIS EXISTS (measured, operator's machine — window 200 mm,
//             1000 mm/s, 50000 mm/s², jerk 2e6 → vmax 5, amax 250, jmax 1e4):
//             Scale sizes the stroke with the min-jerk quintic's own peak/mean
//             ratios, so it asks "how far can a QUINTIC reach in T", not "how
//             far can the MACHINE reach in T". Those are very different
//             questions when the deadline is tight, because a quintic spends
//             its whole span accelerating and decelerating — it never cruises.
//             For a real funscript segment of 140 mm in 167 ms the mean speed
//             required is only 838 mm/s against a 1000 mm/s ceiling, yet the
//             quintic envelope permits just 82 mm (59 %). A flat-top profile
//             covers 122 mm in the same 167 ms (87 %). Scale was shrinking the
//             stroke to fit a SHAPE, not to fit the machine.
//
//             Reshape asks Ruckig the honest question instead: the TIME-
//             OPTIMAL duration of the commanded stroke. If that fits inside
//             the deadline, the full stroke runs stretched to exactly the
//             deadline (full amplitude, exact timing, and — since 0.6.0 —
//             only as much of the bang-cruise-bang shape as the deadline
//             actually forces; WaveformFallback, no WaveformScaled: nothing
//             was lost but some of the spline). If it does not fit, the
//             endpoint is bisected toward the
//             segment's MIDPOINT until the time-optimal duration fits, then
//             stretched to the deadline (WaveformScaled with the achieved
//             fraction, same contract as Scale).
//
//             SHARPNESS FIRST (0.6.0): whichever branch runs, the adopted
//             endpoint is then planned at the SOFTEST jerk ceiling that still
//             reaches it in time, so the shape is given up by degrees instead
//             of all at once — see the header's "SHARPNESS BEFORE AMPLITUDE"
//             note and softestFeasibleJerk. Reshape is still machine-first:
//             not one unit of amplitude is traded for smoothness, ever.
//
//             Cost: 2 Ruckig calculate() calls when the stroke fits, up to
//             2 + infeasible_reshape_steps when it must bisect, plus
//             infeasible_soften_steps when the sharpness search runs (which is
//             exactly the branches that have slack to spend — see the search's
//             own note). Plan-time only, on an event that just failed the
//             quintic scan anyway.
//
// ---- BUDGETED POLICIES (0.8.0) — the two the operator actually reasons about
//
//   PrioritizeAmplitude — spend SMOOTHNESS first, up to infeasible_smooth_budget,
//                         then start spending amplitude.
//   PrioritizeSmooth    — spend AMPLITUDE first, up to infeasible_amplitude_budget,
//                         then start spending smoothness.
//
// Both are identical to no policy at all until a segment is infeasible. Both stay
// inside the QUINTIC family the whole way down, which is the point: the sender's
// curve degrades CONTINUOUSLY toward a straight line instead of snapping to a
// bang-cruise-bang chord the moment the legality scan fails. Reshape's cliff (any
// failure ⇒ full Ruckig profile ⇒ flat-topped velocity) is exactly the "static
// interpolation" artifact the operator identified on hardware.
//
// THE SMOOTHNESS AXIS — what "spend smoothness" means, precisely. A span's shape
// is set by its boundary HANDLES (tangent magnitudes). Lerping the end handle
// toward the span's own CHORD SLOPE, by a fraction alpha, walks the curve
// continuously away from the sender's spline (alpha = 0) toward the flattest
// shape this span can take. Taking a REST-TO-REST span as the clean case:
//     handle >> chord  → overshoot bulge, peak |v| at the ENDS  (Makima on a
//                        shallow span: measured 36x the span's mean velocity)
//     handle == chord  → straight line, peak |v| = 1.0x mean, peak |a| = 0
//     handle == 0      → smoothstep S-curve, peak |v| = 1.5x mean, mid-span
// so the operator reduces peak demand from EITHER side of the chord, which is
// what makes it a correct feasibility knob rather than a one-directional fudge.
//
// alpha = 1 IS NOT A LITERAL STRAIGHT LINE, and the difference matters. Only the
// END handle is ours to move (see below) — the START is the machine's actual
// (v, a). So alpha = 1 means "the straightest quintic reachable FROM THE STATE
// THE MACHINE IS ACTUALLY IN", which is a true straight line only when the
// machine already happens to sit on the chord. In a steady segment chain it gets
// close (the previous span ended at ITS blended vf), and it degrades gracefully
// rather than exactly. Do not describe this as reaching linear interpolation.
//
// WHY CAP IT AT ALL, then. Not "to preserve C1" — because each plan is rebuilt
// from live state, the EXECUTED motion never steps regardless of alpha. The real
// cost is twofold and both parts are gradual: (1) past some alpha the machine has
// simply stopped reproducing the sender's curve, which is the whole product; and
// (2) arriving at the chord slope instead of the script's tangent wrong-foots the
// FOLLOWING span, which then starts further from where its own shape wanted to
// begin. The budget is where the operator decides those costs outweigh the
// amplitude they would otherwise spend. It is a real tradeoff dial, not a
// safety limit — and it wants measuring on hardware, not deriving.
//
// WHAT THE SMOOTHNESS BUDGET CAN AND CANNOT BUY. A min-jerk quintic peaks at
// 15/8 = 1.875x its mean velocity; a straight line at 1.0x. So the entire
// smoothness budget is worth a 1.875x velocity headroom factor and not one unit
// more, because MEAN velocity is |target-p|/T with both terms pinned by the
// sender. Once mean velocity alone exceeds the ceiling, no handle length on
// earth helps and amplitude MUST go — which is why PrioritizeAmplitude still
// needs an amplitude fallback, and why the both-budgets-exhausted case is
// ROUTINE rather than exotic: it is simply "conservative limits + aggressive
// script", the most common infeasible state in the field.
//
// TERMINAL CASE (both budgets spent, still illegal): fall through to the Ruckig
// guard exactly as before — deliver the whole stroke late, DeadlineStretched.
// That is the honest "your machine cannot do this" answer and it already exists.
//
// ONLY THE END HANDLE MOVES. The start (p, v, a) is the machine's ACTUAL state,
// not a number we are free to invent — the motion doctrine's "plan from actual
// state" is not negotiable for feasibility's convenience. That is sufficient:
// the reduction PROPAGATES, because segment N's blended vf becomes segment N+1's
// actual starting v.
enum class InfeasiblePolicy : uint8_t {
    Stretch = 0,   // range-first: keep the full stroke, overrun the deadline
    Scale   = 1,   // timing-first + shape-first: shrink the stroke to a quintic
    Reshape = 2,   // timing-first + machine-first: give up the shape, not the range
    PrioritizeAmplitude = 3,   // budgeted: smoothness first, then amplitude
    PrioritizeSmooth    = 4,   // budgeted: amplitude first, then smoothness
    // ONE SLIDER instead of a choice of four. Spends BOTH axes together in the
    // ratio `infeasible_blend` sets, and only as far as legality demands, so an
    // infeasible segment degrades PROPORTIONALLY rather than by exhausting one
    // axis. Reproduces PrioritizeSmooth at blend 0 and PrioritizeAmplitude at
    // blend 1; the interior is what the other four cannot express.
    Blend               = 5,
};

// THE ORDINAL OF THE LAST POLICY, and the ONE home for it. Every wire clamp,
// NVS load clamp and name table off the engine is a restatement of this number,
// and restating it is how a policy ships unreachable: 0.9.0 added Blend and left
// four device-side tables plus three bounds at 4, so an operator selecting blend
// (5) was CLAMPED to 4 and silently got prio-smooth. Adding a policy means
// bumping this and letting the compiler find the rest — never editing a literal.
inline constexpr uint8_t kInfeasiblePolicyMax = (uint8_t)InfeasiblePolicy::Blend;

// Which CURVE FAMILY the waveform path reconstructs a segment with.
//
// WHY THIS EXISTS. A funscript rendered through Pchip or Makima is a C1 CUBIC
// HERMITE spline: those two interpolators differ ONLY in the rule they use to
// pick knot tangents, and given endpoint positions plus endpoint tangents the
// cubic on a span is uniquely determined. So {target, duration, end_vel} on
// channel 0x0085 is not a lossy summary of the sender's curve — for any C1
// cubic-Hermite family it is a COMPLETE ENCODING of it. Nothing is missing from
// the wire; what differs is what we rebuild it WITH.
//
// A C2 quintic CANNOT reproduce a C1 cubic across a knot, by construction: the
// script's acceleration genuinely STEPS there, and a C2 curve is required to
// make curvature continuous. Rounding that step off is not smoothing noise, it
// is deleting script content — and the `af` the quintic needs is not even on the
// wire, so it is estimated as a backward difference of consecutive handoff
// velocities, i.e. an estimate of a quantity that is genuinely TWO-VALUED at the
// knot it is estimated at. The operator identified this on hardware as motion
// that tracked the script's positions but not its character.
//
//   FollowClient — the sender declares its curve family and we honor it. NO
//                  WIRE SIGNALING EXISTS YET (that is the pending RFC), so with
//                  nothing declared this resolves to C2 — i.e. today's behavior,
//                  byte for byte. This value is the safe default and stays that
//                  way; when the RFC lands, only the resolution changes.
//   ForceC1      — always cubic. Reproduces a Pchip/Makima span exactly.
//   ForceC2      — always quintic. The pre-0.8.0 engine.
//
// SCOPE — READ BEFORE MOVING THE CALL SITES. C1 applies to WAVEFORM SEGMENT
// commits only, never to chase points, settle, or the Ruckig guard. A cubic
// takes four boundary conditions (p, v) → (target, vf) and therefore DROPS the
// machine's current acceleration `a`, so a plan begins with an acceleration STEP
// relative to what the carriage is actually doing. At a script knot that step is
// exactly right — the script has one there too. Anywhere else (a preemption
// mid-span, a chase replan, a settle) it would be a spurious torque step with no
// authoring behind it, which is why those paths keep the quintic.
//
// THE STEP IS DELIBERATELY UNBOUNDED. Nothing clamps it, because clamping it is
// precisely "manufacture curvature continuity the script did not ask for" — the
// thing C1 mode exists to stop doing. The jerk ceiling remains a MACHINE SAFETY
// limit and is not repurposed as a shape control. Consequence worth knowing
// before hardware: a knot between two very differently-sloped spans commands a
// large torque step, and how that feels is a measurement, not a derivation.
enum class CurvePolicy : uint8_t {
    FollowClient = 0,   // honor the sender's declared family (RFC-030)
    ForceC1      = 1,   // cubic Hermite — reproduces the script's own spline
    ForceC2      = 2,   // quintic — curvature-continuous
};

// WaveformCommand::client_curve_family's "c1_cubic". Mirrored rather than
// included: this header stays slopsync-free, so the registry numbering is
// documented (see WaveformCommand) and restated here, never imported.
inline constexpr uint8_t kClientCurveC1Cubic = 1;

// policy + declaration -> reconstruction family, in ONE place. Free and public
// because a caller can need the answer BEFORE commitWaveform adopts the command:
// the bench's sender-curve overlay draws the client's own curve at commit time,
// and an overlay that picks its family by a private re-derivation is how a tuner
// ends up comparing a cubic against a quintic and blaming the planner for the
// difference.
constexpr bool resolveCubic(CurvePolicy policy, uint8_t client_family) {
    switch (policy) {
        case CurvePolicy::ForceC1: return true;
        case CurvePolicy::ForceC2: return false;
        case CurvePolicy::FollowClient: return client_family == kClientCurveC1Cubic;
    }
    return false;
}

struct Config {
    Limits limits{};
    // Bare-point synthesis: hold back ONE sample and render [buffered ->
    // incoming] as a full waveform segment (duration = stamp spacing, end_vel
    // = PCHIP knot tangent, successor chord armed). Samples inherit the whole
    // waveform machine -- C2 chain, anchored commits, dwell/RFC-008/wall
    // guards, feasibility policies -- for one interval of latency. PCHIP
    // tangents are zero at reversals (arrive at rest, overshoot impossible)
    // and Fritsch-Carlson bounded on runs. Gaps > chase_stale_us restart via
    // plain chase, so sparse streams never pay the holdback.
    // Hot chase entries fall back to chase for a knot and re-lock at a
    // benign one; near-ceiling streams chase by design (no headroom). A
    // longer first span is NOT the fix: any span whose duration differs
    // from the knot cadence desynchronizes the chain and corrupts pace.
    bool sample_synthesis = true;
    // Chase jerk scales with the MOVE's own demand: a replan corner spends
    // jerk proportional to demand/vmax (kneed at kChaseJerkKneeFrac) instead
    // of the mechanical ceiling, so slow content stops carrying a 50 Hz notch
    // train while fast content keeps full authority. Softer-only by
    // construction (jerkCeil).
    bool  chase_jerk_scale = true;
    float chase_jerk_floor = 0.15f;   // never below this fraction of jmax
    // Cold-start governor. The FIRST plan out of rest (Idle/Settle, v~0) is a
    // POSITIONING move -- park to the stream's opening position -- not
    // content; planned at limits.vmax it shoots across the window (the
    // script-start dart, sd-d77). That one commit clamps vmax here; the
    // deadline guards then stretch duration instead. 0 = disabled.
    float recovery_vmax = 0.0f;

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
    // Second-order predictive aim. The first-order aim (target + v_est·look)
    // extrapolates along a STRAIGHT line, which is exactly wrong where a
    // waveform turns: approaching a crest the velocity EMA still reads the
    // pre-crest positive velocity, so the aim is thrown PAST the crest.
    // Measured against a source sine cresting at the rail: aim overshot the
    // ideal crest by +0.022, clamped to 1.0, and applyEndVelGuard then saw
    // dist-to-wall = 0 → vf forced to 0 → the carriage PARKED at the rail,
    // dead stop, 127–138 ms per stroke (the source sine's own tangency dwell
    // is 0.35 % — ~8x excess flat time), plus clustered Ruckig PlanFailed
    // drops through the approach. Adding the ½·a_est·look² term pulls the aim
    // back exactly where the stream is turning, because a_est is negative
    // there. Uses the SAME acap-limited estimate as the af feedforward so a
    // noisy second difference cannot fling the aim.
    // This flag gates the WHOLE second-order idea ("predictive aim v2"): the
    // ½·a_est·look² aim term AND the matching arrival-velocity extrapolation
    // (arrive at v_est + a_est·look, the velocity the stream will have when we
    // get there — not the one it has now). Off = pre-v2 behavior, byte for
    // byte.
    bool     chase_aim_accel_extrap = true;
    // Streams with mean interval above this are NOT dense: no feedforward, no
    // extrapolation — isolated points plan a plain point-chase to rest.
    // (The legacy LIVE_TRIGGER idea, relaxed to admit 20 Hz app streams.)
    uint32_t chase_dense_us    = 60000;
    // Estimator resets after a stream gap this long.
    uint32_t chase_stale_us    = 400000;

    // ---- Infeasible-segment handling (WAVEFORM path only) -------------------
    // OPERATOR RULING 2026-07-30, measured on the async-tune bench (GoogleCat,
    // 50-150 mm window, curve follow -> c1, 1000 mm/s / 50000 mm/s2): Stretch
    // wins on BOTH axes at once, which none of the other four do —
    //   stretch 0.761 rms /  14 anomalies      prio-smooth    0.747 / 50
    //   reshape 0.928 rms /  29 anomalies      prio-amplitude 0.765 / 40
    //   scale   1.647 rms /  31 anomalies
    // Fidelity across the top three is a 0.02 mm tie; the anomaly count is not,
    // and Stretch also sidesteps the soften overshoot (LEDGER, pending ruling)
    // because that only fires on Reshape.
    // SUPERSEDED 2026-07-30 by Blend, which is the same decision made
    // continuously instead of by picking a corner — see infeasible_blend for the
    // 60-case sweep. Blend at its best setting beats Stretch on the same
    // objective (regret 0.226 vs 0.248), and unlike Stretch it does not overrun
    // the deadline to do it. Stretch remains selectable and remains the best of
    // the four SEQUENTIAL policies.
    InfeasiblePolicy infeasible_policy = InfeasiblePolicy::Blend;
    // Safety factor on the SCALE policy's stroke size estimate. The estimate
    // is a heuristic, the legality scan is the referee — the margin just
    // biases the first guess low so the scan usually accepts on an early try.
    // Clamped to [0.50, 1.00] on use (a config push is not a trusted input).
    float infeasible_scale_margin = 0.92f;
    // RESHAPE bisection depth. Each step halves the remaining stroke interval,
    // and the interval is the FULL commanded stroke (the search runs from
    // "stay put" to "full amplitude"), so N steps resolve the delivered stroke
    // to stroke/2^N: 6 steps = 1/64 ≈ 2 mm on a 140 mm stroke — below the
    // resolution of anything the operator can feel, and below the 1 % wire
    // quantum of the 0x0085 target field. Each step costs ONE Ruckig
    // calculate(), so this knob is a direct plan-time dial; CLAMPED to [0, 8]
    // on use, because plan time is a budget, not a matter of taste (0 disables
    // the bisection entirely: full amplitude when it fits, guard when it does
    // not).
    uint8_t infeasible_reshape_steps = 6;

    // Curve family for waveform-segment reconstruction. FollowClient is today's
    // behavior byte for byte until the curve_family wire signaling lands.
    CurvePolicy curve_policy = CurvePolicy::FollowClient;

    // ---- Budgeted policies (PrioritizeAmplitude / PrioritizeSmooth) ---------
    // How much of each axis the policy may spend before it switches to the
    // other one. Both are FRACTIONS in [0, 1], clamped on use (a config push is
    // not a trusted input).
    //
    // infeasible_smooth_budget — max alpha: how far the end handle may be
    //   lerped toward the chord slope. 0 = never touch the sender's curve
    //   (degenerates to pure amplitude spending); 1 = the flattest quintic
    //   reachable from the machine's actual state, which is NOT the same thing
    //   as a straight line (see InfeasiblePolicy). 0.5 is the operator's
    //   Blender-derived starting point: halving the handle length was enough to
    //   pull a bulged span back parallel to its own chord.
    //
    // infeasible_amplitude_budget — max fraction of the COMMANDED stroke that
    //   may be surrendered. 0.5 = may shrink to the segment midpoint (the
    //   midpoint-anchored geometry's f = 0); 1.0 = may decline to move at all.
    //   Kept at 0.5 by default because a stroke shortened past its own midpoint
    //   has stopped being the motion the script described.
    float infeasible_smooth_budget    = 0.5f;
    float infeasible_amplitude_budget = 0.5f;
    // Bisection depth on the alpha search. Each step costs ONE quintic build +
    // one legality scan (no Ruckig call), so this is far cheaper per step than
    // infeasible_reshape_steps. CLAMPED to [1, 10] on use. 6 resolves alpha to
    // 1/64 of the budget, well under anything perceptible.
    uint8_t infeasible_blend_steps = 6;

    // ---- InfeasiblePolicy::Blend — the one slider ---------------------------
    // WHAT AN INFEASIBLE SEGMENT GIVES UP, as a single exchange rate:
    //   0.0  keep AMPLITUDE nothing, keep SHAPE everything — surrender reach
    //   1.0  keep AMPLITUDE everything, surrender shape (flattens toward the chord)
    //   0.5  both give equally
    // Clamped [0, 1] on use; a config push is not a trusted input.
    //
    // 0.5 BY MEASUREMENT, and the honest version of that claim: swept over 10
    // recordings x 6 perturbations (window tight/wide/offset, halved speed,
    // weakened accel) = 60 cases, 42 of which actually exercise the policy.
    // Scored as scale-free per-case regret on the operator's own objective —
    // shape match AND amplitude, (1-shape_corr) and |1-reach_ratio|.
    //
    // THE OPTIMUM MOVES WITH THE EXCHANGE RATE, which is exactly why this is a
    // slider and not a constant:
    //   shape weighted ~10x reach -> 0.875 (regret 0.236)
    //   weighted EQUALLY          -> 0.5   (regret 0.239)
    //   reach weighted >= shape   -> 0.5   (regret 0.197 / 0.110)
    // 0.5 is the equal-weight optimum and wins 3 of the 5 weightings tried, so
    // it is the neutral default; 0.875 is the pick if shape matters much more.
    //
    // WHAT IS NOT AMBIGUOUS: the low end is wrong. blend <= 0.25 scores regret
    // ~0.72 against ~0.24 at the top — surrendering AMPLITUDE first is a bad
    // trade almost everywhere, because a small shape concession keeps the plan
    // FEASIBLE while lost reach is both visible and often still falls through to
    // the Ruckig guard. The two cases where 0.0 won had a spread of 0.17, i.e.
    // they were ties.
    float infeasible_blend = 0.5f;

    // ---- Sharpness-first reshaping (RESHAPE only) ---------------------------
    // "Is there a hybrid between scale and stretch where we just adjust the
    // slope factor, so it stays smooth when close to max speed, and straightens
    // out the further it is away?" (operator, verbatim.) Yes: jerk is the slope
    // factor. See the header's SHARPNESS BEFORE AMPLITUDE note for the
    // mechanism and the measured table, and softestFeasibleJerk for the search.
    //
    // OFF restores the 0.5.0 contract exactly: every reshape plan is computed
    // at the full mechanical jmax, flat top and all. This knob is the whole
    // feature's off switch — nothing else in the file changes behavior with
    // it clear.
    //
    // The feature is RESHAPE-only on purpose. Scale plans a QUINTIC, whose
    // shape is fixed by its boundary conditions — there is no jerk ceiling to
    // spend there, only amplitude, which is exactly what Scale already trades.
    // Stretch is untouched for the same reason it is exempt from centering: it
    // promises the whole stroke and nothing else.
    bool     infeasible_soften       = true;
    // The SMOOTH END of the sharpness dial, as a fraction of the mechanical
    // jmax. Softening searches [floor·jmax, jmax] and never leaves it, so this
    // is "how round is the engine ALLOWED to get" — lower = more range for the
    // search to work in, at the cost of resolution per step (the interval is
    // searched in LOG space, so the two trade directly). 0.02 = a 50:1
    // sharpness range, which covers everything measured on the operator's
    // machine (the softest useful critical jerk seen was 6.9 % of jmax, on a
    // 700 mm/s-mean 400 ms segment) with 6 steps still resolving jerk to ~6 %.
    // Clamped to [0.001, 1.0] on use; 1.0 pins the dial at the sharp end
    // (equivalent to `infeasible_soften = false`, just via the analog knob).
    float    infeasible_soften_floor = 0.02f;
    // Sharpness bisection depth — ONE Ruckig calculate() per step, so this is
    // the second plan-time dial next to infeasible_reshape_steps. Resolution is
    // geometric: N steps resolve the effective jerk to a factor of
    // (1/floor)^(1/2^N), i.e. 6 steps ≈ 6 % at the default floor, which is far
    // finer than anything the profile shape reveals. CLAMPED to [0, 10] on use;
    // 0 disables the search (plan at jmax, 0.5.0 behavior).
    uint8_t  infeasible_soften_steps = 6;

    // ---- DC centering of the degraded band (SCALE + RESHAPE) ----------------
    // "The machine should gracefully handle infeasible input by shortening the
    // stroke, MIDPOINT ANCHORED." (operator, verbatim.) When the machine cannot
    // deliver the commanded amplitude on the commanded clock, the achieved band
    // must shrink SYMMETRICALLY about the commanded midpoint instead of walking
    // off one end — see the centering note on commitWaveformReshaped for the
    // control law and the measured numbers.
    //
    // The debt is a WAVEFORM-level concept, not a policy's private business: it
    // is a post-sizing correction on whatever endpoint the policy chose, so it
    // serves Scale and Reshape identically. Stretch is exempt — it delivers the
    // full amplitude (late) by definition, so there is never a deficit to
    // share, and it stays sample-for-sample identical with this flag either way.
    //
    // ON (default) the rule can shorten a stroke the machine COULD have made,
    // because delivering one direction in full while the other is clipped is
    // exactly what walks the band off center. Every such stroke is reported as
    // a WaveformCentered anomaly — a deliberate, visible deviation, never a
    // silent one.
    //
    // OFF restores the 0.4.0 contract: a quintic-feasible segment is never
    // touched, Scale never sees a debt at all, and Reshape's own reversal debt
    // reverts to the old ½·shortfall rule, quarter-stroke cap, reported as
    // WaveformScaled. (One knowing difference: Reshape now measures the
    // machine's reach against the COMMANDED target rather than the debt-
    // shortened one, so a reversal carrying an old-style debt can land within
    // one bisection quantum — ≈1.5 % of the stroke — of where 0.4.0 put it.
    // Same rule, better-measured input.)
    //
    // What OFF costs, measured on the operator's chains (band center error vs
    // the commanded midpoint, last 10 cycles): mixed chain −23.6 mm off vs
    // −4.5 mm on (the operator measured −26.8 mm on the machine itself); Scale
    // on the same chain −45.8 mm off vs −8.5 mm on.
    bool  wave_centering      = true;
    // Strength of the correction, 0..1 (clamped on use). 1 = full centering,
    // 0 = off (same as the bool). A feel dial, NOT a calibration, and not even
    // monotone: the debt loop closes around the pull it actually applied, so a
    // half-strength pull settles at a different fixed point rather than
    // half-way between the two extremes. Measured on the operator's chains,
    // band center error: OFF −23.6 mm, gain 0.5 −11.7 mm, gain 1.0 −4.5 mm on
    // the mixed chain — but on the both-infeasible chain OFF +1.1 mm, gain 0.5
    // −3.3 mm, gain 1.0 +0.2 mm, i.e. the mid setting is WORSE than either
    // end. Use 1.0 or use the bool; the in-between is for experimenting.
    float wave_centering_gain = 1.0f;

    // ---- Handoff sanity guard (RFC-008; WAVEFORM path only) -----------------
    // Fritsch-Carlson chord factor `k` for the one-segment-lookahead bound on
    // an explicit wire end velocity (see boundHandoffVelocity below for the
    // derivation and the measured pathology it exists for).
    //
    // 1.5 is not a fudge: capping BOTH endpoint tangents of a span at
    // k*|chord| makes the Fritsch-Carlson sum condition alpha + beta <= 2k,
    // and 2k <= 3 is the classic sufficient condition for a shape-preserving
    // (non-overshooting) Hermite — exactly at k = 1.5. Raise toward 3.0 for
    // the looser per-tangent box if the machine ends up flatter than the
    // sender through long shallow runs; this is the ONE knob for handoff
    // aggressiveness.
    //
    // 0 DISABLES the guard entirely (pre-0.7.0 behavior, byte for byte) —
    // that is the A/B switch for comparing machine-side bounding against a
    // client that still carries its own limiter. CLAMPED to [0, 8] on use: a
    // config push is not a trusted input.
    //
    // The guard only ever engages when the CALLER supplies a lookahead
    // (Command::has_next_chord). An engine fed no lookahead behaves exactly as
    // it did before this knob existed, whatever k says.
    float handoff_chord_factor = 1.5f;

    // ---- OVERSHOOT GUARD (option A of the 2026-07-30 bench shoot-out) -------
    // The legality scan constrains v/a/j and the WINDOW. It has never
    // constrained "did this plan sail past the endpoint the sender asked for",
    // so a plan may arc far beyond the commanded target and still be legal as
    // long as it stays inside the window.
    //
    // MEASURED (GoogleCat, t=28.168): the machine sits at 186.8 mm doing
    // +1000 mm/s (at vmax) when a segment says "be at 100 mm — 86.8 mm BELOW —
    // in 875 ms". The C1 cubic that satisfies those four boundary conditions
    // over exactly that duration peaks at 305.3 mm: 118 mm past the target,
    // SEVEN TIMES the 16.7 mm a full-authority brake would have cost. Momentum
    // is not the cause — the polynomial is, because a fixed duration plus fixed
    // endpoints uniquely determines the shape and it must SPEND those 875 ms.
    //
    // The allowance is therefore physical, not a taste knob: a plan may pass its
    // own endpoint by as much as STOPPING THERE WOULD CARRY IT ANYWAY, and no
    // further. Overshoot that momentum forces is honest; overshoot the polynomial
    // invented is not.
    //
    // "WOULD CARRY IT ANYWAY" IS MEASURED, NOT A FORMULA — see
    // physicalBandExcess(). The 0.9.0 guard used the closed form v0^2/(2*amax),
    // which ignores the JERK ceiling, and jerk is what dominates a hard stop:
    // braking from 800 mm/s at amax 50 000 mm/s^2 costs 6.4 mm on paper and
    // ~15 mm in fact, because reaching full deceleration takes 25 ms at jmax and
    // the carriage covers 20 mm getting there. A guard built on the paper number
    // is ~2.3x too strict — it rejected plans that were already near-physical and
    // handed them to the flat Ruckig fallback, which is exactly how the knob
    // measured WORSE THAN OFF on OvershootTestThrobbing (mean excursion 4.63 ->
    // 7.40 mm) and non-monotone in its own value. Both were this.
    //
    // 0 disables (pre-guard behavior, byte for byte). Values > 1 are a slack
    // multiplier on the physical floor for operators who want the shape back.
    float overshoot_guard = 1.0f;

    // Slack ON TOP of the physical floor, as a fraction of the segment's own
    // commanded chord. THIS IS WHAT MAKES THE GUARD SELECTIVE, and without it the
    // guard is not worth having.
    //
    // The physical floor alone is an ABSOLUTE bound, so it fires on every stroke
    // whose excursion exceeds it — including strokes that overshoot by 2 % of
    // their own travel, which nobody can feel and which the ceiling scan was
    // right to pass. Each of those rejections buys a straight line, because the
    // Ruckig fallback cruises at vmax on anything near saturation. That is how a
    // correct bound still produced the operator's "some strokes go suddenly
    // linear": measured on GoogleCat at the operator's window, slack 0 took the
    // fallback on 55 segments and pushed flattening 12.2 % -> 25.7 % to remove an
    // excursion of 7.4 mm that was 0.47x its own stroke, i.e. not a defect.
    //
    // Throbbing is a RATIO, not a distance — the complaint is a move that travels
    // further past its target than the move itself was long. Allowing a quarter
    // of the chord on top of the physical floor says exactly that. Measured
    // against slack 0 (2026-07-30, 12 recordings, window 150-350, guard 1, the
    // C1 family the machine actually ships): InterpTest1 flattening
    // 35.7 % -> 5.1 % with 22 fallbacks -> 0, GoogleCat 23.8 -> 20.8 % and
    // 13 -> 2, SYN-truncated 30.8 -> 23.8 % and 9 -> 0 — all for about 0.5 mm of
    // worst-case excursion. The absolute bound was spending a straight line to
    // remove excursions of 0.03x their own stroke.
    //
    // 0 restores the absolute-only bound. The whole guard disarms at
    // overshoot_guard 0 regardless of this value.
    float overshoot_chord_slack = 0.25f;

    // ---- BAD-MOVE BRIDGE (option B of the same shoot-out) -------------------
    // The other answer to the same measurement: when the machine could reach
    // the commanded target FAR sooner than the commanded duration, the command
    // is not a stroke that is merely hard — it is a DISCONTINUITY, and shaping
    // a polynomial across it is what produces the arc.
    //
    // Detection reuses machinery Reshape already pays for: the time-optimal
    // duration from the machine's ACTUAL (p, v, a). Above this ratio the
    // segment is planned TIME-OPTIMALLY (arrive early, hold) instead of being
    // stretched across its deadline.
    //
    // Note the guard path does NOT already do this: it calls planRuckig with
    // minimum_duration = T, so it honors the deadline too and can arc for the
    // same reason the quintic does.
    //
    // The threshold must sit well above 1.0 — measured, 13.4 % of ordinary
    // GoogleCat segments already demand more than the velocity ceiling and the
    // worst ordinary case is 2.09x, while the t=28.168 discontinuity scores
    // ~5.1x. 0 disables (pre-bridge behavior, byte for byte).
    float bridge_ratio = 0.0f;

    // ---- Settle grace (see maybeSettle) -------------------------------------
    // How long an expired plan may HOLD its end state before the engine
    // concludes the stream is starved and brakes to rest. Sized as
    // min(1.5 · estimated stream interval, this cap) and only applied while a
    // recent stream estimate exists — an isolated point move still settles
    // immediately. 0 disables the grace (pre-0.4 behavior: brake the instant
    // the plan expires).
    uint32_t settle_grace_us = 30000;
};

// ---- Command (POD, queue-safe — the InterpSegment successor) ----------------
struct Command {
    float    target       = 0.5f;   // normalized 0..1
    uint32_t duration_us  = 0;      // I<ms> * 1000 when present
    float    end_vel      = 0.0f;   // units/s — TCode G wire value / 1000
    bool     has_end_vel  = false;  // true → v4 gradient handoff velocity
    bool     has_duration = false;  // true + duration ≥ kShortMoveUs → WAVEFORM

    // ---- ONE-SEGMENT LOOKAHEAD (RFC-008 handoff sanity guard) ---------------
    // The mean speed (|Δtarget| / duration, same normalized units/s as
    // end_vel) of the segment that FOLLOWS this one, when the caller already
    // holds it. The firmware's SlopSync pacing ring schedules 0x0085 segments
    // ~120 ms ahead of their start, so at the moment a segment is handed to
    // the engine its successor is frequently already queued — this is that
    // knowledge, and nothing else. It is NOT a command, it never plans
    // anything, and it is a magnitude (never signed): it exists solely to
    // bound end_vel at the knot the two segments share.
    //
    // false = "no successor is known" (tail of a stream, a durationless chase
    // point next, or a caller with no lookahead at all) → the guard does not
    // engage and the command behaves exactly as it did pre-0.7.0. Guessing a
    // chord we do not have would trim well-behaved senders for free, which is
    // a feel regression; not guessing costs nothing, because an unbounded
    // handoff still meets the legality scan + Ruckig guard downstream.
    // RFC-049c evaluated and REJECTED an own-chord fallback here — see
    // commitWaveform's note beside boundHandoffVelocity's call site.
    float    next_chord     = 0.0f;
    bool     has_next_chord = false;

    // ---- Scheduled start (anchored commit) ----------------------------------
    // The command's due time in the engine's own clock domain. When set,
    // commit() anchors the plan here rather than at arrival, so release
    // jitter between the pacing schedule and the commit never becomes
    // rendered geometry. false = plan at arrival (pre-0.9 behavior).
    uint64_t anchor_us  = 0;
    bool     has_anchor = false;

    // ---- RFC-030: the sender's DECLARED curve family ------------------------
    // Values mirror the SlopSync registry's `curve_families` table verbatim
    // (this header stays slopsync-free, so the numbering is documented, not
    // included): 0 = unspecified, 1 = c1_cubic, 2 = c2_quintic, 3 = step.
    // Consumed ONLY by the waveform path's FollowClient resolution — 1 selects
    // the cubic reconstruction, everything else keeps the quintic (today's
    // behavior, including `step`, honestly: no step renderer exists yet).
    // Callers with no wire knowledge leave it 0 and nothing changes.
    uint8_t  client_curve_family = 0;
};

// ---- RFC-008 handoff sanity guard -------------------------------------------
// "The machine plans for the worst so clients don't have to."
//
// THE FAILURE THIS EXISTS FOR (measured live, MFP plugin v0.2.1-0.2.3 against
// slopsim, 2026-07-25): a funscript axis on Makima interpolation produced a
// handoff velocity of 1.816 norm/s into a span whose own mean velocity is
// 0.050 norm/s — 36x over. The client was computing a MATHEMATICALLY CORRECT
// spline tangent; there is simply no monotone quintic that covers 0.050 of
// span-mean displacement while ARRIVING at 1.816. The legality scan rejected
// the quintic, the Ruckig guard took the segment, and a "slow, simple" script
// rendered as straight-line strokes with a flat-topped velocity trace. Worse,
// the af backward-difference estimator then carried the oversized value into
// the NEXT segment's boundary conditions.
//
// THE CRITICAL DETAIL: bounding end_vel against the CURRENT segment's own
// chord is NOT sufficient. 1.816 was entirely sane relative to its own span
// (whose chord was ~3.0 norm/s) and absurd only relative to the NEXT one.
// A guard that cannot see forward cannot catch this class at all.
//
// THE BOUND: the handoff lives at the KNOT shared by two segments, so it must
// be feasible for BOTH —
//
//     |end_vel|  <=  k * min(|chord_in|, |chord_out|)
//
// which is the Fritsch-Carlson knot limiter (the same bound the MFP plugin
// currently carries client-side, applied where RFC-008 says it belongs). Sign
// is preserved: a wrong-signed handoff stays wrong-signed but bounded, so the
// ONLY thing this changes is pathological SIZE. A zero chord on either side (a
// plateau, or "the target IS where the next segment ends") forces 0 — arriving
// at a hold still moving is exactly the kind of lie the Ground Truth doctrine
// exists to prevent.
//
// WHY min AND NOT chord_out ALONE: min() is strictly stronger, it satisfies
// "never exceeds the following chord's limit" by construction, and it stays
// monotone in |chord_out| (min(a, ·) is non-decreasing). It also costs nothing
// to compute — chord_in is |target - p| / T, which the engine already knows
// exactly, from the machine's ACTUAL position rather than a sender's guess.
//
// IN-BOUNDS INPUT IS RETURNED BIT-FOR-BIT UNCHANGED. That is a hard contract,
// not an implementation detail: a guard that perturbs a well-behaved client's
// handoff is a feel regression for every well-behaved client, which is a worse
// bug than the one it was written to fix.
//
// `chord_in` / `chord_out` are non-negative magnitudes in units/s. A NEGATIVE
// value means "unknown" and disables the bound (never bound on a guess); so
// does k <= 0, which is the guard's off switch. Non-finite anything is passed
// through — Engine::commit() rejects non-finite command inputs upstream, and a
// numeric guard is not the place to relitigate that.
inline float boundHandoffVelocity(float end_vel, float chord_in, float chord_out,
                                  float k) noexcept {
    if (!(k > 0.0f)) return end_vel;                        // guard disabled
    if (k > 8.0f) k = 8.0f;                                 // config push is untrusted
    if (!std::isfinite(end_vel) || !std::isfinite(chord_in) ||
        !std::isfinite(chord_out)) return end_vel;
    if (chord_in < 0.0f || chord_out < 0.0f) return end_vel; // "unknown" side
    const float limit = k * (chord_in < chord_out ? chord_in : chord_out);
    const float mag   = end_vel < 0.0f ? -end_vel : end_vel;
    if (mag <= limit) return end_vel;                        // UNTOUCHED
    return end_vel < 0.0f ? -limit : limit;
}

enum class Mode : uint8_t {
    Idle     = 0,   // holding position, no planned motion
    Waveform = 1,   // executing a quintic v4 segment (or its Ruckig fallback)
    Chase    = 2,   // tracking a bare point stream
    Settle   = 3    // braking to rest after stream starvation
};

// APPEND-ONLY (wire-visible: 0x0081 `plan_kind` is a select field whose option
// list is indexed by this enum, and SystemState::sm_plan_kind mirrors it).
//
// Quintic and Cubic are BOTH Hermite polynomials sharing one evaluator — a cubic
// is stored as a quintic with c[4] = c[5] = 0. They are separate VALUES rather
// than one "hermite" value because the operator has to be able to see which
// family actually ran; a readout saying "quintic" while a cubic executes is a
// ground-truth defect, and it cost a live debugging session to notice.
//
// ANY new Hermite family MUST be added to isHermite() in the same change. Every
// evaluation site branches "Hermite → quinticAt, else → Ruckig trajectory", so a
// kind that is Hermite in spirit but missing from that predicate does not fail
// loudly — it silently evaluates as a Ruckig profile that was never planned.
enum class PlanKind : uint8_t { None = 0, Quintic = 1, Ruckig = 2, Cubic = 3 };

// ---- Anomaly instrumentation (same drain pattern as the cubic engine) -------
enum class AnomalyType : uint8_t {
    None              = 0,
    PlanFailed        = 1,  // plan rejected; previous plan kept. detail = (float)Result or -99 input guard
    SettleEngaged     = 2,  // stream starved mid-glide → brake plan. detail = end velocity
    EndVelClamped     = 3,  // requested vf cut by wall/vmax guard. detail = clamped vf
    DeadlineStretched = 4,  // commanded duration infeasible; guard profile runs longer. detail = actual s
    WaveformFallback  = 5,  // quintic broke a ceiling/window → Ruckig shaped the segment instead. detail = worst ratio
    WaveformScaled    = 6,  // the stroke was shrunk to hold the deadline. detail = achieved fraction 0..1, target = the shortened target
    WaveformCentered   = 7,  // the stroke was shrunk to hold the MIDPOINT (the machine could have gone further). detail = achieved fraction 0..1, target = the shortened endpoint
    HandoffBounded    = 8,  // RFC-008: a wire end velocity was cut to the Fritsch-Carlson knot bound of the FOLLOWING segment. detail = the accepted (bounded) vf
    WaveformSmoothed  = 9   // the span's END handle was lerped toward the chord to make the shape legal. detail = alpha spent, 0..1 (1 = straight line)
};
// APPEND-ONLY. Existing values are pinned: the firmware (SystemState::
// sm_anom_kind + kSmAnomalyNames) and the sim (MachineSim.h mirror) index
// per-kind counter tables by this enum, and their drain loops bounds-check
// against the NAME table — a kind with no name there is dropped, not
// miscounted. WaveformCentered = 7 fits the existing SM_ANOM_KINDS = 8 counter
// width, but both name tables need "waveform_centered" appended before the
// count becomes visible in /api/slopmotion or the sim TUI.
// HandoffBounded = 8 SPENT that width: SM_ANOM_KINDS went 8 -> 9 in the same
// change, together with kSmAnomalyNames, the sim's mirror of it, and the
// per-kind field list on the 0x0088 slopmotion-diag channel.
// WaveformSmoothed = 9 SPENT the next slot: SM_ANOM_KINDS went 9 -> 10, same
// three-place update (names, sim mirror, 0x0088 field list).

// ANOMALY VOCABULARY FOR THE INFEASIBLE PATHS (one event per infeasible
// segment, so the counts read as a diagnosis rather than a pile):
//   Stretch : WaveformFallback (+ DeadlineStretched) — shape AND deadline lost.
//   Scale   : WaveformScaled — deadline kept, amplitude lost (detail = how much).
//   Reshape : WaveformFallback alone means "shape lost, NOTHING else" — full
//             commanded stroke, on the commanded deadline, running a Ruckig
//             profile instead of the sender's spline. WaveformScaled means the
//             machine additionally could not reach that far in time. A new
//             anomaly kind was deliberately NOT minted for the reshape-at-full-
//             amplitude case: "the quintic was rejected and Ruckig took the
//             segment" is exactly what WaveformFallback has always meant, and
//             the absence of a companion DeadlineStretched/WaveformScaled is
//             already the "nothing was sacrificed" signal.
//   Centering: WaveformCentered — the endpoint was pulled in to keep the band on
//             the commanded midpoint, and the MACHINE was not the binding
//             constraint (it could have gone further, quintic-feasible strokes
//             included). This one DID get its own kind, because "the machine
//             ran out of road" and "the engine chose symmetry over reach" are
//             different diagnoses and only one of them is a hardware/limit
//             question. Exactly one event per segment: whichever constraint
//             BINDS is the one reported (machine short ⇒ WaveformScaled,
//             centering short ⇒ WaveformCentered).
//   Handoff : HandoffBounded — the SENDER'S HANDOFF was reshaped, one segment
//             BEFORE any of the above could happen. This is the only kind on
//             this list that reports a preventive act rather than a rescue,
//             and it is deliberately loud for that reason: it means the client
//             is asking for a knot velocity its own next segment cannot
//             absorb, and the operator whose script is fighting the planner
//             should be able to find that out. A well-behaved sender never
//             produces one. It can legitimately co-occur with the rescue kinds
//             on the SAME segment (the bounded handoff can still be part of an
//             infeasible shape) — bounding a handoff is not a promise that the
//             rest of the segment fits.

struct Anomaly {
    uint8_t  kind   = 0;      // AnomalyType
    uint16_t seq    = 0;      // rolling event id
    uint64_t t_us   = 0;      // engine time at record
    float    target = 0.0f;   // command target 0..1
    float    detail = 0.0f;   // kind-specific (see AnomalyType)
};

// ---- Telemetry snapshot (WebUI planned-path overlay feed) -------------------
struct Snapshot {
    float    pos        = 0.5f;
    float    vel        = 0.0f;   // units/s
    float    acc        = 0.0f;   // units/s^2
    float    start      = 0.5f;   // active plan's start position
    float    target     = 0.5f;   // active plan's end position
    float    duration_s = 0.0f;   // active plan duration (0 = holding)
    float    elapsed_s  = 0.0f;
    uint8_t  mode       = 0;      // Mode
    uint8_t  plan_kind  = 0;      // PlanKind of the active plan
    uint32_t plans      = 0;      // successful plans since reset
    uint32_t failures   = 0;      // PlanFailed count since reset
    // SHARPNESS of the active plan: its PEAK JERK as a fraction of
    // Limits::jmax. Lower = rounder. For a Ruckig plan this is exactly the
    // ceiling it was planned under (Ruckig's profiles are bang-bang in jerk),
    // which is what the 0.6.0 sharpness search moves — see the header's
    // SHARPNESS BEFORE AMPLITUDE note. For a quintic it is the scanned peak of
    // its own shape, so the field means the same thing on both plan kinds
    // instead of being a policy artifact.
    //
    // NOT monotone across the quintic→reshape boundary, and that is real, not a
    // bug: a velocity-saturated Ruckig double-S at its critical jerk can be
    // GENTLER in jerk than the quintic that was just rejected for exceeding
    // vmax (measured: quintic peak j/jmax 0.13 → softened reshape 0.083 on the
    // same segment). Monotonicity is a property of the sharpness DIAL within
    // the reshape path, which is where the test asserts it.
    //
    // Appended in 0.6.0; every consumer reads Snapshot field by field, so this
    // is additive.
    float    sharpness  = 1.0f;
};

// ---- Engine -----------------------------------------------------------------
class Engine {
public:
    // ---- Sender-curve reconstruction (ANALYZER / tooling API) ---------------
    // Rebuild the curve a SENDER described, from the SENDER'S OWN boundary
    // conditions instead of the machine's live state.
    //
    // This is deliberately NOT how the engine plans. The engine always plans
    // from the machine's actual (p, v, a) — that is the motion doctrine and it
    // is not negotiable. These helpers exist so a diagnostic tool can draw
    // three lines at once: what the client ASKED for, what was PLANNED, and
    // what the machine DID. The gap between the first two is exactly the
    // infeasibility, which is otherwise invisible.
    //
    // They call the same two builders the waveform path uses, so the reference
    // line cannot drift from the thing it is a reference for.
    static void senderCurve(bool cubic, double p, double v, double a,
                            double target, double vf, double af, double T,
                            double* c) {
        if (cubic) buildCubic(p, v, target, vf, T, c);
        else       buildQuintic(p, v, a, target, vf, af, T, c);
    }
    // Evaluate such a curve at normalized tau ∈ [0,1]; derivatives are
    // real-time (per second), matching Snapshot's units. Same algebra as the
    // engine's own quinticAt — a cubic simply carries c[4] = c[5] = 0.
    static void evalCurve(const double* c, double T, double tau,
                          double& p, double& v, double& a) {
        p = ((((c[5]*tau + c[4])*tau + c[3])*tau + c[2])*tau + c[1])*tau + c[0];
        v = ((((5*c[5]*tau + 4*c[4])*tau + 3*c[3])*tau + 2*c[2])*tau + c[1]) / T;
        a = (((20*c[5]*tau + 12*c[4])*tau + 6*c[3])*tau + 2*c[2]) / (T * T);
    }

    explicit Engine(const Config& cfg = {}, float start_pos = 0.5f)
        : _cfg(cfg) {
        resetAt(start_pos, 0);
    }

    // ---- Lifecycle ----------------------------------------------------------
    // Hard-reset to a static hold at `pos`. Used on home/estop/resume/stream
    // rising-edge (seed at the machine's actual position).
    void resetAt(float pos, uint64_t now_us) {
        _syn_ok = false;
        _syn_prev_ok = false;
        _syn_vf_ok = false;
        _syn_chain_ok = false;
        _pend_ok = false;
        _synr_n = 0;
        _synr_w = 0;
        _hold_pos   = clamp01(pos);
        _mode       = Mode::Idle;
        _kind       = PlanKind::None;
        _plan_start = now_us;
        _est_valid  = false;
        _est_ema_ok = false;
        _prev_vf_ok = false;
        _wave_dir     = 0;
        _wave_owed    = 0.0;
        _wave_last_us = 0;
        _plan_jerk_frac = 1.0f;
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
    // Wholesale live-tuning update (firmware pushes the WebUI/API-tuned
    // config every sampler tick — same-core with commit(), no lock needed).
    void setConfig(const Config& c) { _cfg = c; }

    // ---- Command entry (Core 1, after queue drain) --------------------------
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

        // Anchor at the command's SCHEDULED start when the caller carries one
        // (Command::anchor_us): the engine executes the wire timeline, not the
        // arrival timeline, so release jitter never becomes rendered geometry
        // (sd-ar3 chord-join notch). kAnchorMaxLateUs < kCoastCapS keeps the
        // sampled coast state uncapped inside the bound.
        uint64_t t0 = now_us;
        if (cmd.has_anchor && cmd.anchor_us < now_us) {
            t0 = cmd.anchor_us;
            if (now_us - t0 > kAnchorMaxLateUs) t0 = now_us - kAnchorMaxLateUs;
        }

        double p, v, a;
        sampleRaw(t0, p, v, a);

        const double target = clamp01(cmd.target);
        // Estimator feeds BOTH modes; anchored time on purpose (due spacing
        // is the stream's true cadence, arrival spacing carries the jitter).
        updateEstimator(target, t0);

        const bool waveform =
            cmd.has_duration && cmd.duration_us >= kShortMoveUs;

        // Cold-start governor (Config::recovery_vmax): the opening plan out
        // of rest traverses park->content at positioning gentleness. COLD
        // requires rest AND a real command gap: normal stroke content arrives
        // at rest between every action (explicit-rest handoffs), and keying
        // on rest alone clamped nearly every stroke to the user limit
        // (2026-08-10). The clamp is transient and covers every planner this
        // commit reaches. Single-task engine: no concurrent reader of _cfg.
        const bool cold = _cfg.recovery_vmax > 0.0f &&
                          _cfg.recovery_vmax < _cfg.limits.vmax &&
                          (_mode == Mode::Idle || _mode == Mode::Settle) &&
                          std::fabs(v) < 1e-3 &&
                          (_last_commit_us == 0 ||
                           now_us - _last_commit_us > kColdStartGapUs);
        _last_commit_us = now_us;
        const float vmax_full = _cfg.limits.vmax;
        if (cold) _cfg.limits.vmax = _cfg.recovery_vmax;
        bool ok;
        if (waveform) {
            _syn_ok = false;   // real segments preempt the synthesis buffer
            _syn_prev_ok = false;
            ok = commitWaveform(cmd, p, v, a, target, t0);
        } else if (_cfg.sample_synthesis) {
            ok = commitSampleSynth(cmd, p, v, a, target, t0, now_us);
        } else {
            ok = commitChase(cmd, p, v, a, target, t0);
        }
        _cfg.limits.vmax = vmax_full;
        if (ok) _plans++;
        return ok;
    }

    // ---- Bare-point synthesis (Config::sample_synthesis) --------------------
    // Renders the BUFFERED point's span now that its far end is known. Calls
    // commitWaveform directly, below commit()'s kShortMoveUs gate: that gate
    // exists to stop CLIENT micro-segments; these spans are our own
    // construction with honest stamp-derived durations.
    bool commitSampleSynth(const Command& cmd, double p, double v, double a,
                           double target, uint64_t t0, uint64_t now_us) {
        const uint64_t stamp = cmd.has_anchor ? cmd.anchor_us : now_us;
        if (_syn_ok && stamp - _syn_us > (uint64_t)_cfg.chase_stale_us) {
            _syn_ok = false;   // stream gap: restart the holdback
            _syn_prev_ok = false;
            _syn_vf_ok = false;
            _syn_chain_ok = false;
            _synr_n = 0;
            _synr_w = 0;
        }
        // Every priming/fallback chase targets the sample ~2 knots BEHIND
        // the head -- the same delayed timeline the chain renders -- so a
        // span's entry state is already ON that timeline. Chasing the head
        // made every first span a yank-back reversal: illegal, re-prime,
        // churn (bench trace 2026-08-09: five failed locks in 800 ms).
        _synr_us[_synr_w] = stamp;
        _synr_p[_synr_w]  = (float)target;
        _synr_w = (uint8_t)((_synr_w + 1) % kSynRingN);
        if (_synr_n < kSynRingN) _synr_n++;
        const double tgt_d = synthDelayedTarget(stamp, target);
        if (!_syn_ok) {
            // First point of a (re)started stream: plain chase positions to
            // it (the cold governor gentles a genuine opening); synthesis
            // primes behind it.
            _syn_ok = true;
            _syn_p = target;
            _syn_us = stamp;
            return commitChase(cmd, p, v, a, tgt_d, t0);
        }
        if (stamp <= _syn_us) {   // duplicate/regressive stamp: keep newest
            _syn_p = target;
            return true;
        }
        // Coalesce to the knot pitch: samples inside the pitch are decimated
        // (the samples contract); the first sample at/past it becomes the
        // next knot.
        if (stamp - _syn_us < kSynthSpanUs) {
            // While the holdback is still priming, keep chasing at sample
            // rate so a catch-up converges at 50 Hz, not knot pitch.
            if (!_syn_prev_ok) return commitChase(cmd, p, v, a, tgt_d, t0);
            return true;
        }
        // The first mature pair only rotates the buffer: a span needs the
        // 3-point tangent at its END knot, and emitting [here -> here, vf]
        // before one exists launches the machine the WRONG WAY (arrive-in-
        // place-moving means approach from behind).
        if (!_syn_prev_ok) {
            _syn_prev_p = _syn_p;
            _syn_prev_us = _syn_us;
            _syn_prev_ok = true;
            _syn_p = target;
            _syn_us = stamp;
            return true;
        }
        // Span duration is the CONTENT interval it renders ([prev -> cur]
        // knots); the incoming sample's spacing shapes ONLY the end tangent.
        // Borrowing the out-interval as T plays uneven knot cadences at the
        // wrong speed (field report 2026-08-09: "some moves way too fast").
        const double T = double(_syn_us - _syn_prev_us) * 1e-6;
        const double t_out = double(stamp - _syn_us) * 1e-6;
        const double c_out = (target - _syn_p) / t_out;
        // PCHIP knot tangent at the buffered point: zero at reversals,
        // Fritsch-Carlson bounded on runs -- monotone by construction.
        double vf;
        {
            const double c_in = T > 0.0 ? (_syn_p - _syn_prev_p) / T : 0.0;
            vf = (c_in * c_out <= 0.0)
                     ? 0.0
                     : (double)boundHandoffVelocity(
                           (float)(0.5 * (c_in + c_out)),
                           (float)std::fabs(c_in), (float)std::fabs(c_out),
                           1.5f);
        }
        // End-accel estimate from our own knot tangent series.
        double af = 0.0;
        if (_syn_vf_ok) af = (vf - _syn_vf) / T;
        // CHAIN TIME: the holdback is a fixed-latency pipeline, so anchors
        // tile (previous anchor + previous duration), starting at first
        // emission's now. Anchoring at the knot STAMPS renders every span
        // late by the holdback and re-samples a lagging entry -- permanent
        // schedule debt (measured: every span guard-bound, ratios 90-470).
        if (!_syn_chain_ok) {
            // Jitter buffer: seed the schedule AHEAD of real time so span
            // arrivals land EARLY and promote exactly on anchor (pending
            // slot). Seeded at arrival, half of all spans land late, and a
            // late adoption enters mid-span off a linear coast -- a velocity
            // kink at every jitter burst (field report 2026-08-09). MUST
            // stay under one knot pitch: the pending slot is one deep.
            _syn_chain_us = now_us + kSynthJitterUs;
            _syn_chain_ok = true;
        }
        uint64_t tw = _syn_chain_us;
        if (now_us > kAnchorMaxLateUs + tw) tw = now_us - kAnchorMaxLateUs;
        _syn_chain_us = tw + (uint64_t)(T * 1e6);
        double pw, vw, aw;
        // Entry state at the chain anchor: from the PENDING span's end when
        // one is outstanding (the active plan knows nothing past it), else
        // from the live plan.
        if (_pend_ok) evalCurve(_pend_c, _pend_T, 1.0, pw, vw, aw);
        else          sampleRaw(tw, pw, vw, aw);
#ifdef SLOPMOTION_SYNTH_DEBUG
        if (std::fabs(clamp01(_syn_p) - pw) / T > 0.9 * (double)_cfg.limits.vmax)
            std::printf("SYNSLIP now=%llu tw=%llu T=%.4f pw=%.4f knot=%.4f\n",
                        (unsigned long long)now_us, (unsigned long long)tw, T,
                        pw, clamp01(_syn_p));
#endif
        if (std::fabs(clamp01(_syn_p) - pw) / T >
            0.9 * (double)_cfg.limits.vmax) {
            // The span's schedule already saturates the machine: catch-up
            // authority is zero, so slip is DROPPED, never financed (the
            // samples contract is decimation). Re-prime at the freshest
            // point and chase the delayed timeline; synthesis resumes on the
            // next mature pair.
            _syn_p = target;
            _syn_us = stamp;
            _syn_prev_ok = false;
            _syn_vf_ok = false;
            _syn_chain_ok = false;
            return commitChase(cmd, p, v, a, tgt_d, t0);
        }
        bool ok = commitSynthSpan(pw, vw, aw, clamp01(_syn_p), vf, af, T, tw,
                                  now_us);
        if (!ok) {
            // Hot-entry span: chase the delayed timeline instead and
            // re-prime; synthesis re-locks on the next mature pair.
            _syn_p = target;
            _syn_us = stamp;
            _syn_prev_ok = false;
            _syn_vf_ok = false;
            _syn_chain_ok = false;
            return commitChase(cmd, p, v, a, tgt_d, t0);
        }
        _syn_vf = vf;
        _syn_vf_ok = true;
        _syn_prev_p = _syn_p;
        _syn_prev_us = _syn_us;
        _syn_prev_ok = true;
        _syn_p = target;
        _syn_us = stamp;
        return ok;
    }

    // Dedicated adoption for synthesized spans. The stroke machinery
    // (debt/centering/extremes, bridge, dwell, feasibility policies) assumes
    // commands END at stroke extremes; mid-curve knots trip it. Build the
    // quintic, scan it, DEGRADE boundary estimates (af first, then vf) until
    // legal -- estimates are ours to soften -- and only then fall back to the
    // Ruckig guard. Schedule is held; amplitude of a knot is never shaved.
    // Newest held sample at least 2 knots behind `stamp`; the oldest held
    // one when the stream is younger than the delay (opening park point).
    double synthDelayedTarget(uint64_t stamp, double fallback) const {
        if (_synr_n == 0) return fallback;
        const uint64_t want =
            stamp > 2 * kSynthSpanUs ? stamp - 2 * kSynthSpanUs : 0;
        int best = -1, oldest = 0;
        uint64_t best_us = 0, oldest_us = ~0ULL;
        for (int i = 0; i < _synr_n; i++) {
            const uint64_t us = _synr_us[i];
            if (us <= want && (best < 0 || us > best_us)) {
                best = i;
                best_us = us;
            }
            if (us < oldest_us) {
                oldest = i;
                oldest_us = us;
            }
        }
        return (double)_synr_p[best >= 0 ? best : oldest];
    }

    bool commitSynthSpan(double p, double v, double a, double knot, double vf,
                         double af, double T, uint64_t t0, uint64_t now_us) {
        if (!(T > 0.0)) return false;
        // This span's OWN overshoot bound, from its OWN entry state. Never the
        // member: a waveform commit's bound belongs to a different curve on a
        // different path, and reading it here judged this span by that one.
        const double allow = armOvershootAllow(p, v, a, knot, vf, T);
        double c[6];
        buildWaveformCurve(p, v, a, knot, vf, af, T, c);
        if (quinticWorstRatio(c, T, allow) > 1.0) {
            buildWaveformCurve(p, v, a, knot, vf, 0.0, T, c);
            if (quinticWorstRatio(c, T, allow) > 1.0) {
                // Still illegal (hot entry, ceiling reversal): report and let
                // the caller drop to CHASE for one knot -- an in-chain Ruckig
                // guard overruns its span and mints local schedule debt.
#ifdef SLOPMOTION_SYNTH_DEBUG
                std::printf("SYNFAIL t0=%llu now=%llu T=%.4f p=%.4f v=%.3f "
                            "a=%.2f knot=%.4f vf=%.3f ratio=%.2f\n",
                            (unsigned long long)t0, (unsigned long long)now_us,
                            T, p, v, a, knot, vf, quinticWorstRatio(c, T, allow));
#endif
                recordAnomaly(AnomalyType::WaveformFallback, (float)knot,
                              (float)quinticWorstRatio(c, T, allow), t0);
                return false;
            }
        }
#ifdef SLOPMOTION_SYNTH_DEBUG
        std::printf("SYNSPAN t0=%llu now=%llu T=%.4f p=%.4f v=%.3f a=%.2f "
                    "knot=%.4f vf=%.3f pend=%d\n",
                    (unsigned long long)t0, (unsigned long long)now_us, T, p,
                    v, a, knot, vf, (int)(t0 > now_us));
#endif
        if (t0 > now_us) {
            // Emitted ahead of its chain anchor (uneven knot cadence): the
            // active plan keeps playing; maybeSettle promotes at t0. Adopting
            // now would clamp to the span start and teleport.
            // Slot occupied = successor beat the pending's anchor: promote it
            // early rather than drop its knot (the bigger jump).
            if (_pend_ok) adoptQuintic(_pend_c, _pend_T, _pend_start);
            for (int i = 0; i < 6; i++) _pend_c[i] = c[i];
            _pend_T     = T;
            _pend_start = t0;
            _pend_ok    = true;
            return true;
        }
        adoptQuintic(c, T, t0);
        return true;
    }

    // ---- Evaluation (Core 1, ~1 kHz hot path) -------------------------------
    // May engage the SETTLE transition when the clock runs past a trajectory
    // that ends moving.
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
        if (_pend_ok) return true;   // a scheduled successor is motion to come
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
            double ps, vs, as;
            if (isHermite()) quinticAt(0.0, ps, vs, as);
            else                            _traj.at_time(0.0, ps, vs, as);
            s.start      = (float)clamp01(ps);
            s.duration_s = (float)planDuration();
            const double el = elapsedS(now_us);
            s.elapsed_s  = (float)(el < planDuration() ? el : planDuration());
        } else {
            s.target = (float)_hold_pos;
            s.start  = (float)_hold_pos;
        }
        s.mode      = (uint8_t)_mode;
        s.plan_kind = (uint8_t)_kind;
        s.plans     = _plans;
        s.failures  = _failures;
        s.sharpness = _plan_jerk_frac;
        return s;
    }

    // ---- Anomaly drain (Core 1, single-threaded — no lock) ------------------
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
    // I<ms> below this is a dense-stream point, not a plannable segment.
    // 20 ms, not the legacy 50: a scripted mid-stroke knot arrives as a
    // 41 ms segment with an authored tangent, and demoting it to a chase
    // point threw its duration and handoff away and switched planners twice
    // per stroke (measured 2026-09-02, 25 of 427 segments, four planner
    // switches per 0.6 s cycle). The legality referee, not this floor, is
    // what rejects a span that is too short to render.
    static constexpr uint32_t kShortMoveUs  = 20000;
    static constexpr int      kScanSteps    = 64;     // quintic legality grid
    // Overshoot-guard floor, normalized: 0.2 % of the stroke window. A plan
    // that lands exactly on its target still shows rounding-sized excursions on
    // a 64-point grid, and rejecting those would send perfectly good segments to
    // the guard for nothing.
    static constexpr double   kOvershootFloor = 0.002;
    // Slack on the Ruckig legality referee. A profile that STARTS at the
    // ceiling with adverse acceleration must overshoot it a little on the way
    // back inside — the jerk limit says so, and no planner can avoid it (swept:
    // 1.029 worst over 129 600 time-optimal cases at the mechanical ceiling).
    // That inherited overshoot is physics, not a planning error, so it must not
    // be mistaken for one. The pathology this referee exists to catch is 1.4x
    // and up, nowhere near this band.
    static constexpr double   kRuckigLegalEps = 0.05;
    static constexpr double   kAimCapS      = 0.060;  // predictive aim ceiling
    // Scale-policy shrink ladder: bounded so an infeasible segment can never
    // turn plan time into an unbounded search (see commitWaveformScaled).
    // The sizing closed form is quintic-exact for a rest-to-rest stroke, so
    // attempt 1 lands for the overwhelming majority of segments; the ladder
    // only exists for the moving-start cases the closed form does not model.
    static constexpr int      kScaleTries   = 3;
    static constexpr double   kScaleShrink  = 0.85;
    // Peak/mean ratios of a min-jerk (rest-to-rest) quintic covering distance
    // d in time T:  v_peak = 15/8·d/T,  a_peak = 10/√3·d/T²,  j_peak = 60·d/T³.
    static constexpr double   kQuinticVPeak = 1.875;      // 15/8
    static constexpr double   kQuinticAPeak = 5.7735027;  // 10/√3
    static constexpr double   kQuinticJPeak = 60.0;
    // ---- DC-centering constants (see the control-law note) ------------------
    // "Nothing was given up" threshold, in normalized units — well under the
    // 1 % quantum of the 0x0085 wire target field.
    static constexpr double   kCenterEps     = 1e-6;
    // Largest pull-in a single stroke may pay, as a fraction of its own
    // commanded travel. Keeps the endpoint on the far side of the start
    // position by construction (never inverted, never past the target).
    static constexpr double   kCenterCapFrac = 0.50;
    // The 0.4.0 cap, still used when centering is disabled so that knob really
    // does restore the old behavior.
    static constexpr double   kLegacyOwedCap = 0.25;
    // Debt filter gains (see the control-law note). LAG is how much of each
    // stroke's observation is folded into the debt — the lag is what stops the
    // loop from oscillating; RELAX is the share of the machine's SLACK handed
    // back when a stroke was shortened voluntarily — the release valve that
    // stops the band ratcheting shut. Swept on the operator's chains, both
    // speeds, both policies; see the note for the measured table.
    static constexpr double   kCenterLag     = 0.50;
    static constexpr double   kCenterRelax   = 0.25;
    // A debt this old belongs to a phrase that has ended (the machine settled
    // somewhere else). 1 s is far longer than any segment cadence a sender
    // pushes — 0x0085 funscript segments run ~2–4 per second.
    static constexpr uint64_t kWaveDebtStaleUs = 1000000;
    // Settle grace = min(this × estimated stream interval, settle_grace_us).
    // 1.5 intervals: one whole interval of lateness is normal transport
    // scheduling, half of another is the margin before it means something.
    static constexpr double   kSettleGraceMult = 1.5;
    // Coast-past-expiry bound (sampleRaw): 2× the grace cap, so the coast
    // always outlives the window in which settle takes over.
    static constexpr double   kCoastCapS = 0.060;
    // Anchored-commit lateness bound; MUST stay under kCoastCapS (see
    // commit()).
    static constexpr uint64_t kAnchorMaxLateUs = 50000;
    // Below this span a timed segment is a DWELL (see the dwell rule in
    // commitWaveform); 2% of the window, under any real stroke.
    static constexpr double   kDwellSpanNorm = 0.02;
    // Sample-synthesis knot pitch: bare points coalesce into spans at least
    // this long. Jerk scales as 1/T^3, so 20 ms micro-spans turn tiny
    // boundary-estimate errors into ceiling breaks at every curvature
    // extreme (measured: 8 guard-stretches = the test sine's 8 extremes);
    // 60 ms knots give 27x the headroom and the quintic interior does the
    // between-knot smoothing, which is the point of synthesis.
    static constexpr uint64_t kSynthSpanUs = 60000;
    // Synthesis jitter buffer: the chain schedule leads real time by this
    // margin so transport jitter lands spans EARLY (pending slot), never
    // mid-flight. MUST stay under kSynthSpanUs (one pending slot).
    static constexpr uint64_t kSynthJitterUs = 40000;
    // Chase jerk-scale knee: demand fraction of vmax at which full jerk
    // authority returns (see commitChase).
    static constexpr double   kChaseJerkKneeFrac = 0.5;
    // Release time constant of the demand peak-hold (updateEstimator).
    static constexpr double   kSpPeakReleaseS = 0.7;
    // Command silence that makes the next from-rest commit a COLD start.
    // Above the sparsest legitimate content cadence (~1 s point spacing).
    static constexpr uint64_t kColdStartGapUs = 2000000;

    static double clamp01(double x) {
        return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
    }

    double elapsedS(uint64_t now_us) const {
        return now_us <= _plan_start ? 0.0
                                     : (double)(now_us - _plan_start) * 1e-6;
    }

    // ---- Active-plan evaluation ---------------------------------------------
    // Is the active plan one of the Hermite families (evaluated by quinticAt,
    // a cubic being a quintic with two zero high-order terms)? See PlanKind:
    // every branch below reads "Hermite -> quinticAt, else -> Ruckig", so a
    // missing kind here evaluates as a trajectory that was never planned.
    bool isHermite() const {
        return _kind == PlanKind::Quintic || _kind == PlanKind::Cubic;
    }

    double planDuration() const {
        return isHermite() ? _q_T
             : _kind == PlanKind::Ruckig  ? _traj.get_duration()
             : 0.0;
    }

    void planEndState(double& p, double& v, double& a) const {
        if (isHermite())                     quinticAt(1.0, p, v, a);
        else if (_kind == PlanKind::Ruckig)  _traj.at_time(_traj.get_duration(), p, v, a);
        else { p = _hold_pos; v = 0.0; a = 0.0; }
    }

    // Raw kinematic state (UNCLAMPED position — planning continuity must see
    // the true polynomial state even during a transient wall excursion).
    // Past expiry the state COASTS at the end velocity, capped at kCoastCapS:
    // freezing here stamped a flat spot into every chord join whose successor
    // arrived after plan expiry (the 5 ms pacing-drain beat guarantees ~half
    // do), felt as speed-scaled notching; a plan ending at rest coasts
    // nowhere, and maybeSettle stays the stop authority.
    void sampleRaw(uint64_t now_us, double& p, double& v, double& a) const {
        if (_kind == PlanKind::None) {
            p = _hold_pos; v = 0.0; a = 0.0;
            return;
        }
        double t = elapsedS(now_us);
        const double dur = planDuration();
        double over = 0.0;
        if (t >= dur) {
            over = t - dur;
            if (over > kCoastCapS) over = kCoastCapS;
            t = dur;
        }
        if (isHermite()) quinticAt(dur > 0 ? t / dur : 1.0, p, v, a);
        else                            _traj.at_time(t, p, v, a);
        if (over > 0.0) { p += v * over; a = 0.0; }
    }

    // Quintic evaluation at normalized tau ∈ [0,1] (real-time derivatives).
    void quinticAt(double tau, double& p, double& v, double& a) const {
        const double* c = _q_c;
        p = ((((c[5]*tau + c[4])*tau + c[3])*tau + c[2])*tau + c[1])*tau + c[0];
        v = ((((5*c[5]*tau + 4*c[4])*tau + 3*c[3])*tau + 2*c[2])*tau + c[1]) / _q_T;
        a = (((20*c[5]*tau + 12*c[4])*tau + 6*c[3])*tau + 2*c[2]) / (_q_T * _q_T);
    }

    // ---- WAVEFORM (v4 / timed segments): quintic + Ruckig guard -------------
    bool commitWaveform(const Command& cmd, double p, double v, double a,
                        double target, uint64_t now_us) {
        // RFC-030: adopt the command's declared family BEFORE any curve is
        // built — every later re-solve of this plan (Scale, the budgeted
        // search, centering) reads it through waveformIsCubic() and therefore
        // re-solves in the SAME family the sender declared.
        _client_curve_family = cmd.client_curve_family;
        const double T = (double)cmd.duration_us * 1e-6;
        // Disarmed before ANY referee can run on this commit: the bad-move
        // bridge below plans through Ruckig, and the previous segment's bound
        // is not this one's. Armed for real from this entry state further down.
        _oshoot_allow = -1.0;

        // End velocity: wire G when present, else the stream estimate (an
        // I-only stream shouldn't come to rest at every point).
        double vf = cmd.has_end_vel ? (double)cmd.end_vel
                  : (_cfg.chase_feedforward && _est_ema_ok && streamIsDense())
                        ? _est_v_ema * (double)_cfg.chase_ff_gain
                        : 0.0;

        // DWELL RULE: the SAME target re-commanded is a hold; honoring its
        // declared vf whips the machine through the hold point on every
        // re-send, and each whip displaces p, which is why this tests the
        // TARGET, never position (2026-08-09: cost 54 mm of dropped steps).
        // Strokes alternate targets, so the RFC-049c centering regime never
        // matches.
        const bool dwell = _prev_wave_tgt_ok &&
            std::fabs(target - _prev_wave_tgt) < kDwellSpanNorm;
        _prev_wave_tgt = target;
        _prev_wave_tgt_ok = true;
        if (dwell && cmd.has_end_vel && vf != 0.0) {
            recordAnomaly(AnomalyType::HandoffBounded, (float)target, 0.0f,
                          now_us);
            vf = 0.0;
        }

        // ---- RFC-008 HANDOFF SANITY GUARD (one-segment lookahead) -----------
        // Runs FIRST, ahead of every other treatment of vf, because it is the
        // only one that answers "is this handoff a physically meaningful thing
        // for the sender to have asked for at all?" — the wall/vmax guard
        // below answers a different question (does it fit the window and the
        // ceilings) and the measured pathology passed that one comfortably.
        //
        // chord_in is measured from the machine's ACTUAL position, which is
        // the true chord of the motion being planned (|target - p| over the
        // commanded T) — better ground truth than the sender's own script
        // geometry, and free. chord_out comes from the caller's lookahead; no
        // lookahead means no bound (see Command::has_next_chord).
        //
        // RFC-049c NOTE (the panel's "sparse-segment scheduling-depth
        // backstop" ask, H11): a variant bounding chord_out against chord_in
        // itself when no lookahead is present was PROTOTYPED and REJECTED
        // here, not merely deferred. Measured live against this file's own
        // "Mixed feasible/infeasible chain settles centered and STAYS there"
        // regression (the operator's real 26.8 mm-off-center bench case): the
        // own-chord fallback materially shrank the characterized defect's
        // magnitude in the centering-OFF baseline (-23.6 mm -> -9.4 mm) purely
        // by clamping SOME declared down-stroke end velocities whenever the
        // reshape/centering feedback loop's own dynamics had already pulled
        // chord_in below the k-factor bound — an interaction with a physically
        // sensitive, operator-tuned control loop that this pass could not
        // adequately re-validate. Left OPEN, per SlopSync RFC-049(c) (SlopSync
        // repo): a real fix needs the scheduling-depth signal to come
        // from somewhere that can tell "a successor is coming, just not yet
        // queued" apart from "this is genuinely the last segment" — which
        // chord_in alone cannot do — rather than trading the tail case's
        // documented honesty clause for an unverified motion-quality risk.
        //
        // Engages only on an EXPLICIT wire handoff. When has_end_vel is false
        // vf is the engine's OWN stream estimate, which is already conservative
        // and is not the sender's claim to sanity-check.
        if (cmd.has_end_vel && cmd.has_next_chord && T > 0.0) {
            const float chord_in = (float)(std::fabs(target - p) / T);
            const float bounded  = boundHandoffVelocity(
                (float)vf, chord_in, cmd.next_chord, _cfg.handoff_chord_factor);
            if (bounded != (float)vf) {
                // NEVER silent. detail = the ACCEPTED velocity, matching the
                // EndVelClamped convention ("what you actually got").
                recordAnomaly(AnomalyType::HandoffBounded, (float)target,
                              bounded, now_us);
                vf = (double)bounded;
            }
        }
        // The ACCEPTED handoff, post-RFC-008 guard and pre wall/vmax guard.
        // This — not the raw wire value — is what the af series is built from
        // below: the backward difference is an estimate of the SENDER'S
        // curvature at the knot, and once a handoff has been bounded the
        // sender's claimed velocity there is precisely the number we have
        // decided not to believe. Feeding it forward is how one oversized
        // tangent used to poison the NEXT segment's boundary conditions too.
        // Identical to the old behavior whenever the guard does not engage,
        // which is every well-behaved stream.
        const double vf_handoff = vf;
        vf = applyEndVelGuard(vf, target, now_us);

        // End acceleration: the wire carries no af, but consecutive G values
        // imply it (backward difference). Measured on the bench: af=source
        // cuts waveform RMS ~3x vs af=0; the backward difference approximates
        // that. Only trusted between consecutive G-bearing commands of a
        // dense-ish segment chain.
        double af = 0.0;
        if (cmd.has_end_vel && _prev_vf_ok && now_us > _prev_vf_us) {
            const double gap = (double)(now_us - _prev_vf_us) * 1e-6;
            if (gap < 3.0 * T) af = (vf_handoff - _prev_vf) / gap;
        }
        if (cmd.has_end_vel) {
            _prev_vf = vf_handoff;   // accepted handoff, pre wall/vmax guard
            _prev_vf_us = now_us;
            _prev_vf_ok = true;
        } else {
            _prev_vf_ok = false;
        }

        // ---- BAD-MOVE BRIDGE (Config::bridge_ratio) -------------------------
        // Placed AFTER the handoff/wall guards have settled vf, and BEFORE any
        // curve is built — because the question it answers is prior to shape:
        // "is the commanded duration a deadline, or a fiction?"
        //
        // When the machine could be there far sooner than it was told to be,
        // the command is a DISCONTINUITY, not a stroke. Shaping a polynomial
        // across it is what manufactures the arc (see the field's note): a
        // fixed duration plus fixed endpoints uniquely determines the shape,
        // so the excess time is spent as excursion. Planning time-optimally
        // instead arrives early and holds — much closer to the sender's intent
        // than a 118 mm detour it never asked for.
        //
        // The guard path is NOT an alternative here: it pins minimum_duration
        // to T and therefore arcs for exactly the same reason.
        if (_cfg.bridge_ratio > 0.0f && T > 0.0) {
            const double t_opt = timeOptimalDuration(p, v, a, target, vf);
            if (t_opt > 0.0 && T > (double)_cfg.bridge_ratio * t_opt) {
                if (planRuckig(p, v, a, target, vf, 0.0, 0.0, now_us)) {
                    _mode = Mode::Waveform;
                    // The whole stroke is delivered, early. Nothing is owed at
                    // this extreme; slack 0 because the machine was never the
                    // binding constraint — the schedule was.
                    const int8_t bdir = target >= p ? (int8_t)1 : (int8_t)-1;
                    noteWaveformExtreme(bdir, target, target, 0.0, 0.0, now_us);
                    // Reported as a stretched deadline with the sign reversed:
                    // the plan is SHORTER than commanded, and detail carries the
                    // duration actually adopted, same convention as the guard's.
                    recordAnomaly(AnomalyType::DeadlineStretched, (float)target,
                                  (float)t_opt, now_us);
                    return true;
                }
                // Ruckig refused — fall through and shape it the old way rather
                // than leave the segment unplanned.
            }
        }

        // ---- ARM THE OVERSHOOT GUARD FOR THIS SEGMENT -----------------------
        // Once per commit, before any curve exists, because every candidate the
        // policies bisect over shares the same entry state and therefore the same
        // physical floor. Placed AFTER the bridge so a segment the bridge takes
        // pays nothing for it.
        _oshoot_allow = armOvershootAllow(p, v, a, target, vf, T);

        // Build the quintic in normalized tau; scaled boundary derivatives.
        double c[6];
        buildWaveformCurve(p, v, a, target, vf, af, T, c);

        // Stroke geometry + the centering debt owed at the extreme this stroke
        // heads toward. Computed ONCE, here, and handed to whichever sizing
        // rule ends up running: the debt is a property of the BAND the sender
        // is drawing, not of the policy that happened to pick the endpoint
        // (that coupling was artificial — Scale needs centering exactly as much
        // as Reshape does, and for the same reason).
        const int8_t dir   = target >= p ? (int8_t)1 : (int8_t)-1;
        const double sgn   = dir > 0 ? 1.0 : -1.0;
        const double adist = std::fabs(target - p);
        const double pull  = wavePull(dir, adist, now_us);

        double worst = quinticWorstRatio(c, T, _oshoot_allow);
        if (worst <= 1.0) {
            // The commanded segment is LEGAL — the machine can deliver all of
            // it, on the clock, as the sender's own spline.
            //
            // ...and delivering all of it is exactly what walks the band off
            // center when the OTHER direction is being clipped (the operator's
            // measured case: down-strokes quintic-feasible thanks to the wire
            // end velocities, up-strokes reach-limited → the band sat 26.8 mm
            // low and never self-corrected, because "feasible segments are
            // untouched" was a rule). So when a debt is outstanding at this
            // extreme, we deliberately stop short of a target we could have
            // hit. That is a ground-truth-visible deviation on a stroke the
            // machine could have made — it gets its own anomaly kind, never
            // silence.
            if (centeringArmed() && pull > kCenterEps) {
                const double goal = clamp01(target - sgn * pull);
                const double gvf  = applyEndVelGuard(vf, goal, now_us);
                double gc[6];
                buildWaveformCurve(p, v, a, goal, gvf, af, T, gc);
                if (quinticWorstRatio(gc, T, _oshoot_allow) <= 1.0) {
                    adoptQuintic(gc, T, now_us);
                    // Machine shortfall 0: the machine could have reached the
                    // commanded target. That zero is the signal that lets the
                    // OTHER extreme relax again (see the centering note) — it
                    // is why the band converges instead of ratcheting shut.
                    noteWaveformExtreme(dir, target, goal, 0.0, 1.0 - worst,
                                        now_us);
                    recordAnomaly(AnomalyType::WaveformCentered, (float)goal,
                                  (float)(std::fabs(goal - p) /
                                          std::fmax(adist, 1e-6)), now_us);
                    return true;
                }
                // The SHORTENED shape broke a ceiling the full one did not:
                // possible, because the same (vf, af) boundary conditions over
                // a shorter span curve harder. Symmetry is a preference,
                // legality is not — run the segment exactly as commanded.
            }
            // Full commanded stroke, nothing owed afterwards: a stroke the
            // machine delivers in full leaves no amplitude to share out at the
            // next extreme. (State only; not one sample of this plan changes.)
            // `1 - worst` is this segment's ceiling margin — an easy stroke
            // says the machine has room, which is what lets an old debt let go
            // quickly instead of shortening a section that no longer needs it.
            noteWaveformExtreme(dir, target, target, 0.0, 1.0 - worst, now_us);
            adoptQuintic(c, T, now_us);
            return true;
        }

        // The commanded shape is illegal. Before surrendering the deadline to
        // the Ruckig guard, the timing-first policies ask the other questions:
        //   Scale   — can we keep the schedule and give up amplitude instead?
        //   Reshape — how much of this stroke can the MACHINE actually deliver
        //             on schedule if we stop insisting on the quintic shape?
        // (See InfeasiblePolicy — for a scheduled sender these are the only
        // non-degenerate answers, because the next segment preempts us anyway.)
        if (_cfg.infeasible_policy == InfeasiblePolicy::Scale &&
            commitWaveformScaled(p, v, a, target, vf, af, T, dir, pull, now_us)) {
            return true;
        }
        if (_cfg.infeasible_policy == InfeasiblePolicy::Reshape &&
            commitWaveformReshaped(p, v, a, target, vf, T, worst, dir, pull,
                                   now_us)) {
            return true;
        }
        if ((_cfg.infeasible_policy == InfeasiblePolicy::PrioritizeAmplitude ||
             _cfg.infeasible_policy == InfeasiblePolicy::PrioritizeSmooth ||
             _cfg.infeasible_policy == InfeasiblePolicy::Blend) &&
            commitWaveformBudgeted(p, v, a, target, vf, af, T, dir, pull,
                                   now_us)) {
            return true;
        }

        // Quintic broke a ceiling or the window → the Ruckig guard takes the
        // segment: stretched to the deadline when feasible, physical minimum
        // (+ DeadlineStretched) when not. This is the "absurd wire command"
        // path the old cubic executed verbatim.
        recordAnomaly(AnomalyType::WaveformFallback, (float)target,
                      (float)worst, now_us);

        // THE DEADLINE IS KEPT HERE, and switching the guarded case to a
        // TIME-OPTIMAL plan instead was tried and rejected, not overlooked. The
        // premise looked sound — a stretched profile must spend T, and spending T
        // is what manufactures an arc — but the measurement says otherwise:
        // whole shelf, window 50-150, guard 1, 2026-07-30, per-segment excursion
        // came back unchanged (12 recordings, largest move 0.66 -> 0.72 mm, i.e.
        // the wrong way) while sender rms rose on 8 of 12 (GoogleCat 1.83 ->
        // 2.14, synth-sine 0.89 -> 1.09, SYN-mixed 2.80 -> 3.25). Arriving early
        // and holding costs the sender's timing and buys no excursion back.
        const bool ok = planRuckig(p, v, a, target, vf, 0.0, T, now_us);
        if (ok) {
            _mode = Mode::Waveform;
            // The guard delivers the WHOLE stroke (late, if it must), so this
            // extreme owes nothing: shortfall 0, machine term 0, which walks
            // the debt down instead of letting a stale one shorten an
            // unrelated later reversal. (Slack 0 — the guard overran the
            // deadline, which is the opposite of the machine having room.)
            noteWaveformExtreme(dir, target, target, 0.0, 0.0, now_us);
            if (_traj.get_duration() > T * 1.02 + 0.001) {
                recordAnomaly(AnomalyType::DeadlineStretched, (float)target,
                              (float)_traj.get_duration(), now_us);
            }
        }
        return ok;
    }

    // Quintic Hermite coefficients in normalized tau ∈ [0,1] for the boundary
    // conditions (p,v,a) → (target, vf, af) over duration T. Factored out of
    // commitWaveform so the Scale policy can re-solve the SAME curve family
    // against a shrunk target without duplicating (or drifting from) the
    // algebra — one copy of the math, one shape.
    static void buildQuintic(double p, double v, double a, double target,
                             double vf, double af, double T, double* c) {
        const double V0 = v * T, A0 = a * T * T;
        const double VF = vf * T, AF = af * T * T;
        const double R1 = target - p - V0 - A0 / 2.0;
        const double R2 = VF - V0 - A0;
        const double R3 = AF - A0;
        c[0] = p; c[1] = V0; c[2] = A0 / 2.0;
        c[3] =  10.0 * R1 - 4.0 * R2 + R3 / 2.0;
        c[4] = -15.0 * R1 + 7.0 * R2 - R3;
        c[5] =   6.0 * R1 - 3.0 * R2 + R3 / 2.0;
    }

    // Cubic Hermite for (p, v) → (target, vf) over T, written into the SAME six
    // coefficients with c[4] = c[5] = 0. A cubic IS a quintic with two zero
    // high-order terms, so every downstream consumer — the legality scan, the
    // peak-jerk closed form, adoptQuintic, the sampler — works unchanged and
    // cannot disagree about what it is looking at. One evaluator, two families.
    //
    // Deliberately takes NO `a` and NO `af`: that is the whole point (see
    // CurvePolicy). The plan starts at whatever acceleration the cubic's own
    // algebra implies, which is how the script's knot step gets reproduced.
    //
    // Note a cubic has CONSTANT jerk within a span (6·c3/T³), so the legality
    // scan's jerk term becomes a single number per plan rather than a sweep —
    // still the same referee, just an easier question.
    static void buildCubic(double p, double v, double target, double vf,
                           double T, double* c) {
        const double V0 = v * T, VF = vf * T, d = target - p;
        c[0] = p;
        c[1] = V0;
        c[2] =  3.0 * d - 2.0 * V0 - VF;
        c[3] = -2.0 * d + V0 + VF;
        c[4] = 0.0;
        c[5] = 0.0;
    }

    // policy + declaration -> reconstruction family, in ONE place. Static and
    // public because a caller can need the answer BEFORE commitWaveform adopts
    // the command — the bench's sender-curve overlay draws the client's own
    // curve at commit time, and an overlay that picks its family by a private
    // re-derivation is how a tuner ends up comparing a cubic against a quintic
    // and calling the difference a planner error.
    // Is the WAVEFORM path reconstructing with a cubic right now? As of RFC-030
    // the curve_family wire signaling EXISTS, so FollowClient finally has
    // something to follow: the family the ACTIVE waveform command declared. The
    // machine override still outranks the declaration, exactly as the policy
    // enum promises.
    bool waveformIsCubic() const { return resolveCubic(_cfg.curve_policy, _client_curve_family); }

    // THE waveform-path curve builder. Every sizing rule (plain commit,
    // centering, Scale, the budgeted search) goes through here rather than
    // calling buildQuintic directly, so the family is chosen in exactly one
    // place and no policy can silently disagree with another about it.
    void buildWaveformCurve(double p, double v, double a, double target,
                            double vf, double af, double T, double* c) const {
        if (waveformIsCubic()) buildCubic(p, v, target, vf, T, c);
        else                   buildQuintic(p, v, a, target, vf, af, T, c);
    }

    void adoptQuintic(const double* c, double T, uint64_t now_us) {
        _pend_ok = false;   // any adoption supersedes a scheduled successor
        for (int i = 0; i < 6; i++) _q_c[i] = c[i];
        _q_T        = T;
        _kind       = waveformIsCubic() ? PlanKind::Cubic : PlanKind::Quintic;
        _mode       = Mode::Waveform;
        _plan_start = now_us;
        _plan_jerk_frac = (float)(quinticPeakJerk(c, T) /
                                  std::fmax((double)_cfg.limits.jmax, 1e-9));
    }

    // Peak |jerk| of a quintic, closed form — no scan needed. In real time
    // j(tau) = (60·c5·tau² + 24·c4·tau + 6·c3) / T³, a QUADRATIC in tau, so its
    // extremum over [0,1] is one of the two endpoints or the vertex. Used for
    // Snapshot::sharpness so the field means "peak jerk of this plan" on both
    // plan kinds rather than "whatever ceiling the policy happened to use".
    static double quinticPeakJerk(const double* c, double T) {
        if (!(T > 0.0)) return 0.0;
        const double T3 = T * T * T;
        auto j = [&](double tau) {
            return std::fabs((60.0 * c[5] * tau + 24.0 * c[4]) * tau
                             + 6.0 * c[3]) / T3;
        };
        double pk = std::fmax(j(0.0), j(1.0));
        if (std::fabs(c[5]) > 1e-300) {
            const double tv = -c[4] / (5.0 * c[5]);   // dj/dtau = 0
            if (tv > 0.0 && tv < 1.0) pk = std::fmax(pk, j(tv));
        }
        return pk;
    }

    // ---- THE SMOOTHNESS AXIS (0.8.0) ----------------------------------------
    // handle reduction toward the chord
    // Lerp the span's END handle toward its own chord slope by `alpha`, so the
    // curve walks continuously from the sender's spline (alpha = 0) to a dead
    // straight line (alpha = 1). See InfeasiblePolicy for the derivation, the
    // 1.875x headroom ceiling this axis can buy, and why alpha = 1 is C0.
    //
    // ONLY THE END MOVES: the start (p, v, a) is the machine's ACTUAL state and
    // planning from it is doctrine, not a preference. Sufficient anyway — the
    // reduction propagates, because segment N's blended vf IS segment N+1's
    // actual starting v.
    //
    // af GOES TO ZERO rather than toward some blended curvature, for two
    // reasons: a straight line HAS no curvature, and af was never on the wire
    // in the first place (it is a backward-difference ESTIMATE of the sender's
    // curvature — see the af series in commitWaveform). It is the least
    // authoritative number in the boundary set, so it is the first that should
    // give way.
    static void blendEndTowardChord(double p, double target, double T,
                                    double alpha, double& vf, double& af) {
        if (!(T > 0.0) || alpha <= 0.0) return;
        const double al    = alpha > 1.0 ? 1.0 : alpha;
        const double chord = (target - p) / T;
        vf = (1.0 - al) * vf + al * chord;
        af = (1.0 - al) * af;
    }

    // One trial of the two-axis search: build the quintic for (amplitude f,
    // smoothness alpha) and return its worst ceiling/window ratio. THE ONLY
    // place that knows how the two axes compose, so the searches below cannot
    // drift apart from each other.
    //   f     in [-1, 1] : midpoint-anchored amplitude (f = 1 is the full
    //                      commanded stroke, f = 0 stops at the segment
    //                      midpoint, f = -1 does not move). Same geometry
    //                      Reshape bisects, so the two policies size strokes
    //                      the same way.
    //   alpha in [0, 1]  : handle reduction, 0 is the sender's own curve.
    // The end velocity is scaled by the same (1+f)/2 as the travel: a shortened
    // stroke that still demanded the full handoff velocity would be annihilated
    // by applyEndVelGuard at the wall anyway.
    double budgetedTrial(double p, double v, double a, double target, double vf,
                         double af, double T, double f, double alpha,
                         double& out_ep, double* out_c) const {
        const double mid = 0.5 * (p + target);
        const double ep  = clamp01(mid + f * (target - mid));
        double tvf = vf * 0.5 * (1.0 + f);
        double taf = af;
        blendEndTowardChord(p, ep, T, alpha, tvf, taf);
        buildWaveformCurve(p, v, a, ep, tvf, taf, T, out_c);
        out_ep = ep;
        return quinticWorstRatio(out_c, T, _oshoot_allow);
    }

    // ---- InfeasiblePolicy::PrioritizeAmplitude / PrioritizeSmooth -----------
    // Spend one axis up to its budget, then the other one freely; adopt the
    // first legal quintic found. Returns false only when BOTH axes are
    // exhausted and the shape is still illegal — the routine "conservative
    // limits + aggressive script" case — which falls through to the Ruckig
    // guard exactly as every other policy does.
    //
    // Each policy has exactly ONE budget: the cap on the axis it spends FIRST,
    // i.e. the axis it is willing to sacrifice in order to protect the other.
    // The fallback axis is uncapped, because at that point the alternative is
    // not a nicer plan, it is the guard.
    bool commitWaveformBudgeted(double p, double v, double a, double target,
                                double vf, double af, double T, int8_t dir,
                                double pull, uint64_t now_us) {
        if (!(T > 0.0)) return false;
        const bool smooth_first =
            _cfg.infeasible_policy == InfeasiblePolicy::PrioritizeAmplitude;

        double budget = smooth_first ? (double)_cfg.infeasible_smooth_budget
                                     : (double)_cfg.infeasible_amplitude_budget;
        budget = budget < 0.0 ? 0.0 : (budget > 1.0 ? 1.0 : budget);

        const int asteps = _cfg.infeasible_blend_steps < 1 ? 1
                         : (_cfg.infeasible_blend_steps > 10 ? 10
                            : (int)_cfg.infeasible_blend_steps);
        const int fsteps = _cfg.infeasible_reshape_steps > 8
                               ? 8 : (int)_cfg.infeasible_reshape_steps;

        double c[6], ep = target;
        double adopted_alpha = 0.0, adopted_f = 1.0, adopted_worst = 0.0;
        bool   found = false;

        // Smallest legal alpha in [0, cap] at a fixed amplitude. Invariant:
        // lo is known-ILLEGAL, hi is known-LEGAL, so the loop converges on hi.
        auto findAlpha = [&](double cap, double f_fixed) -> bool {
            if (cap <= 0.0) return false;
            double tc[6], tep;
            if (budgetedTrial(p, v, a, target, vf, af, T, f_fixed, cap, tep, tc)
                > 1.0) {
                return false;              // even the full budget is not enough
            }
            double lo = 0.0, hi = cap;
            for (int i = 0; i < asteps; i++) {
                const double m = 0.5 * (lo + hi);
                double mc[6], mep;
                if (budgetedTrial(p, v, a, target, vf, af, T, f_fixed, m, mep, mc)
                    <= 1.0) {
                    hi = m;
                } else {
                    lo = m;
                }
            }
            adopted_worst = budgetedTrial(p, v, a, target, vf, af, T, f_fixed,
                                          hi, ep, c);
            if (adopted_worst > 1.0) return false;   // guard against a rounding edge
            adopted_alpha = hi;
            adopted_f     = f_fixed;
            return true;
        };

        // Largest legal amplitude f in [f_lo, 1] at a fixed alpha. Invariant:
        // lo is known-LEGAL, hi is known-ILLEGAL, so the loop converges on lo.
        auto findF = [&](double f_lo, double alpha_fixed) -> bool {
            if (f_lo >= 1.0) return false;
            double tc[6], tep;
            if (budgetedTrial(p, v, a, target, vf, af, T, f_lo, alpha_fixed, tep,
                              tc) > 1.0) {
                return false;              // even the full budget is not enough
            }
            double lo = f_lo, hi = 1.0;
            for (int i = 0; i < fsteps; i++) {
                const double m = 0.5 * (lo + hi);
                double mc[6], mep;
                if (budgetedTrial(p, v, a, target, vf, af, T, m, alpha_fixed, mep,
                                  mc) <= 1.0) {
                    lo = m;
                } else {
                    hi = m;
                }
            }
            adopted_worst = budgetedTrial(p, v, a, target, vf, af, T, lo,
                                          alpha_fixed, ep, c);
            if (adopted_worst > 1.0) return false;
            adopted_alpha = alpha_fixed;
            adopted_f     = lo;
            return true;
        };

        // ---- InfeasiblePolicy::Blend — ONE SLIDER, BOTH AXES AT ONCE ---------
        // The other four spend one axis to EXHAUSTION before touching the other,
        // which is why an infeasible segment arrives as a straight line: alpha
        // is driven to 1 (the chord) rather than to whatever it actually needed.
        //
        // This walks a RAY instead. A single sacrifice scalar s in [0,1] moves
        // BOTH axes together, in a ratio the slider sets:
        //     shape loss      alpha(s)     = s * blend * k
        //     amplitude loss  (1 - f)/2    = s * (1 - blend) * k
        // and the search returns the SMALLEST s that is legal. Degradation is
        // therefore proportional and continuous — a segment that is 10% over
        // gives up about 10% of the ray, not 100% of one axis.
        //
        // THE RAY MUST REACH THE BOX EDGE, AND `k` IS WHAT MAKES IT. Without it
        // (0.9.0) the two losses were `s*blend` and `s*(1-blend)`, so s = 1 landed
        // on the straight LINE BETWEEN the corners rather than on a corner — at
        // blend 0.5 the search exhausted itself at alpha 0.5 / f 0.0, an interior
        // point, with half of BOTH budgets still unspent. Everything past that
        // point fell through to the Ruckig guard, which is the flattest, latest
        // answer available: measured on OvershootTestThrobbing, 82 of 221
        // segments took the guard under Blend where Reshape took it twice. That
        // is the operator's "some strokes go suddenly linear", and it was this.
        // k = 1 / max(blend, 1 - blend) rescales the ray so s = 1 always lands on
        // whichever box edge the direction hits first, leaving the reachable set
        // no smaller than a sequential policy's.
        //
        // The endpoints are UNCHANGED by k (it is 1 at both): blend = 1 spends
        // only smoothness (f stays 1), blend = 0 spends only amplitude (alpha
        // stays 0, f floors at -1, the same floor findF uses). Everything between
        // them is new, and is the whole point of the knob.
        if (_cfg.infeasible_policy == InfeasiblePolicy::Blend) {
            double bl = (double)_cfg.infeasible_blend;
            bl = bl < 0.0 ? 0.0 : (bl > 1.0 ? 1.0 : bl);
            const double kmax = bl > 1.0 - bl ? bl : 1.0 - bl;
            const double k    = kmax > 1e-9 ? 1.0 / kmax : 1.0;
            const int steps = asteps > fsteps ? asteps : fsteps;
            auto at = [&](double s, double& fo, double& ao) {
                ao = s * bl * k;
                if (ao > 1.0) ao = 1.0;
                double loss = s * (1.0 - bl) * k;
                if (loss > 1.0) loss = 1.0;
                fo = 1.0 - 2.0 * loss;
            };
            double fs, as;
            at(1.0, fs, as);
            double tc[6], tep;
            if (budgetedTrial(p, v, a, target, vf, af, T, fs, as, tep, tc) <= 1.0) {
                // s=1 legal (known), s=0 assumed illegal (the caller only gets
                // here because the full-fidelity curve failed) — bisect down.
                double lo = 0.0, hi = 1.0;
                for (int i = 0; i < steps; i++) {
                    const double m = 0.5 * (lo + hi);
                    double mf, ma, mc[6], mep;
                    at(m, mf, ma);
                    if (budgetedTrial(p, v, a, target, vf, af, T, mf, ma, mep, mc) <= 1.0)
                        hi = m;
                    else
                        lo = m;
                }
                at(hi, fs, as);
                adopted_worst = budgetedTrial(p, v, a, target, vf, af, T, fs, as, ep, c);
                if (adopted_worst <= 1.0) {
                    adopted_alpha = as;
                    adopted_f     = fs;
                    found         = true;
                }
            }
            // Not found here falls through to the Ruckig guard below, exactly as
            // the sequential policies do when both axes are spent.
            //
            // A SECOND SWEEP OF SMOOTHNESS AT FULL AMPLITUDE WAS TRIED HERE AND
            // REJECTED, not merely skipped: `findAlpha(1.0, 1.0)` before
            // conceding does find more legal shapes, and they are worse ones.
            // Measured 2026-07-30 at window 50-150, guard 1, whole shelf —
            // InterpTest1 per-segment excursion 0.23 -> 3.42 mm max and sender
            // rms 2.08 -> 4.20, GoogleCat 0.16 -> 1.15 mm. A curve that satisfies
            // the guard against its OWN band can still hand the next segment a
            // boundary state that does not, and taking it costs the deadline
            // honesty the guard path at least keeps.
        } else if (smooth_first) {
            // Spend SMOOTHNESS up to the budget at full amplitude; if that is
            // not enough, hold smoothness AT the budget and spend amplitude.
            found = findAlpha(budget, 1.0);
            if (!found) found = findF(-1.0, budget);
        } else {
            // Spend AMPLITUDE up to the budget with the sender's own curve; if
            // that is not enough, hold amplitude AT the budget and spend
            // smoothness. budget is the max FRACTION of stroke surrendered, and
            // surrendered(f) = (1-f)/2, so the floor is f = 1 - 2*budget.
            const double f_lo = 1.0 - 2.0 * budget;
            found = findF(f_lo, 0.0);
            if (!found) found = findAlpha(1.0, f_lo);
        }
        if (!found) return false;          // both axes spent → Ruckig guard

        // ---- Centering, same contract as the other policies -----------------
        // A debt outstanding at this extreme may pull the endpoint IN further
        // than the search did. Whichever constraint BINDS is the one reported.
        bool center_bound = false;
        if (centeringArmed() && pull > kCenterEps) {
            const double sgn  = dir > 0 ? 1.0 : -1.0;
            const double goal = clamp01(target - sgn * pull);
            if (std::fabs(goal - p) < std::fabs(ep - p)) {
                double gc[6], gep;
                // Re-express the centering goal in the search's own geometry so
                // one code path builds every adopted shape.
                const double denom = target - 0.5 * (p + target);
                const double gf = std::fabs(denom) > 1e-12
                                      ? (goal - 0.5 * (p + target)) / denom
                                      : adopted_f;
                const double gw = budgetedTrial(p, v, a, target, vf, af, T, gf,
                                                adopted_alpha, gep, gc);
                if (gw <= 1.0) {
                    for (int i = 0; i < 6; i++) c[i] = gc[i];
                    ep = gep; adopted_f = gf; adopted_worst = gw;
                    center_bound = true;
                }
                // A shortened shape can break a ceiling the longer one did not
                // (same boundary conditions over a shorter span curve harder).
                // Symmetry is a preference, legality is not — keep the search's
                // answer.
            }
        }

        adoptQuintic(c, T, now_us);

        // ---- Telemetry: one event per axis actually spent -------------------
        // Never silent, and never a lie about WHICH axis paid.
        if (adopted_alpha > 1e-6) {
            recordAnomaly(AnomalyType::WaveformSmoothed, (float)ep,
                          (float)adopted_alpha, now_us);
        }
        const double adist    = std::fabs(target - p);
        const double achieved = adist > 1e-9 ? std::fabs(ep - p) / adist : 1.0;
        if (adopted_f < 1.0 - 1e-6) {
            recordAnomaly(center_bound ? AnomalyType::WaveformCentered
                                       : AnomalyType::WaveformScaled,
                          (float)ep, (float)achieved, now_us);
        }
        // Machine shortfall is zero when CENTERING chose the endpoint — the
        // machine could have gone further. That zero is what lets the other
        // extreme relax again instead of the band ratcheting shut.
        noteWaveformExtreme(dir, target, ep,
                            center_bound ? 0.0 : std::fabs(target - ep),
                            1.0 - adopted_worst, now_us);
        return true;
    }

    // ---- InfeasiblePolicy::Scale — keep the deadline, shrink the stroke -----
    // Returns true if a SCALED quintic spanning exactly the commanded duration
    // T was adopted; false leaves everything untouched for the Ruckig guard.
    //
    // Sizing (the legality scan is still the referee — but the first guess is
    // now the ANSWER, not a search start). This path plans a min-jerk QUINTIC,
    // so size it with the quintic's own peak/mean ratios rather than a
    // trapezoid's. For a rest-to-rest quintic covering d in T:
    //     v_peak = 15/8 · d/T        a_peak = 10/√3 · d/T²      j_peak = 60·d/T³
    // Invert each against its ceiling and take the binding one:
    //     D_max = min( vmax·T/1.875, amax·T²/5.7735, jmax·T³/60 ) · margin
    //
    // WHY NOT THE TRAPEZOID FORM (this code shipped with it — do not go back):
    // min(amax·T²/4, vmax·T − vmax²/amax) describes a bang-cruise-bang chord,
    // which is a far more aggressive shape than a quintic, and it ignores jerk
    // entirely. On the real limit set (vmax 1.1, amax 16, jmax 500 — window
    // 500 mm) it overshot the largest LEGAL quintic stroke at every realistic
    // segment duration, so every single Scale event burned the whole retry
    // ladder to converge from above and still landed ~20 % short:
    //     T = 0.10 s : trapezoid 0.0316 vs quintic-legal 0.0083  (jerk-bound)
    //     T = 0.20 s : trapezoid 0.1328 vs quintic-legal 0.0667  (jerk-bound)
    //     T = 0.40 s : trapezoid 0.3352 vs quintic-legal 0.2347  (velocity-bound)
    //     T = 0.60 s : trapezoid 0.5376 vs quintic-legal 0.3520  (velocity-bound)
    // The closed form above reproduces the right-hand column exactly.
    //
    // The retry ladder SURVIVES, shortened (3 × 0.85): the closed form assumes
    // a rest-to-rest stroke, and a segment arriving with the carriage already
    // moving — or with a wire-supplied end velocity, or near a window wall —
    // can still fail the scan at the closed-form size. Those cases are the
    // ladder's whole remaining job. Cost is bounded and plan-time only
    // (≤ 3 × 65 polynomial evaluations, on an event that just failed anyway).
    //
    // CENTERING RIDES ON TOP (0.5.0), and it rides on top AFTER the sizing rule
    // has had its say — the ladder runs from d_max exactly as it always did,
    // and only then is the centered (shorter) endpoint tried, once, with its own
    // legality scan. That ordering is not cosmetic: the debt rule needs the
    // SHAPE-forced shortfall measured with the centering pull absent, or the
    // control loop is reading back its own output. Feeding the pull into the
    // ladder's first guess (the obvious implementation, tried first) let the
    // ladder's 15 %-per-rung steps into the feedback path and produced a stable
    // ±5 mm / 10 mm-amplitude wobble on the operator's mixed chain — the same
    // class of oscillation Reshape avoids by probing the machine's reach
    // against the COMMANDED target rather than the centered one.
    // Cost when the pull binds: one extra quintic build + 65-point scan. No
    // Ruckig calls — this path has never had any.
    bool commitWaveformScaled(double p, double v, double a, double target,
                              double vf, double af, double T, int8_t dir,
                              double pull, uint64_t now_us) {
        const double vc = (double)_cfg.limits.vmax;
        const double ac = (double)_cfg.limits.amax;
        const double jc = (double)_cfg.limits.jmax;
        if (!(vc > 0.0) || !(ac > 0.0) || !(jc > 0.0) || !(T > 0.0)) return false;

        const double margin =
            _cfg.infeasible_scale_margin < 0.50f ? 0.50
          : _cfg.infeasible_scale_margin > 1.00f ? 1.00
                                                 : (double)_cfg.infeasible_scale_margin;

        // Largest quintic stroke each ceiling permits in exactly T; the
        // binding one wins.
        double d_max = vc * T / kQuinticVPeak;
        d_max = std::fmin(d_max, ac * T * T / kQuinticAPeak);
        d_max = std::fmin(d_max, jc * T * T * T / kQuinticJPeak);
        d_max *= margin;

        const double dist = target - p;
        const double adist = std::fabs(dist);
        // Nothing to scale: the move already fits inside the reachable
        // envelope, so the illegality lives in the boundary derivatives or the
        // window, not in the stroke length. Shrinking would not fix it — hand
        // the segment to the guard unchanged.
        if (!(adist > d_max)) return false;

        const double sgn = dist >= 0.0 ? 1.0 : -1.0;
        double sc[6];
        double st = 0.0;
        bool   sized = false;
        double d = d_max;
        for (int attempt = 0; attempt < kScaleTries; attempt++) {
            st = clamp01(p + sgn * d);
            // The end velocity is re-guarded against the SCALED target: the
            // wall the machine must be able to brake before moved with it.
            const double svf = applyEndVelGuard(vf, st, now_us);
            buildWaveformCurve(p, v, a, st, svf, af, T, sc);
            if (quinticWorstRatio(sc, T, _oshoot_allow) <= 1.0) { sized = true; break; }
            d *= kScaleShrink;
        }
        if (!sized) return false;   // scan never accepted → guard takes it

        // Everything the SHAPE refused, measured with the centering pull absent
        // (see the note above — this is the debt rule's machine term and it has
        // to be independent of the debt).
        const double mach = adist - std::fabs(st - p);

        // ---- Centering, once, on top of the sized endpoint ------------------
        bool centered = false;
        if (pull > kCenterEps) {
            const double d_goal = adist - pull;   // pull ≤ half the stroke
            if (d_goal > 0.0 && d_goal < std::fabs(st - p) - kCenterEps) {
                const double ct  = clamp01(p + sgn * d_goal);
                const double cvf = applyEndVelGuard(vf, ct, now_us);
                double cc[6];
                buildWaveformCurve(p, v, a, ct, cvf, af, T, cc);
                if (quinticWorstRatio(cc, T, _oshoot_allow) <= 1.0) {
                    for (int i = 0; i < 6; i++) sc[i] = cc[i];
                    st = ct;
                    centered = centeringArmed();   // see the note in Reshape
                }
                // Rejected: the shorter stroke curves harder under the same
                // boundary derivatives. Symmetry is a preference, legality is
                // not — keep the sized stroke.
            }
        }

        adoptQuintic(sc, T, now_us);
        // Slack 0: this path only runs on a segment the quintic envelope
        // already refused, so the shape had no room to give.
        if (centeringArmed()) {
            noteWaveformExtreme(dir, target, st, mach, 0.0, now_us);
        }
        // Honest reporting is the whole point of choosing Scale: the fraction
        // tells the operator (and the WebUI) exactly how much amplitude was not
        // delivered on schedule, and the KIND says which constraint bound —
        // the quintic envelope (WaveformScaled) or the band's midpoint
        // (WaveformCentered, i.e. the machine could have gone further).
        const double frac = std::fabs(st - p) / std::fmax(adist, 1e-6);
        recordAnomaly(centered ? AnomalyType::WaveformCentered
                              : AnomalyType::WaveformScaled,
                      (float)st, (float)frac, now_us);
        return true;
    }

    // ---- InfeasiblePolicy::Reshape — keep the deadline AND the machine's ----
    // ---- real reach, pay with the quintic's shape ---------------------------
    // Returns true if a Ruckig profile spanning exactly the commanded duration
    // T was adopted; false leaves everything untouched for the Ruckig guard.
    //
    // THE QUESTION SCALE ASKS WRONG. Scale sizes the stroke from the min-jerk
    // quintic's peak/mean ratios — a quintic never cruises, it accelerates and
    // decelerates for its whole span, so its peak velocity is 1.875× its mean.
    // That ratio is a property of the SHAPE, not of the machine. Measured on
    // the operator's machine (window 200 mm, vmax 5, amax 250, jmax 1e4) for a
    // real funscript segment of 140 mm in 167 ms:
    //     mean speed the move actually needs : 838 mm/s   (ceiling 1000 mm/s)
    //     largest legal QUINTIC stroke       :  82 mm     (59 %)   ← Scale
    //     largest legal flat-top stroke      : 122 mm     (87 %)   ← Reshape
    // Same machine, same deadline, same ceilings: 40 mm of amplitude was being
    // thrown away to preserve a curve shape the operator cannot see but whose
    // absence they can definitely feel.
    //
    // THE SEARCH. Ruckig's time-optimal duration IS the machine's honest
    // answer to "how long does this move take", so ask it directly:
    //   1. Probe the FULL commanded stroke at min_dur = 0. If it fits inside
    //      T, that IS the machine's reach — plan at min_dur = T for full
    //      amplitude on the exact deadline, the only casualty being the
    //      spline. (2 calculate() calls, + the step-3 sharpness search.)
    //   2. Otherwise bisect the endpoint for the largest stroke that does fit.
    //      Each probe is one calculate().
    //   3. (0.6.0) Whatever endpoint came out of that, plan it at the SOFTEST
    //      jerk ceiling that still reaches it by the deadline — sharpness is
    //      spent by degrees, not all at once. See softestFeasibleJerk and the
    //      header's SHARPNESS BEFORE AMPLITUDE note. Amplitude is never traded
    //      for smoothness: step 3 runs strictly after the endpoint is fixed.
    // Whichever branch ran, the adopted endpoint is then the nearer of that
    // reach and the centering goal — the search measures the MACHINE, the debt
    // decides how much of that reach we spend on this side of the band.
    //
    // The search parameter f runs [-1, +1] over the stroke geometry:
    //     endpoint(f) = mid + f·(target − mid),  travel(f) = |target−p|·(1+f)/2
    //   f = +1 → the full commanded stroke
    //   f =  0 → stop at the segment midpoint (half the stroke)
    //   f = -1 → do not move at all
    // so the bisection interval IS the full commanded stroke and N steps
    // resolve the delivered stroke to stroke/2^N (see
    // Config::infeasible_reshape_steps). NOTE the interval spans the COMMANDED
    // target even when a debt is outstanding — probing the shortened goal
    // instead would make the measured reach a function of the correction, i.e.
    // the control loop reading back its own output.
    // The end velocity is scaled with the same (1+f)/2 factor — a shortened
    // stroke that still demanded the full handoff velocity would just be
    // annihilated by applyEndVelGuard at the wall anyway (measured: scaling vf
    // alone, without the sizing fix, changed nothing at all).
    //
    // DC CENTERING — READ THIS BEFORE "SIMPLIFYING" IT. (The rule itself now
    // lives in wavePull/noteWaveformExtreme and serves Scale too; this is where
    // it was derived and measured, so the derivation stays here.)
    // A shrink that simply takes the farthest reachable endpoint is NEUTRALLY
    // STABLE in DC, and re-parameterizing the search about the segment's
    // midpoint does NOT change that: the geometry is a reparameterization of
    // the same endpoint set, and a greedy search returns the same answer
    // either way. MEASURED (0.30↔1.00 chain at 167 ms, operator's machine):
    // midpoint-parameterized greedy Reshape centers the achieved motion at
    // 0.6008 against a commanded 0.65 — a 9.8 mm sag, better than Scale's
    // 29.0 mm but still a sag. The mechanism is easy to see in the chain: from
    // the bottom extreme the machine cannot reach the top, so the top gets
    // clipped; the return trip is then SHORT ENOUGH TO FIT, so the bottom is
    // hit exactly — and the whole waveform ends up hanging off its bottom
    // extreme. Every full-amplitude return re-anchors the sag.
    //
    // The restoring force therefore has to be an actual asymmetry-aware rule:
    // when the previous stroke fell short of ITS extreme, this REVERSAL gives
    // up part of that shortfall at its own extreme (`_wave_owed`), which is
    // exactly the operator's "shorten the stroke, MIDPOINT ANCHORED". Applied
    // only on a direction reversal, because shortening an intermediate point of
    // a monotone ramp is not symmetry, it is lag.
    //
    // THE CONTROL LAW (0.5.0). Write the two extremes' achieved shortfalls as
    // x (bottom) and y (top); centered means x == y, and the band we want is
    // x = y = (A−R)/2, where A is the commanded amplitude and R the amplitude
    // the machine can actually deliver on the commanded clock. Each stroke
    // ends short by max(debt, whatever the sizing rule forced), and hands the
    // next reversal an updated debt:
    //
    //     level = d − relax·(d − m)          // d = achieved shortfall
    //     owed += LAG · (level − owed)       // m = sizing rule's own shortfall
    //
    // Three ingredients, each load-bearing, each learned the hard way:
    //
    //  * m, the MACHINE/SHAPE term — what the sizing rule itself refused
    //    (Ruckig's reach for Reshape, the quintic envelope for Scale, and ZERO
    //    for a segment that was feasible as commanded). The 0.4.0 rule was
    //    owed = ½·d with no m at all, and on a chain where one direction is
    //    feasible its fixed point is x = (A−R)/3 — a third of the way there.
    //  * relax, the RELEASE VALVE — the share of the machine's slack handed
    //    back. Without it (relax = 0) the debt is self-sustaining: a stroke
    //    shortened for symmetry reports a shortfall, which justifies the next
    //    shortening, and the band ratchets shut and never re-opens when the
    //    content gets easy again. Scaled UP by how much room the segment
    //    turned out to have (see `slack`), so an easy section drops an old
    //    debt in a handful of strokes instead of dragging it for ten seconds:
    //    measured on the 167 ms → 600 ms transition, full amplitude is back
    //    after 7 strokes (Reshape) / 9 (Scale), against 19+ with a flat relax.
    //  * LAG, the DAMPING — and this is the one that is not optional. The
    //    instantaneous rule (owed = level, no lag) has unity loop gain around
    //    the fixed point: a correction at one extreme reduces the next
    //    stroke's shortfall one-for-one, so the band does not converge, it
    //    ORBITS. Measured, mixed chain: a rock-stable period-4 cycle, band
    //    center swinging ±7 mm forever with the MEAN in exactly the right
    //    place — the kind of bug that looks fine in a summary statistic and
    //    feels like the machine wandering.
    //
    // Gains swept over the operator's chains at both speeds, both policies
    // (LAG 0.30–1.00 × relax 0.00–0.50): 0.50 / 0.25 is the knee — every band
    // spread ≤ 0.9 mm with the centering error at its best, while LAG ≥ 0.65
    // starts to ring again (3–4 mm spreads) and LAG ≤ 0.40 just converges
    // slower for no gain.
    //
    // Costs one double of state and NO extra Ruckig calls anywhere: the
    // feasible path is pure arithmetic plus one more 65-point quintic scan,
    // and only when a debt is actually outstanding.
    bool commitWaveformReshaped(double p, double v, double a, double target,
                                double vf, double T, double worst, int8_t dir,
                                double pull, uint64_t now_us) {
        if (!(T > 0.0)) return false;
        const double dist  = target - p;
        const double adist = std::fabs(dist);
        if (!(adist > 1e-9)) return false;  // degenerate: nothing to do
        const double sgn = dist >= 0.0 ? 1.0 : -1.0;

        // ---- 1. How far can the MACHINE actually go by the deadline? --------
        // Probed against the COMMANDED target, not the centered goal: the
        // machine's own shortfall is the debt rule's other half, and a probe
        // of the goal would only ever tell us "at least this far".
        double reach = target;               // farthest endpoint that fits in T
        double opt   = 0.0;
        double slack = 0.0;                  // deadline margin the machine had
        // Did the FULL commanded stroke fit at the mechanical ceiling? That is
        // also the question "is there any deadline slack left to spend on
        // SHAPE" — see step 4.
        bool   full_fits = false;
        if (probeRuckigDuration(p, v, a, target, vf, opt) && opt <= T) {
            slack = 1.0 - opt / T;
            full_fits = true;
        } else {
            // ---- 2. Bisect the endpoint about the midpoint ------------------
            const double mid  = 0.5 * (p + target);
            const double half = target - mid;   // signed half-stroke
            const int steps = _cfg.infeasible_reshape_steps > 8
                                  ? 8 : (int)_cfg.infeasible_reshape_steps;
            double lo = -1.0, hi = 1.0;      // hi is known infeasible (step 1)
            double best_f = -2.0;            // sentinel: nothing feasible yet
            for (int i = 0; i < steps; i++) {
                const double f  = 0.5 * (lo + hi);
                const double ep = clamp01(mid + f * half);
                // Quiet bound here: the bisection probes several candidate
                // endpoints and only ONE of them is ever adopted — recording an
                // EndVelClamped per probe would flood a 16-deep ring with
                // events that never happened.
                const double ev = endVelBound(vf * 0.5 * (1.0 + f), ep);
                double d = 0.0;
                if (probeRuckigDuration(p, v, a, ep, ev, d) && d <= T) {
                    best_f = f;              // largest feasible seen so far
                    lo = f;
                } else {
                    hi = f;
                }
            }
            if (best_f < -1.0) return false; // cannot even half-move → guard
            reach = clamp01(mid + best_f * half);
        }
        const double mach = std::fabs(target - reach);

        // ---- 3. Endpoint = whichever cap is tighter -------------------------
        // The machine's reach and the centering debt are both ceilings on the
        // same travel; the nearer one wins, and it is also the one the anomaly
        // must name (one event per segment, naming the BINDING constraint).
        const double goal =
            pull > kCenterEps ? clamp01(target - sgn * pull) : target;
        const bool pull_binds =
            std::fabs(goal - p) < std::fabs(reach - p) - kCenterEps;
        // Only an ARMED centering debt gets the new anomaly kind. With centering
        // off the pull is 0.4.0's own reversal debt, which has always reported
        // as WaveformScaled — the knob restores the telemetry too, not just the
        // motion.
        const bool centered = pull_binds && centeringArmed();
        const double ep   = pull_binds ? goal : reach;
        const double frac = std::fabs(ep - p) / std::fmax(adist, 1e-6);
        // The end velocity is scaled with the delivered travel fraction — a
        // shortened stroke that still demanded the full handoff velocity would
        // just be annihilated by applyEndVelGuard at the wall anyway (measured:
        // scaling vf alone, without the sizing fix, changed nothing at all).
        const double ev = applyEndVelGuard(vf * frac, ep, now_us);

        // ---- 4. Spend SHARPNESS before amplitude ----------------------------
        // The endpoint is settled; the only fidelity still on the table is the
        // SHAPE, and 0.5.0 always paid all of it (plan at jmax → short ramps,
        // long flat top, a straight line at velocity saturation). Buy back as
        // much curve as the deadline allows: the softest jerk ceiling that
        // still reaches `ep` in T. Amplitude and timing are untouched by
        // construction — this only changes HOW the same endpoint is reached.
        //
        // WHEN it is worth the probes, and this is the whole reason it is not
        // simply always-on:
        //   * full_fits — the machine had deadline slack at full amplitude, so
        //     there is definitely sharpness to give back. This is the branch
        //     0.5.0 got for 2 calculate() calls and the branch where the
        //     measured 74 %→21 % saturation win lives.
        //   * pull_binds — the CENTERING debt pulled the endpoint in short of
        //     the machine's reach, which manufactures slack even on a segment
        //     the machine could not fully deliver.
        // Otherwise `ep` IS the bisected reach, where jmax is marginal by
        // construction (that is what the bisection converged on) and the
        // search would spend its whole budget to return ~jmax. Skipping it
        // there keeps the expensive branch at exactly its 0.5.0 cost.
        double j_eff = (double)_cfg.limits.jmax;
        if (full_fits || pull_binds) {
            j_eff = softestFeasibleJerk(p, v, a, ep, ev, T);
        }
        if (!planRuckig(p, v, a, ep, ev, 0.0, T, now_us, j_eff)) {
            // Belt and braces: the search proved t_opt(j_eff) ≤ T with a
            // time-optimal probe, but the ADOPTED plan additionally pins
            // minimum_duration = T, and Ruckig's stretched-profile families are
            // not the time-optimal ones. If that ever refuses, fall straight
            // back to the ceiling the 0.5.0 engine would have used, so
            // "softening can only make a plan gentler, never make it fail" is
            // true BY CONSTRUCTION rather than by measurement. Costs one extra
            // calculate() on a path that has already failed once; never
            // observed firing on any bench chain or sweep, which is exactly the
            // kind of claim that stops being true the moment nobody guards it.
            if (j_eff >= (double)_cfg.limits.jmax) return false;
            if (!planRuckig(p, v, a, ep, ev, 0.0, T, now_us)) return false;
        }
        _mode = Mode::Waveform;
        noteWaveformExtreme(dir, target, ep, mach, slack, now_us);

        const double lost = std::fabs(target - ep);
        if (lost <= kCenterEps) {
            // Shape lost, nothing else: no WaveformScaled, and no
            // DeadlineStretched either (min_dur = T holds the deadline by
            // construction, and opt ≤ T means Ruckig is stretching, not
            // overrunning). See the anomaly-vocabulary note above.
            recordAnomaly(AnomalyType::WaveformFallback, (float)target,
                          (float)worst, now_us);
        } else {
            // Same honest-reporting contract as Scale: the fraction is how
            // much of the COMMANDED stroke actually got delivered on schedule.
            recordAnomaly(centered ? AnomalyType::WaveformCentered
                                  : AnomalyType::WaveformScaled,
                          (float)ep, (float)frac, now_us);
        }
        return true;
    }

    // Is the centering rule armed for this segment? Stretch is deliberately
    // exempt: it delivers the full amplitude (late) by definition, so there is
    // never a deficit to share out, and touching it would break the one policy
    // whose whole promise is "the stroke you asked for".
    bool centeringArmed() const {
        return _cfg.wave_centering &&
               _cfg.wave_centering_gain > 0.0f &&
               _cfg.infeasible_policy != InfeasiblePolicy::Stretch;
    }

    // The pull-in owed at the extreme a stroke in `dir` is heading toward.
    // Zero unless this is a REVERSAL and a debt is outstanding.
    double wavePull(int8_t dir, double adist, uint64_t now_us) const {
        if (_wave_dir == 0 || dir == _wave_dir) return 0.0;
        if (!centeringArmed()) {
            // 0.4.0 behavior: the debt existed, but only Reshape ever spent
            // it, and only up to a quarter of the stroke.
            return _cfg.infeasible_policy == InfeasiblePolicy::Reshape
                       ? std::fmin(_wave_owed, kLegacyOwedCap * adist)
                       : 0.0;
        }
        // A debt older than the phrase that created it is not a debt: after a
        // pause the machine has settled somewhere else entirely and shortening
        // the first stroke back would be a shrug, not symmetry.
        if (_wave_last_us != 0 && now_us > _wave_last_us &&
            (now_us - _wave_last_us) > kWaveDebtStaleUs) {
            return 0.0;
        }
        double g = (double)_cfg.wave_centering_gain;
        g = g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);
        // Capped at HALF the commanded stroke: enough headroom for the
        // (A−R)/(A+R) the converged state actually asks for (0.24 on the
        // operator's numbers — the 0.4.0 quarter cap would have clipped it and
        // stalled the convergence), while keeping the endpoint on the far side
        // of the start position by construction. Never negative, never past
        // the commanded target.
        const double pull = _wave_owed * g;
        return pull <= 0.0 ? 0.0 : std::fmin(pull, kCenterCapFrac * adist);
    }

    // Book-keeping for the centering rule: remember which way this stroke went
    // and update the debt the next REVERSAL will pay at the other extreme.
    // See the control-law note on commitWaveformReshaped for the derivation;
    // the two constants are why it settles instead of hunting.
    void noteWaveformExtreme(int8_t dir, double target, double achieved,
                             double mach_short, double slack, uint64_t now_us) {
        _wave_dir     = dir;
        _wave_last_us = now_us;
        const double d = std::fabs(target - achieved);
        if (!centeringArmed()) {
            _wave_owed = 0.5 * d;         // 0.4.0 rule, verbatim
            return;
        }
        // Machine slack: how much of this stroke's shortfall was OURS to give
        // back rather than the machine's to refuse. (m ≤ d by construction —
        // clamped anyway, a sizing rule is not a trusted input.)
        const double m = mach_short < 0.0 ? 0.0
                       : (mach_short > d ? d : mach_short);
        // How much ROOM the machine turned out to have on this segment, 0..1
        // (ceiling margin on the quintic path, deadline margin on the reshape
        // path, 0 wherever the sizing rule was the constraint). Different
        // formulas, same meaning, and it is only ever used to decide how fast
        // the debt lets go — never how large it is.
        const double sl = slack < 0.0 ? 0.0 : (slack > 1.0 ? 1.0 : slack);
        const double relax = kCenterRelax + (1.0 - kCenterRelax) * sl;
        const double level = d - relax * (d - m);
        _wave_owed += kCenterLag * (level - _wave_owed);
        if (_wave_owed < 0.0) _wave_owed = 0.0;
    }

    // ---- The sharpness dial: smallest jerk that still meets the deadline ----
    // Returns the SOFTEST jerk ceiling under which the move (p,v,a) → (ep, ev)
    // still finishes inside T. Called only after the caller has established
    // that the FULL mechanical ceiling does fit, so jmax is always a valid
    // answer and the worst this can do is hand back exactly what 0.5.0 used.
    //
    // WHY A BISECTION ON JERK IS WELL-POSED. t_opt(j) — Ruckig's time-optimal
    // duration for a fixed move — is monotonically DECREASING in j: more jerk
    // means shorter accel ramps and an earlier arrival, and no amount of jerk
    // can make a move slower. So "fits inside T" is an UP-SET in j (feasible
    // above a threshold j*, infeasible below), and a plain bisection resolves
    // its boundary in N probes with no hunting and no local minima. This is the
    // same cost class and the same machinery as the distance bisection above —
    // it just moves the OTHER axis of the (amplitude, sharpness) plane.
    //
    // THE INTERVAL IS FIXED, AND THAT IS THE MONOTONICITY GUARANTEE. The search
    // always runs [floor·jmax, jmax], never a demand-dependent range. The
    // obvious alternative — start from the min-jerk quintic's own implied peak
    // jerk, 60·d/T³, which the sizing code already knows as kQuinticJPeak —
    // is a strictly better first guess and was rejected anyway: a lower bound
    // that MOVES with the demand slides the bisection's dyadic grid with it, so
    // the answer stops being "the smallest grid point that fits" and becomes
    // non-monotone in demand by up to one quantum. With a fixed grid the result
    // is the smallest grid point ≥ j*(demand), which is a non-decreasing step
    // function of demand — provable, not merely measured. (The demand sweep in
    // the test suite asserts it regardless; monotone-in-demand was the
    // operator's stated requirement, "never non-monotone or hunting".)
    //
    // LOG SPACING, not linear: jerk spans decades. On the operator's machine
    // jmax normalizes to 1e4 and the measured critical jerk for real segments
    // ran from 4455 down to 694 (6.9 % of the ceiling) — a linear bisection
    // would spend every one of its steps in the top decade and resolve the
    // interesting end not at all.
    //
    // Cost: exactly `steps` calculate() calls, no allocation, no state. The
    // caller decides WHEN this is worth spending (see commitWaveformReshaped
    // step 4) — it is deliberately not spent on endpoints already known to be
    // marginal, where the answer is jmax by construction.
    double softestFeasibleJerk(double p, double v, double a, double ep,
                               double ev, double T) {
        const double jc = (double)_cfg.limits.jmax;
        if (!_cfg.infeasible_soften || !(jc > 0.0) || !(T > 0.0)) return jc;
        const int steps = _cfg.infeasible_soften_steps > 10
                              ? 10 : (int)_cfg.infeasible_soften_steps;
        if (steps <= 0) return jc;
        // A config push is not a trusted input (same doctrine as the scale
        // margin and the bisection depth): clamp, never assume.
        double frac = (double)_cfg.infeasible_soften_floor;
        if (!(frac > 0.0) || frac >= 1.0) return jc;   // pinned at the sharp end
        if (frac < 0.001) frac = 0.001;
        double lo   = std::log(frac * jc);   // known-or-assumed infeasible end
        double hi   = std::log(jc);          // known feasible (caller proved it)
        double best = jc;
        for (int i = 0; i < steps; i++) {
            const double m = 0.5 * (lo + hi);
            const double j = std::exp(m);
            double d = 0.0;
            double w = 0.0;
            // "Fits" is TWO questions, not one. Meeting the deadline was never
            // sufficient: a jerk ceiling too low to turn the boundary state
            // around produces a profile that arrives on time by sailing through
            // the velocity ceiling and out of the stroke window (see
            // ruckigWorstRatio for the measured table — the softest "feasible"
            // ceiling on the captured handoff planned 1.43x vmax). Legality is
            // an UP-SET in j for the same reason the deadline is: more jerk
            // means the profile can turn sooner, so it overshoots less. A
            // candidate that fails EITHER test raises the floor, and `best`
            // still starts at the mechanical ceiling the caller already proved
            // legal, so the worst this search can now return is 0.5.0's jmax.
            if (probeRuckigDuration(p, v, a, ep, ev, d, j, &w) && d <= T &&
                w <= 1.0 + kRuckigLegalEps) {
                best = j;   // softest that fits so far
                hi   = m;
            } else {
                lo = m;
            }
        }
        return best;
    }

    // Time-optimal duration of a candidate point move, computed WITHOUT
    // touching the active plan or the anomaly ring — a speculative question,
    // not a commitment. A rejected probe is simply "not feasible" (it never
    // increments _failures: nothing failed, we were only asking).
    // `worst_out`, when asked for, reports the probed profile's ceiling/window
    // ratio (see ruckigWorstRatio). Only the sharpness search asks: it is the
    // one caller that varies the jerk ceiling, and therefore the one caller
    // that can steer Ruckig into the region where it stops honoring vmax.
    bool probeRuckigDuration(double p, double v, double a, double target,
                             double vf, double& dur_out, double j_ovr = 0.0,
                             double* worst_out = nullptr) {
        ruckig::InputParameter<1> in;
        in.current_position[0]     = p;
        in.current_velocity[0]     = v;
        in.current_acceleration[0] = a;
        in.target_position[0]      = target;
        in.target_velocity[0]      = vf;
        in.target_acceleration[0]  = 0.0;
        in.max_velocity[0]         = _cfg.limits.vmax;
        in.max_acceleration[0]     = _cfg.limits.amax;
        in.max_jerk[0]             = jerkCeil(j_ovr);

        ruckig::Trajectory<1> traj;
        const ruckig::Result res = _calc.calculate(in, traj);
        if ((int)res < 0) return false;
        dur_out = traj.get_duration();
        if (worst_out) *worst_out = ruckigWorstRatio(traj, _oshoot_allow);
        return std::isfinite(dur_out);
    }

    // ---- ONE definition of legal, shared by both referees --------------------
    // Public because the native suite pins the two referees to a single answer
    // on curves both planners can draw; pure queries, they adopt nothing.
public:
    // Scores one sampled point: v/a ceilings, stroke window, overshoot band.
    // `lo`/`hi` are the judged curve's OWN endpoints; `allow` < 0 disarms the
    // band; > 1.0 = illegal. Jerk is the quintic referee's own term (Ruckig
    // cannot violate it, see ruckigWorstRatio). Window grace and band are
    // spelled ONCE here so the two planners cannot disagree about legality.
    //
    // WINDOW, WITH NO GRACE BAND. A plan permitted to bulge past the rail
    // arrives there still DRIVING OUTWARD and hands the follower momentum to
    // absorb (async-tune bench 2026-07-30: 69 samples pinned at the top rail,
    // worst +217 mm/s, 625 mm of travel on a 500 mm rail). The retired 0.02
    // was also 12x looser than MotionArbiter's own 0.5 mm wall.
    //
    // The BAND is two-sided: the measured pathology is a backswing AWAY from
    // the target (a move from 186.8 mm to 100 mm arcing up to 305 mm), which
    // a one-sided test scores negative and waves through.
    double pointWorst(double pp, double vv, double aa, double lo, double hi,
                      double allow) const {
        const double vc = _cfg.limits.vmax, ac = _cfg.limits.amax;
        double worst = 0.0;
        if (vc > 0.0) worst = std::fmax(worst, std::fabs(vv) / vc);
        if (ac > 0.0) worst = std::fmax(worst, std::fabs(aa) / ac);
        if (pp < 0.0) worst = std::fmax(worst, 1.0 + (-pp));
        if (pp > 1.0) worst = std::fmax(worst, 1.0 + (pp - 1.0));
        if (allow >= 0.0) {
            const double excess = std::fmax(lo - pp, pp - hi);
            if (excess > allow)
                worst = std::fmax(worst, 1.0 + (excess - allow));
        }
        return worst;
    }

    // Worst (peak / ceiling) ratio across v/a/j ceilings AND window bounds,
    // scanned on a fixed tau grid. > 1.0 = illegal quintic.
    // The allowance is a PARAMETER, never ambient state: waveform commits and
    // synthesis spans plan different curves from different entry states, and a
    // member read here judged one path's span with the other's bound.
    double quinticWorstRatio(const double* c, double T,
                             double oshoot_allow) const {
        const double jc = _cfg.limits.jmax;
        double worst = 0.0;
        // ---- overshoot-guard preamble (all no-ops when the guard is off) ----
        // The band is this trial's OWN endpoints, c[0] and the curve at tau = 1
        // (the sum of the coefficients), so a shortened or smoothed candidate
        // is judged against the stroke it actually draws.
        //
        // The ALLOWANCE is not recomputed here. It belongs to the commit, not to
        // the trial: it is the excursion physics forces on the move from the
        // caller's entry state, which every candidate in a bisection shares.
        // Recomputing it per trial would also mean a Ruckig solve inside the
        // innermost loop of three different searches. See _oshoot_allow.
        double oshoot_lo = 0.0, oshoot_hi = 0.0;
        if (oshoot_allow >= 0.0) {
            double p_end = 0.0;
            for (int k = 0; k < 6; k++) p_end += c[k];
            oshoot_lo = std::fmin(c[0], p_end);
            oshoot_hi = std::fmax(c[0], p_end);
        }
        for (int i = 0; i <= kScanSteps; i++) {
            const double tau = (double)i / kScanSteps;
            const double pp = ((((c[5]*tau + c[4])*tau + c[3])*tau + c[2])*tau + c[1])*tau + c[0];
            const double vv = ((((5*c[5]*tau + 4*c[4])*tau + 3*c[3])*tau + 2*c[2])*tau + c[1]) / T;
            const double aa = (((20*c[5]*tau + 12*c[4])*tau + 6*c[3])*tau + 2*c[2]) / (T*T);
            const double jj = ((60*c[5]*tau + 24*c[4])*tau + 6*c[3]) / (T*T*T);
            worst = std::fmax(worst, std::fabs(jj) / jc);
            worst = std::fmax(worst, pointWorst(pp, vv, aa, oshoot_lo,
                                                oshoot_hi, oshoot_allow));
        }
        return worst;
    }

    // The SAME referee, for a Ruckig profile. Deliberately identical in shape
    // and in what it calls legal: it scores every sample through the SAME
    // pointWorst predicate, with the same window and the same band allowance,
    // so the two planners cannot disagree about what "legal" means. Pinned by
    // the coincident-curve sweep in test/native/test_slopmotion.
    //
    // WHY THIS HAS TO EXIST — RUCKIG IS NOT A LEGALITY ORACLE. `max_velocity`
    // is an input to Ruckig's profile SEARCH, not a postcondition of its
    // output: when the jerk ceiling is too low to turn the boundary state
    // around, Community returns a profile that sails straight through the
    // velocity ceiling (and, further down, through the stroke window) rather
    // than reporting infeasible. Measured on the captured chase→segment
    // handoff (p 0.798, v −0.468, a −16, → 0.700 at vf −1.095, vmax 1.1):
    //
    //     jerk ceiling   peak |v| / vmax   position span
    //         4000            0.995         [0.700 .. 0.798]   legal
    //          400            0.995         [0.700 .. 0.798]   legal
    //          200            1.007         [0.648 .. 0.798]
    //          115            1.434         [0.457 .. 0.801]   <- the capture
    //          100            1.589         [0.369 .. 0.808]
    //           50            2.753         [-0.605 .. 0.853]  <- off the rail
    //
    // Jerk is NOT scanned: Ruckig's profiles are bang-bang in jerk at exactly
    // the ceiling it was handed, so the sample grid would only ever rediscover
    // that ceiling. Velocity, acceleration and the window are the properties it
    // can actually miss.
    double ruckigWorstRatio(const ruckig::Trajectory<1>& traj,
                            double oshoot_allow) const {
        const double dur = traj.get_duration();
        if (!(dur > 0.0) || !std::isfinite(dur)) return 0.0;
        double oshoot_lo = 0.0, oshoot_hi = 0.0;
        if (oshoot_allow >= 0.0) {
            double p0, p1, vv, aa;
            traj.at_time(0.0, p0, vv, aa);
            traj.at_time(dur, p1, vv, aa);
            oshoot_lo = std::fmin(p0, p1);
            oshoot_hi = std::fmax(p0, p1);
        }
        double worst = 0.0;
        for (int i = 0; i <= kScanSteps; i++) {
            double pp, vv, aa;
            traj.at_time(dur * (double)i / kScanSteps, pp, vv, aa);
            worst = std::fmax(worst, pointWorst(pp, vv, aa, oshoot_lo,
                                                oshoot_hi, oshoot_allow));
        }
        return worst;
    }

private:
    // THE OVERSHOOT GUARD'S REFERENCE: how far outside the commanded band the
    // machine travels when it is trying its hardest not to. The TIME-OPTIMAL
    // plan brakes with every bit of authority there is, so whatever excursion
    // survives it is the excursion this move physically costs — jerk ceiling,
    // velocity ceiling, requested arrival velocity and all. Anything beyond it
    // is the polynomial's invention, and that is exactly the line the guard
    // needs to draw.
    //
    // MEASURING BEATS THE CLOSED FORM because the closed form is wrong by the
    // jerk term (see Config::overshoot_guard for the 6.4 mm vs ~15 mm case that
    // made the old knob actively harmful). It is also self-correcting: a machine
    // with more jerk authority gets a tighter allowance with nothing to retune.
    //
    // ONE Ruckig solve per commit that arms the guard (a waveform segment, or a
    // synthesis span from its own entry state), on the same input the bad-move
    // bridge already probes. Returns < 0 when Ruckig has no opinion, which
    // disarms the guard for that curve rather than inventing a bound.
    double physicalBandExcess(double p, double v, double a, double target,
                              double vf) {
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
        ruckig::Trajectory<1> traj;
        if ((int)_calc.calculate(in, traj) < 0) return -1.0;
        const double dur = traj.get_duration();
        if (!(dur > 0.0) || !std::isfinite(dur)) return -1.0;
        const double lo = std::fmin(p, target), hi = std::fmax(p, target);
        double ex = 0.0;
        for (int i = 0; i <= kScanSteps; i++) {
            double pp, vv, aa;
            traj.at_time(dur * (double)i / kScanSteps, pp, vv, aa);
            ex = std::fmax(ex, std::fmax(lo - pp, pp - hi));
        }
        return ex;
    }

    // The overshoot bound for ONE commit, from THAT commit's entry state.
    // Returns < 0 when the guard is off or Ruckig declines to answer: no
    // answer means no bound, an invented one was the guard's original mistake.
    double armOvershootAllow(double p, double v, double a, double target,
                             double vf, double T) {
        if (!(_cfg.overshoot_guard > 0.0f) || !(T > 0.0)) return -1.0;
        const double floor_mm = physicalBandExcess(p, v, a, target, vf);
        if (floor_mm < 0.0) return -1.0;
        return (double)_cfg.overshoot_guard * floor_mm + kOvershootFloor
               + (double)_cfg.overshoot_chord_slack * std::fabs(target - p);
    }

    // ---- CHASE (bare / short-interval points) -------------------------------
    bool commitChase(const Command& cmd, double p, double v, double a,
                     double target, uint64_t now_us) {
        // A bare point declares no band, so there is nothing for the guard to
        // measure excursion against. Disarmed explicitly: the softened-plan
        // legality recheck in planRuckig reads this member.
        _oshoot_allow = -1.0;
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
            // acap-limited stream curvature. Limited BEFORE it is used
            // anywhere: the accel estimate is a second difference of a jittery
            // signal, and it feeds both the arrival acceleration and (below)
            // the aim POSITION, where an unlimited spike would fling the aim.
            const double acap  = 0.5 * (double)_cfg.limits.amax;
            const double a_est = _est_a_ema < -acap ? -acap
                               : _est_a_ema >  acap ?  acap : _est_a_ema;
            // Second-order aim: a straight-line extrapolation is wrong exactly
            // where a waveform turns, and the turn near a rail is where being
            // wrong costs the most (aim clamps to the wall → end-vel guard
            // sees dist-to-wall 0 → forced vf = 0 → the carriage parks at the
            // rail). ½·a_est·look² pulls the aim back through the crest.
            aim = clamp01(_cfg.chase_aim_accel_extrap
                              ? target + v_est * look + 0.5 * a_est * look * look
                              : target + v_est * look);
            // Arrival velocity must belong to the same instant as the aim.
            // Aiming `look` seconds ahead but requesting the velocity the
            // stream has NOW is internally inconsistent: through a crest the
            // stream is decelerating, so v_est(now) is too fast for the point
            // we plan to be at, and the plan arrives still climbing — straight
            // into the wall/overshoot the second-order aim term was added to
            // fix. Extrapolate the velocity over the same horizon with the
            // same acap-limited a_est (so a noisy second difference cannot
            // fling it either). One flag, one coherent "predictive aim v2":
            // with chase_aim_accel_extrap off, both the aim's a-term and this
            // revert to the pre-v2 behavior byte for byte.
            const double vf_req = _cfg.chase_aim_accel_extrap
                                      ? v_est + a_est * look
                                      : v_est;
            vf  = applyEndVelGuard(vf_req, aim, now_us);
            if (_cfg.chase_accel_ff) af = a_est;
        } else if (cmd.has_end_vel) {
            vf = applyEndVelGuard((double)cmd.end_vel, aim, now_us);
        }
        double j_ovr = 0.0;
        if (_cfg.chase_jerk_scale) {
            const double vm = (double)_cfg.limits.vmax;
            // The MOVE ceiling is the stream's recent PEAK speed, never a
            // local average: an EMA dips at every crest and de-claws the
            // turn exactly where authority is needed (sd-d77.1 bench).
            double dem = std::fmax(std::fabs(v), std::fabs(vf));
            if (_est_ema_ok) dem = std::fmax(dem, _est_sp_pk);
            // Knee: full authority at kChaseJerkKneeFrac of vmax -- tracking
            // jerk follows content jerk (~omega^3), not the velocity fraction.
            double r = dem / (vm > 1e-9 ? kChaseJerkKneeFrac * vm : 1.0);
            const double fl = (double)_cfg.chase_jerk_floor;
            if (r < fl) r = fl;
            if (r > 1.0) r = 1.0;
            j_ovr = (double)_cfg.limits.jmax * r;
        }
        bool ok = planRuckig(p, v, a, aim, vf, af, 0.0, now_us, j_ovr);
        // A softened plan may be DECLINED (legality recheck); the mechanical
        // ceiling is always available as the hard fallback.
        if (!ok && j_ovr > 0.0)
            ok = planRuckig(p, v, a, aim, vf, af, 0.0, now_us);
        if (ok) _mode = Mode::Chase;
        return ok;
    }

    // ---- Shared Ruckig point-planner (chase, guard fallback) ----------------
    // min_dur 0 = time-optimal; > 0 = stretch toward the deadline.
    // j_ovr 0 = plan at the mechanical jerk ceiling; > 0 = plan at a SOFTER one
    // (0.6.0 sharpness search — jerkCeil enforces "softer only, never harder").
    bool planRuckig(double p, double v, double a, double target, double vf,
                    double af, double min_dur, uint64_t now_us,
                    double j_ovr = 0.0) {
        const double jc = jerkCeil(j_ovr);
        ruckig::InputParameter<1> in;
        in.current_position[0]     = p;
        in.current_velocity[0]     = v;
        in.current_acceleration[0] = a;
        in.target_position[0]      = target;
        in.target_velocity[0]      = vf;
        in.target_acceleration[0]  = af;
        in.max_velocity[0]         = _cfg.limits.vmax;
        in.max_acceleration[0]     = _cfg.limits.amax;
        in.max_jerk[0]             = jc;
        if (min_dur > 0.0) in.minimum_duration = min_dur;

        ruckig::Trajectory<1> traj;
        const ruckig::Result res = _calc.calculate(in, traj);
        if ((int)res < 0) {
#ifdef SLOPMOTION_SYNTH_DEBUG
            std::printf("PLANFAIL res=%d p=%.4f v=%.3f a=%.2f tgt=%.4f vf=%.3f af=%.2f mind=%.4f jc=%.1f\n",
                        (int)res, p, v, a, target, vf, af, min_dur, jc);
#endif
            _failures++;
            recordAnomaly(AnomalyType::PlanFailed, (float)target,
                          (float)(int)res, now_us);
            return false;
        }
        // ---- A SOFTENED PLAN MAY ONLY EVER BE GENTLER, NEVER ILLEGAL --------
        // The sharpness search proved its chosen ceiling legal against a
        // TIME-OPTIMAL probe, but the plan adopted here additionally pins
        // minimum_duration, and Ruckig's stretched profile families are not the
        // time-optimal ones — the same mismatch the caller's existing
        // belt-and-braces retry already guards against for outright refusals.
        // Illegality is that same class of surprise and gets the same answer:
        // refuse to adopt, and let the caller re-plan at the mechanical ceiling
        // it already knows is available. NOT counted as a plan failure —
        // nothing failed, this shape was simply declined — but never silent.
        //
        // Gated on a genuinely SOFTENED ceiling: at the mechanical ceiling
        // there is no harder plan to fall back to, so rejecting there would
        // trade a slightly-over profile for no profile at all.
        if (j_ovr > 0.0 && jc < (double)_cfg.limits.jmax) {
            const double worst = ruckigWorstRatio(traj, _oshoot_allow);
            if (worst > 1.0 + kRuckigLegalEps) {
                recordAnomaly(AnomalyType::WaveformFallback, (float)target,
                              (float)worst, now_us);
                return false;
            }
        }
        _pend_ok    = false;   // a point plan supersedes a scheduled successor
        _traj       = traj;
        _kind       = PlanKind::Ruckig;
        _plan_start = now_us;
        _plan_jerk_frac =
            _cfg.limits.jmax > 0.0f ? (float)(jc / (double)_cfg.limits.jmax)
                                    : 1.0f;
        return true;
    }

    // TIME-OPTIMAL duration from the machine's ACTUAL state to a boundary
    // condition, WITHOUT adopting anything. The bad-move bridge needs to know
    // how long the move would really take before it decides whether the
    // commanded duration is a deadline or a fiction, and planRuckig() commits
    // to _traj as a side effect — so the probe gets its own trajectory.
    //
    // Returns a negative value when Ruckig refuses; callers must treat that as
    // "no opinion" and fall through, never as "instant".
    double timeOptimalDuration(double p, double v, double a, double target,
                               double vf) {
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
        ruckig::Trajectory<1> traj;
        const ruckig::Result res = _calc.calculate(in, traj);
        if ((int)res < 0) return -1.0;
        return traj.get_duration();
    }

    // The jerk ceiling a plan/probe actually runs under. A positive override is
    // the 0.6.0 sharpness search asking for a SOFTER profile; it is clamped to
    // the mechanical ceiling here, in ONE place, so no search bug anywhere can
    // hand Ruckig a jerk the machine cannot survive. Non-positive = "use the
    // configured ceiling" (every pre-0.6.0 caller).
    double jerkCeil(double j_ovr) const {
        const double jc = (double)_cfg.limits.jmax;
        if (!(j_ovr > 0.0)) return jc;
        return j_ovr < jc ? j_ovr : jc;
    }

    // Wall guard + ceiling for a requested end velocity: the machine must be
    // able to brake to rest inside the window beyond the target. Trapezoid
    // bound vf² ≤ amax·dist — conservative margin for the jerk-limited tail
    // at sane jmax/amax ratios; the sampler clamp is the hard backstop.
    double applyEndVelGuard(double vf, double target, uint64_t now_us) {
        const double out = endVelBound(vf, target);
        if (std::fabs(out - vf) > 1e-6) {
            recordAnomaly(AnomalyType::EndVelClamped, (float)target,
                          (float)out, now_us);
        }
        return out;
    }

    // The bound itself, with no telemetry side effect — the speculative
    // callers (Reshape's bisection) need to ask the guard's question about
    // endpoints they may never adopt.
    double endVelBound(double vf, double target) const {
        const double vcap = _cfg.limits.vmax;
        if (vf >  vcap) vf =  vcap;
        if (vf < -vcap) vf = -vcap;
        const double dist = vf > 0.0 ? (1.0 - target) : target;
        const double vmax_wall =
            std::sqrt((double)_cfg.limits.amax * std::fmax(dist, 0.0));
        if (std::fabs(vf) > vmax_wall) {
            vf = vf > 0.0 ? vmax_wall : -vmax_wall;
        }
        return vf;
    }

    // ---- Stream estimator (velocity + cadence of the incoming points) -------
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
                    // Peak-hold speed with first-order release (chase jerk
                    // scale): instant attack, decays toward the local speed
                    // over kSpPeakReleaseS. Never an EMA (crest-dip trap).
                    const double sp = std::fabs(raw);
                    if (sp > _est_sp_pk) _est_sp_pk = sp;
                    else _est_sp_pk += (dt / kSpPeakReleaseS) *
                                       (sp - _est_sp_pk);
                    _est_v_ema  += 0.35 * (raw - _est_v_ema);
                    _est_dt_ema += 0.30 * (dt - _est_dt_ema);
                    // Stream curvature: differentiate the (already smoothed)
                    // velocity EMA, then smooth again — accel estimates are
                    // second differences of a jittery signal, treat gently.
                    const double a_raw = (_est_v_ema - v_prev) / dt;
                    _est_a_ema += 0.25 * (a_raw - _est_a_ema);
                } else {
                    _est_v_ema  = raw;
                    _est_sp_pk  = std::fabs(raw);
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

    // ---- Settle grace -------------------------------------------------------
    // How long an expired plan may hold its end state before we call the
    // stream starved. Sized from the stream's OWN cadence — a sender pacing
    // segments every 167 ms is not late until it is late BY that stream's
    // standards — and capped, because the grace exists to absorb transport
    // jitter, not to invent a hold. Zero (no cadence estimate yet, stale
    // stream, or knob disabled) restores the pre-0.4 brake-on-expiry.
    double settleGraceS(uint64_t now_us) const {
        if (_cfg.settle_grace_us == 0) return 0.0;
        if (!_est_ema_ok || !_est_valid) return 0.0;   // isolated point move
        // Staleness is judged from whichever is later: the last commit or
        // the END of the plan in flight. Measured from the last commit alone,
        // a segment longer than chase_stale_us starved its own grace: a
        // 587 ms segment expired, this read "stream gone", and the engine
        // slammed to rest 3 ms before its successor landed, every cycle
        // (field trace 2026-09-02, the periodic hitch).
        uint64_t ref = _est_last_us;
        if (_kind != PlanKind::None) {
            const uint64_t plan_end = _plan_start + (uint64_t)(planDuration() * 1e6 + 0.5);
            if (plan_end > ref) ref = plan_end;
        }
        if (now_us > ref && (now_us - ref) > _cfg.chase_stale_us) {
            return 0.0;                                // the stream really is gone
        }
        const double cap = (double)_cfg.settle_grace_us * 1e-6;
        double g = std::fmin(kSettleGraceMult * _est_dt_ema, cap);
        // A locked synthesis chain PRODUCES at knot pitch, not sample pitch:
        // starvation is judged against the chain's own cadence, or uneven
        // knots brake the machine mid-stream (field report 2026-08-09). A
        // dead stream still zeroes the grace via the staleness check above.
        if (_syn_chain_ok) {
            g = std::fmax(g, 1.5 * (double)kSynthSpanUs * 1e-6);
        }
        return g;
    }

    // ---- Starve-settle ------------------------------------------------------
    // The clock ran past a plan that ends moving and no fresh command
    // replanned it → plan a jerk-limited brake-to-rest from the end state
    // (velocity control interface; lands wherever braking lands, clamped by
    // the sampler at the walls). One-time boundary event.
    //
    // ...but ONLY after the grace window. A plan expiring a few milliseconds
    // before its successor arrives is not a starved stream, it is a network.
    // Measured on the firmware's SlopSync path, whose 5 ms segment-pacing
    // drain guarantees exactly that jitter: a 14-segment funscript chain
    // produced 14 settles and 27 PlanKind flips at 5 ms of arrival lag,
    // against 1 and 1 at 0 ms — the engine was reacting to the transport, not
    // to the sender. Each of those settles is a Ruckig brake plan that the
    // next segment preempts ~5 ms later, so it costs plan time, corrupts the
    // mode/plan telemetry, and (worst) bleeds the velocity the next segment
    // was counting on inheriting. The grace window holds at the endpoint for
    // ms-scale stream jitter (degrading smoothly) and keeps the brake for a
    // real starvation.
    // The §11.3 600 ms SlopSync deadman remains the actual starvation
    // authority; this window only stops the engine from panicking on ms-scale
    // pacing noise.
    void maybeSettle(uint64_t now_us) {
        if (_pend_ok) {
            // A scheduled successor exists: promote it at its anchor, and
            // never settle ahead of it -- the stream is alive by definition.
            if (now_us >= _pend_start) {
                _pend_ok = false;
                adoptQuintic(_pend_c, _pend_T, _pend_start);
            }
            return;
        }
        if (_kind == PlanKind::None || _mode == Mode::Settle) {
            settleToIdle(now_us);
            return;
        }
        const double dur = planDuration();
        if (elapsedS(now_us) < dur) return;

        double p, v, a;
        planEndState(p, v, a);
        if (std::fabs(v) <= kRestVel) {
            // Ended at rest — collapse to a plain hold. (No grace needed: a
            // hold IS the end state, and a fresh command replans from it
            // identically whether we collapsed or not.)
            _hold_pos = clamp01(p);
            _kind     = PlanKind::None;
            _mode     = Mode::Idle;
            return;
        }

        // Grace: the state COASTS at the end velocity (sampleRaw); measured
        // from PLAN EXPIRY, never re-arms.
        const double grace = settleGraceS(now_us);
        if (elapsedS(now_us) - dur < grace) return;

        // Brake from the coasted state at the anchor instant (plan end +
        // grace); grace = 0 keeps the pre-0.9 start state exactly.
        const double coast = grace > kCoastCapS ? kCoastCapS : grace;
        ruckig::InputParameter<1> in;
        in.control_interface       = ruckig::ControlInterface::Velocity;
        in.current_position[0]     = p + v * coast;
        in.current_velocity[0]     = v;
        in.current_acceleration[0] = coast > 0.0 ? 0.0 : a;
        in.target_velocity[0]      = 0.0;
        in.target_acceleration[0]  = 0.0;
        in.max_velocity[0]         = _cfg.limits.vmax;
        in.max_acceleration[0]     = _cfg.limits.amax;
        in.max_jerk[0]             = _cfg.limits.jmax;

        ruckig::Trajectory<1> traj;
        const ruckig::Result res = _calc.calculate(in, traj);
        // Anchor the settle at the moment the COAST ended (plan end + grace),
        // not at this sample's clock, so the brake follows the coasted state
        // seamlessly. Anchoring at plan end alone would be a bug once a grace
        // exists: the brake profile would be entered `grace` seconds deep, and
        // the very first sample would JUMP up to vmax·grace (30 mm on the
        // operator's 200 mm window at a 30 ms grace). With grace = 0 this is
        // byte-identical to the pre-0.4 anchor.
        const uint64_t end_us = _plan_start + (uint64_t)(dur * 1e6 + 0.5)
                                            + (uint64_t)(grace * 1e6 + 0.5);
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
        _plan_jerk_frac = 1.0f;   // a brake is planned at the full ceiling
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

    // ---- State --------------------------------------------------------------
    Config                _cfg;
    ruckig::Ruckig<1>     _calc;        // offline calculate() only — no cycle time
    ruckig::Trajectory<1> _traj;        // active Ruckig plan (chase/guard/settle)
    double                _q_c[6] = {}; // active quintic (normalized tau)
    double                _q_T = 0.0;   // quintic duration, seconds
    PlanKind              _kind = PlanKind::None;
    uint64_t              _plan_start = 0;
    double                _hold_pos = 0.5;
    Mode                  _mode = Mode::Idle;
    // Peak jerk of the ACTIVE plan as a fraction of limits.jmax (see
    // Snapshot::sharpness). Telemetry only — nothing in the sample path reads
    // it; the plan is already an immutable polynomial.
    float                 _plan_jerk_frac = 1.0f;
    // RFC-030: the curve family the ACTIVE waveform command declared (registry
    // curve_families numbering; 0 = undeclared). Adopted at commitWaveform so
    // every re-solve of the same plan resolves FollowClient identically.
    uint8_t               _client_curve_family = 0;
    // Overshoot allowance for the commit IN PROGRESS, in window fractions.
    // < 0 = not armed, which is also the resting value. INVARIANT: every commit
    // path writes it before any referee runs (commitWaveform disarms at entry
    // and arms after the bridge, commitChase disarms), and the referees take it
    // as a PARAMETER. Synthesis spans arm their own local and never touch it.
    double                _oshoot_allow = -1.0;

    // Stream estimator
    bool     _est_valid = false;
    bool     _est_ema_ok = false;
    double   _est_v_ema = 0.0;
    double   _est_sp_pk = 0.0;    // peak-hold |chord speed|, released
    double   _est_a_ema = 0.0;
    double   _est_dt_ema = 0.0;
    double   _est_last_target = 0.5;
    uint64_t _est_last_us = 0;

    // DC-centering memory (see the control-law note on commitWaveformReshaped):
    // direction of the last waveform stroke, the amplitude owed back at the
    // next reversal, and when that stroke was planned (a debt older than
    // kWaveDebtStaleUs belongs to a phrase that has ended).
    int8_t   _wave_dir     = 0;
    double   _wave_owed    = 0.0;
    uint64_t _wave_last_us = 0;

    // Previous wire G (for the backward-difference af estimate)
    bool     _prev_vf_ok = false;
    double   _prev_vf = 0.0;
    uint64_t _prev_vf_us = 0;
    // Last commit arrival (cold-start gap test); 0 = never.
    uint64_t _last_commit_us = 0;
    // Sample-synthesis holdback (see Config::sample_synthesis).
    bool     _syn_ok = false;       // a buffered point exists
    double   _syn_p = 0.0;
    uint64_t _syn_us = 0;
    bool     _syn_prev_ok = false;  // a knot BEHIND the buffer exists
    double   _syn_prev_p = 0.0;
    uint64_t _syn_prev_us = 0;
    bool     _syn_vf_ok = false;    // previous knot tangent (af estimate)
    double   _syn_vf = 0.0;
    bool     _syn_chain_ok = false; // chain-time anchor established
    uint64_t _syn_chain_us = 0;
    // Recent raw samples: the priming/fallback chase target rides ~2 knots
    // behind the head (see commitSampleSynth).
    static constexpr int kSynRingN = 8;
    uint64_t _synr_us[kSynRingN] = {};
    float    _synr_p[kSynRingN] = {};
    uint8_t  _synr_n = 0;
    uint8_t  _synr_w = 0;
    // Pending synthesized span: committed AHEAD of its chain anchor (uneven
    // knot cadence) and promoted at the anchor by maybeSettle. Without this
    // slot an early adoption clamps to the span START (elapsedS) and every
    // uneven knot teleports (field report 2026-08-09).
    bool     _pend_ok = false;
    double   _pend_c[6] = {};
    double   _pend_T = 0.0;
    uint64_t _pend_start = 0;
    // Previous waveform TARGET (dwell rule): a hold is the same target
    // re-commanded, never just "happens to be near" -- a centering-clipped
    // chain lands near its NEXT target legitimately (the RFC-049c regime).
    bool     _prev_wave_tgt_ok = false;
    double   _prev_wave_tgt = 0.0;

    // Counters + anomaly ring
    uint32_t _plans = 0;
    uint32_t _failures = 0;
    Anomaly  _anom_ring[kAnomalyDepth];
    uint8_t  _anom_write = 0;
    uint8_t  _anom_count = 0;
    uint16_t _anom_seq = 0;
};

} // namespace slopmotion
