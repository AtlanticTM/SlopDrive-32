/**
 * Connection-health indicator — the telemetry-gap dot (#teleDot) in the header.
 *
 *   Green  = telemetry flowing clean.
 *   Amber  = dropped frames detected recently (poor connection) OR heartbeat late.
 *   Red    = link down / HTTP fallback / heartbeat stalled.
 *
 * Distinguishes "idle but connected" from "connection stalled" via the STATUS
 * heartbeat age: STATUS keeps flowing (~2Hz) even when the machine is idle and
 * emitting no 0x01 motion frames, so an idle-but-connected rig stays GREEN
 * instead of falsely flashing amber/red. Dropped-frame detection rides the 0x01
 * sequence counter (link.js), so it only fires on frames that genuinely went
 * missing between two that arrived — never on idle. :3
 *
 * The diagnostics graph consumes the SAME link stats to shade gap spans yellow
 * (see features/diag.js) — one source of truth for connection trouble.
 */
import { $ } from '../core/ui.js';
import { getStats } from '../core/link.js';

var DROP_FLASH_MS = 1800;   // hold amber this long after a detected drop
var HB_WARN_MS    = 1500;   // heartbeat late (missed a couple of ~500ms beats)
var HB_BAD_MS     = 3000;   // heartbeat stalled → treat the link as down

var _dot = null;

function set(cls, title) {
  if (!_dot) return;
  var full = 'link-dot ' + cls;
  if (_dot.className !== full) _dot.className = full;   // avoid needless reflow
  if (_dot.title !== title) _dot.title = title;
}

function update() {
  if (!_dot) return;
  var s = getStats();
  var now = performance.now();

  // Hard down: socket closed or on HTTP fallback.
  if (!s.connected) { set('bad', 'Telemetry link OFFLINE — reconnecting'); return; }
  if (s.fallback)   { set('bad', 'Telemetry link degraded — HTTP fallback'); return; }

  var hbAge = s.lastStatusMs > 0 ? (now - s.lastStatusMs) : 0;
  if (s.lastStatusMs > 0 && hbAge > HB_BAD_MS) {
    set('bad', 'Telemetry stalled — no heartbeat for ' + Math.round(hbAge) + 'ms');
    return;
  }

  var recentDrop = s.lastGapMs > 0 && (now - s.lastGapMs) < DROP_FLASH_MS;
  var hbLate     = s.lastStatusMs > 0 && hbAge > HB_WARN_MS;
  if (recentDrop || hbLate) {
    set('warn', 'Telemetry gapping · ' + s.droppedFrames + ' frame' +
        (s.droppedFrames === 1 ? '' : 's') + ' dropped');
    return;
  }

  set('good', 'Telemetry link OK · ' + s.droppedFrames + ' dropped since load');
}

export function initConn() {
  _dot = $('#teleDot');
  if (!_dot) return;
  set('good', 'Telemetry link');
  setInterval(update, 200);
}
