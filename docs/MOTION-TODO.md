# Motion / SlopMotion — open questions and queued work

Deferred items from the 2026-07-26 segment-fidelity session. Nothing here is
normative. Items graduate into an RFC (protocol-visible) or a commit
(machine-local) — see each item's **Disposition**.

---

## M-1 — Hermite overshoot vs the stroke window: clip, or pad?

**Status:** open question, deliberately deferred.
**Disposition:** machine-local first (a window/planning decision); becomes
protocol-visible only if a client needs to be *told* about the padding.

### What was observed

Driving the sim with a plain alternating 0.3 / 0.7 knot chain (window
0–500 mm), the **sender's own curve** — not the plan, the client's authored
spline — reached **0.0 mm**. The knots ask for 150 mm and 350 mm; the curve
between them left the window entirely.

That is not a bug in the planner. A cubic Hermite through two knots with a
non-zero end tangent **overshoots its own knots by construction**, and Makima
tangents overshoot harder than Pchip (measured in the MFP plugin: Makima runs up
to **12.1x** the Fritsch-Carlson knot bound on a same-direction run, Pchip a
maximum of 0.89x and exactly 0 at every extremum). The analyzer's new `raw`
line is what made this visible — it draws the sender's curve, and the recorder
clamps it to [0,1] on the way in.

### The question

Clipping is the correct DEFAULT — the window is a physical safety limit and the
machine must never leave it. But clipping a curve that overshoots *flattens the
interpolation* exactly where it is most expressive, so the machine stops
reproducing the script's character precisely at the extremes the script cares
most about.

The alternative is to **pad**: keep the user's stroke window as the region the
*knots* may occupy, and allow the curve between them to use a margin outside it,
bounded by the physical rail. That preserves the spline shape at the cost of
real travel the operator did not explicitly ask for — which is a safety-adjacent
decision, not a purely aesthetic one.

### What needs computing before deciding

1. **Maximum overshoot of a cubic Hermite, in closed form.** For endpoints
   `p0, p1` and tangents `m0, m1` over duration `T`, position is a cubic, so its
   extrema are the two roots of a quadratic — solvable exactly, no scan. Express
   the worst-case excursion beyond `max(p0,p1)` as a function of the tangents.
2. **Bound it under the Fritsch-Carlson knot limit.** With `|m| <= k*|chord|`
   and `k = 1.5`, the overshoot has a finite bound as a fraction of the chord.
   That number IS the required padding, and it is a clean answer rather than a
   guess.
3. **Do the same for the C2 quintic**, which can bulge more than a cubic because
   it must also honor boundary accelerations.
4. **Decide the policy.** Candidates: (a) always clip (today); (b) pad by the
   computed bound, capped by the physical rail; (c) shrink the knot band so the
   padded curve fits the user window exactly — i.e. the user's window means "the
   curve stays inside this", which is arguably what an operator actually expects
   when they set a stroke window.

Option (c) is the one that best matches "the window is what I told the machine
it may use", and it costs amplitude rather than safety. It should probably win,
but it needs the numbers from (1)-(3) first.

### Related, already landed

- `blendEndTowardChord` (smoothness budget) already reduces overshoot as a side
  effect, because lerping a tangent toward the chord is exactly what kills the
  bulge. A large enough smooth budget may make padding unnecessary on the
  infeasible path — but NOT on the feasible path, which is untouched by policy.
- The engine's quintic legality scan already rejects shapes that leave the
  window (with a +/-0.02 grace band), so an overshooting plan currently falls to
  the Ruckig guard rather than executing. Padding changes what "legal" means and
  must be reconciled with that scan.

---

## M-2 — RFC: interpolation / curve-family signaling in SlopSync

**Status: DONE [2026-07-27].** Shipped as RFC-030 — `curve_family` (registry
key 45), normatively specified in `docs/slopsync/SPEC.md` §9.6. Firmware
deployment status: see `docs/canon/LEDGER.md`. The proposal below is kept as
the design record; the shipped wire values (`unspecified`/`c1_cubic`/
`c2_quintic`/`step`) are the registry's, not this section's draft names.

### Why it is needed

A funscript rendered through Pchip or Makima is a **C1 cubic Hermite** spline.
Those interpolators differ only in the rule used to pick knot tangents, and given
endpoint positions plus endpoint tangents the cubic on a span is uniquely
determined. So `{target, duration, end_vel}` on channel `0x0085` is already a
COMPLETE encoding of the sender's curve for any C1 cubic-Hermite family —
nothing is missing from the wire.

What is missing is a statement of **which family the data came from**, so the hub
knows whether the knot kinks are script content (C1, reproduce them) or an
artifact (C2, smooth them). A C2 quintic cannot reproduce a C1 cubic across a
knot by construction, and today the hub guesses by estimating an end
acceleration that is genuinely two-valued at the knot it is estimated at.

### Shape of the proposal (as agreed)

Two SEPARATE concepts, deliberately not fused:

- **(A) What the sender's curve IS** — a property of the data, belongs on the
  wire.
- **(B) What the machine DOES about it** — `curve_policy`, a machine setting.
  Already implemented and live in slopmotion 0.8.0 as
  `FollowClient / ForceC1 / ForceC2`.

(B)'s "follow" mode is only meaningful once (A) exists; today it resolves to C2,
which is the pre-existing behavior byte for byte.

Proposed for (A):

```
curve_family:  0 = unspecified   (points only, no tangent meaning — hub's choice)
               1 = c1_hermite    (tangent exact; knot kinks are script content)
               2 = c2            (curvature continuous; sender means it)
```

- Carried in **HELLO** (session-scoped), because it changes when a user flips a
  dropdown, not per segment. Putting a byte in the `0x0085` packed struct would
  cost a NEW CHANNEL ID (packed layouts are append-only; a changed field is a new
  channel) and would pay per-sample for something that changes hourly.
- Updatable mid-session by an **INTENT**, because it genuinely can change live —
  an MFP user switching Pchip to Makima mid-playback is normal.
- `0` MUST remain the safe default and must change nothing: it is what every
  existing client is today.

### Open sub-questions for the RFC

- Registry: new enum, new INTENT key, WELCOME echo of the accepted value
  (ground-truth doctrine: the hub reports what it will actually DO, which may
  differ from the wish if `curve_policy` is forcing).
- Does the hub advertise which families it can reconstruct, so a client can tell
  the difference between "honored" and "silently downgraded"? Probably yes —
  otherwise a client cannot know its C1 script is being rendered as C2.
- Interaction with M-1: a C1 sender is exactly the case that overshoots the
  window hardest, so the padding decision and this RFC touch the same behavior.

---

## Measured reference numbers (2026-07-26, slopsim, window 500 mm)

Recorded here so later work does not have to re-derive them.

| Quantity | Value | Conditions |
|---|---|---|
| Steady-state following error (`tgt` vs `pos`) | **0.79 mm** | input 1000 mm/s / 50000 mm/s2, peak 377 mm/s |
| Planner shortfall (`raw` vs `tgt`) | **0.00 mm** | same; nothing infeasible at these limits |
| Startup transient (`tgt` vs `pos`) | 67 mm, first ~2 s only | `safeSpeedCap` soft start — by design, decays fully |
| C1 vs C2 in-span peak jerk | C1 is **3-6x lower** | cubic has constant jerk per span; the jerk relocates to the knot |
| C1 knot acceleration step | 1.4-1.7 units/s2, sign alternating with stroke direction | the reproduced script kink |
| `waveform_fallback` under budgeted policies | **3 -> 0** | vs `reshape`, on a deliberately jerk-limited chain |

**Caveat on the last row:** that A/B needed `--jerk 10000` to force infeasibility
at all. With the operator's real limits (1000 / 50000) the probe's segment chain
is entirely feasible, so any future infeasible-path test must either use a
genuinely demanding script or deliberately lowered ceilings — and must SAY which,
because a sim tuned below the real machine does not simulate a slower machine, it
simulates a machine that never runs the code under test.
