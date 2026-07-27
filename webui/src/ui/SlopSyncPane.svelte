<script>
  /**
   * SlopSyncPane.svelte — protocol observability.
   *
   * This pane is deliberately ABOUT the protocol, so unlike every other
   * component in this refactor it is allowed to show channel ids and names —
   * that data comes off machine.catalog.entries at runtime, which is exactly
   * the carve-out the task brief describes: reading catalog data is not the
   * same thing as hardcoding a device fact in source.
   *
   * The channel table's whole reason to exist is making two kinds of silent
   * failure visible: a granted rate that does not match what the channel
   * offers, and a channel that has simply stopped producing samples. Neither
   * shows up any other way once the control that depends on it has already
   * reverted.
   */
  import { machine } from '../model/machine.svelte.js';
  import { ACCESS_NAME, toHex } from '../core/slopsync/index.js';
  import { bytes, since } from '../model/format.js';

  // A liveness pane full of "since" readouts needs its own clock, or every
  // age freezes the instant this component last happened to re-render.
  let nowTick = $state(Date.now());
  $effect(() => {
    const id = setInterval(() => { nowTick = Date.now(); }, 1000);
    return () => clearInterval(id);
  });
  function ageLabel(ms, _tick) { return since(ms); }

  const link = $derived(machine.link);
  const tierLabel = $derived(link.sessionId != null ? (ACCESS_NAME[link.roles] || ('tier ' + link.roles)) : '--');
  const identity = $derived(link.hubIdentity);

  const etagHex = $derived(machine.catalog.etag && machine.catalog.etag.length ? toHex(machine.catalog.etag) : '--');

  function hexId(id) { return '0x' + id.toString(16).padStart(4, '0').toUpperCase(); }
  function categoryOf(entry) {
    if (entry.category == null) return '--';
    return entry.categoryLabel || entry.categoryName || ('category ' + entry.category);
  }
  function grantedRate(id) {
    const g = machine.grants[id];
    return g && g.rate != null ? (g.rate + ' Hz') : '--';
  }
  function offeredRate(entry) {
    return entry.maxRateHz ? (entry.maxRateHz + ' Hz') : (entry.maxRateHz === 0 ? 'on-change' : '--');
  }
  function lastSample(id, _tick) {
    const ts = machine.sampleTs[id];
    return ts ? since(ts) : '--';
  }

  const nacks = $derived([...machine.events.nacks].reverse());
</script>

<div class="pane-scroll">
  <section class="block">
    <h2>Session</h2>
    <dl class="facts">
      <dt>session id</dt><dd class="mono">{link.sessionId ?? '--'}</dd>
      <dt>link phase</dt><dd>{link.phase}</dd>
      <dt>access tier</dt><dd>{tierLabel}</dd>
      <dt>deadman window</dt><dd>{link.deadmanMs ? link.deadmanMs + ' ms' : '--'}</dd>
      <dt>config generation</dt><dd class="mono">{link.cfgGen ?? '--'}</dd>
      <dt>hub identity</dt>
      <dd>
        {#if identity}
          {identity.hub_name || identity.product || '--'}
          {#if identity.fw_version}<span class="dim"> · fw {identity.fw_version}</span>{/if}
          {#if identity.info}<span class="dim"> · {identity.info}</span>{/if}
        {:else}
          --
        {/if}
      </dd>
    </dl>
  </section>

  <section class="block">
    <h2>Catalog</h2>
    <dl class="facts">
      <dt>state</dt><dd>{machine.catalog.ready ? 'ready' : 'not loaded'}</dd>
      <dt>etag</dt><dd class="mono">{etagHex}</dd>
      <dt>size</dt><dd>{bytes(machine.catalog.bytes)}</dd>
      <dt>source</dt><dd>{machine.catalog.ready ? (machine.catalog.cached ? 'cached' : 'fetched') : '--'}</dd>
      <dt>entries</dt><dd>{machine.catalog.entries.length}</dd>
    </dl>
  </section>

  <section class="block">
    <h2>Channels</h2>
    {#if !machine.catalog.entries.length}
      <p class="empty">No catalog yet.</p>
    {:else}
      <div class="table-wrap">
        <table>
          <thead>
            <tr>
              <th>id</th><th>name</th><th>class</th><th>dir</th><th>category</th>
              <th>offered</th><th>granted</th><th>last sample</th>
            </tr>
          </thead>
          <tbody>
            {#each machine.catalog.entries as e (e.id)}
              <tr>
                <td class="mono">{hexId(e.id)}</td>
                <td>{e.name}</td>
                <td>{e.clsName}</td>
                <td>{e.dirName}</td>
                <td>{categoryOf(e)}</td>
                <td class="mono">{offeredRate(e)}</td>
                <td class="mono" class:mismatch={machine.grants[e.id] && e.maxRateHz && machine.grants[e.id].rate !== e.maxRateHz}>
                  {grantedRate(e.id)}
                </td>
                <td class="mono">{lastSample(e.id, nowTick)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
  </section>

  <section class="block">
    <h2>Link counters</h2>
    <dl class="facts">
      <dt>state pushes</dt><dd class="mono">{machine.stats.statePushes}</dd>
      <dt>frames in</dt><dd class="mono">{machine.stats.framesIn}</dd>
      <dt>frames out</dt><dd class="mono">{machine.stats.framesOut}</dd>
      <dt>bytes in</dt><dd class="mono">{bytes(machine.stats.bytesIn)}</dd>
      <dt>last rx</dt><dd class="mono">{ageLabel(machine.stats.lastRxMs, nowTick)}</dd>
      <dt>clock offset</dt><dd class="mono">{machine.stats.clockOffsetUs != null ? machine.stats.clockOffsetUs + ' µs' : '--'}</dd>
      <dt>clock rtt</dt><dd class="mono">{machine.stats.clockRttUs != null ? machine.stats.clockRttUs + ' µs' : '--'}</dd>
      <dt>reconnects</dt><dd class="mono">{machine.stats.reconnects}</dd>
    </dl>
  </section>

  <section class="block">
    <h2>Recent NACKs</h2>
    {#if !nacks.length}
      <p class="empty">None seen this session.</p>
    {:else}
      <div class="table-wrap">
        <table>
          <thead>
            <tr><th>when</th><th>code</th><th>channel</th><th>detail</th></tr>
          </thead>
          <tbody>
            {#each nacks as n (n.at + ':' + n.code + ':' + n.channel)}
              <tr>
                <td class="mono">{ageLabel(n.at, nowTick)} ago</td>
                <td class="mono">{n.name}</td>
                <td class="mono">{n.channel != null ? hexId(n.channel) : '--'}</td>
                <td>{n.detail || '--'}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
  </section>
</div>

<style>
  .pane-scroll {
    display: flex;
    flex-direction: column;
    gap: var(--gap);
  }
  .block {
    background: var(--bg-card);
    border: 1px solid var(--line);
    border-radius: var(--r);
    padding: 10px 12px;
  }
  h2 {
    font-size: 13px;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--ink-dim);
    margin: 0 0 8px;
  }
  .empty {
    color: var(--ink-faint);
    font-size: 12.5px;
    margin: 0;
  }

  .facts {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 4px 12px;
    margin: 0;
    font-size: 13px;
  }
  .facts dt { color: var(--ink-faint); }
  .facts dd { margin: 0; color: var(--ink); overflow-wrap: anywhere; }
  .dim { color: var(--ink-faint); }

  .table-wrap {
    overflow-x: auto;
    -webkit-overflow-scrolling: touch;
  }
  table {
    border-collapse: collapse;
    width: 100%;
    min-width: 620px;
    font-size: 12.5px;
  }
  th, td {
    text-align: left;
    padding: 5px 8px;
    border-bottom: 1px solid var(--line-soft);
    white-space: nowrap;
  }
  th {
    color: var(--ink-faint);
    text-transform: uppercase;
    font-size: 10.5px;
    letter-spacing: 0.03em;
    font-weight: 500;
  }
  td.mismatch { color: var(--warn); font-weight: 600; }
</style>
