#include "tui/MachineScreen.h"

#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ftxui/dom/canvas.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/loop.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"

#include "common/HostPlatform.h"
#include "common/OpenBrowser.h"

namespace slopsim {

using namespace ftxui;

namespace {

const Color kAccent = Color::Orange1;
const Color kChrome = Color::GrayDark;
const Color kDim = Color::GrayLight;
const Color kPos = Color::Green;
const Color kTgt = Color::Yellow;
const Color kVel = Color::MediumPurple2;

const char* kEngineModes[] = {"idle", "waveform", "chase", "settle"};
// Indexed by slopmotion::PlanKind — keep in step with it AND with the 0x0081
// `plan_kind` select options in SlopSyncCatalog.h.
const char* kPlanKinds[] = {"-", "quintic", "ruckig", "cubic"};
const char* kPatternNames[] = {"stroke", "tease", "shallow-fast"};

// Bounds-checked name lookup for the enum tables above.
//
// NEVER "guard" one of these with % or &. A modulo does not bound an index, it
// ALIASES it onto a valid slot — and a wrong name that looks legitimate is a
// ground-truth lie, which is the one class of bug this product cannot ship.
// This is not hypothetical: `kPlanKinds[plan % 3]` reported slopmotion 0.8.0's
// PlanKind::Cubic (= 3) as "-" (None) for an entire bring-up session, so the
// operator was told the machine had NO ACTIVE PLAN while it was mid-stroke on a
// cubic. The operator caught it by eye; nothing in the code would have.
//
// Out of range must LOOK wrong. "?<n>" names the offending value so the next
// person can find the table that did not grow with its enum.
template <size_t N>
std::string nameOf(const char* const (&tbl)[N], unsigned i) {
    if (i < N) return tbl[i];
    char buf[16];
    std::snprintf(buf, sizeof(buf), "?%u", i);
    return buf;
}

// Canonical wire names for slopmotion::InfeasiblePolicy — the SAME strings the
// device's /api/slopmotion echoes, so a TUI readout and an API response say the
// same word about the same state. FIVE policies since slopmotion 0.8.0 — the
// two budgeted ones subsume scale/reshape (which are exactly themselves with
// the budget pinned at 100 %), kept alongside so the old behaviour stays
// A/B-able against the new.
const char* kInfeasPolicyName(slopmotion::InfeasiblePolicy p) {
    switch (p) {
        case slopmotion::InfeasiblePolicy::Stretch: return "stretch";
        case slopmotion::InfeasiblePolicy::Scale:   return "scale";
        case slopmotion::InfeasiblePolicy::Reshape: return "reshape";
        case slopmotion::InfeasiblePolicy::PrioritizeAmplitude: return "prio-amplitude";
        case slopmotion::InfeasiblePolicy::PrioritizeSmooth:    return "prio-smooth";
    }
    return "?";
}

// Canonical wire names for slopmotion::CurvePolicy — same string the sim's
// /api/slopmotion echoes. "follow" resolves to c2 until the curve_family wire
// signalling lands, and the readout says so rather than pretending otherwise.
const char* kCurvePolicyName(slopmotion::CurvePolicy p) {
    switch (p) {
        case slopmotion::CurvePolicy::FollowClient: return "follow(->c2)";
        case slopmotion::CurvePolicy::ForceC1:      return "c1-cubic";
        case slopmotion::CurvePolicy::ForceC2:      return "c2-quintic";
    }
    return "?";
}

std::string fmt(const char* f, ...) {
    // 512: the `motion` readout is one long line of engine config and the
    // middot separators are 3 bytes each in UTF-8 — 160 truncated it SILENTLY
    // the moment reshape-steps + settle-grace joined the line.
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string t;
    while (is >> t) out.push_back(t);
    return out;
}

// dim labeled rule:  ─ label ────────────
Element section(const std::string& label) {
    return hbox({text("─ ") | color(kChrome), text(label) | color(kChrome),
                 text(" ") | color(kChrome), separatorLight() | color(kChrome) | flex}) |
           size(HEIGHT, EQUAL, 1);
}

Element chip(const char* label, bool on, Color onColor) {
    if (!on) return text(std::string(label)) | color(kChrome);
    return text(std::string(label)) | color(onColor) | bold;
}

// The rail strip: window band, actual carriage, engine target ghost.
Element railElement(const MachineSim::Snapshot& s, int width) {
    const int W = width;
    auto col = [&](float mm) {
        int c = int(mm / (s.max_rail > 1 ? s.max_rail : 1) * float(W - 1) + 0.5f);
        return c < 0 ? 0 : (c >= W ? W - 1 : c);
    };
    const int cMin = col(s.win_min), cMax = col(s.win_max);
    const int cPos = col(s.pos_mm), cTgt = col(s.tgt_mm);
    Elements cells;
    cells.reserve(size_t(W));
    for (int i = 0; i < W; ++i) {
        std::string g = "╌";
        Color fg = kChrome;
        if (i >= cMin && i <= cMax) { g = "─"; fg = kDim; }
        if (i == cMin || i == cMax) { g = "│"; fg = kDim; }
        if (i == cTgt) { g = "▒"; fg = kTgt; }
        if (i == cPos) { g = "█"; fg = s.estop ? Color::Red : kPos; }
        cells.push_back(text(g) | color(fg));
    }
    return hbox(std::move(cells));
}

struct Command {
    std::string name;
    std::string hint;
    std::string desc;
    std::function<std::string(const std::vector<std::string>&)> run;  // returns toast
};

}  // namespace

int runMachineScreen(MachineSim& sim, SessionLog& log, uint16_t wsPort, uint16_t httpPort) {
    auto screen = ScreenInteractive::Fullscreen();
    // Never enable mouse reporting. This TUI is entirely keyboard-driven, and
    // FTXUI turns SGR mouse tracking ON by default — which the terminal only
    // stops emitting when the app disables it on exit. A process that dies
    // WITHOUT unwinding (taskkill, crash, closed pane) therefore leaves the
    // terminal spewing `[<b;x;yM` motion reports into the shell as if typed,
    // every time the mouse crosses the window. Not asking for them in the
    // first place makes that failure mode structurally impossible. :3
    screen.TrackMouse(false);
    const std::string analyzeUrl = fmt("http://127.0.0.1:%u/graph", unsigned(httpPort));

    // ---- UI state -----------------------------------------------------------
    int selSlot = 0;
    bool paletteOpen = false;
    std::string paletteInput;
    int paletteSel = 0;
    int paletteScroll = 0;   // first visible row of the filtered command list
    // Engine-config panel ('m' / the `motion` command). Default OFF: it is ~12
    // rows of standing state, and a screen that only ever changes when the
    // operator changes something belongs behind a toggle, not in the way of the
    // log every session.
    bool cfgPanel = false;
    bool graphOn = false;  // browser analyzer is the instrument; graph.tui re-enables
    bool graphZoom = false;
    bool graphFrozen = false;
    // Inbound wire recorder panel. 0 = auto (visible while a stream is live),
    // 1 = forced on, 2 = off. Auto is the default because the panel is only
    // interesting when someone is actually sending, and an idle screen should
    // stay quiet.
    int segMode = 0;
    double freezeT = 0.0;      // right edge of the frozen view (sim seconds)
    double windowS = 12.0;     // visible seconds
    std::string toast;
    auto toastUntil = std::chrono::steady_clock::now();
    auto say = [&](std::string msg) {
        toast = std::move(msg);
        toastUntil = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    };

    auto now_t = [&]() -> double {
        const size_t n = sim.traceCount();
        return n ? double(sim.traceAt(n - 1).t_s) : 0.0;
    };

    // ---- Command palette ----------------------------------------------------
    auto num = [](const std::vector<std::string>& a, size_t i, float dflt) {
        return a.size() > i ? float(atof(a[i].c_str())) : dflt;
    };
    std::vector<Command> commands = {
        {"home", "", "start the (fake) homing cycle", [&](auto&) { sim.startHoming(); return "homing started"; }},
        {"unhome", "", "drop homed (motion gates close)", [&](auto&) { sim.forceUnhome(); return "unhomed"; }},
        {"estop", "", "latch the e-stop", [&](auto&) { sim.injectEstop(); return "E-STOP latched"; }},
        {"clear", "", "clear the e-stop latch", [&](auto&) { sim.injectClearEstop(); return "e-stop cleared (still unhomed)"; }},
        {"pause", "", "pause motion", [&](auto&) { if (!sim.snapshot().paused) sim.togglePause(); return "paused"; }},
        {"resume", "", "resume motion", [&](auto&) { if (sim.snapshot().paused) sim.togglePause(); return "resumed"; }},
        {"override", "", "toggle manual override", [&](auto&) { sim.toggleOverride(); return "override toggled"; }},
        {"window", "<min> <max>", "stroke window (mm)",
         [&](auto& a) {
             if (a.size() < 2) return std::string("usage: window <min> <max>");
             return sim.uiSetWindow(num(a, 0, 0), num(a, 1, 0))
                        ? fmt("window applied [%.0f..%.0f]", double(sim.snapshot().win_min), double(sim.snapshot().win_max))
                        : std::string("rejected: min >= max");
         }},
        {"speed.user", "<mm/s>", "gentle-set speed ceiling",
         [&](auto& a) { return fmt("user speed -> %.0f mm/s", double(sim.uiSetLimit(MachineSim::LimitKind::UserSpeed, num(a, 0, 0)))); }},
        {"accel.user", "<mm/s2>", "gentle-set accel ceiling",
         [&](auto& a) { return fmt("user accel -> %.0f mm/s2", double(sim.uiSetLimit(MachineSim::LimitKind::UserAccel, num(a, 0, 0)))); }},
        {"speed.input", "<mm/s>", "stream/pattern speed ceiling",
         [&](auto& a) { return fmt("input speed -> %.0f mm/s", double(sim.uiSetLimit(MachineSim::LimitKind::InputSpeed, num(a, 0, 0)))); }},
        {"accel.input", "<mm/s2>", "stream/pattern accel ceiling",
         [&](auto& a) { return fmt("input accel -> %.0f mm/s2", double(sim.uiSetLimit(MachineSim::LimitKind::InputAccel, num(a, 0, 0)))); }},
        {"jerk.input", "<mm/s3>", "stream/pattern jerk ceiling (mechanical, not smoothing)",
         [&](auto& a) { return fmt("input jerk -> %.0f mm/s3", double(sim.uiSetLimit(MachineSim::LimitKind::InputJerk, num(a, 0, 0)))); }},
        // This used to PRINT the whole engine config as one middot-separated
        // toast line. It had grown past the width of any terminal and was one
        // knob away from silently hitting fmt()'s 512-byte ceiling, so it is now
        // the toggle for the grouped panel that shows the same applied values.
        {"motion", "", "toggle the engine config panel (applied values)",
         [&](auto&) {
             cfgPanel = !cfgPanel;
             return std::string(cfgPanel ? "engine config panel: on (m toggles)"
                                         : "engine config panel: off");
         }},
        {"motion.speedmode", "<pegged|matched>", "stream speed feed (device default: pegged)",
         [&](auto& a) {
             if (a.empty()) return std::string("usage: motion.speedmode <pegged|matched>");
             const std::string v = lower(a[0]);
             if (v != "pegged" && v != "matched")
                 return std::string("usage: motion.speedmode <pegged|matched>");
             const uint8_t m = sim.uiSetStreamSpeedMode(v == "matched" ? 1 : 0);
             return m == 1 ? std::string("stream speed -> velocity-matched (FAS vmax = the curve's "
                                         "own speed; no authority to recover lag)")
                           : std::string("stream speed -> ceiling-pegged (DEVICE DEFAULT; micro-"
                                         "target deltas shape velocity)");
         }},
        {"machine.stroke", "<mm|0>", "measured stroke ceiling (0 = use max rail)",
         [&](auto& a) {
             const float applied = sim.uiSetMeasuredStroke(num(a, 0, 0.0f));
             return applied > 0.0f
                        ? fmt("measured stroke -> %.1f mm (effective ceiling)", double(applied))
                        : fmt("measured stroke cleared -> rail %.1f mm is the ceiling",
                              double(sim.effectiveCeilingMm()));
         }},
        {"motion.policy", "<stretch|scale|reshape|amp|smooth>", "infeasible segment: what gives — shape, range, or timing",
         [&](auto& a) {
             static const char* kUsage =
                 "usage: motion.policy <stretch|scale|reshape|amp|smooth>  "
                 "(amp = prioritize amplitude, smooth = prioritize smooth)";
             if (a.empty()) return std::string(kUsage);
             const std::string v = lower(a[0]);
             slopmotion::InfeasiblePolicy want;
             if      (v == "stretch") want = slopmotion::InfeasiblePolicy::Stretch;
             else if (v == "scale")   want = slopmotion::InfeasiblePolicy::Scale;
             else if (v == "reshape") want = slopmotion::InfeasiblePolicy::Reshape;
             else if (v == "amp" || v == "prio-amplitude")
                 want = slopmotion::InfeasiblePolicy::PrioritizeAmplitude;
             else if (v == "smooth" || v == "prio-smooth")
                 want = slopmotion::InfeasiblePolicy::PrioritizeSmooth;
             else return std::string(kUsage);
             switch (sim.uiSetInfeasiblePolicy(want)) {
                 case slopmotion::InfeasiblePolicy::Scale:
                     return std::string("infeasible policy -> scale (keep the deadline + the spline, shrink the stroke)");
                 case slopmotion::InfeasiblePolicy::Reshape:
                     return std::string("infeasible policy -> reshape (keep the deadline + the range, give up the spline shape)");
                 case slopmotion::InfeasiblePolicy::PrioritizeAmplitude:
                     return fmt("infeasible policy -> prio-amplitude (spend smooth to %.0f%%, then amplitude)",
                                double(sim.engineConfig().infeasible_smooth_budget) * 100.0);
                 case slopmotion::InfeasiblePolicy::PrioritizeSmooth:
                     return fmt("infeasible policy -> prio-smooth (spend amplitude to %.0f%%, then smooth)",
                                double(sim.engineConfig().infeasible_amplitude_budget) * 100.0);
                 default:
                     return std::string("infeasible policy -> stretch (keep the stroke, overrun the deadline)");
             }
         }},
        {"motion.curve", "<follow|c1|c2>", "waveform curve family: c1 reproduces the script's own cubic spline",
         [&](auto& a) {
             static const char* kUsage = "usage: motion.curve <follow|c1|c2>";
             if (a.empty()) return std::string(kUsage);
             const std::string v = lower(a[0]);
             slopmotion::CurvePolicy want;
             if      (v == "follow") want = slopmotion::CurvePolicy::FollowClient;
             else if (v == "c1")     want = slopmotion::CurvePolicy::ForceC1;
             else if (v == "c2")     want = slopmotion::CurvePolicy::ForceC2;
             else return std::string(kUsage);
             switch (sim.uiSetCurvePolicy(want)) {
                 case slopmotion::CurvePolicy::ForceC1:
                     return std::string("curve -> c1-cubic (script's own spline; accel STEPS at knots, as authored)");
                 case slopmotion::CurvePolicy::ForceC2:
                     return std::string("curve -> c2-quintic (curvature continuous; knot kinks rounded off)");
                 default:
                     return std::string("curve -> follow client (no wire signalling yet -> resolves to c2)");
             }
         }},
        {"motion.smoothbudget", "<0-1>", "prio-amplitude: max handle reduction toward the chord before amplitude is spent",
         [&](auto& a) {
             const float applied = sim.uiSetSmoothBudget(num(a, 0, 0.5f));
             return fmt("smooth budget -> %.2f (%.0f%% toward a straight line; 1.0 is C0 at the knots)",
                        double(applied), double(applied) * 100.0);
         }},
        {"motion.ampbudget", "<0-1>", "prio-smooth: max fraction of the commanded stroke surrendered before smooth is spent",
         [&](auto& a) {
             const float applied = sim.uiSetAmplitudeBudget(num(a, 0, 0.5f));
             return fmt("amplitude budget -> %.2f (%.0f%% of the stroke; 0.5 stops at the segment midpoint)",
                        double(applied), double(applied) * 100.0);
         }},
        {"motion.blendsteps", "<1-10>", "bisection depth on the smoothness (alpha) search",
         [&](auto& a) {
             const float raw = num(a, 0, 6.0f);
             const uint8_t applied = sim.uiSetBlendSteps(
                 uint8_t(raw < 0.0f ? 0.0f : (raw > 255.0f ? 255.0f : raw)));
             return fmt("blend steps -> %u (alpha resolved to 1/%u of the budget)",
                        unsigned(applied), unsigned(1u << applied));
         }},
        {"motion.margin", "<0.5-1.0>", "stroke-scale margin under the scale policy",
         [&](auto& a) { return fmt("infeasible margin -> %.2f", double(sim.uiSetInfeasibleMargin(num(a, 0, 0.92f)))); }},
        {"motion.reshapesteps", "<0-8>", "reshape bisection depth (plan-time budget; 0 = no bisection)",
         [&](auto& a) {
             const float raw = num(a, 0, 6.0f);
             const uint8_t applied = sim.uiSetReshapeSteps(
                 uint8_t(raw < 0.0f ? 0.0f : (raw > 255.0f ? 255.0f : raw)));
             return fmt("reshape steps -> %u (stroke resolved to 1/%u of the commanded amplitude)",
                        unsigned(applied), unsigned(1u << applied));
         }},
        {"motion.settlegrace", "<ms>", "grace an expired plan may hold before braking to rest (0 = pre-0.4)",
         [&](auto& a) {
             const float applied = sim.uiSetSettleGraceMs(num(a, 0, 30.0f));
             return applied > 0.0f
                        ? fmt("settle grace -> %.0f ms (capped at 1.5x the estimated stream interval)",
                              double(applied))
                        : std::string("settle grace -> 0 ms (pre-0.4: brake the instant the plan expires)");
         }},
        {"motion.jmax", "<units/s3>", "NORMALIZED jerk override (0 = derive from jerk.input)",
         [&](auto& a) {
             const float applied = sim.uiSetJmax(num(a, 0, 0.0f));
             return fmt("jmax -> %.0f units/s3 (%.0f mm/s3) [%s]", double(applied),
                        double(applied * sim.windowSpanMm()),
                        sim.jerkOverrideNorm() > 0.0f ? "override" : "derived from jerk.input");
         }},
        {"motion.aimff", "<on|off>", "2nd-order chase aim (crest overshoot)",
         [&](auto& a) {
             if (a.empty()) return std::string("usage: motion.aimff <on|off>");
             const std::string v = lower(a[0]);
             if (v != "on" && v != "off") return std::string("usage: motion.aimff <on|off>");
             return sim.uiSetChaseAimAccelExtrap(v == "on")
                        ? std::string("chase aim accel-extrap -> on")
                        : std::string("chase aim accel-extrap -> off");
         }},
        {"motion.centring", "<on|off>", "midpoint-anchored stroke shortening when the machine can't reach",
         [&](auto& a) {
             if (a.empty()) return std::string("usage: motion.centring <on|off>");
             const std::string v = lower(a[0]);
             if (v != "on" && v != "off") return std::string("usage: motion.centring <on|off>");
             return sim.uiSetWaveCentering(v == "on")
                        ? std::string("waveform centring -> on (degraded band stays symmetric "
                                      "about the commanded midpoint; reported as waveform_centred)")
                        : std::string("waveform centring -> off (slopmotion 0.4.0 contract: the "
                                      "band is free to walk off one end)");
         }},
        {"motion.centringgain", "<0-1>", "centring strength (1 = full; a FEEL dial, not monotone)",
         [&](auto& a) {
             const float applied = sim.uiSetWaveCenteringGain(num(a, 0, 1.0f));
             return applied > 0.0f
                        ? fmt("centring gain -> %.2f", double(applied))
                        : std::string("centring gain -> 0.00 (same effect as motion.centring off)");
         }},
        {"pattern", "<0|1|2|off>", "run a built-in pattern / stop",
         [&](auto& a) {
             if (a.empty()) return std::string("usage: pattern <0|1|2|off>");
             if (a[0] == "off") { sim.uiSetPattern(false, -1); return std::string("pattern stopped"); }
             sim.uiSetPattern(true, atoi(a[0].c_str()));
             return fmt("pattern %s running",
                        nameOf(kPatternNames, sim.snapshot().pattern_idx).c_str());
         }},
        {"pattern.speed", "<0-100>", "pattern cadence", [&](auto& a) { sim.uiSetPatternParam(3, num(a, 0, 50)); return std::string("pattern speed set"); }},
        {"pattern.depth", "<0-100>", "pattern far end", [&](auto& a) { sim.uiSetPatternParam(4, num(a, 0, 100)); return std::string("pattern depth set"); }},
        {"pattern.stroke", "<0-100>", "pattern amplitude", [&](auto& a) { sim.uiSetPatternParam(5, num(a, 0, 100)); return std::string("pattern stroke set"); }},
        {"pattern.sensation", "<-100..100>", "in/out skew", [&](auto& a) { sim.uiSetPatternParam(6, num(a, 0, 0)); return std::string("pattern sensation set"); }},
        {"segments", "[on|off|auto]", "inbound wire recorder panel (what the CLIENT sent)",
         [&](auto& a) {
             if (!a.empty()) {
                 const std::string v = lower(a[0]);
                 if (v == "on") segMode = 1;
                 else if (v == "off") segMode = 2;
                 else if (v == "auto") segMode = 0;
                 else return std::string("usage: segments [on|off|auto]");
             } else {
                 segMode = (segMode + 1) % 3;  // auto -> on -> off -> auto
             }
             return std::string(segMode == 0 ? "segment recorder: auto (shown while a stream is live)"
                                : segMode == 1 ? "segment recorder: on"
                                               : "segment recorder: off");
         }},
        {"segments.reset", "", "clear the wire recorder ring + its red-flag stats",
         [&](auto&) { sim.resetIngress(); return std::string("wire recorder cleared"); }},
        {"anomalies", "", "show the engine anomaly breakdown (every kind, incl. zeros)",
         [&](auto&) {
             const auto s = sim.snapshot();
             std::string out = fmt("anomalies %u:", unsigned(s.anomalies));
             // Every kind, zeros included — "no waveform_scaled" is an answer,
             // and a name missing from the list would read as a stale build.
             for (size_t k = 1; k < kSmAnomalyNameCount; ++k)
                 out += fmt(" %s %u", kSmAnomalyNames[k], unsigned(s.anom_kind[k]));
             return out;
         }},
        {"anomalies.reset", "", "zero the engine anomaly counters (total + per kind)",
         [&](auto&) { sim.resetAnomalies(); return std::string("anomaly counters cleared"); }},
        {"segments.export", "[file.csv]", "write the wire recorder ring as CSV",
         [&](auto& a) {
             const std::string path = a.empty() ? "slopsim-segments.csv" : a[0];
             const size_t n = sim.exportIngress(path);
             return n ? fmt("exported %zu wire records -> %s", n, path.c_str())
                      : fmt("export FAILED: %s", path.c_str());
         }},
        {"graph", "", "open the rendered graph/analyzer in the browser",
         [&](auto&) { openBrowser(analyzeUrl); return "graph popped out -> " + analyzeUrl; }},
        {"analyze", "", "same as graph (browser popout)",
         [&](auto&) { openBrowser(analyzeUrl); return "graph popped out -> " + analyzeUrl; }},
        {"graph.tui", "", "toggle the in-terminal graph pane", [&](auto&) { graphOn = !graphOn; return graphOn ? "terminal graph on" : "terminal graph off"; }},
        {"freeze", "", "freeze/unfreeze the graph for analysis",
         [&](auto&) {
             graphFrozen = !graphFrozen;
             if (graphFrozen) freezeT = now_t();
             return graphFrozen ? "graph frozen — ←/→ pan, +/- zoom" : "graph live";
         }},
        {"zoom", "", "graph fullscreen toggle", [&](auto&) { graphZoom = !graphZoom; return graphZoom ? "graph zoomed (z restores)" : "layout restored"; }},
        {"export", "[file.csv]", "write the trace ring as CSV",
         [&](auto& a) {
             const std::string path = a.empty() ? "slopsim-trace.csv" : a[0];
             const size_t n = sim.exportTrace(path);
             return n ? fmt("exported %zu samples -> %s", n, path.c_str()) : fmt("export FAILED: %s", path.c_str());
         }},
        {"kick", "<slot>", "disconnect a session", [&](auto& a) { sim.kickSession(uint8_t(num(a, 0, float(selSlot)))); return std::string("kicked"); }},
        {"congestion", "<slot> <0-2>", "inject congestion level",
         [&](auto& a) {
             if (a.size() < 2) return std::string("usage: congestion <slot> <0-2>");
             sim.setCongestion(uint8_t(num(a, 0, 0)), uint8_t(num(a, 1, 0)));
             return fmt("slot %d congestion -> %d", int(num(a, 0, 0)), int(num(a, 1, 0)));
         }},
        {"quit", "", "exit slopsim", [&](auto&) -> std::string { screen.ExitLoopClosure()(); return ""; }},
    };

    // Palette column geometry, derived from the table itself so a longer command
    // can never silently clip the column again (motion.smoothbudget /
    // motion.centringgain / the 34-char motion.policy hint all landed after the
    // old fixed width was chosen).
    size_t nameW = 0, hintW = 0;
    for (const auto& c : commands) {
        if (c.name.size() > nameW) nameW = c.name.size();
        if (c.hint.size() > hintW) hintW = c.hint.size();
    }

    // Suggestion list, RANKED: exact name first, then prefix matches, then
    // anything containing the key. Plain substring matching (the old rule) put
    // `machine.stroke` and `pattern.stroke` above nothing in particular and left
    // an exact hit buried wherever the authoring order happened to put it — so
    // the top row, which is what Tab and Enter complete to, was frequently not
    // the command being typed. Authoring order is preserved WITHIN each rank.
    // Key is lowered because every command name is lowercase.
    auto filtered = [&]() {
        std::vector<const Command*> out;
        out.reserve(commands.size());
        const auto toks = tokens(paletteInput);
        const std::string key = toks.empty() ? "" : lower(toks[0]);
        if (key.empty()) {
            for (const auto& c : commands) out.push_back(&c);
            return out;
        }
        for (int rank = 0; rank < 3; ++rank) {
            for (const auto& c : commands) {
                const size_t at = c.name.find(key);
                if (at == std::string::npos) continue;
                const int r = (c.name == key) ? 0 : (at == 0 ? 1 : 2);
                if (r == rank) out.push_back(&c);
            }
        }
        return out;
    };

    // THE one place the selection index is resolved against the CURRENT list.
    // Clamping only in the renderer (the old arrangement) was not enough:
    // ScreenInteractive::RunOnce drains the WHOLE pending event queue before it
    // redraws, so a burst of ArrowDown/PageDown followed by Tab or Enter in the
    // same batch reached the handlers with an index past the end — Tab indexed
    // the vector out of bounds, and Enter's own divergent `-> 0` clamp completed
    // a different command than the row last drawn as highlighted.
    auto clampSel = [&](const std::vector<const Command*>& list) {
        if (list.empty()) {
            paletteSel = 0;
            paletteScroll = 0;
            return;
        }
        if (paletteSel >= int(list.size())) paletteSel = int(list.size()) - 1;
        if (paletteSel < 0) paletteSel = 0;
    };

    // Completing rewrites the filter, so the list under the cursor is about to
    // become a different list — park the selection at the top of it rather than
    // leaving the highlight on whatever row that index now means.
    auto completeTo = [&](const Command* c) {
        paletteInput = c->name + " ";
        paletteSel = 0;
        paletteScroll = 0;
    };

    auto execPalette = [&] {
        const auto toks = tokens(paletteInput);
        auto list = filtered();
        clampSel(list);
        if (toks.empty() || list.empty()) return;
        // Exact first-token match runs; otherwise Enter completes the selection.
        const Command* exact = nullptr;
        for (const auto& c : commands)
            if (c.name == lower(toks[0])) exact = &c;
        if (!exact) {
            completeTo(list[size_t(paletteSel)]);
            return;
        }
        std::vector<std::string> args(toks.begin() + 1, toks.end());
        const std::string result = exact->run(args);
        if (!result.empty()) {
            say(result);
            log.logf('I', "cmd: %s -> %s", paletteInput.c_str(), result.c_str());
        }
        paletteOpen = false;
        paletteInput.clear();
        paletteSel = 0;
    };

    // ---- Graph --------------------------------------------------------------
    auto graphElement = [&](int cols, int posRows, int velRows) -> Element {
        const int W = cols * 2, Hp = posRows * 4, Hv = velRows * 4;
        const auto s = sim.snapshot();
        const size_t n = sim.traceCount();
        Canvas cp(W, Hp), cv(W, Hv);

        const double tEnd = graphFrozen ? freezeT : now_t();
        const double t0 = tEnd - windowS;
        const float rail = s.max_rail > 1 ? s.max_rail : 1;
        const float vspan = s.max_rail > 0 ? 700.0f : 700.0f;  // vel axis ±700 mm/s

        // window band guides on the position plot
        const int yMin = int((1.0f - s.win_min / rail) * float(Hp - 1));
        const int yMax = int((1.0f - s.win_max / rail) * float(Hp - 1));
        for (int x = 0; x < W; x += 7) {
            cp.DrawPoint(x, yMin, true, kChrome);
            cp.DrawPoint(x, yMax, true, kChrome);
        }
        cv.DrawPointLine(0, Hv / 2, W - 1, Hv / 2, kChrome);

        if (n > 1) {
            int pxPos = -1, pyPos = 0, pyTgt = 0, pyVel = 0;
            for (int x = 0; x < W; ++x) {
                const double t = t0 + (double(x) / double(W - 1)) * windowS;
                const double newest = double(sim.traceAt(n - 1).t_s);
                const long back = lround((newest - t) * 1000.0);
                if (back < 0 || back >= long(n)) continue;
                const auto& sm = sim.traceAt(n - 1 - size_t(back));
                const int yP = int((1.0f - sm.pos_mm / rail) * float(Hp - 1));
                const int yT = int((1.0f - sm.tgt_mm / rail) * float(Hp - 1));
                float vn = sm.vel_mm_s / vspan;
                if (vn > 1) vn = 1;
                if (vn < -1) vn = -1;
                const int yV = int((1.0f - (vn * 0.5f + 0.5f)) * float(Hv - 1));
                if (pxPos >= 0) {
                    cp.DrawPointLine(pxPos, pyTgt, x, yT, kTgt);
                    cp.DrawPointLine(pxPos, pyPos, x, yP, s.estop ? Color::Red : kPos);
                    cv.DrawPointLine(pxPos, pyVel, x, yV, kVel);
                }
                pxPos = x;
                pyPos = yP;
                pyTgt = yT;
                pyVel = yV;
            }
        }

        auto legend = hbox({
            text("pos") | color(kPos), text(" ── ") | color(kChrome),
            text("tgt") | color(kTgt), text(" ── ") | color(kChrome),
            text("vel") | color(kVel),
            filler(),
            text(graphFrozen ? fmt("FROZEN @%.1fs  [%.1fs window]", freezeT, windowS)
                             : fmt("live  [%.1fs window]", windowS)) |
                color(graphFrozen ? kAccent : kChrome),
        });
        return vbox({legend, canvas(std::move(cp)), canvas(std::move(cv))});
    };

    // ---- Inbound wire recorder panel ----------------------------------------
    // Ground truth on what the CLIENT SENT, not on what the machine did with
    // it. Reads MachineSim's ingress ring unlocked — same rule as the graph
    // pane: the TUI runs ON the sim thread, so it cannot race the writer.
    auto segmentsElement = [&](size_t rows) -> Element {
        const auto st = sim.ingressStats();
        const size_t n = sim.ingressCount();
        Elements out;

        // Red flags first — this line is the whole point of the panel.
        const bool flagFloor = st.dur_floor_10ms > 0;
        const bool flagShort = st.dur_under_50ms > 0;
        const bool flagDuty = st.segments >= 3 && st.duty_ratio > 0.0f && st.duty_ratio < 0.9f;
        out.push_back(hbox({
            text("   seg ") | color(kChrome), text(fmt("%u", unsigned(st.segments))),
            text("  smp ") | color(kChrome), text(fmt("%u", unsigned(st.samples))) | color(kChrome),
            text("  ≤10ms ") | color(kChrome),
            text(fmt("%u", unsigned(st.dur_floor_10ms))) | (flagFloor ? color(Color::Red) | bold : color(kChrome)),
            text("  <50ms ") | color(kChrome),
            text(fmt("%u", unsigned(st.dur_under_50ms))) | (flagShort ? color(kTgt) | bold : color(kChrome)),
            // Gap readout follows whichever channel is actually carrying the
            // stream — segments win when both are live, because that is the
            // feed whose cadence is diagnostic.
            text(st.segments > 0 ? "  seg gap " : "  smp gap ") | color(kChrome),
            text(fmt("%.0f/%.0f/%.0f",
                     double(st.segments > 0 ? st.seg_gap.min_ms : st.smp_gap.min_ms),
                     double(st.segments > 0 ? st.seg_gap.mean_ms : st.smp_gap.mean_ms),
                     double(st.segments > 0 ? st.seg_gap.max_ms : st.smp_gap.max_ms))),
            text("  duty ") | color(kChrome),
            text(fmt("%.2f", double(st.duty_ratio))) | (flagDuty ? color(Color::Red) | bold : color(kPos)),
            // "sntl" not "sent": these are segments that arrived WITH the
            // no-end-velocity sentinel, nothing to do with transmission.
            text(fmt("  sntl %u  rej %u", unsigned(st.sentinel), unsigned(st.rejected))) | color(kChrome),
        }));

        if (n == 0) {
            out.push_back(hbox({text("   "), text("no inbound stream recorded yet") | color(kChrome)}));
            return vbox(std::move(out));
        }
        const size_t show = n < rows ? n : rows;
        for (size_t i = n - show; i < n; ++i) {
            const auto& r = sim.ingressAt(i);
            const bool seg = (r.channel_id == 0x0085);
            const double dur_ms = double(r.duration_us) / 1000.0;
            Color durC = kChrome;
            if (seg) {
                durC = r.raw_dur_ms <= 10 ? Color::Red : (r.raw_dur_ms < 50 ? kTgt : Color::Default);
            }
            out.push_back(hbox({
                text("   "),
                text(fmt("%8.3f", double(r.t_s))) | color(kChrome),
                text(fmt("  %04X", unsigned(r.channel_id))) | color(kChrome),
                text(fmt("  %6.4f", double(r.target))) | color(kTgt),
                text(seg ? fmt("  %5.0fms", dur_ms) : std::string("       —")) | color(durC),
                text(r.has_end_vel ? fmt("  %+7.3f", double(r.end_vel)) : std::string("     SENT")) |
                    color(r.has_end_vel ? kVel : kChrome),
                text(r.gap_ms < 0 ? std::string("       —") : fmt("  %6.0fms", double(r.gap_ms))) |
                    color(kDim),
                text(fmt("  due %+6.1fms", double(r.due_delta_ms))) | color(kChrome),
                r.accepted ? text("") : text("  REJECTED") | color(Color::Red) | bold,
                r.ts_clamped ? text("  TS-CLAMP") | color(Color::Red) : text(""),
            }));
        }
        return vbox(std::move(out));
    };

    // ---- Engine config panel ------------------------------------------------
    // "there's currently no way to see what motion policy is set, where the
    // clamp is" (operator). This is that readout, and it is a GROUND-TRUTH one:
    // every field is read from sim.engineConfig() — the slopmotion::Config the
    // engine is actually planning with — never from a TUI-side memory of what a
    // setter was asked for. The same struct backs GET /api/slopmotion's "tuning"
    // block, so the panel and the API cannot disagree about the machine.
    //
    // Built from MANY SHORT fmt() calls on purpose: fmt() truncates SILENTLY at
    // 512 bytes, and the single line this replaces was already within one knob
    // of that ceiling (and long past the width of any terminal).
    //
    // Layout is three columns, ~94 cols wide at full size. The chase column is
    // the one marked shrinkable, so a narrow terminal gives up chase tuning
    // detail before it gives up the policy and the clamps.
    auto configElement = [&](const MachineSim::Snapshot& s) -> Element {
        const auto& c = sim.engineConfig();
        const float span = sim.windowSpanMm();

        // 13 = the longest label (12) + one column of gap. A %-12s pad would let
        // "scale margin" and "settle grace" run straight into their own values.
        // LABELS MUST STAY ASCII: printf pads to a BYTE count while the terminal
        // lays out GLYPHS, so a label carrying a "≤" (3 bytes, 1 cell) comes out
        // two cells short and drags its value out of the column.
        auto kv = [](const char* k, std::string v, Color vc = Color::Default) {
            return hbox({text(fmt("%-13s", k)) | color(kChrome), text(std::move(v)) | color(vc)});
        };
        auto head = [](const char* t) { return text(t) | color(kAccent) | bold; };
        auto onoff = [](bool b) { return b ? "on" : "off"; };

        Elements pol{
            head("waveform policy"),
            kv("infeasible", kInfeasPolicyName(c.infeasible_policy), kAccent),
            kv("curve", kCurvePolicyName(c.curve_policy), kAccent),
            kv("smooth bud", fmt("%.2f", double(c.infeasible_smooth_budget))),
            kv("amp bud", fmt("%.2f", double(c.infeasible_amplitude_budget))),
            kv("blend steps", fmt("%u", unsigned(c.infeasible_blend_steps))),
            kv("reshape st", fmt("%u", unsigned(c.infeasible_reshape_steps))),
            kv("scale margin", fmt("%.2f", double(c.infeasible_scale_margin))),
            kv("soften", fmt("%s  floor %.3f", onoff(c.infeasible_soften),
                             double(c.infeasible_soften_floor))),
            kv("soften steps", fmt("%u", unsigned(c.infeasible_soften_steps))),
            // 0 is the documented "guard disabled" value, not a small factor —
            // say so rather than printing a bare 0.00 that reads like a setting.
            kv("handoff k", c.handoff_chord_factor > 0.0f
                                ? fmt("%.2f", double(c.handoff_chord_factor))
                                : std::string("0.00 (off)")),
            kv("settle grace", fmt("%.0f ms", double(c.settle_grace_us) / 1000.0)),
        };

        // The mm figures are the APPLIED normalized ceiling × the window span —
        // the same conversion the engine's own glue (deriveEngineLimits) does in
        // reverse, and the only honest one available here: MachineSim exposes no
        // mm accessor for the input speed/accel set, and quoting a TUI-side copy
        // of the mm request instead of the ceiling in force is exactly the lie
        // the ground-truth doctrine forbids.
        Elements lim{
            head("limits (ceilings)"),
            kv("vmax", fmt("%.2f /s ≡ %.0f mm/s", double(c.limits.vmax),
                           double(c.limits.vmax * span))),
            kv("amax", fmt("%.1f /s² ≡ %.0f mm/s²", double(c.limits.amax),
                           double(c.limits.amax * span))),
            kv("jmax", fmt("%.0f /s³ ≡ %.0f mm/s³", double(c.limits.jmax),
                           double(c.limits.jmax * span))),
            kv("jmax from", sim.jerkOverrideNorm() > 0.0f ? "OVERRIDE (motion.jmax)"
                                                          : "derived: jerk.input",
               sim.jerkOverrideNorm() > 0.0f ? kTgt : Color::Default),
            kv("input jerk", fmt("%.0f mm/s³", double(sim.inputJerkMmS3()))),
            kv("window", fmt("[%.0f .. %.0f] mm", double(s.win_min), double(s.win_max))),
            kv("window span", fmt("%.1f mm", double(span))),
            kv("max rail", fmt("%.1f mm", double(s.max_rail))),
            // effectiveCeilingMm() falls back to the rail when nothing was ever
            // measured — say WHICH one is in force, not just the number.
            kv("stroke ceil", fmt("%.1f mm (%s)", double(sim.effectiveCeilingMm()),
                                  sim.measuredStrokeMm() > 0.0f ? "measured" : "rail")),
            kv("speed mode", sim.streamSpeedMode() == 1 ? "matched" : "pegged (default)"),
        };

        Elements chase{
            head("chase"),
            kv("feedforward", onoff(c.chase_feedforward)),
            kv("ff gain", fmt("%.2f", double(c.chase_ff_gain))),
            kv("accel ff", onoff(c.chase_accel_ff)),
            kv("aim extrap", onoff(c.chase_aim_accel_extrap)),
            kv("lookahead", fmt("%.2f intervals", double(c.chase_lookahead))),
            kv("dense if <=", fmt("%.0f ms", double(c.chase_dense_us) / 1000.0)),
            kv("stale after", fmt("%.0f ms", double(c.chase_stale_us) / 1000.0)),
            text(""),
            head("centring"),
            kv("wave centre", onoff(c.wave_centering)),
            kv("centre gain", fmt("%.2f", double(c.wave_centering_gain))),
        };

        // Column widths are the longest label (13) + the longest value each
        // group can produce. RESPONSIVE, and not for cosmetics: at 80 columns
        // the three-across layout leaves the chase column ~12 cells, which
        // ftxui clips mid-LABEL — a readout that shows "feedforwardo" is worse
        // than one that shows nothing, so below the threshold the chase group
        // moves under the other two instead of being crushed.
        constexpr int kW1 = 30, kW2 = 40, kW3 = 27;
        if (screen.dimx() >= 3 + kW1 + kW2 + kW3) {
            return hbox({
                text("   "),
                vbox(std::move(pol)) | size(WIDTH, EQUAL, kW1),
                vbox(std::move(lim)) | size(WIDTH, EQUAL, kW2),
                vbox(std::move(chase)) | xflex_shrink,
            });
        }
        return vbox({
            hbox({text("   "), vbox(std::move(pol)) | size(WIDTH, EQUAL, kW1),
                  vbox(std::move(lim)) | xflex_shrink}),
            hbox({text("   "), vbox(std::move(chase)) | xflex_shrink}),
        });
    };

    // ---- Main document ------------------------------------------------------
    auto renderer = Renderer([&] {
        const auto s = sim.snapshot();
        const int cols = 76;

        auto header = hbox({
            text(" ✳ ") | color(kAccent) | bold,
            text("slopsim ") | bold,
            text("machine") | color(kDim),
            text(fmt("  ·  slopsync.v1 :%u  ·  http :80  ·  mdns slopsim._slopsync._tcp", unsigned(wsPort))) | color(kChrome),
            filler(),
            s.estop ? text(" E-STOP ") | bgcolor(Color::Red) | color(Color::White) | bold : text(""),
        });

        auto motionLine = hbox({
            text("pos ") | color(kChrome), text(fmt("%7.2f", double(s.pos_mm))) | color(kPos) | bold,
            text("  tgt ") | color(kChrome), text(fmt("%7.2f", double(s.tgt_mm))) | color(kTgt),
            text("  vel ") | color(kChrome), text(fmt("%+7.1f", double(s.vel_mm_s))) | color(kVel),
            text("  win ") | color(kChrome), text(fmt("[%.0f..%.0f]", double(s.win_min), double(s.win_max))),
            text("  lim ") | color(kChrome), text("i1000/60000-style") | color(kChrome),
            text(fmt("  %s/%s", nameOf(kEngineModes, s.engine_mode).c_str(),
                     nameOf(kPlanKinds, s.engine_plan).c_str())) | color(kAccent),
        });

        auto flags = hbox({
            chip("HOMED", s.homed, kPos), text("  "),
            chip("HOMING", s.homing, kTgt), text("  "),
            chip("PATTERN", s.pattern_running, Color::Cyan), text("  "),
            chip("PAUSED", s.paused, kTgt), text("  "),
            chip("OVERRIDE", s.override_on, kTgt), text("  "),
            chip("ESTOP", s.estop, Color::Red),
        });

        Elements body;
        body.push_back(header);
        body.push_back(separatorLight() | color(kChrome));

        if (!graphZoom) {
            body.push_back(text(""));
            body.push_back(railElement(s, cols));
            body.push_back(motionLine);
            body.push_back(flags);
            body.push_back(text(""));
        }

        if (cfgPanel && !graphZoom) {
            body.push_back(section("engine config (applied)"));
            body.push_back(configElement(s));
            body.push_back(text(""));
        }

        if (graphOn) {
            body.push_back(section("motion"));
            body.push_back(graphElement(cols, graphZoom ? 28 : 10, graphZoom ? 8 : 4));
        }

        if (!graphZoom) {
            // AUTO: show while a stream is live (anything recorded in the last
            // 10 s), so the panel appears exactly when a client starts sending
            // and folds away again when it stops.
            const size_t nIng = sim.ingressCount();
            const bool streamLive =
                nIng > 0 && (now_t() - double(sim.ingressAt(nIng - 1).t_s)) < 10.0;
            const bool showSegs = segMode == 1 || (segMode == 0 && streamLive);
            if (showSegs) {
                body.push_back(section(segMode == 1 ? "wire in (0x0084/0x0085) — pinned"
                                                    : "wire in (0x0084/0x0085)"));
                body.push_back(segmentsElement(8));
            }

            body.push_back(section("sessions"));
            for (uint8_t i = 0; i < SlopSimWsPort::kSlots; ++i) {
                const auto& sl = s.slots[i];
                body.push_back(
                    hbox({text(i == selSlot ? " ▸ " : "   ") | color(kAccent),
                          text(fmt("%u  ", unsigned(i))) | color(kChrome),
                          text(fmt("%-22s", sl.inUse ? sl.peer.c_str() : "—")) | (sl.inUse ? color(Color::Default) : color(kChrome)),
                          text(fmt("  acc %u  drop %u", unsigned(sl.stream_accepted), unsigned(sl.stream_dropped))) | color(kChrome),
                          sl.muted ? text("  MUTED") | color(kTgt) : text("")}));
            }
            body.push_back(hbox({text("   ") ,
                                 text(fmt("sessions %u · bundles %u · samples %u · enq %u · drop %u · planrej %u · tick %.2f/%.1fms · pattern %s · odo %u strokes %.1fm",
                                          unsigned(s.sessions), unsigned(s.sync_bundles), unsigned(s.sync_samples),
                                          unsigned(s.sync_enqueued), unsigned(s.sync_dropped),
                                          unsigned(s.plan_rejected),
                                          double(s.tick_ms_avg), double(s.tick_ms_max),
                                          s.pattern_running
                                              ? nameOf(kPatternNames, s.pattern_idx).c_str()
                                              : "off",
                                          unsigned(s.strokes), double(s.distance_m))) |
                                     color(kChrome)}));

            // ---- Engine anomaly breakdown ---------------------------------
            // The engine's ring used to be drained by NOBODY, so every one of
            // these events was invisible here. Compact by design: names only
            // appear once their count is nonzero, so a clean run stays quiet
            // and the line becomes self-explaining the moment something fires.
            {
                Elements anom{
                    text("   "),
                    text("anomalies ") | color(kChrome),
                    text(fmt("%u", unsigned(s.anomalies))) |
                        (s.anomalies ? color(kTgt) | bold : color(kChrome)),
                };
                for (size_t k = 1; k < kSmAnomalyNameCount; ++k) {
                    if (!s.anom_kind[k]) continue;
                    // waveform_scaled/_fallback mean the planner could NOT
                    // honour the command as sent — red. The rest are the
                    // engine working as designed.
                    const bool loud = (k == 5 || k == 6 || k == 1);
                    anom.push_back(text(fmt("  %s ", kSmAnomalyNames[k])) | color(kChrome));
                    anom.push_back(text(fmt("%u", unsigned(s.anom_kind[k]))) |
                                   (loud ? color(Color::Red) | bold : color(kPos)));
                }
                if (s.anomalies == 0)
                    anom.push_back(text("  (engine ring clean)") | color(kChrome));
                body.push_back(hbox(std::move(anom)));
            }

            body.push_back(section("log"));
            for (const auto& l : log.tail(8)) {
                Color c = l.level == 'E' ? Color::Red : l.level == 'W' ? kTgt : kChrome;
                body.push_back(hbox({text("   "), text(l.text) | color(c)}));
            }
        }

        body.push_back(filler());
        const bool toastLive = std::chrono::steady_clock::now() < toastUntil;
        body.push_back(separatorLight() | color(kChrome));
        body.push_back(hbox({
            text(" / ") | color(kAccent) | bold, text("commands   ") | color(kChrome),
            text("a ") | color(kAccent), text("analyze   ") | color(kChrome),
            text("g ") | color(kAccent), text("graph   ") | color(kChrome),
            text("m ") | color(kAccent), text("config   ") | color(kChrome),
            text("s ") | color(kAccent), text("wire-in   ") | color(kChrome),
            text("z ") | color(kAccent), text("zoom   ") | color(kChrome),
            text("f ") | color(kAccent), text("freeze   ") | color(kChrome),
            text("q ") | color(kAccent), text("quit") | color(kChrome),
            filler(),
            toastLive ? text(toast + " ") | color(kAccent) : text(""),
        }));

        Element doc = vbox(std::move(body));

        if (paletteOpen) {
            auto list = filtered();
            clampSel(list);
            Elements rows;
            rows.push_back(hbox({text(" ❯ ") | color(kAccent) | bold, text(paletteInput),
                                 text("▌") | color(kAccent)}));
            rows.push_back(separatorLight() | color(kChrome));

            // Scrolling viewport over the filtered list. The selection drives
            // the window (not the other way round): moving past either edge
            // drags `paletteScroll` along by exactly one row, so the cursor is
            // always visible and long command lists stay reachable.
            const int total = int(list.size());
            // Rows are capped by the TERMINAL too, not just by taste: 18 rows
            // plus the input line, the two rules and the border overflow a short
            // window, and an overlay taller than the screen loses its bottom
            // (including the "N more" counter that says the list continues).
            int maxRows = screen.dimy() - 8;
            if (maxRows > 18) maxRows = 18;
            if (maxRows < 4) maxRows = 4;
            const int view = total < maxRows ? total : maxRows;
            if (paletteSel < paletteScroll) paletteScroll = paletteSel;
            if (paletteSel >= paletteScroll + view) paletteScroll = paletteSel - view + 1;
            if (paletteScroll > total - view) paletteScroll = total - view;
            if (paletteScroll < 0) paletteScroll = 0;

            // WHY THE NAME AND ARGS COLUMNS ARE PINNED, AND ONLY THE DESCRIPTION
            // SHRINKS: when an hbox is too narrow, ftxui (box_helper::Compute)
            // takes the ComputeShrinkHard path if NOTHING in the row is
            // shrinkable, and that scales EVERY cell down in proportion — which
            // is exactly why long command names were losing their tails to a
            // long description. Giving the description flex_shrink puts the row
            // on the ComputeShrinkEasy path instead, where the fixed-size cells
            // are left alone and the shrinkable one absorbs the whole deficit.
            for (int i = paletteScroll; i < paletteScroll + view; ++i) {
                const auto* c = list[size_t(i)];
                const bool sel = (i == paletteSel);
                auto row = hbox({text(sel ? " ▸ " : "   ") | color(kAccent),
                                 text(c->name) | size(WIDTH, EQUAL, int(nameW)) |
                                     (sel ? bold : color(Color::Default)),
                                 text(" "),
                                 text(c->hint) | size(WIDTH, EQUAL, int(hintW)) | color(kChrome),
                                 text("  "),
                                 text(c->desc) | color(kChrome) | xflex_shrink,
                                 text(" ")});
                rows.push_back(row);
            }
            if (list.empty()) rows.push_back(text("   no matching command") | color(kChrome));
            // Off-screen counts, so it is obvious the list continues.
            if (total > view) {
                const int above = paletteScroll;
                const int below = total - view - paletteScroll;
                rows.push_back(separatorLight() | color(kChrome));
                rows.push_back(hbox({
                    text(above > 0 ? fmt("   ▲ %d more", above) : "   ") | color(kChrome),
                    filler(),
                    text(below > 0 ? fmt("▼ %d more   ", below) : "   ") | color(kChrome),
                }));
            }
            // Width is DERIVED from the table (longest name + longest args hint)
            // plus a description budget, then capped to what the terminal can
            // actually show — a fixed 72 was the old value and it is 20 columns
            // short of the current longest row, which is how names started
            // getting cut. Capping matters because the overlay is CENTERED: a
            // box wider than the screen is clipped on BOTH sides, and the left
            // edge is where the names are.
            const int kDescBudget = 44;   // enough for most descs; the rest ellipse off
            const int rowW = 3 + int(nameW) + 1 + int(hintW) + 2 + kDescBudget + 1;
            const int avail = screen.dimx() - 4;   // border + a column of margin
            int palW = rowW < avail ? rowW : avail;
            if (palW < 40) palW = 40;
            // clear_under: paint over the cells beneath the overlay — without it
            // the live document bleeds through the palette's unpainted cells.
            auto pal = vbox(std::move(rows)) | size(WIDTH, EQUAL, palW) | borderRounded |
                       color(Color::Default) | clear_under;
            doc = dbox({doc, pal | center});
        }
        return doc;
    });

    auto component = CatchEvent(renderer, [&](Event e) {
        if (paletteOpen) {
            if (e == Event::Escape) { paletteOpen = false; paletteInput.clear(); return true; }
            if (e == Event::Return) { execPalette(); return true; }
            // Editing the filter re-lists, so the cursor goes back to the top —
            // keeping a stale index would leave the highlight on an unrelated
            // command after the list under it changed.
            if (e == Event::Backspace) {
                if (!paletteInput.empty()) paletteInput.pop_back();
                paletteSel = 0;
                paletteScroll = 0;
                return true;
            }
            // Tab completes the highlighted suggestion. The clamp is NOT
            // optional here: this used to index the filtered vector with an
            // index the renderer had not had a chance to bound yet (see
            // clampSel), which is an out-of-bounds read, not a cosmetic slip.
            if (e == Event::Tab) {
                auto list = filtered();
                clampSel(list);
                if (!list.empty()) completeTo(list[size_t(paletteSel)]);
                return true;
            }
            // Every mover clamps against the list it just moved within, so the
            // index handlers see is always the index the screen last drew.
            if (e == Event::ArrowUp) { if (paletteSel > 0) --paletteSel; return true; }
            if (e == Event::ArrowDown) { ++paletteSel; clampSel(filtered()); return true; }
            if (e == Event::PageUp) { paletteSel -= 10; if (paletteSel < 0) paletteSel = 0; return true; }
            if (e == Event::PageDown) { paletteSel += 10; clampSel(filtered()); return true; }
            if (e == Event::Home) { paletteSel = 0; paletteScroll = 0; return true; }
            if (e == Event::End) {
                auto list = filtered();
                paletteSel = int(list.size());   // past the end; clampSel lands it on the last row
                clampSel(list);
                return true;
            }
            if (e.is_character()) {
                paletteInput += e.character();
                paletteSel = 0;
                paletteScroll = 0;
                return true;
            }
            return true;  // swallow everything else while open
        }

        if (e == Event::Character('/')) {
            paletteOpen = true;
            paletteInput.clear();
            paletteSel = 0;
            paletteScroll = 0;
            return true;
        }
        if (e == Event::Character('q')) { screen.ExitLoopClosure()(); return true; }
        if (e == Event::Character('a')) {
            openBrowser(analyzeUrl);
            say("analyzer popped out -> " + analyzeUrl);
            return true;
        }
        if (e == Event::Character('g')) { graphOn = !graphOn; return true; }
        if (e == Event::Character('m')) {
            cfgPanel = !cfgPanel;
            say(cfgPanel ? "engine config panel: on" : "engine config panel: off");
            return true;
        }
        if (e == Event::Character('s')) {
            segMode = (segMode + 1) % 3;
            say(segMode == 0 ? "wire recorder: auto (shown while a stream is live)"
                             : segMode == 1 ? "wire recorder: pinned on"
                                            : "wire recorder: off");
            return true;
        }
        if (e == Event::Character('z')) { graphZoom = !graphZoom; return true; }
        if (e == Event::Character('f')) {
            graphFrozen = !graphFrozen;
            if (graphFrozen) freezeT = now_t();
            say(graphFrozen ? "graph frozen — ←/→ pan, +/- zoom, /export saves CSV" : "graph live");
            return true;
        }
        if (e == Event::Character('h')) { sim.startHoming(); return true; }
        if (e == Event::Character('u')) { sim.forceUnhome(); return true; }
        if (e == Event::Character('e')) { sim.injectEstop(); return true; }
        if (e == Event::Character('c')) { sim.injectClearEstop(); return true; }
        if (e == Event::Character('p')) { sim.togglePause(); return true; }
        if (e == Event::Character('o')) { sim.toggleOverride(); return true; }
        if (e == Event::Character('k')) { sim.kickSession(uint8_t(selSlot)); return true; }
        if (e == Event::Character('+') || e == Event::Character('=')) {
            windowS = windowS > 2.0 ? windowS / 1.5 : windowS;
            return true;
        }
        if (e == Event::Character('-')) {
            windowS = windowS < 120.0 ? windowS * 1.5 : windowS;
            return true;
        }
        if (e == Event::ArrowLeft && graphFrozen) { freezeT -= windowS / 4.0; return true; }
        if (e == Event::ArrowRight && graphFrozen) {
            freezeT += windowS / 4.0;
            if (freezeT > now_t()) freezeT = now_t();
            return true;
        }
        if (e == Event::ArrowUp) { if (selSlot > 0) --selSlot; return true; }
        if (e == Event::ArrowDown) { if (selSlot < SlopSimWsPort::kSlots - 1) ++selSlot; return true; }
        return false;
    });

    Loop loop(&screen, component);
    auto nextTick = std::chrono::steady_clock::now();
    auto lastRedraw = std::chrono::steady_clock::now();
    MachineSim::MotionDigest lastDigest{};
    size_t lastLogRev = size_t(-1);
    bool lastToastLive = false;
    while (!loop.HasQuitted()) {
        sim.tick();
        loop.RunOnce();  // input events render on their own; below is DATA-driven redraw

        // Redraw gate: repaint only when visible state actually changed (plus a
        // 1 s heartbeat, and a 30 fps cadence while a live graph pane animates).
        // An idle screen paints nothing — the fix for terminal flicker.
        const auto now = std::chrono::steady_clock::now();
        const auto sinceRedraw = now - lastRedraw;
        const auto digest = sim.motionDigest();
        const size_t logRev = log.revision();
        const bool toastLive = now < toastUntil;
        const bool graphAnimating = graphOn && !graphFrozen;
        const bool changed = !(digest == lastDigest) || logRev != lastLogRev || toastLive != lastToastLive;
        const bool due = graphAnimating ? sinceRedraw >= std::chrono::milliseconds(33)
                                        : (changed ? sinceRedraw >= std::chrono::milliseconds(33)
                                                   : sinceRedraw >= std::chrono::seconds(1));
        if (due) {
            lastRedraw = now;
            lastDigest = digest;
            lastLogRev = logRev;
            lastToastLive = toastLive;
            screen.PostEvent(Event::Custom);
        }
        // 1 ms motion cadence, deadline-based — the TUI shares the sim thread,
        // so a slow repaint must not stretch the substep grid (tickMachine
        // replays the missed 1 ms instants when it does). Process-wide 1 ms
        // scheduler tick comes from main()'s HostTimerResolution.
        nextTick += std::chrono::milliseconds(1);
        if (nextTick < now) nextTick = now;
        preciseSleepUntil(nextTick);
    }
    return 0;
}

}  // namespace slopsim
