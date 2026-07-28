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
# American English only. Stem-based: each family below generates its
# matchable British forms from a stem list plus a CLOSED set of real
# inflectional suffixes -- never a bare wildcard -- so a word that merely
# shares a prefix with a stem (organism, capitalism, modernism, optimism,
# realism, initialism, specialism, apologist, catalyst, analysis,
# paralysis, catalysis, emphasis, sombrero, cancellation) can never match.
# "analyses"/"paralyses"/"catalyses" are the one genuine ambiguity (Greek-
# plural noun, identical in both dialects, vs. the British 3rd-person-
# singular verb spelling) -- matched only when followed by a determiner
# that marks it as a verb-with-object, same heuristic as before.
def _build_british_regex():
    forms = set()

    # -our -> -or (hour/four/your/pour/sour/tour/contour/velour/glamour/
    # devour/flour/paramour/troubadour are real English, never in this set)
    our_stems = (
        "favour", "behaviour", "colour", "honour", "labour", "neighbour",
        "flavour", "armour", "harbour", "humour", "rumour", "saviour",
        "vapour", "endeavour", "rigour", "vigour", "valour", "clamour",
        "odour", "parlour", "splendour", "tumour", "candour", "ardour",
        "fervour", "demeanour",
    )
    our_suffixes = ("", "s", "ed", "ing", "er", "ers", "ite", "ites",
                    "able", "ably", "ful", "fully", "less", "y")
    for stem in our_stems:
        for sfx in our_suffixes:
            forms.add(stem + sfx)
    forms.update(("savour", "savours", "savoured", "savouring",
                  "savoury", "savouries"))

    # -ise/-isation -> -ize/-ization, -yse -> -yze. Built from the verb
    # ROOT (stem minus trailing "e") plus a closed suffix template, so
    # "organism"/"capitalism"/"analysis"/"emphasis"/"catalyst" etc. --
    # which merely share a prefix -- are structurally excluded, not
    # exception-listed.
    ise_stems = (
        "organise", "realise", "recognise", "initialise", "serialise",
        "synchronise", "customise", "minimise", "maximise", "optimise",
        "normalise", "utilise", "categorise", "prioritise", "summarise",
        "authorise", "standardise", "stabilise", "finalise", "generalise",
        "specialise", "visualise", "randomise", "sanitise", "capitalise",
        "centralise", "equalise", "italicise", "memorialise", "modernise",
        "neutralise", "penalise", "personalise", "publicise", "quantise",
        "localise", "tokenise", "emphasise", "apologise", "harmonise",
    )
    for stem in ise_stems:
        root = stem[:-1]  # "organise" -> "organis"
        # NOTE: no bare "root" (empty suffix) form -- for stems like
        # "emphasise" the bare root ("emphasis") IS the real English noun
        # and must never match. The bare verb comes from `stem` itself
        # (below), which still ends in "e".
        for sfx in ("es", "ed", "ing", "er", "ers", "ation", "ations", "able"):
            forms.add(root + sfx)
        forms.add(stem)  # bare verb, e.g. "organise"

    yse_stems = ("analyse", "paralyse", "catalyse")
    for stem in yse_stems:
        root = stem[:-1]  # "analys"
        for sfx in ("", "ed", "ing", "er", "ers"):
            forms.add(root + sfx)
        forms.add(stem)
        # deliberately no "-es" form: "analyses"/"paralyses"/"catalyses"
        # are the ambiguous Greek-plural noun, handled below instead.

    # -re -> -er (mere/acre/genre/mediocre/massacre/ogre/timbre/cadre/
    # sombrero are real English and structurally excluded: bare/plural/
    # past suffixes only, never \w*)
    re_stems = (
        "centre", "metre", "litre", "fibre", "calibre", "theatre",
        "sombre", "spectre", "lustre", "manoeuvre", "sceptre",
        "centimetre", "millimetre", "kilometre",
    )
    for stem in re_stems:
        for sfx in ("", "s", "d"):
            forms.add(stem + sfx)

    # -ogue -> house style -og (this project says "catalog" hundreds of times)
    forms.update((
        "catalogue", "catalogues", "catalogued", "cataloguing", "cataloguer",
        "analogue", "analogues",
        "dialogue", "dialogues", "dialogued", "dialoguing",
        "epilogue", "epilogues",
        "prologue", "prologues",
    ))

    # doubled-L inflections -> single-L (cancellation is correct US, both
    # sides double the L -- excluded by simply never being in this list)
    forms.update((
        "travelled", "travelling", "traveller", "travellers",
        "labelled", "labelling",
        "modelled", "modelling",
        "cancelled", "cancelling",
        "levelled", "levelling",
        "signalled", "signalling",
        "channelled", "channelling",
        "marshalled", "marshalling",
        "totalled", "totalling",
        "equalled", "equalling",
        "fuelled", "fuelling",
        "dialled", "dialling",
        "rivalled", "rivalling",
        "funnelled", "funnelling",
        "tunnelled", "tunnelling",
        "panelled", "panelling",
        "quarrelled", "quarrelling",
        "counselled", "counselling", "counsellor", "counsellors",
        "spiralled", "spiralling",
    ))

    # misc singles (doughnut/glamour: accept both spellings, never listed)
    forms.update((
        "grey", "greys", "greyed", "greying", "greyscale",
        "judgement", "judgements",
        "acknowledgement", "acknowledgements",
        "aluminium",
        "artefact", "artefacts",
        "licence", "licences",
        "defence", "defences",
        "offence", "offences",
        "pretence", "pretences",
        "practise", "practises", "practised", "practising",
        "programme", "programmes",
        "tyre", "tyres",
        "kerb", "kerbs",
        "mould", "moulds", "moulded", "moulding", "mouldy",
        "smoulder", "smoulders", "smouldered", "smouldering",
        "whilst", "amongst", "amidst",
        "learnt", "spelt", "dreamt",
        "storey", "storeys",
        "sceptical", "scepticism",
        "enquire", "enquires", "enquired", "enquiring", "enquiry", "enquiries",
        "fulfil", "fulfils", "fulfilment",
        "skilful", "skilfully",
        "wilful", "wilfully",
        "enrol", "enrols", "enrolment", "enrolments",
        "distil", "distils", "distilment",
        "jewellery",
        "plough", "ploughs", "ploughed", "ploughing",
        "draught", "draughts",
        "behaviourally",
        "speciality", "specialities",
        "cosy", "cosier", "cosiest",
        "snigger", "sniggers", "sniggered", "sniggering",
        "aeroplane", "aeroplanes",
        "cheque", "cheques",
        "gaol", "gaols",
        "moustache", "moustaches",
        "pyjamas",
        "furore",
    ))

    literal_rx = r"\b(?:%s)\b" % "|".join(sorted(forms, key=len, reverse=True))
    # Ambiguous Greek-plural/British-verb collision: only flag when a
    # determiner+object follows, marking it as a verb use ("it analyses
    # the data"), never the bare plural noun ("risk analyses").
    ambiguous_rx = (
        r"\b(?:analyses|paralyses|catalyses)"
        r"(?=\s+(?:the|a|an|this|that|each|every|it|them)\b)"
    )
    return re.compile(literal_rx + "|" + ambiguous_rx, re.IGNORECASE)


BRITISH_RX = _build_british_regex()

# Pre-existing British spellings living in code comments/strings, surfaced
# for the first time by the 2026-07-28 dictionary upgrade (a docs-only
# pass -- rewording code comments was explicitly out of scope for it, see
# the pass's own commit). Tracked by exact line, not silently dropped
# (CANON C-3: a conflict gets flagged, never silently ignored) -- a NEW
# British spelling anywhere else, including new lines in these same files,
# still fails the gate. Remove an entry only by fixing the spelling there.
BRITISH_SPELLING_KNOWN_CODE_HITS = {
    ("include/comms/SlopSyncAsyncWsTransport.h", 48),
    ("include/comms/SlopSyncCatalog.h", 1434),
    ("include/comms/SlopSyncCatalog.h", 1452),
    ("sim/slopsim/src/machine/MachineSim.h", 784),
    ("src/comms/SlopSyncHubService.cpp", 1952),
    ("webui/src/model/format.js", 36),
    ("webui/src/ui/SafetyBar.svelte", 28),
}

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
    dict(
        name="british-spelling",
        msg="British spelling (CANON C-11: American English only)",
        rx=BRITISH_RX,
        include=("",),  # every tracked text file
        exempt=("THIRD_PARTY_LICENSES.md",  # legal texts verbatim
                "LICENSE", "NOTICE",
                "tools/canon_lint.py"),     # this file quotes the banned words
    ),
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
                if (c["name"] == "british-spelling"
                        and (rel, lineno) in BRITISH_SPELLING_KNOWN_CODE_HITS):
                    continue
                m = c["rx"].search(line)
                if m:
                    findings.append((c["name"], rel, lineno, m.group(0), c["msg"]))
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
    findings = run_grep_checks() + run_pin_check()
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
