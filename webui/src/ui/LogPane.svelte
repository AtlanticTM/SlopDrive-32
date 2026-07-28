<script>
  /**
   * LogPane.svelte — device log, motion anomalies, session events.
   *
   * All three rings are decoded EVENT frames (machine.events.*). Per SPEC 8.8,
   * unknown things render generically rather than being dropped, so this
   * component treats an event's `body` as an open bag of fields: it looks up
   * each key against the CHANNEL'S OWN catalog schema at render time (for an
   * `options`-typed field that turns a raw number into the label the device
   * chose) and falls through to a plain "key = value" chip for anything it
   * does not recognize. Nothing here assumes a device-specific field name —
   * the one exception is the spec-core log channel's `level`/`tag`/`message`
   * fields, which are fixed by the SlopSync *library* (lib/slopsync/), not by
   * this device's own catalog, the same way every conforming hub's session
   * and safety event channels share their kind vocabulary.
   */
  import { machine } from '../model/machine.svelte.js';
  import {
    CH, SESSION_EVENT_KIND, SAFETY_EVENT_KIND, LOG_EVENT_KIND, LOG_LEVEL_NAME,
  } from '../core/slopsync/index.js';
  import { optionLabel, formatValue, formatWithUnit } from '../model/format.js';

  let tab = $state('log'); // log | anomaly | session

  const lists = $derived({
    log: machine.events.log,
    anomaly: machine.events.anomaly,
    session: machine.events.session,
  });
  const currentList = $derived(lists[tab] || []);

  // ---- generic body decoding -----------------------------------------------

  function invert(obj) {
    const out = {};
    for (const [k, v] of Object.entries(obj)) out[v] = k;
    return out;
  }
  const KIND_NAMES = {
    [CH.SESSION_EVENTS]: invert(SESSION_EVENT_KIND),
    [CH.SAFETY_EVENTS]: invert(SAFETY_EVENT_KIND),
    [CH.LOG]: invert(LOG_EVENT_KIND),
  };

  /** Decode evt.body generically via the emitting channel's OWN catalog schema. */
  function bodyFields(evt) {
    if (!evt || !evt.body) return [];
    const entry = machine.catalog.entries.find((e) => e.id === evt.channel);
    const byName = new Map(((entry && entry.schema) || []).map((f) => [f.name, f]));
    return Object.keys(evt.body).map((k) => {
      const v = evt.body[k];
      const field = byName.get(k);
      let display;
      if (field && field.options) display = optionLabel(field, v);
      else if (field) display = formatWithUnit(field, v);
      else if (typeof v === 'number') display = formatValue(null, v);
      else display = String(v);
      return { key: k, raw: v, display };
    });
  }

  function levelName(raw) {
    return (typeof raw === 'number' && LOG_LEVEL_NAME[raw]) ? LOG_LEVEL_NAME[raw] : null;
  }

  /** Every top-level field of an event object, generically, for the session tab. */
  function topFields(evt) {
    const skip = new Set(['at', 'channelName', 'channel', 'body']);
    const kindMap = evt.channel != null ? KIND_NAMES[evt.channel] : null;
    const out = [];
    for (const k of Object.keys(evt)) {
      if (skip.has(k)) continue;
      const v = evt[k];
      let display;
      if (k === 'kind' && typeof v === 'number' && kindMap && kindMap[v]) display = kindMap[v];
      else if (v == null) display = '--';
      else if (typeof v === 'object') display = JSON.stringify(v);
      else display = String(v);
      out.push({ key: k, display });
    }
    out.push(...bodyFields(evt));
    return out;
  }

  function timeOf(evt) {
    try { return new Date(evt.at).toLocaleTimeString(); } catch (e) { return '--'; }
  }

  // ---- auto-scroll-if-at-bottom --------------------------------------------

  let containerEl = $state(null);
  let atBottom = $state(true);

  function onScroll() {
    if (!containerEl) return;
    atBottom = (containerEl.scrollHeight - containerEl.scrollTop - containerEl.clientHeight) < 32;
  }

  $effect(() => {
    // Reactive deps: list contents changing, and which tab is showing.
    const n = currentList.length;
    void tab;
    if (containerEl && atBottom) {
      const el = containerEl;
      queueMicrotask(() => { el.scrollTop = el.scrollHeight; });
    }
  });

  function selectTab(t) {
    tab = t;
    atBottom = true;
  }
</script>

<div class="logpane">
  <div class="tabs" role="tablist" aria-label="Event feed">
    <button role="tab" aria-selected={tab === 'log'} class:on={tab === 'log'} onclick={() => selectTab('log')}>
      Log <span class="count">{machine.events.log.length}</span>
    </button>
    <button role="tab" aria-selected={tab === 'anomaly'} class:on={tab === 'anomaly'} onclick={() => selectTab('anomaly')}>
      Anomalies <span class="count">{machine.events.anomaly.length}</span>
    </button>
    <button role="tab" aria-selected={tab === 'session'} class:on={tab === 'session'} onclick={() => selectTab('session')}>
      Session <span class="count">{machine.events.session.length}</span>
    </button>
  </div>

  <div class="feed" bind:this={containerEl} onscroll={onScroll} role="log" aria-live="polite">
    {#if !currentList.length}
      <p class="empty">Nothing yet.</p>
    {:else if tab === 'log'}
      {#each currentList as evt, i (i + '-' + evt.at)}
        {@const fields = bodyFields(evt)}
        {@const lvlField = fields.find((f) => f.key === 'level')}
        {@const lvl = lvlField ? levelName(lvlField.raw) : null}
        {@const msgField = fields.find((f) => f.key === 'message')}
        {@const tagField = fields.find((f) => f.key === 'tag')}
        {@const rest = fields.filter((f) => f.key !== 'level' && f.key !== 'message' && f.key !== 'tag')}
        <div class="line" class:lvl-warn={lvl === 'warn'} class:lvl-error={lvl === 'error' || lvl === 'fatal'}>
          <time class="mono">{timeOf(evt)}</time>
          {#if lvl}<span class="chip lvl-{lvl}">{lvl}</span>{/if}
          {#if tagField}<span class="chip tag">{tagField.display}</span>{/if}
          <span class="text">{msgField ? msgField.display : (evt.channelName || 'channel ' + evt.channel)}</span>
          {#each rest as f}<span class="kv">{f.key}={f.display}</span>{/each}
        </div>
      {/each}
    {:else if tab === 'anomaly'}
      {#each currentList as evt, i (i + '-' + evt.at)}
        {@const fields = bodyFields(evt)}
        <div class="line">
          <time class="mono">{timeOf(evt)}</time>
          <span class="text">{evt.channelName || ('channel ' + evt.channel)}</span>
          {#each fields as f}<span class="kv">{f.key}={f.display}</span>{/each}
        </div>
      {/each}
    {:else}
      {#each currentList as evt, i (i + '-' + evt.at)}
        {@const fields = topFields(evt)}
        <div class="line">
          <time class="mono">{timeOf(evt)}</time>
          <span class="text">{evt.channelName || (evt.channel != null ? ('channel ' + evt.channel) : 'session')}</span>
          {#each fields as f}<span class="kv">{f.key}={f.display}</span>{/each}
        </div>
      {/each}
    {/if}
  </div>

  {#if !atBottom && currentList.length}
    <button type="button" class="jump" onclick={() => { atBottom = true; if (containerEl) containerEl.scrollTop = containerEl.scrollHeight; }}>
      jump to latest
    </button>
  {/if}
</div>

<style>
  .logpane {
    display: flex;
    flex-direction: column;
    gap: 8px;
    height: 100%;
    min-height: 0;
    position: relative;
  }

  .tabs {
    display: flex;
    gap: 6px;
    flex-wrap: wrap;
  }
  .tabs button {
    min-height: var(--tap);
    padding: 0 12px;
    border-radius: var(--r-s);
    background: var(--bg-card);
    border: 1px solid var(--line);
    color: var(--ink-dim);
    font-size: 13px;
  }
  .tabs button.on {
    color: var(--ink);
    border-color: var(--reality);
    background: color-mix(in srgb, var(--reality) 10%, var(--bg-card));
  }
  .count {
    color: var(--ink-faint);
    font-family: var(--mono);
    font-size: 11px;
    margin-left: 4px;
  }

  .feed {
    background: var(--bg-sunken);
    border: 1px solid var(--line);
    border-radius: var(--r);
    padding: 8px 10px;
    overflow-y: auto;
    max-height: 52vh;
    display: flex;
    flex-direction: column;
    gap: 3px;
  }

  .empty {
    color: var(--ink-faint);
    font-size: 12.5px;
    margin: 0;
  }

  .line {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 6px;
    font-size: 12.5px;
    line-height: 1.5;
    padding: 2px 0;
    border-bottom: 1px solid var(--line-soft);
  }
  .line time {
    color: var(--ink-faint);
    font-size: 11px;
    flex: 0 0 auto;
  }
  .line .text {
    color: var(--ink);
    overflow-wrap: anywhere;
  }
  .line.lvl-warn .text { color: var(--warn); }
  .line.lvl-error .text { color: var(--bad); }

  .chip {
    font-size: 10.5px;
    padding: 1px 6px;
    border-radius: 999px;
    background: var(--bg-card);
    border: 1px solid var(--line);
    color: var(--ink-dim);
    text-transform: uppercase;
    letter-spacing: 0.03em;
    flex: 0 0 auto;
  }
  .chip.lvl-warn { color: var(--warn); border-color: color-mix(in srgb, var(--warn) 50%, var(--line)); }
  .chip.lvl-error, .chip.lvl-fatal { color: var(--bad); border-color: color-mix(in srgb, var(--bad) 50%, var(--line)); }
  .chip.tag { text-transform: none; }

  .kv {
    font-family: var(--mono);
    font-size: 11px;
    color: var(--ink-faint);
    flex: 0 0 auto;
  }

  .jump {
    align-self: center;
    min-height: 32px;
    padding: 0 14px;
    border-radius: 999px;
    background: var(--bg-card);
    border: 1px solid var(--reality);
    color: var(--reality);
    font-size: 12px;
  }

  @media (prefers-reduced-motion: reduce) {
    .feed { scroll-behavior: auto; }
  }
</style>
