#!/usr/bin/env python3
"""Generate docs/reference/channel-grid.md, the docs-site's interactive
channel-space grid page (Phase G stamped centerpiece, LEDGER.md "Phase G").

SOURCE OF TRUTH: this script imports tools/gen_channel_grid.py (the
repository's own standalone operator visual) and reuses its ALLOC dict
verbatim. That dict is itself parsed live from SlopSyncCatalog.h through
gen_channel_map.py's own parsers. Nothing here hand-copies a channel id, a
name, or a class: the page can only ever show what the catalog actually
declares, exactly as the standalone visual promises.

This script owns a DIFFERENT RENDERING of that same data: reading-tuned,
docs typography, styled from the site's own `--ss-*` custom properties
(docs/assets/stylesheets/datasheet.css), which already bridge
webui/src/style.css tokens into both color schemes. The standalone visual
hardcodes a light/dark palette because it runs standalone, outside any
site stylesheet; this page does not need to, because it always renders
inside the site's own CSS.

WHY THIS SCRIPT REACHES OUTSIDE docs-site/ (an intentional, documented
exception): docs-site/ is designed to be a self-contained, extractable
subtree (see README.md, "Extracting this directory"). This is the one
generator that cannot honor that fully, because the operator instruction
for this page is explicit: parse the live catalog the SAME WAY
gen_channel_map.py does, by IMPORTING its parsers rather than hand-copying
a table. So this script imports two repo-root modules
(tools/gen_channel_map.py, tools/gen_channel_grid.py) that live outside
docs-site/. If this directory is ever extracted, this one page's generator
either travels with copies of those two files, or is retired; every other
generator in docs-site/ is unaffected.

Usage:
    python docs-site/tools/gen_channel_grid_page.py           # (re)write
    python docs-site/tools/gen_channel_grid_page.py --check   # exit 1 if stale
"""
from __future__ import annotations

import io
import sys
from pathlib import Path

SITE_ROOT = Path(__file__).resolve().parent.parent          # docs-site/
REPO_ROOT = SITE_ROOT.parent
OUT = SITE_ROOT / "docs" / "reference" / "channel-grid.md"

GENERATOR_NAME = "docs-site/tools/gen_channel_grid_page.py"

sys.path.insert(0, str(REPO_ROOT / "tools"))
import gen_channel_grid as gcg  # noqa: E402  (path insert must come first)


def front_matter(p) -> None:
    p("---\n")
    p("title: Channel grid\n")
    p("description: >-\n")
    p("  Every channel slot in the 0xCDSS device space, live from the "
      "catalog. Allocated, named-reserved, and free, in one picture.\n")
    p("register: STE\n")
    p("generated: true\n")
    p("---\n\n")


def banner(p) -> None:
    p("<!-- ==========================================================\n")
    p("     GENERATED FILE. DO NOT EDIT.\n")
    p("     Source of truth: include/comms/SlopSyncCatalog.h, parsed by\n")
    p("     tools/gen_channel_map.py, assembled by tools/gen_channel_grid.py.\n")
    p(f"     Generator:       {GENERATOR_NAME}\n")
    p(f"     Regenerate:      python {GENERATOR_NAME}\n")
    p(f"     CI gate:         python {GENERATOR_NAME} --check\n")
    p("     Hand edits are overwritten and fail the docs build.\n")
    p("     ========================================================== -->\n\n")


def intro(p) -> None:
    p("# Channel grid\n\n")
    p("A SlopSync channel id is not a bare number. It is a small address, "
      "0xCDSS. C picks the class. D picks the domain. SS packs a family "
      "and a member nibble.\n\n")
    p("The grid below shows every slot in that address space for one "
      "device. A filled square is a real channel from the live catalog. "
      "A dashed square is a named reserve, held for a feature the catalog "
      "does not carry yet. An empty square is free runway.\n\n")
    p("Hover a square for its name. Click or press Enter on a square to "
      "pin its full detail below the grid.\n\n")
    p("## Reading one address\n\n")
    p("Take id `0x1010`. The first hex digit is the class. Class `1` "
      "means STATE, a full snapshot. The second digit is the domain. "
      "Domain `0` means machine. The last two digits split into a family "
      "nibble and a member nibble: family `1`, member `0`. Member `0` is "
      "always the family's master row.\n\n")
    p("The **mirror rule** follows from this shape. Two channels can "
      "share the same domain, family and member digits in different "
      "class bands. They are then the same feature seen from two "
      "angles: one reports it, the other commands it. The grid links a "
      "mirrored pair on hover, so the relationship is never a guess.\n\n")
    p("## Protocol core\n\n")
    p("Sixteen classes-worth of headroom is not device space at all. "
      "Ids `0x0000` to `0x007F` are the protocol core: session, safety, "
      "pairing, log. Every conformant hub carries the same core ids, "
      "listed in full on [Spec-core channels]"
      "(registry/channels.md#spec-core-channels). The strip below shows "
      "only the ones a device catalog actually declares. The rest of "
      "the 128 core ids are reserved runway, same as everywhere else on "
      "this page.\n\n")
    p("## Domains, and why two of them borrow site colors\n\n")
    p("Motion and pattern each get the accent color this site already "
      "gives a meaning. Motion is the live, measured feed, so it wears "
      "the same blue as [measured truth](../understand/how-it-works.md) "
      "everywhere else on this site. Pattern is a generated, commanded "
      "sequence, so it wears the same purple as a wish. Machine keeps a "
      "neutral tone: it is the chassis, not a live signal.\n\n")


STYLE = """
<style>
.ss-grid-root { font-family: var(--ss-sans); }
.ss-grid-root .legend {
  display: flex; flex-wrap: wrap; gap: 1rem 1.4rem; align-items: center;
  margin: 0 0 1.1rem; padding: 0.7rem 0.9rem;
  border: var(--ss-hairline) solid var(--ss-rule);
  border-radius: var(--ss-radius); background-color: var(--ss-surface);
  font-size: 0.74rem; color: var(--ss-ink-muted);
}
.ss-grid-root .legend span.sw {
  display: inline-flex; align-items: center; gap: 0.4em;
}
.ss-grid-root i.sw-chip {
  display: inline-block; width: 13px; height: 13px; border-radius: var(--ss-radius);
}
.ss-grid-root .sw-chip.d0 { background: var(--ss-ink-faint); border: 1px solid var(--ss-ink-faint); }
.ss-grid-root .sw-chip.d1 { background: var(--ss-reality); border: 1px solid var(--ss-reality); }
.ss-grid-root .sw-chip.d2 { background: var(--ss-intent); border: 1px solid var(--ss-intent); }
.ss-grid-root .sw-chip.core { background: var(--ss-accent); border: 1px solid var(--ss-accent); }
.ss-grid-root .sw-chip.reserve { background: transparent; border: 1.5px dashed var(--ss-ink-faint); }
.ss-grid-root .sw-chip.free { background: transparent; border: 1px solid var(--ss-rule-strong); }
.ss-grid-root .sw-chip.admin { outline: 2px solid var(--ss-ink); outline-offset: 1px; }

.ss-grid-root section.ss-classblock {
  margin: 1.6rem 0; padding: 0.9rem 1rem 1rem;
  border: var(--ss-hairline) solid var(--ss-rule); border-radius: var(--ss-radius);
  background-color: var(--ss-screen); box-shadow: var(--ss-screen-inset);
  overflow-x: auto;
}
.ss-grid-root h3.ss-classhdr {
  font-family: var(--ss-mono); font-size: 0.82rem; margin: 0 0 0.7rem;
  color: var(--ss-ink); letter-spacing: 0.02em;
}
.ss-grid-root .corestrip {
  display: flex; gap: 5px; flex-wrap: wrap; margin: 0.3rem 0 0.5rem;
  min-width: 20rem;
}
.ss-grid-root .row {
  display: flex; align-items: center; gap: 0.7rem; margin: 0.35rem 0;
  min-width: 34rem;
}
.ss-grid-root .row.hdr { color: var(--ss-ink-faint); font-size: 0.6rem; font-family: var(--ss-mono); }
.ss-grid-root .row .rl {
  width: 9.5rem; flex: none; text-align: right; font-size: 0.68rem;
  color: var(--ss-ink-muted);
}
.ss-grid-root .row.dim .rl { opacity: 0.55; font-style: italic; }
.ss-grid-root .fams { display: flex; gap: 6px; }
.ss-grid-root .famhdr span { width: 62px; text-align: center; display: inline-block; }
.ss-grid-root .fam {
  display: grid; grid-template-columns: repeat(4, 13px); gap: 3px;
  padding: 4px; border: var(--ss-hairline) solid var(--ss-rule); border-radius: 3px;
}
.ss-grid-root button.c {
  width: 13px; height: 13px; padding: 0; margin: 0; display: block;
  border-radius: var(--ss-radius); cursor: pointer;
  border: 1px solid var(--ss-rule-strong); background: transparent;
}
.ss-grid-root button.c:focus-visible { outline: 2px solid var(--ss-accent); outline-offset: 1px; }
.ss-grid-root button.c.free { background: transparent; }
.ss-grid-root button.c.alloc { border-color: transparent; }
.ss-grid-root button.c.d0.alloc { background: var(--ss-ink-faint); }
.ss-grid-root button.c.d1.alloc { background: var(--ss-reality); }
.ss-grid-root button.c.d2.alloc { background: var(--ss-intent); }
.ss-grid-root button.c.core.alloc { background: var(--ss-accent); }
.ss-grid-root button.c.reserve { background: transparent !important; border: 1.5px dashed; }
.ss-grid-root button.c.d0.reserve { border-color: var(--ss-ink-faint); }
.ss-grid-root button.c.d1.reserve { border-color: var(--ss-reality); }
.ss-grid-root button.c.d2.reserve { border-color: var(--ss-intent); }
.ss-grid-root button.c.admin { outline: 2px solid var(--ss-ink); outline-offset: 1px; }
.ss-grid-root #ss-tip {
  position: fixed; pointer-events: none; z-index: 9; opacity: 0;
  transition: opacity 80ms ease; max-width: 21rem;
  background: var(--ss-ink); color: var(--ss-paper);
  padding: 0.5rem 0.65rem; border-radius: var(--ss-radius);
  font-size: 0.72rem; line-height: 1.4;
}
.ss-grid-root #ss-tip b { display: block; font-family: var(--ss-mono); }
.ss-grid-root #ss-tip span { display: block; opacity: 0.8; }
.ss-grid-root #ss-detail {
  margin-top: 1.4rem; padding: 0.9rem 1rem; min-height: 3.4rem;
  border: var(--ss-hairline) solid var(--ss-rule);
  border-left: 3px solid var(--ss-accent); border-radius: var(--ss-radius);
  background-color: var(--ss-surface); font-size: 0.78rem;
}
.ss-grid-root #ss-detail .ph { color: var(--ss-ink-muted); font-style: italic; }
.ss-grid-root #ss-detail b.name { font-family: var(--ss-mono); color: var(--ss-ink); font-size: 0.9rem; }
.ss-grid-root #ss-detail .row2 { margin-top: 0.3rem; color: var(--ss-ink-muted); }
.ss-grid-root .stats { margin-top: 1rem; font-size: 0.74rem; color: var(--ss-ink-muted); }
</style>
""".strip("\n")


def build_html() -> str:
    ALLOC = gcg.ALLOC
    CLASSES = gcg.CLASSES
    DOMAINS = gcg.DOMAINS
    NAMED_RESERVE = gcg.NAMED_RESERVE
    CORE = gcg.CORE

    dom_class = {0: "d0", 1: "d1", 2: "d2"}

    def mirrors_of(cid: int):
        dom_fam_mem = cid & 0x0FFF
        out = []
        for c in CLASSES:
            other = (c << 12) | dom_fam_mem
            if other in ALLOC and other != cid:
                out.append(f"0x{other:04X} {ALLOC[other][0]}")
        return out

    def cell(cid: int) -> str:
        dom = (cid >> 8) & 0xF
        fam, mem = (cid >> 4) & 0xF, cid & 0xF
        cls = CLASSES[(cid >> 12) & 0xF]
        aria = f"0x{cid:04X}"
        if cid in ALLOC:
            name, note = ALLOC[cid]
            kind = "reserve" if cid in NAMED_RESERVE else "alloc"
            dcls = dom_class.get(dom, "")
            admin = " admin" if fam == 0xF else ""
            mir = mirrors_of(cid)
            tip = f"0x{cid:04X}: {name}|{cls}·{DOMAINS[dom].split(' ')[0]}·fam {fam:X} mem {mem:X}"
            if note:
                tip += f"|{note}"
            if mir:
                tip += "|mirrors: " + ", ".join(mir)
            aria = f"{aria}, {name}"
            return (f'<button type="button" class="c {kind}{admin} {dcls}" '
                    f'data-tip="{tip}" aria-label="{aria}"></button>')
        tip = f"0x{cid:04X}: free|{cls}·fam {fam:X} mem {mem:X}"
        return f'<button type="button" class="c free" data-tip="{tip}" aria-label="{aria}, free"></button>'

    def family(cls_i: int, dom: int, fam: int) -> str:
        cells = "".join(cell((cls_i << 12) | (dom << 8) | (fam << 4) | m) for m in range(16))
        return f'<div class="fam">{cells}</div>'

    def dom_row(cls_i: int, dom: int) -> str:
        fams = "".join(family(cls_i, dom, f) for f in range(16))
        cl = "" if dom in dom_class else " dim"
        return (f'<div class="row{cl}"><span class="rl">{DOMAINS[dom]}</span>'
                f'<div class="fams">{fams}</div></div>')

    def class_block(cls_i: int) -> str:
        hdr = "".join(f"<span>{f:X}</span>" for f in range(16))
        rows = "".join(dom_row(cls_i, d) for d in DOMAINS)
        return (f'<section class="ss-classblock"><h3 class="ss-classhdr">'
                f'0x{cls_i}··· {CLASSES[cls_i]}</h3>'
                f'<div class="row hdr"><span class="rl">family →</span>'
                f'<div class="fams famhdr">{hdr}</div></div>{rows}</section>')

    n_alloc = len(ALLOC) - len(NAMED_RESERVE)
    core_cells = "".join(
        f'<button type="button" class="c alloc core" '
        f'data-tip="0x{cid:04X}: {nm}|protocol core" '
        f'aria-label="0x{cid:04X}, {nm}"></button>'
        for cid, nm in CORE
    )
    blocks = "".join(class_block(c) for c in CLASSES)
    free_slots = 5 * 6 * 256 - len(ALLOC)

    w = io.StringIO()
    p = w.write
    p('<div class="ss-grid-root">\n')
    p(STYLE + "\n")
    p('<div class="legend">\n')
    p('<span class="sw"><i class="sw-chip d0"></i>machine</span>\n')
    p('<span class="sw"><i class="sw-chip d1"></i>motion</span>\n')
    p('<span class="sw"><i class="sw-chip d2"></i>pattern</span>\n')
    p('<span class="sw"><i class="sw-chip core"></i>protocol core</span>\n')
    p('<span class="sw"><i class="sw-chip reserve"></i>named reserve</span>\n')
    p('<span class="sw"><i class="sw-chip admin"></i>family F is admin</span>\n')
    p('<span class="sw"><i class="sw-chip free"></i>free</span>\n')
    p("</div>\n\n")
    p('<div class="ss-classblock">\n')
    p('<h3 class="ss-classhdr">0x00·· PROTOCOL CORE (same on every hub)</h3>\n')
    p(f'<div class="corestrip">{core_cells}</div>\n')
    p("</div>\n\n")
    p(blocks + "\n")
    p(f'<p class="stats"><b>{n_alloc}</b> allocated channels, '
      f'<b>{len(NAMED_RESERVE)}</b> named reserves, '
      f'<b>{free_slots:,}</b> free slots across the six domains shown. '
      f'Domains 6 and 7 are unassigned. Domains 8 through F hold the parked '
      f'multi-axis convention: another {5 * 10 * 256:,} slots of runway.</p>\n')
    p('<div id="ss-detail"><span class="ph">Click or press Enter on a square '
      "to pin its detail here.</span></div>\n")
    p('<div id="ss-tip"></div>\n')
    p("</div>\n\n")
    p("<script>\n")
    p("(function(){\n")
    p('  var tip = document.getElementById("ss-tip");\n')
    p('  var detail = document.getElementById("ss-detail");\n')
    p('  document.querySelectorAll(".ss-grid-root .c").forEach(function(c){\n')
    p('    c.addEventListener("mousemove", function(e){\n')
    p('      var parts = c.dataset.tip.split("|");\n')
    p('      tip.innerHTML = "<b>" + parts[0] + "</b>" + parts.slice(1).map(function(x){return "<span>"+x+"</span>";}).join("");\n')
    p("      tip.style.opacity = 1;\n")
    p("      tip.style.left = Math.min(e.clientX + 14, window.innerWidth - 360) + \"px\";\n")
    p('      tip.style.top = (e.clientY + 14) + "px";\n')
    p("    });\n")
    p('    c.addEventListener("mouseleave", function(){ tip.style.opacity = 0; });\n')
    p('    c.addEventListener("click", function(){\n')
    p('      var parts = c.dataset.tip.split("|");\n')
    p('      detail.innerHTML = "<b class=\\"name\\">" + parts[0] + "</b>" + parts.slice(1).map(function(x){return "<div class=\\"row2\\">"+x+"</div>";}).join("");\n')
    p("    });\n")
    p("  });\n")
    p("})();\n")
    p("</script>\n")
    return w.getvalue()


DEMO_NOTE = (
    "> DEMO-CANDIDATE: fetch a live hub's catalog etag over the network and "
    "flag any square whose entry does not match what shipped.\n\n"
)


def build() -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p)
    banner(p)
    intro(p)
    p(DEMO_NOTE)
    p(build_html())
    return w.getvalue()


def main(argv: list[str]) -> int:
    fresh = build()
    if "--check" in argv:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else None
        if current != fresh:
            print(f"STALE: {OUT} does not match a fresh render — regenerate.", file=sys.stderr)
            return 1
        print("channel grid page up to date")
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(fresh, encoding="utf-8", newline="\n")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
