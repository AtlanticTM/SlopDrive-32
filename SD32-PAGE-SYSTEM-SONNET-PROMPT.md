# SD-32 — Page System: Instrument Cluster, Pattern Registry, Elastic Rack
## Executor: Claude Sonnet 5 · full web UI source in context · device LIVE at 192.168.1.229
## APPROVED MOCK: sd32-preview-r4.html (attached) is the visual ground truth — where this text and the mock disagree, the mock wins.
## Approved interactive mocks exist for every zone; this document is their complete spec. Implement EXACTLY this.

═══════════════════════════════════════════════════════════════
0. PRIME RULES
═══════════════════════════════════════════════════════════════
R1. DESIGN LANGUAGE IS FROZEN. Use the existing CSS custom properties and conventions already in
    the bundle (--intent purple, --reality blue, --warn, --bad, --screen recessed, nested-frame
    panels with corner brackets, Chakra Petch labels, Martian Mono numerals with .vv fixed boxes,
    light chip bg = live value). No new colors, no new fonts. Glow only on live data and alarms.
    Red appears ONLY in hazard/danger contexts.
R2. GROUND TRUTH DOCTRINE. Every displayed value has exactly ONE writer, sourced from the existing
    telemetry/status/shadow paths. Nothing optimistic: pattern running state, active transport,
    and all meters render CONFIRMED device state. The wave scope draws REAL telemetry (told
    channel), never a synthetic approximation.
R3. NOTHING SHIPS UNVERIFIED. Every control you build or touch is proven live (payload sent +
    device state change quoted in the executor log). Section 7's V-matrix is mandatory. A control
    that renders but drives nothing is a task failure.
R4. USE EXISTING PATHS. No new endpoints, no protocol changes. Pattern start/stop, settings
    writes, transport switching, log fetch: all through the existing senders with their shadow/
    echo machinery. If an existing path is broken, fix and log it (do not route around).
R5. SCOPE: index.html markup/CSS, pattern.js, settings.js, capabilities.js, main.js wiring,
    and ONE small documented firmware-tolerant behavior (§2.2 fallback). Do not touch rail.js
    internals (the rail/tape assembly is done), link.js, telebuf.js, shadow.js internals.

═══════════════════════════════════════════════════════════════
1. LAYOUT DOCTRINE — "ELASTIC RACK" (applies to every tab)
═══════════════════════════════════════════════════════════════
Panels stretch; nothing wanders.
1.1 Content width is FLUID up to a max-width of 1280px (centered beyond that). Panels elongate to
    use available width — meters, scope, log, and rail get longer, not rearranged.
1.2 EXACTLY ONE BREAKPOINT (~680px): above it, declared 2-col clusters; below it, single-column
    stack in a FIXED rack order (the DOM order — declare it once, §2/§3/§4 give the order per
    tab). That stacking is the page's ENTIRE responsive vocabulary. No other media queries that
    move, hide, or reorder content. (Existing tabs-desktop/tabs-mobile nav machinery stays as-is.)
1.3 MODULE INTERNALS ARE IMMUTABLE at every width: the pattern glyph grid is ALWAYS 4 columns;
    field pairs stay 2-up (they may compress, never stack) EXCEPT below the breakpoint where a
    pair may go 1-col only if it already does in the current build; meter anatomy and ordering
    never changes; the link strip keeps its element order and wraps as whole units only.
    An element's neighbors are the same on a phone and a 4K monitor.
1.4 Density via scale only: if narrow needs breathing room, adjust the existing --s token scope,
    never the arrangement.
1.5 Tabs remain the capacity valve. Drive / Settings / Log keep their current contents' homes;
    nothing migrates between tabs in this task.

═══════════════════════════════════════════════════════════════
1.6 ADDENDA FROM MOCK r2–r4 (all visible in the attached mock, authoritative):
    a) SINGLE COLUMN: header, hero, rail, tab bar, main grid, and footer inner content all share
       width min(80vw, 96%), centered — nothing narrower than the rail. Hairline rule between the
       hero block and the rail panel. Mobile: full width, normal padding.
    b) DRIVE IS NOT A TAB on desktop: it is always present. SETTINGS and LOG float RIGHT in the
       tab bar with a split-pane pictogram (frame, right half filled; fill brightens when active)
       and tooltip "Opens as a panel beside Drive · click again to close". Clicking opens that
       view as a STICKY side pane (position:sticky, max-height:calc(100vh−28px), own thin dark
       scrollbar) beside Drive; the Drive cluster reflows to one column while a pane is open.
       Below 760px: classic three-tab behavior returns, DRIVE tab reappears, pane becomes static.
       First module in the pane aligns flush with the first Drive module (mind the 4px outline
       offset — both columns get identical headroom).
    c) CONTRAST + SCALE: the mock r2 token values ARE the spec (text tiers lifted, all dimensions
       ~12% larger in real values — no transforms).
    d) HEADER ACTIVITY GRID: the decorative barcode is replaced by a small netdata-style heatmap
       canvas (14 cols × 3 rows, 4px cells): rows = |velocity| / bus current / link activity,
       columns = time (~220ms buckets, shifting left), cell alpha = intensity, reality-blue only.
       Sourced from the SAME telemetry values the hero and meters use — real data, not decoration.
       Tooltip explains the rows.
    e) GLYPHS: reticle crosshair glyph beside the "actual" hero label (reality blue, subtle glow).
       Small semantic glyphs of this kind are welcome where they carry meaning; never decorative
       noise.
    f) TOOLTIPS: CSS-only [data-tip] system from the mock, applied to every non-obvious element
       (the mock contains the full inventory — replicate it, adding any new firmware-truth items).
    g) METER COMPONENT: the meter is a reusable class + JS factory (label, min, max, decimals,
       hazard zones, optional peak-hold ratchet, tooltip) in its own module — the POWER card is
       five instantiations. This component will be reused for future Modbus register banks; build
       it as a public, documented surface, not an inline detail.
    h) FOOTER: the LINK strip is a full-width page footer (inner content on the shared column):
       RSSI meter + ch/bssid/disc chips + fw build chip (from /api/capabilities fw_version) +
       ui bundle build chip. The fw chip is the OTA verification surface — it must always reflect
       the device-reported version, never a baked-in string.

═══
2. DRIVE TAB — the instrument cluster (below the rail panel)
═══════════════════════════════════════════════════════════════
Rack order: [rail panel — untouched] → cluster row (PATTERN | POWER) → LINK strip.
Panels carry index numerals + ▸ titles in card heads: 01 PATTERN, 02 POWER, 03 LINK (Martian Mono
ghost index, letterspaced Chakra title — per mock).

2.1 PATTERN (01) — replaces the current pattern card body:
  a) GLYPH GRID replaces the seg control: 4-column grid of tiles, each = small SVG waveform glyph
     + name. Selected tile = --intent border + glow + purple glyph/name; unselected = ink. Tiles
     are buttons wired to the EXISTING pattern-select path (same values the seg sent).
  b) PATTERN REGISTRY (the extendability contract): the grid renders from a runtime list, never a
     hardcoded DOM. Source: capabilities response field `patterns` (array of {id, name, glyph?}
     where glyph is an SVG path string for a 58×20 viewBox). IF the field is absent (current
     firmware), fall back to the existing built-in seven with the glyph set from the approved
     mock (Simple sine / Tease asymmetric ramps / Robo square / Half'n'Half alternating amp /
     Deeper growing amp / Stop'n'Go bursts / Insist dense constant). Any registry entry WITHOUT a
     glyph renders the dashed-placeholder glyph (dashed segments, .55 opacity tile border-style
     dashed). The grid must handle any count (wraps to new 4-wide rows). This decouples the
     future generator-library swap from all UI work.
  c) Sliders speed/depth/stroke/sensation: keep existing inputs/ids/wiring; restyle to the
     language (thin 2px track, rectangular thumb, label row with recessed Martian Mono value
     chip + ghost unit). 2×2 pair grid.
  d) WAVE SCOPE moves INTO this card as a recessed screen strip (reuse #waveScopeCanvas + its
     existing telemetry-fed drawing — R2: if the current implementation still draws synthetic
     shapes, it is telemetry-fed (told channel trailing window) or removed; log which you found).
     Flat line + no glow at standby; live trace only when data flows.
  e) START/STOP: full-width transport button, letterspaced. Idle: neutral border "▶ START
     PATTERN". Running: amber border/glow "■ STOP PATTERN" (amber = attention; NOT red). State
     from confirmed sources only (existing flags path).
  f) Card-head right slot: mono status sub-label ("standby" / "running · <name>" / the existing
     yield/paused notes route here instead of the old patNote hint block).

2.2 POWER (02) — replaces the loadCard stat rows (keep ids so telemetry writers keep working;
    restyle around them):
  a) Meter bank rows in fixed order: BUS V, BUS A, BUS W, DIE °C, PEAK A. Each row = label
     (letterspaced ghost) + Martian Mono value (blue, subtle glow) + recessed meter track (7-8px)
     with: blue gradient fill, HAZARD ZONE hatch segments (same 135° red hatch language as the
     rail keep-outs) at the dangerous end — voltage hazard at the LOW end (<24V warn, <22V bad —
     match the Wave-1.5 threshold fix), current/power/temp hazard at the high end (derive
     thresholds from existing capabilities/constants; log the numbers used) — and a PEAK-HOLD
     caret (1.5px amber tick) on BUS A and BUS W that ratchets to max observed and holds
     (matches existing peak-since-boot semantics; Reset peaks button clears it via the existing
     reset_peaks path).
  b) Value swells amber (existing .w1-style treatment) while inside a hazard zone.
  c) Shared SPARKLINE screen at card bottom: 60s bus-power history, thin blue trace on recessed
     screen, "load · 60s" ghost label. Ring buffer, no per-frame allocation, drawn at a modest
     cadence (2-4Hz is plenty — it's history, not the rail).
  d) healthCard's placeholder sentence: fold its content into this card's info tooltip; remove
     the sentence-in-a-box card entirely (capabilities.js builds these — edit there).

2.3 LINK (03) — slim horizontal strip panel (replaces the current linkCard placement in #health):
    RSSI mono value + horizontal recessed meter with warn/bad threshold ticks at the correct dBm
    positions (scale -90..-30; reuse existing RSSI quality thresholds) + "dBm" ghost unit, then
    data chips: ch, bssid (short form), ip, disc, rec — recessed chip styling. All values from
    the existing 0x02/status writers (keep ids). Wraps as whole units below the breakpoint.

═══════════════════════════════════════════════════════════════
3. SETTINGS TAB — same language, continued indexing
═══════════════════════════════════════════════════════════════
Keep every card's content, wiring, ids, and ORDER. This is a re-skin + head treatment, not a
reorganization:
3.1 Card heads gain continuing index numerals (04, 05, … in DOM order) + ▸ letterspaced titles.
    Info tooltips unchanged.
3.2 Connection card's transport seg becomes mono PORT chips (per mock): rectangular, Martian
    Mono, letterspaced; ACTIVE port = --reality blue border/glow/tint (it is reality — the
    device's confirmed transport, from the existing status source, NOT the clicked one until
    confirmed; pending click may show the existing pending treatment). Same click wiring.
3.3 All sliders/fields get the §2.1c field treatment (thin track, rect thumb, recessed value
    chip + ghost unit). All number inputs + toggles restyled to language (recessed inputs,
    rectangular switches) — visual only, no wiring changes.
3.4 Danger-adjacent cards (expert mode banner etc.) keep their existing amber conventions.

═══════════════════════════════════════════════════════════════
4. LOG TAB — terminal treatment
═══════════════════════════════════════════════════════════════
4.1 Log card: 07 LOG head; the log box becomes a recessed TERMINAL screen — --screen bg, inset
    shadow, Martian Mono 10-11px, phosphor-blue-tinted text (#7C97B8-family from the mock),
    preserved autoscroll/refresh/auto behavior and ids. Blinking block cursor at tail (CSS
    animation; respects prefers-reduced-motion).
4.2 Anomaly counters move INTO the card head as dot+count chips (dot lights amber when count>0,
    existing counts source; keep the detailed anomaly box below if it exists, collapsed by
    default). Serial-mode chip stays in the head, restyled.
4.3 Clients card: 08 CLIENTS, same treatment (recessed rows, mono values). Content unchanged.

═══════════════════════════════════════════════════════════════
5. IMPLEMENTATION CONSTRAINTS
═══════════════════════════════════════════════════════════════
- capabilities.js builds several of these cards via innerHTML: apply the new markup THERE, not by
  fighting it afterward. Keep every existing element id that any writer targets (grep each id for
  writers before renaming anything; renames are forbidden unless you also move the writer, logged).
- No per-frame allocations in any canvas loop; DPR handling on scope + sparkline; canvases redraw
  on resize (elastic width) via the existing resize path or one shared observer.
- prefers-reduced-motion: no cursor blink, no value swell animation, static sparkline refresh.
- Icons: existing icon system (data-ico) — do not introduce a new icon set.
- Keep gzip weight sane: glyph paths inline in JS registry, no image assets.

═══════════════════════════════════════════════════════════════
6. WHAT NOT TO DO
═══════════════════════════════════════════════════════════════
- Do not touch the rail/tape assembly, hero strip, or transport bar (done, approved).
- Do not add fluid multi-breakpoint behavior — §1 is the entire responsive spec.
- Do not reorder any settings cards or move content between tabs.
- Do not invent pattern metadata endpoints — §2.2b's capabilities field + fallback is the whole
  contract (firmware side ships later; UI must be complete without it).
- Do not replace working wiring while restyling: every existing listener keeps functioning.

═══════════════════════════════════════════════════════════════
7. MANDATORY LIVE VERIFICATION (quote evidence per row)
═══════════════════════════════════════════════════════════════
V1  Each glyph tile starts its pattern on the device (quote one payload + confirmed running
    state for two different patterns). Fallback registry renders when capabilities lacks
    `patterns` (current firmware = the live test).
V2  START/STOP button reflects CONFIRMED state only: start via curl with UI open → button flips
    without a click; stop while un-homed shows the rejection path.
V3  Scope: flat at standby, live trace during a pattern, and the trace visibly corresponds to
    the rail's motion (same telemetry).
V4  Meters: values match /api/status within rounding for all five rows; 36V renders nominal
    (NOT red/amber); peak caret holds after a stroke burst; Reset peaks clears it (quote
    before/after).
V5  Transport chips: active chip == device's confirmed transport; switching transports updates
    only after confirmation (quote the sequence).
V6  Log terminal: streams/refreshes as before; anomaly head-chips match the counts source.
V7  Elastic rack: screenshots at 1400px, 900px, 680px, 390px — panels elongate, module internals
    identical, single stack below breakpoint in declared rack order, no element changes
    neighbors. No horizontal scroll at 390px.
V8  Regression: rail tap, tape scrub, band drag, set-min/max all still drive the device (one
    payload each — these are adjacent files; prove you broke nothing).
V9  Perf: 60s run, no per-frame allocations from new code; canvases resize cleanly on window
    drag.

═══════════════════════════════════════════════════════════════
8. EXECUTOR LOG (required)
═══════════════════════════════════════════════════════════════
Per section: files touched / every existing id preserved vs moved (with writer list) / hazard
threshold numbers used and their source / scope R2 finding (was it synthetic?) / rack order as
declared / V1–V9 evidence quoted / out-of-scope observations flagged, not fixed.
