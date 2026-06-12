#pragma once

#include <Arduino.h>
#include "SystemState.h"

// Forward declarations — ConfigStore operates on these through references
// without needing the full class definitions in the header.
class RangeMapper;
class MotorController;

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
    static void save(SystemState& state, RangeMapper& mapper, MotorController& motor);

    // Load persisted settings (or factory defaults if NVS is empty/unreadable).
    // Populates state, mapper, and motor in-place.
    static void load(SystemState& state, RangeMapper& mapper, MotorController& motor);
};