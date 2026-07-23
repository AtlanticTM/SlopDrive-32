#pragma once

#include <cstdint>
#include <cmath>

// ============================================================================
// TrapezoidProfile — closed-form trapezoidal motion profile, native counts
// ============================================================================
//
// Doctrine (CLAUDE.md §2 / plan.md "Design"): intent arrives -> ONE plan is
// computed here -> the streaming executor merely SAMPLES it on its own tick.
// plan() never runs on a clock; sample() never re-plans. This header owns the
// math ONLY — no bus, no FreeRTOS, no I/O. ServoMotionExecutor is the thing
// that calls plan()/sample() against the wall clock.
//
// Units: whatever native unit the caller passes in (this driver's caller
// always uses ENCODER COUNTS, home=0, front=negative — see ModbusServoDriver).
// Time: seconds (float) for durations, microseconds (int64_t, esp_timer_get_time())
// for the wall-clock epoch — mixing a monotonic int64 epoch with float
// DURATIONS keeps the profile numerically stable over a multi-hour uptime
// (a bare float holding raw microseconds would lose sub-ms precision after
// ~a few hours; t - t0 stays small and precise no matter how long we've been
// running). :3
//
// plan() handles four cases, closed-form, no iteration:
//   1. Trivial:      already at target, already stopped -> done immediately.
//   2. v0 == 0:       classic trapezoid/triangle from rest.
//   3. v0 toward target (same sign, doesn't overshoot): fold the existing
//      speed into the accel leg — no wasted brake-then-reaccelerate.
//   4. v0 away from target, OR moving toward it too fast/close to stop by the
//      target (overshoot): decelerate to zero FIRST (t_pre pre-segment), then
//      plan a fresh case-2/3 trapezoid from the position that decel lands on.
//
// Cases 2 and 3 are actually the SAME formula — case 2 is case 3 with
// v0 == 0 substituted in, so plan() implements one "main trapezoid" solver
// with an optional pre-segment in front of it.
struct TrapezoidProfile {
    // ---- Profile epoch + inputs (kept for diagnostics / totalTimeS) --------
    int64_t t0_us  = 0;      // esp_timer_get_time() at plan()
    float   p0     = 0.0f;
    float   v0     = 0.0f;
    float   target = 0.0f;
    float   vmax   = 0.0f;
    float   accel  = 0.0f;

    // ---- Segment durations, seconds -----------------------------------------
    // t_pre    : optional decel-to-zero pre-segment (0 if not needed)
    // t_acc/t_cruise/t_dec : the MAIN trapezoid, starting at t0+t_pre
    float t_pre     = 0.0f;
    float t_acc     = 0.0f;
    float t_cruise  = 0.0f;
    float t_dec     = 0.0f;

    // ---- Derived state for the MAIN trapezoid, cached at plan() time so ----
    // ---- sample() stays branch-light and allocation-free -------------------
    float _dir = 1.0f;    // +1/-1 direction of the MAIN trapezoid
    float _p1  = 0.0f;    // position where the MAIN trapezoid begins
    float _v1m = 0.0f;    // |entry velocity| into the MAIN trapezoid (0 unless folded)

    bool valid = false;

    // Position/velocity epsilons — sub-count precision is meaningless (wire
    // unit is whole encoder counts), so "basically zero" is a fraction of a
    // count / a fraction of a count-per-second. :3
    static constexpr float POSITION_EPS = 0.5f;
    static constexpr float VELOCITY_EPS = 0.5f;

    // ------------------------------------------------------------------------
    // plan() — a dozen-ish float ops + at most one sqrtf(). Called at the
    // stream-sampler rate (~1kHz) by the caller's grit-cache-gated dispatch —
    // MUST stay allocation-free and branch-shallow. :3
    // ------------------------------------------------------------------------
    static TrapezoidProfile plan(float p0, float v0, float target,
                                  float vmax, float accel, int64_t now_us) {
        TrapezoidProfile pr;
        pr.t0_us  = now_us;
        pr.p0     = p0;
        pr.v0     = v0;
        pr.target = target;

        // Floors — never let a zero/garbage ceiling produce a div-by-zero
        // below. Ceilings are ceilings, never targets (CLAUDE.md §2). :3
        vmax  = (vmax  < 1.0f) ? 1.0f : vmax;
        accel = (accel < 1.0f) ? 1.0f : accel;
        pr.vmax  = vmax;
        pr.accel = accel;

        float D = target - p0;

        // ---- Trivial: already there, already stopped ------------------------
        if (fabsf(D) < POSITION_EPS && fabsf(v0) < VELOCITY_EPS) {
            pr.t_pre = pr.t_acc = pr.t_cruise = pr.t_dec = 0.0f;
            pr._dir = 1.0f; pr._p1 = p0; pr._v1m = 0.0f;
            pr.valid = true;
            return pr;
        }

        // Defensive clamp — v0 is always our own previous sample, already
        // inside the ceiling, but a stray caller must never hand sqrtf() a
        // negative operand below. :3
        float v0c = (v0 > vmax) ? vmax : (v0 < -vmax ? -vmax : v0);

        int8_t dirNeeded = (D > 0.0f) ? 1 : (D < 0.0f ? -1 : 0);
        // D == 0 with v0 != 0 is deliberately NOT "same direction" — we're
        // sitting AT the target but still moving, so stop first (case 4). :3
        bool sameDir = (dirNeeded != 0) && (v0c * (float)dirNeeded >= 0.0f);

        float v0m      = sameDir ? fabsf(v0c) : 0.0f;
        float stopDist = (v0m * v0m) / (2.0f * accel);

        float p1, v1;
        if (!sameDir || stopDist > fabsf(D)) {
            // Case 4: wrong way, or right way but can't stop in time — brake
            // to zero first, then plan fresh from wherever that lands. :3
            pr.t_pre = fabsf(v0c) / accel;
            p1 = p0 + (v0c * fabsf(v0c)) / (2.0f * accel);   // signed stopping distance
            v1 = 0.0f;
        } else {
            // Cases 2/3: already heading the right way (or standing still) —
            // fold the existing speed straight into the accel leg. :3
            pr.t_pre = 0.0f;
            p1 = p0;
            v1 = v0c;
        }

        // ---- Main trapezoid, entry velocity v1 (same sign as the direction
        // ---- to the target from p1, or zero) ------------------------------
        float Dr   = target - p1;
        float dirf = (Dr >= 0.0f) ? 1.0f : -1.0f;
        float d    = fabsf(Dr);
        float v1m  = fabsf(v1);

        float d_dec_full = (vmax * vmax) / (2.0f * accel);
        float d_acc_full = (vmax * vmax - v1m * v1m) / (2.0f * accel);
        if (d_acc_full < 0.0f) d_acc_full = 0.0f;   // safety, shouldn't trigger post-clamp

        if (d < POSITION_EPS) {
            // The pre-segment landed (almost) exactly on target — nothing
            // left for the main trapezoid to do. :3
            pr.t_acc = pr.t_cruise = pr.t_dec = 0.0f;
        } else if (d_acc_full + d_dec_full <= d) {
            // Trapezoid: full accel to vmax, cruise, full decel to 0.
            pr.t_acc    = (vmax - v1m) / accel;
            pr.t_cruise = (d - d_acc_full - d_dec_full) / vmax;
            pr.t_dec    = vmax / accel;
        } else {
            // Triangle: peak velocity vp solved from accel-dist + decel-dist == d.
            //   (vp^2 - v1m^2)/(2A) + vp^2/(2A) = d  =>  vp = sqrt(A*d + v1m^2/2)
            float vp = sqrtf(accel * d + 0.5f * v1m * v1m);
            if (vp < v1m) vp = v1m;   // safety — the overshoot gate above should prevent this
            pr.t_acc    = (vp - v1m) / accel;
            pr.t_cruise = 0.0f;
            pr.t_dec    = vp / accel;
        }

        pr._dir  = dirf;
        pr._p1   = p1;
        pr._v1m  = v1m;
        pr.valid = true;
        return pr;
    }

    // ------------------------------------------------------------------------
    // sample() — pure, closed-form, no branches beyond "which segment am I
    // in." Clamps to (target, 0) once the whole profile has elapsed. :3
    // ------------------------------------------------------------------------
    void sample(int64_t now_us, float& pos, float& vel) const {
        if (!valid) { pos = p0; vel = 0.0f; return; }

        float t = (float)(now_us - t0_us) * 1.0e-6f;
        if (t < 0.0f) t = 0.0f;

        if (t < t_pre) {
            // Pre-segment: braking v0 toward zero.
            float s   = (v0 >= 0.0f) ? 1.0f : -1.0f;
            float v0m = fabsf(v0);
            float vt  = v0m - accel * t;
            if (vt < 0.0f) vt = 0.0f;
            pos = p0 + s * (v0m * t - 0.5f * accel * t * t);
            vel = s * vt;
            return;
        }

        float tm = t - t_pre;   // time into the MAIN trapezoid
        float total = t_acc + t_cruise + t_dec;

        if (tm >= total) {
            pos = target;
            vel = 0.0f;
            return;
        }

        if (tm < t_acc) {
            float v = _v1m + accel * tm;
            float d = _v1m * tm + 0.5f * accel * tm * tm;
            pos = _p1 + _dir * d;
            vel = _dir * v;
        } else if (tm < t_acc + t_cruise) {
            float d_acc = _v1m * t_acc + 0.5f * accel * t_acc * t_acc;
            float tc    = tm - t_acc;
            pos = _p1 + _dir * (d_acc + vmax * tc);
            vel = _dir * vmax;
        } else {
            float d_acc = _v1m * t_acc + 0.5f * accel * t_acc * t_acc;
            float d_cru = vmax * t_cruise;
            float td    = tm - t_acc - t_cruise;
            float vpk   = _v1m + accel * t_acc;   // peak velocity reached (== vmax if cruise ran)
            float v     = vpk - accel * td;
            if (v < 0.0f) v = 0.0f;
            float d     = vpk * td - 0.5f * accel * td * td;
            pos = _p1 + _dir * (d_acc + d_cru + d);
            vel = _dir * v;
        }
    }

    float totalTimeS() const { return t_pre + t_acc + t_cruise + t_dec; }

    bool done(int64_t now_us) const {
        if (!valid) return true;
        float t = (float)(now_us - t0_us) * 1.0e-6f;
        return t >= totalTimeS();
    }
};

// ============================================================================
// Unit-test-in-comments — worked examples (counts, counts/s, counts/s²) so a
// reviewer can check plan() by hand. All start at t0_us=0 for readability. :3
//
// Example A — v0=0, classic trapezoid:
//   plan(p0=0, v0=0, target=10000, vmax=5000, accel=25000)
//   d_acc_full = 5000^2/(2*25000) = 500;  d_dec_full = 500;  sum=1000 <= 10000
//   -> trapezoid: t_acc=(5000-0)/25000=0.2s, d_cruise=10000-1000=9000,
//      t_cruise=9000/5000=1.8s, t_dec=0.2s. Total 2.2s.
//   Sanity: 0.5*5000*0.2 (accel dist) + 5000*1.8 (cruise) + 0.5*5000*0.2 (decel)
//         = 500 + 9000 + 500 = 10000 ✓
//
// Example B — v0=0, triangle (too close to reach vmax):
//   plan(p0=0, v0=0, target=500, vmax=5000, accel=25000)
//   d_acc_full+d_dec_full = 1000 > 500 -> triangle
//   vp = sqrt(25000*500 + 0) = sqrt(12,500,000) ≈ 3535.5
//   t_acc = t_dec = 3535.5/25000 ≈ 0.1414s each, t_cruise=0. Total ≈ 0.2828s.
//   Sanity: vp*t_acc = 3535.5*0.1414 ≈ 500 ✓ (triangle area = one accel leg)
//
// Example C — v0 toward target, folded into the accel leg (case 3):
//   plan(p0=0, v0=2000, target=10000, vmax=5000, accel=25000)
//   stopDist = 2000^2/(2*25000)=80 <= D=10000 -> sameDir, no pre-segment.
//   d_acc_full=(5000^2-2000^2)/50000=420, d_dec_full=500, sum=920<=10000
//   -> t_acc=(5000-2000)/25000=0.12s, d_cruise=10000-920=9080,
//      t_cruise=9080/5000=1.816s, t_dec=0.2s.
//
// Example D — v0 AWAY from target, reversal (case 4):
//   plan(p0=0, v0=-3000, target=10000, vmax=5000, accel=25000)
//   dirNeeded=+1, v0*dirNeeded=-3000<0 -> NOT sameDir -> pre-segment.
//   t_pre = 3000/25000 = 0.12s
//   p1 = 0 + (-3000*3000)/(2*25000) = -9,000,000/50000 = -180
//     (carriage keeps sliding backward 180 counts while it brakes)
//   Fresh trapezoid from p1=-180 to 10000: Dr=10180 -> t_acc=0.2s,
//     d_cruise=10180-1000=9180, t_cruise=1.836s, t_dec=0.2s.
//   Total ≈ 0.12 + 2.236 = 2.356s.
//
// Example E — v0 toward target but overshooting (case 4 via stopDist gate):
//   plan(p0=0, v0=4000, target=100, vmax=5000, accel=25000)
//   dirNeeded=+1, v0*dirNeeded=4000>0 -> sameDir so far.
//   stopDist = 4000^2/50000 = 320 > D=100 -> OVERSHOOT -> pre-segment anyway.
//   t_pre = 4000/25000 = 0.16s
//   p1 = 0 + 4000*4000/50000 = 320   (carriage sails 320 past the 100 target)
//   Fresh trapezoid from p1=320 back to target=100: Dr=-220, dirf=-1, d=220.
//   d_acc_full+d_dec_full=1000>220 -> triangle: vp=sqrt(25000*220)=sqrt(5.5e6)≈2345.2
//   t_acc=t_dec≈0.0938s each. The carriage brakes past the target, then a
//   short reverse triangle brings it back — physically correct given the
//   accel ceiling, and much safer than lunging straight through target. :3
// ============================================================================
