# SlopSync Worked Session Traces *(informative — SPEC.md Appendix E)*

Five end-to-end narratives. Every step cites the normative rule it exercises — **a step
that needs a rule this spec doesn't state is a spec bug** (SPEC §17.3). Frame notation:
`TYPE(channel, seq){fields}`; CBOR shown as `{key:value}` with registry key names.

---

## E1 — Cold connect: browser WebUI, dynamic catalog

Preconditions: hub LIVE with one pattern running; browser has no cached etag.

| # | Dir | Frame / action | Rule |
|---|-----|----------------|------|
| 1 | c→h | WS upgrade, subprotocol `slopsync.v1` | §13.2 |
| 2 | c→h | HELLO{proto_ver:1, client_kind:"webui", client_name:"Atlan-desktop", instance_id, subscriptions:[{0x0003,0,3},{0x0080,60,2},{0x0082,0,1},{0x0083,0,1},{0x008C,2,1}]} — no token: viewer | §6.2 |
| 3 | h→c | WELCOME{session_id, boot_id, catalog_etag, cfg_gen, roles:0, limits, deadman_ms:600, deadman_policy, nonce, grants embedded} | §6.3, §10.2 |
| 4 | c | etag unknown → CATALOG_REQ{} (empty map = full transfer) | §8.4 |
| 5 | h→c | CATALOG_CHUNK ×N (192-B chunks; WS batches legally) | §8.4 |
| 6 | c | reassemble, verify etag over deterministic bytes, cache | §8.3 |
| 7 | h→c | retained STATE push: safety(0x0003), motion-status(0x0082), motion-config(0x0083), odometer(0x008C) — full snapshots, current seq | §9.1, §6.3 |
| 8 | c | all granted STATE received once → SYNCING→LIVE; UI un-greys | §2.2 |
| 9 | h→c | STREAM(0x0080) bundles begin @granted 60 Hz (samples decimated hub-side from 240) | §9.2 |
| 10 | c | user drags speed slider → **no local state change**; viewer lacks role → UI renders control locked (roles:0 from step 3) | §1.2-1, §12.2 |

Notes: the pattern that was running is *visible immediately* at step 7 (motion-status
snapshot) — the browser adopted a live session without touching it. No frame in this
trace pushed a default to the hub.

---

## E2 — Reconnect mid-motion: etag skip, reconcile, no silent control resume

Preconditions: paired controller phone (token held) was streaming motion-input(0x0081)
as active STREAMING source; WiFi blips 3 s.

| # | Dir | Frame / action | Rule |
|---|-----|----------------|------|
| 1 | — | last frame from phone at t=0; deadman window 600 ms | §6.5, §11.3 |
| 2 | hub | t=600 ms: deadman fires → STREAMING is initiator-bound → STOP (decel); safety(0x0003) latches STOP bit, cause=deadman; ownership of STREAMING released; control-owner(0x0004) updates; EVENT twin emitted | §11.3, §11.4, §9.4 |
| 3 | h→* | safety + control-owner STATE broadcast to all remaining subscribers | §9.1 |
| 4 | c→h | t=3 s: phone transport restores → HELLO{same instance_id, token, cached catalog_etag, standing wish-list} | §6.7 |
| 5 | h | instance_id matches a dead session — nothing to evict; new session_id issued | §6.3 |
| 6 | h→c | WELCOME{roles:1 (token valid), same boot_id, same etag, ...} | §6.3, §12.2 |
| 7 | c | etag matches cache → **skip catalog entirely** | §6.7 |
| 8 | h→c | retained STATE push incl. safety showing STOP/deadman | §9.1 |
| 9 | c | client had pending unACKed intent (set-speed 380) from before the blip. It does **not** retransmit; it compares desire (380) against adopted motion-config (360) — still wanted → issues fresh INTENT{intent_id:1(new session), value:380} | §6.7, §9.3 |
| 10 | h→c | ECHO{intent_id:1, applied:380, cfg_gen+1}; STATE broadcast to all | §9.3 |
| 11 | c | user taps "resume stream" → activating intent for STREAMING source (fresh, explicit) → granted, ownership reassigned, STOP clears per §11.1 (new motion intent from authorized source) | §6.7, §11.4, §11.1 |
| 12 | c→h | STREAM(0x0081) resumes | §9.2 |

The load-bearing negatives: no intent replay (step 9), no motion on socket-restore
(step 11 required a human-initiated activating intent), no catalog bytes on the wire
(step 7).

---

## E3 — Controller takeover: two remotes, one machine

Preconditions: remote A (paired, controller) owns PATTERN source, pattern running.
Remote B (paired, controller) connects.

| # | Dir | Frame / action | Rule |
|---|-----|----------------|------|
| 1 | B→h | HELLO(token) → WELCOME{roles:1} → SYNCING → LIVE; control-owner(0x0004) snapshot shows PATTERN owned by session A | §6.3, §11.4 |
| 2 | B→h | INTENT on pattern-control(0x0086): {value:{run, pattern:3}} | §9.3 |
| 3 | h→B | NACK{code:SOURCE_CONFLICT(0x0403), channel_id:0x0086} — A owns PATTERN | §11.4 |
| 4 | B | UI renders "controlled by A-remote" (it knows the owner from 0x0004) and offers Take Over | §11.4 |
| 5 | B→h | same INTENT + takeover:true(32); B's role (1) ≥ A's role (1) → transfer | §11.4 |
| 6 | h | ownership PATTERN: A→B; pattern reconfigured to pattern 3 via arbiter | §11.4, §3.1 |
| 7 | h→* | control-owner STATE + takeover EVENT (session-events 0x0007) broadcast | §11.4, §9.4 |
| 8 | A | A's UI (subscribed to 0x0004 like everyone) immediately shows control lost — not via any special message to A, just the same broadcast truth | §1.2-1, §11.4 |
| 9 | h→B | ECHO{applied pattern config} | §9.3 |

If B had been a viewer at step 5: NACK NOT_CONTROLLER(0x0102) — takeover cannot
outrank roles (§11.4, §12.2).

---

## E4 — ESTOP over a lossy relay: repeat-until-latch, fast path

Preconditions: OLED remote behind C5 relay (ESP-NOW, 30 % loss today); machine moving.

| # | Dir | Frame / action | Rule |
|---|-----|----------------|------|
| 1 | remote | user hits E-STOP → sends ESTOP frame `E5E5E5E5, cause:0(user), origin:1, seq:1, crc` — no role check, no session requirement | §5.5, §11.2 |
| 2 | remote | starts repeat timer: retransmit every 50 ms until latch observed (max 20) | §11.2 |
| 3 | radio | transmission 1 lost (30 % loss) — nothing happens. t=50 ms: transmission 2 reaches the relay | §11.2 (this is *why* repeat exists) |
| 4 | relay | raw byte scanner matches 4×0xE5 **before any deframing/queueing**; forwards on ALL attached segments (UART→hub, ESP-NOW back-broadcast) ahead of every buffered frame; CRC check deferred | §14.2, §5.5 |
| 5 | hub | validates CRC → arbiter e-stop path fires **before protocol bookkeeping**; motion stops; ESTOP latched, estop_seq:=1 | §11.2, §3.1 |
| 6 | h→* | safety(0x0003) STATE, critical priority: ESTOP bit + cause + seq; EVENT twin; both traverse relay's critical queue | §11.2, §10.1, §14.1 |
| 7 | remote | t=~120 ms: observes safety STATE with ESTOP latched, estop_seq ≥ 1 → **stops repeating**; UI shows latched state | §11.2 (latch is the ACK) |
| 8 | any | later: WebUI (controller) issues ESTOP_CLEAR intent on 0x0005 → hub verifies: cause resolved (user-initiated: trivially), velocity zero, no pending escalation → clears latch; motion still does NOT start (clearing only re-arms) | §11.2 |
| 9 | — | had all 20 repeats died (relay dead): remote surfaces loud local failure at t=1 s; the machine-side guarantee is then the deadman on whatever source was streaming, and the hardware e-stop path — SlopSync never claimed to replace it | §11.2 honesty clause, §11.5 |

---

## E5 — Constrained C5 client joins: static profile, degraded mode

Preconditions: C5 OLED remote, compiled-in catalog @ etag E1, canned CBOR templates,
no CBOR parser beyond template patching + prefix struct reads. Hub was OTA-updated
last week: catalog now etag E2 (one field *appended* to motion-status, one new channel).

| # | Dir | Frame / action | Rule |
|---|-----|----------------|------|
| 1 | c→h | HELLO from template: patches instance_id/token/etag(E1) value bytes into pre-encoded bytes — legal because deterministic encoding admits exactly one form | §5.3, §8.5 |
| 2 | h→c | WELCOME{catalog_etag:E2, ...} — hub treats static clients identically | §8.5, §6.3 |
| 3 | c | etag mismatch E1≠E2 → declared behavior: **degraded mode** (this device chose (a)) | §8.5 |
| 4 | c | subscribes (canned SUBSCRIBE) to position(0x0080), safety(0x0003), power(0x008C) | §6.6 |
| 5 | h→c | retained STATE: motion-status payload is now 1 byte longer than the C5's compiled struct → C5 parses its known prefix, ignores the tail — nothing breaks | §5.4 append-only, §4.3 |
| 6 | c | degraded-mode obligation: control functions whose schema it cannot re-verify are suppressed — its speed knob greys out; display functions continue; a "update me" glyph appears | §8.5 |
| 7 | c | position STREAM renders on OLED at granted 30 Hz; safety bit drives the red LED | §9.2, §9.1 |
| 8 | — | silent full operation on mismatched etag would have been **non-conformant**; both legal behaviors ((a) shown here, (b) refuse loudly) were available | §8.5 |

The quiet miracle in step 5 is the whole point of §5.4: a firmware update shipped, the
remote predates it, and the failure mode is a greyed knob — not a bricked remote, not
a parse crash, not silent wrongness.
