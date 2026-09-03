#!/usr/bin/env python3
"""segtrace -- read the `segin` / `plan` / `motion` traces and say what the stream asked for.

Pulls the three tags from the C5 bridge (or reads saved files), joins segin
to plan by `due`, and prints the census a feel question needs: per-segment
demanded speed against the ceiling, client lead, sentinel use, gaps between
segments, plan-kind flips, settles and their spacing, and the anomaly events
placed against the segments that caused them. Defaults --out under artifacts/.

Usage:
  python tools/segtrace.py --c5 192.168.1.71 --window 87:187 --ceiling 1000
  python tools/segtrace.py --dir artifacts/segtrace-<stamp>   (re-read a pull)
"""
import argparse, collections, json, os, re, sys, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def pull(c5, tag, timeout=30):
    with urllib.request.urlopen(f'http://{c5}/api/diag/{tag}', timeout=timeout) as r:
        return r.read().decode('utf-8', 'replace')

def parse_kv(line):
    d = {}
    for k, v in re.findall(r'(\w+)=(\S+)', line):
        d[k] = v
    return d

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--c5', default=None, help='bridge address; omit to read --dir')
    ap.add_argument('--dir', default=None, help='directory holding segin.txt/plan.txt/motion.txt')
    ap.add_argument('--window', default='0:1', help='MIN:MAX mm of the stroke window (from the cfg line)')
    ap.add_argument('--ceiling', type=float, default=1000.0, help='input max speed mm/s')
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    lo, hi = (float(x) for x in a.window.split(':'))
    span = hi - lo
    out = a.out or a.dir or os.path.join(ROOT, 'artifacts', 'segtrace-' + time.strftime('%Y%m%d-%H%M%S'))
    os.makedirs(out, exist_ok=True)
    texts = {}
    for tag in ('segin', 'plan', 'motion', 'smplan'):
        p = os.path.join(out, tag + '.txt')
        if a.c5:
            texts[tag] = pull(a.c5, tag); open(p, 'w', encoding='utf-8').write(texts[tag])
        else:
            texts[tag] = open(p, encoding='utf-8').read() if os.path.exists(p) else ''

    segin, plan, events = [], [], []
    for l in texts['segin'].splitlines():
        m = re.match(r'\[\s*([\d.]+) I segin\] (.*)', l)
        if m:
            d = parse_kv(m.group(2)); d['t'] = float(m.group(1)); segin.append(d)
    for l in texts['plan'].splitlines():
        m = re.match(r'\[\s*([\d.]+) [IW] plan\] (.*)', l)
        if m and 'due=' in l:
            d = parse_kv(m.group(2)); d['t'] = float(m.group(1))
            mk = re.search(r'-> (\w+)/(\w+)', l); d['kind'], d['mode'] = (mk.group(1), mk.group(2)) if mk else ('?', '?')
            d['refused'] = 'REFUSED' in l; plan.append(d)
    for l in texts['motion'].splitlines():
        m = re.match(r'\[\s*([\d.]+) I motion\] slopmotion (\w+) target=([\d.]+) detail=([-\d.]+)', l)
        if m: events.append((float(m.group(1)), m.group(2), float(m.group(3)), float(m.group(4))))

    print(f'segin {len(segin)}  plan {len(plan)}  events {len(events)}  -> {out}')
    if not segin:
        print('no segin lines: was the S3 on 2.4.124+ and streaming?'); return 1

    # ---- what the client asked for -----------------------------------------
    prev = None; speeds = []; leads = []; gaps = []; sent = 0; short = 0
    for s in segin:
        tgt = float(s['tgt']); dur = int(s['dur']); lead = int(s['lead']); due = int(s['due'])
        leads.append(lead / 1000.0)
        if s['vf'].startswith('S'): sent += 1
        if dur < 50: short += 1
        if prev is not None:
            v = abs(tgt - prev[0]) * span / (dur / 1000.0)
            speeds.append((v, s['t'], tgt, dur))
            gaps.append((due - prev[1]) / 1000.0)   # ms from previous due to this due
        prev = (tgt, due + dur * 1000, dur)
    speeds.sort(reverse=True)
    over = [x for x in speeds if x[0] > a.ceiling]
    print('\n== INPUT (what MFP sent) ==')
    print(f'segments {len(segin)}, sentinel end-vel {sent} ({100*sent/len(segin):.0f}%), under-50ms {short}')
    print(f'chord speed: max {speeds[0][0]:.0f} mm/s, p90 {speeds[len(speeds)//10][0]:.0f}, over ceiling ({a.ceiling:.0f}) {len(over)} of {len(speeds)} ({100*len(over)/max(1,len(speeds)):.0f}%)')
    if leads:
        ls = sorted(leads); print(f'client lead ms: min {ls[0]:.1f}  p50 {ls[len(ls)//2]:.1f}  max {ls[-1]:.1f}  late(0) {sum(1 for x in ls if x<=0)}')
    if gaps:
        g = sorted(gaps); print(f'gap between consecutive segment ends and next starts, ms: min {g[0]:.0f} p50 {g[len(g)//2]:.0f} p90 {g[int(len(g)*.9)]:.0f} max {g[-1]:.0f}; >200 ms: {sum(1 for x in g if x>200)}')
    print('top over-ceiling chords (mm/s, t, target, dur ms):')
    for v, t, tgt, dur in over[:8]: print(f'  {v:6.0f}  t={t:8.3f}  tgt={tgt:.3f}  dur={dur}')

    # ---- what the planner did ------------------------------------------------
    print('\n== PLAN (what the engine did) ==')
    kinds = collections.Counter(p['kind'] for p in plan); modes = collections.Counter(p['mode'] for p in plan)
    print('plan kinds:', dict(kinds), ' modes:', dict(modes), ' refused:', sum(p['refused'] for p in plan))
    flips = sum(1 for i in range(1, len(plan)) if plan[i]['kind'] != plan[i-1]['kind'])
    print(f'kind flips between consecutive commits: {flips} of {max(0,len(plan)-1)}')
    lates = sorted(int(p['late']) / 1000.0 for p in plan)
    if lates: print(f'commit lateness ms: p50 {lates[len(lates)//2]:.1f} max {lates[-1]:.1f}')
    pus = sorted(int(p['plan'].rstrip('us')) for p in plan if 'plan' in p)
    if pus: print(f'plan time us: p50 {pus[len(pus)//2]} max {pus[-1]}')
    ev = collections.Counter(k for _, k, _, _ in events); print('anomaly events:', dict(ev))
    st = [t for t, k, _, _ in events if k == 'settle']
    if len(st) > 1: print(f'settles {len(st)}, mean spacing {(st[-1]-st[0])/(len(st)-1):.2f} s')
    # place each fallback/settle against the nearest preceding segin
    print('\n== EVENTS placed against the segment in flight ==')
    for t, k, tgt, det in events[-40:]:
        seg = max((s for s in segin if s['t'] <= t), key=lambda s: s['t'], default=None)
        if seg is None: continue
        v = None
        idx = segin.index(seg)
        if idx > 0:
            v = abs(float(seg['tgt']) - float(segin[idx-1]['tgt'])) * span / (int(seg['dur']) / 1000.0)
        print(f'  {t:8.3f} {k:18s} det={det:+.3f}  seg: tgt={seg["tgt"]} dur={seg["dur"]}ms vf={seg["vf"]} lead={int(seg["lead"])/1000:.1f}ms chord={v if v is None else round(v)} mm/s')
    json.dump({'segin': segin, 'plan': plan, 'events': events}, open(os.path.join(out, 'trace.json'), 'w'), indent=1)
    return 0

if __name__ == '__main__':
    sys.exit(main())
