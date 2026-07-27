<script>
  /**
   * HeroStrip.svelte — thin layout container for hero widgets.
   *
   * Knows nothing about what a "hero" is beyond {id, component, fields}: the
   * registry in heroes.js decides which components exist and what roles they
   * claimed, this file only arranges whatever it is handed. If a machine
   * publishes none of the roles any hero wants, `heroes` arrives empty and this
   * renders nothing — a bare settings page is the correct, honest result for
   * that machine, not an error state.
   */
  let { heroes } = $props();
</script>

{#if heroes && heroes.length}
  <div class="hero-strip">
    {#each heroes as hero (hero.id)}
      <div class="hero-slot" data-hero={hero.id}>
        <hero.component fields={hero.fields} />
      </div>
    {/each}
  </div>
{/if}

<style>
  .hero-strip {
    display: grid;
    grid-template-columns: 1fr;
    gap: var(--gap);
    margin: var(--gap) 0;
  }

  .hero-slot {
    min-width: 0; /* let sliders/rails shrink instead of forcing horizontal scroll */
  }

  /* The first registered hero (the rail, when present) reads best as a full-
     width instrument; the rest are happy sharing a row once there is room. */
  @media (min-width: 720px) {
    .hero-strip { grid-template-columns: repeat(2, 1fr); }
    .hero-slot:first-child { grid-column: 1 / -1; }
  }

  @media (min-width: 1000px) {
    .hero-strip { grid-template-columns: repeat(3, 1fr); }
  }
</style>
