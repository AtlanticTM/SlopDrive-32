# SlopMotion waveform path -- referee chain review (2026-09-02)

Reviewer: opus subagent, read-only. Anchors are `hpp:<line>` in `lib/slopmotion/include/slopmotion/slopmotion.hpp` as of commit a872583. Part of the six-area review indexed in `README.md`.

## 1. The decision chain, in order

| # | Referee | Measures | Can change | Reports |
|---|---|---|---|---|
| 0 | family adopt `hpp:1609` | `cmd.client_curve_family` -> sticky member | which builder every later step uses | -- |
| 1 | dwell rule `hpp:1629` | abs(target - prev **target**) < 2 % | zeroes `vf` | `HandoffBounded` (wrong kind, see 2f) |
| 2 | RFC-008 handoff `hpp:1674` | abs(vf) vs `k*min(chord_in, chord_out)` | `vf` magnitude only, sign kept | `HandoffBounded` |
| 3 | wall/vmax guard `hpp:1698` | vf^2 <= amax*dist-to-wall | `vf` | `EndVelClamped` |
| 4 | af estimator `hpp:1704` | backward diff of *accepted* handoffs | `af` | -- |
| 5 | bad-move bridge `hpp:1727` | T vs `bridge_ratio * t_opt` | **duration** (arrive early, hold) | `DeadlineStretched` |
| 6 | arm overshoot `hpp:1758` | 1 Ruckig solve -> physical band excess + chord slack | nothing; sets `_oshoot_allow` | -- |
| 7 | legality scan `hpp:2869` | 65-pt grid: v/a/j ceilings, window, overshoot band | nothing; returns `worst` | -- |
| 8a | centering, feasible `hpp:1790` | outstanding `_wave_owed` at a reversal | **target** (shortens a *legal* stroke) | `WaveformCentered` |
| 8b | Blend search `hpp:2148` | smallest ray step `s` that is legal | **target and end handle** together | `WaveformSmoothed` + `WaveformScaled`/`Centered` |
| 8c | centering, post-search `hpp:2217` | pull vs search endpoint | target again | folded into 8b's one event |
| 9 | Ruckig guard `hpp:1866` | `min_duration = T` | **shape**, and duration if it must | `WaveformFallback` (+ `DeadlineStretched`) |

Then `adoptQuintic` `hpp:1946` labels the plan Cubic/Quintic and `noteWaveformExtreme` `hpp:2693` updates the cross-command debt.

## 2. Incoherences

**(a) The Blend search proves feasibility at the one point on its ray that is hardest -- this is the `waveform_fallback` at ratio 1.001.** `at(1.0)` `hpp:2162` with blend 0.5 gives `k = 2`, so `alpha = 1` and `f = -1`: endpoint = **p**, travel zero, vf zero. For a rail slam entering at v ~ +8 norm/s over T = 56 ms that curve must brake, back up and return; its peak accel is `4*v/T ~ 571` against amax 400 -> ratio 1.43. The probe fails, `found` stays false, and a segment that was **1 % over** falls straight to the flat Ruckig guard with the entire search unused. Feasibility is *not* monotone in `f` -- the file admits it three times ("a shortened shape can break a ceiling the longer one did not", `hpp:1810`, `hpp:2231`, `hpp:2373`) and then builds a bisection whose invariant assumes it is. This is the single highest-value defect in the area, and it exactly matches the field signature.

**(b) Blend ignores both budgets the operator set.** `budget` `hpp:2048` is computed then used only in the sequential branches. `infeasible_amplitude_budget = 0.5` documents "may shrink to the midpoint; a stroke shortened past its own midpoint has stopped being the motion the script described" -- Blend's `at()` reaches `f = -1`, i.e. *no motion at all*. A documented safety statement is unenforced for the shipped policy.

**(c) The smoothness axis silently undoes RFC-008.** `blendEndTowardChord` `hpp:1995` lerps `vf` toward *this span's* chord with no re-check against the `k*min(chord_in, chord_out)` bound that referee 2 just imposed. A span with a large own-chord and a small successor chord gets its bounded handoff pushed back up, which is the precise failure RFC-008 exists for. Related: `_prev_vf` stores `vf_handoff` `hpp:1695` -- the *pre-blend* value -- so the next segment's `af` is derived from a velocity this plan never ends at, off by exactly the alpha spent.

**(d) The overshoot allowance is computed for a stroke the candidate does not draw.** `armOvershootAllow` `hpp:2998` adds `overshoot_chord_slack * abs(target - p)` from the **full commanded** chord, once, and every shortened trial is judged with it. The band `lo/hi` is per-trial `hpp:2884`, so the referee correctly narrows the box while keeping the wide stroke's slack: the guard is *weakest* on the shortest candidates, which are the ones that bulge. The physical-floor term is rightly hoisted (it needs a Ruckig solve); the slack term is `fabs(ep - p)` and free.

**(e) The anomaly lies about which axis paid, in the shipped policy.** `hpp:2260`: `noteWaveformExtreme(..., center_bound ? 0.0 : abs(target - ep), ...)`. When centering binds *after* the search already spent amplitude, the machine-shortfall term is set to **zero** -- the debt controller is told the machine could have made the full stroke when the search proved it could not. Reshape gets this right (`mach` at `hpp:2554` is measured against the commanded target, independent of `pull`); Blend conflates the two. And only one `WaveformScaled`/`Centered` fires for both causes, with `detail = abs(ep-p)/adist` mixing them.

**(f) Wrong anomaly kind on the dwell path.** `hpp:1638` reports `HandoffBounded`, documented at `hpp:897` as "RFC-008: cut to the Fritsch-Carlson knot bound of the FOLLOWING segment". A dwell zeroing is a different referee. Cheap fix, but it makes the census unreadable.

**(g) Slack measured off the wrong curve.** `hpp:1801`: `slack = 1 - worst` where `worst` is the ratio of the curve that was *rejected* in favor of `gc`; `gc`'s own ratio is computed at `hpp:1795` and discarded.

**(h) Dead in the field config.** `bridge_ratio` defaults 0 (`hpp:735`) -- the losing half of a shoot-out, still costing a `timeOptimalDuration` solve when armed. `commitWaveformScaled` (~110 lines), `commitWaveformReshaped` (~150), `softestFeasibleJerk` + three `infeasible_soften*` knobs, and `findAlpha`/`findF` are all unreachable under Blend. With `wave_centering = false` (as in `liveTuning()`, test_main.cpp:2520), `wavePull` `hpp:2666` returns 0 for every non-Reshape policy, yet `noteWaveformExtreme` `hpp:2699` keeps maintaining `_wave_owed` that nothing reads.

**(i) Cross-policy knob coupling.** Blend's step count is `max(blend_steps, reshape_steps)` `hpp:2151` -- a Reshape knob silently sets Blend's plan-time cost.

**(j) The declared cubic is replaced in three places.** `_client_curve_family` is set only in `commitWaveform` `hpp:1609` and never cleared, so synthesis spans (`commitSynthSpan` -> `buildWaveformCurve`) and everything after inherit the last real segment's declaration. Second, `adoptQuintic` reads `waveformIsCubic()` at *adopt* time, but the pending slot is built earlier and promoted later (`hpp:1353`, `hpp:3333`) -- a family change between build and promotion mislabels `PlanKind`, the exact ground-truth defect the enum comment warns about. Third, every fallthrough to Ruckig replaces the cubic with a bang-bang profile; that one is honestly labeled `PlanKind::Ruckig`, so it is fine.

**Fine, moving on:** `boundHandoffVelocity` itself is clean, pure, bit-exact-on-in-bounds, and well tested (four property suites). `pointWorst` as the single shared legality predicate across both planners is right. Planning from `sampleRaw(t0,...)` and refusing to move the *start* handle is correct doctrine.

**Test gap:** the suite never sets `InfeasiblePolicy::Blend`, never sets `ForceC1`, and never asserts anything about the ray. The only Blend coverage is incidental via `liveTuning()`'s defaults in the field replays.

## 3. Where the 17 ms goes

Per Blend commit, when the search *succeeds*:

| step | scans (65 pts each) | Ruckig |
|---|---|---|
| `armOvershootAllow` | -- (65 `at_time`) | 1 solve |
| full-fidelity trial | 1 | |
| `at(1.0)` probe | 1 | |
| bisection, `steps = 6` | 6 | |
| re-trial at `hi` (duplicate) | 1 | |
| centering trial | 1 | |
| **total** | **10 -> 650 grid points** | **1** |

Each grid point does 4 polynomial evaluations (~28 mul/add) **plus five divisions** -- `vv/T`, `aa/T^2`, `jj/T^3`, `abs(vv)/vc`, `abs(aa)/ac` -- all in `double`. The S3's LX7 FPU is single-precision only, so all of it is soft-float: ~60 cycles/op, ~280/divide. 650 x (28*60 + 5*280) ~ **2.0 M cycles ~ 8.3 ms**, plus a double-heavy Ruckig solve. That reconciles with the field: typical 3-5 ms = 1 solve + 2 scans => ~1.4 ms/scan; 17.6 ms = 1 solve + 10 scans. Arithmetic and measurement agree, so the cost is the *referee*, not the policy count.

Bounded-cost design, ranked by return per line changed:

1. **Hoist reciprocals** in `quinticWorstRatio` `hpp:2869` -- 325 soft-float divides per scan become 6. ~40 % off every scan, zero behavior change. Est. 17.6 -> ~11 ms.
2. **Early exit.** Every bisection probe only needs `ratio <= 1.0`; only the adopted trial needs the number. Return on first `worst > 1.0`. Illegal candidates break in the first quarter of the grid. Est. -> ~7 ms.
3. **Scan in `float`.** Keep `buildQuintic` in double, convert the six coefficients once, scan on hardware FPU. A threshold test that already carries `kRuckigLegalEps = 0.05` does not need 15 digits. Est. -> ~2 ms. Doctrine already asks for this (`cpp-style.md`: float over double, double only at plan-time *events*).
4. **Closed-form peaks instead of a 64-point grid.** abs(j) already has one (`quinticPeakJerk` `hpp:1962`); abs(a) peaks at the roots of a quadratic, abs(v) at a cubic, position at the roots of `v`. ~10 evaluations instead of 65, and *more* correct -- a fixed grid can step over a narrow peak, which is a silent safety hole today.
5. **Drop the duplicate re-trial** at `hpp:2172`/`2085`/`2110`: cache the last legal trial's `(c[], ep, worst)` inside the bisection. Free, 1 of 10 scans.
6. Memoizing across commits is not worth it -- plans are rebuilt from live state, so keys rarely repeat exactly. Memoize *within* the commit instead (5).

## 4. Doctrine

**Ceilings are clamps, never targets -- no.** "Shrink the stroke to hold the deadline" turns the ceiling into a *shape generator*. The doctrine sentence enumerates what is derived-and-clamped: "speed and accel are DERIVED from the intent and CLAMPED at ceilings" (architecture.md section 2). Position is not on that list, and Scale/Reshape/Blend all move it. Only `Stretch` is clamp-only. Worse, the centering branch at `hpp:1790` shortens a stroke that passed every ceiling, using a ceiling *another command* hit -- a ceiling reaching sideways into a plan it never constrained.

**CANON FLAG -- doc/code contradiction (C-1, C-5)**
Source A: `.claude/rules/motion-control.md:26` "WAVEFORM ... C2 quintic Hermite over exactly the commanded duration, ceiling and window scanned, **illegal shapes falling through to the Ruckig guard**."
Source B: `hpp:1831-1846` -- five policies stand between the failed scan and that guard, and the shipped one (`Blend`) changes the *endpoint*.
My read: B is what runs; the rules file describes the pre-0.3 engine, and the amplitude-shrinking policies are an unrecorded amendment to "ceilings are clamps".
Your call: ratify amplitude-shrinking as doctrine (and amend both files), or make Stretch the shipped policy.

**One command, one plan -- partly.** `_wave_owed` `hpp:3473` makes this plan a function of the last N strokes; that is a controller whose plant is the command history, which is the clocked-loop disease in slow motion. `_prev_wave_tgt` (dwell) and `_prev_vf` (af) are defensible -- they reconstruct sender intent from a lossy wire.

**Plan from actual state -- yes.** `sampleRaw(t0, ...)` at `hpp:1113`, and the refusal to move the start handle `hpp:1985`. Correct and explicitly reasoned.

## 5. Verdict per mechanism

| Mechanism | Verdict | Sketch |
|---|---|---|
| Legality scan (`pointWorst`/`quinticWorstRatio`/`ruckigWorstRatio`) | **keep, rewrite** | float + closed-form peaks + early exit. This is the safety core and it is also the whole cost. |
| `boundHandoffVelocity` / RFC-008 | **keep** | Pure, cheap, well tested. Re-apply it *after* `blendEndTowardChord`. |
| `applyEndVelGuard` / `endVelBound` | **keep** | Window safety, unchanged. |
| Overshoot guard + `physicalBandExcess` | **keep, simplify** | Move the chord-slack term into the per-trial band; keep the measured physical floor. |
| `InfeasiblePolicy` x 6 | **delete 4, keep 2** | Blend at 0.5 *is* PrioritizeSmooth (0) and PrioritizeAmplitude (1) -- those two are unreachable expressions of one ray. Scale is measured worst. Reshape's flat top is the artifact Blend was built to remove. Keep Blend + Stretch, and make Stretch a bool (`shorten_when_infeasible`) rather than a sixth enum value, because it is the only one with a distinct *contract*. Deletes ~370 lines, 6 knobs, 4 Ruckig call sites. |
| Blend's ray | **fix, then keep** | Floor `f` at the amplitude budget (which it currently ignores), probe the ray *at that floor* rather than at `f = -1`, and document that `f` is not monotone in feasibility -- the bisection returns "smallest legal grid point", not "the boundary". |
| `softestFeasibleJerk` / soften knobs | **delete** | Reshape-only, and Reshape goes. |
| `bridge_ratio` | **delete** | Default 0, lost its shoot-out, costs a solve when armed. |
| Centering (`wavePull`/`noteWaveformExtreme`/`_wave_*`) | **delete from the engine** | Off in the live config, cross-command state in a one-command-one-plan engine, and it shortens legal strokes. If the band really walks, trim the *intent* on the S3 before it crosses the link. ~120 lines and one control law gone. |
| Anomaly kinds | **keep, fix attribution** | Fix (e) and (f); one event per axis actually spent. |
| `af` backward difference | **keep** | Store the *adopted* end velocity in `_prev_vf`, not the pre-blend one. |
| Ruckig guard | **keep** | The honest terminal answer, and chase/settle need Ruckig anyway. |
| `CurvePolicy` / `resolveCubic` | **keep, fix** | Clear `_client_curve_family` on non-waveform commits; capture the family into the pending slot at *build* time, not at promote. |

## 6. Port to the RP2350

**Must survive (this is the engine the doctrine puts on the RP):** `buildQuintic`/`buildCubic`/`quinticAt`/`evalCurve` -- six coefficients plus T is precisely the "intent with an anchor time" the link vocabulary wants; the whole legality scan, because section 2 says "ceilings are therefore measured and enforced in the engine"; `boundHandoffVelocity`; `applyEndVelGuard`; the overshoot guard; the Ruckig guard; **one** infeasible policy. **Warning for the port:** the Cortex-M33's FPU is also single-precision -- the soft-float double cost travels with this code. Do the float conversion of the referee *before* the port, not after, or the 17.6 ms becomes an RP problem.

**S3-host workarounds that should not cross the link:** the whole `commitSampleSynth` / `_syn_*` / `_pend_*` jitter-buffer chain (`kSynthJitterUs`, the one-deep pending slot) is a transport-jitter absorber for the S3's arrival timeline; under "commands are sent the moment they arrive" the holdback is an S3-side policy, not engine machinery. `Command::anchor_us` + `kAnchorMaxLateUs` + the coast-past-expiry in `sampleRaw` `hpp:1156` exist because the S3 renders against a jittery drain -- the RP's own 20 kHz tick makes the anchor native and the coast cap a fix for a deleted problem. `recovery_vmax` is a policy gate (section 2: gates are S3 policy pushed as config). And the 10 ms render runway that 17.6 ms starved does not exist after the port -- which is the reason to fix the cost now for correctness' sake, not to keep tuning `blend_steps`.
