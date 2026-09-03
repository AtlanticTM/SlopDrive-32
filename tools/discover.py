#!/usr/bin/env python3
"""Broadcast a SPEC 13.8 DISCOVER_PROBE and print every hub that answers.

    python tools/discover.py              # one probe, 2 s of replies
    python tools/discover.py --wait 30    # keep listening, reprobe each second

Stdlib only, so it runs from a bare shell on any host with the repo checked
out (architecture.md section 4: standalone invocation never stops working).

The port, magic and field widths are READ FROM include/comms/
SlopSyncDiscoveryWire.h at run time, never transcribed: that header is this
repo's one home for them (it cites registry.yaml `udp_discovery`, which the
SlopSync codegen does not emit as C++ constants). A layout change there
changes this script with no edit; a change that breaks the regexes fails
loudly instead of decoding garbage.
"""
import argparse
import pathlib
import random
import re
import socket
import struct
import sys
import time

WIRE = pathlib.Path(__file__).resolve().parent.parent / "include" / "comms" / "SlopSyncDiscoveryWire.h"


def _consts():
    text = WIRE.read_text(encoding="utf-8")

    def num(name):
        m = re.search(name + r"\s*=\s*(\d+)", text)
        if not m:
            sys.exit(f"FATAL: {name} not found in {WIRE}; the header's shape changed")
        return int(m.group(1))

    m = re.search(r"kMagic\s*=\s*\{([^}]*)\}", text)
    if not m:
        sys.exit(f"FATAL: kMagic not found in {WIRE}; the header's shape changed")
    magic = bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    return {
        "port": num("kPort"),
        "magic": magic,
        "name": num("kHubNameMaxBytes"),
        "fw": num("kFwVersionMaxBytes"),
        "etag": num("kEtagBytes"),
    }


C = _consts()
REPLY_BYTES = 4 + 4 + C["name"] + 8 + 1 + 2 + C["fw"] + C["etag"] + 1


def parse_reply(data, nonce):
    if len(data) != REPLY_BYTES or data[:4] != C["magic"]:
        return None
    off = 4
    (echo,) = struct.unpack_from("<I", data, off); off += 4
    if echo != nonce:
        return None  # someone else's probe, or a stale reply
    name = data[off:off + C["name"]].rstrip(b"\0").decode("utf-8", "replace"); off += C["name"]
    (inst,) = struct.unpack_from("<Q", data, off); off += 8
    proto = data[off]; off += 1
    (ws_port,) = struct.unpack_from("<H", data, off); off += 2
    fw = data[off:off + C["fw"]].rstrip(b"\0").decode("utf-8", "replace"); off += C["fw"]
    etag = data[off:off + C["etag"]].hex(); off += C["etag"]
    flags = data[off]
    return {
        "hub_name": name or "(none)", "hub_instance_id": f"{inst:#018x}",
        "proto_ver": proto, "ws_port": ws_port, "fw_version": fw or "(none)",
        "catalog_etag": etag, "flags": flags,
        "pairing_window_open": bool(flags & 0x01), "ws_available": bool(flags & 0x02),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wait", type=float, default=2.0,
                    help="seconds to listen (default 2); reprobes once per second")
    ap.add_argument("--proto", type=int, default=1, help="proto_ver in the probe (default 1)")
    args = ap.parse_args()

    nonce = random.getrandbits(32)
    probe = C["magic"] + struct.pack("<BI", args.proto, nonce)

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(0.25)
    print(f"probing 255.255.255.255:{C['port']} nonce={nonce:#010x} for {args.wait:g}s")

    seen, deadline, next_probe = {}, time.time() + args.wait, 0.0
    while time.time() < deadline:
        if time.time() >= next_probe:
            s.sendto(probe, ("255.255.255.255", C["port"]))
            next_probe = time.time() + 1.0
        try:
            data, addr = s.recvfrom(512)
        except socket.timeout:
            continue
        r = parse_reply(data, nonce)
        if r is None or addr[0] in seen:
            continue
        seen[addr[0]] = r
        print(f"\n  {addr[0]}:{r['ws_port']}  ws://{addr[0]}:{r['ws_port']}/")
        for k in ("hub_name", "hub_instance_id", "fw_version", "catalog_etag",
                  "proto_ver", "pairing_window_open", "ws_available"):
            print(f"    {k:<20} {r[k]}")

    print(f"\n{len(seen)} hub(s) answered")
    return 0 if seen else 1


if __name__ == "__main__":
    sys.exit(main())
