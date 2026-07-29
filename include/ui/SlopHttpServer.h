#pragma once

// SlopHttpServer — the HTTP backend type name every call site uses
//
// Constraints:
//   Zero-member subclass of IdleGuardWebServer: the synchronous Arduino core
//   WebServer plus the speculative-socket idle guard. No members, no overrides,
//   inherited constructors — code generation and static RAM are identical to
//   using IdleGuardWebServer directly.
//
//   It stays a distinct name for two reasons, neither cosmetic: WebUI.h and
//   OtaService.h forward-declare it rather than including the WebServer
//   headers, and ~57 call sites across 11 files name it. Renaming buys
//   nothing.
//
//   This was an A/B seam (DOCTRINE.md §1) whose B side wrapped PsychicHttp.
//   That A/B was retired 2026-07-29 — the sync WebServer is the only HTTP
//   backend now, and ESP32Async is the only WS transport. Handler bodies in
//   src/ui/WebUI.cpp call send()/arg()/method()/sendHeader()/streamFile() and
//   that surface is unchanged.

#include "ui/IdleGuardWebServer.h"

class SlopHttpServer : public IdleGuardWebServer {
public:
    using IdleGuardWebServer::IdleGuardWebServer;
};
