# SlopMotion lifecycle and timers -- coherence review (2026-09-02)

Reviewer: opus subagent, read-only. Anchors are `slopmotion.hpp:<line>` as of commit a872583 unless stated. Part of the six-area review indexed in `README.md`.

## 1. The map

| Mechanism | Keys on | Set by | Cleared / reset by | Notes |
|---|---|---|---|---|
| `_last_commit_us` :3481 | **arrival** `now_us` | every `commit()` past the isfinite guard :1121 | `resetAt` -> 0 :1050 | 0 = "never", also = "force cold" |
| `_last_plan_end_us` :3482 | `_plan_start + planDuration()` (**plan clock**) | hold-collapse :3353, `settleToIdle` :3417 | `resetAt` -> 0 :1051 | only plans that *ended at rest*; never written on supersede or settle-engage |
| cold-start governor :1113-1121 | `now_us - max(_last_commit_us, _last_plan_end_us) > kColdStartGapUs` (2 s :1537) **and** mode in {Idle,Settle} **and** abs(v) < 1e-3 **and** `0 < recovery_vmax < vmax` | `commit()` | transient: mutates `_cfg.limits.vmax` and restores :1136 | |
| `_est_valid` :3458 | any commit | `updateEstimator` :3254 | `resetAt` :1040 | >= 1 commit |
| `_est_ema_ok` :3459 | 2 commits within `chase_stale_us` (400 ms :463) at **anchored** time | :3245 | gap > stale :3249; `resetAt` | |
| `_est_had_cadence` :3460 | sticky "an EMA once existed" | :3246 | **only** `resetAt` :1042 | |
| `settleGraceS` :3269 | `ref = max(_est_last_us, plan_end)`; `now-ref > stale` -> 0; else `min(1.5*dt_ema, settle_grace_us)`, floor 90 ms if `_syn_chain_ok` :3300 | read per sample | -- | |
| grace window :3359 | `elapsedS - dur < grace` (**plan expiry**) | `maybeSettle` | never re-arms within a plan | |
| coast :1584-1591 | `over = min(now-plan_end, kCoastCapS=60 ms)` | `sampleRaw` | -- | `a` zeroed, **`v` is not** |
| `_pend_ok` :3505 | a synth span committed ahead of its chain anchor :1348 | `commitSynthSpan` | `adoptQuintic` :1947, `planRuckig` :3138, `maybeSettle` promote :3332, `resetAt` :1033 | pins `isBusy` :1392 and short-circuits all of `maybeSettle` |
| `_syn_chain_us/_ok` :3512 | seeded `now + 40 ms`, then tiles by span `T` :1233,1240 | `commitSampleSynth` | slip/hot-entry re-prime, `resetAt` | leads real time |
| `_mode`/`_kind` | plan adoption | `adoptQuintic` :1950, `planRuckig` :3140 (kind only), callers set mode :1735/1866/2628/3080 | `maybeSettle`/`settleToIdle` -> Idle/None | |
| `isBusy` :1391 | `_pend_ok` or elapsed<dur or plan-end abs(v)>1e-4 | -- | **const, never settles** | host gate, evaluated *before* the tick's `maybeSettle` |
| host re-seed | `motor.consumeReseedRequest()` `src/main.cpp:604`; stream rising edge `:583` | `resetAt` | | |

## 2. Contradictions

**2.1 Four disagreeing definitions of "alive", on three different clocks.** The cold governor keys on arrival time + rest-ending plan ends (2 s); `settleGraceS` keys on anchored commit time + the *in-flight* plan end (400 ms); `commitSampleSynth` keys on anchor stamps (400 ms); the debt keys on `_wave_last_us` (1 s). Same question, four references, four constants. Every bug in the brief is one of these; the shape guarantees more.

**2.2 `resetAt` declares a live stream dead by two mechanisms at once.** The driver re-seed (`src/main.cpp:604`, sd-wve) fires *because* the stream is alive and outran the renderer. `resetAt` then zeroes `_est_valid/_est_ema_ok/_est_had_cadence` (:1040-1042) **and** `_last_commit_us` (:1050). Consequence: the first post-reseed segment plans at `recovery_vmax` (intended, tested :2622) **and** gets `settleGraceS == 0` for two more commits -- so a 60 ms segment ending at 0.8 norm/s brakes at expiry, exactly the "forgotten cadence returns zero grace" bug re-entering through the reset door. The cadence estimate is a property of the *stream*, not of the plan; a re-seed replaces the plan.

**2.3 `_prev_wave_tgt_ok` survives `resetAt`.** :1629-1632 is never cleared by `resetAt`. Scenario: chain slips at target 0.62, re-seed, sender's next segment targets 0.63 (delta < `kDwellSpanNorm`) -> classed a dwell, `end_vel` forced to 0, `HandoffBounded` logged. The first segment after every re-seed can silently lose its handoff velocity, which is exactly the state a re-seed most needs to re-acquire.

**2.4 The coast cap no longer outlives the grace window.** :1512 asserts `kCoastCapS` = 2x the grace cap. `settleGraceS` :3300 floors grace at 90 ms whenever `_syn_chain_ok`, ignoring `settle_grace_us` as a cap. Scenario: 60 ms synth span ends at v = 1.0 norm/s, stream stops. Position advances 60 ms of coast, then **freezes for 30 ms while `velocityAt` keeps reporting 1.0** (:1591 zeroes `a`, not `v`) -- the arbiter is fed a moving velocity with a static position (`src/main.cpp:781`). Then the brake at :3364 is planned from `v = 1.0`, which the machine has not had for 30 ms, so the settle lurches forward. Also: `settle_grace_us = 10000` does not produce a 10 ms grace.

**2.5 An unreachable branch, from an unsigned underflow.** :1147 computes `stamp - _syn_us` on `uint64_t`. A regressive anchor stamp wraps to ~2^64, always exceeds `chase_stale_us`, and tears down the holdback -- so the branch written to handle exactly that case, :1174 `if (stamp <= _syn_us)`, is reachable **only for `stamp == _syn_us`**. Contrast :3287, where the same comparison *is* guarded (`now_us > ref &&`). One out-of-order segment costs a chain re-prime plus a chase fallback instead of being dropped.

**2.6 The pend slot performs the teleport it exists to prevent.** :1349 states that adopting a future-anchored span now "would clamp to the span start and teleport"; :1353 then does exactly that when a successor beats the pending's anchor -- `adoptQuintic(_pend_c,_pend_T,_pend_start)` with `_pend_start > now_us`, so `elapsedS` :1544 returns 0 and the plan renders from tau=0. Justified as the lesser jump, but it is the same mechanism, and it is the *only* consumer of the second slot's absence.

**2.7 `_pend_ok` defeats every other timer, with no bound on how long.** :3328 returns before expiry, grace, settle and hold-collapse. `_syn_chain_us` is clamped against *lateness* (:1237) but never against *lead*: a catch-up burst (arrivals faster than content time) advances the chain past `now` by T per emission indefinitely. With `_pend_start` far future, `isBusy` is pinned true (:1392), the host keeps the motor claimed, the active plan expires, and the sampler serves a coast-capped frozen position with non-zero velocity -- until the burst ends.

**2.8 Two "cold" tests that cannot both be right.** `kColdStartGapUs` = 2 s is doing all the work: a stream resuming 1.5 s after a hold plans the park-to-content traverse at full `input` vmax -- the script-start dart the governor exists to stop. The governor's real subject is *a positioning move* (at rest, far from the first target), not *silence*. Also `cold` requires `recovery_vmax < limits.vmax` (:1116); with `user_max_speed >= input_max_speed` the whole governor silently disappears, no anomaly.

**2.9 Minor / fine.** `settleToIdle` is called on the `_kind == None` path (:3337) where it immediately returns -- dead call, fold it. `_mode` is not written by `planRuckig`; every caller does -- fine today, one new caller from a silent mode/kind mismatch. `_plan_start` is written by `resetAt` while `_kind == None` -- harmless, but `lastPlanUs()` reports a plan that does not exist. No unsigned underflow in the cold test: `_last_plan_end_us` is only ever written from a past instant.

## 3. Doctrine (architecture.md section 2)

- **Event-driven, never clocked** -- mostly held: transitions are computed from `now`, not accumulated. But `maybeSettle` is a *poll* run three times per host tick (`positionAt`/`velocityAt`/`snapshot`), and it performs state mutation (pend promotion, hold collapse). Skipping ticks changes rendered geometry (pause/resume: the settle brake is planned from a velocity 400 ms stale). A plan should be evaluable at an arbitrary `t` without side effects.
- **One command, one plan** -- broken by construction in the synthesis path: one commit can promote the previous span *and* pend its own, i.e. two plan transitions. Deliberate, but the doctrine line no longer describes the engine.
- **Plan from actual state** -- `commitSynthSpan` plans from `evalCurve(_pend_c,...,1.0)` (:1243), the *pending* span's end, not the machine. Correct for a pipeline, but the chain is open-loop for one knot and the assumed entry state never occurs if the pend is preempted.
- **Ceilings are clamps, never targets** -- the cold governor mutates `_cfg.limits.vmax` and restores it (:1134-1136). Correct today only because the host pushes `setConfig` before the drain in the same tick (`src/main.cpp:697` then `:704`); any second writer, or one added `return` between, leaves the engine at the recovery ceiling. A ceiling override belongs in the plan call, not in the config object.

## 4. Verdict

| Mechanism | Verdict | Sketch |
|---|---|---|
| `_last_commit_us` / `_last_plan_end_us` / `_est_last_us` | **Merge into one `_last_activity_us`** | stamped in the engine's own clock by every commit *and* every plan end (rest or not). All four staleness tests then key on one reference; three of the four field bugs become unrepresentable. |
| cold-start governor | **Simplify** | drop the gap test; fire on `at rest && abs(target - p) > park threshold`, and pass the ceiling as a `planRuckig` argument instead of mutating `_cfg`. |
| `settleGraceS` | **Simplify to one line** | `grace = min(1.5 x cadence, cap)` where cadence is the *production* cadence (knot pitch when the chain is locked, `dt_ema` otherwise), 0 only when `now - _last_activity_us > stale`. Delete `_est_had_cadence`. Make the cap real, and assert `cap <= kCoastCapS` so the coast/brake continuity claim at :1512 is true again. |
| `_est_had_cadence` | **Delete** | subsumed above. |
| coast :1591 | **Fix** | zero `v` when `over == kCoastCapS`, or refuse to let grace exceed the cap. Reporting a velocity the position does not have is a ground-truth defect. |
| `_pend_*` | **Keep, re-express** | make the plan a two-piece piecewise polynomial that `sampleRaw` selects by time. Then `isBusy`, `maybeSettle` and the promotion branch all fall out of evaluation, :1353's teleport disappears, and a second slot is free. Bound `_syn_chain_us` lead symmetrically with :1237. |
| `maybeSettle` / `settleToIdle` / hold collapse | **Keep the settle, split the poll** | `sample(t)` pure; one `advance(t)` the host calls once per tick. Fold `settleToIdle` in. |
| `_prev_wave_tgt_ok`, `_syn_*`, `_wave_*` | **Fix `resetAt`** | reset is "the plan and the pipeline are void", not "the stream never existed". Clear `_prev_wave_tgt_ok`; keep the cadence estimate. |
| :1147 underflow | **Fix** | `stamp > _syn_us && stamp - _syn_us > stale`. |

## 5. Port vs. host scaffolding

**True engine behavior -- must survive the RP port:** the settle boundary event and its grace window; the cadence estimator as far as it sizes that grace; the rest/hold collapse; the pend slot *as a piecewise plan* (the RP evaluating an anchored successor at its own tick is the doctrine's own description of intents); `resetAt` as e-stop/home/seed; the cold-start ceiling for a positioning move.

**S3 scaffolding only:** `isBusy` (a claim gate for a shared motor the RP will own alone); the coast-past-expiry hack at :1584 (it exists because a 5 ms pacing drain lands successors after expiry -- at a 20 kHz free-running tick the successor is either anchored or it is not); `kAnchorMaxLateUs` and its coupling to `kCoastCapS`; the 40 ms `kSynthJitterUs` lead and the whole chain-time tiling (a jitter buffer for *this* link); the re-seed path (`consumeReseedRequest`) and everything `resetAt` does to accommodate it; the three-`maybeSettle`-calls-per-tick pattern.

**Blocks the port as written:** `commit()` :1084 accepts an anchor **only when it is in the past** -- a future anchor is silently demoted to plan-at-arrival. Architecture.md section 2 says intents cross the link *with anchor times* precisely so the RP evaluates them ahead of time. That one condition has to become a scheduled-plan slot before the port, and it is the same slot `_pend_*` already is.
