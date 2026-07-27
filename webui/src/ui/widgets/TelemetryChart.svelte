<script>
  /**
   * TelemetryChart.svelte — scrolling live strip chart, discovered by ROLE.
   *
   * Reproduces the character of the pre-refactor features/diag.js graph (a
   * scrolling multi-lane scope with gridlines, per-lane auto-scale and a live
   * numeric readout) but knows nothing about SlopDrive-32: it never binds to a
   * channel id or a wire field name. It is handed a list of REGISTRY roles
   * (`ROLE.telemetryPosition`, `ROLE.telemetryCurrent`, ...) and resolves each
   * one against `machine.catalog.model.byRole` — exactly the mechanism
   * roles.js documents for hero widgets. A machine that publishes only
   * position plots one lane; a machine that also publishes current and bus
   * voltage grows more lanes, with no code change here.
   *
   * GROUND TRUTH: every sample is `reportedValue()` off the live decoded
   * STATE snapshot (settings.js) — never a locally-remembered write, never a
   * synthesized zero. A channel that has not reported yet contributes NaN to
   * its ring, which the tracer renders as a gap rather than a false zero.
   *
   * Sampling / drawing is entirely internal state (plain closures, not runes)
   * because it is imperative animation machinery, not UI state a template
   * needs to react to — the template only reads the reactive `resolvedSeries`
   * / `legendVals` derivations, which stay perfectly live independent of
   * whether the canvas loop itself is animating.
   */
  import { machine } from '../../model/machine.svelte.js';
  import { ROLE } from '../../model/roles.js';
  import { reportedValue } from '../../model/settings.js';
  import { formatValue, unitOf } from '../../model/format.js';

  let { roles = [ROLE.telemetryPosition, ROLE.telemetryVelocity] } = $props();

  // ---- role resolution --------------------------------------------------
  //
  // Fixed input order (the `roles` prop), never the order channels happen to
  // resolve in — that is what lets a series keep the SAME colour for its
  // whole lifetime even if an earlier role in the list drops off the catalog
  // mid-session (reconnect to a different / reconfigured machine).
  const resolvedSeries = $derived.by(() => {
    const byRole = machine.catalog.model && machine.catalog.model.byRole;
    if (!byRole) return [];
    const out = [];
    for (const role of roles) {
      const list = byRole.get(role);
      if (list && list.length) out.push(list[0]); // first-authored wins, per roles.js
    }
    return out;
  });

  const legendVals = $derived(
    resolvedSeries.map((f) => reportedValue(f, machine.samples[f.channelId]))
  );

  // Fixed-order palette drawn from the app's own status tokens (style.css)
  // rather than an invented categorical ramp — this UI has none, and the
  // hard rule is "invent no colours". Index is the role's POSITION IN THE
  // PROP, not in resolvedSeries, so identity survives a series appearing or
  // disappearing (see comment above).
  const PALETTE_VARS = ['--reality', '--good', '--warn', '--bad'];
  function paletteVarFor(f) {
    const idx = roles.indexOf(f.role);
    return PALETTE_VARS[(idx < 0 ? 0 : idx) % PALETTE_VARS.length];
  }

  // ---- ring buffers -------------------------------------------------------
  const RING_N = 420;          // "a few hundred points" per series
  const SAMPLE_MS = 60;        // ~16 Hz collection
  const WINDOW_MS = 10000;     // 10 s visible span
  const GAP_MS = SAMPLE_MS * 4;

  function createRing() {
    return { t: new Float64Array(RING_N), v: new Float64Array(RING_N), head: -1, len: 0 };
  }
  function pushRing(ring, t, v) {
    ring.head = (ring.head + 1) % RING_N;
    ring.t[ring.head] = t;
    ring.v[ring.head] = v;
    if (ring.len < RING_N) ring.len++;
  }

  const ringsByUid = new Map(); // field.uid -> ring, lazily created/pruned

  let canvasEl = $state(null);

  $effect(() => {
    if (!canvasEl) return;
    const ctx = canvasEl.getContext('2d');
    const root = document.documentElement;
    const cssVar = (name) => getComputedStyle(root).getPropertyValue(name).trim();
    const paletteColors = PALETTE_VARS.map(cssVar);
    const inkFaint = cssVar('--ink-faint');
    const line = cssVar('--line-soft');
    function colorFor(f) {
      const idx = roles.indexOf(f.role);
      return paletteColors[(idx < 0 ? 0 : idx) % paletteColors.length];
    }

    const mq = window.matchMedia('(prefers-reduced-motion: reduce)');
    let reduced = mq.matches;
    const onMqChange = (e) => { reduced = e.matches; };
    mq.addEventListener('change', onMqChange);

    let cssW = 0, cssH = 0;
    function sizeIfNeeded() {
      const w = canvasEl.clientWidth, h = canvasEl.clientHeight;
      if (!w || !h) return false;
      if (w !== cssW || h !== cssH) {
        const dpr = window.devicePixelRatio || 1;
        canvasEl.width = Math.round(w * dpr);
        canvasEl.height = Math.round(h * dpr);
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        cssW = w; cssH = h;
      }
      return true;
    }

    function sample() {
      const now = performance.now();
      const live = new Set();
      for (const f of resolvedSeries) {
        live.add(f.uid);
        let ring = ringsByUid.get(f.uid);
        if (!ring) { ring = createRing(); ringsByUid.set(f.uid, ring); }
        const raw = reportedValue(f, machine.samples[f.channelId]);
        const val = (typeof raw === 'number' && isFinite(raw)) ? raw : NaN;
        pushRing(ring, now, val);
      }
      // A series that stopped resolving (catalog changed under us) leaks its
      // ring forever otherwise — this is a long-lived component.
      for (const uid of ringsByUid.keys()) if (!live.has(uid)) ringsByUid.delete(uid);
    }

    // Bounds: the field's own annotated [min,max] when the catalog gives one
    // (ground truth about the SENSOR's range, not a guess), else auto-scaled
    // from what has actually been observed in the visible window — same
    // padding heuristic the legacy diag.js lanes used.
    function laneScale(f, ring, now) {
      if (f.min != null && f.max != null && f.max > f.min) return [f.min, f.max];
      let lo = Infinity, hi = -Infinity;
      for (let i = 0; i < ring.len; i++) {
        const idx = (ring.head - i + RING_N) % RING_N;
        if (now - ring.t[idx] > WINDOW_MS) continue;
        const v = ring.v[idx];
        if (!isFinite(v)) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
      if (!isFinite(lo) || !isFinite(hi)) return [0, 1];
      if (hi - lo < 1e-9) { lo -= 1; hi += 1; }
      const pad = (hi - lo) * 0.12;
      return [lo - pad, hi + pad];
    }

    function draw() {
      if (!sizeIfNeeded()) return;
      ctx.clearRect(0, 0, cssW, cssH);
      const n = resolvedSeries.length;
      if (!n) return;
      const now = performance.now();
      const laneH = cssH / n;
      const padL = 4, padR = 4;
      const x0 = padL, x1 = cssW - padR;

      // shared time gridlines every 2s, spanning the full height
      ctx.strokeStyle = line;
      ctx.lineWidth = 1;
      for (let sBack = 0; sBack <= WINDOW_MS / 1000; sBack += 2) {
        const x = x1 - (sBack * 1000 / WINDOW_MS) * (x1 - x0);
        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, cssH); ctx.stroke();
      }

      resolvedSeries.forEach((f, i) => {
        const ring = ringsByUid.get(f.uid);
        if (!ring) return;
        const laneY = i * laneH;
        const [lo, hi] = laneScale(f, ring, now);
        const span = hi - lo || 1;
        const y = (v) => laneY + (1 - Math.min(1, Math.max(0, (v - lo) / span))) * (laneH - 4) + 2;

        ctx.strokeStyle = line;
        ctx.strokeRect(x0, laneY + 1, x1 - x0, laneH - 2);

        ctx.lineWidth = 1.5;
        ctx.strokeStyle = colorFor(f);
        ctx.beginPath();
        let started = false, prevT = 0;
        for (let j = 0; j < ring.len; j++) {
          const idx = (ring.head - j + RING_N) % RING_N;
          const t = ring.t[idx];
          const age = now - t;
          if (age > WINDOW_MS) continue;
          const val = ring.v[idx];
          if (!isFinite(val)) { started = false; continue; }
          const x = x1 - (age / WINDOW_MS) * (x1 - x0);
          if (!started || (prevT - t) > GAP_MS) { ctx.moveTo(x, y(val)); started = true; }
          else ctx.lineTo(x, y(val));
          prevT = t;
        }
        ctx.stroke();

        // faint scale caption so the auto-scaled lanes are not a mystery
        ctx.fillStyle = inkFaint;
        ctx.font = '9px "Martian Mono", monospace';
        ctx.fillText(formatValue(f, hi), x0 + 3, laneY + 9);
        ctx.fillText(formatValue(f, lo), x0 + 3, laneY + laneH - 3);
      });
    }

    let raf = null;
    let timer = null;
    function frame() {
      sample();
      draw();
      raf = requestAnimationFrame(frame);
    }

    // prefers-reduced-motion: no continuously-animating scroll. Data still
    // collects and redraws, just as discrete steps instead of a 60fps sweep —
    // "freeze the animation, still show current values" (the HTML legend
    // below is fully live either way, since it is plain reactive markup, not
    // part of this loop at all).
    if (reduced) {
      sample(); draw();
      timer = setInterval(() => { sample(); draw(); }, 1000);
    } else {
      raf = requestAnimationFrame(frame);
    }

    return () => {
      mq.removeEventListener('change', onMqChange);
      if (raf) cancelAnimationFrame(raf);
      if (timer) clearInterval(timer);
    };
  });
</script>

{#if resolvedSeries.length}
  <div class="tchart">
    <div class="tchart-legend">
      {#each resolvedSeries as f, i (f.uid)}
        <span class="leg">
          <i class="swatch" style="background: var({paletteVarFor(f)})" aria-hidden="true"></i>
          <span class="leg-label">{f.label}</span>
          <output class="mono leg-val">{formatValue(f, legendVals[i])}<span class="unit">{unitOf(f)}</span></output>
        </span>
      {/each}
    </div>
    <div class="tchart-canvas-wrap" style="height: {Math.max(56, resolvedSeries.length * 56)}px">
      <canvas bind:this={canvasEl} role="img" aria-label="Live telemetry strip chart"></canvas>
    </div>
  </div>
{/if}

<style>
  .tchart {
    background: var(--bg-card);
    border: 1px solid var(--line);
    border-radius: var(--r);
    padding: var(--gap);
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .tchart-legend {
    display: flex;
    flex-wrap: wrap;
    gap: 10px 16px;
  }

  .leg {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: 0.78rem;
    color: var(--ink-dim);
  }

  .swatch {
    width: 8px;
    height: 8px;
    border-radius: 2px;
    flex: 0 0 auto;
  }

  .leg-label { white-space: nowrap; }

  .leg-val {
    color: var(--ink);
    font-size: 0.85em;
  }

  .unit {
    color: var(--ink-dim);
    font-size: 0.85em;
    margin-left: 2px;
  }

  .tchart-canvas-wrap {
    position: relative;
    width: 100%;
    background: var(--bg-sunken);
    border: 1px solid var(--line);
    border-radius: var(--r-s);
    box-shadow: inset 0 2px 8px rgba(0, 0, 0, 0.5);
    overflow: hidden;
  }

  .tchart-canvas-wrap canvas {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    display: block;
  }
</style>
