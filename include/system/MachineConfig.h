#pragma once

#include <cstdint>

// ============================================================================
// MachineConfig — tiny NVS accessors for the machine-modes settings
// ============================================================================
//
// Namespace "machcfg" is DELIBERATELY SEPARATE from the main settings
// namespace ("strokeengine", see ConfigStore.cpp) so it's readable
// first-thing in setup() — before ConfigStore::load(), before
// aimGeometryInit(), before ANY motor.* call. A shared namespace would tangle
// this read with the rest of DeviceConfig's load order for no benefit.
//
// Two keys, both INERT since the one-motion-backend ruling (architecture.md
// section 1): nothing reads either to make a decision. They are still stored
// and echoed so the released 0x3030 keys 5 and 6 are not silently
// repurposed; retiring them is a wire evolution and an operator ruling.
//   "backend"   — written by the motion_backend setting (0x3030 key 5).
//   "homestyle" — written by the home_style setting (0x3030 key 6).
uint8_t machineBackendLoad();
void    machineBackendStore(uint8_t v);

uint8_t machineHomeStyleLoad();
void    machineHomeStyleStore(uint8_t v);

// "accelreg" -- desired AIM drive ramp register 0x03, (r/min)/s. 0 = leave the
// drive alone. Reconciled against the drive at boot, never on a hot path.
// See docs/drive-accel-register.md.
uint16_t machineAccelRegLoad();
void     machineAccelRegStore(uint16_t v);
