<script>
  /**
   * Field.svelte — renders ONE catalog field, whatever it is.
   *
   * This component has never heard of a SlopDrive-32. It is handed a field
   * descriptor produced by buildSettingsModel() and picks a control from the
   * field's TYPE and its RFC-009 constraints. A machine that ships a setting we
   * have never seen renders here correctly, with the right bounds, the right
   * step, the right unit and the right option labels, because all of those came
   * off the wire.
   *
   * GROUND TRUTH: the displayed value comes from shadow.displayValue(), which
   * returns the device's reported value except while a write of OUR OWN is in
   * flight. The `data-shadow` attribute carries the write's lifecycle so CSS can
   * show unconfirmed state without this component knowing what amber means.
   */
  import { machine } from '../model/machine.svelte.js';
  import { WIDGET, isFieldEnabled } from '../model/settings.js';
  import { writeSetting, displayValue, statusOf, shadowOf } from '../model/shadow.svelte.js';
  import { formatValue, unitOf, optionLabel, precisionFor, labelFor } from '../model/format.js';

  let { field } = $props();

  // ⓘ affordance state (OG density doctrine — a description is NOT printed
  // inline by default, it lives behind a per-field toggle). Local, default
  // closed; resets whenever this component instance changes field.
  let descOpen = $state(false);
  const descId = $derived(field.uid + '-desc');

  const sample = $derived(machine.samples[field.channelId]);
  const value = $derived(displayValue(field, sample));
  const status = $derived(statusOf(field));
  const sh = $derived(shadowOf(field));

  // Three independent reasons a control may be unusable, and they are NOT
  // interchangeable — the operator needs to know which one applies.
  const maskOn = $derived(isFieldEnabled(field, sample));
  const linkUp = $derived(machine.link.phase === 'live');
  const tierOk = $derived(canWrite(field));
  const enabled = $derived(!field.readOnly && maskOn && linkUp && tierOk);

  const reason = $derived(
    field.readOnly ? 'read-only — the machine reports this, it is not a setting'
    : !linkUp ? 'no hub link'
    : !tierOk ? 'this session is not authorized to change settings'
    : !maskOn ? 'the machine is refusing this setting right now'
    : ''
  );

  /**
   * Does this session's access tier permit writing here? The INTENT channel
   * advertises its own required access; we compare rather than assume, so a
   * watch-tier viewer grays the write plane instead of discovering it by NACK.
   */
  function canWrite(f) {
    if (f.readOnly) return false;
    const e = machine.catalog.entries.find((x) => x.id === f.writeChannel);
    if (!e) return false;
    return (machine.link.roles | 0) >= (e.access | 0);
  }

  function commit(v) {
    if (!enabled) return;
    writeSetting(field, v);
  }

  /**
   * Move one step-sized tick and stay inside the published bounds. Rounded off
   * the float grid because repeated 0.1 nudges otherwise drift into
   * 0.30000000000000004 and write that to the machine.
   */
  function nudge(dir) {
    const base = Number(value);
    let next = (isFinite(base) ? base : (field.min ?? 0)) + dir * step;
    if (field.min != null) next = Math.max(field.min, next);
    if (field.max != null) next = Math.min(field.max, next);
    commit(Math.round(next * 1e6) / 1e6);
  }

  const step = $derived(field.step || (precisionFor(field) === 0 ? 1 : 0.01));

  // A control that prints its own value owns the whole row; a chip repeating it
  // is the duplicate-truth the density pass killed. So the chip is a WHITELIST,
  // not an exclusion list — it has to earn the row by carrying something the
  // control cannot: a slider has no numerals, a readout has no control at all,
  // and a bare number input has nowhere to put a unit.
  const showValueChip = $derived(
    field.widget === WIDGET.slider
    || field.widget === WIDGET.readout
    || (field.widget === WIDGET.stepper && unitOf(field) !== '')
  );

  // Readout archetype (OG "Power card" bar recipe): a read-only numeric with
  // published bounds gets a thin proportional bar under the value, same as
  // every bounded live measurement in the OG right-hand instrument column.
  // A readout with no bounds (a status string, an unbounded counter) gets no
  // bar — there is no range to show it against.
  const hasBounds = $derived(
    field.widget === WIDGET.readout && field.min != null && field.max != null && field.max > field.min
  );
  const boundedFrac = $derived.by(() => {
    if (!hasBounds) return 0;
    const n = Number(value);
    if (!isFinite(n)) return 0;
    return Math.max(0, Math.min(1, (n - field.min) / (field.max - field.min)));
  });
</script>

<div class="field" data-shadow={status} data-widget={field.widget}
     class:disabled={!enabled && !field.readOnly}
     class:readonly={field.readOnly}
     class:settled={sh && sh.settled}>

  <div class="field-head">
    <span class="field-label-group">
      <label class="field-label" for={field.uid}>
        {labelFor(field)}
        {#if field.advanced}<span class="tag adv" title="Advanced setting">adv</span>{/if}
        {#if field.flagBits.restart_required}<span class="tag warn" title="Takes effect after restart">restart</span>{/if}
      </label>
      {#if field.desc}
        <button type="button" class="info" aria-expanded={descOpen} aria-controls={descId}
                onclick={() => (descOpen = !descOpen)}>
          <!-- The OG `i-info` glyph verbatim (core/ui.js sprite + its 24-unit
               stroke-2 round-cap wrapper), at the OG's own in-field metrics.
               Do not redraw it by hand on a smaller viewBox: the dot's ink
               (7..9) and the stem's (11..17) straddle cy 12 exactly, and a
               hand-fitted copy loses that balance while looking correct in
               the source. -->
          <svg viewBox="0 0 24 24" width="11" height="11" aria-hidden="true"
               fill="none" stroke="currentColor" stroke-width="2"
               stroke-linecap="round" stroke-linejoin="round">
            <circle cx="12" cy="12" r="10"/>
            <path d="M12 16v-4"/>
            <path d="M12 8h.01"/>
          </svg>
          <span class="sr-only">{descOpen ? 'Hide' : 'Show'} description</span>
        </button>
      {/if}
    </span>
    {#if showValueChip}
      <output class="field-value" class:readout={field.widget === WIDGET.readout} for={field.uid}>
        {#if field.options}
          {optionLabel(field, value)}
        {:else}
          {formatValue(field, value)}<span class="unit">{unitOf(field)}</span>
        {/if}
      </output>
    {/if}
  </div>

  {#if field.widget === WIDGET.readout}
    <!-- No control at all. A field with no setting_key is effective truth and
         must never render as something you can push. -->
    {#if hasBounds}
      <div class="readout-bar" aria-hidden="true">
        <div class="readout-bar-fill" style="width: {boundedFrac * 100}%"></div>
      </div>
    {/if}

  {:else if field.widget === WIDGET.indicator}
    <!-- §8.4 indicator: a status lamp is NEVER the sole carrier of the fact,
         so every lamp is paired with its state in words (§13). Read-only by
         construction — this branch draws no control at all. -->
    <div class="lamps" id={field.uid}>
      {#if field.bits}
        {#each field.bits as bitName, b}
          {#if bitName}
            <span class="lamp" class:lit={((value | 0) & (1 << b)) !== 0}>
              <i aria-hidden="true"></i>{bitName}
            </span>
          {/if}
        {/each}
      {:else}
        <span class="lamp" class:lit={!!value}>
          <i aria-hidden="true"></i>{field.options ? optionLabel(field, value) : (value ? 'on' : 'off')}
        </span>
      {/if}
    </div>

  {:else if field.widget === WIDGET.toggle}
    <div class="toggle-row">
      <label class="og-switch" class:is-disabled={!enabled}>
        <input type="checkbox" id={field.uid}
               role="switch" aria-checked={!!value} checked={!!value} disabled={!enabled}
               onchange={(e) => commit(e.currentTarget.checked ? 1 : 0)} />
        <span class="track"></span>
      </label>
      <span class="toggle-text">{field.options ? optionLabel(field, value) : (value ? 'on' : 'off')}</span>
    </div>

  {:else if field.widget === WIDGET.segmented}
    <div class="og-seg" role="radiogroup" aria-labeledby={field.uid} id={field.uid}>
      {#each field.options as opt, i}
        <button type="button" role="radio" aria-checked={Number(value) === i}
                class:active={Number(value) === i} disabled={!enabled}
                onclick={() => commit(i)}>{opt || i}</button>
      {/each}
    </div>

  {:else if field.widget === WIDGET.select}
    <select id={field.uid} disabled={!enabled}
            onchange={(e) => commit(Number(e.currentTarget.value))}>
      {#each field.options as opt, i}
        <option value={i} selected={Number(value) === i}>{opt || i}</option>
      {/each}
    </select>

  {:else if field.widget === WIDGET.bitfield}
    <div class="bitfield" id={field.uid}>
      {#each field.bits as bitName, b}
        {#if bitName}
          <label class="bit">
            <input type="checkbox" disabled={!enabled}
                   checked={((value | 0) & (1 << b)) !== 0}
                   onchange={(e) => commit(e.currentTarget.checked
                     ? ((value | 0) | (1 << b))
                     : ((value | 0) & ~(1 << b)))} />
            <span>{bitName}</span>
          </label>
        {/if}
      {/each}
    </div>

  {:else if field.widget === WIDGET.slider}
    <input id={field.uid} type="range"
           min={field.min} max={field.max} step={step}
           value={value ?? field.min} disabled={!enabled}
           oninput={(e) => commit(Number(e.currentTarget.value))} />
    <!-- No printed min…max caption (OG density doctrine — the slider's own
         extent plus the value chip already carry the bounds; a bounds line
         under every slider is exactly the "flat wall of gray text" the OG
         never had). -->

  {:else if field.widget === WIDGET.stepper}
    <!-- §8.4 stepper: typeable, and increments in step-sized ticks. The typing
         half is the native input; the increment half CANNOT be, because the
         global sheet strips native spinners on purpose. Hence the flanking
         nudges — without them this archetype is just a text box. -->
    <div class="stepper">
      <button type="button" disabled={!enabled} aria-label="decrease {labelFor(field)}"
              onclick={() => nudge(-1)}>&minus;</button>
      <input id={field.uid} type="number" class="og-num"
             min={field.min} max={field.max} step={step}
             value={value ?? ''} disabled={!enabled}
             onchange={(e) => commit(Number(e.currentTarget.value))} />
      <button type="button" disabled={!enabled} aria-label="increase {labelFor(field)}"
              onclick={() => nudge(1)}>+</button>
    </div>

  {:else if field.widget === WIDGET.text}
    <input id={field.uid} type="text" class="value-input"
           value={value ?? ''} disabled={!enabled}
           onchange={(e) => commit(e.currentTarget.value)} />

  {:else if field.widget === WIDGET.secret}
    <!-- RFC-009.4: a secret's value NEVER appears in STATE. We can say whether
         one is set, and we can replace it. We can never show it. -->
    <input id={field.uid} type="password" class="value-input" placeholder={value ? '•••••• (set)' : 'not set'}
           disabled={!enabled} onchange={(e) => commit(e.currentTarget.value)} />
  {/if}

  {#if field.desc && descOpen}<p class="field-desc" id={descId}>{field.desc}</p>{/if}

  {#if sh && sh.status === 'fault' && sh.error}
    <p class="field-error" role="status">refused: {sh.error}</p>
  {:else if reason && !field.readOnly}
    <p class="field-reason">{reason}</p>
  {/if}
</div>

<style>
  /* Layout only below — track/thumb/chevron chrome for input[type=range] and
     select, plus .og-seg/.og-switch/.og-num visuals, are owned globally
     (style.css) so every instrument control reads identically. This block
     places things and dresses the parts the global sheet does not own:
     the ground-truth value readout, free-text/secret inputs, labels, tags,
     bitfield rows and the toggle's paired text. */

  /* OG .fld cadence: label/chip/slider read as ONE instrument row — tightened
     from .5rem so the control sits close under its head instead of floating
     in its own paragraph-sized band. */
  .field {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .field-head {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 8px;
  }

  /* Groups the label with its ⓘ toggle so field-head's space-between still
     splits into exactly two things: this group, and the value chip. */
  .field-label-group {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  /* Slider row tightened to the OG's compact cadence (.fld2 input[type=range]
     margin, verified against og-ref/style.css) instead of the global 12px 0 —
     part of reading as one instrument row with its label/chip. */
  .field input[type='range'] {
    margin: 8px 0 2px;
  }

  /* Quiet label voice — same recipe as the hero numerals' .hn-label. Size
     matches the OG stylesheet's base `label` rule (.76rem, Chakra Petch 500,
     tx-mut) verified against og-ref/style.css. */
  .field-label {
    font-family: var(--font);
    font-size: .76rem;
    font-weight: 500;
    color: var(--tx-mut);
    text-transform: lowercase;
    letter-spacing: .04em;
  }

  .tag {
    display: inline-block;
    margin-left: 6px;
    padding: 1px 5px;
    font-size: .62rem;
    font-weight: 500;
    text-transform: uppercase;
    letter-spacing: .04em;
    border-radius: var(--r-s);
    vertical-align: middle;
  }
  .tag.adv {
    color: var(--tx-mut);
    background: var(--bg-sunken);
    box-shadow: inset 0 0 0 1px var(--line-2);
  }
  .tag.warn {
    color: var(--warn);
    background: rgba(245, 185, 77, .12);
    box-shadow: inset 0 0 0 1px rgba(245, 185, 77, .4);
  }

  /* ⓘ description toggle — the OG .info affordance (og-ref/style.css: base
     .info box + its .label-row 18px/11px in-field variant, since this button
     rides beside a field label rather than a card-head). The OG's own hover
     state only brightens its floating .tip popover, not the button itself;
     the border-brightens-on-hover idiom here is the one every other outlined
     icon button in this sheet already uses (.og-btn:hover, .og-seg button:hover). */
  .info {
    position: relative;
    width: 18px;
    height: 18px;
    flex: 0 0 auto;
    display: inline-grid;
    place-items: center;
    border-radius: var(--r-s);
    border: 1px solid var(--line-2);
    background: transparent;
    color: var(--tx-mut);
    font-size: .62rem;
    line-height: 1;
    cursor: pointer;
    transition: border-color .12s, color .12s;
  }
  .info:hover {
    border-color: var(--line-4);
    color: var(--tx);
  }
  .info[aria-expanded='true'] {
    border-color: var(--reality);
    color: var(--reality);
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

  /* Ground-truth readout: same recess recipe as the editable .og-num value
     input (var(--screen), inset shadow + hairline border, Martian Mono at a
     narrower width), written locally because this is an <output>, not an
     input — the global .og-num utility targets editable controls. Recess
     verified verbatim against the OG stylesheet's .num (inset 0 2px 5px
     rgba(0,0,0,.6)) — the old flat 1px inset ring read shallow next to it.
     Size/padding verified against the OG's .field-val chip (.76rem, 1px 6px). */
  .field-value {
    display: inline-flex;
    align-items: center;
    font-family: var(--mono);
    font-variation-settings: 'wdth' 90;
    font-weight: var(--num-wght);
    font-size: .76rem;
    color: var(--tx-val);
    background: var(--screen);
    box-shadow: inset 0 2px 5px rgba(0, 0, 0, .6);
    border: 1px solid var(--line-1);
    border-radius: var(--r-s);
    padding: 1px 6px;
  }
  /* Unit suffix — OG's .field-val em: Chakra Petch (not mono), tx-ghost,
     .64rem literal (not a relative em) so it stays legible at the chip's
     smallest sizes. */
  .field-value .unit {
    margin-left: 3px;
    font-family: var(--font);
    font-weight: 500;
    font-size: .64rem;
    color: var(--tx-ghost);
  }

  /* Read-only bounded numeric (readout archetype) — the OG "Power card"
     instrument voice: the value glows reality-blue like a live measurement
     instead of sitting quiet like a settings chip. */
  .field-value.readout {
    color: var(--reality);
    text-shadow: 0 0 8px rgba(var(--reality-rgb), .35);
  }

  /* Thin proportional bar under a bounded readout — OG Power-card meter
     recipe (core/meter.js's .mtr-track/.mtr-fill), simplified to the plain
     no-hazard case: this is a generic reading, not a bus-voltage instrument. */
  .readout-bar {
    height: 2px;
    margin-top: 2px;
    background: var(--line-2);
    border-radius: 1px;
    overflow: hidden;
  }
  .readout-bar-fill {
    height: 100%;
    width: 0%;
    background: var(--reality);
    box-shadow: 0 0 6px rgba(var(--reality-rgb), .4);
    transition: width .4s cubic-bezier(.3, .7, .3, 1);
  }

  /* Free-text/secret value entries — same recess as .field-value/.og-num
     (OG .num verbatim), left-aligned since the content isn't numeric (SSID
     strings, passphrases). */
  .value-input {
    background: var(--screen);
    box-shadow: inset 0 2px 5px rgba(0, 0, 0, .6);
    border: 1px solid var(--line-1);
    color: var(--tx-val);
    font-family: var(--mono);
    font-variation-settings: 'wdth' 90;
    font-weight: var(--num-wght);
    border-radius: var(--r-s);
    padding: 6px 8px;
    text-align: left;
  }

  /* Layout-only: full-width controls, chrome untouched. */
  .field :is(input[type='range'], input[type='number'], input[type='text'],
              input[type='password'], select, .og-num, .value-input) {
    width: 100%;
    display: block;
  }

  .toggle-row {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .toggle-text {
    font-family: var(--mono);
    font-size: .85rem;
    color: var(--tx-val);
  }
  /* Fallback dimming in case the global .og-switch does not itself gate on
     the checkbox's disabled state; harmless if it already does. */
  .og-switch.is-disabled {
    opacity: .45;
    cursor: not-allowed;
  }

  /* Stepper (indicator's writable neighbor): a compact centered value flanked
     by nudges. The value box does NOT stretch — a 9-position control reading
     as a full-bleed text field is what made it look emptier than the slider it
     replaced. */
  .stepper {
    display: flex;
    align-items: stretch;
    gap: 6px;
  }
  .stepper input {
    flex: 1 1 auto;
    min-width: 0;
    text-align: center;
  }
  .stepper button {
    flex: 0 0 34px;
    border-radius: var(--r-s);
    border: 1px solid var(--line-2);
    background: var(--bg-sunken);
    color: var(--tx-mut);
    font-family: var(--mono);
    font-size: .9rem;
    line-height: 1;
    cursor: pointer;
    transition: border-color .12s, color .12s;
  }
  .stepper button:hover:not(:disabled) {
    border-color: var(--line-4);
    color: var(--tx);
  }
  .stepper button:disabled {
    opacity: .45;
    cursor: not-allowed;
  }

  /* Status lamps (indicator archetype). Same wrap cadence as .bitfield so a
     read-only status byte and a writable one read as the same kind of thing;
     the lamp is a dot rather than a checkbox because nothing here is pushable.
     The lit dot borrows the Power-card instrument glow — a lamp is a live
     measurement, not a settings chip. */
  .lamps {
    display: flex;
    flex-wrap: wrap;
    gap: 6px 14px;
  }
  .lamp {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: .78rem;
    color: var(--tx-mut);
  }
  .lamp i {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: var(--line-2);
    box-shadow: inset 0 1px 2px rgba(0, 0, 0, .6);
  }
  .lamp.lit {
    color: var(--tx-val);
  }
  .lamp.lit i {
    background: var(--reality);
    box-shadow: 0 0 6px rgba(var(--reality-rgb), .55);
  }

  .bitfield {
    display: flex;
    flex-wrap: wrap;
    gap: 10px 16px;
  }
  .bitfield .bit {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: .85rem;
    color: var(--tx);
  }
  .bitfield input[type='checkbox'] {
    width: 16px;
    height: 16px;
    accent-color: var(--reality);
  }

  .field-desc {
    font-size: .72rem;
    color: var(--tx-mut);
  }
</style>
