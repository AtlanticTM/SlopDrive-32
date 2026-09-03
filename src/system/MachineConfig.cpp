#include "MachineConfig.h"

#include <Preferences.h>
#include "config_api.h"

// ============================================================================
// MachineConfig — implementation
// ============================================================================
//
// Namespace "machcfg" — separate from ConfigStore's "strokeengine" namespace
// on purpose (see header doc). Preferences opens/closes per call, same
// lightweight pattern ConfigStore.cpp uses elsewhere — this is read once at
// boot and written only on an explicit commit, never on a hot path.

static const char* MACHCFG_NS = "machcfg";

uint8_t machineBackendLoad() {
    uint8_t v = 0;
#if defined(SD32_MODBUS_TOOLS)
    Preferences prefs;
    if (prefs.begin(MACHCFG_NS, true)) {   // true = read-only
        v = prefs.getUChar("backend", 0);
        prefs.end();
    }
    if (v > 1) v = 0;   // out-of-range NVS value fails safe to FAS
#else
    // Guard: without the Modbus feature compiled in, there is no second
    // backend to select — always FAS, regardless of what NVS might hold from
    // a previous build.
    (void)v;
    v = 0;
#endif
    return v;
}

void machineBackendStore(uint8_t v) {
#if defined(SD32_MODBUS_TOOLS)
    if (v > 1) v = 0;
    Preferences prefs;
    if (prefs.begin(MACHCFG_NS, false)) {   // false = read-write
        prefs.putUChar("backend", v);
        prefs.end();
    }
#else
    (void)v;
#endif
}

uint8_t machineHomeStyleLoad() {
    uint8_t v = 0;
    Preferences prefs;
    if (prefs.begin(MACHCFG_NS, true)) {
        v = prefs.getUChar("homestyle", 0);
        prefs.end();
    }
    if (v > 1) v = 0;
    return v;
}

void machineHomeStyleStore(uint8_t v) {
    if (v > 1) v = 0;
    Preferences prefs;
    if (prefs.begin(MACHCFG_NS, false)) {
        prefs.putUChar("homestyle", v);
        prefs.end();
    }
}

uint16_t machineAccelRegLoad() {
    uint16_t v = 0;
    Preferences prefs;
    if (prefs.begin(MACHCFG_NS, true)) {
        v = prefs.getUShort("accelreg", 0);
        prefs.end();
    }
    return (v > 60098) ? 0 : v;   // out-of-range NVS fails safe to "leave alone"
}

void machineAccelRegStore(uint16_t v) {
    if (v > 60098) v = 60098;
    Preferences prefs;
    if (prefs.begin(MACHCFG_NS, false)) {
        prefs.putUShort("accelreg", v);
        prefs.end();
    }
}
