#!/usr/bin/env python3
"""catalog_lint.py — catch device-catalog mistakes BEFORE spending a flash cycle.

Every rule here is one that previously could only be discovered by building,
flashing, and running the live probe. Each cost real minutes at M5b:

  1. ENTRY ORDER. encodeCatalog() refuses outright — at ANY buffer size — if
     entries are not strictly ascending by channel id. The device-side symptom
     is "CATALOG DID NOT ENCODE (scratch N B)", which reads like a SIZE problem
     and sends you off growing a buffer that was never the constraint.
  2. DESC LENGTH. The registry caps `desc` at 128 bytes. Over-long prose
     encodes fine and only fails at the conformance probe.
  3. ROLE VOCABULARY (WARNING only, never a failure). A field role outside the
     registered `field_roles` set is LEGAL per the spec -- but a TYPO'D role is
     silently identical to no role at all on the wire: the client falls back to
     generic rendering and nothing anywhere complains. Deliberate escapes stay
     quiet: 'action.*' (device-suffixed by design) and device extensions inside
     a registered family ('telemetry.<custom>', 'pattern.<custom>', ...).

Run:  python tools/catalog_lint.py            (exit 1 on any finding)

This is a TEXT lint over the header, deliberately: it needs no toolchain, no
build, and runs in milliseconds, so there is no excuse not to run it. The
authoritative checks remain the compiler, the host encoder, and the live probe.

KNOWN BLIND SPOT, stated because a partial check that looks total is worse than
no check at all: only the entries added by a literal `c.addEntry({.id = ch::...`
in this header are visible here. The reserved channels contributed by the
library helpers (log_channel.hpp, safety_events_channel.hpp, trust_channels.hpp)
are NOT, so the ordering rule is verified across the device's own channels only.
That is where hand-written ordering mistakes actually happen — the helpers emit
a fixed, already-ordered block — but it does mean a clean run here is evidence,
not proof. The device's own boot-time check (initCatalog in
SlopSyncHubService.cpp) covers the full assembled list.
"""

import io
import os
import re
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(_ROOT, "include", "comms", "SlopSyncCatalog.h")
# The generated registry header is the machine-readable role vocabulary; the
# lint reads the GENERATED file (not registry.yaml) so it checks against the
# same constants the firmware compiles against. SlopSync repo split
# (slopsync.pin pins the sha): lib/slopsync is consumed from the sibling
# checkout, never vendored back into this tree.
REGISTRY_HPP = os.path.join(os.path.dirname(_ROOT), "SlopSync", "lib", "slopsync",
                            "include", "slopsync", "generated", "registry_constants.hpp")
DESC_MAX_BYTES = 128

# Matches one or more adjacent C string literals (the way long descs are written).
STRING_RUN = re.compile(r'"((?:[^"\\]|\\.)*)"')


def joined_literal(text):
    """Concatenate a run of adjacent C string literals into one value."""
    return "".join(STRING_RUN.findall(text))


def load_registered_roles():
    """Parse `namespace field_roles { ... }` out of the generated registry
    header. Returns (set_of_role_strings, {cpp_symbol: role_string}); both
    empty when the header is missing/unreadable -- the role check then reports
    itself unavailable rather than spraying false warnings."""
    try:
        src = io.open(REGISTRY_HPP, encoding="utf-8").read()
    except OSError:
        return set(), {}
    m = re.search(r"namespace\s+field_roles\s*\{(.*?)\}\s*//\s*namespace\s+field_roles",
                  src, re.S)
    if not m:
        return set(), {}
    block = m.group(1)
    by_symbol = dict(re.findall(r'std::string_view\s+(\w+)\s*=\s*"([^"]*)"', block))
    return set(by_symbol.values()), by_symbol


def edit_distance(a, b):
    """Plain Levenshtein -- small vocabulary, run once per unknown role."""
    if a == b:
        return 0
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def nearest_role(role, registered):
    return min(registered, key=lambda r: edit_distance(role, r)) if registered else None


def field_name_before(src, pos):
    """The `.name = \"...\"` nearest ABOVE a `.role` match -- textually, the
    field the role annotates."""
    names = list(re.finditer(r'\.name\s*=\s*"((?:[^"\\]|\\.)*)"', src[:pos]))
    return names[-1].group(1) if names else "?"


def main():
    src = io.open(HEADER, encoding="utf-8").read()
    findings = []

    # ---- Rule 1: entries strictly ascending by id --------------------------
    # Resolve `ch::name` -> value from the namespace block.
    ids = dict(re.findall(r"inline constexpr uint16_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", src))
    ids = {k: int(v, 16) for k, v in ids.items()}

    # RFC-047 (Phase C2): the RFC-047 grid sorts by (class, domain, slot), not
    # by original allocation order, so `c.addEntry({.id = ch::X, ...})`
    # AUTHORING order no longer equals WIRE order. Each entry's build logic
    # (addEntry + its fields) is wrapped in an `auto addX = [&]() { ... };`
    # lambda, and ONE ordered call sequence at the end of the function is now
    # the thing that must ascend -- see buildSlopDriveCatalog()'s own comment
    # at that call sequence. Two source shapes both count as "an entry":
    #   auto addFoo = [&]() { ... c.addEntry({.id = ch::X, ...}); ... };
    #     ... later ...    addFoo();
    #   addApModifierChannel(ch::X, "wire-name", "group", keyBase);
    # A catalog with no lambda wrapping at all (every c.addEntry() call is
    # unconditional top-level code, no `auto add* = [&]` anywhere) falls back
    # to the plain textual-order check this rule used before Phase C2.
    lambda_defs = list(re.finditer(r"auto\s+(\w+)\s*=\s*\[&\]\([^)]*\)\s*\{", src))

    if not lambda_defs:
        order = []
        for m in re.finditer(r"c\.addEntry\(\{\.id\s*=\s*([\w:]+)\s*,", src):
            sym = m.group(1)
            short = sym.split("::")[-1]
            if short in ids:
                order.append((sym, ids[short], src.count("\n", 0, m.start()) + 1))
    else:
        # id declared by each lambda: the FIRST `.id = ch::X` (or the entry-
        # authoring `if (feat...) { c.addEntry(...) }` shape) after its
        # opening brace. A bounded forward window, not full brace-matching --
        # every wrapped lambda's own addEntry is its first real statement.
        lambda_id = {}
        for i, m in enumerate(lambda_defs):
            window_end = lambda_defs[i + 1].start() if i + 1 < len(lambda_defs) else len(src)
            window = src[m.end():window_end]
            idm = re.search(r"\.id\s*=\s*ch::(\w+)\s*,", window)
            if idm and idm.group(1) in ids:
                lambda_id[m.group(1)] = ids[idm.group(1)]

        # The ordered invocation sequence: bare `addFoo();` calls plus the six
        # `addApModifierChannel(ch::X, ...)` calls, wherever they appear AFTER
        # every lambda has been defined (the call sequence's own home).
        last_def_end = lambda_defs[-1].end()
        tail = src[last_def_end:]
        tail_line0 = src.count("\n", 0, last_def_end)
        order = []
        for m in re.finditer(r"(\w+)\(\s*\)\s*;|addApModifierChannel\(\s*ch::(\w+)", tail):
            line = tail_line0 + tail.count("\n", 0, m.start()) + 1
            if m.group(2):  # addApModifierChannel(ch::SYMBOL, ...)
                if m.group(2) in ids:
                    order.append(("ch::" + m.group(2), ids[m.group(2)], line))
                continue
            name = m.group(1)
            if name in lambda_id:
                order.append((name + "()", lambda_id[name], line))

    for i in range(1, len(order)):
        (psym, pid, _), (sym, cid, line) = order[i - 1], order[i]
        if cid <= pid:
            findings.append(
                "line %d: entry order — %s (0x%04X) follows %s (0x%04X). "
                "encodeCatalog() refuses the WHOLE catalog; entries must ascend by id "
                "IN INVOCATION ORDER (not source-declaration order, once lambda-wrapped)."
                % (line, sym, cid, psym, pid))

    # ---- Rule 2: desc within the registry cap ------------------------------
    for m in re.finditer(r'\.desc\s*=\s*((?:\s*"(?:[^"\\]|\\.)*")+)', src):
        text = joined_literal(m.group(1))
        n = len(text.encode("utf-8"))
        if n > DESC_MAX_BYTES:
            line = src.count("\n", 0, m.start()) + 1
            findings.append('line %d: desc is %d B, over the %d B registry cap: "%s..."'
                            % (line, n, DESC_MAX_BYTES, text[:50]))

    # ---- Rule 3: role vocabulary (WARNING only) ----------------------------
    # A `.role` is either `roles::symbol` (resolved against the generated
    # header -- a symbol that resolves IS registered, the compiler already
    # guarantees it) or a string literal, which is where typos live: an
    # unregistered role is legal on the wire and renders generically, so the
    # only place a misspelling can ever be caught is here.
    warnings = []
    registered, by_symbol = load_registered_roles()
    families = {r.split(".", 1)[0] for r in registered}
    role_count = 0
    if not registered:
        warnings.append("role vocabulary unavailable (%s missing or has no "
                        "field_roles block) -- role check skipped" % REGISTRY_HPP)
    else:
        for m in re.finditer(
                r'\.role\s*=\s*(?:(?:\w+::)*(\w+)|((?:\s*"(?:[^"\\]|\\.)*")+))', src):
            line = src.count("\n", 0, m.start()) + 1
            sym, lit = m.group(1), m.group(2)
            if sym is not None:
                role_count += 1
                if sym not in by_symbol:
                    # Would not compile if truly absent; flag so a rename in the
                    # generated header is noticed here before a build cycle.
                    warnings.append("line %d: role symbol `%s` not found in the generated "
                                    "field_roles block" % (line, sym))
                continue
            role = joined_literal(lit)
            if not role:
                continue                      # empty string = deliberate no-role
            role_count += 1
            if role in registered:
                continue
            if role.startswith("action."):
                continue                      # device-suffixed by design
            if "." in role and role.split(".", 1)[0] in families:
                continue                      # deliberate device namespace in a registered family
            near = nearest_role(role, registered)
            warnings.append('line %d: field "%s" role "%s" is not a registered field_role '
                            '(nearest: "%s") -- an unregistered role is LEGAL but renders '
                            "generically; if this is a typo it is silently identical to no "
                            "role at all" % (line, field_name_before(src, m.start()), role, near))

    print("catalog_lint: %d entries, %d desc annotations, %d role annotations checked"
          % (len(order), len(re.findall(r"\.desc\s*=", src)), role_count))
    for wmsg in warnings:
        print("  WARN " + wmsg)
    if not findings:
        print("OK — no findings" + (" (%d warning(s))" % len(warnings) if warnings else ""))
        return 0
    for f in findings:
        print("  FAIL " + f)
    return 1


if __name__ == "__main__":
    sys.exit(main())
