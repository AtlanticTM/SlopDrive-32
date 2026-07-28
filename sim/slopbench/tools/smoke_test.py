#!/usr/bin/env python3
"""
smoke_test.py — end-to-end verification for SlopBench's three example
configs (sim/slopbench/configs/*.bench). Launches the built `slopbench`
binary against each config in turn and drives it exactly like a real client
would: HELLO/WELCOME, CATALOG_READY, a full BLOB_REQ catalog fetch (checked
against the channels the config declares), one clamped INTENT write, and a
measurement of how long the mirrored STATE channel takes to reflect it (the
fake echo delay, task item 3).

This is a dev tool, not part of the library: it IMPORTS
tools/slopsync_probe.py as a module for the wire layer (frame encode/decode,
CBOR helpers, BLOB reassembly) rather than re-implementing any of it — per
the task brief, that file is read-only from here.

Usage:
    python sim/slopbench/tools/smoke_test.py [--exe PATH]

Requires the `websocket-client` package (same as slopsync_probe.py itself):
    pip install websocket-client
"""
import argparse
import os
import struct
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SLOPBENCH_DIR = os.path.dirname(SCRIPT_DIR)
REPO_ROOT = os.path.dirname(os.path.dirname(SLOPBENCH_DIR))

sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))
import slopsync_probe as probe  # noqa: E402  (path insert must come first)

try:
    import websocket
except ImportError:
    sys.stderr.write("smoke_test.py requires the 'websocket-client' package.\n"
                      "Install it with:\n\n    pip install websocket-client\n\n")
    sys.exit(1)


PASS = []
FAIL = []


def ok(step, msg):
    PASS.append((step, msg))
    print("  [PASS] %s: %s" % (step, msg))


def bad(step, msg):
    FAIL.append((step, msg))
    print("  [FAIL] %s: %s" % (step, msg))


def wait_for_port(port, timeout_s=5.0):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            ws = websocket.create_connection(
                "ws://127.0.0.1:%d/" % port, subprotocols=[probe.WS_SUBPROTOCOL], timeout=1)
            ws.close()
            return True
        except Exception:
            time.sleep(0.15)
    return False


def fetch_catalog(ws, timeout_s):
    """Full BLOB_REQ/BLOB_CHUNK reassembly (RFC-021, namespace 0). Returns
    the reassembled bytes, or None with a FAIL already recorded."""
    probe.send_frame(ws, probe.FRAME["BLOB_REQ"], 0, probe.build_blob_req())
    chunks = {}
    expect_count = None
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        got = probe.recv_frame(ws, deadline)
        if got is None:
            break
        hdr, payload = got
        if hdr["type"] != probe.FRAME["BLOB_CHUNK"]:
            continue
        c = probe.decode_blob_chunk(payload)
        if c is None:
            bad("blob_chunk", "BLOB_CHUNK shorter than its identity header")
            return None
        expect_count = c["chunk_count"]
        chunks[c["chunk_index"]] = c["bytes"]
        if expect_count and len(chunks) == expect_count:
            break
    if expect_count is None or len(chunks) != expect_count:
        bad("blob_catalog", "incomplete catalog transfer (%d chunks, expected %s)"
            % (len(chunks), expect_count))
        return None
    return b"".join(chunks[i] for i in range(expect_count))


def run_one(exe, config_name, port, state_id, state_struct_fmt, mirrored_field_index,
           intent_id, intent_key, write_value, expect_clamped, expected_channel_ids,
           min_delay_ms, max_delay_ms):
    """Drives one config end to end. Returns True iff every check passed."""
    print("\n=== %s (port %d) ===" % (config_name, port))
    fail_count_at_start = len(FAIL)
    config_path = os.path.join(SLOPBENCH_DIR, "configs", config_name)
    proc = subprocess.Popen([exe, config_path, "--port", str(port), "--headless"],
                            cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    try:
        if not wait_for_port(port):
            bad(config_name, "hub never opened :%d" % port)
            return False

        ws = websocket.create_connection(
            "ws://127.0.0.1:%d/" % port, subprotocols=[probe.WS_SUBPROTOCOL], timeout=5)
        instance_id = os.urandom(8)
        probe.send_frame(ws, probe.FRAME["HELLO"], 0,
                         probe.build_hello("smoke-test", "slopbench-smoke", instance_id))

        deadline = time.time() + 5.0
        welcome = None
        while time.time() < deadline:
            got = probe.recv_frame(ws, deadline)
            if got is None:
                break
            hdr, payload = got
            if hdr["type"] == probe.FRAME["WELCOME"]:
                welcome = probe.cb_decode_full(payload)
                break
        if welcome is None:
            bad(config_name, "no WELCOME within 5s")
            return False

        etag = welcome.get(probe.K["catalog_etag"])
        if not isinstance(etag, bytes) or len(etag) != 8:
            bad(config_name, "WELCOME carried no usable 8-byte catalog_etag")
            return False
        ok(config_name, "WELCOME received, catalog_etag=%s" % etag.hex())

        probe.send_frame(ws, probe.FRAME["CATALOG_READY"], 0, etag)

        catalog_bytes = fetch_catalog(ws, 5.0)
        if catalog_bytes is None:
            return False
        entries = probe._catalog_entries(catalog_bytes)
        missing = [hex(cid) for cid in expected_channel_ids if cid not in entries]
        if missing:
            bad(config_name, "catalog fetch matches config: MISSING channel ids %s" % missing)
        else:
            ok(config_name, "catalog fetch matches config: all %d declared channel ids present "
               "(%d entries total)" % (len(expected_channel_ids), len(entries)))

        probe.send_frame(ws, probe.FRAME["SUBSCRIBE"], 0,
                         probe.build_subscribe([(state_id, 20.0, probe.PRIORITY["normal"])]))

        # Drain until the retained baseline STATE arrives (GRANT precedes it).
        deadline = time.time() + 5.0
        baseline = None
        while time.time() < deadline:
            got = probe.recv_frame(ws, deadline)
            if got is None:
                break
            hdr, payload = got
            if hdr["type"] == probe.FRAME["STATE"] and hdr["channel"] == state_id:
                baseline = payload
                break
        if baseline is None:
            bad(config_name, "no retained STATE for 0x%04X within 5s" % state_id)
            return False

        def mirrored_value(payload):
            vals = struct.unpack_from(state_struct_fmt, payload)
            return vals[mirrored_field_index]

        before = mirrored_value(baseline)

        intent_channel = intent_channel_of(config_name)
        t_send = time.time()
        probe.send_frame(ws, probe.FRAME["INTENT"], intent_channel,
                         probe.build_intent(intent_channel, intent_id,
                                            [(intent_key, probe.cb_f32(write_value))]))

        echo_ok = False
        deadline = time.time() + 5.0
        t_echo = None
        while time.time() < deadline:
            got = probe.recv_frame(ws, deadline)
            if got is None:
                break
            hdr, payload = got
            if hdr["type"] == probe.FRAME["ECHO"]:
                t_echo = time.time()
                decoded = probe.cb_decode_full(payload)
                applied = decoded.get(probe.K["applied"], {})
                applied_val = applied.get(intent_key)
                if applied_val is not None and abs(applied_val - expect_clamped) < 0.01:
                    echo_ok = True
                else:
                    bad(config_name, "ECHO applied[%d]=%s, expected clamp to %.3f"
                        % (intent_key, applied_val, expect_clamped))
                break
            if hdr["type"] == probe.FRAME["NACK"]:
                code = probe.cb_decode_full(payload).get(probe.K["code"])
                bad(config_name, "INTENT NACKed: %s" % probe.nack_name(code))
                break
        if not echo_ok:
            if t_echo is None:
                bad(config_name, "no ECHO within 5s")
            return False
        ok(config_name, "INTENT clamp+echo: wrote %.1f, echo applied %.3f (t_echo +%.0fms)"
           % (write_value, expect_clamped, (t_echo - t_send) * 1000.0))

        t_mirrored = None
        deadline = time.time() + max(5.0, (max_delay_ms / 1000.0) + 3.0)
        while time.time() < deadline:
            got = probe.recv_frame(ws, deadline)
            if got is None:
                break
            hdr, payload = got
            if hdr["type"] == probe.FRAME["STATE"] and hdr["channel"] == state_id:
                v = mirrored_value(payload)
                if abs(v - expect_clamped) < 0.01:
                    t_mirrored = time.time()
                    break
        if t_mirrored is None:
            bad(config_name, "mirrored STATE field never reached the clamped value within the wait window")
            return False
        delay_ms = (t_mirrored - t_send) * 1000.0
        if delay_ms < min_delay_ms:
            bad(config_name, "echo delay %.0fms is BELOW the configured floor %.0fms (before=%.3f)"
                % (delay_ms, min_delay_ms, before))
        elif delay_ms > max_delay_ms:
            bad(config_name, "echo delay %.0fms exceeds the generous ceiling %.0fms" % (delay_ms, max_delay_ms))
        else:
            ok(config_name, "measured STATE-echo delay = %.0fms (configured floor %.0fms, ceiling %.0fms)"
               % (delay_ms, min_delay_ms, max_delay_ms))

        probe.send_frame(ws, probe.FRAME["GOODBYE"], 0, probe.build_goodbye(probe.NACK_CODES_BY_NAME["NORMAL_CLOSURE"]))
        ws.close()
        return len(FAIL) == fail_count_at_start
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


def intent_channel_of(config_name):
    return {
        "tiny-axis.bench": 0x3000,
        "alien.bench": 0x3300,
        "kitchen-sink.bench": 0x3000,
    }[config_name]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=None, help="path to the built slopbench executable")
    args = ap.parse_args()

    exe = args.exe
    if exe is None:
        for candidate in ("slopbench.exe", "slopbench"):
            p = os.path.join(SLOPBENCH_DIR, "build", candidate)
            if os.path.isfile(p):
                exe = p
                break
    if exe is None or not os.path.isfile(exe):
        sys.stderr.write("smoke_test.py: no built slopbench binary found under sim/slopbench/build/ "
                          "(pass --exe explicitly)\n")
        return 1

    # tiny-axis: telemetry (0x1000) = {pos_mm f32, vel_mm_s f32} -> "<ff".
    # axis-set (0x3000) writes pos_mm (key 1), max 500 -> write 9999, expect 500.
    # echo_delay_ms 0: expect a near-instant mirror (floor 0, generous 400ms ceiling).
    run_one(exe, "tiny-axis.bench", 7401, 0x1000, "<ff", 0,
           1001, 1, 9999.0, 500.0, [0x1000, 0x3000], 0, 400)

    # alien: aux-state (0x1300) = {heat_pct f32, lube_pct f32} -> "<ff".
    # aux-set (0x3300) writes heat_pct (key 1), max 100 -> write 500, expect 100.
    # echo_delay_ms 400 (explicit): floor 350 (some scheduling slack), ceiling 1200.
    run_one(exe, "alien.bench", 7402, 0x1300, "<ff", 0,
           2001, 1, 500.0, 100.0, [0x1300, 0x3300, 0x4400, 0x5500, 0x2800], 350, 1200)

    # kitchen-sink: limits (0x1010) = {window_min, window_max, user_speed} f32 -> "<fff".
    # config-set (0x3000) writes window_min (key 1), min 0 -> write -50, expect 0.
    # echo_delay_ms inherited from the hub default (200ms): floor 150, ceiling 1200.
    run_one(exe, "kitchen-sink.bench", 7403, 0x1010, "<fff", 0,
           3001, 1, -50.0, 0.0, [0x1000, 0x1010, 0x1020, 0x1030, 0x3000, 0x3010, 0x4000, 0x2100, 0x5000],
           150, 1200)

    print("\n=== SUMMARY: %d passed, %d failed ===" % (len(PASS), len(FAIL)))
    for step, msg in FAIL:
        print("  FAIL %s: %s" % (step, msg))
    return 0 if len(FAIL) == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
