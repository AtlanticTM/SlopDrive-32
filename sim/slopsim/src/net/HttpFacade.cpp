// HttpFacade — read-only HTTP handlers (see HttpFacade.h for the doctrine).

#include "net/HttpFacade.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <httplib.h>

#include <cstring>
#include <memory>
#include <vector>

#include "machine/MachineSim.h"
#include "machine/MotionReplay.h"
#include "machine/RecordingStore.h"
#include "net/GraphPage.h"

namespace slopsim {

namespace {

// ---- Query-parameter plumbing for the async-tune bench ----------------------
// Every knob is a query parameter rather than a JSON body for two reasons: the
// sim carries no JSON parser (and one is not worth adding to read 30 scalars),
// and a tuning state that IS a URL can be pasted into a bug report, curled from
// a script, or bookmarked as "the settings that felt right".
//
// Absent parameter = keep whatever the caller seeded, which is the LIVE sim's
// current setting. So the panel opens on what the machine is actually doing and
// each control only overrides itself.

float qf(const httplib::Request& r, const char* k, float dflt) {
    if (!r.has_param(k)) return dflt;
    const std::string& s = r.get_param_value(k);
    if (s.empty()) return dflt;
    return float(std::atof(s.c_str()));
}

bool qb(const httplib::Request& r, const char* k, bool dflt) {
    if (!r.has_param(k)) return dflt;
    const std::string& s = r.get_param_value(k);
    if (s.empty()) return dflt;
    return !(s == "0" || s == "false" || s == "off");
}

uint8_t qsteps(const httplib::Request& r, const char* k, uint8_t dflt, uint8_t lo, uint8_t hi) {
    if (!r.has_param(k)) return dflt;
    const int v = std::atoi(r.get_param_value(k).c_str());
    return uint8_t(v < lo ? lo : (v > hi ? hi : v));
}

const char* policyName(slopmotion::InfeasiblePolicy p) {
    switch (p) {
        case slopmotion::InfeasiblePolicy::Stretch: return "stretch";
        case slopmotion::InfeasiblePolicy::Scale:   return "scale";
        case slopmotion::InfeasiblePolicy::Reshape: return "reshape";
        case slopmotion::InfeasiblePolicy::PrioritizeAmplitude: return "prio-amplitude";
        case slopmotion::InfeasiblePolicy::PrioritizeSmooth:    return "prio-smooth";
        case slopmotion::InfeasiblePolicy::Blend:   return "blend";
    }
    return "?";
}

const char* curveName(slopmotion::CurvePolicy c) {
    switch (c) {
        case slopmotion::CurvePolicy::ForceC1: return "c1";
        case slopmotion::CurvePolicy::ForceC2: return "c2";
        case slopmotion::CurvePolicy::FollowClient: return "follow";
    }
    return "?";
}

// Clamps mirror the ones the engine documents on each Config field. They are
// applied HERE as well as there because a query string is not a trusted input:
// the engine clamping on use protects the engine, this protects the readback —
// a panel that echoes 900 for a field the engine will treat as 8 is a
// ground-truth defect even though the motion is correct.
ReplayConfig replayConfigFrom(const httplib::Request& r, MachineSim* sim) {
    ReplayConfig cfg;
    cfg.engine = sim->engineConfig();     // seed: what the live machine runs
    cfg.geom   = sim->arbiterGeometry();

    const std::string pol = r.has_param("policy") ? r.get_param_value("policy") : "";
    if (pol == "stretch") cfg.engine.infeasible_policy = slopmotion::InfeasiblePolicy::Stretch;
    else if (pol == "scale")   cfg.engine.infeasible_policy = slopmotion::InfeasiblePolicy::Scale;
    else if (pol == "reshape") cfg.engine.infeasible_policy = slopmotion::InfeasiblePolicy::Reshape;
    else if (pol == "prio-amplitude" || pol == "amp")
        cfg.engine.infeasible_policy = slopmotion::InfeasiblePolicy::PrioritizeAmplitude;
    else if (pol == "prio-smooth" || pol == "smooth")
        cfg.engine.infeasible_policy = slopmotion::InfeasiblePolicy::PrioritizeSmooth;
    else if (pol == "blend")
        cfg.engine.infeasible_policy = slopmotion::InfeasiblePolicy::Blend;

    const std::string cv = r.has_param("curve") ? r.get_param_value("curve") : "";
    if (cv == "c1") cfg.engine.curve_policy = slopmotion::CurvePolicy::ForceC1;
    else if (cv == "c2") cfg.engine.curve_policy = slopmotion::CurvePolicy::ForceC2;
    else if (cv == "follow") cfg.engine.curve_policy = slopmotion::CurvePolicy::FollowClient;

    // What the SENDER declared (RFC-030), overridden for what-if runs. Distinct
    // from `curve` above, which is the MACHINE's override — see
    // ReplayConfig::client_curve_family. Absent = whatever the recording carries.
    const std::string cf = r.has_param("client_curve") ? r.get_param_value("client_curve") : "";
    if (cf == "unspecified") cfg.client_curve_family = 0;
    else if (cf == "c1") cfg.client_curve_family = 1;
    else if (cf == "c2") cfg.client_curve_family = 2;
    else if (cf == "step") cfg.client_curve_family = 3;

    auto& e = cfg.engine;
    e.infeasible_scale_margin = clampf(qf(r, "scale_margin", e.infeasible_scale_margin), 0.50f, 1.0f);
    e.infeasible_reshape_steps = qsteps(r, "reshape_steps", e.infeasible_reshape_steps, 0, 8);
    e.infeasible_smooth_budget = clampf(qf(r, "smooth_budget", e.infeasible_smooth_budget), 0.0f, 1.0f);
    e.infeasible_amplitude_budget =
        clampf(qf(r, "amplitude_budget", e.infeasible_amplitude_budget), 0.0f, 1.0f);
    e.infeasible_blend_steps = qsteps(r, "blend_steps", e.infeasible_blend_steps, 1, 10);
    e.infeasible_blend = clampf(qf(r, "blend", e.infeasible_blend), 0.0f, 1.0f);
    // The three sharpness knobs and the handoff factor have NO live setter on
    // the sim or the device — they exist in Config and nowhere else. Reaching
    // them is one of the reasons this bench exists (see the panel's `lab` tags).
    e.infeasible_soften = qb(r, "soften", e.infeasible_soften);
    e.infeasible_soften_floor = clampf(qf(r, "soften_floor", e.infeasible_soften_floor), 0.001f, 1.0f);
    e.infeasible_soften_steps = qsteps(r, "soften_steps", e.infeasible_soften_steps, 0, 10);
    e.handoff_chord_factor = clampf(qf(r, "handoff_chord", e.handoff_chord_factor), 0.0f, 8.0f);
    // The overshoot guard: a physical floor (guard) plus a share of the
    // segment's own chord (chord_slack), and the bad-move bridge beside it.
    e.overshoot_guard = clampf(qf(r, "overshoot_guard", e.overshoot_guard), 0.0f, 20.0f);
    e.bridge_ratio    = clampf(qf(r, "bridge_ratio", e.bridge_ratio), 0.0f, 50.0f);
    e.overshoot_chord_slack = clampf(qf(r, "chord_slack", e.overshoot_chord_slack), 0.0f, 5.0f);

    e.wave_centering = qb(r, "wave_centering", e.wave_centering);
    e.wave_centering_gain = clampf(qf(r, "wave_centering_gain", e.wave_centering_gain), 0.0f, 1.0f);
    e.settle_grace_us =
        uint32_t(clampf(qf(r, "settle_grace_ms", float(e.settle_grace_us) / 1000.0f), 0.0f, 200.0f) *
                 1000.0f);

    e.chase_feedforward = qb(r, "chase_ff", e.chase_feedforward);
    e.chase_ff_gain = clampf(qf(r, "chase_gain", e.chase_ff_gain), 0.0f, 2.0f);
    e.chase_lookahead = clampf(qf(r, "chase_lookahead", e.chase_lookahead), 0.0f, 20.0f);
    e.chase_accel_ff = qb(r, "chase_accel_ff", e.chase_accel_ff);
    e.chase_aim_accel_extrap = qb(r, "chase_aim_extrap", e.chase_aim_accel_extrap);
    e.chase_dense_us =
        uint32_t(clampf(qf(r, "chase_dense_ms", float(e.chase_dense_us) / 1000.0f), 1.0f, 1000.0f) *
                 1000.0f);
    e.chase_stale_us =
        uint32_t(clampf(qf(r, "chase_stale_ms", float(e.chase_stale_us) / 1000.0f), 1.0f, 5000.0f) *
                 1000.0f);

    auto& g = cfg.geom;
    g.win_min_mm = qf(r, "win_min", g.win_min_mm);
    g.win_max_mm = qf(r, "win_max", g.win_max_mm);
    if (g.win_max_mm - g.win_min_mm < kMinWindowSpanMm) g.win_max_mm = g.win_min_mm + kMinWindowSpanMm;
    g.ceiling_mm = qf(r, "ceiling", g.ceiling_mm);
    g.input_speed = clampf(qf(r, "input_speed", g.input_speed), 1.0f, 10000.0f);
    g.input_accel = clampf(qf(r, "input_accel", g.input_accel), 1.0f, 100000.0f);
    g.input_jerk = clampf(qf(r, "input_jerk", g.input_jerk), 1.0f, 1e8f);
    g.user_speed = clampf(qf(r, "user_speed", g.user_speed), 1.0f, 10000.0f);
    g.user_accel = clampf(qf(r, "user_accel", g.user_accel), 1.0f, 100000.0f);
    g.stream_speed_mode = qb(r, "matched", g.stream_speed_mode == kSpeedVelocityMatched)
                              ? kSpeedVelocityMatched
                              : kSpeedCeilingPegged;
    // Bench-only: the proposed fix for the window-exit braking trap, so it can be
    // MEASURED before anyone edits firmware. See ArbiterGeometry.
    // Seeded from sim->arbiterGeometry() above, so an un-parameterized replay
    // renders what the LIVE machine is currently configured to do.
    g.gentle_accel_outside = qb(r, "gentle_accel_outside", g.gentle_accel_outside);
    g.safety_filter = qb(r, "safety_filter", g.safety_filter);

    // ---- Normalized ceilings, DERIVED FROM THE GEOMETRY ABOVE ---------------
    // Ordered after the window on purpose: the engine plans in window fractions,
    // so its ceilings are a function of the span the operator just set. This
    // used to inherit the LIVE sim's limits and let the window move
    // independently, which meant a 100 mm bench window still planned at the
    // 500 mm machine's 2.0/s — a fifth of the authority the real machine would
    // have had at that window, and enough on its own to make every infeasible
    // policy look bad.
    //
    // 0 (or absent) = derive, non-zero = override, which is the firmware's own
    // `jovr`/sm-set semantics rather than a bench invention. So "what if jmax
    // were half that" still works; it just has to be asked explicitly.
    e.limits = deriveLimits(g,
                            clampf(qf(r, "vmax", 0.0f), 0.0f, 100.0f),
                            clampf(qf(r, "amax", 0.0f), 0.0f, 5000.0f),
                            clampf(qf(r, "jmax", 0.0f), 0.0f, 1e6f));

    cfg.p0_norm = qf(r, "p0", cfg.p0_norm);
    cfg.tail_s  = double(clampf(qf(r, "tail_s", float(cfg.tail_s)), 0.0f, 30.0f));
    cfg.emit_t0 = r.has_param("t0") ? std::atof(r.get_param_value("t0").c_str()) : -1.0;
    cfg.emit_t1 = r.has_param("t1") ? std::atof(r.get_param_value("t1").c_str()) : -1.0;
    return cfg;
}

// The settings block a saved run carries. Built from the RESOLVED config (post
// clamp), never from the query string, so a run records what actually ran.
RunSettings settingsFor(const ReplayConfig& cfg, const std::string& recording) {
    RunSettings s;
    const auto& e = cfg.engine;
    const auto& g = cfg.geom;
    s.set("recording", recording);
    s.set("curve", curveName(e.curve_policy));
    // "recording" = no override, i.e. the family the take was actually sent
    // under. Recorded because a run saved WITHOUT it is indistinguishable from
    // one saved before the column existed.
    s.set("client_curve", cfg.client_curve_family < 0 ? "recording"
          : cfg.client_curve_family == 1 ? "c1"
          : cfg.client_curve_family == 2 ? "c2"
          : cfg.client_curve_family == 3 ? "step" : "unspecified");
    s.set("policy", policyName(e.infeasible_policy));
    s.set("scale_margin", e.infeasible_scale_margin, 3);
    s.set("reshape_steps", double(e.infeasible_reshape_steps), 0);
    s.set("smooth_budget", e.infeasible_smooth_budget, 3);
    s.set("amplitude_budget", e.infeasible_amplitude_budget, 3);
    s.set("blend_steps", double(e.infeasible_blend_steps), 0);
    s.set("blend", e.infeasible_blend, 3);
    s.set("soften", e.infeasible_soften ? "1" : "0");
    s.set("soften_floor", e.infeasible_soften_floor, 4);
    s.set("soften_steps", double(e.infeasible_soften_steps), 0);
    s.set("handoff_chord", e.handoff_chord_factor, 3);
    s.set("overshoot_guard", e.overshoot_guard, 3);
    s.set("chord_slack", e.overshoot_chord_slack, 3);
    s.set("bridge_ratio", e.bridge_ratio, 3);
    s.set("wave_centering", e.wave_centering ? "1" : "0");
    s.set("wave_centering_gain", e.wave_centering_gain, 3);
    s.set("settle_grace_ms", double(e.settle_grace_us) / 1000.0, 1);
    s.set("chase_ff", e.chase_feedforward ? "1" : "0");
    s.set("chase_gain", e.chase_ff_gain, 3);
    s.set("chase_lookahead", e.chase_lookahead, 2);
    s.set("chase_accel_ff", e.chase_accel_ff ? "1" : "0");
    s.set("chase_aim_extrap", e.chase_aim_accel_extrap ? "1" : "0");
    s.set("chase_dense_ms", double(e.chase_dense_us) / 1000.0, 1);
    s.set("chase_stale_ms", double(e.chase_stale_us) / 1000.0, 1);
    s.set("vmax", e.limits.vmax, 4);
    s.set("amax", e.limits.amax, 3);
    s.set("jmax", e.limits.jmax, 1);
    s.set("win_min", g.win_min_mm, 2);
    s.set("win_max", g.win_max_mm, 2);
    s.set("ceiling", g.ceiling_mm, 2);
    s.set("input_speed", g.input_speed, 1);
    s.set("input_accel", g.input_accel, 1);
    s.set("user_speed", g.user_speed, 1);
    s.set("user_accel", g.user_accel, 1);
    s.set("matched", g.stream_speed_mode == kSpeedVelocityMatched ? "1" : "0");
    s.set("gentle_accel_outside", g.gentle_accel_outside ? "1" : "0");
    s.set("safety_filter", g.safety_filter ? "1" : "0");
    s.set("p0", cfg.p0_norm, 4);
    s.set("tail_s", cfg.tail_s, 2);
    return s;
}

// Metrics as a one-line JSON object. Rides in a RESPONSE HEADER on the binary
// replay feed so one request produces both the samples and the numbers — the
// alternative (a second endpoint) would double the replay cost to answer a
// question about the same run.
std::string statsJson(const ReplayMetrics& m, const Recording& rec, const std::string& warn) {
    std::string byKind;
    for (size_t k = 0; k < kSmAnomalyNameCount; ++k) {
        char kb[64];
        std::snprintf(kb, sizeof(kb), "%s\"%s\":%u", k ? "," : "", kSmAnomalyNames[k],
                      unsigned(m.anom_kind[k]));
        byKind += kb;
    }
    // snprintf TRUNCATES SILENTLY and a clipped stats block is invalid JSON,
    // not a short one — the same trap the tuning block carries a note about.
    char buf[1440];
    std::snprintf(buf, sizeof(buf),
                  "{\"samples\":%u,\"plans\":%u,\"plan_rejected\":%u,\"anomalies\":%u,"
                  "\"peak_vel_mm_s\":%.1f,\"peak_acc_mm_s2\":%.0f,"
                  "\"follow_rms_mm\":%.3f,\"follow_max_mm\":%.3f,"
                  "\"sender_rms_mm\":%.3f,\"sender_max_mm\":%.3f,"
                  "\"band_center_err_mm\":%.3f,"
                  "\"reach_ratio\":%.4f,\"shape_corr\":%.4f,\"flat_frac\":%.4f,"
                  "\"seg_over_mean_mm\":%.3f,\"seg_over_max_mm\":%.3f,"
                  "\"seg_over_ratio_max\":%.3f,\"seg_scored\":%u,"
                  "\"pos_min_mm\":%.2f,\"pos_max_mm\":%.2f,"
                  "\"cmd_min_mm\":%.2f,\"cmd_max_mm\":%.2f,"
                  "\"compute_ms\":%.2f,\"rec_segments\":%u,\"rec_samples\":%u,"
                  "\"rec_rejected\":%u,\"rec_span_s\":%.3f,\"warn\":\"%s\","
                  "\"anomalies_by_kind\":{%s}}",
                  unsigned(m.samples), unsigned(m.plans), unsigned(m.plan_rejected),
                  unsigned(m.anomalies), double(m.peak_vel_mm_s), double(m.peak_acc_mm_s2),
                  double(m.follow_rms_mm), double(m.follow_max_mm), double(m.sender_rms_mm),
                  double(m.sender_max_mm), double(m.band_center_err_mm),
                  double(m.reach_ratio), double(m.shape_corr), double(m.flat_frac),
                  double(m.seg_over_mean_mm), double(m.seg_over_max_mm),
                  double(m.seg_over_ratio_max), unsigned(m.seg_scored),
                  double(m.pos_min_mm),
                  double(m.pos_max_mm), double(m.cmd_min_mm), double(m.cmd_max_mm),
                  double(m.compute_ms), unsigned(rec.segments), unsigned(rec.samples),
                  unsigned(rec.rejected), rec.span_s, warn.c_str(), byKind.c_str());
    return buf;
}

// The 20-byte header + interleaved f32 body that /api/trace.bin already serves.
// Replay and stored-run responses reuse it verbatim so the analyzer ingests all
// three sources with the code it already had.
std::string traceBody(const std::vector<float>& recs, float rail, float wmin, float wmax) {
    const uint32_t stride = kTraceStride;
    const uint32_t n = uint32_t(recs.size() / stride);
    std::string body(20 + recs.size() * sizeof(float), '\0');
    std::memcpy(body.data(), &n, 4);
    std::memcpy(body.data() + 4, &rail, 4);
    std::memcpy(body.data() + 8, &wmin, 4);
    std::memcpy(body.data() + 12, &wmax, 4);
    std::memcpy(body.data() + 16, &stride, 4);
    if (!recs.empty()) std::memcpy(body.data() + 20, recs.data(), recs.size() * sizeof(float));
    return body;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        o.push_back(c);
    }
    return o;
}

}  // namespace

HttpFacade::HttpFacade() = default;
HttpFacade::~HttpFacade() { stop(); }

bool HttpFacade::begin(MachineSim* sim, uint16_t httpPort, uint16_t wsPort, SessionLog* log,
                       std::string webuiPath, std::string recordingsDir) {
    _srv = std::make_unique<httplib::Server>();
    auto store = std::make_shared<RecordingStore>(std::move(recordingsDir));
    // Announced at boot, not discovered at save time. The shelf's location is
    // resolved from the EXE's directory (never the working directory — see
    // RecordingStore), and an operator who cannot write there needs to know
    // before they record a take, not after.
    if (log) {
        log->logf(store->writable() ? 'I' : 'E', "async-tune shelf: %s%s", store->dir().c_str(),
                  store->writable() ? "" : "  *** NOT WRITABLE — saves will fail ***");
    }

    _srv->set_default_headers({{"Access-Control-Allow-Origin", "*"}});

    _srv->Get("/api/capabilities", [sim, wsPort](const httplib::Request&, httplib::Response& res) {
        const auto st = sim->facadeStats();
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "{\"fw_version\":\"slopsim-0.2.0\",\"sim\":true,"
                      "\"max_rail_mm\":%.1f,\"max_travel_mm\":%.1f,"
                      "\"slopsync_port\":%u,\"slopsync_proto\":\"slopsync.v1\","
                      "\"features\":{\"slopsync\":true,\"sim\":true,\"motion_backend\":\"sim\"}}",
                      double(st.max_rail_mm), double(st.max_rail_mm), unsigned(wsPort));
        res.set_content(buf, "application/json");
    });

    // "sync" keeps DEVICE semantics (sm_sync_* parity — bundles/samples/
    // enqueued/dropped). Everything sim-specific lives under "simstats" so a
    // client can't mistake a host artifact for a device counter.
    _srv->Get("/api/slopmotion", [sim](const httplib::Request&, httplib::Response& res) {
        const auto st = sim->facadeStats();
        const auto ing = sim->ingressStats();
        const auto& ec = sim->engineConfig();

        // "tuning" mirrors the DEVICE's block key-for-key (WebUI.cpp
        // handleApiSlopMotion) so sim and device responses diff directly — the
        // whole reason the facade exists. Read straight off the ENGINE's live
        // Config, never a shadow copy, so this cannot lie about what is in
        // force (ground-truth doctrine applies to the sim too).
        // settle_grace_ms: MILLISECONDS on the wire, µs in the engine — same
        // unit boundary the device draws, drawn in the same place.
        // Was a SECOND hand-written name chain here and it had already drifted:
        // it predated `blend` and reported the live default as "?". One home —
        // policyName() above is a switch, so the compiler names the next policy
        // that gets added instead of a readout quietly lying about it.
        const char* polName = policyName(ec.infeasible_policy);
        // 768: the budgeted-policy trio added ~90 bytes and snprintf TRUNCATES
        // SILENTLY — a clipped tuning block is invalid JSON, not a short one.
        const char* curveName =
            ec.curve_policy == slopmotion::CurvePolicy::ForceC1 ? "c1"
          : ec.curve_policy == slopmotion::CurvePolicy::ForceC2 ? "c2"
                                                                : "follow";
        char tuningBuf[896];
        std::snprintf(tuningBuf, sizeof(tuningBuf),
                      "\"tuning\":{\"curve_policy\":\"%s\","
                      "\"infeasible_policy\":\"%s\","
                      "\"infeasible_scale_margin\":%.3f,\"reshape_steps\":%u,"
                      "\"smooth_budget\":%.3f,\"amplitude_budget\":%.3f,\"blend\":%.3f,"
                      "\"blend_steps\":%u,"
                      "\"overshoot_guard\":%.3f,\"overshoot_chord_slack\":%.3f,"
                      "\"settle_grace_ms\":%.1f,\"chase_ff\":%s,"
                      "\"chase_accel_ff\":%s,\"chase_aim_accel_extrap\":%s,"
                      "\"chase_gain\":%.3f,\"chase_lookahead\":%.2f,"
                      "\"chase_dense_ms\":%.1f,\"wave_centering\":%s,"
                      "\"wave_centering_gain\":%.3f,"
                      "\"vmax\":%.4f,\"amax\":%.3f,"
                      "\"jmax\":%.1f}",
                      curveName, polName, double(ec.infeasible_scale_margin),
                      unsigned(ec.infeasible_reshape_steps),
                      double(ec.infeasible_smooth_budget),
                      double(ec.infeasible_amplitude_budget), double(ec.infeasible_blend),
                      unsigned(ec.infeasible_blend_steps),
                      double(ec.overshoot_guard), double(ec.overshoot_chord_slack),
                      double(ec.settle_grace_us) / 1000.0,
                      ec.chase_feedforward ? "true" : "false",
                      ec.chase_accel_ff ? "true" : "false",
                      ec.chase_aim_accel_extrap ? "true" : "false",
                      double(ec.chase_ff_gain), double(ec.chase_lookahead),
                      double(ec.chase_dense_us) / 1000.0,
                      ec.wave_centering ? "true" : "false",
                      double(ec.wave_centering_gain),
                      double(ec.limits.vmax), double(ec.limits.amax),
                      double(ec.limits.jmax));

        // "stats" mirrors the DEVICE's block key-for-key (WebUI.cpp
        // handleApiSlopmotion): `anomalies` scalar total + `anomalies_by_kind`
        // object. Same kSmAnomalyNames table on both sides, so a sim response
        // and a device response can be diffed directly — which is the entire
        // point of having a simulator for this class of investigation.
        std::string byKind;
        for (size_t k = 0; k < kSmAnomalyNameCount; ++k) {
            char kb[64];
            std::snprintf(kb, sizeof(kb), "%s\"%s\":%u", k ? "," : "", kSmAnomalyNames[k],
                          unsigned(st.anom_kind[k]));
            byKind += kb;
        }

        // 2048: the by-kind object adds ~200 bytes, the tuning block another
        // ~300, and snprintf TRUNCATES silently — a clipped response would be
        // invalid JSON, not a warning.
        char buf[2048];
        std::snprintf(buf, sizeof(buf),
                      "{\"sim\":true,%s,\"sync\":{\"bundles\":%u,\"seg_bundles\":%u,\"samples\":%u,"
                      "\"enqueued\":%u,\"dropped\":%u},"
                      "\"stats\":{\"anomalies\":%u,\"anomalies_by_kind\":{%s}},"
                      "\"simstats\":{\"plan_rejected\":%u,\"ts_clamped\":%u,"
                      "\"substeps_discarded\":%u,\"tick_ms_avg\":%.3f,\"tick_ms_max\":%.3f,"
                      "\"stream_speed_mode\":%u,"
                      "\"ingress\":{\"records\":%u,\"segments\":%u,\"samples\":%u,"
                      "\"rejected\":%u,\"sentinel\":%u,\"dur_floor_10ms\":%u,"
                      "\"dur_under_50ms\":%u,"
                      "\"seg_gap\":{\"n\":%u,\"min_ms\":%.2f,\"mean_ms\":%.2f,\"max_ms\":%.2f},"
                      "\"smp_gap\":{\"n\":%u,\"min_ms\":%.2f,\"mean_ms\":%.2f,\"max_ms\":%.2f},"
                      "\"span_s\":%.3f,\"duty_ratio\":%.4f}}}",
                      tuningBuf,
                      unsigned(st.bundles), unsigned(st.seg_bundles), unsigned(st.samples),
                      unsigned(st.enqueued), unsigned(st.dropped),
                      unsigned(st.anomalies), byKind.c_str(),
                      unsigned(st.plan_rejected),
                      unsigned(st.ts_clamped), unsigned(st.substeps_discarded),
                      double(st.tick_ms_avg), double(st.tick_ms_max),
                      unsigned(st.stream_speed_mode),
                      unsigned(ing.records), unsigned(ing.segments), unsigned(ing.samples),
                      unsigned(ing.rejected), unsigned(ing.sentinel),
                      unsigned(ing.dur_floor_10ms), unsigned(ing.dur_under_50ms),
                      unsigned(ing.seg_gap.n), double(ing.seg_gap.min_ms),
                      double(ing.seg_gap.mean_ms), double(ing.seg_gap.max_ms),
                      unsigned(ing.smp_gap.n), double(ing.smp_gap.min_ms),
                      double(ing.smp_gap.mean_ms), double(ing.smp_gap.max_ms),
                      double(ing.span_s), double(ing.duty_ratio));
        res.set_content(buf, "application/json");
    });

    // ---- Inbound wire recorder ----------------------------------------------
    // CSV is the requirement, not a convenience: the operator diffs this
    // against the funscript that was SUPPOSED to be sent. Raw (pre-scale) and
    // decoded columns sit side by side so a units bug shows up as a mismatch
    // between them rather than as "the motion felt wrong".
    // ?since=<t_s> for incremental pulls, exactly like /api/trace.bin.
    _srv->Get("/api/segments.csv", [sim](const httplib::Request& req, httplib::Response& res) {
        float since = -1.0f;
        if (req.has_param("since")) since = float(atof(req.get_param_value("since").c_str()));
        const auto rows = sim->copyIngressSince(since);
        std::string body;
        body.reserve(160 + rows.size() * 128);
        body += MachineSim::ingressCsvHeader();
        char line[256];
        for (const auto& r : rows) {
            MachineSim::formatIngressCsvRow(line, sizeof(line), r);
            body += line;
        }
        res.set_content(std::move(body), "text/csv");
    });

    // Same rows as JSON (+ the red-flag block) for scripted pulls.
    _srv->Get("/api/segments.json", [sim](const httplib::Request& req, httplib::Response& res) {
        float since = -1.0f;
        if (req.has_param("since")) since = float(atof(req.get_param_value("since").c_str()));
        const auto rows = sim->copyIngressSince(since);
        const auto st = sim->ingressStats();
        std::string body;
        body.reserve(256 + rows.size() * 200);
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "{\"stats\":{\"records\":%u,\"segments\":%u,\"samples\":%u,\"rejected\":%u,"
                      "\"sentinel\":%u,\"dur_floor_10ms\":%u,\"dur_under_50ms\":%u,"
                      "\"seg_gap\":{\"n\":%u,\"min_ms\":%.2f,\"mean_ms\":%.2f,\"max_ms\":%.2f},"
                      "\"smp_gap\":{\"n\":%u,\"min_ms\":%.2f,\"mean_ms\":%.2f,\"max_ms\":%.2f},"
                      "\"span_s\":%.3f,\"duty_ratio\":%.4f},\"records\":[",
                      unsigned(st.records), unsigned(st.segments), unsigned(st.samples),
                      unsigned(st.rejected), unsigned(st.sentinel), unsigned(st.dur_floor_10ms),
                      unsigned(st.dur_under_50ms),
                      unsigned(st.seg_gap.n), double(st.seg_gap.min_ms),
                      double(st.seg_gap.mean_ms), double(st.seg_gap.max_ms),
                      unsigned(st.smp_gap.n), double(st.smp_gap.min_ms),
                      double(st.smp_gap.mean_ms), double(st.smp_gap.max_ms),
                      double(st.span_s), double(st.duty_ratio));
        body += buf;
        bool first = true;
        for (const auto& r : rows) {
            std::snprintf(buf, sizeof(buf),
                          "%s{\"t_s\":%.6f,\"ch\":%u,\"raw_target\":%u,\"raw_dur_ms\":%u,"
                          "\"raw_end_vel\":%d,\"target_norm\":%.4f,\"duration_ms\":%.3f,"
                          "\"end_vel_norm\":%.4f,\"has_end_vel\":%s,\"accepted\":%s,"
                          "\"ts_clamped\":%s,\"wire_t_off_us\":%u,\"wire_t_us\":%u,"
                          "\"due_delta_ms\":%.3f,\"gap_ms\":%.3f}",
                          first ? "" : ",", double(r.t_s), unsigned(r.channel_id),
                          unsigned(r.raw_target), unsigned(r.raw_dur_ms), int(r.raw_end_vel),
                          double(r.target), double(r.duration_us) / 1000.0, double(r.end_vel),
                          r.has_end_vel ? "true" : "false", r.accepted ? "true" : "false",
                          r.ts_clamped ? "true" : "false", unsigned(r.wire_t_off_us),
                          unsigned(r.wire_t_us), double(r.due_delta_ms), double(r.gap_ms));
            body += buf;
            first = false;
        }
        body += "]}";
        res.set_content(std::move(body), "application/json");
    });

    // ---- Async-tune bench: the shelf ----------------------------------------
    // Both artifact kinds in one list, tagged. The picker shows them together
    // because an operator thinks in takes, not in file formats — but the tag is
    // load-bearing (a recording REPLAYS under today's engine, a run RECALLS
    // frozen points), so it is never inferred from the name.
    _srv->Get("/api/recordings", [sim, store](const httplib::Request&, httplib::Response& res) {
        // The live ARBITER GEOMETRY rides along so the tuning panel can open on
        // what the machine is actually set to. It is here, on a bench-only
        // endpoint, rather than added to /api/slopmotion — that response mirrors
        // the DEVICE's key for key so the two can be diffed directly, and
        // widening it with host-side fields would cost exactly the property it
        // exists for.
        const auto g = sim->arbiterGeometry();
        char gbuf[320];
        std::snprintf(gbuf, sizeof(gbuf),
                      "\"geom\":{\"win_min\":%.2f,\"win_max\":%.2f,\"ceiling\":%.2f,"
                      "\"input_speed\":%.1f,\"input_accel\":%.1f,\"user_speed\":%.1f,"
                      "\"user_accel\":%.1f,\"matched\":%u,"
                      "\"safety_filter\":%u,\"gentle_accel_outside\":%u},",
                      double(g.win_min_mm), double(g.win_max_mm), double(g.ceiling_mm),
                      double(g.input_speed), double(g.input_accel), double(g.user_speed),
                      double(g.user_accel),
                      unsigned(g.stream_speed_mode == kSpeedVelocityMatched ? 1 : 0),
                      unsigned(g.safety_filter ? 1 : 0),
                      unsigned(g.gentle_accel_outside ? 1 : 0));
        std::string body = "{\"dir\":\"" + jsonEscape(store->dir()) + "\",\"writable\":" +
                           (store->writable() ? "true" : "false") + "," + gbuf + "\"items\":[";
        bool first = true;
        for (const auto& a : store->list()) {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "%s{\"name\":\"%s\",\"file\":\"%s\",\"kind\":\"%s\",\"bytes\":%llu,"
                          "\"rows\":%u}",
                          first ? "" : ",", jsonEscape(a.name).c_str(), jsonEscape(a.file).c_str(),
                          a.is_run ? "run" : "recording", (unsigned long long)a.bytes,
                          unsigned(a.rows));
            body += buf;
            first = false;
        }
        body += "]}";
        res.set_content(std::move(body), "application/json");
    });

    // ---- Async-tune bench: recompute ----------------------------------------
    // Re-runs a RECORDING through a fresh engine under the query string's
    // settings and returns the samples in /api/trace.bin's own format, with the
    // metrics in the X-Replay-Stats header.
    //
    // This is the whole "move a slider, see the script re-render" loop. It runs
    // on the listener thread against its own engine instance — the live sim is
    // not paused, not touched, and not consulted except to seed the defaults.
    _srv->Get("/api/replay.bin", [sim, store](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.has_param("rec") ? req.get_param_value("rec") : "";
        const std::string path = store->pathFor(name, false);
        if (path.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"bad or missing rec\"}", "application/json");
            return;
        }
        Recording rec;
        std::string err;
        if (!loadRecording(path, rec, err)) {
            res.status = 404;
            res.set_content("{\"error\":\"" + jsonEscape(err) + "\"}", "application/json");
            return;
        }
        const ReplayConfig cfg = replayConfigFrom(req, sim);
        const ReplayResult r = replay(rec, cfg);
        res.set_header("X-Replay-Stats", statsJson(r.m, rec, err));
        res.set_content(traceBody(r.trace, sim->facadeStats().max_rail_mm, cfg.geom.win_min_mm,
                                  cfg.geom.win_max_mm),
                        "application/octet-stream");
    });

    // ---- Async-tune bench: clip a region out as a new recording -------------
    // The fixture-making action: zoom to the strokes that matter, save exactly
    // those commands under a name, come back to them later. `src=live` clips the
    // sim's own wire ring (t is trace seconds, the same base the graph draws in);
    // `src=<name>` re-slices an existing recording, which stays lossless because
    // Recording keeps every source line verbatim.
    _srv->Get("/api/rec/save", [sim, store](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.has_param("name") ? req.get_param_value("name") : "";
        if (RecordingStore::sanitizeName(name).empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"bad or missing name\"}", "application/json");
            return;
        }
        const double t0 = req.has_param("t0") ? std::atof(req.get_param_value("t0").c_str()) : -1e30;
        const double t1 = req.has_param("t1") ? std::atof(req.get_param_value("t1").c_str()) : 1e30;
        const std::string src = req.has_param("src") ? req.get_param_value("src") : "live";

        std::vector<std::string> lines;
        if (src == "live") {
            char buf[256];
            for (const auto& r : sim->copyIngressSince(float(t0 <= -1e29 ? -1.0 : t0))) {
                if (double(r.t_s) > t1) break;
                MachineSim::formatIngressCsvRow(buf, sizeof(buf), r);
                lines.emplace_back(buf);
            }
        } else {
            const std::string path = store->pathFor(src, false);
            Recording rec;
            std::string err;
            if (path.empty() || !loadRecording(path, rec, err)) {
                res.status = 404;
                res.set_content("{\"error\":\"source recording not readable\"}", "application/json");
                return;
            }
            for (size_t i = 0; i < rec.cmds.size(); ++i) {
                const double t = rec.cmds[i].arrive_s;
                if (t >= t0 && t <= t1) lines.push_back(rec.lines[i]);
            }
        }
        if (lines.empty()) {
            // Distinguish "the range is wrong" from "nothing has ever streamed
            // in". They are the two very different reasons a clip comes back
            // empty, and a live clip against an idle sim hits the second one —
            // the machine's trace ring is full of position samples the whole
            // time, so the graph looks alive while the WIRE recorder is empty.
            res.status = 400;
            const bool anyWire = sim->ingressCount() > 0;
            res.set_content(anyWire ? "{\"error\":\"no commands in that visible range\"}"
                                    : "{\"error\":\"the wire recorder is empty — nothing has "
                                      "streamed into the sim yet (position samples are not "
                                      "commands)\"}",
                            "application/json");
            return;
        }
        const size_t n = store->write(name, false, MachineSim::ingressCsvHeader(), lines);
        // A WRITE THAT DID NOT HAPPEN IS AN ERROR. This used to answer 200 with
        // {"saved":0}, so the page happily selected a recording that was never
        // created and then failed to replay its own file — the save reported
        // success and the failure surfaced one step later, pointing at the wrong
        // thing entirely.
        if (n == 0) {
            res.status = 500;
            res.set_content("{\"error\":\"could not write to " + jsonEscape(store->dir()) +
                                (store->writable() ? "" : " (not writable)") + "\"}",
                            "application/json");
            return;
        }
        char buf[512];
        std::snprintf(buf, sizeof(buf), "{\"saved\":%zu,\"name\":\"%s\",\"kind\":\"recording\"}", n,
                      jsonEscape(RecordingStore::sanitizeName(name)).c_str());
        res.set_content(buf, "application/json");
    });

    // ---- Async-tune bench: freeze a result ----------------------------------
    // Re-runs with the given settings and stores the SAMPLES plus the settings
    // that produced them. Recalling it later draws the stored points with no
    // engine involved — see RecordingStore.h for why a recomputed baseline is
    // not a baseline.
    _srv->Get("/api/run/save", [sim, store](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.has_param("name") ? req.get_param_value("name") : "";
        const std::string rname = req.has_param("rec") ? req.get_param_value("rec") : "";
        const std::string path = store->pathFor(rname, false);
        if (RecordingStore::sanitizeName(name).empty() || path.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"bad or missing name/rec\"}", "application/json");
            return;
        }
        Recording rec;
        std::string err;
        if (!loadRecording(path, rec, err)) {
            res.status = 404;
            res.set_content("{\"error\":\"" + jsonEscape(err) + "\"}", "application/json");
            return;
        }
        const ReplayConfig cfg = replayConfigFrom(req, sim);
        const ReplayResult r = replay(rec, cfg);
        RunSettings st = settingsFor(cfg, RecordingStore::sanitizeName(rname));
        // The metrics ride in the settings block too: a stored run should be
        // readable as a verdict without re-deriving anything from its samples.
        st.set("metric.follow_rms_mm", r.m.follow_rms_mm, 3);
        st.set("metric.sender_rms_mm", r.m.sender_rms_mm, 3);
        st.set("metric.band_center_err_mm", r.m.band_center_err_mm, 3);
        st.set("metric.reach_ratio", r.m.reach_ratio, 4);
        st.set("metric.shape_corr", r.m.shape_corr, 4);
        st.set("metric.flat_frac", r.m.flat_frac, 4);
        st.set("metric.peak_vel_mm_s", r.m.peak_vel_mm_s, 1);
        st.set("metric.peak_acc_mm_s2", r.m.peak_acc_mm_s2, 0);
        st.set("metric.anomalies", double(r.m.anomalies), 0);
        st.set("metric.plan_rejected", double(r.m.plan_rejected), 0);
        const size_t n = writeRun(*store, name, st, r.trace);
        if (n == 0) {
            res.status = 500;
            res.set_content("{\"error\":\"could not write to " + jsonEscape(store->dir()) +
                                (store->writable() ? "" : " (not writable)") + "\"}",
                            "application/json");
            return;
        }
        char buf[512];
        std::snprintf(buf, sizeof(buf), "{\"saved\":%zu,\"name\":\"%s\",\"kind\":\"run\"}", n,
                      jsonEscape(RecordingStore::sanitizeName(name)).c_str());
        res.set_content(buf, "application/json");
    });

    // ---- Async-tune bench: recall a frozen result ---------------------------
    // NO ENGINE RUNS HERE. The stored points are returned as they were saved,
    // which is the entire point of a run: it is a reference line that a later
    // change to slopmotion cannot quietly move.
    _srv->Get("/api/run.bin", [sim, store](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.has_param("name") ? req.get_param_value("name") : "";
        RunSettings st;
        std::vector<float> trace;
        std::string err;
        if (!readRun(*store, name, st, trace, err)) {
            res.status = 404;
            res.set_content("{\"error\":\"" + jsonEscape(err) + "\"}", "application/json");
            return;
        }
        // The settings block is echoed VERBATIM, unknown keys included — a run
        // saved by a newer build opens here and shows settings this one cannot
        // set, rather than silently losing them.
        std::string js = "{";
        bool first = true;
        for (const auto& p : st.kv) {
            js += (first ? "\"" : ",\"") + jsonEscape(p.first) + "\":\"" + jsonEscape(p.second) + "\"";
            first = false;
        }
        js += "}";
        res.set_header("X-Run-Settings", js);
        const float wmin = st.find("win_min") ? float(std::atof(st.find("win_min")->c_str())) : 0.0f;
        const float wmax = st.find("win_max") ? float(std::atof(st.find("win_max")->c_str())) : 0.0f;
        res.set_content(traceBody(trace, sim->facadeStats().max_rail_mm, wmin, wmax),
                        "application/octet-stream");
    });

    // ---- The analyzer popout: rendered graph + analysis in the browser ------
    _srv->Get("/graph", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kGraphPageHtml, "text/html");
    });

    // Binary trace feed: 20-byte header {u32 n, f32 max_rail, f32 win_min,
    // f32 win_max, u32 stride} + n × stride f32 LE, stride = 5:
    // {t, pos, tgt, vel, cmd_norm}. ?since=<t_s> for increments.
    //
    // The trailing `stride` word is what makes the record layout SELF-DESCRIBING
    // rather than a number two files have to agree on by memory: the analyzer
    // reads it and lays out its columns from it, and a curl/python reader can
    // do the same without consulting this source. It is appended (not inserted)
    // so the first four fields keep their offsets.
    _srv->Get("/api/trace.bin", [sim](const httplib::Request& req, httplib::Response& res) {
        float since = -1.0f;
        if (req.has_param("since")) since = float(atof(req.get_param_value("since").c_str()));
        float rail = 0, wmin = 0, wmax = 0;
        const std::vector<float> recs = sim->copyTraceSince(since, rail, wmin, wmax);
        res.set_content(traceBody(recs, rail, wmin, wmax), "application/octet-stream");
    });

    if (!webuiPath.empty()) {
        std::ifstream f(webuiPath, std::ios::binary);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            auto page = std::make_shared<std::string>(ss.str());
            _srv->Get("/", [page](const httplib::Request&, httplib::Response& res) {
                res.set_content(*page, "text/html");
            });
            if (log) log->logf('I', "http: serving webui page (%zu bytes)", page->size());
        } else if (log) {
            log->logf('W', "http: --webui path not readable: %s", webuiPath.c_str());
        }
    }

    if (!_srv->bind_to_port("0.0.0.0", httpPort)) {
        if (log) log->logf('W', "http: could not bind :%u — /api/capabilities+/api/slopmotion offline",
                           unsigned(httpPort));
        _srv.reset();
        return false;
    }
    _thread = std::thread([this] { _srv->listen_after_bind(); });
    if (log) log->logf('I', "http: facade on :%u (/api/capabilities, /api/slopmotion)", unsigned(httpPort));
    return true;
}

void HttpFacade::stop() {
    if (_srv) _srv->stop();
    if (_thread.joinable()) _thread.join();
    _srv.reset();
}

}  // namespace slopsim
