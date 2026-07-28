// ============================================================================
// MotionInterpolator — implementation (float / normalized 0..1 port)
// ============================================================================
//
// Algorithm derived from TempestMAx's TCode `Axis` (jcfain/TCodeESP32, MIT —
// Copyright (c) 2021 Richard Unger). Reimplemented in single-precision float
// working in normalized 0..1 position units and microsecond time. See the
// header for unit conventions and the MFP slope reconciliation.
//
// SLOPE UNIT RECONCILIATION (the one thing to get right):
//   MFP internal velocity is (0..1)/second, emitted as G = vel_per_s * 1000.
//   Therefore:  vel_per_s = G / 1000.
//   A cubic Hermite tangent is dp/dtau (tau in 0..1 over the segment), and
//   dp/dtau = vel_per_s * duration_seconds. So:
//       endTangent   = (endSlope / 1000) * durationSec
//       startTangent = currentLiveVelocity_per_s * durationSec   (C1 continuity)
//   The start tangent equals the PREVIOUS segment's end slope, which is exactly
//   what MFP's pchip/makima produced for that shared point — so segments join
//   with matching velocity and there is no boundary kink.
//
// NOTE on coefficient names: the cubic coefficients are _coefA.._coefD, NOT
// _A/_B/_C/_D. Single-letter members collide with newlib's ctype.h macros
// (_B = blank, _C = control bitmasks) the moment Arduino.h is in the same TU,
// which preprocesses `_B = 0.0f` into `0200 = 0.0f` and fails to compile.

#include "MotionInterpolator.h"
#include <math.h>
#include <string.h>   // memset via struct copy; kept explicit for clarity

using namespace interp_cfg;

// ---- Construction / reset ---------------------------------------------------

MotionInterpolator::MotionInterpolator(float startPos) {
    reset(startPos);
}

void MotionInterpolator::reset(float pos) {
    if (pos < 0.0f) pos = 0.0f;
    if (pos > 1.0f) pos = 1.0f;
    _startUs        = 0;
    _durationUs     = 0;          // stopped/hold
    _startPos       = pos;
    _endPos         = pos;
    _gradMode       = false;
    _coefA = _coefB = _coefC = 0.0f;
    _coefD          = pos;
    _liveMode       = false;
    _lastShortUs    = 0;
    _lastShortPos   = pos;
    _meanIntervalUs = (float)DEFAULT_SHORT_US;
    _convergeSteps  = 1;
    _lastStyle      = InterpStyle::Ramped;
}

// ---- Internal: advance past any overrun (handles segment timeout) -----------

void MotionInterpolator::handleTimeout(uint64_t nowUs) {
    // New segment begins exactly where the old one ended in time.
    _startUs += _durationUs;

    if (_gradMode || _liveMode) {
        // A gradient or live segment overran with no fresh packet. The OLD
        // behavior called setDecelStop(), which invented a parabolic brake that
        // CONTINUED past the last commanded endpoint (target = curPos + dir*dist,
        // curPos == _endPos at a timeout). At a seam — a reversal or a same-
        // direction continuation where the next MFP packet lands a hair after the
        // scheduled duration — that extension is the invented overshoot the
        // operator sees go PAST MFP's own path on both the interp AND commanded
        // lines, in normalized 0..1 units (so the FAS speed/accel ceilings do
        // nothing to it). v3's 333Hz keeps segments alive so it almost never
        // fires; v4's sparse points with a nonzero end-slope hit it constantly.
        //
        // Reference behavior (TempestMAx Axis) HOLDS at the last endpoint on
        // overrun and never commands beyond it. We do the same: freeze exactly at
        // _endPos and let FAS absorb the carried velocity with its own ramp. Zero
        // commanded overshoot by construction, and harmless to the working v3
        // path. The DecelOverrun anomaly still fires so the seam-timeout stays
        // visible in the 0x05 feed — we just no longer travel past the point. :3
        float vEnd = velFromCurve(_durationUs);   // units/second at old end (telemetry only)
        recordAnomaly(InterpAnomalyType::DecelOverrun, _endPos, _startPos,
                      vEnd, 0.0f, _durationUs, vEnd, nowUs);
        setMotion(0, _endPos, _endPos, false, false);   // hold AT the point — never past it
        _liveMode = false;
    } else {
        // Ordinary timed segment finished — hold at the endpoint.
        setMotion(0, _endPos, _endPos, false, false);
    }
}

// ---- Evaluation -------------------------------------------------------------

float MotionInterpolator::positionAt(uint64_t nowUs) {
    // Catch up through at most a few overruns (large gaps converge to a hold).
    for (int i = 0; i < 3; ++i) {
        if (_durationUs != 0 && nowUs > _startUs &&
            (nowUs - _startUs) > (uint64_t)_durationUs) {
            handleTimeout(nowUs);
        } else {
            break;
        }
    }

    uint32_t elapsed;
    if (nowUs <= _startUs) {
        elapsed = 0;
    } else {
        uint64_t e = nowUs - _startUs;
        elapsed = (e > (uint64_t)_durationUs) ? _durationUs : (uint32_t)e;
    }
    return posFromCurve(elapsed);
}

float MotionInterpolator::velocityAt(uint64_t nowUs) {
    // Assumes positionAt() has already advanced timeouts for this nowUs; if
    // called standalone it is still safe (evaluates clamped elapsed).
    uint32_t elapsed;
    if (nowUs <= _startUs) {
        elapsed = 0;
    } else {
        uint64_t e = nowUs - _startUs;
        elapsed = (e > (uint64_t)_durationUs) ? _durationUs : (uint32_t)e;
    }
    return velFromCurve(elapsed);
}

// ---- Commit a new segment (single Core-1 entry point) -----------------------

void MotionInterpolator::commit(const InterpSegment& seg, uint64_t nowUs) {
    // Start state comes from the CURRENT curve — this is what gives C1
    // continuity. positionAt() also settles any pending timeout first.
    float sp = positionAt(nowUs);
    float sv = velocityAt(nowUs);        // units/second

    float target = seg.targetPos;
    if (target < 0.0f) target = 0.0f;
    if (target > 1.0f) target = 1.0f;

    if (seg.hasSlope) {
        // ---- v4 gradient path (MFP G<slope>) --------------------------------
        _liveMode = false;
        // ANOMALY: MFP sent a slope but no usable I<ms> — we have to invent the
        // segment duration from the rolling mean interval, which is exactly the
        // thing that stretches an 82ms funscript point into a ~750ms glide.
        bool durFallback = (!seg.hasDuration || seg.durationUs == 0);
        uint32_t dur = seg.durationUs;
        if (durFallback) dur = (uint32_t)_meanIntervalUs;
        if (dur < 1000) dur = 1000;      // floor 1 ms
        float durSec = (float)dur * 1e-6f;
        float m0 = sv * durSec;                          // continuity tangent
        float m1 = (seg.endSlope / 1000.0f) * durSec;    // MFP end slope tangent
        // Optional monotone tangent limiting (WebUI toggle). When on, this
        // clamps m0/m1 so the Hermite cubic can never leave [sp,target] — the
        // envelopeOvershoot() check below then reads ~0, which doubles as live
        // proof in the anomaly log that the clamp actually caught the bulge.
        if (_clampOvershoot) monotoneClampTangents(sp, target, m0, m1);
        setCubic(dur, sp, target, m0, m1);
        _startUs   = nowUs;
        _lastStyle = InterpStyle::Gradient;

        if (durFallback) {
            recordAnomaly(InterpAnomalyType::DurFallback, target, sp, sv,
                          seg.endSlope, dur, _meanIntervalUs, nowUs);
        }
        // ANOMALY: a steep G tangent makes the Hermite cubic bulge past the
        // [start,end] envelope — the invented overshoot-then-return micromotion.
        float ov = envelopeOvershoot();
        if (ov > interp_cfg::OVERSHOOT_EPS) {
            recordAnomaly(InterpAnomalyType::Overshoot, target, sp, sv,
                          seg.endSlope, dur, ov, nowUs);
        }

    } else if (seg.hasDuration) {
        // ---- Timed point, no slope (v3 with I, or v4 without G) -------------
        // Linear ramp across the interval. Not C1 by itself, but at stream
        // rates the segments are short and visually smooth; the slope path
        // above is the smooth one.
        _liveMode = false;
        uint32_t dur = seg.durationUs;
        if (dur < 1000) dur = 1000;
        setMotion(dur, sp, target, false, false);
        _startUs   = nowUs;
        _lastStyle = InterpStyle::Ramped;

    } else {
        // ---- Bare point → live/short handling (v3 high-rate) ---------------
        // Snapshot the last-short time BEFORE liveShort() overwrites it, so an
        // anomaly can report the true inter-arrival gap that caused the drop.
        uint64_t prevShortUs = _lastShortUs;
        uint32_t dur = 0;
        bool usePoint = liveShort(seg, dur, nowUs);
        if (!usePoint) {
            // Isolated point after a gap while still in live mode — hold and
            // wait for the next point to re-establish cadence (reference
            // behavior: do NOT jerk to a single stale point).
            // ANOMALY: this is the v3 slow-speed \"fails to move\" — a real
            // funscript point was DROPPED. Capture the gap that tripped it.
            float lastInterval = (prevShortUs != 0)
                                   ? (float)(nowUs - prevShortUs) : 0.0f;
            recordAnomaly(InterpAnomalyType::PointDropped, target, sp, sv,
                          0.0f, 0, lastInterval, nowUs);
            return;
        }
        if (dur < 1000) dur = 1000;
        setMotion(dur, sp, target, false, false);
        _startUs   = nowUs;
        _lastStyle = InterpStyle::Ramped;
    }
}

// ---- Live-mode short-command handler ---------------------------------------

bool MotionInterpolator::liveShort(const InterpSegment& seg,
                                   uint32_t& outDurationUs,
                                   uint64_t nowUs) {
    bool usePoint = true;

    uint64_t lastInterval = (_lastShortUs != 0) ? (nowUs - _lastShortUs)
                                                : (uint64_t)UINT32_MAX;

    if (lastInterval < (uint64_t)LIVE_TRIGGER_US) {
        // Fast stream → live mode. Roll the mean interval and extrapolate the
        // segment to 1.25x it so the timeout/decel path fires < 50% of the time
        // and the curve keeps gliding through a late packet.
        _liveMode = true;
        _meanIntervalUs = (_meanIntervalUs * (float)(MEAN_INTERVAL_STEPS - 1)
                           + (float)lastInterval) / (float)MEAN_INTERVAL_STEPS;
        outDurationUs = (uint32_t)(_meanIntervalUs * LIVE_EXTRAP_FACTOR);
        if (_convergeSteps > 1) _convergeSteps--;   // ease back in after a gap
    } else if (_liveMode) {
        // We WERE live but a lone point arrived after a gap — arm convergence
        // and skip this point (wait for cadence to re-establish).
        _convergeSteps = LIVE_CONVERGE_STEPS;
        usePoint = false;
    } else {
        // Genuinely isolated command — a simple short move.
        outDurationUs = DEFAULT_SHORT_US;
    }

    _lastShortUs  = nowUs;
    _lastShortPos = seg.targetPos;
    return usePoint;
}

// ---- Curve builders ---------------------------------------------------------

void MotionInterpolator::setCubic(uint32_t durationUs, float p0, float p1,
                                  float startTangent, float endTangent) {
    _durationUs = durationUs;
    _startPos   = p0;
    _endPos     = p1;
    _gradMode   = true;

    if (durationUs == 0) {
        _endPos = p0;
        _coefA = _coefB = _coefC = 0.0f;
        _coefD = p0;
        return;
    }

    const float m0 = startTangent;   // dp/dtau at tau=0
    const float m1 = endTangent;     // dp/dtau at tau=1

    // Standard cubic Hermite basis in tau ∈ [0,1]:
    //   pos(tau) = A*tau^3 + B*tau^2 + C*tau + D
    _coefA = 2.0f * p0 - 2.0f * p1 + m0 + m1;
    _coefB = -3.0f * p0 + 3.0f * p1 - 2.0f * m0 - m1;
    _coefC = m0;
    _coefD = p0;
}

void MotionInterpolator::setMotion(uint32_t durationUs, float p0, float p1,
                                   bool easeIn, bool easeOut) {
    _durationUs = durationUs;
    _startPos   = p0;
    _endPos     = p1;
    _gradMode   = false;

    if (durationUs == 0) {
        _endPos = p0;
        _coefA = _coefB = _coefC = 0.0f;
        _coefD = p0;
        return;
    }

    if (!easeIn && !easeOut) {
        // Linear / constant velocity.
        _coefA = 0.0f;
        _coefB = 0.0f;
        _coefC = p1 - p0;
        _coefD = p0;
        return;
    }

    // Eased: express as a Hermite with rest tangents.
    float m0, m1;
    const float delta = p1 - p0;
    if (easeIn && !easeOut) {          // quadratic ease-in (start at rest)
        m0 = 0.0f;
        m1 = 2.0f * delta;
    } else if (!easeIn && easeOut) {   // quadratic ease-out (land at rest)
        m0 = 2.0f * delta;
        m1 = 0.0f;
    } else {                           // rest-to-rest smoothstep
        m0 = 0.0f;
        m1 = 0.0f;
    }

    _coefA = 2.0f * p0 - 2.0f * p1 + m0 + m1;
    _coefB = -3.0f * p0 + 3.0f * p1 - 2.0f * m0 - m1;
    _coefC = m0;
    _coefD = p0;
}

void MotionInterpolator::setDecelStop(float curPos, float curVel) {
    // Graceful parabolic braking when a live/gradient segment overruns.
    if (fabsf(curVel) < 1e-4f) {
        setMotion(0, curPos, curPos, false, false);   // already stopped → hold
        return;
    }

    float dir = (curVel > 0.0f) ? 1.0f : -1.0f;
    float v   = fabsf(curVel);                          // units/second
    float td  = v / DECEL_PER_S2;                       // seconds to stop
    if (td < 0.001f) td = 0.001f;

    float dist   = 0.5f * v * td;                       // stopping distance
    float target = curPos + dir * dist;
    if (target > 1.0f) target = 1.0f;
    if (target < 0.0f) target = 0.0f;

    uint32_t durUs = (uint32_t)(td * 1e6f);
    if (durUs == 0) durUs = 1000;

    setMotion(durUs, curPos, target, false, true);      // ease-out to rest
}

// ---- Polynomial evaluation --------------------------------------------------

float MotionInterpolator::posFromCurve(uint32_t elapsedUs) const {
    if (_durationUs == 0)          return _coefD;   // hold (A=B=C=0)
    if (elapsedUs >= _durationUs)  return _endPos;

    float tau = (float)elapsedUs / (float)_durationUs;   // 0..1
    // Horner form.
    float pos = ((_coefA * tau + _coefB) * tau + _coefC) * tau + _coefD;
    if (pos < 0.0f) return 0.0f;
    if (pos > 1.0f) return 1.0f;
    return pos;
}

float MotionInterpolator::velFromCurve(uint32_t elapsedUs) const {
    if (_durationUs == 0) return 0.0f;

    float tau = (float)elapsedUs / (float)_durationUs;
    if (tau < 0.0f) tau = 0.0f;
    if (tau > 1.0f) tau = 1.0f;

    // d(pos)/d(tau) = 3A*tau^2 + 2B*tau + C
    float dpdtau = (3.0f * _coefA * tau + 2.0f * _coefB) * tau + _coefC;

    // Convert to per-second: dp/dt = dp/dtau * dtau/dt, dtau/dt = 1/durationSec.
    float durSec = (float)_durationUs * 1e-6f;
    return dpdtau / durSec;
}

// ---- Anomaly ring (Core 1 only, single-threaded — no lock) ------------------

void MotionInterpolator::recordAnomaly(InterpAnomalyType kind, float targetPos,
                                       float startPos, float startVel,
                                       float endSlope, uint32_t durationUs,
                                       float extra, uint64_t nowUs) {
    InterpAnomaly& e = _anomRing[_anomWrite];
    e.kind       = (uint8_t)kind;
    e.seq        = _anomSeq++;
    e.tDevUs     = (uint32_t)(nowUs & 0xFFFFFFFFu);
    e.targetPos  = targetPos;
    e.startPos   = startPos;
    e.startVel   = startVel;
    e.endSlope   = endSlope;
    e.durationUs = durationUs;
    e.extra      = extra;

    _anomWrite = (uint8_t)((_anomWrite + 1) % interp_cfg::ANOMALY_RING_DEPTH);
    // If the ring was already full, the oldest event is silently overwritten
    // (count stays pinned at DEPTH). The WebUI drains fast enough that this is
    // only reachable under a pathological anomaly storm.
    if (_anomCount < interp_cfg::ANOMALY_RING_DEPTH) _anomCount++;
}

bool MotionInterpolator::popAnomaly(InterpAnomaly& out) {
    if (_anomCount == 0) return false;
    uint8_t idx = (uint8_t)((_anomWrite - _anomCount
                             + interp_cfg::ANOMALY_RING_DEPTH)
                            % interp_cfg::ANOMALY_RING_DEPTH);
    out = _anomRing[idx];   // FIFO: oldest unread first
    _anomCount--;
    return true;
}

// Peak excursion of the current cubic beyond [min(start,end), max(start,end)].
// Analytic: the extrema of a cubic on [0,1] are at the roots of its derivative
// 3A*tau^2 + 2B*tau + C = 0 that fall inside (0,1). Endpoints are the envelope
// bounds themselves, so only interior extrema can overshoot.
float MotionInterpolator::envelopeOvershoot() const {
    if (_durationUs == 0) return 0.0f;

    float lo = (_startPos < _endPos) ? _startPos : _endPos;
    float hi = (_startPos < _endPos) ? _endPos : _startPos;

    float a = 3.0f * _coefA;   // derivative quadratic coefficients
    float b = 2.0f * _coefB;
    float c = _coefC;

    float taus[2];
    int   n = 0;
    if (fabsf(a) < 1e-9f) {
        if (fabsf(b) > 1e-9f) taus[n++] = -c / b;     // degenerate → linear
    } else {
        float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f) {
            float sq = sqrtf(disc);
            taus[n++] = (-b + sq) / (2.0f * a);
            taus[n++] = (-b - sq) / (2.0f * a);
        }
    }

    float peak = 0.0f;
    for (int i = 0; i < n; i++) {
        float tau = taus[i];
        if (tau <= 0.0f || tau >= 1.0f) continue;
        float pos = ((_coefA * tau + _coefB) * tau + _coefC) * tau + _coefD;
        float ex  = (pos > hi) ? (pos - hi) : (pos < lo ? (lo - pos) : 0.0f);
        if (ex > peak) peak = ex;
    }
    return peak;
}

// Fritsch–Carlson monotone tangent limiter. The Hermite cubic on a single
// interval is guaranteed to stay within [p0,p1] iff it is monotone, and it is
// monotone iff the normalized tangents (alpha,beta) = (m0,m1)/secant sit in the
// non-negative quarter-disc of radius 3. We project onto that region, which
// neutralizes all three observed overshoot flavors without changing the
// endpoints or the segment duration:
//   * secant ≈ 0  (start==end, nonzero carried velocity)  → flatten both
//   * alpha/beta < 0 (tangent opposes the move direction) → clamp that end to 0
//   * alpha²+beta² > 9 (too-hot entry/exit speed)         → scale back onto r=3
void MotionInterpolator::monotoneClampTangents(float p0, float p1,
                                               float& m0, float& m1) {
    const float delta = p1 - p0;   // secant slope in tau-space (tau spans 0..1)

    // Zero-displacement segment: any nonzero tangent makes the cubic bulge off
    // the endpoint and return (the continuity-bulge flavor). Flatten to a hold.
    if (fabsf(delta) < 1e-6f) {
        m0 = 0.0f;
        m1 = 0.0f;
        return;
    }

    float alpha = m0 / delta;      // start tangent in units of the secant slope
    float beta  = m1 / delta;      // end   tangent in units of the secant slope

    // A tangent pointing opposite the secant forces a reversal past an endpoint
    // (the opposing-end-slope flavor). Clamp that end to rest.
    if (alpha < 0.0f) { alpha = 0.0f; m0 = 0.0f; }
    if (beta  < 0.0f) { beta  = 0.0f; m1 = 0.0f; }

    // Outside the radius-3 circle the cubic overshoots (the high-entry-velocity
    // flavor). Scale both tangents back onto the circle, preserving direction.
    const float s = alpha * alpha + beta * beta;
    if (s > 9.0f) {
        const float t = 3.0f / sqrtf(s);
        m0 = t * alpha * delta;
        m1 = t * beta  * delta;
    }
}

// ---- Telemetry --------------------------------------------------------------

InterpDebug MotionInterpolator::snapshot(uint64_t nowUs) {
    InterpDebug d;
    d.curPos     = positionAt(nowUs);     // settles timeouts first
    d.curVel     = velocityAt(nowUs);
    d.startPos   = _startPos;
    d.endPos     = _endPos;
    d.durationUs = _durationUs;
    d.elapsedUs  = (nowUs > _startUs)
                     ? (uint32_t)((nowUs - _startUs > (uint64_t)_durationUs)
                                    ? _durationUs : (nowUs - _startUs))
                     : 0;
    d.liveMode   = _liveMode;
    d.gradMode   = _gradMode;
    d.style      = (uint8_t)_lastStyle;
    return d;
}