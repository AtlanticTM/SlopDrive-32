# SlopMotion window, safety and numerics -- boundary review (2026-09-02)

Reviewer: opus subagent, read-only. Anchors are `slopmotion.hpp:<line>` as of commit a872583. Part of the six-area review indexed in `README.md`.

## 1. The defense chain

| # | Defense | `file:line` | Protects | Position in chain | Bypassable by |
|---|---|---|---|---|---|
| D1 | non-finite `target`/`end_vel` reject | `:1072` | NaN into the planner | commit entry | Nothing checks `cmd.duration_us`/`anchor_us` (u32/u64, cannot be NaN -- fine). Synthesis re-enters `commitWaveform` at `:1265` below this guard, but its inputs are engine-derived. OK. |
| D2 | `target = clamp01(cmd.target)` | `:1092` | commanded endpoint | commit entry | Not bypassable -- Scale/Reshape/Budgeted all re-derive from this `target`, and each re-clamps its own `ep` (`:2021`, `:2345`, `:2564`). Clean. |
| D3 | anchor lateness clamp `kAnchorMaxLateUs` | `:1082` | u64 underflow + unbounded back-dating | commit entry | Guarded by `anchor_us < now_us` first. No wrap. Fine. |
| D4 | `boundHandoffVelocity` (Fritsch-Carlson 1.5x) | `:854` | sender's `vf` vs next chord | waveform, pre-guard | Only arms on `has_end_vel && has_next_chord` (`:1675`). Chase/settle never see it. By design. |
| D5 | `endVelBound` wall bound `vf^2 <= amax*dist` | `:3201` | end velocity pointing into a wall | every planner that sets `vf` | Reachable but sound. Applied in waveform (`:1694`), chase (`:3057`,`:3059`), Reshape probes (`:2554`), budgeted (`:2027` via scaled `vf`). Settle sets `vf=0` structurally. |
| D6 | `quinticWorstRatio` v/a/j + window scan (64 pts) | `:2869` | quintic bulge | waveform adopt gate | **Bypassed when `jmax == 0`** -- see F5. |
| D7 | `pointWorst` window term (no grace) | `:2848` | plan leaving 0..1 | shared referee | Hard-codes the window as `[0,1]` with **no reference to the plan's start** -- see section 3. |
| D8 | `_oshoot_allow` / `physicalBandExcess` | `:2969`, `:2998` | polynomial-invented overshoot past target | armed per commit | Disarmed on chase (`:3013`) and on the bridge path. Correct: a bare point declares no band. |
| D9 | `ruckigWorstRatio` | `:2929` | Ruckig sailing through vmax and off the rail | **only one caller** | **Effectively bypassed on every terminal adoption** -- see F1. This is the headline. |
| D10 | `jerkCeil` softer-only clamp | `:3179` | a search handing Ruckig more jerk than the machine | all Ruckig calls | Solid, single choke point. Keep. |
| D11 | `Result < 0` check | `:3105`, `:2818`, `:3387` | Ruckig refusal | every `calculate()` | Correct and complete -- every error in `ruckig/result.hpp` is negative. `throw_error=false` (`ruckig.hpp:23`), every `throw` is under `if constexpr` (`calculator_target.hpp:312`), so no exception is instantiated. Doctrine satisfied. |
| D12 | `clamp01` on sampled output | `:1371` (pos), `:1409` (snapshot) | the hard backstop | sample path | **Position only. `velocityAt` (`:1374`) and `Snapshot::vel` (`:1411`) are raw** -- see F4. |
| D13 | `resetAt` seed clamp | `:1036` + host `main.cpp:589,610` | -- | seed | **This is the 87 mm bug, not a defense.** Section 3. |

## 2. Failure scenarios

**F1 -- a Ruckig plan leaves the window, unrefereed. Reachable, and it is the normal path.**
`planRuckig` only runs `ruckigWorstRatio` under `if (j_ovr > 0.0 && jc < jmax)` (`:3130`). Every *terminal* adoption misses it:

- chase at full authority -- `chase_jerk_scale` gives `r = 1.0` whenever demand >= `0.5*vmax` (`:3068`), so `jc == jmax`, gate false (`:3075`);
- the chase hard fallback `planRuckig(..., j_ovr=0)` (`:3079`) -- the retry taken *precisely when* the softened plan was judged illegal;
- the waveform Ruckig guard (`:1864`, `j_ovr` defaulted 0);
- the bad-move bridge (`:1734`);
- Reshape's adopted plan when `j_eff == jmax` (`:2614`) and its belt-and-braces retry (`:2626`).

The header's own measured table at `:2915-2925` proves the failure mode: at jerk 50 the profile spans `[-0.605 .. 0.853]`. The engine documents that Ruckig is not a legality oracle and then adopts it unrefereed on five of six paths. The only surviving defense is D12, the output clamp -- which is exactly the thing that produces F4.

**F2 -- settle overshoots the window. Reachable, ~13 mm on the field machine.**
`maybeSettle` (`:3369`) plans a `ControlInterface::Velocity` brake with **no position target and no window scan**. Comment at `:3312` says so ("lands wherever braking lands, clamped by the sampler"). Field limits normalize to vmax 10 u/s, amax 400 u/s^2: `v^2/2a = 0.125 u = 12.5 mm` past the rail, plus the jerk tail. `_hold_pos = clamp01(p)` at `:3414` then quietly erases the discrepancy, so the engine's post-settle belief and the machine's position differ by the overshoot.

**F3 -- the coast extrapolates 60 mm outside the window, and it seeds the next plan.**
`sampleRaw` coasts `p += v*over` with `over <= kCoastCapS = 0.06 s` (`:1591`, `:1512`), linear, unclamped, by contract ("planning continuity must see the true polynomial state"). At field vmax that is **0.6 normalized units = 60 mm**, on a 100 mm window. `commit()` seeds from `sampleRaw(t0, ...)` (`:1090`), so a late command plans from a position the machine was never at and can never have been at. "Committed ~3 ms after its anchor" is the benign case; a 40 ms transport hiccup is not.

**F4 -- `positionAt` and `velocityAt` disagree at the wall. Reachable whenever F1/F2/F3 fire.**
`main.cpp:780-782` reads both and calls `arbiter.submitStreamSample(pos, vel)`. When the clamp is active, `pos` is pinned at 1.0 while `vel` still reports +10 u/s driving outward. Anything downstream that uses velocity as feedforward is being told to keep going into a rail whose position channel says "stopped". This is the webui Ground Truth Doctrine violation in the motion domain, and it is the mechanism by which D12 converts a planning defect into a mechanical one.

**F5 -- `jmax == 0` silently disables the quintic referee. Reachable via a config push.**
`quinticWorstRatio` reads `jc = _cfg.limits.jmax` (`:2871`) and divides by it at `:2895` with **no `jc > 0` guard**, while `pointWorst` explicitly guards `vc > 0.0` and `ac > 0.0` (`:2851-2853`). With `jmax = 0` and a curve whose `c[3..5]` are zero, `jj/jc = 0.0/0.0 = NaN`, and `std::fmax(worst, NaN)` returns `worst` -- so the scan reports **legal**. Host path: `main.cpp:637`, `input_max_jerk_mm_s3 / span`, and a zero in NVS gives zero. Asymmetric with the two limits right next to it; that asymmetry is the bug.

**F6 -- a 1 ms due-time gap inflates the `af` estimate by 1000x. Reachable (the field's own case).**
`af = (vf_handoff - _prev_vf) / gap` (`:1706`), guarded only by `now_us > _prev_vf_us` and `gap < 3*T`. With `gap = 1 ms` and `T >= 20 ms`, both pass; `af` scales as `1/gap`. The quintic scan rejects the result, so it is *contained* -- but the containment is "send it to the guard", and the guard is F1. `gap` needs a floor (a sane one is the segment's own `T`), not just a ceiling.

**F7 -- negative/zero durations.** Not reachable. `T` comes from `uint32_t duration_us >= kShortMoveUs` (`:1074`, `:1455`) or from monotone u64 stamp differences (`:1202`, guarded at `:1174`/`:1181`), and `commitSynthSpan` re-guards `!(T > 0.0)` (`:1316`). Every `elapsedS` (`:1543`) and every stamp subtraction tests ordering first, so **there is no u32 or u64 wrap exposure anywhere in the engine** -- all time state is u64 (`:3439` ff.). One residual: `adoptQuintic` (`:1946`) does *not* guard `T > 0` although `quinticPeakJerk` right below it does (`:1963`); `quinticAt` would divide by `_q_T = 0` (`:1598`). Unreachable today, one line to make it structural.

**F8 -- `planRuckig` adopts without checking `isfinite(get_duration())`**, while `probeRuckigDuration` does (`:2826`). Ruckig returns `ErrorTrajectoryDuration = -101` for this, so low reachability. Add the check for symmetry; not urgent.

**F9 -- the field's 10 ms segments never reach the waveform path.** `kShortMoveUs = 20000` (`:1455`) routes them to chase/synthesis, discarding the authored duration and handoff. Not a numeric fault, but it means the field's hot path is the one with the weakest referee (F1).

**F10 -- the `clamp01(_syn_p)` calls at `:1246`/`:1251`/`:1265` are redundant.** `_syn_p` is assigned from the already-clamped `target` (`:1092` -> `:1177`). Harmless; delete on next touch.

## 3. The out-of-window carriage

**Today's contract is broken, and it is a frame lie, not a clamp.** Host: `norm = (0 - 87)/100 = -0.87`, `constrain(norm, 0, 1) -> 0.0` (`main.cpp:589`), `resetAt` clamps again (`:1036`). The engine now believes the carriage is at 87 mm when it is at 0 mm. The first sample it emits is 87 mm -- an 87 mm step command with no plan behind it. Clamping the seed does not make the state safe; it makes it **false**, and the doctrine's "plan from the machine's ACTUAL state" is violated at the one moment it matters most.

**Recommendation: the engine accepts `p < 0` and `p > 1` as real state and plans the entry itself.** It is already 90 % built -- the whole plan-time machinery is double, unclamped, and `sampleRaw` deliberately reports the true polynomial state.

Three changes, no new abstractions:

1. **Drop both host clamps** (`main.cpp:589`, `:610`) and the clamp in `resetAt` (`:1036`). `_hold_pos` becomes the honest seed.
2. **Make the window term in `pointWorst` relative to the plan's entry**, one line: score against `[min(0.0, p0), max(1.0, p0)]` instead of `[0,1]`, with `p0` passed in the same way `lo`/`hi` already are (`:2848`). Semantics: *a plan may never be further outside the window than it started.* An in-window plan is judged exactly as today, so no existing behavior moves; an out-of-window plan is required to be monotone inward. That single predicate change makes the entry move plannable, and it also makes F2 (settle overshoot) and F3 (coast) *judgeable* rather than invisible.
3. **Track the same `p0` on the sample clamp** (`:1371`, `:1409`) so the output is clamped to `[min(0,p0), max(1,p0)]` rather than `[0,1]`. The entry ramp renders truthfully; once inside, the clamp is byte-identical to today's.

`endVelBound` needs nothing: `dist = vf > 0 ? (1-target) : target` (`:3207`) with `fmax(dist, 0.0)` already forces `vf = 0` for an out-of-window target, and targets stay clamped.

The alternative -- keep clamping and have the *host* command the entry move before enabling the stream -- is defensible and smaller, but it puts a motion plan in glue, which `architecture.md` section 1 calls the smell to hunt, and it leaves F2/F3/F4 untouched.

## 4. Verdict per defense

| Defense | Verdict | Sketch |
|---|---|---|
| D1 non-finite reject | **Keep** | Cheap, deterministic, doctrine-named. |
| D2 target clamp | **Keep** | Correct, and every re-solve re-clamps. |
| D3 anchor clamp | **Keep** | Sound; the `kAnchorMaxLateUs < kCoastCapS` coupling is documented (`:1515`) and holds. |
| D4 handoff bound | **Keep** | Well-tested (5 property suites), inert without lookahead. |
| D5 end-vel wall bound | **Keep** | The trapezoid bound is conservative against the jerk tail at sane ratios -- but say so *with a number*: at field limits the jerk tail adds ~8 ms of ramp, and the bound ignores it. It is conservative because it ignores the vmax cruise, not because it models the tail. Comment at `:3196` overstates. |
| D6/D7 quintic scan + `pointWorst` | **Keep, fix two lines** | Add `jc > 0.0` (F5). Add the `p0`-relative window (section 3). |
| D8 overshoot guard | **Keep** | Measured, self-correcting, disarms honestly on no answer. Well-argued in `:2940-2967`. |
| D9 `ruckigWorstRatio` | **Keep and actually USE it** | Move the check out of the `j_ovr` gate: score *every* trajectory before `_traj = traj` (`:3140`). On failure, refuse and let the caller fall back -- chase's mechanical retry already exists, waveform's guard is the terminal path so it should adopt-with-anomaly rather than refuse. Cost: one 65-point `at_time` sweep per adopted plan, plan-time only, same order as the quintic scan already paid on the path that *does not* need it. |
| D10 `jerkCeil` | **Keep** | Textbook single choke point. |
| D11 `Result` / `throw_error` | **Keep** | Correct and complete. No action. |
| D12 output clamp | **Keep, make consistent** | Either clamp `velocityAt`/`Snapshot::vel` to zero-crossing-at-the-wall, or (better) have `positionAt` and `velocityAt` come from one `sampleClamped()` that zeroes `v` and `a` once the position term saturates. Two callers, one helper. F4 is the only defense-induced defect in the file. |
| D13 seed clamp | **Delete** | Section 3. |
| Settle | **Fix** | Give the brake a window: after `calculate()`, run `ruckigWorstRatio(traj, -1.0)`; if it leaves the window, re-plan as a *position* move to the rail with `vf = 0` instead of a velocity brake. Same Ruckig call count in the common case. |
| Coast | **Simplify** | `kCoastCapS = 0.06` was sized as 2x the grace cap, not against travel. Cap the coast by *distance* as well as time -- `over = min(over, kCoastCapS, kCoastMaxNorm / max(abs(v), eps))` -- so it is bounded in the units that matter. One line. |
| `af` backward difference | **Fix** | Floor `gap` at `T` (F6): `if (gap < 3.0*T && gap >= T)`, or divide by `fmax(gap, T)`. |
| `adoptQuintic` | **Simplify** | Add `if (!(T > 0.0)) return;` -- matches its own neighbor at `:1963`. |

**Test note:** `test_main.cpp:1181` "All three policies keep the sampled window invariant [0,1]" asserts `p >= -1e-9 && p <= 1.0+1e-9` on the output of `positionAt`, which **clamps**. That assertion is tautological and can never fail. The real content of that case is the `abs(v) <= vmax*1.001` line. Re-point it at a raw-state accessor (or at `snapshot().target`, which is also clamped -- so a new one is needed) or the window regressions are unguarded. Same applies to the `sweep()` checks in the `sd-tki.13` case at `:3444`.

## 5. Port to the RP2350

`architecture.md` section 2 makes the RP the position truth and the S3 the policy owner; gates are pushed as config and **enforced where the curve is evaluated**. So the split is: anything that decides *what the polynomial does* goes with the engine; anything that re-checks *what the host already knows* is duplication.

**Must survive the port (they live where the curve is evaluated):**
- D6/D7 quintic legality scan and `pointWorst` -- the RP is the only place the quintic exists.
- D9 `ruckigWorstRatio` -- same, and it is the one that is currently not doing its job.
- D8 overshoot guard -- it needs the entry state at plan time, which is the RP's.
- D5 `endVelBound` and D10 `jerkCeil` -- plan-time, no host equivalent.
- D1 non-finite reject -- a link frame is a *less* trusted input than a queue POD, not more. Keep it on the RP and add one on the S3 side of the link too; a NaN that crosses a 24-byte SPI frame is a wire-integrity question the engine cannot answer.
- Settle, coast, and the F2/F3 fixes -- the RP evaluates at its own tick, so the coast and the brake are entirely its business.

**Duplicates the host, and should be stated as such rather than removed twice:**
- D2 `clamp01(target)` -- the arbiter already window-clamps, and the S3 owns the mm to normalized frame. Keep it in the engine anyway: it is one comparison and it is what makes the engine liftable and native-testable per `architecture.md` section 1. The *host's* clamp is the one to question -- and section 3 says delete the seed half of it.
- D12 the output clamp -- genuinely duplicated with the arbiter's 0.5 mm wall, and after the port the RP's own step limits are a third copy. That is the right number for a safety backstop; do not consolidate. But fix the pos/vel asymmetry before it ships, because on the RP the velocity channel stops being telemetry and starts being step rate.
- D13 the seed clamp -- dies with the port either way. Under the three-board split the RP holds position truth, so the seed becomes "the RP tells the S3 where it is", and clamping it at the boundary would re-create the same 87 mm lie one layer down. Fix the contract now, before the frame moves.

**One thing the port must not inherit:** the `_cfg` mutation in `commit()` for the cold-start governor (`:1120-1124` -- write `_cfg.limits.vmax`, plan, restore). It is safe today only because the engine is single-task and `setConfig` is same-core. On the RP the config arrives over the link, and a config push landing between those two lines makes the restore at `:1136` write a stale ceiling back. Make it a local `Limits` passed down, or take the restore out of the exception-free path's way. That is a mechanism worth naming: it is not a race today, it is a race the moment the config stops being same-core.
