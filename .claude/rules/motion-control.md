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
- **Whichever task calls `commit()` needs a deep stack** because it nests
  KB-scale Ruckig temporaries. Never size one down without a measured
  high-water mark under a real motion workload (T1 class, T21).

## Direction changes: the v=0 gate

**DEPRECATED 2026-09-03 (sd-4k1.8).** The step-and-direction backend this
section describes was deleted with the one-motion-backend ruling
(`architecture.md` section 1). The drive is saved in encoder-follow and the
RP2350 clocks quadrature; there is no DIR pin to race any more. The bullets
below are kept as the record of what the step/dir path cost, and are NOT a
description of the running machine.

**The general rule that survives the backend, and binds the RP2350:** a
reversal must land while the output line is IDLE. Any renderer that flips
direction while pulses are still in flight rewrites the widest pulse of the
move, and a pre-rendered pipeline cannot fix it after the fact. Quadrature has
no separate direction line, so the class is gone by construction rather than
by tuning.


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

**DEPRECATED 2026-09-03 (sd-4k1.8).** No S3 peripheral generates steps any
more; the library these bullets describe is out of `lib_deps`. Kept as record.

**The general rule that survives:** a renderer that buffers nothing ahead
turns every late refill into dead air on the output. That is why pulse
generation moved to a board with nothing else to do, and why the S3 must never
take back a real-time render duty.


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
  (src/system/OomHook.cpp).
- Task map (src/main.cpp): motorTask C1/p3, PatternEngine's own task C1/p2,
  commsTask C0/p2, httpTask C0/p1. Core 0 = comms, Core 1 = motion
  (architecture.md §2). motorTask owns the SPI link: it is the link's single
  owner, and no other task may drive that bus (MlinkServoDriver.h).

## Stack and assignment traps (TRAPS T1, T9)

- Never `obj = T{}` on big objects: the RHS temporary builds on the CURRENT
  stack (T1, cpp-safety.md; canon_lint this-assign rule). A task that calls
  `commit()` needs stack for KB-scale Ruckig temporaries; that is why the
  engine's host lives on the RP2350 and why its stack sizing is its own issue
  (sd-4k1.11).
- T9 stands as a rule with no live instance: a forwarding proxy never restates
  a base class's default argument, because defaults bind to the STATIC type.
  There is no proxy in this tree today (one motion backend, bound directly);
  the rule binds the next one.
