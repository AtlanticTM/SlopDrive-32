#!/usr/bin/env python3
"""Gzip webui/dist/index.html into src/c5_probe/webui_gz.h.

The S3 is headless and serves nothing, so the bridge carries the UI. The C5
mounts no filesystem: the bundle ships as a byte array in flash, served with
Content-Encoding: gzip and the strong ETag emitted here. Standalone:

    python tools/gen_webui_header.py            # regenerate
    python tools/gen_webui_header.py --check    # fail if stale (CI/lint use)

Also runs as a PlatformIO pre-script (env:c5_probe). A missing or unbuilt
webui/dist is a HARD FAILURE in both modes: baking an empty page would flash a
bridge that serves a blank UI and reports success. Standalone invocation must
keep working: SSManager doctrine, .claude/rules/architecture.md section 4.
"""
import argparse
import gzip
import hashlib
import pathlib
import sys

# PlatformIO exec()s an extra script with no __file__ bound, so the repo root
# cannot be derived from this file's path in that context. SCons runs with the
# project directory as cwd, which is the same root.
_self = globals().get("__file__")
ROOT = pathlib.Path(_self).resolve().parent.parent if _self else pathlib.Path.cwd()
SRC = ROOT / "webui" / "dist" / "index.html"
OUT = ROOT / "src" / "c5_probe" / "webui_gz.h"

BANNER = """// webui_gz.h -- the WebUI bundle, gzip'd, as flash bytes. GENERATED.
//
// Constraints:
// - Do NOT hand-edit. Source is webui/dist/index.html; regenerate with
//   tools/gen_webui_header.py (the c5_probe build runs it as a pre-script).
// - Serve with Content-Encoding: gzip. The C5 mounts no filesystem, so this
//   array is the whole delivery mechanism.
// - kWebuiEtag is a STRONG ETag over these exact bytes, quotes included: a
//   firmware carrying a different bundle cannot serve a stale cached page.
#pragma once

#include <cstddef>
#include <cstdint>

"""


def render() -> str:
    if not SRC.exists():
        print(f"FATAL: {SRC} is missing. Build the UI first "
              f"(pio run -e sd32-ota, which runs build_webui.py, or "
              f"npm run build in webui/). Refusing to bake an empty page.",
              file=sys.stderr)
        sys.exit(1)
    # mtime=0: a byte-identical input must produce a byte-identical header, or
    # every build dirties the tree and the staleness check means nothing.
    blob = gzip.compress(SRC.read_bytes(), compresslevel=9, mtime=0)
    etag = hashlib.sha256(blob).hexdigest()[:16]
    rows = []
    for i in range(0, len(blob), 16):
        rows.append("    " + " ".join(f"0x{b:02x}," for b in blob[i:i + 16]))
    return (BANNER
            + "inline constexpr uint8_t kWebuiGz[] = {\n"
            + "\n".join(rows)
            + "\n};\ninline constexpr size_t kWebuiGzLen = sizeof(kWebuiGz);\n"
            + f'inline constexpr char kWebuiEtag[] = "\\"{etag}\\"";\n')


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
                  f"{SRC.relative_to(ROOT)}; run tools/gen_webui_header.py")
            return 1
        print("webui header up to date")
        return 0
    if have != want:
        OUT.write_text(want, encoding="utf-8")
        print(f"wrote {OUT.relative_to(ROOT)} "
              f"({SRC.stat().st_size} B html -> {len(want)} B header)")
    else:
        print("webui header up to date")
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
