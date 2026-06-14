#pragma once
#include <cstdint>
#include <chrono>

// ============================================================================
// PositionTime — waypoint struct for the Core 0 → Core 1 motion queue
// ============================================================================
//
// Pushed by Core 0 (buttplugLinearCmd, TCode parser callback) into
// g_waypoint_queue. Popped by Core 1 (motionConsumerTask) which runs the
// OSSM kinematic pipeline and dispatches to FastAccelStepper.
//
// Mirrors OSSM's PositionTime struct from queue.h — same fields, same
// semantics. The queue owns the memory (FreeRTOS copies by value), so
// there are no lifetime or pointer-safety concerns. :3
//
// position:      0–100 (TCode normalized 0.0–1.0 × 100, mapped to steps by consumer)
// inTime:        milliseconds (the TCode I-parameter — how long to reach position)
// has_set_time:  true when setTime is valid — populated by the transport layer
//                so the Core 1 consumer can measure actual transport lag and apply
//                OSSM-style latency compensation (shorten timeSeconds up to 25%
//                to resync). If false, compensation is skipped cleanly.
struct PositionTime {
    uint8_t  position;       // 0–100
    uint16_t inTime;         // ms
    bool     has_set_time;   // true = setTime field is valid
    std::chrono::steady_clock::time_point setTime;  // RX timestamp (valid iff has_set_time)
};
