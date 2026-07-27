#pragma once

// ============================================================================
// MdnsAdvertiser — advertises the sim exactly like the firmware advertises
// itself (src/comms/TransportManager.cpp): service `_slopsync._tcp`, TXT
// proto=<ws subprotocol> fw=<version>, so discovery-capable clients (the MFP
// plugin's hand-rolled DNS-SD query, future slopsim client mode) find the
// virtual machine the same way they find hardware.
//
// Windows implementation uses the native mDNS responder (dnsapi.dll,
// DnsServiceRegister — Windows 10 1809+): the OS owns the multicast socket,
// answers PTR/SRV/TXT/A, and de-registers on process exit. No thread of ours,
// no packet code. Non-Windows builds compile to a no-op stub for now (Linux
// would use avahi — future).
//
// Instance name is "slopsim" (never the firmware's "slopdrive32" — a sim on
// the LAN must not impersonate the real machine in discovery).
// ============================================================================

#include <cstdint>

#include "common/SessionLog.h"

namespace slopsim {

class MdnsAdvertiser {
public:
    ~MdnsAdvertiser() { stop(); }

    // Non-fatal on failure (logged): discovery is a convenience, the sim runs on.
    bool begin(uint16_t wsPort, SessionLog* log);
    void stop();

private:
    void* _instance = nullptr;  // PDNS_SERVICE_INSTANCE (opaque here)
    bool _registered = false;
    SessionLog* _log = nullptr;
};

}  // namespace slopsim
