// Self-check for logview.html's parser and collapse heuristic.
//   node tools/logview/test_logview.mjs      (exit 0 = pass)
//
// The page is one file with no build step, so the test loads it by extracting
// the <script> block and evaluating it against a fake `module`. That keeps the
// shipped artifact a single file and still leaves the only nontrivial logic
// (parse + group) under a runnable check.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import assert from 'node:assert/strict';

const here = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(join(here, 'logview.html'), 'utf8');
const src = html.slice(html.indexOf('<script>') + 8, html.lastIndexOf('</script>'));
const mod = { exports: {} };
new Function('module', src)(mod);
const { parse, isFooter, group } = mod.exports;

// ---- parse ------------------------------------------------------------------
const r = parse('[35074.845 I sys] heap free=182483 min=179419 maxblock=108532');
assert.equal(r.ms, 35074845);
assert.equal(r.lvl, 2);
assert.equal(r.tag, 'sys');
assert.equal(r.msg, 'heap free=182483 min=179419 maxblock=108532');

// Leading pad and every level letter.
assert.equal(parse('[    0.184 I boot] up').ms, 184);
assert.deepEqual('TDIWEF'.split('').map(c => parse(`[1.000 ${c} t] m`).lvl), [0, 1, 2, 3, 4, 5]);

// A message containing ']' must not truncate: the tag group is non-greedy on
// ']' but the message group is not.
assert.equal(parse('[1.000 W aim] refused [limit] hit').msg, 'refused [limit] hit');

// Unparseable input is kept as raw, never dropped.
assert.equal(parse('garbage from a half-decoded frame').raw, true);

// ---- footers are status, not records ----------------------------------------
assert.equal(isFooter('[diag] 9 lines emitted, 3593 held (16384 capacity), 0 evicted by wrap next=3593'), true);
assert.equal(isFooter('[sloplog] webring evicted T:0 D:1841'), true);
assert.equal(isFooter('[35074.845 I sys] heap free=1'), false);

// ---- shape key --------------------------------------------------------------
// Same call site, different values -> same key. This is the whole collapse.
const a = parse('[10.000 I sys] heap free=182483 min=179419 psram=5360684');
const b = parse('[20.000 I sys] heap free=171008 min=170112 psram=5360600');
assert.equal(a.key, b.key);

// Different level or different tag is a different call site, even verbatim.
assert.notEqual(parse('[1.0 I t] same text').key, parse('[1.0 W t] same text').key);
assert.notEqual(parse('[1.0 I one] same text').key, parse('[1.0 I two] same text').key);

// Non-numeric difference must NOT collapse: only digits are wildcards.
assert.notEqual(parse('[1.0 I sys] motor stalled').key, parse('[1.0 I sys] motor homed').key);

// ---- group ------------------------------------------------------------------
const hit = [
  parse('[1.000 I sys] heap free=100'),
  parse('[2.000 W aim] direction change refused'),
  parse('[3.000 I sys] heap free=200'),
  parse('[4.000 I sys] heap free=300'),
];
const rows = group(hit, new Set());
assert.equal(rows.length, 2, 'three heap lines fold into one row');
// A group sits at its LATEST occurrence, so the repeater lands after the rare line.
assert.equal(rows[0].r.tag, 'aim');
assert.equal(rows[1].n, 3);
assert.equal(rows[1].r.msg, 'heap free=300', 'group shows the most recent values');

// Expanding splices members in beneath the summary row.
const open = group(hit, new Set([rows[1].key]));
assert.equal(open.length, 2 + 3);
assert.equal(open[2].child, true);

// Ungrouped rows carry n===1 so the renderer never prints a "1x" badge.
assert.equal(rows[0].n, 1);

console.log('logview: all checks passed');
