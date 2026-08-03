---
paths:
  - "src/motion/**"
  - "include/motion/**"
  - "lib/slopmotion/**"
  - "src/main.cpp"
---

# Motion control constraints (pointers; DOCTRINE §2/§8 and TRAPS own the story)

## Direction changes: the v=0 gate

- FAS performs the race-free DIR write only when the step queue is empty AND
  stopped; any other state uses a paused-dwell path that can race. Never
  OR PIN_EXTERNAL_FLAG onto the DIR pin: that forces the racy external-pin
  path (src/motion/AIMServoDriver.cpp:24-29,176).
- AIM_DIR_CHANGE_DELAY_US holds DIR steady before the first reversal step;
  the PCNT retime below lands inside that pause. Do not shrink it
  (AIMServoDriver.cpp:178).
- MCPWM backend: AIM_MCPWM_PCNT_RETIME counts FALLING step edges so DIR flips
  after the step line idles, not mid-pulse. The default RISING count flips
  DIR during the widest pulse of the whole move (AIMServoDriver.cpp:133-162).
- RMT backend has NO equivalent fix (pre-rendered pulses, DIR written from
  the refill ISR). Known, unfixable from config (AIMServoDriver.cpp:89-97).
- Distinct concept, do not conflate: MotionArbiter's trapezoid math assumes
  v0=0 when deriving accel from distance+deadline. Deliberate and safe; FAS
  retargets velocity-continuously (src/motion/MotionArbiter.cpp:17-30).

## MCPWM traps

- FAS 1.2.7 + IDF 5.5 prescaler composition bug: init() rewrites
  timer_prescale against the 32 MHz target (halved for COUNT_MODE_UP_DOWN).
  Group 0 / timer 0 hardcoded; revisit if a second stepper ever exists
  (AIMServoDriver.cpp:107-131).
- MCPWM buffers nothing ahead: every late refill ISR is dead air on the step
  pin. Viable only because the C5 comms offload removed Core-0 radio jitter
  (docs/c5-comms-offload.md §1). Do not re-add radio load to the S3.

## ISR / IRAM / core discipline

- Our motion code uses portMUX microcritical sections, not ISRs: sub-
  microsecond float math only, no heap alloc, no ISR context
  (include/motion/MotionArbiter.h:33). IRAM_ATTR appears once in the repo
  (src/system/OomHook.cpp); FAS's own ISRs are upstream's contract
  (DOCTRINE §11 scope).
- Task map (src/main.cpp:1030-1058): motorTask C1/p3, streamSamplerTask C1/p4
  16 KB stack, commsTask C0/p2, httpTask C0/p1, servoBusTask C1/p5, FAS
  StepperTask pinned C0/p24 DELIBERATELY (on C1 it preempts the sampler into
  audible judder; unpinned it stalls behind Core-0 bursts,
  AIMServoDriver.cpp:63-73). Core 0 = comms, Core 1 = motion (DOCTRINE §2).

## Stack and assignment traps (TRAPS T1, T9)

- Never `obj = T{}` on big objects: the RHS temporary builds on the CURRENT
  stack (T1, canon_lint this-assign rule). The sampler's 16 KB stack exists
  because commit() nests KB-scale Ruckig temporaries; never shrink it
  (DOCTRINE §8).
- Forwarding proxies never restate base-class default args; pass sentinels
  (T9, include/motion/MotorProxy.h:52-58).
