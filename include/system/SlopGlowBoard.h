// SlopGlowBoard -- this board's wiring of the SlopGlow engine. Owns the LEDC
// true-color RGB and the mono heartbeat lamp, maps SystemState onto the
// semantic GlowState vocabulary, and exposes the heartbeat sources the
// monitored tasks pulse.
//
// Threading: slopglowUpdate() is pumped by httpTask (Core 0) only.
// Heartbeat pulses are safe from any task.
#pragma once

#include "slopglow/slopglow.h"

struct SystemState;

void slopglowInit();                                   // post-boot only (GPIO0 is a strapping pin)
void slopglowUpdate(const SystemState& state);         // pump from httpTask (~10ms)

// Pulse these every loop iteration of the monitored tasks. Either one going
// silent freezes ALL status LEDs mid-animation — that's the point.
slopglow::HeartbeatSource* slopglowMotorHeartbeat();   // motorTask, Core 1
slopglow::HeartbeatSource* slopglowCommsHeartbeat();   // commsTask, Core 0

// For modules that raise their own conditions (OTA, pairing, SlopSync).
slopglow::GlowEngine& slopglowEngine();
