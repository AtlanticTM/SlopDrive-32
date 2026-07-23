#pragma once

#include <cstdint>

// ============================================================================
// MachineConfig — tiny NVS accessor pair for the runtime motion backend
// ============================================================================
//
// Namespace "machcfg" is DELIBERATELY SEPARATE from the main settings
// namespace ("strokeengine", see ConfigStore.cpp) so it's readable
// first-thing in setup() — before ConfigStore::load(), before aimGeometryInit(),
// before ANY motor.* call. The backend choice decides WHICH concrete driver
// MotorProxy binds to, so it has to be known before anything else touches the
// motor reference. A shared namespace would tangle this read with the rest of
// DeviceConfig's load order for no benefit. :3
//
// Two keys, two independent settings:
//   "backend"   — 0 = FAS step/dir (default), 1 = Modbus direct drive.
//                 Written ONLY by POST /api/machine/commit (WebUI.cpp) —
//                 reboot-to-apply, see the doctrine comment there.
//   "homestyle" — 0 = sensorless current-stall sweep (default),
//                 1 = drive built-in homing (0x19). Live-applied, no reboot
//                 needed (Phase 4 wires the actual behavior switch).
uint8_t machineBackendLoad();
void    machineBackendStore(uint8_t v);

uint8_t machineHomeStyleLoad();
void    machineHomeStyleStore(uint8_t v);
