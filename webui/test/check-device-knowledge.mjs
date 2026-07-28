/**
 * check-device-knowledge.mjs — the mechanical test behind gold-standard claim
 * #1: "Zero device knowledge in the rendering layer."
 *
 * A claim you cannot fail is not a standard, it is a mood. So this script
 * FAILS THE BUILD if the UI layer above core/slopsync/ contains knowledge that
 * only applies to this one machine:
 *
 *   1. A channel-id literal (0x0081, 0x0105, ...). Binding to an id is the
 *      original sin — it works here and nowhere else.
 *   2. A wire field name lifted from THIS device's catalog (`window_min`,
 *      `chase_dense_ms`, ...). The list is extracted from the firmware header
 *      at check time, so it stays honest as the catalog grows: add a field to
 *      the device and this check immediately starts guarding it too.
 *
 * WHAT IS DELIBERATELY ALLOWED:
 *   - core/slopsync/    — the protocol client. It is SUPPOSED to know wire
 *                         numbers; that is its entire job.
 *   - model/roles.js    — role strings like `window.min` are REGISTRY
 *                         vocabulary, not device facts. They mean the same
 *                         thing on every conforming hub, which is exactly why
 *                         binding to them is portable. Note they use dots,
 *                         while wire field names use underscores, so the two
 *                         are mechanically distinguishable.
 *
 * Run: node webui/test/check-device-knowledge.mjs
 */

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = fileURLToPath(new URL('.', import.meta.url));
const WEBUI = join(HERE, '..');
const SRC = join(WEBUI, 'src');
const CATALOG_H = join(WEBUI, '..', 'include', 'comms', 'SlopSyncCatalog.h');

/** Paths exempt from the check, and why. */
const EXEMPT = [
  join('src', 'core', 'slopsync'),   // the protocol client itself
  join('src', 'model', 'roles.js'),  // registry vocabulary, not device facts
];

function walk(dir, out = []) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) walk(p, out);
    else if (/\.(js|svelte)$/.test(name)) out.push(p);
  }
  return out;
}

/**
 * Harvest this device's wire FIELD NAMES from the firmware catalog header.
 *
 * Deliberately narrow: only `.name = "..."` initializers, i.e. the names of
 * actual layout/schema fields. An earlier version swept every snake_case
 * string literal in the header, which also caught OPTION LABELS — and that
 * produced a false positive on `estop_clear`, which is simultaneously a device
 * option label AND a key of the registry's own SAFETY_OP enum. Importing a
 * registry enum member is legitimate protocol vocabulary, not device
 * knowledge, so the check was wrong.
 *
 * That mattered more than the one case: a component author hit the false
 * positive and wrote an obscure workaround to get past it rather than
 * reporting the tool as broken. A checker that cries wolf gets routed around,
 * which is strictly worse than not having one. Precision is the feature.
 *
 * Names shorter than 5 chars are still skipped — "op", "mode", "speed" collide
 * with ordinary English and with local variable names.
 */
function deviceFieldNames() {
  let src;
  try {
    src = readFileSync(CATALOG_H, 'utf8');
  } catch (e) {
    console.error('! cannot read the device catalog header at', CATALOG_H);
    console.error('  (the field-name half of this check is skipped)');
    return new Set();
  }
  const names = new Set();
  for (const m of src.matchAll(/\.name\s*=\s*"([a-z][a-z0-9]*(?:_[a-z0-9]+)+)"/g)) {
    if (m[1].length >= 5) names.add(m[1]);
  }
  return names;
}

const files = walk(SRC).filter((p) => {
  const rel = relative(WEBUI, p);
  return !EXEMPT.some((e) => rel.startsWith(e));
});

const fieldNames = deviceFieldNames();
const findings = [];

/**
 * Blank out comments while PRESERVING line numbering.
 *
 * This matters more than it looks. The files most likely to discuss channel
 * ids and field names are exactly the ones that explain why binding to them is
 * forbidden — settings.js names `0x0081` in the very paragraph defining the
 * rule. A checker that cannot tell prose from code would punish the
 * documentation and teach people to delete the explanation to get green, which
 * is the opposite of what it is for.
 */
function stripComments(text) {
  const blank = (m) => m.replace(/[^\n]/g, ' ');
  return text
    .replace(/\/\*[\s\S]*?\*\//g, blank)   // /* ... */ and /** ... */, multi-line
    .replace(/<!--[\s\S]*?-->/g, blank)    // Svelte/HTML comments
    .replace(/\/\/[^\n]*/g, blank);        // // ...
}

for (const file of files) {
  const rel = relative(WEBUI, file).split(sep).join('/');
  const text = readFileSync(file, 'utf8');
  const lines = stripComments(text).split(/\r?\n/);
  const rawLines = text.split(/\r?\n/);

  lines.forEach((code, i) => {
    const line = rawLines[i] || '';

    // 1. channel-id literals
    for (const m of code.matchAll(/\b0x0[0-9a-fA-F]{3}\b/g)) {
      findings.push({ rel, line: i + 1, kind: 'channel id', hit: m[0], text: line.trim() });
    }

    // 2. this device's wire field names
    for (const name of fieldNames) {
      // Word-boundary match so `pattern` does not fire on `patterns`.
      const re = new RegExp('\\b' + name + '\\b');
      if (re.test(code)) {
        findings.push({ rel, line: i + 1, kind: 'device field name', hit: name, text: line.trim() });
      }
    }
  });
}

console.log('device-knowledge check');
console.log('  scanned  : ' + files.length + ' files under src/ (excluding ' + EXEMPT.length + ' exempt paths)');
console.log('  guarding : ' + fieldNames.size + ' wire field names harvested from the device catalog');

if (!findings.length) {
  console.log('\nPASS — the rendering layer knows nothing about this particular machine.');
  process.exit(0);
}

console.log('\nFAIL — ' + findings.length + ' leak(s) of device knowledge above core/slopsync/:\n');
for (const f of findings) {
  console.log('  ' + f.rel + ':' + f.line + '  [' + f.kind + ': ' + f.hit + ']');
  console.log('      ' + f.text.slice(0, 120));
}
console.log('\nEvery one of these makes the UI work on THIS device and no other.');
console.log('Bind to a catalog annotation or a registry role instead.');
process.exit(1);
