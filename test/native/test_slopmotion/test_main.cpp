// ============================================================================
// test_slopmotion — native doctest suite for the SlopMotion engine
// ============================================================================
//
// Hardware-free, fully deterministic: time is a synthetic uint64 microsecond
// counter, no clocks, no randomness. Every kinematic assertion is checked by
// SAMPLING the produced trajectory on a 1 ms grid (the same cadence the
// firmware's stream sampler uses) — limits are verified as sampled reality,
// not trusted from Ruckig's promises.
//
// Finite-difference tolerances: velocity/accel come from Ruckig analytically,
// but jerk is checked as Δa/Δt on the 1 ms grid, which averages across the
// bang-bang jerk switching instants — so the jerk bound uses a small margin.
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "slopmotion/slopmotion.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using slopmotion::AnomalyType;
using slopmotion::Command;
using slopmotion::Config;
using slopmotion::Engine;
using slopmotion::InfeasiblePolicy;
using slopmotion::Mode;

namespace {

constexpr uint64_t kMs = 1000ULL;        // µs per ms
constexpr uint64_t kS  = 1000000ULL;     // µs per s

Config testConfig() {
    Config cfg;
    cfg.limits.vmax = 2.0f;
    cfg.limits.amax = 20.0f;
    cfg.limits.jmax = 300.0f;
    return cfg;
}

// The limit set of the virtual machine both defects were measured against:
// 500 mm stroke window, 550 mm/s input speed, 8000 mm/s² input accel, fixed
// jerk — normalized by the window span exactly as the firmware glue does.
Config machineConfig() {
    Config cfg;
    cfg.limits.vmax = 1.1f;    // 550 mm/s  / 500 mm
    cfg.limits.amax = 16.0f;   // 8000 mm/s² / 500 mm
    cfg.limits.jmax = 500.0f;  // firmware default (SystemState::sm_tune_jmax_ovr)
    return cfg;
}

// The OPERATOR's machine, measured: stroke window [150, 350] mm (span 200),
// input speed 1000 mm/s, input accel 50000 mm/s², jerk 2e6 mm/s³ — normalized
// by the span exactly as the firmware glue does. All the Reshape numbers below
// are quoted in millimeters against this window, because that is the domain
// the amplitude complaint was made in.
constexpr double kSpanMm = 200.0;
Config operatorConfig() {
    Config cfg;
    cfg.limits.vmax = 5.0f;       // 1000 mm/s  / 200 mm
    cfg.limits.amax = 250.0f;     // 50000 mm/s² / 200 mm
    cfg.limits.jmax = 10000.0f;   // 2e6 mm/s³   / 200 mm
    return cfg;
}

// Drain the ring, reporting whether a given anomaly kind appeared and the
// detail/target of its LAST occurrence.
struct AnomalyHit {
    bool  seen   = false;
    float detail = 0.0f;
    float target = 0.0f;
    int   count  = 0;
};

AnomalyHit drainFor(Engine& e, AnomalyType kind) {
    AnomalyHit h;
    slopmotion::Anomaly ev;
    while (e.popAnomaly(ev)) {
        if (ev.kind == (uint8_t)kind) {
            h.seen = true;
            h.detail = ev.detail;
            h.target = ev.target;
            h.count++;
        }
    }
    return h;
}

struct SweepStats {
    double max_abs_v = 0.0;
    double max_abs_a = 0.0;
    double max_abs_j = 0.0;   // finite-difference of accel on the grid
    double min_p = 1e9, max_p = -1e9;
    double max_dv = 0.0;      // largest velocity step between adjacent samples
};

// Sample [t0, t1] on a 1 ms grid and accumulate kinematic extremes.
SweepStats sweep(Engine& e, uint64_t t0_us, uint64_t t1_us) {
    SweepStats s;
    double prev_v = 0.0, prev_a = 0.0;
    bool first = true;
    for (uint64_t t = t0_us; t <= t1_us; t += kMs) {
        const double p = e.positionAt(t);
        const double v = e.velocityAt(t);
        const double a = e.accelerationAt(t);
        REQUIRE(std::isfinite(p));
        REQUIRE(std::isfinite(v));
        REQUIRE(std::isfinite(a));
        s.max_abs_v = std::max(s.max_abs_v, std::fabs(v));
        s.max_abs_a = std::max(s.max_abs_a, std::fabs(a));
        s.min_p = std::min(s.min_p, p);
        s.max_p = std::max(s.max_p, p);
        if (!first) {
            s.max_abs_j = std::max(s.max_abs_j, std::fabs(a - prev_a) / 1e-3);
            s.max_dv    = std::max(s.max_dv, std::fabs(v - prev_v));
        }
        prev_v = v; prev_a = a; first = false;
    }
    return s;
}

// ---- Band measurement for the DC-centering tests ----------------------------
// A stroke chain's achieved BAND: where the motion actually sits and how wide
// it is, measured on the SAMPLED position (what the operator feels), averaged
// over the last 10 cycles — plus the per-cycle spread, which is what tells a
// converged band apart from one that is merely right ON AVERAGE while orbiting
// (a real failure mode of this control loop — see the header's control-law
// note; the undamped version parked in a rock-stable ±7 mm period-4 orbit).
struct ChainOpts {
    InfeasiblePolicy pol      = InfeasiblePolicy::Reshape;
    bool             centering = true;
    float            gain     = 1.0f;
    float            vmax     = 5.0f;      // 1000 mm/s on the 200 mm window
    bool             ev_down  = false;     // wire end velocity on down-strokes
    double           lo       = 0.30;
    double           hi       = 1.00;
    uint32_t         seg_ms   = 167;
    int              cycles   = 30;
    bool             soften   = true;      // 0.6.0 sharpness search (false = 0.5.0)
};

struct Band {
    double center_err_mm   = 0.0;   // signed, vs the commanded midpoint
    double amp_mm          = 0.0;
    double center_spread_mm = 0.0;  // over the measured cycles: hunting metric
    double amp_spread_mm   = 0.0;
    int    centered = 0, scaled = 0, fallback = 0;
    bool   bounds_ok = true;        // every endpoint inside [start, target]
    // ---- SHAPE, averaged over the measured segments (0.6.0) ----------------
    // sharp = Snapshot::sharpness (peak jerk / jmax); flat_pct = share of the
    // segment spent within 2 % of its own peak velocity, i.e. how much of the
    // stroke is the straight line the operator complained about.
    double sharp    = 0.0;
    double flat_pct = 0.0;
    double vpk_mm   = 0.0;          // peak speed, mm/s
    double apk_mm   = 0.0;          // peak accel, mm/s²
};

Band runChain(const ChainOpts& o) {
    Config cfg;
    cfg.limits.vmax = o.vmax;
    cfg.limits.amax = 250.0f;
    cfg.limits.jmax = 10000.0f;
    cfg.infeasible_policy   = o.pol;
    cfg.infeasible_soften   = o.soften;
    cfg.wave_centering      = o.centering;
    cfg.wave_centering_gain = o.gain;
    Engine e(cfg, (float)o.lo);

    const uint64_t seg = (uint64_t)o.seg_ms * kMs;
    Band b;
    std::vector<double> seg_min, seg_max;
    double cur_min = 1e9, cur_max = -1e9;
    double sh_sum = 0, flat_sum = 0, vpk_sum = 0, apk_sum = 0;
    int    sh_n = 0;
    uint64_t next_cmd = 0;
    for (int i = 0; i <= 2 * o.cycles; i++) {
        const bool up = (i % 2) != 0;
        const double p0 = e.positionAt(next_cmd);
        Command c;
        c.target       = (float)(up ? o.hi : o.lo);
        c.duration_us  = (uint32_t)seg;
        c.has_duration = true;
        if (o.ev_down && !up) { c.end_vel = -2.6f; c.has_end_vel = true; }
        e.commit(c, next_cmd);
        // Bounded correction: the planned endpoint must lie between where we
        // started and what was commanded — never past the target (overshoot),
        // never behind the start (inversion).
        const double ep = e.snapshot(next_cmd).target;
        const double t  = (double)c.target;
        if (ep < std::min(p0, t) - 1e-6 || ep > std::max(p0, t) + 1e-6) {
            b.bounds_ok = false;
        }
        slopmotion::Anomaly ev;
        while (e.popAnomaly(ev)) {
            if (ev.kind == (uint8_t)AnomalyType::WaveformCentered)  b.centered++;
            if (ev.kind == (uint8_t)AnomalyType::WaveformScaled)   b.scaled++;
            if (ev.kind == (uint8_t)AnomalyType::WaveformFallback) b.fallback++;
        }
        if (i > 0) { seg_min.push_back(cur_min); seg_max.push_back(cur_max); }
        cur_min = 1e9; cur_max = -1e9;
        double vpk = 0, apk = 0;
        int    n = 0, flat = 0;
        std::vector<double> vv;
        for (uint64_t t2 = next_cmd; t2 < next_cmd + seg; t2 += kMs) {
            const double p = e.positionAt(t2);
            REQUIRE(p >= -1e-9);
            REQUIRE(p <= 1.0 + 1e-9);
            cur_min = std::min(cur_min, p);
            cur_max = std::max(cur_max, p);
            const double v = std::fabs(e.velocityAt(t2));
            vpk = std::max(vpk, v);
            apk = std::max(apk, (double)std::fabs(e.accelerationAt(t2)));
            vv.push_back(v); n++;
        }
        // Shape stats over the SETTLED tail of the chain only (the first few
        // strokes are the centering loop converging, not steady state).
        if (i >= 2 * o.cycles - 19) {
            for (double v : vv) if (v >= 0.98 * vpk) flat++;
            sh_sum   += e.snapshot(next_cmd).sharpness;
            flat_sum += 100.0 * flat / (double)n;
            vpk_sum  += vpk * kSpanMm;
            apk_sum  += apk * kSpanMm;
            sh_n++;
        }
        next_cmd += seg;
    }
    if (sh_n) {
        b.sharp = sh_sum / sh_n;   b.flat_pct = flat_sum / sh_n;
        b.vpk_mm = vpk_sum / sh_n; b.apk_mm   = apk_sum / sh_n;
    }
    // Pair segments into cycles over the last 10 cycles.
    const size_t n = seg_min.size();
    REQUIRE(n >= 20);
    double c_sum = 0, a_sum = 0, c_lo = 1e9, c_hi = -1e9, a_lo = 1e9, a_hi = -1e9;
    int cyc = 0;
    for (size_t k = n - 20; k + 1 < n; k += 2) {
        const double bot = std::min(seg_min[k], seg_min[k + 1]);
        const double top = std::max(seg_max[k], seg_max[k + 1]);
        const double cen = 0.5 * (bot + top), amp = top - bot;
        c_sum += cen; a_sum += amp; cyc++;
        c_lo = std::min(c_lo, cen); c_hi = std::max(c_hi, cen);
        a_lo = std::min(a_lo, amp); a_hi = std::max(a_hi, amp);
    }
    b.center_err_mm    = (c_sum / cyc - 0.5 * (o.lo + o.hi)) * kSpanMm;
    b.amp_mm           = (a_sum / cyc) * kSpanMm;
    b.center_spread_mm = (c_hi - c_lo) * kSpanMm;
    b.amp_spread_mm    = (a_hi - a_lo) * kSpanMm;
    return b;
}

void reportBand(const std::string& name, const Band& b) {
    MESSAGE(name << ": center " << b.center_err_mm << " mm off, amplitude "
                 << b.amp_mm << " mm, spread (center/amp) "
                 << b.center_spread_mm << "/" << b.amp_spread_mm
                 << " mm, anomalies centered/scaled/fallback " << b.centered
                 << "/" << b.scaled << "/" << b.fallback);
}

// ---- Single-segment SHAPE measurement (the 0.6.0 sharpness work) -----------
// Everything the operator asked to see for one timed segment: what it actually
// delivered, and how straight the line was while it delivered it. Sampled on a
// 0.2 ms grid strictly INSIDE the plan (positionAt runs maybeSettle, and a
// sample past the deadline would measure the brake, not the segment).
struct Shape {
    double travel_mm  = 0.0;
    double sharp      = 1.0;    // Snapshot::sharpness = peak jerk / jmax
    double vpk        = 0.0;    // normalized units/s
    double apk        = 0.0;
    double jpk        = 0.0;    // finite-difference on the sample grid
    double pk_over_mean = 0.0;  // 1.0 = square (flat), 1.875 = min-jerk quintic
    double flat_pct   = 0.0;    // % of the segment within 2 % of its own vpk
    double duration_s = 0.0;
    uint8_t kind      = 0;      // PlanKind
    uint32_t failures = 0;      // PlanFailed count (must never rise with soften)
};

Shape segShape(Config cfg, double start, double target, uint32_t ms,
               bool has_ev = false, float ev = 0.0f) {
    Engine e(cfg, (float)start);
    Command c;
    c.target       = (float)target;
    c.duration_us  = ms * (uint32_t)kMs;
    c.has_duration = true;
    c.end_vel      = ev;
    c.has_end_vel  = has_ev;
    REQUIRE(e.commit(c, 0));

    Shape s;
    s.sharp      = e.snapshot(0).sharpness;
    s.duration_s = e.snapshot(0).duration_s;
    s.failures   = e.snapshot(0).failures;
    s.kind       = (uint8_t)e.planKind();

    const uint64_t end = (uint64_t)ms * kMs;
    std::vector<double> vs;
    double prev_a = 0.0; bool first = true;
    for (uint64_t t = 0; t < end; t += 200) {
        const double v = std::fabs((double)e.velocityAt(t));
        const double a = (double)e.accelerationAt(t);
        s.vpk = std::max(s.vpk, v);
        s.apk = std::max(s.apk, std::fabs(a));
        if (!first) s.jpk = std::max(s.jpk, std::fabs(a - prev_a) / 2e-4);
        prev_a = a; first = false;
        vs.push_back(v);
    }
    s.travel_mm = std::fabs((double)e.positionAt(end) - start) * kSpanMm;
    const double mean = (s.travel_mm / kSpanMm) / ((double)ms * 1e-3);
    s.pk_over_mean = mean > 1e-9 ? s.vpk / mean : 0.0;
    int flat = 0;
    for (double v : vs) if (v >= 0.98 * s.vpk) flat++;
    s.flat_pct = 100.0 * flat / (double)vs.size();
    return s;
}

void reportShape(const std::string& name, const Shape& s, double jmax) {
    MESSAGE(name << ": travel " << s.travel_mm << " mm, j_eff "
                 << (s.sharp * jmax * kSpanMm) << " mm/s^3 (" << s.sharp
                 << " of jmax), vpk " << (s.vpk * kSpanMm) << " mm/s, apk "
                 << (s.apk * kSpanMm) << " mm/s^2, peak/mean " << s.pk_over_mean
                 << ", flat " << s.flat_pct << " %");
}

} // namespace

// ============================================================================
TEST_CASE("Idle: fresh engine holds its seed position, not busy") {
    Engine e(testConfig(), 0.3f);
    CHECK(e.positionAt(0) == doctest::Approx(0.3f));
    CHECK(e.velocityAt(5 * kS) == doctest::Approx(0.0));
    CHECK_FALSE(e.isBusy(0));
    CHECK(e.mode() == Mode::Idle);
}

TEST_CASE("Waveform rest-to-rest: lands on target, at rest, on the deadline") {
    Engine e(testConfig(), 0.2f);

    Command c;
    c.target       = 0.8f;
    c.duration_us  = 600 * (uint32_t)kMs;
    c.has_duration = true;
    const uint64_t t0 = 1 * kS;
    REQUIRE(e.commit(c, t0));
    CHECK(e.mode() == Mode::Waveform);
    CHECK(e.planKind() == slopmotion::PlanKind::Quintic);
    CHECK(e.isBusy(t0 + 10 * kMs));

    auto snap = e.snapshot(t0);
    // A quintic segment spans exactly the commanded duration.
    CHECK(snap.duration_s == doctest::Approx(0.6).epsilon(0.01));

    // Landed: position, rest, not busy.
    const uint64_t tEnd = t0 + 700 * kMs;
    CHECK(e.positionAt(tEnd) == doctest::Approx(0.8).epsilon(1e-4));
    CHECK(e.velocityAt(tEnd) == doctest::Approx(0.0).epsilon(1e-4));
    CHECK_FALSE(e.isBusy(tEnd));

    // No anomalies for a clean feasible move.
    slopmotion::Anomaly ev;
    CHECK_FALSE(e.popAnomaly(ev));
}

TEST_CASE("Waveform quintic is the min-jerk curve (shape fidelity)") {
    // The whole point of the quintic path: reproduce the sender's spline, not
    // a cruise-and-burst chord. Rest-to-rest min-jerk has exact analytic
    // midpoint values: p(T/2) = midpoint, v(T/2) = 1.875 * dist / T.
    Engine e(testConfig(), 0.2f);
    Command c;
    c.target = 0.8f; c.duration_us = 600 * (uint32_t)kMs; c.has_duration = true;
    REQUIRE(e.commit(c, 0));
    REQUIRE(e.planKind() == slopmotion::PlanKind::Quintic);

    CHECK(e.positionAt(300 * kMs) == doctest::Approx(0.5).epsilon(1e-4));
    CHECK(e.velocityAt(300 * kMs) ==
          doctest::Approx(1.875 * 0.6 / 0.6).epsilon(1e-3));
    // Acceleration at both ends is zero (C2 into a rest hold).
    CHECK(e.accelerationAt(0) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(e.accelerationAt(600 * kMs) == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("Consecutive G-slope segments join C2 (no accel jump at boundaries)") {
    // The legacy cubic was C1: acceleration JUMPED at every v4 boundary
    // (bench-measured ~8 u/s^2 discontinuities). The quintic chain replans
    // from its own sampled (p,v,a), so accel is continuous at joints — the
    // largest 1 ms accel step anywhere must be jerk-limited, not a jump.
    Config cfg;
    cfg.limits.vmax = 3.0f; cfg.limits.amax = 30.0f; cfg.limits.jmax = 500.0f;
    const double amp = 0.35, f = 0.5, base = 0.5;
    Engine e(cfg, (float)base);

    auto srcP = [&](double t){ return base + amp * std::sin(2*M_PI*f*t); };
    auto srcV = [&](double t){ return amp*2*M_PI*f * std::cos(2*M_PI*f*t); };

    const uint64_t seg = 500 * kMs;
    uint64_t next_cmd = 0;
    double prev_a = 0.0; bool have_prev = false;
    double max_da = 0.0;
    for (uint64_t t = 0; t <= 4 * kS; t += kMs) {
        if (t >= next_cmd) {
            const double te = (double)(next_cmd + seg) * 1e-6;
            Command c;
            c.target       = (float)srcP(te);
            c.duration_us  = (uint32_t)seg;
            c.has_duration = true;
            c.end_vel      = (float)srcV(te);
            c.has_end_vel  = true;
            REQUIRE(e.commit(c, next_cmd));
            REQUIRE(e.planKind() == slopmotion::PlanKind::Quintic);
            next_cmd += seg;
        }
        const double a = e.accelerationAt(t);
        // skip the first (catch-up) segment: entry-state mismatch is real work
        if (have_prev && t > seg) max_da = std::max(max_da, std::fabs(a - prev_a));
        prev_a = a; have_prev = true;
    }
    // Jerk-limited step, never a discontinuity: da <= jmax * 1ms with margin.
    // (The cubic's boundary jumps were ~8.0 here — an order of magnitude out.)
    CHECK(max_da <= 500.0 * 1e-3 * 1.5 + 0.05);
}

TEST_CASE("Over-demanding waveform falls back to the Ruckig guard, ceilings hold") {
    // 0->1 in 900 ms wants a min-jerk peak velocity of 1.875/0.9 = 2.08 —
    // just over the 2.0 ceiling. The quintic must NOT be executed; the guard
    // takes the segment and every sampled ceiling still holds.
    //
    // Pinned to Stretch: this is the GUARD's test, and the guard is what the
    // Stretch policy uses. Under Scale the same command is now (correctly)
    // scaled to a 0.883 stroke on the 900 ms deadline instead — quintic-exact
    // sizing made the Scale policy fire on cases the old trapezoid envelope
    // declined (adist 1.0 vs an envelope of 1.47, i.e. "looks feasible").
    auto cfg = testConfig();
    cfg.infeasible_policy = InfeasiblePolicy::Stretch;
    Engine e(cfg, 0.0f);

    Command c;
    c.target       = 1.0f;
    c.duration_us  = 900 * (uint32_t)kMs;
    c.has_duration = true;
    REQUIRE(e.commit(c, 0));
    CHECK(e.planKind() == slopmotion::PlanKind::Ruckig);

    slopmotion::Anomaly ev;
    bool saw_fallback = false;
    while (e.popAnomaly(ev)) {
        if (ev.kind == (uint8_t)AnomalyType::WaveformFallback) saw_fallback = true;
    }
    CHECK(saw_fallback);

    auto s = sweep(e, 0, 1 * kS);
    CHECK(s.max_abs_v <= cfg.limits.vmax * 1.001);
    CHECK(s.max_abs_a <= cfg.limits.amax * 1.001);
    // Finite-difference jerk averages across switching instants — margin.
    CHECK(s.max_abs_j <= cfg.limits.jmax * 1.05 + 1.0);
    CHECK(s.min_p >= -1e-9);
    CHECK(s.max_p <= 1.0 + 1e-9);
}

TEST_CASE("Infeasible deadline stretches to physical minimum + anomaly") {
    auto cfg = testConfig();          // vmax = 2 → 0→1 takes ≥ 0.5 s
    cfg.infeasible_policy = InfeasiblePolicy::Stretch;   // the guard's test
    Engine e(cfg, 0.0f);

    Command c;
    c.target       = 1.0f;
    c.duration_us  = 50 * (uint32_t)kMs;   // ludicrous 50 ms demand
    c.has_duration = true;
    REQUIRE(e.commit(c, 0));

    auto snap = e.snapshot(0);
    CHECK(snap.duration_s > 0.5f);    // stretched to ≥ distance / vmax

    slopmotion::Anomaly ev;
    bool saw_fallback = false, saw_stretch = false;
    float stretch_detail = 0.0f;
    while (e.popAnomaly(ev)) {
        if (ev.kind == (uint8_t)AnomalyType::WaveformFallback) saw_fallback = true;
        if (ev.kind == (uint8_t)AnomalyType::DeadlineStretched) {
            saw_stretch = true;
            stretch_detail = ev.detail;
        }
    }
    CHECK(saw_fallback);              // the quintic could never do this move
    REQUIRE(saw_stretch);
    CHECK(stretch_detail == doctest::Approx(snap.duration_s).epsilon(0.01));
}

TEST_CASE("Retarget mid-move is C2-continuous at the commit instant") {
    Engine e(testConfig(), 0.2f);

    Command a;
    a.target = 0.9f; a.duration_us = 800 * (uint32_t)kMs; a.has_duration = true;
    REQUIRE(e.commit(a, 0));

    const uint64_t tSwitch = 300 * kMs;
    const double p1 = e.positionAt(tSwitch);
    const double v1 = e.velocityAt(tSwitch);
    const double a1 = e.accelerationAt(tSwitch);
    REQUIRE(std::fabs(v1) > 0.1);     // genuinely mid-flight

    Command b;                        // bare chase point, hard reversal
    b.target = 0.1f;
    REQUIRE(e.commit(b, tSwitch));
    CHECK(e.mode() == Mode::Chase);

    // The new plan starts EXACTLY from the sampled state — no kink in p/v/a.
    CHECK(e.positionAt(tSwitch)     == doctest::Approx(p1).epsilon(1e-6));
    CHECK(e.velocityAt(tSwitch)     == doctest::Approx(v1).epsilon(1e-6));
    CHECK(e.accelerationAt(tSwitch) == doctest::Approx(a1).epsilon(1e-6));

    // And it still lands on the new target at rest.
    const uint64_t tEnd = tSwitch + 3 * kS;
    CHECK(e.positionAt(tEnd) == doctest::Approx(0.1).epsilon(1e-4));
    CHECK(e.velocityAt(tEnd) == doctest::Approx(0.0).epsilon(1e-4));
}

TEST_CASE("Chase: 60 Hz sine stream tracks smoothly within limits") {
    Config cfg;
    cfg.limits.vmax = 3.0f;
    cfg.limits.amax = 30.0f;
    cfg.limits.jmax = 500.0f;
    Engine e(cfg, 0.5f);

    // 0.75 Hz sine, amplitude 0.4 → peak vel ≈ 1.88, peak acc ≈ 8.9: well
    // inside the ceilings, so a good tracker should hug it.
    const double f = 0.75;
    auto target = [&](double t) {
        return 0.5 + 0.4 * std::sin(2.0 * M_PI * f * t);
    };

    double worst_err = 0.0;
    double prev_v = 0.0;
    bool have_prev = false;
    uint64_t next_cmd = 0;
    for (uint64_t t = 0; t <= 3 * kS; t += kMs) {
        if (t >= next_cmd) {
            Command c;
            c.target = (float)target((double)t * 1e-6);
            REQUIRE(e.commit(c, t));
            next_cmd += 16667;        // ~60 Hz point stream
        }
        const double p = e.positionAt(t);
        const double v = e.velocityAt(t);
        REQUIRE(std::isfinite(p));
        REQUIRE(p >= -1e-9);
        REQUIRE(p <= 1.0 + 1e-9);
        // Velocity must stay ceiling-bounded and step-continuous ACROSS
        // replans — this is the C2 no-microstutter claim, sampled.
        CHECK(std::fabs(v) <= cfg.limits.vmax * 1.001);
        if (have_prev) {
            CHECK(std::fabs(v - prev_v) <=
                  (double)cfg.limits.amax * 1e-3 * 1.05 + 1e-6);
        }
        prev_v = v; have_prev = true;
        if (t > 500 * kMs) {          // after initial catch-up
            worst_err = std::max(worst_err,
                                 std::fabs(p - target((double)t * 1e-6)));
        }
    }
    // Tracking lag exists (the engine chases points, it cannot see the
    // future): predictive aim + velocity/accel feedforward measured ~0.06
    // peak on this clean-grid sine (~35 ms of estimator-smoothing lag at
    // peak velocity — the price of jitter immunity). This bound is a
    // regression tripwire, not a quality target; lag tuning is done with
    // eyes on the scenario graphs (examples/slopmotion_traces).
    CHECK(worst_err < 0.10);
}

TEST_CASE("Starve-settle: dead stream brakes to rest and holds") {
    Config cfg;
    cfg.limits.vmax = 3.0f;
    cfg.limits.amax = 30.0f;
    cfg.limits.jmax = 500.0f;
    Engine e(cfg, 0.5f);

    // Feed an ascending ramp with feedforward so the trajectory is mid-glide
    // with real velocity when the stream dies.
    uint64_t t = 0;
    for (int i = 0; i < 30; i++) {
        Command c;
        c.target = 0.2f + 0.02f * (float)i;   // steady 1.2 units/s ramp
        REQUIRE(e.commit(c, t));
        t += 16667;
    }
    const uint64_t t_dead = t;

    // Keep sampling — no more commands. The engine must brake to rest.
    double final_p = -1.0;
    for (uint64_t ts = t_dead; ts <= t_dead + 2 * kS; ts += kMs) {
        final_p = e.positionAt(ts);
    }
    CHECK(e.velocityAt(t_dead + 2 * kS) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK_FALSE(e.isBusy(t_dead + 2 * kS));
    CHECK(e.mode() == Mode::Idle);

    // Position frozen after settle (hold, no drift).
    CHECK(e.positionAt(t_dead + 3 * kS) == doctest::Approx(final_p).epsilon(1e-9));

    // A SettleEngaged anomaly was recorded.
    slopmotion::Anomaly ev;
    bool saw_settle = false;
    while (e.popAnomaly(ev)) {
        if (ev.kind == (uint8_t)AnomalyType::SettleEngaged) saw_settle = true;
    }
    CHECK(saw_settle);
}

TEST_CASE("End velocity near a wall is clamped bound-safe") {
    auto cfg = testConfig();
    Engine e(cfg, 0.5f);

    Command c;                        // v4 point: land at 0.98 STILL MOVING fast
    c.target       = 0.98f;
    c.duration_us  = 400 * (uint32_t)kMs;
    c.has_duration = true;
    c.end_vel      = 1.5f;
    c.has_end_vel  = true;
    REQUIRE(e.commit(c, 0));

    slopmotion::Anomaly ev;
    bool saw_clamp = false;
    while (e.popAnomaly(ev)) {
        if (ev.kind == (uint8_t)AnomalyType::EndVelClamped) {
            saw_clamp = true;
            // clamped to √(amax·dist) = √(20·0.02) ≈ 0.632, not the asked 1.5
            CHECK(std::fabs(ev.detail) <= std::sqrt(20.0 * 0.02) + 1e-6);
        }
    }
    CHECK(saw_clamp);

    // Sampled trajectory (then starve-settle) never leaves the window.
    auto s = sweep(e, 0, 2 * kS);
    CHECK(s.max_p <= 1.0 + 1e-9);
    CHECK(s.min_p >= -1e-9);
    CHECK(e.velocityAt(2 * kS) == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("Non-finite input is rejected; previous plan keeps executing") {
    Engine e(testConfig(), 0.2f);

    Command good;
    good.target = 0.7f; good.duration_us = 500 * (uint32_t)kMs;
    good.has_duration = true;
    REQUIRE(e.commit(good, 0));

    Command evil;
    evil.target = std::nanf("");
    CHECK_FALSE(e.commit(evil, 100 * kMs));

    slopmotion::Anomaly ev;
    REQUIRE(e.popAnomaly(ev));
    CHECK(ev.kind == (uint8_t)AnomalyType::PlanFailed);
    CHECK(ev.detail == doctest::Approx(-99.0f));

    // The good plan is untouched and still lands.
    CHECK(e.positionAt(600 * kMs) == doctest::Approx(0.7).epsilon(1e-4));
    CHECK(e.snapshot(600 * kMs).failures == 1);
}

TEST_CASE("Determinism: identical command/time sequences → identical samples") {
    auto run = [](std::vector<float>& out) {
        Config cfg;
        cfg.limits.vmax = 3.0f; cfg.limits.amax = 25.0f; cfg.limits.jmax = 400.0f;
        Engine e(cfg, 0.5f);
        uint64_t next_cmd = 0;
        int i = 0;
        for (uint64_t t = 0; t <= 2 * kS; t += kMs) {
            if (t >= next_cmd) {
                Command c;
                c.target = 0.5f + 0.35f * std::sin(0.3 * (double)i);
                if (i % 3 == 0) {      // mix modes
                    c.duration_us = 120 * (uint32_t)kMs;
                    c.has_duration = true;
                }
                e.commit(c, t);
                next_cmd += 40 * kMs;
                i++;
            }
            out.push_back(e.positionAt(t));
            out.push_back(e.velocityAt(t));
        }
    };
    std::vector<float> a, b;
    run(a); run(b);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); i++) REQUIRE(a[i] == b[i]);
}

// ============================================================================
// InfeasiblePolicy — which fidelity gets sacrificed when the wire lies
// ============================================================================

TEST_CASE("Scale policy: infeasible segment shrinks its stroke, keeps the deadline") {
    // 0→1 in 100 ms on the real limit set demands ~10 full strokes per second
    // against a 1.1 stroke/s ceiling. Under Scale the engine must still finish
    // ON the 100 ms deadline, having moved as far as the ceilings allowed.
    auto cfg = machineConfig();
    cfg.infeasible_policy = InfeasiblePolicy::Scale;
    Engine e(cfg, 0.0f);

    Command c;
    c.target       = 1.0f;
    c.duration_us  = 100 * (uint32_t)kMs;
    c.has_duration = true;
    REQUIRE(e.commit(c, 0));

    // A scaled plan is still a QUINTIC spanning exactly the commanded time —
    // that is the entire point (the guard would have stretched it instead).
    CHECK(e.planKind() == slopmotion::PlanKind::Quintic);
    CHECK(e.snapshot(0).duration_s == doctest::Approx(0.1).epsilon(0.01));

    // Done on schedule, at rest — not busy a hair past the deadline.
    CHECK(e.isBusy(50 * kMs));
    CHECK_FALSE(e.isBusy(101 * kMs));
    CHECK(e.velocityAt(101 * kMs) == doctest::Approx(0.0).epsilon(1e-4));

    const double landed = e.positionAt(100 * kMs);
    MESSAGE("Scale 0->1 in 100ms: landed at " << landed);
    CHECK(landed > 0.0);        // it did move
    CHECK(landed < 1.0);        // but nowhere near the commanded stroke

    auto scaled = drainFor(e, AnomalyType::WaveformScaled);
    REQUIRE(scaled.seen);
    CHECK(scaled.detail > 0.0f);
    CHECK(scaled.detail < 1.0f);
    // The reported fraction is the truth the operator/WebUI is shown.
    CHECK(scaled.detail == doctest::Approx(landed).epsilon(0.02));
    CHECK(scaled.target == doctest::Approx(landed).epsilon(0.02));
}

TEST_CASE("Stretch policy: same command keeps the stroke and overruns the deadline") {
    auto cfg = machineConfig();
    cfg.infeasible_policy = InfeasiblePolicy::Stretch;
    Engine e(cfg, 0.0f);

    Command c;
    c.target       = 1.0f;
    c.duration_us  = 100 * (uint32_t)kMs;
    c.has_duration = true;
    REQUIRE(e.commit(c, 0));

    // Pre-0.3 behavior, byte for byte: the Ruckig guard owns the segment.
    CHECK(e.planKind() == slopmotion::PlanKind::Ruckig);
    const double dur = e.snapshot(0).duration_s;
    MESSAGE("Stretch 0->1 in 100ms: plan runs " << dur << " s");
    CHECK(dur > 0.9);                   // ≥ distance / vmax = 1/1.1
    CHECK(e.isBusy(400 * kMs));         // still going long after the deadline

    // Full stroke delivered — late.
    CHECK(e.positionAt(2 * kS) == doctest::Approx(1.0).epsilon(1e-3));

    slopmotion::Anomaly ev;
    bool saw_scaled = false, saw_fallback = false, saw_stretch = false;
    while (e.popAnomaly(ev)) {
        if (ev.kind == (uint8_t)AnomalyType::WaveformScaled)    saw_scaled = true;
        if (ev.kind == (uint8_t)AnomalyType::WaveformFallback)  saw_fallback = true;
        if (ev.kind == (uint8_t)AnomalyType::DeadlineStretched) saw_stretch = true;
    }
    CHECK_FALSE(saw_scaled);            // Stretch never scales
    CHECK(saw_fallback);
    CHECK(saw_stretch);
}

TEST_CASE("Feasible segment with NO debt outstanding is bit-identical under ALL THREE policies") {
    // The policy is a fallback branch, not a filter: a segment the quintic can
    // legally execute must be untouched, sample for sample, whichever policy
    // is armed — Reshape included (it must never reach for Ruckig on a segment
    // the quintic can shape honestly).
    //
    // NOTE (0.5.0): "untouched" is now conditional on there being NO CENTERING
    // DEBT. A feasible stroke that follows a clipped one IS deliberately
    // shortened — see the next test for that contract. With a fresh engine
    // there is no debt, so this one still holds exactly as written.
    auto sample = [](InfeasiblePolicy pol, std::vector<float>& out) {
        auto cfg = machineConfig();
        cfg.infeasible_policy = pol;
        Engine e(cfg, 0.2f);
        Command c;
        c.target       = 0.5f;          // comfortably inside every ceiling
        c.duration_us  = 600 * (uint32_t)kMs;
        c.has_duration = true;
        REQUIRE(e.commit(c, 0));
        REQUIRE(e.planKind() == slopmotion::PlanKind::Quintic);
        for (uint64_t t = 0; t <= 700 * kMs; t += kMs) {
            out.push_back(e.positionAt(t));
            out.push_back(e.velocityAt(t));
            out.push_back(e.accelerationAt(t));
        }
        slopmotion::Anomaly ev;
        while (e.popAnomaly(ev)) {
            CHECK(ev.kind != (uint8_t)AnomalyType::WaveformScaled);
            CHECK(ev.kind != (uint8_t)AnomalyType::WaveformFallback);
        }
    };
    std::vector<float> sc, st, rs;
    sample(InfeasiblePolicy::Scale, sc);
    sample(InfeasiblePolicy::Stretch, st);
    sample(InfeasiblePolicy::Reshape, rs);
    REQUIRE(sc.size() == st.size());
    REQUIRE(sc.size() == rs.size());
    for (size_t i = 0; i < sc.size(); i++) REQUIRE(sc[i] == st[i]);
    for (size_t i = 0; i < sc.size(); i++) REQUIRE(sc[i] == rs[i]);
    // And it actually reached the commanded target on the deadline.
    CHECK(sc[(600 * 3)] == doctest::Approx(0.5f).epsilon(1e-4));
}

TEST_CASE("Feasible segment WITH a debt outstanding is shortened — by the debt, and no more") {
    // THE 0.5.0 CONTRACT, and the operator's explicit instruction: "the machine
    // should gracefully handle infeasible input by shortening the stroke,
    // MIDPOINT ANCHORED". The old rule ("a feasible segment is never touched")
    // is exactly what let the band walk off center — the clipped direction lost
    // amplitude while the feasible one kept all of it, so the midpoint sagged
    // and nothing ever pushed it back. Deliberately giving up amplitude on a
    // stroke the machine COULD have finished is now correct behavior.
    //
    // Setup: one infeasible up-stroke (140 mm in 167 ms — the machine cannot),
    // then a long, comfortably feasible down-stroke. The down-stroke is the one
    // under test.
    struct Run { double endpoint; double start; int centered; int scaled;
                 float detail; float anom_target; slopmotion::PlanKind kind; };
    auto run = [](InfeasiblePolicy pol, bool centering) {
        auto cfg = operatorConfig();
        cfg.infeasible_policy = pol;
        cfg.wave_centering    = centering;
        Engine e(cfg, 0.30f);

        Command up;
        up.target = 1.00f; up.duration_us = 167 * (uint32_t)kMs;
        up.has_duration = true;
        REQUIRE(e.commit(up, 0));
        for (uint64_t t = 0; t <= 167 * kMs; t += kMs) e.positionAt(t);
        slopmotion::Anomaly ev;
        while (e.popAnomaly(ev)) {}          // the up-stroke's own report

        Run r{};
        r.start = e.positionAt(167 * kMs);
        Command dn;                          // 600 ms: legal for every policy
        dn.target = 0.30f; dn.duration_us = 600 * (uint32_t)kMs;
        dn.has_duration = true;
        REQUIRE(e.commit(dn, 167 * kMs));
        r.endpoint = e.snapshot(167 * kMs).target;
        r.kind     = e.planKind();
        while (e.popAnomaly(ev)) {
            if (ev.kind == (uint8_t)AnomalyType::WaveformCentered) {
                r.centered++; r.detail = ev.detail; r.anom_target = ev.target;
            }
            if (ev.kind == (uint8_t)AnomalyType::WaveformScaled) r.scaled++;
        }
        return r;
    };

    const Run rs = run(InfeasiblePolicy::Reshape, true);
    const Run sc = run(InfeasiblePolicy::Scale,   true);
    const Run st = run(InfeasiblePolicy::Stretch, true);
    const Run off = run(InfeasiblePolicy::Reshape, false);
    MESSAGE("feasible down-stroke to 0.30 after a clipped up-stroke:"
            << "  reshape+centering " << rs.endpoint
            << "  scale+centering " << sc.endpoint
            << "  stretch " << st.endpoint
            << "  centering OFF " << off.endpoint);

    for (const Run* r : {&rs, &sc}) {
        // Shortened — deliberately, on a stroke the machine could have made.
        CHECK(r->endpoint > 0.30);
        // ...and it is STILL the sender's quintic, on the sender's clock: the
        // centering correction moves the endpoint, it never changes the shape
        // or hands the segment to the guard.
        CHECK(r->kind == slopmotion::PlanKind::Quintic);
        // Bounded: never past the commanded target, never past the start (the
        // pull is capped at half the stroke), never inverted.
        CHECK(r->endpoint < r->start);
        CHECK(r->endpoint <= 0.30 + 0.5 * (r->start - 0.30) + 1e-9);
        // NOT silent — one WaveformCentered, carrying the achieved fraction and
        // the shortened endpoint, and NOT reported as WaveformScaled (the
        // machine was not the constraint here; the midpoint was).
        CHECK(r->centered == 1);
        CHECK(r->scaled == 0);
        CHECK(r->anom_target == doctest::Approx((float)r->endpoint).epsilon(1e-4));
        const double frac = (r->start - r->endpoint) / (r->start - 0.30);
        CHECK(r->detail == doctest::Approx((float)frac).epsilon(0.01));
        CHECK(r->detail < 1.0f);
    }
    // Stretch keeps its promise ("the stroke you asked for") and centering is
    // never armed for it; the old behavior is one config flag away.
    CHECK(st.endpoint == doctest::Approx(0.30).epsilon(1e-6));
    CHECK(st.centered == 0);
    CHECK(off.endpoint == doctest::Approx(0.30).epsilon(1e-6));
    CHECK(off.centered == 0);
}

TEST_CASE("All three policies keep the sampled window invariant [0,1]") {
    // The scaled path builds a NEW quintic that never went through commit()'s
    // target clamp, and the reshaped path hands Ruckig — which has NO position
    // limits — an endpoint it chose itself, so prove the window still holds
    // under a full-stroke segment chain that is infeasible in both directions.
    for (auto pol : {InfeasiblePolicy::Scale, InfeasiblePolicy::Stretch,
                     InfeasiblePolicy::Reshape}) {
        auto cfg = machineConfig();
        cfg.infeasible_policy = pol;
        Engine e(cfg, 0.5f);

        const uint64_t seg = 250 * kMs;
        uint64_t next_cmd = 0;
        int i = 0;
        double min_p = 1e9, max_p = -1e9;
        for (uint64_t t = 0; t <= 6 * kS; t += kMs) {
            if (t >= next_cmd) {
                Command c;
                c.target       = (i % 2) ? 1.0f : 0.0f;   // rail to rail
                c.duration_us  = (uint32_t)seg;
                c.has_duration = true;
                c.end_vel      = (i % 2) ? 0.8f : -0.8f;  // and still moving
                c.has_end_vel  = true;
                e.commit(c, next_cmd);
                next_cmd += seg;
                i++;
            }
            const double p = e.positionAt(t);
            const double v = e.velocityAt(t);
            REQUIRE(std::isfinite(p));
            REQUIRE(p >= -1e-9);
            REQUIRE(p <= 1.0 + 1e-9);
            CHECK(std::fabs(v) <= cfg.limits.vmax * 1.001);
            min_p = std::min(min_p, p); max_p = std::max(max_p, p);
        }
        MESSAGE("policy " << (int)pol << ": excursion ["
                          << min_p << ", " << max_p << "]");
    }
}

TEST_CASE("Scale vs Stretch on the measured 400 ms full-stroke chain") {
    // The defect case, verbatim: a scheduled sender pushing 0→1→0 at 400 ms
    // per segment (2344 mm/s demanded against a 550 mm/s machine). Under
    // Stretch the guard's 0.9 s plan is PREEMPTED by the next segment every
    // time, so the machine free-runs — it never lands on a commanded target
    // and its reversals drift off the sender's clock. Under Scale each segment
    // is a self-contained quintic that starts and ends ON the beat.
    struct Run { double amp; double phase_err; };
    auto run = [](InfeasiblePolicy pol) {
        auto cfg = machineConfig();
        cfg.infeasible_policy = pol;
        Engine e(cfg, 0.5f);
        const uint64_t seg = 400 * kMs;
        uint64_t next_cmd = 0;
        int i = 0;
        double min_p = 1e9, max_p = -1e9;
        // Phase: how far the sampled velocity zero-crossings (the reversals)
        // sit from the segment boundaries the sender scheduled.
        double worst_phase = 0.0;
        double prev_v = 0.0;
        for (uint64_t t = 0; t <= 8 * kS; t += kMs) {
            if (t >= next_cmd) {
                Command c;
                c.target       = (i % 2) ? 1.0f : 0.0f;
                c.duration_us  = (uint32_t)seg;
                c.has_duration = true;
                e.commit(c, next_cmd);
                next_cmd += seg;
                i++;
            }
            const double p = e.positionAt(t);
            const double v = e.velocityAt(t);
            if (t > 2 * kS) {           // steady state only
                min_p = std::min(min_p, p); max_p = std::max(max_p, p);
                if (prev_v != 0.0 && ((prev_v < 0) != (v < 0))) {
                    const double ph = (double)(t % seg) * 1e-6;
                    worst_phase = std::max(worst_phase,
                                           std::min(ph, 0.4 - ph));
                }
            }
            prev_v = v;
        }
        return Run{max_p - min_p, worst_phase};
    };
    const Run sc = run(InfeasiblePolicy::Scale);
    const Run st = run(InfeasiblePolicy::Stretch);
    MESSAGE("400ms chain  Scale:   amplitude " << sc.amp
            << "  worst reversal phase error " << sc.phase_err << " s");
    MESSAGE("400ms chain  Stretch: amplitude " << st.amp
            << "  worst reversal phase error " << st.phase_err << " s");

    // Both must stay honest about the window and actually move.
    CHECK(sc.amp > 0.0);
    CHECK(st.amp > 0.0);
    // Scale's reversals land on the sender's beat; that is what it bought.
    CHECK(sc.phase_err <= 0.030);
    CHECK(sc.phase_err < st.phase_err);
}

TEST_CASE("Scale sizing is quintic-exact: first attempt, near the legal maximum") {
    // The sizing closed form is the quintic's own peak/mean ratios inverted
    // against each ceiling:
    //     D_max = min(vmax·T/1.875, amax·T²/5.7735, jmax·T³/60) · margin
    // Rest-to-rest (which is what the form models), the FIRST attempt must be
    // accepted — no retry ladder — and the achieved stroke must sit at exactly
    // `margin` of the largest legal quintic stroke found by bisection against
    // the engine's own legality scan.
    const auto cfg0 = machineConfig();
    const double vc = cfg0.limits.vmax, ac = cfg0.limits.amax, jc = cfg0.limits.jmax;
    const double margin = cfg0.infeasible_scale_margin;

    for (double T : {0.10, 0.20, 0.40, 0.60}) {
        auto cfg = machineConfig();
        cfg.infeasible_policy = InfeasiblePolicy::Scale;
        Engine e(cfg, 0.0f);                 // AT REST at the bottom rail

        Command c;
        c.target       = 1.0f;               // full stroke: always infeasible here
        c.duration_us  = (uint32_t)(T * 1e6 + 0.5);
        c.has_duration = true;
        REQUIRE(e.commit(c, 0));
        REQUIRE(e.planKind() == slopmotion::PlanKind::Quintic);

        const double landed = e.positionAt((uint64_t)(T * 1e6 + 0.5));

        // Closed form, recomputed independently of the engine.
        const double closed =
            std::min(std::min(vc * T / 1.875, ac * T * T / 5.7735027),
                     jc * T * T * T / 60.0);

        // Largest stroke the ENGINE's legality scan would actually accept,
        // by bisection — the ground truth the closed form is approximating.
        auto legal = [&](double d) {
            auto cf = machineConfig();
            cf.infeasible_policy = InfeasiblePolicy::Scale;
            Engine probe(cf, 0.0f);
            Command pc;
            pc.target       = (float)d;
            pc.duration_us  = (uint32_t)(T * 1e6 + 0.5);
            pc.has_duration = true;
            probe.commit(pc, 0);
            // Unscaled acceptance = the quintic was legal as commanded.
            slopmotion::Anomaly ev;
            bool touched = false;
            while (probe.popAnomaly(ev)) {
                if (ev.kind == (uint8_t)AnomalyType::WaveformScaled ||
                    ev.kind == (uint8_t)AnomalyType::WaveformFallback) touched = true;
            }
            return !touched && probe.planKind() == slopmotion::PlanKind::Quintic;
        };
        double lo = 0.0, hi = 1.0;
        for (int i = 0; i < 40; i++) {
            const double mid = 0.5 * (lo + hi);
            if (legal(mid)) lo = mid; else hi = mid;
        }
        const double largest_legal = lo;

        MESSAGE("T=" << T << "s  closed-form " << closed
                     << "  achieved " << landed
                     << "  largest-legal " << largest_legal
                     << "  achieved/legal " << (landed / largest_legal));

        // Attempt 1 landed: achieved == closed form × margin, exactly.
        CHECK(landed == doctest::Approx(closed * margin).epsilon(1e-3));
        // And the closed form is a genuine (slightly conservative) estimate of
        // the scan's own boundary — within 5 %.
        CHECK(closed <= largest_legal * 1.05);
        CHECK(closed >= largest_legal * 0.90);
        // The delivered stroke is within the margin of everything achievable.
        CHECK(landed >= largest_legal * (margin - 0.06));
    }
}

TEST_CASE("Quintic sizing beats the old trapezoid envelope on delivered stroke") {
    // Regression guard on the whole point of change 1: the trapezoid envelope
    // + 0.75 ladder converged from ABOVE and always landed short. Compare the
    // shipped sizing against a faithful replay of the old first-guess ladder.
    for (double T : {0.10, 0.20, 0.40, 0.60}) {
        auto cfg = machineConfig();
        cfg.infeasible_policy = InfeasiblePolicy::Scale;
        Engine e(cfg, 0.0f);
        Command c;
        c.target = 1.0f; c.duration_us = (uint32_t)(T * 1e6 + 0.5);
        c.has_duration = true;
        REQUIRE(e.commit(c, 0));
        const double now = e.positionAt((uint64_t)(T * 1e6 + 0.5));

        // Old envelope: min(amax·T²/4, vmax·T − vmax²/amax) · margin, then
        // ×0.75 until the scan accepts (that ladder is what shipped).
        const double vc = cfg.limits.vmax, ac = cfg.limits.amax;
        double old_d = 0.25 * ac * T * T;
        const double trap = vc * T - vc * vc / ac;
        if (trap > 0.0) old_d = std::min(old_d, trap);
        old_d *= cfg.infeasible_scale_margin;
        double old_landed = 0.0;
        for (int i = 0; i < 6; i++) {
            auto cf = machineConfig();
            Engine probe(cf, 0.0f);
            Command pc;
            pc.target = (float)std::min(1.0, old_d);
            pc.duration_us = (uint32_t)(T * 1e6 + 0.5);
            pc.has_duration = true;
            probe.commit(pc, 0);
            slopmotion::Anomaly ev; bool touched = false;
            while (probe.popAnomaly(ev)) {
                if (ev.kind == (uint8_t)AnomalyType::WaveformScaled ||
                    ev.kind == (uint8_t)AnomalyType::WaveformFallback) touched = true;
            }
            if (!touched) { old_landed = std::min(1.0, old_d); break; }
            old_d *= 0.75;
        }
        MESSAGE("T=" << T << "s  new sizing " << now
                     << "  old trapezoid ladder " << old_landed
                     << "  gain " << (now / std::max(old_landed, 1e-9)) << "x");
        CHECK(now > old_landed);
    }
}

TEST_CASE("Scale now fires on moves the trapezoid envelope called feasible") {
    // The silent under-firing the old sizing caused: 0→1 in 900 ms on the
    // test limit set (vmax 2, amax 20, jmax 300) needs a quintic peak velocity
    // of 1.875/0.9 = 2.08 > 2.0, so it IS infeasible — but the trapezoid
    // envelope sized the reachable stroke at 1.47 (> the 1.0 requested), so
    // commitWaveformScaled returned false without trying and the Ruckig guard
    // silently overran the deadline. Under Scale that must now be a scaled
    // quintic that lands ON the 900 ms deadline.
    auto cfg = testConfig();
    cfg.infeasible_policy = InfeasiblePolicy::Scale;
    Engine e(cfg, 0.0f);

    Command c;
    c.target = 1.0f; c.duration_us = 900 * (uint32_t)kMs; c.has_duration = true;
    REQUIRE(e.commit(c, 0));
    CHECK(e.planKind() == slopmotion::PlanKind::Quintic);
    CHECK(e.snapshot(0).duration_s == doctest::Approx(0.9).epsilon(0.01));
    CHECK_FALSE(e.isBusy(905 * kMs));

    auto scaled = drainFor(e, AnomalyType::WaveformScaled);
    REQUIRE(scaled.seen);
    // vmax·T/1.875 · margin = 2·0.9/1.875 · 0.92 = 0.8832 — a 12 % haircut
    // instead of a 100 ms deadline overrun.
    CHECK(e.positionAt(900 * kMs) == doctest::Approx(0.8832).epsilon(0.01));
    CHECK(scaled.detail == doctest::Approx(0.8832f).epsilon(0.01));
}

TEST_CASE("Moving start still resolves: the shortened ladder is enough") {
    // The closed form models a REST-to-rest stroke. A segment arriving with
    // the carriage already moving fast the wrong way can fail the scan at the
    // closed-form size — that is the only remaining job of the ladder.
    // Sweep a chain of infeasible reversals and prove every one of them still
    // gets a deadline-honoring quintic (or an honest guard fallback), never a
    // window violation.
    auto cfg = machineConfig();
    cfg.infeasible_policy = InfeasiblePolicy::Scale;
    Engine e(cfg, 0.5f);

    const uint64_t seg = 150 * kMs;   // hard infeasible at 1.1 stroke/s
    uint64_t next_cmd = 0;
    int i = 0, scaled = 0, fallback = 0;
    for (uint64_t t = 0; t <= 5 * kS; t += kMs) {
        if (t >= next_cmd) {
            Command c;
            c.target       = (i % 2) ? 1.0f : 0.0f;
            c.duration_us  = (uint32_t)seg;
            c.has_duration = true;
            REQUIRE(e.commit(c, next_cmd));
            next_cmd += seg;
            i++;
            slopmotion::Anomaly ev;
            while (e.popAnomaly(ev)) {
                if (ev.kind == (uint8_t)AnomalyType::WaveformScaled)   scaled++;
                if (ev.kind == (uint8_t)AnomalyType::WaveformFallback) fallback++;
            }
        }
        const double p = e.positionAt(t);
        const double v = e.velocityAt(t);
        REQUIRE(std::isfinite(p));
        REQUIRE(p >= -1e-9);
        REQUIRE(p <= 1.0 + 1e-9);
        CHECK(std::fabs(v) <= cfg.limits.vmax * 1.001);
    }
    MESSAGE("150ms reversal chain: " << scaled << " scaled, "
                                     << fallback << " guard fallbacks of " << i);
    CHECK(scaled > 0);
    // The ladder must carry the moving-start cases — the guard should be rare.
    CHECK(fallback * 4 < i);
}

// ============================================================================
// Second-order predictive aim
// ============================================================================

TEST_CASE("Second-order chase aim stops overshooting a crest near the rail") {
    // A sine cresting just under the top rail is the shape that exposed the
    // linear aim: at the crest the velocity EMA still reads the pre-crest
    // climb, so aim = target + v_est·look is thrown PAST the crest, clamps at
    // the wall, and the end-vel guard then parks the carriage there. The
    // ½·a_est·look² term is negative through a crest and should pull it back.
    const double base = 0.55, amp = 0.40, f = 0.75;   // crest at 0.95
    const double crest = base + amp;

    struct Result { double peak; double rail_ms; };
    auto run = [&](bool extrap) {
        Config cfg;
        cfg.limits.vmax = 3.0f;
        cfg.limits.amax = 30.0f;
        cfg.limits.jmax = 500.0f;
        cfg.chase_aim_accel_extrap = extrap;
        Engine e(cfg, (float)base);
        auto src = [&](double t) { return base + amp * std::sin(2*M_PI*f*t); };

        double peak = -1e9;
        int near_crest_ms = 0;
        uint64_t next_cmd = 0;
        for (uint64_t t = 0; t <= 4 * kS; t += kMs) {
            if (t >= next_cmd) {
                Command c;
                c.target = (float)src((double)t * 1e-6);
                e.commit(c, t);
                next_cmd += 16667;      // ~60 Hz bare point stream
            }
            const double p = e.positionAt(t);
            REQUIRE(p >= -1e-9);
            REQUIRE(p <= 1.0 + 1e-9);
            if (t > 700 * kMs) {        // past the catch-up transient
                peak = std::max(peak, p);
                if (p > crest) near_crest_ms++;   // time spent ABOVE the crest
            }
        }
        return Result{peak, (double)near_crest_ms};
    };

    const Result on  = run(true);
    const Result off = run(false);
    MESSAGE("crest " << crest << "  2nd-order peak " << on.peak
            << " (over-crest " << on.rail_ms << " ms)   linear peak "
            << off.peak << " (over-crest " << off.rail_ms << " ms)");

    // Measured here: linear aim peaks 0.0381 above the crest, second-order
    // 0.0256 — the accel term removes a third of the overshoot. The RESIDUAL
    // is not aim error, it is chase LAG (the tracker is still climbing when
    // the source turns, ~35 ms of estimator smoothing at peak velocity — the
    // known part-1 residual). This bound is a regression tripwire on that
    // combined figure, not a quality target.
    CHECK(on.peak <= crest + 0.030);
    // Turning the term off measurably re-introduces aim overshoot on top.
    CHECK(off.peak > on.peak + 0.005);
    CHECK(off.rail_ms > on.rail_ms);
}

TEST_CASE("Second-order aim shortens the dead-stop park at the rail") {
    // The costly form of the same defect: when the source crest sits ON the
    // top rail the overshooting aim CLAMPS to 1.0, applyEndVelGuard then sees
    // dist-to-wall = 0 and forces vf = 0, and the carriage sits at the rail at
    // a dead stop (measured 127–138 ms per stroke against the source sine's
    // own 0.35 % tangency dwell). Second-order aim should cut the park time.
    const double base = 0.60, amp = 0.40, f = 0.75;   // crest exactly at 1.0

    auto park_ms = [&](bool extrap) {
        Config cfg;
        cfg.limits.vmax = 3.0f;
        cfg.limits.amax = 30.0f;
        cfg.limits.jmax = 500.0f;
        cfg.chase_aim_accel_extrap = extrap;
        Engine e(cfg, (float)base);
        auto src = [&](double t) { return base + amp * std::sin(2*M_PI*f*t); };

        int parked = 0;
        uint64_t next_cmd = 0;
        for (uint64_t t = 0; t <= 4 * kS; t += kMs) {
            if (t >= next_cmd) {
                Command c;
                c.target = (float)src((double)t * 1e-6);
                e.commit(c, t);
                next_cmd += 16667;
            }
            const double p = e.positionAt(t);
            const double v = e.velocityAt(t);
            REQUIRE(p >= -1e-9);
            REQUIRE(p <= 1.0 + 1e-9);
            // "Parked": pinned at the wall with essentially no motion left.
            if (t > 700 * kMs && p > 0.999 && std::fabs(v) < 0.05) parked++;
        }
        return parked;
    };
    const int on  = park_ms(true);
    const int off = park_ms(false);
    MESSAGE("rail park: 2nd-order " << on << " ms   linear " << off << " ms");
    CHECK(on < off);
}

TEST_CASE("Predictive aim v2 arrives at the velocity the stream will HAVE") {
    // The aim/arrival-velocity consistency fix. v1 aimed `look` seconds ahead
    // but requested v_est(NOW) as the arrival velocity — so through a crest,
    // where the stream is decelerating, every plan was told to arrive at the
    // pre-crest climb speed and duly overshot. v2 extrapolates the arrival
    // velocity over the same horizon (v_est + a_est·look).
    //
    // Measured as tracking error against the source sine, which is where the
    // inconsistency actually shows up: a plan that arrives too fast is a plan
    // that is in the wrong place one interval later.
    const double base = 0.55, amp = 0.40, f = 0.75;
    struct Track { double rms; double peak_err; double max_pos; };
    auto run = [&](bool v2) {
        Config cfg;
        cfg.limits.vmax = 3.0f;
        cfg.limits.amax = 30.0f;
        cfg.limits.jmax = 500.0f;
        cfg.chase_aim_accel_extrap = v2;
        Engine e(cfg, (float)base);
        auto src = [&](double t) { return base + amp * std::sin(2*M_PI*f*t); };

        double sq = 0.0, peak = 0.0, maxp = -1e9;
        int n = 0;
        uint64_t next_cmd = 0;
        for (uint64_t t = 0; t <= 4 * kS; t += kMs) {
            if (t >= next_cmd) {
                Command c;
                c.target = (float)src((double)t * 1e-6);
                e.commit(c, t);
                next_cmd += 16667;      // ~60 Hz bare point stream
            }
            const double p = e.positionAt(t);
            REQUIRE(p >= -1e-9);
            REQUIRE(p <= 1.0 + 1e-9);
            if (t > 700 * kMs) {
                const double err = std::fabs(p - src((double)t * 1e-6));
                sq += err * err; peak = std::max(peak, err); n++;
                maxp = std::max(maxp, p);
            }
        }
        return Track{std::sqrt(sq / n), peak, maxp};
    };
    const Track v2 = run(true);
    const Track v1 = run(false);
    MESSAGE("sine track  v2 rms " << v2.rms << " peak " << v2.peak_err
            << " max-pos " << v2.max_pos
            << "   |  v1 rms " << v1.rms << " peak " << v1.peak_err
            << " max-pos " << v1.max_pos);
    // v2 must not be worse on either figure, and must strictly cut the
    // overshoot past the source crest (base+amp = 0.95).
    CHECK(v2.rms <= v1.rms);
    CHECK(v2.max_pos < v1.max_pos);
}

// ============================================================================
// InfeasiblePolicy::Reshape — size the stroke to the MACHINE, not to a shape
// ============================================================================

TEST_CASE("Reshape: a stroke the machine CAN make keeps its full amplitude") {
    // The measured 80 mm / 133 ms funscript segment on the operator's machine.
    // The quintic is infeasible (peak v = 1.875·0.40/0.133 = 5.64 > 5.0), but
    // the MACHINE reaches 80 mm in ~125 ms flat-top — inside the deadline. So
    // Reshape must deliver the WHOLE stroke, on the commanded deadline, and
    // report only that the shape changed (WaveformFallback, no WaveformScaled).
    auto cfg = operatorConfig();
    cfg.infeasible_policy = InfeasiblePolicy::Reshape;
    Engine e(cfg, 0.80f);

    Command c;
    c.target       = 0.40f;                  // 80 mm down-stroke
    c.duration_us  = 133 * (uint32_t)kMs;
    c.has_duration = true;
    REQUIRE(e.commit(c, 0));

    CHECK(e.planKind() == slopmotion::PlanKind::Ruckig);   // shape given up
    CHECK(e.snapshot(0).duration_s == doctest::Approx(0.133).epsilon(0.02));
    const double landed = e.positionAt(133 * kMs);
    const double travel_mm = std::fabs(0.80 - landed) * kSpanMm;

    // Same segment under Scale, for the number that motivated all of this.
    auto scfg = operatorConfig();
    scfg.infeasible_policy = InfeasiblePolicy::Scale;
    Engine se(scfg, 0.80f);
    REQUIRE(se.commit(c, 0));
    const double s_travel_mm =
        std::fabs(0.80 - se.positionAt(133 * kMs)) * kSpanMm;

    MESSAGE("80mm/133ms  Reshape " << travel_mm << " mm   Scale "
                                   << s_travel_mm << " mm");
    CHECK(landed == doctest::Approx(0.40).epsilon(1e-3));   // full amplitude
    CHECK(travel_mm > 79.0);
    CHECK(travel_mm > s_travel_mm * 1.15);                  // materially better
    CHECK_FALSE(e.isBusy(140 * kMs));                       // done ON the deadline

    // Shape lost, nothing else: fallback recorded, amplitude NOT scaled, and
    // no DeadlineStretched (min_dur = T holds the schedule by construction).
    auto fb  = drainFor(e, AnomalyType::WaveformFallback);
    CHECK(fb.seen);
    slopmotion::Anomaly ev;      // ring already drained; re-run for the rest
    Engine e2(cfg, 0.80f);
    REQUIRE(e2.commit(c, 0));
    bool scaled = false, stretched = false;
    while (e2.popAnomaly(ev)) {
        if (ev.kind == (uint8_t)AnomalyType::WaveformScaled)    scaled = true;
        if (ev.kind == (uint8_t)AnomalyType::DeadlineStretched) stretched = true;
    }
    CHECK_FALSE(scaled);
    CHECK_FALSE(stretched);
}

TEST_CASE("Reshape: an impossible stroke shrinks to the machine's real reach") {
    // The measured 140 mm / 167 ms segment. Time-optimal for the full stroke is
    // ~185 ms > 167 ms, so this one genuinely cannot be delivered whole — but
    // the mean speed it asks for is only 838 mm/s against a 1000 mm/s ceiling,
    // so the honest answer is ~122 mm, not the 82 mm the quintic envelope
    // permits. Reshape bisects to the machine's actual reach; Scale does not.
    auto cfg = operatorConfig();
    cfg.infeasible_policy = InfeasiblePolicy::Reshape;
    Engine e(cfg, 0.30f);

    Command c;
    c.target       = 1.00f;                  // 140 mm up-stroke
    c.duration_us  = 167 * (uint32_t)kMs;
    c.has_duration = true;
    REQUIRE(e.commit(c, 0));
    CHECK(e.planKind() == slopmotion::PlanKind::Ruckig);
    CHECK(e.snapshot(0).duration_s == doctest::Approx(0.167).epsilon(0.02));

    // Peak displacement lands ON the deadline, not before and not after.
    double peak = -1e9; uint64_t peak_t = 0;
    for (uint64_t t = 0; t <= 400 * kMs; t += kMs) {
        const double p = e.positionAt(t);
        REQUIRE(p >= -1e-9);
        REQUIRE(p <= 1.0 + 1e-9);
        if (p > peak) { peak = p; peak_t = t; }
    }
    const double travel_mm = (peak - 0.30) * kSpanMm;

    auto scfg = operatorConfig();
    scfg.infeasible_policy = InfeasiblePolicy::Scale;
    Engine se(scfg, 0.30f);
    REQUIRE(se.commit(c, 0));
    const double s_travel_mm = (se.positionAt(167 * kMs) - 0.30) * kSpanMm;

    MESSAGE("140mm/167ms  Reshape " << travel_mm << " mm (peak at "
            << (peak_t / 1000) << " ms)   Scale " << s_travel_mm << " mm");

    CHECK(travel_mm > 115.0);            // the machine's honest reach, near 122
    CHECK(travel_mm < 141.0);            // never more than commanded
    CHECK(s_travel_mm < 85.0);           // the quintic envelope's 82 mm
    CHECK(travel_mm > s_travel_mm * 1.4);
    CHECK(peak_t >= 160 * kMs);          // ON TIME: peak at the deadline
    CHECK(peak_t <= 175 * kMs);

    auto sc = drainFor(e, AnomalyType::WaveformScaled);
    REQUIRE(sc.seen);
    // The reported fraction is the truth the operator/WebUI is shown.
    CHECK(sc.detail == doctest::Approx(travel_mm / 140.0).epsilon(0.02));
    CHECK(sc.target == doctest::Approx(peak).epsilon(0.01));
}

// ============================================================================
// DC centering — the band shrinks about the commanded MIDPOINT
// ============================================================================

TEST_CASE("Both directions infeasible: the degraded band sits on the commanded midpoint") {
    // A greedy shrink is neutrally stable in DC: every segment travels as far
    // as it can, the unreachable extreme gets clipped, the (now shorter)
    // return trip fits and lands exactly — so the whole waveform hangs off one
    // extreme. The reversal-debt rule hands part of each shortfall back at the
    // opposite extreme. This was the 0.4.0 result (+0.7 mm) and it must not
    // regress now that the same debt also spends itself on feasible strokes.
    ChainOpts o;                       // 0.30 <-> 1.00 @167 ms, no wire vf
    o.pol = InfeasiblePolicy::Reshape;
    const Band on  = runChain(o);
    o.centering = false;
    const Band off = runChain(o);
    o.pol = InfeasiblePolicy::Scale; o.centering = true;
    const Band s_on  = runChain(o);
    o.centering = false;
    const Band s_off = runChain(o);
    reportBand("both-infeasible  reshape + centering", on);
    reportBand("both-infeasible  reshape centering OFF", off);
    reportBand("both-infeasible  scale + centering", s_on);
    reportBand("both-infeasible  scale centering OFF", s_off);

    CHECK(std::fabs(on.center_err_mm) < 3.0);      // measured +0.16 mm
    CHECK(on.center_spread_mm < 2.0);              // settled, not orbiting
    CHECK(on.bounds_ok);
    // Reshape's amplitude advantage over Scale survives the centering work.
    CHECK(on.amp_mm > s_on.amp_mm * 1.3);
    // Scale is centered now too (0.5.0 hoisted the debt out of Reshape) — and
    // that is the whole point: -29 mm was Scale's measured sag before.
    CHECK(std::fabs(s_on.center_err_mm) < 3.0);
    CHECK(std::fabs(s_off.center_err_mm) > 20.0);
    CHECK(s_on.centered > 0);
    // Centering off restores the 0.4.0 TELEMETRY too: its own reversal debt
    // reports as WaveformScaled, exactly as it always did. A kind the
    // firmware/sim counter tables do not know yet must not appear when the
    // feature is switched off.
    CHECK(s_off.centered == 0);
    CHECK(off.centered == 0);
    // Centering costs amplitude only at the margins, never a collapse.
    CHECK(on.amp_mm > off.amp_mm * 0.95);
    CHECK(s_on.amp_mm > s_off.amp_mm * 0.95);
}

TEST_CASE("Mixed feasible/infeasible chain settles centered and STAYS there") {
    // THE OPERATOR'S REAL CASE. 0.30 <-> 1.00 at 167 ms with wire end
    // velocities: the down-strokes come out quintic-feasible, the up-strokes do
    // not. Under the old "feasible segments are untouched" rule the feasible
    // direction kept its full amplitude while the other was clipped, so the
    // band walked off center and never came back — measured 26.8 mm low on the
    // operator's machine, 23.6 mm here.
    ChainOpts o;
    o.ev_down = true;
    const Band rs  = runChain(o);
    o.centering = false;
    const Band rs_off = runChain(o);
    o.centering = true; o.gain = 0.5f;
    const Band rs_half = runChain(o);
    o.gain = 1.0f; o.pol = InfeasiblePolicy::Scale;
    const Band sc = runChain(o);
    o.centering = false;
    const Band sc_off = runChain(o);
    reportBand("mixed  reshape + centering", rs);
    reportBand("mixed  reshape centering OFF", rs_off);
    reportBand("mixed  reshape gain 0.5", rs_half);
    reportBand("mixed  scale + centering", sc);
    reportBand("mixed  scale centering OFF", sc_off);

    // The defect: uncorrected, the band hangs off its bottom extreme.
    CHECK(rs_off.center_err_mm < -15.0);
    CHECK(sc_off.center_err_mm < -30.0);
    // Fixed — and by a factor of five, not a nudge. (The residual is the
    // sender's OWN doing: it asked to still be moving at the bottom of each
    // stroke, so the carriage dips past the planned endpoint on the turn. The
    // engine centers what it controls — the endpoints — which land within
    // ~1 mm; the dip rides on top, exactly as commanded.)
    CHECK(std::fabs(rs.center_err_mm) < 8.0);
    CHECK(std::fabs(rs.center_err_mm) * 4.0 < std::fabs(rs_off.center_err_mm));
    CHECK(std::fabs(sc.center_err_mm) * 4.0 < std::fabs(sc_off.center_err_mm));
    // SETTLED, not hunting: the loop's undamped form parked in a stable
    // period-4 orbit swinging the center ±7 mm with a perfect mean.
    CHECK(rs.center_spread_mm < 2.0);
    CHECK(rs.amp_spread_mm    < 3.0);
    CHECK(sc.center_spread_mm < 2.0);
    CHECK(sc.amp_spread_mm    < 3.0);
    // Amplitude is not the price: centering took none of it here.
    CHECK(rs.amp_mm >= rs_off.amp_mm);
    CHECK(sc.amp_mm >= sc_off.amp_mm * 0.98);
    // The deviation is visible, on every stroke it happens to.
    CHECK(rs.centered > 10);
    CHECK(sc.centered > 10);
    CHECK(rs_off.centered == 0);
    CHECK(sc_off.centered == 0);
    CHECK(rs.bounds_ok);
    CHECK(sc.bounds_ok);
    // The strength dial does something in between — but NOT monotonically
    // (the loop closes around the pull it applied), which is why it is
    // documented as a feel dial and 1.0 is the default.
    CHECK(std::fabs(rs_half.center_err_mm) < std::fabs(rs_off.center_err_mm));
}

TEST_CASE("Centering holds at half machine speed (the 500 mm/s bench case)") {
    // The operator halved the speed ceiling and Reshape started rendering
    // near-linear (velocity-saturated) strokes — correct, and the reason Scale
    // became interesting again. Both policies must stay centered there too.
    ChainOpts o;
    o.vmax = 2.5f;                       // 500 mm/s on the 200 mm window
    const Band both = runChain(o);
    o.centering = false;
    const Band both_off = runChain(o);
    o.centering = true; o.ev_down = true;
    const Band mixed = runChain(o);
    o.centering = false;
    const Band mixed_off = runChain(o);
    o.centering = true; o.ev_down = false; o.pol = InfeasiblePolicy::Scale;
    const Band s_both = runChain(o);
    o.centering = false;
    const Band s_both_off = runChain(o);
    reportBand("500 mm/s  both-infeasible reshape + centering", both);
    reportBand("500 mm/s  both-infeasible reshape OFF", both_off);
    reportBand("500 mm/s  mixed reshape + centering", mixed);
    reportBand("500 mm/s  mixed reshape OFF", mixed_off);
    reportBand("500 mm/s  both-infeasible scale + centering", s_both);
    reportBand("500 mm/s  both-infeasible scale OFF", s_both_off);

    CHECK(std::fabs(both.center_err_mm) < 3.0);
    CHECK(std::fabs(mixed.center_err_mm) < 6.0);
    CHECK(both.center_spread_mm < 2.0);
    CHECK(mixed.center_spread_mm < 2.0);
    CHECK(std::fabs(both.center_err_mm)  < std::fabs(both_off.center_err_mm));
    CHECK(std::fabs(mixed.center_err_mm) < std::fabs(mixed_off.center_err_mm));
    CHECK(both_off.centered == 0);        // OFF means off, telemetry included
    CHECK(mixed_off.centered == 0);
    CHECK(s_both_off.centered == 0);
    // Scale at half speed: still a big improvement, though this limit set
    // leaves it further off than Reshape (its envelope binds much harder).
    CHECK(std::fabs(s_both.center_err_mm) * 3.0 <
          std::fabs(s_both_off.center_err_mm));
}

TEST_CASE("Stretch is untouched by the centering work, sample for sample") {
    // Stretch delivers the full commanded amplitude (late) by definition, so
    // there is never a deficit to share out — centering must not even arm for
    // it. Prove it the only way that means anything: identical samples with the
    // flag on and off, across a chain that hammers the guard.
    auto sample = [](bool centering, std::vector<float>& out) {
        auto cfg = operatorConfig();
        cfg.infeasible_policy = InfeasiblePolicy::Stretch;
        cfg.wave_centering    = centering;
        Engine e(cfg, 0.30f);
        const uint64_t seg = 167 * kMs;
        uint64_t next_cmd = 0;
        for (int i = 0; i < 24; i++) {
            Command c;
            c.target       = (i % 2) ? 1.00f : 0.30f;
            c.duration_us  = (uint32_t)seg;
            c.has_duration = true;
            e.commit(c, next_cmd);
            for (uint64_t t = next_cmd; t < next_cmd + seg; t += kMs) {
                out.push_back(e.positionAt(t));
                out.push_back(e.velocityAt(t));
            }
            next_cmd += seg;
            slopmotion::Anomaly ev;
            while (e.popAnomaly(ev)) {
                CHECK(ev.kind != (uint8_t)AnomalyType::WaveformCentered);
            }
        }
    };
    std::vector<float> on, off;
    sample(true, on);
    sample(false, off);
    REQUIRE(on.size() == off.size());
    for (size_t i = 0; i < on.size(); i++) REQUIRE(on[i] == off[i]);
}

TEST_CASE("The centering debt lets go when the machine stops being the constraint") {
    // The failure mode a self-referential debt would have: a stroke shortened
    // for symmetry reports a shortfall, which justifies shortening the next
    // one, and the band ratchets shut forever. It must instead re-open as soon
    // as the content stops asking for the impossible — the release valve is
    // scaled by how much room each segment turns out to have.
    for (auto pol : {InfeasiblePolicy::Reshape, InfeasiblePolicy::Scale}) {
        auto cfg = operatorConfig();
        cfg.infeasible_policy = pol;
        Engine e(cfg, 0.30f);
        uint64_t now = 0;
        int i = 0;
        auto stroke = [&](uint32_t seg_ms, bool ev) {
            const bool up = (i % 2) != 0;
            Command c;
            c.target       = up ? 1.00f : 0.30f;
            c.duration_us  = seg_ms * (uint32_t)kMs;
            c.has_duration = true;
            if (ev && !up) { c.end_vel = -2.6f; c.has_end_vel = true; }
            e.commit(c, now);
            const double ep = e.snapshot(now).target;
            for (uint64_t t = now; t < now + seg_ms * kMs; t += kMs) e.positionAt(t);
            now += (uint64_t)seg_ms * kMs;
            i++;
            slopmotion::Anomaly a;
            while (e.popAnomaly(a)) {}
            return ep;
        };
        for (int k = 0; k < 24; k++) stroke(167, true);   // converge shortened
        int restored = -1;
        for (int k = 0; k < 14 && restored < 0; k++) {
            const double ep = stroke(600, false);          // now it is easy
            const double miss_mm =
                std::fabs(((i % 2) ? 0.30 : 1.00) - ep) * kSpanMm;
            if (miss_mm < 1.0) restored = k;
        }
        MESSAGE("policy " << (int)pol << ": full amplitude restored after "
                          << restored << " easy strokes");
        CHECK(restored >= 0);
        CHECK(restored <= 12);
    }
}

TEST_CASE("Reshape bisection depth is a bounded, honest dial") {
    // 0 steps = "full amplitude when it fits, guard when it does not" (the
    // cheapest possible Reshape, 1 probe). More steps buy stroke resolution,
    // monotonically, at one Ruckig calculate() each.
    double prev = 0.0;
    for (int steps : {0, 2, 4, 6, 8}) {
        auto cfg = operatorConfig();
        cfg.infeasible_policy = InfeasiblePolicy::Reshape;
        cfg.infeasible_reshape_steps = (uint8_t)steps;
        Engine e(cfg, 0.30f);
        Command c;
        c.target = 1.00f; c.duration_us = 167 * (uint32_t)kMs;
        c.has_duration = true;
        REQUIRE(e.commit(c, 0));
        const double got = e.positionAt(167 * kMs);
        const double travel_mm = (got - 0.30) * kSpanMm;
        MESSAGE("reshape_steps=" << steps << "  travel " << travel_mm << " mm");
        if (steps == 0) {
            // No bisection and the full stroke does not fit → the guard takes
            // it (Stretch behavior: full stroke, overrun deadline).
            CHECK(e.planKind() == slopmotion::PlanKind::Ruckig);
        } else {
            CHECK(travel_mm >= prev - 1e-6);   // never gets worse with depth
            prev = travel_mm;
        }
    }
}

// ============================================================================
// SHARPNESS BEFORE AMPLITUDE (0.6.0) — jerk is the shape parameter
// ============================================================================
// The operator, watching Reshape at 500 mm/s: "is there a hybrid between scale
// and stretch where we just adjust the slope factor, so it stays smooth when
// close to max speed, and straightens out the further it is away?"
// There is, and the slope factor is JERK. These tests hold the engine to the
// three promises that makes: it costs NO amplitude and NO timing, it never
// exceeds the mechanical ceiling, and it is monotone in demand.

TEST_CASE("Softening buys curve back at zero cost in amplitude or timing") {
    // The regime the hybrid exists for: the quintic is illegal (velocity-bound)
    // but the MACHINE still reaches the whole stroke inside the deadline, so
    // 0.5.0 planned it at the full mechanical jmax — short ramps, long flat top,
    // a straight line for most of the stroke. Same endpoint, same deadline,
    // softer jerk: the flat top shrinks and the profile rounds out.
    auto hard = operatorConfig();          // 200 mm window, 1000 mm/s
    hard.infeasible_policy = InfeasiblePolicy::Reshape;
    hard.limits.vmax = 2.5f;               // 500 mm/s — the provoking speed
    auto soft = hard;
    hard.infeasible_soften = false;        // 0.5.0
    soft.infeasible_soften = true;         // 0.6.0

    // 140 mm in 400 ms: mean 350 mm/s against a 500 mm/s ceiling — the machine
    // has real deadline slack, and 0.5.0 spent all of it on a flat top anyway.
    const Shape a = segShape(hard, 0.15, 0.85, 400);
    const Shape b = segShape(soft, 0.15, 0.85, 400);
    reportShape("0.5.0 (jmax)   ", a, hard.limits.jmax);
    reportShape("0.6.0 (softened)", b, soft.limits.jmax);

    REQUIRE(a.kind == (uint8_t)slopmotion::PlanKind::Ruckig);
    REQUIRE(b.kind == (uint8_t)slopmotion::PlanKind::Ruckig);
    // NOTHING was traded: same amplitude to within a sample, same deadline.
    CHECK(b.travel_mm == doctest::Approx(a.travel_mm).epsilon(0.005));
    CHECK(b.duration_s == doctest::Approx(a.duration_s).epsilon(0.005));
    // ...and the shape came back. Measured: j_eff 2e6 → 1.44e5 (7.2 % of the
    // ceiling), flat 87.7 % → 47.4 %, peak accel 27227 → 8444 mm/s².
    CHECK(a.sharp == doctest::Approx(1.0).epsilon(0.02));
    CHECK(b.sharp < 0.20);
    CHECK(b.flat_pct < a.flat_pct - 20.0);
    CHECK(b.pk_over_mean > a.pk_over_mean + 0.20);
    CHECK(b.apk < a.apk * 0.5);            // gentler on the mechanism too
    // The price, and it is the only one: a rounder profile needs a higher peak
    // velocity for the same distance in the same time. Bounded by vmax — that
    // ceiling is exactly what stops the search.
    CHECK(b.vpk >= a.vpk);
    CHECK(b.vpk <= (double)soft.limits.vmax * 1.001);
}

TEST_CASE("Softening never exceeds the mechanical jerk ceiling") {
    // The whole feature spends UP TO jmax and never past it. Sweep the demand
    // across the softening range and check the SAMPLED jerk, not the promise.
    auto cfg = operatorConfig();
    cfg.infeasible_policy = InfeasiblePolicy::Reshape;
    cfg.limits.vmax = 2.5f;
    // Deliberately absurd knobs: a config push is not a trusted input.
    cfg.infeasible_soften_floor = -3.0f;   // nonsense → pinned sharp
    cfg.infeasible_soften_steps = 200;     // clamped to 10
    for (double d = 0.10; d <= 0.90; d += 0.05) {
        const Shape s = segShape(cfg, 0.05, 0.05 + d, 250);
        CHECK(s.vpk <= (double)cfg.limits.vmax * 1.02);
        CHECK(s.apk <= (double)cfg.limits.amax * 1.02);
        CHECK(s.jpk <= (double)cfg.limits.jmax * 1.10);   // FD across switches
        CHECK(s.sharp <= 1.0 + 1e-6);
    }
    cfg.infeasible_soften_floor = 0.02f;
    cfg.infeasible_soften_steps = 6;
    for (double d = 0.10; d <= 0.90; d += 0.05) {
        const Shape s = segShape(cfg, 0.05, 0.05 + d, 250);
        CHECK(s.vpk <= (double)cfg.limits.vmax * 1.02);
        CHECK(s.apk <= (double)cfg.limits.amax * 1.02);
        CHECK(s.jpk <= (double)cfg.limits.jmax * 1.10);
        CHECK(s.sharp <= 1.0 + 1e-6);
    }
}

TEST_CASE("Sharpness is MONOTONE in demand — never hunting, never backwards") {
    // The operator's determinism requirement, on both demand axes: a longer
    // stroke at a fixed deadline, and a shorter deadline at a fixed stroke.
    // More demand must never come out SOFTER. Guaranteed by construction (the
    // search interval is fixed, so the answer is the smallest point of a fixed
    // dyadic grid that fits, which is a non-decreasing step function of the
    // demand) — asserted here anyway, because "provable" and "true of the code
    // in front of you" are different claims.
    //
    // Restricted to the RESHAPE path: a quintic's reported sharpness is its own
    // peak jerk, and a velocity-saturated Ruckig double-S at critical jerk can
    // legitimately be gentler than the quintic that was just rejected — the
    // step across that boundary is physics, not a search fault.
    auto sweepStroke = [](float vmax, uint32_t ms) {
        auto cfg = operatorConfig();
        cfg.infeasible_policy = InfeasiblePolicy::Reshape;
        cfg.limits.vmax = vmax;
        double prev = -1.0, lo_seen = 2.0, hi_seen = 0.0;
        int n = 0, viol = 0;
        for (int i = 50; i <= 950; i++) {          // stroke 0.050 .. 0.950
            Engine e(cfg, 0.02f);
            Command c;
            c.target = (float)(0.02 + i * 0.001);
            c.duration_us = ms * (uint32_t)kMs;
            c.has_duration = true;
            REQUIRE(e.commit(c, 0));
            if (e.planKind() != slopmotion::PlanKind::Ruckig) continue;
            const double s = e.snapshot(0).sharpness;
            n++;
            lo_seen = std::min(lo_seen, s); hi_seen = std::max(hi_seen, s);
            if (prev >= 0.0 && s < prev - 1e-12) viol++;
            prev = s;
        }
        MESSAGE("stroke sweep vmax " << vmax << " T " << ms << " ms: " << n
                << " reshape points, sharpness " << lo_seen << ".." << hi_seen
                << ", monotonicity violations " << viol);
        CHECK(n > 100);              // the sweep really did exercise the path
        CHECK(viol == 0);
        CHECK(lo_seen < 0.5);        // it really did soften somewhere
        CHECK(hi_seen == doctest::Approx(1.0).epsilon(1e-6));
    };
    sweepStroke(2.5f, 250);
    sweepStroke(2.5f, 400);
    sweepStroke(5.0f, 167);

    // Deadline axis: squeeze the same stroke from 600 ms down to 100 ms.
    auto sweepDeadline = [](float vmax) {
        auto cfg = operatorConfig();
        cfg.infeasible_policy = InfeasiblePolicy::Reshape;
        cfg.limits.vmax = vmax;
        double prev = -1.0; int n = 0, viol = 0;
        for (int ms = 600; ms >= 100; ms--) {
            Engine e(cfg, 0.02f);
            Command c;
            c.target = 0.72f; c.duration_us = (uint32_t)ms * (uint32_t)kMs;
            c.has_duration = true;
            REQUIRE(e.commit(c, 0));
            if (e.planKind() != slopmotion::PlanKind::Ruckig) continue;
            const double s = e.snapshot(0).sharpness;
            n++;
            if (prev >= 0.0 && s < prev - 1e-12) viol++;
            prev = s;
        }
        MESSAGE("deadline sweep vmax " << vmax << ": " << n
                << " reshape points, monotonicity violations " << viol);
        CHECK(n > 100);
        CHECK(viol == 0);
    };
    sweepDeadline(2.5f);
    sweepDeadline(5.0f);
}

TEST_CASE("infeasible_soften = false is 0.5.0, sample for sample") {
    // The off switch has to be a real off switch: with it clear, not one sample
    // of any policy may differ from the 0.5.0 engine. (Scale and Stretch never
    // touch the search at all — checked here too, because "RESHAPE-only" is a
    // claim in the header and claims get tested.)
    for (auto pol : {InfeasiblePolicy::Stretch, InfeasiblePolicy::Scale,
                     InfeasiblePolicy::Reshape}) {
        for (float vmax : {2.5f, 5.0f}) {
            auto a = operatorConfig(); a.infeasible_policy = pol;
            a.limits.vmax = vmax; a.infeasible_soften = false;
            auto b = a;              // identical but for the knob
            b.infeasible_soften = true;
            Engine ea(a, 0.30f), eb(b, 0.30f);
            uint64_t t = 0;
            int diffs = 0;
            for (int i = 0; i < 24; i++) {
                Command c;
                c.target = (i % 2) ? 1.00f : 0.30f;
                c.duration_us = 167 * (uint32_t)kMs; c.has_duration = true;
                ea.commit(c, t); eb.commit(c, t);
                for (uint64_t u = t; u < t + 167 * kMs; u += kMs) {
                    if (ea.positionAt(u) != eb.positionAt(u)) diffs++;
                }
                t += 167 * kMs;
            }
            if (pol == InfeasiblePolicy::Reshape) {
                // Reshape SHOULD differ somewhere — otherwise the feature is
                // not wired up and the rest of this file is testing nothing.
                CHECK(diffs > 0);
            } else {
                CHECK(diffs == 0);
            }
        }
    }
    // ...and with the knob clear on BOTH engines, Reshape is bit-identical.
    auto a = operatorConfig();
    a.infeasible_policy = InfeasiblePolicy::Reshape;
    a.limits.vmax = 2.5f; a.infeasible_soften = false;
    auto b = a;
    b.infeasible_soften_floor = 1.0f;      // analog knob pinned at the sharp end
    b.infeasible_soften = true;
    Engine ea(a, 0.30f), eb(b, 0.30f);
    uint64_t t = 0;
    for (int i = 0; i < 16; i++) {
        Command c;
        c.target = (i % 2) ? 1.00f : 0.30f;
        c.duration_us = 167 * (uint32_t)kMs; c.has_duration = true;
        ea.commit(c, t); eb.commit(c, t);
        for (uint64_t u = t; u < t + 167 * kMs; u += kMs) {
            REQUIRE(ea.positionAt(u) == eb.positionAt(u));
        }
        t += 167 * kMs;
    }
    // Same for steps = 0: no probes, no softening, 0.5.0 behavior.
    auto z = a; z.infeasible_soften = true; z.infeasible_soften_steps = 0;
    Engine ez(z, 0.30f);
    Engine e0(a, 0.30f);
    t = 0;
    for (int i = 0; i < 16; i++) {
        Command c;
        c.target = (i % 2) ? 1.00f : 0.30f;
        c.duration_us = 167 * (uint32_t)kMs; c.has_duration = true;
        ez.commit(c, t); e0.commit(c, t);
        for (uint64_t u = t; u < t + 167 * kMs; u += kMs) {
            REQUIRE(ez.positionAt(u) == e0.positionAt(u));
        }
        t += 167 * kMs;
    }
}

TEST_CASE("Softening costs the centered band nothing (amplitude AND center)") {
    // The centering debt is a control loop; the sharpness search sits strictly
    // downstream of the endpoint it settles on, so the loop must not even
    // notice. The operator's own chains, both speeds, with and without the wire
    // end velocities — amplitude and band center have to land where 0.5.0 put
    // them, and only the SHAPE columns may move.
    for (float vmax : {2.5f, 5.0f}) {
        for (bool ev : {false, true}) {
            ChainOpts o;
            o.pol = InfeasiblePolicy::Reshape;
            o.vmax = vmax; o.ev_down = ev;
            o.soften = false;
            const Band off = runChain(o);
            o.soften = true;
            const Band on  = runChain(o);
            MESSAGE("vmax " << vmax << " ev " << ev
                    << " | 0.5.0 amp " << off.amp_mm << " mm center "
                    << off.center_err_mm << " mm, sharp " << off.sharp
                    << ", flat " << off.flat_pct << " %, apk " << off.apk_mm
                    << " mm/s^2   ||   0.6.0 amp " << on.amp_mm << " mm center "
                    << on.center_err_mm << " mm, sharp " << on.sharp
                    << ", flat " << on.flat_pct << " %, apk " << on.apk_mm
                    << " mm/s^2");
            CHECK(on.amp_mm == doctest::Approx(off.amp_mm).epsilon(0.01));
            CHECK(on.center_err_mm ==
                  doctest::Approx(off.center_err_mm).epsilon(0.05).scale(1.0));
            CHECK(std::fabs(on.center_err_mm - off.center_err_mm) < 0.5);
            CHECK(on.bounds_ok);
            CHECK(on.sharp <= off.sharp + 1e-6);      // never sharper than 0.5.0
            CHECK(on.flat_pct <= off.flat_pct + 1e-6);
        }
    }
}

TEST_CASE("The operator's chain: sharpness spent only where there is slack") {
    // The honest half of the story. With WIRE END VELOCITIES the centering debt
    // pulls each endpoint in short of the machine's reach, which manufactures
    // deadline slack — and the search finds it (measured: flat 70.3 % → 46.7 %
    // at 500 mm/s, 57.6 % → 32.8 % at 1000 mm/s, peak accel down 23 % / 19 %).
    // WITHOUT them the chain converges onto the bisected reach itself, where
    // jmax is MARGINAL by construction, and there is nothing left to give back
    // (66.3 % → 64.4 %). That is not a bug in the search — it is what "maximum
    // amplitude at a fixed deadline" MEANS. Buying curve back in that regime
    // costs amplitude, which is InfeasiblePolicy::Scale, a different intent.
    ChainOpts o;
    o.pol = InfeasiblePolicy::Reshape;
    o.vmax = 2.5f;                       // 500 mm/s — the provoking case

    o.ev_down = true;  o.soften = false; const Band ev_off = runChain(o);
    o.soften = true;                     const Band ev_on  = runChain(o);
    o.ev_down = false; o.soften = false; const Band no_off = runChain(o);
    o.soften = true;                     const Band no_on  = runChain(o);

    MESSAGE("500 mm/s wire-G chain : flat " << ev_off.flat_pct << " % -> "
            << ev_on.flat_pct << " %, sharp " << ev_off.sharp << " -> "
            << ev_on.sharp << ", amp " << ev_off.amp_mm << " -> "
            << ev_on.amp_mm << " mm");
    MESSAGE("500 mm/s bare  chain : flat " << no_off.flat_pct << " % -> "
            << no_on.flat_pct << " %, sharp " << no_off.sharp << " -> "
            << no_on.sharp << ", amp " << no_off.amp_mm << " -> "
            << no_on.amp_mm << " mm");

    // Where the debt makes slack, the win is large...
    CHECK(ev_on.flat_pct < ev_off.flat_pct - 15.0);
    CHECK(ev_on.sharp    < ev_off.sharp * 0.75);
    CHECK(ev_on.apk_mm   < ev_off.apk_mm);
    // ...and where it does not, the engine must not pretend otherwise — but it
    // must not go BACKWARDS either.
    CHECK(no_on.flat_pct <= no_off.flat_pct + 1e-6);
    CHECK(no_on.amp_mm   == doctest::Approx(no_off.amp_mm).epsilon(0.01));
}

TEST_CASE("Soften knobs are bounded, honest dials") {
    // Both knobs are plan-time budget, same doctrine as infeasible_reshape_
    // steps: deeper search = finer sharpness resolution, monotonically, one
    // Ruckig calculate() per step; a lower floor = more range to soften into.
    auto cfg = operatorConfig();
    cfg.infeasible_policy = InfeasiblePolicy::Reshape;
    cfg.limits.vmax = 2.5f;

    double prev = 2.0;
    for (int steps : {0, 1, 2, 4, 6, 8, 10}) {
        auto c = cfg; c.infeasible_soften_steps = (uint8_t)steps;
        const Shape s = segShape(c, 0.15, 0.85, 400);
        MESSAGE("soften_steps=" << steps << "  sharpness " << s.sharp
                << "  flat " << s.flat_pct << " %  travel " << s.travel_mm
                << " mm");
        CHECK(s.sharp <= prev + 1e-9);          // never worse with more depth
        CHECK(s.travel_mm > 139.0);             // amplitude untouched, always
        prev = s.sharp;
        if (steps == 0) CHECK(s.sharp == doctest::Approx(1.0).epsilon(1e-6));
    }

    // Floor: a HIGHER floor can only ever produce a sharper (or equal) plan,
    // because it removes the soft end of the search interval.
    prev = 0.0;
    for (float floor : {0.005f, 0.02f, 0.10f, 0.35f, 0.80f, 1.00f}) {
        auto c = cfg; c.infeasible_soften_floor = floor;
        const Shape s = segShape(c, 0.15, 0.85, 400);
        MESSAGE("soften_floor=" << floor << "  sharpness " << s.sharp);
        CHECK(s.sharp >= prev - 1e-9);
        CHECK(s.sharp >= floor * 0.99);         // never below its own floor
        prev = s.sharp;
    }
    // Nonsense floors fall back to the sharp end rather than doing something
    // creative (a live-tuning POST is not a trusted input).
    for (float floor : {-1.0f, 0.0f, 4.0f}) {
        auto c = cfg; c.infeasible_soften_floor = floor;
        const Shape s = segShape(c, 0.15, 0.85, 400);
        CHECK(s.sharp == doctest::Approx(1.0).epsilon(1e-6));
        CHECK(s.travel_mm > 139.0);
    }
}

TEST_CASE("Softening can only make a plan gentler — never worse, never absent") {
    // The broad grid: stroke × deadline × ceiling × start position × wire end
    // velocity, soften on vs off, ~5000 segments. Three invariants, and they are
    // the whole safety argument for the feature:
    //   * the amplitude and the plan duration are IDENTICAL (softening is
    //     strictly downstream of the endpoint decision);
    //   * the sampled ceilings hold on both;
    //   * softening never turns a plan that SUCCEEDED into a PlanFailed — the
    //     search probes time-optimal profiles but the adopted plan pins
    //     minimum_duration, so commitWaveformReshaped keeps a fallback to the
    //     mechanical ceiling for the case where those two disagree.
    int softened = 0, total = 0;
    for (float vmax : {2.0f, 2.5f, 3.5f, 5.0f}) {
        for (uint32_t ms : {60u, 100u, 167u, 250u, 400u, 600u}) {
            for (double start : {0.0, 0.20, 0.50}) {
                for (double d : {0.10, 0.25, 0.45, 0.70, 0.95}) {
                    if (start + d > 1.0) continue;
                    for (bool ev : {false, true}) {
                        auto off = operatorConfig();
                        off.infeasible_policy = InfeasiblePolicy::Reshape;
                        off.limits.vmax = vmax;
                        off.infeasible_soften = false;
                        auto on = off; on.infeasible_soften = true;
                        const float wire = ev ? (float)(0.4 * vmax) : 0.0f;
                        const Shape a = segShape(off, start, start + d, ms, ev, wire);
                        const Shape b = segShape(on,  start, start + d, ms, ev, wire);
                        total++;
                        if (b.sharp < a.sharp - 1e-6) softened++;
                        // identical outcome, gentler journey
                        CHECK(b.travel_mm ==
                              doctest::Approx(a.travel_mm).epsilon(0.01));
                        CHECK(b.duration_s ==
                              doctest::Approx(a.duration_s).epsilon(0.01));
                        CHECK(b.failures <= a.failures);
                        CHECK(b.sharp <= a.sharp + 1e-6);
                        // ceilings hold on the softened plan, as SAMPLED
                        CHECK(b.vpk <= (double)vmax * 1.02);
                        CHECK(b.apk <= (double)on.limits.amax * 1.02);
                        CHECK(b.jpk <= (double)on.limits.jmax * 1.10);
                    }
                }
            }
        }
    }
    MESSAGE("grid: " << softened << " of " << total
                     << " segments got a softer plan");
    CHECK(total > 200);
    CHECK(softened > 30);      // the grid really does exercise the feature
}

TEST_CASE("Snapshot::sharpness reports the plan's real peak jerk") {
    // The field has to mean the same thing on both plan kinds, or it is a
    // policy artifact rather than telemetry. Cross-check it against the
    // SAMPLED jerk on an easy quintic (where the shape is entirely the
    // sender's) and on a softened reshape (where it is the search's).
    auto cfg = operatorConfig();
    cfg.infeasible_policy = InfeasiblePolicy::Reshape;
    cfg.limits.vmax = 2.5f;
    const double jmax = cfg.limits.jmax;

    const Shape q = segShape(cfg, 0.30, 0.55, 600);     // comfortably feasible
    REQUIRE(q.kind == (uint8_t)slopmotion::PlanKind::Quintic);
    CHECK(q.sharp < 0.05);                              // an easy stroke IS soft
    CHECK(q.sharp * jmax == doctest::Approx(q.jpk).epsilon(0.05));

    const Shape r = segShape(cfg, 0.15, 0.85, 400);     // softened reshape
    REQUIRE(r.kind == (uint8_t)slopmotion::PlanKind::Ruckig);
    // Ruckig is bang-bang in jerk, so the sampled peak IS the planning ceiling
    // (the finite difference smears the switching instants, hence the margin).
    CHECK(r.sharp * jmax == doctest::Approx(r.jpk).epsilon(0.10));
}

// ============================================================================
// Settle grace — transport jitter is not starvation
// ============================================================================

TEST_CASE("Settle grace holds the end state, then brakes when the stream is really gone") {
    // Two paced segments establish a cadence estimate, the second ends MOVING,
    // then the stream stops. Inside the grace the engine must HOLD (legacy
    // handleTimeout behavior); past it, the brake must engage as it always
    // did.
    auto run = [](uint32_t grace_us) {
        auto cfg = operatorConfig();
        cfg.settle_grace_us = grace_us;
        Engine e(cfg, 0.30f);

        Command c1;
        c1.target = 0.45f; c1.duration_us = 100 * (uint32_t)kMs;
        c1.has_duration = true; c1.end_vel = 1.5f; c1.has_end_vel = true;
        REQUIRE(e.commit(c1, 0));
        Command c2;
        c2.target = 0.60f; c2.duration_us = 100 * (uint32_t)kMs;
        c2.has_duration = true; c2.end_vel = 1.5f; c2.has_end_vel = true;
        REQUIRE(e.commit(c2, 100 * kMs));
        return e;
    };

    SUBCASE("grace on: expiry+10 ms is still frozen at the plan's end state") {
        Engine e = run(30000);
        const double p_end = e.positionAt(200 * kMs);
        const double v_end = e.velocityAt(200 * kMs);
        REQUIRE(std::fabs(v_end) > 0.5);          // genuinely ends moving
        // Sample forward through the grace on the 1 ms grid.
        double max_jump = 0.0, prev = p_end;
        for (uint64_t t = 200 * kMs; t <= 229 * kMs; t += kMs) {
            const double p = e.positionAt(t);
            max_jump = std::max(max_jump, std::fabs(p - prev));
            prev = p;
        }
        CHECK(e.positionAt(229 * kMs) == doctest::Approx(p_end).epsilon(1e-9));
        CHECK(max_jump == doctest::Approx(0.0).epsilon(1e-9));
        CHECK(e.mode() == Mode::Waveform);        // NOT Settle
        CHECK(e.planKind() == slopmotion::PlanKind::Quintic);
        CHECK(drainFor(e, AnomalyType::SettleEngaged).seen == false);

        // Past the grace the brake engages — and does so CONTINUOUSLY (the
        // settle is anchored at the end of the hold, not at plan expiry: the
        // wrong anchor would enter the brake profile 30 ms deep and jump).
        double jump = 0.0; prev = e.positionAt(229 * kMs);
        for (uint64_t t = 230 * kMs; t <= 500 * kMs; t += kMs) {
            const double p = e.positionAt(t);
            jump = std::max(jump, std::fabs(p - prev));
            prev = p;
        }
        CHECK(e.mode() != Mode::Waveform);
        CHECK(jump <= 5.0 * 1e-3 * 1.05);         // ≤ vmax·1 ms: no jump
        CHECK(drainFor(e, AnomalyType::SettleEngaged).seen);
        CHECK(e.velocityAt(600 * kMs) == doctest::Approx(0.0).epsilon(1e-6));
    }

    SUBCASE("grace off (0): pre-0.4 behavior, brakes the instant it expires") {
        Engine e = run(0);
        e.positionAt(201 * kMs);
        CHECK(e.mode() == Mode::Settle);
        CHECK(drainFor(e, AnomalyType::SettleEngaged).seen);
    }

    SUBCASE("isolated point move still settles promptly (no cadence estimate)") {
        auto cfg = operatorConfig();
        Engine e(cfg, 0.30f);
        Command c;
        c.target = 0.45f; c.duration_us = 100 * (uint32_t)kMs;
        c.has_duration = true; c.end_vel = 1.5f; c.has_end_vel = true;
        REQUIRE(e.commit(c, 0));                  // ONE command: no estimate
        e.positionAt(101 * kMs);
        CHECK(e.mode() == Mode::Settle);
    }
}

TEST_CASE("Segment chain with 5 ms arrival jitter: no settle storm, no mode flap") {
    // The measured defect: the firmware's 5 ms SlopSync pacing drain makes
    // segment arrivals jitter around their scheduled instant, so plans expire
    // a few ms before their successor lands. Pre-0.4 that fired a full Ruckig
    // brake plan every time — 14 settles and 27 PlanKind flips over a
    // 14-segment chain at 5 ms of jitter, against 1 and 1 at 0 ms. The grace
    // window must make the jittered run behave like the clean one.
    // The precise signature of the defect is an UNSOLICITED REPLAN: the plan
    // changing on a sample where no command arrived, i.e. the engine acting on
    // the transport's timing rather than on the sender's intent. Counting
    // those is sharper than counting PlanKind flips (a flip also happens
    // legitimately when a segment is quintic-feasible and its neighbor is
    // not).
    struct Run { int settles; int unsolicited; int flips; double amp_mm; };
    auto run = [](InfeasiblePolicy pol, uint32_t grace_us, uint64_t jitter_us,
                  bool wire_end_vel) {
        auto cfg = operatorConfig();
        cfg.infeasible_policy = pol;
        cfg.settle_grace_us   = grace_us;
        Engine e(cfg, 0.30f);

        const uint64_t seg = 167 * kMs;
        int i = 0, settles = 0, unsolicited = 0, flips = 0;
        uint8_t prev_kind = 0;
        double lo = 1e9, hi = -1e9;
        uint64_t next_cmd = 0, last_plan = 0;
        for (uint64_t t = 0; t <= 16 * 167 * kMs; t += kMs) {
            bool commanded = false;
            if (t >= next_cmd) {
                Command c;
                c.target       = (i % 2) ? 1.00f : 0.30f;
                c.duration_us  = (uint32_t)seg;
                c.has_duration = true;
                if (wire_end_vel) {   // the sender's slope handoff
                    c.end_vel     = (i % 2) ? 2.6f : -2.6f;
                    c.has_end_vel = true;
                }
                e.commit(c, next_cmd);
                commanded = true;
                i++;
                // Scheduled instant + alternating transport lag: every other
                // plan is preempted 5 ms early, every other one is left
                // hanging 5 ms past its expiry. That second case is the one
                // that used to fire a brake.
                next_cmd  = (uint64_t)i * seg + ((i % 2) ? jitter_us : 0);
                last_plan = e.lastPlanUs();
            }
            const double p = e.positionAt(t);
            REQUIRE(p >= -1e-9);
            REQUIRE(p <= 1.0 + 1e-9);
            if (!commanded && e.lastPlanUs() != last_plan) {
                unsolicited++;
                last_plan = e.lastPlanUs();
            }
            const uint8_t k = (uint8_t)e.planKind();
            if (prev_kind != 0 && k != prev_kind) flips++;
            prev_kind = k;
            if (t > 2 * kS) { lo = std::min(lo, p); hi = std::max(hi, p); }
            slopmotion::Anomaly ev;
            while (e.popAnomaly(ev)) {
                if (ev.kind == (uint8_t)AnomalyType::SettleEngaged) settles++;
            }
        }
        return Run{settles, unsolicited, flips, (hi - lo) * kSpanMm};
    };

    for (bool g : {false, true}) {
        const Run fixed  = run(InfeasiblePolicy::Reshape, 30000, 5 * kMs, g);
        const Run clean  = run(InfeasiblePolicy::Reshape, 30000, 0,       g);
        const Run before = run(InfeasiblePolicy::Scale,   0,     5 * kMs, g);
        MESSAGE((g ? "with wire G  " : "no wire G    ")
                << "jittered Reshape+grace: " << fixed.settles << " settles, "
                << fixed.unsolicited << " unsolicited replans, " << fixed.flips
                << " kind flips, amp " << fixed.amp_mm << " mm   |  clean: "
                << clean.settles << "/" << clean.unsolicited << "/"
                << clean.flips << ", amp " << clean.amp_mm
                << " mm   |  pre-0.4 (Scale, no grace): " << before.settles
                << "/" << before.unsolicited << "/" << before.flips
                << ", amp " << before.amp_mm << " mm");

        // The grace must make the jittered run behave like the clean one: at
        // most the single end-of-chain settle, which is real starvation.
        CHECK(fixed.settles     <= clean.settles + 1);
        CHECK(fixed.unsolicited <= clean.unsolicited + 1);
        // ...while Reshape delivers materially more stroke than Scale did.
        CHECK(fixed.amp_mm > before.amp_mm * 1.3);

        if (g) {
            // ONLY the G-bearing chain can starve mid-glide: without a wire
            // end velocity every segment ends at rest, `maybeSettle` collapses
            // straight to a hold, and there was never anything to fix. With
            // G the pre-0.4 engine fired a brake on every late segment.
            CHECK(before.settles    >= 6);
            CHECK(before.unsolicited >= 6);
            CHECK(fixed.unsolicited * 4 < before.unsolicited);
        } else {
            CHECK(before.settles == 0);
        }
    }
}

TEST_CASE("Reset drops everything back to a hold") {
    Engine e(testConfig(), 0.5f);
    Command c;
    c.target = 0.9f; c.duration_us = 500 * (uint32_t)kMs; c.has_duration = true;
    REQUIRE(e.commit(c, 0));
    REQUIRE(e.isBusy(100 * kMs));

    e.resetAt(0.42f, 200 * kMs);
    CHECK_FALSE(e.isBusy(200 * kMs));
    CHECK(e.positionAt(250 * kMs) == doctest::Approx(0.42f));
    CHECK(e.velocityAt(250 * kMs) == doctest::Approx(0.0));
    CHECK(e.mode() == Mode::Idle);
}

// ============================================================================
// RFC-008 — hub-side handoff sanity guard (one-segment lookahead)
// ============================================================================
//
// "The machine plans for the worst so clients don't have to."
//
// The guard is a SAFETY/QUALITY bound over a CONTINUOUS domain, so a handful of
// hand-picked cases would prove almost nothing about it. These tests sweep the
// whole (chord_in, chord_out, end_vel, k) grid and assert the invariants
// everywhere, in the same spirit as the other exhaustive sweeps in this file:
// proof by construction, not by sampling three points and hoping.
//
// The MEASURED pathology that motivated RFC-008 is a NAMED POINT inside that
// swept domain (kPathoEndVel below), so the real-world failure is a labeled
// regression case rather than folklore.
// ============================================================================

namespace {

using slopmotion::boundHandoffVelocity;

// The measured failure, MFP plugin v0.2.1 against slopsim, 2026-07-25: a Makima
// tangent of 1.816 norm/s handed into a span whose own mean velocity is 0.050
// norm/s -- 36x over. Sane relative to its OWN span (whose chord was steep) and
// absurd only relative to the NEXT one, which is exactly why a current-chord-
// only bound cannot catch it.
constexpr float kPathoEndVel  = 1.816f;   // norm/s, as measured on the wire
constexpr float kPathoNext    = 0.050f;   // norm/s, the FOLLOWING span's mean
constexpr float kPathoCurrent = 3.000f;   // norm/s, its own span's mean
constexpr float kK            = 1.5f;     // the shape-preserving chord factor

// The swept axes. Chords run from a dead plateau (0 -- "arriving at a hold")
// through everything a normalized stroke window can physically mean, and are
// deliberately DENSE at the small end, because that is where the bound bites.
// The measured pathology's numbers are members of their own axes.
const std::vector<float> kChordAxis = {
    0.0f, 0.001f, 0.01f, 0.025f, 0.050f, 0.075f, 0.1f, 0.15f, 0.2f, 0.3f,
    0.4f, 0.5f, 0.7f, 0.9f, 1.0f, 1.2f, 1.5f, 1.816f, 2.0f, 2.5f,
    3.0f, 4.0f, 5.0f, 8.0f, 12.0f, 32.767f,
};
// Signed handoffs: reversals matter as much as runs, 0 is a legitimate "ends at
// rest", and the extremes are the 0x0085 wire's own +-32.767 norm/s rails.
const std::vector<float> kEndVelAxis = {
    -32.767f, -12.0f, -5.0f, -3.0f, -2.0f, -1.816f, -1.5f, -1.0f, -0.7f,
    -0.5f, -0.3f, -0.2f, -0.15f, -0.1f, -0.075f, -0.050f, -0.025f, -0.01f,
    -0.001f, -0.0f,
    0.0f, 0.001f, 0.01f, 0.025f, 0.050f, 0.075f, 0.1f, 0.15f, 0.2f, 0.3f,
    0.5f, 0.7f, 1.0f, 1.5f, 1.816f, 2.0f, 3.0f, 5.0f, 12.0f, 32.767f,
};
// k is the ONE aggressiveness knob: 0 = guard off, 1.5 = the Fritsch-Carlson
// shape-preserving value, 3.0 = the looser per-tangent box, 8.0 = the clamp
// ceiling. 12.0 is swept to prove the clamp actually clamps.
const std::vector<float> kFactorAxis = { 0.0f, 1.5f, 3.0f, 8.0f, 12.0f };

inline float sgnf(float x) { return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); }

// One guarded WAVEFORM segment, sampled on the firmware's own 1 ms grid.
// Returns how many HandoffBounded anomalies the engine recorded.
int runGuardedSegment(bool lookahead, float end_vel, float next_chord, float k,
                      std::vector<double>& out) {
    Config cfg = testConfig();
    cfg.handoff_chord_factor = k;
    Engine e(cfg, 0.2f);
    Command c;
    c.target         = 0.6f;
    c.duration_us    = 400 * (uint32_t)kMs;
    c.has_duration   = true;
    c.end_vel        = end_vel;
    c.has_end_vel    = true;
    c.next_chord     = next_chord;
    c.has_next_chord = lookahead;
    REQUIRE(e.commit(c, 0));
    // POSITION AND VELOCITY, interleaved. Position alone is not enough to tell
    // two handoffs apart: a quintic lands on its commanded target either way --
    // the whole difference between a bounded and an unbounded handoff is the
    // VELOCITY it arrives with. Comparing the pair is what makes "identical
    // trajectory" mean identical motion rather than identical endpoints.
    out.clear();
    for (uint64_t t = 0; t <= 400 * kMs; t += kMs) {
        out.push_back(e.positionAt(t));
        out.push_back(e.velocityAt(t));
    }
    return drainFor(e, AnomalyType::HandoffBounded).count;
}

// True when two sampled trajectories differ ANYWHERE.
bool trajectoriesDiffer(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return true;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return true;
    return false;
}

}  // namespace

TEST_CASE("RFC-008 handoff bound: property sweep over the whole (in, out, vf, k) grid") {
    // Every assertion below is a real doctest CHECK, so the reported assertion
    // count IS the coverage. Grid: 26 chords x 26 chords x 40 handoffs x 5 k.
    size_t points = 0;

    for (float k_raw : kFactorAxis) {
        // The engine clamps an untrusted config push to [0, 8]; the invariants
        // are stated against the EFFECTIVE k, which is what actually applied.
        const float k = k_raw > 8.0f ? 8.0f : k_raw;
        for (float cin : kChordAxis) {
            for (float cout : kChordAxis) {
                const float limit = k * std::min(cin, cout);
                for (float vf : kEndVelAxis) {
                    const float got = boundHandoffVelocity(vf, cin, cout, k_raw);
                    ++points;

                    if (k_raw <= 0.0f) {
                        // OFF SWITCH: byte-for-byte passthrough, no exceptions.
                        // This is what makes the M5d A/B honest.
                        CHECK(got == vf);
                        continue;
                    }

                    // (1) THE POINT OF THE WHOLE EXERCISE: never faster than
                    //     the FOLLOWING chord's Fritsch-Carlson limit.
                    CHECK(std::fabs(got) <= k * cout + 1e-5f);
                    // (2) ...nor the knot limit, which is the min of both sides.
                    CHECK(std::fabs(got) <= limit + 1e-5f);
                    // (3) The guard only ever REMOVES speed.
                    CHECK(std::fabs(got) <= std::fabs(vf) + 1e-5f);
                    // (4) Sign is preserved: a wrong-signed handoff stays
                    //     wrong-signed but bounded. The only thing the guard
                    //     changes is pathological SIZE.
                    if (got != 0.0f) CHECK(sgnf(got) == sgnf(vf));
                    // (5) IN-BOUNDS INPUT IS UNTOUCHED, bit for bit. A guard
                    //     that perturbs a well-behaved client is a feel
                    //     regression for every well-behaved client.
                    if (std::fabs(vf) <= limit) CHECK(got == vf);
                }
            }
        }
    }

    MESSAGE("handoff bound swept " << points
            << " (chord_in, chord_out, end_vel, k) points");
    CHECK(points == kChordAxis.size() * kChordAxis.size() *
                    kEndVelAxis.size() * kFactorAxis.size());
}

TEST_CASE("RFC-008 handoff bound: monotone in both chords") {
    // Bounding must be MONOTONE in the next segment's slope -- a roomier
    // successor can never buy LESS handoff speed than a tighter one. Without
    // this the guard would be a source of discontinuous feel changes as a
    // script's chord lengths drift. (kChordAxis is ascending by construction.)
    size_t pairs = 0;
    for (float cin : kChordAxis) {
        for (float vf : kEndVelAxis) {
            float prev = 0.0f;
            bool  first = true;
            for (float cout : kChordAxis) {
                const float got = std::fabs(boundHandoffVelocity(vf, cin, cout, kK));
                if (!first) { CHECK(got >= prev - 1e-5f); ++pairs; }
                prev = got; first = false;
            }
        }
    }
    // ...and monotone in the CURRENT chord for the same reason, by symmetry of
    // the min(). Asserted rather than assumed: min() is symmetric, the code
    // that calls it need not be.
    for (float cout : kChordAxis) {
        for (float vf : kEndVelAxis) {
            float prev = 0.0f;
            bool  first = true;
            for (float cin : kChordAxis) {
                const float got = std::fabs(boundHandoffVelocity(vf, cin, cout, kK));
                if (!first) { CHECK(got >= prev - 1e-5f); ++pairs; }
                prev = got; first = false;
            }
        }
    }
    MESSAGE("handoff bound monotonicity: " << pairs << " adjacent-pair checks");
}

TEST_CASE("RFC-008 handoff bound: zero, plateau, reversal and unknown-chord cases") {
    // end_vel = 0 is a REAL slope ("ends at rest"), never the absent sentinel
    // (that is INT16_MIN on the 0x0085 wire, decoded at ingress into
    // has_end_vel = false, so the guard never sees it at all). It must survive
    // every chord combination untouched.
    for (float cin : kChordAxis)
        for (float cout : kChordAxis)
            CHECK(boundHandoffVelocity(0.0f, cin, cout, kK) == 0.0f);

    // A plateau on EITHER side forces a dead stop at the knot: arriving at a
    // hold still moving is exactly the lie the Ground Truth doctrine forbids.
    CHECK(boundHandoffVelocity(2.0f, 0.0f, 5.0f, kK) == 0.0f);
    CHECK(boundHandoffVelocity(2.0f, 5.0f, 0.0f, kK) == 0.0f);
    CHECK(boundHandoffVelocity(-2.0f, 5.0f, 0.0f, kK) == 0.0f);

    // Reversal: a handoff pointing the OPPOSITE way to both chords keeps its
    // sign and only loses magnitude.
    CHECK(boundHandoffVelocity(-4.0f, 1.0f, 1.0f, kK) == doctest::Approx(-1.5f));
    CHECK(boundHandoffVelocity(4.0f, 1.0f, 1.0f, kK) == doctest::Approx(1.5f));

    // "Unknown" chord (negative) disables the bound on that side entirely --
    // the guard never bounds on a guess.
    CHECK(boundHandoffVelocity(9.0f, -1.0f, 0.01f, kK) == 9.0f);
    CHECK(boundHandoffVelocity(9.0f, 0.01f, -1.0f, kK) == 9.0f);

    // Non-finite passthrough: Engine::commit() rejects non-finite command
    // inputs upstream, and a numeric guard is not the place to relitigate it.
    CHECK(std::isnan(boundHandoffVelocity(std::nanf(""), 1.0f, 1.0f, kK)));
    CHECK(std::isinf(boundHandoffVelocity(INFINITY, 1.0f, 1.0f, kK)));
    // A non-finite CHORD is passed through as the caller's own end_vel, NOT as
    // a NaN: a garbage lookahead must never contaminate a sane handoff.
    CHECK(boundHandoffVelocity(1.0f, std::nanf(""), 1.0f, kK) == 1.0f);
    CHECK(boundHandoffVelocity(1.0f, 1.0f, std::nanf(""), kK) == 1.0f);
}

TEST_CASE("RFC-008 handoff bound: the MEASURED pathology (1.816 into a 0.050 span)") {
    // The regression case, named. Bounding against the CURRENT segment's own
    // chord alone leaves the pathological value completely untouched...
    CHECK(boundHandoffVelocity(kPathoEndVel, kPathoCurrent, kPathoCurrent, kK)
          == kPathoEndVel);
    // ...which is the whole reason the guard has to look ONE SEGMENT FORWARD.
    const float bounded =
        boundHandoffVelocity(kPathoEndVel, kPathoCurrent, kPathoNext, kK);
    CHECK(bounded == doctest::Approx(0.075f));
    // 36x over becomes exactly 1.5x the following span's mean speed -- the same
    // 1.816 -> 0.075 the plugin's own client-side limiter measured.
    MESSAGE("measured pathology: end_vel " << kPathoEndVel << " -> " << bounded
            << " norm/s (" << (kPathoEndVel / kPathoNext)
            << "x the next span's mean -> " << (bounded / kPathoNext) << "x)");
    CHECK(bounded / kPathoNext == doctest::Approx(kK));
}

TEST_CASE("RFC-008 guard in the engine: lookahead arms it, absence changes nothing") {
    // The ENGINE-level contract, sampled as reality on the firmware's own 1 ms
    // grid. A guard that changes motion for good input is a defect, and this is
    // the assertion that says so about the real planner rather than about the
    // arithmetic.
    SUBCASE("well-behaved handoff: untouched, no anomaly, identical trajectory") {
        // 0.4 units over 0.4 s -> chord_in 1.0; a 0.8 norm/s successor chord
        // permits up to 1.2 norm/s, so 0.5 is comfortably inside the bound.
        std::vector<double> without, with;
        CHECK(runGuardedSegment(false, 0.5f, 0.8f, kK, without) == 0);
        CHECK(runGuardedSegment(true,  0.5f, 0.8f, kK, with)    == 0);
        REQUIRE(without.size() == with.size());
        for (size_t i = 0; i < without.size(); ++i) CHECK(without[i] == with[i]);
    }

    SUBCASE("the measured pathology: bounded once, counted, motion changes") {
        std::vector<double> without, with;
        CHECK(runGuardedSegment(false, kPathoEndVel, kPathoNext, kK, without) == 0);
        CHECK(runGuardedSegment(true,  kPathoEndVel, kPathoNext, kK, with)    == 1);
        // The machine no longer tries to arrive 36x too fast, so the motion is
        // genuinely different -- and specifically the ARRIVAL VELOCITY is, which
        // is where a handoff lives. (The quintic lands on 0.6 either way.)
        CHECK(trajectoriesDiffer(without, with));
        CHECK(std::fabs(without.back()) > std::fabs(with.back()));
    }

    SUBCASE("TAIL CASE: no known successor is accepted exactly as sent") {
        // Deliberate -- see the ingress note in
        // SlopSyncHubService::drainMotionStream. Guessing a chord we do not
        // have would trim well-behaved senders for free. RFC-049c evaluated an
        // own-chord fallback for exactly this case and REJECTED it (see
        // commitWaveform's comment) after it measurably perturbed the
        // centering/reshape regression bench — this stays the honest tail case.
        std::vector<double> tail;
        CHECK(runGuardedSegment(false, kPathoEndVel, kPathoNext, kK, tail) == 0);
    }

    SUBCASE("k = 0 is a true off switch (the M5d A/B control)") {
        std::vector<double> off_with, off_without, on_with;
        CHECK(runGuardedSegment(true,  kPathoEndVel, kPathoNext, 0.0f, off_with)    == 0);
        CHECK(runGuardedSegment(false, kPathoEndVel, kPathoNext, 0.0f, off_without) == 0);
        CHECK(runGuardedSegment(true,  kPathoEndVel, kPathoNext, kK,   on_with)     == 1);
        REQUIRE(off_with.size() == off_without.size());
        for (size_t i = 0; i < off_with.size(); ++i) CHECK(off_with[i] == off_without[i]);
        CHECK(trajectoriesDiffer(on_with, off_with));
    }
}

TEST_CASE("RFC-008 guard: a bounded handoff does not poison the NEXT segment's af") {
    // The second half of the measured failure: the engine estimates a segment's
    // end ACCELERATION as a backward difference of consecutive end velocities,
    // so before the guard existed one oversized tangent corrupted the FOLLOWING
    // segment's boundary conditions too. The guard feeds the ACCEPTED value
    // forward, so the af series is built from velocities the machine actually
    // intends to reach.
    auto chain = [](bool lookahead) {
        Engine e(testConfig(), 0.2f);
        Command a;
        a.target = 0.6f; a.duration_us = 200 * (uint32_t)kMs; a.has_duration = true;
        a.end_vel = kPathoEndVel; a.has_end_vel = true;
        a.next_chord = kPathoNext; a.has_next_chord = lookahead;
        REQUIRE(e.commit(a, 0));
        Command b;
        b.target = 0.61f; b.duration_us = 200 * (uint32_t)kMs; b.has_duration = true;
        b.end_vel = 0.05f; b.has_end_vel = true;   // a sane, gentle successor
        REQUIRE(e.commit(b, 200 * kMs));
        return sweep(e, 200 * kMs, 400 * kMs);
    };
    const SweepStats poisoned = chain(false);
    const SweepStats guarded  = chain(true);
    MESSAGE("successor segment peak |a|: unguarded " << poisoned.max_abs_a
            << " vs guarded " << guarded.max_abs_a);
    // The successor is a 0.01-unit crawl; with the guard armed it stays a crawl
    // instead of inheriting a wild af from a handoff nobody could honor.
    CHECK(guarded.max_abs_a < poisoned.max_abs_a);
}

// ============================================================================
// M7a — THE SPEED CEILING HOLDS IN BOTH DIRECTIONS
// ============================================================================
//
// Found by SlopScope on its first real capture against slopsim: the plan-strip
// channel reported cur_vel = -896 mm/s on a machine whose input speed ceiling
// was 550 mm/s, while the POSITIVE peaks sat at exactly +550. The asymmetry was
// a red herring — every ceiling test in this engine is on |v|, and always was.
// What the capture had actually caught was this:
//
//   RUCKIG IS NOT A LEGALITY ORACLE. `max_velocity` is an input to its profile
//   SEARCH, not a postcondition of its output. Handed a jerk ceiling too low to
//   turn the boundary state around, Ruckig Community returns a profile that
//   sails through the velocity ceiling — and, lower still, clean out of the
//   stroke window — instead of reporting the move infeasible.
//
// The sharpness search (softestFeasibleJerk) hunts for the SOFTEST jerk ceiling
// that still meets the deadline, so it walks straight into that region and
// picks it, because "arrives in time" was the whole feasibility predicate. On
// the captured chase→segment handoff it chose j = 115 of a 4000 ceiling, where
// Ruckig plans 1.43x vmax and swings 0.34 units past its own start position.
//
// These cases pin the fix from both ends: the search must not SELECT an illegal
// ceiling, and the planner must not ADOPT one. Only shapes that were already
// illegal change — a legal plan scans identically before and after.
namespace {

// Tolerance on the sampled ceiling, mirroring the engine's kRuckigLegalEps. A
// plan that STARTS at the ceiling with adverse acceleration must overshoot it
// slightly on the way back inside; the jerk limit says so and no planner can
// avoid it. The pathology under test is 1.4x and up, nowhere near this band.
constexpr double kCeilTol = 1.05;

// One chase→segment handoff. `pts` dense stream points along a sine (bare
// points, so they plan as CHASE), then ONE timed segment carrying no end
// velocity — the 0x0085 INT16_MIN sentinel — which is precisely what makes the
// engine fill vf from its own stream estimate rather than from the wire.
SweepStats handoffCase(const Config& cfg, double amp, double freq, double phase,
                       int pts, float seg_target, uint32_t seg_ms) {
    Engine e(cfg, 0.5f);
    const uint64_t dt = 20 * kMs;                       // 50 Hz, dense
    uint64_t clock = 0;
    for (int i = 0; i < pts; i++) {
        const uint64_t at = (uint64_t)i * dt;
        // Advance the engine the way the firmware actually does — the 1 ms
        // sampler runs BETWEEN commands, and it is what drives maybeSettle.
        // Committing on a frozen clock is a different machine.
        for (; clock < at; clock += kMs) (void)e.positionAt(clock);
        const double t = (double)at * 1e-6;
        Command c;
        c.target      = (float)(0.5 + amp * std::sin(2*M_PI*freq*t + phase));
        c.end_vel     = (float)(amp * 2*M_PI*freq * std::cos(2*M_PI*freq*t + phase));
        c.has_end_vel = true;
        e.commit(c, at);                                // bare point -> chase
    }
    const uint64_t seg_at = (uint64_t)pts * dt;
    Command s;
    s.target       = seg_target;
    s.duration_us  = seg_ms * (uint32_t)kMs;
    s.has_duration = true;
    s.has_end_vel  = false;                             // the wire sentinel
    e.commit(s, seg_at);
    return sweep(e, seg_at, seg_at + (uint64_t)seg_ms * kMs);
}

}  // namespace

TEST_CASE("M7a: the captured SlopScope excursion — chase handoff into a segment") {
    // The capture, reconstructed: slopsim's limit set, the probe's own sine
    // (0.5 + 0.35·sin 2π·0.8t at 50 Hz), then the probe's own segment (target
    // 0.7, 900 ms, end-velocity sentinel). Before the fix this planned 1.43x
    // vmax and dived 0.34 units the wrong way; the plan-strip reported it
    // faithfully, which is how SlopScope caught it.
    // slopsim derives the engine's normalized jerk ceiling from the mm-domain
    // input limit set (2e6 mm/s³ / 500 mm span = 4000), not from the firmware's
    // sm_tune_jmax default that machineConfig() carries — and the capture ran
    // under the derived one. The distinction matters here and only here: the
    // sharpness search hunts DOWNWARD from this ceiling, so where it starts is
    // what decides whether it can reach the illegal region at all.
    Config cfg = machineConfig();
    cfg.limits.jmax = 4000.0f;
    const SweepStats s = handoffCase(cfg, 0.35, 0.8, 0.0, 400, 0.7f, 900);
    MESSAGE("handoff peak |v| = " << s.max_abs_v << " (vmax " << cfg.limits.vmax
            << ", ratio " << s.max_abs_v / cfg.limits.vmax << "), position span ["
            << s.min_p << ", " << s.max_p << "]");
    CHECK(s.max_abs_v <= cfg.limits.vmax * kCeilTol);
    CHECK(s.max_abs_a <= cfg.limits.amax * kCeilTol);
}

TEST_CASE("M7a: |v| <= vmax across the whole handoff domain, both directions") {
    // The property sweep. If the ceiling can be broken at all, one capture is a
    // poor way to find out — so plan the handoff across a domain of stream
    // shapes (which set the inherited p, v, a AND the vf the engine estimates),
    // segment targets on BOTH sides of the handoff position (reversals), and
    // segment durations spanning the stretch factor that drives the sharpness
    // search. Upward and downward targets are tracked separately: the original
    // report was an asymmetry claim, and this is what settles it.
    for (int cfg_idx = 0; cfg_idx < 2; cfg_idx++) {
        const Config cfg = cfg_idx == 0 ? machineConfig() : operatorConfig();
        const std::string name = cfg_idx == 0 ? "virtual machine" : "operator machine";
        double worst = 0.0, worst_up = 0.0, worst_down = 0.0;
        int cases = 0;

        for (double amp : {0.20, 0.35})
        for (double freq : {0.5, 0.8, 1.2})
        for (double phase : {0.0, 0.5*M_PI, M_PI, 1.5*M_PI})
        for (float tgt : {0.3f, 0.7f})
        for (uint32_t ms : {200u, 500u, 900u}) {
            const SweepStats s = handoffCase(cfg, amp, freq, phase, 200, tgt, ms);
            cases++;
            const double ratio = s.max_abs_v / (double)cfg.limits.vmax;
            worst = std::max(worst, ratio);
            if (tgt > 0.5f) worst_up   = std::max(worst_up, ratio);
            else            worst_down = std::max(worst_down, ratio);
            CHECK(s.max_abs_v <= cfg.limits.vmax * kCeilTol);
        }
        MESSAGE(name << ": " << cases << " handoffs, worst |v|/vmax = " << worst
                << " (upward targets " << worst_up << ", downward " << worst_down
                << ")");
        // The asymmetry the report suspected does not exist. Both directions
        // are bounded alike, and the bound is the same number.
        CHECK(worst_up   <= kCeilTol);
        CHECK(worst_down <= kCeilTol);
    }
}

TEST_CASE("M7a: softened plans stay legal — the search cannot pick an illegal jerk") {
    // The mechanism in isolation, with no stream involved: a moving start into
    // a short reversal on a long deadline is exactly the shape whose softest
    // in-time jerk ceiling is illegal. Reshape is the policy that runs the
    // sharpness search, so it is the policy under test; Stretch never softens
    // and is the control that proves the sweep is not just measuring Reshape.
    auto worstFor = [](InfeasiblePolicy pol, float end_vel, uint32_t ms) {
        Config cfg = machineConfig();
        cfg.infeasible_policy = pol;
        Engine e(cfg, 0.8f);
        // Seed a moving state heading DOWN, then command a short reversal that
        // has to be delivered on a long clock.
        Command a;
        a.target = 0.72f; a.duration_us = 120 * (uint32_t)kMs; a.has_duration = true;
        a.end_vel = -1.0f; a.has_end_vel = true;
        e.commit(a, 0);
        Command b;
        b.target = 0.70f; b.duration_us = ms * (uint32_t)kMs; b.has_duration = true;
        b.end_vel = end_vel; b.has_end_vel = true;
        e.commit(b, 120 * kMs);
        const SweepStats s = sweep(e, 120 * kMs, 120 * kMs + (uint64_t)ms * kMs);
        return s.max_abs_v / (double)cfg.limits.vmax;
    };

    for (uint32_t ms : {300u, 600u, 900u, 1400u})
    for (float ev : {-1.05f, -0.8f, -0.4f, 0.0f, 0.4f, 0.8f, 1.05f}) {
        CHECK(worstFor(InfeasiblePolicy::Reshape, ev, ms) <= kCeilTol);
        CHECK(worstFor(InfeasiblePolicy::Stretch, ev, ms) <= kCeilTol);
    }
}
