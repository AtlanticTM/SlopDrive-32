"""Fetch source fonts and subset them for the SlopDrive-32 web UI.

This is a one-shot build helper, NOT shipped to the device. It downloads the
upstream fonts and produces WOFF2 subsets in this directory.

Subset character set (per SD32-overhaul-plan.md Task 1):
  digits, A-Z a-z, and the punctuation  .·–—%°+-:/

Variable-axis retention is mandatory for Martian Mono (wght 100-800, wdth
75-112.5). pyftsubset retains axes by default when --layout-features='*' is
passed and no --drop-tables removes fvar/gvar/HVAR. We verify the fvar table
survives after subsetting.
"""
import sys
import urllib.request
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Chakra Petch: Google Fonts serves static 400/500 TTFs (not a variable font
# in the repo). Martian Mono: we want the FULL variable font with both wght
# and wdth axes. The gstatic latin woff2 pins font-stretch:100% which suggests
# wdth may be instanced out, so we pull the source variable TTF from the
# Google Fonts GitHub repo instead.
SOURCES = {
    "ChakraPetch-400.ttf": (
        "https://fonts.gstatic.com/s/chakrapetch/v13/cIf6MapbsEk7TDLdtEz1BwkmmA.ttf"
    ),
    "ChakraPetch-500.ttf": (
        "https://fonts.gstatic.com/s/chakrapetch/v13/cIflMapbsEk7TDLdtEz1BwkebIlFQA.ttf"
    ),
    "MartianMono[wght,wdth].ttf": (
        "https://github.com/google/fonts/raw/main/ofl/martianmono/MartianMono%5Bwdth%2Cwght%5D.ttf"
    ),
}

# Subset glyphs: digits, ASCII letters, and the punctuation set required by
# the brief. Space included so words render. Use explicit U+ notation with
# comma separators so pyftsubset's range parser doesn't misread the bare "-"
# in "+-:/" as a range bound.
UNICODES = ",".join([
    "U+0030-0039",  # 0-9
    "U+0041-005A",  # A-Z
    "U+0061-007A",  # a-z
    "U+0020",       # space
    "U+002E",       # .
    "U+00B7",       # middle dot ·
    "U+2013",       # en dash –
    "U+2014",       # em dash —
    "U+0025",       # %
    "U+00B0",       # degree °
    "U+002B",       # +
    "U+002D",       # -
    "U+003A",       # :
    "U+002F",       # /
])


def fetch(name: str, url: str) -> Path:
    dst = HERE / name
    if dst.exists() and dst.stat().st_size > 10000:
        print(f"[fonts] {name} already present ({dst.stat().st_size} bytes)")
        return dst
    print(f"[fonts] downloading {name} from {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=120) as r, open(dst, "wb") as f:
        f.write(r.read())
    print(f"[fonts] saved {dst} ({dst.stat().st_size} bytes)")
    return dst


def subset(src: Path, out: Path):
    """Run pyftsubset retaining all variable axes (fvar/gvar/HVAR kept)."""
    args = [
        sys.executable, "-m", "fontTools.subset",
        str(src),
        "--output-file=" + str(out),
        "--flavor=woff2",
        "--unicodes=" + UNICODES,
        "--layout-features=*",
        "--no-hinting",
        "--desubroutinize",
        "--drop-tables+=DSIG",
    ]
    print("[fonts] pyftsubset " + src.name)
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        raise SystemExit(f"[fonts] pyftsubset failed for {src}")
    print(r.stdout)
    print(f"[fonts] wrote {out} ({out.stat().st_size} bytes)")


def verify_axes(woff2: Path, expect_axes: list):
    """Confirm fvar table and expected axes survived subsetting."""
    from fontTools.ttLib import TTFont

    tt = TTFont(woff2)
    if "fvar" not in tt:
        raise SystemExit(f"[fonts][VERIFY] FAIL: {woff2} has no fvar table — axes stripped!")
    axes = [a.axisTag for a in tt["fvar"].axes]
    missing = [a for a in expect_axes if a not in axes]
    if missing:
        raise SystemExit(f"[fonts][VERIFY] FAIL: {woff2} missing axes {missing}; has {axes}")
    print(f"[fonts][VERIFY] OK: {woff2.name} retains axes {axes}")
    if "gvar" not in tt:
        print(f"[fonts][VERIFY] WARNING: {woff2.name} has fvar but no gvar — variation may not render")
    else:
        print(f"[fonts][VERIFY] OK: {woff2.name} retains gvar ({len(tt['gvar'].variations)} glyphs)")


def merge_chakra_weights():
    """Merge the static 400 and 500 TTFs into a single variable-style family
    is overkill; instead we ship two separate @font-face entries (400 and 500)
    pointing at two subsetted woff2 files. Simpler and matches the brief."""
    pass


def main():
    for name, url in SOURCES.items():
        fetch(name, url)

    # Chakra Petch: two static weights, subsetted separately.
    subset(HERE / "ChakraPetch-400.ttf", HERE / "ChakraPetch-400.woff2")
    subset(HERE / "ChakraPetch-500.ttf", HERE / "ChakraPetch-500.woff2")

    # Martian Mono: variable, both wght and wdth axes MUST survive.
    subset(HERE / "MartianMono[wght,wdth].ttf", HERE / "MartianMono.woff2")
    verify_axes(HERE / "MartianMono.woff2", ["wght", "wdth"])

    print("[fonts] done")


if __name__ == "__main__":
    main()