<script>
  // THROWAWAY — build/visual verification of TelemetryChart.svelte and
  // PlanStrip.svelte against slopsim, wired outside App.svelte so the real
  // composition root is never touched. Deleted before this task is reported
  // done (mirrors the pattern already in HarnessApp.svelte/harness-main.js).
  import { machine, connect } from './model/machine.svelte.js';
  import TelemetryChart from './ui/widgets/TelemetryChart.svelte';
  import PlanStrip from './ui/widgets/PlanStrip.svelte';

  const host = location.hostname || '127.0.0.1';
  connect({ host, port: 82 });
</script>

<div style="padding:20px; max-width:900px; margin:0 auto; display:flex; flex-direction:column; gap:16px;">
  <p style="color:#9ad; font-family:monospace;">
    link: {machine.link.phase} | catalog: {machine.catalog.ready ? machine.catalog.entries.length + ' entries' : 'not ready'}
  </p>
  <TelemetryChart />
  <PlanStrip />
</div>
