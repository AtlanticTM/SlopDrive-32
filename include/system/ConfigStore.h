#pragma once

#include <Arduino.h>
#include "SystemState.h"

// Forward declarations — ConfigStore operates on these through references
// without needing the full class definitions in the header.
class RangeMapper;
class MotorDriver;

// ============================================================================
// ConfigStore — NVS (non-volatile storage) persistence for runtime settings
// ============================================================================
//
// Owns its own Preferences instance internally.  All I/O happens through the
// two static methods below; callers never touch Preferences directly.
//
//   save(state, mapper, motor)   — persist everything to NVS
//   load(state, mapper, motor)   — read everything from NVS, populating state,
//                                  mapper, and motor with saved values (or
//                                  factory defaults when nothing is stored).
//
// Thread-safety: save/load are always called from Core 0 context (setup + HTTP
// handlers).  They serialise internally via Preferences so no extra mutex is
// needed.

class ConfigStore {
public:
    // Persist all runtime settings to the "strokeengine" NVS namespace.
    static void save(SystemState& state, RangeMapper& mapper, MotorDriver& motor);

    // Load persisted settings (or factory defaults if NVS is empty/unreadable).
    // Populates state, mapper, and motor in-place.
    static void load(SystemState& state, RangeMapper& mapper, MotorDriver& motor);

    // ---- Secondary WiFi credentials (serial-settable fallback) --------------
    // A second SSID/password pair stored in NVS, tried by setupWiFi() when the
    // compile-time primary creds (secrets.h) fail to connect. Set over USB
    // serial with the `WIFI <ssid> <password>` command so a rig on an unknown
    // network can be recovered without a reflash. Stored as NVS strings in the
    // same "strokeengine" namespace (keys "wifi_ssid2" / "wifi_pass2"). :3
    // Both writers take the SystemState so they can honor the OTA-in-flight
    // guard (state.ota_active) — an NVS flash write during the OTA write window
    // can reset the chip, same hazard save() already defers around. :3
    static void saveWifiCreds(const SystemState& state, const char* ssid, const char* pass);
    // Returns true and fills the buffers when a non-empty secondary SSID exists.
    static bool loadWifiCreds(char* ssid, size_t ssidLen, char* pass, size_t passLen);
    static void clearWifiCreds(const SystemState& state);
};
