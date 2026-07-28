#!/usr/bin/env python3
"""SlopSync channel-space grid visual — renders the 0xCDSS allocation as HTML.

Shows every slot: allocated, named-reserved, and (the point) EMPTY runway.
Phase C4 landed the family-nibble sub-slot convention this visual proposed;
the device-space allocation below is now PARSED FROM THE LIVE CATALOG
(SlopSyncCatalog.h, via gen_channel_map's own parsing helpers — one source of
truth, never a second hand-typed table to drift from it). Named reserves
(battery/thermal) are not real catalog entries yet, so they stay a small
static overlay.

Usage:
    python tools/gen_channel_grid.py [--out PATH]   (default docs/slopsync/channel-grid.html)
    python tools/gen_channel_grid.py --check        # exit 1 if the committed
                                                     # HTML doesn't match a
                                                     # fresh render (stale
                                                     # catalog vs. doc)
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_channel_map as gcm  # reuses parse_ch_symbols/parse_add_entries/parse_ap_modifier_invocations + DESCRIPTIONS

CLASSES = {1: 'STATE', 2: 'STREAM', 3: 'INTENT', 4: 'EVENT', 5: 'STORE'}
DOMAINS = {0: 'machine', 1: 'motion', 2: 'pattern',
           3: 'auxiliary · reserved', 4: 'playback · reserved', 5: 'automation · reserved'}

# domain hue slots (validated: dataviz reference palette, light/dark)
DOM_VAR = {0: 'dom-machine', 1: 'dom-motion', 2: 'dom-pattern'}

# ---- named reserves: hold a slot with no catalog entry behind it yet -------
NAMED_RESERVE_ALLOC = {
    0x1011: ('battery', 'NAMED RESERVE — RFC-048 capability interface'),
    0x1012: ('thermal', 'NAMED RESERVE — RFC-048 capability interface'),
}
NAMED_RESERVE = set(NAMED_RESERVE_ALLOC)


def build_alloc() -> dict[int, tuple[str, str]]:
    """id -> (wire name, one-line note), sourced from SlopSyncCatalog.h's own
    ch:: namespace + c.addEntry()/addApModifierChannel() calls — the exact
    same parse gen_channel_map.py's device table uses — plus the static
    named-reserve overlay above. This is what makes staleness impossible: the
    grid can only ever show what the catalog actually declares."""
    src = gcm.CATALOG_H.read_text(encoding='utf-8')
    ch_symbols = gcm.parse_ch_symbols(src)
    id_to_sym = {v: k for k, v in ch_symbols.items()}
    alloc: dict[int, tuple[str, str]] = {}

    for id_val, name, cls in gcm.parse_add_entries(src, ch_symbols):
        if id_val is None or name is None or id_val not in id_to_sym:
            continue
        alloc[id_val] = (name, gcm.DESCRIPTIONS.get(id_to_sym[id_val], ''))

    for sym, wire_name in gcm.parse_ap_modifier_invocations(src):
        id_val = ch_symbols.get(sym)
        if id_val is None or id_val in alloc:
            continue
        alloc[id_val] = (wire_name, gcm.DESCRIPTIONS.get(sym, ''))

    alloc.update(NAMED_RESERVE_ALLOC)
    return alloc


ALLOC = build_alloc()

CORE = [
    (0x0000, 'SESSION'), (0x0001, 'catalog'), (0x0002, 'session-roster (reserved)'),
    (0x0003, 'safety'), (0x0004, 'control-owner'), (0x0005, 'safety-intents'),
    (0x0006, 'hub-status'), (0x0007, 'session-events'), (0x0008, 'log'),
    (0x0009, 'session-admin'), (0x000A, 'pending-pairing'), (0x000B, 'pairing-events'),
    (0x000C, 'paired-devices'), (0x000D, 'paired-devices-roster'), (0x000E, 'safety-events'),
]


def mirrors_of(cid):
    dom_fam_mem = cid & 0x0FFF
    return [f'0x{(c << 12) | dom_fam_mem:04X} {ALLOC[(c << 12) | dom_fam_mem][0]}'
            for c in CLASSES if ((c << 12) | dom_fam_mem) in ALLOC and ((c << 12) | dom_fam_mem) != cid]


def cell(cid):
    dom = (cid >> 8) & 0xF
    fam, mem = (cid >> 4) & 0xF, cid & 0xF
    cls = CLASSES[(cid >> 12) & 0xF]
    if cid in ALLOC:
        name, note = ALLOC[cid]
        kind = 'reserve' if cid in NAMED_RESERVE else 'alloc'
        admin = ' admin' if fam == 0xF else ''
        mir = mirrors_of(cid)
        tip = f'0x{cid:04X} — {name}|{cls}·{DOMAINS[dom].split(" ")[0]}·fam {fam:X} mem {mem:X}'
        if note: tip += f'|{note}'
        if mir: tip += '|mirrors: ' + ', '.join(mir)
        return f'<i class="c {kind}{admin} d{dom}" data-tip="{tip}"></i>'
    return f'<i class="c free" data-tip="0x{cid:04X} — free|{cls}·fam {fam:X} mem {mem:X}"></i>'


def family(cls_i, dom, fam):
    cells = ''.join(cell((cls_i << 12) | (dom << 8) | (fam << 4) | m) for m in range(16))
    return f'<div class="fam">{cells}</div>'


def dom_row(cls_i, dom):
    fams = ''.join(family(cls_i, dom, f) for f in range(16))
    cl = '' if dom in DOM_VAR else ' dim'
    return f'<div class="row{cl}"><span class="rl">{DOMAINS[dom]}</span><div class="fams">{fams}</div></div>'


def class_block(cls_i):
    hdr = ''.join(f'<span>{f:X}</span>' for f in range(16))
    rows = ''.join(dom_row(cls_i, d) for d in DOMAINS)
    return (f'<section><h2>0x{cls_i}··· — {CLASSES[cls_i]}</h2>'
            f'<div class="row hdr"><span class="rl">family →</span><div class="fams famhdr">{hdr}</div></div>'
            f'{rows}</section>')


def build():
    n_alloc = len(ALLOC) - len(NAMED_RESERVE)
    core_cells = ''.join(
        f'<i class="c alloc core" data-tip="0x{cid:04X} — {nm}|protocol core"></i>' for cid, nm in CORE)
    blocks = ''.join(class_block(c) for c in CLASSES)
    return f"""<title>SlopSync Channel Grid — Phase C4</title>
<style>
.viz-root{{color-scheme:light;--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--line:#e4e2dc;
--dom0:#2a78d6;--dom1:#eb6834;--dom2:#1baf7a;--core:#4a3aa7;
font:14px/1.5 system-ui,sans-serif;background:var(--surface);color:var(--ink);padding:20px 24px 60px}}
@media (prefers-color-scheme:dark){{:root:where(:not([data-theme="light"])) .viz-root{{color-scheme:dark;
--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--line:#3a3936;--dom0:#3987e5;--dom1:#d95926;--dom2:#199e70;--core:#9085e9}}}}
:root[data-theme="dark"] .viz-root{{color-scheme:dark;--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--line:#3a3936;
--dom0:#3987e5;--dom1:#d95926;--dom2:#199e70;--core:#9085e9}}
.viz-root h1{{font-size:20px;margin:0 0 2px}} .viz-root .sub{{color:var(--ink2);margin:0 0 18px}}
.viz-root h2{{font-size:14px;margin:26px 0 8px;letter-spacing:.04em}}
.legend{{display:flex;gap:18px;flex-wrap:wrap;align-items:center;margin:14px 0 4px;color:var(--ink2);font-size:13px}}
.legend i{{vertical-align:-2px;margin-right:5px}}
.c{{display:inline-block;width:11px;height:11px;border-radius:2px;margin:0}}
.free{{background:transparent;border:1px solid var(--line)}}
.alloc{{border:1px solid transparent}}
.d0.alloc{{background:var(--dom0)}}.d1.alloc{{background:var(--dom1)}}.d2.alloc{{background:var(--dom2)}}
.core.alloc{{background:var(--core)}}
.reserve{{background:transparent!important;border:1.5px dashed}}
.d0.reserve{{border-color:var(--dom0)}}.d1.reserve{{border-color:var(--dom1)}}.d2.reserve{{border-color:var(--dom2)}}
.admin{{outline:2px solid var(--ink);outline-offset:1px}}
.fam{{display:grid;grid-template-columns:repeat(4,11px);gap:2px;padding:3px;border:1px solid var(--line);border-radius:3px}}
.fams{{display:flex;gap:5px}}
.famhdr span{{width:54px;text-align:center;color:var(--ink2);font-size:11px}}
.row{{display:flex;align-items:center;gap:10px;margin:5px 0}}
.row.dim .rl{{opacity:.55;font-style:italic}}
.rl{{width:150px;text-align:right;color:var(--ink2);font-size:12.5px;flex:none}}
.corestrip{{display:flex;gap:4px;margin:6px 0 2px}}
.wrap{{overflow-x:auto}}
#tt{{position:fixed;pointer-events:none;background:var(--ink);color:var(--surface);padding:7px 10px;border-radius:5px;
font-size:12.5px;max-width:340px;opacity:0;transition:opacity .08s;z-index:9}}
#tt b{{display:block}} #tt span{{opacity:.75}}
.stats{{margin-top:22px;color:var(--ink2);font-size:13px}}
</style>
<div class="viz-root">
<h1>SlopSync channel space — the 0xCDSS grid (Phase C4, landed)</h1>
<p class="sub">0xCDSS: <b>C</b>lass band · <b>D</b>omain row · slot = [<b>family</b>][<b>member</b>], shown as one 4×4 block per family.
Same position across class bands = the mirror rule (hover any colored cell). Allocation is parsed live from
SlopSyncCatalog.h — this page cannot go stale without the catalog going with it.</p>
<div class="legend">
<span><i class="c alloc d0"></i>machine</span><span><i class="c alloc d1"></i>motion</span>
<span><i class="c alloc d2"></i>pattern</span><span><i class="c alloc core"></i>protocol core</span>
<span><i class="c reserve d0"></i>named reserve</span><span><i class="c alloc d0 admin"></i>family F = admin</span>
<span><i class="c free"></i>free</span></div>
<section><h2>0x00·· — PROTOCOL CORE (identical on every hub)</h2>
<div class="corestrip">{core_cells}</div>
<p class="sub">…0x000F–0x007F: 113 free protocol slots.</p></section>
<div class="wrap">{blocks}</div>
<p class="stats"><b>{n_alloc}</b> allocated · <b>{len(NAMED_RESERVE)}</b> named reserves · <b>{5 * 6 * 256 - len(ALLOC):,}</b> free slots in the six shown domains — plus domains 6–7 unassigned and 8–F held for the parked multi-axis convention (another {5 * 10 * 256:,} slots of runway).</p>
</div>
<div id="tt"></div>
<script>
const tt=document.getElementById('tt');
document.querySelectorAll('.c').forEach(c=>{{
 c.addEventListener('mousemove',e=>{{const p=c.dataset.tip.split('|');
  tt.innerHTML='<b>'+p[0]+'</b>'+p.slice(1).map(x=>'<span>'+x+'</span>').join('<br>');
  tt.style.opacity=1;tt.style.left=Math.min(e.clientX+14,innerWidth-360)+'px';tt.style.top=(e.clientY+14)+'px';}});
 c.addEventListener('mouseleave',()=>tt.style.opacity=0);}});
</script>"""


if __name__ == '__main__':
    out = ROOT / 'docs' / 'slopsync' / 'channel-grid.html'
    if '--out' in sys.argv:
        out = Path(sys.argv[sys.argv.index('--out') + 1])

    fresh = build()
    if '--check' in sys.argv:
        current = out.read_text(encoding='utf-8') if out.exists() else None
        if current != fresh:
            print(f'STALE: {out} does not match a fresh render of SlopSyncCatalog.h — regenerate.',
                  file=sys.stderr)
            sys.exit(1)
        print('channel grid up to date')
        sys.exit(0)

    out.write_text(fresh, encoding='utf-8')
    print(f'wrote {out}')
