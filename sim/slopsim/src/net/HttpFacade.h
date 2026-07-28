#pragma once

// HttpFacade — the sim's minimal HTTP surface: discovery JSON and the built
// webui, never control.
// Constraints:
//   100% of CONTROL goes through SlopSync; HTTP exists only to deliver the
//   page and read-only JSON views (GET /api/capabilities, GET /api/slopmotion,
//   GET / when --webui points at a built bundle).
//   cpp-httplib runs its own listener thread. It NEVER touches the hub or the
//   engine: the sim thread deposits a FacadeStats copy under a mutex
//   (MachineSim::facadeStats), and handlers format JSON from that copy.

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
    bool begin(MachineSim* sim, uint16_t httpPort, uint16_t wsPort, SessionLog* log,
               std::string webuiPath);
    void stop();

private:
    std::unique_ptr<httplib::Server> _srv;
    std::thread _thread;
};

}  // namespace slopsim
