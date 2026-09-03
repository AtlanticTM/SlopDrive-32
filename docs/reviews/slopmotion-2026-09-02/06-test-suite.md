# SlopMotion test suite review (2026-09-02)

Reviewer: opus subagent, read-only. Suite: `test/native/test_slopmotion/test_main.cpp` (71 cases, ~2M assertions, doctest) as of commit a872583. Anchors are `test_main.cpp:<line>` unless prefixed. Part of the six-area review indexed in `README.md`.

## 1. Coverage map

| Mechanism (engine anchor) | Pinned by | What is actually asserted |
|---|---|---|
| Mode state machine (`slopmotion.hpp:865`, gate `:1098`, `kShortMoveUs` `:1455`) | `:343`, `:351`, `:777`, `:2794` | `mode()`/`planKind()` observed at four points. **No test at the waveform/chase threshold at all** -- nothing pins 20 ms, and nothing would have failed when it was 50 ms. The 41 ms knot appears once, at `:2540`, asserting nothing about mode. |
| Cold start (`:1100-1123`, gap `:1537`) | `:474`, `:515`, `:2622` | Peak abs(v) vs `recovery_vmax`. Never crosses `kColdStartGapUs` (2 s) except from a fresh engine; the largest in-stream gap tested is 500 ms (`:530`). |
| Settle / grace (`:3269`, `:3327`) | `:2387`, `:2645`, `:2693`, `:2598` | Coast equals `p_end + v_end*grace` exactly, `SettleEngaged` counts, no-jump at the brake anchor. Good. `:2645` and `:2598` are today's additions and are the only long-segment coverage. |
| Estimator (`:3218`) | none directly | Only observable through grace and chase aim. `_est_had_cadence`, the peak-hold release `kSpPeakReleaseS`, and the `chase_stale_us` forget path have **no test that names them**. |
| Anchored commits (`:1082`) | `:2480` | One segment, 4 ms late, identical to on-time reference. `kAnchorMaxLateUs` saturation is **untested** -- no test releases a command later than the cap. |
| Handoff bound (`:852`) | `:2901`, `:2950`, `:2984`, `:3019`, `:3036`, `:3084` | The 135 k-point grid. This is where essentially all ~2 M assertions live: a 12-line pure function is ~99 % of the suite's assertion count. Genuinely well covered. |
| Stretch (`:3088` guard) | `:437`, `:748`, `:1021`, `:1874` | Duration >= distance/vmax, anomaly kinds, ceilings sampled. Fine. |
| Scale (`:2311`) | `:983`, `:1221`, `:1280`, `:1354`, `:1399`, `:1426` | Deadline held, achieved fraction, sizing near the legal max. Fine. |
| Reshape (`:2516`) + softening (`:2757`) | `:1630`, `:1682`, `:1953`, `:1990`-`:2362` | Monotonicity, jerk ceiling, `soften=false` bit-identity. Strong. |
| **PrioritizeAmplitude / PrioritizeSmooth (`:2041`)** | **NONE** | ~225 lines of engine, zero direct tests. |
| **Blend (`:307`) -- the policy the machine runs** | only indirectly via `fieldreplay` (`:2514`) | No test names it. `:1181` is titled "All three policies keep the sampled window invariant" and iterates three of six. `infeasible_blend`, its single knob, is never varied. |
| Centering (`:2653`) | `:1736`, `:1778`, `:1835`, `:1874`, `:1910`, `:2198` | Band center/spread over 30 cycles, orbit detection. Strong -- for a feature the machine has **off** (`:2522`). |
| Chase aim (`:3008`) | `:716`, `:806`, `:1471`, `:1530`, `:1572` | Tracking error, crest overshoot, arrival velocity. Fine. |
| Synthesis (`:1144`, `:1295`, `:1315`) | `:538`, `:603`, `:652` | No teleport, pace <= 1.35x source tempo, anomaly census, jittered 60 Hz stamps. Strong. |
| Window/legality scan (`:2869`, `:2929`) | `:3364`, `:3417` | Both referees agree over ~300+ comparisons; the rail-graze case pinned absolutely. |
| Output clamp (`:1367`) | `:3444` (4 subcases) + every `sweep()` | `[0,1]` on all four command kinds. Fine (but see the window review: it asserts on a clamped accessor). |
| `resetAt` (`:1028`) | `:2794`, `:2622` | `:2794` is a **"does not crash"-grade** test: reset, `CHECK_FALSE(isBusy)`, done. `:2622` (today) is the first that commits after a reset. |
| **Config push (`setConfig` `:1064`, `setLimits` `:1056`, `setChaseFeedforward` `:1058`)** | **NONE** | Zero references in the suite. `src/main.cpp:697` calls `setConfig` **every tick with a freshly default-constructed Config**, so any field main.cpp forgets is silently reset to the engine default forever. Currently unpushed: `sample_synthesis`, `chase_jerk_scale/_floor`, `chase_stale_us`, `overshoot_guard`, `overshoot_chord_slack`, `infeasible_soften*`, `bridge_ratio`, and **`infeasible_blend`** -- the one slider of the policy the machine runs. |

## 2. Tests that encode a workaround, drift from the machine, or pass for the wrong reason

**Config drift is the headline.** Every case outside `fieldreplay` runs a fixture that differs from the machine on five axes at once (`:36` `testConfig`, `:70` `operatorConfig` vs `:2513` `liveTuning`): limits 2/20/300 or 5/250/10000 against the machine's **10/400/50000**; policy Reshape/Scale/Stretch against **Blend**; `overshoot_guard` pinned to 0 (`:51`, `:77`, `:181`) against the machine's default **1.0**; quintic reconstruction against the machine's **c1_cubic** (`client_curve_family=1`, only ever set at `:2557`); `wave_centering` on in the whole `runChain` family against the machine's **off**; `settle_grace_us` 30 ms against **200 ms**. The pinning comments at `:41-50` are right that a fixture must not float on a default -- but pinning to a config nothing ships is how ~40 cases became a study of a machine that does not exist.

**Measured but not asserted.** `:2693` computes `flips` at `:2748`, prints it at `:2765/2768/2770`, and never `CHECK`s it. The mixed-planner flip-flop the operator could only see on the machine is *already being counted by this test* and thrown away.

**Assert-on-message.** `:538`, `:603`, `:652`, `:2762`, `:3401`, `:3520` route the interesting number through `MESSAGE`. `:592` is explicit about it ("measured 16 ... `CHECK(an[5] <= 20)`") -- a tripwire four counts wide around whatever the code did that day, i.e. a workaround pinned as intent. Same class: `:846` `worst_err < 0.10` and `:601` `err < 0.20`, both self-described as tripwires.

**Compares against the code under test.** `:3364` asserts `quinticWorstRatio == ruckigWorstRatio`. That is agreement, not correctness -- both wrong the same way passes. Mitigated by `:3417`, which pins one absolute verdict; keep that one and treat the sweep as a consistency check, not a legality test.

**Field replays assert only counters.** `:2570`, `:2598`, `:2622` check `settles == 0`, `endvel == 0`, `vpk` thresholds and nothing about rendered geometry. A regression that produces garbage motion without settling passes all three. They also leave `printf` scaffolding on (`print=true`, `:2586`, `:2617`, `:2641`) -- hundreds of lines of noise per run.

Fine as-is, one line each: `:951` determinism, `:929` non-finite rejection, `:3444` clamp sweep, `:397` C2 joins, `:2901` handoff grid.

## 3. Why the four defects were invisible

- **Settle grace vs long segments** -- no case had a segment longer than `chase_stale_us`. Longest streamed segment before today was 167 ms (`:2713`); the 1200 ms case at `:3450` is a single command from a fresh engine, so grace is 0 by the isolated-point rule and there is nothing to starve.
- **Cold governor after a 6.9 s hold** -- no case ever crosses `kColdStartGapUs` from inside a live stream. `:515` deliberately probes the gap rule at 500 ms, i.e. on the safe side of the boundary, and never on the far side.
- **`resetAt` not making the next plan cold** -- `:2794` resets and asserts `!isBusy`. No case committed after a reset until `:2622` today.
- **Forgotten cadence returning zero grace** -- needs a hold longer than `chase_stale_us` *inside* a stream, then a plan end. Same missing shape as the cold-governor one.
- **`kShortMoveUs` 50 ms** -- no boundary test exists in either direction; nothing in the suite distinguishes a demoted knot from a promoted one.

**The class:** every long-standing case is a short, evenly paced, uninterrupted stream on a soft machine. Absent are streams with realistic durations *and* gaps (41 ms next to 6875 ms in the same script), anchored commits with real lateness or lateness past the cap, host-driven `resetAt` mid-stream, and any mixed-duration chain. The suite tests one segment or one uniform chain; the field runs a script.

## 4. Scenario harness proposal

Keep the `fieldreplay::S`/`play` shape (`:2511-2568`) -- it is already the right idea -- and finish it:

- `S` gains `mode_expect` (optional) so a scripted row can pin waveform-vs-chase, which is the `kShortMoveUs` regression that got away.
- `play()` gains a per-row `late_ms` instead of the hardcoded `3 * kMs` at `:2547`, so lateness and cap saturation are scriptable.
- Trace ingest: one `tools/` script turning a `PlanTrace` census (`src/main.cpp:709`) into an `S[]` literal plus the `liveTuning()` snapshot read off channel 0x1122. One step, no hand-transcription (T20's lesson).
- `print` defaults **false**; the census `Log` is the assertion surface.

**Default invariants every replay asserts, in `play()` itself, not per-case:**
1. `settles == 0` while any segment's successor is still scheduled.
2. No cold clamp mid-stream (`endvel == 0` and no vmax collapse after row 0).
3. Position continuous: abs(delta p) per 1 ms <= `vmax*1e-3*1.05`.
4. Velocity continuous and bounded: abs(v) <= `vmax*1.001`, abs(delta v) <= `amax*1e-3*1.5`.
5. `p` in `[0,1]` every sample.
6. Plan cost bound: unsolicited replans (`lastPlanUs()` moving with no commit) <= 1 per script, and `PlanKind` flips <= commits -- this is the flip-flop metric `:2748` already computes and discards.

**Replaces:** `:2693` (jitter chain) and `:2645` (long segment) become two rows in the replay table; `:2387`'s three subcases become one replay with a scripted stream end; `:1221`, `:1778`, `:1835`, `:2232` (the 167 ms/400 ms chains) collapse into replays with the policy as a parameter, which finally makes running them under **Blend** free. Keep `:2901` (pure-function grid), `:3364/:3417` (referees), `:951`, `:3444` -- none are scenarios.

## 5. What moves to the RP2350, what stays on the S3

**Moves with the engine (everything that only needs `Engine` + a synthetic clock):** the entire file except the glue concerns below. Specifically all of `:343`-`:2508` and `:2808`-`:3526`. It compiles hardware-free today (`:1`-`:12`) and should be run in the RP's own native env unchanged.

**S3-host concerns, which must NOT follow the engine and currently have no home:**
- The config-push contract (`src/main.cpp:661-698`): full-struct overwrite every tick, the policy and curve-family switch maps, the unpushed-field set. This is a host test -- one case asserting `setConfig` round-trips every `Config` field, which would have caught the `infeasible_blend` gap and the 2.1.49 unreachable-Reshape bug.
- `resetAt` *triggers* -- stream rising edge (`main.cpp:589`) and driver re-seed (`main.cpp:610`). The engine-side "a re-seed is cold" (`:2622`) moves; "who resets and when" stays.
- Anchor derivation and queue lateness (`main.cpp:709`, `PlanTrace`): under the three-board split this becomes link-vocabulary framing, and the S3 keeps it.
- `fieldreplay::liveTuning()` (`:2513`) is a transcription of S3 state. Once the engine lives on the RP it must be generated from the trace, not hand-copied, or it is T20 again.
