/**
 * model-vs-device.mjs — build the renderable model from a LIVE machine's
 * catalog and print the page it would produce.
 *
 * This is the fastest way to see whether the generic renderer actually covers a
 * real device, without rendering anything: connect, adopt the catalog, run
 * buildSettingsModel(), and dump the tabs -> cards -> fields tree with the
 * widget each field resolved to.
 *
 * It also reports the things that would make the page WRONG rather than merely
 * plain: settings whose write channel is missing, fields that resolved to a
 * fallback widget, and channels that carry a category but no annotated fields.
 *
 * Read-only: subscribes to nothing, sends no intent, touches no motion.
 *
 * Run: node webui/test/model-vs-device.mjs [host] [port]
 */

import { createSession } from '../../../SlopSync/clients/js/index.js';
import { acquireToken } from '../../../SlopSync/clients/js/credentials.js';
import { buildSettingsModel, WIDGET } from '../src/model/settings.js';
import { claimRoles, ROLE } from '../src/model/roles.js';

const HOST = process.argv[2] || '192.168.1.229';
const PORT = parseInt(process.argv[3] || '82', 10);

const s = createSession({
  host: HOST, port: PORT, clientKind: 'webui', clientName: 'model-probe',
  autoReconnect: false, WebSocketImpl: WebSocket,
  token: (h) => acquireToken(h),
});

const done = new Promise((resolve, reject) => {
  const to = setTimeout(() => reject(new Error('timeout waiting for catalog')), 20000);
  s.on('catalog', (entries, _m, meta) => { clearTimeout(to); resolve({ entries, meta }); });
  s.on('close', (c) => { clearTimeout(to); reject(new Error('closed: ' + (c.reason || c.code))); });
});

s.connect();
const { entries, meta } = await done;

console.log('host      : ' + HOST + ':' + PORT);
console.log('entries   : ' + entries.length);
console.log('catalog   : ' + (s.catalogBytes ? s.catalogBytes.length : '?') + ' B'
  + (meta && meta.cached ? ' (from cache)' : ' (fetched)'));
console.log('tier      : ' + (s.state.roles | 0));

const model = buildSettingsModel(entries);

console.log('\n=== THE PAGE THIS MACHINE PRODUCES ===');
for (const cat of model.categories) {
  const n = cat.groups.reduce((a, g) => a + g.fields.length, 0);
  console.log('\n  TAB  ' + cat.label + '   (category ' + cat.id + ', '
    + n + ' fields, ' + (cat.writable ? 'writable' : 'read-only') + ')');
  for (const g of cat.groups) {
    console.log('    CARD  ' + (g.name || '(ungrouped)'));
    for (const f of g.fields) {
      const w = f.widget.padEnd(9);
      const bounds = (f.min != null && f.max != null) ? ('[' + f.min + '..' + f.max + ']') : '';
      const key = f.readOnly ? 'read-only' : ('-> ch 0x' + f.writeChannel.toString(16) + ' key ' + f.settingKey);
      console.log('      ' + w + ' ' + f.name.padEnd(20) + ' ' + String(f.unit).padEnd(7)
        + ' ' + bounds.padEnd(18) + ' ' + key);
    }
  }
}

console.log('\n=== ACTIONS (RFC-019 action.* roles) ===');
if (!model.actions.length) console.log('  none — this machine exposes no discoverable verbs');
for (const a of model.actions) {
  console.log('  ' + a.role.padEnd(18) + ' ch 0x' + a.channelId.toString(16)
    + ' key ' + a.key + '  options: ' + (a.options ? a.options.join(', ') : '(none)'));
}

console.log('\n=== HERO CLAIMS (bespoke widgets, bound by role) ===');
const specs = [
  ['rail', { require: { min: ROLE.windowMin, max: ROLE.windowMax },
             optional: { pos: ROLE.telemetryPosition, vel: ROLE.telemetryVelocity } }],
  ['pattern', { require: { running: ROLE.patternRunning, select: ROLE.patternSelect },
                optional: { speed: ROLE.patternSpeed, depth: ROLE.patternDepth,
                            stroke: ROLE.patternStroke, sensation: ROLE.patternSensation } }],
  ['limits', { require: { userSpeed: ROLE.limitUserSpeed, userAccel: ROLE.limitUserAccel },
               optional: { inputSpeed: ROLE.limitInputSpeed, inputAccel: ROLE.limitInputAccel,
                           inputJerk: ROLE.limitInputJerk } }],
];
for (const [name, spec] of specs) {
  const c = claimRoles(model.byRole, spec);
  if (!c) { console.log('  ' + name.padEnd(9) + ' DECLINED — required roles absent on this machine'); continue; }
  const bound = Object.entries(c).filter(([k, v]) => k !== 'claimed' && v)
    .map(([k, v]) => k + '=' + v.name).join(' ');
  console.log('  ' + name.padEnd(9) + ' claims ' + c.claimed.size + ' fields: ' + bound);
}

// ---- things that would make the page WRONG, not merely plain ---------------
console.log('\n=== WARNINGS ===');
let warn = 0;
for (const f of model.fields) {
  if (!f.readOnly && !entries.some((e) => e.id === f.writeChannel)) {
    console.log('  ! ' + f.name + ' names write channel 0x' + f.writeChannel.toString(16)
      + ' which is NOT in the catalog — the control would render and do nothing'); warn++;
  }
  if (!f.readOnly && f.widget === WIDGET.number && f.min == null && f.max == null) {
    console.log('  ~ ' + f.name + ' has no bounds; renders as a bare number box'); warn++;
  }
  if (!f.readOnly && !f.desc) {
    console.log('  ~ ' + f.name + ' has no desc — no tooltip text for any client'); warn++;
  }
}
const categorized = entries.filter((e) => e.category != null);
for (const e of categorized) {
  if (!model.fields.some((f) => f.channelId === e.id)) {
    console.log('  ! channel ' + e.name + ' carries a category but produced no fields'); warn++;
  }
}
if (!warn) console.log('  none');

// Uncategorized channels never reach the settings surface at all — worth
// listing so a missing tab is traceable to a missing annotation.
const uncat = entries.filter((e) => e.category == null && e.layout && e.layout.length);
console.log('\n=== UNCATEGORIZED STATE CHANNELS (no settings tab; telemetry only) ===');
for (const e of uncat) console.log('  0x' + e.id.toString(16).padStart(4, '0') + ' ' + e.name);

s.close();
setTimeout(() => process.exit(0), 300);
