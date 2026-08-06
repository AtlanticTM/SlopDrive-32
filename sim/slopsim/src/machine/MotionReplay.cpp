// MotionReplay — offline re-run of a recorded session (see MotionReplay.h for
// the offline/determinism contract).

#include "machine/MotionReplay.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace slopsim {

namespace {

// Substep grid, matching the live sampler's. SYNTHETIC: instant i is exactly
// i x 1000 µs, so nothing about the host's scheduling can reach the result.
constexpr uint64_t kDtUs = 1000;

// Same gate the live drain applies: a stream that has been quiet for longer than
// this is a NEW stream, and a new stream soft-starts (SystemState::safeSpeedCap
// ramps its ceiling back up from the approach speed). It shapes the first ~1.2 s
// of every take, so a replay that skipped it would flatter every config equally
// and hide a real difference at the top of a scene.
constexpr uint32_t kNewStreamGapMs = 2000;

// Zero-based index of `curve_family` in MachineSim::ingressCsvHeader().
constexpr int kColCurveFamily = 17;

// Chord below which a segment's excursion RATIO is not reported. A re-anchor
// segment commands where the carriage already is, so its chord is ~0 and the
// ratio is a divide by noise; the absolute excursion is still scored.
constexpr float kSegChordEps = 0.05f;   // mm

// Reads one APPENDED column by position, returning `fallback` when the row is
// short. Kept separate from the main sscanf rather than widening it: widening
// would make every pre-existing recording fail the field-count check and be
// counted as malformed, which is the opposite of what an appended column means.
unsigned columnOr(const char* line, int idx, unsigned fallback) {
    int col = 0;
    for (const char* p = line; *p; ++p) {
        if (*p != ',') continue;
        if (++col != idx) continue;
        ++p;
        if (*p < '0' || *p > '9') return fallback;   // empty or non-numeric
        unsigned v = 0;
        for (; *p >= '0' && *p <= '9'; ++p) v = v * 10 + unsigned(*p - '0');
        return v;
    }
    return fallback;
}

}  // namespace

// ---- Loading ----------------------------------------------------------------

bool loadRecording(const std::string& path, Recording& out, std::string& err) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    out = Recording{};
    // The basename, for the picker and the graph caption.
    const size_t slash = path.find_last_of("/\\");
    out.name = slash == std::string::npos ? path : path.substr(slash + 1);

    char line[512];
    double t0 = 0;
    bool haveBase = false;
    uint32_t badRows = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r') continue;
        if (line[0] == 't' && line[1] == '_') continue;   // the header row

        // Column order is MachineSim::ingressCsvHeader()'s, which is the ONE
        // home for this schema — both the file export and /api/segments.csv
        // format through it, so a recording cannot have a layout this reader
        // does not expect. Suppressed conversions skip the decoded float
        // columns: replay decodes from the RAW wire integers instead (see
        // ReplayCommand).
        double t_s = 0;
        unsigned ch = 0, rawT = 0, rawDur = 0;
        int rawEv = 0, accepted = 1;
        unsigned long long due = 0;
        const int n = std::sscanf(line,
                                  "%lf,%x,%*[^,],%u,%u,%d,%*f,%*f,%*f,%*d,%d,%*d,%*u,%*u,%llu",
                                  &t_s, &ch, &rawT, &rawDur, &rawEv, &accepted, &due);
        if (n != 7) {
            ++badRows;
            continue;
        }
        if (!haveBase) {
            t0 = t_s;
            haveBase = true;
        }
        ReplayCommand c;
        c.arrive_s = t_s - t0;
        // due_us is absolute sim µs from the recording session; re-base it onto
        // the same zero as the arrivals so the sender's pacing is preserved
        // exactly, including a client whose timestamps ran ahead or behind.
        c.due_s = double(due) / 1e6 - t0;
        if (c.due_s < c.arrive_s) c.due_s = c.arrive_s;
        c.is_segment  = (rawDur != 0) || (ch == 0x0085);
        c.raw_target  = uint16_t(rawT);
        c.raw_dur_ms  = uint16_t(rawDur);
        c.raw_end_vel = int16_t(rawEv);
        c.accepted    = accepted != 0;
        c.curve_family = uint8_t(columnOr(line, kColCurveFamily, 0));
        if (c.is_segment) ++out.segments; else ++out.samples;
        if (!c.accepted) ++out.rejected;
        out.cmds.push_back(c);
        out.lines.emplace_back(line);
    }
    std::fclose(f);

    if (out.cmds.empty()) {
        err = badRows ? "no parsable rows (" + std::to_string(badRows) + " malformed)"
                      : "recording is empty";
        return false;
    }
    out.span_s = out.cmds.back().due_s;
    // A partial parse is reported on SUCCESS, not swallowed: half a recording
    // that looks whole is how a tuning session reaches a confident wrong answer.
    if (badRows) err = std::to_string(badRows) + " malformed row(s) skipped";
    return true;
}

// ---- Replay -----------------------------------------------------------------

ReplayResult replay(const Recording& rec, const ReplayConfig& cfg) {
    const auto t_start = std::chrono::steady_clock::now();
    ReplayResult out;
    if (rec.cmds.empty()) return out;

    // ---- the parallel motion chain, its own instance of every live piece ----
    slopmotion::Engine engine(cfg.engine, 0.5f);
    SimStepper  stepper;
    SenderCurve sender;
    PacingRing  ring;

    // Where the carriage starts — see ReplayConfig::p0_norm for why the first
    // commanded target is the default.
    float p0 = cfg.p0_norm;
    if (p0 < 0.0f) p0 = float(rec.cmds.front().raw_target) / 10000.0f;
    p0 = clampf(p0, 0.0f, 1.0f);
    const float p0_mm = clampf(cfg.geom.normToMm(p0), cfg.geom.win_min_mm, cfg.geom.win_max_mm);
    stepper.reset(p0_mm);
    engine.resetAt(p0, 0);

    const double end_s = rec.span_s + cfg.tail_s;
    const uint64_t steps = uint64_t(end_s * 1000.0) + 1;
    const double emit0 = cfg.emit_t0 < 0 ? -1e30 : cfg.emit_t0;
    const double emit1 = cfg.emit_t1 < 0 ?  1e30 : cfg.emit_t1;

    size_t nextCmd = 0;                 // recording cursor (arrivals are sorted)
    float  trace_cmd_norm = -1.0f;      // held between commands, like the live ring
    uint32_t resume_start_ms = 0, last_cmd_ms = 0;
    float  prev_vel = 0.0f;

    // Metric accumulators. Kept in double so a long take does not lose the tail
    // of the sum to float rounding.
    double follow_sq = 0, sender_sq = 0, dc_sum = 0;
    uint32_t follow_n = 0, sender_n = 0, dc_n = 0;
    bool first_emit = true, first_cmd = true;
    // Shape accumulators. The correlation runs over the same paired samples the
    // sender error does, so "shape" and "sender rms" always describe one set.
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    float raw_lo = 0, raw_hi = 0;
    bool  first_raw = true;
    uint32_t moving_n = 0, flat_n = 0;
    float prev_plan_v = 0.0f;
    bool  prev_plan_ok = false;
    // Per-segment excursion. `seg_open` spans one segment's commit to the next
    // commit of any kind, which is exactly how long that segment's plan can be
    // the thing moving the carriage.
    bool   seg_open = false;
    double seg_lo = 0, seg_hi = 0, seg_chord = 0, seg_excess = 0;
    double seg_over_sum = 0;

    out.trace.reserve(size_t(steps) * kTraceStride);

    for (uint64_t i = 0; i < steps; ++i) {
        const uint64_t t = i * kDtUs;
        const double   t_s = double(t) * 1e-6;
        const uint32_t t_ms = uint32_t(t / 1000);

        // ---- arrivals: push into the pacing ring at their RECORDED arrival --
        // Not at their due time. The 64-deep ring's overwrite behavior and the
        // RFC-008 one-segment lookahead both depend on what has ARRIVED but not
        // yet fired, so pushing on the due edge would hand the engine a
        // lookahead the machine never had.
        while (nextCmd < rec.cmds.size() && rec.cmds[nextCmd].arrive_s <= t_s) {
            const ReplayCommand& c = rec.cmds[nextCmd++];
            if (!c.accepted) continue;      // refused at decode on the wire too
            PacingEntry e{};
            if (!decodeWireSample(c.is_segment, c.raw_target, c.raw_dur_ms, c.raw_end_vel, e))
                continue;
            e.due_us = uint64_t(c.due_s * 1e6);
            e.curve_family = cfg.client_curve_family < 0 ? c.curve_family
                                                         : uint8_t(cfg.client_curve_family);
            ring.push(e);
        }

        // ---- drain + commit, at THIS substep's instant ----------------------
        // Same ordering rule as the live loop: commit at t, sample at t, so a
        // fresh plan is "current" for exactly one 1 ms sample.
        PacingEntry entry;
        while (ring.popDue(t, entry)) {
            // The gates the live drain applies that MOVE THE CARRIAGE. The
            // homed/paused/override checks are deliberately absent: a recording
            // replays as a homed, running machine, because a take made through
            // closed gates has no motion in it to tune.
            if (last_cmd_ms == 0 || (t_ms - last_cmd_ms) > kNewStreamGapMs)
                resume_start_ms = t_ms;
            last_cmd_ms = t_ms;

            slopmotion::Command cmd;
            cmd.target       = entry.target;
            cmd.end_vel      = entry.vel;
            cmd.has_end_vel  = entry.has_end_vel;
            cmd.duration_us  = entry.duration_us;
            cmd.has_duration = entry.has_duration;
            cmd.client_curve_family = entry.curve_family;   // RFC-030

            // RFC-008 one-segment lookahead, verbatim from the live drain.
            if (entry.has_duration && entry.has_end_vel) {
                const PacingEntry* next = ring.peekOldest();
                if (next && next->has_duration && next->duration_us > 0) {
                    const float next_dur_s = float(next->duration_us) * 1e-6f;
                    cmd.next_chord     = std::fabs(next->target - entry.target) / next_dur_s;
                    cmd.has_next_chord = true;
                }
            }

            // Close the outgoing segment before the new plan can move anything,
            // then open the incoming one against the position the ENGINE is
            // planning from (commitWaveform's `p`) rather than the stepper's,
            // which lags it — the band the plan draws is the one being scored.
            if (seg_open) {
                seg_over_sum += seg_excess;
                ++out.m.seg_scored;
                if (seg_excess > out.m.seg_over_max_mm)
                    out.m.seg_over_max_mm = float(seg_excess);
                if (seg_chord > kSegChordEps) {
                    const float r = float(seg_excess / seg_chord);
                    if (r > out.m.seg_over_ratio_max) out.m.seg_over_ratio_max = r;
                }
                seg_open = false;
            }
            if (cmd.has_duration) {
                const double p_mm = cfg.geom.normToMm(engine.positionAt(t));
                const double e_mm = cfg.geom.normToMm(cmd.target);
                seg_lo = p_mm < e_mm ? p_mm : e_mm;
                seg_hi = p_mm < e_mm ? e_mm : p_mm;
                seg_chord = seg_hi - seg_lo;
                seg_excess = 0;
                // Scored only if the segment STARTS inside the emit window, but
                // then followed to its end whatever the window does — half a
                // segment's excursion is not a smaller excursion, it is a wrong
                // one.
                seg_open = t_s >= emit0 && t_s <= emit1;
            }

            trace_cmd_norm = cmd.target;
            // Snapshot behind the ternary — only the FIRST segment may sample
            // the engine here (SenderCurve::anchored()).
            sender.note(cmd, t,
                        slopmotion::resolveCubic(cfg.engine.curve_policy,
                                                 cmd.client_curve_family),
                        sender.anchored() ? 0.0f : engine.snapshot(t).pos);
            if (engine.commit(cmd, t)) ++out.m.plans; else ++out.m.plan_rejected;
        }

        slopmotion::Anomaly ev;
        while (engine.popAnomaly(ev)) {
            ++out.m.anomalies;
            if (ev.kind < 16) ++out.m.anom_kind[ev.kind];
        }

        // ---- sample + arbiter + integrate -----------------------------------
        // Named locals in this order: both calls run maybeSettle() and can
        // transition the engine, and argument evaluation order is unspecified.
        const float norm     = engine.positionAt(t);
        const float vel_norm = engine.velocityAt(t);
        const auto sc = applyArbiter(cfg.geom, norm, vel_norm, stepper.positionMm(),
                                     resume_start_ms, t_ms);
        stepper.command(sc.target_mm, sc.speed_mm_s, sc.accel_mm_s2);
        stepper.step(0.001f);

        const float pos = stepper.positionMm();
        const float tgt = stepper.targetMm();
        const float vel = stepper.velocityMmS();
        const float raw = sender.sampleAt(t);

        // Excursion runs OUTSIDE the emit gate: a segment admitted at its commit
        // is followed to its close even if the view ends first.
        if (seg_open) {
            const double ex = pos < seg_lo ? seg_lo - pos
                            : pos > seg_hi ? pos - seg_hi : 0.0;
            if (ex > seg_excess) seg_excess = ex;
        }

        if (t_s < emit0 || t_s > emit1) {
            prev_vel = vel;
            continue;
        }

        // ---- metrics, over the EMITTED window only --------------------------
        const float acc = (vel - prev_vel) / 0.001f;
        prev_vel = vel;
        if (std::fabs(vel) > out.m.peak_vel_mm_s) out.m.peak_vel_mm_s = std::fabs(vel);
        if (std::fabs(acc) > out.m.peak_acc_mm_s2) out.m.peak_acc_mm_s2 = std::fabs(acc);

        const float fe = std::fabs(pos - tgt);
        follow_sq += double(fe) * fe;
        ++follow_n;
        if (fe > out.m.follow_max_mm) out.m.follow_max_mm = fe;

        if (raw >= 0.0f) {
            const float raw_mm = cfg.geom.normToMm(raw);
            const float se = std::fabs(pos - raw_mm);
            sender_sq += double(se) * se;
            ++sender_n;
            if (se > out.m.sender_max_mm) out.m.sender_max_mm = se;
            // Shape: paired sums for a Pearson r of achieved vs the sender's
            // own curve, plus the sender's travel for the reach ratio.
            sx += raw_mm; sy += pos;
            sxx += double(raw_mm) * raw_mm; syy += double(pos) * pos;
            sxy += double(raw_mm) * pos;
            if (first_raw || raw_mm < raw_lo) raw_lo = raw_mm;
            if (first_raw || raw_mm > raw_hi) raw_hi = raw_mm;
            first_raw = false;
        }

        // Flattening: a plan holding a CONSTANT velocity is a straight line in
        // position. Compared against the previous sample rather than a window,
        // because the fallback's cruise is exactly constant — it does not drift.
        // Gated on actually moving, so dwell does not read as flat.
        const float pv = vel_norm;
        if (std::fabs(pv) > 0.05f) {
            ++moving_n;
            if (prev_plan_ok && std::fabs(pv - prev_plan_v) < 0.002f * std::fabs(pv)) ++flat_n;
        }
        prev_plan_v = pv;
        prev_plan_ok = true;
        if (trace_cmd_norm >= 0.0f) {
            const float cmd_mm = cfg.geom.normToMm(trace_cmd_norm);
            dc_sum += double(pos) - double(cmd_mm);
            ++dc_n;
            // Seeded off ITS OWN first sample, not the emit window's. A take
            // whose first emitted sample predates the first command (the usual
            // case — the carriage is already being drawn before anything is
            // commanded) left these two at their zero-initialized value, so the
            // readout reported a commanded travel of 0.0 mm on a window that
            // starts at 100.
            if (first_cmd || cmd_mm < out.m.cmd_min_mm) out.m.cmd_min_mm = cmd_mm;
            if (first_cmd || cmd_mm > out.m.cmd_max_mm) out.m.cmd_max_mm = cmd_mm;
            first_cmd = false;
        }
        if (first_emit || pos < out.m.pos_min_mm) out.m.pos_min_mm = pos;
        if (first_emit || pos > out.m.pos_max_mm) out.m.pos_max_mm = pos;
        first_emit = false;

        out.trace.push_back(float(t_s));
        out.trace.push_back(pos);
        out.trace.push_back(tgt);
        out.trace.push_back(vel);
        out.trace.push_back(trace_cmd_norm);
        out.trace.push_back(raw);
        out.trace.push_back(float(uint8_t(engine.planKind())));
        out.trace.push_back(vel_norm);
        ++out.m.samples;
    }

    // The last segment of a take never sees a successor commit to close it.
    if (seg_open) {
        seg_over_sum += seg_excess;
        ++out.m.seg_scored;
        if (seg_excess > out.m.seg_over_max_mm) out.m.seg_over_max_mm = float(seg_excess);
        if (seg_chord > kSegChordEps) {
            const float r = float(seg_excess / seg_chord);
            if (r > out.m.seg_over_ratio_max) out.m.seg_over_ratio_max = r;
        }
    }
    if (out.m.seg_scored) out.m.seg_over_mean_mm = float(seg_over_sum / out.m.seg_scored);

    if (follow_n) out.m.follow_rms_mm = float(std::sqrt(follow_sq / follow_n));
    if (sender_n) out.m.sender_rms_mm = float(std::sqrt(sender_sq / sender_n));
    if (dc_n)     out.m.band_center_err_mm = float(dc_sum / dc_n);

    // ---- shape metrics ------------------------------------------------------
    if (sender_n > 1) {
        const double n = double(sender_n);
        const double cov = sxy - sx * sy / n;
        const double vx  = sxx - sx * sx / n;
        const double vy  = syy - sy * sy / n;
        // A degenerate axis (a take that never moved) has no correlation to
        // report; 0 is the honest answer, not a divide.
        if (vx > 1e-9 && vy > 1e-9) out.m.shape_corr = float(cov / std::sqrt(vx * vy));
        const float sender_travel = raw_hi - raw_lo;
        const float achieved = out.m.pos_max_mm - out.m.pos_min_mm;
        if (sender_travel > 1e-3f) out.m.reach_ratio = achieved / sender_travel;
    }
    if (moving_n) out.m.flat_frac = float(double(flat_n) / moving_n);

    out.m.compute_ms = float(std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - t_start)
                                 .count());
    return out;
}

}  // namespace slopsim
