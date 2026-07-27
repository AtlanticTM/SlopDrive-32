<script>
  /**
   * RailWidget.svelte — the flagship instrument: travel rail + stroke-window
   * band + input tape + comet trail + hero numerals.
   *
   * A FAITHFUL PORT of the pre-refactor rail (tag `webui-prerefactor`,
   * `src/features/rail.js` + the hero-strip markup in `index.html`), rebound
   * from hardcoded field names/channel ids to ROLES (roles.js) so it draws on
   * any conforming SlopSync hub, not just this one. `pos`/`vel` are optional —
   * a machine with no telemetry roles still gets a correct window editor, just
   * without the comet/numerals.
   *
   * ── What did NOT survive the port, and why ─────────────────────────────────
   *
   * 1. THE INPUT TAPE COMMANDS A MOVE ONLY WHEN THE CATALOG SAYS IT CAN.
   *    RFC-032 gave the wire a role for exactly this — `command.position` on
   *    an INTENT schema field is a SETPOINT, not a verb, so (unlike an
   *    `action.*` field) it renders as a positional control rather than a
   *    button. When the hero claim resolves `fields.move`, the tape is live:
   *    tap or drag sends `sendIntent(move.channelId, { [move.key]: value })`
   *    directly (NOT writeSetting — this is not an RFC-009 setting, there is
   *    no settingKey/settingChannel for it). When a machine has not
   *    annotated its move channel this way, the tape correctly declines —
   *    spans the window, disabled, with a reason — exactly like a
   *    Field.svelte control the session cannot write. That decline path is
   *    the honest fallback per hard rule 3, not a placeholder waiting on a
   *    role that does not exist.
   *
   * 2. "COMMANDED" AND "LAG" hero numerals are back. RFC-032 registered
   *    `telemetry.target` (the machine's live setpoint, as opposed to
   *    `telemetry.position`'s measured truth) precisely to unblock this.
   *    Lag is still not its own role — it is target - position, computed
   *    client-side in HeroNumerals — see that file's header.
   *
   * 3. Manual mode (tape spans full travel, Set-Min/Max-here buttons with
   *    yielding-bounds) depended on a client-side "bypass limits" toggle that
   *    has no role either. Dropped rather than half-built.
   *
   * Everything else is a real port: the ruler, the hazard keep-out ribbons,
   * the draggable/resizable band, the tapered-gradient comet trail, and the
   * hero numerals' exact typographic treatment (huge clamp() numeral, reticle
   * glyph, reality glow) all come straight off the original, generalized to
   * work off whatever `[lo, hi]` and unit the catalog reports instead of an
   * assumed 0-999mm rail.
   */
  import { machine, getSession } from '../../model/machine.svelte.js';
  import { isFieldEnabled } from '../../model/settings.js';
  import { writeSetting, displayValue, statusOf, STATUS } from '../../model/shadow.svelte.js';
  import { formatValue, unitOf } from '../../model/format.js';
  import { ACCENT, ac } from '../../model/theme.js';
  import { createTelebuf, createTrail } from './telebuf.js';
  import HeroNumerals from './HeroNumerals.svelte';

  let { fields } = $props();
  // Read through the prop rather than destructuring once — heroes.js hands us
  // a fresh `fields` object whenever the catalog rebuilds, and a plain
  // destructure would freeze on the first machine we ever saw.
  const min = $derived(fields.min);
  const max = $derived(fields.max);
  const pos = $derived(fields.pos);
  const vel = $derived(fields.vel);
  // RFC-032 optional claims. `move` is an INTENT schema field (isIntentField:
  // true from settings.js pass 2) — it has channelId/key/access but no
  // settingKey/writeChannel, so it is written via sendIntent, never
  // writeSetting. `target` is an ordinary STATE field like pos/vel.
  const move = $derived(fields.move);
  const target = $derived(fields.target);

  function sampleOf(f) { return f ? machine.samples[f.channelId] : undefined; }

  /** Mirrors Field.svelte's three independent gates: mask, link, access tier. */
  function enabledOf(f) {
    if (!f || f.readOnly) return false;
    if (!isFieldEnabled(f, sampleOf(f))) return false;
    if (machine.link.phase !== 'live') return false;
    const e = machine.catalog.entries.find((x) => x.id === f.writeChannel);
    if (!e) return false;
    return (machine.link.roles | 0) >= (e.access | 0);
  }

  const minVal = $derived(displayValue(min, sampleOf(min)));
  const maxVal = $derived(displayValue(max, sampleOf(max)));

  const minEnabled = $derived(enabledOf(min));
  const maxEnabled = $derived(enabledOf(max));
  const bandEnabled = $derived(minEnabled && maxEnabled);

  /**
   * May THIS session command a move? `move` has no enabled_mask (it is not a
   * RFC-009 setting) and no writeChannel/settingKey — its own `access` is the
   * whole gate, checked the same way SafetyBar checks an option's access:
   * against the catalog's own data, so this can never disagree with what the
   * hub will actually accept. Reads machine.link.roles/phase explicitly
   * because the session object lives outside Svelte's reactivity.
   */
  const moveEnabled = $derived.by(() => {
    void machine.link.roles; void machine.link.phase;
    if (!move) return false;
    if (machine.link.phase !== 'live') return false;
    const session = getSession();
    return !!session && session.isLive && session.canUse(move.channelId, move.key, 0);
  });
  const moveReason = $derived.by(() => {
    if (!move) return '';
    if (machine.link.phase !== 'live') return 'no hub link';
    if (!moveEnabled) return 'this session is not authorised to command motion';
    return '';
  });

  function worstStatus(a, b) {
    const order = [STATUS.fault, STATUS.overdue, STATUS.pending, STATUS.confirmed];
    for (const s of order) if (a === s || b === s) return s;
    return STATUS.confirmed;
  }

  // Rail extent per the contract: the descriptors' own bounds, not the current
  // window — the rail must show the whole travel even when the window is small.
  const lo = $derived(min.min ?? 0);
  const hi = $derived(max.max ?? (lo + 1));
  const span = $derived(Math.max(hi - lo, 1e-9));

  function pct(v) {
    if (v == null || !isFinite(v)) return null;
    return Math.min(1, Math.max(0, (v - lo) / span));
  }

  const minPct = $derived(pct(minVal));
  const maxPct = $derived(pct(maxVal));
  const haveWindow = $derived(minPct != null && maxPct != null);

  function clamp(v, a, b) {
    const lo2 = Math.min(a, b), hi2 = Math.max(a, b);
    return Math.min(hi2, Math.max(lo2, v));
  }

  function snap(v, field) {
    let out = v;
    if (field.step) out = Math.round(out / field.step) * field.step;
    if (field.min != null) out = Math.max(out, field.min);
    if (field.max != null) out = Math.min(out, field.max);
    return out;
  }

  // ---------------------------------------------------------------------------
  // Ruler ticks — "nice numbers", scale/unit agnostic. The original assumed an
  // integer-mm rail and hand-rolled its tick spacing accordingly; this derives
  // a pleasant step from span alone so it looks right whether the catalog's
  // unit is mm, inches, or something nobody has invented yet.
  // ---------------------------------------------------------------------------
  function niceStep(spanV, targetCount) {
    if (!(spanV > 0)) return 1;
    const raw = spanV / targetCount;
    const mag = Math.pow(10, Math.floor(Math.log10(raw)));
    const norm = raw / mag;
    const mult = norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10;
    return mult * mag;
  }
  function buildTicks(loV, hiV, step) {
    const out = [];
    const spanV = hiV - loV;
    if (!(spanV > 0) || !(step > 0)) return out;
    const count = Math.min(Math.round(spanV / step) + 1, 400);
    for (let i = 0; i < count; i++) {
      out.push({ frac: (i * step) / spanV, major: i % 5 === 0 });
    }
    return out;
  }
  const minorStep = $derived(niceStep(span, 50));
  const ticks = $derived(buildTicks(lo, hi, minorStep));

  // ---------------------------------------------------------------------------
  // Telemetry smoothing — one telebuf per available role, fed on every real
  // STATE push, sampled every rendered frame. Never fabricates: `fresh` is
  // false (and callers must withhold) until a real sample has landed and stays
  // that way once samples stop arriving.
  // ---------------------------------------------------------------------------
  const posTele = createTelebuf();
  const velTele = createTelebuf();
  let posTeleChannel = null;
  let velTeleChannel = null;

  $effect(() => {
    const f = pos;
    if (!f) return;
    if (posTeleChannel !== f.channelId) { posTele.reset(); posTeleChannel = f.channelId; }
    const ts = machine.sampleTs[f.channelId];
    const s = machine.samples[f.channelId];
    if (ts && s) {
      const v = displayValue(f, s);
      if (typeof v === 'number' && isFinite(v)) posTele.push(v, ts);
    }
  });

  $effect(() => {
    const f = vel;
    if (!f) return;
    if (velTeleChannel !== f.channelId) { velTele.reset(); velTeleChannel = f.channelId; }
    const ts = machine.sampleTs[f.channelId];
    const s = machine.samples[f.channelId];
    if (ts && s) {
      const v = displayValue(f, s);
      if (typeof v === 'number' && isFinite(v)) velTele.push(v, ts);
    }
  });

  // Same treatment for the commanded setpoint, so the "commanded" numeral and
  // the tape's own live cursor never disagree about "now" with each other or
  // with the actual-position comet — all three are sampled from the same rAF
  // instant below.
  const targetTele = createTelebuf();
  let targetTeleChannel = null;

  $effect(() => {
    const f = target;
    if (!f) return;
    if (targetTeleChannel !== f.channelId) { targetTele.reset(); targetTeleChannel = f.channelId; }
    const ts = machine.sampleTs[f.channelId];
    const s = machine.samples[f.channelId];
    if (ts && s) {
      const v = displayValue(f, s);
      if (typeof v === 'number' && isFinite(v)) targetTele.push(v, ts);
    }
  });

  // ---------------------------------------------------------------------------
  // rAF render loop — drives the canvas AND the hero numerals from the SAME
  // interpolated instant, so the phosphor dot and the big numeral never
  // disagree about "now" (the whole reason HeroNumerals is composed here
  // rather than reading telemetry independently).
  // ---------------------------------------------------------------------------
  let posDisplay = $state(null);
  let speedDisplay = $state(null);
  let moving = $state(false);
  let fresh = $state(false);
  let targetDisplay = $state(null);
  let targetFresh = $state(false);

  let hostEl = $state(null);
  let canvasEl = $state(null);

  const reducedMotion = (typeof window !== 'undefined' && window.matchMedia)
    ? window.matchMedia('(prefers-reduced-motion: reduce)').matches
    : false;

  $effect(() => {
    if (!hostEl || !canvasEl) return;

    let ctx = canvasEl.getContext('2d');
    let dpr = 1, rectW = 0, rectH = 0;
    let rafId = 0;
    let lastFrameTs = 0;
    let lastMoveAt = 0;
    let speedEma = 0;
    let velSmoothPxPerMs = 0;
    let prevPx = null;
    const trail = createTrail();

    function sizeCanvas() {
      const w = hostEl.clientWidth, h = hostEl.clientHeight;
      dpr = window.devicePixelRatio || 1;
      const cw = Math.round(w * dpr), ch = Math.round(h * dpr);
      if (canvasEl.width !== cw || canvasEl.height !== ch) { canvasEl.width = cw; canvasEl.height = ch; }
      rectW = w; rectH = h;
    }

    function flushRibbon(ctx2, pts, startI, endI, midY) {
      const nPts = endI - startI + 1;
      if (nPts < 2) return;
      const headX = pts.x[startI], tailX = pts.x[endI];
      const g = ctx2.createLinearGradient(headX, 0, tailX, 0);
      g.addColorStop(0, ac('r', 0.55));
      g.addColorStop(1, ac('r', 0));
      ctx2.fillStyle = g;
      ctx2.beginPath();
      ctx2.moveTo(pts.x[startI], midY - pts.w[startI]);
      for (let i = startI + 1; i <= endI; i++) ctx2.lineTo(pts.x[i], midY - pts.w[i]);
      for (let j = endI; j >= startI; j--) ctx2.lineTo(pts.x[j], midY + pts.w[j]);
      ctx2.closePath();
      ctx2.fill();
    }

    function drawComet(nowMs, midY, headHalf, glowActive) {
      const capacity = 320;
      const px = new Float64Array(capacity);
      const pw = new Float64Array(capacity);
      let m = 0, runStart = 0, runDir = 0;
      ctx.globalCompositeOperation = 'source-over';
      ctx.setLineDash([]);
      ctx.shadowColor = ac('r', 0.6);
      ctx.shadowBlur = glowActive ? 8 : 5;

      trail.forEachRecent(nowMs, 850, (x, t, age) => {
        if (m >= capacity) return;
        const f = Math.max(0, 1 - age / 850);
        const taper = f * f;
        if (m > 0) {
          const dx = x - px[m - 1];
          const dir = dx > 0.001 ? 1 : (dx < -0.001 ? -1 : runDir);
          if (runDir !== 0 && dir !== 0 && dir !== runDir) {
            flushRibbon(ctx, { x: px, w: pw }, runStart, m - 1, midY);
            runStart = m - 1;
            runDir = dir;
          } else if (runDir === 0) {
            runDir = dir;
          }
        }
        px[m] = x;
        pw[m] = Math.max(headHalf * taper, 0.15);
        m++;
      });
      if (m - 1 > runStart) flushRibbon(ctx, { x: px, w: pw }, runStart, m - 1, midY);
      ctx.shadowBlur = 0;
      ctx.globalAlpha = 1;
    }

    function drawReducedTrail(nowMs, midY) {
      ctx.globalCompositeOperation = 'source-over';
      ctx.setLineDash([]);
      ctx.shadowBlur = 0;
      ctx.globalAlpha = 0.4;
      ctx.strokeStyle = ACCENT.reality;
      ctx.lineWidth = 1;
      ctx.beginPath();
      let started = false;
      trail.forEachRecent(nowMs, 850, (x) => {
        if (!started) { ctx.moveTo(x, midY); started = true; }
        else ctx.lineTo(x, midY);
      });
      ctx.stroke();
      ctx.globalAlpha = 1;
    }

    function draw(nowMs) {
      let dtMs = nowMs - lastFrameTs;
      if (dtMs <= 0 || dtMs > 500) dtMs = 16.667;
      lastFrameTs = nowMs;

      // Pull ground truth through the telebufs at THIS instant.
      if (pos) {
        const r = posTele.sampleAt(nowMs);
        posDisplay = r.value;
        fresh = r.fresh;
        let speedPerSec = null;
        if (vel) {
          const rv = velTele.sampleAt(nowMs);
          if (rv.value != null) speedPerSec = Math.abs(rv.value);
        } else if (r.value != null) {
          speedPerSec = Math.abs(r.velPerMs) * 1000;
        }
        speedEma += 0.2 * ((speedPerSec ?? 0) - speedEma);
        speedDisplay = fresh ? speedEma : null;
        const movingThreshold = span * 0.002; // 0.2%-of-span/s reads as "moving"
        if (fresh && speedEma > movingThreshold) lastMoveAt = nowMs;
        moving = (nowMs - lastMoveAt) < 300;
      } else {
        posDisplay = null; speedDisplay = null; moving = false; fresh = false;
      }

      if (target) {
        const rt = targetTele.sampleAt(nowMs);
        targetDisplay = rt.value;
        targetFresh = rt.fresh;
      } else {
        targetDisplay = null; targetFresh = false;
      }

      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      ctx.clearRect(0, 0, rectW, rectH);

      if (pos && posDisplay != null && fresh && rectW > 0) {
        const frac = pct(posDisplay);
        const px2 = frac * rectW;
        const midY = rectH * 0.5;
        trail.record(px2, nowMs);

        const instVel = prevPx != null ? Math.abs(px2 - prevPx) / Math.max(dtMs, 1) : 0;
        velSmoothPxPerMs += 0.2 * (instVel - velSmoothPxPerMs);

        const H = rectH * 0.5;
        if (reducedMotion) {
          drawReducedTrail(nowMs, midY);
        } else {
          // Reference speed: crossing the FULL span in ~2s reads as "fast".
          // Scale/unit independent — see niceStep's comment for why this
          // beats the original's device-specific max-speed lookup.
          const refPxPerMs = rectW / 2000;
          const speedNorm = Math.min(1, Math.max(0, velSmoothPxPerMs / Math.max(refPxPerMs, 1e-6)));
          const headHalf = Math.max(1, 1 + (0.25 * H - 1) * speedNorm);
          drawComet(nowMs, midY, headHalf, moving);
        }

        ctx.strokeStyle = ACCENT.reality;
        ctx.lineWidth = 1;
        ctx.shadowColor = ac('r', 0.55);
        ctx.shadowBlur = 4;
        ctx.beginPath();
        ctx.moveTo(px2, rectH * 0.22);
        ctx.lineTo(px2, rectH * 0.72);
        ctx.stroke();

        ctx.shadowColor = ac('r', 0.9);
        ctx.shadowBlur = moving ? 12 : 8;
        ctx.fillStyle = ACCENT.core;
        ctx.beginPath();
        ctx.arc(px2, midY, 2.6, 0, Math.PI * 2);
        ctx.fill();
        ctx.shadowBlur = 0;

        prevPx = px2;
      } else {
        prevPx = null;
        velSmoothPxPerMs = 0;
      }

      rafId = requestAnimationFrame(draw);
    }

    sizeCanvas();
    const ro = (typeof ResizeObserver !== 'undefined') ? new ResizeObserver(sizeCanvas) : null;
    if (ro) ro.observe(hostEl);
    rafId = requestAnimationFrame(draw);

    return () => {
      cancelAnimationFrame(rafId);
      if (ro) ro.disconnect();
    };
  });

  // ---- drag state -----------------------------------------------------------

  let dragMode = $state(null); // null | 'min' | 'max' | 'band'
  let dragStartX = 0;
  let dragStartMin = 0;
  let dragStartMax = 0;

  function startDrag(mode, e) {
    const ok = mode === 'min' ? minEnabled : mode === 'max' ? maxEnabled : bandEnabled;
    if (!ok) return;
    dragMode = mode;
    dragStartX = e.clientX;
    dragStartMin = minVal ?? lo;
    dragStartMax = maxVal ?? hi;
    try { e.currentTarget.setPointerCapture(e.pointerId); } catch (err) { /* unsupported: still works via window fallback */ }
    e.preventDefault();
  }

  function onDragMove(e) {
    if (!dragMode || !hostEl) return;
    const rect = hostEl.getBoundingClientRect();
    if (!rect.width) return;
    const dv = ((e.clientX - dragStartX) / rect.width) * span;

    if (dragMode === 'min') {
      const upper = dragStartMax;
      writeSetting(min, snap(clamp(dragStartMin + dv, lo, upper), min));
    } else if (dragMode === 'max') {
      const lower = dragStartMin;
      writeSetting(max, snap(clamp(dragStartMax + dv, lower, hi), max));
    } else if (dragMode === 'band') {
      const width = dragStartMax - dragStartMin;
      const newMin = clamp(dragStartMin + dv, lo, hi - width);
      writeSetting(min, snap(newMin, min));
      writeSetting(max, snap(newMin + width, max));
    }
  }

  function endDrag() { dragMode = null; }

  function onBandKey(e) {
    if (!bandEnabled) return;
    const width = (maxVal ?? hi) - (minVal ?? lo);
    const step = min.step || max.step || Math.max(span / 100, 1e-6);
    let dv = 0;
    if (e.key === 'ArrowRight' || e.key === 'ArrowUp') dv = step;
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowDown') dv = -step;
    else if (e.key === 'Home') dv = lo - (minVal ?? lo);
    else if (e.key === 'End') dv = hi - (maxVal ?? hi);
    else return;
    e.preventDefault();
    const newMin = clamp((minVal ?? lo) + dv, lo, hi - width);
    writeSetting(min, snap(newMin, min));
    writeSetting(max, snap(newMin + width, max));
  }

  function onHandleKey(e, which) {
    const field = which === 'min' ? min : max;
    const ok = which === 'min' ? minEnabled : maxEnabled;
    if (!ok) return;
    const step = field.step || Math.max(span / 100, 1e-6);
    let target;
    const cur = which === 'min' ? (minVal ?? lo) : (maxVal ?? hi);
    if (e.key === 'ArrowRight' || e.key === 'ArrowUp') target = cur + step;
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowDown') target = cur - step;
    else if (e.key === 'Home') target = lo;
    else if (e.key === 'End') target = hi;
    else if (e.key === 'PageUp') target = cur + step * 10;
    else if (e.key === 'PageDown') target = cur - step * 10;
    else return;
    e.preventDefault();
    const lower = which === 'min' ? lo : (minVal ?? lo);
    const upper = which === 'min' ? (maxVal ?? hi) : hi;
    writeSetting(field, snap(clamp(target, lower, upper), field));
  }

  const bandLabel = $derived(
    haveWindow
      ? formatValue(min, minVal) + '–' + formatValue(max, maxVal)
        + ' · ' + formatValue(min, (maxVal ?? hi) - (minVal ?? lo)) + unitOf(min)
      : ''
  );

  // ---------------------------------------------------------------------------
  // Input tape — commands a move (RFC-032 command.position), when the catalog
  // gave us `move`. Not a Field.svelte control and not writeSetting: there is
  // no settingKey/settingChannel/shadow record for an INTENT field, only a
  // sendIntent call and its post-clamp ECHO. Ground truth is preserved by NOT
  // inventing a local "confirmed" value: while dragging the cursor tracks the
  // operator's hand (clearly a live drag, not a claim about the machine);
  // once released it falls straight back to `telemetry.target`, the same
  // ground-truth setpoint the "commanded" hero numeral shows, so the tape and
  // the numeral can never disagree.
  // ---------------------------------------------------------------------------
  let moveDragging = $state(false);
  let moveDragValue = $state(null);
  let moveLastResult = $state(null); // {ok, error, at}
  let lastMoveSentAt = 0;
  const MOVE_MIN_INTERVAL_MS = 80; // client-side throttle while dragging; tap/release always send

  function moveValueFromClientX(clientX) {
    if (!hostEl) return null;
    const rect = hostEl.getBoundingClientRect();
    if (!rect.width) return null;
    const frac = clamp((clientX - rect.left) / rect.width, 0, 1);
    const vlo = move && move.min != null ? move.min : lo;
    const vhi = move && move.max != null ? move.max : hi;
    return vlo + frac * (vhi - vlo);
  }

  async function commandMove(value) {
    const session = getSession();
    if (!session || !move || !moveEnabled) return;
    try {
      await session.sendIntent(move.channelId, { [move.key]: value });
      moveLastResult = { ok: true, at: Date.now() };
    } catch (err) {
      moveLastResult = { ok: false, error: (err && (err.name || err.message)) || 'rejected', at: Date.now() };
    }
  }

  function requestMove(value, force) {
    if (value == null) return;
    const now = Date.now();
    if (!force && (now - lastMoveSentAt) < MOVE_MIN_INTERVAL_MS) return;
    lastMoveSentAt = now;
    commandMove(value);
  }

  function onTapePointerDown(e) {
    if (!moveEnabled) return;
    moveDragging = true;
    try { e.currentTarget.setPointerCapture(e.pointerId); } catch (err) { /* unsupported: still works via window fallback */ }
    const v = moveValueFromClientX(e.clientX);
    moveDragValue = v;
    requestMove(v, true);
    e.preventDefault();
  }
  function onTapePointerMove(e) {
    if (!moveDragging) return;
    const v = moveValueFromClientX(e.clientX);
    moveDragValue = v;
    requestMove(v, false);
  }
  function onTapePointerUp() {
    if (!moveDragging) return;
    moveDragging = false;
    requestMove(moveDragValue, true); // guarantee the released position lands, even mid-throttle
  }

  // Cursor position: the live drag value while dragging, else the machine's
  // own reported setpoint (never a locally-remembered request once released).
  const tapeVal = $derived(
    moveDragging ? moveDragValue : (target && targetFresh && targetDisplay != null ? targetDisplay : null)
  );
  const tapePct = $derived(tapeVal != null ? pct(tapeVal) : null);
</script>

<div class="hero rail-hero">
  {#if pos}
    <HeroNumerals
      posField={pos} velField={vel} targetField={target}
      posVal={posDisplay} speedVal={speedDisplay} targetVal={targetDisplay}
      moving={moving} fresh={fresh} targetFresh={targetFresh}
    />
  {/if}

  <div class="rail-readouts">
    <span class="ro">
      <span class="ro-label">{min.label}</span>
      <output class="mono" data-shadow={statusOf(min)}>{formatValue(min, minVal)}<span class="unit">{unitOf(min)}</span></output>
    </span>
    <span class="ro">
      <span class="ro-label">{max.label}</span>
      <output class="mono" data-shadow={statusOf(max)}>{formatValue(max, maxVal)}<span class="unit">{unitOf(max)}</span></output>
    </span>
  </div>

  {#if move}
    <!-- Input tape — a live command surface. Tap or drag anywhere across the
         full travel to send a move INTENT; the hub clamps (window, limits)
         and the post-clamp ECHO plus telemetry.target are what the cursor
         shows once the drag ends — never an optimistic local guess. -->
    <div class="rail-tape-assembly" class:drag-live={moveDragging} class:disabled={!moveEnabled}>
      <div class="rail-tape-labels">
        <span class="rail-tape-mode">tap &middot; drag to move</span>
        <span class="rail-tape-extent mono">{tapeVal != null ? formatValue(move, tapeVal) + unitOf(move) : '--'}</span>
      </div>
      <div class="rail-tape-track"
           role="slider" tabindex={moveEnabled ? 0 : -1}
           aria-label={move.label} aria-orientation="horizontal"
           aria-valuemin={lo} aria-valuemax={hi} aria-valuenow={tapeVal ?? lo}
           aria-disabled={!moveEnabled}
           onpointerdown={onTapePointerDown}
           onpointermove={onTapePointerMove}
           onpointerup={onTapePointerUp}
           onpointercancel={onTapePointerUp}>
        {#if tapePct != null}
          <div class="rail-tape-cursor" style="left:{tapePct * 100}%"></div>
        {/if}
      </div>
      {#if moveLastResult && !moveLastResult.ok && (Date.now() - moveLastResult.at) < 4000}
        <p class="rail-reason err">move refused: {moveLastResult.error}</p>
      {:else if !moveEnabled}
        <p class="rail-reason">{moveReason}</p>
      {/if}
    </div>
  {:else}
    <!-- Fallback for a machine that has not tagged a move INTENT by role —
         renders the window extent so the visual rhythm survives, commands
         nothing, and says exactly why. -->
    <div class="rail-tape-assembly disabled" aria-disabled="true">
      <div class="rail-tape-labels">
        <span class="rail-tape-mode">input &middot; window</span>
        <span class="rail-tape-extent mono">{haveWindow ? formatValue(min, minVal) + '–' + formatValue(max, maxVal) : '--'}</span>
      </div>
      <div class="rail-tape-track">
        <div class="rail-tape"
             style="left:{haveWindow ? minPct * 100 : 0}%; width:{haveWindow ? Math.max(0, (maxPct - minPct) * 100) : 100}%">
          <span class="rail-tape-micro">no move intent on this catalog</span>
        </div>
      </div>
      <p class="rail-reason">This catalog does not tag a move INTENT by role, so a generic client cannot find it safely &mdash; see RailWidget.svelte.</p>
    </div>
  {/if}

  <div class="spine-rail-host" class:drag-live={dragMode !== null} bind:this={hostEl}>
    <svg class="rail-ruler-svg" viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
      {#each ticks as t}
        <line x1={t.frac * 100} x2={t.frac * 100} y1={t.major ? 34 : 44} y2="58"
              stroke={t.major ? 'var(--line-3)' : 'var(--line-2)'} stroke-width="1" opacity={t.major ? 1 : 0.6} />
      {/each}
      <line x1="0" y1="58" x2="100" y2="58" stroke="var(--line-1)" stroke-width="1" />
    </svg>
    <span class="rail-endcap lo mono">{formatValue(min, lo)}</span>
    <span class="rail-endcap hi mono">{formatValue(max, hi)}</span>
    <span class="rail-ghost mono">{formatValue(max, (lo + hi) / 2)}</span>

    <div class="rail-hz lo" style="clip-path: inset(0 {haveWindow ? (100 - minPct * 100) : 100}% 0 0)"></div>
    <div class="rail-hz hi" style="clip-path: inset(0 0 0 {haveWindow ? (maxPct * 100) : 100}%)"></div>

    <canvas class="rail-canvas" bind:this={canvasEl}></canvas>

    {#if haveWindow}
      <div class="rail-band"
           class:disabled={!bandEnabled}
           class:pending={statusOf(min) !== STATUS.confirmed || statusOf(max) !== STATUS.confirmed}
           role="slider" tabindex={bandEnabled ? 0 : -1}
           aria-label="Stroke window" aria-orientation="horizontal"
           aria-valuemin={lo} aria-valuemax={hi} aria-valuenow={minVal ?? lo}
           aria-valuetext={bandLabel}
           aria-disabled={!bandEnabled}
           data-shadow={worstStatus(statusOf(min), statusOf(max))}
           style="left:{minPct * 100}%; width:{Math.max(0, (maxPct - minPct) * 100)}%"
           onpointerdown={(e) => startDrag('band', e)}
           onpointermove={onDragMove}
           onpointerup={endDrag}
           onpointercancel={endDrag}
           onkeydown={onBandKey}>
        <span class="rail-band-label mono">{bandLabel}</span>
      </div>

      <div class="rail-band-handle lo"
           class:disabled={!minEnabled}
           role="slider" tabindex={minEnabled ? 0 : -1}
           aria-label={min.label} aria-orientation="horizontal"
           aria-valuemin={lo} aria-valuemax={maxVal ?? hi} aria-valuenow={minVal ?? lo}
           aria-valuetext={formatValue(min, minVal) + (unitOf(min) ? ' ' + unitOf(min) : '')}
           aria-disabled={!minEnabled}
           data-shadow={statusOf(min)}
           style="left:{minPct * 100}%"
           onpointerdown={(e) => startDrag('min', e)}
           onpointermove={onDragMove}
           onpointerup={endDrag}
           onpointercancel={endDrag}
           onkeydown={(e) => onHandleKey(e, 'min')}></div>

      <div class="rail-band-handle hi"
           class:disabled={!maxEnabled}
           role="slider" tabindex={maxEnabled ? 0 : -1}
           aria-label={max.label} aria-orientation="horizontal"
           aria-valuemin={minVal ?? lo} aria-valuemax={hi} aria-valuenow={maxVal ?? hi}
           aria-valuetext={formatValue(max, maxVal) + (unitOf(max) ? ' ' + unitOf(max) : '')}
           aria-disabled={!maxEnabled}
           data-shadow={statusOf(max)}
           style="left:{maxPct * 100}%"
           onpointerdown={(e) => startDrag('max', e)}
           onpointermove={onDragMove}
           onpointerup={endDrag}
           onpointercancel={endDrag}
           onkeydown={(e) => onHandleKey(e, 'max')}></div>
    {:else}
      <p class="rail-waiting">waiting for device&hellip;</p>
    {/if}
  </div>

  <div class="rail-hint">
    <span>drag band &middot; drag edges &middot; arrow keys to nudge</span>
  </div>

  {#if min.desc || max.desc}
    <p class="rail-desc">{min.desc || max.desc}</p>
  {/if}
</div>

<style>
  .hero {
    background: var(--bg-card);
    border: 1px solid var(--line);
    border-radius: var(--r);
    padding: var(--gap);
    display: flex;
    flex-direction: column;
    gap: var(--gap);
  }

  .rail-readouts {
    display: flex;
    flex-wrap: wrap;
    gap: 14px;
  }
  .ro { display: flex; flex-direction: column; align-items: flex-start; line-height: 1.2; }
  .ro-label {
    font-size: 0.68rem;
    color: var(--tx-mut);
    text-transform: lowercase;
    letter-spacing: 0.04em;
  }
  .ro output { font-size: 0.95rem; color: var(--reality); padding: 1px 4px; }
  .unit { color: var(--tx-mut); font-size: 0.75em; margin-left: 2px; }

  /* ---- input tape (disabled command surface) ------------------------------ */
  .rail-tape-assembly { width: 100%; }
  .rail-tape-assembly.disabled { opacity: 0.7; }
  .rail-tape-labels {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    margin-bottom: 4px;
  }
  .rail-tape-mode {
    font-size: 0.62rem;
    letter-spacing: 0.12em;
    text-transform: lowercase;
    color: var(--tx-mut);
  }
  .rail-tape-extent { font-size: 0.62rem; color: var(--tx-ghost); }
  .rail-tape-track {
    position: relative;
    width: 100%;
    height: max(calc(var(--tap) * 0.6), 22px);
    border-top: 1px dashed var(--line-1);
    border-bottom: 1px dashed var(--line-1);
    touch-action: none;
  }
  /* Live tape (a `move` role was claimed): the whole track is the command
     surface, cursor: crosshair like the rail host itself. */
  .rail-tape-assembly:not(.disabled) .rail-tape-track { cursor: crosshair; }
  .rail-tape-assembly.disabled .rail-tape-track { cursor: not-allowed; }
  .rail-tape-cursor {
    position: absolute;
    top: 0; bottom: 0;
    width: 2px;
    transform: translateX(-1px);
    background: var(--intent);
    box-shadow: 0 0 8px rgba(var(--intent-rgb), .65);
    pointer-events: none;
    transition: left .12s ease;
  }
  .rail-tape-assembly.drag-live .rail-tape-cursor { transition: none; }
  .rail-tape {
    position: absolute;
    top: 0; bottom: 0; left: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background:
      repeating-linear-gradient(90deg, var(--line-2) 0 1px, transparent 1px 7px),
      var(--bg-sunken);
    border: 1px solid var(--line-2);
    border-radius: var(--r-s);
    cursor: not-allowed;
    overflow: hidden;
    transition: left .25s ease, width .25s ease;
  }
  .rail-tape-micro {
    font-size: 0.58rem;
    letter-spacing: 0.1em;
    color: var(--tx-ghost);
    white-space: nowrap;
    text-transform: uppercase;
  }
  .rail-reason { margin: 4px 0 0; color: var(--tx-ghost); font-size: 0.72rem; }
  .rail-reason.err { color: var(--bad); }

  /* ---- rail host ----------------------------------------------------------- */
  .spine-rail-host {
    position: relative;
    height: max(calc(var(--s) * 72px), 64px);
    background: var(--bg-sunken);
    border: 1px solid var(--line-1);
    border-radius: var(--r-s);
    box-shadow: inset 0 2px 8px rgba(0, 0, 0, 0.6);
    overflow: hidden;
    touch-action: none;
    cursor: crosshair;
  }

  .rail-ruler-svg { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }
  .rail-endcap {
    position: absolute;
    bottom: 2px;
    font-size: 0.56rem;
    color: var(--tx-ghost);
    pointer-events: none;
  }
  .rail-endcap.lo { left: 4px; }
  .rail-endcap.hi { right: 4px; }
  .rail-ghost {
    position: absolute;
    left: 50%; top: 50%;
    transform: translate(-50%, -50%);
    font-size: 0.7rem;
    color: var(--tx-ghost);
    opacity: 0.5;
    pointer-events: none;
  }

  /* Hazard keep-out ribbons — whisper-level red hatch outside the window,
     full-track boxes revealed only via clip-path so the hatch never slides
     when a window edge is dragged (matches the original's fix for that). */
  .rail-hz {
    position: absolute;
    left: 0; right: 0;
    top: 50%;
    height: 9px;
    transform: translateY(-50%);
    pointer-events: none;
    background: repeating-linear-gradient(135deg, rgba(255, 71, 87, .22) 0 3px, rgba(255, 71, 87, .03) 3px 7px);
    transition: clip-path .25s ease;
    z-index: 1;
  }

  .rail-canvas { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }

  .rail-band {
    position: absolute;
    top: 18%;
    height: 44%;
    background: rgba(var(--intent-deep-rgb), .08);
    border-left: 1px solid var(--intent);
    border-right: 1px solid var(--intent);
    box-shadow: 0 0 14px rgba(var(--intent-deep-rgb), .16), inset 0 0 18px rgba(var(--intent-deep-rgb), .07);
    cursor: grab;
    touch-action: none;
    transition: left .12s ease, width .12s ease;
  }
  .rail-band:active { cursor: grabbing; }
  .rail-band.pending { border-left-style: dashed; border-right-style: dashed; }
  .rail-band.disabled { cursor: not-allowed; opacity: 0.55; }
  /* Programmatic window changes (echo/adoption) ease in; a LIVE drag must
     track the pointer 1:1 with zero lag, so drag-live kills the transition
     on the band and its handles for as long as a drag is in flight (mirrors
     the original's "instant during drag" rule). The move tape's own cursor
     gets the identical treatment via .rail-tape-assembly.drag-live above. */
  .drag-live .rail-band,
  .drag-live .rail-band-handle { transition: none; }

  .rail-band-label {
    position: absolute;
    top: -16px;
    left: 50%;
    transform: translateX(-50%);
    font-size: 0.6rem;
    color: var(--intent);
    white-space: nowrap;
    pointer-events: none;
  }

  /* Handles are siblings of the band (not nested — each positions from its own
     independent pct so a keyboard nudge on one never has to touch the other's
     DOM), so their top/height are percentages of the RAIL HOST, tuned to
     straddle the band's 18%-62% vertical span. `left` is set inline per handle
     from minPct/maxPct; the transform centers the touch target on that edge. */
  .rail-band-handle {
    position: absolute;
    top: 40%;
    width: var(--tap);
    height: var(--tap);
    transform: translate(-50%, -50%);
    cursor: ew-resize;
    touch-action: none;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: left .12s ease;
  }
  .rail-band-handle::before {
    content: '';
    width: 3px;
    height: 16px;
    background: var(--intent);
    box-shadow: 0 0 8px rgba(var(--intent-rgb), .65);
  }
  .rail-band-handle.disabled::before { background: var(--tx-ghost); box-shadow: none; }

  .rail-waiting {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    color: var(--tx-ghost);
    font-size: 0.8rem;
    margin: 0;
  }

  .rail-hint {
    display: flex;
    justify-content: space-between;
    font-size: 0.62rem;
    color: var(--tx-ghost);
  }

  .rail-desc { margin: 0; color: var(--tx-mut); font-size: 0.78rem; }
</style>
