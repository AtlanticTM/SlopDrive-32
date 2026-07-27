#!/usr/bin/env python3
"""
slopsoak.py -- SlopSync STABILITY / SOAK harness.

slopsync_probe.py answers "is the protocol correct?". slopsoak.py answers the
question that actually decides whether anyone keeps the plugin installed:
"does it stay up?".  It exists because the operator's #1 complaint about
ecosystem clients is *clients that bug out and drop*, and because we are
replacing the firmware's WebSocket transport (links2004/arduinoWebSockets ->
ESP32Async AsyncWebSocket).  A transport swap that is *assumed* better is a
regression waiting to happen; this tool produces the before/after evidence.

  Run it on the OLD firmware  -> baseline.
  Run it on the NEW firmware  -> comparison.
  Diff the two JSON summaries -> the claim.

--------------------------------------------------------------------------
WIRE FORMAT: NOT REIMPLEMENTED HERE.
--------------------------------------------------------------------------
Every byte this tool puts on the wire comes from `slopsync_probe.py`, imported
as a module from the same directory.  Frame header, the SPEC 5.3 deterministic
CBOR profile, message builders, packed-STATE decoders and the registry
constants all live there, and duplicating them here would create exactly the
drift the registry-discipline rule exists to prevent.

  !! COUPLING NOTE: tools/slopsync_probe.py is currently GIT-IGNORED while
  !! this file is tracked.  A clean checkout therefore gets a slopsoak that
  !! cannot import its own wire layer.  The .gitignore already carries a note
  !! recommending the probe be tracked; this file makes that decision urgent.

--------------------------------------------------------------------------
SCENARIOS (each individually selectable, or run as a suite)
--------------------------------------------------------------------------
  b2b     Back-to-back sessions, NO reboot between them.  CLAUDE.md 8 records
          this as a MANDATORY verification pattern -- it is how the source-
          ownership teardown leak was caught (every earlier probe pass hid it
          because deploys rebooted the device between runs).
  rst     Abrupt death: SO_LINGER 0 -> close() sends a TCP RST mid-session.
          Exercises the 11.4 ownership teardown on the rude-death path, and
          proves a later client can still take the source.
  churn   Rapid connect/handshake/disconnect cycling.  Slot/heap churn.
  wedge   THE headline test.  A client completes the handshake, subscribes at
          a high rate, then STOPS READING its socket without closing it.  The
          TCP receive window goes to zero and the hub's next send has nowhere
          to go.  Under links2004 `sendBIN` is a SYNCHRONOUS BUSY-WAIT, so
          that send stalls the whole SlopSync service task -- and with it
          every other client's telemetry.  Two sub-modes:
            silent  wedged client sends nothing either (the firmware's
                    mute-then-evict sweep should reclaim it after ~2 s)
            chatty  wedged client keeps PINGing.  Nastier, and the realistic
                    one: SlopSyncWsTransport::pushRx() clears the send-stall
                    mute on ANY inbound byte, so a client that talks but never
                    listens re-arms the blocking write over and over.
          What is measured is not the wedged client -- it is the HEALTHY ones.
  stream  Streaming load: motion-input (0x0084) at rate while watchers watch.
          Default amplitude 0.0 == hold current position: full ingress path,
          zero machine motion (this device is shared; see --stream-amp).
  soak    N concurrent steady-state sessions for a long duration.  The plain
          "does it just stay up" test.  Any disconnect here is a FAILURE.

Throughout, two background monitors run for the WHOLE session:
  * HTTP monitor  -- GET /api/status on an interval.  This is the operator-
                     visible symptom ("the WebUI says unreachable"), measured
                     directly rather than inferred.
  * Log monitor   -- GET /api/log, de-duplicated, parsing the `[sys] heap
                     free=/min=/maxblock=/psram=` beacon for a heap trend, and
                     harvesting the firmware's own W/E lines as evidence.

--------------------------------------------------------------------------
FAILURE vs EXPECTED -- the harness is strict about this on purpose
--------------------------------------------------------------------------
  * A deliberate RST, a deliberate GOODBYE, or the firmware evicting a client
    we DELIBERATELY wedged: EXPECTED.  Not counted as a dropout.
  * A disconnect of a client that was reading its socket and sending
    keepalives: UNEXPECTED.  That is a dropout, and it is what we are hunting.
  * STATE `seq` gaps are NOT loss.  The hub bumps a channel's retained seq on
    every publish and paces sends at the granted rate, so a 60 Hz channel
    granted at 20 Hz legitimately steps +3.  Only seq REGRESSION or a repeat
    is a wire defect.  Loss/stall is measured as ARRIVAL GAPS instead.

Usage:
    set PYTHONIOENCODING=utf-8
    python tools/slopsoak.py --ip 192.168.1.229 --label links2004-baseline
    python tools/slopsoak.py --scenarios wedge,soak --soak-duration 120
    python tools/slopsoak.py --list

Requires: websocket-client  (pip install websocket-client)
"""
import argparse
import json
import math
import os
import re
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import slopsync_probe as sp
except ImportError as e:  # pragma: no cover
    sys.stderr.write(
        "slopsoak.py needs tools/slopsync_probe.py beside it (it is the wire\n"
        "layer -- frame header, CBOR profile, builders, registry constants).\n"
        "Import failed: %s\n" % e)
    sys.exit(2)

try:
    import websocket
except ImportError:  # pragma: no cover
    sys.stderr.write("slopsoak.py requires 'websocket-client'.  pip install websocket-client\n")
    sys.exit(2)


# =============================================================================
# Small helpers
# =============================================================================

def now():
    return time.time()


def pct(values, p):
    """Nearest-rank percentile.  Returns None for an empty sample."""
    if not values:
        return None
    s = sorted(values)
    k = max(0, min(len(s) - 1, int(math.ceil(p / 100.0 * len(s))) - 1))
    return s[k]


def stamp(t=None):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(t if t is not None else now()))


def http_get(url, timeout=6.0):
    """(elapsed_s, body_str_or_None, error_str_or_None)."""
    t0 = now()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            body = r.read().decode("utf-8", "replace")
        return now() - t0, body, None
    except urllib.error.HTTPError as e:
        return now() - t0, None, "HTTP %s" % e.code
    except Exception as e:  # noqa: BLE001 -- any failure to reach the device is the datum
        return now() - t0, None, type(e).__name__ + ": " + str(e)


# =============================================================================
# Background monitors
# =============================================================================

class HttpMonitor(threading.Thread):
    """Polls /api/status.  This is the "is the WebUI reachable" metric, and it
    runs for the whole harness lifetime so every scenario has HTTP health
    attributable to its own time window."""

    def __init__(self, ip, interval=2.0, timeout=6.0):
        super().__init__(daemon=True)
        self.url = "http://%s/api/status" % ip
        self.interval = interval
        self.timeout = timeout
        self._stop = threading.Event()
        self.lock = threading.Lock()
        self.samples = []      # [(t, latency_s, ok_bool, err_or_None)]
        # REBOOT DETECTION. /api/status already carries uptime_ms, so watching
        # it cost nothing and is worth everything: a device that restarts
        # mid-run resets every device-side counter, and without this the
        # differences come out as negative deltas and impossible percentages
        # instead of "the device restarted at 18:31:04".
        self.reboots = []      # [(t, prev_uptime_s, new_uptime_s)]
        self._last_uptime = None

    def run(self):
        while not self._stop.is_set():
            dt, body, err = http_get(self.url, self.timeout)
            t = now()
            up = None
            if body:
                try:
                    up = json.loads(body)["uptime_ms"] / 1000.0
                except (ValueError, KeyError):
                    up = None
            with self.lock:
                self.samples.append((t, dt, err is None and bool(body), err))
                if up is not None:
                    if self._last_uptime is not None and up < self._last_uptime - 1.0:
                        self.reboots.append((t, self._last_uptime, up))
                    self._last_uptime = up
            self._stop.wait(self.interval)

    def stop(self):
        self._stop.set()

    def window(self, t0, t1):
        with self.lock:
            rows = [s for s in self.samples if t0 <= s[0] <= t1]
            reboots = [r for r in self.reboots if t0 <= r[0] <= t1]
        lat = [r[1] for r in rows if r[2]]
        fails = [r for r in rows if not r[2]]
        return {
            "polls": len(rows),
            "failures": len(fails),
            "device_reboots": len(reboots),
            "device_reboot_times": [stamp(r[0]) for r in reboots],
            "failure_reasons": sorted({r[3] for r in fails})[:6],
            "latency_p50_ms": None if not lat else round(pct(lat, 50) * 1000, 1),
            "latency_p95_ms": None if not lat else round(pct(lat, 95) * 1000, 1),
            "latency_max_ms": None if not lat else round(max(lat) * 1000, 1),
        }


HEAP_RE = re.compile(r"heap free=(\d+) min=(\d+) maxblock=(\d+) psram=(\d+)")
LOGLINE_RE = re.compile(r"^\[\s*([0-9.]+)\s+([TDIWEF])\s+([a-zA-Z0-9_.-]+)\]\s*(.*)$")
# Firmware messages that are direct evidence for/against the transport claim.
EVIDENCE_PAT = re.compile(
    r"send stalled|stalled >|RX ring full|dropped RX frame|blocked \d+ms|STALL|"
    r"deadman|evicted|WS client#\d+ disconnected|session .* left|heap|panic|abort|"
    r"Guru Meditation|watchdog", re.I)


class LogMonitor(threading.Thread):
    """Polls /api/log (a full ring dump each call) and de-duplicates.

    Two products: the heap trend (from the `[sys] heap ...` beacon, ~10 s) and
    the firmware's own warnings.  The ring EVICTS, so the poll interval must
    stay well under the ring's turnover -- 6 s here against a beacon every
    10 s, with the de-dup set doing the rest."""

    def __init__(self, ip, interval=6.0, timeout=8.0):
        super().__init__(daemon=True)
        self.ip = ip
        self.url = "http://%s/api/log" % ip
        self.interval = interval
        self.timeout = timeout
        self._stop = threading.Event()
        self.lock = threading.Lock()
        self.seen = set()
        self.heap = []        # [(t, free, min, maxblock, psram)]
        self.evidence = []    # [(t, line)]
        self.poll_failures = 0
        # DEVICE-TIME ATTRIBUTION. Every log line is stamped with the device's
        # own uptime, not wall clock; attributing lines to a scenario by the
        # moment WE happened to poll smears them across up to `interval`
        # seconds and puts a warning in the wrong test. One uptime<->wallclock
        # anchor fixes that for the whole run.
        self.t_anchor = None
        self.uptime_anchor = None

    def anchor(self):
        _dt, body, err = http_get("http://%s/api/status" % self.ip, self.timeout)
        t = now()
        if err or not body:
            return False
        try:
            self.uptime_anchor = json.loads(body)["uptime_ms"] / 1000.0
        except (ValueError, KeyError):
            return False
        self.t_anchor = t
        return True

    def _wall(self, device_s, fallback):
        if self.t_anchor is None or device_s is None:
            return fallback
        return self.t_anchor + (device_s - self.uptime_anchor)

    def poll_once(self):
        _dt, body, err = http_get(self.url, self.timeout)
        if err or not body:
            with self.lock:
                self.poll_failures += 1
            return
        t_poll = now()
        with self.lock:
            for raw in body.splitlines():
                line = raw.rstrip()
                if not line or line in self.seen:
                    continue
                self.seen.add(line)
                lm = LOGLINE_RE.match(line)
                t = self._wall(float(lm.group(1)) if lm else None, t_poll)
                m = HEAP_RE.search(line)
                if m:
                    self.heap.append((t, int(m.group(1)), int(m.group(2)),
                                      int(m.group(3)), int(m.group(4)),
                                      float(lm.group(1)) if lm else None))
                lvl = lm.group(2) if lm else "?"
                if lvl in ("W", "E", "F") or EVIDENCE_PAT.search(line):
                    self.evidence.append((t, line))
            if len(self.seen) > 20000:
                self.seen = set(list(self.seen)[-8000:])

    def run(self):
        while not self._stop.is_set():
            try:
                self.poll_once()
            except Exception:  # noqa: BLE001 -- a monitor must never kill the run
                with self.lock:
                    self.poll_failures += 1
            self._stop.wait(self.interval)

    def stop(self):
        self._stop.set()

    def heap_summary(self):
        with self.lock:
            rows = list(self.heap)
        if not rows:
            return {"samples": 0}
        free = [r[1] for r in rows]

        # LEAK TREND -- the one number here that can lie loudly.
        #
        # A least-squares fit over the whole run is wrong twice over. It is
        # dominated by (a) the first minute after boot, when free heap falls
        # steeply as tasks, WiFi and the web stack settle -- that is startup,
        # not a leak -- and (b) any REBOOT inside the window, which teleports
        # free heap back up and turns a flat run into a confident downward
        # slope. The first baseline run reported "-1629 B/min, leak" purely
        # from those two artifacts.
        #
        # So: split the series wherever the device's own uptime goes backwards
        # (that IS a reboot, detected from the log's own timestamps and needing
        # nothing else), drop the first 60 s of each segment, and fit the
        # longest surviving segment. Fewer samples, but an honest number.
        segments, cur = [], []
        prev_dev = None
        for r in rows:
            dev = r[5]
            if prev_dev is not None and dev is not None and dev < prev_dev - 1.0:
                segments.append(cur); cur = []
            prev_dev = dev if dev is not None else prev_dev
            cur.append(r)
        segments.append(cur)
        best, slope = [], None
        for seg in segments:
            usable = [r for r in seg if r[5] is None or r[5] >= 60.0]
            if len(usable) > len(best):
                best = usable
        med_first = med_last = spread = None
        leak = None
        if len(best) >= 3:
            slope = _slope([r[0] for r in best], [r[1] for r in best])
            # SLOPE ALONE IS NOT ENOUGH. Free heap on this device swings
            # ~38-58 KB minute to minute as sockets, the log ring and the web
            # stack breathe, and a least-squares line through that noise will
            # confidently report "leak" on a run whose first and last samples
            # are both HIGHER than the middle. (Observed: -2596 B/min on a
            # 5-minute soak that ended +3504 B up.)
            #
            # So a leak has to show up as the DISTRIBUTION moving down, not
            # just a line sloping down: the last third's median must sit below
            # the first third's by more than the run's own p10-p90 spread.
            # Noise cancels in the medians; a real leak does not.
            vals = [r[1] for r in best]
            k = max(1, len(vals) // 3)
            med_first = pct(vals[:k], 50)
            med_last = pct(vals[-k:], 50)
            spread = (pct(vals, 90) or 0) - (pct(vals, 10) or 0)
            leak = bool(slope is not None and slope < 0
                        and med_last < med_first - spread)
        return {
            "samples": len(rows),
            "free_first": free[0], "free_last": free[-1],
            "free_min": min(free), "free_max": max(free),
            "free_delta": free[-1] - free[0],
            "min_watermark_last": rows[-1][2],
            "maxblock_last": rows[-1][3],
            "psram_last": rows[-1][4],
            "reboot_segments": len(segments),
            "trend_samples": len(best),
            "trend_span_min": round((best[-1][0] - best[0][0]) / 60.0, 1) if len(best) >= 2 else None,
            "free_slope_bytes_per_min": slope,
            "median_first_third": med_first,
            "median_last_third": med_last,
            "noise_p10_p90": spread,
            "leak_indicated": leak,
        }

    def heap_window(self, t0, t1):
        """Heap inside one scenario's window. Per-scenario rather than only
        run-wide because the interesting number is not the average -- it is
        which specific abuse drove free heap toward the floor."""
        with self.lock:
            rows = [r for r in self.heap if t0 <= r[0] <= t1]
        if not rows:
            return {"samples": 0}
        return {
            "samples": len(rows),
            "free_min": min(r[1] for r in rows),
            "free_max": max(r[1] for r in rows),
            "free_last": rows[-1][1],
            "watermark_min": min(r[2] for r in rows),
            "maxblock_min": min(r[3] for r in rows),
        }

    def evidence_window(self, t0, t1, limit=40):
        with self.lock:
            rows = [e for e in self.evidence if t0 <= e[0] <= t1]
        return [e[1] for e in rows[:limit]]

    def evidence_counts(self, t0=None, t1=None):
        with self.lock:
            rows = list(self.evidence)
        if t0 is not None:
            rows = [e for e in rows if t0 <= e[0] <= t1]
        buckets = {}
        for _t, line in rows:
            for key, pat in (("send_stalled", "send stalled"),
                             ("stall_evicted", "stalled >"),
                             ("rx_ring_full", "RX ring full"),
                             ("rx_frame_dropped", "dropped RX frame"),
                             ("http_blocked", "blocked "),
                             ("deadman_stop", "deadman"),
                             ("ws_disconnect", "WS client#")):
                if pat in line:
                    buckets[key] = buckets.get(key, 0) + 1
        return buckets


class UiWsMonitor(threading.Thread):
    """Passive observer of the WEBUI's OWN telemetry socket (ws://ip:81/ws/ui).

    Why this is here even though the migration is about the SlopSync plane
    (:82): the operator's actual reported symptom is "the WebUI says
    unreachable", and the WebUI does not use SlopSync for that -- it uses this
    separate legacy links2004 WebSocketsServer with its own sender task. A
    harness that only watched :82 could report a spotless run while the thing
    the user complains about was dropping every two minutes.

    It mimics link.js exactly as far as liveness goes: connect, wait for the
    0x00 HELLO, then send a 0x03 CLOCK every 2 s and count everything that
    comes back. link.js declares the UI 'degraded' after two failures inside
    10 s, so consecutive reconnects are the metric that matches what the user
    sees on screen.

    It never sends a 0x10 CMD. This observer must not be able to move anything."""

    HELLO, CLOCK = 0x00, 0x03
    CLOCK_INTERVAL_S = 2.0

    def __init__(self, ip, port=81, path="/ws/ui"):
        super().__init__(daemon=True)
        self.url = "ws://%s:%d%s" % (ip, port, path)
        self._stop = threading.Event()
        self.lock = threading.Lock()
        self.frames = []        # [(t, frame_type)]
        self.events = []        # [(t, "connected"|"hello"|"disconnected: ...")]
        self.connects = 0
        self.disconnects = 0
        self.connect_failures = 0

    def _note(self, kind):
        with self.lock:
            self.events.append((now(), kind))

    def run(self):
        backoff = 0.5
        while not self._stop.is_set():
            ws = None
            try:
                ws = websocket.create_connection(self.url, timeout=5.0)
                self.connects += 1
                self._note("connected")
                backoff = 0.5
                last_clock = 0.0
                while not self._stop.is_set():
                    if now() - last_clock > self.CLOCK_INTERVAL_S:
                        ws.send(struct.pack("<BI", self.CLOCK, sp.client_now_us()),
                                opcode=websocket.ABNF.OPCODE_BINARY)
                        last_clock = now()
                    ws.settimeout(0.25)
                    try:
                        opcode, data = ws.recv_data()
                    except websocket.WebSocketTimeoutException:
                        continue
                    if opcode == websocket.ABNF.OPCODE_CLOSE or not data:
                        raise websocket.WebSocketConnectionClosedException("close frame")
                    if opcode == websocket.ABNF.OPCODE_BINARY:
                        with self.lock:
                            self.frames.append((now(), data[0]))
                            if data[0] == self.HELLO:
                                self.events.append((now(), "hello"))
            except Exception as e:  # noqa: BLE001 -- every failure mode is a datum
                if not self._stop.is_set():
                    self.disconnects += 1
                    self._note("disconnected: %s: %s" % (type(e).__name__, e))
            finally:
                try:
                    if ws:
                        ws.close()
                except Exception:  # noqa: BLE001
                    pass
            if self._stop.is_set():
                break
            self._stop.wait(backoff)
            backoff = min(5.0, backoff * 2)   # link.js's own backoff shape

    def stop(self):
        self._stop.set()

    def window(self, t0, t1):
        with self.lock:
            fr = [f for f in self.frames if t0 <= f[0] <= t1]
            ev = [e for e in self.events if t0 <= e[0] <= t1]
        drops = [e for e in ev if e[1].startswith("disconnected")]
        gaps = [fr[i][0] - fr[i - 1][0] for i in range(1, len(fr))]
        return {
            "frames": len(fr),
            "frame_hz": round(len(fr) / (t1 - t0), 2) if t1 > t0 else None,
            "reconnects": sum(1 for e in ev if e[1] == "connected"),
            "drops": len(drops),
            "drop_reasons": [e[1] for e in drops][:6],
            "max_gap_ms": round(max(gaps) * 1000, 1) if gaps else None,
        }


def _slope(xs, ys):
    """Least-squares slope in units/minute.  None when it cannot be computed."""
    n = len(xs)
    if n < 3:
        return None
    mx = sum(xs) / n
    my = sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    if den <= 0:
        return None
    return round(sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den * 60.0, 1)


# =============================================================================
# The soak client
# =============================================================================

DEFAULT_SUBS = [
    (sp.CH_SAFETY, 0.0, "critical"),
    (sp.CH_MOTION, 20.0, "elevated"),
    (sp.CH_PLAN_STRIP, 20.0, "elevated"),
    (sp.CH_ODOMETER, 1.0, "background"),
    (sp.CH_MOTION_DIAG, 1.0, "background"),
]

PING_INTERVAL_S = 0.4     # 6.5 silence-based keepalive ceiling; refined from WELCOME
STALL_FLOOR_S = 1.0       # an arrival gap under this is never called a stall
SILENCE_FAIL_S = 5.0      # socket open but no frames this long == a dropout


class SoakClient:
    """One SlopSync session, driven on its own thread.

    Deliberately NOT a subclass of anything in slopsync_probe: the probe's
    session is a scripted linear script with global narration state, which is
    useless when three of them have to run at once.  Only the wire helpers are
    shared."""

    def __init__(self, ctx, name, sub_rate=20.0, publishes=None, rcvbuf=None,
                 label=None, subs=None):
        self.ctx = ctx
        self.name = name
        self.label = label or name
        self.sub_rate = sub_rate
        self.subs = subs or DEFAULT_SUBS
        self.publishes = publishes or []     # [(channel_id, rate_hz)]
        self.rcvbuf = rcvbuf
        self._plan_raw = None
        self._last_send = 0.0
        self.deadman_ms = None
        self.ping_interval = PING_INTERVAL_S
        self._last_rx = None
        self.silence_fail_s = SILENCE_FAIL_S

        self.ws = None
        self.thread = None
        self.instance_id = os.urandom(8)
        self.session_id = None
        self.boot_id = None
        self.etag = None
        self.granted = {}                    # channel -> granted rate
        self.granted_publishes = {}
        self.hub_offset_us = None

        # --- control flags (set by scenarios, read by the loop) -------------
        self._stop = threading.Event()
        self.wedged = False                  # stop reading the socket
        self.wedge_chatty = True             # ...but keep sending PINGs
        self.expect_connected = False        # a disconnect right now is a FAILURE
        self.deliberate_teardown = False
        # Set on clients we are DELIBERATELY abusing (wedge victims, RST
        # subjects). Their disconnects are the test working, not a dropout --
        # keeping that distinction in the data, not in the reader's head, is
        # what stops the baseline being quietly inflated.
        self.is_pathological = False

        # --- metrics --------------------------------------------------------
        self.m = {
            "name": self.label,
            "connects": 0,
            "handshake_failures": 0,
            "unexpected_disconnects": 0,
            "expected_disconnects": 0,
            "disconnect_events": [],         # [(t, reason, expected_bool)]
            "frames_rx": 0,
            "bytes_rx": 0,
            "by_frame_type": {},
            "nacks": {},                     # nack name -> count
            "state": {},                     # channel -> stats dict
            "stream_bundles_sent": 0,
            "errors": [],
        }
        self.lock = threading.Lock()

    # ---------------------------------------------------------------- wire --

    def _sock_opts(self):
        opts = []
        if self.rcvbuf:
            # A small receive buffer is what makes the wedge bite in seconds
            # rather than minutes: the zero-window is reached once the kernel
            # buffer fills, and at ~20-60 Hz of ~20-byte frames a default
            # 64 KB buffer would take a minute-plus.
            opts.append((socket.SOL_SOCKET, socket.SO_RCVBUF, self.rcvbuf))
        return opts

    def connect(self, timeout=6.0):
        url = "ws://%s:%d/" % (self.ctx.args.ip, self.ctx.args.port)
        self.ws = websocket.create_connection(
            url, subprotocols=[sp.WS_SUBPROTOCOL], timeout=timeout,
            sockopt=self._sock_opts() or None)
        self.m["connects"] += 1
        self._last_send = now()
        return self.ws

    def send(self, ftype, channel, payload, seq=0):
        self.ws.send(sp.encode_frame(ftype, channel, payload, seq),
                     opcode=websocket.ABNF.OPCODE_BINARY)
        self._last_send = now()

    def recv(self, deadline):
        """(hdr, payload) or None on timeout/close.  Records every frame."""
        while True:
            remaining = deadline - now()
            if remaining <= 0:
                return None
            self.ws.settimeout(min(remaining, 0.25))
            try:
                opcode, data = self.ws.recv_data()
            except websocket.WebSocketTimeoutException:
                return "timeout"
            except (websocket.WebSocketConnectionClosedException, OSError):
                return None
            if opcode == websocket.ABNF.OPCODE_CLOSE:
                return None
            if opcode != websocket.ABNF.OPCODE_BINARY:
                continue
            hdr = sp.decode_frame_header(data)
            if hdr is None:
                continue
            payload = data[8:8 + hdr["len"]]
            self._record(hdr, payload, len(data))
            return hdr, payload

    def _record(self, hdr, payload, nbytes):
        t = now()
        if hdr["type"] == sp.FRAME["STATE"] and hdr["channel"] == sp.CH_PLAN_STRIP:
            # The streaming scenario needs the machine's CURRENT normalized
            # position so it can hold there instead of guessing a target.
            try:
                self._plan_raw = sp.decode_plan_strip(payload)["cur"]
            except (ValueError, struct.error):
                pass
        with self.lock:
            self.m["frames_rx"] += 1
            self.m["bytes_rx"] += nbytes
            fname = sp.FRAME_NAME.get(hdr["type"], "0x%02X" % hdr["type"])
            self.m["by_frame_type"][fname] = self.m["by_frame_type"].get(fname, 0) + 1
            if hdr["type"] == sp.FRAME["NACK"]:
                try:
                    code = sp.cb_decode_full(payload).get(sp.K["code"])
                except ValueError:
                    code = None
                nm = sp.nack_name(code)
                self.m["nacks"][nm] = self.m["nacks"].get(nm, 0) + 1
            if hdr["type"] == sp.FRAME["STATE"]:
                st = self.m["state"].setdefault(hdr["channel"], {
                    "count": 0, "first_t": t, "last_t": None, "last_seq": None,
                    "seq_regressions": 0, "seq_repeats": 0, "max_seq_step": 0,
                    "max_gap_s": 0.0, "stalls": [], "arrival_gaps": [],
                })
                if st["last_t"] is not None:
                    gap = t - st["last_t"]
                    st["arrival_gaps"].append(gap)
                    if gap > st["max_gap_s"]:
                        st["max_gap_s"] = gap
                st["last_t"] = t
                st["count"] += 1
                seq = hdr["seq"]
                if st["last_seq"] is not None:
                    d = _u16_delta(seq, st["last_seq"])
                    if d == 0:
                        st["seq_repeats"] += 1
                    elif d < 0:
                        st["seq_regressions"] += 1
                    else:
                        st["max_seq_step"] = max(st["max_seq_step"], d)
                st["last_seq"] = seq

    # ----------------------------------------------------------- handshake --

    def handshake(self, timeout=6.0, subs=None):
        """HELLO -> WELCOME -> CATALOG_READY -> SUBSCRIBE -> GRANT.
        Returns True on a fully granted session."""
        pubs = self.publishes or None
        # AUTH (fw 2.1.59+): only spend a /uitoken mint when this client
        # actually needs controller tier — i.e. when it PUBLISHES a stream.
        # Pure watchers (the wedge/soak telemetry clients) are granted their
        # subscriptions at `watch` and do not need one.
        #
        # This distinction is not just tidiness: the mint is rate-limited to one
        # per 250 ms DEVICE-WIDE, and the churn scenario opens 30 sessions as
        # fast as it can. Minting for every one of them would turn a rate limit
        # into a test failure and blame the device for it.
        token = None
        if pubs:
            token = sp.mint_uitoken(self.ctx.args.ip)
            if token is None:
                self._note_error("no /uitoken — publisher will be watch tier")
        self.send(sp.FRAME["HELLO"], 0,
                  sp.build_hello("soak", "slopsoak.py/%s" % self.label,
                                 self.instance_id, publishes=pubs, token=token))
        deadline = now() + timeout
        w = None
        while now() < deadline:
            got = self.recv(deadline)
            if got is None:
                self._note_error("connection closed during HELLO")
                return False
            if got == "timeout":
                continue
            hdr, payload = got
            if hdr["type"] == sp.FRAME["WELCOME"]:
                w = sp.cb_decode_full(payload)
                break
            if hdr["type"] == sp.FRAME["NACK"]:
                self._note_error("HELLO NACKed: %s" %
                                 sp.nack_name(sp.cb_decode_full(payload).get(sp.K["code"])))
                return False
        if w is None:
            self._note_error("no WELCOME within %.1fs" % timeout)
            return False

        self.session_id = w.get(sp.K["session_id"])
        self.boot_id = w.get(sp.K["boot_id"])
        self.etag = w.get(sp.K["catalog_etag"])
        # Ping at a third of whatever liveness window the hub actually declared,
        # rather than a constant we guessed. A hub that tightens its deadman in
        # a later firmware must not silently turn this harness into a source of
        # fake dropouts.
        self.deadman_ms = w.get(sp.K["deadman_ms"])
        if self.deadman_ms:
            self.ping_interval = max(0.1, min(PING_INTERVAL_S, self.deadman_ms / 3000.0))
        for gp in w.get(sp.K["granted_publishes"], []) or []:
            self.granted_publishes[gp.get(sp.K["channel_id"])] = gp.get(sp.K["granted_rate_hz"])

        if not isinstance(self.etag, bytes) or len(self.etag) != 8:
            self._note_error("WELCOME carried no usable catalog_etag")
            return False
        # 8.4 readiness gate: no STATE, no STREAM, no INTENT until we declare
        # which catalog we decode against.
        self.send(sp.FRAME["CATALOG_READY"], 0, self.etag)

        wishes = []
        for ch, rate, prio in (subs or self.subs):
            # rate 0.0 means "on change" (safety); otherwise the client's own
            # sub_rate is the wish, capped by what the channel is worth asking
            # for.  The hub clamps to the catalog ceiling regardless.
            wishes.append((ch, 0.0 if rate <= 0 else min(self.sub_rate, rate),
                           sp.PRIORITY[prio]))
        self.send(sp.FRAME["SUBSCRIBE"], 0, sp.build_subscribe(wishes))

        pending = {ch for ch, _, _ in wishes}
        deadline = now() + timeout
        while pending and now() < deadline:
            got = self.recv(deadline)
            if got is None:
                self._note_error("connection closed during SUBSCRIBE")
                return False
            if got == "timeout":
                continue
            hdr, payload = got
            if hdr["type"] == sp.FRAME["GRANT"]:
                for e in sp.cb_decode_full(payload).get(sp.K["grants"], []):
                    ch = e.get(sp.K["channel_id"])
                    self.granted[ch] = e.get(sp.K["granted_rate_hz"])
                    pending.discard(ch)
            elif hdr["type"] == sp.FRAME["NACK"]:
                n = sp.cb_decode_full(payload)
                pending.discard(n.get(sp.K["channel_id"]))
        if pending:
            self._note_error("channels never granted: %s" %
                             ["0x%04X" % c for c in sorted(pending)])
            return False
        return True

    def clock_sync(self, timeout=3.0):
        """One 7.1 CLOCK exchange -> hub-time offset for STREAM t_base."""
        t0 = sp.client_now_us()
        self.send(sp.FRAME["CLOCK"], 0, sp.CLOCK_REQUEST_STRUCT.pack(t0))
        deadline = now() + timeout
        while now() < deadline:
            got = self.recv(deadline)
            if got is None:
                return False
            if got == "timeout":
                continue
            hdr, payload = got
            if hdr["type"] == sp.FRAME["CLOCK"] and len(payload) >= sp.CLOCK_REPLY_STRUCT.size:
                t3 = sp.client_now_us()
                t0e, t1, t2 = sp.CLOCK_REPLY_STRUCT.unpack_from(payload, 0)
                self.hub_offset_us = (sp.wrap_diff(t1, t0e) + sp.wrap_diff(t2, t3)) // 2
                return True
        return False

    # ------------------------------------------------------------- run loop --

    def start_loop(self, streamer=None):
        """Run the read/keepalive loop on a thread until stop() is called.
        `streamer` is an optional callable(client) invoked on the loop's own
        cadence -- used by the streaming scenario so STREAM frames and the read
        path share one thread (and therefore one socket, no locking)."""
        self.expect_connected = True
        self.thread = threading.Thread(target=self._loop, args=(streamer,), daemon=True)
        self.thread.start()

    def _loop(self, streamer):
        try:
            while not self._stop.is_set():
                if self.wedged:
                    # THE WEDGE: we do not read.  The kernel buffer fills, the
                    # advertised window closes, and the hub's next write has
                    # nowhere to go.  In `chatty` mode we still talk, which on
                    # the links2004 transport re-clears the send-stall mute on
                    # every inbound frame and re-arms the blocking write.
                    if self.wedge_chatty and (now() - self._last_send) > self.ping_interval:
                        try:
                            self.send(sp.FRAME["PING"], 0, b"")
                        except Exception as e:  # noqa: BLE001
                            self._disconnected("send failed while wedged: %s" % e)
                            return
                    time.sleep(0.05)
                    continue

                if streamer is not None:
                    try:
                        streamer(self)
                    except Exception as e:  # noqa: BLE001
                        self._disconnected("stream send failed: %s" % e)
                        return

                # KEEPALIVE FIRST, and unconditionally on a timer.
                # 6.5 liveness is one-directional: frames arriving FROM the hub
                # prove nothing about us, so a client that only pings when its
                # read times out will never ping at all while telemetry is
                # flowing -- and the hub reaps it mid-stream. That bug lived in
                # this file for one afternoon and produced a "the whole hub
                # dies after 3 seconds" false positive. Do not move this back
                # inside the timeout branch.
                if (now() - self._last_send) > self.ping_interval:
                    try:
                        self.send(sp.FRAME["PING"], 0, b"")
                    except Exception as e:  # noqa: BLE001
                        self._disconnected("keepalive send failed: %s" % e)
                        return

                got = self.recv(now() + (0.01 if streamer is not None else 0.1))
                if got is None:
                    self._disconnected("socket closed by peer")
                    return
                if got != "timeout":
                    self._last_rx = now()
                elif (self._last_rx and not self.wedged
                      and (now() - self._last_rx) > self.silence_fail_s):
                    # SOCKET ALIVE, SESSION DEAD. The hub can tear a session
                    # down (deadman, eviction, an internal fault) while the TCP
                    # connection stays perfectly open, and a client that only
                    # watches for a close event will sit there forever showing
                    # stale data. To a user that IS a dropout, so it is scored
                    # as one.
                    self._disconnected("session went silent for %.1fs with the socket still open"
                                       % (now() - self._last_rx))
                    return
        except Exception as e:  # noqa: BLE001
            self._disconnected("loop exception: %s: %s" % (type(e).__name__, e))

    def _disconnected(self, reason):
        expected = (self.deliberate_teardown or self.is_pathological
                    or not self.expect_connected)
        with self.lock:
            self.m["disconnect_events"].append(
                {"t": stamp(), "reason": reason, "expected": expected})
            if expected:
                self.m["expected_disconnects"] += 1
            else:
                self.m["unexpected_disconnects"] += 1
        self.expect_connected = False

    def _note_error(self, msg):
        with self.lock:
            self.m["handshake_failures"] += 1
            self.m["errors"].append("%s %s" % (stamp(), msg))

    def stop(self, join=True):
        self._stop.set()
        if join and self.thread is not None:
            self.thread.join(timeout=5.0)

    # ------------------------------------------------------------ teardown --

    def close_clean(self):
        self.deliberate_teardown = True
        self.expect_connected = False
        try:
            self.send(sp.FRAME["GOODBYE"], 0, sp.build_goodbye(0x0107))  # NORMAL_CLOSURE
        except Exception:  # noqa: BLE001
            pass
        try:
            self.ws.close()
        except Exception:  # noqa: BLE001
            pass

    def close_rst(self):
        """Abrupt death: SO_LINGER {on=1, linger=0} makes close() emit a TCP
        RST instead of a FIN.  No GOODBYE, no close frame -- the hub learns
        about it from the socket layer, which is exactly the path that once
        leaked source ownership forever."""
        self.deliberate_teardown = True
        self.expect_connected = False
        try:
            s = self.ws.sock
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            s.close()
        except Exception as e:  # noqa: BLE001
            with self.lock:
                self.m["errors"].append("RST close failed: %s" % e)

    def snapshot(self):
        with self.lock:
            shallow = {k: v for k, v in self.m.items() if k != "state"}
            out = json.loads(json.dumps(shallow, default=_jsonable))
            states = {ch: dict(st, arrival_gaps=list(st["arrival_gaps"]))
                      for ch, st in self.m["state"].items()}
        # Post-process per-channel arrival stats into something diffable.
        # The raw per-frame gap list is DELIBERATELY not exported: a 12-minute
        # 3-client soak produces ~45k floats per channel and nobody diffs those.
        st_out = {}
        for ch, st in states.items():
            gaps = st["arrival_gaps"]
            granted = self.granted.get(ch)
            thresh = STALL_FLOOR_S
            if granted:
                thresh = max(STALL_FLOOR_S, 5.0 / granted)
            span = (st["last_t"] - st["first_t"]) if st["last_t"] else 0.0
            st_out["0x%04X" % ch] = {
                "name": sp.CHANNEL_NAMES.get(ch, "?"),
                "granted_hz": granted,
                "frames": st["count"],
                "measured_hz": round((st["count"] - 1) / span, 2) if span > 0.5 else None,
                "gap_p95_ms": None if not gaps else round(pct(gaps, 95) * 1000, 1),
                "gap_max_ms": round(st["max_gap_s"] * 1000, 1),
                "stalls_over_%dms" % int(thresh * 1000):
                    sum(1 for g in gaps if g > thresh),
                "seq_regressions": st["seq_regressions"],
                "seq_repeats": st["seq_repeats"],
                "max_seq_step": st["max_seq_step"],
                # A channel that went silent and STAYED silent never records a
                # closing gap -- the last arrival has no successor to be
                # measured against. Without this the worst possible outcome
                # (telemetry stopped and never came back) reads as a clean run.
                "quiet_for_s": round(now() - st["last_t"], 1) if st["last_t"] else None,
            }
        out["state"] = st_out
        out["session_id"] = self.session_id
        out["deadman_ms"] = self.deadman_ms
        out["ping_interval_s"] = round(self.ping_interval, 3)
        out["granted"] = {"0x%04X" % k: v for k, v in self.granted.items()}
        out["granted_publishes"] = {"0x%04X" % k: v for k, v in self.granted_publishes.items()}
        return out


def _u16_delta(a, b):
    """Wrap-aware u16 difference a-b, nearest representative in [-32768, 32768)."""
    d = (a - b) & 0xFFFF
    return d - 0x10000 if d >= 0x8000 else d


def _jsonable(o):
    if isinstance(o, (bytes, bytearray)):
        return o.hex()
    return str(o)


# =============================================================================
# Context
# =============================================================================

class Ctx:
    def __init__(self, args):
        self.args = args
        self.http = HttpMonitor(args.ip, interval=args.http_interval)
        self.log = LogMonitor(args.ip, interval=args.log_interval)
        self.uiws = UiWsMonitor(args.ip, port=args.uiws_port) if args.watch_uiws else None
        self.results = []

    def start_monitors(self):
        self.log.anchor()          # device-uptime <-> wall-clock, once
        self.http.start()
        self.log.start()
        if self.uiws:
            self.uiws.start()

    def stop_monitors(self):
        self.http.stop()
        self.log.stop()
        if self.uiws:
            self.uiws.stop()

    def envelope(self, name, t0, t1, findings, detail):
        http = self.http.window(t0, t1)
        uw = self.uiws.window(t0, t1) if self.uiws else None
        if uw and uw["drops"]:
            findings = findings + [finding(
                "fail", "the WEBUI's own :81 link dropped %d time(s) during this scenario (%s) -- "
                        "this is the exact symptom the operator reports"
                        % (uw["drops"], "; ".join(uw["drop_reasons"])))]
        if http["device_reboots"]:
            # Loud, and prepended: every other number in this scenario is
            # measured across a device restart and is therefore suspect.
            findings = [finding("fail", "THE DEVICE REBOOTED %d time(s) during this scenario (%s) "
                                "-- device-side counters reset, so every delta below is "
                                "untrustworthy" % (http["device_reboots"],
                                                   ", ".join(http["device_reboot_times"])))] + findings
        return {
            "scenario": name,
            "started": stamp(t0), "ended": stamp(t1),
            "duration_s": round(t1 - t0, 1),
            "http": http,
            "webui_ws": self.uiws.window(t0, t1) if self.uiws else None,
            "heap": self.log.heap_window(t0, t1),
            "device_log": self.log.evidence_counts(t0, t1),
            "device_log_lines": self.log.evidence_window(t0, t1),
            "findings": findings,
            "detail": detail,
            "verdict": "FAIL" if any(f["severity"] == "fail" for f in findings) else
                       ("WARN" if any(f["severity"] == "warn" for f in findings) else "PASS"),
        }


def finding(severity, text):
    return {"severity": severity, "text": text}


def make_client(ctx, name, **kw):
    kw.setdefault("sub_rate", ctx.args.sub_rate)
    return SoakClient(ctx, name, **kw)


def teardown(client, mode="clean"):
    """Stop the read loop FIRST (so nothing races the socket), then close it
    the requested way.  `mode`: clean | rst | abandon."""
    client.deliberate_teardown = True
    client.expect_connected = False
    client.stop()
    if mode == "rst":
        client.close_rst()
    elif mode == "abandon":
        try:
            client.ws.close()
        except Exception:  # noqa: BLE001
            pass
    else:
        client.close_clean()


def bring_up(ctx, name, **kw):
    """Connect + handshake.  Returns (client, ok); a failure is already
    recorded on the client's own metrics, so the caller can just report it."""
    c = make_client(ctx, name, **kw)
    try:
        c.connect(timeout=ctx.args.timeout)
    except Exception as e:  # noqa: BLE001
        c._note_error("connect failed: %s" % e)
        return c, False
    ok = c.handshake(timeout=ctx.args.timeout)
    return c, ok


# =============================================================================
# Scenarios
# =============================================================================

def scenario_b2b(ctx):
    """CLAUDE.md 8: back-to-back sessions with NO reboot between them.  Three
    consecutive FULL sessions -- each one handshakes, takes the motion-input
    publish grant (the resource the ownership leak used to strand), watches
    telemetry, and departs.  Session 2 and 3 failing to get the grant is the
    exact signature of the teardown leak."""
    t0 = now()
    n = ctx.args.b2b_sessions
    detail = []
    findings = []
    grant_ok = 0
    for i in range(n):
        mode = ["clean", "rst", "abandon"][i % 3]
        c, ok = bring_up(ctx, "b2b%d" % (i + 1),
                         publishes=[(sp.CH_MOTION_INPUT, 50.0)])
        row = {"session": i + 1, "teardown": mode, "handshake": ok,
               "session_id": c.session_id,
               "motion_input_grant": c.granted_publishes.get(sp.CH_MOTION_INPUT)}
        if ok:
            if row["motion_input_grant"]:
                grant_ok += 1
            c.start_loop()
            time.sleep(ctx.args.b2b_dwell)
            row["frames_rx"] = c.m["frames_rx"]
            row["unexpected_disconnects"] = c.m["unexpected_disconnects"]
            teardown(c, mode)
        detail.append(row)
        # Deliberately SHORT: the point is that the hub reclaims the source
        # without needing a grace period, not that it eventually does.
        time.sleep(1.0)

    if grant_ok == n:
        findings.append(finding("pass", "all %d consecutive sessions were granted "
                                "motion-input (0x0084) -- no stranded source ownership" % n))
    else:
        findings.append(finding("fail", "only %d/%d consecutive sessions got the motion-input "
                                "grant -- source ownership is leaking across teardown" % (grant_ok, n)))
    bad = [d for d in detail if not d["handshake"]]
    if bad:
        findings.append(finding("fail", "%d/%d sessions failed to handshake" % (len(bad), n)))
    return ctx.envelope("b2b", t0, now(), findings, detail)


def scenario_rst(ctx):
    """Abrupt death, repeated.  Each iteration: full session, brief telemetry,
    then a TCP RST.  A RST is EXPECTED and never counted as a dropout -- what
    is measured is whether the NEXT session comes up clean, and whether the
    device logs anything alarming."""
    t0 = now()
    n = ctx.args.rst_iterations
    detail = []
    fails = 0
    for i in range(n):
        c, ok = bring_up(ctx, "rst%d" % (i + 1), publishes=[(sp.CH_MOTION_INPUT, 50.0)])
        row = {"iteration": i + 1, "handshake": ok, "session_id": c.session_id,
               "motion_input_grant": c.granted_publishes.get(sp.CH_MOTION_INPUT)}
        if ok:
            c.start_loop()
            time.sleep(ctx.args.rst_dwell)
            row["frames_rx"] = c.m["frames_rx"]
            teardown(c, "rst")
        else:
            fails += 1
            row["errors"] = c.m["errors"]
        detail.append(row)
        time.sleep(0.5)

    findings = []
    if fails:
        findings.append(finding("fail", "%d/%d post-RST sessions failed to establish" % (fails, n)))
    else:
        findings.append(finding("pass", "%d RST teardowns, every following session "
                                "established and was granted its channels" % n))
    lost = sum(1 for d in detail if d.get("handshake") and not d.get("motion_input_grant"))
    if lost:
        findings.append(finding("fail", "%d session(s) after a RST could not take the "
                                "motion-input source (ownership stranded)" % lost))
    return ctx.envelope("rst", t0, now(), findings, detail)


def scenario_churn(ctx):
    """Rapid connect/handshake/disconnect cycling.  Hunting slot leaks, heap
    churn and handshake failures under pressure."""
    t0 = now()
    n = ctx.args.churn_iterations
    ok_n = 0
    errs = []
    latencies = []
    for i in range(n):
        c = make_client(ctx, "churn%d" % (i + 1))
        h0 = now()
        try:
            c.connect(timeout=ctx.args.timeout)
            if c.handshake(timeout=ctx.args.timeout):
                ok_n += 1
                latencies.append(now() - h0)
            else:
                errs.extend(c.m["errors"][-1:])
        except Exception as e:  # noqa: BLE001
            errs.append("iter %d: %s" % (i + 1, e))
        teardown(c, "clean")
        time.sleep(ctx.args.churn_gap)

    detail = {
        "iterations": n, "successful": ok_n, "failed": n - ok_n,
        "handshake_p50_ms": None if not latencies else round(pct(latencies, 50) * 1000, 1),
        "handshake_p95_ms": None if not latencies else round(pct(latencies, 95) * 1000, 1),
        "handshake_max_ms": None if not latencies else round(max(latencies) * 1000, 1),
        "errors": errs[:20],
    }
    findings = []
    if ok_n == n:
        findings.append(finding("pass", "%d rapid connect/disconnect cycles, 100%% handshake success" % n))
    elif ok_n >= n * 0.95:
        findings.append(finding("warn", "%d/%d churn cycles handshook (%.1f%%)" % (ok_n, n, 100.0 * ok_n / n)))
    else:
        findings.append(finding("fail", "%d/%d churn cycles handshook (%.1f%%)" % (ok_n, n, 100.0 * ok_n / n)))
    return ctx.envelope("churn", t0, now(), findings, detail)


def scenario_wedge(ctx, mode):
    """The headline test.  Watchers first establish a BASELINE window, then one
    client stops reading its socket, then it resumes.  The measurement is the
    watchers' telemetry continuity across the three windows."""
    t0 = now()
    args = ctx.args
    watchers = []
    for i in range(args.wedge_watchers):
        c, ok = bring_up(ctx, "watch%d" % (i + 1))
        if not ok:
            return ctx.envelope("wedge-%s" % mode, t0, now(),
                                [finding("fail", "watcher %d could not establish: %s"
                                         % (i + 1, c.m["errors"]))], {})
        c.start_loop()
        watchers.append(c)

    # The victim subscribes at the highest rate the catalog will grant and uses
    # a tiny receive buffer, so the zero-window arrives in seconds instead of
    # minutes.
    victim, ok = bring_up(ctx, "wedge-%s" % mode, rcvbuf=args.wedge_rcvbuf,
                          sub_rate=args.wedge_sub_rate)
    if not ok:
        for c in watchers:
            teardown(c, "clean")
        return ctx.envelope("wedge-%s" % mode, t0, now(),
                            [finding("fail", "wedge client could not establish")], {})
    victim.wedge_chatty = (mode == "chatty")
    victim.is_pathological = True
    victim.start_loop()

    # Raw per-phase frame counters, read straight off each watcher's own
    # running total. Deliberately redundant with _window_stats: if the two ever
    # disagree, the gap-reconstruction is wrong and the whole result is
    # suspect. A claim this strong needs a second, dumber witness.
    def raw_counts():
        return [c.m["frames_rx"] for c in watchers]

    # --- window A: baseline -------------------------------------------------
    a0 = now(); raw_a = raw_counts()
    time.sleep(args.wedge_baseline); a1 = now()
    base = _window_stats(watchers, a0, a1)

    # --- window B: wedged ---------------------------------------------------
    victim.wedged = True
    b0 = now(); raw_b = raw_counts()
    time.sleep(args.wedge_hold); b1 = now()
    wedged = _window_stats(watchers, b0, b1)
    victim_alive_at_end = victim.expect_connected
    victim_frames_at_unwedge = victim.m["frames_rx"]

    # --- window C: recovery -------------------------------------------------
    # `expect_connected` cannot tell us much for a SILENT wedge (a client that
    # neither reads nor writes learns nothing), so recovery is judged by
    # whether frames actually resume flowing once we start reading again.
    victim.wedged = False
    c0 = now(); raw_c = raw_counts()
    time.sleep(args.wedge_recover); c1 = now()
    recov = _window_stats(watchers, c0, c1)
    victim_frames_after = victim.m["frames_rx"] - victim_frames_at_unwedge
    victim_recovered = victim.expect_connected

    # --- window D: victim GONE ----------------------------------------------
    # The decisive question if C is dead: is the hub wedged only for as long as
    # the bad socket exists, or is the whole service permanently stuck? Nothing
    # else distinguishes "recoverable stall" from "reboot required".
    teardown(victim, "clean")
    d0 = now(); raw_d = raw_counts()
    time.sleep(args.wedge_after_close); d1 = now()
    after_close = _window_stats(watchers, d0, d1)
    raw_end = raw_counts()

    for c in watchers:
        teardown(c, "clean")

    detail = {
        "mode": mode,
        "wedge_rcvbuf": args.wedge_rcvbuf,
        "windows": {"baseline_s": round(a1 - a0, 1), "wedged_s": round(b1 - b0, 1),
                    "recovery_s": round(c1 - c0, 1),
                    "after_victim_closed_s": round(d1 - d0, 1)},
        "watchers_baseline": base,
        "watchers_during_wedge": wedged,
        "watchers_after_wedge": recov,
        "watchers_after_victim_closed": after_close,
        "raw_frames_rx_per_watcher": {
            "at_baseline_start": raw_a, "at_wedge_start": raw_b,
            "at_resume": raw_c, "at_victim_close": raw_d, "at_end": raw_end,
            "delta_baseline": [y - x for x, y in zip(raw_a, raw_b)],
            "delta_wedged": [y - x for x, y in zip(raw_b, raw_c)],
            "delta_recovery": [y - x for x, y in zip(raw_c, raw_d)],
            "delta_after_close": [y - x for x, y in zip(raw_d, raw_end)],
        },
        "victim": {
            "granted": {"0x%04X" % k: v for k, v in victim.granted.items()},
            "frames_before_wedge": victim_frames_at_unwedge,
            "still_connected_at_end_of_wedge": victim_alive_at_end,
            "frames_received_after_resume": victim_frames_after,
            "still_connected_after_resume": victim_recovered,
            "outcome": ("recovered" if victim_frames_after > 0 else
                        "evicted-or-dead" if not victim_recovered else "silent"),
            "disconnect_events": victim.m["disconnect_events"],
        },
        "http_baseline": ctx.http.window(a0, a1),
        "http_during_wedge": ctx.http.window(b0, b1),
        "watcher_snapshots": [c.snapshot() for c in watchers],
    }

    findings = []
    drops = sum(c.m["unexpected_disconnects"] for c in watchers)
    if drops:
        findings.append(finding("fail", "%d HEALTHY watcher(s) were disconnected while another "
                                "client was wedged -- collateral dropout" % drops))

    # THE CORE NUMBER: what happened to the HEALTHY clients' telemetry?
    # Note the ordering -- "zero frames" is checked BEFORE any gap ratio,
    # because a window with no arrivals at all has no measurable gap and would
    # otherwise fall through every comparison and be silently scored PASS.
    # That is the single worst outcome, so it must be the loudest.
    base_hz = base["motion_hz"] or 0.0
    wedge_hz = wedged["motion_hz"] or 0.0
    recov_hz = recov["motion_hz"] or 0.0
    detail["motion_hz_baseline_wedge_recovery"] = [round(base_hz, 2), round(wedge_hz, 2),
                                                   round(recov_hz, 2)]
    if base["motion_frames"] == 0:
        findings.append(finding("warn", "no baseline telemetry to compare against -- "
                                "the wedge result cannot be scored"))
    elif wedged["motion_frames"] == 0:
        findings.append(finding("fail", "HEALTHY watchers received ZERO motion frames for the "
                                "entire %.0f s wedge (baseline %.1f Hz) -- one client that stops "
                                "reading its socket silences telemetry for EVERY other client"
                                % (b1 - b0, base_hz)))
    elif wedge_hz < base_hz * 0.5:
        findings.append(finding("fail", "healthy watchers' telemetry collapsed from %.1f Hz to "
                                "%.1f Hz during the wedge (worst gap %.0f ms vs %.0f ms baseline)"
                                % (base_hz, wedge_hz, wedged["motion_max_gap_ms"] or 0,
                                   base["motion_max_gap_ms"] or 0)))
    elif (wedged["motion_max_gap_ms"] or 0) >= 1000:
        findings.append(finding("fail", "healthy watchers saw a %.0f ms telemetry gap while a peer "
                                "was wedged (baseline max %.0f ms)"
                                % (wedged["motion_max_gap_ms"], base["motion_max_gap_ms"] or 0)))
    else:
        findings.append(finding("pass", "healthy watchers unaffected by the wedge "
                                "(%.1f Hz vs %.1f Hz baseline, worst gap %.0f ms)"
                                % (wedge_hz, base_hz, wedged["motion_max_gap_ms"] or 0)))

    # Recovery is a SEPARATE question from collateral damage: a hub that stalls
    # but comes back is a bad afternoon; a hub that never comes back is the
    # reported "client dropped and stayed dropped".
    if base["motion_frames"] and recov["motion_frames"] == 0:
        findings.append(finding("fail", "healthy watchers' telemetry NEVER RESUMED in the %.0f s "
                                "after the wedge cleared -- the stall is permanent for the "
                                "session, not transient" % (c1 - c0)))
    elif base["motion_frames"] and recov_hz < base_hz * 0.5:
        findings.append(finding("warn", "healthy watchers only recovered to %.1f Hz of a %.1f Hz "
                                "baseline after the wedge cleared" % (recov_hz, base_hz)))
    hw = ctx.http.window(b0, b1)
    if hw["failures"]:
        findings.append(finding("fail", "HTTP (/api/status) failed %d/%d times during the wedge -- "
                                "this is the operator-visible 'WebUI unreachable'"
                                % (hw["failures"], hw["polls"])))
    elif hw["latency_max_ms"] and hw["latency_max_ms"] > 2000:
        findings.append(finding("warn", "HTTP latency spiked to %.0f ms during the wedge"
                                % hw["latency_max_ms"]))
    if base["motion_frames"] and recov["motion_frames"] == 0 and after_close["motion_frames"] > 0:
        findings.append(finding("warn", "healthy watchers only resumed AFTER the wedged socket was "
                                "closed (%.1f Hz) -- the hub is held hostage for exactly as long "
                                "as the bad client exists" % (after_close["motion_hz"] or 0)))
    elif base["motion_frames"] and after_close["motion_frames"] == 0:
        findings.append(finding("fail", "healthy watchers were STILL silent %.0f s after the wedged "
                                "client was fully closed -- the hub does not self-recover"
                                % (d1 - d0)))

    if victim_frames_after > 0:
        findings.append(finding("pass", "the wedged client itself RECOVERED on resuming reads "
                                "(%d frames) -- the hub un-muted it rather than dropping it"
                                % victim_frames_after))
    else:
        findings.append(finding("pass", "the wedged client was reclaimed by the device and got "
                                "nothing back after resuming -- expected%s"
                                % (" (a silent wedge also trips the session deadman, so this "
                                   "mode cannot separate stall-eviction from deadman-reap)"
                                   if mode == "silent" else "")))
    return ctx.envelope("wedge-%s" % mode, t0, now(), findings, detail)


def _window_stats(clients, t0, t1):
    """Telemetry continuity for a set of clients inside [t0, t1].

    Rebuilt from raw arrival timestamps rather than the running counters so a
    window can be scored independently of everything before it."""
    out = {"clients": len(clients), "motion_frames": 0,
           "motion_max_gap_ms": None, "motion_gaps_over_500ms": 0,
           "per_client": []}
    worst = 0.0
    for c in clients:
        with c.lock:
            st = c.m["state"].get(sp.CH_MOTION)
            if st:
                gaps = list(st["arrival_gaps"])   # copy under the lock: the
                last_t = st["last_t"]             # loop thread is still writing
        if not st:
            out["per_client"].append({"name": c.label, "motion_frames": 0})
            continue
        # Reconstruct absolute arrival times backwards from last_t, then keep
        # only the ones inside the window.  Storing gaps rather than absolute
        # times keeps the hot path cheap; this is the once-per-window cost.
        times = [last_t]
        for g in reversed(gaps):
            times.append(times[-1] - g)
        times.reverse()
        inwin = [(times[i], gaps[i - 1]) for i in range(1, len(times))
                 if t0 <= times[i] <= t1]
        n = len(inwin)
        mx = max((g for _t, g in inwin), default=None)
        out["motion_frames"] += n
        if mx is not None:
            worst = max(worst, mx)
        out["motion_gaps_over_500ms"] += sum(1 for _t, g in inwin if g > 0.5)
        out["per_client"].append({
            "name": c.label, "motion_frames": n,
            "max_gap_ms": None if mx is None else round(mx * 1000, 1),
            "measured_hz": round(n / (t1 - t0), 2) if t1 > t0 else None,
        })
    out["motion_max_gap_ms"] = round(worst * 1000, 1) if worst else None
    span = t1 - t0
    out["motion_hz"] = round(out["motion_frames"] / len(clients) / span, 2) \
        if (clients and span > 0) else None
    return out


def scenario_stream(ctx):
    """Streaming load: one client publishes motion-input (0x0084) at rate while
    watchers watch.  Amplitude defaults to 0.0 -- a hold at the machine's
    CURRENT position -- so the whole ingress path runs with zero machine motion
    on a shared device.  Raise --stream-amp deliberately, on a machine you own."""
    t0 = now()
    args = ctx.args
    watchers = []
    for i in range(args.stream_watchers):
        c, ok = bring_up(ctx, "swatch%d" % (i + 1))
        if not ok:
            return ctx.envelope("stream", t0, now(),
                                [finding("fail", "watcher %d could not establish" % (i + 1))], {})
        c.start_loop()
        watchers.append(c)

    pub, ok = bring_up(ctx, "streamer", publishes=[(sp.CH_MOTION_INPUT, args.stream_rate)])
    if not ok:
        for c in watchers:
            teardown(c, "clean")
        return ctx.envelope("stream", t0, now(),
                            [finding("fail", "streamer could not establish")], {})
    granted = pub.granted_publishes.get(sp.CH_MOTION_INPUT)
    pub.clock_sync(timeout=ctx.args.timeout)

    # Hold target = the machine's CURRENT normalized position.
    #
    # DO NOT take this from plan-strip's `cur`. It is the PLANNER's segment
    # cursor, and on an idle machine with no active plan it reads 0.5 -- which
    # is the middle of the stroke window, not where the carriage is. Streaming
    # that "hold" moved a homed machine 72 mm the first time this ran. The
    # authoritative pair is /api/settings (the window) and /api/status (the
    # live position), so that is what is used, and streaming is REFUSED
    # outright if either is unreadable.
    hold = _current_norm_position(args.ip)
    if hold is None:
        for c in watchers + [pub]:
            teardown(c, "clean")
        return ctx.envelope("stream", t0, now(),
                            [finding("warn", "could not establish the machine's current position "
                                     "and stroke window; refusing to stream a guessed target at a "
                                     "homed machine")], {})

    # Pace ~2% UNDER the granted rate and allow only a 2-sample catch-up burst.
    # The hub polices publishes with a token bucket, so a client that sends
    # exactly at the granted rate (let alone in bursts) earns RATE_LIMITED
    # NACKs and its samples are correctly refused -- which would then be
    # mis-reported here as wire loss. A real client paces under its grant; so
    # does this one.
    period = 1.02 / max(1.0, granted or args.stream_rate)
    state = {"next": now(), "n": 0, "t_start": now()}

    def streamer(client):
        # Emit every sample that has come due since the last pass, not just
        # one. The read loop wakes on socket activity, not on a stream clock,
        # so a one-per-pass streamer silently delivers whatever rate the loop
        # happens to spin at (measured 20 Hz of a granted 50) and the load test
        # quietly stops being a load test.
        sent = 0
        while now() >= state["next"] and sent < 2:
            state["next"] += period
            phase = now() - state["t_start"]
            tgt = hold + args.stream_amp * math.sin(2.0 * math.pi * 0.5 * phase)
            tgt = max(0.0, min(1.0, tgt))
            t_base = (sp.client_now_us() + (client.hub_offset_us or 0)) & 0xFFFFFFFF
            client.send(sp.FRAME["STREAM"], sp.CH_MOTION_INPUT,
                        sp.encode_stream_bundle(t_base, [(0, tgt, 0.0)]))
            sent += 1
        if state["next"] < now() - period:    # fell far behind: resync, never spin
            state["next"] = now() + period
        if sent:
            with client.lock:
                client.m["stream_bundles_sent"] += sent

    sm_before = _slopmotion(ctx.args.ip)
    pub.start_loop(streamer=streamer)
    s0 = now()
    time.sleep(args.stream_duration)
    s1 = now()
    sm_after = _slopmotion(ctx.args.ip)

    sent = pub.m["stream_bundles_sent"]
    for c in watchers + [pub]:
        teardown(c, "clean")

    dsync = {}
    if sm_before and sm_after:
        for k in ("bundles", "samples", "enqueued", "dropped", "seg_bundles"):
            dsync[k] = sm_after["sync"].get(k, 0) - sm_before["sync"].get(k, 0)

    detail = {
        "granted_publish_hz": granted,
        "hold_target_norm": round(hold, 4),
        "amplitude_norm": args.stream_amp,
        "bundles_sent": sent,
        "duration_s": round(s1 - s0, 1),
        "send_rate_hz": round(sent / (s1 - s0), 2) if s1 > s0 else None,
        "device_sync_delta": dsync,
        "watchers": _window_stats(watchers, s0, s1),
        "streamer_snapshot": pub.snapshot(),
        "watcher_snapshots": [c.snapshot() for c in watchers],
    }
    findings = []
    rate_limited = pub.m["nacks"].get("RATE_LIMITED", 0)
    if dsync:
        loss = sent - dsync.get("bundles", 0)
        detail["wire_loss_bundles"] = loss
        detail["rate_limited_nacks"] = rate_limited
        if loss > max(2, sent * 0.01) and rate_limited:
            findings.append(finding("warn", "%d/%d bundles unaccounted, but the hub sent %d "
                                    "RATE_LIMITED NACK(s) -- this is the token bucket refusing "
                                    "an over-rate sender, not transport loss"
                                    % (loss, sent, rate_limited)))
        elif loss > max(2, sent * 0.01):
            findings.append(finding("fail", "%d/%d stream bundles never reached the device "
                                    "(%.2f%% wire loss, zero NACKs -- bytes vanished)"
                                    % (loss, sent, 100.0 * loss / max(sent, 1))))
        else:
            findings.append(finding("pass", "%d bundles sent, %d counted by the device "
                                    "(loss %d)" % (sent, dsync.get("bundles", 0), loss)))
    drops = sum(c.m["unexpected_disconnects"] for c in watchers) + pub.m["unexpected_disconnects"]
    if drops:
        findings.append(finding("fail", "%d unexpected disconnect(s) under streaming load" % drops))
    else:
        findings.append(finding("pass", "no unexpected disconnects under streaming load"))
    return ctx.envelope("stream", t0, now(), findings, detail)


def scenario_soak(ctx):
    """N concurrent steady-state sessions for a long duration.  Boring by
    design: nothing here provokes the device, so ANY unexpected disconnect,
    NACK storm, telemetry stall or heap trend is a real defect."""
    t0 = now()
    args = ctx.args
    clients = []
    for i in range(args.clients):
        c, ok = bring_up(ctx, "soak%d" % (i + 1))
        if not ok:
            findings = [finding("fail", "client %d could not establish: %s" % (i + 1, c.m["errors"]))]
            for x in clients:
                teardown(x, "clean")
            return ctx.envelope("soak", t0, now(), findings, {})
        c.start_loop()
        clients.append(c)

    heap0 = ctx.log.heap_summary()
    end = now() + args.soak_duration
    reconnects = 0
    while now() < end:
        time.sleep(2.0)
        for c in clients:
            if not c.expect_connected and not c.deliberate_teardown:
                # Reconnect: a real client would, and a harness that gives up
                # after the first drop cannot measure the drop RATE.
                c.stop(join=False)
                try:
                    c.ws.close()
                except Exception:  # noqa: BLE001
                    pass
                c._stop.clear()
                try:
                    c.connect(timeout=args.timeout)
                    if c.handshake(timeout=args.timeout):
                        reconnects += 1
                        c.start_loop()
                except Exception as e:  # noqa: BLE001
                    c._note_error("reconnect failed: %s" % e)

    snaps = [c.snapshot() for c in clients]
    for c in clients:
        teardown(c, "clean")
    heap1 = ctx.log.heap_summary()

    drops = sum(s["unexpected_disconnects"] for s in snaps)
    nacks = {}
    for s in snaps:
        for k, v in s["nacks"].items():
            nacks[k] = nacks.get(k, 0) + v
    stalls = 0
    worst_gap = 0.0
    worst_quiet = 0.0
    for s in snaps:
        for ch, st in s["state"].items():
            for k, v in st.items():
                if k.startswith("stalls_over_"):
                    stalls += v
            if st["gap_max_ms"]:
                worst_gap = max(worst_gap, st["gap_max_ms"])
            if ch == "0x%04X" % sp.CH_MOTION and st.get("quiet_for_s"):
                worst_quiet = max(worst_quiet, st["quiet_for_s"])

    detail = {
        "clients": args.clients,
        "duration_s": round(args.soak_duration, 1),
        "reconnects": reconnects,
        "unexpected_disconnects": drops,
        "nacks": nacks,
        "telemetry_stalls": stalls,
        "worst_telemetry_gap_ms": worst_gap,
        "worst_silent_tail_s": worst_quiet,
        "heap_at_start": heap0,
        "heap_at_end": heap1,
        "client_snapshots": snaps,
    }
    findings = []
    if drops:
        findings.append(finding("fail", "%d unexpected disconnect(s) across %d clients in %.0f s "
                                "of steady-state soak -- THIS is the reported dropout"
                                % (drops, args.clients, args.soak_duration)))
    else:
        findings.append(finding("pass", "%d clients, %.0f s, zero unexpected disconnects"
                                % (args.clients, args.soak_duration)))
    if worst_quiet > 5.0:
        findings.append(finding("fail", "a client's motion telemetry had been silent for %.1f s "
                                "at the end of the soak -- connected but dead" % worst_quiet))
    if stalls:
        findings.append(finding("warn", "%d telemetry stall(s); worst gap %.0f ms"
                                % (stalls, worst_gap)))
    if nacks:
        findings.append(finding("warn", "NACKs during steady state: %s" % nacks))
    hw = ctx.http.window(t0, now())
    if hw["failures"]:
        findings.append(finding("fail", "HTTP (/api/status) failed %d/%d times during the soak"
                                % (hw["failures"], hw["polls"])))
    slope = heap1.get("free_slope_bytes_per_min")
    span = heap1.get("trend_span_min") or 0
    if heap1.get("leak_indicated") and span >= 5.0:
        findings.append(finding("fail", "free heap LEAK: median fell %d -> %d B across the run "
                                "(p10-p90 noise %d B, slope %s B/min, %.1f min reboot-free)"
                                % (heap1["median_first_third"], heap1["median_last_third"],
                                   heap1["noise_p10_p90"], slope, span)))
    elif slope is not None:
        findings.append(finding("pass", "no heap leak: median %d -> %d B against %d B of p10-p90 "
                                "noise (raw slope %s B/min over %.1f min)"
                                % (heap1.get("median_first_third") or 0,
                                   heap1.get("median_last_third") or 0,
                                   heap1.get("noise_p10_p90") or 0, slope, span)))
    return ctx.envelope("soak", t0, now(), findings, detail)


def _current_norm_position(ip):
    """The carriage's position as a fraction of the ACTIVE stroke window, or
    None.  Both halves have to be readable: a window without a position (or the
    reverse) is exactly the situation where a guess becomes a move."""
    _d1, st, e1 = http_get("http://%s/api/status" % ip)
    _d2, sett, e2 = http_get("http://%s/api/settings" % ip)
    if e1 or e2 or not st or not sett:
        return None
    try:
        pos = json.loads(st)["position"]
        s = json.loads(sett)
        lo, hi = float(s["range_min"]), float(s["range_max"])
    except (ValueError, KeyError, TypeError):
        return None
    if not (hi > lo):
        return None
    return max(0.0, min(1.0, (float(pos) - lo) / (hi - lo)))


def _slopmotion(ip):
    _dt, body, err = http_get("http://%s/api/slopmotion" % ip, timeout=6.0)
    if err or not body:
        return None
    try:
        return json.loads(body)
    except ValueError:
        return None


# =============================================================================
# Reporting
# =============================================================================

SCENARIOS = {
    "b2b": scenario_b2b,
    "rst": scenario_rst,
    "churn": scenario_churn,
    "wedge-silent": lambda ctx: scenario_wedge(ctx, "silent"),
    "wedge-chatty": lambda ctx: scenario_wedge(ctx, "chatty"),
    "stream": scenario_stream,
    "soak": scenario_soak,
}
SUITE_ORDER = ["b2b", "rst", "churn", "wedge-silent", "wedge-chatty", "stream", "soak"]


def print_table(summary):
    print("")
    print("=" * 78)
    print("slopsoak summary   label=%s   fw=%s   device=%s"
          % (summary["label"], summary["fw_version"], summary["device"]))
    print("started %s   ended %s   wall %s"
          % (summary["started"], summary["ended"], summary["wall_clock"]))
    print("=" * 78)
    name_w = max([len(r["scenario"]) for r in summary["scenarios"]] + [10])
    print("%-*s  %-6s  %7s  %5s  %7s  %8s  %10s  %6s" %
          (name_w, "scenario", "result", "dur(s)", "http", "httpErr",
           "logWarns", "heapMin(B)", "ui:81"))
    print("-" * 78)
    for r in summary["scenarios"]:
        lw = sum(r["device_log"].values())
        hm = r.get("heap", {}).get("free_min")
        uw = r.get("webui_ws") or {}
        print("%-*s  %-6s  %7.1f  %5d  %7d  %8d  %10s  %6s" %
              (name_w, r["scenario"], r["verdict"], r["duration_s"],
               r["http"]["polls"], r["http"]["failures"], lw,
               hm if hm is not None else "-",
               ("%dd/%dr" % (uw["drops"], uw["reconnects"])) if uw else "-"))
    print("-" * 78)
    h = summary["heap"]
    if h.get("samples"):
        print("heap  free first/last/min = %d / %d / %d B   delta %+d B"
              % (h["free_first"], h["free_last"], h["free_min"], h["free_delta"]))
        print("      trend %s B/min over %s min reboot-free (%d segment(s), %d samples); "
              "median %s -> %s B vs %s B noise; leak=%s"
              % (h.get("free_slope_bytes_per_min"), h.get("trend_span_min"),
                 h.get("reboot_segments", 1), h.get("trend_samples", 0),
                 h.get("median_first_third"), h.get("median_last_third"),
                 h.get("noise_p10_p90"), h.get("leak_indicated")))
        print("      min-watermark %d B   maxblock %d B   psram %d B"
              % (h["min_watermark_last"], h["maxblock_last"], h["psram_last"]))
    ha = summary["http_overall"]
    print("http  %d polls, %d failures, p50 %s ms, p95 %s ms, max %s ms"
          % (ha["polls"], ha["failures"], ha["latency_p50_ms"],
             ha["latency_p95_ms"], ha["latency_max_ms"]))
    if ha["device_reboots"]:
        print("REBOOT  the device restarted %d time(s) during this run: %s"
              % (ha["device_reboots"], ", ".join(ha["device_reboot_times"])))
    uw = summary.get("webui_ws_overall")
    if uw:
        print("ui:81 %d frames (%.1f Hz), %d drop(s), %d reconnect(s), worst gap %s ms"
              % (uw["frames"], uw["frame_hz"] or 0.0, uw["drops"], uw["reconnects"],
                 uw["max_gap_ms"]))
    print("")
    print("FINDINGS")
    print("-" * 78)
    for r in summary["scenarios"]:
        for f in r["findings"]:
            print("  [%-4s] %-14s %s" % (f["severity"].upper(), r["scenario"], f["text"]))
    print("")
    print("device log evidence (whole run): %s" % json.dumps(summary["device_log_totals"]))
    print("overall verdict: %s" % summary["verdict"])
    print("=" * 78)


# =============================================================================
# main
# =============================================================================

def main():
    ap = argparse.ArgumentParser(
        description="SlopSync stability/soak harness (transport migration evidence tool)")
    ap.add_argument("--ip", default="192.168.1.229")
    ap.add_argument("--port", type=int, default=82)
    ap.add_argument("--timeout", type=float, default=6.0)
    ap.add_argument("--label", default="unlabelled",
                    help="tag for this run, e.g. links2004-baseline / asyncws")
    ap.add_argument("--scenarios", default="suite",
                    help="'suite', or a comma list: %s" % ",".join(SUITE_ORDER))
    ap.add_argument("--list", action="store_true", help="list scenarios and exit")
    ap.add_argument("--out", default=None, help="JSON summary path (default slopsoak-<label>-<ts>.json)")

    ap.add_argument("--sub-rate", type=float, default=20.0, help="telemetry subscription rate wish")
    ap.add_argument("--clients", type=int, default=3, help="soak: concurrent sessions")
    ap.add_argument("--soak-duration", type=float, default=720.0, help="soak: seconds")

    ap.add_argument("--b2b-sessions", type=int, default=3)
    ap.add_argument("--b2b-dwell", type=float, default=4.0)
    ap.add_argument("--rst-iterations", type=int, default=10)
    ap.add_argument("--rst-dwell", type=float, default=2.5)
    ap.add_argument("--churn-iterations", type=int, default=30)
    ap.add_argument("--churn-gap", type=float, default=0.4)

    ap.add_argument("--wedge-watchers", type=int, default=2)
    ap.add_argument("--wedge-baseline", type=float, default=12.0)
    ap.add_argument("--wedge-hold", type=float, default=25.0)
    ap.add_argument("--wedge-recover", type=float, default=15.0)
    ap.add_argument("--wedge-after-close", type=float, default=10.0,
                    help="observation window after the wedged client is closed")
    ap.add_argument("--wedge-rcvbuf", type=int, default=2048,
                    help="SO_RCVBUF on the wedged socket; small = fast zero-window")
    ap.add_argument("--wedge-sub-rate", type=float, default=60.0,
                    help="the wedged client's subscription rate wish (high = fills fast)")

    ap.add_argument("--stream-watchers", type=int, default=2)
    ap.add_argument("--stream-duration", type=float, default=120.0)
    ap.add_argument("--stream-rate", type=float, default=50.0)
    ap.add_argument("--stream-amp", type=float, default=0.0,
                    help="normalized amplitude around the CURRENT position. 0.0 (default) "
                         "exercises the full ingress path with zero machine motion.")

    ap.add_argument("--http-interval", type=float, default=2.0)
    ap.add_argument("--log-interval", type=float, default=6.0)
    ap.add_argument("--watch-uiws", dest="watch_uiws", action="store_true", default=True,
                    help="observe the WebUI's own ws://ip:81/ws/ui link (default on)")
    ap.add_argument("--no-watch-uiws", dest="watch_uiws", action="store_false")
    ap.add_argument("--uiws-port", type=int, default=81)
    args = ap.parse_args()

    if args.list:
        for k in SUITE_ORDER:
            print("  %s" % k)
        return 0

    want = SUITE_ORDER if args.scenarios == "suite" else [
        s.strip() for s in args.scenarios.split(",") if s.strip()]
    unknown = [s for s in want if s not in SCENARIOS]
    if unknown:
        sys.stderr.write("unknown scenario(s): %s\n" % ", ".join(unknown))
        return 2

    _dt, caps, err = http_get("http://%s/api/capabilities" % args.ip)
    if err or not caps:
        sys.stderr.write("device at %s is not answering /api/capabilities (%s).\n"
                         "STOP -- do not power-cycle, do not flash; report it.\n" % (args.ip, err))
        return 3
    fw = json.loads(caps).get("fw_version", "?")

    ctx = Ctx(args)
    ctx.start_monitors()
    t_start = now()
    print("slopsoak: device %s fw %s, label %r, scenarios: %s"
          % (args.ip, fw, args.label, ", ".join(want)))
    sm_before = _slopmotion(args.ip)

    scenarios = []
    try:
        for name in want:
            print("\n---- %s : running ----" % name)
            r = SCENARIOS[name](ctx)
            scenarios.append(r)
            print("---- %s : %s (%.1fs) ----" % (name, r["verdict"], r["duration_s"]))
            for f in r["findings"]:
                print("     [%s] %s" % (f["severity"].upper(), f["text"]))
            time.sleep(2.0)
    except KeyboardInterrupt:
        print("\n! interrupted -- writing partial summary")
    finally:
        t_end = now()
        # One last log poll so the tail of the run is in evidence.
        try:
            ctx.log.poll_once()
        except Exception:  # noqa: BLE001
            pass
        ctx.stop_monitors()

    sm_after = _slopmotion(args.ip)
    verdict = "PASS"
    if any(r["verdict"] == "FAIL" for r in scenarios):
        verdict = "FAIL"
    elif any(r["verdict"] == "WARN" for r in scenarios):
        verdict = "WARN"

    summary = {
        "tool": "slopsoak.py", "schema": 1,
        "label": args.label,
        "device": "%s:%d" % (args.ip, args.port),
        "fw_version": fw,
        "started": stamp(t_start), "ended": stamp(t_end),
        "wall_clock": "%.1f min" % ((t_end - t_start) / 60.0),
        "args": {k: v for k, v in vars(args).items() if k not in ("list",)},
        "scenarios": scenarios,
        "heap": ctx.log.heap_summary(),
        "http_overall": ctx.http.window(t_start, t_end),
        "webui_ws_overall": ctx.uiws.window(t_start, t_end) if ctx.uiws else None,
        "device_log_totals": ctx.log.evidence_counts(),
        "slopmotion_before": (sm_before or {}).get("sync"),
        "slopmotion_after": (sm_after or {}).get("sync"),
        "verdict": verdict,
    }

    out = args.out or ("slopsoak-%s-%s.json"
                       % (args.label, time.strftime("%Y%m%d-%H%M%S", time.localtime(t_start))))
    with open(out, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, default=_jsonable)
    print_table(summary)
    print("JSON summary: %s" % os.path.abspath(out))
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
