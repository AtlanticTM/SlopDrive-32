#!/usr/bin/env python3
"""
dir_ladder.py -- reversal step-loss ladder for the AIM servo step/DIR path.

Answers ONE question with a number: how much position does the machine lose
per reversal on a given firmware build? Run it once per rung, change exactly
one thing between rungs, diff the rows.

The full experiment design -- what the rungs are, what order they go in, and
what each rung is supposed to prove -- lives in docs/canon/LEDGER.md under
"DIR LADDER". Read that first. This file is the instrument, not the plan.

WIRE FORMAT: NOT REIMPLEMENTED HERE.
    Every byte on the wire comes from ../SlopSync/tools/slopsync_probe.py,
    imported as a module -- the same coupling slopsoak.py already uses, for
    the same reason (a second CBOR encoder is a second thing to drift).
    That file is currently git-ignored in the SlopSync repo, so a clean
    checkout gets a ladder that cannot import its own wire layer.

TWO BOARDS, TWO ADDRESSES -- this is not a convenience, it is the topology.
    --ip      the S3. The machine. Everything MEASURED comes from here over
              read-only HTTP: encoder deviation, homed state, fw version, the
              `backend=` boot line, and the /uitoken mint.
    --hub-ip  the C5. Since the comms offload it terminates the SlopSync
              WebSocket and relays frames to the S3 over UART, so every
              CONTROL byte (home, motion) goes here. The S3 has no listener.
Note the S3's /api/capabilities still advertises `slopsync_port` as if it
served SlopSync itself. The PORT is right; the implied host is not.

HTTP is READ-ONLY on this device -- every control route answers 410.

    python tools/dir_ladder.py --label "rung1-rmt-defaults" --strokes 200

Requires: pip install websocket-client
"""

import argparse
import json
import math
import os
import struct
import sys
import time
import urllib.request

_SLOPSYNC_TOOLS = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "SlopSync", "tools")
sys.path.insert(0, os.path.abspath(_SLOPSYNC_TOOLS))
try:
    import slopsync_probe as sp
except ImportError as e:
    sys.exit("dir_ladder.py needs slopsync_probe.py (the wire layer) at\n"
             "  %s\n%s" % (os.path.abspath(_SLOPSYNC_TOOLS), e))
import websocket


# ---- HTTP read-only side -----------------------------------------------------

def http_json(ip, path, timeout=6.0):
    with urllib.request.urlopen("http://%s%s" % (ip, path), timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def http_text(ip, path, timeout=8.0):
    with urllib.request.urlopen("http://%s%s" % (ip, path), timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def read_backend(ip):
    """Ground truth for which pulse backend is RUNNING, from the boot log.
    AIMServoDriver logs `backend=` at init, at WARN so the boot INFO burst
    cannot evict it. Never infer this from the source tree -- "it built" is not
    evidence that it is the one flashed.

    /api/log serves PLAIN TEXT, not JSON. Do not route it through http_json."""
    try:
        log = http_text(ip, "/api/log")
    except Exception:
        return None
    for line in reversed(log.splitlines()):
        if "backend=" in line:
            return line.split("backend=", 1)[1].split()[0].strip(", ")
    return None


def read_sync(ip):
    """Device-side count of stream bundles that actually reached the motion
    engine. The ONLY proof the machine received what we sent -- frames can
    cross the C5 bridge, be accepted by the hub, and still be dropped at
    §11.4 source-ownership validation without a NACK the client will see."""
    return (http_json(ip, "/api/slopmotion").get("sync") or {}).get("bundles", 0)


def read_encoder(ip):
    enc = http_json(ip, "/api/servo").get("enc")
    if not enc:
        raise SystemExit("no `enc` block in /api/servo -- encoder validator is "
                         "absent (not a DRIVER_AIM_SERVO build, or no RS485 link)")
    return enc


# ---- SlopSync control link ---------------------------------------------------

class Link:
    """One publisher session: HELLO -> WELCOME -> CATALOG_READY -> CLOCK.
    No subscriptions -- this tool reads its measurements over HTTP, so the
    only thing the socket carries outbound is motion."""

    def __init__(self, hub_ip, port, auth_ip, timeout=6.0, publishes=None):
        # hub_ip is the C5 (WebSocket). auth_ip is the S3 -- /uitoken is a
        # WebUI sideband and lives on the machine, not on the bridge.
        self.ip, self.port, self.timeout = hub_ip, port, timeout
        self.auth_ip = auth_ip
        self.ws = None
        self.hub_offset_us = 0
        self.granted_hz = 0.0
        # channel_id -> granted rate. Callers publishing 0x2101 read this
        # instead of granted_hz, which names 0x2100 only.
        self.granted = {}
        self.publishes = publishes or [(sp.CH_MOTION_INPUT, 50.0)]
        self.deadman_ms = None
        self._intent_id = 0
        self._seq = 0

    def open(self):
        self.ws = websocket.create_connection(
            "ws://%s:%d/" % (self.ip, self.port),
            subprotocols=[sp.WS_SUBPROTOCOL], timeout=self.timeout)
        # Publishing a stream needs controller tier. The mint is rate-limited
        # device-wide to one per 250 ms -- one session per run, so one mint.
        token = sp.mint_uitoken(self.auth_ip)
        if token is None:
            raise SystemExit("/uitoken mint failed -- without it this session is "
                             "watch tier and cannot move the machine")
        # instance_id is a bstr and must be EXACTLY 8 bytes -- anything else
        # NACKs the HELLO as MALFORMED.
        #
        # RANDOM PER SESSION, never fixed. A fixed id was tried and every FIRST
        # run after an S3 reboot delivered zero stream bundles while the second
        # run on the same boot worked -- frames arriving at the hub (framesRx
        # counted them) and being dropped before onStreamBundle. slopsoak.py
        # uses os.urandom(8) for the same reason. Do not "stabilize" this.
        self.send(sp.FRAME["HELLO"], 0, sp.build_hello(
            "diag", "dir_ladder.py", os.urandom(8),
            publishes=self.publishes, token=token))

        w = self._await(sp.FRAME["WELCOME"])
        etag = w.get(sp.K["catalog_etag"])
        if not isinstance(etag, bytes) or len(etag) != 8:
            raise SystemExit("WELCOME carried no usable catalog_etag")
        self.deadman_ms = w.get(sp.K["deadman_ms"])
        for gp in w.get(sp.K["granted_publishes"], []) or []:
            ch = gp.get(sp.K["channel_id"])
            self.granted[ch] = float(gp.get(sp.K["granted_rate_hz"]) or 0.0)
        self.granted_hz = self.granted.get(sp.CH_MOTION_INPUT, 0.0)
        if not any(hz > 0 for hz in self.granted.values()):
            raise SystemExit("hub granted no publish rate on any requested motion "
                             "channel -- another client probably owns the source")
        # 8.4 readiness gate: no STREAM or INTENT until we name our catalog.
        self.send(sp.FRAME["CATALOG_READY"], 0, etag)
        self._clock_sync()
        return self

    def send(self, ftype, channel, payload):
        self.ws.send(sp.encode_frame(ftype, channel, payload, self._seq),
                     opcode=websocket.ABNF.OPCODE_BINARY)
        self._seq = (self._seq + 1) & 0xFFFF

    def drain(self):
        """Non-blocking read of anything the hub sent us. Unread bytes stall
        the hub's writer, which is the exact failure slopsoak's wedge test
        exists to catch -- do not let this tool become that client."""
        self.ws.settimeout(0.0)
        try:
            while True:
                opcode, data = self.ws.recv_data()
                if opcode == websocket.ABNF.OPCODE_CLOSE:
                    raise SystemExit("hub closed the session mid-run")
                if opcode != websocket.ABNF.OPCODE_BINARY or not data:
                    continue
                hdr = sp.decode_frame_header(data)
                if hdr and hdr["type"] == sp.FRAME["NACK"]:
                    n = sp.cb_decode_full(data[sp.FRAME_HDR.size:])
                    print("  NACK ch=0x%04X %s" % (n.get(sp.K["channel_id"], 0),
                                                   sp.nack_name(n.get(sp.K["code"]))))
        except Exception:
            pass
        finally:
            self.ws.settimeout(self.timeout)

    def _await(self, want, timeout=None):
        deadline = time.time() + (timeout or self.timeout)
        while time.time() < deadline:
            self.ws.settimeout(max(0.05, deadline - time.time()))
            try:
                opcode, data = self.ws.recv_data()
            except Exception:
                continue
            if opcode != websocket.ABNF.OPCODE_BINARY or not data:
                continue
            hdr = sp.decode_frame_header(data)
            if hdr is None:
                continue
            payload = data[sp.FRAME_HDR.size:]
            if hdr["type"] == want:
                return sp.cb_decode_full(payload)
            if hdr["type"] == sp.FRAME["NACK"]:
                n = sp.cb_decode_full(payload)
                raise SystemExit("NACK %s ch=0x%04X detail=%r" % (
                    sp.nack_name(n.get(sp.K["code"])),
                    n.get(sp.K["channel_id"], 0), n.get(sp.K["detail"])))
        raise SystemExit("no frame 0x%02X within %.1fs" % (want, timeout or self.timeout))

    def _clock_sync(self):
        t0 = sp.client_now_us()
        self.send(sp.FRAME["CLOCK"], 0, sp.CLOCK_REQUEST_STRUCT.pack(t0))
        deadline = time.time() + 3.0
        while time.time() < deadline:
            self.ws.settimeout(max(0.05, deadline - time.time()))
            try:
                opcode, data = self.ws.recv_data()
            except Exception:
                continue
            if opcode != websocket.ABNF.OPCODE_BINARY or not data:
                continue
            hdr = sp.decode_frame_header(data)
            if hdr is None:
                continue
            payload = data[sp.FRAME_HDR.size:]
            if hdr["type"] == sp.FRAME["CLOCK"] and len(payload) >= sp.CLOCK_REPLY_STRUCT.size:
                t3 = sp.client_now_us()
                t0e, t1, t2 = sp.CLOCK_REPLY_STRUCT.unpack_from(payload, 0)
                self.hub_offset_us = (sp.wrap_diff(t1, t0e) + sp.wrap_diff(t2, t3)) // 2
                return
        print("  WARN: no CLOCK reply -- stream t_base is unsynced")

    def intent(self, channel, fields):
        self._intent_id = (self._intent_id + 1) & 0xFFFF
        self.send(sp.FRAME["INTENT"], channel,
                  sp.build_intent(channel, self._intent_id, fields))

    def sample(self, target_norm, vel_norm):
        t_base = (sp.client_now_us() + self.hub_offset_us) & 0xFFFFFFFF
        self.send(sp.FRAME["STREAM"], sp.CH_MOTION_INPUT,
                  sp.encode_stream_bundle(t_base, [(0, target_norm, vel_norm)]))

    def close(self, stop_first=True):
        if self.ws is None:
            return
        try:
            if stop_first:
                self.intent(sp.CH_SAFETY_INTENT, [(1, sp.cb_uint(sp.SAFETY_OP["stop"]))])
                time.sleep(0.2)
            self.send(sp.FRAME["GOODBYE"], 0, sp.build_goodbye(0))
        except Exception:
            pass
        try:
            self.ws.close()
        except Exception:
            pass
        self.ws = None


# ---- Ladder steps ------------------------------------------------------------

def home(link, ip, timeout=90.0):
    """Homing is also the ZEROING step: EncoderValidator drops its reference
    on unhome and re-latches on the next home, so dev_steady_mm starts each
    rung at ~0 with no separate reset call.

    op 1 ONLY, and field 2 is deliberately not sent. Field 2 (`stroke`) is read
    only by op 2, `force_home` — the bench op that declares the machine homed
    WITHOUT measuring and asserts a stroke length nothing verified, while also
    clearing the e-stop latch. With a motor attached that is a collision
    hazard. Never give this call a stroke to carry."""
    link.intent(sp.CH_HOME_INTENT, [(1, sp.cb_uint(1))])
    deadline = time.time() + timeout
    while time.time() < deadline:
        link.drain()
        time.sleep(1.0)
        st = http_json(ip, "/api/status")
        if st.get("homed") and not st.get("homing"):
            return st
    raise SystemExit("homing did not finish within %.0fs" % timeout)


def settle(ip, seconds=5.0, timeout=40.0):
    """EncoderValidator only scores at STANDSTILL, and needs a streak of >=2
    still Modbus samples (~540 ms) before it will move dev_steady_mm. Anything
    read sooner is Modbus timing skew, not step loss.

    Then WAIT FOR TRACKING. state: 0 idle, 1 sign-detect, 2 tracking; only
    state 2 ever writes dev_steady_mm. Reading in state 0/1 returns the init
    value 0, which is indistinguishable from "no drift" and is how this tool
    first reported a confident 0.000 for a run that had not been measured at
    all. Returns None if it never gets there -- callers must treat that as
    fatal, never as a zero."""
    time.sleep(seconds)
    deadline = time.time() + timeout
    while time.time() < deadline:
        enc = read_encoder(ip)
        if enc.get("state") == 2 and enc.get("have_dev"):
            return enc
        time.sleep(1.0)
    return None


def run_strokes(link, strokes, rate_hz, low, high, sample_hz):
    """Continuous sine between `low` and `high`. Continuous is the point: the
    ledger's own measurement found ~10x more loss per stroke back-to-back than
    with idle gaps, so a paced run would measure the wrong regime."""
    mid, amp = (high + low) / 2.0, (high - low) / 2.0
    period = 1.0 / sample_hz
    duration = strokes / rate_hz
    omega = 2.0 * math.pi * rate_hz
    t_start = time.time()
    nxt = t_start
    sent = 0
    while True:
        now = time.time()
        phase = now - t_start
        if phase >= duration:
            break
        if now < nxt:
            time.sleep(min(period / 4.0, nxt - now))
            continue
        nxt += period
        if nxt < now - period:      # fell behind: resync, never spin to catch up
            nxt = now + period
        link.sample(max(0.0, min(1.0, mid + amp * math.sin(omega * phase))),
                    amp * omega * math.cos(omega * phase))
        sent += 1
        if sent % 25 == 0:
            link.drain()
    # Park on the midpoint at zero velocity so the run ends at a standstill
    # rather than wherever the sine happened to stop.
    for _ in range(5):
        link.sample(mid, 0.0)
        time.sleep(0.02)
    return sent, time.time() - t_start


# ---- Main --------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default="192.168.1.229",
                    help="the S3 -- everything measured comes from here")
    ap.add_argument("--hub-ip", default="192.168.1.71",
                    help="the C5 -- terminates the SlopSync WebSocket")
    # Port 80 is the WEB UI. SlopSync listens on its own port and announces it
    # in /api/capabilities -- ask the device rather than hardcoding, or the
    # handshake gets index.html back and fails with a 200.
    ap.add_argument("--port", type=int, default=0,
                    help="0 = read slopsync_port from /api/capabilities")
    ap.add_argument("--label", required=True,
                    help="which rung this is, e.g. rung1-rmt-defaults")
    ap.add_argument("--strokes", type=int, default=200,
                    help="full out-and-back cycles; each is TWO reversals")
    ap.add_argument("--rate", type=float, default=2.0, help="strokes per second")
    ap.add_argument("--low", type=float, default=0.2, help="normalized bottom")
    ap.add_argument("--high", type=float, default=0.8, help="normalized top")
    ap.add_argument("--sample-hz", type=float, default=50.0,
                    help="stream rate; clamped to whatever the hub grants")
    ap.add_argument("--arm", type=int, default=8,
                    help="strokes run BEFORE the baseline to bring the encoder "
                         "validator into tracking; excluded from the result")
    ap.add_argument("--settle", type=float, default=5.0,
                    help="standstill seconds before reading the encoder")
    ap.add_argument("--out", default="artifacts/dir_ladder.json")
    ap.add_argument("--note", default="")
    args = ap.parse_args()

    rows = []
    if os.path.exists(args.out):
        with open(args.out, encoding="utf-8") as f:
            rows = json.load(f)

    caps = http_json(args.ip, "/api/capabilities")
    fw = caps.get("fw_version")
    port = args.port or int(caps.get("slopsync_port") or 0)
    if not port:
        raise SystemExit("device did not report slopsync_port; pass --port")
    backend = read_backend(args.ip)
    print("machine %s fw=%s backend=%s | hub ws://%s:%d/" %
          (args.ip, fw, backend, args.hub_ip, port))
    # A row with no backend is a row that cannot be attributed to a rung.
    if backend is None:
        raise SystemExit("no `backend=` line in /api/log -- cannot prove which "
                         "backend is flashed. Re-deploy and run immediately, or "
                         "the boot line has aged out of the ring.")

    # A rung that never flashed is the one failure mode that silently produces
    # a plausible number. fw_version is the only proof the build changed.
    for prev in rows:
        if prev["fw"] == fw and prev["label"] != args.label:
            print("  WARN: fw %s already recorded as '%s'. Either you forgot the "
                  "FIRMWARE_VERSION bump, or this rung is not flashed." %
                  (fw, prev["label"]))

    link = Link(args.hub_ip, port, args.ip).open()
    sample_hz = min(args.sample_hz, link.granted_hz)
    print("granted %.0f Hz on 0x2100, streaming at %.0f Hz" % (link.granted_hz, sample_hz))

    try:
        print("homing (this also zeroes the encoder reference)...")
        home(link, args.ip)

        # ARMING. The validator cannot be in tracking before the first strokes:
        # it needs MOTION to resolve sign and counts/mm, so straight after a
        # home it sits in sign-detect however long you wait. Stroke a little to
        # bring it up, and only THEN take the baseline -- so whatever these
        # strokes and the homing sweeps cost is BELOW the baseline and excluded
        # from the measurement instead of being silently counted as drift.
        print("arming the validator (%d strokes, excluded from the result)..." % args.arm)
        run_strokes(link, args.arm, args.rate, args.low, args.high, sample_hz)
        before = settle(args.ip, args.settle)
        if before is None:
            raise SystemExit(
                "ABORT -- EncoderValidator never reached tracking after %d arming\n"
                "strokes. Without a tracked baseline every later reading is the\n"
                "instrument's init value. Try more --arm strokes. NO ROW WRITTEN."
                % args.arm)
        print("  baseline dev_steady=%+.4f mm (state=2, tracking)" % before["dev_steady_mm"])

        print("running %d strokes (%d reversals) at %.2f/s..." %
              (args.strokes, args.strokes * 2, args.rate))
        bundles_before = read_sync(args.ip)
        sent, elapsed = run_strokes(link, args.strokes, args.rate,
                                    args.low, args.high, sample_hz)
        print("  %d samples in %.1f s (%.1f Hz)" % (sent, elapsed, sent / max(elapsed, 1e-6)))
    finally:
        link.close()

    # ---- GUARD: did the machine receive anything? --------------------------
    # A run that moves nothing still produces a clean-looking 0.000 um/reversal,
    # which is the worst output this tool could give. Fail loudly instead.
    landed = read_sync(args.ip) - bundles_before
    if landed < sent * 0.5:
        raise SystemExit(
            "ABORT -- sent %d stream bundles, the engine counted %d.\n"
            "The frames reached the hub and were dropped, almost certainly at\n"
            "SPEC 11.4 source ownership: another session owns the motion source.\n"
            "Close any other SlopSync client (check ws_slots_used at\n"
            "http://<c5>/probe) and re-run. NO ROW WAS WRITTEN." % (sent, landed))

    after = settle(args.ip, args.settle)
    if after is None:
        raise SystemExit("ABORT -- EncoderValidator dropped out of tracking during "
                         "the run (a re-home or an unhome resets it). NO ROW WRITTEN.")
    reversals = args.strokes * 2
    drift_mm = after["dev_steady_mm"] - before["dev_steady_mm"]
    # Travel is the OTHER denominator. A per-reversal defect and a per-distance
    # defect are indistinguishable at a fixed window, so both are recorded and
    # the classification is which one holds constant across a window change.
    stroke_mm = float(http_json(args.ip, "/api/status").get("measured_stroke_mm") or 0.0)
    travel_m = reversals * (args.high - args.low) * stroke_mm / 1000.0
    row = {
        "label": args.label, "note": args.note,
        "stamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "fw": fw, "backend": backend,
        "strokes": args.strokes, "reversals": reversals, "arm_strokes": args.arm,
        "rate_hz": args.rate, "window_norm": [args.low, args.high],
        "sample_hz": round(sample_hz, 1), "samples_sent": sent,
        "dev_steady_before_mm": round(before["dev_steady_mm"], 5),
        "dev_steady_after_mm": round(after["dev_steady_mm"], 5),
        "max_steady_mm": round(after["max_steady_mm"], 5),
        "drift_mm": round(drift_mm, 5),
        "um_per_reversal": round(1000.0 * drift_mm / reversals, 3),
        "stroke_mm": round(stroke_mm, 2),
        "travel_m": round(travel_m, 3),
        "um_per_m": round(1000.0 * drift_mm / travel_m, 3) if travel_m > 0 else None,
        "warn": bool(after.get("warn")),
        "cpmm_meas": after.get("cpmm_meas"),
    }
    rows.append(row)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(rows, f, indent=2)

    print()
    print("%-28s %-12s %10s %14s %8s %10s" %
          ("label", "backend", "drift_mm", "um/reversal", "travel_m", "um/m"))
    for r in rows:
        print("%-28s %-12s %10.4f %14.3f %8s %10s" %
              (r["label"], r["backend"] or "?", r["drift_mm"], r["um_per_reversal"],
               r.get("travel_m", "-"), r.get("um_per_m", "-")))
    print("\nwrote %s (%d rows)" % (args.out, len(rows)))


if __name__ == "__main__":
    main()
