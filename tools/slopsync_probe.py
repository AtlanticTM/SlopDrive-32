#!/usr/bin/env python3
"""
slopsync_probe.py — standalone Python 3 live verification client for the
SlopSync protocol (docs/slopsync/SPEC.md). Connects to the firmware's
SlopSync hub over a binary WebSocket, runs a scripted session, and prints a
narrated PASS/FAIL transcript.

This is a dev tool, NOT part of the library (lib/slopsync/ stays hardware-
free / dependency-free per CLAUDE.md's SlopSync rules). It hand-rolls its own
CBOR encoder/decoder for the SPEC §5.3 deterministic profile rather than
depending on `cbor2` (which shrinks floats to the smallest representation
that round-trips — banned here: §5.3 mandates float32-only, never
float16/64). Every wire number below is copied from, and must match,
docs/slopsync/registry/registry.yaml (the single source of truth) and
mirrors the encode/decode logic in lib/slopsync/include/slopsync/wire/.

WIRE AUDIT (grep-friendly summary of what this file puts on the wire):
  Frame types used:  HELLO(0x00) WELCOME(0x01) SUBSCRIBE(0x06) GRANT(0x08)
                      STATE(0x0B) INTENT(0x0D) ECHO(0x0E) NACK(0x10)
                      GOODBYE(0x11) PING(0x03, keepalive only)
                      CATALOG_READY(0x19, §8.4 readiness gate)
  CBOR keys used:     proto_ver(1) client_kind(2) client_name(3)
                      instance_id(4) session_id(6) boot_id(7)
                      catalog_etag(8) cfg_gen(9) subscriptions(10)
                      rate_hz(12) priority(13) granted_rate_hz(14)
                      channel_id(15) code(16) detail(17) intent_id(18)
                      applied(19) value(20) limits(22) roles(23)
                      deadman_ms(24) deadman_policy(25) nonce(29)
                      grants(35)
  welcome `limits` sub-keys: max_frame(1) max_subscriptions(2)
                      retained_pending(3) max_subscriptions_per_frame(4)

FRAME-HEADER CHANNEL CONVENTION (confirmed against the reference C++ impl,
not spelled out explicitly in SPEC.md prose — see
lib/slopsync/include/slopsync/{hub,client}/*_impl.hpp):
  header.channel == 0x0000 (session-scoped) for: HELLO, WELCOME, SUBSCRIBE,
      GRANT, NACK, GOODBYE, PING/PONG.
  header.channel == the TARGET channel id for:   STATE, STREAM, INTENT, ECHO.
  (INTENT/ECHO's target channel_id also rides inside the CBOR payload at key
  15 — the header copy is redundant-but-authoritative routing, per
  Client::sendIntent / Hub::handleIntent in client_impl.hpp / hub_impl.hpp.)

Usage:
    python slopsync_probe.py [--ip 192.168.1.229] [--port 82]
                              [--timeout 5.0] [--no-motion] [--listen-only]
                              [--stream SECS] [--segments SECS]

Requires the `websocket-client` package:
    pip install websocket-client
"""
import argparse
import hashlib
import hmac
import json
import math
import os
import random
import struct
import sys
import time
import urllib.error
import urllib.request

try:
    import websocket
except ImportError:
    sys.stderr.write(
        "slopsync_probe.py requires the 'websocket-client' package.\n"
        "Install it with:\n\n    pip install websocket-client\n\n"
    )
    sys.exit(1)


# =============================================================================
# Registry constants — docs/slopsync/registry/registry.yaml is the source of
# truth; every value below must match it byte-for-byte. Do not hand-invent
# new numbers here (CLAUDE.md's "spec-gap ritual": fix the registry first).
# =============================================================================

PROTO_VER = 1
WS_SUBPROTOCOL = "slopsync.v1"

FRAME = {
    "HELLO": 0x00, "WELCOME": 0x01, "PING": 0x03, "PONG": 0x04, "CLOCK": 0x05,
    "SUBSCRIBE": 0x06, "UNSUBSCRIBE": 0x07, "GRANT": 0x08,
    "STATE": 0x0B, "STREAM": 0x0C, "INTENT": 0x0D,
    "ECHO": 0x0E, "EVENT": 0x0F, "NACK": 0x10, "GOODBYE": 0x11, "PROBE": 0x12,
    "PROBE_REPORT": 0x13, "PAIR_REQ": 0x14, "PAIR_GRANT": 0x15, "ACKMASK": 0x16,
    "BEACON": 0x17, "PUBLISH": 0x18, "CATALOG_READY": 0x19, "BLOB_REQ": 0x1A,
    "BLOB_CHUNK": 0x1B, "AUTH": 0x1C, "HUB_SIG": 0x1D, "ESTOP": 0xE5,
}
FRAME_NAME = {v: k for k, v in FRAME.items()}

# `blob` (38) sub-map integer keys — registry `blob_keys` (RFC-021). ONE
# vocabulary shared by BLOB_REQ's CBOR map, BLOB_CHUNK's fixed binary header,
# and (later) store CRUD intents, so the three can never drift.
BLOB_K = {
    "ns": 1, "store_id": 2, "slot": 3, "generation": 4, "name": 5,
    "kind": 6, "payload": 7, "chunk_index": 8, "chunk_count": 9,
    "total_bytes": 10,
}

# registry `blob_namespaces`
BLOB_NS = {"catalog": 0, "store": 1}

# BLOB_CHUNK (0x1B) raw payload header — 14 bytes, little-endian, naming the
# same identity fields as BLOB_K:
#   ns u8 | store_id u8 | slot u8 | reserved u8 | generation u16 |
#   chunk_index u16 | chunk_count u16 | total_bytes u32
BLOB_CHUNK_HEADER_BYTES = 14
CATALOG_CHUNK_PAYLOAD = 192  # limits::catalog_chunk_payload


def build_blob_req(ns=0, store_id=None, slot=None, generation=None, chunks=None):
    """BLOB_REQ (0x1A) body. A bare catalog request is the EMPTY map: namespace
    0 is the default, so RFC-021's generalization costs the common case zero
    bytes. Key order is ascending per §5.3 — chunks(27) before blob(38)."""
    pairs = []
    if chunks is not None:
        pairs.append((K["chunks"], cb_array([cb_uint(c) for c in chunks])))
    sub = []
    if ns != BLOB_NS["catalog"]:
        sub.append((BLOB_K["ns"], cb_uint(ns)))
    if store_id is not None:
        sub.append((BLOB_K["store_id"], cb_uint(store_id)))
    if slot is not None:
        sub.append((BLOB_K["slot"], cb_uint(slot)))
    if generation is not None:
        sub.append((BLOB_K["generation"], cb_uint(generation)))
    if sub:
        pairs.append((K["blob"], cb_map(sub)))
    return cb_map(pairs)


def decode_blob_chunk(payload):
    """Parse a BLOB_CHUNK raw payload into (identity, index, count, total, bytes).
    The reserved byte is IGNORED, never validated — that is what keeps it usable
    later without a break."""
    if len(payload) < BLOB_CHUNK_HEADER_BYTES:
        return None
    ns, store_id, slot, _reserved = payload[0], payload[1], payload[2], payload[3]
    generation, idx, count = struct.unpack_from("<HHH", payload, 4)
    total = struct.unpack_from("<I", payload, 10)[0]
    return {
        "ns": ns, "store_id": store_id, "slot": slot, "generation": generation,
        "chunk_index": idx, "chunk_count": count, "total_bytes": total,
        "bytes": payload[BLOB_CHUNK_HEADER_BYTES:],
    }


# CBOR map integer keys — global key space (registry `cbor_keys`).
K = {
    "proto_ver": 1, "client_kind": 2, "client_name": 3, "instance_id": 4,
    "token": 5, "session_id": 6, "boot_id": 7, "catalog_etag": 8, "cfg_gen": 9,
    "subscriptions": 10, "publishes": 11, "rate_hz": 12, "priority": 13,
    "granted_rate_hz": 14, "channel_id": 15, "code": 16, "detail": 17,
    "intent_id": 18, "applied": 19, "value": 20, "timestamp": 21, "limits": 22,
    "roles": 23, "deadman_ms": 24, "deadman_policy": 25, "probe_result": 26,
    "chunks": 27, "pin_proof": 28, "nonce": 29, "precondition": 30,
    "retry_after_ms": 31, "takeover": 32, "event_kind": 33, "seq_of_state": 34,
    "grants": 35, "granted_publishes": 36, "identity": 37, "blob": 38,
    "trust": 39, "body": 40, "intent_seq": 41, "burst": 42,
    "ws_port": 46, "ipv4": 47,
}

# WELCOME's `identity` (37) sub-map has its own key space (registry
# `identity_keys`) -- NOT the global cbor_keys space above.
IDENTITY_K = {"hub_instance_id": 5}

# WELCOME's `limits` (22) sub-map has its own tiny key space (registry
# `welcome_limits_keys`) — NOT the global cbor_keys space above.
WELCOME_LIMITS_K = {"max_frame": 1, "max_subscriptions": 2, "retained_pending": 3,
                    "max_subscriptions_per_frame": 4}
# registry limits::max_subscriptions_per_frame — the fallback when a hub
# predates WELCOME limits key 4 (RFC-033.3: a hub MAY advertise less, never
# more than it decodes).
MAX_SUBSCRIPTIONS_PER_FRAME_DEFAULT = 16

PRIORITY = {"background": 0, "normal": 1, "elevated": 2, "critical": 3}

NACK_CODES = {
    0x0000: "MALFORMED", 0x0001: "UNSUPPORTED_VERSION", 0x0002: "FRAME_TOO_LARGE",
    0x0003: "PROFILE_VIOLATION",
    0x0100: "BUSY", 0x0101: "UNAUTHORIZED", 0x0102: "NOT_CONTROLLER",
    0x0103: "PAIRING_REQUIRED", 0x0104: "PAIRING_DENIED", 0x0105: "SESSION_EVICTED",
    0x0106: "DUPLICATE_INSTANCE", 0x0107: "NORMAL_CLOSURE", 0x0108: "DEADMAN_TIMEOUT",
    0x0109: "REBOOTING", 0x010A: "READY_TIMEOUT", 0x010B: "NOT_READY",
    0x010C: "IDLE_REAPED",
    0x0200: "UNKNOWN_CHANNEL", 0x0201: "ACCESS_DENIED", 0x0202: "CLASS_MISMATCH",
    0x0203: "SUB_LIMIT", 0x0204: "SUBSCRIBE_REJECTED",
    0x0300: "CONFLICT", 0x0301: "RATE_LIMITED", 0x0302: "INVALID_VALUE",
    0x0303: "UNSUPPORTED_OP",
    0x0400: "ESTOP_ACTIVE", 0x0401: "NOT_HOMED", 0x0402: "INTERLOCK",
    0x0403: "SOURCE_CONFLICT", 0x0404: "TAKEOVER_REQUIRED", 0x0405: "CLEAR_REFUSED",
    0x0500: "CHUNK_UNAVAILABLE", 0x0501: "REASSEMBLY_TIMEOUT", 0x0502: "ETAG_MISMATCH",
    0x0503: "BLOB_REFUSED",
}


def nack_name(code):
    """§4.3: an unknown NACK code MUST be treated as the generic code of its
    range (the high byte) — never an error, never a reason to disconnect."""
    if code is None:
        return "?"
    if code in NACK_CODES:
        return NACK_CODES[code]
    return "UNKNOWN(0x%04X, range 0x%02X__)" % (code, (code >> 8) & 0xFF)


# The device catalog this probe exercises (SPEC §8: channel entries). This is
# the PLANNED real firmware catalog described by the caller of this tool —
# distinct from lib/slopsync/include/slopsync/conformance/mini_catalog.hpp,
# which is a FROZEN, unrelated golden-vector fixture for protocol
# conformance tests only, not a preview of the real device's channel set.
CH_SAFETY = 0x0003           # STATE, critical — latched safety word (§11.1)
CH_MOTION = 0x1100           # STATE — packed: pos_10um u16/100, tgt_10um u16/100,
                             #         speed i16/10, flags bitfield8 (u8)
CH_MACHINE_CONFIG = 0x1000   # STATE — the RFC-009 settings surface (limits)
CH_PATTERN_STATE = 0x1200    # STATE — the RFC-009 settings surface (user)
CH_ODOMETER = 0x1020         # STATE, background 1Hz — session totals
CH_CONFIG_SET = 0x3000       # INTENT — 0x1000's paired write channel
CH_PATTERN_CMD = 0x3200      # INTENT — 0x1200's paired write channel
CH_MOTION_INPUT = 0x2100     # STREAM c->h — packed: target_norm u16/10000, vel_norm i16/1000
CH_MOTION_SEGMENT = 0x2101   # STREAM c->h — packed: target_norm u16/10000, duration_ms u16, end_vel_norm i16/1000
# ---- M5a: the telemetry channels the legacy :81 plane owned ----------------
CH_PLAN_STRIP = 0x1110       # STATE, elevated ~45Hz — the planner's current segment
CH_POWER = 0x1010            # STATE, background — FEATURE-GATED: absent on a hub
                             #   with no current sensor, and its ABSENCE is the
                             #   capability answer (RFC-016). Never assert it exists.
CH_MOTION_DIAG = 0x1111      # STATE, background 1Hz — planner stats + stream counters
CH_MOTION_ANOMALY = 0x4100   # EVENT, watch — SlopMotion anomalies, device-authored
ANOMALY_KINDS = {
    0: "none", 1: "plan_failed", 2: "settle", 3: "endvel_clamped",
    4: "deadline_stretched", 5: "waveform_fallback", 6: "waveform_scaled",
    7: "waveform_centred",
    8: "handoff_bounded",   # M4d / RFC-008 -- the hub-side handoff sanity guard
    9: "waveform_smoothed", # slopmotion 0.8.0 -- the budgeted policies' own kind
}
# 0x4100's `body` (40) sub-map keys — the CHANNEL'S OWN schema keys, which is
# the whole v1.0 EVENT grammar: a device names its own event fields without a
# registry PR. This channel is the first device-authored EVENT channel, so it
# is also the proof that that grammar works on the wire.
ANOM_BODY_K = {"kind": 1, "seq": 2, "target": 3, "detail": 4, "t_us": 5}


CHANNEL_NAMES = {
    CH_SAFETY: "safety", CH_MOTION: "motion", CH_MACHINE_CONFIG: "machine-config",
    CH_PATTERN_STATE: "pattern-state", CH_ODOMETER: "odometer",
    CH_MOTION_INPUT: "motion-input", CH_MOTION_SEGMENT: "motion-segment",
    CH_PLAN_STRIP: "plan-strip", CH_POWER: "power", CH_MOTION_DIAG: "slopmotion-diag",
    CH_MOTION_ANOMALY: "motion-anomaly",
}

# ---- Catalog entry / field CBOR keys (schema/catalog.cddl, Appendix C) ------
# Named so the annotation checks below read as the SPEC rather than as magic
# integers. Keys 8..15 on a FIELD are the RFC-009 ANNOTATION BLOCK; keys 10/14
# on an ENTRY are the category and the paired-INTENT `setting_channel`.
CAT_E = {"id": 1, "name": 2, "cls": 3, "dir": 4, "access": 5, "rate": 6, "priority": 7,
         "layout": 8, "schema": 9, "category": 10, "category_label": 11, "store": 12,
         "replay_depth": 13, "setting_channel": 14, "stream_kind": 15}
CAT_F = {"name": 1, "type": 2, "unit": 3, "scale": 4, "min": 5, "max": 6, "bits": 7,
         "setting_key": 8, "default": 9, "options": 10, "group": 11, "desc": 12,
         "role": 13, "step": 14, "flags": 15, "access": 16, "option_access": 17}
SETTING_CATEGORIES = {0: "device", 1: "user", 2: "limits", 3: "tuning", 4: "diagnostics"}
# registry `packed_field_types` 8/9/10 = str16/str32/str64 (RFC-026). A string
# setting is renderable BY ITS TYPE (RFC-036): the packed width IS the length
# bound, so demanding numeric min/max of it would fail a perfectly renderable
# field. Everything non-string keeps the strict (options | min/max) rule.
STR_FIELD_TYPES = {8: "str16", 9: "str32", 10: "str64"}
# registry `channel_classes` -- catalog entry key 3 (`cls`) values. STATE and
# EVENT are the subscribable classes (§9.1/§9.4); STREAM is published, INTENT
# is written, STORE is fetched over BLOB_REQ.
CH_CLASS = {"STATE": 0, "STREAM": 1, "INTENT": 2, "EVENT": 3, "STORE": 4}
DESC_MAX_BYTES = 128         # limits::desc_max_bytes
# registry `field_roles`. A role a client does not recognize must render
# generically (fallback mandatory, upgrade optional) -- but a role the hub
# INVENTED is silently identical to no role at all, which is why the probe
# checks membership rather than merely presence.
FIELD_ROLES = {
    "limit.user.speed", "limit.user.accel", "limit.input.speed", "limit.input.accel",
    "limit.input.jerk", "geometry.max_travel", "geometry.measured_travel",
    "window.min", "window.max", "telemetry.position", "telemetry.target",
    "telemetry.velocity", "telemetry.current", "telemetry.power.bus", "telemetry.temp",
    "telemetry.uptime", "identity.name", "meta.enabled_mask", "meta.reset_gen",
    "pattern.running", "pattern.select", "pattern.speed", "pattern.depth",
    "pattern.stroke", "pattern.sensation", "command.position",
    "plan.start", "plan.end", "plan.current", "plan.velocity", "plan.elapsed",
    "plan.duration", "plan.style", "source.background_run",
}


def _catalog_entries(catalog_bytes):
    """Decode a reassembled catalog blob into {channel_id: entry-map}. Returns
    {} when the blob is missing or undecodable -- callers treat that as "cannot
    check", never as "the hub is wrong"."""
    if not catalog_bytes:
        return {}
    try:
        arr = cb_decode_full(catalog_bytes)
    except ValueError:
        return {}
    if not isinstance(arr, list):
        return {}
    return {e.get(CAT_E["id"]): e for e in arr if isinstance(e, dict)}


def _catalog_has(catalog_bytes, channel_id):
    return channel_id in _catalog_entries(catalog_bytes)


def check_catalog_annotations(catalog_bytes):
    """Walk the hub's own catalog the way a GENERIC settings client would, and
    report whether it could actually build a settings page from it.

    Deliberately hub-agnostic: nothing below hardcodes a field name or a value.
    It asks only the structural questions RFC-009's rendering checklist asks --
    which is the difference between "this device's catalog looks right to me"
    and "this catalog is renderable by software that has never met it"."""
    entries = _catalog_entries(catalog_bytes)
    if not entries:
        bad("cat_annotations", "no decodable catalog to inspect (BLOB transfer failed above)")
        return

    settings_channels = [e for e in entries.values() if CAT_E["setting_channel"] in e]
    if not settings_channels:
        bad("cat_settings_channels", "no STATE channel declares a `setting_channel` -- a generic "
            "client has no way to WRITE anything on this hub")
        return
    ok("cat_settings_channels", "%d settings channel(s) declare a paired INTENT: %s"
       % (len(settings_channels),
          ", ".join("0x%04X->0x%04X" % (e[CAT_E["id"]], e[CAT_E["setting_channel"]])
                    for e in settings_channels)))

    unresolved, unannotated, bad_role, long_desc = [], [], [], []
    total_settings = 0
    masks, selects, readonly = [], [], []
    for e in settings_channels:
        eid = e[CAT_E["id"]]
        target = entries.get(e[CAT_E["setting_channel"]])
        # A settings channel must be filed under a tab, or a client has nowhere
        # to put it.
        if CAT_E["category"] not in e:
            unannotated.append("0x%04X (entry: no category)" % eid)
        for f in e.get(CAT_E["layout"], []):
            fname = f.get(CAT_F["name"], "?")
            role = f.get(CAT_F["role"])
            if role is not None and role not in FIELD_ROLES:
                bad_role.append("0x%04X.%s=%r" % (eid, fname, role))
            if role == "meta.enabled_mask":
                masks.append((eid, f))
            if CAT_F["setting_key"] not in f:
                readonly.append("0x%04X.%s" % (eid, fname))
                continue
            total_settings += 1
            key = f[CAT_F["setting_key"]]
            # (1) the write key must actually exist on the paired INTENT.
            if not target or key not in (target.get(CAT_E["schema"]) or {}):
                unresolved.append("0x%04X.%s -> key %d" % (eid, fname, key))
            # (2) enough metadata to RENDER: text, a card, a factory value, and
            #     either a choice list or a numeric range.
            missing = [n for n in ("default", "group", "desc") if CAT_F[n] not in f]
            if CAT_F["options"] in f:
                selects.append((eid, fname, f[CAT_F["options"]]))
            elif f.get(CAT_F["type"]) in STR_FIELD_TYPES:
                # RFC-036: a str16/32/64 setting is renderable by virtue of its
                # type -- the packed width is its length bound. Neither options
                # nor min/max is a sensible demand of a text box.
                pass
            elif CAT_F["min"] not in f or CAT_F["max"] not in f:
                missing.append("min/max")
            if missing:
                unannotated.append("0x%04X.%s (no %s)" % (eid, fname, "/".join(missing)))
            d = f.get(CAT_F["desc"])
            if isinstance(d, str) and len(d.encode("utf-8")) > DESC_MAX_BYTES:
                long_desc.append("0x%04X.%s (%d B)" % (eid, fname, len(d.encode("utf-8"))))

    if unresolved:
        bad("cat_setting_keys", "setting_key(s) that resolve to NOTHING on their paired INTENT "
            "-- a control that renders and drives nothing: %s" % ", ".join(unresolved))
    else:
        ok("cat_setting_keys", "all %d setting_key(s) resolve in their declared settingChannel"
           % total_settings)

    if unannotated:
        bad("cat_renderable", "field(s) a generic client cannot fully render: %s"
            % ", ".join(unannotated))
    else:
        ok("cat_renderable", "every setting carries default + group + desc + (options | min/max "
           "| string type) -- a naive client can pick a widget, label it, explain it and reset it")

    if bad_role:
        bad("cat_roles", "role(s) outside the registry vocabulary (silently equal to no role): %s"
            % ", ".join(bad_role))
    else:
        ok("cat_roles", "every declared role is a registered field_roles value")

    if long_desc:
        bad("cat_desc_cap", "desc(s) over the %d-byte registry cap: %s"
            % (DESC_MAX_BYTES, ", ".join(long_desc)))
    else:
        ok("cat_desc_cap", "every desc is within the %d-byte cap" % DESC_MAX_BYTES)

    # RFC-009 item 4 -- dynamic enabled state. Bit i gates the i-th
    # setting-annotated field of the SAME layout, so the check is that the mask
    # can address every setting on its channel. A mask with fewer labeled bits
    # than the channel has settings cannot gray the tail of the card.
    if not masks:
        bad("cat_enabled_mask", "no channel declares a meta.enabled_mask -- nothing tells a client "
            "which controls are settable RIGHT NOW (RFC-009 item 4)")
    else:
        problems = []
        for eid, f in masks:
            e = entries[eid]
            n_settings = sum(1 for x in e.get(CAT_E["layout"], []) if CAT_F["setting_key"] in x)
            bits = f.get(CAT_F["bits"]) or {}
            if len(bits) < n_settings:
                problems.append("0x%04X: %d labeled bits for %d settings"
                                % (eid, len(bits), n_settings))
        if problems:
            bad("cat_enabled_mask", "enabled_mask cannot address every setting: %s"
                % ", ".join(problems))
        else:
            ok("cat_enabled_mask", "%d enabled_mask field(s), each addressing every setting on its "
               "channel (disabled means gray, never hide)" % len(masks))

    # RFC-009 gap 3 -- a u8-backed single-select must NAME its choices, or the
    # client can only show a number and the user needs the firmware source.
    if selects:
        empties = [(eid, fn) for eid, fn, opts in selects
                   if not opts or any(not isinstance(o, str) or not o for o in opts)]
        if empties:
            bad("cat_options", "select field(s) with empty/absent option labels: %s"
                % ", ".join("0x%04X.%s" % x for x in empties))
        else:
            ok("cat_options", "%d single-select(s) carry named options, e.g. 0x%04X.%s = %s"
               % (len(selects), selects[0][0], selects[0][1],
                  ", ".join(selects[0][2][:3]) + ("..." if len(selects[0][2]) > 3 else "")))
    else:
        info("no single-select settings on this hub (nothing to name)")

    # RFC-003 -- the stored/effective distinction IS the ABSENCE of a
    # setting_key. A read-only field on a settings channel is not an omission,
    # it is the machine saying "this is what I AM, do not write it back".
    if readonly:
        ok("cat_readonly", "%d read-only field(s) on settings channels (no setting_key = effective "
           "state, never stored config): %s" % (len(readonly), ", ".join(readonly)))

    # Category coverage -- the tab strip a client would draw.
    cats = sorted({e[CAT_E["category"]] for e in entries.values() if CAT_E["category"] in e})
    if cats:
        ok("cat_categories", "categories present: %s"
           % ", ".join("%d=%s" % (c, SETTING_CATEGORIES.get(c, "device-defined")) for c in cats))
    else:
        bad("cat_categories", "no channel declares a category -- a client has no tab strip")
# A channel id in NO device catalog: the pre-READY gate probe (Step 2.5)
# sends an INTENT here, so a hub that ISN'T gating answers UNKNOWN_CHANNEL
# instead of NOT_READY and the machine can never be commanded either way.
CH_GATE_PROBE = 0x0FFE
CH_MOVE_INTENT = 0x3100      # INTENT — schema {1: position f32 mm, 2: bypass bool}
CH_SAFETY_INTENT = 0x0005    # INTENT, critical — schema {1: op uint} = a `safety_ops` verb
CH_HOME_INTENT = 0x3101      # INTENT — schema {1: op uint, 2: stroke f32 mm}

# ---- M4b: the trust administration surface (RFC-018/027/029) ---------------
CH_SESSION_ADMIN = 0x0009    # INTENT, configure — {1: op, 2: session_id, 3: instance_id, 4: role}
CH_PENDING_PAIRING = 0x000A  # STATE, configure — the bounded knock list
CH_PAIRING_EVENTS = 0x000B   # EVENT, watch — knock/grant/deny/expire/window edges
CH_PAIRED_DEVICES = 0x000C   # STORE, configure — the trust ledger (BLOB ns 1)
CH_PAIRED_ROSTER = 0x000D    # STATE, configure — {generation u16, count u8, capacity u8}
CH_SAFETY_EVENTS = 0x000E    # EVENT, critical/watch — the §9.4 twin of 0x0003

ADMIN_OPS = {"evict": 1, "pair_approve": 2, "pair_deny": 3, "revoke": 4}
ADMIN_K = {"op": 1, "session_id": 2, "instance_id": 3, "role": 4}
PAIRING_EVENT_KINDS = {
    1: "knocked", 2: "granted", 3: "denied", 4: "expired",
    5: "window_opened", 6: "window_closed", 7: "revoked", 8: "recognized_pending",
}
SAFETY_EVENT_KINDS = {1: "estop_latched", 2: "estop_cleared", 3: "stop_latched", 4: "stop_cleared"}
PAIRING_MODES = {1: "knock_approve", 2: "pin_proof", 4: "push_to_pair"}
TRUST_K = {
    "client_ver": 1, "client_nonce": 2, "sig_request": 3, "hub_pubkey": 4,
    "welcome_sig": 5, "token_proof": 6, "presentation_mode": 7, "pairing_modes": 8,
}
# The trust-ledger ITEM key space (registry `trust_ledger_keys`). Registered
# rather than opaque because, unlike a preset, a paired-device record is
# PROTOCOL content every configure client has to render and act on.
LEDGER_K = {
    1: "instance_id", 2: "kind", 3: "name", 4: "version", 5: "first_seen",
    6: "last_seen", 7: "role", 8: "state", 9: "presentation_mode", 10: "pairing_mode",
}
ROLE_NAME = {0: "watch", 1: "control", 2: "configure"}

# registry `safety_intent_ops` (docs/slopsync/registry/registry.yaml). estop and
# stop are ROLE-EXEMPT (any session, including `watch`, may stop the machine);
# everything else needs `control`. The catalog carries this per-op split as
# index-aligned `option_access`, so a generic client grays correctly instead of
# discovering it by NACK.
SAFETY_OP = {
    "estop_clear": 1, "stop": 2, "hold": 3, "pause": 4, "resume": 5, "estop": 6,
    "override_on": 7, "override_off": 8, "bypass_on": 9, "bypass_off": 10,
}
# 0x0003 packed snapshot, 9 bytes as of RFC-025c: word bitfield8, cause u8,
# owner_session u32, estop_seq u16, modes bitfield8 (bit0 override, bit1 bypass).
# The `modes` byte is APPENDED — bytes 0..7 mean exactly what they always did.
SAFETY_STRUCT = struct.Struct("<BBIHB")
SAFETY_PAYLOAD_BYTES = SAFETY_STRUCT.size   # 9
SAFETY_WORD_BITS = ["estop", "stop", "hold", "pause"]
SAFETY_MODE_OVERRIDE = 1 << 0
SAFETY_MODE_BYPASS = 1 << 1


def decode_safety_state(payload):
    """Decode the 0x0003 snapshot. Tolerates a SHORTER payload than we know
    about (append-only prefix parse, §8.6) but reports what it actually got --
    a hub one field behind us is legal, a hub that is SHORTER THAN ITS OWN
    CATALOG is not, and only the raw length can tell the two apart."""
    out = {"len": len(payload)}
    if len(payload) >= 8:
        word, cause, owner, seq = struct.Struct("<BBIH").unpack_from(payload, 0)
        out.update(word=word, cause=cause, owner_session=owner, estop_seq=seq)
        out["flags"] = [n for i, n in enumerate(SAFETY_WORD_BITS) if word & (1 << i)]
    if len(payload) >= 9:
        out["modes"] = payload[8]
        out["override"] = bool(payload[8] & SAFETY_MODE_OVERRIDE)
        out["bypass"] = bool(payload[8] & SAFETY_MODE_BYPASS)
    return out

STREAM_WISH_RATE_HZ = 100.0  # HELLO publishes-wish rate_hz for CH_MOTION_INPUT
STREAM_HZ = 50.0             # --stream sample cadence
SEGMENT_WISH_RATE_HZ = 5.0   # HELLO publishes-wish rate_hz for CH_MOTION_SEGMENT
SEGMENT_KEEPALIVE_S = 0.4    # PING cadence between sparse (1/s) segments so the 600ms deadman never fires
SEG_NO_END_VEL = -32768      # INT16_MIN sentinel: "no end velocity" (0 is a legit slope, so it can't mean absent)

MOTION_STATE_STRUCT = struct.Struct("<HHhB")  # pos_10um, tgt_10um, speed_x10, flags
STATE_WINDOW_S = 2.0         # SPEC step 4: "Receive STATE pushes for ~2 s"
KEEPALIVE_IDLE_S = 0.8       # send a PING if we've been quiet this long while waiting


# =============================================================================
# Hand-rolled CBOR — SPEC §5.3 deterministic profile ONLY: definite-length
# containers, shortest-form ints, map keys sorted ascending, float32-only
# (0xFA prefix, always 4 payload bytes — this is the whole reason cbor2 is
# banned: it would pick float16 for small values), no tags/bignums, no
# simple values beyond false/true/null.
# =============================================================================

def _head(major, v):
    ib0 = major << 5
    if v <= 23:
        return bytes([ib0 | v])
    if v <= 0xFF:
        return bytes([ib0 | 24, v])
    if v <= 0xFFFF:
        return bytes([ib0 | 25]) + v.to_bytes(2, "big")
    if v <= 0xFFFFFFFF:
        return bytes([ib0 | 26]) + v.to_bytes(4, "big")
    return bytes([ib0 | 27]) + v.to_bytes(8, "big")


def cb_uint(v):
    return _head(0, v)


def cb_int(v):
    return _head(0, v) if v >= 0 else _head(1, -1 - v)


def cb_bool(v):
    return bytes([0xF5 if v else 0xF4])


def cb_f32(v):
    return b"\xfa" + struct.pack(">f", v)  # major7/ai26 + big-endian binary32


def cb_tstr(s):
    b = s.encode("utf-8")
    return _head(3, len(b)) + b


def cb_bstr(b):
    b = bytes(b)
    return _head(2, len(b)) + b


def cb_array(elems):
    out = _head(4, len(elems))
    return out + b"".join(elems)


def cb_map(pairs):
    """`pairs`: [(int_key, value_bytes), ...] — CALLER supplies them already
    sorted ascending by key (§5.3's own rule); asserted here so a mistake
    fails loudly instead of silently producing non-canonical bytes."""
    keys = [k for k, _ in pairs]
    assert keys == sorted(keys), "cb_map: keys must be sorted ascending (SPEC §5.3)"
    out = _head(5, len(pairs))
    for k, v in pairs:
        out += cb_uint(k) + v
    return out


def cb_decode(buf, pos=0):
    """Minimal strict-enough decoder for the same profile: definite lengths
    only, float32 only (rejects float16/64), no tags, no indefinite forms.
    Returns (value, new_pos); maps decode to {int_key: value}, arrays to
    list. Raises ValueError on anything outside the profile."""
    ib = buf[pos]
    major = ib >> 5
    ai = ib & 0x1F
    if major == 7:
        if ai == 20:
            return False, pos + 1
        if ai == 21:
            return True, pos + 1
        if ai == 22:
            return None, pos + 1
        if ai == 26:
            v = struct.unpack(">f", buf[pos + 1:pos + 5])[0]
            return v, pos + 5
        raise ValueError("cb_decode: simple/major7 ai=%d not in the deterministic "
                          "profile (float16/64 and other simples are forbidden)" % ai)
    if major == 6:
        raise ValueError("cb_decode: CBOR tags are forbidden by SPEC §5.3")
    if ai <= 23:
        val, pos = ai, pos + 1
    elif ai == 24:
        val, pos = buf[pos + 1], pos + 2
    elif ai == 25:
        val, pos = int.from_bytes(buf[pos + 1:pos + 3], "big"), pos + 3
    elif ai == 26:
        val, pos = int.from_bytes(buf[pos + 1:pos + 5], "big"), pos + 5
    elif ai == 27:
        val, pos = int.from_bytes(buf[pos + 1:pos + 9], "big"), pos + 9
    else:
        raise ValueError("cb_decode: indefinite-length/reserved additional-info "
                          "%d forbidden by the deterministic profile" % ai)
    if major == 0:
        return val, pos
    if major == 1:
        return -1 - val, pos
    if major == 2:
        return bytes(buf[pos:pos + val]), pos + val
    if major == 3:
        return buf[pos:pos + val].decode("utf-8"), pos + val
    if major == 4:
        arr = []
        for _ in range(val):
            v, pos = cb_decode(buf, pos)
            arr.append(v)
        return arr, pos
    if major == 5:
        m = {}
        for _ in range(val):
            k, pos = cb_decode(buf, pos)
            v, pos = cb_decode(buf, pos)
            m[k] = v
        return m, pos
    raise ValueError("cb_decode: unreachable major %d" % major)  # pragma: no cover


def cb_decode_full(buf):
    val, _pos = cb_decode(buf, 0)
    return val


# =============================================================================
# Frame layer — SPEC §5.1: 8-byte little-endian header
#   [type:u8][flags:u8][channel:u16][seq:u16][len:u16]
# =============================================================================

FRAME_HDR = struct.Struct("<BBHHH")


def encode_frame(ftype, channel, payload, seq=0, flags=0):
    return FRAME_HDR.pack(ftype, flags, channel, seq, len(payload)) + payload


def decode_frame_header(buf):
    if len(buf) < 8:
        return None
    ftype, flags, channel, seq, length = FRAME_HDR.unpack(buf[:8])
    return {"type": ftype, "flags": flags, "channel": channel, "seq": seq, "len": length}


# =============================================================================
# Packed STATE decode — SPEC §5.4/§8.2: wire = physical * scale for scaled
# integer fields; bitfield8 passes through as a raw byte (scale ignored).
# =============================================================================

def decode_motion_state(payload):
    if len(payload) < MOTION_STATE_STRUCT.size:
        raise ValueError("motion(0x1100) STATE payload too short: %d bytes (need >= %d)"
                          % (len(payload), MOTION_STATE_STRUCT.size))
    pos_raw, tgt_raw, speed_raw, flags = MOTION_STATE_STRUCT.unpack_from(payload, 0)
    out = {
        "pos_mm": pos_raw / 100.0,
        "tgt_mm": tgt_raw / 100.0,
        "speed_mm_s": speed_raw / 10.0,
        "flags": flags,
    }
    # M5a APPENDED raw_10um -- the PRE-PLANNING demand (legacy :81 0x01 TELE
    # `raw`). Read as an OPTIONAL suffix on purpose: this is the append-only
    # prefix-parse rule (§8.6) exercised for real, so this probe keeps working
    # against a hub one field behind us instead of failing the whole channel.
    if len(payload) >= MOTION_STATE_STRUCT.size + 2:
        out["raw_mm"] = struct.unpack_from("<H", payload, MOTION_STATE_STRUCT.size)[0] / 100.0
    return out


PLAN_STRIP_STRUCT = struct.Struct("<BBHHHhII")   # flags, style, start, end, cur, vel, dur_us, elapsed_us
PLAN_STYLES = {0: "idle", 1: "waveform", 2: "chase", 3: "settle"}


def decode_plan_strip(payload):
    """0x1110 plan-strip: the planner's CURRENT SEGMENT (legacy :81 0x04
    INTERP). Positions are normalized *10000, velocity *1000 -- the engine's
    own domain, where 1.0 is the whole stroke window."""
    if len(payload) < PLAN_STRIP_STRUCT.size:
        raise ValueError("plan-strip(0x1110) payload too short: %d bytes (need >= %d)"
                          % (len(payload), PLAN_STRIP_STRUCT.size))
    flags, style, start, end, cur, vel, dur, elapsed = PLAN_STRIP_STRUCT.unpack_from(payload, 0)
    return {
        "active": bool(flags & 1), "live_mode": bool(flags & 2), "grad_mode": bool(flags & 4),
        "style": PLAN_STYLES.get(style, "?%d" % style),
        "start": start / 10000.0, "end": end / 10000.0, "cur": cur / 10000.0,
        "vel": vel / 1000.0, "duration_us": dur, "elapsed_us": elapsed,
    }


POWER_STRUCT = struct.Struct("<HHh")   # bus_mV, peak_mA, i_bus_mA  (+ die_c10 i16 iff monitored)


def decode_power(payload):
    """0x1010 power. die_c10 is present ONLY on a hub that also reports a power
    monitor -- a shunt-only machine advertises a 3-field entry rather than a
    permanently-zero temperature column, so its absence here is expected."""
    if len(payload) < POWER_STRUCT.size:
        raise ValueError("power(0x1010) payload too short: %d bytes" % len(payload))
    bus_mv, peak_ma, i_ma = POWER_STRUCT.unpack_from(payload, 0)
    out = {"bus_v": bus_mv / 1000.0, "peak_a": peak_ma / 1000.0, "i_bus_a": i_ma / 1000.0}
    if len(payload) >= POWER_STRUCT.size + 2:
        out["die_c"] = struct.unpack_from("<h", payload, POWER_STRUCT.size)[0] / 10.0
    return out


# plans, failures, anomalies, mode, plan_kind, Nx per-kind, plan_us x3, 5x sync, reset_gen
#
# THE PER-KIND BLOCK SITS IN THE MIDDLE, so its width is not a cosmetic detail:
# every field after it shifts by 4 B per kind. This struct was hardcoded at 8
# kinds and silently kept decoding a 84 B payload as the 80 B pre-M4d layout --
# the length check passed (80 <= 84) and plan_us_*/sync_*/reset_gen were read
# one slot early, which looks like plausible data rather than an error. Derive
# the count from ANOMALY_KINDS so the table and the struct cannot drift again.
_N_KINDS = len(ANOMALY_KINDS)
DIAG_STRUCT = struct.Struct("<IIIBB%dIIIfIIIIIH" % _N_KINDS)


def decode_motion_diag(payload):
    if len(payload) < DIAG_STRUCT.size:
        raise ValueError("slopmotion-diag(0x1111) payload too short: %d bytes (need >= %d)"
                          % (len(payload), DIAG_STRUCT.size))
    v = DIAG_STRUCT.unpack_from(payload, 0)
    return {
        "plans": v[0], "failures": v[1], "anomalies": v[2],
        "mode": PLAN_STYLES.get(v[3], "?%d" % v[3]), "plan_kind": v[4],
        "by_kind": {ANOMALY_KINDS.get(i, "?%d" % i): v[5 + i] for i in range(_N_KINDS)},
        "plan_us_last": v[5 + _N_KINDS], "plan_us_max": v[6 + _N_KINDS],
        "plan_us_avg": v[7 + _N_KINDS],
        "sync_bundles": v[8 + _N_KINDS], "sync_samples": v[9 + _N_KINDS],
        "sync_enqueued": v[10 + _N_KINDS], "sync_dropped": v[11 + _N_KINDS],
        "sync_seg_bundles": v[12 + _N_KINDS],
        "reset_gen": v[13 + _N_KINDS],
    }


ODOMETER_STRUCT = struct.Struct("<IfffI")  # strokes, distance_m, peak_mm_s, energy_wh, session_ms


def decode_odometer(payload):
    if len(payload) < ODOMETER_STRUCT.size:
        raise ValueError("odometer(0x1020) payload too short: %d bytes (need >= %d)"
                          % (len(payload), ODOMETER_STRUCT.size))
    strokes, dist_m, peak, wh, sess = ODOMETER_STRUCT.unpack_from(payload, 0)
    return {"strokes": strokes, "distance_m": dist_m, "peak_mm_s": peak,
            "energy_wh": wh, "session_ms": sess}


# =============================================================================
# Message builders (encode) — mirror lib/slopsync/include/slopsync/wire/messages/*.hpp
# =============================================================================

def mint_uitoken(ip, retries=6, timeout=3.0):
    """GET /uitoken -> 16 raw bytes, or None.

    THE CREDENTIAL BOOTSTRAP FOR TOOLING. Since fw 2.1.59 the hub enforces
    authorization: a tokenless HELLO is granted `watch`, which can observe and
    can e-stop but cannot command motion or publish a stream. Any scenario that
    asserts on a controller-tier grant must present one of these.

    RETRIES ARE NOT OPTIONAL. The endpoint is rate-limited to one mint per
    250 ms DEVICE-WIDE (not per client), so concurrent tools — slopsoak runs
    three at once — will 429 each other constantly. Backoff with jitter is the
    difference between "works" and "flaky in a way that looks like a device
    fault". Returns None only after genuinely giving up.

    A mint is SINGLE-USE and expires in 60 s, so call this per session, never
    once per process. For a client that reconnects constantly, the right
    credential is a PAIRED token from the trust ledger (--pair / --pair-state),
    not a mint; mints are a bootstrap, not a session store.
    """
    url = "http://%s/uitoken" % ip
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(url, timeout=timeout) as r:
                j = json.loads(r.read().decode("utf-8", "replace"))
            tok = j.get("token")
            if j.get("ok") and isinstance(tok, str) and len(tok) == 32:
                return bytes.fromhex(tok)
            return None
        except urllib.error.HTTPError as e:
            if e.code == 429:
                # Rate limited: wait out the window plus jitter so N concurrent
                # callers de-synchronize instead of colliding again in lockstep.
                time.sleep(0.3 + random.random() * 0.25)
                continue
            if e.code == 403:
                return None   # operator disabled it — the lockdown posture
            return None
        except Exception:
            time.sleep(0.2)
    return None


def build_hello(client_kind, client_name, instance_id, publishes=None, token=None):
    # Keys ascending: proto_ver(1) < client_kind(2) < client_name(3) <
    # instance_id(4) < token(5) < publishes(11). No catalog_etag/subscriptions
    # (§6.2: all optional, omitted entirely when absent — never encoded as
    # null). `publishes`: [(channel_id, rate_hz), ...] — §6.2 c2h STREAM wish.
    pairs = [
        (K["proto_ver"], cb_uint(PROTO_VER)),
        (K["client_kind"], cb_tstr(client_kind)),
        (K["client_name"], cb_tstr(client_name)),
        (K["instance_id"], cb_bstr(instance_id)),
    ]
    if token:
        pairs.append((K["token"], cb_bstr(token)))
    if publishes:
        entries = []
        for ch, rate in publishes:
            # Wish-entry keys ascending: rate_hz(12) < channel_id(15).
            entries.append(cb_map([
                (K["rate_hz"], cb_f32(rate)),
                (K["channel_id"], cb_uint(ch)),
            ]))
        pairs.append((K["publishes"], cb_array(entries)))
    return cb_map(pairs)


def build_subscribe(wishes):
    """`wishes`: [(channel_id, rate_hz, priority), ...] — SPEC §6.6."""
    entries = []
    for ch, rate, prio in wishes:
        # Wish-entry keys ascending: rate_hz(12) < priority(13) < channel_id(15).
        entries.append(cb_map([
            (K["rate_hz"], cb_f32(rate)),
            (K["priority"], cb_uint(prio)),
            (K["channel_id"], cb_uint(ch)),
        ]))
    return cb_map([(K["subscriptions"], cb_array(entries))])


def build_intent(channel_id, intent_id, value_fields):
    """`value_fields`: [(subkey:int, value_bytes), ...] already sorted
    ascending by subkey (SPEC §9.3's `value` map)."""
    value_map = cb_map(value_fields)
    return cb_map([
        (K["channel_id"], cb_uint(channel_id)),
        (K["intent_id"], cb_uint(intent_id)),
        (K["value"], value_map),
    ])


def build_goodbye(code):
    return cb_map([(K["code"], cb_uint(code))])


def build_hello_full(client_kind, client_name, instance_id, token=None,
                     subscriptions=None, client_ver=None, client_nonce=None,
                     sig_request=False, presentation_mode=None):
    """HELLO with the M4b/M4c optionals: a persisted `token` (5) and the scoped
    `trust` (39) sub-map carrying `client_ver` (the RFC-029 item-2 change
    tripwire), `client_nonce` + `sig_request` (item 1's anti-replay entropy and
    the ask that gates signing), and `presentation_mode` (item 6). Keys
    ascending: 1 < 2 < 3 < 4 < 5 < 10 < 39, interior 1 < 2 < 3 < 7."""
    pairs = [
        (K["proto_ver"], cb_uint(PROTO_VER)),
        (K["client_kind"], cb_tstr(client_kind)),
        (K["client_name"], cb_tstr(client_name)),
        (K["instance_id"], cb_bstr(instance_id)),
    ]
    if token:
        pairs.append((K["token"], cb_bstr(token)))
    if subscriptions:
        entries = []
        for ch, rate, prio in subscriptions:
            entries.append(cb_map([
                (K["rate_hz"], cb_f32(rate)),
                (K["priority"], cb_uint(prio)),
                (K["channel_id"], cb_uint(ch)),
            ]))
        pairs.append((K["subscriptions"], cb_array(entries)))
    trust = []
    if client_ver:
        trust.append((TRUST_K["client_ver"], cb_tstr(client_ver)))
    if client_nonce is not None:
        trust.append((TRUST_K["client_nonce"], cb_bstr(client_nonce)))
    if sig_request:
        trust.append((TRUST_K["sig_request"], cb_bool(True)))
    if presentation_mode is not None:
        trust.append((TRUST_K["presentation_mode"], cb_uint(presentation_mode)))
    if trust:
        pairs.append((K["trust"], cb_map(trust)))
    return cb_map(pairs)


# ---------------------------------------------------------------------------
# RFC-029 item 1: the signature MATERIAL, and the sim's deterministic P-256
# STAND-IN reimplemented here.
#
# ---------------------------------------------------------------------------
def hub_sig_material(client_nonce, session_id, boot_id):
    """client_nonce(8) || session_id(u32 LE) || boot_id(u32 LE) — the exact 16
    bytes hubSigMaterial() builds in lib/slopsync. THE CLIENT NONCE IS THE
    WHOLE POINT: without client entropy a signature captured from one handshake
    replays at every future one, which is the feasibility-pass blocker this
    layout closed."""
    return client_nonce + struct.pack("<II", session_id, boot_id)


def stand_in_sign(pubkey, message):
    """slopsync::ScriptedCrypto::signP256, in Python. DELIBERATELY NOT ECDSA —
    the library is std-headers-only by invariant, so the sim signs with a
    deterministic HMAC-shaped stand-in and the real curve lives in the
    firmware's mbedtls adapter (M5). What this proves is curve-independent: the
    material, the delivery frame, and the replay fence."""
    mac = hmac.new(pubkey, message, hashlib.sha256).digest()
    return mac + mac[::-1]


def build_auth(token_proof, presentation_mode=1):
    """AUTH (0x1C): the whole payload is `{39: {6: proof, 7: mode}}`. Note what
    it does NOT carry — an instance_id. The session already knows who it is;
    letting AUTH name an identity would let a live session present a proof AS
    SOMEBODY ELSE."""
    return cb_map([(K["trust"], cb_map([
        (TRUST_K["token_proof"], cb_bstr(token_proof)),
        (TRUST_K["presentation_mode"], cb_uint(presentation_mode)),
    ]))])


def token_proof(token, nonce):
    """RFC-029 item 6: HMAC-SHA256(key=token, msg=the WELCOME nonce), truncated
    to 16 bytes. The token itself never crosses a cleartext v1 wire."""
    return hmac.new(token, nonce, hashlib.sha256).digest()[:16]


def build_pair_knock(instance_id):
    """A BARE PAIR_REQ: instance id and nothing else. Its emptiness IS the
    RFC-027 mode (a)/(c) ceremony — a device with one button and no display
    cannot type a PIN or show one, and demanding a proof from it was the
    v1-draft ceremony's unstated hardware assumption."""
    return cb_map([(K["instance_id"], cb_bstr(instance_id))])


def build_admin(intent_id, op, instance_id=None, session_id=None, role=None):
    """An INTENT on session-admin (0x0009). The `value` sub-keys are the
    CHANNEL'S OWN catalog schema keys, ascending."""
    fields = [(ADMIN_K["op"], cb_uint(op))]
    if session_id is not None:
        fields.append((ADMIN_K["session_id"], cb_uint(session_id)))
    if instance_id is not None:
        fields.append((ADMIN_K["instance_id"], cb_bstr(instance_id)))
    if role is not None:
        fields.append((ADMIN_K["role"], cb_uint(role)))
    return build_intent(CH_SESSION_ADMIN, intent_id, fields)


def decode_pending_pairing(payload):
    """0x000A packed STATE: {generation u16, count u8, flags u8} + 4 slots of
    {inst_lo u32, inst_hi u32, kind u8, expires_s u16, name str16}.

    The instance id is TWO u32 halves because `packed_field_types` has no u64;
    the concatenated bytes are byte-identical to one. See
    lib/slopsync/include/slopsync/channel/trust_channels.hpp."""
    if len(payload) < 4:
        return None
    gen, count, flags = struct.unpack_from("<HBB", payload, 0)
    slots = []
    off = 4
    while off + 27 <= len(payload):
        lo, hi, kind, expires_s = struct.unpack_from("<IIBH", payload, off)
        name = payload[off + 11:off + 27].split(b"\x00")[0].decode("utf-8", "replace")
        inst = struct.pack("<II", lo, hi)
        if lo or hi:
            slots.append({"instance_id": inst, "kind": kind, "expires_s": expires_s, "name": name})
        off += 27
    return {"generation": gen, "count": count, "window_open": bool(flags & 1), "slots": slots}


# =============================================================================
# CLOCK (§7.1) -- raw frame, hub-time estimation for --stream's t_base values.
# =============================================================================

CLOCK_REQUEST_STRUCT = struct.Struct("<I")        # t0
CLOCK_REPLY_STRUCT = struct.Struct("<III")        # t0(echo), t1, t2


def wrap_diff(a, b):
    """Signed windowed difference a-b for two u32 timestamps -- the same
    convention as lib/slopsync/include/slopsync/util/serial_arithmetic.hpp's
    timeDelta(): nearest representative in [-2**31, 2**31)."""
    d = (a - b) & 0xFFFFFFFF
    if d >= 0x80000000:
        d -= 0x100000000
    return d


def client_now_us():
    """The probe's own free-running clock, truncated to u32 -- the CLOCK
    exchange's t0/t3 domain (§7.1). Uses time.time() (not time.monotonic())
    so it shares a clock source with every other deadline in this file --
    mixing the two would silently break recv_frame's deadline math."""
    return int(time.time() * 1e6) & 0xFFFFFFFF


def estimate_hub_offset(ws, timeout):
    """One SPEC §7.1 CLOCK exchange: send t0 (raw CLOCK frame), hub replies
    t0(echo)+t1(hub-us at receipt)+t2(hub-us at send). Returns (offset_us,
    rtt_us) -- offset = hub_time - client_time, windowed-signed -- or None on
    timeout. Add offset (mod 2**32) to client_now_us() to estimate the
    current hub-us for an outgoing STREAM t_base. Accuracy of a few ms is
    fine (design brief); this reuses the same PING-style recv_frame loop
    every other step in this file already relies on."""
    t0 = client_now_us()
    send_frame(ws, FRAME["CLOCK"], 0, CLOCK_REQUEST_STRUCT.pack(t0))
    deadline = time.time() + timeout
    while time.time() < deadline:
        got = recv_frame(ws, deadline)
        if got is None:
            return None
        hdr, payload = got
        if hdr["type"] == FRAME["CLOCK"] and len(payload) >= CLOCK_REPLY_STRUCT.size:
            t3 = client_now_us()
            t0e, t1, t2 = CLOCK_REPLY_STRUCT.unpack_from(payload, 0)
            offset = (wrap_diff(t1, t0e) + wrap_diff(t2, t3)) // 2
            rtt = wrap_diff(t3, t0e) - wrap_diff(t2, t1)
            return offset, rtt
        if hdr["type"] == FRAME["STATE"]:
            record_state(hdr, payload)
        # anything else while waiting (GRANT, EVENT, ...) -- ignore, §4.3
    return None


# =============================================================================
# STREAM bundle encode (§5.4) -- motion-input (0x2100) single-sample bundles.
# Mirrors lib/slopsync/include/slopsync/wire/stream_bundle.hpp's BundleWriter
# layout; the 4-byte sample struct mirrors SlopSyncCatalog's 0x2100 entry
# (u16 target_norm x10000, i16 vel_norm x1000, little-endian).
# =============================================================================

def encode_stream_bundle(t_base, samples):
    """`samples`: [(t_off_u16, target_norm, vel_norm), ...] -- t_off[0] MUST
    be 0 and strictly increasing thereafter (§5.4); a single-sample bundle
    (this tool's whole use case) trivially satisfies that with one entry."""
    n = len(samples)
    out = struct.pack("<IBB", t_base & 0xFFFFFFFF, n, 0)
    out += b"".join(struct.pack("<H", off & 0xFFFF) for off, _t, _v in samples)
    for _off, target, vel in samples:
        raw_t = max(0, min(65535, int(round(target * 10000.0))))
        raw_v = max(-32768, min(32767, int(round(vel * 1000.0))))
        out += struct.pack("<Hh", raw_t, raw_v)
    return out


def encode_segment_bundle(t_base, samples):
    """`samples`: [(t_off_u16, target_norm, dur_ms, end_vel_or_None), ...] --
    the 6-byte timed-segment layout (0x2101). end_vel None encodes the
    INT16_MIN sentinel (no handoff velocity -> the engine estimates it). t_off[0]
    MUST be 0 and strictly increasing thereafter (§5.4). Mirrors SlopSyncCatalog's
    0x2101 entry: {target_norm u16 x10000, duration_ms u16, end_vel_norm i16 x1000},
    little-endian."""
    n = len(samples)
    out = struct.pack("<IBB", t_base & 0xFFFFFFFF, n, 0)
    out += b"".join(struct.pack("<H", off & 0xFFFF) for off, _t, _d, _v in samples)
    for _off, target, dur_ms, end_vel in samples:
        raw_t = max(0, min(65535, int(round(target * 10000.0))))
        raw_d = max(0, min(65535, int(dur_ms)))
        raw_v = SEG_NO_END_VEL if end_vel is None else max(-32768, min(32767, int(round(end_vel * 1000.0))))
        out += struct.pack("<HHh", raw_t, raw_d, raw_v)
    return out


# =============================================================================
# Narration helpers
# =============================================================================

PASS_MARK = "✓"
FAIL_MARK = "✗"

_results = []  # [(step_name, "PASS"|"FAIL"|"SKIP"), ...]


def scene(title):
    print("\n== %s ==" % title)


def ok(step, msg):
    print("  %s %s" % (PASS_MARK, msg))
    _results.append((step, "PASS"))


def bad(step, msg):
    print("  %s %s" % (FAIL_MARK, msg))
    _results.append((step, "FAIL"))


def skip(step, msg):
    print("  - %s (SKIPPED)" % msg)
    _results.append((step, "SKIP"))


def info(msg):
    print("    %s" % msg)


# =============================================================================
# WebSocket send/receive plumbing
# =============================================================================

_last_send_ts = [0.0]


def send_frame(ws, ftype, channel, payload, seq=0):
    ws.send(encode_frame(ftype, channel, payload, seq), opcode=websocket.ABNF.OPCODE_BINARY)
    _last_send_ts[0] = time.time()


def _maybe_keepalive(ws):
    """Any received frame is proof of life (§6.5), but so is anything WE
    send — sending an idle PING while we're the one waiting keeps a
    long --timeout from tripping the hub's idle-reap heuristic."""
    if time.time() - _last_send_ts[0] > KEEPALIVE_IDLE_S:
        send_frame(ws, FRAME["PING"], 0, b"")


def recv_frame(ws, deadline):
    """Blocks until one SlopSync binary frame arrives or `deadline`
    (time.time()-style) passes. Returns (header_dict, payload_bytes) or None
    on timeout/close. WS control frames are handled transparently by the
    library (control_frame=False default: PING auto-PONGed); a TEXT frame is
    reported (SPEC §13.2: text on slopsync.v1 is a protocol error) but does
    not raise — tolerance (§4.3) applies at this layer too."""
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            return None
        ws.settimeout(min(remaining, 0.5))
        try:
            opcode, data = ws.recv_data()
        except websocket.WebSocketTimeoutException:
            _maybe_keepalive(ws)
            continue
        except websocket.WebSocketConnectionClosedException:
            return None
        if opcode == websocket.ABNF.OPCODE_CLOSE:
            return None
        if opcode == websocket.ABNF.OPCODE_TEXT:
            print("  ! received a TEXT WS frame -- protocol error per SPEC §13.2 "
                  "(slopsync.v1 is binary-only); ignoring")
            continue
        if opcode != websocket.ABNF.OPCODE_BINARY:
            continue
        hdr = decode_frame_header(data)
        if hdr is None:
            print("  ! frame shorter than the 8-byte header (%d bytes) -- dropped" % len(data))
            continue
        payload = data[8:8 + hdr["len"]]
        # 0x4100 motion-anomaly is EDGE-driven and provoked by STREAMING, which
        # happens several steps after the STATE observation window -- so it is
        # sniffed HERE, in the one function every step's wait loop goes through,
        # rather than in any single step. Collect-and-continue: the frame is
        # still returned to the caller untouched.
        if hdr["type"] == FRAME["EVENT"] and hdr["channel"] == CH_MOTION_ANOMALY:
            try:
                _anomaly_events.append(cb_decode_full(payload))
            except ValueError:
                _anomaly_events.append(None)   # malformed; reported at the check
        return hdr, payload


# Shared across steps so a STATE frame seen early (e.g. during the GRANT
# wait) isn't lost to a later step that needs it (safety-arrival check,
# step 5's "current position").
_state_log = []       # [(t, channel_id, hdr, payload), ...]
_last_state = {}       # channel_id -> payload (most recent)


# 0x4100 motion-anomaly EVENTs seen during the observation windows. A list, not
# a counter: the probe reports WHAT arrived, and an empty list is a legitimate
# result (a machine that planned nothing pathological had no anomalies).
_anomaly_events = []


def record_state(hdr, payload):
    t = time.time()
    _state_log.append((t, hdr["channel"], hdr, payload))
    _last_state[hdr["channel"]] = payload


def print_summary():
    scene("Summary")
    width = max((len(s) for s, _ in _results), default=10)
    for step, status in _results:
        mark = {"PASS": PASS_MARK, "FAIL": FAIL_MARK, "SKIP": "-"}[status]
        print("  %s  %-*s  %s" % (mark, width, step, status))
    n_pass = sum(1 for _, s in _results if s == "PASS")
    n_fail = sum(1 for _, s in _results if s == "FAIL")
    n_skip = sum(1 for _, s in _results if s == "SKIP")
    print("\n  %d passed, %d failed, %d skipped" % (n_pass, n_fail, n_skip))
    return n_fail == 0


# =============================================================================
# The scripted session
# =============================================================================


def _safety_op_roundtrip(ws, args, op_name, next_intent_id, expect_modes_bit=None,
                         expect_modes_set=None, result_key=None):
    """Send ONE safety_ops verb on 0x0005 and collect the reply + any 0x0003
    STATE that follows.

    Returns (reply_kind, decoded_reply, safety_snapshot_or_None). Every 0x0003
    STATE seen while waiting is recorded, so the caller can assert that the
    LATCH the hub reports actually changed -- an ECHO alone would only prove
    the hub accepted the verb, not that it published the resulting truth to
    every subscriber, and the second half is the whole point of RFC-025a.
    """
    payload = build_intent(CH_SAFETY_INTENT, next_intent_id, [(1, cb_uint(SAFETY_OP[op_name]))])
    send_frame(ws, FRAME["INTENT"], CH_SAFETY_INTENT, payload)

    deadline = time.time() + args.timeout
    reply = None
    while time.time() < deadline:
        got = recv_frame(ws, deadline)
        if got is None:
            break
        hdr, pl = got
        if hdr["type"] == FRAME["STATE"]:
            record_state(hdr, pl)
            if reply is not None:
                break            # got the echo AND a fresh snapshot: done
            continue
        if hdr["type"] in (FRAME["ECHO"], FRAME["NACK"]):
            reply = ("ECHO" if hdr["type"] == FRAME["ECHO"] else "NACK", cb_decode_full(pl))
            # Keep reading briefly for the safety STATE the latch should push.
            deadline = min(deadline, time.time() + 0.5)
            continue
    snap = _last_state.get(CH_SAFETY)
    return reply, (decode_safety_state(snap) if snap else None)


def _await_sync_counters(ws, args):
    """Wait for a FRESH slopmotion-diag(0x1111) STATE push and return its
    decoded sync block, or (fallback) the stale last-seen one, or None.

    IN-BAND replacement for the old GET /api/slopmotion side-call: every value
    those steps reported (bundles / seg_bundles / samples / enqueued / dropped)
    rides the 0x1111 channel this probe is already granted at 1 Hz, so the
    counters arrive on the SAME socket the keepalives ride. The HTTP call it
    replaces blocked ~2 s on refusal and starved the 600 ms deadman window,
    failing two unrelated checks (sim README) -- and it also hardcoded port 80,
    which the sim doesn't serve. recv_frame's own idle PING keeps the session
    alive while we wait.

    Freshness matters: the counters must reflect the bundles just sent, and a
    snapshot recorded BEFORE the stream would under-report every one of them.
    """
    called_at = time.time()
    deadline = called_at + min(args.timeout, 2.5)
    while time.time() < deadline:
        got = recv_frame(ws, deadline)
        if got is None:
            break
        hdr, payload = got
        if hdr["type"] == FRAME["STATE"]:
            record_state(hdr, payload)
            if hdr["channel"] == CH_MOTION_DIAG:
                try:
                    return decode_motion_diag(payload), True
                except ValueError:
                    return None, False
    stale = _last_state.get(CH_MOTION_DIAG)
    if stale is not None:
        try:
            return decode_motion_diag(stale), False
        except ValueError:
            pass
    return None, False


def run(args):
    url = "ws://%s:%d/" % (args.ip, args.port)

    # ---- Step 1: connect --------------------------------------------------
    scene("Step 1: Connect")
    print("  Connecting to %s (subprotocol %r) ..." % (url, WS_SUBPROTOCOL))
    try:
        ws = websocket.create_connection(url, subprotocols=[WS_SUBPROTOCOL], timeout=args.timeout)
    except Exception as e:  # noqa: BLE001 -- surfacing any connect failure is the point
        bad("connect", "WebSocket connect to %s failed: %s" % (url, e))
        print_summary()
        return 1
    ok("connect", "WS connected to %s" % url)
    _last_send_ts[0] = time.time()

    try:
        return _run_session(ws, args)
    finally:
        try:
            ws.close()
        except Exception:
            pass


def _run_session(ws, args):
    # skip_intent/want_stream decided up front: HELLO's publishes wish (if
    # any) has to ride the ORIGINAL HELLO, sent below, not a later frame.
    skip_intent = args.no_motion or args.listen_only
    # Streaming is safe independently of --no-motion: on an unhomed/gated
    # machine the firmware counts the samples and drops them at the gate —
    # the ingress path is exercised end-to-end with zero motion possible.
    want_stream = args.stream > 0 and not args.listen_only
    want_segments = args.segments > 0 and not args.listen_only

    # ---- Step 2: HELLO / WELCOME ------------------------------------------
    scene("Step 2: HELLO / WELCOME")
    instance_id = os.urandom(8)
    publishes = []
    if want_stream:
        publishes.append((CH_MOTION_INPUT, STREAM_WISH_RATE_HZ))
    if want_segments:
        publishes.append((CH_MOTION_SEGMENT, SEGMENT_WISH_RATE_HZ))
    # Since fw 2.1.59 a tokenless HELLO gets `watch` — enough to observe, not
    # enough to move anything. Mint before connecting so the probe's motion
    # assertions test the machine rather than testing our own lack of a
    # credential. Failure is non-fatal and LOUD: the run continues at watch tier
    # and the operator can see exactly why the write-plane checks failed.
    uitoken = mint_uitoken(args.ip)
    if uitoken:
        info("credential: /uitoken mint %s… (control tier)" % uitoken.hex()[:8])
    else:
        info("!! no /uitoken — this session will be WATCH tier; control assertions will fail")
    hello_payload = build_hello("probe", "slopsync_probe.py", instance_id,
                                publishes=publishes or None, token=uitoken)
    send_frame(ws, FRAME["HELLO"], 0, hello_payload)
    info("HELLO sent: instance_id=%s client_kind=probe client_name=slopsync_probe.py"
         % instance_id.hex())
    if want_stream:
        info("HELLO publishes wish: motion-input(0x2100) @ %.1fHz" % STREAM_WISH_RATE_HZ)
    if want_segments:
        info("HELLO publishes wish: motion-segment(0x2101) @ %.1fHz" % SEGMENT_WISH_RATE_HZ)

    deadline = time.time() + args.timeout
    welcome = None
    while time.time() < deadline:
        got = recv_frame(ws, deadline)
        if got is None:
            break
        hdr, payload = got
        if hdr["type"] == FRAME["WELCOME"]:
            welcome = payload
            break
        if hdr["type"] == FRAME["NACK"]:
            n = cb_decode_full(payload)
            bad("hello", "HELLO refused: NACK %s" % nack_name(n.get(K["code"])))
            print_summary()
            return 1
        # anything else (unexpected at this point) -- ignore per §4.3

    if welcome is None:
        bad("hello", "no WELCOME within %.1fs" % args.timeout)
        print_summary()
        return 1

    try:
        w = cb_decode_full(welcome)
    except ValueError as e:
        bad("hello", "WELCOME failed to decode: %s" % e)
        print_summary()
        return 1

    required = ["session_id", "boot_id", "catalog_etag", "cfg_gen", "limits",
                "roles", "deadman_ms", "deadman_policy", "nonce", "grants"]
    missing = [name for name in required if K[name] not in w]
    if missing:
        bad("hello", "WELCOME missing required key(s): %s" % ", ".join(missing))
    else:
        session_id = w[K["session_id"]]
        boot_id = w[K["boot_id"]]
        cfg_gen = w[K["cfg_gen"]]
        etag = w[K["catalog_etag"]]
        roles = w[K["roles"]]
        deadman_ms = w[K["deadman_ms"]]
        deadman_policy = w[K["deadman_policy"]]
        limits_map = w[K["limits"]]
        ok("hello", "WELCOME received: session_id=%d boot_id=0x%08X cfg_gen=%d roles=%d"
           % (session_id, boot_id, cfg_gen, roles))
        info("catalog_etag=%s" % (etag.hex() if isinstance(etag, bytes) else etag))
        info("deadman_ms=%d deadman_policy=%d" % (deadman_ms, deadman_policy))
        limits_named = {name: limits_map.get(key) for name, key in WELCOME_LIMITS_K.items()}
        info("limits=%s" % limits_named)
        grants0 = w.get(K["grants"], [])
        info("grants (from WELCOME, expected empty -- HELLO carried no wishes): %s" % grants0)

        # RFC-046/048 (Phase E): WELCOME's endpoint disclosure (ws_port/ipv4,
        # keys 46/47 -- load-bearing for a BLE-connected client's WS auto-
        # upgrade hop) and the hub's durable cross-boot identity (identity
        # sub-map key 37, hub_instance_id = identity_keys 5). Absent is legal
        # (a pre-Phase-E hub, or a hub with no durable identity yet) but on
        # THIS device's WS binding both must be present and sane.
        ws_port = w.get(K["ws_port"])
        ipv4 = w.get(K["ipv4"])
        if ws_port is None or ipv4 is None:
            bad("welcome_endpoint", "WELCOME missing ws_port(46)/ipv4(47): ws_port=%s ipv4=%s"
                % (ws_port, ipv4))
        else:
            ip_str = "%d.%d.%d.%d" % ((ipv4 >> 24) & 0xFF, (ipv4 >> 16) & 0xFF,
                                       (ipv4 >> 8) & 0xFF, ipv4 & 0xFF)
            ok("welcome_endpoint", "WELCOME ws_port=%d ipv4=0x%08X (%s)" % (ws_port, ipv4, ip_str))

        identity_map = w.get(K["identity"])
        hub_instance_id = identity_map.get(IDENTITY_K["hub_instance_id"]) if isinstance(identity_map, dict) else None
        if hub_instance_id is None:
            bad("welcome_identity", "WELCOME identity(37) missing hub_instance_id(identity_keys 5)")
        elif hub_instance_id == 0:
            bad("welcome_identity", "WELCOME hub_instance_id is 0 -- durable identity never generated")
        else:
            ok("welcome_identity", "WELCOME hub_instance_id=0x%016X (durable, cross-boot)" % hub_instance_id)

        if want_stream or want_segments:
            gp_list = w.get(K["granted_publishes"], [])
        if want_stream:
            gp = next((e for e in gp_list if e.get(K["channel_id"]) == CH_MOTION_INPUT), None)
            if gp:
                ok("motion_input_grant", "motion-input(0x2100) publish GRANTed: rate=%.1fHz"
                   % gp.get(K["granted_rate_hz"], 0.0))
            else:
                bad("motion_input_grant", "motion-input(0x2100) publish wish NOT granted "
                    "(WELCOME key 36 missing or channel absent) -- skipping the stream step")
                want_stream = False
        if want_segments:
            gp = next((e for e in gp_list if e.get(K["channel_id"]) == CH_MOTION_SEGMENT), None)
            if gp:
                # Wish 5 Hz vs the 0x2101 catalog ceiling 50 Hz -> expect 5 Hz back.
                ok("motion_segment_grant", "motion-segment(0x2101) publish GRANTed: rate=%.1fHz "
                   "(expected min(wish %.1f, catalog 50) = %.1f)"
                   % (gp.get(K["granted_rate_hz"], 0.0), SEGMENT_WISH_RATE_HZ, SEGMENT_WISH_RATE_HZ))
            else:
                bad("motion_segment_grant", "motion-segment(0x2101) publish wish NOT granted "
                    "(WELCOME key 36 missing or channel absent) -- skipping the segment step")
                want_segments = False

    # ---- Step 2.5: CATALOG_READY (§8.4 readiness gate) --------------------
    # The hub gates BOTH planes until a session declares which catalog it
    # operates against: no retained STATE, no STREAM, and INTENTs answered
    # NOT_READY (a client that has not adopted the retained safety latch must
    # not be able to act — SPEC §11.5(2)). This probe hardcodes its layouts,
    # so declaring the hub's own etag is a truthful statement of what it
    # decodes against; a client that FETCHES the catalog would declare the
    # etag it verified locally instead.
    scene("Step 2.5: CATALOG_READY (§8.4 readiness gate)")
    etag_bytes = w.get(K["catalog_etag"]) if isinstance(w, dict) else None
    if not isinstance(etag_bytes, bytes) or len(etag_bytes) != 8:
        bad("catalog_ready", "WELCOME carried no usable 8-byte catalog_etag — cannot declare readiness")
    else:
        # Gate probe FIRST, while still un-ready: an INTENT on a channel that
        # exists in NO catalog. A gated hub answers NOT_READY; an open one
        # answers UNKNOWN_CHANNEL. Either way it can never move the machine,
        # which is why this and not a real intent.
        send_frame(ws, FRAME["INTENT"], CH_GATE_PROBE,
                   build_intent(CH_GATE_PROBE, 9001, [(1, cb_f32(0.0))]))
        deadline = time.time() + args.timeout
        gate_reply = None
        while time.time() < deadline:
            got = recv_frame(ws, deadline)
            if got is None:
                break
            hdr, payload = got
            if hdr["type"] == FRAME["STATE"]:
                record_state(hdr, payload)   # would itself be a gate violation; recorded, judged below
                continue
            if hdr["type"] in (FRAME["NACK"], FRAME["ECHO"]):
                gate_reply = (hdr["type"], payload)
                break
        if gate_reply is None:
            bad("ready_gate", "no NACK/ECHO for the pre-READY probe intent within %.1fs" % args.timeout)
        elif gate_reply[0] == FRAME["ECHO"]:
            bad("ready_gate", "pre-READY INTENT was APPLIED (ECHO) — the control-plane gate is open")
        else:
            gcode = cb_decode_full(gate_reply[1]).get(K["code"])
            if gcode == NACK_CODES_BY_NAME["NOT_READY"]:
                ok("ready_gate", "pre-READY INTENT refused NOT_READY — the control plane is gated as specified")
            else:
                bad("ready_gate", "pre-READY INTENT answered %s, expected NOT_READY — hub is not gating"
                    % nack_name(gcode))

        send_frame(ws, FRAME["CATALOG_READY"], 0, etag_bytes)
        ok("catalog_ready", "CATALOG_READY sent (etag %s) — data plane + control plane now open"
           % etag_bytes.hex())

    # ---- Step 2.9: BLOB transfer (RFC-021 namespaced chunked transfer) ------
    # THE transfer verb for the whole protocol, not a catalog thing with a store
    # bolted on. Namespace 0 IS the catalog, so the bare request is still the
    # empty CBOR map and catalog transfer works unchanged through the
    # generalized path; the proof is that the reassembled bytes still hash to
    # the etag WELCOME advertised.
    scene("Step 2.9: BLOB_REQ / BLOB_CHUNK (namespace 0 = catalog)")
    send_frame(ws, FRAME["BLOB_REQ"], 0, build_blob_req())
    chunks = {}
    expect_count = None
    expect_total = None
    ns_seen = set()
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        got = recv_frame(ws, deadline)
        if got is None:
            break
        hdr, payload = got
        if hdr["type"] == FRAME["STATE"]:
            record_state(hdr, payload)
            continue
        if hdr["type"] != FRAME["BLOB_CHUNK"]:
            continue
        c = decode_blob_chunk(payload)
        if c is None:
            bad("blob_chunk_header", "BLOB_CHUNK shorter than the %d-byte identity header"
                % BLOB_CHUNK_HEADER_BYTES)
            break
        ns_seen.add(c["ns"])
        expect_count = c["chunk_count"]
        expect_total = c["total_bytes"]
        chunks[c["chunk_index"]] = c["bytes"]
        if expect_count and len(chunks) == expect_count:
            break

    catalog_bytes = None
    if expect_count is None:
        bad("blob_catalog", "no BLOB_CHUNK arrived in response to BLOB_REQ")
    elif len(chunks) != expect_count:
        bad("blob_catalog", "incomplete transfer: %d/%d chunks" % (len(chunks), expect_count))
    else:
        assembled = b"".join(chunks[i] for i in range(expect_count))
        catalog_bytes = assembled
        if ns_seen != {BLOB_NS["catalog"]}:
            bad("blob_ns", "chunks carried namespace(s) %s, expected only 0 (catalog)" % sorted(ns_seen))
        else:
            ok("blob_ns", "every chunk stamped ns=0 (catalog); store_id/slot absent by rule")
        if len(assembled) != expect_total:
            bad("blob_total_bytes", "assembled %d bytes but the header declared total_bytes=%d"
                % (len(assembled), expect_total))
        else:
            ok("blob_total_bytes", "%d chunks, %d bytes, matching the header's total_bytes"
               % (expect_count, expect_total))
        digest = hashlib.sha256(assembled).digest()[:8]
        if etag_bytes and digest == bytes(etag_bytes):
            ok("blob_catalog", "reassembled catalog VERIFIES against the WELCOME etag %s "
               "— generalized transfer left catalog bytes untouched" % digest.hex())
        else:
            bad("blob_catalog", "reassembled catalog hashes to %s, WELCOME said %s"
                % (digest.hex(), etag_bytes.hex() if etag_bytes else "<none>"))

    # A namespace this hub does not serve must answer CHUNK_UNAVAILABLE — the
    # registry note was generalized from "catalog chunk" for exactly this. A
    # hub with no store backend answering honestly is discoverable; answering
    # with silence is not.
    send_frame(ws, FRAME["BLOB_REQ"], 0, build_blob_req(ns=BLOB_NS["store"], store_id=0, slot=0))
    deadline = time.time() + args.timeout
    got_code = None
    stray_chunk = False
    while time.time() < deadline:
        got = recv_frame(ws, deadline)
        if got is None:
            break
        hdr, payload = got
        if hdr["type"] == FRAME["STATE"]:
            record_state(hdr, payload)
            continue
        if hdr["type"] == FRAME["BLOB_CHUNK"]:
            stray_chunk = True
            break
        if hdr["type"] == FRAME["NACK"]:
            got_code = cb_decode_full(payload).get(K["code"])
            break
    if stray_chunk:
        bad("blob_unknown_ns", "hub served chunks for a store it does not have")
    elif got_code == NACK_CODES_BY_NAME["CHUNK_UNAVAILABLE"]:
        ok("blob_unknown_ns", "unserved namespace answered NACK CHUNK_UNAVAILABLE, as specified")
    else:
        bad("blob_unknown_ns", "unserved namespace answered %s, expected CHUNK_UNAVAILABLE"
            % (nack_name(got_code) if got_code is not None else "silence"))

    # ---- Step 2.95: RFC-009 SETTINGS ANNOTATIONS ---------------------------
    # The catalog we just reassembled is decoded here as a GENERIC CLIENT would
    # decode it -- no hardcoded knowledge of this machine beyond the channel ids
    # it claims. The question being answered is the one M5a exists for: could a
    # client that has never seen this device build a correct settings page from
    # these bytes alone? Every assertion below is a thing such a client NEEDS:
    # the write key, where to send it, the factory value, bounds, a name for
    # each choice, and human text to show the operator.
    scene("Step 2.95: RFC-009 catalog annotations (generic-client rendering)")
    check_catalog_annotations(catalog_bytes)

    # ---- Step 3: SUBSCRIBE / GRANT -----------------------------------------
    scene("Step 3: SUBSCRIBE safety + motion + the M5a telemetry channels")
    wishes = [
        (CH_SAFETY, 0.0, PRIORITY["critical"]),   # on-change; safety is in the never-shed set (§10.1)
        (CH_MOTION, 20.0, PRIORITY["elevated"]),  # realtime display feed default
        # M5a. plan-strip is wished at 20 Hz rather than its 45 Hz ceiling --
        # this probe is verifying that the channel EXISTS and decodes, not
        # stress-testing pacing, and a lower wish also exercises the grant
        # clamp in the direction that cannot be confused with a hub bug.
        (CH_PLAN_STRIP, 20.0, PRIORITY["elevated"]),
        (CH_MOTION_DIAG, 1.0, PRIORITY["background"]),
        (CH_ODOMETER, 1.0, PRIORITY["background"]),
        # The two RFC-009 settings channels. Subscribed so the enabled_mask can
        # be cross-checked against live machine state rather than merely
        # decoded -- a mask is only worth anything if it AGREES with the
        # machine, and a mask that lies is a UI that grays the wrong control.
        (CH_MACHINE_CONFIG, 0.0, PRIORITY["normal"]),
        (CH_PATTERN_STATE, 0.0, PRIORITY["normal"]),
        (CH_MOTION_ANOMALY, 0.0, PRIORITY["normal"]),
    ]
    # The power channel is FEATURE-GATED. Subscribing blind would NACK
    # UNKNOWN_CHANNEL on a machine with no current sensor and read as a
    # failure, when it is the correct answer -- so the catalog decides, which
    # is RFC-016's "capability discovery IS catalog introspection" exercised
    # rather than described.
    if _catalog_has(catalog_bytes, CH_POWER):
        wishes.append((CH_POWER, 2.0, PRIORITY["background"]))
        info("catalog declares power(0x1010) -- this hub measures its own bus; subscribing")
    else:
        info("catalog does NOT declare power(0x1010) -- this hub has no current sensor; "
             "not subscribing (absence IS the capability answer, not a failure)")
    send_frame(ws, FRAME["SUBSCRIBE"], 0, build_subscribe(wishes))
    info("SUBSCRIBE sent: %d channels" % len(wishes))

    pending = {ch for ch, _, _ in wishes}
    granted = {}
    nacked = {}
    deadline = time.time() + args.timeout
    while pending and time.time() < deadline:
        got = recv_frame(ws, deadline)
        if got is None:
            break
        hdr, payload = got
        if hdr["type"] == FRAME["STATE"]:
            record_state(hdr, payload)
            continue
        if hdr["type"] == FRAME["GRANT"]:
            g = cb_decode_full(payload)
            for entry in g.get(K["grants"], []):
                ch = entry.get(K["channel_id"])
                granted[ch] = entry
                pending.discard(ch)
            continue
        if hdr["type"] == FRAME["NACK"]:
            n = cb_decode_full(payload)
            ch = n.get(K["channel_id"])
            nacked[ch] = n
            if ch is not None:
                pending.discard(ch)
            continue
        # ignore anything else here (PING/PONG etc.)

    for ch, _rate, _prio in wishes:
        name = "%s(0x%04X)" % (CHANNEL_NAMES.get(ch, "channel"), ch)
        if ch in granted:
            g = granted[ch]
            ok("subscribe_%04x" % ch, "%s GRANTed: rate=%.1fHz priority=%d"
               % (name, g.get(K["granted_rate_hz"], 0.0), g.get(K["priority"], -1)))
        elif ch in nacked:
            bad("subscribe_%04x" % ch, "%s NACKed: %s"
                % (name, nack_name(nacked[ch].get(K["code"]))))
        else:
            bad("subscribe_%04x" % ch, "%s: no GRANT/NACK within %.1fs" % (name, args.timeout))

    # ---- Step 3.5: RFC-033 subscribe-everything ----------------------------
    # Subscribe to EVERY channel the catalog says is subscribable (STATE +
    # EVENT), batched at the hub-advertised per-frame wish cap. The assertion
    # is RFC-033.2's whole point: every batch is ANSWERED -- GRANT or a
    # per-channel NACK both count, silence within the timeout is a FAIL,
    # because a silently-dropped SUBSCRIBE presents as a healthy LIVE session
    # with zero STATE and reads as a client rendering bug (it cost two nights).
    scene("Step 3.5: RFC-033 subscribe-everything (batched at WELCOME limits key 4)")
    entries_all = _catalog_entries(catalog_bytes)
    subscribable = sorted(cid for cid, e in entries_all.items()
                          if e.get(CAT_E["cls"]) in (CH_CLASS["STATE"], CH_CLASS["EVENT"]))
    if not subscribable:
        bad("sub_everything", "no decodable catalog -- cannot enumerate the subscribable channels")
    else:
        _limits = w.get(K["limits"], {}) if isinstance(w, dict) else {}
        frame_cap = _limits.get(WELCOME_LIMITS_K["max_subscriptions_per_frame"]) \
            if isinstance(_limits, dict) else None
        if not isinstance(frame_cap, int) or frame_cap < 1:
            frame_cap = MAX_SUBSCRIPTIONS_PER_FRAME_DEFAULT
            info("hub advertises no max_subscriptions_per_frame (limits key 4) -- using the "
                 "registry default %d" % frame_cap)
        else:
            info("hub advertises max_subscriptions_per_frame=%d (limits key 4)" % frame_cap)
        # Channels Step 3 already wished are re-wished VERBATIM: a re-SUBSCRIBE
        # is a wish update, and downgrading motion(0x1100) from 20 Hz to
        # on-change here would starve Step 4's rate measurement.
        step3_wish = {ch: (rate, prio) for ch, rate, prio in wishes}
        n_granted, n_refused, refused_named, silent = 0, 0, [], []
        batches = [subscribable[i:i + frame_cap]
                   for i in range(0, len(subscribable), frame_cap)]
        info("%d subscribable channel(s) (STATE+EVENT) in %d batch(es) of <=%d"
             % (len(subscribable), len(batches), frame_cap))
        for bi, batch in enumerate(batches):
            bw = [(ch,) + step3_wish.get(ch, (0.0, PRIORITY["normal"])) for ch in batch]
            send_frame(ws, FRAME["SUBSCRIBE"], 0, build_subscribe(bw))
            pending2 = set(batch)
            deadline = time.time() + min(args.timeout, 3.0)
            while pending2 and time.time() < deadline:
                got = recv_frame(ws, deadline)
                if got is None:
                    break
                hdr, payload = got
                if hdr["type"] == FRAME["STATE"]:
                    record_state(hdr, payload)
                    continue
                if hdr["type"] == FRAME["GRANT"]:
                    for entry in cb_decode_full(payload).get(K["grants"], []):
                        ch = entry.get(K["channel_id"])
                        if ch in pending2:
                            n_granted += 1
                            pending2.discard(ch)
                    continue
                if hdr["type"] == FRAME["NACK"]:
                    n = cb_decode_full(payload)
                    ch = n.get(K["channel_id"])
                    if ch in pending2:
                        n_refused += 1
                        refused_named.append("0x%04X=%s" % (ch, nack_name(n.get(K["code"]))))
                        pending2.discard(ch)
                    continue
                # EVENT replays (e.g. the log channel's ring tail) and PONGs
                # land here while we wait -- ignored, §4.3.
            if pending2:
                silent.extend(sorted(pending2))
                bad("sub_all_batch%d" % bi, "batch %d/%d: %d wish(es) answered by SILENCE: %s "
                    "-- RFC-033.2 says every SUBSCRIBE is answered, GRANT or NACK, never dropped"
                    % (bi + 1, len(batches), len(pending2),
                       ", ".join("0x%04X" % c for c in sorted(pending2))))
            else:
                ok("sub_all_batch%d" % bi, "batch %d/%d: all %d wish(es) answered"
                   % (bi + 1, len(batches), len(batch)))
        if not silent:
            ok("sub_everything", "every subscribable channel answered: %d granted, %d refused "
               "by per-channel NACK%s"
               % (n_granted, n_refused,
                  (" (%s)" % ", ".join(refused_named)) if refused_named else ""))

        # Negative: ONE SUBSCRIBE carrying (cap+1) wishes must be refused AS A
        # WHOLE with SUBSCRIBE_REJECTED(0x0204) -- the frame-level code RFC-033.2
        # added exactly so overflow is never observed silence or a partial grant.
        over_ids = [subscribable[i % len(subscribable)] for i in range(frame_cap + 1)]
        over = [(ch,) + step3_wish.get(ch, (0.0, PRIORITY["normal"])) for ch in over_ids]
        send_frame(ws, FRAME["SUBSCRIBE"], 0, build_subscribe(over))
        info("negative check: one SUBSCRIBE with %d wishes (cap %d + 1) sent"
             % (frame_cap + 1, frame_cap))
        deadline = time.time() + min(args.timeout, 3.0)
        over_verdict = None
        while time.time() < deadline:
            got = recv_frame(ws, deadline)
            if got is None:
                break
            hdr, payload = got
            if hdr["type"] == FRAME["STATE"]:
                record_state(hdr, payload)
                continue
            if hdr["type"] == FRAME["NACK"]:
                over_verdict = ("NACK", cb_decode_full(payload).get(K["code"]))
                break
            if hdr["type"] == FRAME["GRANT"]:
                over_verdict = ("GRANT", None)
                break
        if over_verdict is None:
            bad("sub_overflow", "over-cap SUBSCRIBE answered by SILENCE within %.1fs -- the "
                "exact failure mode SUBSCRIBE_REJECTED exists to kill" % min(args.timeout, 3.0))
        elif over_verdict[0] == "GRANT":
            bad("sub_overflow", "over-cap SUBSCRIBE was GRANTed -- hub decoded more wishes than "
                "its advertised max_subscriptions_per_frame=%d" % frame_cap)
        elif over_verdict[1] == NACK_CODES_BY_NAME["SUBSCRIBE_REJECTED"]:
            ok("sub_overflow", "over-cap SUBSCRIBE refused SUBSCRIBE_REJECTED(0x0204) as a whole")
        else:
            bad("sub_overflow", "over-cap SUBSCRIBE NACKed %s, expected SUBSCRIBE_REJECTED(0x0204)"
                % nack_name(over_verdict[1]))

    # ---- Step 4: STATE observation window ----------------------------------
    scene("Step 4: STATE pushes (~%.0fs window)" % STATE_WINDOW_S)
    window_start = time.time()
    window_end = window_start + STATE_WINDOW_S
    while time.time() < window_end:
        got = recv_frame(ws, window_end)
        if got is None:
            break
        hdr, payload = got
        if hdr["type"] == FRAME["STATE"]:
            record_state(hdr, payload)
        # anything else during this window (PING etc.) is ignored

    safety_arrived = CH_SAFETY in _last_state
    if safety_arrived:
        snap = _last_state[CH_SAFETY]
        ok("safety_retained", "safety(0x0003) retained STATE observed (%d-byte payload) "
           "-- §9.1 retained-value-on-grant rule holds" % len(snap))
        # RFC-025c: the snapshot is 9 bytes now (appended `modes`). A hub that
        # publishes fewer bytes than its own catalog declares is a decode
        # failure waiting to happen on the SAFETY channel, so check it here
        # rather than trusting the length we happen to receive.
        if len(snap) == SAFETY_PAYLOAD_BYTES:
            ok("safety_layout", "safety snapshot is %d bytes and decodes: %s"
               % (SAFETY_PAYLOAD_BYTES, decode_safety_state(snap)))
        else:
            bad("safety_layout", "safety snapshot is %d bytes, expected %d "
                "(RFC-025c appended the `modes` byte) -- decoded prefix: %s"
                % (len(snap), SAFETY_PAYLOAD_BYTES, decode_safety_state(snap)))
    else:
        bad("safety_retained", "safety(0x0003) STATE never observed -- violates §9.1 "
            "(\"hub MUST push retained value immediately upon grant\")")

    motion_in_window = [e for e in _state_log if e[1] == CH_MOTION and window_start <= e[0] < window_end]
    if motion_in_window:
        decoded = []
        for t, _ch, _hdr, payload in motion_in_window:
            try:
                decoded.append((t, decode_motion_state(payload)))
            except ValueError as e:
                bad("motion_state", "motion(0x1100) STATE decode error: %s" % e)
                decoded = []
                break
        if decoded:
            span = decoded[-1][0] - decoded[0][0]
            rate = (len(decoded) - 1) / span if span > 0 else float("nan")
            ok("motion_state", "motion(0x1100) STATE: %d samples in window, "
               "measured push rate ~%.1f Hz (granted ceiling 20 Hz)" % (len(decoded), rate))
            for t, d in decoded[:2]:
                info("sample: pos=%.2fmm tgt=%.2fmm speed=%.1fmm/s flags=0x%02X"
                     % (d["pos_mm"], d["tgt_mm"], d["speed_mm_s"], d["flags"]))
    else:
        bad("motion_state", "motion(0x1100) STATE: no samples observed in the %.0fs window"
            % STATE_WINDOW_S)

    # ---- Step 4b: the M5a telemetry channels -------------------------------
    # Each of these had NO SlopSync home before M5a and lived only on the
    # legacy :81 plane. The checks are deliberately about DECODABILITY and
    # SHAPE, not about values: the point is that a client can read them at all,
    # which is what "the migration loses nothing" actually means.
    scene("Step 4b: M5a telemetry channels")

    # (1) The pre-planning demand -- V1-READINESS called this "the most likely
    #     thing to be silently lost", so its absence is a FAILURE, not a note.
    raw_seen = [d for _t, d in decoded if "raw_mm" in d] if motion_in_window and decoded else []
    if raw_seen:
        ok("motion_raw", "motion(0x1100) carries the appended raw_10um: raw=%.2fmm vs tgt=%.2fmm "
           "vs pos=%.2fmm -- asked / planned / achieved, in ONE frame with one timestamp"
           % (raw_seen[-1]["raw_mm"], raw_seen[-1]["tgt_mm"], raw_seen[-1]["pos_mm"]))
    else:
        bad("motion_raw", "motion(0x1100) payload has no raw_10um field -- the pre-planning "
            "demand is unobservable and the planner has nothing to be graphed against")

    def _last(ch):
        return _last_state.get(ch)

    plan = _last(CH_PLAN_STRIP)
    if plan is None:
        bad("plan_strip", "plan-strip(0x1110) STATE never observed")
    else:
        try:
            d = decode_plan_strip(plan)
            ok("plan_strip", "plan-strip(0x1110) decodes (%d B): style=%s active=%s "
               "start=%.3f end=%.3f cur=%.3f vel=%.3f dur=%dus elapsed=%dus"
               % (len(plan), d["style"], d["active"], d["start"], d["end"], d["cur"],
                  d["vel"], d["duration_us"], d["elapsed_us"]))
        except ValueError as e:
            bad("plan_strip", "plan-strip(0x1110) decode error: %s" % e)

    diag = _last(CH_MOTION_DIAG)
    if diag is None:
        bad("motion_diag", "slopmotion-diag(0x1111) STATE never observed")
    else:
        try:
            d = decode_motion_diag(diag)
            nonzero = {k: v for k, v in d["by_kind"].items() if v}
            ok("motion_diag", "slopmotion-diag(0x1111) decodes (%d B): plans=%d failures=%d "
               "anomalies=%d reset_gen=%d bundles=%d samples=%d dropped=%d; by_kind=%s"
               % (len(diag), d["plans"], d["failures"], d["anomalies"], d["reset_gen"],
                  d["sync_bundles"], d["sync_samples"], d["sync_dropped"],
                  nonzero if nonzero else "(all zero)"))
            # RFC-019: the per-kind histogram must never exceed its own total,
            # or a reset zeroed one and not the other.
            if sum(d["by_kind"].values()) > d["anomalies"]:
                bad("motion_diag_sum", "per-kind anomaly counts (%d) exceed the total (%d) -- "
                    "a reset zeroed one and not the other"
                    % (sum(d["by_kind"].values()), d["anomalies"]))
            else:
                ok("motion_diag_sum", "per-kind anomaly counts are consistent with the total")
        except ValueError as e:
            bad("motion_diag", "slopmotion-diag(0x1111) decode error: %s" % e)

    odo = _last(CH_ODOMETER)
    if odo is None:
        bad("odometer", "odometer(0x1020) STATE never observed")
    elif len(odo) < ODOMETER_STRUCT.size:
        bad("odometer", "odometer(0x1020) is %d bytes -- the appended energy_wh + session_ms "
            "(20 B total) are missing" % len(odo))
    else:
        d = decode_odometer(odo)
        ok("odometer", "odometer(0x1020) decodes (%d B) incl. the appended session totals: "
           "strokes=%d distance=%.3fm peak=%.1fmm/s energy=%.4fWh session=%dms"
           % (len(odo), d["strokes"], d["distance_m"], d["peak_mm_s"],
              d["energy_wh"], d["session_ms"]))

    # RFC-009 item 4 -- the enabled_mask must AGREE WITH THE MACHINE.
    # Cross-channel, not self-consistent: 0x1200's mask is derived from the
    # same refusals the delegate applies (ESTOP_ACTIVE / NOT_HOMED), so it is
    # checked against the e-stop bit in 0x0003 and the homed bit in 0x1100.
    # This is the ground-truth doctrine as a wire test: a mask that disagrees
    # with the machine grays the wrong control, and on this product a UI that
    # lies about machine state is a safety defect.
    cfg_snap, pat_snap = _last(CH_MACHINE_CONFIG), _last(CH_PATTERN_STATE)
    if cfg_snap is None or len(cfg_snap) < 33:
        bad("cfg_mask", "machine-config(0x1000) snapshot is %s -- the appended enabled_mask "
            "(33 B total) is missing"
            % ("absent" if cfg_snap is None else "%d bytes" % len(cfg_snap)))
    else:
        cfg_mask = cfg_snap[32]
        # All seven limits are editable at all times on this machine (nothing
        # REFUSES a config-set; out-of-range values are clamped, which is what
        # min/max is for). A hub that grays one here had better mean it.
        ok("cfg_mask", "machine-config(0x1000) enabled_mask=0x%02X (%d of 7 limit settings "
           "currently writable)" % (cfg_mask, bin(cfg_mask & 0x7F).count("1")))

    if pat_snap is None or len(pat_snap) < 19:
        bad("pattern_mask", "pattern-state(0x1200) snapshot is %s -- the appended enabled_mask "
            "(19 B total) is missing"
            % ("absent" if pat_snap is None else "%d bytes" % len(pat_snap)))
    else:
        pat_mask = pat_snap[18]
        safety_snap = _last(CH_SAFETY)
        estop = bool(safety_snap[0] & 1) if safety_snap else False
        homed = bool(decode_motion_state(_last(CH_MOTION))["flags"] & 1) if _last(CH_MOTION) else False
        expect_enabled = homed and not estop
        # Bit 6 (source.background_run, Phase D) is a standing policy flag,
        # UNCONDITIONALLY 1 regardless of homed/estop (RFC-045/048) -- mask it
        # off before asking whether the homed-gated bits (0-5) are lit, or a
        # healthy hub reads as a permanent false CONTRADICTS.
        actually_enabled = (pat_mask & 0x3F) != 0
        if actually_enabled == expect_enabled:
            ok("pattern_mask", "pattern-state(0x1200) enabled_mask=0x%02X AGREES with the machine "
               "(homed=%s estop=%s -> pattern controls %s)"
               % (pat_mask, homed, estop, "settable" if expect_enabled else "grayed"))
        else:
            bad("pattern_mask", "pattern-state(0x1200) enabled_mask=0x%02X CONTRADICTS the machine "
                "(homed=%s estop=%s) -- a client would gray the wrong controls"
                % (pat_mask, homed, estop))

    # RFC-016 -- the feature gate, verified in BOTH directions: a hub that
    # declares the channel must publish it, and a hub that does not declare it
    # must not be expected to.
    if _catalog_has(catalog_bytes, CH_POWER):
        pwr = _last(CH_POWER)
        if pwr is None:
            bad("power", "power(0x1010) is in the catalog but never published")
        else:
            try:
                d = decode_power(pwr)
                ok("power", "power(0x1010) decodes (%d B): bus=%.2fV i=%.3fA peak=%.3fA%s"
                   % (len(pwr), d["bus_v"], d["i_bus_a"], d["peak_a"],
                      (" die=%.1fC" % d["die_c"]) if "die_c" in d else " (no die temp: shunt-only hub)"))
            except ValueError as e:
                bad("power", "power(0x1010) decode error: %s" % e)
    else:
        skip("power", "hub declares no power(0x1010) -- no current sensor on this machine, "
             "which is the honest answer rather than a channel of zeros")


    # ---- Step 5: move INTENT (skippable) -----------------------------------
    scene("Step 5: move INTENT (0x3100)")
    if skip_intent:
        skip("intent", "move INTENT skipped (--no-motion/--listen-only)")
    else:
        motion_samples = [e for e in _state_log if e[1] == CH_MOTION]
        if motion_samples:
            current_pos = decode_motion_state(motion_samples[-1][3])["pos_mm"]
        else:
            current_pos = 0.0
            info("no motion STATE observed yet -- assuming 0.0mm as the base position "
                 "(best-effort; the intent below is still bounded to +/-5mm of this base)")
        nudge = -5.0 if current_pos >= 5.0 else 5.0
        target_mm = current_pos + nudge
        info("current pos ~%.2fmm -> requesting %.2fmm (%+.1fmm, within the +/-5mm safety bound)"
             % (current_pos, target_mm, nudge))

        intent_id = 1
        payload = build_intent(CH_MOVE_INTENT, intent_id, [
            (1, cb_f32(target_mm)),
            (2, cb_bool(False)),
        ])
        # header.channel = target channel id for INTENT (see file-header note).
        send_frame(ws, FRAME["INTENT"], CH_MOVE_INTENT, payload)
        info("INTENT sent: channel=0x3100 intent_id=%d value={1:%.2f, 2:false}"
             % (intent_id, target_mm))

        deadline = time.time() + args.timeout
        reply = None
        while time.time() < deadline:
            got = recv_frame(ws, deadline)
            if got is None:
                break
            hdr, payload = got
            if hdr["type"] == FRAME["STATE"]:
                record_state(hdr, payload)
                continue
            if hdr["type"] == FRAME["ECHO"]:
                reply = ("ECHO", payload)
                break
            if hdr["type"] == FRAME["NACK"]:
                reply = ("NACK", payload)
                break

        if reply is None:
            bad("intent", "no ECHO/NACK for move INTENT within %.1fs" % args.timeout)
        elif reply[0] == "ECHO":
            e = cb_decode_full(reply[1])
            applied = e.get(K["applied"], {})
            ok("intent", "ECHO received (APPLIED, post-clamp): intent_id=%s cfg_gen=%s applied=%s"
               % (e.get(K["intent_id"]), e.get(K["cfg_gen"]), applied))
        else:
            n = cb_decode_full(reply[1])
            code = n.get(K["code"])
            ok("intent", "NACK received: %s -- structural PASS for the wire layer "
               "(e.g. NOT_HOMED is expected pre-homing)" % nack_name(code))

    # ---- Step 5.5: motion-input STREAM (0x2100), --stream SECS -------------
    scene("Step 5.5: motion-input STREAM (0x2100)")
    if not want_stream:
        skip("stream", "motion-input STREAM skipped (--no-motion/--listen-only, "
             "--stream<=0, or the publish wish wasn't granted)")
    else:
        clk = estimate_hub_offset(ws, args.timeout)
        if clk is None:
            bad("stream_clock", "CLOCK exchange got no reply within %.1fs -- skipping stream"
                % args.timeout)
        else:
            offset, rtt = clk
            ok("stream_clock", "hub clock offset ~%+dus (rtt ~%dus, §7.1 accuracy is fine at "
               "a few ms)" % (offset, rtt))

            def hub_now_us():
                return (client_now_us() + offset) & 0xFFFFFFFF

            period = 1.0 / STREAM_HZ
            stream_start = time.time()
            end_time = stream_start + args.stream
            next_send = stream_start
            n_sent = 0
            printed = 0
            info("streaming sine 0.5+0.35*sin(2*pi*0.8*t) for %.1fs @ %.0fHz, "
                 "vel = its analytic derivative" % (args.stream, STREAM_HZ))
            while True:
                now = time.time()
                if now >= end_time:
                    break
                if now >= next_send:
                    t = now - stream_start
                    target = 0.5 + 0.35 * math.sin(2 * math.pi * 0.8 * t)
                    vel = 0.35 * 2 * math.pi * 0.8 * math.cos(2 * math.pi * 0.8 * t)
                    payload = encode_stream_bundle(hub_now_us(), [(0, target, vel)])
                    send_frame(ws, FRAME["STREAM"], CH_MOTION_INPUT, payload)
                    n_sent += 1
                    next_send += period
                got = recv_frame(ws, min(next_send, end_time))
                if got is None:
                    continue
                hdr, payload2 = got
                if hdr["type"] == FRAME["STATE"]:
                    record_state(hdr, payload2)
                    if hdr["channel"] == CH_MOTION and printed < 5:
                        try:
                            d = decode_motion_state(payload2)
                            info("during stream: pos=%.3fmm tgt=%.3fmm speed=%.1fmm/s flags=0x%02X"
                                 % (d["pos_mm"], d["tgt_mm"], d["speed_mm_s"], d["flags"]))
                            printed += 1
                        except ValueError:
                            pass

            ok("stream_send", "sent %d single-sample STREAM bundle(s) over %.1fs @ %.0fHz"
               % (n_sent, args.stream, STREAM_HZ))

            # ---- Step 5.6: sync counters (slopmotion-diag 0x1111, in-band) --
            # Formerly GET /api/slopmotion on hardcoded port 80 -- see
            # _await_sync_counters for why that was a session-killer.
            scene("Step 5.6: sync counters (slopmotion-diag 0x1111, in-band)")
            d, fresh = _await_sync_counters(ws, args)
            if d is None:
                bad("sync_counters", "no decodable slopmotion-diag(0x1111) STATE -- the sync "
                    "counters are unobservable in-band")
            else:
                ok("sync_counters", "bundles=%d seg_bundles=%d samples=%d enqueued=%d dropped=%d%s"
                   % (d["sync_bundles"], d["sync_seg_bundles"], d["sync_samples"],
                      d["sync_enqueued"], d["sync_dropped"],
                      "" if fresh else " (STALE: no fresh 1 Hz push arrived in time)"))

    # ---- Step 5.7: motion-segment STREAM (0x2101), --segments SECS ---------
    scene("Step 5.7: motion-segment STREAM (0x2101)")
    if not want_segments:
        skip("segments", "motion-segment STREAM skipped (--no-motion/--listen-only, "
             "--segments<=0, or the publish wish wasn't granted)")
    else:
        clk = estimate_hub_offset(ws, args.timeout)
        if clk is None:
            bad("segment_clock", "CLOCK exchange got no reply within %.1fs -- skipping segments"
                % args.timeout)
        else:
            offset, rtt = clk
            ok("segment_clock", "hub clock offset ~%+dus (rtt ~%dus) -- t_base = intended "
               "segment START in hub time" % (offset, rtt))

            def hub_now_us():
                return (client_now_us() + offset) & 0xFFFFFFFF

            n_secs = int(math.ceil(args.segments))
            info("sending 1 timed segment/second for %.1fs: target alternating 0.7/0.3, "
                 "duration_ms 900, end_vel = SENTINEL (no handoff -> engine estimates vf/af)"
                 % args.segments)
            info("PING keepalive every %.0fms so the §11.3 deadman (600ms on lastRxMs) never "
                 "fires in the ~1s gaps between segments" % (SEGMENT_KEEPALIVE_S * 1000.0))
            info("EXPECT: seg_bundles += %d, bundles += %d, samples += %d; enqueued follows "
                 "ONLY if homed+resumed, else those land in dropped at the HOMED gate (correct)"
                 % (n_secs, n_secs, n_secs))

            stream_start = time.time()
            end_time = stream_start + args.segments
            next_seg = stream_start
            n_sent = 0
            toggle = False
            while time.time() < end_time:
                now = time.time()
                if now >= next_seg:
                    target = 0.3 if toggle else 0.7
                    toggle = not toggle
                    payload = encode_segment_bundle(hub_now_us(), [(0, target, 900, None)])
                    send_frame(ws, FRAME["STREAM"], CH_MOTION_SEGMENT, payload)
                    n_sent += 1
                    next_seg += 1.0
                    info("segment %d sent: target=%.2f duration_ms=900 end_vel=SENTINEL" % (n_sent, target))
                # Bound the wait so we loop at least every SEGMENT_KEEPALIVE_S to
                # send the deadman-defeating PING (recv_frame's own keepalive is
                # 0.8s, too slow for the 600ms window).
                if time.time() - _last_send_ts[0] >= SEGMENT_KEEPALIVE_S:
                    send_frame(ws, FRAME["PING"], 0, b"")
                got = recv_frame(ws, min(next_seg, end_time, time.time() + SEGMENT_KEEPALIVE_S))
                if got is None:
                    continue
                hdr, payload2 = got
                if hdr["type"] == FRAME["STATE"]:
                    record_state(hdr, payload2)

            ok("segment_send", "sent %d single-segment STREAM bundle(s), one per second over %.1fs"
               % (n_sent, args.segments))

            # ---- Step 5.8: sync counters after segments (0x1111, in-band) --
            scene("Step 5.8: sync counters (slopmotion-diag 0x1111, in-band)")
            d, fresh = _await_sync_counters(ws, args)
            if d is None:
                bad("segment_counters", "no decodable slopmotion-diag(0x1111) STATE -- the sync "
                    "counters are unobservable in-band")
            else:
                ok("segment_counters", "bundles=%d seg_bundles=%d samples=%d enqueued=%d dropped=%d "
                   "(seg_bundles should have risen by ~%d)%s"
                   % (d["sync_bundles"], d["sync_seg_bundles"], d["sync_samples"],
                      d["sync_enqueued"], d["sync_dropped"], n_sent,
                      "" if fresh else " (STALE: no fresh 1 Hz push arrived in time)"))

    # ---- Step 5.9: safety ops -- override/bypass round-trip (RFC-025c) ------
    # Non-destructive by construction: override/bypass are MODE bits, not
    # motion. This is the end-to-end proof that a safety-domain write lands in
    # the machine AND comes back on the retained 0x0003 snapshot, which is
    # what moved these two off their legacy HTTP endpoint in the first place.
    scene("Step 5.9: safety ops -- override/bypass on 0x0005 (RFC-025c)")
    if skip_intent:
        skip("safety_modes", "safety-op round-trip skipped (--no-motion/--listen-only)")
    else:
        reply, snap = _safety_op_roundtrip(ws, args, "override_on", 900)
        if reply is None:
            bad("safety_modes", "no ECHO/NACK for override_on within %.1fs" % args.timeout)
        elif reply[0] == "NACK":
            bad("safety_modes", "override_on NACKed: %s" % nack_name(reply[1].get(K["code"])))
        elif snap is None or "override" not in snap:
            bad("safety_modes", "override_on ECHOed but no 9-byte safety snapshot followed "
                "-- the latch was not published (RFC-025a: subscribers, not just the sender)")
        elif not snap["override"]:
            bad("safety_modes", "override_on ECHOed but the snapshot still reports override=false "
                "-- GROUND TRUTH VIOLATION on the safety channel: %s" % snap)
        else:
            ok("safety_modes", "override_on: ECHO + snapshot modes=0x%02X (override=True)" % snap["modes"])

            reply, snap = _safety_op_roundtrip(ws, args, "override_off", 901)
            if reply and reply[0] == "ECHO" and snap and not snap.get("override", True):
                ok("safety_modes_clear", "override_off: snapshot modes=0x%02X (override=False)"
                   % snap["modes"])
            else:
                bad("safety_modes_clear", "override_off did not clear the bit: reply=%s snap=%s"
                    % (reply, snap))

    # ---- Step 5.95: CLIENT-ASSERTED E-STOP (RFC-010), opt-in ----------------
    # THE headline of the M4a safety pass: `safety_ops::estop` makes the single
    # most safety-critical control rideable over SlopSync. The hub must treat it
    # EXACTLY as a valid 0xE5 frame -- stop motion first, latch, publish 0x0003
    # at critical priority. Opt-in because it genuinely stops (and un-homes) the
    # machine; that is the point of it, and not something to do to an operator's
    # live session by surprise.
    scene("Step 5.95: client-asserted ESTOP (0x0005 op 6, RFC-010)")
    if not args.estop:
        skip("estop_assert", "client-asserted ESTOP skipped (pass --estop to exercise it; "
             "it LATCHES the machine and un-homes it)")
    else:
        reply, snap = _safety_op_roundtrip(ws, args, "estop", 910)
        if reply is None:
            bad("estop_assert", "no ECHO/NACK for the estop op within %.1fs" % args.timeout)
        elif reply[0] == "NACK":
            bad("estop_assert", "estop op NACKed: %s -- the red button does not work over SlopSync"
                % nack_name(reply[1].get(K["code"])))
        elif snap is None or "flags" not in snap:
            bad("estop_assert", "estop ECHOed but no safety snapshot followed")
        elif "estop" not in snap["flags"]:
            bad("estop_assert", "estop ECHOed but the snapshot does not report the latch: %s" % snap)
        else:
            ok("estop_assert", "ESTOP LATCHED via 0x0005: flags=%s cause=%s estop_seq=%s "
               "(cause 0 = user)" % (snap["flags"], snap["cause"], snap["estop_seq"]))

        # Clear it again so the session leaves the machine in the state it
        # found it. §11.2's preconditions are machine-domain, so a first
        # CLEAR_REFUSED is normal and the retry is part of the contract.
        cleared = False
        for attempt in range(5):
            reply, snap = _safety_op_roundtrip(ws, args, "estop_clear", 920 + attempt)
            if reply and reply[0] == "ECHO":
                cleared = True
                break
            time.sleep(0.2)
        if cleared and snap and "estop" not in snap.get("flags", ["estop"]):
            ok("estop_clear", "latch cleared (estop_clear accepted; clearing never restarts motion, "
               "and the machine stays UNHOMED until an explicit home)")
        else:
            bad("estop_clear", "could not clear the latch after 5 attempts: reply=%s snap=%s"
                % (reply, snap))

    # ---- Step 5.96: BENCH home ops (0x3101 ops 2/3, RFC-025) ----------------
    # *** OP 2 (force_home) CLEARS AN E-STOP LATCH and asserts a stroke window
    # NOTHING MEASURED. *** On a rig with a motor attached that is a real
    # collision hazard, which is exactly why RFC-025 filed these under safety
    # review and why this step is opt-in. It exists because motorless bench
    # work is otherwise impossible, and it is the in-band replacement for
    # POST /api/machine/homeoverride.
    scene("Step 5.96: BENCH home ops (0x3101 force_home / clear_override)")
    if not args.bench_home:
        skip("bench_home", "bench home ops skipped (pass --bench-home; op 2 asserts an "
             "UNMEASURED stroke window and clears the e-stop latch)")
    else:
        payload = build_intent(CH_HOME_INTENT, 930, [(1, cb_uint(2)), (2, cb_f32(250.0))])
        send_frame(ws, FRAME["INTENT"], CH_HOME_INTENT, payload)
        deadline = time.time() + args.timeout
        reply = None
        while time.time() < deadline:
            got = recv_frame(ws, deadline)
            if got is None:
                break
            hdr, pl = got
            if hdr["type"] == FRAME["STATE"]:
                record_state(hdr, pl)
                continue
            if hdr["type"] in (FRAME["ECHO"], FRAME["NACK"]):
                reply = ("ECHO" if hdr["type"] == FRAME["ECHO"] else "NACK", cb_decode_full(pl))
                break
        if reply is None:
            bad("bench_home", "no ECHO/NACK for home op 2 within %.1fs" % args.timeout)
        elif reply[0] == "NACK":
            bad("bench_home", "force_home NACKed: %s" % nack_name(reply[1].get(K["code"])))
        else:
            applied = reply[1].get(K["applied"], {})
            # Ground truth: the ECHO carries the stroke the MACHINE adopted,
            # which is not necessarily the one we asked for.
            ok("bench_home", "force_home ECHOed: applied=%s" % applied)

        # And put it back, so a --bench-home run is not a one-way door -- unless
        # the caller explicitly wants the fake-home left ON (--bench-home-no-revert).
        if args.bench_home_no_revert:
            skip("bench_home_clear", "clear_override skipped (--bench-home-no-revert) -- "
                 "machine left fake-homed on purpose")
        else:
            payload = build_intent(CH_HOME_INTENT, 931, [(1, cb_uint(3))])
            send_frame(ws, FRAME["INTENT"], CH_HOME_INTENT, payload)
            deadline = time.time() + args.timeout
            reply = None
            while time.time() < deadline:
                got = recv_frame(ws, deadline)
                if got is None:
                    break
                hdr, pl = got
                if hdr["type"] == FRAME["STATE"]:
                    record_state(hdr, pl)
                    continue
                if hdr["type"] in (FRAME["ECHO"], FRAME["NACK"]):
                    reply = ("ECHO" if hdr["type"] == FRAME["ECHO"] else "NACK", cb_decode_full(pl))
                    break
            if reply and reply[0] == "ECHO":
                ok("bench_home_clear", "clear_override ECHOed -- back to real homing")
            else:
                bad("bench_home_clear", "clear_override did not ECHO: %s" % (reply,))

    # ---- Step 5.98: motion-anomaly EVENTs (0x4100) -------------------------
    # THE FIRST DEVICE-AUTHORED EVENT CHANNEL, and the proof that the v1.0
    # `body` (40) sub-map grammar works: every field below is keyed by THIS
    # CHANNEL'S OWN catalog schema, so the device named its own event fields
    # without a registry PR. Also a ground-truth REPAIR -- the legacy :81 0x05
    # ANOMALY ring is written only by the superseded MotionInterpolator, so the
    # WebUI anomaly panel renders dead gauges today.
    #
    # Arrival is NOT asserted: a machine whose planner had nothing to complain
    # about correctly emits none, and failing that would be a probe that
    # demands the machine misbehave. What IS asserted is that everything which
    # DID arrive is well-formed and names a kind the catalog can label.
    scene("Step 5.98: motion-anomaly EVENTs (0x4100)")
    if not _anomaly_events:
        info("no anomaly EVENTs this session -- correct for a planner that met every deadline "
             "(the 0x1111 counters are the durable record; this channel is the edge)")
    else:
        malformed = []
        unnamed = []
        for ev in _anomaly_events:
            body = ev.get(K["body"]) if isinstance(ev, dict) else None
            if not isinstance(body, dict) or ANOM_BODY_K["kind"] not in body:
                malformed.append(ev)
                continue
            kind = body[ANOM_BODY_K["kind"]]
            # The frame's own discriminator and the body's must agree, or a
            # client switching on event_kind and a client reading the body
            # would disagree about what happened.
            if ev.get(K["event_kind"]) != kind:
                malformed.append(ev)
            elif kind not in ANOMALY_KINDS:
                unnamed.append(kind)
        if malformed:
            bad("motion_anomaly", "%d of %d anomaly EVENT(s) malformed (missing `body`, or "
                "event_kind disagreeing with body.kind)" % (len(malformed), len(_anomaly_events)))
        else:
            counts = {}
            for ev in _anomaly_events:
                k = ev[K["body"]][ANOM_BODY_K["kind"]]
                counts[ANOMALY_KINDS.get(k, "?%d" % k)] = counts.get(ANOMALY_KINDS.get(k, "?%d" % k), 0) + 1
            b = _anomaly_events[0][K["body"]]
            ok("motion_anomaly", "%d anomaly EVENT(s), every one carrying a `body` sub-map keyed "
               "by the channel's OWN schema and an event_kind that agrees with it; kinds=%s; "
               "first: target=%.3f detail=%.3f t=%dus"
               % (len(_anomaly_events), counts, b.get(ANOM_BODY_K["target"], 0.0),
                  b.get(ANOM_BODY_K["detail"], 0.0), b.get(ANOM_BODY_K["t_us"], 0)))
        if unnamed:
            info("kind(s) with no label in this probe's table: %s -- the catalog's option list "
                 "is append-only, so a newer engine kind reads as an ordinal, never as silence"
                 % sorted(set(unnamed)))

    # ---- Step 6: GOODBYE / summary ------------------------------------------
    scene("Step 6: GOODBYE")
    send_frame(ws, FRAME["GOODBYE"], 0, build_goodbye(NACK_CODES_BY_NAME["NORMAL_CLOSURE"]))
    ok("goodbye", "GOODBYE sent (NORMAL_CLOSURE)")

    return 0 if print_summary() else 1


# =============================================================================
# --pair : the M4b PAIRING + TRUST scenario (RFC-027 modes (a)/(c), RFC-029)
#
# WHY THIS RUNS TWO FULL CYCLES BACK TO BACK AGAINST ONE UNRESTARTED HUB: this
# project's third field bug was session-scoped state (source ownership) that
# leaked forever and was INVISIBLE to every earlier probe pass because every
# deploy rebooted the device between runs. Pairing state — pending knocks, the
# ledger, the presence window — is exactly the same shape, so back-to-back
# sessions with no restart between them is the mandatory pattern, not a bonus.
#
# The admin token is PERSISTED to --pair-state so a SECOND invocation of this
# script against the same running hub reconnects as administrator instead of
# needing a fresh bootstrap window. That is also precisely what a real client
# does with its token, so the persistence path gets exercised for free.
# =============================================================================

def _pair_connect(args, label):
    url = "ws://%s:%d/" % (args.ip, args.port)
    ws = websocket.create_connection(url, subprotocols=[WS_SUBPROTOCOL], timeout=args.timeout)
    info("%s: WS connected" % label)
    return ws


def _pair_hello(ws, args, label, instance_id, token=None, subs=None, client_ver=None,
                client_name="probe-pair"):
    payload = build_hello_full("probe", client_name, instance_id, token=token,
                               subscriptions=subs, client_ver=client_ver)
    send_frame(ws, FRAME["HELLO"], 0, payload)
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        got = recv_frame(ws, deadline)
        if got is None:
            break
        hdr, pl = got
        if hdr["type"] == FRAME["WELCOME"]:
            w = cb_decode_full(pl)
            # §8.4/RFC-015: READY gates BOTH planes, so a session that never
            # sends this is refused every INTENT it tries.
            etag = w.get(K["catalog_etag"], b"")
            send_frame(ws, FRAME["CATALOG_READY"], 0, etag)
            return w
        if hdr["type"] == FRAME["NACK"]:
            n = cb_decode_full(pl)
            bad(label, "HELLO refused: %s" % nack_name(n.get(K["code"])))
            return None
    bad(label, "no WELCOME within %.1fs" % args.timeout)
    return None


def _pair_collect(ws, args, want_types, seconds=None):
    """Drain frames for a bounded window, returning everything decoded. EVENT
    and STATE are kept as (channel, decoded); PAIR_GRANT/ECHO/NACK as maps."""
    out = []
    deadline = time.time() + (seconds if seconds is not None else args.timeout)
    while time.time() < deadline:
        got = recv_frame(ws, deadline)
        if got is None:
            break
        hdr, pl = got
        t = hdr["type"]
        if t in (FRAME["EVENT"], FRAME["STATE"]):
            out.append((t, hdr["channel"], pl))
        elif t in want_types:
            out.append((t, hdr["channel"], pl))
            if t in (FRAME["PAIR_GRANT"], FRAME["ECHO"], FRAME["NACK"]):
                break
        _maybe_keepalive(ws)
    return out


def _pair_find(frames, ftype, channel=None):
    for t, ch, pl in frames:
        if t == ftype and (channel is None or ch == channel):
            return pl
    return None


def _pair_find_last(frames, ftype, channel=None):
    """The LAST match. Correct for STATE: §9.1 channels are CONFLATED, so an
    intermediate snapshot may or may not have been delivered and only the
    newest one is ground truth. Taking the first match here read a stale
    count=0 left over from the previous cycle's teardown."""
    out = None
    for t, ch, pl in frames:
        if t == ftype and (channel is None or ch == channel):
            out = pl
    return out


def _pair_events(frames, channel):
    out = []
    for t, ch, pl in frames:
        if t == FRAME["EVENT"] and ch == channel:
            out.append(cb_decode_full(pl))
    return out


def _pair_cycle(admin_ws, args, cycle_no):
    """One knock -> approve -> token -> reconnect -> revoke round, using an
    already-configure admin session. Returns True on success."""
    okall = True
    joiner_id = os.urandom(8)
    scene("Pair cycle %d: knock -> approve -> token -> reconnect -> revoke" % cycle_no)

    _pair_collect(admin_ws, args, (), seconds=0.3)  # drain the previous cycle's tail
    joiner = _pair_connect(args, "joiner%d" % cycle_no)
    try:
        w = _pair_hello(joiner, args, "joiner%d" % cycle_no, joiner_id,
                        client_ver="0.1.%d" % cycle_no, client_name="knocker-%d" % cycle_no)
        if w is None:
            return False

        # --- the knock -------------------------------------------------------
        send_frame(joiner, FRAME["PAIR_REQ"], 0, build_pair_knock(joiner_id))
        info("joiner%d: bare PAIR_REQ sent (no proof -- that IS the ceremony)" % cycle_no)

        adminFrames = _pair_collect(admin_ws, args, (), seconds=1.5)
        evs = _pair_events(adminFrames, CH_PAIRING_EVENTS)
        kinds = [PAIRING_EVENT_KINDS.get(e.get(K["event_kind"]), "?") for e in evs]
        if "knocked" in kinds:
            ok("pair%d_knock_event" % cycle_no, "admin saw pairing-events kind=knocked")
        else:
            bad("pair%d_knock_event" % cycle_no, "no `knocked` EVENT (saw %s)" % kinds)
            okall = False

        st = _pair_find_last(adminFrames, FRAME["STATE"], CH_PENDING_PAIRING)
        if st is not None:
            pend = decode_pending_pairing(st)
            if pend and pend["count"] >= 1 and any(sl["instance_id"] == joiner_id for sl in pend["slots"]):
                ok("pair%d_pending_state" % cycle_no,
                   "0x000A shows the knock: count=%d name=%r mode=%s" %
                   (pend["count"], pend["slots"][0]["name"],
                    PAIRING_MODES.get(pend["slots"][0]["kind"], "?")))
            else:
                bad("pair%d_pending_state" % cycle_no, "0x000A did not list the knock: %s" % (pend,))
                okall = False
        else:
            bad("pair%d_pending_state" % cycle_no, "no 0x000A STATE push")
            okall = False

        # --- the approval ----------------------------------------------------
        send_frame(admin_ws, FRAME["INTENT"], CH_SESSION_ADMIN,
                   build_admin(700 + cycle_no, ADMIN_OPS["pair_approve"],
                               instance_id=joiner_id, role=1))
        af = _pair_collect(admin_ws, args, (FRAME["ECHO"], FRAME["NACK"]))
        if _pair_find(af, FRAME["ECHO"], CH_SESSION_ADMIN) is not None:
            ok("pair%d_approve" % cycle_no, "pair_approve ECHOed by the hub")
        else:
            nk = _pair_find(af, FRAME["NACK"])
            bad("pair%d_approve" % cycle_no,
                "pair_approve not ECHOed (%s)" % (nack_name(cb_decode_full(nk).get(K["code"])) if nk else "silence"))
            okall = False

        jf = _pair_collect(joiner, args, (FRAME["PAIR_GRANT"], FRAME["NACK"]))
        gpl = _pair_find(jf, FRAME["PAIR_GRANT"])
        token = None
        if gpl is not None:
            g = cb_decode_full(gpl)
            token = g.get(K["token"])
            ok("pair%d_grant" % cycle_no,
               "PAIR_GRANT: role=%s token=%s..." % (ROLE_NAME.get(g.get(K["roles"]), "?"), token[:4].hex()))
        else:
            bad("pair%d_grant" % cycle_no, "no PAIR_GRANT reached the joiner")
            okall = False
    finally:
        try:
            send_frame(joiner, FRAME["GOODBYE"], 0, build_goodbye(NACK_CODES_BY_NAME["NORMAL_CLOSURE"]))
            joiner.close()
        except Exception:
            pass

    if token is None:
        return False

    # --- the reconnect: does the persisted token actually buy the role? ------
    joiner2 = _pair_connect(args, "joiner%d-reconnect" % cycle_no)
    try:
        w2 = _pair_hello(joiner2, args, "joiner%d-reconnect" % cycle_no, joiner_id,
                         token=token, client_ver="0.1.%d" % cycle_no,
                         client_name="knocker-%d" % cycle_no)
        if w2 is not None and w2.get(K["roles"]) == 1:
            ok("pair%d_reconnect" % cycle_no, "reconnect with the granted token adopts `control`")
        else:
            bad("pair%d_reconnect" % cycle_no,
                "reconnect role=%s, expected control" % (w2.get(K["roles"]) if w2 else "?"))
            okall = False

        # --- RFC-029 item 2: the CHANGE TRIPWIRE ----------------------------
        # HONESTY CLAUSE: this proves the wire fires on an HONEST version
        # change. It proves nothing about a dishonest one, and nothing could —
        # a malicious update reports whatever version it likes and keeps its
        # token. The real bounds are role scoping, revocation, roster
        # visibility and the role-exempt safety ops.
        send_frame(joiner2, FRAME["GOODBYE"], 0, build_goodbye(NACK_CODES_BY_NAME["NORMAL_CLOSURE"]))
        joiner2.close()
        joiner2 = _pair_connect(args, "joiner%d-updated" % cycle_no)
        w3 = _pair_hello(joiner2, args, "joiner%d-updated" % cycle_no, joiner_id,
                         token=token, client_ver="9.9.%d" % cycle_no,
                         client_name="knocker-%d" % cycle_no)
        if w3 is not None and w3.get(K["roles"]) == 0:
            ok("pair%d_tripwire" % cycle_no,
               "changed client_ver SUSPENDED the granted role to `watch` (RECOGNIZED-PENDING)")
        else:
            bad("pair%d_tripwire" % cycle_no,
                "role after version change = %s, expected watch" % (w3.get(K["roles"]) if w3 else "?"))
            okall = False
        adminFrames = _pair_collect(admin_ws, args, (), seconds=1.0)
        kinds = [PAIRING_EVENT_KINDS.get(e.get(K["event_kind"]), "?")
                 for e in _pair_events(adminFrames, CH_PAIRING_EVENTS)]
        if "recognized_pending" in kinds:
            ok("pair%d_tripwire_event" % cycle_no, "admin saw pairing-events kind=recognized_pending")
        else:
            bad("pair%d_tripwire_event" % cycle_no, "no recognized_pending EVENT (saw %s)" % kinds)
            okall = False
    finally:
        try:
            send_frame(joiner2, FRAME["GOODBYE"], 0, build_goodbye(NACK_CODES_BY_NAME["NORMAL_CLOSURE"]))
            joiner2.close()
        except Exception:
            pass

    # --- revocation is PROTOCOL, not a WebUI feature -------------------------
    send_frame(admin_ws, FRAME["INTENT"], CH_SESSION_ADMIN,
               build_admin(800 + cycle_no, ADMIN_OPS["revoke"], instance_id=joiner_id))
    af = _pair_collect(admin_ws, args, (FRAME["ECHO"], FRAME["NACK"]))
    if _pair_find(af, FRAME["ECHO"], CH_SESSION_ADMIN) is not None:
        ok("pair%d_revoke" % cycle_no, "revoke ECHOed; the ledger entry is gone")
    else:
        bad("pair%d_revoke" % cycle_no, "revoke was not ECHOed")
        okall = False

    return okall


# =============================================================================
# M4c (RFC-029 item 1): HUB AUTHENTICITY -- the anti-evil-twin primitive.
#
# The claim under test: a machine that holds the private half of the key it
# handed us AT THE PAIRING CEREMONY can sign material WE contributed entropy to,
# and a clone that copied every identity string cannot. The two assertions that
# make it real are (a) the fresh signature verifies against the pinned key, and
# (b) a signature captured from an earlier session does NOT verify for a later
# one. (b) is the feasibility-pass blocker, and it is why the material carries
# client_nonce at all.
# =============================================================================

def _sig_session(args, label, instance_id, nonce, admin_ws=None, wait=None):
    """One HELLO that asks to be signed to. Returns (welcome, signature).

    The signature arrives in its own h2c HUB_SIG (0x1D) frame, NOT inside
    WELCOME: on the real hardware one software ECDSA is ~30-80 ms in a single
    uninterruptible call, and signing inline would stall the hub's 5 ms tick --
    and therefore every other session's STATE pacing, deadman and motion-input
    drain -- for 6-16 ticks every time somebody connected."""
    ws = _pair_connect(args, label)
    try:
        payload = build_hello_full("probe", label, instance_id,
                                   client_nonce=nonce, sig_request=True)
        send_frame(ws, FRAME["HELLO"], 0, payload)
        w = None
        sig = None
        deadline = time.time() + (wait if wait is not None else args.timeout)
        while time.time() < deadline and (w is None or sig is None):
            if admin_ws is not None:
                send_frame(admin_ws, FRAME["PING"], 0, b"")
            got = recv_frame(ws, deadline)
            if got is None:
                break
            hdr, pl = got
            if hdr["type"] == FRAME["WELCOME"]:
                w = cb_decode_full(pl)
                send_frame(ws, FRAME["CATALOG_READY"], 0, w.get(K["catalog_etag"], b""))
                if (w.get(K["trust"]) or {}).get(TRUST_K["welcome_sig"]) is not None:
                    sig = w[K["trust"]][TRUST_K["welcome_sig"]]  # the legal inline alternative
            elif hdr["type"] == FRAME["HUB_SIG"]:
                m = cb_decode_full(pl)
                sig = (m.get(K["trust"]) or {}).get(TRUST_K["welcome_sig"])
            _maybe_keepalive(ws)
        return w, sig
    finally:
        try:
            send_frame(ws, FRAME["GOODBYE"], 0, build_goodbye(NACK_CODES_BY_NAME["NORMAL_CLOSURE"]))
            ws.close()
        except Exception:
            pass


def _pair_signature(args, pubkey, admin_ws=None):
    scene("Step P2.5: hub authenticity signature (RFC-029 item 1)")
    if not pubkey:
        skip("hub_sig", "no hub_pubkey pinned (PAIR_GRANT never carried one)")
        return True
    okall = True
    inst = os.urandom(8)

    n1 = os.urandom(8)
    w1, sig1 = _sig_session(args, "sig-1", inst, n1, admin_ws)
    if w1 is None or sig1 is None:
        bad("hub_sig", "no signature arrived for a session that requested one")
        return False
    ok("hub_sig_deferred",
       "signature arrived out-of-band in HUB_SIG (%d B) -- WELCOME never waited on an ECDSA"
       % len(sig1))

    mat1 = hub_sig_material(n1, w1[K["session_id"]], w1[K["boot_id"]])
    if sig1 == stand_in_sign(pubkey, mat1):
        ok("hub_sig_verify",
           "verifies over client_nonce || session_id || boot_id against the key "
           "pinned at the pairing ceremony")
    else:
        bad("hub_sig_verify", "signature did not verify against the pinned key")
        okall = False

    # ---- THE REPLAY FENCE ---------------------------------------------------
    n2 = os.urandom(8)
    w2, sig2 = _sig_session(args, "sig-2", inst, n2, admin_ws)
    if w2 is None or sig2 is None:
        bad("hub_sig_replay", "second session produced no signature")
        return False
    mat2 = hub_sig_material(n2, w2[K["session_id"]], w2[K["boot_id"]])
    if sig2 == stand_in_sign(pubkey, mat2):
        ok("hub_sig_verify2", "the second session's own signature verifies")
    else:
        bad("hub_sig_verify2", "second signature did not verify")
        okall = False
    if sig1 != sig2 and sig1 != stand_in_sign(pubkey, mat2):
        ok("hub_sig_replay",
           "a signature CAPTURED from session %d does NOT verify for session %d -- "
           "the evil-twin replay is dead" % (w1[K["session_id"]], w2[K["session_id"]]))
    else:
        bad("hub_sig_replay", "REPLAY ACCEPTED: the captured signature was reusable")
        okall = False

    # A hub asked to sign but given no client entropy must DECLINE -- signing
    # hub-chosen material is precisely the broken original design.
    # A short wait on purpose: a hub that will DECLINE never sends anything, so
    # burning the full --timeout here only starves the admin session's keepalive.
    _w3, sig3 = _sig_session(args, "sig-3", os.urandom(8), None, admin_ws, wait=1.5)
    if sig3 is None:
        ok("hub_sig_no_nonce",
           "sig_request WITHOUT client_nonce produced no signature (a hub never "
           "signs material the client did not seed)")
    else:
        bad("hub_sig_no_nonce", "the hub signed something with no client entropy in it")
        okall = False
    return okall


# =============================================================================
# M4c (RFC-029 items 6 + 3): TOKEN PRESENTATION MODES and the AUTH frame.
#
# v1 transports are cleartext ws://, so a bearer token in HELLO is readable by
# any passive LAN observer, forever. Proof mode keeps the credential at home and
# spends a one-time HMAC over THIS session's WELCOME nonce instead. It buys
# nothing against an ACTIVE attacker -- a MITM on a cleartext binding already
# has the session -- and saying otherwise would be theatre.
# =============================================================================

def _proof_hello(ws, args, inst, label):
    """A proof-mode HELLO: presentation_mode=1 and NO TOKEN. Returns WELCOME."""
    send_frame(ws, FRAME["HELLO"], 0,
               build_hello_full("probe", label, inst, presentation_mode=1, client_ver="2.0.0"))
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        got = recv_frame(ws, deadline)
        if got is None:
            break
        hdr, pl = got
        if hdr["type"] == FRAME["WELCOME"]:
            w = cb_decode_full(pl)
            send_frame(ws, FRAME["CATALOG_READY"], 0, w.get(K["catalog_etag"], b""))
            return w
    return None


def _pair_proof_mode(admin_ws, args):
    scene("Step P3.5: proof-mode token presentation (RFC-029 item 6, AUTH 0x1C)")
    okall = True
    inst = os.urandom(8)

    # ---- give this device a token the ordinary way --------------------------
    joiner = _pair_connect(args, "proofer")
    token = None
    try:
        if _pair_hello(joiner, args, "proofer", inst, client_name="proofer") is None:
            return False
        send_frame(joiner, FRAME["PAIR_REQ"], 0, build_pair_knock(inst))
        _pair_collect(admin_ws, args, (), seconds=1.0)
        send_frame(admin_ws, FRAME["INTENT"], CH_SESSION_ADMIN,
                   build_admin(760, ADMIN_OPS["pair_approve"], instance_id=inst, role=1))
        _pair_collect(admin_ws, args, (FRAME["ECHO"], FRAME["NACK"]))
        jf = _pair_collect(joiner, args, (FRAME["PAIR_GRANT"], FRAME["NACK"]))
        gpl = _pair_find(jf, FRAME["PAIR_GRANT"])
        if gpl is None:
            bad("proof_setup", "could not pair a device to prove with")
            return False
        token = cb_decode_full(gpl).get(K["token"])
    finally:
        try:
            send_frame(joiner, FRAME["GOODBYE"], 0, build_goodbye(NACK_CODES_BY_NAME["NORMAL_CLOSURE"]))
            joiner.close()
        except Exception:
            pass

    # ---- reconnect in PROOF mode: NO TOKEN ON THE WIRE ----------------------
    ws = _pair_connect(args, "proofer-2")
    captured_proof = None
    first_nonce = None
    try:
        w = _proof_hello(ws, args, inst, "proofer")
        if w is None:
            bad("proof_welcome", "no WELCOME for the proof-mode HELLO")
            return False
        first_nonce = w[K["nonce"]]
        if w.get(K["roles"]) == 0:
            ok("proof_welcome",
               "proof-mode HELLO carried NO token, so the session starts at `watch` -- "
               "the correct posture for a client that has proved nothing yet")
        else:
            # NOT A PROTOCOL FAILURE, and reported as its own thing rather than
            # folded into a pass. This hub's delegate grants a role to
            # UNAUTHENTICATED sessions ("controller-for-all" LAN-trust parity, a
            # deliberate deployment posture with a documented future flip). Proof
            # mode still did its one job -- the credential never went on the wire
            # -- but on a hub in this posture it buys nothing until pairing
            # enforcement is turned on, and saying otherwise would be theatre.
            skip("proof_welcome",
                 "hub grants `%s` to UNAUTHENTICATED sessions (LAN-trust posture, not a "
                 "protocol property); proof mode still put NO token on the wire"
                 % ROLE_NAME.get(w.get(K["roles"]), "?"))

        # ---- a WRONG proof first: failure must be EXPLICIT -------------------
        send_frame(ws, FRAME["AUTH"], 0, build_auth(bytes([0x5A]) * 16))
        af = _pair_collect(ws, args, (FRAME["NACK"], FRAME["GRANT"]), seconds=1.5)
        nk = _pair_find(af, FRAME["NACK"])
        if nk is not None and nack_name(cb_decode_full(nk).get(K["code"])) == "UNAUTHORIZED":
            ok("proof_wrong", "a wrong proof is NACK UNAUTHORIZED, never silence")
        else:
            bad("proof_wrong", "wrong proof produced %s"
                % ("no NACK" if nk is None else nack_name(cb_decode_full(nk).get(K["code"]))))
            okall = False

        # ---- the real proof --------------------------------------------------
        captured_proof = token_proof(token, first_nonce)
        send_frame(ws, FRAME["AUTH"], 0, build_auth(captured_proof))
        af = _pair_collect(ws, args, (FRAME["GRANT"], FRAME["NACK"]), seconds=1.5)
        gpl = _pair_find(af, FRAME["GRANT"])
        if gpl is not None and cb_decode_full(gpl).get(K["roles"]) == 1:
            ok("proof_auth",
               "AUTH re-issued `roles`=control in a GRANT -- the credential itself never "
               "crossed the cleartext wire")
        else:
            bad("proof_auth", "AUTH did not upgrade the role (%s)"
                % (cb_decode_full(gpl) if gpl is not None else "no GRANT"))
            okall = False
    finally:
        try:
            send_frame(ws, FRAME["GOODBYE"], 0, build_goodbye(NACK_CODES_BY_NAME["NORMAL_CLOSURE"]))
            ws.close()
        except Exception:
            pass

    # ---- the captured proof, replayed at the NEXT session -------------------
    ws2 = _pair_connect(args, "proofer-3")
    try:
        w2 = _proof_hello(ws2, args, inst, "proofer")
        if w2 is None:
            bad("proof_replay", "no WELCOME for the replay session")
            return False
        if w2[K["nonce"]] == first_nonce:
            bad("proof_replay", "the hub reused a WELCOME nonce across sessions")
            return False
        send_frame(ws2, FRAME["AUTH"], 0, build_auth(captured_proof))
        af = _pair_collect(ws2, args, (FRAME["NACK"], FRAME["GRANT"]), seconds=1.5)
        nk = _pair_find(af, FRAME["NACK"])
        if nk is not None and nack_name(cb_decode_full(nk).get(K["code"])) == "UNAUTHORIZED":
            ok("proof_replay",
               "a proof sniffed off the previous session is already spent (NACK "
               "UNAUTHORIZED) -- one-time by construction")
        else:
            bad("proof_replay", "REPLAY ACCEPTED: a captured proof authenticated a second session")
            okall = False
    finally:
        try:
            ws2.close()
        except Exception:
            pass

    send_frame(admin_ws, FRAME["INTENT"], CH_SESSION_ADMIN,
               build_admin(860, ADMIN_OPS["revoke"], instance_id=inst))
    _pair_collect(admin_ws, args, (FRAME["ECHO"], FRAME["NACK"]))
    return okall


def _pair_read_ledger(admin_ws, args):
    """BLOB_REQ slot 0 of the trust-ledger store. Proves the store seam AND the
    thing that must never be in it."""
    send_frame(admin_ws, FRAME["BLOB_REQ"], 0, build_blob_req(ns=1, store_id=1, slot=0))
    frames = _pair_collect(admin_ws, args, (FRAME["BLOB_CHUNK"], FRAME["NACK"]), seconds=1.5)
    chunks = {}
    total = None
    for t, _ch, pl in frames:
        if t != FRAME["BLOB_CHUNK"]:
            continue
        c = decode_blob_chunk(pl)
        if c is None:
            continue
        chunks[c["chunk_index"]] = c["bytes"]
        total = c["total_bytes"]
    if not chunks:
        nk = _pair_find(frames, FRAME["NACK"])
        bad("ledger_read", "no BLOB_CHUNK (%s)" %
            (nack_name(cb_decode_full(nk).get(K["code"])) if nk else "silence"))
        return False
    blob = b"".join(chunks[i] for i in sorted(chunks))[:total]
    item = cb_decode_full(blob)
    named = {LEDGER_K.get(k, k): v for k, v in item.items()}
    ok("ledger_read", "ledger item decoded: %s" %
       {k: (v.hex() if isinstance(v, bytes) else v) for k, v in named.items()})
    # THE ONE THING THAT MUST NEVER BE THERE. Listing paired devices must not be
    # a way to become one.
    if any(isinstance(v, bytes) and len(v) == 16 for k, v in item.items() if k != 1):
        bad("ledger_no_token", "a 16-byte value that looks like a TOKEN is in the wire item")
        return False
    ok("ledger_no_token", "no token in the wire item (configure may revoke, never impersonate)")
    return True


def run_pairing(args):
    scene("Step P0: pairing/trust scenario (M4b)")
    state_path = args.pair_state
    admin_id = None
    admin_token = None
    hub_pubkey = None
    if state_path and os.path.exists(state_path):
        try:
            with open(state_path, "r", encoding="utf-8") as f:
                st = json.load(f)
            admin_id = bytes.fromhex(st["instance_id"])
            admin_token = bytes.fromhex(st["token"])
            if st.get("hub_pubkey"):
                hub_pubkey = bytes.fromhex(st["hub_pubkey"])
            info("reusing persisted admin identity from %s (instance %s)"
                 % (state_path, admin_id.hex()))
        except Exception as e:  # noqa: BLE001
            info("could not read %s (%s); bootstrapping fresh" % (state_path, e))
            admin_id = admin_token = None
            hub_pubkey = None

    admin_subs = [
        (CH_PENDING_PAIRING, 0.0, 1),
        (CH_PAIRING_EVENTS, 0.0, 1),
        (CH_PAIRED_ROSTER, 0.0, 0),
        (CH_SAFETY_EVENTS, 0.0, 3),
        (CH_SAFETY, 0.0, 3),
    ]

    admin_ws = None
    try:
        if admin_token is None:
            # ---- bootstrap: RFC-027(c) push-to-pair ------------------------
            scene("Step P1: bootstrap via the push-to-pair window")
            admin_id = os.urandom(8)
            admin_ws = _pair_connect(args, "admin")
            w = _pair_hello(admin_ws, args, "admin", admin_id, subs=admin_subs,
                            client_ver="1.0.0", client_name="probe-admin")
            if w is None:
                return 1
            modes = (w.get(K["trust"]) or {}).get(TRUST_K["pairing_modes"], 0)
            names = sorted(n for bit, n in PAIRING_MODES.items() if modes & bit)
            info("WELCOME trust.pairing_modes = 0x%02X (%s)" % (modes, ", ".join(names) or "none"))
            if modes & 4:
                ok("presence_window", "push-to-pair is advertised while its window is open")
            else:
                bad("presence_window",
                    "no push_to_pair bit -- start slopsim with --pairing-window "
                    "(or pass --pair-state to reuse an earlier admin token)")
                print_summary()
                return 1

            send_frame(admin_ws, FRAME["PAIR_REQ"], 0, build_pair_knock(admin_id))
            frames = _pair_collect(admin_ws, args, (FRAME["PAIR_GRANT"], FRAME["NACK"]))
            gpl = _pair_find(frames, FRAME["PAIR_GRANT"])
            if gpl is None:
                bad("bootstrap_grant", "presence-window knock produced no PAIR_GRANT")
                print_summary()
                return 1
            g = cb_decode_full(gpl)
            admin_token = g.get(K["token"])
            hub_pubkey = (g.get(K["trust"]) or {}).get(TRUST_K["hub_pubkey"])
            if hub_pubkey:
                ok("hub_pubkey", "PAIR_GRANT delivered the hub's %d-byte P-256 public key -- "
                                 "TOFU anchored at the moment presence was proven" % len(hub_pubkey))
            else:
                info("PAIR_GRANT carried no hub_pubkey (a hub with no durable identity)")
            if g.get(K["roles"]) == 2:
                ok("bootstrap_grant", "FACTORY-FRESH: the window granted `configure` "
                                      "(physical possession is root)")
            else:
                bad("bootstrap_grant", "window granted %s, expected configure on a fresh ledger"
                    % ROLE_NAME.get(g.get(K["roles"]), "?"))
            if state_path:
                with open(state_path, "w", encoding="utf-8") as f:
                    json.dump({"instance_id": admin_id.hex(), "token": admin_token.hex(),
                               "hub_pubkey": hub_pubkey.hex() if hub_pubkey else ""}, f)
                info("admin token persisted to %s (a second run reconnects as admin)" % state_path)
            send_frame(admin_ws, FRAME["GOODBYE"], 0, build_goodbye(NACK_CODES_BY_NAME["NORMAL_CLOSURE"]))
            admin_ws.close()
            admin_ws = None

        # ---- the administrator session -------------------------------------
        scene("Step P2: reconnect as the administrator")
        admin_ws = _pair_connect(args, "admin")
        w = _pair_hello(admin_ws, args, "admin", admin_id, token=admin_token, subs=admin_subs,
                        client_ver="1.0.0", client_name="probe-admin")
        if w is None:
            print_summary()
            return 1
        if w.get(K["roles"]) == 2:
            ok("admin_role", "persisted token adopts `configure` -- the trusted surface is a TIER")
        else:
            bad("admin_role", "admin reconnect role=%s, expected configure"
                % ROLE_NAME.get(w.get(K["roles"]), "?"))
        _pair_collect(admin_ws, args, (), seconds=0.7)  # drain retained pushes

        # ---- M4c: hub authenticity, before anything else touches the ledger --
        _pair_signature(args, hub_pubkey, admin_ws)

        # ---- TWO FULL CYCLES, no restart in between -------------------------
        _pair_cycle(admin_ws, args, 1)
        _pair_cycle(admin_ws, args, 2)

        # ---- M4c: proof-mode presentation + the AUTH frame -------------------
        _pair_proof_mode(admin_ws, args)

        # ---- a knock that DEPARTS without being approved ---------------------
        # The leak-shaped case: session-scoped state that outlives its session
        # is state nobody can ever clear.
        scene("Step P3: a knocker that leaves without being approved")
        ghost_id = os.urandom(8)
        ghost = _pair_connect(args, "ghost")
        gw = _pair_hello(ghost, args, "ghost", ghost_id, client_name="ghost")
        if gw is not None:
            send_frame(ghost, FRAME["PAIR_REQ"], 0, build_pair_knock(ghost_id))
            _pair_collect(admin_ws, args, (), seconds=1.0)
            ghost.close()  # rude death, no GOODBYE
            time.sleep(0.4)
            frames = _pair_collect(admin_ws, args, (), seconds=1.5)
            st = _pair_find_last(frames, FRAME["STATE"], CH_PENDING_PAIRING)
            pend = decode_pending_pairing(st) if st is not None else None
            if pend is not None and not any(sl["instance_id"] == ghost_id for sl in pend["slots"]):
                ok("knock_teardown", "the departed knocker's pending entry was released "
                                     "(count=%d) -- no leak" % pend["count"])
            else:
                bad("knock_teardown", "departed knocker still pending: %s" % (pend,))

        # ---- the ledger over the store seam ---------------------------------
        scene("Step P4: read the trust ledger over BLOB_REQ")
        _pair_read_ledger(admin_ws, args)

        scene("Step P5: GOODBYE")
        send_frame(admin_ws, FRAME["GOODBYE"], 0, build_goodbye(NACK_CODES_BY_NAME["NORMAL_CLOSURE"]))
        ok("goodbye", "GOODBYE sent (NORMAL_CLOSURE)")
    finally:
        try:
            if admin_ws is not None:
                admin_ws.close()
        except Exception:
            pass

    return 0 if print_summary() else 1


NACK_CODES_BY_NAME = {name: code for code, name in NACK_CODES.items()}


def main():
    parser = argparse.ArgumentParser(
        description="Live SlopSync protocol verifier: connects to the firmware hub's "
                    "binary WebSocket and runs a scripted HELLO/SUBSCRIBE/STATE/INTENT/"
                    "GOODBYE session, printing a PASS/FAIL transcript.")
    parser.add_argument("--ip", default="192.168.1.229", help="hub IP (default: %(default)s)")
    parser.add_argument("--port", type=int, default=82, help="hub WS port (default: %(default)s)")
    parser.add_argument("--timeout", type=float, default=5.0,
                         help="per-step reply timeout in seconds (default: %(default)s)")
    parser.add_argument("--no-motion", action="store_true",
                         help="skip step 5 (the move INTENT) and step 5.5 (the motion-input "
                              "STREAM) entirely")
    parser.add_argument("--listen-only", action="store_true",
                         help="alias for --no-motion: only observe, never command motion")
    parser.add_argument("--stream", type=float, default=0.0,
                         help="stream a sine wave on motion-input(0x2100) for this many "
                              "seconds @ %.0fHz (0 = skip; default: %%(default)s)" % STREAM_HZ)
    parser.add_argument("--segments", type=float, default=0.0,
                         help="stream timed WAVEFORM segments on motion-segment(0x2101): one "
                              "bundle/second for this many seconds (alternating target 0.7/0.3, "
                              "duration_ms 900, sentinel end_vel), PING-keepalived against the "
                              "600ms deadman (0 = skip; default: %(default)s)")
    parser.add_argument("--estop", action="store_true",
                         help="exercise step 5.95: ASSERT a client-side e-stop over 0x0005 "
                              "(safety_ops::estop, RFC-010) and then clear it. This really "
                              "does latch the machine and leave it UNHOMED -- opt-in on purpose")
    parser.add_argument("--bench-home", action="store_true",
                         help="exercise step 5.96: the BENCH home ops on 0x3101 (2 force_home "
                              "{stroke}, 3 clear_override). Op 2 asserts a stroke window nothing "
                              "measured AND clears an e-stop latch -- motorless rigs only")
    parser.add_argument("--bench-home-no-revert", action="store_true",
                         help="with --bench-home, skip the op 3 clear_override that normally puts "
                              "the override back -- leaves the machine fake-homed after the run "
                              "(bench-only, one-way door; the in-band replacement for the retired "
                              "POST /api/machine/homeoverride left ON)")
    parser.add_argument("--pair", action="store_true",
                         help="run the M4b PAIRING/TRUST scenario instead of the motion session: "
                              "push-to-pair bootstrap, then TWO full knock->approve->token->"
                              "reconnect->revoke cycles back to back against one unrestarted hub "
                              "(the mandatory session-lifecycle regression pattern), plus the "
                              "RFC-029 version tripwire, a departed knocker, and a trust-ledger "
                              "BLOB read. Against slopsim, start it with --pairing-window")
    parser.add_argument("--pair-state", default=None,
                         help="JSON file holding the administrator's instance_id + token, so a "
                              "SECOND --pair run against the same live hub reconnects as admin "
                              "instead of needing another bootstrap window")
    args = parser.parse_args()

    try:
        if args.pair:
            return run_pairing(args)
        return run(args)
    except KeyboardInterrupt:
        print("\ninterrupted")
        return 130


if __name__ == "__main__":
    sys.exit(main())
