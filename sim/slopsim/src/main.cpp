// slopsim — SlopDrive-32 terminal simulator entry point (roadmap §6 "SlopSim").
// Constraints:
//   Machine mode is a virtual SlopDrive: the REAL slopsync::Hub + REAL
//   slopmotion::Engine behind a real WebSocket server, with FAS modeled at
//   the MotorDriver seam. Flag usage is the single copy printed below (the
//   unknown-mode branch); it is not restated here to avoid the two drifting.
// See: python tools/slopsync_probe.py --ip 127.0.0.1 (conformance check)

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>

#include "common/HostPlatform.h"
#include "common/SessionLog.h"
#include "machine/MachineSim.h"
#include "net/HttpFacade.h"
#include "net/MdnsAdvertiser.h"
#include "tui/MachineScreen.h"

namespace {
std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
    using namespace slopsim;

    uint16_t port = 82;
    uint16_t httpPort = 80;
    bool homed = false;
    // Opens the RFC-027(c) push-to-pair presence window at boot, standing in
    // for the firmware's physical-presence gesture (a future NVS boot
    // counter). The library provides the WINDOW; deciding presence was
    // proven is the application's job, and on a simulator the flag IS the
    // gesture.
    bool pairingWindow = false;
    bool headless = false;
    int durationS = 0;
    std::string webuiPath;
    bool noMdns = false;
    // SlopMotion engine knobs settable at launch. They exist as flags (not just
    // palette commands) because comparing engine policies is a SCRIPTED job —
    // headless run, drive the wire, read /api/trace.bin — and the palette needs
    // a terminal. Empty/NaN = "leave the engine default alone".
    std::string policy;
    int   reshapeSteps = -1;    // <0 = leave the engine default (6)
    // Budgeted-policy caps (slopmotion 0.8.0). <0 = leave the engine default
    // (0.5 each). Each policy reads only ITS OWN budget — see InfeasiblePolicy.
    float smoothBudget    = -1.0f;
    float amplitudeBudget = -1.0f;
    // Waveform curve family. Empty = leave the engine default (FollowClient,
    // which is today's C2 behavior until curve_family wire signaling lands).
    std::string curve;
    float settleGrace  = -1.0f; // ms; <0 = leave the engine default (30 ms)
    float jmax = 0.0f;   // NORMALIZED override (units/s^3); 0 = derive
    float jerk = 0.0f;   // mm-domain INPUT jerk ceiling (mm/s^3); 0 = leave default
    std::string speedmode;      // pegged|matched (SystemState::stream_speed_mode)
    // DC centering of a degraded band (slopmotion 0.5.0). Empty/<0 = leave the
    // engine default (on, gain 1.0) — the A/B this exists for is exactly
    // "centered vs not" on a scripted infeasible chain.
    std::string centering;       // on|off
    float centeringGain = -1.0f; // <0 = leave the engine default (1.0)
    bool noTimerBoost = false;  // A/B escape hatch for the 1 ms scheduler tick
    // Catalog profile (SlopDeck DESIGN.md §5/§7 sim-fidelity ruling). `device`
    // is the DEFAULT: the real SlopDrive-32 catalog, byte-for-byte. `alien` is
    // benchrig, a deliberately different conformant hub. `minimal` is a subset
    // of the real device catalog — the potato-client floor.
    std::string profileArg = "device";

    // claude-CLI shape: a bare `slopsim` (or `SlopCLI`) drops straight into the
    // machine TUI; subcommands stay for scripting. Flags may follow either way.
    std::string mode = "machine";
    int flagStart = 1;
    if (argc > 1 && argv[1][0] != '-') {
        mode = argv[1];
        flagStart = 2;
    }

    for (int i = flagStart; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = uint16_t(atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--http") && i + 1 < argc) httpPort = uint16_t(atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--homed")) homed = true;
        else if (!std::strcmp(argv[i], "--headless")) headless = true;
        else if (!std::strcmp(argv[i], "--duration") && i + 1 < argc) durationS = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--webui") && i + 1 < argc) webuiPath = argv[++i];
        else if (!std::strcmp(argv[i], "--no-mdns")) noMdns = true;
        else if (!std::strcmp(argv[i], "--pairing-window")) pairingWindow = true;
        else if (!std::strcmp(argv[i], "--policy") && i + 1 < argc) policy = argv[++i];
        else if (!std::strcmp(argv[i], "--reshape-steps") && i + 1 < argc) reshapeSteps = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--curve") && i + 1 < argc) curve = argv[++i];
        else if (!std::strcmp(argv[i], "--smooth-budget") && i + 1 < argc) smoothBudget = float(atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--amplitude-budget") && i + 1 < argc) amplitudeBudget = float(atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--settle-grace") && i + 1 < argc) settleGrace = float(atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--jmax") && i + 1 < argc) jmax = float(atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--jerk") && i + 1 < argc) jerk = float(atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--speedmode") && i + 1 < argc) speedmode = argv[++i];
        else if (!std::strcmp(argv[i], "--centering") && i + 1 < argc) centering = argv[++i];
        else if (!std::strcmp(argv[i], "--centering-gain") && i + 1 < argc) centeringGain = float(atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--no-timer-boost")) noTimerBoost = true;
        else if (!std::strcmp(argv[i], "--profile") && i + 1 < argc) profileArg = argv[++i];
    }

    if (profileArg != "device" && profileArg != "alien" && profileArg != "minimal") {
        std::fprintf(stderr, "slopsim: --profile takes 'device', 'alien' or 'minimal'\n");
        return 2;
    }
    const Profile profile = profileArg == "alien"   ? Profile::Alien
                           : profileArg == "minimal" ? Profile::Minimal
                                                      : Profile::Device;

    if (!policy.empty() && policy != "scale" && policy != "stretch" &&
        policy != "reshape" && policy != "amp" && policy != "prio-amplitude" &&
        policy != "smooth" && policy != "prio-smooth") {
        std::fprintf(stderr, "slopsim: --policy takes 'scale', 'stretch', "
                             "'reshape', 'amp' or 'smooth'\n");
        return 2;
    }
    if (!curve.empty() && curve != "follow" && curve != "c1" && curve != "c2") {
        std::fprintf(stderr, "slopsim: --curve takes 'follow', 'c1' or 'c2'\n");
        return 2;
    }
    if (smoothBudget > 1.0f || (smoothBudget < 0.0f && smoothBudget != -1.0f)) {
        std::fprintf(stderr, "slopsim: --smooth-budget takes 0.0..1.0\n");
        return 2;
    }
    if (amplitudeBudget > 1.0f ||
        (amplitudeBudget < 0.0f && amplitudeBudget != -1.0f)) {
        std::fprintf(stderr, "slopsim: --amplitude-budget takes 0.0..1.0\n");
        return 2;
    }
    if (reshapeSteps > 8 || (reshapeSteps < 0 && reshapeSteps != -1)) {
        std::fprintf(stderr, "slopsim: --reshape-steps takes 0..8\n");
        return 2;
    }
    if (settleGrace > 200.0f) {
        std::fprintf(stderr, "slopsim: --settle-grace takes 0..200 (ms)\n");
        return 2;
    }
    if (!speedmode.empty() && speedmode != "pegged" && speedmode != "matched") {
        std::fprintf(stderr, "slopsim: --speedmode takes 'pegged' or 'matched'\n");
        return 2;
    }
    if (!centering.empty() && centering != "on" && centering != "off") {
        std::fprintf(stderr, "slopsim: --centering takes 'on' or 'off'\n");
        return 2;
    }
    if (centeringGain > 1.0f) {
        std::fprintf(stderr, "slopsim: --centering-gain takes 0..1\n");
        return 2;
    }

    if (mode == "client") {
        std::fprintf(stderr, "slopsim client: cockpit lands in the next milestone — "
                             "machine mode is live, point clients at it.\n");
        return 2;
    }
    if (mode != "machine") {
        std::fprintf(stderr,
                     "usage:\n"
                     "  slopsim                     launch the machine TUI (/ opens the palette)\n"
                     "  slopsim machine [--port 82] [--http 80] [--homed] [--headless]\n"
                     "                  [--duration S] [--webui <dist/index.html>] [--no-mdns]\n"
                     "                  [--policy scale|stretch|reshape|amp|smooth]\n"
                     "                                        infeasible-segment policy (engine\n"
                     "                                        default: reshape). amp = prioritize\n"
                     "                                        amplitude, smooth = prioritize smooth\n"
                     "                  [--reshape-steps 0-8] reshape bisection depth (default 6;\n"
                     "                                        0 = no bisection)\n"
                     "                  [--smooth-budget 0-1] prio-amplitude: max handle reduction\n"
                     "                                        toward the chord (default 0.5)\n"
                     "                  [--amplitude-budget 0-1]\n"
                     "                                        prio-smooth: max fraction of the\n"
                     "                                        stroke surrendered (default 0.5)\n"
                     "                  [--settle-grace <ms>] expired-plan grace before braking\n"
                     "                                        to rest (default 30; 0 = pre-0.4)\n"
                     "                  [--jerk <mm/s^3>]     INPUT-set jerk ceiling (mechanical\n"
                     "                                        limit; default 2000000, /span -> jmax)\n"
                     "                  [--jmax <units/s^3>]  NORMALIZED jerk override; wins over\n"
                     "                                        --jerk when > 0 (0 = derive)\n"
                     "                  [--centering on|off]   midpoint-anchored stroke shortening\n"
                     "                                        when the machine can't reach\n"
                     "                                        (engine default: on)\n"
                     "                  [--centering-gain 0-1] centering strength (default 1.0;\n"
                     "                                        a feel dial, not monotone)\n"
                     "                  [--speedmode pegged|matched]  stream speed feed\n"
                     "                                        (device default: pegged)\n"
                     "                  [--no-timer-boost]    leave Windows at its 15.6 ms timer\n"
                     "                                        tick (A/B only — wrecks fidelity)\n"
                     "                  [--profile device|alien|minimal]\n"
                     "                                        catalog profile (default: device — the\n"
                     "                                        real SlopDrive-32 catalog, byte-for-byte;\n"
                     "                                        alien = benchrig, deliberately different;\n"
                     "                                        minimal = a subset of device)\n"
                     "  slopsim client <host>       (not yet)\n");
        return 2;
    }

    // 1 ms scheduler tick for the whole process, released on scope exit.
    // Everything below — the headless loop AND the TUI loop — depends on it.
    HostTimerResolution timerRes(!noTimerBoost);

    ix::initNetSystem();
    std::signal(SIGINT, onSignal);

    SessionLog log;
    if (headless) log.setEcho(true);
    log.logf('I', "sim: catalog profile = %s (--profile)", profileArg.c_str());
    MachineSim sim(log, profile);
    // Applied before begin() so the very first planned segment already sees
    // them; both go through the same setters the palette uses (one seam).
    if (!policy.empty()) {
        // FIVE policies since slopmotion 0.8.0 — Reshape is still the engine
        // default and must be reachable from the CLI, or a scripted A/B can
        // only ever compare the policies that are NOT the default. The budgeted
        // pair take the short names the TUI uses (`amp` / `smooth`), because the
        // A/B this flag exists for is exactly prio-amplitude vs prio-smooth.
        const auto applied = sim.uiSetInfeasiblePolicy(
            policy == "scale"   ? slopmotion::InfeasiblePolicy::Scale
          : policy == "reshape" ? slopmotion::InfeasiblePolicy::Reshape
          : (policy == "amp" || policy == "prio-amplitude")
                                ? slopmotion::InfeasiblePolicy::PrioritizeAmplitude
          : (policy == "smooth" || policy == "prio-smooth")
                                ? slopmotion::InfeasiblePolicy::PrioritizeSmooth
                                : slopmotion::InfeasiblePolicy::Stretch);
        log.logf('I', "sim: infeasible policy = %s (--policy)",
                 applied == slopmotion::InfeasiblePolicy::Scale   ? "scale"
               : applied == slopmotion::InfeasiblePolicy::Reshape ? "reshape"
               : applied == slopmotion::InfeasiblePolicy::PrioritizeAmplitude
                     ? "prio-amplitude"
               : applied == slopmotion::InfeasiblePolicy::PrioritizeSmooth
                     ? "prio-smooth"
                                                                  : "stretch");
    }
    if (!curve.empty()) {
        const auto applied = sim.uiSetCurvePolicy(
            curve == "c1" ? slopmotion::CurvePolicy::ForceC1
          : curve == "c2" ? slopmotion::CurvePolicy::ForceC2
                          : slopmotion::CurvePolicy::FollowClient);
        log.logf('I', "sim: curve policy = %s (--curve)",
                 applied == slopmotion::CurvePolicy::ForceC1 ? "c1-cubic"
               : applied == slopmotion::CurvePolicy::ForceC2 ? "c2-quintic"
                                                             : "follow(->c2)");
    }
    if (smoothBudget >= 0.0f) {
        log.logf('I', "sim: smooth budget = %.2f (--smooth-budget)",
                 double(sim.uiSetSmoothBudget(smoothBudget)));
    }
    if (amplitudeBudget >= 0.0f) {
        log.logf('I', "sim: amplitude budget = %.2f (--amplitude-budget)",
                 double(sim.uiSetAmplitudeBudget(amplitudeBudget)));
    }
    if (reshapeSteps >= 0) {
        log.logf('I', "sim: reshape steps = %u (--reshape-steps)",
                 unsigned(sim.uiSetReshapeSteps(uint8_t(reshapeSteps))));
    }
    if (settleGrace >= 0.0f) {
        log.logf('I', "sim: settle grace = %.0f ms (--settle-grace)",
                 double(sim.uiSetSettleGraceMs(settleGrace)));
    }
    // --jerk first: it is the mm-domain limit the normalized ceiling derives
    // from, so a run passing BOTH ends up with --jmax on top (override wins).
    if (jerk > 0.0f) {
        log.logf('I', "sim: input jerk = %.0f mm/s^3 (--jerk)",
                 double(sim.uiSetLimit(MachineSim::LimitKind::InputJerk, jerk)));
    }
    if (jmax > 0.0f) {
        log.logf('I', "sim: jmax override = %.0f units/s^3 (--jmax)", double(sim.uiSetJmax(jmax)));
    }
    if (!centering.empty()) {
        log.logf('I', "sim: waveform centering = %s (--centering)",
                 sim.uiSetWaveCentering(centering == "on") ? "on" : "off");
    }
    if (centeringGain >= 0.0f) {
        log.logf('I', "sim: centering gain = %.2f (--centering-gain)",
                 double(sim.uiSetWaveCenteringGain(centeringGain)));
    }
    if (!speedmode.empty()) {
        const uint8_t m = sim.uiSetStreamSpeedMode(speedmode == "matched" ? 1 : 0);
        log.logf('I', "sim: stream speed mode = %s (--speedmode)", m == 1 ? "matched" : "pegged");
    }
    log.logf(timerRes.held() ? 'I' : 'W', "sim: host timer tick %s",
             timerRes.held() ? "1 ms (timeBeginPeriod)" : "OS DEFAULT (~15.6 ms on Windows)");
    if (pairingWindow) {
        // Factory-fresh (no configure token in the ledger yet) means the first
        // knock in this window gets `configure` — physical possession is root.
        sim.hub().openPresenceWindow();
        log.logf('I', "sim: push-to-pair window OPEN (--pairing-window); first knock grants %s",
                 sim.hub().pairing().hasConfigureToken() ? "control" : "configure");
    }
    if (!sim.begin(port, homed)) {
        std::fprintf(stderr, "slopsim: failed to start WS listener on :%u\n", unsigned(port));
        return 1;
    }

    HttpFacade http;
    http.begin(&sim, httpPort, port, &log, webuiPath);  // bind failure is non-fatal (logged)

    MdnsAdvertiser mdns;
    if (!noMdns) mdns.begin(port, &log);  // discovery parity with the firmware

    int rc = 0;
    if (headless) {
        std::printf("slopsim machine (headless) — slopsync.v1 on :%u%s\n", unsigned(port),
                    homed ? " [pre-homed]" : "");
        const auto start = std::chrono::steady_clock::now();
        // Fixed 1 ms cadence, deadline-based (not sleep-after-work) so tick
        // cost doesn't stack onto the period — the motion substep grid is only
        // as good as this loop.
        auto next = std::chrono::steady_clock::now();
        while (!g_stop) {
            sim.tick();
            next += std::chrono::milliseconds(1);
            const auto now = std::chrono::steady_clock::now();
            if (next < now) next = now;  // fell behind: don't spiral, resync
            preciseSleepUntil(next);
            if (durationS > 0 &&
                std::chrono::steady_clock::now() - start > std::chrono::seconds(durationS)) {
                break;
            }
        }
    } else {
        rc = runMachineScreen(sim, log, port, httpPort);
    }

    mdns.stop();
    http.stop();
    sim.shutdown();
    ix::uninitNetSystem();
    return rc;
}
