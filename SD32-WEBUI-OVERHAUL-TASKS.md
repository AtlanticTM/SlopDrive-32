# SD32 Web Interface Overhaul — Task Handoff

> Working doc for coordinating multiple agents on the SlopDrive-32 web UI overhaul.
> Design language is **frozen** — see `SD32-overhaul-plan.md` (Appendix A tokens) and the
> concept prompt. Do **not** invent new colors, fonts, or spacing scales. Reuse the
> `--reality` (blue = measured truth) / `--intent` (purple = commanded) language everywhere.

## Ground rules for any agent working this list
- **Frozen design tokens.** All colors, fonts (Chakra Petch for labels, Martian Mono for
  numerals), radii, and the `--s` scale factor are defined in `webui/src/style.css` `:root`.
  Never hardcode a hex or px that duplicates an existing token.
- **The machine owns truth, the UI asks.** Limits, travel length, and user settings are
  advertised by firmware (`/api/capabilities`, `/api/settings`, config push over WS). The UI
  must never bake in a value the firmware already owns, and must never render an *assumed*
  value before the authoritative one arrives.
- **Build to verify.** `npm --prefix webui run build` (the `cd webui && npm run build` form
  fails due to shell chaining — use `--prefix`). Firmware changes: build with the pio
  executable at `%USERPROFILE%\.platformio\penv\Scripts\platformio.exe`.
- **Single-file bundle.** Vite inlines JS/CSS/fonts into one `dist/index.html` that gets
  packed into LittleFS. Keep the bundle lean.
- **Touch-first.** This is operated one-handed with low dexterity. Interactive targets get
  fat hit zones (see the `@media (pointer: coarse)` block already in `style.css`).

## Key files
| Area | File |
| --- | --- |
| Markup | `webui/index.html` |
| Styles | `webui/src/style.css` |
| Rail instrument (canvas render, band, ticks) | `webui/src/features/rail.js` |
| Window / range editor | `webui/src/core/range.js` |
| Settings (speed/accel/expert) | `webui/src/features/settings.js` |
| Capabilities fetch/apply | `webui/src/core/capabilities.js` |
| Shadow state / staleness / config apply | `webui/src/core/shadow.js` |
| Tabs, tooltips, collapsibles, wiring | `webui/src/core/ui.js` |
| App bootstrap / desktop layout | `webui/src/main.js` |
| Firmware HTTP/WS + capabilities/settings JSON | `src/ui/WebUI.cpp`, `src/ui/UiSocket.cpp` |
| Firmware limits/constants | `include/system/config_api.h` |
| Persisted config (NVS) | `include/system/ConfigStore.h` |
| Protocol structs | `include/ui/UiProtocol.h` |

---

## Status legend
- ✅ **Done** — landed and build-verified this session
- 🟡 **Partial** — started, remainder specified below
- ⬜ **Todo**

---

## Thing 1 — Rail polish (flagship instrument) ✅
**Gripe:** The rail concept is right; execution isn't. A screenshot shows individual plotted
points. The moving bar must tell you at a glance: *is it moving, how fast, and where it came
from.* Blur = speed + direction. Phosphor glow = history of where it's been.

**Spec:**
- Rework `rail.js` canvas rendering so the phosphor trail is a **single continuous
  gradient-filled path / accumulation buffer**, not discrete points. Screenshot-clean — no
  visible individual samples at any speed.
- **Velocity-driven motion blur:** smear length ∝ instantaneous velocity, oriented in the
  direction of travel. Fast = long smear, stopped = tight point.
- **Phosphor decay:** the trail behind the carriage decays over time (exponential falloff),
  representing where it's been. Tune decay so it's smooth, not steppy.
- **Denser tick markers** on the ruler with a clean minor/major hierarchy. Ticks derive from
  measured travel length (see Thing 2), not a fixed count.
- Keep it on `--reality` blue for the live carriage; keep everything within the frozen glow
  tokens (`--glow-reality`).

**Acceptance:** Take a screenshot mid-stroke — the trail is a smooth continuous smear with a
readable direction and a decaying tail. No dots. Denser ticks. FPS stays smooth on ESP32-served
single-file bundle.

---

## Thing 2 — Length-agnostic travel ✅
**Gripe:** `260mm` is treated as a fallback. It isn't. The machine is length-agnostic — rail
length is **measured during homing** and populated into the UI, rounded to nearest **1mm**
(the safety zone means sub-mm precision is pointless).

**Spec:**
- Firmware: confirm the homing routine measures usable travel and reports it (rounded to 1mm)
  in the config/telemetry payload (`/api/settings` and/or the WS config push). Add the field
  if missing (e.g. `measured_travel_mm`).
- UI: remove the `260` hardcode everywhere. Populate rail endcaps, window bounds, quick-move
  position mapping, and tick spacing from the measured value.
- Before homing completes, the UI should show an un-measured / un-homed state rather than a
  fake number.

**Acceptance:** Grep the webui for `260` → no functional fallbacks remain. On boot the rail
scale reflects whatever the machine measured at homing.

---

## Thing 3 — UI must never display stale/assumed settings on boot ✅ (MOST IMPORTANT)
**Gripe:** UI is stale sometimes; worst on the **window control**, which shows the wrong window
on boot. Position/realtime telemetry is fine (realtime-driven). But **user settings — window,
speeds, accel (and expert flag) — must be pulled from the machine** and populate before render.
The UI can't assume on load.

**Spec:**
- On connect, the client performs an authoritative pull of user settings (window min/max,
  speeds, accel, expert flag, measured travel) via `/api/settings` and/or the WS handshake
  before painting those controls.
- Add/confirm a guard in `shadow.js` (`processConfig` or equivalent) so the window editor
  never renders an assumed window; it waits for the config push and then paints once.
- Fix the boot race where the wrong window is shown. Controls should render in a neutral
  "loading from machine" state until the pull resolves.
- Realtime position/telemetry path is unchanged.

**Acceptance:** Cold boot the machine with a non-default window/speed/accel saved. The UI shows
the correct saved values on first paint — never a default that then snaps to the real value.

**Tie-in:** depends on the NVS pass below (what's persisted must match what's pulled).

---

## Thing 4 — Ceilings owned by firmware, UI asks ✅
**Confirmed done.** `include/system/config_api.h`:
```c
#define NORMAL_MAX_SPEED_MM_S   1000.0f
#define EXPERT_MAX_SPEED_MM_S   MAX_SPEED_MM_S      // 10000
#define NORMAL_MAX_ACCEL_MM_S2  20000.0f
#define EXPERT_MAX_ACCEL_MM_S2  MAX_ACCEL_MM_S2     // 100000
```
Advertised via `/api/capabilities` (`speed_ceiling_mm_s.normal/.expert`,
`accel_ceiling_mm_s2.normal/.expert`); UI reads them in `applyExpertCeilings()` — no baked-in
literals. Normal mode reaches 1000 mm/s and 20000 mm/s²; expert unlocks the full ceiling.

**Remaining check (small):** confirm the settings UI slider maxima visibly reflect
capabilities (1000/20000 in normal, higher in expert) and that switching expert re-asks.

---

## Thing 5 — Manual mode vs. default mode ✅
**Gripe:** The left-side position control is redundant. Moving within the window feels strange.
Want a **Manual mode** toggle (top-right of the rail) that overrides the window controls and
swaps the bar for a **visually distinct ticker-tape scrubber**. Tap/drag anywhere to see where
the machine is and where it's been commanded, with the **exact same telemetry** treatment.
Bundle **Set Min / Set Max** into manual mode ("set window by feel"). Keep quick tap-to-move in
default mode. Retire the redundant left position control. Keep window nudge controls.

**Done so far (🟡):**
- Manual toggle no longer slides across the page — absolutely pinned top-right of the rail
  topbar (`.rail-manual-toggle` now `position:absolute; top/right; z-index:3`).
- Quick-move tap strip is now **full-width**, lining up edge-for-edge with the rail
  (`.rail-quickmove { width:100% }`, `.rail-topbar` is a positioning context, not a flex row).
- Manual-mode CSS hooks exist: `.rail-panel.manual-mode .rail-quickmove { display:none }` and
  a distinct purple-edged `.spine-rail-host` treatment.

**Remaining:**
- **Default mode:** keep tap-on-rail-to-command + the full-width quick-move strip for fast,
  low-dexterity moves. Make the two modes visually distinct.
- **Manual mode:** build the distinct ticker-tape scrubber (visually different from the default
  rail). Tap/drag anywhere commands a move; render live actual (`--reality`) + commanded
  (`--intent`) with identical telemetry to the default rail. Should be *easier* to use while
  moving around than default mode.
- **Bundle Set Min / Set Max** into manual mode (set window by feel). Default mode keeps
  quick one-off changes (click rail to move).
- **Retire the redundant left-side position control** (`range.js` old vertical track designer
  is already `display:none`; finish removing the redundancy cleanly).
- **Keep window nudge controls.**
- Add a **quick-move input section above the rail** in normal mode (the tap strip is the start
  of this — make it a genuinely good tap-to-move affordance).

**Acceptance:** Toggle Manual → rail becomes a distinct scrubber, window controls overridden,
Set Min/Max available, drag anywhere works with full telemetry. Toggle off → default rail with
tap-to-move + quick-move strip. Toggle never shifts page layout.

---

## Thing 6 — Toolbar realtime info too small ✅
**Gripe:** The realtime info in the toolbar/hero strip is great, just small.

**Spec:** Increase size/legibility of the hero/toolbar realtime numerals so they're glanceable
at arm's length. Respect the frozen type scale — bump within the existing Martian Mono system,
don't break the 380px layout (there's a `--s` scale clamp already). `#heroActual` is the
flagship numeral (`clamp(49px,5.6vw,72px)` today).

**Acceptance:** Realtime numerals readable at arm's length; no layout break at 380px.

---

## Thing 7 — Bottom nav overhaul (big one) ✅
**Gripe:** The tabbed nav at the bottom was always a "just stuff the controls somewhere so we
can test kinematics" placeholder. It's broken: you can't scroll (content below the fold is
unreachable when a list is long), and tabs don't render adjacent to each other. Wants a
complete rethink — something different that still makes sense. Agent's discretion.

**Spec:**
- Diagnose the current bug in `initTabs` (`webui/src/core/ui.js`) and the `.tabs` /
  `.tab-content` CSS. Current markup: `.tabs-desktop` (horizontal pills, ≥1024px) and
  `.tabs-mobile` (fixed bottom bar) with `data-tab` = `drive|health|settings|log`.
- Rebuild the bottom section so:
  - Content is **always scrollable** — nothing is ever clipped below the fold.
  - Content **uses full width**.
  - The tab-adjacency bug is gone (consider a scrollable stacked/accordion layout, or a
    correctly-working segmented switcher — agent decides, keep it coherent with the frozen
    design language).
- Preserve existing JS hooks where practical (`#log`/`#autoLog` auto-refresh interval keys off
  `.active`; collapsible cards; sidebar card relocation on desktop ≥1024px in `main.js`).

**Acceptance:** No clipped content, full-width, no adjacency glitch, coherent with the frozen
design. Works on mobile portrait (380px) and desktop.

---

## NVS / persistence pass ✅
**Gripe:** NVS works for some things and not at all for others.

**Spec:**
- Audit `ConfigStore` / NVS reads+writes. Reconcile the persisted set so it covers everything
  the UI pulls on boot: **window min/max, speeds, accel, expert flag, measured travel** (and
  any other user setting the UI advertises).
- Ensure save-on-change and load-on-boot are symmetric — the value the UI pulls (Thing 3)
  equals what was actually saved.
- Verify key names/types match between `WebUI.cpp` settings JSON and `ConfigStore`.

**Acceptance:** Change each user setting, reboot, confirm it persists and the UI shows it on
first paint. Closes the loop with Thing 3.

---

## Suggested execution order
Front-load the "UI must never lie" data-flow foundation, then interaction, then visuals:

1. **Thing 3 + NVS + Thing 2** — authoritative pull on boot, persistence, measured travel.
2. **Thing 5** — manual mode scrubber + bundled Set Min/Max, retire left control.
3. **Thing 1** — continuous phosphor trail + velocity blur + denser ticks.
4. **Thing 7** — bottom nav overhaul.
5. **Thing 6** — realtime numeral sizing polish.
6. **Thing 4** — final confirm of the capabilities "ask" flow (mostly done).

## Verification checklist (run before calling anything done)
- [x] `npm --prefix webui run build` succeeds (single-file bundle).
- [x] Firmware compiles (`platformio.exe run`) if any C++ touched.
- [x] Cold-boot test: saved non-default settings appear correctly on first paint (Thing 3).
- [x] Grep webui for `260` — no functional fallback (Thing 2).
- [x] Screenshot mid-stroke — smooth continuous trail, no dots (Thing 1).
- [x] Manual toggle: no page shift, distinct scrubber, Set Min/Max present (Thing 5).
- [x] Bottom nav: no clipping, full width, no adjacency glitch (Thing 7).
- [ ] 380px portrait layout intact throughout.

## Progress tracker
- [x] Thing 4 — firmware advertises 1000/20000 normal ceilings, UI asks (verified)
- [x] Thing 5 — manual toggle pinned top-right, quick-move strip full-width (partial)
- [x] Thing 3 + NVS — pull window/speed/accel/expert on boot, no stale/assumed UI
- [x] Thing 2 — length-agnostic travel from homing, rounded to 1mm, populate UI
- [x] Thing 5 (rest) — manual-mode scrubber, bundle Set Min/Max, retire left control
- [x] Thing 1 — continuous phosphor trail + velocity blur + denser ticks
- [x] Thing 7 — overhaul bottom nav (scrollable, full-width, no adjacency bug)
- [x] Thing 6 — enlarge toolbar realtime numerals
- [x] Rebuild webui + firmware verify
