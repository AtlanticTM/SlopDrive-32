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
  import { formatValue, unitOf, optionLabel, precisionFor } from '../model/format.js';

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
    : !tierOk ? 'this session is not authorised to change settings'
    : !maskOn ? 'the machine is refusing this setting right now'
    : ''
  );

  /**
   * Does this session's access tier permit writing here? The INTENT channel
   * advertises its own required access; we compare rather than assume, so a
   * watch-tier viewer greys the write plane instead of discovering it by NACK.
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
</script>

<div class="field" data-shadow={status} data-widget={field.widget}
     class:disabled={!enabled && !field.readOnly}
     class:readonly={field.readOnly}
     class:settled={sh && sh.settled}>

  <div class="field-head">
    <label class="field-label" for={field.uid}>
      {field.label}
      {#if field.flagBits.advanced}<span class="tag adv" title="Advanced setting">adv</span>{/if}
      {#if field.flagBits.restart_required}<span class="tag warn" title="Takes effect after restart">restart</span>{/if}
    </label>
    {#if field.widget !== WIDGET.toggle && field.widget !== WIDGET.bitfield}
      <output class="field-value" for={field.uid}>
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

  {:else if field.widget === WIDGET.toggle}
    <button id={field.uid} type="button" class="toggle"
            role="switch" aria-checked={!!value} disabled={!enabled}
            onclick={() => commit(value ? 0 : 1)}>
      <span class="toggle-track"><span class="toggle-thumb"></span></span>
      <span class="toggle-text">{field.options ? optionLabel(field, value) : (value ? 'on' : 'off')}</span>
    </button>

  {:else if field.widget === WIDGET.segmented}
    <div class="segmented" role="radiogroup" aria-labelledby={field.uid} id={field.uid}>
      {#each field.options as opt, i}
        <button type="button" role="radio" aria-checked={Number(value) === i}
                class:on={Number(value) === i} disabled={!enabled}
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
    <input id={field.uid} type="range" class="slider"
           min={field.min} max={field.max} step={step}
           value={value ?? field.min} disabled={!enabled}
           oninput={(e) => commit(Number(e.currentTarget.value))} />
    <div class="bounds"><span>{formatValue(field, field.min)}</span><span>{formatValue(field, field.max)}</span></div>

  {:else if field.widget === WIDGET.number}
    <input id={field.uid} type="number" class="numbox"
           min={field.min} max={field.max} step={step}
           value={value ?? ''} disabled={!enabled}
           onchange={(e) => commit(Number(e.currentTarget.value))} />

  {:else if field.widget === WIDGET.text}
    <input id={field.uid} type="text" class="textbox"
           value={value ?? ''} disabled={!enabled}
           onchange={(e) => commit(e.currentTarget.value)} />

  {:else if field.widget === WIDGET.secret}
    <!-- RFC-009.4: a secret's value NEVER appears in STATE. We can say whether
         one is set, and we can replace it. We can never show it. -->
    <input id={field.uid} type="password" class="textbox" placeholder={value ? '•••••• (set)' : 'not set'}
           disabled={!enabled} onchange={(e) => commit(e.currentTarget.value)} />
  {/if}

  {#if field.desc}<p class="field-desc">{field.desc}</p>{/if}

  {#if sh && sh.status === 'fault' && sh.error}
    <p class="field-error" role="status">refused: {sh.error}</p>
  {:else if reason && !field.readOnly}
    <p class="field-reason">{reason}</p>
  {/if}
</div>
