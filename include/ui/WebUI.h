#pragma once

#include <Arduino.h>
#include "SystemState.h"

// Forward declarations — WebUI stores references to these, not values.
class MotorDriver;
class RangeMapper;
class Generator;
class Interpolator;
class TransportManager;
class SerialTransport;
class WebSocketTransport;
class BleTransport;

// Forward-declared to avoid pulling <WebServer.h> into every translation unit
// that includes this header (works around a PlatformIO include-path quirk when
// framework headers are included from a subdirectory header).
class WebServer;

// ============================================================================
// WebUI — all HTTP API handlers + LittleFS root-page serving
// ============================================================================
//
// Owns the WebServer instance.  All route registration, page serving, and
// /api/* JSON handlers live here so main.cpp only does composition wiring.
//
// Handlers take references to every subsystem they query or mutate (injected
// via the constructor) — no global reach-through.
//
// Lifecycle:
//   init()          — registers every route + calls httpServer.begin()
//   update()        — httpServer.handleClient(); call frequently (was httpTask)
//
// Paired with Step 7 cleanup (Risk #2):  handleApiStatus emits a stub
// driver{} block (valid:false) and handleApiClearFault is a no-op.  The
// frontend can hide those cards dynamically via feature-advertised flags
// (JSON modularity per .clinerules §3).

class WebUI {
public:
    WebUI(SystemState&        state,
          MotorDriver&        motor,
          RangeMapper&        mapper,
          Generator&          generator,
          Interpolator&       interpolator,
          TransportManager&   transportMgr,
          SerialTransport&    serialTransport,
          WebSocketTransport& wsTransport,
          BleTransport&       bleTransport);

    ~WebUI();

    /// Register all HTTP routes and start the server.
    void init();

    /// Service the HTTP server (was httpTask body).  Call frequently.
    void update();

private:
    // ---- Injected dependencies ----
    SystemState&        _state;
    MotorDriver&        _motor;
    RangeMapper&        _mapper;
    Generator&          _generator;
    Interpolator&       _interpolator;
    TransportManager&   _transportMgr;
    SerialTransport&    _serialTransport;
    WebSocketTransport& _wsTransport;
    BleTransport&       _bleTransport;

    // ---- Owned instance (pointer — allocated in constructor, freed in dtor) --
    WebServer*          _httpServer = nullptr;

    // ---- Handler methods (one per route) ------------------------------------
    void handleRoot();
    void handleApiStatus();
    void handleApiClearFault();
    void handleApiSettings();
    void handleApiMove();
    void handleApiHome();
    void handleApiStop();
    void handleApiPause();
    void handleApiHalt();
    void handleApiOverride();
    void handleApiTmc();
    void handleApiGen();
    void handleApiInterp();
    void handleApiLog();
    void handleApiMode();
};