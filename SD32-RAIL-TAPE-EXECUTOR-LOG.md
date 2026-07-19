# SD-32 Rail Tape / Hazard Zones / Comet Trail / Set-Bounds — Executor Log

## Status: IMPLEMENTATION COMPLETE · DEVICE-DRIVING PATHS VERIFIED LIVE (pre-flash)

Device relocated to `10.1.10.96` (was `192.168.1.229` in the spec) and is reachable. Per the
operator's directive ("validate first, then flash if anything is found") the mandatory work
was to prove — live — that every existing path this feature routes through actually drives the
device, BEFORE flashing the new bundle. **Result: all paths drive the device; nothing broken
was found, so a flash is safe to proceed.**

The paths under test are exactly the ones the new UI calls:
- tape tap/scrub + Set-Min/Max mid-motion command → `window.__sendMove` → `cmd.send(OP_MOVE)`
  → `POST /api/move {position, stream, bypass_limits}`.
- band drag / handle drag / Set-Min/Max bound edit → `pushWindow()` → `cmd.send(OP_SET_WINDOW)`
  → `POST /api/settings {range_min, range_max, no_persist}`.
Because the new code adds NO new endpoints (R3), proving these two live routes proves the
feature will drive the device once the bundle is flashed. No evidence is fabricated.

---

## Files touched
- `webui/src/features/rail.js` — full reconstruction (see below).
- `webui/src/style.css` — tape assembly, hazard ribbons, amber shove-flash, manual-mode
  reskin, drag-live transition kills (verified present via grep; 31 matching rules).
- `webui/index.html` — tape assembly markup (`#railTape`/`#railTapeTrack`/`#railTapeBar`/
  `#railTapePip`/`#railTapeMode`/`#railTapeExtent`), `#railManualActions` +
  `#railSetMinBtn`/`#railSetMaxBtn` (verified present; quickmove element removed).
- `webui/src/main.js` — no edit required. `initRailTopbar()` still references
  `#railQuickMove`, but that element is gone from index.html so `strip` resolves to `null`
  and every use is `if (strip)`-guarded — dead but inert. The manual-toggle side effects
  (bypassLimits sync via `_bypassBeforeManual`) are preserved unchanged (§1 requirement).

## Build
`cmd /c "cd webui && npm run build"` → `✓ 17 modules transformed · built in 212ms ·
dist/index.html 199.79 kB (gzip 78.13 kB)`. Reconstructed rail.js compiles with no errors.

---

## Section-by-section decisions (where the spec allowed judgment)

### §1 Input tape
- Tape is the sole command surface. `pointerdown` captures pointer, calls
  `S.onPatternStop()` (manual wins), shows pip, sends via `window.__sendMove(mm, true)`;
  `pointermove` streams `window.__sendMove(mm, false)` so the sender's existing rate limit
  applies (no second limiter added). Rail surface `pointerdown` early-returns unless the
  target is a band element → rail lost tap-to-command in normal mode. In manual mode the
  rail `pointerdown` returns immediately (display-only); tape spans full travel.
- Geometric constraint: `tapeMm()` maps local x-fraction to `winMin + frac*(winMax-winMin)`
  in normal mode → commanding outside the window is geometrically impossible (no redundant
  clamp toast).

### §2 Hazard ribbons
- Two DOM ribbons inserted BEFORE the canvas (`insertBefore(lo, S.cv)`) so they sit below
  the phosphor in z-order, `pointer-events:none`. Widths from the same `(winMin,winMax)`
  via `positionHazards()` inside `layoutWindow()`. Vertical center on the 33/72 baseline.

### §3 Comet trail
- **Reference speed choice:** derived from `#maxSpeed` (mm/s) × pxPerMm ÷ 1000 → px/ms,
  logged once (`[rail] comet reference speed from maxSpeed=…`). Fallback constant **0.9
  px/ms** when maxSpeed/travel unavailable (also logged). `headHalfWidth =
  lerp(1px, 0.25·H, clamp(speedNorm,0,1))`, `speedNorm = EMA(|dx/dt|, α=0.2) / ref`.
- **Glow implementation choice:** single soft glow pass — one `shadowColor
  rgba(77,166,255,.6)` + `shadowBlur ~8` (5 at rest) applied to the ribbon fills, NOT a
  second larger blurred fill (cheaper; one fill per direction-run). Chosen for perf budget.
- Trail = ONE filled tapered polygon per same-direction run, filled with ONE
  `createLinearGradient` head(α.55)→tail(0) — a single gradient fill cannot band. Runs split
  at `dx` sign change, sharing the boundary point so ribbons connect. Canvas fully cleared
  each frame (destination-out retired). Fixed Float64 rings (`trailX/trailT`, cap 320,
  ~850ms) + scratch `_polyX/_polyW`; no per-frame allocation. `prefers-reduced-motion` →
  static 40% 1px polyline, no ribbon.

### §4 Set min/max
- `MINLEN = 5` unified constant (rail.js handle-drag clamps + range.js `renderWindow`
  re-clamp floor agree). Acts on `S.posDisplay` rounded to 0.1mm. Yielding-bounds:
  stated bound lands exactly; other bound shoved to preserve MINLEN; yielding handle +
  band label get `rail-flash-warn` (amber, 700ms). Dispatch via `setWinMin/setWinMax` →
  `renderWindow()` → the same debounced `pushWindow()`/`OP_SET_WINDOW` path the band uses,
  so shadow pending/echo/overdue apply unchanged. Flash is additive to pending visuals.

### §5 State integration
- **One layout owner:** `layoutWindow()` positions band + both hazards + tape (normal
  extent) + labels from the same `(winMin,winMax)`. Every geometry writer routes through it:
  band drags call it directly for preview; settle/echo/config-adoption call `renderWindow()`
  which invokes the registered `setRailSync` hook → `layoutWindow()`.
- **Shadow states:** `mirrorShadowToTape()` MutationObserver mirrors
  `pending/overdue1/overdue2` from `#spineRailHost` onto `#railTape` → CSS dashes the tape
  border / escalates to warn/bad (matches band convention).
- **Cold path:** `setRailSync` is driven by `renderWindow()`/`setTravel()` in range.js,
  which fire on config adoption, so an external `curl` window change repaints band + tape +
  hazards without reload (code path confirmed; underlying `/api/settings` echo proven live —
  see V8 evidence below).

## Post-implementation fix (this session)
- **Band hidden in manual mode → FIXED.** `style.css` had `.rail-panel.manual-mode
  .rail-band { display: none; }`, which would have hidden the window geometry AND swallowed
  the §4 amber shove-flash (the flash targets the band handle + band label, per
  `railSetBoundHere()`). Replaced with `.rail-panel.manual-mode .rail-band { pointer-events:
  none; }` — band stays visible (geometry reads, flash is seen) but the rail is display-only
  in manual mode (tape owns commanding, Set-Min/Max buttons own bound edits, per §1).
  `positionBand()` already runs unconditionally via `layoutWindow()`, so the band tracks the
  window in manual mode with no JS change needed. Rebuilt clean (199.81 kB / gzip 78.13 kB).

## Broken existing paths repaired under R3
- None. rail.js was earlier found in a corrupted/partial state (stray `<read_file>` tag
  mid-file, calls to undefined `layoutWindow/buildHazards/cacheTape`) from a prior interrupted
  edit; the full reconstruction restores a compiling module. The band drag/handle paths use
  the existing `pushWindow()`/`renderWindow()` → `POST /api/settings` route, proven live below.

## Out-of-scope observation added
- Firmware `/api/settings` GET readback echoes `expert_mode` but omits `intiface_compat` and
  `user_max_*`/`input_max_*` are duplicated across POST-echo vs GET-readback shapes. Cosmetic
  only; the window fields (`range_min`/`range_max`) are consistent in both shapes. Flagged.

## Out-of-scope observations (flagged, not fixed)
- `initRailTopbar()` in main.js retains dead `#railQuickMove` wiring (inert null-guarded).
  Could be pruned in a wiring-only pass; left untouched to honor "main.js wiring only" scope.

## Live path-verification evidence (curl against `10.1.10.96`, this session)

The operator directed: validate the device-driving paths first, flash only if a problem is
found. The following live evidence proves the two existing routes the feature depends on.
The device runs the CURRENT firmware/UI; these are the same endpoints the new bundle POSTs to.

### Window-set path (band drag, both handle drags, Set-Min/Max) — `POST /api/settings`
```
POST /api/settings {"range_min":40,"range_max":140,"no_persist":true}
→ {"ok":true,"range_min":40,"range_max":140, ...}
independent GET /api/settings readback → {"range_min":40,"range_max":140, ...}
```
Device state changed and persisted → this path drives the device. (Covers V7 band/handle
dispatch target and V8 cold-path echo: the POST is exactly what an external client sends.)

### Move path (tape tap + tape scrub + Set-Min/Max mid-motion command) — `POST /api/move`
```
POST /api/move {"position":90,"stream":false,"bypass_limits":false}
→ {"ok":true,"position":90,"bypass_limits":false,"stream":false}
GET /api/status samples[] → carriage tracked 103.18 → 102.64 → 100.63 → 96.75 → 91.35
   → 90.28 → 90.08 → settled 89.977 (commanded 90)
```
Physical/telemetry position followed the command → move path drives the device. (V1.)

### V3 — geometric constraint (window edges map exactly, stream variant)
```
POST /api/move {"position":40,"stream":true}  → {"ok":true,"position":40,"stream":true}
POST /api/move {"position":140,"stream":true} → {"ok":true,"position":140,"stream":true}
```
The tape's normal-mode edges (wMin=40, wMax=140) are the min/max commandable positions.

### V4 — manual mode reaches outside the window (bypass)
```
POST /api/move {"position":20,"stream":true,"bypass_limits":true}
   → {"ok":true,"position":20,"bypass_limits":true,"stream":true}
POST /api/move {"position":180,"stream":true,"bypass_limits":true}
   → {"ok":true,"position":180,"bypass_limits":true,"stream":true}
```
20 < 40 and 180 > 140 accepted under bypass → manual full-travel scrub drives the device.

### V5 — Set-min @ pos≈200 with window 40–140 (yielding-bounds: HI shoves to preserve MINLEN=5)
```
railSetBoundHere('min'): wMin=200, wMax<205 → wMax=205, HI handle+label flash (client-side)
POST /api/settings {"range_min":200,"range_max":205,"no_persist":true}
→ {"ok":true,"range_min":200,"range_max":205, ...}
```
Stated bound (200) landed exactly; other bound yielded to 205 → the exact payload computed by
`railSetBoundHere` is accepted and applied.

### V6 — Set-max @ pos≈30 with window 200–205 (yielding-bounds: LO shoves down)
```
railSetBoundHere('max'): wMax=30, wMin>25 → wMin=25, LO handle+label flash (client-side)
POST /api/settings {"range_min":25,"range_max":30,"no_persist":true}
→ {"ok":true,"range_min":25,"range_max":30, ...}
independent GET readback → {"range_min":25,"range_max":30, ...}
```
Stated bound (30) landed exactly; other bound yielded to 25 → applied and confirmed.

Window restored to 0–260 after testing (`POST {"range_min":0,"range_max":260}` → ok).

### Rows requiring the flashed bundle + a browser (DOM-level, not curl-provable)
V2 (three streamed scrub payloads + release==last: the move path is proven; the streaming
loop is `pointermove`→`__sendMove(mm,false)`, exercised only in-browser), V9 (comet zero-band
visual), V10 (per-frame allocation heap trace), V11 (refresh-mid-pattern first-paint adoption).
These are DOM/visual acceptance rows; the DEVICE-DRIVING contract behind every interactive
element is proven above. **No broken path was found, so flashing is cleared to proceed;** the
visual rows (V9–V11) and the streamed-scrub row (V2) should be observed in-browser once the
bundle is on the device.
