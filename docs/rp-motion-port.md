# RP2350 owns motion -- the port contract (sd-4k1.4)

The one text both halves are built against. Doctrine: `.claude/rules/architecture.md` section 2 (three-board split) and `.claude/rules/motion-control.md`. Vocabulary: `include/comms/MotionLinkProtocol.h` (v2 ops, frozen for the port at the commit that adds this file). Engine: `lib/slopmotion/include/slopmotion/slopmotion.hpp` after the 2026-09-03 refactor (one activity clock, scheduled anchors, honest seed, Blend plus Stretch only, no synthesis). Status of the work lives on the dev board, never here.

## Split of responsibilities

| concern | RP2350 (`src/rp2350_motion`) | S3 (`src/motion`, `src/main.cpp`, hub) |
|---|---|---|
| the plan | owns the `slopmotion::Engine`; commits on core 1; renders at the 20 kHz tick on core 0 | never samples, never re-renders; the S3 engine instance and the sampler task are DELETED |
| position truth | the rendered position; reported in every status | reads it from status; `EncoderValidator` audits it against the drive encoder |
| commands | `kOpCommand` in, queued to core 1, committed at once | arbiter gates and forwards on ARRIVAL, never on a tick (`MotionArbiter` is the sole caller) |
| ceilings | holds BOTH sets (`kCfgInput*`, `kCfgUser*`), selects per command by `limit_set`, folds `kCfgSoftStartCap`; RP-initiated maneuvers (settle, recovery, window entry) use the USER set | pushes both sets, the window, the gates, the soft-start ramp value and the engine tuning as `kOpConfig` tags, on change |
| window | `kCfgWindowMinCounts/Max`: normalized 0..1 spans exactly that; plans outside are honest and must move inward (engine rule) | the arbiter's own clamp stays as policy; window edits push new tags |
| gates | `kCfgGates` bits; a denied command is dropped and reported `kEvtCommandGated`; motion denied mid-stroke brakes to rest from live state | homed, paused, override are policy on the S3; e-stop stays `kOpEstop` |
| anomalies | engine anomaly ring drained into the event ring; `kOpEventPull` | pulls when `event_seq` moves; feeds the SlopSync anomaly channel and the plan strip |
| clock | slave microseconds; `ClockFilter` on every frame; `kEvtClockStep` on restart | translates anchors into slave time once, at send (`clockDelta`), never elsewhere |
| homing | `kOpRetarget`, `kOpSetPos`, the stall probe: unchanged; `kOpSetPos` re-seeds the engine at the honest normalized position | `MlinkServoDriver` homing sequence unchanged (INA228, sweeps, glides) |
| firmware update | `kOpFlash*`, unchanged | `RpFlashLink`, unchanged |
| emitter cap | `kOpSetLimits` stays a FAULT DETECTOR: counts, never shapes | pushes it once at link-up as today |

## RP core is hardware-free

`include/comms/RpMotionCore.h` (header-only, no Arduino, no pico headers; the pattern is `include/comms/RpFlashCore.h`): owns the engine, the `ConfigImage`, the clock filter, the command queue, the event ring, the render plan hand-off, the status fill. `src/rp2350_motion/main.cpp` is glue: SPI DMA slave, PIO stepgen, watchdog, flash, LEDs. `test/native/test_rpmotion` drives the core with frames produced by the S3-side encoder and checks rendered positions against the engine run natively.

Core boundary on the RP: the frame processor runs in SPI IRQ context and may only decode and enqueue; `commit()` is milliseconds and runs on core 1; the 20 kHz tick on core 0 evaluates a PUBLISHED render plan (coefficients, kind, start, duration, or a copied Ruckig trajectory) behind a sequence lock, never the engine object itself. Core 1 advances the engine (settle, promotion, coast) at least every millisecond and republishes when the plan changed. A plan is a function of time, so a late switch is still continuous.

## Units

Engine: normalized over the window. Wire: `LinkCommand` normalized, `kCfgWindow*` in counts, status `pos`/`vel` in counts. The RP converts with the window span; a normalized value outside 0..1 is a real position outside the window, never clamped on the wire. Counts per mm and the sign convention are the existing `AIM_STEPS_PER_MM` and wire-sign rules on the S3; the RP never sees millimeters.

## What the S3 deletes (C-9, same series)

`streamSamplerTask`, `g_slopmotion` and `g_interp_queue` in `main.cpp`; `MotionArbiter::submitStreamSample` and the stream speed feed; `MlinkServoDriver`'s segment path (`sendSegment*`, the chain, re-anchors, sweep governor, re-seed, `mchunk` census, runway gate); `kOpSegment`/`kOpSegment2`/`kRunwayTargetMs`/`kSegmentDepth` on both ends; `EngineConfigMap.h` becomes the tuning-to-tag census with its test kept. `PlanTrace` and the `plan` diag tag are rebuilt from `kEvtPlanAdopted`.

## Telemetry after the port

SlopSync motion channel: pos = rendered (tag PLANNED per the bead note on sd-4k1.4), the encoder-fed measured field = ACTUAL, raw = demand. Plan strip: start = status pos at the plan event's `t_us`, end = event target, duration = event detail, cur/vel = status, elapsed = now minus `t_us` through the clock filter. Anomalies: pulled events with kind below `kEvtLinkBase` map one for one onto the existing names.

## Verification owed before "live" (C-8)

T3 back-to-back sessions without a reboot; the jitter script (`tools/segtrace.py` census: settles only at stream end, no engine resets, no renderer overrun bursts); encoder deviation flat through a ceiling sweep; `test_rpmotion`, `test_motionlink`, `test_engine_config` green; every firmware env green.
