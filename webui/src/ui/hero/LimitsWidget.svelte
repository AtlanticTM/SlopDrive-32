<script>
  /**
   * LimitsWidget.svelte — the two kinematic ceilings: manual (user) and
   * machine-driven (input). CLAUDE.md is explicit that these are CEILINGS, never
   * targets, so every descriptor's `desc` prose is surfaced rather than dropped
   * on the floor the way a bare slider would.
   *
   * The input-set group is opportunistic: a machine with no machine-driven
   * motion source simply never annotates those roles, and the whole second
   * group disappears rather than showing three permanently-empty sliders.
   */
  import { machine } from '../../model/machine.svelte.js';
  import { isFieldEnabled } from '../../model/settings.js';
  import { writeSetting, displayValue, statusOf } from '../../model/shadow.svelte.js';
  import { formatValue, unitOf, precisionFor } from '../../model/format.js';

  let { fields } = $props();
  // Read through the prop rather than destructuring once — heroes.js hands us
  // a fresh `fields` object whenever the catalog rebuilds.
  const userSpeed = $derived(fields.userSpeed);
  const userAccel = $derived(fields.userAccel);
  const inputSpeed = $derived(fields.inputSpeed);
  const inputAccel = $derived(fields.inputAccel);
  const inputJerk = $derived(fields.inputJerk);

  function sampleOf(f) { return f ? machine.samples[f.channelId] : undefined; }

  function enabledOf(f) {
    if (!f || f.readOnly) return false;
    if (!isFieldEnabled(f, sampleOf(f))) return false;
    if (machine.link.phase !== 'live') return false;
    const e = machine.catalog.entries.find((x) => x.id === f.writeChannel);
    if (!e) return false;
    return (machine.link.roles | 0) >= (e.access | 0);
  }

  function stepOf(f) {
    return f.step || (precisionFor(f) === 0 ? 1 : 0.01);
  }

  function commit(f, v) {
    if (!enabledOf(f)) return;
    writeSetting(f, v);
  }

  const userKnobs = $derived([userSpeed, userAccel].filter((f) => f != null));
  const inputKnobs = $derived(
    [inputSpeed, inputAccel, inputJerk].filter((f) => f != null)
  );
</script>

<div class="hero limits-hero">
  <section class="limit-group">
    <h3 class="group-title">Manual limits <span class="group-sub">ceiling, not target</span></h3>
    <div class="limit-list">
      {#each userKnobs as f (f.uid)}
        {@const val = displayValue(f, sampleOf(f))}
        {@const en = enabledOf(f)}
        <div class="limit-row" class:disabled={!en} data-shadow={statusOf(f)}>
          <div class="limit-head">
            <span class="limit-label">{f.label}</span>
            <output class="mono">{formatValue(f, val)}<span class="unit">{unitOf(f)}</span></output>
          </div>
          <input type="range" class="limit-slider"
                 min={f.min} max={f.max} step={stepOf(f)}
                 value={val ?? f.min} disabled={!en}
                 aria-label={f.label}
                 oninput={(e) => commit(f, Number(e.currentTarget.value))} />
          {#if f.desc}<p class="limit-desc">{f.desc}</p>{/if}
        </div>
      {/each}
    </div>
  </section>

  {#if inputKnobs.length}
    <section class="limit-group">
      <h3 class="group-title">Machine-driven limits <span class="group-sub">ceiling, not target</span></h3>
      <div class="limit-list">
        {#each inputKnobs as f (f.uid)}
          {@const val = displayValue(f, sampleOf(f))}
          {@const en = enabledOf(f)}
          <div class="limit-row" class:disabled={!en} data-shadow={statusOf(f)}>
            <div class="limit-head">
              <span class="limit-label">{f.label}</span>
              <output class="mono">{formatValue(f, val)}<span class="unit">{unitOf(f)}</span></output>
            </div>
            <input type="range" class="limit-slider"
                   min={f.min} max={f.max} step={stepOf(f)}
                   value={val ?? f.min} disabled={!en}
                   aria-label={f.label}
                   oninput={(e) => commit(f, Number(e.currentTarget.value))} />
            {#if f.desc}<p class="limit-desc">{f.desc}</p>{/if}
          </div>
        {/each}
      </div>
    </section>
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
    gap: calc(var(--gap) * 1.25);
  }

  .limit-group {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .group-title {
    margin: 0;
    font-size: 0.85rem;
    font-weight: 500;
    color: var(--ink);
    display: flex;
    align-items: baseline;
    gap: 8px;
  }
  .group-sub {
    font-size: 0.68rem;
    font-weight: 400;
    color: var(--ink-faint);
    text-transform: uppercase;
    letter-spacing: 0.03em;
  }

  .limit-list {
    display: flex;
    flex-direction: column;
    gap: 12px;
  }

  .limit-row {
    display: flex;
    flex-direction: column;
    gap: 4px;
    padding: 2px;
    border-radius: var(--r-s);
  }
  .limit-row.disabled { opacity: 0.55; }

  .limit-head {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    font-size: 0.85rem;
  }
  .limit-label { color: var(--ink-dim); }
  .limit-head output { color: var(--reality); }
  .unit { color: var(--ink-dim); font-size: 0.75em; margin-left: 2px; }

  .limit-slider {
    width: 100%;
    height: var(--tap);
    accent-color: var(--reality);
  }

  .limit-desc {
    margin: 0;
    color: var(--ink-faint);
    font-size: 0.74rem;
  }
</style>
