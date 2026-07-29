<script>
  /**
   * ThemePicker.svelte — accent theme swatches + the high-legibility toggle.
   *
   * Both controls here are BROWSER preferences, never device state: nothing
   * in this file is sent to the machine, and the ground-truth doctrine
   * (device echoes, shadow pending/confirmed lifecycle) does not apply to any
   * of it. Theme choice and hi-vis persist to localStorage only.
   *
   * Driven entirely by model/theme.js's THEMES table — never a hardcoded
   * list, so a new theme added there needs no change here. The custom pair
   * uses the same setCustomColors()/customColors() the rest of the app reads
   * through ac()/ACCENT, so a hand-picked pair repaints the canvas renderers
   * (heatmap, rail, scopes) identically to a preset.
   *
   * No props — reads/writes theme.js and localStorage directly. Wire it in
   * bare: <ThemePicker />
   */
  import { THEMES, applyTheme, currentThemeId, setCustomColors, customColors } from '../model/theme.js';

  let current = $state(currentThemeId());
  let custom = $state(customColors());

  function pick(id) {
    applyTheme(id);
    current = id;
  }

  function setRealityColor(e) {
    custom = { ...custom, reality: e.target.value };
    setCustomColors(custom.reality, custom.intent);
    current = 'custom';
  }
  function setIntentColor(e) {
    custom = { ...custom, intent: e.target.value };
    setCustomColors(custom.reality, custom.intent);
    current = 'custom';
  }
  function pickCustom() {
    setCustomColors(custom.reality, custom.intent);
    current = 'custom';
  }

  // Hi-vis: a browser preference persisted to localStorage `ui_hivis`
  // ('1'/'0'). main.js already restores this class onto <html> before first
  // paint (so the page never flashes the default weight) — this component
  // only needs to read that starting state and flip it from here on.
  let hivis = $state(
    typeof document !== 'undefined' && document.documentElement.classList.contains('hivis')
  );
  function toggleHivis() {
    hivis = !hivis;
    document.documentElement.classList.toggle('hivis', hivis);
    try { localStorage.setItem('ui_hivis', hivis ? '1' : '0'); } catch (e) { /* private mode: preference only */ }
  }

  // Terse instrument: hides `.explain` teaching copy on the hero instruments
  // (style.css owns the rule + the convention). Settings descriptions are
  // NOT affected — that split is the point, not an accident. Same
  // localStorage-before-first-paint mechanism as hivis (main.js restores).
  let terse = $state(
    typeof document !== 'undefined' && document.documentElement.classList.contains('terse')
  );
  function toggleTerse() {
    terse = !terse;
    document.documentElement.classList.toggle('terse', terse);
    try { localStorage.setItem('ui_terse', terse ? '1' : '0'); } catch (e) { /* private mode: preference only */ }
  }
</script>

<div class="theme-picker">
  <div class="swatches" role="list" aria-label="Accent theme">
    {#each THEMES as t (t.id)}
      <button
        type="button"
        class="swatch"
        class:active={current === t.id}
        role="listitem"
        aria-pressed={current === t.id}
        title={t.name + ' · reality ' + t.reality + ' / intent ' + t.intent}
        onclick={() => pick(t.id)}
      >
        <span class="dot" style="background:{t.reality};box-shadow:0 0 6px {t.reality}" aria-hidden="true"></span>
        <span class="dot" style="background:{t.intent}" aria-hidden="true"></span>
        <span class="name">{t.name}</span>
      </button>
    {/each}

    <div class="custom-swatch" role="listitem">
      <button
        type="button"
        class="swatch"
        class:active={current === 'custom'}
        aria-pressed={current === 'custom'}
        title="Your own accent pair"
        onclick={pickCustom}
      >
        <span class="dot" style="background:{custom.reality};box-shadow:0 0 6px {custom.reality}" aria-hidden="true"></span>
        <span class="dot" style="background:{custom.intent}" aria-hidden="true"></span>
        <span class="name">Custom</span>
      </button>
      <label class="color-input">
        <span class="sr-only">Custom reality accent color — measured truth</span>
        <input type="color" value={custom.reality} oninput={setRealityColor} aria-label="Reality accent color" />
      </label>
      <label class="color-input">
        <span class="sr-only">Custom intent accent color — commanded / window band</span>
        <input type="color" value={custom.intent} oninput={setIntentColor} aria-label="Intent accent color" />
      </label>
    </div>
  </div>

  <p class="hint">
    Accent colors only — safety amber/red are identical in every theme. Browser preference; the machine is unaffected.
  </p>

  <!-- High-legibility toggle: brighter, heavier text, hierarchy preserved.
       Browser-only; never touches device settings. -->
  <button type="button" class="hivis-toggle" aria-pressed={hivis} onclick={toggleHivis}>
    <span class="hv-dot" aria-hidden="true"></span>
    High legibility
    <span class="hv-state">{hivis ? 'on' : 'off'}</span>
  </button>

  <button type="button" class="hivis-toggle" aria-pressed={terse} onclick={toggleTerse}>
    <span class="hv-dot" aria-hidden="true"></span>
    Terse instruments
    <span class="hv-state">{terse ? 'on' : 'off'}</span>
  </button>
  <p class="hint">
    Terse hides usage hints and descriptions on the instrument cards once you know them. Settings pages always keep their explanations.
  </p>
</div>

<style>
  .theme-picker {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .swatches {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
  }

  .swatch {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    min-height: var(--tap);
    padding: 0 10px;
    border-radius: var(--r-s);
    background: var(--bg-card);
    border: 1px solid var(--line);
    color: var(--ink-dim);
    font-size: 12.5px;
    font-weight: 500;
    white-space: nowrap;
  }
  .swatch.active {
    color: var(--ink-hi);
    border-color: var(--reality);
    box-shadow: inset 0 0 0 1px var(--reality);
  }
  .swatch .dot {
    width: 9px;
    height: 9px;
    border-radius: 50%;
    flex: 0 0 auto;
  }
  .swatch .name {
    text-transform: uppercase;
    letter-spacing: .03em;
  }

  .custom-swatch {
    display: inline-flex;
    align-items: center;
    gap: 4px;
  }
  .color-input {
    display: inline-flex;
    min-width: var(--tap);
    min-height: var(--tap);
    align-items: center;
    justify-content: center;
    border-radius: var(--r-s);
    border: 1px solid var(--line);
    background: var(--bg-card);
    padding: 4px;
  }
  .color-input input[type='color'] {
    width: 100%;
    height: 100%;
    min-width: 28px;
    min-height: 28px;
    border: none;
    background: none;
    padding: 0;
    cursor: pointer;
  }

  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    padding: 0;
    margin: -1px;
    overflow: hidden;
    clip: rect(0, 0, 0, 0);
    white-space: nowrap;
    border: 0;
  }

  .hint {
    margin: 0;
    font-size: 11.5px;
    color: var(--ink-faint);
  }

  .hivis-toggle {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    align-self: flex-start;
    min-height: var(--tap);
    padding: 0 12px;
    border-radius: var(--r-s);
    background: var(--bg-card);
    border: 1px solid var(--line);
    color: var(--ink-dim);
    font-size: 12.5px;
    font-weight: 500;
  }
  .hv-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: var(--line-2);
    flex: 0 0 auto;
    transition: background .18s, box-shadow .18s;
  }
  .hivis-toggle[aria-pressed='true'] {
    color: var(--reality);
    border-color: color-mix(in srgb, var(--reality) 45%, var(--line));
  }
  .hivis-toggle[aria-pressed='true'] .hv-dot {
    background: var(--reality);
    box-shadow: var(--glow-reality);
  }
  .hv-state {
    color: var(--ink-faint);
    text-transform: uppercase;
    font-size: 10px;
    letter-spacing: .04em;
  }

  @media (prefers-reduced-motion: reduce) {
    .hv-dot { transition: none; }
  }
</style>
