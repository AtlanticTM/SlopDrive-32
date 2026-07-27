#!/usr/bin/env python3
"""Generate the documentation site's Specification tier from the normative
source document.

    <spec_path>     docs/slopsync/SPEC.md               the normative spec
    <traces_path>   docs/slopsync/examples/session-traces.md   Appendix E
    <cddl_path>     docs/slopsync/schema/catalog.cddl   Appendix C, the catalog schema

Usage:
    python docs-site/tools/gen_spec_pages.py          # (re)write generated files
    python docs-site/tools/gen_spec_pages.py --check  # exit 1 if any file is stale
    python docs-site/tools/gen_spec_pages.py --list   # print the generated file list
    python docs-site/tools/gen_spec_pages.py --nav    # print the mkdocs.yml nav block
    python docs-site/tools/gen_spec_pages.py --spec PATH   # override the source

WHY THIS EXISTS
    A hand-copied specification is the worst kind of hand-copied constant,
    because an implementer trusts a specification absolutely and does not
    compile it. This project has already shipped the copy-drift bug four
    times: a stale JS intent-schema table that encoded a float as an int, a
    hand-rolled EstopCause enum, a hardcoded WebUI layout table that truncated
    a telemetry field, and a mermaid-vendoring caveat. A second copy of the
    normative text would beat all four.

    So there is no second copy. SPEC.md is the source; these pages are a
    build product; `--check` fails the build the moment they disagree.

    This is the specification sibling of tools/gen_docs_tables.py, which does
    the same job for the registry tables. The two scripts deliberately do not
    import each other: different sources of truth, different output grammars,
    and each must stay independently editable. They share conventions only —
    same config file, same `--check` semantics, same banner.

WHAT THIS SCRIPT IS ALLOWED TO CHANGE
    Normative text is copied VERBATIM. The only transformations are:

    1. heading levels shift by page, and every heading gains an explicit,
       number-derived anchor (§6.4 -> `#s6-4`, Appendix G -> `#appendix-g`);
    2. `§n.m` cross-references become links to wherever that clause landed
       after the split (this is the whole difficulty of splitting a
       cross-referenced document, so it is checked: an unresolvable reference
       is a hard error, and every emitted link is anchor-validated by
       `mkdocs build --strict`);
    3. repo-relative links (`registry/registry.yaml`, `RFC-QUEUE.md`, ...) are
       resolved against SOURCE_LINKS: published companions become site links,
       unpublished ones degrade to their own link text. An unlisted target is
       a hard error, so a new link in SPEC.md cannot silently rot;
    4. front matter, the DO-NOT-EDIT banner, page titles, and clearly-marked
       site notes are added around the text.

    No word of the normative register is rewritten, summarised or reordered.
"""
from __future__ import annotations

import io
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

import yaml

# --------------------------------------------------------------------------
# Locations. Derived from this file's own position, so the script works from
# any working directory and survives `git subtree split`.
# --------------------------------------------------------------------------
SITE_ROOT = Path(__file__).resolve().parent.parent      # docs-site/
CONFIG = SITE_ROOT / "site.config.yml"
DOCS = SITE_ROOT / "docs"
GEN_DIR = DOCS / "spec"

GENERATOR_NAME = "docs-site/tools/gen_spec_pages.py"


def _configured_path(flag: str, env: str, key: str, label: str) -> Path:
    """Resolve one outside path: --flag > $ENV > site.config.yml key."""
    if flag in sys.argv:
        raw = sys.argv[sys.argv.index(flag) + 1]
    elif os.environ.get(env):
        raw = os.environ[env]
    else:
        cfg = yaml.safe_load(CONFIG.read_text(encoding="utf-8")) or {}
        raw = cfg.get(key)
        if not raw:
            raise SystemExit(f"{CONFIG} has no `{key}`.")
    p = Path(raw)
    if not p.is_absolute():
        p = (SITE_ROOT / p).resolve()
    if not p.exists():
        raise SystemExit(
            f"{label} not found: {p}\n"
            f"  Set `{key}` in {CONFIG} (relative to that file),\n"
            f"  or export {env}, or pass {flag} PATH."
        )
    return p


def spec_path() -> Path:
    return _configured_path("--spec", "SLOPSYNC_SPEC", "spec_path", "specification")


def traces_path() -> Path:
    return _configured_path("--traces", "SLOPSYNC_TRACES", "traces_path", "session traces")


def cddl_path() -> Path:
    return _configured_path("--cddl", "SLOPSYNC_CDDL", "cddl_path", "catalog schema")


# ==========================================================================
# THE SPLIT
#
# 1544 lines on one page is hostile to reading, to the browser's find, and to
# deep-linking. The grouping below is BY CONCERN, and every group is a thing
# an implementer works on in one sitting:
#
#   - Foundations   (1-4)  everything you must agree on before a byte moves:
#                          scope, the words, the shape, the compatibility
#                          contract. Short sections that are only read once.
#   - one page per  (5-13, 15-18) the load-bearing layers. Each is cited
#     major layer          heavily and deserves its own URL and its own TOC.
#   - Transports    (13-14) the relay is a forwarding role defined entirely in
#                          terms of the binding matrix above it; splitting
#                          them would put §14.3's timestamp rules on a page
#                          that never states the MTUs they are bounded by.
#   - Appendices    (A-G)  the lookup material: tables, schema, worked sketch,
#                          vector index. Kept together because a reader
#                          consulting one usually consults its neighbour.
#   - Rationale     (H-J)  the informative history: rejected alternatives, the
#                          gap-closure audit, the draft delta. Long, read once,
#                          and never during implementation. Its own page keeps
#                          it out of the lookup material's way.
#   - Traces               Appendix E's content, which lives in its own source
#                          file and is a narrative, not a clause.
#   - Schema               Appendix C's content: the catalog CDDL, which lives
#                          in its own source file and is NORMATIVE — it wins
#                          over §8.1's prose on any disagreement. A normative
#                          artifact that the site rendered as un-linkable
#                          inline code was the one remaining place a reader
#                          could be sent to a file this site does not carry.
#
# Every section number in SPEC.md MUST be claimed by exactly one page below.
# A new §19 therefore FAILS this generator until someone decides where it is
# published — the same discipline gen_docs_tables.py applies to registry
# sections, for the same reason.
# ==========================================================================
@dataclass(frozen=True)
class Page:
    slug: str
    nav: str                 # nav label and front-matter title
    description: str
    sections: tuple          # section keys claimed: "5", or "A"
    promote: bool = False    # single-section page: its heading becomes the H1
    title: str = ""          # generated H1 for non-promoted pages


PAGES: tuple[Page, ...] = (
    Page("index", "Specification",
         "The normative SlopSync v1.0 protocol specification: reading guide, "
         "clause map, and the generated clause pages.",
         ("0",), title="SlopSync Protocol Specification"),
    Page("foundations", "Foundations",
         "SlopSync clauses 1-4: purpose and non-goals, design philosophy, RFC 2119 "
         "conventions, the honesty clauses, terminology, roles, architecture, and the "
         "versioning and compatibility model.",
         ("1", "2", "3", "4"), title="Foundations"),
    Page("wire-format", "Wire format",
         "SlopSync clause 5: the 8-byte frame header, the deterministic CBOR profile, "
         "packed data-plane layouts, the ESTOP frame, fragmentation, and parser totality.",
         ("5",), promote=True),
    Page("session", "Session layer",
         "SlopSync clause 6: identity, HELLO and WELCOME, the readiness gate, the network "
         "probe, liveness, mid-session subscription management, reconnect and teardown.",
         ("6",), promote=True),
    Page("time", "Time and sequencing",
         "SlopSync clause 7: hub time as the timebase, the CLOCK exchange, timestamp "
         "formats and wraparound, sequence numbers, and time through relays.",
         ("7",), promote=True),
    Page("catalog", "Catalog",
         "SlopSync clause 8: the channel entry, the schema language, etag computation, "
         "blob transfer, the static-client profile, stores, and the settings metamodel.",
         ("8",), promote=True),
    Page("channels", "Channel classes",
         "SlopSync clause 9: STATE, STREAM, INTENT/ECHO, EVENT and STORE semantics, and "
         "the closed motion input surface.",
         ("9",), promote=True),
    Page("qos", "QoS and congestion",
         "SlopSync clause 10: priorities and the never-shed set, the grant model, "
         "per-binding congestion signals, the normative shedding table, and ingress limits.",
         ("10",), promote=True),
    Page("safety", "Safety",
         "SlopSync clause 11: the stop taxonomy and safety snapshot, ESTOP end to end, "
         "the deadman, control arbitration, and the invariants under partial failure.",
         ("11",), promote=True),
    Page("security", "Security and trust",
         "SlopSync clause 12: threat model, access tiers, pairing ceremonies, token "
         "presentation, hub authenticity, the trust ledger, and the administration surface.",
         ("12",), promote=True),
    Page("transports", "Transports and relays",
         "SlopSync clauses 13-14: the binding contract and its matrix, the WebSocket, "
         "ESP-NOW, BLE, serial and in-process bindings, discovery, and the relay role.",
         ("13", "14"), title="Transport bindings and the relay role"),
    Page("legacy", "Legacy interop",
         "SlopSync clause 15: legacy text-protocol edges as synthetic sessions, and the "
         "predecessor-protocol migration map.",
         ("15",), promote=True),
    Page("errors", "Errors and diagnostics",
         "SlopSync clause 16: the NACK and GOODBYE code taxonomy, and the observability "
         "channels a hub exposes about itself.",
         ("16",), promote=True),
    Page("conformance", "Conformance",
         "SlopSync clause 17: conformance profiles, golden vectors and the fixture freeze, "
         "behavioural checklists, and the fuzzing totality gate.",
         ("17",), promote=True),
    Page("limitations", "Known limitations",
         "SlopSync clause 18: every known limitation of v1.0, stated so that nobody "
         "rediscovers one as a surprise.",
         ("18",), promote=True),
    Page("traces", "Worked session traces",
         "Five annotated end-to-end SlopSync session traces, each step citing the "
         "normative rule it exercises.",
         (), title="Worked session traces"),
    Page("schema", "Catalog schema (CDDL)",
         "The normative CDDL definition of the SlopSync channel catalog: channel "
         "entries, packed layouts, CBOR schemas, the settings metamodel and store "
         "descriptors. Appendix C, in full.",
         (), title="Catalog schema (CDDL)"),
    Page("appendices", "Appendices A-G",
         "SlopSync appendices A-G: frame types, CBOR keys, the catalog schema, a worked "
         "catalog sketch, the trace and golden-vector indexes, and limits and defaults.",
         ("A", "B", "C", "D", "E", "F", "G"), title="Appendices A-G"),
    Page("rationale", "Rationale and history",
         "SlopSync appendices H-J: design rationale and rejected alternatives, the "
         "design-review gap-closure map, and what changed since the v1 draft.",
         ("H", "I", "J"), title="Appendices H-J: rationale and history"),
)

PAGE_BY_SLUG = {p.slug: p for p in PAGES}

# Pages in the Specification tier that this generator does NOT own. Listed so
# the nav helper stays honest about what the tier contains.
HAND_WRITTEN = (("Errata", "spec/errata.md"),)


# --------------------------------------------------------------------------
# Repo-relative link targets appearing in the source documents.
#
# "page"     -> a page this site publishes
# "strip"    -> not published here; keep the link TEXT, drop the dead link
#
# An unlisted target is a hard error. A new companion artifact referenced from
# SPEC.md must be given a home here, deliberately, rather than silently
# becoming a 404 or a dead-looking code span.
# --------------------------------------------------------------------------
SOURCE_LINKS: dict[str, tuple[str, str]] = {
    "registry/registry.yaml": ("page", "../reference/registry/index.md"),
    "examples/session-traces.md": ("page", "traces.md"),
    "schema/catalog.cddl": ("page", "schema.md"),
    # The vector manifest stays a repository artifact, deliberately. It is
    # normative as to COVERAGE, but it is a generation plan whose byte-exact
    # outputs do not exist yet, and half its rows name files that are not
    # written. Publishing it would put a to-do list in the normative tier.
    # Revisit when the vectors are generated and tagged.
    "vectors/manifest.yaml": ("strip", ""),
    "vectors/fixtures/mini-catalog.yaml": ("strip", ""),
    "vectors/": ("strip", ""),
    "RFC-QUEUE.md": ("strip", ""),
    "V1-READINESS.md": ("strip", ""),
}

# Appendices that reproduce a registry table. The site generates the live view
# of each from registry.yaml, so the appendix gets a pointer to it. The
# appendix TEXT is untouched: it is the tagged v1.0 view and stays readable
# standalone, exactly as a printed specification's appendix must.
REGISTRY_VIEWS: dict[str, tuple[str, str]] = {
    "A": ("Frame types", "../reference/registry/frames.md"),
    "B": ("CBOR keys", "../reference/registry/cbor-keys.md"),
    "G": ("Limits and defaults", "../reference/registry/limits.md"),
}


# ==========================================================================
# Anchors
#
# A specification is deep-linked and cited, so its anchors must be derivable
# from a clause number without reading the page. The scheme, in full:
#
#     §5      -> #s5          §6.4 -> #s6-4        Appendix G -> #appendix-g
#
# Nothing is slugified from heading TEXT. Rewording a heading therefore cannot
# break an inbound citation, which is the failure mode that matters for a
# document people link to from bug reports and other repositories.
# ==========================================================================
def anchor_for(key: str, sub: str | None = None) -> str:
    if key.isdigit():
        return f"s{key}" if sub is None else f"s{key}-{sub}"
    return f"appendix-{key.lower()}"


# --------------------------------------------------------------------------
# Source parsing
# --------------------------------------------------------------------------
SECTION_H2 = re.compile(r"^## (\d+)\.\s+(.*)$")
APPENDIX_H2 = re.compile(r"^## Appendix ([A-J])\s+(.*)$")
SUBSECTION_H3 = re.compile(r"^### (\d+)\.(\d+)\s+(.*)$")


@dataclass
class Block:
    key: str            # "5" or "A"
    heading: str        # heading text WITHOUT the leading "## "
    lines: list         # body lines, heading excluded
    subs: list          # [(subnumber, heading text)]


def parse_spec(text: str) -> tuple[list[str], dict[str, Block]]:
    """Split SPEC.md into its preamble and its top-level blocks.

    Fence state is tracked so that a ``` block containing a #-leading line can
    never be mistaken for a heading. Blockquoted headings (`> ###`, used by
    Appendix D's example-ids warning) are not heading lines and are left alone
    by construction.
    """
    preamble: list[str] = []
    blocks: dict[str, Block] = {}
    current: Block | None = None
    in_fence = False

    for raw in text.splitlines():
        if raw.startswith("```"):
            in_fence = not in_fence
        if not in_fence:
            m = SECTION_H2.match(raw) or APPENDIX_H2.match(raw)
            if m:
                key, heading = m.group(1), m.group(2).strip()
                if key in blocks:
                    raise SystemExit(f"SPEC.md declares section {key} twice.")
                current = Block(key, heading, [], [])
                blocks[key] = current
                continue
            if raw.startswith("# "):
                # Document title and the "# Appendices" divider. Pages carry
                # their own H1, so these are structure, not content.
                continue
            sm = SUBSECTION_H3.match(raw)
            if sm and current is not None:
                current.subs.append((sm.group(2), sm.group(3).strip()))
        (current.lines if current is not None else preamble).append(raw)

    return preamble, blocks


def trim(lines: list[str]) -> list[str]:
    """Drop surrounding blank lines and a trailing `---` section divider."""
    out = list(lines)
    while out and not out[-1].strip():
        out.pop()
    if out and out[-1].strip() == "---":
        out.pop()
        while out and not out[-1].strip():
            out.pop()
    while out and not out[0].strip():
        out.pop(0)
    return out


# --------------------------------------------------------------------------
# Masking. Code spans and fenced blocks are never rewritten: `§<section>` in
# §0 is a grammar illustration, not a cross-reference, and a link inside a
# code span would render as literal brackets.
# --------------------------------------------------------------------------
_FENCE = re.compile(r"^```.*?^```", re.S | re.M)
_INLINE_CODE = re.compile(r"`[^`\n]*`")
_MASK = "\x00%d\x00"
_MASK_RE = re.compile(r"\x00(\d+)\x00")


def mask(text: str) -> tuple[str, list[str]]:
    store: list[str] = []

    def keep(m: re.Match) -> str:
        store.append(m.group(0))
        return _MASK % (len(store) - 1)

    text = _FENCE.sub(keep, text)
    text = _INLINE_CODE.sub(keep, text)
    return text, store


def unmask(text: str, store: list[str]) -> str:
    return _MASK_RE.sub(lambda m: store[int(m.group(1))], text)


# --------------------------------------------------------------------------
# Cross-reference rewriting
#
# Forms that occur in the sources, all of which must survive the split:
#
#   §5  §6.4                      plain clause and subclause
#   §5.8-4  §18-19  §1.2-1        numbered item inside a clause
#   §12.3a  §12.3c                lettered paragraph inside a clause
#   §10.4-style                   a clause used adjectivally: only §10.4 links
#   §12.3–§12.5  §6.2–6.3         ranges: each §-prefixed token links alone
#
# The matched TEXT is always preserved verbatim; only a link wrapper is added.
# --------------------------------------------------------------------------
SECTION_REF = re.compile(r"§(\d{1,2})(?:\.(\d{1,2}))?(-\d{1,2}|[a-c])?")
APPENDIX_REF = re.compile(r"\bAppendix ([A-J])\b")
APPENDICES_REF = re.compile(r"\bAppendices ([A-J]), ([A-J]) and ([A-J])\b")
MD_LINK = re.compile(r"\[([^\]]*)\]\(([^)]+)\)")


class Linker:
    """Resolves clause references to `page.md#anchor`, and reports what it
    could not resolve. A dangling `§n.m` is a defect in the SOURCE document,
    so it is surfaced by name rather than silently emitted as plain text."""

    def __init__(self, homes: dict[str, str], subs: dict[str, set]):
        self.homes = homes              # section key -> page slug
        self.subs = subs                # section key -> {subnumbers}
        self.unresolved: set[str] = set()

    def target(self, slug: str, key: str, sub: str | None, here: str) -> str:
        a = anchor_for(key, sub)
        return f"#{a}" if slug == here else f"{slug}.md#{a}"

    def rewrite(self, text: str, here: str, source: str) -> str:
        text, store = mask(text)
        out_lines = []
        for line in text.split("\n"):
            if not line.startswith("#"):        # headings keep plain text
                line = MD_LINK.sub(lambda m: self._link(m, source), line)
                line = SECTION_REF.sub(lambda m: self._section(m, here), line)
                line = APPENDICES_REF.sub(lambda m: self._appendices(m, here), line)
                line = APPENDIX_REF.sub(lambda m: self._appendix(m, here), line)
            out_lines.append(line)
        return unmask("\n".join(out_lines), store)

    # -- markdown links to repo-relative companions -------------------------
    def _link(self, m: re.Match, source: str) -> str:
        label, href = m.group(1), m.group(2)
        if href.startswith(("http://", "https://", "#", "../")):
            return m.group(0)                       # absolute, or site-authored
        if href.partition("#")[0].removesuffix(".md") in PAGE_BY_SLUG:
            return m.group(0)                       # site-authored page link
        if href not in SOURCE_LINKS:
            raise SystemExit(
                f"{source}: link target '{href}' has no home.\n"
                f"  Add it to SOURCE_LINKS in {GENERATOR_NAME}: either a page this\n"
                "  site publishes, or 'strip' if it is a repository artifact the\n"
                "  site does not carry. An unlisted target would ship as a 404."
            )
        kind, dest = SOURCE_LINKS[href]
        return label if kind == "strip" else f"[{label}]({dest})"

    # -- clause references --------------------------------------------------
    def _section(self, m: re.Match, here: str) -> str:
        whole, major, minor = m.group(0), m.group(1), m.group(2)
        if major not in self.homes:
            self.unresolved.add(whole)
            return whole
        if minor is not None and minor not in self.subs[major]:
            self.unresolved.add(f"§{major}.{minor}")
            return whole
        slug = self.homes[major]
        return f"[{whole}]({self.target(slug, major, minor, here)})"

    def _appendix(self, m: re.Match, here: str) -> str:
        letter = m.group(1)
        if letter not in self.homes:
            self.unresolved.add(f"Appendix {letter}")
            return m.group(0)
        return f"[{m.group(0)}]({self.target(self.homes[letter], letter, None, here)})"

    def _appendices(self, m: re.Match, here: str) -> str:
        letters = [m.group(1), m.group(2), m.group(3)]
        links = []
        for letter in letters:
            if letter not in self.homes:
                self.unresolved.add(f"Appendix {letter}")
                return m.group(0)
            links.append(f"[{letter}]({self.target(self.homes[letter], letter, None, here)})")
        return f"Appendices {links[0]}, {links[1]} and {links[2]}"


# --------------------------------------------------------------------------
# Heading rendering
# --------------------------------------------------------------------------
def render_headings(lines: list[str], shift: int) -> list[str]:
    """Shift `###` subsection headings by `shift` levels and give each one its
    number-derived anchor. Fences are respected; blockquoted headings are not
    headings and never match."""
    out: list[str] = []
    in_fence = False
    for line in lines:
        if line.startswith("```"):
            in_fence = not in_fence
            out.append(line)
            continue
        m = SUBSECTION_H3.match(line) if not in_fence else None
        if m:
            major, minor, rest = m.group(1), m.group(2), m.group(3)
            hashes = "#" * (3 + shift)
            out.append(f"{hashes} {major}.{minor} {rest} {{#{anchor_for(major, minor)}}}")
        else:
            out.append(line)
    return out


def section_heading(block: Block, shift: int) -> str:
    hashes = "#" * (2 + shift)
    if block.key.isdigit():
        text = f"{block.key}. {block.heading}"
    else:
        text = f"Appendix {block.key} {block.heading}"
    return f"{hashes} {text} {{#{anchor_for(block.key)}}}"


# --------------------------------------------------------------------------
# Markdown chrome
# --------------------------------------------------------------------------
def front_matter(p, *, title: str, description: str) -> None:
    p("---\n")
    p(f"title: {title}\n")
    p("description: >-\n")
    for chunk in wrap(description, 76):
        p(f"  {chunk}\n")
    p("register: IEEE\n")
    p("generated: true\n")
    p("---\n\n")


def hard_breaks(text: str) -> str:
    """Force a line break between consecutive non-blank lines.

    SPEC.md's metadata header is one bold field per line. Python-Markdown
    collapses that into a run-on paragraph, GitHub does not. Two trailing
    spaces is the canonical Markdown hard break: every word survives verbatim
    and the author's line structure survives with it.
    """
    lines = text.split("\n")
    out = []
    for i, line in enumerate(lines):
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        if line.strip() and nxt.strip() and not line.endswith("  "):
            line += "  "
        out.append(line)
    return "\n".join(out)


def wrap(text: str, width: int) -> list[str]:
    words, line, out = text.split(), "", []
    for w in words:
        if line and len(line) + 1 + len(w) > width:
            out.append(line)
            line = w
        else:
            line = f"{line} {w}".strip()
    if line:
        out.append(line)
    return out


def banner(p, src_display: str) -> None:
    p("<!-- ==========================================================\n")
    p("     GENERATED FILE — DO NOT EDIT.\n")
    p(f"     Source of truth: {src_display}\n")
    p(f"     Generator:       {GENERATOR_NAME}\n")
    p("     Regenerate:      python docs-site/tools/gen_spec_pages.py\n")
    p("     CI gate:         python docs-site/tools/gen_spec_pages.py --check\n")
    p("     Normative text is copied verbatim. Hand edits are overwritten\n")
    p("     and fail the docs build. Edit the specification instead.\n")
    p("     ========================================================== -->\n\n")


# --------------------------------------------------------------------------
# Page builders
# --------------------------------------------------------------------------
def build_index(page: Page, blocks: dict[str, Block], homes: dict[str, str],
                preamble: str, body: str, src_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title=page.nav, description=page.description)
    banner(p, src_display)
    p(f"# {page.title}\n\n")

    p('!!! danger "This tier is generated. Edit the specification, not these pages."\n\n')
    p(f"    Every page under **Specification** is produced from `{src_display}`\n")
    p(f"    by `{GENERATOR_NAME}`. That file is the normative source and the only\n")
    p("    place to make a change. A hand edit here is overwritten by the next\n")
    p("    build, and `--check` fails the build before that happens.\n\n")
    p("    There is deliberately no second copy of the normative text on this\n")
    p("    site. An implementer trusts a specification absolutely, so a\n")
    p("    specification that can drift from its source is a defect, not a\n")
    p("    convenience.\n\n")

    p(hard_breaks(preamble) + "\n\n")
    p(body + "\n\n")

    p("## The clause pages {#clause-pages}\n\n")
    p("The document is split by concern. Clause numbering is unchanged: §6.4 is\n")
    p("§6.4 wherever it is published.\n\n")
    p("| Page | Clauses |\n|---|---|\n")
    for pg in PAGES:
        if pg.slug in ("index", "traces", "schema") or not pg.sections:
            continue
        cells = []
        for key in pg.sections:
            b = blocks[key]
            label = f"§{key}" if key.isdigit() else f"Appendix {key}"
            title = re.sub(r"\s*\*\(.*?\)\*\s*$", "", b.heading).strip()
            cells.append(f"[{label} {title}]({pg.slug}.md#{anchor_for(key)})")
        p(f"| [{pg.nav}]({pg.slug}.md) | " + " · ".join(cells) + " |\n")
    p(f"| [Worked session traces](traces.md) | Appendix "
      f"[E](appendices.md#appendix-e), in full |\n")
    p(f"| [Catalog schema (CDDL)](schema.md) | Appendix "
      f"[C](appendices.md#appendix-c), in full |\n")
    p("\n")

    p("## Citing a clause {#citing}\n\n")
    p("Anchors are derived from clause numbers, never from heading text. A\n")
    p("reworded heading therefore cannot break a citation made from a bug\n")
    p("report or another repository.\n\n")
    p("| Clause | Anchor | Example |\n|---|---|---|\n")
    p("| Section §n | `#sn` | [§11](safety.md#s11) |\n")
    p("| Subsection §n.m | `#sn-m` | [§6.4](session.md#s6-4) |\n")
    p("| Appendix X | `#appendix-x` | [Appendix G](appendices.md#appendix-g) |\n")
    p("| Trace En | `#en` | [E4](traces.md#e4) |\n")
    p("\n")
    p("Numbered items and lettered paragraphs inside a clause — §5.8-4, §12.3a —\n")
    p("resolve to their parent clause, which is where they are read from.\n\n")

    p("## Companion artifacts {#companions}\n\n")
    p("The specification's companions live beside it in the source repository.\n")
    p("Three of them are published on this site, generated the same way these\n")
    p("pages are:\n\n")
    p("| Artifact | Where |\n|---|---|\n")
    p("| `registry/registry.yaml` — every wire number | "
      "[Registry reference](../reference/registry/index.md), generated |\n")
    p("| `examples/session-traces.md` — Appendix E | "
      "[Worked session traces](traces.md), generated |\n")
    p("| `schema/catalog.cddl` — Appendix C, the normative catalog encoding | "
      "[Catalog schema (CDDL)](schema.md), generated |\n")
    p("| `vectors/manifest.yaml` — Appendix F, golden-vector coverage | "
      "source repository |\n")
    p("| `RFC-QUEUE.md`, `V1-READINESS.md` — change history and rationale | "
      "source repository |\n")
    p("\n")

    p("## The register {#register}\n\n")
    p("This tier is written in the IEEE and RFC normative register. Numbered\n")
    p("clauses. RFC 2119 keywords. Exact cross-references. It is permitted to be\n")
    p("dense: implementers need exactness more than approachability.\n\n")
    p("Glossary tooltips are stripped from every page in this tier. In a\n")
    p("normative document every word is normative or visibly marked otherwise,\n")
    p("so a hover definition over a term inside a MUST clause would be a second,\n")
    p("invisible source of meaning. [The Dictionary](../reference/dictionary.md)\n")
    p("stays one click away.\n\n")
    p("Everything outside this tier is written in the STE register. See\n")
    p("[Contributing](../community/contributing.md).\n")
    return w.getvalue()


def build_page(page: Page, rendered: dict[str, str], src_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title=page.nav, description=page.description)
    banner(p, src_display)
    if not page.promote:
        p(f"# {page.title}\n\n")
    for key in page.sections:
        p(rendered[key])
        p("\n\n")
    return w.getvalue().rstrip("\n") + "\n"


def build_cddl(page: Page, text: str, src_display: str) -> str:
    """Publish the catalog CDDL verbatim, inside one fenced block.

    The file is NOT reformatted, re-indented, split or annotated inline. It is
    normative and it wins over §8.1's prose, so the published bytes must be the
    file's bytes; the only additions are the page chrome around the fence.
    Cross-reference linking is deliberately skipped: everything inside a fence
    is masked from the linker anyway, and a normative schema must read exactly
    as it reads in the repository.
    """
    w = io.StringIO()
    p = w.write
    front_matter(p, title=page.nav, description=page.description)
    banner(p, src_display)
    p(f"# {page.title}\n\n")

    p('!!! danger "Normative. This schema wins over the prose."\n\n')
    p("    This is [Appendix C](appendices.md#appendix-c) in full: the normative\n")
    p("    encoding of the channel catalog. [§8.1](catalog.md#s8-1) is its prose\n")
    p("    companion, and **the CDDL wins on any disagreement** with it.\n\n")
    p(f"    The page is generated from `{src_display}`. It cannot drift from that\n")
    p("    file, because `--check` fails the build the moment it does.\n\n")

    p("## Reading it {#reading}\n\n")
    p("The schema is written in CDDL (RFC 8610), the concise data definition\n")
    p("language for CBOR. Three conventions carry most of the meaning:\n\n")
    p("| Notation | Meaning |\n|---|---|\n")
    p("| `1 => uint` | map key 1, holding an unsigned integer. Keys are the wire "
      "numbers from the [registry](../reference/registry/cbor-keys.md). |\n")
    p("| `? 8 => ...` | the key is optional. Absent is not the same as null: "
      "the deterministic profile never encodes null ([§5.3](wire-format.md#s5-3)). |\n")
    p("| `* entry` / `+ field` | zero-or-more, and one-or-more. |\n")
    p("\n")
    p("Comments carry normative constraints that the grammar cannot express —\n")
    p("the depth budget, which keys are mutually exclusive, and why. Read them.\n\n")

    p("## The schema {#cddl}\n\n")
    p("```cddl\n")
    p(text.rstrip("\n") + "\n")
    p("```\n\n")

    p("## Related {#related}\n\n")
    p("- [§8 Catalog](catalog.md#s8) — the prose companion, and the etag rules.\n")
    p("- [Channel catalog reference](../reference/channel-catalog.md) — the\n")
    p("  SlopDrive-32 catalog that this schema describes, entry by entry.\n")
    p("- [Catalog vocabulary](../reference/registry/catalog-vocabulary.md) — the\n")
    p("  generated view of every enumeration named above.\n")
    return w.getvalue().rstrip("\n") + "\n"


def build_traces(page: Page, text: str, linker: Linker, src_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title=page.nav, description=page.description)
    banner(p, src_display)

    lines = text.splitlines()
    out: list[str] = []
    in_fence = False
    for line in lines:
        if line.startswith("```"):
            in_fence = not in_fence
        if not in_fence:
            if line.startswith("# "):
                out.append(line)
                continue
            m = re.match(r"^## (E\d)\s+(.*)$", line)
            if m:
                out.append(f"## {m.group(1)} {m.group(2)} {{#{m.group(1).lower()}}}")
                continue
        out.append(line)
    body = linker.rewrite("\n".join(out).strip(), page.slug, src_display)

    # The H1 arrives from the source file, so the site note goes after it.
    head, _, rest = body.partition("\n")
    p(head + "\n\n")
    p('!!! info "Appendix E, in full"\n\n')
    p("    These traces are the narrative form of the [§17.3](conformance.md#s17-3)\n")
    p("    behavioural checklist. Every step cites the normative rule it exercises,\n")
    p("    and a step with no rule to cite is a specification bug.\n\n")
    p(rest.lstrip("\n"))
    return w.getvalue().rstrip("\n") + "\n"


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------
def build_all() -> dict[Path, str]:
    sp = spec_path()
    tp = traces_path()
    cp = cddl_path()
    text = sp.read_text(encoding="utf-8")
    preamble_lines, blocks = parse_spec(text)

    # Every section claimed exactly once; every claim exists.
    claimed: dict[str, str] = {}
    for page in PAGES:
        for key in page.sections:
            if key in claimed:
                raise SystemExit(f"section {key} is claimed by two pages.")
            claimed[key] = page.slug
    unclaimed = sorted(set(blocks) - set(claimed), key=sort_key)
    if unclaimed:
        raise SystemExit(
            "SPEC.md has section(s) no page claims: " + ", ".join(unclaimed)
            + f"\n  Add each to PAGES in {GENERATOR_NAME} and publish it.\n"
            "  An unpublished clause is exactly what this gate exists to stop."
        )
    missing = sorted(set(claimed) - set(blocks), key=sort_key)
    if missing:
        raise SystemExit("pages expect section(s) SPEC.md no longer has: "
                         + ", ".join(missing))

    subs = {k: {n for n, _ in b.subs} for k, b in blocks.items()}
    linker = Linker(claimed, subs)

    try:
        src_display = sp.relative_to(SITE_ROOT.parent).as_posix()
        traces_display = tp.relative_to(SITE_ROOT.parent).as_posix()
        cddl_display = cp.relative_to(SITE_ROOT.parent).as_posix()
    except ValueError:
        src_display, traces_display = sp.as_posix(), tp.as_posix()
        cddl_display = cp.as_posix()

    # Render every section body once, on the page that owns it.
    rendered: dict[str, str] = {}
    for key, block in blocks.items():
        page = PAGE_BY_SLUG[claimed[key]]
        shift = -1 if page.promote else 0
        chunk = [section_heading(block, shift), ""]
        if key in REGISTRY_VIEWS:
            label, href = REGISTRY_VIEWS[key]
            chunk += [
                '!!! info "Live registry view"',
                "",
                f"    This appendix is the view of the registry frozen at the v1.0 tag.",
                f"    The always-current generated table is [{label}]({href}).",
                "    On any conflict the registry wins (§5.7).",
                "",
            ]
        chunk += render_headings(trim(block.lines), shift)
        rendered[key] = linker.rewrite("\n".join(chunk), page.slug, src_display)

    preamble = linker.rewrite("\n".join(trim(preamble_lines)), "index", src_display)

    out: dict[Path, str] = {}
    for page in PAGES:
        if page.slug == "index":
            out[GEN_DIR / "index.md"] = build_index(
                page, blocks, claimed, preamble, rendered["0"], src_display)
        elif page.slug == "traces":
            out[GEN_DIR / "traces.md"] = build_traces(
                page, tp.read_text(encoding="utf-8"), linker, traces_display)
        elif page.slug == "schema":
            out[GEN_DIR / "schema.md"] = build_cddl(
                page, cp.read_text(encoding="utf-8"), cddl_display)
        else:
            out[GEN_DIR / f"{page.slug}.md"] = build_page(page, rendered, src_display)

    if linker.unresolved:
        raise SystemExit(
            "unresolvable cross-reference(s) in the source document:\n  "
            + "\n  ".join(sorted(linker.unresolved))
            + "\n\nEach names a clause that does not exist. That is a defect in the\n"
            "specification, not in this generator: fix the reference at the source,\n"
            "or add the missing clause. Splitting a document silently past a dead\n"
            "cross-reference is how a specification stops being trustworthy."
        )
    return out


def sort_key(k: str) -> tuple:
    return (0, int(k), "") if k.isdigit() else (1, 0, k)


def nav_block() -> str:
    out = ["  - Specification:"]
    for page in PAGES:
        if page.slug == "index":
            out.append("      - spec/index.md")
        else:
            out.append(f"      - {page.nav}: spec/{page.slug}.md")
    for label, path in HAND_WRITTEN:
        out.append(f"      - {label}: {path}")
    return "\n".join(out) + "\n"


def main() -> int:
    if "--nav" in sys.argv:
        sys.stdout.write(nav_block())
        return 0
    if "--list" in sys.argv:
        for path in build_all():
            print(path.relative_to(SITE_ROOT.parent).as_posix())
        return 0

    files = build_all()
    stale = [path for path, text in files.items()
             if (path.read_text(encoding="utf-8") if path.exists() else None) != text]

    if "--check" in sys.argv:
        if stale:
            print("STALE generated specification pages:", file=sys.stderr)
            for path in stale:
                print(f"  {path}", file=sys.stderr)
            print(
                "\nRegenerate with:\n"
                "  python docs-site/tools/gen_spec_pages.py\n"
                "then commit the result. The normative text is never typed by hand "
                "twice.",
                file=sys.stderr,
            )
            return 1
        print(f"spec pages up to date ({len(files)} files)")
        return 0

    for path, text in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {len(files)} files ({len(stale)} changed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
