#!/usr/bin/env python3
"""SlopCanon mechanical floor -- judgment-free doctrine checks.

Defined by docs/canon/CANON.md SS5. Every hit is a defect BY DEFINITION:
these checks encode only hard rules (violation classes that have actually
bitten this project). If a check fires falsely, the fix is a C-7 amendment
to the exemption lists in this file -- never ignoring the output.

Deliberately NOT here (judgment calls belong to review, not grep):
  - delay() outside init/calibration (init exceptions are legal, grep can't tell)
  - digitalWrite/ledcWrite on LEDs outside SlopGlowBoard.cpp (grep can't tell
    an LED pin from a relay pin)

Usage:
    python tools/canon_lint.py            # all checks
    python tools/canon_lint.py --no-gen   # skip the CHANNEL-MAP.md codegen --check

Exit codes: 0 clean, 1 findings, 2 could not run.
"""

import hashlib
import io
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIBLING = ROOT.parent / "SlopSync"
PIN_FILE = ROOT / "slopsync.pin"

# ---------------------------------------------------------------- frozen (C-6)
# mini_catalog.hpp/mini-catalog.yaml moved to the SlopSync repo split (they are
# lib/slopsync + spec conformance artifacts, not machine-repo files) -- this
# tree no longer holds them, so the frozen check now runs against the pinned
# SIBLING checkout (see run_pin_check below). Byte-identical there or it's a
# protocol break; SlopSync's own tools/slopsync_lint.py carries the same pins
# as its half of the belt-and-suspenders check.
FROZEN_SHA256_SIBLING = {
    "lib/slopsync/include/slopsync/conformance/mini_catalog.hpp":
        "2a90bf8a5658b4ecb2c96a28d9aa9e39a908926c91e0aebba5844366c3252eb2",
    "spec/vectors/fixtures/mini-catalog.yaml":
        "b2b6a3063e66b56916683c6878ce237085fbcd8ea145c02369f5b96a45c6901c",
}

VENDORED_PREFIXES = (
    "lib/ruckig/", "lib/espasyncwebserver/", "lib/asynctcp/",
    "webui/src/fonts/", ".cache/",
)
BINARY_SUFFIXES = (".bin", ".png", ".jpg", ".webp", ".ico", ".pdf",
                   ".woff", ".woff2", ".idx", ".gz", ".lock")

# ------------------------------------------------------------------ C-11
# American English only. Operator ruling 2026-07-28: the hand-rolled
# stem/suffix regex is retired -- no reinventing a British-word list by hand
# where a proven tool exists. codespell's en-GB_to_en-US builtin dictionary
# is now the mechanism. This is a hard dependency of this check: if
# codespell is not importable, the check FAILS LOUDLY (see
# run_codespell_check below), never silently skips. Minimum version 2.4
# (built and verified against codespell 2.4.3).
try:
    import codespell_lib
    _CODESPELL_IMPORT_ERROR = None
except ImportError as e:
    codespell_lib = None
    _CODESPELL_IMPORT_ERROR = str(e)

# Small supplemental list for real words this project's prose actually uses
# that codespell's en-GB_to_en-US dictionary is verified NOT to carry (checked
# 2026-07-28 against codespell 2.4.3, test words favour/acknowledgement/
# catalogue/analyse/initialise/grey/judgement/behaviour/centre/travelled --
# every one of those matched natively EXCEPT "travelled", which codespell has
# no "travel-" doubled-L entry for at all). Anything codespell already
# catches must NOT be duplicated here -- verify against codespell's actual
# dictionary before adding, and name the gap in the comment like this one.
BRITISH_SPELLING_EXTRAS = {
    "travelled": "traveled",
    "travelling": "traveling",
    "traveller": "traveler",
    "travellers": "travelers",
}
_BRITISH_EXTRAS_RX = re.compile(
    r"\b(?:%s)\b" % "|".join(sorted(BRITISH_SPELLING_EXTRAS, key=len, reverse=True)),
    re.IGNORECASE,
)

# ---------------------------------------------------------------- C-11 (camelCase gap)
# codespell's word regex is r"[\w\-'']+" -- \w includes underscore, so it
# treats "colourMode", "waveform_centred", and "kColourTable" as ONE token
# each and never matches them against the dictionary key "colour"/"centred".
# This closes that gap by splitting every identifier on case boundaries,
# underscores, and digit boundaries before the dictionary check -- same
# codespell dictionary, no hand-rolled wordlist (operator ruling 2026-07-28,
# full-tree camelCase sweep). Any subword >=4 chars that matches is a hit.
_IDENT_WORD_RX = re.compile(r"[A-Za-z][A-Za-z0-9_]*")
_DIGIT_BOUNDARY_RX = re.compile(r"(?<=[A-Za-z])(?=[0-9])|(?<=[0-9])(?=[A-Za-z])")
_CAMEL_BOUNDARY_RX = re.compile(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])")


def _split_subwords(token):
    out = []
    for piece in token.split("_"):
        if not piece:
            continue
        piece = _DIGIT_BOUNDARY_RX.sub(" ", piece)
        for chunk in piece.split(" "):
            if not chunk:
                continue
            chunk = _CAMEL_BOUNDARY_RX.sub(" ", chunk)
            out.extend(c for c in chunk.split(" ") if c)
    return out


def _load_gb_dictionary():
    """The exact dictionary codespell ships -- read from its own package data,
    never retyped. Same file run_codespell_check's codespell_lib.main() reads
    internally; this just gives us subword-level access to it."""
    if codespell_lib is None:
        return {}
    data_dir = Path(codespell_lib.__file__).resolve().parent / "data"
    path = data_dir / "dictionary_en-GB_to_en-US.txt"
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or "->" not in line:
            continue
        brit, us = line.split("->", 1)
        out[brit.strip().lower()] = us.strip()
    return out


def _subword_hits(token, dictionary):
    hits = []
    for sub in _split_subwords(token):
        if len(sub) < 4 or sub.lower() == token.lower():
            continue  # whole-word hits are codespell's own job, not this gap
        low = sub.lower()
        if low in dictionary:
            hits.append((sub, dictionary[low]))
        elif low in BRITISH_SPELLING_EXTRAS:
            hits.append((sub, BRITISH_SPELLING_EXTRAS[low]))
    return hits


def run_camelcase_check():
    """C-11 gap-closer: British spelling hiding inside a camelCase/PascalCase/
    combined-snake_case identifier, where codespell's whole-word match can't
    see it. Filenames are in scope too -- a British-spelled path is the same
    defect as a British-spelled variable."""
    if codespell_lib is None:
        return []  # run_codespell_check already reports the missing dependency loudly
    dictionary = _load_gb_dictionary()
    findings = []

    all_files = list(tracked_files())
    for rel in all_files:
        for component in rel.split("/"):
            stem = component.rsplit(".", 1)[0] if "." in component else component
            for tok in _IDENT_WORD_RX.findall(stem):
                for sub, sugg in _subword_hits(tok, dictionary):
                    findings.append(("british-spelling-subword", rel, 0,
                                     f"{tok} (filename subword {sub!r} -> {sugg})",
                                     "British spelling inside a filename component "
                                     "(CANON C-11 -- camelCase/subword gap)"))

    content_files = [f for f in all_files if f not in BRITISH_SPELLING_SCAN_EXEMPT]
    for rel in content_files:
        try:
            text = (ROOT / rel).read_text(encoding="utf-8", errors="strict")
        except (UnicodeDecodeError, OSError):
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if (rel, lineno) in BRITISH_SPELLING_KNOWN_CODE_HITS:
                continue
            for tok in _IDENT_WORD_RX.findall(line):
                if len(tok) < 4:
                    continue
                for sub, sugg in _subword_hits(tok, dictionary):
                    findings.append(("british-spelling-subword", rel, lineno,
                                     f"{tok} (subword {sub!r} -> {sugg})",
                                     "British spelling inside a compound identifier "
                                     "(CANON C-11 -- camelCase/subword gap, codespell "
                                     "can't see across case/underscore boundaries)"))
    return findings

# Files the spelling scan must never touch: this file names the banned words
# by construction (the extras dict above, this comment), and legal texts are
# verbatim by law, not by style.
BRITISH_SPELLING_SCAN_EXEMPT = ("THIRD_PARTY_LICENSES.md", "LICENSE", "NOTICE",
                                "tools/canon_lint.py")

# Every catalogued code-comment hit was fixed in the 2026-07-28 pass. Kept as
# a MECHANISM, not a list: adding an entry here requires an operator-visible
# justification in the commit that adds it, never a silent exemption.
#
# Justification for the docs/canon/LEDGER.md section below (2026-07-28
# BRITISH-SPELLING TOTAL SWEEP entry): that entry is an audit record, and
# quotes the actual banned strings it found/fixed/flagged/false-positived
# (the same reason this file's own BRITISH_SPELLING_EXTRAS comment above is
# exempt) -- not a live comment or prose defect. Never add a section here
# for an ordinary hit; fix it instead.
#
# Anchored to the SECTION HEADER, never line numbers: the ledger is a living
# document and line-pinned exemptions break on any edit above the section
# (proven 2026-07-28 -- a 6-line insertion shifted the sweep entry and
# produced 18 false findings). A section runs from its `## ` header to the
# next `## ` header.
BRITISH_SPELLING_EXEMPT_SECTIONS = {
    "docs/canon/LEDGER.md": ("## BRITISH-SPELLING TOTAL SWEEP",),
}


def _exempt_section_lines():
    hits = set()
    for rel, headers in BRITISH_SPELLING_EXEMPT_SECTIONS.items():
        try:
            lines = (ROOT / rel).read_text(encoding="utf-8",
                                           errors="replace").splitlines()
        except OSError:
            continue
        active = False
        for lineno, line in enumerate(lines, 1):
            if line.startswith("## "):
                active = any(line.startswith(h) for h in headers)
            if active:
                hits.add((rel, lineno))
    return hits


BRITISH_SPELLING_KNOWN_CODE_HITS = _exempt_section_lines()

GREP_CHECKS = [
    dict(
        name="serial-print",
        msg="Serial.print outside the SlopLog sink (CLAUDE.md SS7.5: logging goes through SlopLog. Only.)",
        rx=re.compile(r"\bSerial\.print"),
        include=("src/", "include/", "lib/sloplog/", "lib/slopglow/",
                 "lib/slopmotion/", "lib/slopsync/"),
        exempt=("src/system/AppLog.cpp",          # the serial SINK itself
                "src/c5_tdongle/", "src/c5_waveshare/"),  # until sloplog vendored to C5s
    ),
    dict(
        name="slopsync-purity",
        msg="platform header inside hardware-free lib/slopsync (CLAUDE.md SS8: std headers only)",
        rx=re.compile(r'#\s*include\s*[<"](?:Arduino\.h|freertos/|esp_|driver/|soc/|nvs)'),
        include=("lib/slopsync/include/",),
        exempt=(),
    ),
    dict(
        name="this-assign",
        msg="*this = T{...} reset pattern (field bug #1: 9 KB stack temporary blew the task stack; "
            "use in-place destroy + placement-new)",
        rx=re.compile(r"\*\s*this\s*=\s*"),
        include=("src/", "include/", "lib/slopsync/", "lib/slopmotion/",
                 "lib/sloplog/", "lib/slopglow/"),
        exempt=(),
    ),
    dict(
        name="links2004-ghost",
        msg="reference to the deleted links2004 WS stack (removed in M5c, fw 2.1.65)",
        rx=re.compile(r"links2004|arduinoWebSockets|\bWebSocketsServer\b|\bWebSocketsClient\b"),
        include=("src/", "include/", "webui/src/", "platformio.ini"),
        exempt=(),
    ),
    # british-spelling moved to run_codespell_check() (operator ruling
    # 2026-07-28: codespell, not a hand-rolled regex, does this job now) --
    # it does not fit the single-regex-per-check shape of this list.
]


def tracked_files():
    out = subprocess.run(["git", "ls-files"], cwd=ROOT,
                         capture_output=True, text=True, check=True).stdout
    for f in out.splitlines():
        if f.startswith(VENDORED_PREFIXES) or f.endswith(BINARY_SUFFIXES):
            continue
        yield f


def run_grep_checks():
    findings = []
    for rel in tracked_files():
        applicable = [c for c in GREP_CHECKS
                      if any(rel.startswith(p) for p in c["include"])
                      and not any(rel.startswith(e) for e in c["exempt"])]
        if not applicable:
            continue
        try:
            text = (ROOT / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            for c in applicable:
                m = c["rx"].search(line)
                if m:
                    findings.append((c["name"], rel, lineno, m.group(0), c["msg"]))
    return findings


def run_codespell_check():
    """C-11 American-English check: codespell's en-GB_to_en-US builtin
    dictionary plus the small BRITISH_SPELLING_EXTRAS gap-list. Hard
    dependency -- codespell missing is a loud finding, never a silent skip."""
    if codespell_lib is None:
        return [("codespell-missing", "tools/canon_lint.py", 0, "",
                 f"codespell is not installed ({_CODESPELL_IMPORT_ERROR}) -- "
                 "pip install codespell (>=2.4) -- the British-spelling rule "
                 "has no fallback and refuses to silently skip")]

    files = [f for f in tracked_files() if f not in BRITISH_SPELLING_SCAN_EXEMPT]
    if not files:
        return []
    abs_paths = [str(ROOT / f) for f in files]

    buf = io.StringIO()
    old_stdout = sys.stdout
    sys.stdout = buf
    try:
        codespell_lib.main(*abs_paths, "--builtin", "en-GB_to_en-US")
    finally:
        sys.stdout = old_stdout

    findings = []
    hit_rx = re.compile(r"^(.*):(\d+): (\S+) ==>")
    for line in buf.getvalue().splitlines():
        m = hit_rx.match(line)
        if not m:
            continue
        try:
            rel = Path(m.group(1)).resolve().relative_to(ROOT).as_posix()
        except ValueError:
            rel = m.group(1)
        lineno = int(m.group(2))
        if (rel, lineno) in BRITISH_SPELLING_KNOWN_CODE_HITS:
            continue
        findings.append(("british-spelling", rel, lineno, m.group(3),
                         "British spelling (CANON C-11: American English only "
                         "-- codespell en-GB_to_en-US)"))

    for rel in files:
        try:
            text = (ROOT / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if (rel, lineno) in BRITISH_SPELLING_KNOWN_CODE_HITS:
                continue
            m = _BRITISH_EXTRAS_RX.search(line)
            if m:
                findings.append(("british-spelling", rel, lineno, m.group(0),
                                 "British spelling (CANON C-11: American English "
                                 "only -- house extras list)"))
    return findings


def run_pin_check():
    """slopsync.pin RULE: FAIL if ../SlopSync is missing or its HEAD doesn't
    match the pin; WARN (not fail) if the sibling working tree is dirty; FAIL
    if the sibling's frozen conformance artifacts don't match our pinned
    hashes (the moved half of the old in-tree frozen check, C-6)."""
    findings = []
    try:
        pinned = PIN_FILE.read_text(encoding="utf-8").splitlines()[0].strip()
    except OSError:
        return [("pin-missing", "slopsync.pin", 0, "", "slopsync.pin is missing")]

    if not SIBLING.is_dir():
        return [("pin-sibling-missing", "../SlopSync", 0, "",
                 "sibling checkout not found next to this repo -- clone SlopSync alongside SlopDrive-32")]

    r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=SIBLING,
                       capture_output=True, text=True)
    if r.returncode != 0:
        return [("pin-sibling-not-git", "../SlopSync", 0, "",
                 "sibling exists but `git rev-parse HEAD` failed there")]
    head = r.stdout.strip()
    if head != pinned:
        findings.append(("pin-mismatch", "slopsync.pin", 0, head[:16],
                         f"../SlopSync HEAD {head[:16]} != pinned {pinned[:16]} -- "
                         "bump slopsync.pin (and re-run the gauntlet) or check out the pinned sha"))

    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=SIBLING,
                           capture_output=True, text=True).stdout.strip()
    if dirty:
        print("WARN: ../SlopSync working tree is dirty (not a lint failure)")

    for rel, want in FROZEN_SHA256_SIBLING.items():
        p = SIBLING / rel
        if not p.exists():
            findings.append(("pin-frozen-missing", f"../SlopSync/{rel}", 0, "",
                             "frozen artifact is GONE from the sibling"))
            continue
        got = hashlib.sha256(p.read_bytes()).hexdigest()
        if got != want:
            findings.append(("pin-frozen-changed", f"../SlopSync/{rel}", 0, got[:16],
                             "frozen artifact modified in the sibling (C-6) -- protocol break unless amended"))
    return findings


def run_channel_map_check():
    gen = ROOT / "tools" / "gen_channel_map.py"
    try:
        r = subprocess.run([sys.executable, str(gen), "--check"], cwd=ROOT,
                           capture_output=True, text=True, timeout=120)
    except Exception as e:  # missing yaml module etc. -- report, don't hide
        return [("channel-map-check", "tools/gen_channel_map.py", 0, str(e)[:60],
                 "could not run channel-map --check (run it manually with the pio python)")]
    if r.returncode != 0:
        return [("channel-map-drift", "docs/slopsync/CHANNEL-MAP.md", 0, "",
                 "generated channel-map tables out of sync -- regenerate, never hand-edit "
                 "inside a GENERATED marker pair")]
    return []


def main(argv):
    findings = run_grep_checks() + run_pin_check() + run_codespell_check() + run_camelcase_check()
    if "--no-gen" not in argv:
        findings += run_channel_map_check()

    if not findings:
        print("canon_lint: clean (0 findings)")
        return 0

    findings.sort(key=lambda f: (f[0], f[1], f[2]))
    by_check = {}
    for name, rel, lineno, match, msg in findings:
        by_check.setdefault(name, []).append((rel, lineno, match, msg))
    for name, items in by_check.items():
        print("[%s] %d hit(s) -- %s" % (name, len(items), items[0][3]))
        for rel, lineno, match, _ in items[:40]:
            print("   %s:%d  %r" % (rel, lineno, match))
        if len(items) > 40:
            print("   ... and %d more" % (len(items) - 40))
    print("canon_lint: %d finding(s)" % len(findings))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
