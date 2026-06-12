#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "SystemState.h"

class RangeMapper;
class MotorDriver;

// ============================================================================
// Generator — on-device waveform motion generator
// ============================================================================
//
// Self-contained bundle: owns the FreeRTOS task that computes carrier waveforms
// (with optional FM/AM modulation) and streams target positions to the motor.
// Dependencies are injected by reference — no global reach-through.
//
// Lifecycle:
//   init()          — creates the FreeRTOS task (call in setup() after motor init)
//   emergencyStop() — forcibly stops waveform emission (sets running=false)
//
// The task auto-yields when Intiface commands arrive (unless the user has
// paused or overridden). Parameters are set live via the WebUI /api/gen handler
// which writes directly into SystemState::gen.

class Generator {
public:
    Generator(SystemState& state, RangeMapper& mapper, MotorDriver& motor);
    ~Generator();

    // Create the FreeRTOS task (pinned to Core 1, priority 2, 4k stack).
    // Safe to call once during setup().
    void init();

    // Forcibly stop waveform emission. The task remains alive and can be
    // restarted by setting gen.running = true via /api/gen.
    void emergencyStop();

    // Read-only status queries for the WebUI (non-blocking, no lock needed —
    // SystemState scalars are volatile on ESP32-S3).
    bool isRunning() const;
    bool isActive()  const;

private:
    SystemState& _state;
    RangeMapper& _mapper;
    MotorDriver& _motor;

    TaskHandle_t     _task = nullptr;

    // Static trampoline for FreeRTOS → member function dispatch.
    static void taskFunction(void* param);

    // The actual task loop (runs on Core 1).
    void run();
};