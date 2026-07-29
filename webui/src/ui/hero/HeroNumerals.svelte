<script>
  /**
   * HeroNumerals.svelte — the big glowing readout row above the rail.
   *
   * A faithful port of the pre-refactor rail's hero numerals (actual /
   * commanded / lag / speed). Commanded and lag were dropped in the first
   * pass of this refactor because nothing in `field_roles` named "the live
   * setpoint" — rendering it off window bounds or a locally-remembered
   * request would have been exactly the optimistic-UI lie CLAUDE.md forbids.
   * RFC-032 registered `telemetry.target` for exactly this (the machine's own
   * commanded position, as opposed to `telemetry.position`'s measured truth),
   * so both numerals are back — still entirely ground-truth: `targetVal` is
   * whatever the device actually reported, and `lag` is target - position,
   * computed client-side here (still no THIRD role for that subtraction —
   * see roles.js's note on `telemetry.target`).
   *
   * NOT a hero registered in heroes.js — HeroStrip only knows {id, component,
   * fields} entries from that registry, and this widget has no roles of its
   * own to claim. RailWidget composes it directly, feeding it the SAME
   * interpolated numbers driving its canvas, so the numerals and the phosphor
   * dot never disagree about where "now" is.
   *
   * Every number is either `posVal`/`speedVal`/`targetVal` (already smoothed
   * from real telemetry samples in RailWidget's telebuf, never fabricated) —
   * this component does no ground-truth reading of its own.
   */
  import { formatValue, unitOf, precisionFor, labelFor } from '../../model/format.js';

  let {
    posField = null,
    velField = null,
    targetField = null,
    posVal = null,
    speedVal = null,
    targetVal = null,
    moving = false,
    fresh = false,
    targetFresh = false,
  } = $props();

  const posText = $derived(posField ? formatValue(posField, fresh ? posVal : null) : '--');
  const posUnit = $derived(posField ? unitOf(posField) : '');

  // Speed has no field descriptor of its own when derived (no telemetry.velocity
  // role); reuse the position field's precision/unit-with-per-second as the
  // closest honest presentation, still entirely off the catalog's own metadata.
  const speedPrecision = $derived(velField ? precisionFor(velField) : (posField ? Math.max(0, precisionFor(posField) - 1) : 0));
  const speedText = $derived(
    fresh && speedVal != null && isFinite(speedVal) ? speedVal.toFixed(speedPrecision) : '--'
  );
  const speedUnit = $derived(velField ? unitOf(velField) : (posField && unitOf(posField) ? unitOf(posField) + '/s' : ''));

  const commandedText = $derived(targetField ? formatValue(targetField, targetFresh ? targetVal : null) : '--');
  const commandedUnit = $derived(targetField ? unitOf(targetField) : '');

  // lag = target - position (RFC-032: deliberately not its own role). Only
  // meaningful when BOTH sides are fresh ground truth this instant.
  const lagVal = $derived(
    (fresh && targetFresh && posVal != null && targetVal != null) ? (targetVal - posVal) : null
  );
  const lagPrecision = $derived(targetField ? precisionFor(targetField) : (posField ? precisionFor(posField) : 0));
  const lagText = $derived(lagVal != null && isFinite(lagVal) ? lagVal.toFixed(lagPrecision) : '--');
</script>

<div class="hero-numerals" class:stale={!fresh}>
  <div class="hn-item hn-primary">
    <span class="hn-label">
      <svg class="hn-reticle" width="12" height="12" viewBox="0 0 12 12" aria-hidden="true">
        <circle cx="6" cy="6" r="4" fill="none" stroke="currentColor" stroke-width="1"/>
        <line x1="6" y1="0" x2="6" y2="2.6" stroke="currentColor" stroke-width="1"/>
        <line x1="6" y1="9.4" x2="6" y2="12" stroke="currentColor" stroke-width="1"/>
        <line x1="0" y1="6" x2="2.6" y2="6" stroke="currentColor" stroke-width="1"/>
        <line x1="9.4" y1="6" x2="12" y2="6" stroke="currentColor" stroke-width="1"/>
        <circle cx="6" cy="6" r="0.9" fill="currentColor"/>
      </svg>
      {posField ? labelFor(posField).toLowerCase() : 'actual'}
    </span>
    <span class="hn-val mono" class:glow={moving && fresh}>{posText}<span class="hn-unit">{posUnit}</span></span>
  </div>

  {#if targetField}
    <div class="hn-item hn-secondary">
      <span class="hn-label">{labelFor(targetField).toLowerCase()}</span>
      <span class="hn-val mono">{commandedText}<span class="hn-unit">{commandedUnit}</span></span>
    </div>

    <!-- "lag" has no role of its own (roles.js: it is target - position,
         computed client-side) — there is no field to resolve a label from,
         so this stays a plain string rather than a fabricated ROLE_LABEL
         entry. -->
    <div class="hn-item hn-secondary">
      <span class="hn-label">lag</span>
      <span class="hn-val mono">{lagText}<span class="hn-unit">{commandedUnit}</span></span>
    </div>
  {/if}

  <div class="hn-item hn-secondary">
    <!-- Same treatment as the speed VALUE above: labeled from velField when
         the machine annotated telemetry.velocity, else the plain fallback
         (this number is client-derived from position, not its own field). -->
    <span class="hn-label">{velField ? labelFor(velField).toLowerCase() : 'speed'}</span>
    <span class="hn-val mono">{speedText}<span class="hn-unit">{speedUnit}</span></span>
  </div>
</div>

<style>
  .hero-numerals {
    display: flex;
    align-items: flex-end;
    gap: 18px;
    flex-wrap: wrap;
  }

  .hn-item {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 2px;
  }

  .hn-label {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-family: var(--font);
    font-size: .68rem;
    color: var(--tx-mut);
    font-weight: 500;
    text-transform: lowercase;
    letter-spacing: .06em;
  }
  .hn-primary .hn-label { color: var(--reality); }
  .hn-reticle { flex: 0 0 auto; filter: drop-shadow(0 0 3px rgba(var(--reality-rgb), .5)); }

  .hn-val { font-size: 1.35rem; color: var(--tx-val); }

  /* The flagship numeral — OG sizing: clamp(54px, 6.2vw, 80px) desktop,
     dropping to a tighter clamp under 1024px so the hero row never wraps on
     a tablet (`webui-prerefactor` .hero-val breakpoints). */
  .hn-primary .hn-val {
    font-size: clamp(54px, 6.2vw, 80px);
    line-height: 0.95;
    color: var(--reality);
    text-shadow: var(--glow-reality);
    font-variation-settings: 'wght' 500;
  }
  @media (max-width: 1023px) {
    .hn-primary .hn-val { font-size: clamp(42px, 8.5vw, 54px); }
  }
  .hn-val.glow {
    color: var(--reality);
    text-shadow: var(--glow-reality);
  }

  .hero-numerals.stale .hn-primary .hn-val {
    color: var(--tx-ghost);
    text-shadow: none;
  }

  .hn-unit {
    font-family: var(--font);
    font-size: .5em;
    color: var(--tx-mut);
    margin-left: 3px;
  }
  .hn-primary .hn-unit { font-size: .22em; }

  @media (prefers-reduced-motion: reduce) {
    .hn-val { transition: none; }
  }
</style>
