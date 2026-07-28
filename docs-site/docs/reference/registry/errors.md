---
title: NACK codes
description: Generated table of every SlopSync NACK and GOODBYE reason code, grouped by range.
register: IEEE
generated: true
---

<!-- ==========================================================
     GENERATED FILE. DO NOT EDIT.
     Source of truth: docs/slopsync/registry/registry.yaml
     Generator:       docs-site/tools/gen_docs_tables.py
     Regenerate:      python docs-site/tools/gen_docs_tables.py
     CI gate:         python docs-site/tools/gen_docs_tables.py --check
     Hand edits are overwritten and fail the docs build.
     ========================================================== -->

# NACK and GOODBYE codes

There is **one** code space, not two. A GOODBYE `code` is drawn from this
same table.

A receiver that meets an unknown code treats it as the generic code of
its range, taken from the high byte. That fallback is why a second,
overlapping space was rejected. With two spaces, the range of an
unknown code is ambiguous.

## `0x00xx`: protocol

The frame itself is unusable.

| Code | Name | Meaning |
|---|---|---|
| `0x0000` | `MALFORMED` | undecodable frame/CBOR |
| `0x0001` | `UNSUPPORTED_VERSION` | HELLO proto_ver not servable |
| `0x0002` | `FRAME_TOO_LARGE` | exceeds negotiated max_frame |
| `0x0003` | `PROFILE_VIOLATION` | CBOR not in deterministic profile |

## `0x01xx`: session and authorization

The session cannot proceed as asked.

| Code | Name | Meaning |
|---|---|---|
| `0x0100` | `BUSY` | client limit reached; carries retry_after_ms |
| `0x0101` | `UNAUTHORIZED` | token invalid/revoked |
| `0x0102` | `NOT_CONTROLLER` | control op without controller role |
| `0x0103` | `PAIRING_REQUIRED` | controller requested, no token, pairing window closed |
| `0x0104` | `PAIRING_DENIED` | bad pin_proof or pairing window closed |
| `0x0105` | `SESSION_EVICTED` | slow-consumer or admin kick (GOODBYE code) |
| `0x0106` | `DUPLICATE_INSTANCE` | instance_id already in live session; old session evicted instead: see §6.8 |
| `0x0107` | `NORMAL_CLOSURE` | clean voluntary teardown (GOODBYE code, either direction): not an error |
| `0x0108` | `DEADMAN_TIMEOUT` | hub-initiated session teardown: silence exceeded the deadman window (§11.3, GOODBYE code) |
| `0x0109` | `REBOOTING` | hub is committing a change by rebooting and is closing every session first (RFC-020/022.2, GOODBYE code). Preceded by an ECHO carrying reboot_in_ms; on return the changed boot_id tells clients what happened. |
| `0x010A` | `READY_TIMEOUT` | session never sent CATALOG_READY within catalog_ready_timeout_ms (RFC-015, GOODBYE code). Needed because liveness reaping NEVER fires on a client that PINGs happily but never finishes adopting the catalog: it would hold a slot forever with both planes gated shut. |
| `0x010B` | `NOT_READY` | frame refused because the session has not sent CATALOG_READY yet (RFC-015). READY gates BOTH planes: pre-READY INTENTs are NACK'd, not queued, because a client acting before it has adopted the retained safety latch breaks §11.5(2). |
| `0x010C` | `IDLE_REAPED` | RFC-039.4: hub-initiated teardown of a NON-OWNING session that fell silent past idle_reap_multiplier x ping_interval_idle_ms (RFC-024, GOODBYE code). Distinct from DEADMAN_TIMEOUT on purpose: reaping a dark viewer is housekeeping with zero motion consequence, and before this code existed it was reported with the motion-safety code: a reaped dashboard read as a deadman event in every log and client. RFC-042: silence no longer reaches this code directly; it marks a session STALE instead (session_event_kinds.4), so the reference hub no longer emits DEADMAN_TIMEOUT or IDLE_REAPED for silence; both stay registered for a hub/policy combination that still wants to terminate outright. |
| `0x010D` | `SLOT_RECLAIMED` | RFC-042: a HELLO that would otherwise NACK BUSY instead evicted a STALE session to make room (lowest access tier first, tie-break longest continuously stale): best-effort GOODBYE code, since the reclaimed session was stale for a reason and may never receive it. Distinguishable from SESSION_EVICTED (admin/slow-consumer) and from DEADMAN_TIMEOUT/IDLE_REAPED (which no longer fire for silence at all). |

## `0x02xx`: subscription and QoS

The subscription request is refused.

| Code | Name | Meaning |
|---|---|---|
| `0x0200` | `UNKNOWN_CHANNEL` | channel id not in catalog |
| `0x0201` | `ACCESS_DENIED` | channel access level above session role |
| `0x0202` | `CLASS_MISMATCH` | e.g. SUBSCRIBE to an INTENT channel |
| `0x0203` | `SUB_LIMIT` | per-session subscription cap reached |
| `0x0204` | `SUBSCRIBE_REJECTED` | RFC-033.2: the SUBSCRIBE frame as a WHOLE could not be processed (undecodable, or more wishes than max_subscriptions_per_frame): as opposed to the per-channel codes above, which reject one wish and grant the rest. `detail` carries the reason. Exists because the alternative was observed silence: a dropped SUBSCRIBE leaves a healthy-looking LIVE session with zero STATE, which presents as a client rendering bug and cost two debugging nights. |

## `0x03xx`: intent

The intent is refused on its own merits.

| Code | Name | Meaning |
|---|---|---|
| `0x0300` | `CONFLICT` | precondition (cfg_gen CAS) failed |
| `0x0301` | `RATE_LIMITED` | ingress intent rate exceeded |
| `0x0302` | `INVALID_VALUE` | outside schema min/max or wrong type; also a store import whose kind or size the hub refuses (RFC-021.5) |
| `0x0303` | `UNSUPPORTED_OP` | intent op not implemented on this hub |

## `0x04xx`: safety refusal

The machine refuses on safety grounds. A client SHOULD render these distinctly.

| Code | Name | Meaning |
|---|---|---|
| `0x0400` | `ESTOP_ACTIVE` | refused while e-stop latched |
| `0x0401` | `NOT_HOMED` | motion intent before homing |
| `0x0402` | `INTERLOCK` | hub-specific safety interlock |
| `0x0403` | `SOURCE_CONFLICT` | another session owns this arbiter source |
| `0x0404` | `TAKEOVER_REQUIRED` | control exists; retry with takeover flag |
| `0x0405` | `CLEAR_REFUSED` | e-stop clear conditions not met (§11.2) |

## `0x05xx`: transfer

A chunked transfer failed.

| Code | Name | Meaning |
|---|---|---|
| `0x0500` | `CHUNK_UNAVAILABLE` | blob chunk index out of range, or a store/slot that does not exist WITHIN a registered namespace (generalized from 'catalog chunk' by RFC-021; the catalog is now namespace 0). An unregistered NAMESPACE itself is the more specific INVALID_NAMESPACE (0x0504, RFC-049e): this code no longer covers that case. |
| `0x0501` | `REASSEMBLY_TIMEOUT` | fragment reassembly abandoned (5 s) |
| `0x0502` | `ETAG_MISMATCH` | static-profile client etag != hub catalog etag |
| `0x0503` | `BLOB_REFUSED` | RFC-039.2: a RECEIVER refusing a declared blob (total_bytes over its reassembly budget), sent as a GOODBYE code by the client rather than idling in a half-session. The observed failure: a client's DoS-guard cap silently refused a grown catalog's transfer header and the session went LIVE WITH NO CATALOG: no error anywhere, every STATE frame undecodable, READY_TIMEOUT eventually killing it 15 s later and blaming the client. Refusal is legal; SILENT refusal is not. |
| `0x0504` | `INVALID_NAMESPACE` | RFC-049e: a BLOB_REQ naming a `blob.ns` value not in the registered `blob_namespaces` table (0 catalog / 1 store; 128-255 device-defined are legal too: only a value truly outside every registered/device range trips this). Split out of CHUNK_UNAVAILABLE so a client can tell 'namespace does not exist' from 'namespace exists, item does not' (§18-8's panel-flagged gap) instead of receiving the same code for both. Same range as its siblings because it is a transfer-request refusal, not a new error class. Implementation: Phase D. |

