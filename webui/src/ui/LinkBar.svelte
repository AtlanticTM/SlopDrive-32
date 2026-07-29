<script>
  /**
   * LinkBar.svelte — the persistent header: are we even talking to the machine?
   *
   * CLAUDE.md 3 (Ground Truth Doctrine) applied to the link itself, not just to
   * settings: this bar never claims a healthier link than machine.link.phase
   * actually reports, and it says so LOUDLY the moment phase !== 'live' —
   * there is no fallback control path on this firmware (HTTP is read-only), so
   * a dead link means nothing on the page can drive the machine, and the
   * operator must know that at a glance, not discover it by a control that
   * quietly does nothing.
   *
   * Restores the pre-refactor identity (tag `webui-prerefactor`): the
   * registration crosshair, the SD·32 wordmark, the live activity heatmap
   * canvas, the two link-health dots, and the chip row. The two safety-
   * relevant facts — link phase and access tier — are pinned chips that never
   * scroll out of view; everything else rides a horizontally-scrolling strip
   * so a phone never gets page-level horizontal overflow.
   *
   * Everything here reads machine.* (and the catalog's own role-tagged
   * fields) plus the browser's own location/theme — never a device fact that
   * isn't something the machine actually sent.
   */
  import { machine } from '../model/machine.svelte.js';
  import { ACCESS_NAME } from '../../../../SlopSync/clients/js/index.js';
  import { bytes, since } from '../model/format.js';
  import { ROLE } from '../model/roles.js';
  import { ac } from '../model/theme.js';

  /** Presentation only — every phase machine.link.phase can actually be. */
  const PHASE = {
    idle: { label: 'idle', tone: 'dim' },
    connecting: { label: 'connecting…', tone: 'warn' },
    handshaking: { label: 'handshaking…', tone: 'warn' },
    live: { label: 'live', tone: 'good' },
    retrying: { label: 'reconnecting…', tone: 'warn' },
    failed: { label: 'no link', tone: 'bad' },
  };

  const phaseInfo = $derived(PHASE[machine.link.phase] || { label: String(machine.link.phase), tone: 'dim' });
  const isLive = $derived(machine.link.phase === 'live');
  const hasSession = $derived(machine.link.sessionId != null);
  const tierLabel = $derived(hasSession
    ? (ACCESS_NAME[machine.link.roles] || ('tier ' + machine.link.roles))
    : '--');

  const hostFallback = $derived(typeof location !== 'undefined' ? location.hostname : '--');
  const identity = $derived(machine.link.hubIdentity);
  const hubLabel = $derived(identity ? (identity.hub_name || identity.product || hostFallback) : hostFallback);
  const fwLabel = $derived(identity && identity.fw_version ? identity.fw_version : '');

  const catalogLabel = $derived(
    machine.catalog.ready
      ? ('ready · ' + bytes(machine.catalog.bytes) + (machine.catalog.cached ? ' · cached' : ' · fetched'))
      : 'not loaded'
  );

  // A liveness readout needs a clock of its own — nothing else in this bar
  // re-renders on a schedule, so without a tick "since(...)" would freeze the
  // instant a frame stops arriving, which is exactly the moment it matters most.
  let nowTick = $state(Date.now());
  $effect(() => {
    const id = setInterval(() => { nowTick = Date.now(); }, 1000);
    return () => clearInterval(id);
  });
  function ageLabel(ms, _tick) { return since(ms); }
  const rxAge = $derived(ageLabel(machine.stats.lastRxMs, nowTick));
  const rxTone = $derived.by(() => {
    if (!isLive || !machine.stats.lastRxMs) return 'dim';
    const age = nowTick - machine.stats.lastRxMs;
    if (age < 1500) return 'good';
    if (age < 3000) return 'warn';
    return 'bad';
  });
  const rxToneLabel = $derived(
    rxTone === 'good' ? 'flowing' : rxTone === 'warn' ? 'gapping' : rxTone === 'bad' ? 'stalled' : 'no data'
  );

  // ===========================================================================
  // Activity heatmap — rows = live telemetry series discovered by ROLE, plus a
  // link-activity row derived from protocol stats (never device knowledge:
  // the roles are registry vocabulary and machine.stats is protocol-level).
  // A machine that publishes none of the telemetry roles simply gets the one
  // link-activity row — never a fabricated series.
  // ===========================================================================
  const heatRows = $derived.by(() => {
    const rows = [];
    const byRole = machine.catalog.model && machine.catalog.model.byRole;
    if (byRole) {
      const vel = byRole.get(ROLE.telemetryVelocity);
      if (vel && vel.length) rows.push({ key: 'vel', label: 'velocity', field: vel[0] });
      const cur = byRole.get(ROLE.telemetryCurrent);
      if (cur && cur.length) rows.push({ key: 'cur', label: 'current', field: cur[0] });
      const pwr = byRole.get(ROLE.telemetryPowerBus);
      if (pwr && pwr.length) rows.push({ key: 'pwr', label: 'bus power', field: pwr[0] });
    }
    rows.push({ key: 'link', label: 'link activity', field: null });
    return rows;
  });

  const heatmapAriaLabel = $derived(
    'Activity heatmap, last ~3 seconds. Rows: ' + heatRows.map((r) => r.label).join(', ') + '.'
  );

  const AG_COLS = 14, AG_CELL = 4, AG_GAP = 1;
  let heatCanvas = $state(null);

  // The bar's height is variable (banners appear and disappear). Publishing
  // the MEASURED height lets everything else that sticks to the viewport top
  // (nav rail, tab strip) sit exactly below this bar instead of under it.
  let barH = $state(0);
  $effect(() => {
    document.documentElement.style.setProperty('--linkbar-h', barH + 'px');
    return () => document.documentElement.style.removeProperty('--linkbar-h');
  });

  $effect(() => {
    const rows = heatRows;             // establishes the reactive dependency
    const canvas = heatCanvas;
    if (!canvas || typeof window === 'undefined') return;

    const reduceMotion = window.matchMedia &&
      window.matchMedia('(prefers-reduced-motion: reduce)').matches;

    const dpr = window.devicePixelRatio || 1;
    const cssW = AG_COLS * (AG_CELL + AG_GAP);
    const cssH = rows.length * (AG_CELL + AG_GAP + 1);
    canvas.width = Math.round(cssW * dpr);
    canvas.height = Math.round(cssH * dpr);
    canvas.style.width = cssW + 'px';
    canvas.style.height = cssH + 'px';
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    // Bounded history buffer, newest column last.
    let data = [];
    for (let c = 0; c < AG_COLS; c++) data.push(rows.map(() => 0));

    // Adaptive per-row ceilings: a field with a catalog-declared max scales
    // against that (ground truth); a field with none — or the link-activity
    // row, which has no "max" at all — scales against a slowly-decaying
    // observed peak, so "fully lit" always means "near this row's own recent
    // peak" rather than a guessed, device-specific number.
    const peaks = {};
    let lastPushes = machine.stats.statePushes;

    function sampleFrac(row) {
      if (row.key === 'link') {
        const cur = machine.stats.statePushes;
        const delta = Math.max(0, cur - lastPushes);
        lastPushes = cur;
        const ceiling = Math.max((peaks.link || 1) * 0.995, delta, 1);
        peaks.link = ceiling;
        return Math.min(1, delta / ceiling);
      }
      const sample = machine.samples[row.field.channelId];
      const raw = sample ? sample[row.field.name] : null;
      if (typeof raw !== 'number' || !isFinite(raw)) return 0;
      const abs = Math.abs(raw);
      let ceiling;
      if (isFinite(row.field.max) && row.field.max > 0) {
        ceiling = Math.abs(row.field.max);
      } else {
        ceiling = Math.max((peaks[row.key] || 0) * 0.995, abs, 1e-6);
        peaks[row.key] = ceiling;
      }
      return Math.min(1, abs / ceiling);
    }

    function draw() {
      ctx.clearRect(0, 0, cssW, cssH);
      for (let c = 0; c < AG_COLS; c++) {
        for (let r = 0; r < rows.length; r++) {
          const v = data[c][r];
          const a = Number((0.06 + v * 0.85).toFixed(2));
          ctx.fillStyle = ac('r', a);
          ctx.fillRect(c * (AG_CELL + AG_GAP), r * (AG_CELL + AG_GAP + 1), AG_CELL, AG_CELL);
        }
      }
    }

    function tick() {
      const frame = rows.map(sampleFrac);
      if (reduceMotion) {
        // Freeze the scroll animation but keep painting current values: every
        // column shows the same live reading instead of a moving history, so
        // the grid holds still while still being honest about "now".
        data = data.map(() => frame.slice());
      } else {
        data.shift();
        data.push(frame);
      }
      draw();
    }

    tick();
    const id = setInterval(tick, 220);
    return () => clearInterval(id);
  });
</script>

<!-- Registration crosshair — pinned top-right of the viewport, decorative. -->
<svg class="crosshair" viewBox="0 0 14 14" fill="none" stroke="currentColor" stroke-width="1" aria-hidden="true">
  <path d="M7 0v14M0 7h14"/>
  <circle cx="7" cy="7" r="2.5"/>
</svg>

<header class="linkbar" bind:clientHeight={barH}>
  <div class="hdr-row">
    <div class="header-left">
      <canvas bind:this={heatCanvas} class="act-grid" aria-label={heatmapAriaLabel}></canvas>
      <span class="wordmark">SD&middot;32</span>
      <span class="link-dot tone-{phaseInfo.tone}" role="img" aria-label={'session link: ' + phaseInfo.label}></span>
      <span class="link-dot tone-{rxTone}" role="img" aria-label={'telemetry: ' + rxToneLabel}></span>
    </div>

    <div class="chips">
      <!-- Safety-relevant facts: pinned, never scrolled out of view. -->
      <span class="chip chip-pin tone-{phaseInfo.tone}" role="status" aria-live="polite">
        <span class="chip-dot"></span>{phaseInfo.label}
      </span>
      <span class="chip chip-pin">
        <span class="chip-lbl">tier</span>{tierLabel}
      </span>

      <div class="chips-scroll">
        <span class="chip" title={fwLabel ? ('firmware ' + fwLabel) : ''}>
          <span class="chip-lbl">hub</span>
          <span class="mono">{hubLabel}{fwLabel ? ' · ' + fwLabel : ''}</span>
        </span>
        <span class="chip">
          <span class="chip-lbl">catalog</span>{catalogLabel}
        </span>
        <span class="chip tone-{rxTone}">
          <span class="chip-lbl">rx</span>
          <span class="mono">{rxAge}</span>
        </span>
      </div>
    </div>
  </div>

  {#if !isLive}
    <div class="banner tone-{phaseInfo.tone}" role="alert">
      <strong>{phaseInfo.label.toUpperCase()}</strong>
      — no hub link: nothing on this page can drive the machine right now.
      {#if machine.link.closeReason}<span class="reason">({machine.link.closeReason})</span>{/if}
    </div>
  {/if}

  {#if machine.link.error}
    <!-- RFC-033: SUBSCRIBE_REJECTED (and any other link-level protocol error
         this client causes) surfaces here, not just in the SlopSync pane's
         NACK table — it means a client bug, and burying it in a list of
         routine NACKs is how it goes unnoticed. -->
    <div class="banner tone-bad" role="alert">
      <strong>LINK ERROR</strong> — {machine.link.error}
    </div>
  {/if}
</header>

<style>
  .crosshair {
    position: fixed;
    top: calc(8px + env(safe-area-inset-top, 0px));
    right: 8px;
    width: 14px;
    height: 14px;
    color: var(--line-3);
    z-index: 60;
    pointer-events: none;
  }

  .linkbar {
    position: sticky;
    top: 0;
    z-index: 20;
    background: var(--bg-raised);
    border-bottom: 1px solid var(--line);
    /* Edge-to-edge devices (viewport-fit=cover): keep the bar's content out
       of the status-bar/notch zone; the background still paints under it. */
    padding: calc(8px + env(safe-area-inset-top, 0px)) var(--gap) 8px;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .hdr-row {
    display: flex;
    align-items: center;
    gap: 10px;
    flex-wrap: wrap;
    row-gap: 6px;
  }

  .header-left {
    display: flex;
    align-items: center;
    gap: 8px;
    flex: 0 0 auto;
  }

  .act-grid {
    image-rendering: pixelated;
    flex: 0 0 auto;
    border-radius: 1px;
  }

  .wordmark {
    font-family: var(--mono);
    font-weight: 700;
    font-size: 15px;
    letter-spacing: 0.02em;
    color: var(--ink-hi);
    flex: 0 0 auto;
    white-space: nowrap;
  }

  .link-dot {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: var(--line-2);
    flex: 0 0 auto;
    transition: background .2s, box-shadow .2s;
  }
  .link-dot.tone-good { background: var(--good); box-shadow: var(--glow-reality); }
  .link-dot.tone-warn { background: var(--warn); box-shadow: 0 0 8px var(--warn); }
  .link-dot.tone-bad  { background: var(--bad); box-shadow: 0 0 8px var(--bad); }
  .link-dot.tone-dim  { background: var(--line-2); box-shadow: none; }

  /* ---- chips: pinned safety facts + a horizontally-scrolling rest ---- */
  .chips {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-left: auto;
    min-width: 0;
    flex: 1 1 auto;
    justify-content: flex-end;
  }

  .chip {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    font-size: .68rem;
    font-weight: 500;
    padding: 4px 7px;
    border-radius: var(--radius);
    background: var(--bg-card);
    border: 1px solid var(--line);
    color: var(--ink-dim);
    white-space: nowrap;
    flex: 0 0 auto;
  }
  .chip-lbl {
    color: var(--ink-faint);
    text-transform: uppercase;
    letter-spacing: .04em;
    font-size: .6rem;
  }
  .chip-dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: currentColor;
    flex: 0 0 auto;
  }

  /* Pinned chips carry the tone as text color + a tinted wash, so phase and
     tier read at a glance without relying on the dot alone. */
  .chip-pin {
    font-weight: 700;
    color: var(--ink-hi);
  }
  .chip-pin.tone-good { border-color: color-mix(in srgb, var(--good) 45%, var(--line)); color: var(--good); }
  .chip-pin.tone-warn { border-color: color-mix(in srgb, var(--warn) 45%, var(--line)); color: var(--warn); }
  .chip-pin.tone-bad  { border-color: color-mix(in srgb, var(--bad) 45%, var(--line)); color: var(--bad); }
  .chip.tone-good .mono { color: var(--good); }
  .chip.tone-warn .mono { color: var(--warn); }
  .chip.tone-bad  .mono { color: var(--bad); }

  .chips-scroll {
    display: flex;
    gap: 6px;
    overflow-x: auto;
    scrollbar-width: none;
    min-width: 0;
  }
  .chips-scroll::-webkit-scrollbar { display: none; }

  .banner {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    align-items: baseline;
    padding: 6px 8px;
    border-radius: var(--r-s);
    background: color-mix(in srgb, var(--bad) 12%, var(--bg-card));
    border: 1px solid color-mix(in srgb, var(--bad) 40%, var(--line));
    font-size: 12.5px;
    color: var(--ink);
  }
  .banner.tone-warn {
    background: color-mix(in srgb, var(--warn) 12%, var(--bg-card));
    border-color: color-mix(in srgb, var(--warn) 40%, var(--line));
  }
  .banner.tone-dim {
    background: var(--bg-card);
    border-color: var(--line);
    color: var(--ink-dim);
  }
  .reason { color: var(--ink-faint); }

  @media (max-width: 400px) {
    .chip { font-size: .62rem; padding: 3px 6px; }
    .wordmark { font-size: 13px; }
  }
</style>
