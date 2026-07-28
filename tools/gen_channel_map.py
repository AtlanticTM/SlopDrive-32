#!/usr/bin/env python3
"""Generate the two tables in docs/slopsync/CHANNEL-MAP.md from the wire
sources of truth: docs/slopsync/registry/registry.yaml (core channels,
0x0000-0x007F) and include/comms/SlopSyncCatalog.h (this device's own
channels, 0x0080-0x7FFF). RFC-047 (Phase C2, then Phase C4's family-nibble
sub-slot convention): the device grid was renumbered onto 0xCDSS and the
hand-written tables drifted from the code the moment a single id changed —
this script is how CHANNEL-MAP.md never lies again.

Usage:
    python tools/gen_channel_map.py           # (re)write the two GENERATED
                                               # sections in CHANNEL-MAP.md
    python tools/gen_channel_map.py --check   # exit 1 if the committed doc
                                               # doesn't match

Run with PlatformIO's bundled python (has PyYAML):
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools/gen_channel_map.py

WHAT IS GENERATED vs CURATED:
  * id, wire name, class, and the "Old" id (for device channels that moved)
    are MECHANICALLY DERIVED from the source files below — never hand-typed.
  * The one-line "What it is" / "Contains" prose is CURATED in the
    DESCRIPTIONS dict below (the catalog has no entry-level free-text
    description field to derive it from). Adding a new device channel means
    adding one line here; --check fails loudly if a channel has no
    description, so this can't silently go stale into a blank cell.
  * Everything OUTSIDE the two <!-- GENERATED:*:BEGIN/END --> marker pairs in
    CHANNEL-MAP.md (the intro prose, the 0xCDSS grid legend, the worked
    examples, the legality note) is hand-maintained and untouched by this
    script.
"""
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "docs" / "slopsync" / "registry" / "registry.yaml"
CATALOG_H = ROOT / "include" / "comms" / "SlopSyncCatalog.h"
DOC = ROOT / "docs" / "slopsync" / "CHANNEL-MAP.md"

GENERATOR_NAME = "tools/gen_channel_map.py"

# ---------------------------------------------------------------------------
# RFC-047 device grid decode: 0xCDSS -- C=class, D=domain, SS=slot.
# ---------------------------------------------------------------------------
CLASS_NAMES = {1: "STATE", 2: "STREAM", 3: "INTENT", 4: "EVENT", 5: "STORE"}
DOMAIN_NAMES = {0: "machine", 1: "motion", 2: "pattern"}


def decode_grid(id_: int) -> str:
    """Phase C4: the low byte is [family][member] nibbles, not a flat slot
    counter -- `1:0` reads as family 1 member 0 (the family's master), and
    matching family:member across class bands is exactly the mirror rule
    (e.g. slopmotion-limits STATE `2:0` / slopmotion-set INTENT `2:0`)."""
    c = (id_ >> 12) & 0xF
    d = (id_ >> 8) & 0xF
    fam = (id_ >> 4) & 0xF
    mem = id_ & 0xF
    cname = CLASS_NAMES.get(c, "?")
    dname = DOMAIN_NAMES.get(d, "?")
    return f"{cname}·{dname}·{fam:X}:{mem:X}"


# ---------------------------------------------------------------------------
# CURATED one-line descriptions, keyed by the ch:: symbol name (device) or the
# registry core_channels name (core, pulled straight from registry.yaml's own
# `note` field instead -- core channels don't need a second copy here).
# ---------------------------------------------------------------------------
DESCRIPTIONS = {
    "machine_config": "geometry, limits, window — the machine's shape",
    "power": "volts/amps/watts from the INA228",
    "odometer": "session totals: distance, strokes, energy, peak",
    "machine_modes": "blend / stream-speed / overshoot toggles",
    "motion": "position/target/raw/speed — THE live motion feed",
    "plan_strip": "the planner's current plan, visualized",
    "motion_diag": "engine internals: plan timing, anomaly counters",
    "sm_limits": "live ceilings the engine derived",
    "sm_chase": "chase-mode tuning state",
    "sm_waveform": "waveform-mode tuning state",
    "pattern_state": "generator running/pattern/speed/depth",
    "pattern_advanced": "fray-d Advanced mode master state",
    "pattern_adv_mod_depth1": "Advanced modifier lane: depth (max stroke)",
    "pattern_adv_mod_depth2": "Advanced modifier lane: depth (min stroke)",
    "pattern_adv_mod_speedin": "Advanced modifier lane: in-speed",
    "pattern_adv_mod_speedout": "Advanced modifier lane: out-speed",
    "pattern_adv_mod_accelin": "Advanced modifier lane: in-accel",
    "pattern_adv_mod_accelout": "Advanced modifier lane: out-accel",
    "pattern_presets_roster": "saved presets, listed",
    "motion_input": "dense c2h samples {target, velocity} — the MFP wire",
    "motion_segment": "c2h segments {target, duration, end_vel} — funscript wire",
    "config_set": "window/speed/accel writes (post-clamp echo)",
    "modes_set": "machine-modes writes",
    "machine_admin": "clear-fault, servo-scan, save-to-NVS, reboot",
    "move": "manual point move",
    "home": "begin homing",
    "sm_set": "SlopMotion live-tuning writes",
    "pattern_cmd": "run/stop/pattern-select/speed/depth",
    "pattern_advanced_cmd": "shared writer behind all seven pattern-advanced STATE cards",
    "pattern_presets_cmd": "save/load/delete/rename for the preset store",
    "motion_anomaly": "engine anomaly edges (scaled/centered/fallback…)",
    "pattern_presets": "Advanced preset blobs",
}

# ---------------------------------------------------------------------------
# OLD id, keyed by ch:: symbol name -- each channel's immediately-preceding
# id, as of the last renumber IT participated in. Two renumber waves so far:
# Phase C2 (RFC-047, full grid move off the flat 0x0080+ range) touched every
# channel; Phase C4 (the family-nibble sub-slot convention, LEDGER.md
# "Phase C4 STAMPED") touched only a subset. A channel this wave didn't move
# still shows its Phase C2 value -- that IS its most recent change. A channel
# with no entry here is treated as NEWLY ALLOCATED (blank Old cell).
# ---------------------------------------------------------------------------
OLD_ID = {
    "motion": "0x0080",
    "machine_config": "0x0081",
    "pattern_state": "0x0082",
    "odometer": "0x1002",
    "motion_input": "0x0084",
    "motion_segment": "0x0085",
    "plan_strip": "0x1101",
    "power": "0x1001",
    "motion_diag": "0x1102",
    "motion_anomaly": "0x0089",
    "machine_modes": "0x1003",
    "sm_limits": "0x1103",
    "sm_chase": "0x1104",
    "sm_waveform": "0x1105",
    "pattern_advanced": "0x1201",
    "pattern_adv_mod_depth1": "0x1202",
    "pattern_adv_mod_depth2": "0x1203",
    "pattern_adv_mod_speedin": "0x1204",
    "pattern_adv_mod_speedout": "0x1205",
    "pattern_adv_mod_accelin": "0x1206",
    "pattern_adv_mod_accelout": "0x1207",
    "pattern_presets": "0x5200",
    "pattern_presets_roster": "0x1208",
    "move": "0x0100",
    "config_set": "0x0101",
    "pattern_cmd": "0x0102",
    "home": "0x0103",
    "modes_set": "0x3001",
    "sm_set": "0x3102",
    "machine_admin": "0x3002",
    "pattern_advanced_cmd": "0x3201",
    "pattern_presets_cmd": "0x3202",
}


def parse_ch_symbols(src: str) -> dict[str, int]:
    """`namespace ch { inline constexpr uint16_t NAME = 0xXXXX; ... }` -> {NAME: id}."""
    m = re.search(r"namespace ch \{(.*?)\}\s*//\s*namespace ch", src, re.DOTALL)
    if not m:
        raise SystemExit(f"{CATALOG_H}: could not find 'namespace ch { ... }' block")
    body = m.group(1)
    out = {}
    for name, hexid in re.findall(
        r"inline constexpr uint16_t\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", body
    ):
        out[name] = int(hexid, 16)
    return out


def parse_add_entries(src: str, ch_symbols: dict[str, int]):
    """Every `c.addEntry({ ... });` — flat designated-init literals, no nested
    braces — yielding (id_or_None, name_or_None, cls_or_None) per call. `id`
    is None when the call uses a lambda PARAMETER (addApModifierChannel's
    `.id = id`) rather than a literal `ch::X`; those are resolved separately
    from their invocation sites.
    """
    out = []
    for body in re.findall(r"c\.addEntry\(\{(.*?)\}\);", src, re.DOTALL):
        id_m = re.search(r"\.id\s*=\s*([\w:]+)", body)
        name_m = re.search(r'\.name\s*=\s*"([^"]+)"', body)
        cls_m = re.search(r"\.cls\s*=\s*ChannelClass::(\w+)", body)
        id_val = None
        if id_m:
            tok = id_m.group(1)
            if tok.startswith("ch::"):
                sym = tok[len("ch::") :]
                id_val = ch_symbols.get(sym)
            elif tok.startswith("slopsync::channels::") or tok == "id":
                id_val = None  # core channel (resolved via registry.yaml) or a lambda param
        out.append(
            (
                id_val,
                name_m.group(1) if name_m else None,
                cls_m.group(1) if cls_m else None,
            )
        )
    return out


def parse_ap_modifier_invocations(src: str):
    """addApModifierChannel(ch::SYMBOL, "wire-name", "group", keyBase); x6."""
    out = []
    for sym, wire_name in re.findall(
        r"addApModifierChannel\(\s*ch::(\w+)\s*,\s*\"([^\"]+)\"", src
    ):
        out.append((sym, wire_name))
    return out


def gen_core_table(reg: dict) -> str:
    lines = [
        "| ID | Name | Class | What it is |",
        "|---|---|---|---|",
    ]
    # 0x0000 SESSION is a pseudo-channel under `channel_id_ranges`, not a real
    # entry in `core_channels` (it names frames, not a subscribable channel) —
    # prepended by hand from that section, matching the original table's row.
    session = reg["channel_id_ranges"][0x0000]
    lines.append(f"| 0x0000 | {session['name']} | — | {session['note']} |")
    for cid in sorted(reg["core_channels"]):
        e = reg["core_channels"][cid]
        lines.append(f"| 0x{cid:04X} | {e['name']} | {e['class']} | {e['note']} |")
    return "\n".join(lines) + "\n"


def gen_device_table(ch_symbols: dict[str, int], src: str) -> str:
    # Reverse-lookup: id -> symbol name.
    id_to_sym = {v: k for k, v in ch_symbols.items()}

    entries = parse_add_entries(src, ch_symbols)
    # Resolve device (non-core) rows: id_val present and matches a known ch:: id.
    rows = []  # (id, name, cls)
    seen_ids = set()
    for id_val, name, cls in entries:
        if id_val is None or name is None or cls is None:
            continue
        if id_val not in id_to_sym:
            continue  # a core channel resolved some other way; skip
        rows.append((id_val, name, cls))
        seen_ids.add(id_val)

    # The addApModifierChannel lambda's own addEntry uses `.id = id` (a
    # parameter) and `.cls = ChannelClass::STATE` (a literal) — resolved from
    # its six invocation call sites instead.
    ap_cls_m = re.search(
        r"auto addApModifierChannel = \[&\]\([^)]*\)\s*\{.*?\.cls\s*=\s*ChannelClass::(\w+)",
        src,
        re.DOTALL,
    )
    ap_cls = ap_cls_m.group(1) if ap_cls_m else "STATE"
    for sym, wire_name in parse_ap_modifier_invocations(src):
        id_val = ch_symbols.get(sym)
        if id_val is None or id_val in seen_ids:
            continue
        rows.append((id_val, wire_name, ap_cls))
        seen_ids.add(id_val)

    missing_desc = sorted(
        {id_to_sym[i] for i, _, _ in rows if id_to_sym.get(i) not in DESCRIPTIONS}
    )
    if missing_desc:
        raise SystemExit(
            f"{GENERATOR_NAME}: no curated description for: {', '.join(missing_desc)} "
            f"-- add one to DESCRIPTIONS before regenerating."
        )

    rows.sort(key=lambda r: r[0])

    lines = [
        "| New ID | Reads as | Name | Old | What it is |",
        "|---|---|---|---|---|",
    ]
    for id_val, name, cls in rows:
        sym = id_to_sym[id_val]
        old = OLD_ID.get(sym, "(new)")
        desc = DESCRIPTIONS[sym]
        lines.append(f"| 0x{id_val:04X} | {decode_grid(id_val)} | {name} | {old} | {desc} |")
    return "\n".join(lines) + "\n"


BEGIN = "<!-- GENERATED:{tag}:BEGIN — DO NOT EDIT. Source: {src} via " + GENERATOR_NAME + " -->\n"
END = "<!-- GENERATED:{tag}:END -->\n"


def splice(doc_text: str, tag: str, src_desc: str, table_md: str) -> str:
    begin_re = re.compile(
        r"<!-- GENERATED:" + tag + r":BEGIN.*?-->\n", re.DOTALL
    )
    end_re = re.compile(r"<!-- GENERATED:" + tag + r":END -->\n")
    b = begin_re.search(doc_text)
    e = end_re.search(doc_text)
    if not b or not e or e.start() < b.end():
        raise SystemExit(
            f"{DOC}: missing or malformed <!-- GENERATED:{tag}:BEGIN/END --> markers"
        )
    new_block = BEGIN.format(tag=tag, src=src_desc) + table_md + END.format(tag=tag)
    return doc_text[: b.start()] + new_block + doc_text[e.end() :]


def main(argv) -> int:
    reg = yaml.safe_load(REGISTRY.read_text(encoding="utf-8"))
    src = CATALOG_H.read_text(encoding="utf-8")
    ch_symbols = parse_ch_symbols(src)

    core_md = gen_core_table(reg)
    device_md = gen_device_table(ch_symbols, src)

    doc_text = DOC.read_text(encoding="utf-8") if DOC.exists() else ""
    if not doc_text:
        raise SystemExit(f"{DOC} does not exist -- cannot splice, nothing to preserve")

    new_text = splice(doc_text, "CORE", "registry.yaml core_channels", core_md)
    new_text = splice(new_text, "DEVICE", "SlopSyncCatalog.h", device_md)

    if "--check" in argv:
        if new_text != doc_text:
            print(f"STALE: {DOC} does not match generated tables — regenerate.", file=sys.stderr)
            return 1
        print("channel map up to date")
        return 0

    DOC.write_text(new_text, encoding="utf-8", newline="\n")
    print(f"wrote {DOC}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
