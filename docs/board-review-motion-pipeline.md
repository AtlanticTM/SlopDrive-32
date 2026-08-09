# BOARD REVIEW — MOTION PIPELINE (MFP plugin → S3 hub → RP2350 coprocessor)
**Chair's final report.** 14 cold-context reviewers, two independent syntheses, converged. Every claim below re-checked against source; where the syntheses were wrong, I say so and cite the line.

---

## 1. EXECUTIVE SUMMARY

The pipeline is in unusually good health for its stage: its defining property is that failure modes ship as **counted, named telemetry** (residue, `qdrops`, `emit_overrun`, `late_ticks`, `vel_clamped`) rather than as silent drift, and nearly every "dislike" 14 cold readers raised turned out to already carry an in-source comment explaining the tradeoff once checked against the file. The genuine gaps are not correctness bugs in the hot path — they are **evidence gaps**: constants with no derivation, safety properties proven once by a bench sweep recorded in a comment, and a pointer to a document that was retired six days ago. The single structural finding, which neither synthesis named and which is the root of four separate reviewer complaints, is that `test/native/` contains **no suite at all** for MotionArbiter, RangeMapper, or MlinkServoDriver — the entire motion-command layer has zero host-test coverage, so every "no regression gate" complaint is one problem wearing four hats.

---

## 2. DISLIKES (ranked) — these become work items

Ranked by fixable value × strength of evidence, not by reviewer headcount. Where that diverges from the reviewer vote I say why.

### D1 — The motion-command layer has zero host tests
**Where:** `test/native/` contains exactly `test_slopglow`, `test_sloplog`, `test_slopmotion`, `test_slopsync_devicecatalog`, `test_slopsync_discovery`. Nothing covers `src/motion/MotionArbiter.cpp`, `src/motion/range_mapper.cpp`, or `src/motion/MlinkServoDriver.cpp`.

**Why it matters:** this is the actual root of D4, D5, and three separate reviewer complaints phrased as "no regression gate." The window-entry accel asymmetry (`MotionArbiter.cpp:133-151`) is a *safety* property — "never reduce accel authority while overshooting" — currently proven by a one-time 54-config bench sweep whose result lives in a comment. A future edit to `_isOutsideWindow` or the ceiling selection reintroduces the measured 625 mm runaway with nothing to catch it. Same for the glide clamp at `range_mapper.cpp:43-44`, which is correct today and load-bearing for sd-ey0.

**Recommendation:** start with `RangeMapper`. It is pure float math whose only include is `config_api.h` (`include/motion/range_mapper.h:3`) — it is already native-testable with zero refactor, which makes it the cheapest possible first suite. Assert: glide lands exactly on goal and never overshoots for any `dt`; `setRange` swap-and-clamp; the minimum-window rule (D2). Then the arbiter's ceiling-selection table as a second pass. **Board item: new, `area:motion`.**

### D2 — Hardcoded 5 mm minimum stroke window, no rationale anywhere
**Where:** `src/motion/range_mapper.cpp:26-29`
```cpp
// Ensure at least some minimum range size
if (_goal_max_mm - _goal_min_mm < 5.0f) {
    _goal_max_mm = _goal_min_mm + 5.0f;
}
```
**Why it matters:** 6 of 14 reviewers found it independently and not one could answer "why 5" from the tree. It silently *widens* an operator's window — an operator asking for a 2 mm window gets 5 mm and no echo of the override — which is a Ground-Truth Doctrine concern, not just a style one. The comment restates the code (C-12) instead of stating the constraint.

**Recommendation:** the cheapest correct fix in the whole set. Name it in `config_api.h` (`MIN_STROKE_WINDOW_MM`) with a one-line constraint comment saying which it is: motor resolution floor, numerical-stability floor for `positionToIntensity`'s divide, or UX floor. **Same commit, same file:** `range_mapper.h:5` still describes the class as mapping "Buttplug intensity" — verify that name is current given the SlopSync-only intake ruling, and correct the file header to the `// Constraints:` form while you're in there. **Board item: new, `area:motion`, P3.**

### D3 — Two comments point at a retired document
**Where:** `src/motion/MotionArbiter.cpp:150` — `// 306.78 mm -> 1.06 mm (289x). See LEDGER "Pending rulings".` and `src/motion/AIMServoDriver.cpp:781` — `// TODO(LEDGER: AUTHORING-LEGIBILITY ceilings ruling 2026-07-29)`.

**Why it matters:** `LEDGER.md` was retired 2026-08-03 (governance.md §6 amendment; volatile truth moved to the board). Both are C-12 violations — a comment referencing something that is gone — and worse, `MotionArbiter.cpp:150` is the **only pointer to the evidence for a safety property**. The 289x bench sweep is the justification for a deliberate accel asymmetry on a runaway path, and its citation now resolves to nothing. Neither synthesis caught this.

**Recommendation:** move the 54-config sweep result onto the board as a stamped `[verified 2026-…  -- bench sweep 54 configs]` note, repoint the comment at the issue id, and give `AIMServoDriver.cpp:781` a `TODO(<board id>)` per cpp-style.md or delete it. **Do this in the same commit as D1's arbiter test** — the test is the durable form of the same evidence. **Board item: new, `area:motion`, P2.**

### D4 — The `v0 = 0` trapezoid assumption
**Where:** `src/motion/MotionArbiter.cpp:17-30` (the rationale block), consumed at `:445-458` — `derived_accel = 4 * distance / T²`, the from-rest triangle minimum.

**Why it matters, and why I rank it 4th and not 1st:** every reviewer in both syntheses landed on this — Synthesis B says 14/14, A says ~12/14 — making it the single most distrusted line in the tree. But I verified the defense and it holds: the derived accel is clamped at the ceiling (`:465`, "Ceilings still clamp below — never exceeded"; `:151` on the stream path), hard step bounds are enforced independently (`:114-116`), and doctrine already says ceilings are never targets. The comment is honest that `getCurrentSpeedInMilliHz()` exists and is unwired. **This is a legibility defect, not a correctness defect** — but a line that makes 14 independent expert readers stop and distrust it *is* a defect, just a different one.

**Recommendation:** do **not** rewrite the planner. File the deferral as a board item so the comment can point at a tracked id instead of reading as abandoned, and pin the "≤1% above 100 Hz retarget" claim with one case in D1's arbiter suite. If it's still unwired in three months, the board item is the thing that says so out loud. **Board item: new, `area:motion`, P3.**

### D5 — The window-entry accel asymmetry is measured on ONE of the two paths it's applied to
**Where:** `src/motion/MotionArbiter.cpp:133-151` (stream path, measured) and `:402-421` (trapezoid path, **not** measured).

**Why it matters:** both syntheses called this "bench-proven" and cited 11/54 → 0/54 as covering it. **Both overstated it, and the code is more honest than either synthesis.** `:413-418` says so explicitly: *"the 54-config bench sweep that measured this (11/54 runaways -> 0/54) exercised the STREAM path only. Applying it here is the same defect and the same rule, but it is UNMEASURED on this path."* The comment even names the behavior to watch — a pattern/segment move re-entering the window now keeps full accel authority. That is an open, self-declared measurement gap on a safety path, sitting one scroll below the line every reviewer praised.

**Recommendation:** run the same sweep against the `_planAndDispatch` path (MANUAL-excluded sources, deadline and no-deadline both), and either stamp it or record the divergence. This is the highest-value *bench* item in the report. **Board item: new, `area:motion`, `safety` label, P2.**

### D6 — The interior-velocity census measures but never gates
**Where:** `src/motion/MlinkServoDriver.cpp:143-186` — 9 derivative evaluations (`k = 0..8`) of the exact quintic the slave will render, worst-per-second logged via `SLOGI("mchunk", ...)`.

**Why it matters:** the census finds real peaks (comment cites >1900 mm/s interior spikes with clean endpoints, and 1230-1278 mm/s against a 950 ceiling on every re-anchor). Nothing clamps, reshapes, or rejects the offending chunk — it only prints. Meanwhile the *sweep* path one function up **does** act on the same physics (`:117-142`: the 1.875x peak-governed stretch and the 1.5x Fritsch-Carlson bound). So the loop is closed for re-anchor sweeps and open for everything else. Separately, the cost claim ("T27-negligible next to the SPI xfer", `:148`) is comparative against the ~40 µs transaction, not an absolute check against motorTask's tick budget — reviewers were right to note that, though at 9 float evals it is not plausibly a real risk.

**Recommendation:** decide and write it down: either this is deliberately observe-only for the duration of the sd-ar3.1 grit hunt (say so in the comment, with the board id), or the peak-governed stretch at `:125-127` generalizes from the sweep path to every chunk. Do not leave it ambiguous — an instrument that finds ceiling violations and does nothing is the shape of a bug report nobody filed. **Attach to sd-ar3.1 / sd-d77.**

### D7 — `liveCounts()` dead-reckoning capped at 3 ticks, cap not derived
**Where:** `src/motion/MlinkServoDriver.cpp:71-78`; rationale at `include/motion/MlinkServoDriver.h:118-120`.
```cpp
uint32_t age = millis() - _status_ms + kTickMs;
if (age > 3 * kTickMs) age = 3 * kTickMs;
return _pos_counts + _vel_counts * (float(age) * 1e-3f);
```
**Why it matters:** constant-velocity extrapolation, so it under/overestimates whenever the RP is mid-accel, and it is load-bearing for **every chain re-base** (`.h:120`, "Use for every chain re-base"). The header ties the mechanism to a dated incident ("the chain-start gap made re-anchors teleport, 2026-08-09") — so it was tuned against a real symptom, not invented — but the *3* itself has no measurement behind it. This feeds directly into the sd-dxy.1.x drift/teleport family that is currently open.

**Recommendation:** instrument the actual distribution of `millis() - _status_ms` at the call site for one bench session and set the cap from the observed p99, or state in the comment that 3 ticks is a deliberate "beyond this, trust nothing" floor rather than a staleness estimate. The second is a legitimate answer and costs one line. **Attach to sd-dxy.1.**

### D8 — The segment-split priming gate is depth-0-only, and the gap is known
**Where:** `src/motion/MlinkServoDriver.cpp:392-398`, calling `sendSegmentSplit()` at `:214-241`.
```
// depth 0: sustained multi-frame ticks exceed the slave's per-frame-
// reset budget (2.4.87: torn 83k, qdrops 607); deeper waits on sd-dxy.
```
**Why it matters:** five reviewers called the split a "band-aid" and were wrong — it re-evaluates the exact quintic at `u=0.5` for a true midpoint `(p, v, a)`, which is a correct curve split, not an approximation, and the comment states the real reason it exists (production is real-time-capped, so steady one-per-tick shipping can never deepen the ring by itself). The genuine gap is narrower and only one reviewer found it: **ring depths 1 through 7 get no priming at all**, and that is an explicitly deferred known limitation with a measured cost attached to it, not a design choice.

**Recommendation:** no code change. Confirm the deferral is actually captured under sd-dxy — if it isn't, the comment's "deeper waits on sd-dxy" is a second dangling pointer of the D3 class.

### D9 — `kRefreshMs = 100` has no derivation
**Where:** `src/motion/MlinkServoDriver.cpp:29`; described as "drop insurance" at `include/motion/MlinkServoDriver.h:135`.

**Why it matters:** minor, and listed for completeness because four reviewers raised it. The *mechanism* is well-reasoned — the seq-echo ack exists precisely because "a torn retarget otherwise waits out the full 100 ms refresh (felt as a mid-stroke stall under EMI)" (`.h:141-142`) — but the 100 itself is unanchored to any measured drop rate.

**Recommendation:** lowest priority in the set. Fold a one-line justification into whatever commit next touches the file. Not worth its own item.

### Discarded as noise (both syntheses agree, and I concur)
- **No `alignas()` on the PSRAM placement-new'd hub.** Single-source, no evidence of misalignment offered, and `MALLOC_CAP_8BIT` covers the class. Not elevated.
- **`RangeMapper::tick()` overshoots on a stalled task.** **Refuted at source:** `range_mapper.cpp:43-44` clamps the step to the exact remaining distance (`fabsf(dmin) <= step ? dmin : ...`), so an oversized `dt` lands *on* the goal. Overshoot is impossible.
- **Retarget refresh has an unclear trigger.** Refuted — the trigger is `now - _last_cmd_ms >= kRefreshMs`, stated plainly. (The *value* survives as D9; those are different complaints and one synthesis conflated them.)
- **`MotionPassthrough.h` included but unused.** It is included at `MlinkServoDriver.cpp:19`; the reviewer admitted not checking usage. Not a finding.

---

## 3. LIKES — the patterns that work, stated as templates for fixing the dislikes

These are not compliments. Each one is a **reusable shape**, and the dislikes above should be fixed by reaching for the nearest one.

### L1 — Ship the two numbers whose *difference* is the diagnostic
`include/comms/MotionLinkProtocol.h:83-115`, `include/motion/MlinkServoDriver.h:99-121`. `pos` is the commanded position recomputed from the polynomial every tick — "the number that cannot be wrong." `emitted` is what was actually pulsed. Their difference is renderer residue, and the header states the rule: *"Read them as a pair or neither is evidence."* Every counter names one distinct failure (`qdrops` = PIO FIFO full, `emit_overrun` = illegal plan or late tick, `late_ticks` = timeline stretch), and they **saturate rather than wrap** (`sat16`, `:113`) because a pinned counter honestly reads "lots" while a wrapped one reads "healthy" and lies.

**Template for:** D6 and D7. A census that only reports a peak (D6) is half of this pattern — it has the measurement but no paired number that makes it actionable. `liveCounts()` (D7) ships an estimate with no companion residual saying how wrong the estimate was.

### L2 — Enforce doctrine at compile time, not by convention
`include/motion/MotorDriver.h:9-16, 57-67`. The motion methods are `protected` with `friend class MotionArbiter` as the grant, so a call through a `MotorDriver&` from outside the arbiter is a **compile error**. The header is precise about the one exception (`friend class MotorProxy` at `:67`, because NVS isn't readable at static-init time so main.cpp binds the concrete driver at boot) and about the trap (`:15-16`: friendship doesn't inherit, so a concrete driver's overrides must stay protected or the lock leaks through the derived type).

**Template for:** D1. Where the compiler can't hold a rule, a test must. The sole-caller rule needs no test because it cannot be violated; the accel-asymmetry rule needs one because it can.

### L3 — When the safe-looking choice is the dangerous one, say so at length and with numbers
`src/motion/MotionArbiter.cpp:133-151`. Speed is gentled on window entry, acceleration deliberately is not, because `entering` is keyed on position alone and cannot distinguish "parked outside at rest" from "just overshot at speed." The comment names both cases, quotes the measured collapse (50,000 → 200 mm/s², 250x, at the instant braking is needed), the worst measured excursion (625 mm on a 500 mm rail — into the end stop), and the sweep that fixed it. Then `:413-418` goes further and marks the *scope limit* of its own evidence.

**Template for:** D2 and D9. This is what a load-bearing constant looks like when it's done right. `5.0f` with `// Ensure at least some minimum range size` is the same category of decision documented at the opposite extreme.

### L4 — Glide the goal, never snap the effective value
`src/motion/range_mapper.cpp:38-45`. `setRange()` moves the *goal*; `tick()` walks the effective window toward it at a bounded rate, clamped to land exactly on the goal, never past. Physical limits are the deliberate exception — `setMaxRailMm` clamps goal *and* effective immediately, with the reason stated (`:10-11`: "a hard limit never glides"). Six lines, branch-light, and it is the fix for sd-ey0's violent reposition.

**Template for:** any future "the operator changed a live value" path. The goal/effective split with an explicit hard-limit carve-out is the general shape.

### L5 — Document the hardware fact that makes the code look wrong
`include/comms/MotionLinkProtocol.h:25-30` (SPI MODE 1 is load-bearing: the RP2350's PL022 slave in mode 0 delivers exactly one byte per continuous-CS 32-byte burst, measured 2026-08-06) and `src/motion/MlinkServoDriver.cpp:38-49` (the 200 µs inter-frame gap, and the `vTaskSuspendAll()` around a ~40 µs transaction because the sampler at prio 4 on the same core otherwise preempts mid-frame and the slave's IRQ spin bails at ~300 µs of silence — "which is why tears landed exactly on command boundaries").

**Template for:** every constant in D7 and D9. The difference between `200` here and `100` there is one sentence of measurement.

### L6 — Make continuity exact by construction, not by approximation
`src/motion/MlinkServoDriver.cpp:99-106` and `src/rp2350_motion/main.cpp:265-278`. `holdKnotAccel()` computes the endpoint accel by centered difference off the analytic curve, and the shipped `a1` is reused **verbatim** as the next segment's `a0` — so the quintic chain is exactly C2, not approximately. The comment names what the C1 cubic cost: a 100 Hz torque notch the servo renders as texture. The `dt_us == 0 || dt_us > 200000u` guard is real; note it returns `0.0f` silently, which is the one place this pattern could use a throttled WARN.

### L7 — Invert the master/slave pacing assumption and state it as a ruling
`include/comms/MotionLinkProtocol.h:7-10`. The schedule is time-indexed: the RP renders segments at their own pace and **never drains faster to catch up**; overflow is the producer's fault and the S3 paces on the reported runway. Stated as an operator ruling with a board pointer, in the one header both boards include (`:3`, "ONE definition, BOTH ends include it (T20). Never copy a constant out."). See INTERESTING #1 for where the implementation diverges from this header's own next sentence.

### L8 — Bypass your own setters when the setter's side effect is the bug
`clients/mfp/SlopSync.cs:242-246`. `SeedWindowDrafts` writes the backing fields directly, because the public setters (`:214`, `:219`) are what set `_windowDirty = true`. A device-STATE-driven reseed through the setters would mark the UI dirty on the very first STATE update and then swallow the second field write — and the comment at `:238-241` names that exact historical bug rather than describing what the code does. Paired with the three-marker rail (`RailPositionPx:391` = what the device *is* doing, `RailTargetPx:392` = what it was *commanded*, `RailAxisPx:408` = what the client *asked for*), this is Ground-Truth Doctrine made visible: clamps and lag become something an operator can see instead of guess at.

---

## 4. INTERESTING — with my lean, and the one look that settles each

### I1 — RP underrun zeroes velocity instantly. Lean: **negative, and already ours.**
`src/rp2350_motion/main.cpp:235-244` sets `s_state = kStateSettled; s_flags |= kFlagUnderran; s_vel = 0.0f;` and returns without advancing `s_pos`. That is a freeze, not a brake-to-rest, and it contradicts this link's own header three files away (`MotionLinkProtocol.h:10`, "Underrun gets SETTLE (hold at the last endpoint)").

Synthesis A called this "the most consequential item in the whole set." **I am demoting it, for a good reason:** `bd show sd-dxy.1.4` is already open at P1, dated 2026-08-08, and quotes this exact code block, names the stamped operator ruling it violates, and identifies the deeper C-1 failure — SlopMotion on the S3 defines Settle as *braking* to rest with a `settle_grace_us = 30000` guard, while the RP uses the same word for a hard stop. Same word, two behaviors, one on each side of the link.

**What this actually tells us is better news than a new finding:** 14 cold readers with no board access independently rediscovered a P1 the operator had already characterized in more depth than the review did. That is a calibration signal for the whole review — it says the reviewers' instincts track the board. **The one look:** none needed. Work the existing issue.

### I2 — The census/sweep loop is closed for re-anchors and open for everything else. Lean: **genuinely unclear.**
See D6. The sweep path acts on interior peaks (`MlinkServoDriver.cpp:117-142`); the general census only prints (`:143-186`). **The one look:** ask whoever ran the mchunk census whether non-sweep chunks were ever observed above ceiling *outside* a re-anchor. If no, this is correctly diagnostic-only and should say so; if yes, the peak-governed stretch generalizes.

### I3 — Fixed `u = 0.5` split, and the 1.875 / 1.5 constants. Lean: **positive on all three.**
The split is a mathematically exact bisection (`:229-238`), not a heuristic, and priming to depth 2 in one tick is exactly what an empty ring needs given real-time-capped production. `1.875` is the from-rest quintic's interior-peak-to-mean ratio, cited against a measured census (`:120-124`). `1.5` is the standard Fritsch-Carlson monotonicity bound (`:132-136`) — a textbook constant, not a placeholder, and the comment even handles the consequence (`:140-141`: a clamped `v1` makes the passed `a1` inconsistent, so the sweep lands flat). **The one look:** whether 1.5 has ever been swept downward for a tighter bound. Low value; I'd leave it.

### I4 — `kJumpWalkMaxCounts = 4096` doesn't scale with the live ceiling. Lean: **mildly negative, cheap to settle.**
`src/rp2350_motion/main.cpp:106-108` states its derivation ("drains in ≤100 ms at a 400 mm/s ceiling") but is a `constexpr`, while the actual ceiling arrives at runtime via `kOpSetLimits` and lands in `s_maxCountsPerTick` (`:98-101`). At a 950 mm/s ceiling the stated 100 ms property no longer holds. The 64-count teleport trigger (`:257`) has no equivalent comment at all, though `MlinkServoDriver.h:151-154` explains what it cost: microsecond timeline stamps exist *because* millisecond quantization put ±10% speed error on a 10 ms chunk, which at speed exceeded this same 64-count guard at every boundary. **The one look:** derive `kJumpWalkMaxCounts` from `s_maxCountsPerTick` (it's one multiply) or restate the comment as "≤100 ms at 400 mm/s; longer at higher ceilings, bounded by X." Note sd-dxy.1.7 already proposes retiring the teleport as a routine path, which may make this moot.

### I5 — `kEmitCatchupMargin = 1.25f`, self-labeled unproven. Lean: **positive — take the honesty at face value.**
`src/rp2350_motion/main.cpp:103-105`: *"Margin above the ceiling is unproven drive speed (sd-ar3 layer 3): only discontinuity recovery runs there (~1.4 ms per 64-count deficit)."* The code names its own uncertainty, bounds who is allowed into that regime, and points at the board. **The one look:** a bench measurement of actual drive follow above ceiling — not a code review finding.

### I6 — Curve family fixed at HELLO. Lean: **positive; not a firmware question.**
`clients/mfp/SlopSync.cs:954-1020`: the family is chosen at HELLO from the interpolation type, and the client *does* detect and log a downgrade when the hub grants a different family (`:1017-1020`). Reconnect-to-change is a coherent contract. **The one look:** whether RFC-030 anticipates mid-session renegotiation — a SlopSync spec question, on the SlopSync board, not a firmware fix.

### I7 — MFP script-time → hub-time re-anchoring. Lean: **genuinely unclear, and the highest-value live trace in the report.**
`clients/mfp/SlopSync.cs:536-545` documents a real, hard-won subsystem: the script→hub time mapping is established **once per re-anchor** because anchoring each span against a freshly sampled `(axisPos, HubNowUs)` pair made consecutive spans stop tiling — the media clock and wall clock don't advance at the same rate, and the hub read the result as segments *overlapping* in scheduled time (measured `gap=[-1060,+899] µs`). The tell was that re-anchoring by *any* means — a settings change, pause/resume, any value — restored smoothness, which proves the mapping and not the value was stale. Several reviewers independently reconstructed a suspicion that the resync cadence produces an operator-visible "smooth, drift, snap back" cycle. **The one look:** a live trace of span-boundary gaps across a resync boundary. This is cross-repo, stateful, and wall-clock-dependent — it will not yield to more reading.

---

## 5. DISSENT — where the two syntheses disagreed, and my ruling

**§5.1 — SETTLE semantics. A: a freeze that contradicts the ruling. B: listed under LIKES as "brake to rest."**
**Ruling for A. B is factually wrong.** `src/rp2350_motion/main.cpp:240` is `s_vel = 0.0f;` with no deceleration and no `emitTowardPos()`. B appears to have read the *header's* description (`MotionLinkProtocol.h:10`) and credited the implementation with it — which is exactly the doc-vs-code trap this repo's governance is built to catch, reproduced inside the review itself. Instructive: a comment good enough to be praised is also good enough to be mistaken for the code. **But see I1** — this is sd-dxy.1.4, already open, already better characterized on the board than in either synthesis.

**§5.2 — Retarget 100 ms. A: unjustified (dislike). B: refuted (noise).**
**Both right, answering different questions.** B correctly refutes "unclear trigger" — `now - _last_cmd_ms >= kRefreshMs` is plain. A correctly notes the *value* 100 has no measured basis (`MlinkServoDriver.cpp:29`). Ruling: trigger is clear, value is unanchored, survives as **D9** at the bottom of the list where it belongs.

**§5.3 — The `streamSample`/`update` write race. A: noise, "explicitly refuted by the header comment." B: dislike, "narrows the window, doesn't eliminate."**
**Ruling: B is right that A over-dismissed it; A is right that it isn't a finding as stated.** `include/motion/MlinkServoDriver.h:146-150` establishes that both writers are Core 1, so torn state is preemption between two statements rather than true concurrency, and that `streamSample()` orders its stores so `_seg_mode` reads true only after the chain fields are coherent. That is a real ordering discipline and it *is* a refutation for `_seg_mode` — A read the comment correctly. It is **not** a refutation for the other `_seg_*` fields, which is B's actual point and which neither synthesis checked. **Demoted to a settle-with-one-look:** audit every store to `_seg_p/_seg_v/_chain_*/_hold_*` from `streamSample()` for the same discipline. Cheap, and it either confirms the comment generalizes or finds the one that doesn't.

**§5.4 — Segment split. A: dislike #5, downgraded mid-write. B: Interesting #5.**
**Ruling for B's placement, A's reasoning.** A did the better analysis (correctly identifying that the split is an exact bisection and that the *real* gap is the depth-0-only gate with `2.4.87: torn 83k, qdrops 607` behind it) but left it filed as a dislike after arguing itself out of the complaint. Filed as **D8**, documentation-only.

**§5.5 — The accel asymmetry's evidence base. Both syntheses: "bench-proven, 54 configs, 289x."**
**Ruling against both.** The sweep covered the stream path only; `MotionArbiter.cpp:413-418` says so in the code, in capitals, four lines below the passage both syntheses quoted approvingly. Neither caught it. Filed as **D5** and it is the strongest *new* finding to come out of chairing this. Worth noting as a review-process lesson: both syntheses independently stopped reading at the end of the paragraph they were praising.

**§5.6 — Reviewer counts (A: ~12/14 on v0=0; B: 14/14).**
Immaterial, and B's own caveat is the correct one — independent phrasing of the same `file:line` makes exact tallies fuzzy. Do not let a headcount set a priority; D4 is ranked on evidence, not votes.

**§5.7 — Findings neither synthesis produced.**
Three, all verified: the **zero host-test coverage** of the entire motion-command layer (D1, and the structural root of four separate reviewer complaints), the **two dangling `LEDGER` pointers** including the one holding the only citation for a safety property's evidence (D3), and the **unmeasured second application** of the accel asymmetry (D5). All three are cheap. Two of them are documentation.

---

### Chair's closing note
Nothing in this report says the motion path is unsafe. It says the pipeline's *evidence* is younger than its code: the reasoning is excellent and almost entirely in-source, but it lives in comments, and comments are not gates. Three of the top five items are "write down what you already know" and one is "run the sweep you already built against the second path you already applied it to." The pattern to copy while doing all of it is L1 — ship the pair of numbers whose difference is the diagnostic — because every item on the dislike list is, underneath, a place where only one of the two numbers exists.
