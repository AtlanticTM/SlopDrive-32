# SlopMotion non-waveform paths -- chase and synthesis review (2026-09-02)

Reviewer: opus subagent, read-only. Anchors are `slopmotion.hpp:<line>` as of commit a872583. Part of the six-area review indexed in `README.md`.

## 1. The map

**Routing** -- `slopmotion.hpp:1097` is the only fork: `waveform = cmd.has_duration && cmd.duration_us >= kShortMoveUs` (20 ms, `:1455`). Everything else goes to `commitSampleSynth` (`:1144`, since `sample_synthesis` defaults true at `:411` and **nothing in the S3 firmware ever sets it** -- `main.cpp:639-665` pushes 9 chase knobs and not this one; only `src/rp2350_bench/main.cpp:256` touches it). `commit()` samples live state at `t0` (`:1091`), feeds `updateEstimator(target, t0)` (`:1095`) for **both** paths, and clears `_syn_ok`/`_syn_prev_ok` on the waveform branch (`:1126`).

**CHASE** (`:3008-3081`). Reads `_est_v_ema`, `_est_a_ema`, `_est_dt_ema`, `_est_sp_pk`. Writes `_oshoot_allow = -1`, `_mode = Chase`, and (via `planRuckig:3138`) `_pend_ok = false`. Aim = `target + v_est*look + 0.5*a_est*look^2`, arrival `vf = v_est + a_est*look` through `applyEndVelGuard`, `af = a_est`, jerk scaled by peak-hold demand. Control returns to waveform on the next command with duration >= 20 ms -- no handshake, the Ruckig plan is simply superseded from its own sampled state.

**SYNTHESIS** (`:1144-1362`). A one-knot holdback that turns bare points into segments: coalesce to `kSynthSpanUs` = 60 ms (`:1525`), PCHIP tangent at the buffered knot (`:1206-1216`), then `commitSynthSpan` (`:1315`) which calls the *waveform* curve builder, legality scan, and `adoptQuintic` -- or parks the curve in a one-deep pending slot promoted by `maybeSettle:3328`. Its own private timeline: `_syn_chain_us` seeded at `now + 40 ms` (`:1233`) and tiled by `+= T`. Fallbacks (prime, slip, illegal span) all drop to `commitChase` aimed at `synthDelayedTarget` (`:1295`) -- a point ~120 ms *behind* the head, pulled from an 8-entry ring.

**Deadline-less point move**: there is no separate path. `has_duration=false` -> synthesis -> chase. Fine as a design; see below for what the ring does to it.

## 2. Incoherences

**(a) The delayed-target ring is never cleared by a waveform commit -- every short knot chases a position from a previous stroke.** `commit()` clears `_syn_ok`/`_syn_prev_ok` (`:1126`) but not `_synr_n`/`_synr_w`. The stale-gap reset that *would* clear it (`:1147`) is guarded by `if (_syn_ok && ...)`, which is now false. So on the field script's `1.000 dur=10 ms` knot: `!_syn_ok` -> prime -> `commitChase(..., tgt_d, ...)` where `tgt_d = synthDelayedTarget(stamp, target)` picks the newest ring entry <= `stamp - 120 ms`. In a mixed stream the ring only receives entries on the synthesis path, i.e. once per stroke, so that entry is **the previous stroke's knot**. The commanded 1.000 is discarded. Same defect makes an isolated manual point move after a script chase the script's old position. This is the one to fix first even if everything else stays.

**(b) `_syn_chain_ok` and `_syn_chain_us` also survive a waveform commit.** Consequences: `settleGraceS:3300` keeps floor-ing the grace at `1.5 x 60 ms = 90 ms` forever after any lock, three times the configured 30 ms cap (`:753`); and when synthesis next locks, `:1226` sees the chain "already established", skips the jitter-buffer seed, and `tw` clamps to `now - kAnchorMaxLateUs` (`:1237`, 50 ms) -- adopting a span 50 ms into its own profile. That is precisely the "late adoption enters mid-span off a linear coast -- a velocity kink" the jitter buffer at `:1228-1234` was written to prevent.

**(c) The estimator has two different meanings of "target" and one meaning of "dt", and neither is the content timeline.** `updateEstimator:3218` computes `raw = (target - last_target)/dt` with `dt` = *commit anchor spacing*. For chase points that is the content velocity. For a waveform segment, `target` is where the machine will be at `anchor + duration_us`, not at `anchor` -- the segment's real chord rate is `abs(target - p) / T`, which `commitWaveform:1670` already computes for the handoff guard and then throws away. In the field's mixed pair the two anchors are 1 ms apart, so `raw = (0.900 - 1.000)/0.001 = -100` window/s against a `vmax` of a few. Downstream, per stroke:

- `_est_sp_pk` latches ~100 and releases over 0.7 s (`kSpPeakReleaseS:1534`) -> `r` clamps to 1.0 at `:3070` -> **`chase_jerk_scale` is dead in this stream**, permanently at the mechanical ceiling.
- `_est_dt_ema` is dragged 30 % toward 1 ms each stroke -> `streamIsDense()` reads true when it should not, and `settleGraceS:3295` computes `1.5 x dt_ema` ~ single-digit ms (masked only by the stuck 90 ms floor from (b) -- two bugs canceling).
- `_est_a_ema` takes a second difference over 1 ms; it is acap-clamped at `:3031` before use, so it saturates rather than explodes, but it saturates *every stroke*.
- `commitWaveform:1617-1621` uses `_est_v_ema x chase_ff_gain` as the end-velocity fallback for any segment without a wire `end_vel` -- i.e. a poisoned number.

This is T18 (webui.md) in the motion engine: arrival/anchor spacing standing in for a source timeline.

**(d) Tunables that are dead or half-dead at the device's values.** `chase_lookahead = 0` => `look = 0` at `:3022`, so `aim = target` and `vf_req = v_est`: **`chase_aim_accel_extrap` is a total no-op** despite being on, and the two test cases that justify it (`test_main.cpp:1471`, `:1530`) exercise a configuration the machine does not run. What remains live is `vf = 0.1 x v_est` (gain 0.1) *together with* `af = a_est` at full strength (`:3055`). Asking Ruckig to arrive at 10 % of the stream's velocity but 100 % of its acceleration is not a damping choice, it is two incompatible requests; the acceleration will immediately undo the velocity damping. If gain 0.1 is the field answer, `chase_accel_ff` should be scaled by the same gain or turned off.

**(e) Synthesis duplicates the waveform path by construction, and plans from a predicted state.** `commitSynthSpan` is `commitWaveform`'s curve builder + legality scan + adopt, with a hand-rolled segmenter in front. `:1243`: `if (_pend_ok) evalCurve(_pend_c, _pend_T, 1.0, pw, vw, aw);` -- the entry state is the *pending curve's predicted end*, not sampled state. That is a client-side segmenter (compute duration, tangent, schedule) living inside the engine, on the wrong side of the wire.

**(f) The 10 ms knot itself.** It arrives on 0x0085 with a real `duration_us` and a real `end_vel`; the 20 ms floor routes it to synthesis, which reads neither -- `cmd.duration_us` is unused in `commitSampleSynth`, and `commitChase` ignores `cmd.end_vel` whenever the feedforward branch is taken (`:3017` vs `:3055`). An authored tangent is discarded, then the segment 1 ms later replans over a 1 ms-old Ruckig plan whose acceleration has already ramped `j*0.001`. Also:

**CANON FLAG -- code comment contradicts observed device behavior**
Source A: `src/comms/SlopSyncHubService.cpp:1788-1790` "a durationless 0x0084 chase point has no chord ... and a mixed stream cannot happen anyway (section 11.4 source ownership gives one client one motion source)."
Source B: field trace 2026-09-02 -- `1.000 dur=10 ms` followed 1 ms later by `0.900 dur=248 ms`, plan lines showing `ruckig/chase` between cubic segments.
My read: the comment is right about *channels* and wrong about *planners*. One client on 0x0085 still produces a mixed planner stream because `kShortMoveUs` splits one channel across two planners. The lookahead `has_next_chord` is also silently skipped for the short knot's successor pair.
Your call: is the fix to raise/remove the duration floor, or to keep the floor and document that 0x0085 is planner-mixed?

## 3. Doctrine check

- **Event-driven, never clocked** -- chase passes. Synthesis fails in spirit: `kSynthSpanUs` is a fixed 60 ms decimation tick and `_syn_chain_us += T` is a self-maintained schedule seeded at a fabricated `now + 40 ms`, not the command's anchor. `architecture.md` section 2: "a loop computing positions on a clock is rebuilding a disease this project already cured."
- **One command, one plan** -- chase passes. Synthesis fails: one command produces zero plans (buffered), or one plan for a *different* command's interval, or a pending curve promoted later by a sampler-side branch (`maybeSettle:3328`).
- **Plan from the machine's ACTUAL state** -- chase passes. Synthesis fails at `:1243` (plans from a predicted curve end). Note (a) also breaks it at the *target* end: the chase plans toward a stale target rather than the commanded one.
- Window clamping, non-finite rejection, `_oshoot_allow` disarm on bare points: all correct, no comment needed.

## 4. Verdict per mechanism

| Mechanism | Verdict |
|---|---|
| `commitChase` + Ruckig replan | **Keep.** It is the doctrine's named planner for bare points. |
| Predictive aim (`chase_lookahead`, `chase_aim_accel_extrap`) | **Delete or fix the tuning.** Dead at `look = 0`. If the field wants it dead, delete `:3022-3053` and the two crest tests; if not, the operator needs to know `aim_extrap=1` currently does nothing. |
| `chase_ff_gain` / `chase_accel_ff` pairing | **Simplify:** `af = a_est x chase_ff_gain` (one token), or gate `chase_accel_ff` off when gain < 0.5. |
| `chase_jerk_scale` peak-hold | **Keep the idea, fix the input.** It is currently pinned at ceiling by (c). |
| `updateEstimator` | **Keep, reform the feed.** Sketch: when `cmd.has_duration`, feed `raw = (target - p)/T` and `dt = T`; otherwise the current anchor difference. Two lines in `commit()`, and it removes (c) entirely. |
| `sample_synthesis` + `_syn_*` + `_synr_*` + pending slot | **Delete.** Nothing in-tree sends bare 0x0084 at 333 Hz: the ESP-NOW relay pair went today, MFP sends 0x0085 segments, and the S3 firmware never even exposes the flag. Deleting removes ~220 lines (`:1139-1362`), 14 state members (`:3483-3508`), the pending-promotion branch in the 1 kHz `maybeSettle` (`:3328-3335`), the synth grace floor (`:3300-3302`), `kSynthSpanUs`/`kSynthJitterUs`/`kSynRingN`, the `SLOPMOTION_SYNTH_DEBUG` block, three test cases (`test_main.cpp:538/603/652`), **and incoherences (a), (b), (e) outright.** Bare points then land on plain chase, which is what `motion-control.md` says they should do. |
| `kShortMoveUs` routing floor | **Simplify to `has_duration`.** The comment at `:1449-1454` already argues the legality referee is the right rejector; the floor is a second referee that switches planners. Removing it makes the field's 10 ms knot a 10 ms quintic span with its authored tangent -- one planner per stroke. |
| Settle grace, coast, `_last_plan_end_us` cold-start guard | Fine as written; only the synth floor at `:3300` needs to go with the rest. |

## 5. Port to RP2350 (sd-4k1)

**Must survive:** the Ruckig chase planner and its jerk-scale, the estimator (reformed), `applyEndVelGuard`, window clamping, `maybeSettle` + brake-to-rest, anchored commits, `resetAt`.

**S3-host workarounds -- leave them behind:** the entire synthesis holdback (it is a re-render seam, the exact family the three-board split exists to delete); `kSynthJitterUs` (a jitter buffer against the S3's 5 ms pacing drain -- the RP evaluates the plan at its own tick, so there is no arrival jitter to buffer); `kAnchorMaxLateUs`/`kCoastCapS` late-anchor clamping (sized for queue-crossing lag that the link's anchored intents remove); the `settleGraceS` transport-jitter term (`:3295`) -- on the RP, starvation is a link fact, not a network guess; the pending slot's promotion inside the 1 kHz sampler.

**Ambiguous:** `chase_dense_us` = 60 ms is a *client cadence* property, not a host property -- it ports, but re-measure it once arrival jitter is gone.
