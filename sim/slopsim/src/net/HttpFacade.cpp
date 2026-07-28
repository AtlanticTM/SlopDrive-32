// HttpFacade — read-only HTTP handlers (see HttpFacade.h for the doctrine).

#include "net/HttpFacade.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <httplib.h>

#include <cstring>

#include "machine/MachineSim.h"
#include "net/GraphPage.h"

namespace slopsim {

HttpFacade::HttpFacade() = default;
HttpFacade::~HttpFacade() { stop(); }

bool HttpFacade::begin(MachineSim* sim, uint16_t httpPort, uint16_t wsPort, SessionLog* log,
                       std::string webuiPath) {
    _srv = std::make_unique<httplib::Server>();

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
        const char* policyName =
            ec.infeasible_policy == slopmotion::InfeasiblePolicy::Stretch ? "stretch"
          : ec.infeasible_policy == slopmotion::InfeasiblePolicy::Scale   ? "scale"
          : ec.infeasible_policy == slopmotion::InfeasiblePolicy::Reshape ? "reshape"
          : ec.infeasible_policy == slopmotion::InfeasiblePolicy::PrioritizeAmplitude
                ? "prio-amplitude"
          : ec.infeasible_policy == slopmotion::InfeasiblePolicy::PrioritizeSmooth
                ? "prio-smooth"
                                                                          : "?";
        // 768: the budgeted-policy trio added ~90 bytes and snprintf TRUNCATES
        // SILENTLY — a clipped tuning block is invalid JSON, not a short one.
        const char* curveName =
            ec.curve_policy == slopmotion::CurvePolicy::ForceC1 ? "c1"
          : ec.curve_policy == slopmotion::CurvePolicy::ForceC2 ? "c2"
                                                                : "follow";
        char tuningBuf[768];
        std::snprintf(tuningBuf, sizeof(tuningBuf),
                      "\"tuning\":{\"curve_policy\":\"%s\","
                      "\"infeasible_policy\":\"%s\","
                      "\"infeasible_scale_margin\":%.3f,\"reshape_steps\":%u,"
                      "\"smooth_budget\":%.3f,\"amplitude_budget\":%.3f,"
                      "\"blend_steps\":%u,"
                      "\"settle_grace_ms\":%.1f,\"chase_ff\":%s,"
                      "\"chase_accel_ff\":%s,\"chase_aim_accel_extrap\":%s,"
                      "\"chase_gain\":%.3f,\"chase_lookahead\":%.2f,"
                      "\"chase_dense_ms\":%.1f,\"wave_centering\":%s,"
                      "\"wave_centering_gain\":%.3f,"
                      "\"vmax\":%.4f,\"amax\":%.3f,"
                      "\"jmax\":%.1f}",
                      curveName, policyName, double(ec.infeasible_scale_margin),
                      unsigned(ec.infeasible_reshape_steps),
                      double(ec.infeasible_smooth_budget),
                      double(ec.infeasible_amplitude_budget),
                      unsigned(ec.infeasible_blend_steps),
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
        const uint32_t stride = MachineSim::kTraceStride;
        const uint32_t n = uint32_t(recs.size() / stride);
        std::string body(20 + recs.size() * sizeof(float), '\0');
        std::memcpy(body.data(), &n, 4);
        std::memcpy(body.data() + 4, &rail, 4);
        std::memcpy(body.data() + 8, &wmin, 4);
        std::memcpy(body.data() + 12, &wmax, 4);
        std::memcpy(body.data() + 16, &stride, 4);
        if (!recs.empty()) std::memcpy(body.data() + 20, recs.data(), recs.size() * sizeof(float));
        res.set_content(std::move(body), "application/octet-stream");
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
