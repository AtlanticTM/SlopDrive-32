#!/usr/bin/env python3
"""Emit compile_commands.json for first-party firmware sources.

PlatformIO's own `-t compiledb` target does NOT work for this project: with
framework=espidf it walks only the CMake/IDF translation units and emits zero
entries for src/, in every env. src/ is compiled by PlatformIO's SCons layer
instead. Without entries clangd parses our headers as C and every declaration
inside namespace slopdrive goes invisible to symbol search while grep keeps
working, so the failure is silent. See .claude/rules/serena.md.

This reads `pio run -t idedata`, which reports the exact driver, flags,
defines and include roots the real build used, and writes one entry per
first-party TU. Headers are not TUs; clangd infers their command from the
nearest entry, which is why src/ coverage is enough to fix include/ too.
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIO = os.path.expandvars(r"%USERPROFILE%\.platformio\penv\Scripts\platformio.exe")
DEFAULT_ENV = "sd32-ota"
SRC_DIRS = ("src",)
SRC_EXT = (".cpp", ".cc", ".c")

# GCC-only flags clang rejects outright. The .clangd Remove: list covers these
# for entries clangd reads, but stripping here keeps the db usable by any tool.
DROP_PREFIXES = (
    "-mlongcalls", "-mdisable-hardware-atomics", "-mtext-section-literals",
    "-mtarget-align", "-mno-target-align", "-fno-tree-switch-conversion",
    "-fstrict-volatile-bitfields", "-free", "-fipa-pta", "-fno-shrink-wrap",
    "-fno-malloc-dce", "-fzero-init-padding-bits", "-fmacro-prefix-map",
)


def idedata(env):
    out = subprocess.run(
        [PIO, "run", "-t", "idedata", "-e", env],
        cwd=ROOT, capture_output=True, text=True, timeout=600,
    ).stdout
    best = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if "cxx_path" in obj and (best is None or len(line) > best[0]):
                best = (len(line), obj)
    if best is None:
        sys.exit("idedata produced no JSON; run the build once and retry")
    return best[1]


def flags(d):
    keep = [f for f in d.get("cxx_flags", [])
            if not any(f.startswith(p) for p in DROP_PREFIXES)]
    inc = []
    for group in d.get("includes", {}).values():
        inc.extend(group)
    seen, roots = set(), []
    for p in inc:
        if p not in seen:
            seen.add(p)
            roots.append("-I" + p.replace("\\", "/"))
    return keep + ["-D" + x for x in d.get("defines", [])] + roots


def sources():
    for top in SRC_DIRS:
        for base, _, files in os.walk(os.path.join(ROOT, top)):
            for f in files:
                if f.endswith(SRC_EXT):
                    yield os.path.join(base, f)


def main():
    env = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_ENV
    d = idedata(env)
    common = flags(d)
    cxx = d["cxx_path"].replace("\\", "/")
    db = []
    for path in sources():
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        db.append({
            "directory": ROOT.replace("\\", "/"),
            "file": path.replace("\\", "/"),
            "arguments": [cxx] + common + ["-c", rel],
        })
    dest = os.path.join(ROOT, "compile_commands.json")
    with open(dest, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(db, fh, indent=1)
    print("wrote %d entries to compile_commands.json (env %s)" % (len(db), env))
    print("verify: get_symbols_overview on a header under include/comms/")
    print("        must report kind Namespace for slopdrive, not Variable")


if __name__ == "__main__":
    main()
