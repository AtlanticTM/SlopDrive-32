<script>
  /**
   * PlanStrip.svelte — in-flight motion-plan visualiser, discovered by NAME.
   *
   * Reproduces the character of the pre-refactor features/planstrip.js (a
   * glowing span between the plan's start/end, a sweep head at the live
   * setpoint, a fading trail of recently-completed segments) with a cleaner
   * execution: no bespoke render-clock module, no wire-specific state
   * machine — it just redraws from whatever the catalog's own decoded
   * snapshot says, at whatever rate the hub is granting the channel.
   *
   * ── Why this file is NOT like roles.js discovery ────────────────────────
   *
   * Every other hero/widget in this app binds to a REGISTRY ROLE
   * (`telemetry.position`, `window.min`, ...) — a vocabulary the protocol
   * defines, so the same widget draws correctly on any conforming machine.
   * Plan telemetry has NO registered role yet (checked against
   * docs/slopsync/registry/registry.yaml's field_roles at the time this was
   * written), so there is nothing to claim. What follows are TWO SEPARATE,
   * WEAKER heuristics, both explicitly name/shape-based rather than
   * role-based, and both are exactly the kind of thing a real role
   * vocabulary would delete:
   *
   *   1. Channel discovery: an h2c STATE channel whose catalog NAME matches
   *      /plan/i. This is the "catalog entry NAME pattern" fallback the
   *      generic-rendering contract explicitly allows when no role exists.
   *      A machine with a differently-named plan channel, or none at all,
   *      is invisible to this heuristic — the component then renders
   *      nothing, which is the correct, honest degrade.
   *
   *   2. Sub-field classification: once a candidate channel is found, its
   *      OWN layout fields are sorted into start/end/current-position,
   *      current-velocity, duration/elapsed and style/flags by matching
   *      regexes against each field's `name` + `desc` prose (never an exact
   *      wire field name — see classifyPlanFields below). Two differently
   *      authored plan channels that both describe themselves in plain
   *      English ("where the current plan started" vs "segment origin")
   *      may classify differently or not at all; that is the cost of not
   *      having a role for this yet.
   *
   * THE DURABLE FIX is a registered `plan.*` role family (start/end/
   * position/velocity/elapsed/duration/style/flags) in the SlopSync
   * registry, at which point this file collapses to a claimRoles() call
   * exactly like RailWidget's. Worth an RFC — see docs/slopsync/RFC-QUEUE.md.
   */
  import { machine } from '../../model/machine.svelte.js';
  import { CHANNEL_CLASS, PACKED } from '../../core/slopsync/index.js';
  import { humanize } from '../../model/settings.js';
  import { formatValue, unitOf, optionLabel } from '../../model/format.js';

  const NUMERIC = new Set([
    PACKED.u8, PACKED.i8, PACKED.u16, PACKED.i16, PACKED.u32, PACKED.i32, PACKED.f32,
  ]);

  /** name+desc, underscores turned to spaces so `\b` word-boundaries land inside `snake_case_names`. */
  function fieldText(f) {
    return ((f.name || '').replace(/[_-]/g, ' ') + ' ' + (f.desc || '')).toLowerCase();
  }

  /**
   * The name/shape heuristic documented above. Priority order matters: a
   * field whose prose is "how long the current plan runs" must land on
   * `duration`, not `position`, even though it also contains the word
   * "current" — so duration/elapsed are matched before position/velocity.
   */
  function classifyPlanFields(layout) {
    const out = {
      start: null, end: null, position: null, velocity: null,
      duration: null, elapsed: null, style: null, flags: null,
    };
    for (const f of layout) {
      const t = fieldText(f);
      const numeric = NUMERIC.has(f.type);
      if (numeric && !out.duration && /\bduration\b|\btotal\b|\boverall\b/.test(t)) { out.duration = f; continue; }
      if (numeric && !out.elapsed && /\belapsed\b|\bprogress\b|\bhow far\b/.test(t)) { out.elapsed = f; continue; }
      if (numeric && !out.start && /\bstart(ed|ing)?\b|\bbegin/.test(t)) { out.start = f; continue; }
      if (numeric && !out.end && /\bend(s|ed|ing)?\b|\btarget\b|\bfinish/.test(t)) { out.end = f; continue; }
      if (numeric && !out.velocity && /\bvel(ocity)?\b|\bspeed\b/.test(t)) { out.velocity = f; continue; }
      if (numeric && !out.position && /\bcur(rent)?\b|\bnow\b|\bsetpoint\b/.test(t)) { out.position = f; continue; }
      if (!out.flags && f.bits && f.bits.some(Boolean)) { out.flags = f; continue; }
      if (!out.style && f.options && f.options.length) { out.style = f; continue; }
    }
    return out;
  }

  /** A bit in the flags field that reads as "a plan is actually running". Opportunistic — absent on a machine that doesn't name one this way. */
  function activeBitName(flagsField) {
    if (!flagsField || !flagsField.bits) return null;
    return flagsField.bits.find((b) => b && /active|running|live|busy/i.test(b)) || null;
  }

  function clamp(v, lo, hi) { return Math.min(hi, Math.max(lo, v)); }

  /** Normalized 0..1 position: the field's own [min,max] if annotated, else assume the value is already normalized (matches every "_norm" style field seen on this and similarly-shaped channels). */
  function pct(f, sample) {
    if (!f || !sample) return null;
    const v = sample[f.name];
    if (v == null || !isFinite(v)) return null;
    if (f.min != null && f.max != null && f.max > f.min) return clamp((v - f.min) / (f.max - f.min), 0, 1);
    return clamp(v, 0, 1);
  }

  // ---- discovery ------------------------------------------------------------
  const planEntry = $derived.by(() => {
    const entries = machine.catalog.entries || [];
    return entries.find((e) => e.layout && e.dir === 0 && e.cls === CHANNEL_CLASS.STATE && /plan/i.test(e.name)) || null;
  });

  const fields = $derived(planEntry ? classifyPlanFields(planEntry.layout) : null);
  const sample = $derived(planEntry ? machine.samples[planEntry.id] : undefined);

  const startPct = $derived(fields ? pct(fields.start, sample) : null);
  const endPct = $derived(fields ? pct(fields.end, sample) : null);
  const curPct = $derived(fields ? pct(fields.position, sample) : null);
  const haveSpan = $derived(startPct != null && endPct != null);
  const haveAnyPosition = $derived(haveSpan || curPct != null);

  const velVal = $derived(fields && fields.velocity && sample ? sample[fields.velocity.name] : undefined);
  const durVal = $derived(fields && fields.duration && sample ? sample[fields.duration.name] : undefined);
  const elapsedVal = $derived(fields && fields.elapsed && sample ? sample[fields.elapsed.name] : undefined);
  const progressFrac = $derived.by(() => {
    if (durVal == null || elapsedVal == null || !isFinite(durVal) || durVal <= 0) return null;
    return clamp(elapsedVal / durVal, 0, 1);
  });

  const styleVal = $derived(fields && fields.style && sample ? sample[fields.style.name] : undefined);

  const activeBit = $derived(fields ? activeBitName(fields.flags) : null);
  const flagBits = $derived(
    fields && fields.flags && sample ? (sample[fields.flags.name + '_bits'] || {}) : {}
  );
  // "is a plan actually running right now" if the flags field names a bit
  // that reads that way; otherwise show whenever the channel has data at
  // all, since we have no better signal.
  const isActive = $derived(activeBit ? !!flagBits[activeBit] : haveAnyPosition);
  // Other true bits, for display as plain chips — excludes the one already
  // conveyed by the strip's own on/off fade so it isn't shown twice.
  const otherFlagChips = $derived.by(() => {
    if (!fields || !fields.flags || !fields.flags.bits) return [];
    return fields.flags.bits
      .map((name, b) => ({ name, on: !!flagBits[name] }))
      .filter((b) => b.name && b.name !== activeBit && b.on);
  });

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

      // Caret at the "to" end, amber if the field-level heuristic detected
      // no live channel data at all (falls back to the neutral warn tone
      // rather than inventing a "late" concept this generic widget cannot
      // actually verify).
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
    };
  });
</script>

{#if planEntry && haveAnyPosition}
  <div class="plan-strip" class:on={isActive}>
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
      {#each otherFlagChips as b (b.name)}
        <span class="chip flag">{humanize(b.name)}</span>
      {/each}
    </div>
  </div>
{/if}

<style>
  /* Presence is opacity-only (height always reserved) so a plan starting or
     stopping never reflows the page around it — matches the legacy
     planstrip.js's own reasoning for the same choice. */
  .plan-strip {
    background: var(--bg-card);
    border: 1px solid var(--line);
    border-radius: var(--r);
    padding: var(--gap);
    display: flex;
    flex-direction: column;
    gap: 8px;
    opacity: 0.4;
    transition: opacity 0.35s ease;
  }
  .plan-strip.on { opacity: 1; }
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
  .chip.flag { color: var(--warn); border-color: color-mix(in srgb, var(--warn) 40%, var(--line)); }
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
