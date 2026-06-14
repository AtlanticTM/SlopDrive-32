/**
 * Motion Graph + Fixed-Cadence Telemetry Playback. :3
 *
 * The firmware runs a dedicated 10ms sampler and ships only the samples that
 * are NEW since our last poll (server tracks a monotonic seq; we send ?since=).
 * Each sample is [actual_pos_mm, planned_target_mm, raw_mapped_mm] and they are
 * spaced a fixed `dtMs` (10ms) apart. Three traces, three stages of the pipe:
 * "took" (where the shaft got), "told" (planner output), "asked" (rawest
 * TCode+RangeMapper demand, pre-planner). We append them to a local playback
 * queue, each

 * stamped with a browser-clock time computed PURELY locally:
 *
 *   nextSlotTs = max(lastScheduledTs + dtMs, now - small_lead)
 *
 * i.e. we lay the new samples down 10ms apart starting right after the last
 * one we scheduled — UNLESS we've fallen behind (a slow/late poll), in which
 * case we snap the schedule back up near "now" so playback never lags more
 * than a fraction of a second behind reality.
 *
 * WHY NO FIRMWARE TIMESTAMPS: the old design aligned firmware millis() to the
 * browser clock once and trusted it forever. An ESP32 reboot (millis() resets
 * to 0) made every future sample land thousands of seconds "in the past" and
 * the graph froze. By scheduling purely on the local clock at a known fixed
 * spacing, a reboot is a non-event — the rhythm just keeps going. :3
 *
 * The actual-position trace ("took") and commanded-target trace ("told") are
 * carried side-by-side through the whole pipeline so the graph can show the
 * Deceleration Trap made visible: where they diverge, the shaft is struggling
 * to keep up with what the host demanded. :3
 */

import { $, clamp } from '../core/ui.js';
import { TRAVEL, setPosTarget, setTargetMarker } from '../core/range.js';


// ---- Playback queue --------------------------------------------------------
// Ordered oldest→newest list of {ts, posMm, tgtMm}. `ts` is a browser
// performance.now() timestamp we assigned locally at a fixed cadence. The
// playback cursor walks this in real time and linearly interpolates. :3
var queue = [];            // [{ts, posMm, tgtMm}, ...]
var lastScheduledTs = 0;   // browser-ts assigned to the most recent queued sample
var defaultDtMs = 10;      // playback spacing; updated from firmware tele_dt

var lastPos = 0;           // last interpolated ACTUAL position (held when dry)
var lastTgt = 0;           // last interpolated COMMANDED target (held when dry)
var animRunning = false;

// Highest absolute firmware seq we've ALREADY swallowed. The firmware tags each
// batch with tele_from (the seq of samples[0]); since samples are strictly
// contiguous, sample i has absolute seq (fromSeq + i). We refuse to queue or
// plot any sample whose seq we've already taken — so a stale or overlapping
// poll response can never smear a duplicate line over the live one. Starts at
// -1 so seq 0 (the very first sample) is still accepted. Gaps are fine — if the
// browser falls behind and the firmware skips ahead, lastSeenSeq just jumps and
// the fixed-cadence playback glides over the hole. :3
var lastSeenSeq = -1;


// We re-anchor every batch so the newest sample sits PLAYBACK_LEAD_MS ahead of
// "now" — a tiny buffer so interpolation always has a "next" sample to glide
// toward. MAX_LEAD_MS is the hard ceiling: even if batches overlap or arrive
// bursty, the schedule can never sit more than this far ahead of real-time, so
// playback can't accumulate the multi-second lag the old free-running schedule
// suffered. :3
const PLAYBACK_LEAD_MS = 30;   // keep ~30ms of buffered lead for smoothness
const MAX_LEAD_MS      = 150;  // hard ceiling on how far ahead of now we sit


// ---- Graph data store ------------------------------------------------------
// Same samples, kept for the rolling chart. Pruned to GRAPH_WINDOW_MS. :3
var graphData = [];        // [{ts, posMm, tgtMm}, ...]

const MAX_PLOT_POINTS = 2000;
const GRAPH_WINDOW_MS  = 10000;

// ---- Canvas refs -----------------------------------------------------------
var graphCanvas = null;
var graphCtx    = null;

/**
 * Push a fresh telemetry batch from a /api/status poll.
 * @param {Array<Array<number>>} rawSamples - [[actual_pos_mm, target_mm], ...]
 *        ordered oldest→newest, each a fixed dtMs apart.
 * @param {number} dtMs - fixed spacing between samples (firmware tele_dt).
 * @param {number} [fromSeq] - absolute firmware seq of rawSamples[0] (tele_from).
 *        Used to drop samples we've already swallowed. If omitted (older
 *        firmware), dedupe is skipped and we trust the poll mutex alone. :3
 */
export function pushTelemetryBatch(rawSamples, dtMs, fromSeq) {
  if (!rawSamples || rawSamples.length === 0) return;
  if (typeof dtMs === 'number' && dtMs > 0) defaultDtMs = dtMs;

  // ---- Seq dedupe — refuse anything we've already taken. -----------------
  // If the firmware told us the batch's starting seq, slice off any leading
  // samples whose absolute seq (fromSeq + i) we've already plotted. An
  // overlapping/stale poll re-shipping an old window thus gets fully swallowed
  // and produces ZERO duplicate points. We then advance lastSeenSeq to the last
  // sample we accept. No fromSeq? Skip the slice and lean on the poll mutex. :3
  var skip = 0;
  if (typeof fromSeq === 'number') {
    if (fromSeq + rawSamples.length - 1 <= lastSeenSeq) return;  // entire batch is stale
    if (fromSeq <= lastSeenSeq) skip = lastSeenSeq - fromSeq + 1;
    lastSeenSeq = fromSeq + rawSamples.length - 1;
  }
  if (skip > 0) rawSamples = rawSamples.slice(skip);
  if (rawSamples.length === 0) return;

  var now = performance.now();
  var n = rawSamples.length;

  // ---- Anchor the batch to real-time, every time. ------------------------
  // The single most important rule: the NEWEST sample in this batch always
  // lands just slightly ahead of "now" (PLAYBACK_LEAD_MS). The rest are laid
  // out backward at the fixed dtMs spacing. This locks playback ~30ms behind
  // wall-clock no matter what — no free-running schedule that can drift seconds
  // ahead and make "told" lag forever. If a poll is late, we simply continue
  // from real-time; if it's early, same thing. Playback can NEVER accumulate
  // lag because it's re-anchored to now on every batch. :3
  //
  // To avoid a visible seam, we don't let the new batch start BEFORE the last
  // sample we already scheduled (which we're mid-playing) — we nudge the anchor
  // forward a touch if they'd overlap. But we hard-cap how far ahead we sit so
  // a burst of backlog can't push us into the future.
  var lastTs = n * defaultDtMs;                    // span of this batch
  var anchorEnd = now + PLAYBACK_LEAD_MS;          // where the newest sample sits
  // Don't rewind before what we're currently playing, but never sit more than
  // MAX_LEAD_MS ahead of now (kills runaway buffering / multi-second lag).
  if (anchorEnd < lastScheduledTs + defaultDtMs) anchorEnd = lastScheduledTs + defaultDtMs;
  if (anchorEnd > now + MAX_LEAD_MS) anchorEnd = now + MAX_LEAD_MS;
  var startTs = anchorEnd - (n - 1) * defaultDtMs; // first sample of the batch

  // Lay the samples down at a fixed dtMs cadence.
  for (var i = 0; i < n; i++) {
    var ts  = startTs + i * defaultDtMs;
    var pos = clamp(rawSamples[i][0], 0, TRAVEL);
    var tgt = clamp(rawSamples[i].length > 1 ? rawSamples[i][1] : rawSamples[i][0], 0, TRAVEL);
    // Third element = the rawest demand ("asked"), straight out of the TCode
    // parser + RangeMapper before the planner shaped it. Older firmware that
    // only ships 2 elements falls back to mirroring the target so nothing
    // breaks. :3
    var raw = clamp(rawSamples[i].length > 2 ? rawSamples[i][2] : tgt, 0, TRAVEL);
    queue.push({ ts: ts, posMm: pos, tgtMm: tgt, rawMm: raw });
    graphData.push({ ts: ts, posMm: pos, tgtMm: tgt, rawMm: raw });
    lastScheduledTs = ts;
  }


  // Prune the playback queue — drop samples that are well in the past (already
  // played). Keep a short tail so getInterpolatedPos always has a `prev`. :3
  var qcut = now - 500;
  while (queue.length > 1 && queue[0].ts < qcut) queue.shift();

  pruneGraphData(now);

  if (!animRunning) startAnimLoop();
}


function pruneGraphData(now) {
  var cutoff = now - GRAPH_WINDOW_MS;
  while (graphData.length > 0 && graphData[0].ts < cutoff) {
    graphData.shift();
  }
  if (graphData.length > MAX_PLOT_POINTS) {
    graphData.splice(0, graphData.length - MAX_PLOT_POINTS);
  }
}

// ---- Wall-clock playback interpolation -------------------------------------
// Find the two queued samples bracketing nowMs and linearly interpolate. Past
// the newest sample we hold the last value (no extrapolation — honest motion,
// no fake predictions). :3
function getInterpolatedPos(nowMs) {
  if (queue.length === 0) return lastPos;

  // Drop fully-consumed samples (anything with a successor also <= now).
  while (queue.length > 1 && queue[1].ts <= nowMs) queue.shift();

  var prev = queue[0];
  if (queue.length === 1 || nowMs <= prev.ts) {
    // Before the first sample, or only one left — hold it.
    lastPos = prev.posMm;
    lastTgt = prev.tgtMm;
    return lastPos;
  }

  var next = queue[1];
  if (nowMs >= next.ts) {
    lastPos = next.posMm;
    lastTgt = next.tgtMm;
    return lastPos;
  }

  // Interpolate prev→next. Actual ("took") and target ("told") glide in
  // lockstep so the two traces stay time-aligned. :3
  var span = next.ts - prev.ts;
  var frac = span > 0 ? clamp((nowMs - prev.ts) / span, 0, 1) : 1;
  lastPos = prev.posMm + (next.posMm - prev.posMm) * frac;
  lastTgt = prev.tgtMm + (next.tgtMm - prev.tgtMm) * frac;
  return lastPos;
}


// ---- rAF animation loop ----------------------------------------------------
// Runs at the display's native refresh rate. Every frame: interpolate the
// position, drive the stroke-window dot + target marker, redraw the graph. :3
function animFrame(nowMs) {
  animRunning = true;

  var pos = getInterpolatedPos(nowMs);
  setPosTarget(pos);
  setTargetMarker(lastTgt);

  drawGraph(nowMs);

  requestAnimationFrame(animFrame);
}

function startAnimLoop() {
  if (animRunning) return;
  animRunning = true;
  requestAnimationFrame(animFrame);
}

// ===================== Motion Graph Canvas ==================================
// Rolling position-vs-time chart. Horizontal axis = browser time (left=older,
// right=now). Vertical axis = 0..TRAVEL mm. :3

export function initMotionGraph() {
  var cv = $('#motionGraphCanvas');
  if (!cv) return;
  graphCanvas = cv;
  graphCtx = cv.getContext('2d');
  sizeGraphCanvas();
  window.addEventListener('resize', sizeGraphCanvas);

  // ResizeObserver: the canvas often gets created while its tab is hidden
  // (clientWidth/Height = 0), so the initial sizing comes up empty and the
  // graph stays a squished little nub until a full window resize. Watching the
  // element directly means the instant the tab is shown and the canvas swells
  // to its real size, we re-measure and the trace fills out properly. No more
  // limp little graph that won't stand up until you jiggle the window. :3
  if (typeof ResizeObserver !== 'undefined') {
    var ro = new ResizeObserver(function () { sizeGraphCanvas(); });
    ro.observe(graphCanvas);
  }
}


function sizeGraphCanvas() {
  if (!graphCanvas) return;
  var w = graphCanvas.clientWidth;
  var h = graphCanvas.clientHeight;
  var dpr = window.devicePixelRatio || 1;
  if (graphCanvas.width !== Math.round(w * dpr) || graphCanvas.height !== Math.round(h * dpr)) {
    graphCanvas.width  = Math.round(w * dpr);
    graphCanvas.height = Math.round(h * dpr);
  }
}

function drawGraph(nowMs) {
  if (!graphCtx || !graphCanvas) return;

  var w = graphCanvas.clientWidth;
  var h = graphCanvas.clientHeight;
  if (w === 0 || h === 0) return;

  var dpr = window.devicePixelRatio || 1;
  graphCtx.setTransform(dpr, 0, 0, dpr, 0, 0);
  graphCtx.clearRect(0, 0, w, h);

  var viewEnd   = nowMs;
  var viewStart = viewEnd - GRAPH_WINDOW_MS;

  // ---- Grid lines (subtle) ------------------------------------------------
  graphCtx.strokeStyle = 'rgba(44,49,88,0.35)';
  graphCtx.lineWidth = 0.5;

  // Vertical grid every 2 seconds.
  for (var ts = Math.ceil(viewStart / 2000) * 2000; ts <= viewEnd; ts += 2000) {
    var x = ((ts - viewStart) / GRAPH_WINDOW_MS) * w;
    graphCtx.beginPath(); graphCtx.moveTo(x, 0); graphCtx.lineTo(x, h); graphCtx.stroke();
  }

  // Horizontal grid at 25%, 50%, 75% travel.
  [0.25, 0.5, 0.75].forEach(function (frac) {
    var y = h - frac * h;
    graphCtx.beginPath(); graphCtx.moveTo(0, y); graphCtx.lineTo(w, y); graphCtx.stroke();
  });

  // ---- Data line ----------------------------------------------------------
  pruneGraphData(nowMs);

  if (graphData.length >= 2) {
    // ---- Raw "asked" trace (the rawest demand, pre-planner) ---------------
    // Drawn FIRST (underneath everything) so it's the foundation the other two
    // ride on top of. A faint blue dotted line — exactly what the host ASKED
    // for, straight out of the TCode parser + RangeMapper before our kinematics
    // planner laid a finger on it. Compare it to the amber "told" line to see
    // how much the planner reshapes the demand; compare it to the green "took"
    // line to see total end-to-end fidelity. Three stages, one glance. :3
    graphCtx.strokeStyle = 'rgba(96,165,250,0.55)';
    graphCtx.lineWidth = 1.0;
    graphCtx.lineJoin = 'round';
    graphCtx.setLineDash([2, 3]);
    graphCtx.beginPath();
    var rfirst = true;
    for (var ri = 0; ri < graphData.length; ri++) {
      var rpt = graphData[ri];
      var rpx = ((rpt.ts - viewStart) / GRAPH_WINDOW_MS) * w;
      var rpy = h - (clamp(rpt.rawMm, 0, TRAVEL) / TRAVEL) * h;
      if (rfirst) { graphCtx.moveTo(rpx, rpy); rfirst = false; }
      else         { graphCtx.lineTo(rpx, rpy); }
    }
    graphCtx.stroke();
    graphCtx.setLineDash([]);

    // ---- Target trace (what the host TOLD us to take) ---------------------

    // Drawn FIRST so the actual-position trace rides on top of it. A muted
    // amber dashed line — the demand the shaft is chasing. Where target and
    // actual diverge, you can literally see the machine struggling to keep up
    // (the Deceleration Trap made visible). :3
    graphCtx.strokeStyle = 'rgba(251,191,36,0.7)';
    graphCtx.lineWidth = 1.25;
    graphCtx.lineJoin = 'round';
    graphCtx.setLineDash([5, 4]);
    graphCtx.beginPath();
    var tfirst = true;
    for (var ti = 0; ti < graphData.length; ti++) {
      var tpt = graphData[ti];
      var tpx = ((tpt.ts - viewStart) / GRAPH_WINDOW_MS) * w;
      var tpy = h - (clamp(tpt.tgtMm, 0, TRAVEL) / TRAVEL) * h;
      if (tfirst) { graphCtx.moveTo(tpx, tpy); tfirst = false; }
      else         { graphCtx.lineTo(tpx, tpy); }
    }
    graphCtx.stroke();
    graphCtx.setLineDash([]);

    // Main trace (where the shaft ACTUALLY got to).
    graphCtx.strokeStyle = 'rgba(52,211,153,0.85)';

    graphCtx.lineWidth = 1.5;
    graphCtx.lineJoin = 'round';
    graphCtx.beginPath();
    var first = true;
    for (var i = 0; i < graphData.length; i++) {
      var pt = graphData[i];
      var px = ((pt.ts - viewStart) / GRAPH_WINDOW_MS) * w;
      var py = h - (clamp(pt.posMm, 0, TRAVEL) / TRAVEL) * h;
      if (first) { graphCtx.moveTo(px, py); first = false; }
      else        { graphCtx.lineTo(px, py); }
    }
    graphCtx.stroke();

    // Glow bloom.
    graphCtx.strokeStyle = 'rgba(52,211,153,0.12)';
    graphCtx.lineWidth = 6;
    graphCtx.stroke();
  } else if (lastPos > 0) {
    // Not enough data yet — flat line at last known position.
    var yFlat = h - (clamp(lastPos, 0, TRAVEL) / TRAVEL) * h;
    graphCtx.strokeStyle = 'rgba(52,211,153,0.25)';
    graphCtx.lineWidth = 1;
    graphCtx.beginPath(); graphCtx.moveTo(0, yFlat); graphCtx.lineTo(w, yFlat); graphCtx.stroke();
  }

  // ---- Current position marker --------------------------------------------
  var markerY = h - (clamp(lastPos, 0, TRAVEL) / TRAVEL) * h;

  // Dashed horizontal line.
  graphCtx.strokeStyle = 'rgba(52,211,153,0.45)';
  graphCtx.lineWidth = 1;
  graphCtx.setLineDash([4, 8]);
  graphCtx.beginPath(); graphCtx.moveTo(0, markerY); graphCtx.lineTo(w, markerY); graphCtx.stroke();
  graphCtx.setLineDash([]);

  // Glowing dot at the right edge.
  graphCtx.fillStyle = '#34d399';
  graphCtx.shadowColor = 'rgba(52,211,153,0.8)';
  graphCtx.shadowBlur = 8;
  graphCtx.beginPath(); graphCtx.arc(w - 6, markerY, 4, 0, Math.PI * 2); graphCtx.fill();
  graphCtx.shadowBlur = 0;

  // ---- Time labels --------------------------------------------------------
  graphCtx.fillStyle = 'rgba(107,112,154,0.65)';
  graphCtx.font = '10px -apple-system, BlinkMacSystemFont, sans-serif';
  graphCtx.textAlign = 'center';
  for (var tsL = Math.ceil(viewStart / 2000) * 2000; tsL <= viewEnd; tsL += 2000) {
    var lx = ((tsL - viewStart) / GRAPH_WINDOW_MS) * w;
    var sec = Math.round((tsL - nowMs) / 1000);
    graphCtx.fillText(sec + 's', lx, h - 4);
  }

  // ---- Position scale labels ----------------------------------------------
  graphCtx.fillStyle = 'rgba(107,112,154,0.55)';
  graphCtx.textAlign = 'right';
  [0, 0.25, 0.5, 0.75, 1].forEach(function (frac) {
    var yy = h - frac * h + 4;
    graphCtx.fillText(Math.round(frac * TRAVEL), w - 6, yy);
  });
}

/** Return the current interpolated ACTUAL position (for external readouts). */
export function getSmoothedPosition() { return lastPos; }

/** Return the current interpolated COMMANDED target (for external readouts). */
export function getSmoothedTarget() { return lastTgt; }
