#pragma once

// ============================================================================
// MotionInterpolator — on-device cubic motion generator for TCode v0.4 (A2)
// ============================================================================
//
// PURPOSE
// -------
// This is the SlopDrive-32 port of TempestMAx's TCode `Axis` motion engine
// (OSR2/SR6 reference firmware, MIT — Copyright (c) 2021 Richard Unger). The
// original evaluates a cubic Hermite curve in Q16.16 fixed-point against
// millis() every free-running loop. We reimplement the SAME algorithm in
// single-precision float (ESP32-S3 has a hardware FPU; .clinerules §4 mandates
// float over double) and in NORMALIZED 0..1 position units so it drops straight
// onto the TCode parser output and the RangeMapper input with no 0..10000
// scaling layer.
//
// WHY (the two bugs this fixes)
// -----------------------------
//   * v4 microstutter: previously each sparse v4 point triggered a fresh FAS
//     trapezoid re-plan with a brand-new cruise speed → velocity discontinuity
//     at every segment boundary. Here we build a C1-continuous cubic between
//     points (start tangent = current live velocity, end tangent = MFP's
//     G<slope>) and feed FAS a VELOCITY-MATCHED cruise so it coasts the segment
//     instead of ramping. No boundary discontinuity → no microstutter.
//   * v3 slow-speed dropout: bare high-rate points enter "live mode" — the
//     engine extrapolates 1.25x the rolling-mean interval so it keeps gliding
//     through late/jittery packets instead of collapsing onto the 1 step/s
//     floor and tripping the driver's grit-cache / stall interaction.
//
// UNITS (read this before touching the math)
// ------------------------------------------
//   position   : normalized 0.0 .. 1.0   (0 = home end, 1 = far end of window)
//   time       : microseconds (uint64 from esp_timer_get_time())
//   duration   : microseconds
//   SLOPE/grad : normalized units per 100 ms   <-- MFP convention.
//                MFP works in (0..1)/second and multiplies by 1000 to emit
//                TCode "G" slope, i.e. G = velocity_per_s * 1000 = per-100ms*... 
//                    velocity_per_s   = G / 1000
//                    velocity_per_100ms = G / 10000
//                We store the tangent as dp/dtau (see setCubic) so the eval
//                path is a plain polynomial.
//   velocity() : returned to callers in normalized units per SECOND (handy for
//                converting to mm/s → steps/s for the FAS cruise feed).
//
// THREADING
// ---------
// This object is single-threaded: ALL of prep*/commit/position/velocity run on
// Core 1 (the motion task). Cross-core handoff of new segments happens OUTSIDE
// this class (a FreeRTOS queue in the composition root), so there are no locks
// in the hot eval path. See .clinerules §2 (dual-core separation).
//
// MIT attribution: algorithm derived from jcfain/TCodeESP32 and TempestMAx's
// Axis library. See THIRD_PARTY_LICENSES.md.

#include <stdint.h>

// ---- Tunables (ported from reference Axis.h, retimed to microseconds) -------
namespace interp_cfg {
    // Below this inter-point interval a v3 point is treated as an instantaneous
    // "short" hold rather than a timed segment (reference SHORT_MOVE_INTERVAL).
    constexpr uint32_t SHORT_MOVE_INTERVAL_US = 50000;   // 50 ms
    // If bare points arrive faster than this, engage live extrapolation mode.
    constexpr uint32_t LIVE_TRIGGER_US        = 25000;   // 25 ms  (>= ~40 Hz)
    // Rolling-mean window length for the live interval estimator.
    constexpr uint32_t MEAN_INTERVAL_STEPS    = 5;
    // Convergence steps used to ease back in after a signal gap.
    constexpr uint16_t LIVE_CONVERGE_STEPS    = 10;
    // Default hold interval for an isolated (non-live) short command.
    constexpr uint32_t DEFAULT_SHORT_US       = 50000;   // 50 ms
    // Live extrapolation factor: each live segment is extended to 1.25x the
    // mean interval so the timeout (decel) path fires < 50% of the time.
    // Expressed as numerator/denominator to avoid a magic float.
    constexpr float    LIVE_EXTRAP_FACTOR     = 1.25f;
    // Constant deceleration used by the graceful stop when a live/gradient
    // segment overruns with no fresh packet. Normalized units per second^2.
    // A full 0..1 sweep brakes from cruise in ~1/DECEL_PER_S2 seconds; tune to
    // taste. Kept modest so a dropped stream eases out rather than slamming.
    constexpr float    DECEL_PER_S2           = 8.0f;
    // Overshoot detection: a committed cubic whose peak excursion leaves the
    // [start,end] envelope by more than this fraction records an OVERSHOOT
    // anomaly. 0.02 = 2% of full stroke (matches the capture-side analysis eps).
    constexpr float    OVERSHOOT_EPS          = 0.02f;
    // Depth of the Core-1-local anomaly ring (events drained by the sampler).
    constexpr uint8_t  ANOMALY_RING_DEPTH     = 32;
}

// Movement shaping for a committed segment.
enum class InterpStyle : uint8_t {
    Ramped,     // linear / constant velocity between points (v3 live default)
    EaseIn,     // quadratic start-from-rest
    EaseOut,    // quadratic soft landing
    EaseBoth,   // rest-to-rest smoothstep
    Gradient    // full Hermite using supplied end slope (v4 G<slope> path)
};

// A compact, POD segment request — safe to shove through a FreeRTOS queue for
// the Core 0 → Core 1 handoff. `hasSlope` selects the v4 Gradient path.
struct InterpSegment {
    float    targetPos    = 0.5f;   // normalized 0..1
    uint32_t durationUs   = 0;      // 0 = derive from live cadence / default
    float    endSlope     = 0.0f;   // MFP G<slope> in units-per-100ms
    bool     hasSlope     = false;  // true → v4 gradient segment
    bool     hasDuration  = false;  // true → explicit I<ms> interval present
    bool     isLivePoint  = false;  // true → bare v3 high-rate point
};

// Read-only snapshot of the interpolator's current plan, for WebUI telemetry.
struct InterpDebug {
    float    startPos   = 0.5f;
    float    endPos     = 0.5f;
    float    curPos     = 0.5f;
    float    curVel     = 0.0f;   // units/second
    uint32_t durationUs = 0;
    uint32_t elapsedUs  = 0;
    bool     liveMode   = false;
    bool     gradMode   = false;
    uint8_t  style      = 0;      // InterpStyle
};

// ---- Anomaly instrumentation -----------------------------------------------
// The interpolator records, into a small Core-1-local ring, any event where the
// generated motion is suspect. Each event captures the INPUT that caused it so
// the WebUI (and the offline capture harness) can pin the funscript point that
// triggered a stutter / non-move without guessing. Drained by the sampler task
// and forwarded to a cross-core ring for the 0x05 ANOMALY WS frame.
enum class InterpAnomalyType : uint8_t {
    None         = 0,
    Overshoot    = 1,  // cubic left the [start,end] envelope (steep G tangent)
    PointDropped = 2,  // liveShort() discarded a bare point after a gap
    DecelOverrun = 3,  // segment starved → graceful decel-to-stop fired
    DurFallback  = 4   // gradient path had no I<ms>; fell back to mean interval
};

struct InterpAnomaly {
    uint8_t  kind       = 0;     // InterpAnomalyType
    uint16_t seq        = 0;     // rolling event id (wraps harmlessly)
    uint32_t tDevUs     = 0;     // esp_timer us at record time
    float    targetPos  = 0.0f;  // segment target 0..1
    float    startPos   = 0.0f;  // curve start pos 0..1 (sp)
    float    startVel   = 0.0f;  // curve start vel, units/s (sv)
    float    endSlope   = 0.0f;  // raw MFP G<slope> wire value
    uint32_t durationUs = 0;     // planned segment duration
    float    extra      = 0.0f;  // kind-specific: overshoot frac / interval_us / vEnd
};

class MotionInterpolator {
public:
    explicit MotionInterpolator(float startPos = 0.5f);

    // ---- Lifecycle (Core 1) -------------------------------------------------
    // Hard-reset the curve to a static hold at `pos`. Used on home/estop/resume.
    void reset(float pos);

    // Commit a new segment NOW. `nowUs` is the current esp_timer time. This is
    // the single entry point the Core-1 task calls after dequeuing an
    // InterpSegment. It computes the start position/velocity from the CURRENT
    // curve (C1 continuity) and builds the new polynomial.
    void commit(const InterpSegment& seg, uint64_t nowUs);

    // ---- Evaluation (Core 1, hot path) --------------------------------------
    // Position on the curve at `nowUs`, clamped 0..1. Auto-handles running past
    // the segment end (holds endPos, or in live/grad mode decels to a stop).
    float positionAt(uint64_t nowUs);

    // Instantaneous velocity at `nowUs`, normalized units per SECOND (signed).
    float velocityAt(uint64_t nowUs);

    // Is the curve effectively stopped (zero-duration hold)?
    bool  isHolding() const { return _durationUs == 0; }

    // Is a committed segment still gliding at `nowUs` (clock not yet past its
    // duration)? Unlike isHolding() this is TIME-AWARE: it answers "does the
    // curve still have planned motion left to render right now?" The stream
    // sampler gates on this so it keeps feeding FAS through the WHOLE segment
    // even when the next sparse v4 packet is farther away than the idle timeout
    // (v4 lands points ~700-1000 ms apart — longer than the 500 ms timeout —
    // which previously froze motion ~499 ms into a 933 ms stroke). The SEGMENT,
    // not the packet cadence, decides when a move is finished. A zero-duration
    // hold is never busy, so once the last move completes this goes false and
    // the sampler can release the motor after the idle timeout.
    bool  isBusy(uint64_t nowUs) const {
        return _durationUs != 0 &&
               (nowUs <= _startUs || (nowUs - _startUs) < (uint64_t)_durationUs);
    }

    // Time the last segment was committed (esp_timer us).
    uint64_t lastCommitUs() const { return _startUs; }

    // ---- Runtime tuning (Core 0 may set; read on Core 1 at commit) ----------
    // When true, the v4 gradient cubic's Hermite tangents are monotone-limited
    // (Fritsch–Carlson) before setCubic so the curve can never bulge outside
    // [startPos, endPos] — kills the invented overshoot micromotion at the cost
    // of slightly softening MFP's requested end-slope shaping. Toggled live from
    // the WebUI. A plain bool is fine: single-writer (sampler pushes g_state's
    // value each tick), read only inside commit() on the same core.
    void setClampOvershoot(bool on) { _clampOvershoot = on; }
    bool clampOvershoot() const { return _clampOvershoot; }

    // ---- Telemetry ----------------------------------------------------------
    InterpDebug snapshot(uint64_t nowUs);

    // ---- Anomaly drain (Core 1) ---------------------------------------------
    // Pop the oldest un-read anomaly event. Returns false when the ring is
    // empty. The sampler task drains this each tick and forwards events to the
    // cross-core ring for the WebUI. Single-threaded (Core 1) — no lock.
    bool popAnomaly(InterpAnomaly& out);

private:
    // Build a cubic Hermite from (p0,p1) and tangents (dp/dtau at each end).
    void setCubic(uint32_t durationUs, float p0, float p1,
                  float startTangent, float endTangent);
    // Build a linear or eased segment (delegates to setCubic for the eased ones).
    void setMotion(uint32_t durationUs, float p0, float p1,
                   bool easeIn, bool easeOut);
    // Parabolic decel-to-stop when a live/gradient segment overruns.
    void setDecelStop(float curPos, float curVel);
    // Called when the clock runs past the current segment's duration.
    void handleTimeout(uint64_t nowUs);
    // Live-mode short-command handler: returns duration to use, and whether the
    // point should actually be applied (false = hold pending extrapolation).
    bool liveShort(const InterpSegment& seg, uint32_t& outDurationUs,
                   uint64_t nowUs);

    // Polynomial evaluation: pos(tau) = A*tau^3 + B*tau^2 + C*tau + D, tau in 0..1
    float posFromCurve(uint32_t elapsedUs) const;
    float velFromCurve(uint32_t elapsedUs) const;  // units/second

    // ---- Anomaly recording (Core 1) -----------------------------------------
    // Push an anomaly into the local ring (overwrites oldest if full).
    void recordAnomaly(InterpAnomalyType kind, float targetPos, float startPos,
                       float startVel, float endSlope, uint32_t durationUs,
                       float extra, uint64_t nowUs);
    // Peak excursion of the CURRENT cubic beyond its [start,end] envelope
    // (0 if it stays inside). Analytic — evaluates the cubic at its interior
    // extrema. Used right after setCubic() to flag Hermite overshoot.
    float envelopeOvershoot() const;
    // Monotone (Fritsch–Carlson) tangent limiter. Given the segment endpoints
    // and the two dp/dtau tangents, clamps the tangents IN PLACE so the cubic
    // Hermite stays monotone on [0,1] — guaranteeing it never leaves the
    // [p0,p1] envelope. Handles all three overshoot flavors: opposing end slope
    // (tangent reversed vs secant → clamp to 0), zero-displacement continuity
    // bulge (secant ≈ 0 → both tangents to 0), and high entry velocity
    // (alpha²+beta² > 9 → scale down). No-op when the secant is nonzero and the
    // tangents already satisfy the monotonicity region.
    static void monotoneClampTangents(float p0, float p1, float& m0, float& m1);

    // ---- Current segment ----------------------------------------------------
    uint64_t _startUs   = 0;
    uint32_t _durationUs = 0;     // 0 = stopped/hold
    float    _startPos  = 0.5f;
    float    _endPos    = 0.5f;
    bool     _gradMode  = false;
    bool     _clampOvershoot = false;   // WebUI toggle: monotone-limit v4 tangents

    // Cubic coefficients (normalized position units).
    // NOTE: single-letter names like _B/_C collide with newlib ctype.h macros
    // (blank/control bitmasks) once Arduino.h is in the TU — must stay prefixed.
    float _coefA = 0.0f, _coefB = 0.0f, _coefC = 0.0f, _coefD = 0.5f;

    // ---- Live-mode state ----------------------------------------------------
    bool     _liveMode        = false;
    uint64_t _lastShortUs     = 0;
    float    _lastShortPos    = 0.5f;
    float    _meanIntervalUs  = (float)interp_cfg::DEFAULT_SHORT_US;
    uint16_t _convergeSteps   = 1;
    InterpStyle _lastStyle    = InterpStyle::Ramped;

    // ---- Anomaly ring (Core 1 only — single-threaded, no lock) --------------
    InterpAnomaly _anomRing[interp_cfg::ANOMALY_RING_DEPTH];
    uint8_t  _anomWrite = 0;   // next write slot
    uint8_t  _anomCount = 0;   // unread events in ring
    uint16_t _anomSeq   = 0;   // monotonic event id
};
