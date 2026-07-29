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
  // `accessory` is a layout snippet forwarded to the FIRST hero only — the
  // instrument hero's row carries the transport controls (OG .hero-row).
  // This strip stays ignorant of what the snippet contains.
  let { heroes, accessory = null } = $props();
</script>

{#if heroes && heroes.length}
  <div class="hero-strip">
    {#each heroes as hero, i (hero.id)}
      <div class="hero-slot" data-hero={hero.id}>
        <hero.component fields={hero.fields} accessory={i === 0 ? accessory : null} />
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

  /* ONE column at every width — do not reintroduce a multi-column grid here.
     The OG had no hero grid at all: every instrument was a full-bleed row
     (og style.css .hero-strip/.hero-row), and a hero is an instrument you read
     across, not a dashboard tile that tolerates being a third as wide. */
</style>
