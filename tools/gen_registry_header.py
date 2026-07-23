#!/usr/bin/env python3
"""Generate lib/slopsync/include/slopsync/generated/registry_constants.hpp
from docs/slopsync/registry/registry.yaml — the single source of truth.

Usage:
    python tools/gen_registry_header.py           # (re)write the header
    python tools/gen_registry_header.py --check   # exit 1 if committed header is stale

Run with PlatformIO's bundled python (has PyYAML):
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools/gen_registry_header.py

The generated header is COMMITTED (ESP32/Arduino builds must never need python).
SPEC.md rule: on any conflict, registry.yaml wins — this script is how it wins.
"""
import sys
import io
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "docs" / "slopsync" / "registry" / "registry.yaml"
OUT = ROOT / "lib" / "slopsync" / "include" / "slopsync" / "generated" / "registry_constants.hpp"


def ident(name: str) -> str:
    """Sanitize a registry name into a C++ identifier (dashes -> underscores)."""
    return name.replace("-", "_")


def gen(reg: dict) -> str:
    w = io.StringIO()
    p = w.write
    meta = reg["meta"]

    p("// ============================================================================\n")
    p("// GENERATED FILE — DO NOT EDIT.\n")
    p("// Source of truth: docs/slopsync/registry/registry.yaml\n")
    p("// Regenerate:      python tools/gen_registry_header.py   (--check in CI)\n")
    p("// ============================================================================\n")
    p("#pragma once\n\n")
    p("#include <cstdint>\n#include <string_view>\n\n")
    p("namespace slopsync {\n\n")

    p(f"inline constexpr uint8_t  kProtocolVersion = {meta['protocol_version']};\n")
    p(f"inline constexpr uint8_t  kHeaderBytes     = {meta['header_bytes']};\n\n")

    # ---- Frame types --------------------------------------------------------
    p("enum class FrameType : uint8_t {\n")
    for code in sorted(reg["frame_types"]):
        e = reg["frame_types"][code]
        p(f"    {e['name']} = 0x{code:02X},  // {e['dir']}, {e['plane']}, {e['ref']}\n")
    p("};\n\n")

    # ---- Header flags -------------------------------------------------------
    p("namespace flags {\n")
    for bit in sorted(reg["header_flags"]):
        e = reg["header_flags"][bit]
        shift = int(bit.removeprefix("bit"))
        p(f"inline constexpr uint8_t {e['name']} = 1u << {shift};  // {e['ref']}\n")
    p("}  // namespace flags\n\n")

    # ---- Simple u8 enums ----------------------------------------------------
    for section, cpp in (("channel_classes", "ChannelClass"),
                         ("access_levels", "AccessLevel"),
                         ("priority_classes", "Priority"),
                         ("packed_field_types", "PackedFieldType")):
        p(f"enum class {cpp} : uint8_t {{\n")
        for code in sorted(reg[section]):
            p(f"    {ident(reg[section][code]['name'])} = {code},\n")
        p("};\n\n")

    # ---- Core channels ------------------------------------------------------
    p("namespace channels {\n")
    for cid in sorted(reg["core_channels"]):
        e = reg["core_channels"][cid]
        p(f"inline constexpr uint16_t {ident(e['name'])} = 0x{cid:04X};  // {e['class']}: {e['note']}\n")
    p("}  // namespace channels\n\n")

    # ---- CBOR keys ----------------------------------------------------------
    p("enum class CborKey : uint8_t {\n")
    for k in sorted(reg["cbor_keys"]):
        e = reg["cbor_keys"][k]
        p(f"    {ident(e['name'])} = {k},  // {e['type']}: {e['note']}\n")
    p("};\n\n")

    # ---- WELCOME limits sub-map keys (scoped key space, not global) --------
    p("namespace welcome_limits {\n")
    for k in sorted(reg["welcome_limits_keys"]):
        e = reg["welcome_limits_keys"][k]
        p(f"inline constexpr uint8_t {ident(e['name'])} = {k};  // {e['note']}\n")
    p("}  // namespace welcome_limits\n\n")

    # ---- NACK codes ---------------------------------------------------------
    p("enum class NackCode : uint16_t {\n")
    for code in sorted(reg["nack_codes"]):
        e = reg["nack_codes"][code]
        p(f"    {e['name']} = 0x{code:04X},  // {e['note']}\n")
    p("};\n\n")

    # ---- Limits -------------------------------------------------------------
    p("namespace limits {\n")
    for key in reg["limits"]:  # preserve registry order — it groups related limits
        v = reg["limits"][key]
        if isinstance(v, str):
            p(f'inline constexpr std::string_view {ident(key)} = "{v}";\n')
        else:
            p(f"inline constexpr uint32_t {ident(key)} = {v};\n")
    p("}  // namespace limits\n\n")

    p("}  // namespace slopsync\n")
    return w.getvalue()


def main() -> int:
    reg = yaml.safe_load(REGISTRY.read_text(encoding="utf-8"))
    text = gen(reg)
    if "--check" in sys.argv:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            print(f"STALE: {OUT} does not match {REGISTRY} — regenerate.", file=sys.stderr)
            return 1
        print("registry header up to date")
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
