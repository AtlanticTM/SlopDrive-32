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
  import { formatValue, unitOf, optionLabel, precisionFor, labelFor } from '../../model/format.js';

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

  function toggleRunning() {
    if (!runningEnabled) return;
    writeSetting(running, isRunning ? 0 : 1);
  }

  function chooseOption(i) {
    if (!selectEnabled || Number(selectVal) === i) return;
    writeSetting(select, i);
  }

  function knobStep(field) {
    return field.step || (precisionFor(field) === 0 ? 1 : 0.01);
  }

  function commitKnob(field, v) {
    if (!enabledOf(field)) return;
    writeSetting(field, v);
  }
</script>

<div class="hero pattern-hero">
  <div class="pattern-head">
    <button type="button" class="run-btn" class:on={isRunning}
            role="switch" aria-checked={isRunning} disabled={!runningEnabled}
            data-shadow={statusOf(running)}
            onclick={toggleRunning}>
      <span class="run-dot" aria-hidden="true"></span>
      <span class="run-text">{isRunning ? 'Running' : 'Stopped'}</span>
    </button>
  </div>

  {#if select.options && select.options.length}
    <div class="pattern-grid" role="radiogroup" aria-label={labelFor(select)}
         data-shadow={statusOf(select)}>
      {#each select.options as _opt, i}
        <button type="button" role="radio" aria-checked={Number(selectVal) === i}
                class:on={Number(selectVal) === i}
                disabled={!selectEnabled}
                onclick={() => chooseOption(i)}>
          {optionLabel(select, i)}
        </button>
      {/each}
    </div>
  {/if}

  {#if knobs.length}
    <div class="pattern-knobs">
      {#each knobs as f (f.uid)}
        {@const val = displayValue(f, sampleOf(f))}
        {@const en = enabledOf(f)}
        <div class="knob" class:disabled={!en} data-shadow={statusOf(f)}>
          <div class="knob-head">
            <span class="knob-label">{labelFor(f)}</span>
            <output class="mono">{formatValue(f, val)}<span class="unit">{unitOf(f)}</span></output>
          </div>
          <input type="range" class="knob-slider"
                 min={f.min} max={f.max} step={knobStep(f)}
                 value={val ?? f.min} disabled={!en}
                 aria-label={labelFor(f)}
                 oninput={(e) => commitKnob(f, Number(e.currentTarget.value))} />
        </div>
      {/each}
    </div>
  {/if}

  {#if select.desc}<p class="hint">{select.desc}</p>{/if}
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

  .pattern-head {
    display: flex;
  }

  .run-btn {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 0 18px;
    min-height: var(--tap);
    border-radius: var(--r);
    background: var(--bg-sunken);
    border: 1px solid var(--line);
    font-weight: 500;
    width: 100%;
    justify-content: center;
  }
  .run-btn.on {
    background: color-mix(in srgb, var(--reality) 22%, var(--bg-sunken));
    border-color: var(--reality);
    color: var(--reality);
  }
  .run-btn:disabled { opacity: 0.5; cursor: not-allowed; }

  .run-dot {
    width: 10px; height: 10px;
    border-radius: 50%;
    background: var(--ink-faint);
  }
  .run-btn.on .run-dot {
    background: var(--good);
    box-shadow: 0 0 6px color-mix(in srgb, var(--good) 70%, transparent);
  }
  @media (prefers-reduced-motion: no-preference) {
    .run-btn.on .run-dot { animation: pulse 1.6s ease-in-out infinite; }
  }
  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.45; }
  }

  .pattern-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(88px, 1fr));
    gap: 8px;
  }
  .pattern-grid button {
    min-height: var(--tap);
    padding: 6px 8px;
    border-radius: var(--r-s);
    background: var(--bg-sunken);
    border: 1px solid var(--line);
    font-size: 0.82rem;
    text-align: center;
    line-height: 1.2;
  }
  .pattern-grid button.on {
    background: color-mix(in srgb, var(--reality) 22%, var(--bg-sunken));
    border-color: var(--reality);
    color: var(--reality);
  }
  .pattern-grid button:disabled { opacity: 0.5; cursor: not-allowed; }

  .pattern-knobs {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .knob { display: flex; flex-direction: column; gap: 4px; padding: 2px; border-radius: var(--r-s); }
  .knob.disabled { opacity: 0.55; }

  .knob-head {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    font-size: 0.82rem;
  }
  .knob-label { color: var(--ink-dim); }
  .knob-head output { color: var(--reality); }
  .unit { color: var(--ink-dim); font-size: 0.75em; margin-left: 2px; }

  .knob-slider {
    width: 100%;
    height: var(--tap);
    accent-color: var(--reality);
  }

  .hint {
    margin: 0;
    color: var(--ink-dim);
    font-size: 0.78rem;
  }
</style>
