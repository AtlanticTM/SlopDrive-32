# SlopMotion coherence review -- 2026-09-02

Six read-only reviewers (opus subagents), one area each, same brief: map every mechanism in plain language, find the contradictions, check against `architecture.md` section 2, give keep / simplify / delete verdicts, and separate engine behavior that must survive the RP2350 port (sd-4k1) from S3-host scaffolding. Engine at commit a872583 (2.4.128). Field evidence: the 2026-09-02 traces under `artifacts/segtrace-jitter-0{3,5,6,7}` and `artifacts/scope/`.

| # | Area | File |
|---|---|---|
| 1 | Lifecycle, timers, reset, settle grace | `01-lifecycle-and-timers.md` |
| 2 | Waveform referee chain, infeasibility policies, plan cost | `02-waveform-referee-chain.md` |
| 3 | Chase, sample synthesis, estimator | `03-chase-and-synthesis.md` |
| 4 | Window, safety defenses, numerics | `04-window-safety-numerics.md` |
| 5 | Engine to S3-host contract, the re-render seam | `05-engine-host-contract.md` |
| 6 | Test suite coverage and fixture drift | `06-test-suite.md` |

Board: epic sd-6b2 (children carry the findings; rulings are labeled `pending-ruling`).

## Verdict in one paragraph

The core is sound and worth porting: the quintic and cubic builders, the shared legality predicate, the handoff bound, the end-velocity wall bound, the overshoot guard, the Ruckig fallback, the settle. Around that core, roughly a third of the 3,500-line header is either scaffolding for the S3 host (a jitter buffer, a coast past expiry, a re-seed path, a claim gate) or policy variants that the machine never runs (four of six infeasibility policies, centering, a bridge ratio, three softening knobs). The disease is not any one mechanism; it is that the engine answers "is the stream alive" four ways on three clocks with four constants, and every defect this week was two of those disagreeing. Six reviewers independently converged on the same shape: one activity clock, one infeasibility policy that works, one reset with one meaning, and a referee that is both cheaper and more correct than the grid it uses today.

## Findings that are bugs today, not taste

1. **The Blend search probes the wrong point.** It tests legality at the far end of its ray, a zero-travel curve that must brake, reverse and return; on a rail slam that probe fails even when the real segment is 1% over, so the search is discarded and the segment drops to the flat Ruckig fallback. This is the `waveform_fallback det=1.00x` on every rail-end in the field. It also ignores both budgets the operator set. (02 section 2a, 2b)
2. **Ruckig plans are adopted unrefereed on five of six paths**, including the exact fallback taken when a shaped plan was judged illegal; the engine's own table shows such a profile leaving the window. The settle brake has no window either (~13 mm past the rail at field limits), and the coast extrapolates up to 60 mm outside the window and seeds the next plan. The only remaining defense is the output clamp, which pins position while still reporting velocity into the wall. (04 F1 to F4)
3. **A zero jerk limit silently disables the quintic referee** (`0/0` is NaN, `fmax` drops it, the scan says legal). The two neighboring limits are guarded; this one is not. (04 F5)
4. **The host wipes every engine setting it does not push, every millisecond.** `setConfig` receives a default-constructed `Config` each tick, so `infeasible_blend` (the one slider of the shipped policy), `chase_stale_us`, `overshoot_guard`, `sample_synthesis` and others sit at engine defaults forever. No test covers the push. (06 section 1)
5. **`commit()` only honors an anchor that is already in the past**; a future anchor is demoted to plan-at-arrival. Anchored intents crossing the link ahead of time is the whole point of the port. (01 section 5)
6. **Every 10 ms scripted knot goes to the sample-synthesis path**, built for a 333 Hz bare-point client that no longer exists, and that path chases a target from the previous stroke because its ring is never cleared by a real segment. The same 1 ms gap poisons the cadence estimator once per stroke, pinning the chase jerk scale at the ceiling. (03 section 2a to 2c)
7. **A stream entry costs two cold starts**: the host's rising-edge reset and the driver's re-seed both fire for one connect. The sampler also reuses one timestamp across a 17 ms commit and the sample after it, stamping 17 ms of wall time onto 1 ms of curve. (05 section 2b, 2c)
8. **`resetAt` erases the stream's cadence**, so the first plan after any re-seed settles with zero grace; and `_prev_wave_tgt_ok` survives the reset, so the first segment after a re-seed can lose its handoff velocity to the dwell rule. (01 section 2.2, 2.3)

## Recommended order of work

**Engine, before the port (moves with it):**
1. One activity clock stamped by every commit and every plan end; all staleness tests key on it. `resetAt` voids the plan and pipeline, keeps the cadence, clears the dwell memory. Delete `_est_had_cadence`.
2. Fix the Blend ray: floor at the amplitude budget, probe there, re-apply the handoff bound after smoothing. Delete Scale, Reshape, PrioritizeAmplitude, PrioritizeSmooth, centering, `bridge_ratio`, the soften knobs. Keep Blend plus Stretch as a boolean. About 500 lines.
3. Referee: hoist reciprocals, early exit, scan in float, closed-form peaks. 17.6 ms to about 2 ms, and no grid that can step over a spike. Apply `ruckigWorstRatio` to every adopted trajectory; window the settle brake; guard `jmax > 0`; floor the `af` gap at `T`; make `velocityAt` consistent with the clamped position.
4. Delete sample synthesis and its ring and pending promotion; route on `has_duration` alone (ruling). Feed the estimator with chord rate for segments.
5. Honor future anchors as a scheduled slot. Accept an out-of-window seed and judge plans against `[min(0,p0), max(1,p0)]`.
6. Tests: the machine's tuning as the fixture; a scenario harness with default invariants; assert the flip count; fix the tautological window assertions; a config round-trip test.

**S3 host, until the port:** push config on change with a generation counter; one reset owner; never reuse a timestamp across a commit and a sample; keep the reset-cause logging added in 2.4.129.

## Rulings needed from the operator

- **Amplitude shrinking versus "ceilings are clamps".** `motion-control.md` says illegal shapes fall to the Ruckig guard; the shipped Blend policy shortens the stroke instead. Recommendation: keep Blend (fixed) plus Stretch and amend the rule, because a stroke that holds its deadline at 84% amplitude is the better feel on this machine than one that arrives late. (02 canon flag)
- **The duration floor.** Route every duration-carrying segment to the waveform planner, so a 10 ms knot is a 10 ms span with its authored tangent. Recommendation: yes. (03 canon flag)
- **Runway band and RP dead-stop on underrun.** Already sd-jff.
