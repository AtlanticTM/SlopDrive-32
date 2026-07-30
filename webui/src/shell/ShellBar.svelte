<script>
  /**
   * ShellBar.svelte — the Tauri shell's own chrome: hub discovery + transport
   * control. SHELL ONLY (mounted by main.js's SHELL branch; never in the
   * embedded bundle).
   *
   * Constraints:
   * - This is SHELL chrome, not kernel UI: it may know about transports and
   *   addresses. It must NEVER touch machine state — it only picks which
   *   transport the one kernel session rides (DOCTRINE: everything through
   *   SlopSync).
   * - BLE sessions have no HTTP sideband, so no /uitoken: they land at watch
   *   tier by design. Control arrives with the WS upgrade.
   */
  import { startScan, stopScan, checkPermissions } from '@mnlphlp/plugin-blec';
  import { machine, connect, disconnect } from '../model/machine.svelte.js';
  import { makeBleWebSocket, ipv4ToString, BLE_SERVICE, bleStats } from './ble-ws.js';

  let scanning = $state(false);
  let hubs = $state([]);
  // The bar publishes its own MEASURED height as --shell-chrome-top so the
  // page reserves exactly that much top padding (style.css's .app) and the
  // LinkBar sticks below it. clientHeight includes padding, so the notch
  // inset this bar absorbs is already in the number. The kernel reads the var
  // with a 0px default and never knows the shell exists.
  //
  // Zeroing --chrome-inset-top is the other half of that: this bar is now the
  // topmost chrome, so it owns the notch and the LinkBar must not pad for the
  // same inset a second time.
  let barH = $state(0);
  $effect(() => {
    const root = document.documentElement.style;
    root.setProperty('--shell-chrome-top', barH + 'px');
    root.setProperty('--chrome-inset-top', '0px');
    return () => {
      root.removeProperty('--shell-chrome-top');
      root.removeProperty('--chrome-inset-top');
    };
  });
  // No baked-in address: discovery is the front door. The input remembers
  // only a host the operator themselves connected to before.
  let manualHost = $state(localStorage.getItem('shell_host') || '');
  let mode = $state(localStorage.getItem('shell_mode') || 'ws');
  let note = $state('');
  let expanded = $state(true);

  const phase = $derived(machine.link.phase);
  const endpoint = $derived(machine.link.endpoint || null);

  // BLE wire counters, polled — bleStats is a plain module object (the bridge
  // is not reactive code), so a 1 Hz sample into $state is the honest view.
  let stats = $state('');
  $effect(() => {
    if (mode !== 'ble') { stats = ''; return; }
    const t = setInterval(() => {
      stats = 'rx ' + bleStats.rx + ' tx ' + bleStats.tx
        + (bleStats.lastError ? ' err ' + bleStats.lastError : '');
    }, 1000);
    return () => clearInterval(t);
  });
  const canUpgrade = $derived(
    mode === 'ble' && phase === 'live' && endpoint && endpoint.ipv4 && endpoint.wsPort
  );

  async function scan() {
    if (scanning) { await stopScan().catch(() => {}); scanning = false; return; }
    note = '';
    hubs = [];
    try {
      await checkPermissions(true);
      scanning = true;
      await startScan((devices) => {
        // A SlopSync hub advertises the service UUID in its primary payload
        // (fw: SlopSyncBlePort). Match on that, never on the name.
        hubs = devices.filter((d) =>
          (d.services || []).some((s) => String(s).toLowerCase() === BLE_SERVICE));
      }, 6000);
      setTimeout(() => { scanning = false; }, 6100);
    } catch (e) {
      scanning = false;
      note = 'scan failed: ' + e;
    }
  }

  async function connectBle(dev) {
    // Never GATT-connect with a scan still running: Android's stack handles
    // it badly, and the scan has done its job the moment a hub is chosen.
    if (scanning) { await stopScan().catch(() => {}); scanning = false; }
    disconnect();
    mode = 'ble';
    localStorage.setItem('shell_mode', 'ble');
    note = 'BLE → ' + (dev.name || dev.address) + ' (watch tier until WS upgrade)';
    connect({ host: dev.address, WebSocketImpl: makeBleWebSocket(dev.address) });
  }

  function connectWs(host, port) {
    if (!host || !host.trim()) { note = 'enter a hub address or scan'; return; }
    host = host.trim();
    disconnect();
    mode = 'ws';
    localStorage.setItem('shell_mode', 'ws');
    localStorage.setItem('shell_host', host);
    note = '';
    connect({ host, port: port || 82 });
  }

  function upgrade() {
    const ip = ipv4ToString(endpoint.ipv4);
    note = 'upgraded → ws://' + ip + ':' + endpoint.wsPort;
    connectWs(ip, endpoint.wsPort);
  }
</script>

<div class="shellbar" class:collapsed={!expanded} bind:clientHeight={barH}>
  <!-- Hangs from the top now: the glyph points toward where tapping moves
       the bar's free edge — down (▾) when collapsed-and-about-to-expand,
       up (▴) when expanded-and-about-to-collapse. -->
  <button class="sb-toggle mono" onclick={() => (expanded = !expanded)}
          aria-label="toggle shell bar">{expanded ? '▴' : '▾'} shell</button>
  {#if expanded}
    <span class="sb-mode mono" data-mode={mode}>{mode.toUpperCase()}</span>
    <span class="sb-phase mono">{phase}</span>

    <button class="sb-btn" onclick={scan}>{scanning ? 'stop' : 'scan BLE'}</button>
    {#each hubs as h (h.address)}
      <button class="sb-hub mono" onclick={() => connectBle(h)}>
        <span class="hub-name">{h.name || 'hub'}</span>
        <span class="hub-addr">{h.address}</span>
        {#if h.rssi}<span class="hub-rssi">{h.rssi} dBm</span>{/if}
      </button>
    {/each}
    {#if scanning && hubs.length === 0}<span class="sb-note">scanning…</span>{/if}

    <span class="sb-sep"></span>
    <input class="sb-host mono" bind:value={manualHost} placeholder="host"
           onkeydown={(e) => { if (e.key === 'Enter') connectWs(manualHost); }} />
    <button class="sb-btn" onclick={() => connectWs(manualHost)}>WS</button>

    {#if canUpgrade}
      <button class="sb-btn sb-upgrade" onclick={upgrade}>
        ↑ WS {ipv4ToString(endpoint.ipv4)}:{endpoint.wsPort}
      </button>
    {/if}
    {#if note}<span class="sb-note">{note}</span>{/if}
    {#if stats}<span class="sb-note mono">{stats}</span>{/if}
  {/if}
</div>

<style>
  .shellbar {
    position: fixed;
    /* TOPMOST chrome. It must be, and the offset must be 0: `.app` reserves
       this bar's measured height as its own padding-top, which pushes the
       LinkBar down by exactly that much. Offsetting the bar as well
       double-counts — it lands over the LinkBar's lower half (drawing behind
       it) with dead space above and below. */
    top: 0;
    left: 0;
    right: 0;
    z-index: 21; /* above the LinkBar's stacking (20): it scrolls under this */
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
    /* Topmost chrome owns the notch inset (style.css --chrome-inset-top),
       and the $effect above zeroes that var so the LinkBar stops padding for
       it. No inset-bottom: this never touches the bottom edge, SafetyBar
       owns that exclusively. */
    padding: calc(4px + env(safe-area-inset-top, 0px)) 10px 4px;
    background: color-mix(in srgb, var(--bg-raised) 94%, transparent);
    border-bottom: 1px solid var(--line);
    font-size: 0.72rem;
    color: var(--ink-dim);
  }
  .collapsed {
    padding: 0 10px;
    background: color-mix(in srgb, var(--bg-raised) 70%, transparent);
    border-bottom: none;
  }
  .sb-toggle {
    color: var(--ink-faint);
    font-size: 0.68rem;
    padding: 3px 4px;
    text-transform: uppercase;
    letter-spacing: .04em;
  }
  .sb-toggle:hover { color: var(--ink); }
  .sb-mode {
    padding: 4px 7px;
    border-radius: var(--radius);
    font-weight: 700;
    background: var(--bg-card);
    border: 1px solid var(--line);
  }
  .sb-mode[data-mode='ws'] {
    border-color: color-mix(in srgb, var(--reality) 45%, var(--line));
    color: var(--reality);
  }
  .sb-mode[data-mode='ble'] {
    border-color: color-mix(in srgb, var(--intent) 45%, var(--line));
    color: var(--intent);
  }
  .sb-phase { color: var(--ink-dim); }
  .sb-btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    min-height: 36px;
    padding: 0 12px;
    background: var(--bg-card);
    border: 1px solid var(--line);
    border-radius: var(--radius);
    color: var(--ink);
    font-size: 12.5px;
  }
  .sb-btn:hover { border-color: var(--line-4); }
  .sb-upgrade {
    border-color: color-mix(in srgb, var(--reality) 45%, var(--line));
    background: color-mix(in srgb, var(--reality) 12%, var(--bg-card));
    color: var(--reality);
  }
  .sb-hub {
    display: inline-flex;
    align-items: center;
    min-height: 36px;
    padding: 0 12px;
    gap: 7px;
    background: var(--bg-card);
    border: 1px dashed color-mix(in srgb, var(--intent) 55%, var(--line));
    border-radius: var(--radius);
    color: var(--intent);
  }
  .sb-hub .hub-name { font-weight: 600; }
  .sb-hub .hub-addr { color: var(--ink-dim); }
  .sb-hub .hub-rssi { color: var(--ink-faint); }
  .sb-host {
    width: 130px;
    min-height: 36px;
    background: var(--bg-sunken);
    border: 1px solid var(--line);
    border-radius: var(--radius);
    padding: 2px 8px;
    color: var(--ink);
    font-size: 0.72rem;
  }
  .sb-sep { flex: 0 0 8px; }
  .sb-note { color: var(--ink-faint); font-style: italic; font-size: 11px; }
</style>
