// MotionPassthrough -- GPIO-matrix route: RP2350 pulses in on GPIO1/2, out on
// AIM_PIN_STEP/DIR. Constraints and mechanism: MotionPassthrough.cpp header.
// Never enable while FAS is the live backend and commanding motion.
#pragma once

void motionPassthroughEnable();
void motionPassthroughDisable();
