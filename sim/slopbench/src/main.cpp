// slopbench — the "be anything" SlopSync test hub (docs/canon/LEDGER.md
// "Morning ruling batch" item 3). A dumb, config-file-driven conformant hub:
// point it at a .bench file and it serves exactly the catalog that file
// describes, with a generic INTENT-clamp/echo/STATE-mirror write plane and
// an optional configurable fake STATE-echo delay.
//
//   slopbench <config.bench> [--port 7000] [--headless] [--duration S]
//
// Verify against it with tools/slopsync_probe.py (same wire layer sim/slopsim
// uses) or sim/slopbench/tools/smoke_test.py, which drives all three example
// configs end to end.
// Constraints:
//   - HOST-ONLY: never touches the device, never deploys, no pio.
// See: sim/slopbench/README.md.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>

#include "common/SessionLog.h"
#include "config/ConfigParser.h"
#include "hub/BenchHub.h"
#include "tui/BenchTui.h"

namespace {
std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
    using namespace slopbench;

    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: slopbench <config.bench> [--port 7000] [--headless] [--duration S]\n");
        return 2;
    }
    const std::string configPath = argv[1];

    uint16_t port = 7000;
    bool headless = false;
    int durationS = 0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = uint16_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--headless")) headless = true;
        else if (!std::strcmp(argv[i], "--duration") && i + 1 < argc) durationS = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "slopbench: unknown flag '%s'\n", argv[i]);
            return 2;
        }
    }

    HubConfig cfg;
    std::string err;
    if (!loadConfigFile(configPath, cfg, err)) {
        std::fprintf(stderr, "slopbench: %s\n", err.c_str());
        return 1;
    }

    ix::initNetSystem();
    std::signal(SIGINT, onSignal);

    SessionLog log;
    if (headless) log.setEcho(true);
    log.logf('I', "slopbench: loaded '%s' -- hub '%s', %zu channel(s)",
             configPath.c_str(), cfg.name.c_str(), cfg.channels.size());

    BenchHub hub(log, cfg);
    if (!hub.begin(port)) {
        std::fprintf(stderr, "slopbench: failed to start (see log above)\n");
        return 1;
    }
    log.logf('I', "slopbench: listening on :%u (slopsync.v1)", unsigned(port));

    if (!headless) BenchTui::enableAnsi();

    const auto start = std::chrono::steady_clock::now();
    while (!g_stop) {
        hub.tick();
        if (!headless) {
            BenchTui::draw(hub, log, uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count()));
            if (BenchTui::pollQuit()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (durationS > 0 &&
            std::chrono::steady_clock::now() - start > std::chrono::seconds(durationS)) {
            break;
        }
    }

    hub.shutdown();
    ix::uninitNetSystem();
    return 0;
}
