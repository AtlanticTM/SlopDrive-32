<script>
  /**
   * PlanStrip.svelte — in-flight motion-plan visualizer, bound by ROLE.
   *
   * Reproduces the character of the pre-refactor features/planstrip.js (a
   * glowing span between the plan's start/end, a sweep head at the live
   * setpoint, a fading trail of recently-completed segments) with a cleaner
   * execution: no bespoke render-clock module, no wire-specific state
   * machine — it just redraws from whatever the catalog's own decoded
   * snapshot says, at whatever rate the hub is granting the channel(s).
   *
   * ── RFC-035, and ROLE-ONLY discipline ────────────────────────────────────
   *
   * The registry names `plan.start/end/current/velocity/elapsed/duration/
   * style` (the SlopSync repo's registry.yaml, field_roles), so this binds
   * like every other hero/widget: claimRoles() against
   * machine.catalog.model.byRole, same as RailWidget. That claim resolves
   * per-FIELD, not per-channel — each resolved field carries its own
   * channelId — so a hub is free to spread plan telemetry across more than
   * one channel and this still draws correctly.
   *
   * This component used to also carry a NAME-HEURISTIC fallback (find an h2c
   * STATE channel whose catalog name matched /plan/i, then classify its
   * layout fields by regex over name+desc) for a hub that had not tagged the
   * roles yet. That fallback is DELETED, not demoted — this is the reference
   * SlopSync client, and shipping a name-guessing example teaches every
   * third-party client the exact technique the role catalog exists to make
   * unnecessary. A hub that has not tagged plan.* renders nothing here; its
   * fields still show up as ordinary generic controls elsewhere on the page.
   *
   * "Is a plan actually running right now" also has no role (a device-
   * specific status bitfield is not something a DIFFERENT machine's planner
   * would necessarily share, so it fails the registry's own inclusion test)
   * — see `isActive` below for the role-agnostic substitute this uses instead
   * of reading a bit by name.
   */
  import { machine } from '../../model/machine.svelte.js';
  import { formatValue, unitOf, optionLabel } from '../../model/format.js';
  import { ROLE, claimRoles } from '../../model/roles.js';

  function clamp(v, lo, hi) { return Math.min(hi, Math.max(lo, v)); }

  /** Per-field sample lookup — every role-claimed field carries its own
      channelId, so a claim spread across multiple channels still reads the
      right sample for each piece. */
  function fieldSample(f) { return f ? machine.samples[f.channelId] : undefined; }
  function fieldValue(f) {
    const s = fieldSample(f);
    return (f && s) ? s[f.name] : undefined;
  }

  /** Normalized 0..1 position: the field's own [min,max] if annotated, else assume the value is already normalized (matches every "_norm" style field seen on this and similarly-shaped channels). */
  function pct(f) {
    if (!f) return null;
    const v = fieldValue(f);
    if (v == null || !isFinite(v)) return null;
    if (f.min != null && f.max != null && f.max > f.min) return clamp((v - f.min) / (f.max - f.min), 0, 1);
    return clamp(v, 0, 1);
  }

  // ---- discovery: ROLE, and ONLY role — see this file's header --------------
  const fields = $derived.by(() => {
    const byRole = machine.catalog.model && machine.catalog.model.byRole;
    if (!byRole) return null;
    const claim = claimRoles(byRole, {
      optional: {
        start: ROLE.planStart, end: ROLE.planEnd, position: ROLE.planCurrent,
        velocity: ROLE.planVelocity, elapsed: ROLE.planElapsed, duration: ROLE.planDuration,
        style: ROLE.planStyle,
      },
    });
    // None of the span/position roles resolved: this hub has not annotated
    // plan.* — decline entirely (opportunities, never requirements) rather
    // than rendering an empty strip off a "successful" but useless claim.
    if (!claim || !(claim.start || claim.end || claim.position)) return null;
    return claim;
  });

  const startPct = $derived(fields ? pct(fields.start) : null);
  const endPct = $derived(fields ? pct(fields.end) : null);
  const curPct = $derived(fields ? pct(fields.position) : null);
  const haveSpan = $derived(startPct != null && endPct != null);
  const haveAnyPosition = $derived(haveSpan || curPct != null);

  const velVal = $derived(fields && fields.velocity ? fieldValue(fields.velocity) : undefined);
  const durVal = $derived(fields && fields.duration ? fieldValue(fields.duration) : undefined);
  const elapsedVal = $derived(fields && fields.elapsed ? fieldValue(fields.elapsed) : undefined);
  const progressFrac = $derived.by(() => {
    if (durVal == null || elapsedVal == null || !isFinite(durVal) || durVal <= 0) return null;
    return clamp(elapsedVal / durVal, 0, 1);
  });

  const styleVal = $derived(fields && fields.style ? fieldValue(fields.style) : undefined);

  // ---------------------------------------------------------------------------
  // "Is a plan actually running right now" — bug #4's visibility rule (the
  // strip must appear ONLY while a plan is actually streaming, the same as
  // the pre-refactor original). No role names a "plan active" concept (see
  // this file's header), so this reads the one signal every role-claimed
  // channel already gives for free, generically: how recently it last
  // pushed. This device's plan-strip publisher only republishes while a plan
  // is live (an idle machine costs a subscriber nothing after one baseline
  // snapshot) — the exact behavior the original's own "is the 0x04 frame
  // still arriving" check depended on, just read off the generic per-channel
  // sample clock instead of a wire-specific staleness field. FRESH_MS mirrors
  // the original's 250ms recency window; HIDE_GRACE_MS mirrors its 1000ms
  // post-stream fade-out grace so a brief gap between segments doesn't
  // flicker the strip off and straight back on.
  // ---------------------------------------------------------------------------
  const FRESH_MS = 250;
  const HIDE_GRACE_MS = 1000;
  let isActive = $state(false);
  let hideTimer = null;

  function claimedChannelIds() {
    if (!fields) return [];
    const ids = new Set();
    for (const k of ['start', 'end', 'position', 'velocity', 'elapsed', 'duration', 'style']) {
      if (fields[k]) ids.add(fields[k].channelId);
    }
    return [...ids];
  }

  function pollActivity() {
    const ids = claimedChannelIds();
    const now = Date.now();
    const fresh = ids.some((id) => (now - (machine.sampleTs[id] || 0)) < FRESH_MS);
    if (fresh) {
      if (hideTimer) { clearTimeout(hideTimer); hideTimer = null; }
      isActive = true;
    } else if (isActive && !hideTimer) {
      hideTimer = setTimeout(() => { isActive = false; hideTimer = null; }, HIDE_GRACE_MS);
    }
  }

  // ---- canvas: span + sweep head + fading segment ghosts ---------------------
  let canvasEl = $state(null);
  const GHOST_N = 5;
  const GHOST_FADE_MS = 1200;

  $effect(() => {
    if (!canvasEl || !haveAnyPosition) return;
    const ctx = canvasEl.getContext('2d');
    const root = document.documentElement;
    const cssVar = (name) => getComputedStyle(root).getPropertyValue(name).trim();
    const cIntent = cssVar('--reality');
    const cWarn = cssVar('--warn');
    const cLine = cssVar('--line');

    const mq = window.matchMedia('(prefers-reduced-motion: reduce)');
    let reduced = mq.matches;
    const onMqChange = (e) => { reduced = e.matches; };
    mq.addEventListener('change', onMqChange);

    let cssW = 0, cssH = 0;
    function sizeIfNeeded() {
      const w = canvasEl.clientWidth, h = canvasEl.clientHeight;
      if (!w || !h) return false;
      if (w !== cssW || h !== cssH) {
        const dpr = window.devicePixelRatio || 1;
        canvasEl.width = Math.round(w * dpr);
        canvasEl.height = Math.round(h * dpr);
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        cssW = w; cssH = h;
      }
      return true;
    }

    let dispFrom = null, dispTo = null;
    let lastKey = '';
    const gFrom = new Float64Array(GHOST_N);
    const gTo = new Float64Array(GHOST_N);
    const gBorn = new Float64Array(GHOST_N);
    let gHead = 0, gLen = 0;
    function pushGhost(from, to, now) {
      gHead = (gHead + 1) % GHOST_N;
      gFrom[gHead] = from; gTo[gHead] = to; gBorn[gHead] = now;
      if (gLen < GHOST_N) gLen++;
    }

    function draw() {
      pollActivity();
      if (!sizeIfNeeded()) return;
      ctx.clearRect(0, 0, cssW, cssH);
      const h = cssH;
      const now = performance.now();

      // Track baseline + faint window ticks.
      ctx.strokeStyle = cLine;
      ctx.lineWidth = 1;
      ctx.strokeRect(0.5, 0.5, cssW - 1, h - 1);

      const from = startPct != null ? startPct : curPct;
      const to = endPct != null ? endPct : curPct;
      if (from == null || to == null) return;

      const key = from.toFixed(4) + ':' + to.toFixed(4);
      if (dispFrom == null) { dispFrom = from; dispTo = to; lastKey = key; }
      else if (key !== lastKey) {
        pushGhost(dispFrom, dispTo, now);
        lastKey = key;
      }
      const ease = reduced ? 1 : 0.3;
      dispFrom += (from - dispFrom) * ease;
      dispTo += (to - dispTo) * ease;

      // Ghost trail of recently-superseded spans.
      for (let g = 0; g < gLen; g++) {
        const gi = (gHead - g + GHOST_N) % GHOST_N;
        const age = now - gBorn[gi];
        if (age > GHOST_FADE_MS) continue;
        const f2 = 1 - age / GHOST_FADE_MS;
        const ga = gFrom[gi] * cssW, gb = gTo[gi] * cssW;
        ctx.fillStyle = `color-mix(in srgb, ${cIntent} ${Math.round(16 * f2 * f2)}%, transparent)`;
        ctx.fillRect(Math.min(ga, gb), h * 0.5 - 1.5, Math.max(1, Math.abs(gb - ga)), 3);
      }

      // Current span, glowing gradient toward the "to" end.
      const x0 = dispFrom * cssW, x1 = dispTo * cssW;
      const lo = Math.min(x0, x1), hi = Math.max(x0, x1);
      const grad = ctx.createLinearGradient(x0, 0, x1, 0);
      grad.addColorStop(0, `color-mix(in srgb, ${cIntent} 12%, transparent)`);
      grad.addColorStop(1, `color-mix(in srgb, ${cIntent} 55%, transparent)`);
      ctx.fillStyle = grad;
      ctx.fillRect(lo, 1, Math.max(2, hi - lo), h - 2);

      // Sweep head at the live setpoint, if the channel reports one.
      if (curPct != null) {
        const hx = curPct * cssW;
        ctx.save();
        ctx.shadowColor = cIntent;
        ctx.shadowBlur = reduced ? 0 : 7;
        ctx.fillStyle = cIntent;
        ctx.fillRect(hx - 0.75, 0, 1.5, h);
        ctx.restore();
      }

      // Caret at the "to" end. isActive is the freshness signal above; the
      // fade-out grace means this can briefly draw a "just went stale" frame
      // in the warn tone before the whole strip's opacity finishes dropping
      // to 0 — cosmetic, not a second source of truth for "is it running".
      ctx.save();
      ctx.shadowColor = isActive ? cIntent : cWarn;
      ctx.shadowBlur = reduced ? 0 : 6;
      ctx.fillStyle = isActive ? cIntent : cWarn;
      ctx.fillRect(x1 - 1, -1, 2, h + 2);
      ctx.restore();
    }

    let raf = null, timer = null;
    function frame() { draw(); raf = requestAnimationFrame(frame); }
    if (reduced) { draw(); timer = setInterval(draw, 1000); }
    else raf = requestAnimationFrame(frame);

    return () => {
      mq.removeEventListener('change', onMqChange);
      if (raf) cancelAnimationFrame(raf);
      if (timer) clearInterval(timer);
      if (hideTimer) { clearTimeout(hideTimer); hideTimer = null; }
    };
  });
</script>

{#if fields && haveAnyPosition}
  <!-- Presence in the DOM follows the role claim (bug #4: only renders at
       all when a hub tags plan.*) so mounting never causes a jump of its
       own; `.on` (both here and on .plan-strip) follows `isActive`, the
       freshness-based "is a plan actually streaming right now" signal.
       reproducing the pre-refactor original's "invisible except while
       streaming" placement under the rail.

       BUG FIX (dead space under the rail): the original reserved this
       card's full height PERMANENTLY (opacity-only show/hide) — correct for
       "never reflows", wrong for "never wastes ~85px of idle vertical space"
       (measured: .plan-strip's own box was 61px plus a 12px flex gap on
       each side = 85px of nothing between the rail and the hint text, for
       every session that never streams a plan). `.plan-collapse` animates
       the row's height via the `grid-template-rows: 0fr -> 1fr` technique
       instead of reserving it outright: a smooth grow/shrink (no snap, no
       jump) that costs ~0px while idle rather than a fixed ~85px forever.
       The 24px of flex gap around this element is what's left when idle —
       ordinary rhythm spacing, not a hole. -->
  <div class="plan-collapse" class:on={isActive}>
    <div class="plan-strip" class:on={isActive} aria-hidden={!isActive}>
      <div class="plan-lane">
        <canvas bind:this={canvasEl} role="img" aria-label="In-flight motion plan"></canvas>
      </div>

      <div class="plan-meta">
        {#if fields.style}
          <span class="chip">{optionLabel(fields.style, styleVal)}</span>
        {/if}
        {#if fields.velocity}
          <output class="chip mono">{formatValue(fields.velocity, velVal)}<span class="unit">{unitOf(fields.velocity)}</span></output>
        {/if}
        {#if progressFrac != null}
          <span class="chip progress">
            <span class="progress-track"><span class="progress-fill" style="width:{progressFrac * 100}%"></span></span>
            <output class="mono">{formatValue(fields.elapsed, elapsedVal)}<span class="unit">{unitOf(fields.elapsed)}</span> / {formatValue(fields.duration, durVal)}<span class="unit">{unitOf(fields.duration)}</span></output>
          </span>
        {:else if fields.elapsed}
          <output class="chip mono">{formatValue(fields.elapsed, elapsedVal)}<span class="unit">{unitOf(fields.elapsed)}</span></output>
        {:else if fields.duration}
          <output class="chip mono">{formatValue(fields.duration, durVal)}<span class="unit">{unitOf(fields.duration)}</span></output>
        {/if}
      </div>
    </div>
  </div>
{/if}

<style>
  /* Height-collapsing wrapper — see the header note above. `0fr`/`1fr` on a
     single-row grid is the standard trick for animating to/from an
     un-measured intrinsic height; `.plan-strip` below is the grid item and
     needs `min-height: 0` + `overflow: hidden` for the track to actually be
     able to size it down to (visually) nothing while collapsed. */
  .plan-collapse {
    display: grid;
    grid-template-rows: 0fr;
    transition: grid-template-rows 0.35s ease;
  }
  .plan-collapse.on { grid-template-rows: 1fr; }
  @media (prefers-reduced-motion: reduce) {
    .plan-collapse { transition: none; }
  }

  /* `.on` here still gates opacity/pointer-events (unchanged from before) so
     the content crossfades as the row grows/shrinks rather than popping in
     at the end of the height transition. This is a plain layout wrapper
     nested inside RailWidget's own card, not a card of its own — the visual
     chrome lives on .plan-lane below, same as the original. */
  .plan-strip {
    display: flex;
    flex-direction: column;
    gap: 8px;
    opacity: 0;
    min-height: 0;
    overflow: hidden;
    pointer-events: none;
    transition: opacity 0.35s ease;
  }
  .plan-strip.on { opacity: 1; pointer-events: auto; }
  @media (prefers-reduced-motion: reduce) {
    .plan-strip { transition: none; }
  }

  .plan-lane {
    position: relative;
    height: 30px;
    background: var(--bg-sunken);
    border: 1px solid var(--line);
    border-radius: var(--r-s);
    box-shadow: inset 0 1px 4px rgba(0, 0, 0, 0.5);
  }
  .plan-lane canvas {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    display: block;
  }

  .plan-meta {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 8px;
  }

  .chip {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 2px 8px;
    border-radius: 999px;
    background: var(--bg-sunken);
    border: 1px solid var(--line);
    font-size: 0.74rem;
    color: var(--ink-dim);
  }
  .chip output, .chip.mono { color: var(--ink); }
  .unit { color: var(--ink-dim); font-size: 0.9em; margin-left: 1px; }

  .progress { min-width: 0; }
  .progress-track {
    width: 48px;
    height: 4px;
    border-radius: 2px;
    background: var(--line-soft);
    overflow: hidden;
    flex: 0 0 auto;
  }
  .progress-fill {
    display: block;
    height: 100%;
    background: var(--reality);
  }
</style>
