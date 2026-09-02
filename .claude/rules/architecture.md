---
paths:
  - "**"
---

# Architecture constraints

The product: an extensible, high-performance modular linear motion control
platform on the ESP32-S3 ecosystem. Hardware-agnostic, community-extensible.

## 1. Hardware-agnostic modularity

- **Driver polymorphism.** All physical hardware interaction (motors, sensors,
  inputs) sits behind C++ interface classes, ABCs with pure virtuals.
- **Build configuration.** Conditional-compilation flags (`#if defined(...)`)
  isolate hardware driver implementations; unused driver objects are not
  compiled.
- **Pin allocation.** No hardware pins inside functional classes. Pins, serial
  ports, and timer channels are constructor-injected or mapped in
  `include/system/config_api.h`.
- **Module boundary doctrine (operator-ratified 2026-07-27).** Functional
  cores are LIFTABLE: hardware-free, dependency-injected (clock, randomness,
  transport, motor handed IN, never grabbed), native-testable, stitchable into
  another codebase. The slop-libs are the proof pattern. Glue is the opposite
  and proudly so: the composition root (main.cpp), delegates, and board wiring
  are deliberately machine-specific, small, and honest. The smell to hunt is a
  core-shaped thing living inside glue. New functionality starts by deciding
  which of the two it is.

## 2. Core performance and safety (NON-NEGOTIABLE)

- **Non-blocking runtime.** Operational loops and real-time motion paths never
  block. `delay()` is PROHIBITED during regular runtime; short blocking delays
  are permitted ONLY in boot/init, module hardware setup, and isolated
  slow-speed calibration/homing cycles. The ban means millisecond-scale
  sleeps that stall a task's other duties. A bounded busy-wait under one
  millisecond with a measured hardware reason (the 200 us inter-frame gap
  the RP2350 SPI slave needs) is not a delay in this sense (operator ruling
  2026-09-02).
- **Motion doctrine, event-driven and never clocked.** ONE COMMAND, ONE PLAN,
  the motion engine executes. Plans are computed at intent arrival from the
  machine's ACTUAL state (live position plus velocity); speed and accel are
  DERIVED from the intent and CLAMPED at ceilings. Ceilings are never targets,
  the one exception being deadline-less manual point moves, which plan at user
  ceilings. A loop computing positions on a clock is rebuilding a disease this
  project already cured. Segmentation follows commands and waveform structure
  only.
- **Three-board split (operator-ratified 2026-09-02).** The RP2350 OWNS
  MOTION: it runs slopmotion and pulse generation, holds the plan, and its
  rendered position is the machine's position truth. The S3 is the SlopSync
  hub and the policy owner. The C5 is the network peripheral. Motion commands
  (samples, C1/C2 segments, point moves, estop) cross the S3-RP link as
  INTENTS with anchor times, never as rendered chunks: the RP evaluates the
  plan at its own tick, so there is no runway to starve and no re-render.
  Commands are sent the moment they arrive, never on a tick (the link is a
  bus, not a schedule); only status polling may be periodic. Any planner that
  speaks the link vocabulary is a valid motion processor; the vocabulary
  carries an axis id from day one (multi-axis is a parked goal, no effort
  now). The encoder is the AUDITOR of that truth: the drive follows quadrature
  exactly unless asked for the impossible, so a calc-vs-encoder deviation
  means an infeasible demand reached the motor. Ceilings are therefore
  measured and enforced in the engine; the RP emitter cap is a fault
  detector, never a shaper. Landing state on the dev board (sd-4k1). Until
  the port lands, the S3-side driver and its segment path remain the live
  implementation.
- **MotionArbiter sole-caller rule.** The MotionArbiter is the ONLY component
  that commands the motion processor. Input sources (manual UI, TCode
  transports, PatternEngine, SlopSync) never touch the link: they submit
  intents. The arbiter owns arbitration, limit-set selection (user set for
  manual, input set for machine-driven), and every safety gate -- homed,
  paused, e-stop, window clamping, soft-start. Under the three-board split the
  gates are POLICY on the S3, pushed to the RP as config and ENFORCED there,
  where the curve is evaluated.
- **Dual-core separation.** Core 0 is system and comms (networking, LittleFS,
  WebSockets, transport parsing, monitoring). Core 1 is motion real-time
  (arbiter dispatch, plan submission, step timing). PLANNED CHANGE (sd-4k1):
  after the port, Core 1 hosts the link driver, Modbus and homing only; the
  real-time path lives on the RP.
- **Cross-core data.** Anything shared between cores uses FreeRTOS primitives
  (atomics, mutexes, `xQueue`). Async-library callbacks run on the library's
  own task: enqueue, never mutate owner state. See `.claude/rules/transport.md`
  T5.

## 3. Naming doctrine

Invented ecosystem-level things (protocols, subsystems, tools) get
zero-collision, SEO-unique names: "SlopSync", never "SyncManager". Ordinary
classes and variables keep plain descriptive names.

## 4. SSManager -- the tool surface has ONE door

SSManager is the SlopSync project manager: one UI over every tool in the
ecosystem. Its home is the SlopSync repo so it ships with the SDK; a vendor
building a hub who never clones SlopDrive still gets it. Landing state lives
on the dev board.

- **Every tool registers a manifest entry, and that is the whole of adding
  it.** A tool is declarative data: name, command, input globs, how its
  pass/fail reads, what toolchain it needs. Adding tool N+1 must require ZERO
  changes to SSManager's own code. If SSManager has to learn about a tool, the
  registration is wrong: fix the manifest schema, not the console.
- **SSManager knows nothing tool-specific.** No branch anywhere may name a
  tool, a repo, a language, or a build system. It reads manifests and spawns
  processes. A single `if tool == ...` is the whole design failing, and it is
  a flag, not a shortcut.
- **Standalone invocation NEVER stops working.** Every tool stays runnable
  from a plain shell exactly as it is today. SSManager is a funnel, not a
  gate: CI, headless agents, and an operator with a terminal must never depend
  on it. A tool that only works through the UI is a defect.
- **A result carries a fingerprint of its inputs; this is C-4 in software.**
  Results are stored against a hash of the entry's declared inputs. When those
  inputs move the result goes STALE, never "failed": stale means
  no-longer-evidence, which is exactly C-4's "touched by commits since its
  stamp is hearsay". Never show a stale pass as a pass.
- **The manifest is the home for how-to-run.** PLANNED CHANGE: the build and
  deploy procedure in `.claude/rules/build-test-deploy.md` becomes a pointer
  into the manifest in the same commit that lands v0. Until then that file
  remains the home (C-1), so do not split it early and do not let both stand
  afterward.
