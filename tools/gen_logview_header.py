#!/usr/bin/env python3
"""Gzip tools/logview/logview.html into src/c5_probe/logview_gz.h.

The C5 bridge mounts no filesystem, so the log viewer ships as a byte array in
flash and is served with Content-Encoding: gzip. Standalone:

    python tools/gen_logview_header.py            # regenerate
    python tools/gen_logview_header.py --check    # fail if stale (CI/lint use)

Also runs as a PlatformIO pre-script (env:c5_probe), so a build can never flash
a page older than the source file. Standalone invocation must keep working:
SSManager doctrine, .claude/rules/architecture.md section 4.
"""
import argparse
import gzip
import pathlib
import sys

# PlatformIO exec()s an extra script with no __file__ bound, so the repo root
# cannot be derived from this file's path in that context. SCons runs with the
# project directory as cwd, which is the same root.
_self = globals().get("__file__")
ROOT = pathlib.Path(_self).resolve().parent.parent if _self else pathlib.Path.cwd()
SRC = ROOT / "tools" / "logview" / "logview.html"
OUT = ROOT / "src" / "c5_probe" / "logview_gz.h"

BANNER = """// logview_gz.h -- the /logs page, gzip'd, as flash bytes. GENERATED.
//
// Constraints:
// - Do NOT hand-edit. Source is tools/logview/logview.html; regenerate with
//   tools/gen_logview_header.py (the c5_probe build runs it as a pre-script).
// - Serve with Content-Encoding: gzip. The C5 mounts no filesystem, so this
//   array is the whole delivery mechanism.
#pragma once

#include <cstddef>
#include <cstdint>

"""


def render() -> str:
    # mtime=0: a byte-identical input must produce a byte-identical header, or
    # every build dirties the tree and the staleness check means nothing.
    blob = gzip.compress(SRC.read_bytes(), compresslevel=9, mtime=0)
    rows = []
    for i in range(0, len(blob), 16):
        rows.append("    " + " ".join(f"0x{b:02x}," for b in blob[i:i + 16]))
    return (BANNER
            + "inline constexpr uint8_t kLogviewGz[] = {\n"
            + "\n".join(rows)
            + "\n};\ninline constexpr size_t kLogviewGzLen = sizeof(kLogviewGz);\n")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if the header does not match the source")
    args = ap.parse_args(argv)

    want = render()
    have = OUT.read_text(encoding="utf-8") if OUT.exists() else None
    if args.check:
        if have != want:
            print(f"STALE: {OUT.relative_to(ROOT)} does not match "
                  f"{SRC.relative_to(ROOT)}; run tools/gen_logview_header.py")
            return 1
        print("logview header up to date")
        return 0
    if have != want:
        OUT.write_text(want, encoding="utf-8")
        print(f"wrote {OUT.relative_to(ROOT)} "
              f"({SRC.stat().st_size} B html -> {len(want.splitlines()) * 16} B gz approx)")
    else:
        print("logview header up to date")
    return 0


# PlatformIO injects Import() into an extra script's globals; standalone Python
# does not have it, which is how one file serves both callers.
try:
    Import  # noqa: F821  (SCons builtin, present only under PlatformIO)
except NameError:
    if __name__ == "__main__":
        sys.exit(main())
else:
    main([])
