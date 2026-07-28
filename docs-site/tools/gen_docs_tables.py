#!/usr/bin/env python3
"""Generate the SlopSync documentation site's registry reference pages and the
Dictionary, from the two sources of truth:

    <registry_path>        every wire number (frame types, CBOR keys, NACK
                           codes, channel ids, limits, roles, ...)
    docs-site/dictionary.yaml   every defined term, one definition each

Usage:
    python docs-site/tools/gen_docs_tables.py            # (re)write generated files
    python docs-site/tools/gen_docs_tables.py --check    # exit 1 if any file is stale
    python docs-site/tools/gen_docs_tables.py --list     # print the generated file list
    python docs-site/tools/gen_docs_tables.py --registry PATH   # override the registry

WHY THIS EXISTS
    A wrong NACK code in documentation is worse than a wrong NACK code in
    source, because implementers trust documentation and do not compile it.
    This project has already shipped the hand-copied-constant drift bug three
    times (a stale JS intent-schema table, a hand-rolled EstopCause enum, a
    hardcoded WebUI layout table). No wire number on this site is typed by a
    human. `--check` runs in CI and fails the build when a generated file
    drifts from the registry.

    This is the documentation sibling of tools/gen_registry_header.py, which
    does the same job for C++ constants. It deliberately does not import that
    script: the two have different output grammars and must be independently
    editable.

CONFIGURATION
    Exactly one path reaches outside docs-site/: the registry. It is resolved
    from docs-site/site.config.yml, overridable by $SLOPSYNC_REGISTRY or
    --registry. See that file's comments.
"""
from __future__ import annotations

import io
import os
import re
import sys
from pathlib import Path

import yaml

# --------------------------------------------------------------------------
# Locations. Everything is derived from this file's own position, so the
# script works from any working directory and survives `git subtree split`.
# --------------------------------------------------------------------------
SITE_ROOT = Path(__file__).resolve().parent.parent      # docs-site/
CONFIG = SITE_ROOT / "site.config.yml"
DOCS = SITE_ROOT / "docs"
GEN_DIR = DOCS / "reference" / "registry"
DICT_SRC = SITE_ROOT / "dictionary.yaml"
DICT_OUT = DOCS / "reference" / "dictionary.md"
ABBR_OUT = SITE_ROOT / "includes" / "abbreviations.md"

GENERATOR_NAME = "docs-site/tools/gen_docs_tables.py"


def registry_path() -> Path:
    """Resolve the registry location: --registry > $SLOPSYNC_REGISTRY > config."""
    if "--registry" in sys.argv:
        raw = sys.argv[sys.argv.index("--registry") + 1]
    elif os.environ.get("SLOPSYNC_REGISTRY"):
        raw = os.environ["SLOPSYNC_REGISTRY"]
    else:
        cfg = yaml.safe_load(CONFIG.read_text(encoding="utf-8")) or {}
        raw = cfg.get("registry_path")
        if not raw:
            raise SystemExit(f"{CONFIG} has no `registry_path`.")
    p = Path(raw)
    if not p.is_absolute():
        p = (SITE_ROOT / p).resolve()
    if not p.exists():
        raise SystemExit(
            f"registry not found: {p}\n"
            f"  Set `registry_path` in {CONFIG} (relative to that file),\n"
            f"  or export SLOPSYNC_REGISTRY, or pass --registry PATH."
        )
    return p


# --------------------------------------------------------------------------
# Every top-level registry section MUST be claimed by a page below. A new
# section added to registry.yaml therefore FAILS the docs build until someone
# decides where it is documented. Silence is the failure mode this whole
# script exists to prevent, so it is not an option here either.
# --------------------------------------------------------------------------
SECTION_HOMES: dict[str, str] = {
    "meta": "index.md",
    "frame_types": "frames.md",
    "header_flags": "frames.md",
    "channel_classes": "channels.md",
    "stream_kinds": "channels.md",
    "access_levels": "channels.md",
    "priority_classes": "channels.md",
    "channel_id_ranges": "channels.md",
    "core_channels": "channels.md",
    "cbor_keys": "cbor-keys.md",
    "welcome_limits_keys": "cbor-keys.md",
    "probe_result_keys": "cbor-keys.md",
    "identity_keys": "cbor-keys.md",
    "blob_keys": "cbor-keys.md",
    "trust_keys": "cbor-keys.md",
    "trust_ledger_keys": "cbor-keys.md",
    "blob_namespaces": "cbor-keys.md",
    "packed_field_types": "catalog-vocabulary.md",
    "field_roles": "catalog-vocabulary.md",
    # setting_categories is RETIRED (Phase C2 tombstone in registry.yaml);
    # ui_categories is its wire-key-10 successor, homed on rendering.md.
    "setting_flags": "catalog-vocabulary.md",
    "procedure_phases": "catalog-vocabulary.md",
    "curve_families": "catalog-vocabulary.md",
    "session_event_kinds": "events.md",
    "log_event_kinds": "events.md",
    "pairing_event_kinds": "events.md",
    "safety_event_kinds": "events.md",
    "log_levels": "events.md",
    "safety_intent_ops": "safety.md",
    "safety_causes": "safety.md",
    "session_admin_ops": "pairing.md",
    "pairing_modes": "pairing.md",
    "trust_states": "pairing.md",
    "presentation_modes": "pairing.md",
    "nack_codes": "errors.md",
    "limits": "limits.md",
    "ble_identity": "discovery.md",
    "ble_adv_flags": "discovery.md",
    "udp_discovery": "discovery.md",
    "ui_categories": "rendering.md",
    "ui_ranks": "rendering.md",
    "value_aspects": "rendering.md",
    "value_scopes": "rendering.md",
    "value_provenance": "rendering.md",
    "unit_ids": "rendering.md",
    "action_tags": "rendering.md",
    "ui_archetypes": "rendering.md",
    "ui_regions": "rendering.md",
    "renderer_classes": "rendering.md",
    "widget_patterns": "rendering.md",
}


# --------------------------------------------------------------------------
# Markdown helpers
# --------------------------------------------------------------------------
def cell(text: object) -> str:
    """Render a registry value as one Markdown table cell.

    Table cells cannot contain a raw pipe or a newline. Registry notes are
    single-line YAML scalars today; the collapse below keeps that from
    becoming a silent formatting break if someone folds one tomorrow.
    """
    s = "" if text is None else str(text)
    s = re.sub(r"\s+", " ", s).strip()
    return s.replace("|", "\\|")


def code(text: object) -> str:
    """A cell rendered as inline code. Empty stays empty, never an empty span."""
    s = cell(text)
    return f"`{s}`" if s else ""


def table(p, headers: list[str], rows: list[list[str]]) -> None:
    """Emit a GitHub-flavored Markdown table. Rows are already cell()-safe."""
    p("| " + " | ".join(headers) + " |\n")
    p("|" + "|".join("---" for _ in headers) + "|\n")
    for r in rows:
        p("| " + " | ".join(r) + " |\n")
    p("\n")


def hexcode(value: int, width: int) -> str:
    return f"0x{value:0{width}X}"


def front_matter(p, *, title: str, description: str, register: str) -> None:
    p("---\n")
    p(f"title: {title}\n")
    p(f"description: {description}\n")
    p(f"register: {register}\n")
    p("generated: true\n")
    p("---\n\n")


def banner(p, reg_display: str) -> None:
    p("<!-- ==========================================================\n")
    p("     GENERATED FILE. DO NOT EDIT.\n")
    p(f"     Source of truth: {reg_display}\n")
    p(f"     Generator:       {GENERATOR_NAME}\n")
    p("     Regenerate:      python docs-site/tools/gen_docs_tables.py\n")
    p("     CI gate:         python docs-site/tools/gen_docs_tables.py --check\n")
    p("     Hand edits are overwritten and fail the docs build.\n")
    p("     ========================================================== -->\n\n")


def bit_rows(section: dict) -> list[list[str]]:
    rows = []
    for bit in sorted(section, key=lambda b: int(b.removeprefix("bit"))):
        e = section[bit]
        shift = int(bit.removeprefix("bit"))
        rows.append([
            code(f"0x{1 << shift:02X}"),
            code(f"bit {shift}"),
            code(e["name"]),
            cell(e.get("note") or e.get("ref", "")),
        ])
    return rows


# --------------------------------------------------------------------------
# The `limits` block carries its rationale in YAML COMMENTS, which safe_load
# discards. The values below always come from safe_load; this scrape only adds
# a Notes column and group headings. If the scrape finds nothing the table is
# still correct, just terser — it can never contradict the parsed value.
# --------------------------------------------------------------------------
def scrape_limit_notes(raw: str) -> tuple[dict[str, str], dict[str, str]]:
    notes: dict[str, str] = {}
    groups: dict[str, str] = {}
    in_limits = False
    current_group = ""
    for line in raw.splitlines():
        if re.match(r"^limits:\s*$", line):
            in_limits = True
            continue
        if in_limits and re.match(r"^\S", line):
            break
        if not in_limits:
            continue
        if not line.strip():
            # A blank line ends a group. Without this, the two trailing
            # ungrouped constants inherit the last `# ---- logging ----`
            # heading and the page claims they are logging settings.
            current_group = ""
            continue
        g = re.match(r"^\s*#\s*-{2,}\s*(.+?)\s*$", line)
        if g:
            title = g.group(1).strip().rstrip("-").strip()
            # A heading that wraps onto a second comment line arrives cut in
            # half, with an opening parenthesis left dangling. Trim to the
            # part that is definitely complete.
            if title.count("(") != title.count(")"):
                title = title.split("(")[0].strip()
            current_group = title
            continue
        m = re.match(r"^  ([A-Za-z_][A-Za-z0-9_]*):\s*(.*?)\s*(?:#\s*(.*))?$", line)
        if m:
            key, _val, note = m.group(1), m.group(2), m.group(3)
            groups[key] = current_group
            if note:
                notes[key] = note.strip()
    return notes, groups


# --------------------------------------------------------------------------
# Page builders. Each returns the complete file text.
# --------------------------------------------------------------------------
def page_index(reg: dict, reg_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(
        p,
        title="Registry reference",
        description=(
            "Generated index of the SlopSync protocol registry: frame types, "
            "CBOR keys, channels, error codes, limits."
        ),
        register="IEEE",
    )
    banner(p, reg_display)
    m = reg["meta"]
    p("# Registry reference\n\n")
    p("The registry is the single source of truth for every number SlopSync\n")
    p("puts on the wire. These pages are generated from it. No number on this\n")
    p("site is typed by a human.\n\n")
    p("If a page here disagrees with prose elsewhere, this page wins.\n\n")

    p("## Protocol identity\n\n")
    table(p, ["Property", "Value"], [
        [cell("Protocol name"), code(m["protocol_name"])],
        [cell("Protocol version"), code(m["protocol_version"])],
        [cell("Byte order"), code(m["byte_order"])],
        [cell("Header size"), code(f"{m['header_bytes']} bytes")],
    ])

    p("## Pages\n\n")
    table(p, ["Page", "Covers"],
          [[f"[{title}]({fname})", cell(desc)] for fname, title, desc in PAGE_INDEX])

    p("## How to change a number\n\n")
    p("1. Edit `registry.yaml`. It is the only place a number is decided.\n")
    p("2. Run the C++ generator. It rewrites `registry_constants.hpp`.\n")
    p("3. Run the docs generator. It rewrites these pages.\n")
    p("4. Commit the registry and both generated outputs together.\n\n")
    p("Released numbers are never reused and never renumbered.\n")
    return w.getvalue()


# (filename, nav title, one-line description) — used by the index table and
# by --list. Kept beside the builders so adding a page cannot forget the index.
PAGE_INDEX: list[tuple[str, str, str]] = [
    ("frames.md", "Frame types", "The `type` byte and the header flag bits."),
    ("channels.md", "Channels", "Channel classes, stream kinds, access levels, priorities, id ranges, spec-core channels."),
    ("cbor-keys.md", "CBOR keys", "The global control-plane key space and every scoped sub-map key space."),
    ("catalog-vocabulary.md", "Catalog vocabulary", "Packed field types, field roles, setting categories and flags, procedure phases."),
    ("events.md", "Event kinds", "Event kind values for the spec-core EVENT channels, and log severity levels."),
    ("safety.md", "Safety codes", "Safety intent operations and safety cause codes."),
    ("pairing.md", "Pairing modes", "The pairing mode bitmask advertised in WELCOME."),
    ("errors.md", "NACK codes", "Every NACK and GOODBYE reason code, by range."),
    ("limits.md", "Limits and defaults", "Well-known sizes, timeouts, caps and defaults."),
    ("discovery.md", "Discovery", "BLE GATT identity and advertising flags, and the UDP discovery probe/reply."),
    ("rendering.md", "Rendering vocabulary", "Categories, ranks, value axes, units, action tags, archetypes, regions, renderer classes and widget patterns: the numbers behind RENDERING.md (RFC-048)."),
]


def page_frames(reg: dict, reg_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title="Frame types",
                 description="Generated table of every SlopSync frame type byte and header flag.",
                 register="IEEE")
    banner(p, reg_display)
    p("# Frame types\n\n")
    p("The `type` byte is the first discriminator in the 8-byte header.\n\n")
    p("`dir` reads `c2h` for client to hub, `h2c` for hub to client, `any` for\n")
    p("either direction. `plane` reads `control` for a CBOR payload, `data` for\n")
    p("a packed payload, `raw` for a fixed layout defined in its own clause.\n\n")
    p("An endpoint that receives an unknown frame type ignores the frame. The\n")
    p("header always carries the length, so skipping is safe.\n\n")

    rows = []
    for c in sorted(reg["frame_types"]):
        e = reg["frame_types"][c]
        rows.append([code(hexcode(c, 2)), code(e["name"]), code(e["dir"]),
                     code(e["plane"]), cell(e["ref"])])
    table(p, ["Code", "Name", "Direction", "Plane", "Clause"], rows)

    p("## Burned and reserved ranges\n\n")
    p("Frame types `0x09` and `0x0A` are **burned**. They carried the retired\n")
    p("`CATALOG_REQ` and `CATALOG_CHUNK` verbs. They are never reallocated, so a\n")
    p("stale peer meets an unknown type and fails loudly instead of misreading a\n")
    p("blob transfer.\n\n")
    p("Types `0x80` to `0xDF` are experimental. They never appear in a tagged\n")
    p("release. Types `0xE0` to `0xFF` are reserved, except `0xE5` (ESTOP).\n\n")

    p("## Header flags\n\n")
    p("Bits not listed are zero on send and ignored on receive.\n\n")
    table(p, ["Mask", "Bit", "Name", "Clause"], bit_rows(reg["header_flags"]))
    p("`FRAG_START` plus `FRAG_MORE` marks the first fragment. `FRAG_MORE` alone\n")
    p("marks a middle fragment. `FRAG_START` alone marks an unfragmented frame.\n")
    p("Neither flag, after prior fragments, marks the last fragment.\n\n")
    p("> DEMO-CANDIDATE: capture one real frame's 8-byte header live and "
      "annotate each byte against this table.\n")
    return w.getvalue()


def page_channels(reg: dict, reg_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title="Channels",
                 description="Generated tables of channel classes, stream kinds, access levels, priority classes, channel id ranges and the spec-core channels.",
                 register="IEEE")
    banner(p, reg_display)
    p("# Channels\n\n")
    p("A channel is a named, numbered, typed data flow declared in the catalog.\n")
    p("Its class decides the frames it uses and the delivery rules it obeys.\n\n")

    p("## Channel classes\n\n")
    rows = [[code(c), code(reg["channel_classes"][c]["name"]),
             cell(reg["channel_classes"][c].get("note") or reg["channel_classes"][c].get("ref", ""))]
            for c in sorted(reg["channel_classes"])]
    table(p, ["Value", "Class", "Notes"], rows)
    p('The word "STREAM" is a channel class. Transports are described as\n')
    p("*stream-oriented* or *datagram-oriented*, never as stream transports.\n\n")

    p("## Stream kinds\n\n")
    p("A STREAM channel declares what one sample **is**. The congestion rules\n")
    p("read this property. They never guess it from a unit string.\n\n")
    rows = [[code(c), code(reg["stream_kinds"][c]["name"]), cell(reg["stream_kinds"][c].get("note", ""))]
            for c in sorted(reg["stream_kinds"])]
    table(p, ["Value", "Kind", "Meaning"], rows)

    p("## Access levels\n\n")
    p("One session holds one access level. A channel declares the level a\n")
    p("session needs to subscribe to it, or to send on it.\n\n")
    rows = [[code(c), code(reg["access_levels"][c]["name"])] for c in sorted(reg["access_levels"])]
    table(p, ["Value", "Level"], rows)
    p("The safety operations `estop` and `stop` are exempt from these levels.\n")
    p("Any session may stop the machine.\n\n")

    p("## Priority classes\n\n")
    p("A subscription carries a priority class. The lower number sheds first.\n\n")
    rows = [[code(c), code(reg["priority_classes"][c]["name"]),
             cell(reg["priority_classes"][c].get("note", ""))]
            for c in sorted(reg["priority_classes"])]
    table(p, ["Value", "Class", "Notes"], rows)

    p("## Channel id ranges\n\n")
    rows = []
    for k, e in reg["channel_id_ranges"].items():
        label = k if isinstance(k, str) else hexcode(k, 4)
        rows.append([code(label), code(e["name"]), cell(e.get("note", ""))])
    table(p, ["Range", "Name", "Notes"], rows)

    p("## Spec-core channels\n\n")
    p("These channel ids mean the same thing on every hub. A hub still declares\n")
    p("each one it implements in its catalog. A channel absent from the catalog\n")
    p("does not exist on that hub.\n\n")
    rows = []
    for cid in sorted(reg["core_channels"]):
        e = reg["core_channels"][cid]
        rows.append([code(hexcode(cid, 4)), code(e["name"]), code(e["class"]), cell(e["note"])])
    table(p, ["Id", "Name", "Class", "Notes"], rows)
    return w.getvalue()


def page_cbor_keys(reg: dict, reg_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title="CBOR keys",
                 description="Generated table of the SlopSync control-plane CBOR integer key space and every scoped sub-map key space.",
                 register="IEEE")
    banner(p, reg_display)
    p("# CBOR keys\n\n")
    p("Control-plane payloads are CBOR maps with integer keys. The key space is\n")
    p("**global**: a key means the same thing in every message that uses it.\n\n")
    p("Keys 1 to 63 are core. Keys 64 to 127 are reserved. Keys 128 and above are\n")
    p("experimental and never appear in a tagged release.\n\n")
    p("A receiver ignores an unknown key. It never NACKs one.\n\n")

    p("## Global key space\n\n")
    rows = [[code(k), code(reg["cbor_keys"][k]["name"]), code(reg["cbor_keys"][k]["type"]),
             cell(reg["cbor_keys"][k]["note"])] for k in sorted(reg["cbor_keys"])]
    table(p, ["Key", "Name", "Type", "Notes"], rows)

    p("## Scoped sub-map key spaces\n\n")
    p("A feature that needs several keys takes **one** global key and numbers\n")
    p("its interior in its own tiny space. Those spaces are listed below. A\n")
    p("number here has no meaning in the global space above.\n\n")

    subs = [
        ("welcome_limits_keys", "`limits` (key 22)",
         "Sizing caps the hub advertises in WELCOME."),
        ("probe_result_keys", "`probe_result` (key 26)",
         "The client's measurement of the optional post-WELCOME network probe."),
        ("identity_keys", "`identity` (key 37)",
         "Who this hub is. There is exactly one home for hub identity."),
        ("blob_keys", "`blob` (key 38)",
         "Which blob, and its item fields. Shared by BLOB_REQ, BLOB_CHUNK and store intents."),
        ("trust_keys", "`trust` (key 39)",
         "Identity proof, signature material, token presentation, pairing modes. Every key is optional."),
    ]
    for section, heading, intro in subs:
        p(f"### {heading}\n\n")
        p(intro + "\n\n")
        rows = [[code(k), code(reg[section][k]["name"]), cell(reg[section][k].get("note", ""))]
                for k in sorted(reg[section])]
        table(p, ["Sub-key", "Name", "Notes"], rows)

    p("## Blob namespaces\n\n")
    p("One transfer verb serves the whole protocol. The namespace names what a\n")
    p("transfer carries. Values 0 to 127 are spec-governed. Values 128 to 255\n")
    p("are device-defined.\n\n")
    rows = [[code(k), code(reg["blob_namespaces"][k]["name"]), cell(reg["blob_namespaces"][k]["note"])]
            for k in sorted(reg["blob_namespaces"])]
    table(p, ["Value", "Namespace", "Notes"], rows)
    p("\n> DEMO-CANDIDATE: decode one captured HELLO or WELCOME frame live, "
      "key by key, against this table.\n")
    return w.getvalue()


def page_catalog_vocabulary(reg: dict, reg_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title="Catalog vocabulary",
                 description="Generated tables of packed field types, field roles, setting categories, setting flags and procedure phases.",
                 register="IEEE")
    banner(p, reg_display)
    p("# Catalog vocabulary\n\n")
    p("The catalog describes what a hub's channels **are**. These are the\n")
    p("registered words it uses to do that.\n\n")

    p("## Packed field types\n\n")
    p("A packed layout has static field offsets. Every type below is\n")
    p("fixed-width, which is what makes append-only evolution safe.\n\n")
    rows = [[code(k), code(reg["packed_field_types"][k]["name"]),
             cell(reg["packed_field_types"][k].get("note", ""))]
            for k in sorted(reg["packed_field_types"])]
    table(p, ["Value", "Type", "Notes"], rows)
    p("STREAM sample layouts stay string-free. The motion path never pays for\n")
    p("text.\n\n")

    p("## Field roles\n\n")
    p("A role is the semantic tag on a catalog field. It is a text string, not\n")
    p("a number, because action roles carry a device-chosen suffix.\n\n")
    p("Roles are **opportunities, never requirements**. A client that\n")
    p("recognizes a role may render a bespoke widget. A client that does not\n")
    p("must render it generically instead, by type and constraints. An unknown\n")
    p("role is never an error.\n\n")
    rows = [[code(role), cell((reg["field_roles"][role] or {}).get("note", ""))]
            for role in reg["field_roles"]]
    table(p, ["Role", "Meaning"], rows)
    p("Two conventions extend the list without registering entries:\n\n")
    p("- `<role>.peak` is the peak companion of any telemetry role.\n")
    p("- `action.<name>` marks an INTENT field as a verb, not a value.\n\n")

    # `setting_categories` was retired in favor of `ui_categories` (RFC-047/048,
    # Phase C2 tombstone in registry.yaml) — see "Categories" on rendering.md.

    p("## Setting flags\n\n")
    table(p, ["Mask", "Bit", "Name", "Notes"], bit_rows(reg["setting_flags"]))

    p("## Procedure phases\n\n")
    p("Only the lifecycle phases are registered. Any generic client can render\n")
    p("these without knowing the procedure. Values 128 to 255 are device-defined\n")
    p("intermediate steps. A client that does not recognize one renders it as\n")
    p("`running`.\n\n")
    rows = [[code(k), code(reg["procedure_phases"][k]["name"]),
             cell(reg["procedure_phases"][k].get("note", ""))]
            for k in sorted(reg["procedure_phases"])]
    table(p, ["Value", "Phase", "Notes"], rows)

    p("## Curve families\n\n")
    p("The `curve_family` sub-key is CBOR key 45, inside a `publishes` or "
      "`granted_publishes` entry. It names which smoothness class a segment "
      "stream's sender means. The wish rides on HELLO or PUBLISH. The grant "
      "echoes the effective family, so a client can tell honored from "
      "downgraded.\n\n")
    rows = [[code(k), code(reg["curve_families"][k]["name"]),
             cell(reg["curve_families"][k].get("note", ""))]
            for k in sorted(reg["curve_families"])]
    table(p, ["Value", "Family", "Notes"], rows)
    return w.getvalue()


def page_events(reg: dict, reg_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title="Event kinds",
                 description="Generated tables of event kind values for the spec-core EVENT channels, plus log severity levels.",
                 register="IEEE")
    banner(p, reg_display)
    p("# Event kinds\n\n")
    p("An EVENT reports an edge: something happened. A device-authored EVENT\n")
    p("channel enumerates its own kinds in its catalog entry. A spec-core\n")
    p("channel's kinds are agreed here, because hub and client cannot negotiate\n")
    p("them.\n\n")
    p("Events are best-effort and are not replayed. Anything a client cannot\n")
    p("afford to miss also has a latched STATE twin.\n\n")

    for section, heading, intro in [
        ("session_event_kinds", "session-events (`0x0007`)",
         "Session lifecycle and control-ownership transfer."),
        ("log_event_kinds", "log (`0x0008`)",
         "One kind. The per-line content rides the `body` sub-map, schema'd by the channel's own catalog entry."),
        ("pairing_event_kinds", "pairing-events (`0x000B`)",
         "The EVENT twin of the [pending-pairing STATE channel](channels.md#spec-core-channels). None of these is a safety latch."),
        ("safety_event_kinds", "safety-events (`0x000E`)",
         "The EVENT twin of the [safety STATE channel](channels.md#spec-core-channels). It fires only on a transition. A repeated e-stop re-broadcasts the latch. This is how loss recovery works. It does not re-announce an edge that did not happen."),
    ]:
        p(f"## {heading}\n\n{intro}\n\n")
        rows = [[code(k), code(reg[section][k]["name"]), cell(reg[section][k].get("note", ""))]
                for k in sorted(reg[section])]
        table(p, ["Kind", "Name", "Meaning"], rows)

    p("## Log severity levels\n\n")
    p("The [log channel](channels.md#spec-core-channels)'s `body.level` field.\n")
    p("These values mirror the firmware logging library number for number, so\n")
    p("the bridge is a cast and never a translation table.\n\n")
    rows = [[code(k), code(reg["log_levels"][k]["name"]), cell(reg["log_levels"][k].get("note", ""))]
            for k in sorted(reg["log_levels"])]
    table(p, ["Value", "Level", "Notes"], rows)
    p("There is no wire value for `off`. `off` is a floor sentinel, so no record\n")
    p("can arrive at that level.\n")
    return w.getvalue()


def page_safety(reg: dict, reg_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title="Safety codes",
                 description="Generated tables of SlopSync safety intent operations and safety cause codes.",
                 register="IEEE")
    banner(p, reg_display)
    p("# Safety codes\n\n")

    p("## Safety intent operations\n\n")
    p("These are the `value` map key 1 of the [`safety-intents` channel]"
      "(channels.md#spec-core-channels) (`0x0005`).\n\n")
    p("**`stop` and `estop` are role-exempt. Any session may send them,\n")
    p("including a `watch` session.** Safety outranks authorization. The wrong\n")
    p("choice here means the person who is in the room cannot stop the\n")
    p("machine. Every other operation requires `control`.\n\n")
    rows = [[code(k), code(reg["safety_intent_ops"][k]["name"]),
             cell(reg["safety_intent_ops"][k].get("note", ""))]
            for k in sorted(reg["safety_intent_ops"])]
    table(p, ["Op", "Name", "Meaning"], rows)

    p("## Safety causes\n\n")
    p("One taxonomy has two wire homes. They are the ESTOP frame's `cause`\n")
    p("byte, and the `cause` field of the latched [`safety` STATE snapshot]"
      "(channels.md#spec-core-channels) (`0x0003`).\n\n")
    rows = [[code(k), code(reg["safety_causes"][k]["name"]), cell(reg["safety_causes"][k].get("note", ""))]
            for k in sorted(reg["safety_causes"])]
    table(p, ["Value", "Cause", "Meaning"], rows)
    p("`deadman` means the silence window actually elapsed. Every other way a\n")
    p("session ends latches `session_loss`. A closed browser tab is not the\n")
    p("same event as a deadman timeout. An earlier bug reported them as the\n")
    p("same thing. These are two different events.\n")
    return w.getvalue()


def page_pairing(reg: dict, reg_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title="Pairing modes",
                 description="Generated table of the SlopSync pairing mode bitmask advertised in WELCOME.",
                 register="IEEE")
    banner(p, reg_display)
    p("# Pairing modes\n\n")
    p("A hub advertises the modes it currently offers as a bitmask in the\n")
    p("WELCOME `trust` sub-map. The trust ledger records the single bit a paired\n")
    p("device used.\n\n")
    p("All three modes end in the same `PAIR_GRANT {token, role}`. **The role is\n")
    p("an attribute of the grant, never of the ceremony.** A hub has zero or one\n")
    p("PIN, never one secret per tier.\n\n")
    table(p, ["Mask", "Bit", "Name", "Notes"], bit_rows(reg["pairing_modes"]))

    p("\n## Administration operations\n\n")
    p("These are the `value` map key 1 of the [`session-admin` channel]"
      "(channels.md#spec-core-channels) (`0x0009`). The channel requires "
      "`configure`.\n\n")
    p("**The trusted surface is a tier, not an app.** Any `configure` session\n")
    p("reaches every operation here. Nothing in the protocol knows or cares\n")
    p("whether that session is the machine's own web page, a phone, or a\n")
    p("command line.\n\n")
    p("A `configure` session may grant up to its own tier, `configure`\n")
    p("included. The paired-device roster is the audit trail, rather than a\n")
    p("ceiling that would leave the first administrator unable to appoint a\n")
    p("second.\n\n")
    rows = [[code(k), code(reg["session_admin_ops"][k]["name"]),
             cell(reg["session_admin_ops"][k].get("note", ""))]
            for k in sorted(reg["session_admin_ops"])]
    table(p, ["Op", "Name", "Meaning"], rows)

    p("## Trust ledger states\n\n")
    p("This is the `state` field of a paired-devices item. A revoked device\n")
    p("has no entry at all, so revocation is an absence and never a third\n")
    p("state.\n\n")
    rows = [[code(k), code(reg["trust_states"][k]["name"]), cell(reg["trust_states"][k].get("note", ""))]
            for k in sorted(reg["trust_states"])]
    table(p, ["Value", "State", "Meaning"], rows)
    p("\n## Token presentation modes\n\n")
    p("This is the `trust` sub-map's `presentation_mode`. The ledger records\n")
    p("it per device, so an operator can see the security posture.\n\n")
    p("`bearer` is the floor and the default. `proof` is recommended for any\n")
    p("client that already has SHA-256, and is never required of anyone.\n\n")
    rows = [[code(k), code(reg["presentation_modes"][k]["name"]),
             cell(reg["presentation_modes"][k].get("note", ""))]
            for k in sorted(reg["presentation_modes"])]
    table(p, ["Value", "Mode", "Meaning"], rows)

    p("A reported client version is a tripwire, not an attestation. It catches\n")
    p("an honest update. A deliberately malicious one reports whatever version\n")
    p("it likes and keeps its token. What bounds a hostile client is role\n")
    p("scoping, immediate revocation, its visibility in the roster, and the\n")
    p("fact that safety operations are role-exempt for everyone.\n")
    return w.getvalue()


def page_discovery(reg: dict, reg_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title="Discovery",
                 description="Generated tables of the SlopSync BLE GATT identity, its advertising flags, and the UDP discovery probe/reply (RFC-046).",
                 register="IEEE")
    banner(p, reg_display)
    p("# Discovery\n\n")
    p("A client finds a hub two ways before it has a session. One is a pinned\n")
    p("BLE GATT identity. The other is a UDP broadcast probe for WS-side\n")
    p("clients without BLE. Both are read-only identity surfaces. Neither\n")
    p("carries a control plane.\n\n")

    p("## BLE GATT identity\n\n")
    p("Every conformant BLE hub advertises the **same** service UUID, so a client\n")
    p("scans for exactly one thing. The first three groups spell the project name\n")
    p("in ASCII, deliberately, so the UUID is greppable rather than an opaque v4.\n\n")
    ble = reg["ble_identity"]
    rows = [
        [cell("Service"), code(ble["service_uuid"])],
        [cell("Write characteristic (c2h)"), code(ble["write_char_uuid"])],
        [cell("Notify characteristic (h2c)"), code(ble["notify_char_uuid"])],
    ]
    table(p, ["Role", "UUID"], rows)

    p("## BLE advertising flags\n\n")
    p("A legacy (≤31 B) advertising payload can spare one byte for flags,\n")
    p("after the service UUID and a shortened hub name. Bits not listed are\n")
    p("zero.\n\n")
    table(p, ["Mask", "Bit", "Name", "Notes"], bit_rows(reg["ble_adv_flags"]))

    p("## UDP discovery\n\n")
    p("This is the canonical WS-side discovery path for a LAN client without\n")
    p("BLE. It uses plain UDP sockets on both ends. It is immune to the\n")
    p("multicast, mesh-AP and Android failure modes that make mDNS unreliable\n")
    p("in real homes.\n\n")
    ud = reg["udp_discovery"]
    rows = [
        [cell("Port"), code(ud["port"])],
        [cell("Magic"), code(ud["magic"])],
        [cell("Reply rate limit"), cell(f"{ud['reply_rate_limit_per_source_s']} / source / second")],
    ]
    table(p, ["Property", "Value"], rows)
    p("The probe and reply frames themselves, `DISCOVER_PROBE` (`0x1E`) and\n")
    p("`DISCOVER_REPLY` (`0x1F`), are frame types. See [Frame types](frames.md).\n")
    p("A reply carries `magic + nonce + hub_name + hub_id + proto_ver + ws_port +\n")
    p("fw_version + catalog_etag + flags`. A passive observer of a normal\n")
    p("WELCOME could already learn all of it.\n\n")
    p("> DEMO-CANDIDATE: send a live UDP probe to a real hub and decode its "
      "reply on the page.\n")
    return w.getvalue()


def page_rendering(reg: dict, reg_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title="Rendering vocabulary",
                 description="Generated tables of the RFC-048 rendering vocabulary: categories, ranks, value axes, units, action tags, archetypes, regions, renderer classes and widget patterns.",
                 register="IEEE")
    banner(p, reg_display)
    p("# Rendering vocabulary\n\n")
    p("These are the numbers behind [RENDERING.md](../../spec/rendering.md), the "
      "normative UI-rendering companion to the specification. Every vocabulary "
      "below is frozen at the v1.0 tag. None is wired onto a real catalog entry "
      "yet. See the specification's [known limitations]"
      "(../../spec/limitations.md).\n\n")

    p("## Categories\n\n")
    p("`category` answers WHERE a catalog entry lives. Ids 1 to 14 are the "
      "frozen, complete spec set, in canonical menu order. An unrecognized "
      "id, including an untaught vendor id, MUST render under `other`. It "
      "keeps the catalog-provided label. It is never dropped.\n\n")
    rows = [[code(k), code(reg["ui_categories"][k]["name"]), cell(reg["ui_categories"][k].get("note", ""))]
            for k in sorted(reg["ui_categories"])]
    table(p, ["Id", "Category", "Notes"], rows)
    p("`0x40` to `0x7E` is the vendor/device range. A hub that declares one "
      "MUST supply a label. `15` to `0x3F` is reserved for future "
      "spec-registered categories. `0x7F` and above is reserved.\n\n")

    p("## Ranks\n\n")
    p("`rank` answers HOW MUCH a catalog entry or field matters by default. "
      "Unknown rank, or no rank annotation at all, renders as `detail`.\n\n")
    rows = [[code(k), code(reg["ui_ranks"][k]["name"]), cell(reg["ui_ranks"][k].get("note", ""))]
            for k in sorted(reg["ui_ranks"])]
    table(p, ["Id", "Rank", "Notes"], rows)

    p("## Value axes\n\n")
    p("Three small, orthogonal vocabularies tag what statistic a field is. "
      "The default, when none is given, is `live` / `session` / `actual`.\n\n")
    for section, heading in (("value_aspects", "Aspect"), ("value_scopes", "Scope"), ("value_provenance", "Provenance")):
        p(f"### {heading}\n\n")
        rows = [[code(k), code(reg[section][k]["name"]), cell(reg[section][k].get("note", ""))]
                for k in sorted(reg[section])]
        table(p, ["Id", heading, "Notes"], rows)

    p("## Units\n\n")
    p("Units are a frozen numeric companion to the existing free-string "
      "`unit` field. Both exist side by side. This table is not yet wired "
      "onto real catalog fields; that is next-phase work. The list is "
      "deliberately larger than current needs, to cover future actuators. "
      "An unrecognized unit id renders the catalog's own label string "
      "verbatim.\n\n")
    rows = [[code(k), code(reg["unit_ids"][k]["name"]), cell(reg["unit_ids"][k].get("note", ""))]
            for k in sorted(reg["unit_ids"])]
    table(p, ["Id", "Unit", "Quantity"], rows)

    p("## Action tags\n\n")
    p("A conformant client MAY special-case the specific `action.<name>` "
      "suffixes below. This lets it upgrade a generic `trigger` archetype "
      "into a purpose-specific rendering. An unregistered suffix remains "
      "legal. An unrecognized one renders as a generic trigger.\n\n")
    rows = [[code(tag), cell((reg["action_tags"][tag] or {}).get("note", ""))]
            for tag in reg["action_tags"]]
    table(p, ["Tag", "Meaning"], rows)

    p("## Archetypes\n\n")
    p("An archetype is the control style and interaction contract a catalog "
      "field or channel renders with. A normative decision table derives it "
      "in the common case (RENDERING.md §8.2). An explicit `archetype` hint "
      "overrides that table. `Fallback` is the mandatory composition of "
      "frozen primitives every archetype declares. A primitive lists "
      "itself.\n\n")
    rows = []
    for k in sorted(reg["ui_archetypes"]):
        e = reg["ui_archetypes"][k]
        fallback = " + ".join(code(f) for f in e.get("fallback", []))
        rows.append([code(k), code(e["name"]), cell(e.get("note", "")), fallback])
    table(p, ["Id", "Archetype", "Semantic", "Fallback"], rows)

    p("## Regions\n\n")
    p("There are four abstract placement zones, plus one modal layer. "
      "Geometry, position, size and style within a region are the "
      "renderer author's craft. What lives in each region is normative.\n\n")
    rows = [[code(k), code(reg["ui_regions"][k]["name"]), cell(reg["ui_regions"][k].get("note", ""))]
            for k in sorted(reg["ui_regions"])]
    table(p, ["Id", "Region", "Contents"], rows)

    p("## Renderer classes\n\n")
    p("All classes render one category tree. They differ in projection and "
      "default surfacing, never in reachable content. A device between "
      "budgets adopts the nearer class.\n\n")
    rows = [[code(k), code(reg["renderer_classes"][k]["name"]), cell(reg["renderer_classes"][k].get("note", ""))]
            for k in sorted(reg["renderer_classes"])]
    table(p, ["Id", "Class", "Notes"], rows)

    p("## Widget patterns\n\n")
    p("These are proven compositions extracted from the reference client. "
      "`Required` marks a pattern that a handheld or full client MUST "
      "provide when its capability is present. A glance-class device may "
      "reach it through the category tree instead.\n\n")
    rows = []
    for k in sorted(reg["widget_patterns"]):
        e = reg["widget_patterns"][k]
        rows.append([code(k), code(e["name"]), cell(e.get("note", "")),
                     code("yes") if e.get("required") else ""])
    table(p, ["Id", "Pattern", "Composition", "Required"], rows)
    return w.getvalue()


def page_errors(reg: dict, reg_display: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title="NACK codes",
                 description="Generated table of every SlopSync NACK and GOODBYE reason code, grouped by range.",
                 register="IEEE")
    banner(p, reg_display)
    p("# NACK and GOODBYE codes\n\n")
    p("There is **one** code space, not two. A GOODBYE `code` is drawn from this\n")
    p("same table.\n\n")
    p("A receiver that meets an unknown code treats it as the generic code of\n")
    p("its range, taken from the high byte. That fallback is why a second,\n")
    p("overlapping space was rejected. With two spaces, the range of an\n")
    p("unknown code is ambiguous.\n\n")

    ranges = {
        0x00: ("`0x00xx`: protocol", "The frame itself is unusable."),
        0x01: ("`0x01xx`: session and authorization", "The session cannot proceed as asked."),
        0x02: ("`0x02xx`: subscription and QoS", "The subscription request is refused."),
        0x03: ("`0x03xx`: intent", "The intent is refused on its own merits."),
        0x04: ("`0x04xx`: safety refusal", "The machine refuses on safety grounds. A client SHOULD render these distinctly."),
        0x05: ("`0x05xx`: transfer", "A chunked transfer failed."),
    }
    by_range: dict[int, list] = {}
    for c in sorted(reg["nack_codes"]):
        by_range.setdefault(c >> 8, []).append(c)
    for hi in sorted(by_range):
        heading, intro = ranges.get(hi, (f"`0x{hi:02X}xx`", ""))
        p(f"## {heading}\n\n")
        if intro:
            p(intro + "\n\n")
        rows = [[code(hexcode(c, 4)), code(reg["nack_codes"][c]["name"]),
                 cell(reg["nack_codes"][c].get("note", ""))] for c in by_range[hi]]
        table(p, ["Code", "Name", "Meaning"], rows)
    return w.getvalue()


def page_limits(reg: dict, reg_display: str, raw: str) -> str:
    w = io.StringIO()
    p = w.write
    front_matter(p, title="Limits and defaults",
                 description="Generated table of SlopSync well-known limits, timeouts, caps and defaults.",
                 register="IEEE")
    banner(p, reg_display)
    p("# Limits and defaults\n\n")
    p("Some of these are hard protocol constants. Others are recommended\n")
    p("defaults a hub may tune and then advertise. The Notes column carries the\n")
    p("registry's own rationale where it records one.\n\n")

    notes, groups = scrape_limit_notes(raw)
    order: list[str] = []
    seen = set()
    for key in reg["limits"]:
        g = groups.get(key, "")
        if g not in seen:
            seen.add(g)
            order.append(g)

    for group in order:
        keys = [k for k in reg["limits"] if groups.get(k, "") == group]
        if not keys:
            continue
        if group:
            p(f"## {group[0].upper() + group[1:]}\n\n")
        else:
            p("## Core protocol constants\n\n")
        rows = []
        for k in keys:
            v = reg["limits"][k]
            rows.append([code(k), code(v if isinstance(v, str) else str(v)), cell(notes.get(k, ""))])
        table(p, ["Name", "Value", "Notes"], rows)
    return w.getvalue()


# --------------------------------------------------------------------------
# The Dictionary. Source: docs-site/dictionary.yaml.
# Two outputs, so a term can never have two definitions:
#   docs/reference/dictionary.md      the page a reader reads
#   includes/abbreviations.md         the tooltip definitions appended to
#                                     every page by pymdownx.snippets
# --------------------------------------------------------------------------
def anchor(term: str) -> str:
    """Reproduce the heading slug Python-Markdown's toc extension produces.

    Copied from markdown.extensions.toc.slugify with separator '-'. Note that
    it strips punctuation but KEEPS underscores — `Cfg_gen` slugs to `cfg_gen`,
    not `cfg-gen`. Getting this wrong produces a site full of dead in-page
    links that only `mkdocs build --strict` with anchor validation catches.
    """
    s = re.sub(r"[^\w\s-]", "", term).strip().lower()
    return re.sub(r"[-\s]+", "-", s)


def build_dictionary(d: dict) -> tuple[str, str, int]:
    w = io.StringIO()
    p = w.write
    front_matter(
        p,
        title="The SlopSync Dictionary",
        description=(
            "Every SlopSync term with exactly one definition: hub, client, session, "
            "channel, catalog, etag, grant, shadow, deadman, intent, echo, and the rest."
        ),
        register="STE",
    )
    p("<!-- ==========================================================\n")
    p("     GENERATED FILE. DO NOT EDIT.\n")
    p("     Source of truth: docs-site/dictionary.yaml\n")
    p(f"     Generator:       {GENERATOR_NAME}\n")
    p("     Edit the YAML, then regenerate. The tooltip definitions in\n")
    p("     includes/abbreviations.md come from the same source, so a term\n")
    p("     can never have two definitions.\n")
    p("     ========================================================== -->\n\n")

    p("# The SlopSync Dictionary\n\n")
    p(d["meta"]["intro"].strip() + "\n\n")

    # Index of every term, alphabetical, so a reader can land on a word.
    all_terms = [(t["term"], g) for g in d["groups"] for t in g["terms"]]
    p("!!! abstract \"One term, one meaning\"\n\n")
    p("    Every term below has exactly one definition on this site. If prose\n")
    p("    anywhere uses a word from this page, it means what this page says.\n")
    p("    Hover any defined term anywhere on the site to see its definition.\n\n")

    # A `see` target that is not a defined term would render as a dead in-page
    # link. Catch it here, where the message can name the offender, instead of
    # letting the reader find it.
    defined = {t["term"] for g in d["groups"] for t in g["terms"]}
    for g in d["groups"]:
        for t in g["terms"]:
            for ref in t.get("see", []):
                if ref not in defined:
                    raise SystemExit(
                        f"dictionary.yaml: term '{t['term']}' points `see` at "
                        f"'{ref}', which is not a defined term."
                    )

    count = 0
    abbr = io.StringIO()
    abbr.write("<!-- GENERATED FILE. DO NOT EDIT. "
               f"Source: docs-site/dictionary.yaml via {GENERATOR_NAME} -->\n")
    abbr.write("<!-- Appended to every page by pymdownx.snippets.auto_append, so a\n")
    abbr.write("     defined term shows its Dictionary definition on hover. -->\n\n")
    emitted: dict[str, str] = {}

    for g in d["groups"]:
        p(f"## {g['name']}\n\n")
        if g.get("intro"):
            p(re.sub(r"\s+", " ", g["intro"]).strip() + "\n\n")
        for t in g["terms"]:
            count += 1
            term = t["term"]
            p(f"### {term}\n\n")
            short = re.sub(r"\s+", " ", t["short"]).strip()
            p(f"**{short}**\n\n")
            if t.get("body"):
                body = t["body"].strip()
                p(body + "\n\n")
            meta_bits = []
            if t.get("see"):
                links = ", ".join(f"[{s}](#{anchor(s)})" for s in t["see"])
                meta_bits.append(f"See also: {links}")
            if t.get("source"):
                meta_bits.append(f"Source: {t['source']}")
            if meta_bits:
                p(" · ".join(meta_bits) + "\n{ .ss-termmeta }\n\n")

            # Tooltip triggers. `abbr` matching is case-sensitive, so each
            # written form needs its own definition line.
            tip = re.sub(r"[`*]", "", short)
            tip = tip.replace("[", "").replace("]", "")
            for form in dict.fromkeys(t.get("abbr", [term])):
                if form in emitted and emitted[form] != tip:
                    raise SystemExit(
                        f"dictionary.yaml: '{form}' has two different tooltip "
                        f"definitions. One term, one meaning — merge them."
                    )
                emitted[form] = tip
    for form in sorted(emitted, key=lambda s: (-len(s), s)):
        abbr.write(f"*[{form}]: {emitted[form]}\n")

    return w.getvalue(), abbr.getvalue(), count


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------
def build_all() -> dict[Path, str]:
    reg_path = registry_path()
    raw = reg_path.read_text(encoding="utf-8")
    reg = yaml.safe_load(raw)

    unclaimed = sorted(set(reg) - set(SECTION_HOMES))
    if unclaimed:
        raise SystemExit(
            "registry.yaml has section(s) no documentation page claims: "
            + ", ".join(unclaimed)
            + f"\n  Add each to SECTION_HOMES in {GENERATOR_NAME} and render it.\n"
            "  An undocumented wire number is exactly what this gate exists to stop."
        )
    missing = sorted(set(SECTION_HOMES) - set(reg))
    if missing:
        raise SystemExit(
            "documentation expects registry section(s) that no longer exist: "
            + ", ".join(missing)
        )

    # Display path is relative when it can be, so generated files do not carry
    # anybody's home directory into git.
    try:
        reg_display = reg_path.relative_to(SITE_ROOT.parent).as_posix()
    except ValueError:
        reg_display = reg_path.as_posix()

    out: dict[Path, str] = {
        GEN_DIR / "index.md": page_index(reg, reg_display),
        GEN_DIR / "frames.md": page_frames(reg, reg_display),
        GEN_DIR / "channels.md": page_channels(reg, reg_display),
        GEN_DIR / "cbor-keys.md": page_cbor_keys(reg, reg_display),
        GEN_DIR / "catalog-vocabulary.md": page_catalog_vocabulary(reg, reg_display),
        GEN_DIR / "events.md": page_events(reg, reg_display),
        GEN_DIR / "safety.md": page_safety(reg, reg_display),
        GEN_DIR / "pairing.md": page_pairing(reg, reg_display),
        GEN_DIR / "errors.md": page_errors(reg, reg_display),
        GEN_DIR / "limits.md": page_limits(reg, reg_display, raw),
        GEN_DIR / "discovery.md": page_discovery(reg, reg_display),
        GEN_DIR / "rendering.md": page_rendering(reg, reg_display),
    }

    d = yaml.safe_load(DICT_SRC.read_text(encoding="utf-8"))
    dict_md, abbr_md, count = build_dictionary(d)
    out[DICT_OUT] = dict_md
    out[ABBR_OUT] = abbr_md
    build_all.term_count = count  # type: ignore[attr-defined]
    return out


def main() -> int:
    if "--list" in sys.argv:
        for path in build_all():
            print(path.relative_to(SITE_ROOT.parent).as_posix())
        return 0

    files = build_all()
    stale: list[Path] = []
    for path, text in files.items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current != text:
            stale.append(path)

    if "--check" in sys.argv:
        if stale:
            print("STALE generated documentation:", file=sys.stderr)
            for path in stale:
                print(f"  {path}", file=sys.stderr)
            print(
                "\nRegenerate with:\n"
                "  python docs-site/tools/gen_docs_tables.py\n"
                "then commit the result. Documentation numbers are never typed "
                "by hand.",
                file=sys.stderr,
            )
            return 1
        print(f"docs tables up to date ({len(files)} files, "
              f"{getattr(build_all, 'term_count', 0)} dictionary terms)")
        return 0

    for path, text in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {len(files)} files ({len(stale)} changed), "
          f"{getattr(build_all, 'term_count', 0)} dictionary terms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
