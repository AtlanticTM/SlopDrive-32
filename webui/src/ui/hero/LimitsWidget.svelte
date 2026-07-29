<script>
  /**
   * LimitsWidget.svelte — the two kinematic ceilings: manual (user) and
   * machine-driven (input). Renders each knob through Field.svelte (OG .fld2
   * compact grid, same recipe as PatternWidget) instead of a hand-rolled
   * slider — one slider aesthetic for the whole app, one home for a field's
   * description (behind its own ⓘ, not restated here).
   *
   * The input-set group is opportunistic: a machine with no machine-driven
   * motion source simply never annotates those roles, and the whole second
   * group disappears rather than showing three permanently-empty sliders.
   */
  import Field from '../Field.svelte';

  let { fields } = $props();
  // Read through the prop rather than destructuring once — heroes.js hands us
  // a fresh `fields` object whenever the catalog rebuilds.
  const userSpeed = $derived(fields.userSpeed);
  const userAccel = $derived(fields.userAccel);
  const inputSpeed = $derived(fields.inputSpeed);
  const inputAccel = $derived(fields.inputAccel);
  const inputJerk = $derived(fields.inputJerk);

  const userKnobs = $derived([userSpeed, userAccel].filter((f) => f != null));
  const inputKnobs = $derived(
    [inputSpeed, inputAccel, inputJerk].filter((f) => f != null)
  );
</script>

<div class="hero limits-hero">
  <section class="limit-group">
    <h3 class="group-title">Manual limits <span class="group-sub">ceiling, not target</span></h3>
    <div class="fld2">
      {#each userKnobs as f (f.uid)}
        <Field field={f} />
      {/each}
    </div>
  </section>

  {#if inputKnobs.length}
    <section class="limit-group">
      <h3 class="group-title">Machine-driven limits <span class="group-sub">ceiling, not target</span></h3>
      <div class="fld2">
        {#each inputKnobs as f (f.uid)}
          <Field field={f} />
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

  /* OG .fld2 — compact 2-col slider grid (mock r6, Pattern card), same recipe
     as PatternWidget's. Field.svelte owns every other visual (label, chip,
     hairline slider); this only owns the grid rhythm and tightens the
     slider's vertical margin to the OG's fld2-specific value. */
  .fld2 {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px 16px;
  }
  .fld2 :global(input[type='range']) {
    margin: 8px 0 2px;
  }
</style>
