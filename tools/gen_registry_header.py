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


# Registry names become C++ identifiers, so they may not collide with a
# keyword. Caught here with a pointed message rather than 200 lines later as
# "expected unqualified-id before 'namespace'".
CPP_KEYWORDS = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch",
    "char", "class", "concept", "const", "consteval", "constexpr", "constinit",
    "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
    "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
    "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
    "not", "nullptr", "operator", "or", "private", "protected", "public",
    "register", "reinterpret_cast", "requires", "return", "short", "signed",
    "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
    "template", "this", "thread_local", "throw", "true", "try", "typedef",
    "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
    "volatile", "wchar_t", "while", "xor",
}


def ident(name: str) -> str:
    """Sanitize a registry name into a C++ identifier.

    Dashes and dots both become underscores: `session-roster` -> session_roster,
    `limit.user.speed` -> limit_user_speed (field_roles are dotted tstr values).
    """
    out = name.replace("-", "_").replace(".", "_")
    if out in CPP_KEYWORDS:
        raise SystemExit(
            f"registry name '{name}' generates the C++ keyword '{out}' — "
            f"pick a different name in registry.yaml (the wire number is "
            f"unaffected; only the generated identifier changes)."
        )
    return out


def esc(text: str) -> str:
    """Escape a registry string for a C++ string literal."""
    return text.replace("\\", "\\\\").replace('"', '\\"')


def emit_bits(p, section: dict, ns: str) -> None:
    """Emit a bit-flag section (`bitN: {name, note|ref}`) as a namespace."""
    p(f"namespace {ns} {{\n")
    for bit in sorted(section, key=lambda b: int(b.removeprefix("bit"))):
        e = section[bit]
        shift = int(bit.removeprefix("bit"))
        why = e.get("ref") or e.get("note", "")
        p(f"inline constexpr uint8_t {ident(e['name'])} = 1u << {shift};  // {why}\n")
    p(f"}}  // namespace {ns}\n\n")


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
    emit_bits(p, reg["header_flags"], "flags")

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

    # ---- Scoped sub-map key spaces (not the global cbor_keys space) --------
    #      ...plus the small u8 enums that are values inside those spaces.
    for section, ns in (("welcome_limits_keys", "welcome_limits"),
                        ("probe_result_keys", "probe_result"),
                        ("identity_keys", "identity"),
                        ("blob_keys", "blob"),
                        ("trust_keys", "trust"),
                        ("trust_ledger_keys", "trust_ledger"),
                        ("trust_states", "trust_states"),
                        ("presentation_modes", "presentation_modes"),
                        ("blob_namespaces", "blob_ns"),
                        ("session_event_kinds", "session_events"),
                        ("log_event_kinds", "log_events"),
                        ("pairing_event_kinds", "pairing_events"),
                        ("safety_event_kinds", "safety_events"),
                        ("log_levels", "log_levels"),
                        ("safety_intent_ops", "safety_ops"),
                        ("session_admin_ops", "session_admin_ops"),
                        ("safety_causes", "safety_causes"),
                        ("setting_categories", "setting_categories"),
                        ("stream_kinds", "stream_kinds"),
                        ("procedure_phases", "procedure_phases"),
                        ("curve_families", "curve_families")):
        p(f"namespace {ns} {{\n")
        for k in sorted(reg[section]):
            e = reg[section][k]
            p(f"inline constexpr uint8_t {ident(e['name'])} = {k};  // {e['note']}\n")
        p(f"}}  // namespace {ns}\n\n")

    # ---- Bit-flag spaces ----------------------------------------------------
    emit_bits(p, reg["setting_flags"], "setting_flags")
    emit_bits(p, reg["pairing_modes"], "pairing_modes")

    # ---- Field roles --------------------------------------------------------
    # These are TSTR values on the wire (dotted namespace, device-extensible),
    # not an integer enum — RFC-019's `action.<name>` roles carry a
    # device-chosen suffix that no enum can express.
    p("namespace field_roles {\n")
    for role in reg["field_roles"]:  # registry order
        e = reg["field_roles"][role] or {}
        p(f'inline constexpr std::string_view {ident(role)} = "{esc(role)}";  // {e.get("note", "")}\n')
    p("}  // namespace field_roles\n\n")

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
