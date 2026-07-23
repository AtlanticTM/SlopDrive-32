// slopmotion_traces — scenario harness: SlopMotion (Ruckig) vs the legacy
// cubic MotionInterpolator, same command streams, sampled side by side.
// Desktop-only; emits one JSON trace file per scenario for graphing. Both
// engines are the REAL code: the vendored Ruckig solvers and the actual
// src/motion/MotionInterpolator.cpp the firmware runs today.
//
// Build & run (from repo root, MinGW g++ on PATH):
//   g++ -std=gnu++2b -O2 -I lib/slopmotion/include -I lib/ruckig/include \
//       -I include/motion examples/slopmotion_traces/traces.cpp \
//       src/motion/MotionInterpolator.cpp lib/ruckig/src/ruckig/*.cpp \
//       -o slopmotion_traces && ./slopmotion_traces <out_dir>
//
// Determinism: synthetic clock, LCG jitter with fixed seed — identical JSON
// on every run.

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#include "slopmotion/slopmotion.hpp"
#include "MotionInterpolator.h"

namespace {

constexpr uint64_t kMs = 1000ULL;
constexpr uint64_t kS  = 1000000ULL;
constexpr uint64_t kSampleUs = 4 * kMs;   // 250 Hz trace grid (plot-friendly)

// Stream limits used for every scenario (normalized units — the firmware glue
// derives the real set from the mm limits / stroke window in part 2).
slopmotion::Config streamConfig() {
    slopmotion::Config cfg;
    cfg.limits.vmax = 3.0f;
    cfg.limits.amax = 30.0f;
    cfg.limits.jmax = 500.0f;
    return cfg;
}

// Deterministic LCG for arrival jitter.
struct Lcg {
    uint32_t s = 0xC0FFEE01u;
    uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
    // uniform in [-r, +r] microseconds
    int64_t jitterUs(int64_t r) {
        return (int64_t)(next() % (uint32_t)(2 * r + 1)) - r;
    }
};

// ---- Command timeline -------------------------------------------------------
struct Cmd {
    uint64_t t_us;
    float    target;
    uint32_t duration_us = 0;   // 0 = bare point
    float    end_vel     = 0.0f;   // units/s
    bool     has_end_vel = false;
};

// ---- Trace container --------------------------------------------------------
struct Series {
    std::string name;
    std::vector<double> t, p, v, a;
};

struct Trace {
    std::string name;
    std::string title;
    std::string note;
    std::vector<Cmd> cmds;
    std::vector<Series> series;
    float vmax = 0, amax = 0, jmax = 0;
};

void writeArray(FILE* f, const char* key, const std::vector<double>& xs,
                int decimals) {
    std::fprintf(f, "\"%s\":[", key);
    for (size_t i = 0; i < xs.size(); i++) {
        std::fprintf(f, i ? ",%.*f" : "%.*f", decimals, xs[i]);
    }
    std::fprintf(f, "]");
}

void writeTrace(const Trace& tr, const std::string& dir) {
    const std::string path = dir + "/" + tr.name + ".json";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    std::fprintf(f, "{\"name\":\"%s\",\"title\":\"%s\",\"note\":\"%s\",",
                 tr.name.c_str(), tr.title.c_str(), tr.note.c_str());
    std::fprintf(f, "\"limits\":{\"vmax\":%.3f,\"amax\":%.3f,\"jmax\":%.3f},",
                 tr.vmax, tr.amax, tr.jmax);
    std::fprintf(f, "\"cmds\":[");
    for (size_t i = 0; i < tr.cmds.size(); i++) {
        const Cmd& c = tr.cmds[i];
        std::fprintf(f, "%s{\"t\":%.4f,\"target\":%.4f,\"dur\":%.4f,\"vf\":%.4f,\"has_vf\":%s}",
                     i ? "," : "", (double)c.t_us * 1e-6, c.target,
                     (double)c.duration_us * 1e-6, c.end_vel,
                     c.has_end_vel ? "true" : "false");
    }
    std::fprintf(f, "],\"series\":[");
    for (size_t i = 0; i < tr.series.size(); i++) {
        const Series& s = tr.series[i];
        std::fprintf(f, "%s{\"name\":\"%s\",", i ? "," : "", s.name.c_str());
        writeArray(f, "t", s.t, 4); std::fprintf(f, ",");
        writeArray(f, "p", s.p, 5); std::fprintf(f, ",");
        writeArray(f, "v", s.v, 4); std::fprintf(f, ",");
        writeArray(f, "a", s.a, 3);
        std::fprintf(f, "}");
    }
    std::fprintf(f, "]}\n");
    std::fclose(f);
    std::printf("  wrote %s\n", path.c_str());
}

// ---- Engine runners ---------------------------------------------------------
// Run the command timeline through SlopMotion, sampling on the trace grid.
Series runSlopMotion(const std::vector<Cmd>& cmds, uint64_t t_end,
                     float start_pos, const slopmotion::Config& cfg,
                     const char* name = "slopmotion") {
    slopmotion::Engine e(cfg, start_pos);
    Series s; s.name = name;
    size_t ci = 0;
    for (uint64_t t = 0; t <= t_end; t += kMs) {   // 1 kHz internal stepping
        while (ci < cmds.size() && cmds[ci].t_us <= t) {
            slopmotion::Command c;
            c.target       = cmds[ci].target;
            c.duration_us  = cmds[ci].duration_us;
            c.has_duration = cmds[ci].duration_us > 0;
            c.end_vel      = cmds[ci].end_vel;
            c.has_end_vel  = cmds[ci].has_end_vel;
            e.commit(c, cmds[ci].t_us);
            ci++;
        }
        if (t % kSampleUs == 0) {
            s.t.push_back((double)t * 1e-6);
            s.p.push_back(e.positionAt(t));
            s.v.push_back(e.velocityAt(t));
            s.a.push_back(e.accelerationAt(t));
        } else {
            (void)e.positionAt(t);   // keep settle logic ticking like the sampler
        }
    }
    return s;
}

// Run the same timeline through the legacy cubic engine. Acceleration is not
// exposed by MotionInterpolator — central finite difference of velocityAt.
Series runCubic(const std::vector<Cmd>& cmds, uint64_t t_end, float start_pos) {
    MotionInterpolator e(start_pos);
    Series s; s.name = "cubic";
    size_t ci = 0;
    for (uint64_t t = 0; t <= t_end; t += kMs) {
        while (ci < cmds.size() && cmds[ci].t_us <= t) {
            InterpSegment seg;
            seg.targetPos   = cmds[ci].target;
            seg.durationUs  = cmds[ci].duration_us;
            seg.hasDuration = cmds[ci].duration_us > 0;
            seg.endSlope    = cmds[ci].end_vel * 1000.0f;  // wire G convention
            seg.hasSlope    = cmds[ci].has_end_vel;
            seg.isLivePoint = !seg.hasSlope && !seg.hasDuration;
            e.commit(seg, cmds[ci].t_us);
            ci++;
        }
        if (t % kSampleUs == 0) {
            s.t.push_back((double)t * 1e-6);
            s.p.push_back(e.positionAt(t));
            const double v = e.velocityAt(t);
            s.v.push_back(v);
            const uint64_t h = 2 * kMs;
            const double vm = e.velocityAt(t > h ? t - h : 0);
            const double vp = e.velocityAt(t + h);
            s.a.push_back((vp - vm) / (2.0 * (double)h * 1e-6));
        } else {
            (void)e.positionAt(t);   // drive its timeout/live machinery
        }
        InterpAnomaly ev;
        while (e.popAnomaly(ev)) {}  // drain, authentic to the sampler task
    }
    return s;
}

// Ideal reference curve (what the stream is sampled FROM).
Series referenceSine(double amp, double freq, double base, uint64_t t_end,
                     const char* name = "stream-source") {
    Series s; s.name = name;
    for (uint64_t t = 0; t <= t_end; t += kSampleUs) {
        const double ts = (double)t * 1e-6;
        s.t.push_back(ts);
        s.p.push_back(base + amp * std::sin(2.0 * M_PI * freq * ts));
        s.v.push_back(amp * 2.0 * M_PI * freq * std::cos(2.0 * M_PI * freq * ts));
        s.a.push_back(-amp * std::pow(2.0 * M_PI * freq, 2.0) *
                      std::sin(2.0 * M_PI * freq * ts));
    }
    return s;
}

void stats(const Trace& tr) {
    for (const Series& s : tr.series) {
        double maxv = 0, maxa = 0;
        for (double v : s.v) maxv = std::max(maxv, std::fabs(v));
        for (double a : s.a) maxa = std::max(maxa, std::fabs(a));
        std::printf("    %-14s peak |v|=%6.2f  peak |a|=%7.1f\n",
                    s.name.c_str(), maxv, maxa);
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const auto cfg = streamConfig();

    // ------------------------------------------------------------------
    // 1. v4 sparse stroke stream (MFP-style: I duration + G end slope)
    // ------------------------------------------------------------------
    {
        std::printf("scenario: v4-sparse\n");
        const double amp = 0.42, freq = 0.55, base = 0.5;
        const uint64_t interval = 700 * kMs, t_end = 6 * kS;
        std::vector<Cmd> cmds;
        for (uint64_t t = 200 * kMs; t + interval < t_end; t += interval) {
            const double te = (double)(t + interval) * 1e-6;   // segment end
            Cmd c;
            c.t_us        = t;
            c.target      = (float)(base + amp * std::sin(2 * M_PI * freq * te));
            c.duration_us = (uint32_t)interval;
            c.end_vel     = (float)(amp * 2 * M_PI * freq *
                                    std::cos(2 * M_PI * freq * te));
            c.has_end_vel = true;
            cmds.push_back(c);
        }
        Trace tr;
        tr.name  = "v4_sparse";
        tr.title = "TCode v4 sparse stream (700 ms points, I + G slope)";
        tr.note  = "One-shot mode: each point plans a deadline-stretched jerk-limited profile with the G-slope handoff velocity. The cubic is C1 (velocity-continuous) - watch its acceleration jump at every boundary; SlopMotion is C2.";
        tr.vmax = cfg.limits.vmax; tr.amax = cfg.limits.amax; tr.jmax = cfg.limits.jmax;
        tr.cmds = cmds;
        tr.series.push_back(runSlopMotion(cmds, t_end, 0.5f, cfg));
        tr.series.push_back(runCubic(cmds, t_end, 0.5f));
        tr.series.push_back(referenceSine(amp, freq, base, t_end));
        stats(tr); writeTrace(tr, dir);
    }

    // ------------------------------------------------------------------
    // 2. v3 dense bare-point stream, 60 Hz with arrival jitter
    // ------------------------------------------------------------------
    {
        std::printf("scenario: v3-dense\n");
        const double amp = 0.4, freq = 0.75, base = 0.5;
        const uint64_t t_end = 4 * kS;
        Lcg rng;
        std::vector<Cmd> cmds;
        for (uint64_t t = 100 * kMs; t < t_end; t += 16667) {
            Cmd c;
            const int64_t j = rng.jitterUs(3000);   // ±3 ms network jitter
            c.t_us   = (uint64_t)((int64_t)t + j);
            const double ts = (double)t * 1e-6;     // value sampled on the grid
            c.target = (float)(base + amp * std::sin(2 * M_PI * freq * ts));
            cmds.push_back(c);
        }
        Trace tr;
        tr.name  = "v3_dense";
        tr.title = "TCode v3 dense stream (60 Hz bare points, +/-3 ms jitter)";
        tr.note  = "Chase mode vs the cubic's live-extrapolation mode. Same jittery stream. Watch velocity smoothness and tracking lag.";
        tr.vmax = cfg.limits.vmax; tr.amax = cfg.limits.amax; tr.jmax = cfg.limits.jmax;
        tr.cmds = cmds;
        tr.series.push_back(runSlopMotion(cmds, t_end, 0.5f, cfg));
        tr.series.push_back(runCubic(cmds, t_end, 0.5f));
        tr.series.push_back(referenceSine(amp, freq, base, t_end));
        stats(tr); writeTrace(tr, dir);
    }

    // ------------------------------------------------------------------
    // 3. Stream starvation mid-glide
    // ------------------------------------------------------------------
    {
        std::printf("scenario: starve\n");
        const double amp = 0.4, freq = 0.75, base = 0.5;
        const uint64_t t_dead = 1500 * kMs, t_end = 3 * kS;
        std::vector<Cmd> cmds;
        for (uint64_t t = 100 * kMs; t < t_dead; t += 16667) {
            Cmd c;
            c.t_us   = t;
            const double ts = (double)t * 1e-6;
            c.target = (float)(base + amp * std::sin(2 * M_PI * freq * ts));
            cmds.push_back(c);
        }
        Trace tr;
        tr.name  = "starve";
        tr.title = "Stream dies mid-glide at t=1.5 s";
        tr.note  = "SlopMotion plans one jerk-limited brake-to-rest (velocity-interface settle); the cubic fires its parabolic DecelOverrun. Both must come to rest and hold.";
        tr.vmax = cfg.limits.vmax; tr.amax = cfg.limits.amax; tr.jmax = cfg.limits.jmax;
        tr.cmds = cmds;
        tr.series.push_back(runSlopMotion(cmds, t_end, 0.5f, cfg));
        tr.series.push_back(runCubic(cmds, t_end, 0.5f));
        stats(tr); writeTrace(tr, dir);
    }

    // ------------------------------------------------------------------
    // 4. Isolated bare-point retarget with a mid-flight reversal
    // ------------------------------------------------------------------
    {
        std::printf("scenario: step-retarget\n");
        std::vector<Cmd> cmds = {
            { 200 * kMs, 0.9f },
            { 750 * kMs, 0.1f },   // reversal while still moving toward 0.9
        };
        Trace tr;
        tr.name  = "step_retarget";
        tr.title = "Isolated points + mid-flight reversal (no timing data)";
        tr.note  = "The legacy engine treats an isolated bare point as a 50 ms hold-jump - it commands a physically absurd sweep and lets FAS clamp the wreckage. SlopMotion plans the reversal within the ceilings, C2-continuous through the direction change.";
        tr.vmax = cfg.limits.vmax; tr.amax = cfg.limits.amax; tr.jmax = cfg.limits.jmax;
        tr.cmds = cmds;
        tr.series.push_back(runSlopMotion(cmds, 3 * kS, 0.2f, cfg));
        tr.series.push_back(runCubic(cmds, 3 * kS, 0.2f));
        stats(tr); writeTrace(tr, dir);
    }

    // ------------------------------------------------------------------
    // 5. Infeasible deadline: full sweep demanded in 50 ms
    // ------------------------------------------------------------------
    {
        std::printf("scenario: deadline\n");
        std::vector<Cmd> cmds = { { 200 * kMs, 1.0f, 50 * (uint32_t)kMs } };
        Trace tr;
        tr.name  = "deadline";
        tr.title = "Full 0->1 sweep demanded in 50 ms (vmax makes >=500 ms physical)";
        tr.note  = "The cubic obeys the wire and commands a 20 units/s sweep (6.7x the ceiling - FAS is left to clamp it). SlopMotion stretches to the physical minimum under the ceilings and flags DeadlineStretched.";
        tr.vmax = cfg.limits.vmax; tr.amax = cfg.limits.amax; tr.jmax = cfg.limits.jmax;
        tr.cmds = cmds;
        tr.series.push_back(runSlopMotion(cmds, 1200 * kMs, 0.0f, cfg));
        tr.series.push_back(runCubic(cmds, 1200 * kMs, 0.0f));
        stats(tr); writeTrace(tr, dir);
    }

    // ------------------------------------------------------------------
    // 6. The feel knob: same one-shot move at four jerk ceilings
    // ------------------------------------------------------------------
    {
        std::printf("scenario: jmax-sweep\n");
        std::vector<Cmd> cmds = { { 100 * kMs, 0.9f, 600 * (uint32_t)kMs } };
        Trace tr;
        tr.name  = "jmax_sweep";
        tr.title = "Same 0.1->0.9 / 600 ms move, four jerk ceilings";
        tr.note  = "jmax is the feel knob: low = silky S-curve easing, high = approaches the trapezoid. Same target, same deadline, same vmax/amax.";
        tr.vmax = cfg.limits.vmax; tr.amax = cfg.limits.amax;
        tr.jmax = 0;   // varies per series
        tr.cmds = cmds;
        for (float j : { 50.0f, 150.0f, 500.0f, 2000.0f }) {
            auto c = streamConfig();
            c.limits.jmax = j;
            char nm[32];
            std::snprintf(nm, sizeof nm, "jmax=%g", (double)j);
            tr.series.push_back(runSlopMotion(cmds, 900 * kMs, 0.1f, c, nm));
        }
        stats(tr); writeTrace(tr, dir);
    }

    std::printf("done.\n");
    return 0;
}
