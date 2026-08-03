#!/usr/bin/env bash
# spec-drift.sh: diff this repo's observed protocol reality against the
# sibling SlopSync checkout (pinned by slopsync.pin). Output is the RFC
# backlog: every divergence is a candidate RFC, grouped by class. Flow is
# dev -> RFC -> ruling -> spec+registry -> pin bump; never edit the sibling
# to match this code. Companion doc: .claude/skills/drift/SKILL.md.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIB="$ROOT/../SlopSync"
FINDINGS=0

say() { printf '%s\n' "$*"; }
finding() { FINDINGS=$((FINDINGS + 1)); printf '  !! %s\n' "$*"; }

say "=== spec-drift: SlopDrive-32 vs sibling SlopSync ==="
say ""

# ---------------------------------------------------------------- PIN DRIFT
say "[PIN DRIFT]"
if [ ! -d "$SIB/.git" ]; then
  finding "sibling checkout not found at $SIB; every other check is meaningless"
  exit 1
fi
PIN=$(tr -d ' \r\n' < "$ROOT/slopsync.pin" 2>/dev/null || true)
HEAD=$(git -C "$SIB" rev-parse HEAD 2>/dev/null || true)
if [ -z "$PIN" ]; then
  finding "slopsync.pin missing or empty"
elif [ "$PIN" != "$HEAD" ]; then
  finding "pin $PIN != sibling HEAD $HEAD (candidate: pin bump after upstream review)"
else
  say "  ok: pin matches sibling HEAD ($PIN)"
fi
DIRTY=$(git -C "$SIB" status --porcelain 2>/dev/null | wc -l | tr -d ' ')
if [ "$DIRTY" != "0" ]; then
  finding "sibling working tree has $DIRTY uncommitted change(s); builds may not match the pin"
  git -C "$SIB" status --porcelain | sed 's/^/     /'
else
  say "  ok: sibling working tree clean"
fi
say ""

# ---------------------------------------------------------- STALE GENERATED
say "[STALE GENERATED]"
if (cd "$SIB" && python tools/gen_registry_header.py --check >/dev/null 2>&1); then
  say "  ok: registry codegen outputs match registry.yaml"
else
  finding "generated headers stale vs registry.yaml; run: (cd ../SlopSync && python tools/gen_registry_header.py)"
fi
say ""

# ------------------------------------------- HAND-TRANSCRIPTION + the rest
# Value-level checks need light parsing; python owns them. Registry sections
# udp_discovery / ble_identity / ble_adv_flags are NOT emitted by codegen
# (verified 2026-08-03), so this repo hand-copies them: the TRAPS T20 class,
# zero compiler symptom on drift. This is the highest-value check here.
python - "$ROOT" "$SIB" <<'PY'
# ponytail: regex extraction against known file shapes, not a C++ parser.
# Upgrade path if the shapes churn: emit these sections from codegen instead
# (the open "third target" intent in SlopSync registry-workflow skill).
import re, sys, pathlib

root, sib = map(pathlib.Path, sys.argv[1:3])
findings = 0

def say(msg): print(msg)
def finding(msg):
    global findings
    findings += 1
    print(f"  !! {msg}")

try:
    import yaml
    reg = yaml.safe_load((sib / "spec/registry/registry.yaml").read_text(encoding="utf-8"))
except Exception as e:
    print(f"  !! cannot parse registry.yaml ({e}); value checks skipped")
    sys.exit(1)

def grab(path, pattern, cast=str):
    text = (root / path).read_text(encoding="utf-8", errors="replace")
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else None

say("[HAND-TRANSCRIPTION (TRAPS T20 class)]")
disco = "include/comms/SlopSyncDiscoveryWire.h"
udp = reg.get("udp_discovery") or {}
port_code = grab(disco, r"kPort\s*=\s*(\d+)", int)
if port_code is None:
    finding(f"could not locate kPort in {disco}; file shape changed, update this check")
elif port_code != udp.get("port"):
    finding(f"udp port: code {port_code} vs registry {udp.get('port')} ({disco})")
else:
    say(f"  ok: udp_discovery.port {port_code}")

magic_code = grab(disco, r"kMagic\s*(?:\[4\]|=)?[^={]*=\s*\{([^}]*)\}")
magic_reg = udp.get("magic")
if magic_code and magic_reg:
    got = "".join(chr(int(b, 16)) for b in re.findall(r"0x([0-9A-Fa-f]{2})", magic_code))
    if got != magic_reg:
        finding(f"udp magic: code '{got}' vs registry '{magic_reg}' ({disco})")
    else:
        say(f"  ok: udp_discovery.magic '{got}'")
else:
    finding(f"could not compare udp magic (code={bool(magic_code)}, registry={bool(magic_reg)})")

adv = reg.get("ble_adv_flags") or {}
for bit, meta in adv.items():
    n = int(bit.replace("bit", ""))
    name = meta.get("name", "")
    kname = "kFlag" + "".join(w.capitalize() for w in name.split("_"))
    val = grab(disco, kname + r"\s*=\s*0x([0-9A-Fa-f]+)", lambda s: int(s, 16))
    if val is None:
        finding(f"ble_adv_flags {name}: no {kname} in {disco} (new flag the code does not carry?)")
    elif val != (1 << n):
        finding(f"ble_adv_flags {name}: code 0x{val:02X} vs registry bit{n} (0x{1 << n:02X})")
    else:
        say(f"  ok: ble_adv_flags.{name} = bit{n}")

ble_h = "include/comms/SlopSyncBleTransport.h"
ident = reg.get("ble_identity") or {}
pairs = [("service_uuid", "kBleServiceUuid"), ("write_char_uuid", "kBleWriteCharUuid"),
         ("notify_char_uuid", "kBleNotifyCharUuid")]
for rkey, cname in pairs:
    want = ident.get(rkey)
    got = grab(ble_h, cname + r"\s*(?:\[\])?\s*=\s*\"([^\"]+)\"")
    if want is None:
        continue
    if got is None:
        finding(f"ble_identity.{rkey}: no {cname} in {ble_h}")
    elif got.lower() != str(want).lower():
        finding(f"ble_identity.{rkey}: code '{got}' vs registry '{want}'")
    else:
        say(f"  ok: ble_identity.{rkey}")
say("")

say("[INTERNAL VOCAB (C5 bridge, two-ended constants)]")
a = grab("include/comms/SlopSyncUartTransport.h", r"kBridgeOpSlotClosed\s*=\s*0x([0-9A-Fa-f]+)", lambda s: int(s, 16))
b = grab("src/c5_probe/main.cpp", r"kBridgeOpSlotClosed\s*=\s*0x([0-9A-Fa-f]+)", lambda s: int(s, 16))
if a is None or b is None:
    finding(f"kBridgeOpSlotClosed missing on one end (S3={a}, C5={b}); one vocabulary, two ends")
elif a != b:
    finding(f"kBridgeOpSlotClosed: S3 0x{a:02X} vs C5 0x{b:02X}")
else:
    say(f"  ok: kBridgeOpSlotClosed 0x{a:02X} on both ends")
say("")

say("[SYMBOL DRIFT (used here, absent from pinned generated header)]")
gen = (sib / "lib/slopsync/include/slopsync/generated/registry_constants.hpp").read_text(encoding="utf-8", errors="replace")
defined = set(re.findall(r"\b(?:k[A-Za-z0-9_]+|[A-Z][A-Z0-9_]{2,})\b", gen))
used = set()
for d in ("include", "src"):
    for f in (root / d).rglob("*"):
        if f.suffix in (".h", ".hpp", ".cpp", ".cc"):
            txt = f.read_text(encoding="utf-8", errors="replace")
            for ns in ("FrameType", "CborKey", "NackCode", "ChannelClass", "AccessLevel"):
                used |= {(ns, m) for m in re.findall(ns + r"::(\w+)", txt)}
            used |= {("limits", m) for m in re.findall(r"limits::(\w+)", txt)}
missing = sorted(f"{ns}::{name}" for ns, name in used
                 if name not in gen and name.upper() not in defined)
if missing:
    for m in missing:
        finding(f"{m} used in this repo but not in the pinned generated header (implemented ahead of spec: candidate RFC)")
else:
    say(f"  ok: all {len(used)} referenced registry symbols exist in the pinned header")

sys.exit(0 if findings == 0 else 42)
PY
PYRC=$?
[ "$PYRC" = "42" ] && FINDINGS=$((FINDINGS + 1))
[ "$PYRC" != "0" ] && [ "$PYRC" != "42" ] && { finding "value-check pass failed (exit $PYRC)"; }

say ""
if [ "$FINDINGS" = "0" ]; then
  say "=== no drift: the spec knows everything this machine has built ==="
  exit 0
else
  say "=== drift detected: each !! above is a candidate RFC (draft in ../SlopSync/spec/RFC-QUEUE.md) ==="
  exit 1
fi
