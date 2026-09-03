# SlopMotion engine to S3 host -- contract review (2026-09-02)

Reviewer: opus subagent, read-only. Sources: `lib/slopmotion/include/slopmotion/slopmotion.hpp`, `src/main.cpp:536-880`, `src/motion/MotionArbiter.cpp:107-250`, `src/motion/MlinkServoDriver.cpp`, `src/comms/SlopSyncHubService.cpp:1036-1130,1697-1810`, `include/comms/MotionLinkProtocol.h`, as of commit a872583. Board: sd-wve, sd-yun, sd-jff, sd-4k1. Part of the six-area review indexed in `README.md`.

## 1. The contract as actually implemented

**Position truth: three claimants, no arbiter.**
- The engine believes it owns it. `resetAt`/`commit` sample from `_hold_pos` or the in-flight polynomial (`slopmotion.hpp:1028`, `:1090`), never from a sensor. Its frame is normalized 0..1 over the stroke window.
- The RP owns it in fact (`architecture.md` section 2). `_pos_counts` arrives at 100 Hz and `getPosition()` extrapolates it (`MlinkServoDriver.cpp:790-802`).
- The driver's `_chain_*` is a fourth, private copy -- the tail of the last chunk shipped (`MlinkServoDriver.cpp:263-266`), which is the *future* the RP has not rendered yet.

Nobody reconciles these. The engine is never told the machine's real position except through the two `resetAt` calls, which are destructive.

**Time: one clock, three restatements.** Everything is `esp_timer_get_time()`. The hub carries it as u64 `due_us` (`SlopSyncHubService.cpp:1070`), the engine as u64 (`slopmotion.hpp:146-148`), the driver as **u32 `micros()`** (`MlinkServoDriver.cpp:729`, `_chain_us`/`_hold_us`/`_samp_us`). Same domain, different widths -- the u32 side already produced the "71-minute wedge segment" the comment at `:450` guards against. Fine today; a latent class.

**Commit cadence.** The engine assumes commits are events, not ticks, and it means it -- `commit()` is O(ms) (up to 17.6 ms budgeted Blend, sd-wve). The host obeys: `while (xQueueReceive(...))` drains 0-1 entries per 1 ms tick (`main.cpp:708`), released by the pacing ring only when due (`SlopSyncHubService.cpp:1701`).

**Anchor lateness.** The engine accepts a past anchor and back-samples its own state to it, clamping lateness at `kAnchorMaxLateUs = 50 ms` (`slopmotion.hpp:1085-1087`, `:1515`), sized under `kCoastCapS = 60 ms` so the coast state stays uncapped. The host's 1-5 ms is comfortably inside. **This part of the contract is sound and is the one seam that is working.**

**`resetAt` means two different things.** To the engine: a hard reset -- kills the in-flight plan, drops the synthesis chain, the cadence estimator, `_est_had_cadence`, `_prev_vf_ok`, and zeroes `_last_commit_us` so the next commit is a **cold start** clamped to `recovery_vmax` (`:1028-1052` + `:1112-1122`). To the host it reads as "seed the engine where the shaft is" (`main.cpp:583`) -- a positioning act. The host has no idea it is also erasing the stream's cadence, which is what makes `settleGraceS()` return 0 (`:3277`) and brake at the next plan end. That is sd-wve's chain, and it is a *contract* mismatch, not a bug in either side.

**`isBusy` means two different things.** To the engine: "does the plan still have motion left", including a scheduled successor (`:1391`). To the host it is a **motor-ownership latch** -- one of three terms in `streamActive` (`main.cpp:580`), alongside packet recency and a 500 ms post-move hold. So an engine-internal state (`_pend_ok`) decides who owns the machine.

**Config push.** `setConfig(c)` is a wholesale struct copy (`:1064`), and the host rebuilds a **default-constructed `Config`** every 1 ms tick and overwrites it (`main.cpp:623-697`). The host's comment is right that whatever it writes *is* the policy. The engine assumes ceilings change only between plans (`setLimits` note, `:1055`) and that is honored -- an in-flight polynomial is immutable. So the push is wasteful, not wrong: ~40 scalar copies at 1 kHz that could be one copy on `cfg_gen` change. One line: **fine, just noisy.** (The test-suite review adds the sharper consequence: any `Config` field the host does not push is reset to the engine default every tick.)

## 2. Where the two sides disagree

**(a) The engine's frame is the window; the machine is not always in it.** Both seed sites clamp to `[0,1]` (`main.cpp:589`, `:610`). After homing the carriage is at 0 mm with a window of 87-187: `norm` is -0.87, clamped to 0.0. The engine now believes it is at the window floor while the carriage is 87 mm away. Every plan it makes starts from a lie, and the driver's chain gap is guaranteed > `kReseedGapMm` (8 mm), so the re-seed can never converge -- the code knows this (`MlinkServoDriver.cpp:157-159`) and falls through to a sweep, but the engine is left holding a false position for the whole entry.

**(b) Chunk time vs sample time.** `nowUs` is captured once at the top of the tick (`main.cpp:541`) and used for both `commit()` (`:710`) and `positionAt/velocityAt` (`:780-781`). After a 17 ms commit the sampled point is the curve at **t-17 ms**, and the driver then stamps it with a fresh `micros()` (`MlinkServoDriver.cpp:729`). The chunk `[chain -> samp]` therefore claims to cover 17 ms of wall time with content that only advanced 1 ms. That is pace corruption injected by the host's own clock reuse, and it is the same family as sd-2fb/sd-beq/sd-2vp.

**(c) Three re-seed/reset paths, and two of them fire for one event.**

| # | Trigger | Site | Effect |
|---|---|---|---|
| 1 | `streamActive` rising edge | `main.cpp:585` | engine `resetAt` |
| 2 | driver request | `main.cpp:604` <- `MlinkServoDriver.cpp:169` | engine `resetAt` |
| 3 | driver chain re-anchor (entry `:748`, settled-underrun `:450`, 100 ms silence `:462`) | driver only | **engine never told** |

Path 3 is the structural one: the driver silently moves its own position truth to `liveCounts()` and zeroes velocity, and the engine keeps planning from a state that no longer joins to the chain.

Paths 1 and 2 **do** double-fire on stream entry. Tick *t*: rising edge resets the engine at a clamped norm; the same tick's sample reaches `streamSample`, which sees `!_seg_mode`, re-anchors and sets `_reseed_armed = true` (`:748-756`). Within 10 ms `sendSegmentTo` measures the gap, requests a re-seed, sets `_reseed_req` **and re-arms both `_sweep_pending` and `_reseed_armed`** (`:169-171`). The sampler resets the engine a second time (`:610`), ~10 ms after the first, from the same stale `actual_position_mm`. Two cold starts for one connect. `_reseed_tried` bounds it to one *retry*, not one *reset*.

Also: `consumeReseedRequest()` is outside the `streamActive` guard (`main.cpp:604`), so a request left pending when a stream drops resets an idle engine.

**(d) `_reseed_armed` is host-invisible state deciding host behavior.** The driver decides whether a gap is "entry" or "mid-stream" from a private bool; the host obeys unconditionally. The host cannot audit or veto it. sd-wve's fix lives entirely inside that bool, which is why the surviving run-07 reset was not provable from the archive. (2.4.129 added log lines at both reset sites for exactly this reason.)

**(e) The sweep governor is the arbiter's last dispatched speed.** `_samp_vcap = speed_steps_s` (`:760`), which in the default `SPEED_CEILING_PEGGED` mode is the input ceiling -- fine. In `SPEED_VELOCITY_MATCHED` it is *the curve's instantaneous speed at the last sample*, floored at `SAFE_APPROACH_SPEED_MM_S` (`MotionArbiter.cpp:178-190`). A recovery sweep then glides at whatever speed the content happened to have. Latent, mode-gated.

**(f) The 10 ms driver tick is a clock in a command path.** `update()` early-returns until `kTickMs` elapses (`MlinkServoDriver.cpp:335-337`), then the runway gate decides whether to ship (`:475-477`). A chunk detected at *t* can ship at *t+0.8 s* -- sd-yun exactly. The ship-time re-anchor (`:139-146`) patches the symptom; the clocked gate is the cause.

## 3. Re-derived quantities the engine already knew exactly

| Quantity | Engine has | Re-derived at | Cost |
|---|---|---|---|
| **acceleration** | `accelerationAt()` (`:1385`), `Snapshot.acc` -- analytic | `holdKnotAccel()`: centered difference of two sampled velocities (`MlinkServoDriver.cpp:116-122`) | The chunk's `a1` is a finite difference across a 1 ms window of a curve whose second derivative is closed-form. Never plumbed. Quantization noise enters the Hermite's `A1` term directly. |
| **velocity** | `velocityAt()` | round-tripped: norm/s -> mm/s -> counts/s (`MotionArbiter.cpp:236`), separately clamped as a *speed* feed (`:178-201`) that the mlink path ignores | two float scalings and a sign flip per sample; the speed feed is dead weight on this driver except as `_samp_vcap` |
| **duration** | `planDuration()`, and the wire's own `duration_us` | `dur_us = t1_us - _chain_us` from two `micros()` stamps (`MlinkServoDriver.cpp:132`) | chunk boundaries are sampler-tick artifacts, not plan knots. A knot lands mid-chunk and is smoothed away. |
| **the curve itself** | one quintic, coefficients in hand | re-fit as Hermite from 4 sampled boundary values (`:246-262`), then **split again** by evaluating at u=0.5 (`sendSegmentSplit`, `:279-306`) | a fit-of-a-sample-of-a-fit; the interior-velocity scan at `:214-244` exists solely to detect what this loses |
| **peak velocity** | scanned at plan time by the legality scan | scanned again, 9 points per chunk (`:225-236`) | duplicate work; the second scan reports, never corrects |
| **the plan is infeasible** | anomaly ring, at plan time | inferred downstream from `_rc_ov`/`residue_max` (`:400-412`) and `lag_mm > 8` (`MotionArbiter.cpp:219`) | the diagnosis arrives after the motion |

Net: the engine emits an analytic C2 curve; the host discards the derivatives, samples it at 1 kHz, and reconstructs a C2 curve from the samples. Every step is lossy and every loss got its own guard.

## 4. Doctrine check -- the S3-side re-render seam

`architecture.md` section 2 condemns "rendered chunks" and "a loop computing positions on a clock". These host mechanisms are that seam:

1. **`streamSamplerTask`'s 1 kHz `vTaskDelayUntil`** (`main.cpp:876`) evaluating `positionAt` -- a clocked position loop, verbatim the thing the ruling names.
2. **`MlinkServoDriver`'s 10 ms tick + runway gate** (`:335`, `:475`) -- commands shipped on a schedule, not on arrival.
3. **The `_chain_*` / `_hold_*` re-fit and split** (`:128-306`) -- the S3 rendering chunks the RP then re-renders.
4. **The holdback** (`:481-483`) -- one full driver tick of manufactured latency.
5. **Both engine `resetAt` paths** -- an S3-side attempt to re-sync two position truths that only exist because the render is split across two boards.

**The RP host must not reproduce:** any sample-then-refit stage; any clocked evaluation loop; any second position variable that shadows the renderer's counter; any "re-seed the planner because the chain drifted" path -- with one planner on one board there is no chain to drift. Ship the *plan*, evaluate it once, at the tick.

## 5. Minimal engine-host contract for the port

**Commands in.** One `Command`, unchanged, submitted the instant it arrives -- no pacing ring on the motion board. `anchor_us` in the **renderer's clock**, which is now the engine's clock and the position truth's clock: one clock, u64, no restatements. Keep `kAnchorMaxLateUs`. The S3 sends intents with anchors in hub time; the RP link layer translates once, at ingress, and nowhere else.

**Position/velocity out.** `positionAt`/`velocityAt`/`accelerationAt` at the 20 kHz tick, feeding the emitter directly. No intermediate chunk, no refit, no `_chain_*`. The engine's `_hold_pos`/plan **is** the machine's position truth; the encoder audits it (section 2), it does not seed it.

**Reset.** Exactly one caller and one meaning: `resetAt` is a **stream-entry / e-stop / post-home** act, never a mid-stream correction. Split the current overload -- the cold-start clamp and the cadence drop are separate concerns and only entry wants both. Nothing downstream may request a reset; a downstream component that thinks it needs one is reporting a bug.

**Config.** Push on change, not per tick -- a generation counter, compared cheaply. Semantics unchanged: ceilings bind at the next plan.

**Positions outside the window.** The clamp must stop being silent. Either the engine takes an unclamped seed and plans a window-entry move itself, or the host is required to complete entry before the first commit. Today's `constrain(norm, 0, 1)` is a lie the engine cannot detect.

**The host may never:** sample the engine and reconstruct a curve from the samples; hold a second position variable; re-derive velocity, acceleration or duration the engine can be asked for; reset the engine to fix a downstream disagreement; evaluate on a clock other than the render tick; or reuse one timestamp across a commit and a sample (seam (b) above -- cheap to get wrong, cheap to forbid).
