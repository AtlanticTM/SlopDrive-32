<script>
  /**
   * FootStrip.svelte — the persistent footer link/diagnostic strip.
   *
   * Restores the pre-refactor page's full-width "▶ LINK" footer identity, but
   * carries only what this refactor's contracts actually have: link-quality
   * counters from machine.stats, the catalog's own identity, the deadman
   * window the hub granted this session, and the shipped UI build id. There
   * is no WiFi RSSI/BSSID/heap surface here — that lived in the retired HTTP
   * plane (docs/http-plane-retirement.md) and machine.stats does not carry it,
   * so showing it would mean fabricating a number nobody sent.
   *
   * Read-only and quiet on purpose: this bar never offers a control, only
   * tells the operator what the SlopSync link is doing. Ground truth applies
   * here too — every value is `--` until the machine (or the session itself)
   * has actually produced it.
   *
   * No props — reads the one machine spine directly. Wire it in bare:
   *   <FootStrip />
   */
  import { machine } from '../model/machine.svelte.js';
  import { since } from '../model/format.js';

  // A page full of "since" readouts needs its own clock, or the age freezes
  // the instant this component last happened to re-render.
  let nowTick = $state(Date.now());
  $effect(() => {
    const id = setInterval(() => { nowTick = Date.now(); }, 1000);
    return () => clearInterval(id);
  });
  function ageLabel(ms, _tick) { return since(ms); }

  const etagShort = $derived(
    machine.catalog.etag && machine.catalog.etag.length ? machine.catalog.etag.slice(0, 10) : '--'
  );
  const deadman = $derived(machine.link.deadmanMs ? machine.link.deadmanMs + ' ms' : '--');
  const clockOffset = $derived(machine.stats.clockOffsetUs != null ? machine.stats.clockOffsetUs + ' µs' : '--');
  const clockRtt = $derived(machine.stats.clockRttUs != null ? machine.stats.clockRttUs + ' µs' : '--');
  const rxAge = $derived(ageLabel(machine.stats.lastRxMs, nowTick));

  // Vite global (vite.config.js `define`) — short git hash, or a UTC build
  // timestamp in a repo-less checkout. Never a hand-maintained literal.
  const buildId = typeof __UI_BUILD__ !== 'undefined' ? __UI_BUILD__ : '--';
</script>

<footer class="footstrip" aria-label="Link diagnostics">
  <span class="fs-label" aria-hidden="true">&#9656; LINK</span>

  <div class="facts">
    <span class="fact"><span class="k">reconnects</span><span class="v mono">{machine.stats.reconnects}</span></span>
    <span class="fact"><span class="k">state pushes</span><span class="v mono">{machine.stats.statePushes}</span></span>
    <span class="fact"><span class="k">clock offset</span><span class="v mono">{clockOffset}</span></span>
    <span class="fact"><span class="k">clock rtt</span><span class="v mono">{clockRtt}</span></span>
    <span class="fact"><span class="k">catalog etag</span><span class="v mono" title={machine.catalog.etag || '--'}>{etagShort}</span></span>
    <span class="fact"><span class="k">deadman</span><span class="v mono">{deadman}</span></span>
    <span class="fact"><span class="k">last rx</span><span class="v mono">{rxAge}</span></span>
    <span class="fact"><span class="k">ui build</span><span class="v mono">{buildId}</span></span>
  </div>
</footer>

<style>
  .footstrip {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 6px 14px;
    padding: 8px var(--gap);
    background: var(--bg-raised);
    border-top: 1px solid var(--line);
    color: var(--ink-faint);
    font-size: 11px;
  }

  .fs-label {
    flex: 0 0 auto;
    color: var(--ink-faint);
    letter-spacing: .08em;
    font-weight: 600;
  }

  .facts {
    display: flex;
    flex-wrap: wrap;
    gap: 4px 14px;
    min-width: 0;
    flex: 1 1 auto;
  }

  .fact {
    display: inline-flex;
    align-items: baseline;
    gap: 5px;
    white-space: nowrap;
  }
  .k {
    color: var(--ink-faint);
    text-transform: uppercase;
    letter-spacing: .05em;
    font-size: 9.5px;
  }
  .v {
    color: var(--ink-dim);
    font-size: 11px;
  }

  @media (max-width: 480px) {
    .footstrip { font-size: 10.5px; }
  }
</style>
