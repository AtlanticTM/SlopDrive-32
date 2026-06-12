#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "SystemState.h"

class RangeMapper;
class MotorDriver;

// ============================================================================
// Interpolator — time-delayed jitter-buffer playback engine
// ============================================================================
//
// Self-contained bundle: owns the FreeRTOS task that consumes time-stamped
// samples from the ring buffer and replays them on a clock that runs a fixed
// delay behind real wall-clock time.  This turns sporadic/bursty Intiface
// input streams into smooth continuous motion.
//
// Dependencies are injected by reference — no global reach-through.
//
// Lifecycle:
//   init()          — creates the FreeRTOS task (call in setup() after motor init)
//   pushSample(mm)  — push a true Intiface sample (already range-mapped) into the ring
//   emergencyStop() — clear the ring buffer and reset playback state
//
// Parameters (easing, depth, tick rate) are set live via the WebUI /api/interp
// handler which writes directly into SystemState fields.

class Interpolator {
public:
    Interpolator(SystemState& state, RangeMapper& mapper, MotorDriver& motor);
    ~Interpolator();

    // Create the FreeRTOS task (pinned to Core 1, priority 2, 4k stack).
    // Safe to call once during setup().
    void init();

    // Forcibly clear the jitter buffer and reset playback state.
    void emergencyStop();

    // Push a true Intiface sample (already range-mapped to mm) into the ring
    // buffer.  Called from buttplugLinearCmd() while in BUFFERED mode.
    // Timestamps are taken with esp_timer_get_time() (microseconds, monotonic)
    // so the playback clock and sample times share one wall-clock reference.
    void pushSample(float pos_mm);

    // Read-only status query for the WebUI (non-blocking).
    bool isActive() const;

private:
    SystemState& _state;
    RangeMapper& _mapper;
    MotorDriver& _motor;

    TaskHandle_t     _task = nullptr;

    // Playback clock state (was static locals in the original interpolatorTask).
    // In the same ms timebase as the sample timestamps.  0 = not yet started.
    uint32_t _play_clock_ms = 0;
    uint32_t _last_real_us  = 0;

    // Static trampoline for FreeRTOS → member function dispatch.
    static void taskFunction(void* param);

    // The actual task loop (runs on Core 1).
    void run();
};