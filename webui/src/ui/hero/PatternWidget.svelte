<script>
  /**
   * PatternWidget.svelte — the built-in pattern generator, as a start/stop
   * control plus a pattern-tile grid plus whichever knobs the machine publishes.
   *
   * `running` and `select` are guaranteed by the claim spec in heroes.js; the
   * four knobs (speed/depth/stroke/sensation) are opportunistic — a machine
   * that only publishes speed still gets a clean card with one slider.
   */
  import { machine } from '../../model/machine.svelte.js';
  import { isFieldEnabled } from '../../model/settings.js';
  import { writeSetting, displayValue, statusOf } from '../../model/shadow.svelte.js';
  import { optionLabel, labelFor } from '../../model/format.js';
  import Field from '../Field.svelte';

  let { fields } = $props();
  // Read through the prop rather than destructuring once — heroes.js hands us
  // a fresh `fields` object whenever the catalog rebuilds.
  const running = $derived(fields.running);
  const select = $derived(fields.select);
  const speed = $derived(fields.speed);
  const depth = $derived(fields.depth);
  const stroke = $derived(fields.stroke);
  const sensation = $derived(fields.sensation);

  function sampleOf(f) { return f ? machine.samples[f.channelId] : undefined; }

  function enabledOf(f) {
    if (!f || f.readOnly) return false;
    if (!isFieldEnabled(f, sampleOf(f))) return false;
    if (machine.link.phase !== 'live') return false;
    const e = machine.catalog.entries.find((x) => x.id === f.writeChannel);
    if (!e) return false;
    return (machine.link.roles | 0) >= (e.access | 0);
  }

  const runningVal = $derived(displayValue(running, sampleOf(running)));
  const isRunning = $derived(!!runningVal);
  const runningEnabled = $derived(enabledOf(running));

  const selectVal = $derived(displayValue(select, sampleOf(select)));
  const selectEnabled = $derived(enabledOf(select));

  const knobs = $derived(
    [speed, depth, stroke, sensation].filter((f) => f != null)
  );

  // Card-head status sub-label (OG §2.1f, .card-state): "standby" or
  // "running · <pattern name>". The title bar itself belongs to DashItem
  // (this component only owns the card BODY), so this line rides at the top
  // of the body as the closest in-scope stand-in for the OG's title-row chip.
  const patternStateText = $derived(
    isRunning ? 'running · ' + optionLabel(select, Number(selectVal)).toLowerCase() : 'standby'
  );

  function toggleRunning() {
    if (!runningEnabled) return;
    writeSetting(running, isRunning ? 0 : 1);
  }

  function chooseOption(i) {
    if (!selectEnabled || Number(selectVal) === i) return;
    writeSetting(select, i);
  }
</script>

<div class="hero pattern-hero">
  <div class="pattern-topline">
    <span class="pattern-state" data-shadow={statusOf(running)}>{patternStateText}</span>
  </div>

  <div class="pattern-head">
    <button type="button" class="run-btn og-btn" class:primary={!isRunning} class:running={isRunning}
            role="switch" aria-checked={isRunning} disabled={!runningEnabled}
            data-shadow={statusOf(running)}
            onclick={toggleRunning}>
      <span class="run-dot" aria-hidden="true"></span>
      <span class="run-text">{isRunning ? 'STOP PATTERN' : 'START PATTERN'}</span>
    </button>
  </div>

  {#if select.options && select.options.length}
    <!-- OG .pat-grid/.pat-tile: bordered label tiles, active = intent border
         + glow (commanded-but-not-yet-measured, same semantic as everywhere
         else intent purple appears). The OG tiles also carried a per-pattern
         waveform glyph (device art); that art has no source here, so this
         renders label-only — a waveform-glyph hint is a catalog/RFC
         candidate (a `glyph` field on the pattern registry entry), never a
         client-invented shape. -->
    <div class="pattern-grid" role="radiogroup" aria-label={labelFor(select)}
         data-shadow={statusOf(select)}>
      {#each select.options as _opt, i}
        <button type="button" class="pat-tile" role="radio" aria-checked={Number(selectVal) === i}
                class:on={Number(selectVal) === i}
                disabled={!selectEnabled}
                onclick={() => chooseOption(i)}>
          <span class="pat-tile-label">{optionLabel(select, i)}</span>
        </button>
      {/each}
    </div>
  {/if}

  {#if knobs.length}
    <!-- OG .fld2 compact 2-col slider grid — reuses Field.svelte's slider
         widget verbatim (label top-left, recessed value chip top-right,
         hairline range below) instead of a second hand-rolled slider, so
         these rows are pixel-identical to every other slider in the app. -->
    <div class="fld2">
      {#each knobs as f (f.uid)}
        <Field field={f} />
      {/each}
    </div>
  {/if}

  {#if select.desc}<p class="hint explain">{select.desc}</p>{/if}
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

  /* Card-state chip — OG .card-state: mono, small, ghost-gray ALWAYS (the
     text itself carries "standby" vs "running", the color does not). */
  .pattern-topline {
    display: flex;
    justify-content: flex-end;
  }
  .pattern-state {
    font-family: var(--mono);
    font-weight: var(--num-wght);
    font-variation-settings: 'wdth' 90;
    font-size: .64rem;
    letter-spacing: .04em;
    color: var(--tx-ghost);
    white-space: nowrap;
  }

  .pattern-head {
    display: flex;
  }

  /* Chrome (border/color/disabled/hover) comes from the global .og-btn /
     .og-btn.primary / .og-btn.running utilities — restating those here would
     fork the recipe. This only opts back into full width (og-btn is
     width:auto by design) and matches the OG's #patStartBtn letter-spacing +
     padding, which apply unconditionally, running or not. */
  .run-btn {
    width: 100%;
    justify-content: center;
    gap: 10px;
    padding: 12px;
    letter-spacing: .14em;
  }

  .run-dot {
    width: 10px; height: 10px;
    border-radius: 50%;
    background: var(--ink-faint);
  }
  /* .primary (idle) glows reality, .running glows warn — currentColor picks
     up whichever the button is currently wearing, so the dot never drifts
     out of sync with the border/text. */
  .run-btn.primary .run-dot,
  .run-btn.running .run-dot {
    background: currentColor;
    box-shadow: 0 0 6px currentColor;
  }
  @media (prefers-reduced-motion: no-preference) {
    .run-btn.running .run-dot { animation: pulse 1.6s ease-in-out infinite; }
  }
  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.45; }
  }

  /* OG .pat-grid/.pat-tile — bordered label tiles, responsive rather than
     the OG's fixed 4-column grid (dashboard cards here are user-resizable,
     the OG's Pattern card was not). */
  .pattern-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(72px, 1fr));
    gap: 7px;
  }
  .pat-tile {
    min-height: var(--tap);
    padding: 8px 4px;
    border-radius: var(--r-s);
    border: 1px solid var(--line-2);
    background: transparent;
    text-align: center;
    transition: border-color .15s, box-shadow .2s;
  }
  .pat-tile-label {
    display: block;
    font-family: var(--font);
    font-size: .66rem;
    letter-spacing: .04em;
    color: var(--tx-mut);
  }
  /* Active tile = intent purple: commanded/selected, not yet a measured
     truth — the same semantic intent already carries everywhere else. */
  .pat-tile.on {
    border-color: var(--intent);
    box-shadow: 0 0 12px rgba(var(--intent-deep-rgb), .25), inset 0 0 14px rgba(var(--intent-deep-rgb), .08);
  }
  .pat-tile.on .pat-tile-label { color: var(--intent); }
  .pat-tile:disabled { opacity: 0.5; cursor: not-allowed; }

  /* OG .fld2 — compact 2-col slider grid (mock r6, Pattern card). Field.svelte
     owns every other visual (label, chip, hairline slider); this only owns
     the grid rhythm and tightens the slider's vertical margin to the OG's
     fld2-specific value (the page-wide default is roomier). */
  .fld2 {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px 16px;
  }
  .fld2 :global(input[type='range']) {
    margin: 8px 0 2px;
  }

  .hint {
    margin: 0;
    color: var(--ink-dim);
    font-size: 0.78rem;
  }
</style>
