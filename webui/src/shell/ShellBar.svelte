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
  import { makeBleWebSocket, ipv4ToString, BLE_SERVICE } from './ble-ws.js';

  let scanning = $state(false);
  let hubs = $state([]);
  let manualHost = $state(localStorage.getItem('shell_host') || '192.168.1.229');
  let mode = $state(localStorage.getItem('shell_mode') || 'ws');
  let note = $state('');
  let expanded = $state(true);

  const phase = $derived(machine.link.phase);
  const endpoint = $derived(machine.link.endpoint || null);
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

  function connectBle(dev) {
    disconnect();
    mode = 'ble';
    localStorage.setItem('shell_mode', 'ble');
    note = 'BLE → ' + (dev.name || dev.address) + ' (watch tier until WS upgrade)';
    connect({ host: dev.address, WebSocketImpl: makeBleWebSocket(dev.address) });
  }

  function connectWs(host, port) {
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

<div class="shellbar" class:collapsed={!expanded}>
  <button class="sb-toggle" onclick={() => (expanded = !expanded)}
          aria-label="toggle shell bar">{expanded ? '▾' : '▴'} shell</button>
  {#if expanded}
    <span class="sb-mode mono" data-mode={mode}>{mode.toUpperCase()}</span>
    <span class="sb-phase mono">{phase}</span>

    <button class="sb-btn" onclick={scan}>{scanning ? 'stop' : 'scan BLE'}</button>
    {#each hubs as h (h.address)}
      <button class="sb-hub mono" onclick={() => connectBle(h)}>
        {h.name || '?'} {h.address} {h.rssi ? h.rssi + 'dBm' : ''}
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
  {/if}
</div>

<style>
  .shellbar {
    position: fixed;
    bottom: 0;
    left: 0;
    right: 0;
    z-index: 9999;
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
    padding: 4px 10px;
    background: rgba(10, 12, 16, 0.92);
    border-top: 1px solid #2a2e38;
    font-size: 0.72rem;
    color: #aab;
  }
  .collapsed { padding: 0 10px; background: rgba(10, 12, 16, 0.6); border-top: none; }
  .sb-toggle { color: #778; font-size: 0.68rem; padding: 3px 4px; }
  .sb-mode { padding: 1px 6px; border-radius: 2px; font-weight: 600; }
  .sb-mode[data-mode='ws'] { background: #16324a; color: #7fc4ff; }
  .sb-mode[data-mode='ble'] { background: #2c1e4a; color: #c0a4ff; }
  .sb-phase { color: #889; }
  .sb-btn {
    border: 1px solid #2a2e38;
    border-radius: 2px;
    padding: 2px 8px;
    color: #ccd;
    font-size: 0.72rem;
  }
  .sb-btn:hover { border-color: #55617a; }
  .sb-upgrade { border-color: #2e5d3a; color: #8fe0a4; }
  .sb-hub { border: 1px dashed #3a4152; border-radius: 2px; padding: 2px 8px; color: #c0a4ff; }
  .sb-host {
    width: 130px;
    background: #12151c;
    border: 1px solid #2a2e38;
    border-radius: 2px;
    padding: 2px 6px;
    color: #ccd;
    font-size: 0.72rem;
  }
  .sb-sep { flex: 0 0 8px; }
  .sb-note { color: #98a; font-style: italic; }
</style>
