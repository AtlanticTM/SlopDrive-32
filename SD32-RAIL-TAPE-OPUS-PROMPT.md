# SD-32 — Rail Evolution: Input Tape · Hazard Zones · Comet Trail · Set-Bounds
## Executor: Claude Opus 4.8 · full web UI source in context · device LIVE at 192.168.1.229, motor connected, homing works
## An approved interactive design mock exists; this document is its complete spec. Implement EXACTLY this — no reinterpretation.

═══════════════════════════════════════════════════════════════
0. PRIME RULES
═══════════════════════════════════════════════════════════════
R1. GROUND TRUTH DOCTRINE: the tape, hazard zones, and band all render from the SAME source of
    truth as the device's window (the shadow-reported window state). The UI must never display a
    window geometry that differs from the device's, in either direction. Local drag state may
    preview, but settled rendering follows the device echo.
R2. NOTHING SHIPS UNVERIFIED: every interactive element you build or touch must be proven to
    actually drive the device, live, with evidence quoted in the executor log (the request payload
    AND the resulting device-state change from telemetry//api/settings). This codebase has a
    history of controls that render but drive nothing — band drags that never applied, sliders
    wired to void. A control that looks right and does nothing is a failure of this task, full
    stop. Section 6 defines the mandatory verification matrix.
R3. USE EXISTING PATHS: no new endpoints, no protocol changes, no new WS ops. The tape commands
    through the existing move path; set-bounds goes through the existing window-set path so the
    shadow pending/echo/overdue machinery applies automatically. If an existing path is broken,
    fix it (and log it) rather than routing around it.
R4. DESIGN LANGUAGE IS FROZEN: use the existing CSS custom properties (--intent, --reality,
    --warn, --bad, --screen, --line-*, --tx-*, --s). No new colors, no new fonts, no glow on
    structure. Red appears ONLY in the hazard zones (and existing e-stop/fault surfaces).
5. SCOPE: rail.js, main.js (wiring only), index.html (rail panel markup/CSS only). Do not touch
    firmware, link.js, telebuf.js internals, shadow.js internals, or unrelated features.

═══════════════════════════════════════════════════════════════
1. THE INPUT TAPE (new command surface above the rail)
═══════════════════════════════════════════════════════════════
Structure: inside #railPanel, ABOVE #spineRailHost, replace the existing #railQuickMove strip
with the tape assembly (the tape supersedes quickmove — remove quickmove's element, keep and
rewire its useful handlers where noted):

  label row:  left  = tape mode label, Chakra Petch 10px letterspaced, --intent-tinted
                      "input · window"            (normal mode)
                      "input · full travel · manual" (manual mode)
              right = tape extent, Martian Mono 10px ghost: "048–176" / "000–260"
  tape track: full-width, height ~26px (respect --s), with 1px dashed --line-1 top/bottom
              guides spanning the full rail width (these show where full travel IS even when
              the tape doesn't cover it)
  tape:       absolutely positioned inside the track; background --screen with inset shadow;
              1px border rgba(--intent, .45); faint vertical tick texture
              (repeating-linear-gradient, --intent at .10, 7px pitch); centered ghost
              microlabel "TAP · SCRUB" (Martian Mono 9px, letterspaced, --tx-ghost);
              a 2px --intent scrub pip (opacity 0 → 1 while pointer down, follows pointer).

GEOMETRY LAW (the core concept — implement precisely):
  Normal mode:  tape.left/width == the REPORTED window's extent, exactly, at all times —
                animated (~250ms cubic-bezier(.3,.7,.3,1)) as the window changes from ANY
                source (local drag settle, device echo, curl-side change arriving via config).
                The tape must track the band because both render from the same state (R1).
  Manual mode:  tape spans the full rail width; border/glow step up to full --intent
                treatment; label/extent text swap as above.
  The tape's pointer math maps its LOCAL x-fraction to [wMin..wMax] (normal) or [0..TRAVEL]
  (manual). It is therefore GEOMETRICALLY IMPOSSIBLE to command outside the permitted range in
  normal mode. Do not add a redundant clamp-check toast; the geometry is the constraint.

BEHAVIOR:
  pointerdown on tape: capture pointer; if a pattern is running, stop it via the existing
    onPatternStop path (manual wins — same as current rail-tap semantics); send the mapped
    position through the existing move sender (window.__sendMove / za(mm, true) — the stream
    variant, same one quickmove used); show pip.
  pointermove while captured: continue streaming mapped positions (existing rate limit in the
    sender applies — do not add another).
  pointerup/cancel: hide pip, release.
  The rail itself (spineRailHost) LOSES its tap-to-command behavior in normal mode: rail
  surface is now display + window editing only. Band body drag = move window; band handles =
  resize; those keep their existing wiring (verify it works — see §6; if the band paths are
  still broken from the overhaul, fixing them is IN scope under R3).
  In manual mode, the existing full-rail scrub behavior (r.manual drag='scrub') is REPLACED by
  the tape (now full-width): manual scrubbing happens on the tape, not the rail. The rail in
  manual mode is display-only. Keep the existing manual-toggle side effects (bypassLimits
  checkbox sync) exactly as they are.

═══════════════════════════════════════════════════════════════
2. HAZARD ZONES (thin red keep-out hatching outside the window)
═══════════════════════════════════════════════════════════════
Two DOM ribbons inside #spineRailHost (siblings of the band, below the canvas in z-order,
pointer-events:none):
  .rail-hz.lo: left:0, width = wMin fraction
  .rail-hz.hi: right:0, width = (TRAVEL - wMax) fraction
  Vertical: thin — 9px tall (scale by --s), vertically centered on the track baseline
  (the existing 1px track line at mid-rail).
  Fill: repeating-linear-gradient(135deg, rgba(255,71,87,.22) 0 3px, rgba(255,71,87,.03) 3px 7px)
  — use the --bad channel; whisper-level, must never outshine the phosphor or the band.
  Updates: live during band drag (same layout function as the band), AND from device echo /
  config adoption (R1). Width transitions instant during drag (no lag), 250ms ease on
  programmatic changes.
  Manual mode: ribbons fade to opacity .18 (limits exist but are unenforced) — transition .25s.

═══════════════════════════════════════════════════════════════
3. COMET TRAIL (phosphor rework — perfectly smooth, speed-scaled)
═══════════════════════════════════════════════════════════════
Replace the current trail rendering in the rail canvas draw with a dot-emitted comet meeting
ALL of these:

3.1 EMISSION: the trail is generated ONLY from the center dot's path (the marker's midpoint,
    track baseline Y). The full-height marker line and the purple commanded caret are redrawn
    crisp every frame and leave NO trail. Canvas is FULLY cleared each frame (no
    destination-out persistence — that technique is retired for this canvas).
3.2 HISTORY: maintain a ring buffer of (x, t) samples of the dot's interpolated position
    (sourced from the same render-clock interpolation the marker uses — telebuf.sampleAt at
    t_render). Fixed-size Float64Array ring, no per-frame allocation. Retain ~850ms.
3.3 PERFECT SMOOTHNESS — this is the acceptance-critical detail: the trail must contain ZERO
    visually distinguishable segments, steps, or banding at any speed. Do NOT stroke per-
    segment with stepped alpha (the previous approach — it bands). Render the trail as ONE
    filled, tapered ribbon per direction-run:
      - For each contiguous run of same-direction motion in the history window, build a single
        polygon: top edge = points (x_i, Y − w_i) oldest→newest, bottom edge = (x_i, Y + w_i)
        newest→oldest, where w_i = headHalfWidth × f_i and f_i = ((1 − age_i/850ms) clamped)²
        (tapering to ~0 at the tail).
      - Fill each ribbon with ONE createLinearGradient along x from head (rgba --reality-blue,
        α ≈ .55) to tail (fully transparent). A single gradient fill cannot band.
      - Split ribbons at direction reversals (sign change of dx) so polygons never self-
        intersect; the newest ribbon draws last.
      - One soft glow pass: shadowColor rgba(77,166,255,.6), shadowBlur ~8 on the fill (or a
        second slightly-larger blurred fill — whichever is cheaper; measure).
3.4 SPEED-SCALED THICKNESS: headHalfWidth scales with the dot's speed. Marker line height is H
    (currently 36px span); the trail's FULL thickness at the head reaches up to 50% of H at
    high speed:
      headHalfWidth = lerp(1px, 0.25·H, clamp(speedNorm, 0, 1))
      speedNorm = EMA-smoothed |dx/dt| (α ≈ 0.2) normalized against a reference speed —
      derive the reference from the input-set max speed mapped to px/ms at current rail width
      (fall back to a sane constant if unavailable; log the choice).
    Slow motion → thin filament; fast strokes → bold comet. At rest the trail tapers out
    within ~1s leaving only the crisp marker + dot.
3.5 BUDGET: no allocations inside the rAF loop (reuse ring + one scratch path/point array);
    DPR handling preserved; prefers-reduced-motion → static 40% opacity 1px trail, no ribbon.

═══════════════════════════════════════════════════════════════
4. MANUAL MODE: SET MIN / SET MAX FROM CURRENT POSITION
═══════════════════════════════════════════════════════════════
In the existing #railManualActions row (visible only in manual mode), provide two buttons
(reuse .btn styling, --intent-bordered, flex:1):
  "⟸ Set min here"   /   "Set max here ⟹"
Both act on the CURRENT ACTUAL position (the interpolated displayed position, rounded to
0.1mm), not the commanded target.

YIELDING-BOUNDS RULE (approved conflict resolution — implement exactly):
  MINLEN = 5mm (single constant, shared with the handle-drag clamps — unify if currently
  duplicated).
  Set min: wMin = clamp(pos, 0, TRAVEL − MINLEN); if wMax < wMin + MINLEN then wMax =
    min(TRAVEL, wMin + MINLEN) and the HI handle + band label FLASH amber (--warn) for 700ms.
  Set max: symmetric (wMax = clamp(pos, MINLEN, TRAVEL); shove wMin down if needed; LO handle
    + label flash).
  The user's stated bound always lands exactly where they stood; the other bound yields to
  preserve MINLEN; the shove is always announced (flash), never silent; the window can never
  collapse to 0 or invert. No confirmation dialogs.
DISPATCH: after computing, push through the SAME debounced window-set path the band handles
use (the mt()/ht() + J()/Ue() route), so the shadow enters pending, the echo confirms, and
overdue states apply unchanged. The amber shove-flash is additive to (not a replacement for)
the pending/settle visuals.

═══════════════════════════════════════════════════════════════
5. STATE INTEGRATION (the part that keeps the UI honest)
═══════════════════════════════════════════════════════════════
5.1 One layout function owns geometry: band, tape (normal-mode extent), and both hazard
    ribbons all position from the same (wMin, wMax) pair in one place. Local drag updates it
    for preview; settle/echo/config-adoption update it from reported state. Grep every current
    writer of band geometry and route them through this function.
5.2 Shadow states extend to the new elements: while the window shadow is pending/overdue, the
    tape border goes dashed (matching the band's pending convention) and hazard widths follow
    the DESIRED value (preview), settling to reported on confirm — identical lifecycle to the
    band. Stale/suspended body states apply (canvas dims etc.) — verify the new elements
    inherit the existing body.stale / body.suspended CSS reach, add rules only if they don't.
5.3 Cold-path truth: a window change arriving from OUTSIDE this client (curl a settings
    change) must re-render band + tape + hazards within one config/echo cycle without reload.

═══════════════════════════════════════════════════════════════
6. MANDATORY LIVE VERIFICATION (quote evidence for every row)
═══════════════════════════════════════════════════════════════
Perform against the live device with devtools + curl; paste payloads/telemetry excerpts into
the executor log. A row without evidence is not done.
V1  Tape tap (normal): exact POST/WS payload quoted; telemetry commanded target changes to the
    mapped mm; physical/telemetry position follows.
V2  Tape scrub (normal): stream of positions sent (quote 3); final release position == last
    input; pattern running at start is stopped by first touch.
V3  Tape geometric constraint: with window 40–140, the minimum/maximum commandable positions
    from tape edges are 40 and 140 (quote the two payloads).
V4  Manual mode: tape extends full travel; scrub reaches <40 and >140 (quote payloads);
    bypass checkbox side-effect unchanged from current behavior.
V5  Set min mid-motion at pos≈200 with window 40–140: /api/settings (or echo) shows
    min=200, max=205; hi-handle amber flash observed; shadow went pending→confirmed.
V6  Set max at pos≈30 with window 200–205: shows min=25, max=30; lo flash.
V7  Band drag + both handle drags STILL DRIVE THE DEVICE (regression on the Wave-1.5 fixes):
    quote one payload + settings echo for each of the three gestures. If any is dead, fix
    before proceeding and log the root cause.
V8  Cold path: curl a window change → band, tape, and hazards re-render ≤1s, no reload.
V9  Comet: run a 1–2Hz pattern; screenshot-level check: zero distinguishable segments at any
    speed; thickness visibly grows with stroke speed up to ~half marker height; at rest trail
    fully decays ≤1s; marker and caret always crisp.
V10 Perf: 60s run with devtools performance — no per-frame allocations from the new code
    (steady heap), frame budget kept at display refresh.
V11 Refresh mid-pattern: page adopts device window; tape/hazards correct on first paint
    (no default-window flash).

═══════════════════════════════════════════════════════════════
7. EXECUTOR LOG (required)
═══════════════════════════════════════════════════════════════
Per section: files touched / decisions made where this spec allowed judgment (reference
speed constant, glow implementation choice, any unified constants) / every broken existing
path you had to repair under R3, with root cause / V1–V11 evidence, quoted / anything
observed out-of-scope worth flagging (flag, don't fix).
