#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
slopscope.py -- SlopScope, the motion-pipeline oscilloscope for SlopDrive-32.

WHAT IT IS FOR
    Graph the RAW COMMANDED INPUT arriving over SlopSync, scaled into the
    stroke window, against what the motion planner actually did with it. Three
    lines on one mm axis:

        ASKED     0x1100 raw_10um   -- the demand as it landed, pre-planning
        PLANNED   0x1100 tgt_10um   -- where SlopMotion is driving to
        ACHIEVED  0x1100 pos_10um   -- where the carriage actually is

    plus the planner's own current segment (plan-strip: start -> end over
    durationUs) drawn as the commanded envelope around the trace inside it,
    the velocity plane, and the engine's anomaly EVENTs marked on the time
    axis. That is the whole diagnostic vocabulary for FEEL questions: quintic
    shaping, Ruckig-guard fallbacks, chase lag, handoff bounding.

SAFETY GUARANTEE
    SlopScope connects as a `watch`-tier session and SUBSCRIBES ONLY. It never
    sends an INTENT, never publishes a STREAM, never carries a `publishes`
    wish in its HELLO, and therefore cannot command motion -- not by accident,
    not by flag. There is no code path in this file that builds an INTENT
    frame. Drive the machine with something else (the WebUI, a player, the
    probe's --stream/--segments) and watch it here.

RELATIONSHIP TO tools/slopsync_probe.py
    The probe is this project's primary verification client and stays
    untouched. SlopScope IMPORTS it as a wire library -- CBOR codec, frame
    header, message builders, registry constants, the CLOCK exchange, the
    send/recv plumbing -- so there is exactly one implementation of the
    SlopSync wire in tools/ and it is the heavily-exercised one. What
    SlopScope adds on top is a catalog-DRIVEN decoder: where the probe
    hardcodes struct layouts (correct for a conformance tool that must fail
    when the device drifts), SlopScope decodes every packed STATE payload from
    the catalog the hub just served it, so it follows an append-only layout
    change without a code edit.

DISCOVERY IS BY ROLE AND NAME, NEVER BY CHANNEL ID
    Nothing in this file hardcodes 0x1100/0x1000/0x1110/0x1111/0x4100 (RFC-047
    Phase C4; motion/machine-config/motion-anomaly never moved, plan-strip and
    slopmotion-diag did — see CHANNEL-MAP.md for the renumber). The
    stroke window is resolved by the registry field_roles `window.min` /
    `window.max`; the position/velocity fields by `telemetry.position` /
    `telemetry.velocity`; the rest by the catalog's own declared entry and
    field NAMES. See resolve_* below, and the `resolution` block written into
    every trace header -- a trace records HOW it found each thing, so a trace
    taken against a differently-numbered hub is still readable.

    Known gap, worth a registry PR one day: `tgt_10um` and `raw_10um` carry
    catalog descriptions but no field_role, so ASKED and PLANNED are resolved
    by field name (with the layout-order fallback described in
    resolve_motion). That is recorded honestly in the header rather than
    papered over.

USAGE
    python slopscope.py record  [--ip IP] [--port 82] [--seconds S] [--out T.jsonl]
                                [--render G.html]
    python slopscope.py live    [--ip IP] [--port 82] [--seconds S] [--out T.jsonl]
    python slopscope.py render  T.jsonl [--out G.html] [--theme dark|light]

    `record` captures a JSONL trace; `render` turns a trace into ONE
    self-contained HTML file (inline SVG + inline JS, zero network requests,
    zero dependencies -- it opens on a bench laptop with no internet);
    `live` prints a terminal dashboard while streaming and can record too.

REQUIREMENTS
    Python 3.8+, the `websocket-client` package (same as the probe). The
    RENDER path needs nothing at all -- stdlib only -- so a trace can be
    rendered anywhere.
"""
import argparse
import io
import json
import math
import os
import struct
import sys
import time
import urllib.request

# --- import the probe as the wire library ------------------------------------
# Same directory, no packaging: put tools/ on the path and import. The probe is
# import-safe (module level defines constants and functions only; everything
# with side effects lives under main()/run()).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slopsync_probe as ss  # noqa: E402


TOOL_NAME = "slopscope"
TOOL_VERSION = "1.0.0"
TRACE_FORMAT_VERSION = 1

# Client identity on the wire. `client_kind` is the machine-readable species,
# `client_name` the human one -- both show up in the hub's session roster, so a
# glance at the roster tells the operator a passive scope is attached.
CLIENT_KIND = "scope"
CLIENT_NAME = "slopscope.py"


# =============================================================================
# Catalog-driven packed decoding (SPEC §5.4 / §8.2 / Appendix C)
#
# The hub serves its own catalog; every packed STATE channel declares its
# layout as an ordered list of fields with a type, a unit and a scale. Decoding
# from THAT rather than from a struct string is what lets this tool follow an
# append-only layout evolution (the raw_10um append is exactly such an event)
# without an edit -- and what lets it work against a hub whose channel numbers
# are not this firmware's.
# =============================================================================

# registry `packed_field_types` -> (struct fmt or None for fixed-width strings,
# byte width). None fmt => zero-padded UTF-8 of that width (RFC-026).
PACKED_TYPES = {
    0: ("<B", 1), 1: ("<b", 1), 2: ("<H", 2), 3: ("<h", 2),
    4: ("<I", 4), 5: ("<i", 4), 6: ("<f", 4), 7: ("<B", 1),
    8: (None, 16), 9: (None, 32), 10: (None, 64),
}
TYPE_NAMES = {0: "u8", 1: "i8", 2: "u16", 3: "i16", 4: "u32", 5: "i32",
              6: "f32", 7: "bitfield8", 8: "str16", 9: "str32", 10: "str64"}
BITFIELD8 = 7


def _field(f):
    """One catalog field map -> a plain dict with the keys this tool uses.
    Unknown/absent annotations come back as None, never as invented values."""
    F = ss.CAT_F
    return {
        "name": f.get(F["name"]),
        "type": f.get(F["type"]),
        "type_name": TYPE_NAMES.get(f.get(F["type"]), "?%s" % f.get(F["type"])),
        "unit": f.get(F["unit"]),
        "scale": f.get(F["scale"], 1.0) or 1.0,
        "role": f.get(F["role"]),
        "group": f.get(F["group"]),
        "desc": f.get(F["desc"]),
        "options": f.get(F["options"]),
        "bits": f.get(F["bits"]),
    }


class CatalogView:
    """A decoded catalog blob, queried the way a generic client queries it."""

    def __init__(self, catalog_bytes):
        self.raw = catalog_bytes
        self.entries = ss._catalog_entries(catalog_bytes)   # {id: entry-map}

    def __bool__(self):
        return bool(self.entries)

    # -- entry accessors ----------------------------------------------------
    def name(self, cid):
        return self.entries.get(cid, {}).get(ss.CAT_E["name"])

    def cls(self, cid):
        return self.entries.get(cid, {}).get(ss.CAT_E["cls"])

    def rate(self, cid):
        return self.entries.get(cid, {}).get(ss.CAT_E["rate"])

    def priority(self, cid):
        return self.entries.get(cid, {}).get(ss.CAT_E["priority"])

    def layout(self, cid):
        """Ordered layout fields of a packed channel, or [] if it has none
        (INTENT/EVENT channels carry a `schema` instead)."""
        raw = self.entries.get(cid, {}).get(ss.CAT_E["layout"])
        if not isinstance(raw, list):
            return []
        return [_field(f) for f in raw if isinstance(f, dict)]

    def schema(self, cid):
        """A CBOR-keyed channel schema as {cbor_key: field}. Note the shape
        difference from `layout`: a layout is an ORDERED ARRAY (byte offsets
        come from the order), a schema is a MAP KEYED BY ITS OWN CBOR KEY --
        because a schema's keys are sparse, append-only and never renumbered."""
        raw = self.entries.get(cid, {}).get(ss.CAT_E["schema"])
        if not isinstance(raw, dict):
            return {}
        return {k: _field(v) for k, v in raw.items() if isinstance(v, dict)}

    def enum_options(self, cid, field_name, in_schema=True):
        """The `options` label list of a select field -- the one registered
        mechanism for turning a wire number into a name (catalog field key 10).
        This is how anomaly kinds get their names: the channel that emits them
        carries its own vocabulary."""
        src = self.schema(cid).values() if in_schema else self.layout(cid)
        for f in src:
            if f["name"] == field_name and isinstance(f["options"], list):
                return list(f["options"])
        return None

    def summary(self):
        out = []
        for cid in sorted(self.entries):
            out.append({
                "id": cid, "name": self.name(cid), "cls": self.cls(cid),
                "dir": self.entries[cid].get(ss.CAT_E["dir"]),
                "rate_hz": self.rate(cid), "priority": self.priority(cid),
                "fields": len(self.layout(cid)) or len(self.schema(cid)),
            })
        return out

    # -- discovery ----------------------------------------------------------
    def find_role(self, role):
        """(channel_id, field_name) for the FIRST layout field carrying `role`,
        scanning channels in ascending id so the answer is deterministic across
        runs. Returns (None, None) when no channel claims it."""
        for cid in sorted(self.entries):
            for f in self.layout(cid):
                if f["role"] == role:
                    return cid, f["name"]
        return None, None

    def find_by_name(self, entry_name, cls=None):
        for cid in sorted(self.entries):
            if self.name(cid) == entry_name and (cls is None or self.cls(cid) == cls):
                return cid
        return None

    def find_with_fields(self, names, cls=None, need=None):
        """The channel whose layout declares the most of `names` (>= `need`).
        A structural fingerprint: it identifies a channel by the shape of what
        it publishes, not by what it is called or numbered."""
        want = set(names)
        need = need if need is not None else len(want)
        best, best_hits = None, 0
        for cid in sorted(self.entries):
            if cls is not None and self.cls(cid) != cls:
                continue
            hits = len(want & {f["name"] for f in self.layout(cid)})
            if hits > best_hits:
                best, best_hits = cid, hits
        return best if best_hits >= need else None


def unpack_layout(payload, layout):
    """Decode a packed STATE payload against its catalog layout.

    PREFIX-PARSE (§8.6): fields are consumed in order until the payload runs
    out, and what was decoded is returned. That is the append-only rule doing
    its job -- an older hub sending 7 bytes where the catalog we hold declares
    9 yields the first four fields and no exception. Truncation is reported via
    the returned field count, never by throwing.

    Scaled integers come back as PHYSICAL values (wire / scale). bitfield8
    passes through raw (scale is meaningless on a bit vector, §5.4).
    """
    out, off, n = {}, 0, len(payload)
    for f in layout:
        spec = PACKED_TYPES.get(f["type"])
        if spec is None:
            break                          # unknown type: cannot know its width
        fmt, size = spec
        if off + size > n:
            break                          # prefix parse: stop, keep what we have
        if fmt is None:
            raw = bytes(payload[off:off + size])
            out[f["name"]] = raw.split(b"\x00", 1)[0].decode("utf-8", "replace")
        else:
            v = struct.unpack_from(fmt, payload, off)[0]
            if f["type"] == BITFIELD8:
                out[f["name"]] = int(v)
            elif f["scale"] and f["scale"] != 1.0:
                out[f["name"]] = v / float(f["scale"])
            else:
                out[f["name"]] = v
        off += size
    return out


def decode_event(payload, schema):
    """An EVENT frame is a CBOR map in the GLOBAL registry key space
    (channel_id, timestamp, event_kind, body...), whose `body` (40) sub-map is
    in the CHANNEL'S OWN schema key space. Those two vocabularies are
    unrelated, and mixing them is how you end up printing a motion anomaly's
    `target` as `client_name` -- so each half is relabeled with its own."""
    try:
        m = ss.cb_decode_full(payload)
    except ValueError:
        return None
    if not isinstance(m, dict):
        return m
    out = _relabel_global(m)
    body = out.get("body")
    if isinstance(body, dict):
        out["body"] = _relabel_schema(body, schema)
    return out


def _relabel_global(m):
    out = {}
    for k, v in m.items():
        name = ss_key_name(k) or ("k%s" % k)
        out[name] = v.hex() if isinstance(v, (bytes, bytearray)) else v
    return out


def _relabel_schema(m, schema):
    """Channel-schema keys ONLY. An unknown key stays 'k<N>' -- never borrows a
    name from the global space, which would be a different field entirely."""
    out = {}
    for k, v in m.items():
        f = (schema or {}).get(k)
        name = (f or {}).get("name") or ("k%s" % k)
        out[name] = v.hex() if isinstance(v, (bytes, bytearray)) else v
    return out


_SS_KEY_NAME = {v: k for k, v in ss.K.items()}


def ss_key_name(k):
    return _SS_KEY_NAME.get(k)


# =============================================================================
# Resolution: what this tool needs, found in whatever catalog it was handed
# =============================================================================

class Resolved:
    """Everything SlopScope needs, plus a written record of HOW each was
    found. The `how` strings land in the trace header so a trace read months
    later can be trusted (or distrusted) on its own evidence."""

    def __init__(self):
        self.how = {}
        self.window_ch = None
        self.window_min_field = None
        self.window_max_field = None
        self.motion_ch = None
        self.f_achieved = None      # position (mm)
        self.f_velocity = None      # speed (mm/s)
        self.f_planned = None       # planner target (mm)
        self.f_asked = None         # pre-planning demand (mm)
        self.plan_ch = None
        self.diag_ch = None
        self.anom_ch = None
        self.safety_ch = None
        self.limit_fields = {}      # role -> (channel_id, field_name)
        self.window_override = None  # (min_mm, max_mm) from --window, if given

    def missing(self):
        m = []
        if self.motion_ch is None:
            m.append("motion (no telemetry.position role and no pos/tgt/speed layout)")
        if self.window_ch is None and not self.window_override:
            m.append("stroke window (field_roles window.min / window.max; "
                     "or pass --window MIN:MAX)")
        return m


# registry field_roles this tool looks for. Names, not numbers.
ROLE_WINDOW_MIN = "window.min"
ROLE_WINDOW_MAX = "window.max"
ROLE_POSITION = "telemetry.position"
ROLE_VELOCITY = "telemetry.velocity"
LIMIT_ROLES = ["limit.user.speed", "limit.user.accel", "limit.input.speed",
               "limit.input.accel", "limit.input.jerk"]

CLASS_STATE, CLASS_STREAM, CLASS_INTENT, CLASS_EVENT, CLASS_STORE = 0, 1, 2, 3, 4


def resolve(cat, window_override=None):
    """Find every channel/field this tool plots, from the catalog alone."""
    r = Resolved()
    r.window_override = window_override

    # ---- the stroke window: BY ROLE, always. -----------------------------
    # This is the one lookup the brief pins down, and the reason is RFC-003:
    # limits normalized against the window MOVE when the window does, so the
    # window is live STATE, and a client that hardcoded where to read it would
    # be wrong on the next hub it met. window.min/window.max are registry
    # roles; a conforming hub that exposes a window says so this way or not at
    # all.
    cmin, fmin = cat.find_role(ROLE_WINDOW_MIN)
    cmax, fmax = cat.find_role(ROLE_WINDOW_MAX)
    if cmin is not None and cmax is not None:
        r.window_ch, r.window_min_field, r.window_max_field = cmin, fmin, fmax
        r.how["window"] = ("field_role %s/%s -> channel 0x%04X %r fields %r/%r"
                           % (ROLE_WINDOW_MIN, ROLE_WINDOW_MAX, cmin,
                              cat.name(cmin), fmin, fmax))
        if cmax != cmin:
            r.how["window"] += "  (NOTE: max lives on 0x%04X, a different channel)" % cmax
    elif r.window_override:
        r.how["window"] = ("OPERATOR OVERRIDE --window %.3f:%.3f mm. This hub's catalog carries "
                           "no %s / %s role, so the window is NOT machine truth here and the "
                           "trace says so." % (r.window_override[0], r.window_override[1],
                                               ROLE_WINDOW_MIN, ROLE_WINDOW_MAX))
    else:
        r.how["window"] = ("UNRESOLVED -- no catalog field carries %s / %s. Pass "
                           "--window MIN:MAX to plot anyway (recorded as an operator override, "
                           "never as machine truth)."
                           % (ROLE_WINDOW_MIN, ROLE_WINDOW_MAX))

    for role in LIMIT_ROLES:
        c, f = cat.find_role(role)
        if c is not None:
            r.limit_fields[role] = (c, f)

    # ---- the motion triple ------------------------------------------------
    # ACHIEVED and the velocity plane are role-resolved. PLANNED and ASKED are
    # not: no registry role names "the planner's target" or "the pre-planning
    # demand" yet, so they are found by the catalog's own field NAMES, with a
    # documented fallback. This is stated in the header rather than hidden.
    mch, fpos = cat.find_role(ROLE_POSITION)
    if mch is None:
        # Role absent: fall back to a layout fingerprint. Recorded as such --
        # a guess that says it is a guess beats refusing to plot a machine
        # whose catalog is merely under-annotated.
        mch = cat.find_with_fields(["pos_10um", "tgt_10um", "speed"], cls=CLASS_STATE, need=2)
        if mch is not None:
            fpos = _pick([f["name"] for f in cat.layout(mch)], ("pos",))
            r.how["achieved_fallback"] = ("no %s role on this hub; matched layout fingerprint "
                                          "-> 0x%04X field %r" % (ROLE_POSITION, mch, fpos))
    if mch is not None:
        r.motion_ch, r.f_achieved = mch, fpos
        r.how["achieved"] = "field_role %s -> 0x%04X %r field %r" % (
            ROLE_POSITION, mch, cat.name(mch), fpos)
        vch, fvel = cat.find_role(ROLE_VELOCITY)
        if vch == mch:
            r.f_velocity = fvel
            r.how["velocity"] = "field_role %s -> same channel, field %r" % (ROLE_VELOCITY, fvel)
        names = [f["name"] for f in cat.layout(mch)]
        r.f_planned = _pick(names, ("tgt", "target"), exclude=(r.f_achieved,))
        r.f_asked = _pick(names, ("raw", "asked", "demand"), exclude=(r.f_achieved, r.f_planned))
        r.how["planned"] = ("field name match on 0x%04X layout -> %r (no field_role exists "
                            "for 'planner target'; matched on tgt|target)"
                            % (mch, r.f_planned)) if r.f_planned else \
                           "UNRESOLVED -- no tgt|target field on the motion channel"
        r.how["asked"] = ("field name match on 0x%04X layout -> %r (no field_role exists for "
                          "'pre-planning demand'; matched on raw|asked|demand)"
                          % (mch, r.f_asked)) if r.f_asked else \
                         ("UNRESOLVED -- this hub's motion channel publishes no raw/asked field. "
                          "The ASKED line will be absent; the hub predates it.")
    else:
        r.how["achieved"] = "UNRESOLVED -- no catalog field carries %s" % ROLE_POSITION

    # ---- plan strip: structural fingerprint, then name -------------------
    r.plan_ch = cat.find_with_fields(
        ["start_norm", "end_norm", "cur_norm", "cur_vel", "duration_us", "elapsed_us"],
        cls=CLASS_STATE, need=4)
    if r.plan_ch is not None:
        r.how["plan_strip"] = ("layout fingerprint {start,end,cur,cur_vel,duration,elapsed} -> "
                               "0x%04X %r" % (r.plan_ch, cat.name(r.plan_ch)))
    else:
        c = cat.find_by_name("plan-strip", cls=CLASS_STATE)
        if c is not None:
            r.plan_ch = c
            r.how["plan_strip"] = "entry name 'plan-strip' -> 0x%04X" % c
        else:
            r.how["plan_strip"] = "UNRESOLVED -- no plan-strip channel; the envelope overlay is off"

    # ---- diag ------------------------------------------------------------
    r.diag_ch = cat.find_with_fields(["plans", "failures", "anomalies", "plan_us_last"],
                                     cls=CLASS_STATE, need=3)
    r.how["diag"] = (("layout fingerprint {plans,failures,anomalies,plan_us_*} -> 0x%04X %r"
                      % (r.diag_ch, cat.name(r.diag_ch))) if r.diag_ch is not None
                     else "UNRESOLVED -- no planner-stats channel")

    # ---- anomaly EVENT ---------------------------------------------------
    for cid in sorted(cat.entries):
        if cat.cls(cid) != CLASS_EVENT:
            continue
        nm = (cat.name(cid) or "")
        if "anomal" in nm:
            r.anom_ch = cid
            r.how["anomaly"] = "EVENT-class entry whose name contains 'anomal' -> 0x%04X %r" % (cid, nm)
            break
    if r.anom_ch is None:
        r.how["anomaly"] = "UNRESOLVED -- no anomaly EVENT channel; the marker rail stays empty"

    # ---- safety latch (context only; never plotted as a series) ----------
    r.safety_ch = cat.find_by_name("safety", cls=CLASS_STATE)
    r.how["safety"] = ("entry name 'safety' -> 0x%04X" % r.safety_ch) if r.safety_ch is not None \
        else "not present"
    return r


def _pick(names, needles, exclude=()):
    for n in names:
        if n in exclude or not n:
            continue
        low = n.lower()
        for nd in needles:
            if nd in low:
                return n
    return None


# =============================================================================
# The session: connect, adopt the catalog, subscribe, pump.
#
# WATCH TIER, SUBSCRIBE ONLY. There is no intent builder anywhere below.
# =============================================================================

class ScopeSession:
    def __init__(self, args):
        self.args = args
        self.ws = None
        self.welcome = {}
        self.cat = None
        self.res = None
        self.clock_offset_us = None
        self.clock_rtt_us = None
        self.grants = {}          # channel_id -> granted_rate_hz
        self.t0 = None            # capture start, time.time()
        # PING cadence, set from the hub's own advertised deadman_ms once
        # WELCOME lands (a third of the window, floor 200 ms).
        self.ping_interval_s = 0.25

    # -- lifecycle ---------------------------------------------------------
    def connect(self):
        url = "ws://%s:%d/" % (self.args.ip, self.args.port)
        import websocket
        self.ws = websocket.create_connection(
            url, subprotocols=[ss.WS_SUBPROTOCOL], timeout=self.args.timeout)
        ss._last_send_ts[0] = time.time()
        return url

    def hello(self):
        """HELLO with NO `publishes` wish -- structurally incapable of
        streaming motion -- and no token. The hub answers with the tier its
        policy grants; SlopScope only ever needs `watch`."""
        instance_id = os.urandom(8)
        payload = ss.build_hello(CLIENT_KIND, CLIENT_NAME, instance_id, publishes=None)
        ss.send_frame(self.ws, ss.FRAME["HELLO"], 0, payload)
        deadline = time.time() + self.args.timeout
        while time.time() < deadline:
            got = ss.recv_frame(self.ws, deadline)
            if got is None:
                break
            hdr, pl = got
            if hdr["type"] == ss.FRAME["WELCOME"]:
                self.welcome = ss.cb_decode_full(pl)
                dm = self.welcome.get(ss.K["deadman_ms"])
                if isinstance(dm, int) and dm > 0:
                    self.ping_interval_s = max(0.2, dm / 3000.0)
                return self.welcome
            if hdr["type"] == ss.FRAME["NACK"]:
                code = ss.cb_decode_full(pl).get(ss.K["code"])
                raise RuntimeError("HELLO refused: NACK %s" % ss.nack_name(code))
        raise RuntimeError("no WELCOME within %.1fs" % self.args.timeout)

    def fetch_catalog(self):
        """BLOB namespace 0. Fetched BEFORE CATALOG_READY, because a client
        that declares an etag should have verified those bytes itself -- the
        readiness gate covers the control and data planes, not blob transfer."""
        ss.send_frame(self.ws, ss.FRAME["BLOB_REQ"], 0, ss.build_blob_req())
        chunks, count, total = {}, None, None
        deadline = time.time() + self.args.timeout
        while time.time() < deadline:
            got = ss.recv_frame(self.ws, deadline)
            if got is None:
                break
            hdr, pl = got
            if hdr["type"] != ss.FRAME["BLOB_CHUNK"]:
                continue
            c = ss.decode_blob_chunk(pl)
            if c is None:
                break
            count, total = c["chunk_count"], c["total_bytes"]
            chunks[c["chunk_index"]] = c["bytes"]
            if count and len(chunks) == count:
                break
        if not count or len(chunks) != count:
            raise RuntimeError("catalog BLOB transfer incomplete (%d/%s chunks)"
                               % (len(chunks), count))
        blob = b"".join(chunks[i] for i in range(count))
        import hashlib
        digest = hashlib.sha256(blob).digest()[:8]
        etag = self.welcome.get(ss.K["catalog_etag"])
        verified = isinstance(etag, (bytes, bytearray)) and digest == bytes(etag)
        self.cat = CatalogView(blob)
        self.res = resolve(self.cat, getattr(self.args, "window", None))
        return verified, digest

    def declare_ready(self):
        etag = self.welcome.get(ss.K["catalog_etag"])
        if not isinstance(etag, (bytes, bytearray)) or len(etag) != 8:
            raise RuntimeError("WELCOME carried no usable 8-byte catalog_etag")
        ss.send_frame(self.ws, ss.FRAME["CATALOG_READY"], 0, bytes(etag))

    def sync_clock(self):
        got = ss.estimate_hub_offset(self.ws, self.args.timeout)
        if got:
            self.clock_offset_us, self.clock_rtt_us = got
        return got

    def subscribe(self):
        """Wish each resolved telemetry channel at its own catalog ceiling --
        this is a diagnostic instrument, so the honest wish is 'everything you
        have'. The hub clamps; whatever it GRANTS is what the trace header
        records, never the wish."""
        r, cat = self.res, self.cat
        wishes = []

        def wish(cid, prio, cap=None):
            if cid is None:
                return
            rate = cat.rate(cid)
            rate = 0.0 if not rate else float(rate)
            if cap is not None and rate:
                rate = min(rate, cap)
            wishes.append((cid, rate, prio))

        if r.safety_ch is not None:
            wishes.append((r.safety_ch, 0.0, ss.PRIORITY["critical"]))
        wish(r.motion_ch, ss.PRIORITY["elevated"])
        wish(r.plan_ch, ss.PRIORITY["elevated"])
        if r.window_ch is not None:
            wishes.append((r.window_ch, 0.0, ss.PRIORITY["normal"]))   # on-change
        wish(r.diag_ch, ss.PRIORITY["background"])
        wish(r.anom_ch, ss.PRIORITY["normal"])
        if not wishes:
            raise RuntimeError("nothing to subscribe to -- the catalog resolved no telemetry")
        ss.send_frame(self.ws, ss.FRAME["SUBSCRIBE"], 0, ss.build_subscribe(wishes))

        deadline = time.time() + self.args.timeout
        pending = {c for c, _, _ in wishes}
        while time.time() < deadline and pending:
            got = ss.recv_frame(self.ws, deadline)
            if got is None:
                break
            hdr, pl = got
            if hdr["type"] == ss.FRAME["GRANT"]:
                g = ss.cb_decode_full(pl)
                for e in g.get(ss.K["grants"], []) or []:
                    cid = e.get(ss.K["channel_id"])
                    self.grants[cid] = e.get(ss.K["granted_rate_hz"])
                    pending.discard(cid)
            elif hdr["type"] == ss.FRAME["NACK"]:
                n = ss.cb_decode_full(pl)
                cid = n.get(ss.K["channel_id"])
                self.grants[cid] = None
                pending.discard(cid)
        return self.grants

    def goodbye(self):
        try:
            ss.send_frame(self.ws, ss.FRAME["GOODBYE"], 0,
                          ss.build_goodbye(ss.NACK_CODES_BY_NAME["NORMAL_CLOSURE"]))
        except Exception:
            pass
        try:
            self.ws.close()
        except Exception:
            pass

    # -- pump --------------------------------------------------------------
    def hub_now_us(self):
        """Best estimate of the hub's own microsecond clock right now (u32
        domain, §7.1). STATE frames carry no hub timestamp -- only seq -- so a
        trace records this ESTIMATE alongside the client clock and says so.
        Where a real hub stamp exists (an anomaly body's t_us) it is kept
        verbatim and never overwritten by this."""
        if self.clock_offset_us is None:
            return None
        return (ss.client_now_us() + self.clock_offset_us) & 0xFFFFFFFF

    def keepalive(self):
        """§6.5 liveness is SILENCE-BASED and it is measured on what the CLIENT
        sends, not on what it receives. A pure subscriber is the pathological
        case: telemetry pours in at 60 Hz, so the receive path never idles and
        a receive-triggered keepalive never fires -- and the hub reaps the
        session for silence a few seconds in. (Observed exactly that on the
        first sim run: `ws: slot 0 detached` at ~3 s with the trace simply
        stopping.) So SlopScope pings on ITS OWN clock, well inside the
        advertised deadman window, for as long as it is attached."""
        if time.time() - ss._last_send_ts[0] > self.ping_interval_s:
            ss.send_frame(self.ws, ss.FRAME["PING"], 0, b"")

    def pump(self, deadline):
        """Yield (kind, t_rel, hub_us, decoded) until `deadline`."""
        r, cat = self.res, self.cat
        layouts = {}
        for cid in (r.motion_ch, r.plan_ch, r.window_ch, r.diag_ch):
            if cid is not None:
                layouts[cid] = cat.layout(cid)
        kind_of = {r.motion_ch: "m", r.plan_ch: "p", r.window_ch: "c",
                   r.diag_ch: "d", r.anom_ch: "a", r.safety_ch: "s"}
        anom_schema = cat.schema(r.anom_ch) if r.anom_ch is not None else {}

        while time.time() < deadline:
            self.keepalive()
            got = ss.recv_frame(self.ws, min(deadline, time.time() + 0.25))
            # keep the probe's shared anomaly sniffer from growing unbounded on
            # a long capture -- we decode EVENTs ourselves below.
            if ss._anomaly_events:
                del ss._anomaly_events[:]
            if got is None:
                continue
            hdr, pl = got
            t_rel = time.time() - self.t0
            hub_us = self.hub_now_us()
            ftype, cid = hdr["type"], hdr["channel"]
            k = kind_of.get(cid)
            if ftype == ss.FRAME["STATE"] and k in ("m", "p", "c", "d"):
                yield k, t_rel, hub_us, unpack_layout(pl, layouts.get(cid, []))
            elif ftype == ss.FRAME["STATE"] and k == "s":
                try:
                    yield "s", t_rel, hub_us, ss.decode_safety_state(pl)
                except Exception:
                    pass
            elif ftype == ss.FRAME["EVENT"] and cid == r.anom_ch:
                m = decode_event(pl, anom_schema)
                if isinstance(m, dict):
                    yield "a", t_rel, hub_us, m


# =============================================================================
# Capture -> JSONL
# =============================================================================

def http_capabilities(ip, http_port, timeout=2.0):
    """fw_version and friends, best-effort. A hub with no HTTP face is a
    perfectly good SlopSync hub; the trace just records null."""
    try:
        with urllib.request.urlopen("http://%s:%d/api/capabilities" % (ip, http_port),
                                    timeout=timeout) as f:
            return json.loads(f.read().decode("utf-8", "replace"))
    except Exception:
        return None


def build_header(sess, args, caps, etag_verified, digest):
    r, cat = sess.res, sess.cat
    w = sess.welcome
    etag = w.get(ss.K["catalog_etag"])
    ident = w.get(ss.K["identity"])
    if isinstance(ident, dict):
        ident = {(ss_key_name(k) or "k%s" % k): (v.hex() if isinstance(v, (bytes, bytearray)) else v)
                 for k, v in ident.items()}

    chans = {}
    for label, cid in (("motion", r.motion_ch), ("plan_strip", r.plan_ch),
                       ("machine_config", r.window_ch), ("diag", r.diag_ch),
                       ("anomaly", r.anom_ch), ("safety", r.safety_ch)):
        if cid is None:
            continue
        chans[label] = {
            "id": cid, "name": cat.name(cid), "cls": cat.cls(cid),
            "catalog_rate_hz": cat.rate(cid), "granted_rate_hz": sess.grants.get(cid),
            "layout": [{"name": f["name"], "type": f["type_name"], "unit": f["unit"],
                        "scale": f["scale"], "role": f["role"], "group": f["group"],
                        "desc": f["desc"], "options": f["options"], "bits": f["bits"]}
                       for f in cat.layout(cid)],
            # CBOR-keyed channels (the EVENT feed) carry their vocabulary here,
            # including the `options` label list that names each anomaly kind.
            "schema": {str(k): {"name": f["name"], "type": f["type_name"], "unit": f["unit"],
                                "desc": f["desc"], "options": f["options"]}
                       for k, f in cat.schema(cid).items()},
        }

    return {
        "rec": "header",
        "tool": TOOL_NAME, "tool_version": TOOL_VERSION,
        "trace_format": TRACE_FORMAT_VERSION,
        "started_unix": sess.t0,
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(sess.t0)),
        "hub": {
            "ip": args.ip, "ws_port": args.port, "http_port": args.http_port,
            "proto_ver": ss.PROTO_VER,
            "session_id": w.get(ss.K["session_id"]),
            "boot_id": w.get(ss.K["boot_id"]),
            "cfg_gen": w.get(ss.K["cfg_gen"]),
            "roles": w.get(ss.K["roles"]),
            "deadman_ms": w.get(ss.K["deadman_ms"]),
            "identity": ident,
            "catalog_etag": bytes(etag).hex() if isinstance(etag, (bytes, bytearray)) else None,
            "catalog_etag_verified": bool(etag_verified),
            "catalog_sha256_8": digest.hex() if digest else None,
            "catalog_bytes": len(cat.raw) if cat and cat.raw else 0,
        },
        "firmware": {
            "fw_version": (caps or {}).get("fw_version"),
            "device": ((caps or {}).get("device") or (caps or {}).get("name")
                       or ("slopsim (SIMULATED MACHINE)" if (caps or {}).get("sim") else None)
                       or (ident or {}).get("hub_name")),
            "motion_backend": ((caps or {}).get("features") or {}).get("motion_backend"),
            "simulated": bool((caps or {}).get("sim")),
            "source": "GET /api/capabilities" if caps else "unavailable",
        },
        "clock": {
            "offset_us": sess.clock_offset_us, "rtt_us": sess.clock_rtt_us,
            "note": ("t is client seconds since capture start; t_hub is the hub-us estimate "
                     "(client + CLOCK offset, u32 wrap domain). STATE frames carry no hub "
                     "timestamp of their own -- only the anomaly body's t_us is a true hub "
                     "stamp, and it is recorded verbatim."),
        },
        "window": None,        # filled by the first config record; see capture()
        "limits": None,
        "resolution": r.how,
        "series": {
            "asked": {"channel": r.motion_ch, "field": r.f_asked, "unit": "mm"},
            "planned": {"channel": r.motion_ch, "field": r.f_planned, "unit": "mm"},
            "achieved": {"channel": r.motion_ch, "field": r.f_achieved, "unit": "mm"},
            "velocity": {"channel": r.motion_ch, "field": r.f_velocity, "unit": "mm/s"},
        },
        # The full layout of every channel recorded, so `render` is a pure
        # offline function of the trace: it re-resolves roles and field names
        # from THIS block, never from a live hub.
        "channels": chans,
        "catalog_summary": cat.summary() if cat else [],
        "safety": ("SlopScope subscribes only: watch tier, no publishes wish, "
                   "no INTENT frame is built anywhere in the tool."),
    }


def window_from(cfg, res):
    """(min_mm, max_mm) out of a decoded machine-config record, by role."""
    if not cfg or res.window_min_field is None:
        return None
    lo = cfg.get(res.window_min_field)
    hi = cfg.get(res.window_max_field)
    if lo is None or hi is None:
        return None
    return float(lo), float(hi)


def capture(args, on_sample=None):
    """Run a session and write a JSONL trace. `on_sample(kind, rec, state)`
    lets `live` render a dashboard off the same pump."""
    sess = ScopeSession(args)
    url = sess.connect()
    print("[slopscope] connected %s (subprotocol %s)" % (url, ss.WS_SUBPROTOCOL))
    try:
        sess.hello()
        role = sess.welcome.get(ss.K["roles"])
        print("[slopscope] WELCOME session=%s roles=%s (watch tier is all this tool needs)"
              % (sess.welcome.get(ss.K["session_id"]), role))
        verified, digest = sess.fetch_catalog()
        print("[slopscope] catalog %d B, %d entries, etag %s"
              % (len(sess.cat.raw), len(sess.cat.entries),
                 "VERIFIED" if verified else "MISMATCH"))
        sess.declare_ready()
        sess.sync_clock()

        miss = sess.res.missing()
        for line in ("  resolve %-11s %s" % (k, v) for k, v in sess.res.how.items()):
            print(line)
        if miss:
            raise RuntimeError("this hub does not expose what SlopScope plots: "
                               + "; ".join(miss))

        grants = sess.subscribe()
        print("[slopscope] grants: %s" % ", ".join(
            "%s=%s" % (sess.cat.name(c) or "0x%04X" % c,
                       ("%.1fHz" % g) if g else ("on-change" if g == 0 else "DENIED"))
            for c, g in sorted(grants.items(), key=lambda kv: kv[0])))

        sess.t0 = time.time()
        caps = http_capabilities(args.ip, args.http_port)
        header = build_header(sess, args, caps, verified, digest)

        out = None
        if args.out:
            out = io.open(args.out, "w", encoding="utf-8", newline="\n")

        state = {"counts": {}, "window": None, "limits": None, "diag": None,
                 "anomalies": [], "safety": None, "first": {}, "last": {},
                 "div": [], "header": header}
        if sess.res.window_override:
            lo, hi = sess.res.window_override
            state["window"] = {"min_mm": lo, "max_mm": hi, "span_mm": hi - lo,
                               "resolved_by": sess.res.how.get("window")}
            header["window"] = state["window"]

        def emit(obj):
            if out is not None:
                out.write(json.dumps(obj, ensure_ascii=False, separators=(",", ":")))
                out.write("\n")

        # The header is written LAST-KNOWN-GOOD at close (so window/limits are
        # filled), but a trace must be readable even if the tool is killed --
        # so it is also written FIRST, immediately, in its initial form. The
        # reader takes the LAST header record it sees.
        emit(header)

        deadline = time.time() + args.seconds if args.seconds else float("inf")
        stop = False
        try:
            for kind, t_rel, hub_us, dec in sess.pump(deadline):
                state["counts"][kind] = state["counts"].get(kind, 0) + 1
                rec = {"r": kind, "t": round(t_rel, 4)}
                if hub_us is not None:
                    rec["th"] = hub_us
                rec["v"] = _round_values(dec)
                if kind == "c":
                    win = window_from(dec, sess.res)
                    if win:
                        state["window"] = {"min_mm": win[0], "max_mm": win[1],
                                           "span_mm": win[1] - win[0],
                                           "resolved_by": sess.res.how.get("window")}
                    state["limits"] = {role: dec.get(f)
                                       for role, (c, f) in sess.res.limit_fields.items()
                                       if c == sess.res.window_ch}
                elif kind == "d":
                    state["diag"] = dec
                elif kind == "a":
                    state["anomalies"].append((t_rel, dec))
                elif kind == "s":
                    state["safety"] = dec
                elif kind == "m":
                    state["last"] = dec
                    _push_div(state, sess.res, dec)
                emit(rec)
                if on_sample:
                    on_sample(kind, rec, state)
        except KeyboardInterrupt:
            stop = True

        header["window"] = state["window"]
        header["limits"] = state["limits"]
        header["ended_unix"] = time.time()
        header["duration_s"] = round(time.time() - sess.t0, 3)
        header["counts"] = dict(state["counts"])
        header["stopped_by"] = "ctrl-c" if stop else ("duration" if args.seconds else "eof")
        emit(header)
        if out is not None:
            out.close()
        return header, state
    finally:
        sess.goodbye()


def _round_values(d):
    out = {}
    for k, v in d.items():
        if isinstance(v, float):
            out[k] = round(v, 4)
        else:
            out[k] = v
    return out


def _push_div(state, res, dec):
    """Rolling asked-vs-achieved divergence, in mm."""
    if not res.f_asked or not res.f_achieved:
        return
    a, p = dec.get(res.f_asked), dec.get(res.f_achieved)
    t = dec.get(res.f_planned)
    if a is None or p is None:
        return
    state["div"].append((a - p, (a - t) if t is not None else 0.0))
    if len(state["div"]) > 300:
        del state["div"][:len(state["div"]) - 300]


# =============================================================================
# Live terminal dashboard
# =============================================================================

BLOCKS = " ▁▂▃▄▅▆▇█"


def _spark(vals, width=32):
    if not vals:
        return " " * width
    v = vals[-width:]
    lo, hi = min(v), max(v)
    if hi - lo < 1e-9:
        return BLOCKS[1] * len(v)
    return "".join(BLOCKS[min(8, max(0, int((x - lo) / (hi - lo) * 8)))] for x in v)


def _pct(vals, q):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(q * (len(s) - 1)))]


def run_live(args):
    # Windows consoles need one no-op system call to switch on VT sequences.
    if os.name == "nt":
        os.system("")
    last_paint = [0.0]
    rates = {}

    def paint(kind, rec, state):
        now = time.time()
        rates.setdefault(kind, []).append(now)
        for k, v in rates.items():
            cutoff = now - 3.0
            while v and v[0] < cutoff:
                v.pop(0)
        if now - last_paint[0] < 0.25:
            return
        last_paint[0] = now
        h = state["header"]
        res_names = h["series"]
        win = state["window"]
        last = state["last"]
        div = state["div"]
        d_ap = [abs(x[0]) for x in div]
        d_at = [abs(x[1]) for x in div]

        L = []
        L.append("\x1b[H\x1b[J")
        L.append("SlopScope live  ·  %s:%d  ·  fw %s  ·  watch tier, subscribe-only" % (
            h["hub"]["ip"], h["hub"]["ws_port"], h["firmware"]["fw_version"] or "?"))
        L.append("-" * 78)
        if win:
            L.append("window   %.1f .. %.1f mm  (span %.1f)   limits %s" % (
                win["min_mm"], win["max_mm"], win["span_mm"],
                " ".join("%s=%g" % (k[6:] if k.startswith("limit.") else k, v)
                         for k, v in sorted((state["limits"] or {}).items()) if v is not None)))
        else:
            L.append("window   (waiting for the machine-config snapshot)")
        L.append("")
        L.append("rates    " + "  ".join(
            "%s %5.1fHz" % (n, len(rates.get(k, [])) / 3.0)
            for k, n in (("m", "motion"), ("p", "plan"), ("d", "diag"), ("a", "anom"))))
        L.append("")
        fa, fp, fv = res_names["asked"]["field"], res_names["planned"]["field"], \
            res_names["velocity"]["field"]
        fac = res_names["achieved"]["field"]
        L.append("position asked %8s   planned %8s   achieved %8s   vel %8s" % (
            _fmt(last.get(fa)), _fmt(last.get(fp)), _fmt(last.get(fac)), _fmt(last.get(fv))))
        L.append("")
        L.append("divergence  asked-vs-achieved   mean %6.2f mm  p95 %6.2f  max %6.2f" % (
            (sum(d_ap) / len(d_ap)) if d_ap else 0.0, _pct(d_ap, 0.95), max(d_ap) if d_ap else 0.0))
        L.append("            asked-vs-planned    mean %6.2f mm  p95 %6.2f  max %6.2f" % (
            (sum(d_at) / len(d_at)) if d_at else 0.0, _pct(d_at, 0.95), max(d_at) if d_at else 0.0))
        L.append("            |asked-achieved|  " + _spark(d_ap, 46))
        L.append("")
        diag = state["diag"] or {}
        if diag:
            L.append("planner  plans %-8s failures %-6s anomalies %-6s  plan_us last/max/avg %s/%s/%s" % (
                diag.get("plans"), diag.get("failures"), diag.get("anomalies"),
                diag.get("plan_us_last"), diag.get("plan_us_max"), _fmt(diag.get("plan_us_avg"))))
            kinds = [(k, v) for k, v in diag.items() if k.startswith("anom_") and v]
            L.append("anomaly  " + ("  ".join("%s=%d" % (k[5:], v) for k, v in kinds)
                                    if kinds else "(none counted)"))
            L.append("ingress  " + "  ".join("%s=%s" % (k[5:], diag.get(k)) for k in
                                             ("sync_bundles", "sync_samples", "sync_enqueued",
                                              "sync_dropped", "sync_seg_bundles")
                                             if k in diag))
        L.append("")
        recent = state["anomalies"][-6:]
        L.append("recent anomaly EVENTs (%d total)" % len(state["anomalies"]))
        labels = _anom_labels(h)
        for t, a in recent:
            body = a.get("body") or {}
            kind = a.get("event_kind", body.get("kind"))
            L.append("   t=%7.2fs  %-18s target=%-8s detail=%s" % (
                t, labels.get(kind, "kind %s" % kind), _fmt(body.get("target")),
                _fmt(body.get("detail"))))
        if not recent:
            L.append("   (none)")
        L.append("")
        L.append("ctrl-c to stop" + ("  ·  recording -> %s" % args.out if args.out else ""))
        sys.stdout.write("\n".join(L) + "\n")
        sys.stdout.flush()

    header, state = capture(args, on_sample=paint)
    print("\n[slopscope] %s" % _summary_line(header, state))
    return 0


def _fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return "%.2f" % v
    return str(v)


def _summary_line(header, state):
    c = header.get("counts") or {}
    return "captured %.1fs: %d motion, %d plan, %d diag, %d anomaly%s" % (
        header.get("duration_s", 0.0), c.get("m", 0), c.get("p", 0), c.get("d", 0), c.get("a", 0),
        ("  -> %s" % header.get("_out")) if header.get("_out") else "")


# =============================================================================
# Render: one self-contained HTML file
#
# WHY HAND-ROLLED SVG AND NOT A PLOTTING LIBRARY
#   The output has to open on a bench laptop with no internet and no Python
#   environment, months later, and be e-mailable as one file. A CDN <script>
#   is out by the brief; a bundled JS charting library would be a vendored
#   dependency this repo would then own; a matplotlib PNG cannot be zoomed,
#   and would make the RENDER path depend on a package the record path does
#   not need. So: the page carries its data as JSON and draws itself with
#   ~200 lines of inline JS into inline SVG. Stdlib only, no network, no
#   fonts to fetch (system stacks), and it zooms.
# =============================================================================

# ---- palette ---------------------------------------------------------------
# Semantic, taken from the product's own tokens so an operator recognizes it
# instantly:
#   webui/src/style.css      --reality #4DA6FF (measured/achieved)
#                            --intent  #A78BFA (commanded/unconfirmed)
#                            --warn    #F5B94D / --bad #FF4757 (safety only)
#   docs-site .../datasheet.css  the light-ground re-tuning of the same pair
#
# PLANNED is the third line and the brief asked for a justified choice. It is
# drawn as a DEEPER STEP OF THE REALITY BLUE, not a new hue: the planner's
# target is the hub's ACCEPTED intent -- already through arbitration, clamping
# and the window, i.e. across the boundary and on reality's side. Same hue =
# same family; the lightness step says "accepted, not yet executed". It is also
# the color-blind-safest choice available, because lightness is the one
# channel every CVD type keeps.
#
# HONEST NOTE ON THE PRODUCT PALETTE (measured, not guessed -- ΔE in OKLab×100
# under Machado 2009 severity 1.0): #4DA6FF vs #A78BFA is ΔE 1.1 under
# deuteranopia and 11.4 under normal vision. The product's reality/intent pair
# is therefore NOT distinguishable by color alone. Recognition across the
# whole product is the requirement here, so the pair is kept -- and every
# series carries mandatory secondary encoding (its own dash pattern, a legend
# key drawn in that pattern, a direct end-label and a named crosshair readout).
# The `cvd` palette in the toolbar re-steps the same semantic families to a set
# that passes: light #7B4BE0/#0d7f5f/#14539e passes all six checks; dark
# #C4A0FF/#14A89A/#2E90E8 passes CVD, normal-vision, chroma and contrast.
PALETTES = {
    "product": {
        "dark": dict(asked="#A78BFA", planned="#2E7FD6", achieved="#4DA6FF",
                     warn="#F5B94D", bad="#FF4757"),
        "light": dict(asked="#6234c4", planned="#4a86c4", achieved="#14539e",
                      warn="#8a5200", bad="#ab1f2c"),
    },
    "cvd": {
        "dark": dict(asked="#C4A0FF", planned="#14A89A", achieved="#2E90E8",
                     warn="#F5B94D", bad="#FF4757"),
        "light": dict(asked="#7B4BE0", planned="#0d7f5f", achieved="#14539e",
                      warn="#8a5200", bad="#ab1f2c"),
    },
}


def read_trace(path):
    header, recs = None, []
    with io.open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                o = json.loads(line)
            except ValueError:
                continue
            if o.get("rec") == "header":
                header = o          # last one wins: it has the final window/counts
            else:
                recs.append(o)
    if header is None:
        raise RuntimeError("%s has no header record -- not a SlopScope trace" % path)
    return header, recs


def build_plot_data(header, recs):
    """Trace -> the arrays the page draws. All position values in mm; the
    plan-strip's normalized values are converted with the window that was in
    force AT THAT MOMENT (the window is live STATE and can move mid-trace)."""
    S = header["series"]
    f_asked, f_planned = S["asked"]["field"], S["planned"]["field"]
    f_ach, f_vel = S["achieved"]["field"], S["velocity"]["field"]

    win = header.get("window") or {}
    wmin = win.get("min_mm")
    wmax = win.get("max_mm")

    t, asked, planned, achieved, vel, flags = [], [], [], [], [], []
    pt, pvel, pcur = [], [], []
    segs, anoms, cfgs = [], [], []
    cur_win = (wmin, wmax) if wmin is not None else (None, None)

    # a plan-strip sample opens a NEW segment when its (start,end,duration)
    # identity changes or its elapsed clock runs backwards (a replan).
    open_seg = None
    last_elapsed = None

    for o in recs:
        k, tt = o.get("r"), o.get("t")
        v = o.get("v") or {}
        if k == "c":
            lo, hi = v.get(_role_field(header, "window", "min")), \
                v.get(_role_field(header, "window", "max"))
            if lo is not None and hi is not None:
                cur_win = (float(lo), float(hi))
                cfgs.append({"t": tt, "min": cur_win[0], "max": cur_win[1]})
        elif k == "m":
            t.append(tt)
            asked.append(_g(v, f_asked))
            planned.append(_g(v, f_planned))
            achieved.append(_g(v, f_ach))
            vel.append(_g(v, f_vel))
            flags.append(v.get("flags"))
        elif k == "p":
            lo, hi = cur_win
            span = (hi - lo) if (lo is not None and hi is not None) else None
            def mm(x):
                return None if (x is None or span is None) else lo + x * span
            pt.append(tt)
            pcur.append(mm(v.get("cur_norm")))
            pvel.append(None if (v.get("cur_vel") is None or span is None)
                        else v.get("cur_vel") * span)
            dur = v.get("duration_us") or 0
            el = v.get("elapsed_us") or 0
            active = bool((v.get("flags") or 0) & 1)
            ident = (v.get("start_norm"), v.get("end_norm"), dur, v.get("style"))
            replan = (last_elapsed is not None and el < last_elapsed)
            if active and dur > 0:
                if open_seg is None or open_seg["ident"] != ident or replan:
                    open_seg = {
                        "ident": ident,
                        "t0": tt - el / 1e6,
                        "t1": tt - el / 1e6 + dur / 1e6,
                        "start": mm(v.get("start_norm")), "end": mm(v.get("end_norm")),
                        "style": v.get("style"), "dur_ms": dur / 1000.0,
                    }
                    segs.append(open_seg)
            else:
                open_seg = None
            last_elapsed = el
        elif k == "a":
            body = v.get("body") if isinstance(v.get("body"), dict) else {}
            anoms.append({
                "t": tt,
                # event_kind (registry key 33) is the PROTOCOL's discriminator
                # and the thing a generic client switches on; the body's `kind`
                # mirrors it so the label list has somewhere to hang. Prefer the
                # protocol one, fall back to the mirror.
                "kind": v.get("event_kind", body.get("kind")),
                "seq": body.get("seq"),
                "target": body.get("target"),
                # the anomaly's provoking target is normalized 0..1 across the
                # window (its catalog unit says so) -- mapped to mm here with
                # the window in force at that instant, so it is comparable with
                # every other number on the position plot.
                "target_mm": (None if (body.get("target") is None or cur_win[0] is None)
                              else cur_win[0] + body["target"] * (cur_win[1] - cur_win[0])),
                "detail": body.get("detail"),
                "t_us": body.get("t_us"),
            })

    for s in segs:
        s.pop("ident", None)

    # anomaly kind -> label, from the DIAG channel's own anom_* field names
    # where available (catalog-driven), falling back to the numeric kind.
    labels = _anom_labels(header)
    for a in anoms:
        a["label"] = labels.get(a["kind"], "kind %s" % a["kind"])

    return {
        "t": _r(t), "asked": _r(asked), "planned": _r(planned),
        "achieved": _r(achieved), "vel": _r(vel), "flags": flags,
        "pt": _r(pt), "pcur": _r(pcur), "pvel": _r(pvel),
        "segs": segs, "anoms": anoms, "cfgs": cfgs,
        "window": {"min": wmin, "max": wmax},
    }


def _role_field(header, group, which):
    """The machine-config field name that carried window.min / window.max.
    Recorded in the header's channel layout by role, so this stays a role
    lookup even at render time, offline, months later."""
    ch = (header.get("channels") or {}).get("machine_config") or {}
    role = "window.min" if which == "min" else "window.max"
    for f in ch.get("layout", []):
        if f.get("role") == role:
            return f["name"]
    return "window_min" if which == "min" else "window_max"


def _anom_labels(header):
    """kind number -> name. FIRST from the anomaly channel's own `options`
    label list (the registered mechanism), THEN from the diag channel's
    anom_<name> counter fields (the same vocabulary in the same order), and
    only then numeric. Both sources come out of the trace header, so this works
    offline against a hub that no longer exists."""
    chans = header.get("channels") or {}
    sch = (chans.get("anomaly") or {}).get("schema") or {}
    for f in sch.values():
        if f.get("name") == "kind" and isinstance(f.get("options"), list):
            return {i: n for i, n in enumerate(f["options"])}
    out, idx = {}, 0
    for f in (chans.get("diag") or {}).get("layout", []):
        n = f.get("name") or ""
        if n.startswith("anom_"):
            out[idx] = n[5:]
            idx += 1
    return out


def _g(v, key):
    return None if key is None else v.get(key)


def _r(xs):
    return [None if x is None else round(float(x), 4) for x in xs]


def render(header, recs, out_path, theme="dark", palette="product"):
    data = build_plot_data(header, recs)
    meta = _meta_rows(header, data)
    html = HTML_TEMPLATE
    html = html.replace("__TITLE__", _esc(_title(header)))
    html = html.replace("__META__", json.dumps(meta, ensure_ascii=False))
    html = html.replace("__DATA__", json.dumps(data, ensure_ascii=False, separators=(",", ":")))
    html = html.replace("__PALETTES__", json.dumps(PALETTES, ensure_ascii=False))
    html = html.replace("__THEME__", theme)
    html = html.replace("__PALNAME__", palette)
    html = html.replace("__HEADER__", json.dumps(header, ensure_ascii=False))
    with io.open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)
    return out_path, data


def _title(header):
    fw = (header.get("firmware") or {}).get("fw_version")
    return "SlopScope %s %s" % (header.get("started_utc", ""), ("fw %s" % fw) if fw else "")


def _meta_rows(header, data):
    w = header.get("window") or {}
    lim = header.get("limits") or {}
    c = header.get("counts") or {}
    hub = header.get("hub") or {}
    rows = [
        ("device", "%s  ·  fw %s" % ((header.get("firmware") or {}).get("device") or "?",
                                     (header.get("firmware") or {}).get("fw_version") or "?")),
        ("captured", "%s  ·  %.1f s  ·  stopped by %s" % (
            header.get("started_utc"), header.get("duration_s") or 0.0,
            header.get("stopped_by") or "?")),
        ("hub", "%s:%s  session %s  boot 0x%08X" % (
            hub.get("ip"), hub.get("ws_port"), hub.get("session_id"), hub.get("boot_id") or 0)),
        ("catalog", "etag %s  (%s, %s B)" % (
            hub.get("catalog_etag"),
            "verified" if hub.get("catalog_etag_verified") else "UNVERIFIED",
            hub.get("catalog_bytes"))),
        ("window", ("%.1f .. %.1f mm  (span %.1f)" % (w.get("min_mm"), w.get("max_mm"),
                                                      w.get("span_mm"))
                    if w.get("min_mm") is not None else "UNRESOLVED")),
        ("limits", "  ".join("%s %g" % (k.replace("limit.", ""), v)
                             for k, v in sorted(lim.items()) if v is not None) or "-"),
        ("frames", "motion %d · plan %d · config %d · diag %d · anomaly %d" % (
            c.get("m", 0), c.get("p", 0), c.get("c", 0), c.get("d", 0), c.get("a", 0))),
        ("clock", "hub offset %s us, rtt %s us" % ((header.get("clock") or {}).get("offset_us"),
                                                   (header.get("clock") or {}).get("rtt_us"))),
    ]
    res = header.get("resolution") or {}
    for k in ("window", "achieved", "planned", "asked", "velocity", "plan_strip", "anomaly"):
        if k in res:
            rows.append(("resolve · " + k, res[k]))
    return rows


def _esc(s):
    return (s or "").replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# =============================================================================
# CLI
# =============================================================================

def _window_arg(s):
    try:
        lo, hi = s.split(":")
        lo, hi = float(lo), float(hi)
    except Exception:
        raise argparse.ArgumentTypeError("--window wants MIN:MAX in mm, e.g. 0:180")
    if hi <= lo:
        raise argparse.ArgumentTypeError("--window MAX must exceed MIN")
    return (lo, hi)


def add_conn_args(p):
    p.add_argument("--ip", default="127.0.0.1", help="hub address (default 127.0.0.1)")
    p.add_argument("--port", type=int, default=82, help="SlopSync WS port (default 82)")
    p.add_argument("--http-port", type=int, default=80,
                   help="HTTP port for the optional /api/capabilities read (default 80)")
    p.add_argument("--timeout", type=float, default=5.0, help="per-step timeout, seconds")
    p.add_argument("--window", type=_window_arg, default=None, metavar="MIN:MAX",
                   help="ESCAPE HATCH for a hub whose catalog carries no window.min/window.max "
                        "field_role: supply the stroke window in mm yourself. Recorded in the "
                        "trace as an operator override, never as machine truth.")


def main():
    ap = argparse.ArgumentParser(
        prog="slopscope",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "SlopScope -- graph what SlopSync was ASKED for against what the motion planner\n"
            "PLANNED and what the machine ACHIEVED, in mm inside the stroke window.\n"
            "\n"
            "SAFETY: SlopScope connects as a `watch`-tier session and SUBSCRIBES ONLY.\n"
            "It sends no INTENT, publishes no STREAM, and carries no `publishes` wish in\n"
            "its HELLO -- it is structurally incapable of commanding motion. Drive the\n"
            "machine from somewhere else and watch it here."),
        epilog=(
            "examples:\n"
            "  slopscope.py record --ip 127.0.0.1 --seconds 20 --out run.jsonl --render run.html\n"
            "  slopscope.py live   --ip 192.168.1.229 --seconds 60\n"
            "  slopscope.py render run.jsonl --out run.html --theme light\n"))
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("record", help="capture a JSONL trace")
    add_conn_args(p)
    p.add_argument("--seconds", type=float, default=20.0, help="capture length (0 = until ctrl-c)")
    p.add_argument("--out", default=None, help="trace file (default slopscope-<stamp>.jsonl)")
    p.add_argument("--render", default=None, metavar="HTML", help="also render the graph here")
    p.add_argument("--theme", default="dark", choices=("dark", "light"))
    p.add_argument("--palette", default="product", choices=tuple(PALETTES))

    p = sub.add_parser("live", help="terminal dashboard while streaming")
    add_conn_args(p)
    p.add_argument("--seconds", type=float, default=0.0, help="0 = until ctrl-c")
    p.add_argument("--out", default=None, help="also record a trace here")

    p = sub.add_parser("render", help="trace -> self-contained HTML graph")
    p.add_argument("trace")
    p.add_argument("--out", default=None)
    p.add_argument("--theme", default="dark", choices=("dark", "light"))
    p.add_argument("--palette", default="product", choices=tuple(PALETTES))

    args = ap.parse_args()
    if not args.cmd:
        ap.print_help()
        return 2

    if args.cmd == "render":
        header, recs = read_trace(args.trace)
        out = args.out or (os.path.splitext(args.trace)[0] + ".html")
        path, data = render(header, recs, out, args.theme, args.palette)
        print("[slopscope] %s  (%d motion, %d plan, %d segments, %d anomalies)"
              % (path, len(data["t"]), len(data["pt"]), len(data["segs"]), len(data["anoms"])))
        return 0

    if args.cmd == "live":
        args.render = None
        args.theme = "dark"
        args.palette = "product"
        if args.seconds == 0:
            args.seconds = 0.0
        try:
            return run_live(args)
        except RuntimeError as e:
            print("[slopscope] %s" % e)
            return 1

    # record
    if not args.out:
        args.out = "slopscope-%s.jsonl" % time.strftime("%Y%m%d-%H%M%S")
    try:
        header, state = capture(args)
    except RuntimeError as e:
        print("[slopscope] %s" % e)
        return 1
    print("[slopscope] %s -> %s" % (_summary_line(header, state), args.out))
    if args.render:
        h, recs = read_trace(args.out)
        path, data = render(h, recs, args.render, args.theme, args.palette)
        print("[slopscope] %s  (%d motion, %d plan, %d segments, %d anomalies)"
              % (path, len(data["t"]), len(data["pt"]), len(data["segs"]), len(data["anoms"])))
    return 0


HTML_TEMPLATE = r"""<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__TITLE__</title>
<style>
:root{
  --bg:#08090B; --screen:#040507; --line1:#1D2026; --line3:#33373E; --line4:#454A54;
  --tx-hi:#ECEFF4; --tx:#C3C8D1; --tx-mut:#666C78; --tx-ghost:#4D525C;
  --asked:#A78BFA; --planned:#2E7FD6; --achieved:#4DA6FF; --warn:#F5B94D; --bad:#FF4757;
  --mono: ui-monospace,"Cascadia Mono","Consolas","DejaVu Sans Mono",monospace;
}
:root[data-theme="light"]{
  --bg:#faf9f5; --screen:#f1efe7; --line1:#e0ddd4; --line3:#c8c4b8; --line4:#b9b4a6;
  --tx-hi:#15181c; --tx:#23262b; --tx-mut:#5b6168; --tx-ghost:#878d94;
  --asked:#6234c4; --planned:#4a86c4; --achieved:#14539e; --warn:#8a5200; --bad:#ab1f2c;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);font:13px/1.5 var(--mono);
     -webkit-font-smoothing:antialiased}
.wrap{max-width:1500px;margin:0 auto;padding:18px}
h1{font:600 15px/1.3 var(--mono);color:var(--tx-hi);margin:0 0 2px;letter-spacing:.02em}
.sub{color:var(--tx-mut);font-size:11px;margin-bottom:14px}
.bar{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin:10px 0 12px}
button,.tog{background:var(--screen);color:var(--tx);border:1px solid var(--line1);
  border-radius:2px;padding:4px 9px;font:11px/1.4 var(--mono);cursor:pointer}
button:hover,.tog:hover{border-color:var(--line4);color:var(--tx-hi)}
button[aria-pressed="true"]{border-color:var(--line4);color:var(--tx-hi);background:var(--bg)}
.spacer{flex:1}
.panel{background:var(--screen);border:1px solid var(--line1);border-radius:2px;padding:8px 6px 2px;margin-bottom:10px;overflow-x:auto}
svg{display:block;width:100%;touch-action:none}
.grid{stroke:var(--line3);stroke-opacity:.35;stroke-width:1;shape-rendering:crispEdges}
.axis{stroke:var(--line3);stroke-width:1;shape-rendering:crispEdges}
.tick{fill:var(--tx-mut);font:10px var(--mono)}
.axname{fill:var(--tx-ghost);font:10px var(--mono)}
.ser{fill:none;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}
.s-asked{stroke:var(--asked);stroke-dasharray:7 5}
.s-planned{stroke:var(--planned);stroke-dasharray:2 3}
.s-achieved{stroke:var(--achieved)}
.envfill{fill:var(--planned);fill-opacity:.10}
.envline{stroke:var(--planned);stroke-width:1;stroke-opacity:.55;fill:none;shape-rendering:crispEdges}
.winband{fill:var(--line3);fill-opacity:.10}
.anom{stroke:var(--warn);stroke-width:1;stroke-opacity:.75;shape-rendering:crispEdges}
.anom.sev{stroke:var(--bad)}
.anomlab{font:9px var(--mono);fill:var(--warn)}
.anomlab.sev{fill:var(--bad)}
.cross{stroke:var(--line4);stroke-width:1;shape-rendering:crispEdges;stroke-opacity:.8}
.dot{stroke:var(--screen);stroke-width:2}
.endlab{font:10px var(--mono)}
.legend{display:flex;gap:16px;flex-wrap:wrap;align-items:center;padding:2px 8px 8px;color:var(--tx-mut);font-size:11px}
.key{display:inline-flex;align-items:center;gap:6px}
.key svg{width:26px;height:8px;display:inline-block}
table{border-collapse:collapse;font-size:11px;width:100%}
td,th{border-bottom:1px solid var(--line1);padding:3px 8px;text-align:left;vertical-align:top}
th{color:var(--tx-mut);font-weight:400;white-space:nowrap;width:1%}
td{color:var(--tx)}
details{margin-bottom:10px;background:var(--screen);border:1px solid var(--line1);border-radius:2px;padding:6px 10px}
summary{cursor:pointer;color:var(--tx-mut);font-size:11px}
.readout{display:flex;gap:14px;flex-wrap:wrap;font-size:11px;color:var(--tx-mut);padding:0 8px 6px;min-height:19px}
.readout b{color:var(--tx-hi);font-weight:600}
.sw{display:inline-block;width:8px;height:8px;border-radius:1px;vertical-align:-1px;margin-right:4px}
.note{color:var(--tx-ghost);font-size:10px;padding:0 8px 8px}
</style>
<div class="wrap">
<h1>SlopScope — asked · planned · achieved</h1>
<div class="sub" id="sub"></div>

<div class="bar">
  <button id="b-asked" aria-pressed="true">asked</button>
  <button id="b-planned" aria-pressed="true">planned</button>
  <button id="b-achieved" aria-pressed="true">achieved</button>
  <button id="b-env" aria-pressed="true">plan envelope</button>
  <button id="b-anom" aria-pressed="true">anomalies</button>
  <span class="spacer"></span>
  <button id="b-pal">palette: product</button>
  <button id="b-theme">theme: dark</button>
  <button id="b-reset">reset zoom</button>
</div>

<div class="panel">
  <div class="readout" id="ro1"></div>
  <svg id="p1" viewBox="0 0 1400 430" preserveAspectRatio="none" height="430"></svg>
  <div class="legend" id="leg"></div>
</div>

<div class="panel">
  <div class="readout" id="ro2"></div>
  <svg id="p2" viewBox="0 0 1400 260" preserveAspectRatio="none" height="260"></svg>
  <div class="note">velocity plane — reported carriage speed (achieved) and the active plan's own
    velocity (planned), both mm/s. Separate plot, never a second y-axis on the position chart.</div>
</div>

<details><summary>trace metadata — what this graph is, and how each channel was found</summary>
  <table id="meta"></table>
</details>
<details><summary>anomaly log</summary><table id="anomtab"></table></details>
<details><summary>series summary (the table view — every value is reachable without color)</summary>
  <table id="stats"></table></details>
</div>

<script>
const META = __META__;
const D = __DATA__;
const PALETTES = __PALETTES__;
let theme = "__THEME__", palname = "__PALNAME__";
const show = {asked:true, planned:true, achieved:true, env:true, anom:true};

// anomaly kinds that mean "the machine could not do what it was told" get the
// bad color; the rest are shaping/limit events and get warn. Safety colors,
// safety meanings, nothing else.
const SEVERE = new Set(["plan_failed"]);

const W = 1400, H1 = 430, H2 = 260, ML = 62, MR = 124, MT = 16, MB = 26, RAIL = 22;

function applyPalette(){
  const p = PALETTES[palname][theme];
  const r = document.documentElement;
  for (const k of ["asked","planned","achieved","warn","bad"]) r.style.setProperty("--"+k, p[k]);
  r.setAttribute("data-theme", theme);
  document.getElementById("b-theme").textContent = "theme: " + theme;
  document.getElementById("b-pal").textContent = "palette: " + palname;
}

const tAll = D.t.concat(D.pt);
let T0 = tAll.length ? Math.min.apply(null, tAll) : 0;
let T1 = tAll.length ? Math.max.apply(null, tAll) : 1;
if (T1 - T0 < 1e-6) T1 = T0 + 1;
let view = [T0, T1];

function finite(a){ return a.filter(v => v !== null && isFinite(v)); }
function extent(arrs){
  let lo = Infinity, hi = -Infinity;
  for (const a of arrs) for (const v of a) if (v !== null && isFinite(v)) { if (v<lo) lo=v; if (v>hi) hi=v; }
  if (!isFinite(lo)) { lo = 0; hi = 1; }
  if (hi - lo < 1e-6) { hi = lo + 1; }
  return [lo, hi];
}
function nice(lo, hi, n){
  const raw = (hi - lo) / n, mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const norm = raw / mag, step = (norm<1.5?1:norm<3?2:norm<7?5:10) * mag;
  return {step, start: Math.ceil(lo/step)*step};
}
function fmt(v, d){ return v===null||v===undefined||!isFinite(v) ? "—" : v.toFixed(d===undefined?2:d); }

// ---- position extent: include the window band so the stroke window is always
// visible context, not something the autoscale can hide.
const posBase = [D.asked, D.planned, D.achieved, D.pcur];
let PY = extent(posBase);
if (D.window.min !== null && D.window.min !== undefined){
  PY = [Math.min(PY[0], D.window.min), Math.max(PY[1], D.window.max)];
}
const VY = extent([D.vel, D.pvel]);
function pad(e){ const m=(e[1]-e[0])*0.06; return [e[0]-m, e[1]+m]; }
const PYp = pad(PY), VYp = pad(VY);

function svgEl(n, a){ const e = document.createElementNS("http://www.w3.org/2000/svg", n);
  for (const k in a) e.setAttribute(k, a[k]); return e; }

function draw(){
  drawPlot(document.getElementById("p1"), H1, PYp, "position  mm", true);
  drawPlot(document.getElementById("p2"), H2, VYp, "velocity  mm/s", false);
  drawLegend();
  document.getElementById("ro1").innerHTML = hoverHTML(null, true);
  document.getElementById("ro2").innerHTML = "";
}

function xs(t, h){ return ML + (t - view[0]) / (view[1] - view[0]) * (W - ML - MR); }
function ysF(ext, h){ const top = MT + (h===H1?RAIL:0);
  return v => (h - MB) - (v - ext[0]) / (ext[1] - ext[0]) * ((h - MB) - top); }

function path(ts, vs, h, ext){
  const y = ysF(ext, h);
  let d = "", pen = false;
  for (let i=0;i<ts.length;i++){
    const v = vs[i];
    if (v === null || v === undefined || !isFinite(v)) { pen = false; continue; }
    if (ts[i] < view[0] - 1 || ts[i] > view[1] + 1) { pen = false; continue; }
    const X = xs(ts[i], h).toFixed(2), Y = y(v).toFixed(2);
    d += (pen ? "L" : "M") + X + " " + Y; pen = true;
  }
  return d;
}

function drawPlot(svg, h, ext, name, isPos){
  svg.setAttribute("viewBox", "0 0 " + W + " " + h);
  while (svg.firstChild) svg.removeChild(svg.firstChild);
  const y = ysF(ext, h), top = MT + (isPos?RAIL:0);

  // window band (position plot only) — the stroke window as physical context
  if (isPos && D.window.min !== null && D.window.min !== undefined){
    const a = y(D.window.max), b = y(D.window.min);
    svg.appendChild(svgEl("rect", {x:ML, y:Math.min(a,b), width:W-ML-MR,
      height:Math.abs(b-a), class:"winband"}));
  }

  // plan envelope: each planned segment as a shaded start→end span over its
  // commanded duration, with brackets at both ends.
  if (isPos && show.env){
    for (const s of D.segs){
      if (s.end === null || s.start === null) continue;
      if (s.t1 < view[0] || s.t0 > view[1]) continue;
      const x0 = xs(Math.max(s.t0, view[0]), h), x1 = xs(Math.min(s.t1, view[1]), h);
      if (x1 - x0 < 0.6) continue;
      const ya = y(s.start), yb = y(s.end);
      svg.appendChild(svgEl("rect", {x:x0, y:Math.min(ya,yb), width:Math.max(1,x1-x0),
        height:Math.abs(yb-ya), class:"envfill"}));
      svg.appendChild(svgEl("path", {class:"envline",
        d:"M"+x0+" "+ya+"L"+x0+" "+yb+"M"+x1+" "+ya+"L"+x1+" "+yb+"M"+x0+" "+yb+"L"+x1+" "+yb}));
    }
  }

  // grid + y ticks
  const ny = nice(ext[0], ext[1], 5);
  for (let v = ny.start; v <= ext[1]; v += ny.step){
    const Y = y(v);
    svg.appendChild(svgEl("line", {x1:ML, x2:W-MR, y1:Y, y2:Y, class:"grid"}));
    const tx = svgEl("text", {x:ML-7, y:Y+3, class:"tick", "text-anchor":"end"});
    tx.textContent = (Math.abs(ny.step)<1 ? v.toFixed(1) : Math.round(v).toLocaleString());
    svg.appendChild(tx);
  }
  // x ticks
  const nx = nice(view[0], view[1], 8);
  for (let v = nx.start; v <= view[1]; v += nx.step){
    const X = xs(v, h);
    svg.appendChild(svgEl("line", {x1:X, x2:X, y1:top, y2:h-MB, class:"grid"}));
    const tx = svgEl("text", {x:X, y:h-MB+13, class:"tick", "text-anchor":"middle"});
    tx.textContent = v.toFixed(nx.step < 0.5 ? 2 : 1) + "s";
    svg.appendChild(tx);
  }
  svg.appendChild(svgEl("line", {x1:ML, x2:W-MR, y1:h-MB, y2:h-MB, class:"axis"}));
  svg.appendChild(svgEl("line", {x1:ML, x2:ML, y1:top, y2:h-MB, class:"axis"}));
  const an = svgEl("text", {x:ML, y:top-5, class:"axname"}); an.textContent = name;
  svg.appendChild(an);

  // Anomaly markers live on their OWN RAIL above the plot, as short ticks.
  // Full-height hairlines are drawn only when few enough to stay readable --
  // at 273 events over a minute they turn into a solid amber barcode that
  // erases the data underneath, which is the opposite of a diagnostic.
  if (show.anom){
    const vis = D.anoms.filter(a => a.t >= view[0] && a.t <= view[1]);
    const hair = vis.length <= 40;
    let lastLab = -1e9, labeled = 0;
    for (const a of vis){
      const X = xs(a.t, h), sev = SEVERE.has(a.label);
      const cls = "anom" + (sev?" sev":"");
      svg.appendChild(svgEl("line",
        {x1:X, x2:X, y1:isPos?MT+11:top, y2:isPos?MT+RAIL-1:top+8, class:cls}));
      if (hair) svg.appendChild(svgEl("line",
        {x1:X, x2:X, y1:top, y2:h-MB, class:cls, "stroke-opacity":0.22}));
      if (isPos && X - lastLab > 120 && labeled < 12){
        const t = svgEl("text", {x:X+2, y:MT+8, class:"anomlab" + (sev?" sev":"")});
        t.textContent = a.label;
        svg.appendChild(t); lastLab = X; labeled++;
      }
    }
    if (isPos && vis.length){
      const c = svgEl("text", {x:W-MR, y:MT+8, class:"anomlab", "text-anchor":"end"});
      c.textContent = vis.length + " anomaly event" + (vis.length===1?"":"s")
        + (labeled < vis.length ? " (" + labeled + " labeled — zoom in for more)" : "");
      svg.appendChild(c);
    }
  }

  // series
  const S = isPos
    ? [["achieved", D.t, D.achieved, "s-achieved"],
       ["planned",  D.t, D.planned,  "s-planned"],
       ["asked",    D.t, D.asked,    "s-asked"]]
    : [["achieved", D.t, D.vel,  "s-achieved"],
       ["planned",  D.pt, D.pvel, "s-planned"]];
  // draw order: asked (back) → planned → achieved (front, it is the truth)
  for (const [k, ts, vs, cls] of S.slice().reverse()){
    if (!show[k]) continue;
    const d = path(ts, vs, h, ext);
    if (d) svg.appendChild(svgEl("path", {d:d, class:"ser " + cls}));
  }
  // Direct end-labels -- identity without relying on color. These series
  // CONVERGE (three lines describing one position), so the labels collide by
  // construction; they are nudged apart to a minimum gap and each keeps a
  // LEADER LINE back to its own end-dot, rather than being stacked loose where
  // they would stop belonging to any line.
  const ends = [];
  for (const [k, ts, vs] of S){
    if (!show[k]) continue;
    let li = -1;
    for (let i=ts.length-1;i>=0;i--){
      if (ts[i] <= view[1] && vs[i] !== null && isFinite(vs[i])) { li = i; break; }
    }
    if (li < 0) continue;
    ends.push({k, x:Math.min(W-MR, xs(ts[li], h)), y:y(vs[li]), v:vs[li]});
  }
  ends.sort((a,b) => a.y - b.y);
  const GAP = 12;
  for (let i=1;i<ends.length;i++)
    if (ends[i].y - ends[i-1].y < GAP) ends[i].y = ends[i-1].y + GAP;
  const overflow = ends.length ? ends[ends.length-1].y - (h - MB) : 0;
  if (overflow > 0) for (const e of ends) e.y -= overflow;
  for (const e of ends){
    const dy = Math.abs(e.y - y(e.v));
    if (dy > 1.5)
      svg.appendChild(svgEl("path", {d:"M"+(e.x+4)+" "+y(e.v)+"L"+(e.x+11)+" "+e.y,
        stroke:"var(--line4)", "stroke-width":1, fill:"none"}));
    svg.appendChild(svgEl("circle", {cx:e.x, cy:y(e.v), r:4, fill:"var(--"+e.k+")", class:"dot"}));
    const t = svgEl("text", {x:e.x+13, y:e.y+3, class:"endlab", fill:"var(--tx-mut)"});
    t.textContent = e.k + " " + fmt(e.v, 1);
    svg.appendChild(t);
  }
  svg.__ext = ext; svg.__h = h; svg.__isPos = isPos;
}

function drawLegend(){
  const l = document.getElementById("leg");
  const items = [
    ["asked", "asked — the demand as it arrived (0x1100 raw), intent"],
    ["planned", "planned — the planner's target + its segment envelope"],
    ["achieved", "achieved — where the carriage actually is, reality"],
  ];
  l.innerHTML = items.map(([k, label]) =>
    '<span class="key"><svg viewBox="0 0 26 8"><line x1="1" y1="4" x2="25" y2="4" '
    + 'class="ser s-' + k + '" stroke="var(--' + k + ')"/></svg>' + label + '</span>').join("");
}

// ---- hover / crosshair ------------------------------------------------------
function nearest(ts, t){
  let lo = 0, hi = ts.length - 1;
  if (!ts.length) return -1;
  while (lo < hi){ const m = (lo+hi)>>1; if (ts[m] < t) lo = m+1; else hi = m; }
  if (lo > 0 && Math.abs(ts[lo-1]-t) < Math.abs(ts[lo]-t)) lo--;
  return lo;
}
function hoverHTML(t, isPos){
  if (t === null){
    return '<span>hover for values · wheel = zoom · drag = pan · double-click = reset</span>';
  }
  const i = nearest(D.t, t), j = nearest(D.pt, t);
  const sw = k => '<span class="sw" style="background:var(--'+k+')"></span>';
  if (isPos){
    const a = D.asked[i], p = D.planned[i], c = D.achieved[i];
    let s = '<span>t <b>'+t.toFixed(3)+'s</b></span>'
      + '<span>'+sw("asked")+'asked <b>'+fmt(a)+'</b> mm</span>'
      + '<span>'+sw("planned")+'planned <b>'+fmt(p)+'</b> mm</span>'
      + '<span>'+sw("achieved")+'achieved <b>'+fmt(c)+'</b> mm</span>';
    if (a!==null && c!==null) s += '<span>asked−achieved <b>'+fmt(a-c)+'</b> mm</span>';
    if (a!==null && p!==null) s += '<span>asked−planned <b>'+fmt(a-p)+'</b> mm</span>';
    const seg = D.segs.filter(x => x.t0 <= t && t <= x.t1).pop();
    if (seg) s += '<span>plan <b>'+fmt(seg.start,1)+'→'+fmt(seg.end,1)+'</b> mm over <b>'
      + fmt(seg.dur_ms,0)+'</b> ms (style '+seg.style+')</span>';
    return s;
  }
  return '<span>t <b>'+t.toFixed(3)+'s</b></span>'
    + '<span>'+sw("achieved")+'reported <b>'+fmt(D.vel[i])+'</b> mm/s</span>'
    + '<span>'+sw("planned")+'plan vel <b>'+fmt(D.pvel[j])+'</b> mm/s</span>';
}

function wire(svg, roId){
  let drag = null;
  const toT = ev => {
    const r = svg.getBoundingClientRect();
    const x = (ev.clientX - r.left) / r.width * W;
    return view[0] + (x - ML) / (W - ML - MR) * (view[1] - view[0]);
  };
  svg.addEventListener("mousemove", ev => {
    const t = toT(ev);
    if (drag !== null){
      const dt = drag.t - t, span = view[1]-view[0];
      let a = drag.v0 + dt, b = a + span;
      if (a < T0) { a = T0; b = a + span; }
      if (b > T1) { b = T1; a = b - span; }
      view = [a, b]; draw(); return;
    }
    document.getElementById(roId).innerHTML = hoverHTML(t, svg.__isPos);
    const old = svg.querySelector(".cross"); if (old) old.remove();
    const X = xs(t, svg.__h);
    if (X >= ML && X <= W-MR)
      svg.appendChild(svgEl("line", {x1:X, x2:X, y1:MT, y2:svg.__h-MB, class:"cross"}));
  });
  svg.addEventListener("mouseleave", () => {
    const old = svg.querySelector(".cross"); if (old) old.remove();
    document.getElementById(roId).innerHTML = hoverHTML(null, svg.__isPos);
  });
  svg.addEventListener("mousedown", ev => { drag = {t: toT(ev), v0: view[0]}; ev.preventDefault(); });
  window.addEventListener("mouseup", () => { drag = null; });
  svg.addEventListener("dblclick", () => { view = [T0, T1]; draw(); });
  svg.addEventListener("wheel", ev => {
    ev.preventDefault();
    const t = toT(ev), f = ev.deltaY > 0 ? 1.25 : 0.8;
    let a = t - (t - view[0]) * f, b = t + (view[1] - t) * f;
    if (b - a < 0.02) return;
    view = [Math.max(T0, a), Math.min(T1, b)];
    draw();
  }, {passive:false});
}

// ---- tables -----------------------------------------------------------------
function tables(){
  document.getElementById("meta").innerHTML = META.map(
    ([k,v]) => "<tr><th>"+k+"</th><td>"+String(v).replace(/</g,"&lt;")+"</td></tr>").join("");
  const counts = {};
  for (const a of D.anoms) counts[a.label] = (counts[a.label]||0) + 1;
  const at = D.anoms.length
    ? "<tr><th colspan='6'>" + Object.keys(counts).sort().map(k => k+" &times; "+counts[k]).join(" &middot; ")
      + "</th></tr>"
      + "<tr><th>t (s)</th><th>kind</th><th>target mm</th><th>target norm</th>"
      + "<th>detail</th><th>hub t_us</th></tr>"
      + D.anoms.map(a => "<tr><td>"+a.t.toFixed(3)+"</td><td>"+a.label+"</td><td>"
        + fmt(a.target_mm,1)+"</td><td>"+fmt(a.target,4)+"</td><td>"+fmt(a.detail,4)
        + "</td><td>"+(a.t_us===undefined?"—":a.t_us)+"</td></tr>").join("")
    : "<tr><td>no anomaly EVENTs in this trace</td></tr>";
  document.getElementById("anomtab").innerHTML = at;

  const rows = [];
  function stat(name, arr, unit){
    const f = arr.filter(v => v!==null && isFinite(v));
    if (!f.length) { rows.push([name, "—","—","—","—"]); return; }
    const mean = f.reduce((a,b)=>a+b,0)/f.length;
    rows.push([name+" ("+unit+")", fmt(Math.min.apply(null,f)), fmt(Math.max.apply(null,f)),
               fmt(mean), f.length]);
  }
  stat("asked", D.asked, "mm"); stat("planned", D.planned, "mm");
  stat("achieved", D.achieved, "mm");
  stat("reported velocity", D.vel, "mm/s"); stat("plan velocity", D.pvel, "mm/s");
  const dev = [], dtp = [];
  for (let i=0;i<D.t.length;i++){
    if (D.asked[i]!==null && D.achieved[i]!==null) dev.push(Math.abs(D.asked[i]-D.achieved[i]));
    if (D.asked[i]!==null && D.planned[i]!==null) dtp.push(Math.abs(D.asked[i]-D.planned[i]));
  }
  stat("|asked − achieved|", dev, "mm"); stat("|asked − planned|", dtp, "mm");
  document.getElementById("stats").innerHTML =
    "<tr><th>series</th><th>min</th><th>max</th><th>mean</th><th>n</th></tr>"
    + rows.map(r => "<tr><td>"+r[0]+"</td><td>"+r[1]+"</td><td>"+r[2]+"</td><td>"+r[3]
      + "</td><td>"+r[4]+"</td></tr>").join("");
}

// ---- boot -------------------------------------------------------------------
document.getElementById("sub").textContent =
  META[0][1] + "  ·  " + META[1][1] + "  ·  window " + (META[4] ? META[4][1] : "?");
for (const k of ["asked","planned","achieved","env","anom"]){
  const b = document.getElementById("b-"+k);
  b.addEventListener("click", () => {
    show[k] = !show[k]; b.setAttribute("aria-pressed", show[k] ? "true" : "false"); draw();
  });
}
document.getElementById("b-theme").addEventListener("click", () => {
  theme = theme === "dark" ? "light" : "dark"; applyPalette(); draw();
});
document.getElementById("b-pal").addEventListener("click", () => {
  palname = palname === "product" ? "cvd" : "product"; applyPalette(); draw();
});
document.getElementById("b-reset").addEventListener("click", () => { view=[T0,T1]; draw(); });
applyPalette();
draw();
tables();
wire(document.getElementById("p1"), "ro1");
wire(document.getElementById("p2"), "ro2");
</script>
"""


if __name__ == "__main__":
    sys.exit(main())
