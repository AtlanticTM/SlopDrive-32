#pragma once

// MotionReplay — the analyzer's ASYNC TUNE mode: re-run a recorded session
// through slopmotion at whatever rate the host can manage, under a config the
// operator is moving live.
// Constraints:
//   PURELY OFFLINE. It owns its OWN Engine, SimStepper, SenderCurve and
//   PacingRing, touches no machine state, takes no locks, and reads no clock
//   except to report its own compute cost. Two consequences worth stating
//   plainly: a replay can run on the HTTP thread while the sim thread drives
//   the real virtual machine, and the SAME recording + SAME config always
//   produces the SAME samples — the substep grid is synthetic (i x 1 ms), never
//   wall time, so host jitter cannot enter the result the way it can on the live
//   path (which replays missed substeps to keep up with a real clock).
//   It is a LAB BENCH, not a control path: nothing here reaches the running sim
//   or SlopSync. The output of a session is knowledge — the numbers that become
//   compile-time defaults, and the list of knobs that turned out to be worth
//   exposing on the real machine.
//   Every rule it shares with the live path comes from MotionCore.h. It must
//   never grow a private copy of one; a tuner that renders differently than the
//   machine is worse than no tuner.

#include <cstdint>
#include <string>
#include <vector>

#include "MotionCore.h"
#include "slopmotion/slopmotion.hpp"

namespace slopsim {

// ---- A recording ------------------------------------------------------------
// One row of the wire recorder, as saved by `rec.save` — which is the SAME CSV
// GET /api/segments.csv serves, so a recording is also just a wire log an
// operator can diff against the funscript that was supposed to be sent.
//
// The RAW wire fields are what replay decodes from, deliberately: they are
// lossless integers, and running them back through decodeWireSample() means a
// recording renders through the identical scaling the live stream got. The
// decoded float columns in the CSV are for human eyes.
struct ReplayCommand {
    double   arrive_s = 0;      // when the bundle landed, re-based to 0
    double   due_s    = 0;      // resolved pacing due time, same base
    bool     is_segment = false;
    uint16_t raw_target = 0;
    uint16_t raw_dur_ms = 0;
    int16_t  raw_end_vel = 0;
    bool     accepted = true;   // false = refused at decode (0 ms duration)
    // RFC-030 EFFECTIVE curve family, from the recording's trailing column.
    // 0 on a recording written before that column existed — which resolves
    // FollowClient to quintic, i.e. exactly how those takes were rendered when
    // they were made. An old recording therefore still replays as itself.
    uint8_t  curve_family = 0;
};

struct Recording {
    std::string name;
    std::vector<ReplayCommand> cmds;
    // The VERBATIM source line for each command, index-parallel to `cmds`.
    // Clipping a region out of a recording (the analyzer's "save this stroke as
    // a fixture" action) is then a line-subset COPY rather than a reformat, so a
    // clip of a clip of a clip is still bit-identical to the wire log it came
    // from. Re-serializing through the float columns would round every hop.
    std::vector<std::string> lines;
    uint32_t segments = 0, samples = 0, rejected = 0;
    double span_s = 0;          // first arrival -> last due
};

// Returns false with `err` set. Tolerates the header line and blank lines; a row
// that does not parse is COUNTED, not silently dropped (see `err` on return
// true — a partial parse is a warning the caller shows, because half a recording
// that looks whole is exactly how a tuning session reaches a wrong conclusion).
bool loadRecording(const std::string& path, Recording& out, std::string& err);

// ---- What a replay is run under ---------------------------------------------
struct ReplayConfig {
    slopmotion::Config engine{};
    ArbiterGeometry    geom{};
    // Where the carriage starts, normalized. <0 means "the first commanded
    // target", which is the default for a REASON: a recording carries no record
    // of where the machine happened to be sitting when the take began, so any
    // other choice invents an opening lunge that never happened and then charges
    // its overshoot to whatever config is being evaluated.
    float p0_norm = -1.0f;
    // LAB override of the recording's declared curve family (<0 = use what the
    // recording carries). This is what makes "what if this client had declared
    // c2?" answerable on the bench without a client, and it is deliberately
    // SEPARATE from Config::curve_policy: the policy is the MACHINE's override
    // and lives on the wire, this is a what-if about the SENDER. Conflating
    // them would make it impossible to tell a machine-forced cubic from a
    // client-declared one in the same readout.
    int16_t client_curve_family = -1;
    // Seconds of quiet run appended after the last command is due, so the final
    // segment finishes and SETTLE has room to brake. Without it the tail of
    // every take is cropped mid-stroke.
    double tail_s = 1.0;
    // EMIT window (the analyzer's view bounds). Samples outside it are still
    // SIMULATED, just not returned, and the metrics are computed over the
    // emitted window only — so "zoom to one stroke and tune it" costs a small
    // response and gives numbers about the stroke you are looking at.
    //
    // SIMULATION ALWAYS STARTS AT THE RECORDING'S FIRST COMMAND, never at emit_t0,
    // and that is not laziness — it is the only correct choice. Most engine state
    // decays within a segment (each plan is rebuilt from actual state), but the
    // handoff series and the stream estimator do not: a window entered mid-take
    // would carry boundary conditions and a cadence estimate a cold start would
    // not have. Seeking into the middle would silently render DIFFERENT motion
    // than the machine's, which is precisely the class of lie this whole tool
    // exists to expose.
    // <0 on either bound = "no limit on that side".
    double emit_t0 = -1.0, emit_t1 = -1.0;
};

// ---- What comes back --------------------------------------------------------
// The numbers a tuning session is actually deciding on. Everything here is
// derived from the SAME samples the graph draws, so a claim in this block can
// always be checked by looking at the lines.
struct ReplayMetrics {
    uint32_t samples = 0;          // 1 ms trace rows produced
    uint32_t plans = 0;            // engine.commit() accepted
    uint32_t plan_rejected = 0;    // engine.commit() refused
    uint32_t anomalies = 0;
    uint32_t anom_kind[16] = {};   // indexed by slopmotion::AnomalyType
    float peak_vel_mm_s = 0;
    float peak_acc_mm_s2 = 0;
    // FOLLOWING error: what the MACHINE could not track (pos vs the setpoint it
    // was fed). Moves with the stepper's ceilings, not with slopmotion's config.
    float follow_rms_mm = 0, follow_max_mm = 0;
    // FIDELITY error: what the whole chain could not deliver of the SENDER'S own
    // curve (pos vs the raw line). THIS is the number a curve/policy A/B moves.
    float sender_rms_mm = 0, sender_max_mm = 0;
    // DC offset of achieved vs commanded position — the "band walked off center"
    // measurement. A shortened stroke is midpoint-anchored by construction (the
    // search's own geometry), so a nonzero value here is a real asymmetry.
    float band_center_err_mm = 0;
    float pos_min_mm = 0, pos_max_mm = 0;   // achieved travel
    float cmd_min_mm = 0, cmd_max_mm = 0;   // commanded travel, same window
    // ---- SHAPE metrics -------------------------------------------------------
    // RMS conflates four different failures into one number — offset, timing
    // lag, amplitude loss and shape loss — which is exactly how a stroke the
    // machine flattened from an S-curve into a ramp scores the same as one it
    // tracked perfectly but slightly late. These separate them.
    //
    //   reach   — achieved travel / the SENDER'S travel. <1 = amplitude
    //             surrendered, >1 = overshot. Says nothing about shape.
    //   shape   — Pearson correlation of achieved position against the sender's
    //             own curve. This is the operator's "parallelism": 1.0 means the
    //             machine took the same journey, whatever its size or offset.
    //   flat    — fraction of MOVING time the plan held a constant velocity. A
    //             constant-velocity plan is a STRAIGHT LINE in position, so this
    //             is the flattening, measured directly. It is the one that finds
    //             a waveform-fallback stroke; rms will not.
    float reach_ratio = 0;
    float shape_corr = 0;
    float flat_frac = 0;
    // ---- PER-SEGMENT EXCURSION ----------------------------------------------
    // How far the carriage went OUTSIDE the band each segment's own endpoints
    // describe — past its target, or backwards away from it before turning.
    //
    // WHY THIS IS NOT `pos_max` vs `cmd_max`: those are take-level extremes, so a
    // short move that flies 16 mm past its own endpoint is invisible whenever any
    // longer stroke in the same take reaches further. Measured on
    // OvershootTestThrobbing: take-level 0.05 mm, per-segment max 16.82 mm. The
    // take-level number is not a weaker version of this one, it is blind to the
    // defect entirely.
    //
    // `ratio` is excursion / that segment's OWN chord, which is the scale-free
    // form: > 1 means the carriage traveled further past the target than the move
    // was ever asked to cover. Chords below kSegChordEps are skipped rather than
    // divided into.
    float seg_over_mean_mm = 0;
    float seg_over_max_mm = 0;
    float seg_over_ratio_max = 0;
    uint32_t seg_scored = 0;       // segments the excursion metric could score
    float compute_ms = 0;          // wall-clock cost of this replay
};

struct ReplayResult {
    // Interleaved f32, kTraceStride floats per sample:
    // {t_s, pos_mm, tgt_mm, vel_mm_s, cmd_norm, raw_norm} — byte-identical to
    // what /api/trace.bin serves for the LIVE ring, so the analyzer page ingests
    // a replay with the code it already had.
    std::vector<float> trace;
    ReplayMetrics m;
};

ReplayResult replay(const Recording& rec, const ReplayConfig& cfg);

}  // namespace slopsim
