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

  const step = $derived(field.step || (precisionFor(field) === 0 ? 1 : 0.01));

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
    <label class="field-label" for={field.uid}>
      {labelFor(field)}
      {#if field.flagBits.advanced}<span class="tag adv" title="Advanced setting">adv</span>{/if}
      {#if field.flagBits.restart_required}<span class="tag warn" title="Takes effect after restart">restart</span>{/if}
    </label>
    {#if field.widget !== WIDGET.toggle && field.widget !== WIDGET.bitfield}
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
    <div class="bounds"><span>{formatValue(field, field.min)}</span><span>{formatValue(field, field.max)}</span></div>

  {:else if field.widget === WIDGET.number}
    <input id={field.uid} type="number" class="og-num"
           min={field.min} max={field.max} step={step}
           value={value ?? ''} disabled={!enabled}
           onchange={(e) => commit(Number(e.currentTarget.value))} />

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

  {#if field.desc}<p class="field-desc">{field.desc}</p>{/if}

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

  .field {
    display: flex;
    flex-direction: column;
    gap: .5rem;
  }

  .field-head {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 8px;
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

  /* Ground-truth readout: same recess recipe as the editable .og-num value
     input (var(--screen), inset hairline, Martian Mono at a narrower width),
     written locally because this is an <output>, not an input — the global
     .og-num utility targets editable controls. Size/padding verified against
     the OG stylesheet's .field-val chip (.76rem, 1px 6px). */
  .field-value {
    display: inline-flex;
    align-items: center;
    font-family: var(--mono);
    font-variation-settings: 'wdth' 90;
    font-weight: var(--num-wght);
    font-size: .76rem;
    color: var(--tx-val);
    background: var(--screen);
    box-shadow: inset 0 0 0 1px var(--line-1);
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

  /* Free-text/secret value entries — og-num-like recess, left-aligned since
     the content isn't numeric (SSID strings, passphrases). */
  .value-input {
    background: var(--screen);
    box-shadow: inset 0 0 0 1px var(--line-1);
    color: var(--tx-val);
    font-family: var(--mono);
    font-variation-settings: 'wdth' 90;
    font-weight: var(--num-wght);
    border-radius: var(--r-s);
    border: none;
    padding: 6px 8px;
    text-align: left;
  }

  /* Layout-only: full-width controls, chrome untouched. */
  .field :is(input[type='range'], input[type='number'], input[type='text'],
              input[type='password'], select, .og-num, .value-input) {
    width: 100%;
    display: block;
  }

  .bounds {
    display: flex;
    justify-content: space-between;
    font-family: var(--mono);
    font-size: .7rem;
    color: var(--tx-ghost);
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
