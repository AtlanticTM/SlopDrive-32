#!/usr/bin/env python3
"""Sibling of tools/gen_registry_header.py: generate the documentation site's
registry tables and Dictionary from docs/slopsync/registry/registry.yaml.

Usage (identical to gen_registry_header.py):
    python tools/gen_docs_tables.py           # (re)write the generated pages
    python tools/gen_docs_tables.py --check   # exit 1 if any page is stale

Run with PlatformIO's bundled python (has PyYAML), or the docs venv:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools/gen_docs_tables.py

WHY THIS FILE IS A LAUNCHER AND NOT THE IMPLEMENTATION
    The generator's real home is docs-site/tools/gen_docs_tables.py, because
    docs-site/ must stay a self-contained subtree: SlopSync is designed to move
    to its own repository, and `git subtree split --prefix=docs-site` has to
    take the site AND the tool that regenerates it. A generator stranded
    outside the prefix would leave the extracted repo unable to rebuild its own
    tables — the exact staleness this tool exists to prevent.

    So the canonical script lives with the site, and this launcher keeps the
    repo-root `tools/` convention working for anyone (or any CI step) that
    expects to find it beside gen_registry_header.py. It forwards every
    argument and every exit code unchanged. Delete it freely at extraction
    time; nothing depends on it.

    The registry's location is configured ONCE, in docs-site/site.config.yml.
    This file deliberately does not know it.
"""
import runpy
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "docs-site" / "tools" / "gen_docs_tables.py"

if not TARGET.exists():
    raise SystemExit(
        f"documentation generator not found at {TARGET}\n"
        "  The docs-site/ subtree is missing or has moved. If it was extracted\n"
        "  into its own repository, this launcher has no job left — delete it."
    )

sys.argv[0] = str(TARGET)
runpy.run_path(str(TARGET), run_name="__main__")
