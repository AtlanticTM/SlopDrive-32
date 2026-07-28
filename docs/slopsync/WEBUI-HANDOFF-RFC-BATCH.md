# WebUI handoff — RFC-030..040 + 016(a) batch (2026-07-27)

*For the agent holding the webui rebuild context. The firmware/library/registry
side of this batch is LANDED (see RFC-QUEUE.md statuses + registry.yaml). This
file lists exactly what the client can now rely on and what it should change.
Delete this file once absorbed.*

## What the wire now guarantees (no client action required, but stop working around it)

1. **RFC-033 — SUBSCRIBE is never silently dropped.** A SUBSCRIBE the hub
   cannot process answers `NACK SUBSCRIBE_REJECTED (0x0204)` with the reason in
   `detail`. The per-frame wish cap is now ADVERTISED: WELCOME `limits` sub-map
   key **4** = `max_subscriptions_per_frame` (16 on this hub).
   - **Change:** replace the fixed batch-of-8 workaround with batches sized
     from limits key 4 (fall back to 8 only when key 4 is absent — a pre-batch
     hub). Handle 0x0204 as a real error surface (it means a client bug, show
     it loudly in the link diagnostics).
   - **Ruling recorded in SPEC:** mixing STATE and EVENT wishes in ONE
     SUBSCRIBE is LEGAL and always was — both observed "mixed frame" drops
     were actually the undeclared 16-wish cap. The split-by-class workaround
     can go; batching by count is the only constraint.

2. **RFC-032 — the move/target roles exist and the device advertises them.**
   `position` on `0x3100 move` now carries role `command.position`;
   `tgt_10um` on `0x1100 motion` carries `telemetry.target`.
   - `model/settings.js` already indexes non-action roles into `byRole`, so
     the rail tap-to-move tape, the `commanded` hero numeral, and `lag`
     (= target − position, computed client-side, per the RFC) should light up
     with **zero code** — verify against the live device and delete the
     "this catalog does not tag a move INTENT by role" disabled-state copy
     path if it renders anything stale.

3. **RFC-035 — `plan.*` roles.** 0x1101 plan-strip fields now carry
   `plan.start/end/current/velocity/elapsed/duration/style`.
   - **Change:** PlanStrip should bind BY ROLE first and keep the documented
     `/plan/i` name heuristic only as a fallback for role-less hubs.

4. **RFC-016(a) — WELCOME `identity` (key 37) is live.** Sub-map:
   1=`product` ("slopdrive-32"), 2=`fw_version` (e.g. "2.1.75"), 3=`hub_name`
   (empty for now). Show fw_version in the link/about surface instead of any
   HTTP-derived version.

## Client changes to make

5. **RFC-034 (option 3) — kill the "reserved" button regex.** Normative rule:
   for a select field carrying an `action.*` role, wire value 0 is NEVER an
   operation. Replace the `/^(reserved|none|unused)$/i` label heuristic with
   the index-0 rule (gray it, never hide — the option_access gating stays as
   defense-in-depth and already grays it for sub-configure sessions).

6. **RFC-037 — prefer the catalog's explicit field width.** Layout fields MAY
   carry catalog key **18** = `size` (bytes). Decoder rule: prefer declared
   size over type-derived width; an UNKNOWN type with a declared size is a
   skippable hole — decode past it instead of truncating the layout tail
   (replaces the `offsets are unknowable past here` break in
   `core/slopsync/catalog.js:470`). For known types, treat declared≠derived
   as a catalog authoring error (warn, trust the type).
   Note: the reference device does not EMIT size yet (encoder-side is a
   follow-up) — implement the decode rule now so the client is ready.

7. **RFC-038 — ask for a browser-honest deadman window.** HELLO MAY carry key
   **44** = `deadman_wish_ms`; hub clamps into [250, 5000] and echoes the
   APPLIED value on the existing key 24. Recommended: wish ~2000 ms for the
   browser client and keep the visibilitychange re-establish hack as belt +
   suspenders (background-tab throttling is ~60 s, still beyond any legal
   window). Always ADOPT key 24's echo as the real window — never assume the
   wish was honored.

8. **RFC-039 — blob refusal is answered.** If the reassembler refuses a
   declared blob (total_bytes over its cap), send GOODBYE with code
   `BLOB_REFUSED (0x0503)` and surface a visible error — never idle in a
   half-session (this is the "LIVE WITH NO CATALOG" outage from
   `BlobReassembler`'s own comment, made conformant). Also: an idle-reaped
   session now receives GOODBYE `IDLE_REAPED (0x010C)` instead of
   `DEADMAN_TIMEOUT` — treat it as housekeeping (quiet reconnect), not as a
   safety event in the link log.

9. **`/uitoken` housekeeping (sideband, not protocol):** fetch it via a
   RELATIVE URL (`/uitoken`) instead of building `http://<host>/uitoken` —
   the hosted page's own origin is the device, which kills the port-80
   assumption in `core/slopsync/credentials.js:113`.

## Verification (ground-truth doctrine applies)

Every item above that changes a control or readout needs the live-device
check: payload observed on the wire + device state change (or state render)
confirmed. The rail tap-to-move is the headline — it goes from disabled-by-
principle to the machine's most-used control, so it gets the full
end-to-end pass (tap → 0x3100 INTENT with the role-bound key → post-clamp
ECHO → carriage moves → `telemetry.target` follows).
