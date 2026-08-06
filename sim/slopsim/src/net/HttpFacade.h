#pragma once

// HttpFacade — the sim's minimal HTTP surface: discovery JSON, the built webui,
// and the analyzer's async-tune bench. Never control.
// Constraints:
//   100% of CONTROL goes through SlopSync; HTTP exists only to deliver the
//   page and read-only JSON views (GET /api/capabilities, GET /api/slopmotion,
//   GET / when --webui points at a built bundle).
//
//   OPERATOR RULING (2026-07-30): this facade is DEBUG SCAFFOLDING and sits
//   outside the conformant hub surface entirely — a SlopSync-compatible hub
//   needs no HTTP at all. So the async-tune endpoints writing files
//   (/api/rec/save, /api/run/save) are not a widening of the control rule and
//   did not warrant a second server. The rule that still binds, verbatim: NO
//   HTTP HANDLER MAY WRITE MACHINE STATE. Names arriving from query strings are
//   filesystem input and go through RecordingStore::sanitizeName() without
//   exception.
//
//   cpp-httplib runs its own listener thread. It NEVER touches the hub or the
//   engine: the sim thread deposits a FacadeStats copy under a mutex
//   (MachineSim::facadeStats), and handlers format JSON from that copy. The
//   replay endpoints keep that invariant a different way — they build their OWN
//   slopmotion::Engine per request (MotionReplay), so a tuning sweep runs
//   entirely on the listener thread while the sim thread drives the real virtual
//   machine, with no shared state to contend for.

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "common/SessionLog.h"

namespace httplib {
class Server;
}

namespace slopsim {

class MachineSim;

class HttpFacade {
public:
    // Ctor/dtor out-of-line: members hold unique_ptr<httplib::Server> over a
    // forward declaration, so every special member that could destroy it must
    // be emitted where the full type is visible (HttpFacade.cpp).
    HttpFacade();
    ~HttpFacade();

    // Returns false (logged) if the port can't be bound — the sim keeps
    // running; only the HTTP conveniences are lost.
    // `recordingsDir` empty = RecordingStore's default (`slopsim-recordings` in
    // the working directory).
    bool begin(MachineSim* sim, uint16_t httpPort, uint16_t wsPort, SessionLog* log,
               std::string webuiPath, std::string recordingsDir = {});
    void stop();

private:
    std::unique_ptr<httplib::Server> _srv;
    std::thread _thread;
};

}  // namespace slopsim
