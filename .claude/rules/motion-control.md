---
paths:
  - "**"
---

# Motion control constraints

Architecture-level motion doctrine (one command one plan, the MotionArbiter
sole-caller rule, dual-core separation) lives in
`.claude/rules/architecture.md` §2. This file is the mechanism layer.

## SlopMotion (`lib/slopmotion/`)

Every command becomes ONE trajectory planned from the engine's actual
(p, v, a); the 1 kHz sampler evaluates it. Event-driven, never clocked.

- **Map:** header-only, hardware-free `slopmotion::Engine` wrapping vendored
  `lib/ruckig/`, which is BYTE-IDENTICAL to upstream. Wrap, never patch; see
  `lib/ruckig/VENDORED.md`. Tests in `test/native/test_slopmotion`; scenario
  harness in `examples/slopmotion_traces/`, which includes the retired cubic
  as a bench baseline and is not firmware.
- **Division of labor (MEASURED; re-run the bench before re-litigating).**
  Ruckig Community is a point-to-point planner, not a waveform interpolator.
  WAVEFORM (every duration-carrying segment, with NO duration floor: a 10 ms
  knot is a 10 ms span with its authored tangent) is a Hermite curve in the
  client's declared family over exactly the commanded duration, ceiling and
  window scanned. An illegal shape is first shortened toward a legal stroke
  that still holds the deadline (the Blend policy; the amplitude budget is a
  FLOOR the search must honor, never cross), and only a shape still illegal at
  that floor falls through to the Ruckig guard. Stretch (keep the stroke,
  overrun the deadline) is the one alternative contract. Amplitude is the one
  quantity a ceiling may shape; this is the operator-ratified exception
  (2026-09-02) to "ceilings are clamps, never targets". CHASE (bare points,
  no duration) is Ruckig replan-per-point. Sample synthesis is gone
  (2026-09-02): no client sends bare points at a rate that needs a holdback.
  SETTLE is brake-to-rest when a plan ends still-moving with no fresh command.
- **One activity clock (operator ruling 2026-09-02).** Every "is the stream
  alive" question in the engine (settle grace, cold start, staleness) keys on
  ONE reference stamped by every commit and every plan end, in the engine's
  own clock. A reset voids the plan and the pipeline; it never erases the
  stream's cadence. Two mechanisms answering the same question from different
  references is the defect class that produced every field hitch of
  2026-09-02 (docs/reviews/slopmotion-2026-09-02).
- **Safety:** Ruckig Community has NO position limits and quintics can bulge,
  so the Engine owns the window: targets clamped, end velocities bound-safe,
  quintics legality-scanned, sampled output clamped. Exceptions are never
  instantiated; non-finite inputs are rejected at `commit()`.
- **The sampler task stack is 16 KB** because `commit()` nests KB-scale Ruckig
  temporaries. Never shrink it (T1 class).

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
  (cpp-safety.md scope).
- Task map (src/main.cpp:1030-1058): motorTask C1/p3, streamSamplerTask C1/p4
  16 KB stack, commsTask C0/p2, httpTask C0/p1, servoBusTask C1/p5, FAS
  StepperTask pinned C0/p24 DELIBERATELY (on C1 it preempts the sampler into
  audible judder; unpinned it stalls behind Core-0 bursts,
  AIMServoDriver.cpp:63-73). Core 0 = comms, Core 1 = motion (architecture.md §2).

## Stack and assignment traps (TRAPS T1, T9)

- Never `obj = T{}` on big objects: the RHS temporary builds on the CURRENT
  stack (T1, cpp-safety.md; canon_lint this-assign rule). The sampler's 16 KB stack exists
  because commit() nests KB-scale Ruckig temporaries; never shrink it
  (SlopMotion section above).
- Forwarding proxies never restate base-class default args; pass sentinels
  (T9, include/motion/MotorProxy.h:52-58).
