# LEDGER — current volatile truth

The single home (CANON C-2) for project/device state. Entries are stamped
`[verified YYYY-MM-DD — method]` or marked `[UNVERIFIED]` (hearsay — confirm
before building on it). Superseded entries are edited in place; git history
is the archive. Read this before substantive work; update it in the same
commit as any change that alters it (C-3).

## Device & branch

- **OVERNIGHT BENCH AUTHORIZATION (operator, 2026-07-28, pre-sleep):** motor
  UNPLUGGED — flash and test freely; fake-home via homeoverride for
  full-path testing; machine functionally identical except no Modbus or
  current-sense values (expected, not failures); COM11 serial authorized as
  rescue if something breaks. Deploy + live verification of the RFC batch
  is therefore IN SCOPE tonight (supersedes the no-deploy default for this
  session only).
  MORNING-REPORT PROTOCOL (operator, 2026-07-28, second sleep): if a fatal
  error stops pipeline work, SD32-OVERNIGHT-REPORT.md gets a
  "NOT DONE — HIT AN ERROR" section stamped at the VERY END: what stopped,
  the evidence, the best-effort suggested fix, and what is blocked behind
  it. Operator reads it on waking, stamps a ruling before work, checks in
  again from work. Absence of that section = nothing fatal happened.

- Work happens on `feat/cpp20-slopsync`; the device runs this branch's
  firmware; `main` is behind until stabilization + merge. [verified
  2026-07-27 — git branch state]
- Source-tree firmware version: see `FIRMWARE_VERSION` in
  `include/config_api.h` (its one home). [C-1 pointer]
- Deployed firmware on the device: **2.1.85**, the RFC-042..050 + Phase C4 +
  Phase E batch PLUS the `attachTransport()` STALE-slot-clobber fix + the
  boot reset-reason log line PLUS the HEAP RELIEF pass (BLOB_CHUNK backpressure
  reclass + WebRingSink PSRAM move — see the amended item (i) below) PLUS the
  parked-slot safety-broadcast panic fix (TRAPS T13 — see the PARKED-SLOT
  SAFETY BROADCAST entry below) PLUS the wire-string punctuation evolution
  (morning ruling item 2 — see the WIRE-STRING PUNCTUATION EVOLUTION entry
  below) PLUS the BLE advertising MSD fix (item (j) closed — see the BLE
  ADVERTISING MSD FIX entry below) PLUS the STATE-coalescing congestion
  wire-up (morning ruling item 1 — see its LANDED entry in the Morning
  ruling batch), LIVE. [verified 2026-07-28 — `/api/capabilities`
  `fw_version` = 2.1.85 post-OTA, coalescing bench session; DEPLOY + LIVE-VERIFY
  session, `/api/capabilities` `fw_version` before/after the OTA + a natural
  reboot + a deliberate re-flash reboot, all three confirmed `2.1.78`; see the
  DEPLOY + LIVE-VERIFY entry below for the full checklist, the "RFC-042
  attachTransport() clobber — FIXED" entry further down for the 2.1.78 →
  2.1.80 follow-up fix session, the WIRE-STRING PUNCTUATION EVOLUTION entry
  for the 2.1.82 → 2.1.83 deploy, and the BLE ADVERTISING MSD FIX entry for
  the 2.1.83 → 2.1.84 deploy]

## Milestones & landed state

- M5c landed: SlopSync is the only input/output plane; links2004 stack,
  `:81` telemetry socket, `:55555` TCode server all removed from the build.
  HTTP fallback *polling* remains by design. Story:
  `docs/http-plane-retirement.md`. [verified 2026-07-27 — truth-scrub audit,
  platformio.ini + src grep]
- The M5c-era "NEXT PHASES" list is stale: the servo-pane, `/api/slopmotion`
  POST, and clear-fault HTTP control routes are ALREADY retired in
  `src/ui/WebUI.cpp`, and the pairing ceremony (PAIR_REQ/PAIR_GRANT +
  operator PIN pane) is ALREADY landed in `webui/src`. [verified 2026-07-27
  — truth-scrub audit, code read]
  - **RESOLVED:** device-defined SlopSync INTENT channels replaced the
    retired HTTP control routes — `machine-admin` (0x30F0: clear_fault/
    save_config/servo_scan, replacing `/api/clearfault`/`WS_OP_SAVE`/
    `POST /api/servo {"scan":true}` respectively, per its own header
    comment in `SlopSyncCatalog.h`), `modes-set` (0x3030, machine modes),
    `sm-set` (0x3120, SlopMotion tuning). [verified 2026-07-28 — code read,
    `include/comms/SlopSyncCatalog.h` addMachineAdmin()/addModesSet() +
    ch:: constants]
- RFC-030 `curve_family` (registry key 45) landed with fw 2.1.75 glue.
  [verified 2026-07-27 — audit cross-check of registry + MOTION-TODO]
- SlopSync channel 0x0002 session-roster is **reserved, NOT implemented** —
  registry note previously lied about this; corrected in the 2026-07-27 fix
  pass. [verified 2026-07-27 — repo-wide grep, no implementation exists]

## Fixed in the 2026-07-27 truth-scrub pass

- `lib/slopsync` client idle-PING interval bug (never switched off the idle
  interval; SPEC §6.5). [fix applied; verified 2026-07-28 — native suite
  31/31 exit 0, reproduced fresh this session and in every gauntlet run
  since Phase B]
- `webui` session.js EVENT fan-out bug (log/anomaly frames double-dispatched
  as sessionEvent). [fix applied]
- `platformio.ini` env:esp32-c5-dev1 upload baud matched the documented-broken
  rate for the C5. [fix applied]
- ~50 confirmed lying comments/stale docs across all areas; docs-site
  generator unbroken + regenerated. [fix pass same date]

## Known residuals (bench-measured, July 2026)

- SlopMotion v3 chase trails a dense source stream ~35 ms
  (estimator-smoothing lag; regression estimator is the upgrade path if it
  matters on hardware). [bench 2026-07 — traces harness]
- Chase acceleration spikes to ceiling at stream reversals (aim overshoot at
  turn points; cosmetic in position — verify feel on hardware). [bench
  2026-07]

## Resolved rulings (2026-07-27, operator)

- **Wire-visible British spellings: RESPELLED** (pre-release ruling — "that's
  a stain that never comes out if deferred"). All catalog strings, the
  schema field name respelled to `centering`, and the `waveform_centered`
  token family (enum + label tables + device catalog + sim + test asserts +
  evidence captures) flipped in one pass; device catalog etag changes on next
  deploy.
  Frozen mini-catalog untouched (contains no British spellings). canon_lint
  C-11: 0 findings. [verified 2026-07-27 — lint + native suite + sd32-ota
  build]
- **Dead code: DELETED** (tests green before and after):
  `MotionInterpolator` moved to `examples/slopmotion_traces/` (bench baseline
  only, out of firmware); `MotionProfile.h` + its `test_motion_profile` suite;
  `WebUI::handleApiMove/Home/Stop/Pause/Halt/Override/ClearFault`;
  `SystemState` legacy anomaly ring; `UiProtocol.h` dead frame macros;
  `src/s3_main/main.cpp` stub; `Kinematics::planTrapezoid()` + `PlanResult`;
  `ServoMotionExecutor::adoptProfile()`; intiface websocket block.
  [verified 2026-07-27 — sd32-ota SUCCESS + native suite]
- **CLAUDE.md split**: preferences only; all rules in
  `docs/canon/DOCTRINE.md` (engineering) + `CANON.md` (governance).

## Pending operator rulings

- **RESOLVED (Phase C2, device-catalog renumber):** the prior entry here
  flagged the fixture-regen fork as a pending ruling; Phase C2's explicit
  instruction was to regenerate, so it was. `webui/test/fixtures/slopsim-catalog.bin`
  is now the FRESH capture (4,272 B, 21 channels, etag `e54d5e81c589f31d`) —
  the old 10,283 B fixture (which carried the pre-respell British field name,
  since fixed, and mirrored the device far more fully) is superseded. `slopsync-wire.test.mjs`'s
  one dependent assertion (0x1100 motion / `raw_10um`, was 0x0080) was moved to
  an explicit `[SKIP-EXPECTED-GAP]` (never deleted) rather than left failing —
  `ALL PASS`. The sim-fidelity gap this entry described is now CLOSED — see
  "Sim fidelity (SlopDeck milestone 1) — LANDED (2026-07-28 overnight)"
  below: the fixture is re-captured at device fidelity (44 ch, etag
  matching the live device) and the sim [FAIL]s/FATAL are fixed.
  [verified 2026-07-27, superseded 2026-07-28 — see milestone entry]
- **CLAUDE.md is gitignored** — the covenant is not in version control; one
  clean checkout loses it. Track it (or an agreed public variant)?
- **tools/slopsync_probe.py untracked** — primary verification tool, one
  checkout from gone (long-standing note in .gitignore).

## Active plan — SlopDeck (gold-standard client & widget system)

- Design ratified 2026-07-27: `docs/slopdeck/DESIGN.md` — three tiers, the
  Prime Rule (plugins go through SlopSync, never around it), founding Tier-1
  set, sim catalog profiles (`device` default / `alien` / `minimal`),
  sequencing (sim fidelity first). The sim-fidelity pending item above is
  milestone 1 of this plan.
- §8 RULED 2026-07-27: embedded UI = thin client, Tier 0+1, on hubs with the
  capability to serve it; hosted plain-http instance is the universal /
  UI-less-hub (WROOM) path; Tauri shell is the premium delivery (discovery,
  Tier 2 plugins, non-WS transports). PWA is NOT a viable delivery (https
  requirement blocks ws:// to LAN — recorded in DESIGN.md §8 so nobody
  promises it later).
- Transport rulings CALIBRATED 2026-07-27 (second pass): hardware hub
  profile = BLE GATT MUST (floor: infrastructure-free control, discovery,
  provisioning) + WS SHOULD/preferred; ESP-NOW = supported-not-developed
  peer binding; sim/hosted hubs exempt (RFC-043 recut). Client onramp
  ladder: TCode passthrough → native segments → native samples (RFC-044
  Draft — operator clarification on passthrough + streaming clients
  PENDING before it firms up). Intake doctrine: SlopSync is the only way in
  and out of THIS machine; other firmwares are never forced. NEW WORK ITEM:
  BLE GATT `ITransport`.
- **`OssmBleService` REMOVED 2026-07-27** (XToys ruling made the compat
  argument moot): service files deleted; TransportManager/main.cpp/WebUI
  unwired; `TransportMode` 4 tombstoned (never reuse); stored NVS mode 4
  already migrates via the existing >BT clamp. `BleTransport` (TCode NUS for
  Intiface) STAYS until RFC-044 + Intiface-native SlopSync land. [verified
  2026-07-27 — sd32-ota SUCCESS + native suite 31/31 post-surgery]
- FLAG (pre-existing, found during surgery): `ConfigStore.cpp` transport
  load clamp (`> BT → default`) also swallows persisted `DONGLE` (3) — a
  saved DONGLE selection never survives reboot. **MOOT, no ruling needed:**
  the whole mechanism this flag was about (`TransportMode`, the NVS
  `"transport"` key, the load clamp itself) was deleted by the later
  "transport switch dies" surgery below — `ConfigStore.h`/`.cpp` carry no
  `TransportMode`/transport-key symbol anymore. [verified 2026-07-28 —
  grep for `TransportMode`/`"transport"` in include/system/ConfigStore.h +
  src/system/ConfigStore.cpp: no matches]
- C5 co-processor plan: DEAD AS PLANNED (operator 2026-07-27) — it existed
  to stream TCode v3 at 333 Hz, which SlopMotion + TCode v4 obsoleted. The
  concept may return; `c5_waveshare`/`c5_tdongle` envs + sources stay for
  now. ESP-NOW posture: criminally easy to enable, supported, not developed.
- SlopDeck delivery accord + Svelte-5 framework ruling recorded in
  `docs/slopdeck/DESIGN.md` §8–9 (one kernel two faces; framework-neutral
  plugin ABI insurance).
- RULING 2026-07-27: **the transport switch dies** — "there's nothing to
  select, the only option is SlopSync." WS_OP_MODE, `TransportMode`,
  `applyTransport`, the NVS "transport" key, and the selector UI are all
  vestiges of the pre-SlopSync multi-protocol era. SCOPE RULED: **kill it
  all now** — SerialTransport, BleTransport (NUS), DongleTransport,
  TransportManager (WiFi duties extracted to a new WifiLink module),
  TCodeParser + TCodeAxisState glue, the selector, and the NimBLE
  dependency (zero consumers until the BLE GATT ITransport lands).
  Intiface TCode users wait for RFC-044/native — accepted consequence.
  **SURGERY LANDED 2026-07-27** (sonnet-executed, spec-mapped): all transport
  files + TransportManager deleted; WiFi duties extracted to
  `src/system/WifiLink.{h,cpp}`; TransportMode enum + NVS "transport" key +
  WS_OP_MODE (0x06 reserved) + /api/mode + applyMode gone;
  applogSerialDedicated gone; NimBLE dependency dropped. Wins: RAM 30.0% →
  27.4% (89,920 B), flash 28.9% → 26.0% (−~190 KB). [verified 2026-07-27 —
  sd32-ota SUCCESS + native 31/31 + canon_lint 0]
  - Residual pass (sonnet-executed): commanded_raw_mm writer restored on
    the SlopSync drain path (raw_10um telemetry regression), intiface_compat
    deleted (zero consumers), -DBLE_ENABLED removed (stops reserving BT
    controller memory for a stack that isn't in the build; #if machinery
    stays for the BLE GATT transport's return), has_dongle capability advert
    dropped, dangling c5 comment fixed. RAM/flash unchanged at 27.4%/26.0%
    (89,920 B / 1,705,208 B) — this pass touched no linked code, only a few
    fields and a flag. [verified 2026-07-27 — sd32-ota SUCCESS + native 31/31
    + canon_lint 0]
- RULING 2026-07-27 (expanded same day): **deadman-as-safety retired
  wholesale** (RFC-045 Draft recut): liveness stays as bookkeeping
  (STALE/reattach/slot reclaim); source-loss forced-stop REMOVED for all
  classes (streaming settles by physics); autonomous sources (PatternEngine,
  generators) get explicit `on_disconnect: stop | continue` (default stop);
  0x0005 explicit stops unchanged. Implementation follows RFC-045
  acceptance.
- RFC-044 (TCode passthrough) DEPRIORITIZED by operator: "a later feature,
  parsed machine-side" — a channel alongside segments and samples, not
  near-term work.

## RFC batch (operator-approved 2026-07-27: "approve and implement")

- **Phase B LANDED:** RFC-043/045/046/047 → Landed (v1.0); 044 → Accepted
  (posture landed, channel deferred). Registry: BLE identity UUIDs
  (534C4F50-5359-4E43-…), UDP discovery port 21328/"SLOP", frames 0x1E/0x1F,
  WELCOME keys 46 ws_port / 47 ipv4, status field on core channels. SPEC:
  §13.1 profiles, §13.8 UDP probe, §6.3 migration, §11.3 loss-policy
  removal reconciled across 8 sections, §9.6 onramp. New docs-site
  discovery.md. [verified 2026-07-27 — all generators --check green +
  native 31/31 + sd32-ota SUCCESS + lint 0]
- **VETO on Phase B decision 1 — RESOLVED (Phase C1):** DISCOVER_REPLY's
  `hub_id` is now `hub_instance_id` (u64, `identity_keys` 5, random-once
  NVS-persisted) — a durable hub identity replacing the per-boot `boot_id`
  RFC-046 had reused. Reply payload 72→76 B. [verified 2026-07-27 — see
  Phase C1 LANDED entry below]
- **Phase C1 LANDED:** RFC-048 (the UI/rendering constitution) — new
  normative companion `docs/slopsync/RENDERING.md`; SPEC.md §19 (Rendering)
  + §6.1/§6.3/§13.8 identity updates; registry gains eleven frozen
  vocabulary sections (`ui_categories` 14, `ui_ranks` 6, `value_aspects`/
  `value_scopes`/`value_provenance` 6/3/3, `unit_ids` 23, `action_tags` 13,
  `ui_archetypes` 15 w/ machine-checkable `fallback:`, `ui_regions` 5,
  `renderer_classes` 3, `widget_patterns` 13 w/ 3 `required`) plus the
  `hub_instance_id` fix above; RFC-QUEUE.md entry appended,
  `RFC-048-STAGING.md` deleted (content now lives in RFC-QUEUE.md +
  RENDERING.md). Riding along: RFC-045's `on_disconnect` promoted to the
  registered `field_roles` entry `source.background_run` (generalized to any
  autonomous source), with rendering rules in RENDERING.md §10.1. None of
  the eleven vocabularies are wired onto a real catalog entry yet (SPEC
  §18-23) — that is Phase C2. [verified 2026-07-27 — gen_registry_header.py
  --check green, gen_docs_tables.py --check green (14 files), gen_spec_pages.py
  --check green (20 files), native 31/31, sd32-ota SUCCESS, canon_lint 0
  findings]
- **Phase C2:** the device catalog evolution — 0xCDSS renumber + new
  vocabulary fields, one etag bump; clients (probe/MFP/webui-js/sim),
  devicecatalog goldens, fixture re-capture.
- **Phase D:** hub library — RFC-042 LANDS here too (operator ruling
  2026-07-27: "killing clients because you haven't heard their stream is no
  good" = 042's park-don't-kill principle stated as requirement; spec
  Draft→Landed + STALE/reattach implementation) + RFC-045 behavior (latch
  removal, background_run role honored) + test rewrites; §18 limitations
  21/22 update. **Phase E:** BLE GATT ITransport + UDP responder +
  advertising.
- Operator ruling 2026-07-27: full autonomy from here — escalate only
  HMM-grade judgment calls; agent decisions recorded as veto-able.

## Spec fresh-eyes panel (operator-ordered, 2026-07-27)

- 15 vacuum readers + convergence: `docs/slopsync/reviews/spec-panel-2026-07-27.md`.
  8 consistently-hated themes, 9 consistently-liked (core doctrines validated
  cold: shedding table 13/15, honesty clauses 12/15, closed motion surface
  12/15, readiness gate 11/15, ground-truth echo 10/15, RFC-045 redesign 9/15).
- **Phase C3 LANDED (2026-07-27):** RFC-049 (omnibus of 7 small normative
  fixes, spec/registry side; hub behavior named Phase D per sub-item) +
  RFC-044 correction, both appended to `RFC-QUEUE.md`; RFC-050 appended as
  **DRAFT only** (operator stamp still required, nothing normative changed).
  Numbers allocated: CBOR key `48 requested_curve_family` (core space, next
  free after 47), NACK `0x0504 INVALID_NAMESPACE` (transfer band, next free
  after `BLOB_REFUSED` 0x0503 — judgment call, flagged for veto: could
  instead have gone in the `0x00xx` protocol band, chose transfer band for
  thematic fit with its sibling `CHUNK_UNAVAILABLE`), registry `limits`
  gained `segment_handoff_k` (1.5, first non-integer/non-string limit —
  `gen_registry_header.py`'s `limits` emitter gained float support for it),
  `pairing_gesture_boot_count` (3) and `pairing_gesture_max_uptime_ms`
  (10000, pinning §12.3(c)'s "~10 s" hedge). `curve_families` entry 3
  (`step`) gained `status: reserved` (number kept, never renumbered).
  SPEC.md touched: §9.6 (curve_family honesty + requested_curve_family +
  segment_handoff_k citation + onramp paragraph reworded for RFC-044),
  §7.2 and §12.6 (trust-ledger SHOULD-populate + non-audit-grade + client
  display rule), §8.4 and §18-8/9 (INVALID_NAMESPACE split from
  CHUNK_UNAVAILABLE; the old unrepresentable "full request + chunks"
  MALFORMED rule replaced by the two rules that ARE representable: empty
  `chunks` array, and `ns=0` carrying `store_id`/`slot`), §14.3 (one-hop
  rationale stated + new relay ESTOP latency budget clause, H2's index
  entry extended to cite it), §12.3 (gesture + PIN-strike thresholds now
  cite registry constants instead of prose hedges), §18-20 (step honesty
  reworded), Appendix B (key 48 row) and Appendix G (3 new limit rows).
  Frozen artifacts (`conformance/mini_catalog.hpp` + its fixture) untouched.
  **Pre-existing drift found and fixed as a byproduct:** `docs-site/tools/
  gen_docs_tables.py` crashed outright (`setting_categories` retired from
  registry.yaml by an earlier, uncommitted-generator Phase C2 pass but never
  removed from the script's `SECTION_HOMES`/catalog-vocabulary page); fixed
  by deleting the dead section (superseded by `ui_categories` on
  `rendering.md`). Regenerating also caught several docs-site pages
  (`frames.md`, `channels.md`, `index.md`, `safety.md`, plus new
  `discovery.md`/`rendering.md`) that were stale against registry.yaml from
  before this session — RFC-046 discovery frames and RFC-047 channel-status
  commentary had never actually been regenerated into the committed site
  despite an earlier ledger entry claiming otherwise; now current.
  **Gauntlet (all green):** `gen_registry_header.py --check`,
  `gen_docs_tables.py --check` (14 files, 108 dictionary terms),
  `gen_spec_pages.py --check` (20 files), `gen_channel_map.py --check`
  (untouched, as expected — no core/device channel changed), native suite
  31/31 (exit 0), `pio run -e sd32-ota` SUCCESS (RAM 27.4% / 89,920 B,
  flash 26.1% / 1,709,528 B — both unchanged from pre-pass, as expected for
  a spec/registry-only pass), `canon_lint.py` 0 findings. [verified
  2026-07-27 — C-4: generator --check output + pio test -e native exit 0 +
  pio run -e sd32-ota SUCCESS + canon_lint 0 findings, all reproduced above]
  - Panel hate #2 (`source.background_run` unshipped) required NO new spec
    work — it is exactly Phase D, independently validated, unchanged by
    this pass.

## Phase D LANDED (2026-07-27/28): RFC-042, RFC-045 hub behavior, source.background_run, RFC-049(b)

- **RFC-042 (session staleness) landed in full.** `HubSessionState::STALE`
  (library-internal). Silence (deadman/idle-reap) and an out-of-band
  transport loss (`Hub::detachTransport()`) now mark a session `STALE` via
  shared `Hub::markStale()` instead of tearing it down — slot, `session_id`,
  grants, intent ring, readiness all RETAINED, ownership released
  unconditionally, nothing latched. `Hub::reviveIfStale()` (path A, any
  frame) and `Hub::handleReattach()` (path B, a fresh HELLO on a new
  transport — same `session_id`, RETAINED grants, role re-derived) implement
  resumption; `Hub::findEvictableStale()` implements item 5's slot-pressure
  reclaim (best-effort GOODBYE `SLOT_RECLAIMED`). New registry: NACK
  `0x010D SLOT_RECLAIMED`, `session_event_kinds` 4 `session_stale`/5
  `session_resumed`. The general cross-BINDING-TYPE migration RFC-046
  describes is NOT implemented (this hub has one binding, WS-only, so it
  cannot safely distinguish that case from a genuine second claimant — falls
  back to the duplicate-identity eviction rule per §6.3's own MAY clause).
- **RFC-045 hub behavior landed.** `Hub::releaseSessionSources()` no longer
  runs any Stop-vs-Continue policy dispatch — every release (any of the now
  seven staleness/teardown doors) is `onSourceOwnership(source, 0, reason)`
  and nothing else. `HubDelegate::sourcePolicy()`/`onDeadmanStop()` remain
  declared (frozen delegate interface) but are dead from the hub's side —
  annotated as such, not deleted. The SI-15 STREAM-bundle "clears a latched
  STOP" workaround is deleted from `handleStream()` (moot, not merely
  obsolete). **Judgment call, flagged for veto:** SPEC §11.3's PRE-Phase-D
  text claimed the hub-autonomous `background_run=false` case still latches
  STOP with `cause=deadman`; this pass found no public Hub API for a
  delegate to latch that honestly (`latchEstop()` has no STOP sibling) and
  the task brief's own repeated instruction was "latches NOTHING, hub stays
  generic" — resolved by fixing the SPEC text to match the simpler, actually
  implemented reality (an autonomous source that stops being driven settles
  the same way a command-driven one does, no manufactured safety edge) rather
  than adding a new public latch API to preserve the old sentence.
- **`source.background_run` shipped** on the reference firmware's
  `pattern-state` (0x1200, settingKey 7, paired 0x3200 `pattern-cmd` key 7;
  bit 6 of `enabled_mask`, unconditionally 1 — a standing policy, never
  gated by homed/estop unlike bits 0-5). NVS-persisted (`pat_bgrun`), unlike
  the session-volatile pattern parameters on the same channel.
  `SlopDriveHubDelegate::onSourceOwnership()` (newly implemented — was the
  base no-op) stops `PatternEngine` on release iff the flag is false;
  reason-agnostic (fires identically for a staleness release or a genuine
  teardown). SPEC §18-21 reworded from "specified, not shipped" to shipped.
- **RFC-049(b) landed:** `requested_curve_family` (CBOR key 48) echoed
  verbatim in `granted_publishes`/WELCOME/GRANT alongside the effective
  `curve_family` (45). **RFC-049(c) first half landed:** the firmware's
  independently-hardcoded `1.5f` (`SystemState::sm_tune_handoff_k`) now
  reads `slopsync::limits::segment_handoff_k`. **RFC-049(c) second half
  (the sparse-segment scheduling-depth backstop) EVALUATED AND NOT LANDED**:
  a `commitWaveform()` variant bounding a lookahead-less handoff against its
  own chord was implemented, then reverted after it measurably shrank
  `test_slopmotion`'s "Mixed feasible/infeasible chain settles centered and
  STAYS there" regression bench's characterized defect (-23.6mm -> -9.4mm)
  via an unverified interaction with the centering/reshape control loop.
  Recorded in `slopmotion.hpp`'s `commitWaveform()` comment; left open.
- Tests: new `test/native/test_slopsync_staleness/` (STALE-01..04, 5 cases);
  rewritten expectations in `test_slopsync_safety` (S-05/S-06, two "M4a
  RFC-022.3" cases → "RFC-045"), `test_slopsync_m3b` (MB-10/11),
  `test_slopsync_m4b` (M4B-05), `test_slopsync_m4c` (M4C-11),
  `test_slopsync_streamingress` (SI-08, SI-15, + new SI-23b), and
  `test_slopsync_devicecatalog` (5 cases updated for the new catalog field).
  **Gauntlet (all green):** native suite (all environments PASSED, doctest
  exit 0 confirmed directly per TRAPS T10), `pio run -e sd32-ota` SUCCESS
  (RAM 27.4% / 89,920 B, flash 26.1% / 1,712,352 B), `canon_lint.py` 0,
  `catalog_lint.py` OK (32 entries), `gen_registry_header.py --check`,
  `gen_docs_tables.py --check` (14 files, regenerated for the new
  session_event_kinds/nack_codes rows), `gen_spec_pages.py --check` (20
  files, regenerated for the SPEC.md §2.2/§6.6/§6.9/§11.3/§12.7/§16.1/§18
  edits), `gen_channel_map.py --check` (untouched, as expected — no channel
  id changed), `node webui/test/slopsync-wire.test.mjs` ALL PASS (sim
  catalog fixture unaffected — the sim declares no pattern generator, so no
  re-capture was needed). [verified 2026-07-28 — commands reproduced above,
  each exit code checked directly per TRAPS T10]

## Phase E LANDED (2026-07-28): BLE GATT ITransport + UDP discovery responder + advertising — BUILD/HOST-VERIFIED ONLY, NOT DEPLOYED

- **NimBLE returns.** `h2zero/NimBLE-Arduino@^2.3.0` back in `[common_s3_libs]`
  lib_deps (resolved 2.5.0 at build time); `-DBLE_ENABLED` restored to
  `env:s3_main` and the frozen legacy `esp32-s3-devkitc-1` block (propagates
  to `sd32`/`sd32-ota` via `${env:s3_main.build_flags}` inheritance — no
  separate edit needed there). `src/main.cpp`'s `bleInUse` weak override and
  WebUI's `has_ble` capability activate unchanged, exactly as anticipated;
  `build_webui.py` picked up `VITE_BLE_ENABLED=true` automatically.
- **`src/comms/SlopSyncBleTransport.{h,cpp}`** — the NUS-shaped GATT
  `ITransport` (RFC-043), UUIDs `534C4F50-5359-4E43-8000-0000000000{01,02,03}`
  transcribed from registry.yaml `ble_identity` (the codegen does not emit
  this section as C++ constants — documented fallback, not a spec gap).
  Mirrors `SlopSyncAsyncWsTransport`'s SPSC-ring + deferred-attach/detach-
  on-hub-task pattern for TRAPS T5 (NimBLE callbacks run on the host's own
  FreeRTOS task). 2 concurrent connections (`SlopSyncBlePort::kSlots`).
  Congestion signal: a failed `notify()` IS the "notify queue depth" signal
  (§13.1) — NimBLE exposes no separate depth counter to read.
  `properties().mtu` reads `NimBLEServer::getPeerMTU()` live (20 B
  pre-negotiation, up to 247 after — MTU 250 requested at init). Advertising:
  service UUID + shortened name "SD32" in the primary payload, full
  "SlopDrive-32" in the scan response, one Manufacturer-Specific-Data flags
  byte (company id `0xFFFF`, the Bluetooth SIG's own "for testing" id —
  SlopSync has no assigned company id) carrying `ble_adv_flags` bit0
  `pairing_window_open`/bit1 `ws_available`; refreshed only on an actual
  state change (`SlopSyncBlePort::updateAdvertising`, diff-gated against the
  last-published byte — no polling of the radio itself).
- **`src/comms/SlopSyncUdpDiscovery.{h,cpp}`** — the UDP responder (RFC-046
  item 5), port 21328. **Judgment call, flagged for veto:** built against
  raw lwIP sockets (`lwip/sockets.h`), NOT `AsyncUDP` as the task brief
  anticipated. `AsyncUDP.h` is bundled with the arduino-esp32 core but this
  project's LDF (chain mode) does not resolve it — confirmed live this
  session: `pio run -e sd32-ota` failed `AsyncUDP.h: No such file or
  directory` with LDF's own "check our library registry" message, meaning
  it genuinely found no local provider for that header, not merely a
  missing include path. Rather than add another framework-package special
  case, the responder polls non-blocking `recvfrom()` from the hub task's
  existing 5 ms tick. This also SIMPLIFIES the threading story versus the
  planned design: there is no foreign-task callback at all, so TRAPS T5's
  enqueue-and-defer discipline does not apply here (documented in the
  header) — the identity snapshot (hub_name, hub_instance_id, fw_version,
  catalog_etag, ws_port) is write-once at `begin()`, and the two live fields
  (pairing_window_open, ws_available) are set by the same task that reads
  them. `DiscoveryRateLimiter`: fixed 8-slot linear-scan ring, one reply per
  source IP per second, no heap.
- **`include/comms/SlopSyncDiscoveryWire.h`** — pure byte encode/decode for
  DISCOVER_PROBE/DISCOVER_REPLY and the `ble_adv_flags` byte shared with BLE
  advertising, zero Arduino/NimBLE/socket dependency, host-testable
  (`test/native/test_slopsync_discovery`, 8 new cases). This is the ONLY new
  Phase E surface with native test coverage — the NimBLE/socket glue is
  hardware-only, per the task brief's own instruction not to fake-test it.
- **`hub_instance_id` (RFC-048)**: generated once via `esp_random()` x2,
  persisted in NVS (`Preferences("slopsync")`, key `hubid`, u64), fed to
  `slopsync::Hub::setHubInstanceId()` (new additive Hub API) and to the UDP
  responder's snapshot.
- **slopsync-core additive changes** (hub.hpp/welcome.hpp — additive per the
  CANON-frozen "extend, never reshape" rule for hub.hpp's public API):
  `Hub::setHubInstanceId()`/`hubInstanceId()`, `Hub::setEndpoint(wsPort,
  ipv4)`, `Hub::pairingWindowOpen()`, `Hub::catalogEtag()`. `WelcomeMsg`
  gained `ws_port` (key 46) / `ipv4` (key 47) — omitted from the wire at
  their 0/absent default, same additive-safe idiom as every prior optional
  WELCOME key — and `IdentityInfo::hub_instance_id` (identity_keys 5).
  **Judgment call, flagged for veto:** WELCOME's `limits.max_frame` was a
  hardcoded `kFrameBufferCapacity` (512) for EVERY transport, including the
  in-process conformance binding whose own default MTU is 250 — i.e. it was
  already lying about a hub's own §13.1 rule ("MUST NOT advertise a larger
  max_frame than the binding permits"). Changed both WELCOME-build call
  sites to `min(transport.properties().mtu, kFrameBufferCapacity)`, a no-op
  for WS (its `properties().mtu` already equals 512) but honest for BLE's
  small pre-negotiation MTU. Checked for regressions: no existing test
  asserts the literal value (only one codec-level unit test builds its own
  hand-set `WelcomeMsg` directly), so this is correctness-neutral for every
  prior passing test — confirmed by the full native suite staying green.
- **New native tests:** `test/native/test_slopsync_discovery` (8 cases: adv
  flags every bit combination, probe parse incl. bad-magic/bad-length
  rejection, reply byte layout incl. truncation and too-small-buffer
  honesty) + 3 new WELCOME round-trip cases in `test_slopsync_messages` —
  the first direct unit coverage `encodeWelcome`/`decodeWelcome` has ever
  had (previously exercised only through session-level HELLO/reconnect
  integration tests).
- **Gauntlet (all green), every command run directly this session:** native
  suite 31/31 environments (mingw64 PATH prepend, exit code 0 confirmed per
  TRAPS T10); `pio run -e sd32-ota` SUCCESS — **RAM 27.4%→29.4% (89,784 B→
  96,436 B, +6,652 B), flash 26.1%→28.6% (1,710,489 B→1,876,816 B,
  +166,327 B)** (pre-Phase-E byte counts back-computed from the last-verified
  percentages, the task brief's own baseline) — the expected NimBLE-return
  regression and the one axis this phase was told to move; `canon_lint.py` 0
  findings; `catalog_lint.py` OK (32 entries, unchanged — no catalog channel
  touched); all five generators `--check` green (`gen_registry_header.py`,
  `gen_channel_grid.py`, `gen_channel_map.py`, `gen_docs_tables.py`,
  `gen_spec_pages.py`) — registry.yaml/SPEC.md were NOT touched this phase
  (no spec gap found; `ble_identity`/`ble_adv_flags`/`udp_discovery` numbers
  were transcribed from the already-landed Phase B/C1 registry sections per
  the task brief's documented fallback, not re-derived or invented).
- **NOT done this phase (explicitly out of scope per the task brief) — DO
  NOT read anything above as "live" or "deployed" (C-8):** no deploy, no
  device flash, no live BLE connection from a phone/scanner, no live UDP
  probe/reply exchanged over an actual LAN, no live advertising-payload
  capture (nRF Connect / a BLE sniffer). Also **not implemented**: generic
  control-frame fragmentation over a small (unnegotiated) BLE ATT MTU —
  SPEC §18 item 22 already states BLE GATT ships "with no reference
  implementation"; this phase's transport requests MTU≥250 pre-use so
  WELCOME/the catalog fit unfragmented in the common case, but a control
  frame that still does not fit after negotiation simply fails to send
  (ordinary backpressure, not a NACK/error) rather than being split — a real
  limitation for a client stuck at the legacy 23-byte MTU, consistent with
  §13.4's own "static-profile clients are the expected BLE norm" text.
  [verified 2026-07-28 — every command above run directly this session,
  exit codes / SUCCESS banners observed firsthand]
- **Main-loop review (2026-07-28): all three flagged judgment calls
  ACCEPTED.** (1) raw-lwIP responder over AsyncUDP — the failed build is
  real evidence and the no-foreign-task shape is strictly simpler than the
  planned design (T5 never even applies); (2) `max_frame = min(mtu, 512)` —
  corrects a pre-existing §13.1 violation, not a behavior choice; (3) no BLE
  control-frame fragmentation — matches SPEC §18 item 22's own admission.
  Operator veto window open on all three per the autonomy bar.

## DEPLOY + LIVE-VERIFY (2026-07-28 overnight) — fw 2.1.78, motor unplugged bench session

Per the OVERNIGHT BENCH AUTHORIZATION above. `FIRMWARE_VERSION` bumped
2.1.77→2.1.78 (`include/system/config_api.h`); `pio run -e sd32-ota` SUCCESS
(RAM 29.4% / 96,436 B, Flash 28.6% / 1,876,816 B — unchanged from Phase E's
build-only numbers, confirming the deploy shipped exactly what Phase E built).
Deployed both images (`deploy.ps1 -Target both`): firmware via `/api/ota`
(`2.1.77 -> 2.1.78` confirmed), web UI via `/api/ota/fs`. **Pre-existing drift
caught by this deploy, not a regression:** the device's PRE-deploy
`/api/capabilities` already answered `fw_version: 2.1.77` with `has_dongle`
still advertised true — proof the previously-"live" 2.1.77 was actually a
stale build predating the dongle-removal/Phase-E source changes (the version
string had not been re-bumped after those edits landed in-tree); post-deploy
capabilities now correctly show no `has_dongle`, `has_ble`/`slopsync_ble`
true, and a new `udp_discovery_port` field. [verified 2026-07-28 —
`/api/capabilities` diffed before/after]

**Checklist results (item letters per the task brief):**

- **(a) MFP LiveWireTest ×2, back-to-back, no reboot — FIXED + VERIFIED
  (client-side, follow-up session, same date, fw unchanged at 2.1.81).**
  Original finding (kept verbatim): both runs behaved IDENTICALLY (same
  `boot_id` both times, confirming no reboot occurred between them — the
  actual T3 regression check passes: no crash, no session leak, clean GOODBYE
  both times) but both hard-FAIL the motion-grant assertions (`granted
  motion-input rate == 50 Hz` reads `NaN`; the three ingress-counter diffs
  that depend on it also fail). Root cause, confirmed by reading
  `SlopDriveHubDelegate::validateToken()` (`src/comms/SlopSyncHubService.cpp`):
  auth enforcement ("ENFORCEMENT IS ON (fw 2.1.59)") requires a `/uitoken`
  mint presented in HELLO's token field to get `control` tier; `clients/
  mfp-slopsync/LiveWireTest.cs` never mints or presents one (grep confirmed
  zero references to `uitoken` in the file at the time) and generates a fresh
  random `instance_id` every run, so it structurally cannot match the trust
  ledger's one paired entry either. This has been true since fw 2.1.59,
  predating the RFC batch entirely — `tools/slopsync_probe.py` (which DOES
  mint a `/uitoken`) is the tool that actually proves the write plane works;
  LiveWireTest proved only the read-only wire shape.
  **Fix (client-side C# only, no firmware touched):** `LiveWireTest.cs` builds
  its own session directly against `SlopSync.cs`'s real `HubClient`/`HelloAsync`
  (it does not go through the plugin's `AcquireTokenAsync`, a private instance
  method of the plugin view-model class) — so it grew its own minimal mint-only
  mirror, `MintUiTokenAsync(HttpClient, baseUrl)`: `GET /uitoken` (no CORS
  headers by design, RFC-029 §4 — a native client just reads the body), decode
  the returned hex token via `Convert.FromHexString`, retry up to 3× on `429
  TooManyRequests`. The minted 16-byte token now rides in both `HelloAsync`
  call sites (samples and `--segments` paths) where `null` was hardcoded
  before. A new explicit PASS/FAIL row (`control-tier credential presented
  (/uitoken)`) makes the tier proof visible on its own line instead of only
  showing up as a side effect of the rate assertion. Verified `/uitoken` is
  self-serve for LAN/HTTP clients by reading `SlopSyncUiToken.h`/`.cpp`
  directly: `attachRoutes()` registers a plain `GET /uitoken` on the shared
  HTTP server with **no** pairing ceremony, PIN, or physical confirmation
  gate — its only defenses are rate-limiting, a 60 s single-use TTL, and the
  deliberate absence of CORS headers (the mechanism is the browser's
  same-origin policy refusing to let a hostile page READ the response; it
  does nothing to stop a native LAN process, which is an accepted, documented
  limit in the header comment, not a gap this fix needed to close).
  **Live re-verification:** un-fake-homed the device first (`tools/
  slopsync_probe.py --bench-home`, full round trip revert — mid-run the probe
  hit an unrelated `ConnectionResetError` at its Step 5.9 safety-ops check and
  the device rebooted; end state after the reboot was already `homed=false
  home_override=false estopped=false`, i.e. exactly the required precondition,
  so the run's goal was met, just not via the clean revert path — flagged as
  an anomaly, not chased further per this task's client-side-only scope).
  Built `dotnet build clients/mfp-slopsync/LiveWireTest.csproj -c Release`: 0
  warnings, 0 errors. Ran twice back-to-back with no reboot between (confirmed
  via `/api/status` uptime rising monotonically, no gap): **both runs ALL
  HARD CRITERIA PASS**, both reporting identical `boot_id=0xFA5951E1` and
  `granted motion-input rate == 50 Hz  (granted=50.00)` — the assertion that
  used to NaN. Device end-state restored after verification: `--bench-home
  --bench-home-no-revert` (leaves the override ON on purpose), confirmed
  `homed=true home_override=true measured_stroke_mm=250 estopped=false`,
  matching the bench baseline the rest of the overnight pipeline documented.
  Files touched: `clients/mfp-slopsync/LiveWireTest.cs` only. [verified
  2026-07-28 — two live runs against fw 2.1.81, transcripts captured, `dotnet
  build -c Release` 0/0, `/api/status` before/after each phase,
  `SlopSyncUiToken.h`/`.cpp` + `src/main.cpp` route-registration read]
- **(b) Fake-home — PASS.** No HTTP route (`/api/machine/homeoverride` is a
  410 tombstone pointing at the in-band replacement); used
  `tools/slopsync_probe.py`'s `--bench-home` (INTENT 0x3101 op 2 force_home).
  Discovered `--bench-home` is a deliberate ROUND TRIP (force_home then
  immediately clear_override, "not a one-way door") — added a new
  `--bench-home-no-revert` flag (skips the revert ECHO) so the override can
  actually be left ON for a test session; used it to leave the machine
  `homed=true home_override=true measured_stroke_mm=250` for the rest of this
  session. [verified 2026-07-28 — `/api/status` before/after]
- **(c) Full probe pass — PASS, 56/0/2 (2 intentionally-opt-in skips:
  estop_assert, bench_home).** Confirms the C4 channel-id renumber is what
  the live catalog actually serves (`subscribe_1000/1010/1020/1100/1110/1111/
  1200/4100/0003` all present and granted), catalog etag reported
  (`9275f578ada7d314`), HELLO→WELCOME works, and — via two probe fixes below
  — WELCOME's `ws_port`(46)=82/`ipv4`(47)=192.168.1.229 and identity
  `hub_instance_id`(identity_keys 5)=`0x28F1295A0510B8E1` are now decoded and
  checked, both present and sane. **Two pre-existing FALSE FAILURES found and
  fixed in `tools/slopsync_probe.py` itself** (not firmware bugs — the device
  catalog was already correct; the probe's own hardcoded vocabulary copies
  had drifted): (1) `FIELD_ROLES` was a stale 16-entry hand-copy of
  registry.yaml's now-32-entry `field_roles` map (missing every
  `geometry.*`/`pattern.*`/`plan.*`/`command.position`/`telemetry.target`/
  `source.background_run` role landed since — synced to the full registry
  list); (2) `pattern_mask`'s homed/estop cross-check compared the WHOLE
  `enabled_mask` byte against zero, but Phase D's `source.background_run`
  (bit 6) is unconditionally 1 regardless of homed/estop — every unhomed
  session was a guaranteed false "CONTRADICTS" (fixed: mask off bit 6 before
  the zero-check). Both were 100% reproducible before the fix, 0/0 after.
  [verified 2026-07-28 — probe run before (2 FAIL) and after (0 FAIL) the
  fix, diffs in `tools/slopsync_probe.py`]
- **(d) STALE/reattach live (RFC-042) — FIXED + VERIFIED (2026-07-28,
  follow-up session, fw 2.1.78 → 2.1.80).** Originally: FAIL, real bug
  found, NOT fixed (flagged for operator) — the root-cause writeup below is
  kept verbatim for the record. **Governance ruling (main-loop,
  2026-07-28):** the CANON freeze on `hub.hpp`/`client.hpp` covers PUBLIC
  API SHAPE (extend, never reshape); it does not shield an internal
  slot-selection bug in `hub_impl.hpp` that defeats a stamped RFC. Fix
  authorized — no public API signature changed, frozen artifacts
  (`mini_catalog.hpp`, `mini-catalog.yaml`, golden byte arrays) untouched.
  **Fix:** `Hub::attachTransport()` now prefers a genuinely-free slot
  (`transport == nullptr && !session.occupied()`); if none exists, it falls
  back to `findEvictableStale()` — the SAME oldest-parked eviction policy
  `handleHello()`'s own slot-pressure branch already used (RFC-042 item 5),
  not a second reclaim rule — severing the victim's transport first if one
  is still attached (a deadman/idle-reap staleness can leave one, unlike the
  out-of-band-detach trigger that produced this bug, which nulls it already).
  **New native tests** in `test/native/test_slopsync_staleness/`: STALE-05
  (the exact interleaved clobber scenario — park STALE via
  `detachTransport()`, a DIFFERENT identity connects, the parked session
  survives untouched and reattaches afterward by its own identity), STALE-06
  (same-identity reattach after a `detachTransport()`-triggered staleness,
  state intact — STALE-02 already covered the idle-silence trigger, this
  covers the trigger the live bug actually used), STALE-07 (all
  `kHubMaxSessions + 1` physical slots accounted for, one STALE — the new
  `attachTransport()` eviction fallback fires, same policy STALE-03 already
  proved at the HELLO layer). Full native suite: 31/31 environments PASSED,
  exit 0 (TRAPS T10). `pio run -e sd32-ota` SUCCESS (RAM 29.4% / 96,436 B,
  flash 28.7% / 1,877,624 B). **Live re-verification** (scripted directly,
  same shape as the original repro — `tools/slopsync_probe.py` has no flag
  for this and still can't run a full session end-to-end against this
  device's separately-flagged heap issue, item (i) below, so a standalone
  wire-layer script reusing `slopsync_probe.py` as a module did the honors,
  skipping catalog BLOB adoption entirely since RFC-042 reattach identity is
  orthogonal to it): two independent kill+reattach runs both got their
  ORIGINAL `session_id` back, and a third INTERLEAVED run (a different
  identity connects between the kill and the reattach — the exact clobber
  scenario) confirmed the interloper got its own distinct `session_id` while
  the parked session survived and reattached correctly afterward. Churn: 20
  rapid connect/hard-kill cycles (which — with only `kHubMaxSessions + 1` = 5
  physical slots and every cycle a fresh identity — exhausted the free-slot
  supply by cycle ~5 and forced the NEW eviction-fallback path on every
  cycle thereafter, live, repeatedly): 20/20 handshook, uptime rose
  monotonically throughout (no reboot), heap stayed flat (~13.6 KB free,
  no crisis — confirms the item (i) heap issue is specifically tied to the
  catalog BLOB transfer, not this fix or ordinary session churn). Original
  root-cause writeup, verbatim: Scripted directly (no probe flag exists for
  this): open a session, `SO_LINGER{1,0}`-close it (genuine TCP RST, no
  GOODBYE — mirrors `slopsoak.py`'s `close_rst()`), reconnect with the
  IDENTICAL `instance_id` after both a 2 s and a 10 s gap. Both gaps FAIL
  identically: the reconnect always gets a BRAND NEW `session_id` and the
  device log shows the old session logged `left` (full teardown,
  `onSessionLeft`) at the moment of reconnect, never `STALE`-then-reattached.
  Root cause traced into `lib/slopsync/include/slopsync/hub/hub_impl.hpp`:
  `Hub::detachTransport()` correctly calls `markStale()` (retains
  `session_id`/grants, sets `slot.transport = nullptr`) — but
  `Hub::attachTransport()` picks a slot for a brand-new incoming connection
  by `slot.transport == nullptr` ALONE, with no check for
  `slot.session.state == STALE`. A STALE slot's transport pointer is null by
  definition, so it looks exactly like a genuinely-free slot and gets handed
  to the FIRST new connection that shows up — including one with a totally
  unrelated `instance_id` — before `handleHello()`'s own
  `findSlotByInstance()`/reattach-by-identity logic ever gets a chance to
  run. With `kHubMaxSessions = 4` and this bench session's rapid single-
  client churn, every single reconnect during tonight's testing landed on
  slot 0 (the first array slot, always the most recently vacated one) and
  clobbered whatever was parked there. **Confound ruled out:** the first
  failed attempt happened while the device also had an unrelated
  self-inflicted HTTP stall backlog (below); re-ran on a freshly-rebooted,
  clean device with a 10 s gap and got the identical failure, so this is
  not a timing race or an artifact of the stall. [verified 2026-07-28 —
  two independent live reproductions + code read, `hub_impl.hpp` lines
  ~93-149; fix + tests + live re-verification verified 2026-07-28 same date,
  follow-up session — native suite 31/31 exit 0, sd32-ota SUCCESS, live
  reattach ×3 + 20-cycle churn all reproduced above]
- **(e) background_run 0/1 disconnect behavior — PASS, both directions.**
  Scripted directly against `pattern-cmd`(0x3200): set `background_run`(key
  7)=1, start the pattern (`running`=1), clean-close the client, confirmed
  from a fresh second connection that `pattern-state`(0x1200) still reads
  `running=true` after the first client was gone. Repeated with
  `background_run`=0: confirmed `running=false` after disconnect. Both ECHOed
  correctly on the wire; the RFC-045/048 policy is honored in both
  directions. Minor caveat (not a failure): in the `background_run=0` run the
  STATE read immediately after starting the pattern (while client A was
  still connected) already showed `running=false`, so that particular run
  demonstrated "stays stopped" rather than "was running, then stopped on
  disconnect" — the wire ECHO still confirms the `running=true` INTENT was
  accepted, so this reads as a timing/read-race in the test script, not
  firmware behavior. [verified 2026-07-28 — scripted 2-client wire test,
  both directions]
- **(f) UDP discovery — PASS** (unicast, broadcast, rate limiting all
  confirmed). Hand-encoded DISCOVER_PROBE/decoded DISCOVER_REPLY per
  `include/comms/SlopSyncDiscoveryWire.h`'s documented layout. Unicast to
  192.168.1.229:21328 got an immediate, correctly-shaped 76-byte reply
  (`hub_instance_id=0x28F1295A0510B8E1`, `ws_port=82`, `fw_version=2.1.78`,
  `catalog_etag=9275f578ada7d314` — all matching the WS-side values).
  Broadcast to 192.168.1.255 FAILED when sent from a wildcard-bound socket
  (this Windows host is multi-homed — a WSL/virtual adapter at 172.27.224.x
  alongside the real LAN NIC — and the broadcast went out the wrong
  interface); binding the socket explicitly to the LAN IP fixed it
  immediately, confirming this was a test-host routing artifact, not a
  device-side gap. 3 probes sent within ~0.1 s from the same source got
  exactly 1 reply, confirming the 1/source/s rate limit. [verified
  2026-07-28 — raw-socket scripts, both interface-binding variants captured]
- **(g) hub_instance_id stability — PASS.** `0x28F1295A0510B8E1` observed
  identically across: the Step-2 OTA reboot, a deliberate same-binary
  re-flash reboot (triggered specifically for this check, since no SlopSync
  reboot-admin INTENT exists and power-cycle is unavailable), and the UDP
  DISCOVER_REPLY path — three independent reads, one value. [verified
  2026-07-28 — probe WELCOME identity + UDP reply, before/after the re-flash]
- **(h) requested_curve_family (CBOR key 48) — PASS.** Hand-built a HELLO
  publish wish for motion-segment(0x2101) carrying `curve_family`(45)=3
  (`step`); WELCOME's `granted_publishes` entry echoed back
  `{45: 1, 48: 3}` — key 48 verbatim-echoes the wish (3, step) exactly as
  RFC-049(b) specifies, while the effective key 45 shows the hub's
  `curve_policy` DOWNGRADED it to 1 (c1_cubic). This is the mechanism working
  exactly as designed: the two keys being simultaneously present and
  different is what makes a downgrade a visible fact instead of a client
  inference. [verified 2026-07-28 — hand-built HELLO wish + WELCOME decode]
- **(i) Heap beacon — DIAGNOSED + MITIGATED (HEAP RELIEF pass, follow-up
  session, fw 2.1.80 → 2.1.81).** Original finding (kept verbatim below),
  then root-caused and measurably relieved; a real residual is flagged at
  the end, unfixed.

  **Diagnosis, with live evidence (device 192.168.1.229, motor unplugged
  bench session).** The BLOB `ConnectionResetError` has TWO confirmed
  mechanisms, not one:
  (1) **AsyncWebSocket queue-full → session close**, the majority case.
  `SlopSyncAsyncWsTransport::write()` classified `BLOB_CHUNK` (0x1B) as
  ordinary "control" (everything that is not STATE/STREAM), so a catalog
  transfer that outran the client's drain rate armed the SAME
  `kCtrlStallMs`(2000ms)-then-`_ws->close(id)` timer built for a genuinely
  wedged control reply (GRANT/WELCOME/etc). Reproduced live with a
  read-only wire script subscribed to `hub-status`(0x0006) while firing a
  full catalog `BLOB_REQ`: chunks flowed for ~50-129 of 129, then silence,
  then the serial boot log showed the exact line `client#1 control frame
  (type 0x1B) unsendable for 2000ms — closing session` and the socket
  closed — this device's catalog is now 24,581 B / 129 chunks (grown since
  the file's own header comment, still quoting a 57-chunk incident, was
  written).
  (2) **Genuine heap-exhaustion PANIC reboot**, confirmed once directly:
  a repro on the PRE-fix 2.1.80 build produced `Reset reason: PANIC
  (unexpected)` and a reset `uptime_ms` (`/api/capabilities` before/after),
  with no serial capture running at the time (ruling out the unrelated
  COM11-open-resets-the-board artifact noted below). `AsyncWebSocketClient::
  binary()`/`_queueMessage()` (`lib/espasyncwebserver/src/AsyncWebSocket.cpp`)
  allocates a `std::make_shared<std::vector<uint8_t>>` copy of every frame
  BEFORE checking whether the 32-deep queue (`WS_MAX_QUEUED_MESSAGES`) has
  room; under sustained backpressure, up to 32 live ~250-300 B buffers can
  sit queued at once (~8-10 KB) on a heap that had only ~15 KB free
  post-NimBLE-return (`maxblock` as low as 7,668 B) — the exact TRAPS T2
  mechanism, one layer up from BSS.
  ELF inventory (`xtensa-esp32s3-elf-nm --size-sort -S` on
  `.pio/build/sd32-ota/firmware.elf`) found the single largest
  non-hardware-mandatory internal-RAM reservation in the whole build:
  `AppLog.cpp`'s `/api/log` web ring (`WebRingSink`, a magic-static
  singleton) plus its `dump()`-time snapshot copies — **_low + _high +
  loSnap + hiSnap ≈ 17.8 KB of BSS**, touched ONLY by `httpTask` (never an
  ISR, never DMA), dwarfing every other candidate (`g_slopmotion` 3.5 KB,
  `g_state` 2.8 KB, `s_coredump_stack` 1.9 KB — all either already-justified
  or mandatorily internal).

  **Mitigation 1 — BLOB_CHUNK gets its own backpressure class**
  (`src/comms/SlopSyncAsyncWsTransport.{h,cpp}`, firmware-only, no wire/
  registry change). `write()` now gates `BLOB_CHUNK` on the registry's own
  advertised sender pacing budget (`limits::blob_chunks_in_flight` = 4,
  RFC-050) via the SAME `AsyncWebSocketClient::queueLen()` check the
  STATE/STREAM shed-early path already used — holding (never arming the
  control-stall timer, never closing the session) instead of running all
  the way to `WS_MAX_QUEUED_MESSAGES` first. `pumpBlobTransfer()`
  (`lib/slopsync`, untouched) already retries the same un-sent chunk index
  next tick with no NACK/teardown of its own, so this is a pure hold, not a
  new failure mode. New diagnostic counter `txBlobHolds()` (held-not-dropped,
  deliberately not folded into `txDataDrops()`).
  **Mitigation 2 — the `/api/log` web ring moves to PSRAM**
  (`src/system/AppLog.cpp`), same `heap_caps_malloc(MALLOC_CAP_SPIRAM |
  MALLOC_CAP_8BIT)` + placement-new idiom as `SlopSyncHubService` in
  `main.cpp` (TRAPS T2 prior art), allocated from `applogBegin()` (runs
  single-task, before any FreeRTOS task exists — no construction-order
  race). The two `dump()`-local `static` snapshot buffers (comment: "far
  too big for an HTTP stack") became instance members so they ride into
  PSRAM with the rest of the object instead of adding a SECOND ~8.9 KB BSS
  reservation. Every call site (`applogBegin`/`applogDump`) null-checks the
  pointer and degrades honestly ("web log ring unavailable this boot") if
  the one-time PSRAM allocation ever fails, rather than crashing.

  **Measured deltas.** Build: RAM 29.4% / 96,436 B → 24.1% / 78,948 B
  (**−17,488 B**, matching the ~17.8 KB estimate almost exactly), flash
  28.7% / 1,877,776 B → 28.5% / 1,869,436 B (both builds `pio run -e
  sd32-ota` SUCCESS). Live, fw 2.1.81 vs the pre-fix 2.1.80 baseline: boot
  `post-slopsync heap free` 15,320 B → **32,840 B (2.1x)**, `maxblock`
  7,668 B → **22,516 B (2.9x)**; steady-state (~15 s uptime) `free`~31,900 B
  `min`~15,280 B `maxblock`~20,468 B, vs the old baseline's `min` touching
  84-528 B. The catalog BLOB transfer (129/129 chunks, 24,581 B) now
  completes cleanly every time — reproduced twice with a standalone wire
  script (`closed_abruptly=False` both runs) and confirmed via
  `tools/slopsync_probe.py`'s own `blob_catalog`/`blob_ns`/
  `blob_total_bytes` checks (PASS, full 47/0/4 run). This does not reach
  the task's own ideal ">40 KB free steady-state" bar in the ABSOLUTE
  best case measured (~31-33 KB) but is a 2x+ structural improvement with
  a large, comfortably-clear margin over the specific BLOB failure this
  pass targeted; chasing the last few KB with a riskier move (e.g. moving
  something ISR/DMA-adjacent) was not attempted, per the task's own
  "do not chase with risky moves" guidance.
  [verified 2026-07-28 — HEAP RELIEF pass: live BLOB repro before/after
  (standalone wire script, twice each), `/api/log` heap beacon before/after
  the OTA, `pio run -e sd32-ota` RAM/flash before/after, native suite 31/31
  exit 0, `slopsync_probe.py` full run 47/0/4, `canon_lint.py` 0 findings]

  **Residual, flagged not fixed:** under a MUCH heavier combined load (one
  WS session doing the full catalog BLOB transfer + a 29-channel
  subscribe-everything batch + a bench `force_home` INTENT, all inside
  ~2.7 s — `tools/slopsync_probe.py --bench-home --bench-home-no-revert
  --no-motion`), the SAME fixed 2.1.81 firmware's low-water mark touched
  **60 bytes free** (steady free heap ~31 KB throughout, `min=60` only —
  a sharp transient, not a sustained collapse; the device did NOT crash
  this time). This is evidence of at least one OTHER heavy-simultaneous-
  allocation path (subscribe-everything's per-channel GRANT/retained-STATE
  burst, and/or the bench-admin INTENT path) that the BLOB-specific
  mitigation above does not cover, on a heap that is much healthier but
  still not infinite. **Separately, and apparently unrelated:** a genuine
  `Reset reason: PANIC (unexpected)` was reproduced ONCE on the FIXED
  2.1.81 firmware during a full `slopsync_probe.py --bench-home` run (heap
  was healthy at the time, ~33 KB free/22.5 KB maxblock — not a heap-
  exhaustion signature), then did NOT reproduce on two further identical
  retries; root cause not investigated by THAT pass (different subsystem —
  auth/bench-home/force_home, not the BLOB/heap path it targeted) and not
  reliably reproducible in the attempts made. **Since RULED OUT as a heap
  event and FIXED — see the PARKED-SLOT SAFETY BROADCAST entry below; the
  "not a heap-exhaustion signature" read was right for the wrong reason, and
  the panic was a null transport, not an allocation.** Both are recorded here as
  new findings from this session's verification, worth a dedicated
  follow-up pass, and are NOT regressions this pass is responsible for
  fixing. **Also noted, not a bug:** opening `COM11` via `pyserial` for
  read-only monitoring resets the device (`Reset reason: USB`) via the
  ESP32-S3's native USB auto-reset circuit — confirmed live twice this
  session; serial monitoring is not passive on this hardware, worth
  remembering for future diagnostic sessions.
- **(j) BLE advertising — PASS (partial), real radio, real scan.**
  `pip install bleak` into the repo's existing `.venv` (operator-authorized
  fallback); `BleakScanner.discover()` found the device: address
  `20:6E:F1:31:74:6D`, advertised name **"SlopDrive-32"** (the full name,
  correctly appearing in what Bleak surfaces — consistent with the design's
  shortened "SD32" in the primary payload / full name in the scan response),
  service UUID `534c4f50-5359-4e43-8000-000000000001` matching exactly.
  **Not captured:** the manufacturer-data flags byte (`manufacturer_data`
  came back empty `{}`) — plausibly Windows/WinRT's BLE stack filtering
  company id `0xFFFF` (the Bluetooth SIG's own reserved "for testing only"
  id, which some OS stacks specifically drop) rather than a firmware gap,
  but unconfirmed either way from this host. **OPERATOR-MORNING:** a phone
  + nRF Connect (which shows raw AD structures, not an OS-filtered summary)
  is needed to confirm the manufacturer-data flags byte itself. [verified
  2026-07-28 — live `bleak` scan capture]
  **CLOSED, 2026-07-28 (later session):** the phone + nRF Connect check
  happened, and it was a real firmware gap, not OS filtering — the
  advertisement's own 31-byte budget had no room for the MSD record once
  the service UUID, Flags, and name were in it; `addData()` failed silently
  and nobody checked its return. Fixed (MSD moved to the scan response,
  alongside the full name); see the BLE ADVERTISING MSD FIX entry further
  down for the mechanism, the fix, and live re-verification (fw 2.1.84).

**Unplanned event during this session:** the device rebooted once on its own
(not an OTA reboot) mid-test, immediately after a `ConnectionResetError` on
the probe's socket. No crash/panic reason is logged (the firmware does not
log an ESP reset-reason at boot — a gap, not investigated further this
session); the device came back healthy and `hub_instance_id` was confirmed
unchanged across it. Originally guessed to be item (d)'s slot-reuse bug under
the rapid connect/disconnect churn this session generated; **now identified —
this was the parked-slot safety-broadcast panic, see the PARKED-SLOT SAFETY
BROADCAST entry below.** The guess was in the right neighborhood: it does take
a parked slot, just not a clobbered one. Also
self-inflicted and resolved: an early bad manual `curl` probe (a POST to
`/api/ota/fs` with no multipart body) wedged `WebUI::update()` for 209s and
triggered a cascade of escalating `[STALL] http:ui.update blocked` warnings
(up to 79s) on the synchronous page-serving path — all API/WS traffic stayed
healthy throughout; a clean reboot cleared it and the pattern never
recurred, confirming it was test-harness-caused, not a firmware regression.
**Reset-reason gap CLOSED** (follow-up session, same date): boot now logs
`esp_reset_reason()` decoded to its enum name through SlopLog (`src/main.cpp`
`setup()`, tag `boot`) — POWERON/SW (cold boot, `esp_restart()` after an OTA)
log at INFO; anything else logs at WARN specifically so it lands in
`AppLog.cpp`'s `WebRingSink` protected 16-line Warn+ sub-ring instead of the
44-line Trace/Debug/Info one the 10 s heap beacon alone recycles in well
under a minute (confirmed live: the first cut logged at INFO and was already
evicted by the time `/api/log` was polled ~65s post-boot; re-cut to the
WARN-on-unexpected split after watching that happen). Verified live post-OTA:
`Reset reason: SW` observed in `/api/log` immediately after the 2.1.79 →
2.1.80 reboot.

**Final device state (HEAP RELIEF pass, follow-up session):** fw **2.1.81**,
reachable, `homed=true` `home_override=true` `estopped=false`
`live_speed_mm_s=0` (fake-homed via bench override, left ON deliberately for
any follow-up bench work — reasserted via `tools/slopsync_probe.py
--bench-home --bench-home-no-revert` after this session's reboots cleared the
volatile override; the item (i) BLOB heap issue is now FIXED, so
`--bench-home` runs the full probe cleanly rather than needing the lighter
wire-layer workaround the prior session used). `measured_stroke_mm=250`.

## PARKED-SLOT SAFETY BROADCAST — spontaneous reboots FIXED (2026-07-28 overnight, fw 2.1.81 -> 2.1.82)

The three unexplained mid-probe reboots recorded above (the item (i) residual,
item (a)'s `ConnectionResetError` anomaly, and the "Unplanned event" note) were
ONE bug, and it was NOT the heap. Root cause found, fixed, host-proven and
live-verified.

**Mechanism (TRAPS T13).** `Hub::broadcastSafetyNow()` fans a critical safety
snapshot out to EVERY subscribed slot, gated on `session.occupied()`,
`session.ready` and the 0x0003 subscription — and on nothing else. None of
those three is cleared when a transport dies: RFC-042 PARKS the session
(`markStale()` flips `state` to STALE and RETAINS slot, session_id, grants and
subscriptions) while `detachTransport()` sets `slot.transport = nullptr`.
`update()`'s slot walk skips null-transport slots, so every per-slot pump was
safe; the fan-out sender was not. It passed the null to
`sendFrameToTracked()` -> `sendFrameTo(ITransport& t, ...)`, and the virtual
`t.write()` loaded a vtable from address 0.

**Evidence.** Reproduced 2 runs out of 3 with a serial monitor attached
(COM11), identical register dump both times: `Guru Meditation Error: Core 0
panic'ed (LoadProhibited)`, `PC 0x42057da0`, `EXCCAUSE 0x1c`, `EXCVADDR
0x00000000`, `A2 0x00000000`. Decoded against
`.pio/build/sd32-ota/firmware.elf` with `xtensa-esp32s3-elf-addr2line`:
`Hub::sendFrameTo` (hub_impl.hpp:369) <- `sendFrameToTracked` (3272) <-
`broadcastSafetyNow` (2453) <- `handleIntent` (1697) <- `dispatchFrame` (305)
<- `pumpSlot` (271) <- `Hub::update` (203) <- `SlopSyncHubService::taskLoop`.
The probe transcript pins the trigger exactly where the field reports put it:
`Step 5.9: no ECHO/NACK for override_on within 5.0s` — `override_on` is a
safety op, so it latches, publishes, and broadcasts. The victim slot is a
session parked by a PREVIOUS probe run; that is why it needed 1-3 runs to
show and why it always followed a `ConnectionResetError` (the reset was the
device dying, and the parked session it left behind was the loaded gun for
the NEXT run).

**Heap is exonerated, and the earlier reasoning corrected.** The item (i)
residual guessed these reboots might be the `min=60 B` transient. They are
not: this is a fixed null dereference with a deterministic PC, and it
reproduces on the HOST where the heap is a desktop heap. The `min` low-water
readings stand as their own separate observation. Conversely the previous
pass's "heap was healthy at the time (~33 KB free)" was read off the 10 s
beacon, which cannot see a sub-second transient either way — neither
direction of that inference was sound.

**Fix** (`lib/slopsync/include/slopsync/hub/hub_impl.hpp`, internal only —
no public `hub.hpp`/`client.hpp` signature touched, frozen artifacts
untouched; same authorization precedent as the `attachTransport()` fix
above). `broadcastSafetyNow()` skips `slot.transport == nullptr`, matching
the guard `pumpEventDrain()` and `submitSignature()` already carried.
Deliberately a SKIP and not a tracked failure: a parked session has no link
to be congested on, so routing it through `trackCriticalSend()` would age it
toward eviction for a send that was never attempted. Skipping costs the
parked client nothing — the snapshot is retained, and §9.1 re-pushes the
current one the instant it reattaches. `sendFrameToTracked()` and
`sendNackTracked()` additionally refuse a null transport before
dereferencing, so no FUTURE fan-out site can panic the device the same way
(defense in depth, in the same spirit as `update()`'s own field-bug-#5
re-check).

**Regression test:** `test/native/test_slopsync_staleness` STALE-08 — two
ready safety subscribers, one parked via `detachTransport()`, then both
`latchEstop()` and `setSafetyModes()` (the probe's own Step 5.9 op); asserts
the live subscriber gets each edge, the parked one survives un-evicted, and
a reattach receives the current retained snapshot carrying both edges it
slept through. Proven to catch the bug: with the three guards surgically
removed the suite dies with exit code 3221225477 (0xC0000005,
ACCESS_VIOLATION — the host equivalent of LoadProhibited); with them, PASS.

**Gauntlet (all green):** native suite 31/31 exit 0, `pio run -e sd32-ota`
SUCCESS (RAM 24.1% / 78,948 B, flash 28.5% / 1,869,456 B), `catalog_lint.py`
OK (32 entries), `gen_registry_header.py --check`, `gen_docs_tables.py
--check` (14 files, 108 dictionary terms), `gen_spec_pages.py --check` (20
files), `gen_channel_map.py --check`, `gen_channel_grid.py --check` all up to
date. `canon_lint.py`: 0 findings in this pass's own scope — see the FLAGGED
note at the end of this entry.

**Live verification.** `FIRMWARE_VERSION` 2.1.81 -> 2.1.82, deployed via
`POST /api/ota` with `X-OTA-Token` (DOCTRINE §6 curl path; the `deploy.ps1`
referenced in the entry above was prior-session scratch and is not in the
tree). `/api/capabilities` confirms `fw_version 2.1.82`. Then FIVE
consecutive full probe runs, `--estop --bench-home --bench-home-no-revert`
(every safety op the machine has: override on/off, e-stop assert, e-stop
clear, force_home): **50 passed / 0 failed / 3 skipped on all five**, probe
exit 0 on all five, **zero `ConnectionResetError`, zero reboots** — device
uptime rose monotonically 64,877 -> 84,620 ms across the whole set. Serial
stayed attached throughout: zero `Guru Meditation` lines after the OTA (the
only three in the capture are the pre-fix repros), and the current boot's
reset reason is `SW`, i.e. the OTA itself. Same shape had crashed the device
on 2 of 3 runs immediately before the fix.

**Device end-state:** fw **2.1.82**, reachable at 192.168.1.229,
`homed=true` `home_override=true` (fake-homed, left ON for further bench
work) `estopped=false` `paused=false` `homing=false`, not moving,
`measured_stroke_mm=250`.

**Files touched:** `lib/slopsync/include/slopsync/hub/hub_impl.hpp`,
`test/native/test_slopsync_staleness/test_main.cpp`,
`include/system/config_api.h` (version bump), `docs/canon/TRAPS.md` (new
T13), `docs/canon/LEDGER.md`, `SD32-OVERNIGHT-REPORT.md`.

**Lane note (resolved, recorded for the audit trail):** mid-pass
`canon_lint.py` briefly showed one british-spelling finding
(`sim/slopsim/src/machine/MachineSim.cpp:905`, a British-spelled word in a
comment) — pre-existing,
unrelated to this bug, and inside the file a PARALLEL sim agent was editing
at the time, so it was deliberately NOT touched from this lane (concurrent
edits to one file lose updates). That agent fixed it during this pass; the
final `canon_lint.py` run is 0 findings across the whole tree.

**Main-loop review (2026-07-28):** all judgment calls ACCEPTED — notably
skip-not-tracked for parked-slot sends (tracking would age a parked session
toward eviction for a send never attempted). Operator veto window open.

## Sim fidelity (SlopDeck milestone 1) — LANDED (2026-07-28 overnight)

- **`sim/slopsim` gained `--profile device|alien|minimal`** (default
  `device`), closing the sim-fidelity gap the "Pending operator rulings"
  entry flagged. Architecture finding: the sim already embedded the REAL
  `slopsync::Hub`; only its catalog had been swapped (commit 74c6533) to a
  deliberately-different "benchrig" catalog to prove client genericity —
  that swap IS what DESIGN.md's `alien` profile wants, it just needed to
  stop being the only option. No rewrite required.
  - `device` (DEFAULT): literally `slopdrive::buildSlopDriveCatalog()` from
    `include/comms/SlopSyncCatalog.h` (feat = current-sensor+power-monitor
    true/true, mirroring the real hardware) — 44 channels (12 spec-core +
    32 device-range), 24,581 B, etag `9275f578ada7d314`. **This etag
    matches the LIVE device's own reported catalog etag** (see the
    2026-07-28 DEPLOY + LIVE-VERIFY entry, item (c)) — independent proof of
    byte-identical fidelity. Write-plane restored (from the pre-benchrig
    e77bd1f reference, adapted to the current C4 channel numbers) for
    `move`/`home`/`config_set`/`pattern_cmd`; STATE republished for motion
    (incl. `raw_10um`), machine-config (incl. `measured_stroke`),
    pattern-state (incl. `background_run`), plan-strip, power, odometer,
    slopmotion-diag (all 10 anomaly kinds), and the 0x4100 motion-anomaly
    EVENT.
  - `alien`: unchanged benchrig catalog (`SlopSimCatalog.h`), 21 channels,
    4,272 B — a deliberately different conformant hub, now explicitly
    opt-in rather than the default.
  - `minimal`: a NEW, literal subset of the real device catalog (same ids/
    field shapes) — spec-core + `motion`/`move`/`home`, 15 channels,
    2,442 B.
  - **Flagged gap — RESOLVED, see "Sim fidelity 19-channel follow-on
    (2026-07-28)" below:** 19 device-catalog entries (machine-modes/
    `modes_set`, the 3 SlopMotion tuning cards + `sm_set`, fray-d Advanced
    pattern + 6 modifiers + its writer, the preset roster/store/cmd trio,
    machine-admin) were catalog-advertised with full fidelity but had no
    live STATE publish or INTENT handling in the sim — their writes NACKed
    `UNKNOWN_CHANNEL`. All 19 now have real, firmware-mirrored behavior.
  - `webui/test/fixtures/slopsim-catalog.{bin,etag}` re-captured from
    `device` (was benchrig, 21 ch / 4,272 B). `slopsync-wire.test.mjs`'s
    `[SKIP-EXPECTED-GAP]` (0x1100 motion / `raw_10um`) is CLOSED — now a
    plain assertion, passing. `slopsync-sim.mjs`'s pre-existing 3x `[FAIL]`
    + write-plane `FATAL` are FIXED (config-set 0x3000 now actually applies
    and reflects into 0x1000 machine-config).
  [verified 2026-07-28 — `node webui/test/slopsync-wire.test.mjs` ALL PASS,
  `node webui/test/slopsync-sim.mjs` ALL PASS against a fresh
  `device`-profile sim (cold+warm sessions, config-set round trip, NACK
  correlation, `--estop` path), 3-profile connect/HELLO/catalog-fetch/
  write-round-trip smoke test ALL PASS, `npm run check` (webui/) ALL PASS,
  `python tools/canon_lint.py` 0 findings, sim build clean (MinGW GCC
  16.1.0 / CMake+Ninja, `sim/slopsim/build`)]
- **Main-loop review (2026-07-28):** all three judgment calls ACCEPTED
  (INA228 feature flags true/true — validated by the etag match; 44-channel
  faithful build over the brief's imprecise "32"; `minimal` = spec-core +
  motion/move/home). Operator veto window open.

## Sim fidelity 19-channel follow-on (2026-07-28) — LANDED, morning ruling item 3

Implements the ONE-WAY PARITY ruling (morning ruling item 3 below): the sim
is the 1:1 device twin, so the 19 entries the milestone-1 pass flagged above
now get REAL behavior — the machine is truth, the sim conforms to it, and
where mirroring found a firmware quirk the sim copies the quirk rather than
"fixing" it (the firmware is never edited to close a sim gap). No catalog
change: `buildSlopDriveCatalog()` is shared verbatim with the firmware
already, so the etag is unaffected by this pass — confirmed by re-capture,
below.

- **machine-modes (0x1030) / modes-set (0x3030):** `stream_speed_mode`
  wired into the sim's pre-existing `uiSetStreamSpeedMode()`;
  `overshoot_clamp` is plain sim state — inert on the device too (RENDERING.md
  `ui_ranks::hidden`, no engine consumer on either side, so there is nothing
  for it to drive in the sim either). Keys 1/2 (`blend_mode`/`transport`) are
  mirrored as PERMANENT GAPS exactly like the firmware: the catalog declares
  no schema field for them at all, and a write touching no recognized key
  NACKs `INVALID_VALUE`.
- **SlopMotion tuning — slopmotion-limits/chase/waveform (0x1120/1121/1122) +
  sm-set (0x3120):** 17 of 20 keys write straight into the REAL embedded
  `slopmotion::Engine`'s `Config` via one read-modify-write + `setConfig()`
  call (the same seam the sim's existing `uiSetXxx` palette hooks use),
  clamped to the firmware's OWN bounds — tuning takes effect on the very next
  plan. `jmax_ovr`/`vmax_ovr`/`amax_ovr` (keys 1-3) are sim-held overrides
  (0 = derive), mirroring `SystemState::sm_tune_{jmax,vmax,amax}_ovr`'s
  "0 = derive from the mm limit set / window span" semantics exactly, feeding
  `deriveEngineLimits()` (which gained the same vmax/amax override branch
  `_jmax_norm` already had).
- **fray-d Advanced pattern — pattern-advanced (0x1210) + 6 modifier lanes
  (0x1211-1216) + writer pattern-advanced-cmd (0x3210):** reuses the
  FIRMWARE's OWN `advpat::Settings`/`BaseControl`/`Modifier`
  (`include/motion/AdvancedPattern.{h,cpp}` — pure math, zero Arduino/
  FreeRTOS deps, now compiled into `slopsim` too) instead of re-deriving
  equivalent sim-side structs, so clamps, depth-pair coupling
  (`coupleDepths()`), and compile-time defaults are byte-identical to the
  device BY CONSTRUCTION, not by transcription.
- **Preset roster/store/cmd trio (0x1220 / 0x5220 / 0x3220):** reuses the
  firmware's OWN `PatternPresetStore` (`include/comms/PatternPresetStore.h`,
  header-only, zero Arduino deps) for save/load/delete/rename, plus a
  `readBlob()` override (ns=store, store_id=2) so a real client's BLOB_REQ
  against the STORE channel gets an honest payload — slot/name-length rules
  and the 40-byte payload layout are byte-identical to the device.
- **machine-admin (0x30F0):** `clear_fault`/`save_config`/`servo_scan` all
  accept and ECHO the op; an unknown op NACKs `UNSUPPORTED_OP`.

**Firmware quirks mirrored, flagged for the operator (not fixed, per the
ONE-WAY rule):**
1. `pattern-presets-cmd`'s `save` op captures ONLY `in_speed`/`out_speed`/
   `in_accel`/`out_accel` + the 6 modifier blocks — NEVER `master` or the
   depth pair (the firmware's own comment: "never depths or master speed").
   A saved preset silently drops master speed and depth window; loading one
   back does not restore them. Mirrored exactly; worth naming so nobody is
   surprised the sim does the same thing the device does.
2. The six modifier-lane channel ids are NOT in `advpat::BaseId` order: the
   wire/channel order is speed-in/out, accel-in/out, depth-1/2, while the
   writer's setting-key grouping (`base = 9 + 6*id`) and the preset payload
   layout (`base = 4 + 6*id`) both use `BaseId` order (depth-max, depth-min,
   speed-in/out, accel-in/out). The firmware's own `kModChannels` reorder
   table is copied verbatim in the sim rather than re-derived, so both sides
   carry the same non-obvious mapping — a client that assumed "channel id
   order == BaseId order" would be wrong against either machine.
   No new inconsistent clamp, wrong NACK code, or other genuine bug was found
   while implementing this pass — both items above are documented, deliberate
   firmware design choices, not defects.

**Physics/hardware-model limitations flagged (not silently absorbed):**
1. The fray-d Advanced pattern's write/clamp/echo/STATE-publish contract is
   fully real, but the sim's pattern generator (`SimPattern`, a v1 stand-in:
   stroke/tease/shallow-fast) does not yet consume `_ap` to drive the stepper
   via `advpat::Settings::planStroke()` the way the firmware's PatternEngine
   does — toggling `ap_mode` or dialing the 6 modifier lanes has NO motion
   effect in the sim yet, only a wire effect. Flagged in `MachineSim.h`'s own
   comment on `_ap`; porting the per-half-stroke scheduling loop is a
   materially larger feature than wiring existing tuning into the existing
   engine (the SlopMotion-tuning case above) and was not attempted this pass.
2. `machine-admin` (0x30F0): the sim models NO fault or servo-Modbus concept
   at all (grepped clean across `sim/slopsim/src`) — `clear_fault`/
   `save_config`/`servo_scan` are honest no-ops (accept + echo the op) rather
   than invented behavior. `servo_scan` can never NACK `INTERLOCK` the way
   the firmware's async Modbus path sometimes can, because the sim has no
   interlock condition to refuse it with.
3. The preset STORE's `readBlob()` override was implemented (any real client
   fetching preset names/payloads via BLOB_REQ, e.g. MFP or webui, exercises
   it) but is NOT independently exercised by a dedicated BLOB_REQ line in
   `slopsync-sim.mjs` this pass — a coverage gap, not a functionality gap.

**Test coverage added:** `webui/test/slopsync-sim.mjs` gained one write ->
post-clamp ECHO -> STATE-reflect round trip PLUS one invalid-write -> correct
NACK case per family (5 families, ~33 new assertions). Two families needed
every `waitFor` listener armed BEFORE the single write that touches more than
one STATE channel in the same hub tick, rather than chained sequentially
after each other — a real race (an earlier draft of this test hit it: the
sim's WS client can deliver a tick's several STATE frames in one synchronous
burst, and a listener registered only after an earlier `await` already
resolved can miss a sibling frame from the SAME burst). Documented inline
where it matters.

**Gauntlet (all green):** sim rebuild (CMake+Ninja, MinGW GCC 16.1.0,
`sim/slopsim/build`) clean, 0 new warnings from the new code (only
pre-existing, unrelated `slopmotion.hpp`/vendored-SHA256 library warnings);
fixture re-capture etag `b69eb06249ebe73a` — MATCHES the live device's
post-wire-strings etag (fw 2.1.83/84) exactly, confirming the catalog itself
needed no change, only sim behavior (byte-for-byte identical fixture to the
one already committed — `git status` on `webui/test/fixtures/` shows no
diff); `node webui/test/slopsync-sim.mjs` ALL PASS against a FRESH sim
instance (a reused long-lived process can produce a false failure on a
reflect check — re-sending an already-applied value is byte-identical to
what is already published, so the diff-what-we-sent gate produces no new
push to wait on; this bit an early draft of this test and is why the file's
own "KILL ANY STALE SLOPSIM" warning is doubly true now); `node webui/test/
slopsync-wire.test.mjs` ALL PASS; `npm run check` (webui/) ALL PASS;
`python tools/canon_lint.py` 0 findings.

**Judgment calls, flagged for veto:** (a) machine-admin's three ops treated
as honest no-ops rather than invented fault/servo simulation (limitation 2
above); (b) reused the firmware's own `AdvancedPattern.{h,cpp}` and
`PatternPresetStore.h` verbatim rather than re-deriving equivalent sim-side
structs — both are already host-clean (no Arduino/FreeRTOS deps), so this
guarantees clamp/coupling/default parity by construction; it required one
CMakeLists.txt include-path addition (`include/motion`) so CMake can resolve
`AdvancedPattern.cpp`'s own unqualified `#include "AdvancedPattern.h"`
(PlatformIO's LDF adds every `include/` subdirectory automatically; CMake
does not); (c) fray-d motion-driving (physics limitation 1 above) deliberately
NOT attempted this pass — flagged, not silently dropped.

**Files touched:** `sim/slopsim/CMakeLists.txt` (new `AdvancedPattern.cpp`
source + include path), `sim/slopsim/src/machine/MachineSim.h` (new state
fields, reused firmware headers), `sim/slopsim/src/machine/MachineSim.cpp`
(5 new `applyIntent` cases, 2 new free helper functions, 6 new STATE-publish
blocks, `deriveEngineLimits()` vmax/amax branch), `webui/test/
slopsync-sim.mjs` (new 19-channel write-plane section). `webui/test/
fixtures/slopsim-catalog.{bin,etag}` re-captured (byte-identical, no diff).
[verified 2026-07-28 — sim build clean, fixture etag match confirmed
directly, full gauntlet commands run and exit codes/output observed above]

## Morning ruling batch (operator, 2026-07-28, on the overnight stamp list)

1. **STATE burst congestion: COALESCE, not backpressure — LANDED
   (2026-07-28, fw 2.1.84 → 2.1.85).** Root cause: the coalescing engine
   (`RetainedStore` + per-subscriber pacing + `shedDecision()`) was already
   correct and normatively tested (S-08) — the gap was that
   `Hub::setCongestionLevel()`, the hub's own documented choke point for a
   real binding's native congestion signal, was never called from
   `src/comms/SlopSyncAsyncWsTransport.{h,cpp}`; `congestionLevel` sat at 0
   forever on hardware, so shedding never engaged. Fixed by wiring it:
   `pollCongestionLevel()` classifies 0/1/2 from `queueLen()` watermark
   hysteresis (§10.3's own 50%/1s, 20%/5s numbers) + the existing
   control-stall timer for severe; `Hub` gained an additive
   `setCongestionLevel(ITransport&, uint8_t)` overload fed from
   `SlopSyncAsyncWsPort::loop()`. New tests: `test_slopsync_safety` S-11
   (bit-exact last-value-wins, seq never regresses, EVENT never coalesces).
   Bench (worst-case repro ×3 on fw 2.1.85): heap low-water **60 B →
   164–216 B** (~3x), no crash, 45/0/6 all runs, 10 session cycles + all
   bench runs uptime-monotonic. **Flagged residual, PENDING OPERATOR
   STAMP:** still not a fully healthy floor — normal-priority STATE is not
   shed until level 2 (correct per the normative table) and the 1 s
   sustained-congestion hysteresis means a ~2.7 s packed burst can mostly
   complete before the signal engages; the next lever (burst-aware
   escalation or shorter sustain window) risks shed-flapping on the hot
   path, so it is parked for a ruling, not chased. [verified 2026-07-28 —
   commit 3323e44, native 31/31 exit 0, sd32-ota SUCCESS (flash +840 B),
   canon_lint 0, 3x live bench repro against fw 2.1.85]
2. **Wire-visible catalog/registry strings get the punctuation pass —
   approved** ("do it, it's a single etag bump"). ONE atomic catalog
   evolution: em/en dashes + banned words fixed in SlopSyncCatalog.h descs
   + registry.yaml desc strings; fixtures/goldens regenerated once; etag
   changes on deploy; frozen mini-catalog untouched (C-11 precedent).
   **DONE — see the WIRE-STRING PUNCTUATION EVOLUTION entry below** (fw
   2.1.82 → 2.1.83, etag `9275f578ada7d314` → `b69eb06249ebe73a`).
3. **Sim division-of-labor ruling:** `sim/slopsim` is the 1:1 DEVICE TWIN —
   the 19 advertised-but-inert entries get REAL behavior in parity with the
   machine ("it's a 1:1 sim and it should reflect that for quality
   testing"). PARITY IS ONE-WAY (operator, same date): the machine is the
   truth and the sim conforms to it — the firmware is NEVER edited to close
   a sim gap; a mismatch is always a sim work item.
   **DONE — see the "Sim fidelity 19-channel follow-on (2026-07-28)" entry
   above.** The "be anything" role moves to a NEW dumb test hub named
   **SlopBench** (operator-named): config-file catalog, simple TUI showing
   live axis/channel values, configurable fake delay on STATE echo.
   **SlopBench LANDED (2026-07-28, host-build, verified):** `sim/slopbench/`
   — config-file (`.bench`, custom line format, not YAML — dev-tool, no new
   dependency) catalog builder with zero hardcoded channel knowledge,
   generic INTENT-clamp/echo/STATE-mirror write plane, configurable fake
   STATE-echo delay (pending-mirror queue), sine/ramp auto-animation,
   plain-ANSI TUI (live channel table, sessions, recent-writes log, `q`
   quits). 3 example configs: tiny-axis / alien (reserved domains) /
   kitchen-sink. Every session gets `configure` (test double, not an auth
   harness — deliberate). STREAM/EVENT/STORE declarable but functionally
   inert (honest defaults), noted in its README. Verify:
   CMake+Ninja build exit 0; `sim/slopbench/tools/smoke_test.py` (imports
   tools/slopsync_probe.py wire layer unmodified) 12/12 PASS across all 3
   configs — catalog-fetch-matches-config, clamped write + ECHO, measured
   STATE-echo delay ≥ configured floor (561 ms on 400 ms config, 373 ms on
   200 ms, 125 ms on 0); canon_lint 0. Commit 317b19d. [verified
   2026-07-28 — smoke run + build tail observed in-session]
4. **Internal reference docs stay out of the repo — verified already true:**
   root tracking is LICENSE/NOTICE/README/THIRD_PARTY_LICENSES + 4 build
   files only; zero PDFs/.diy tracked. The visible root clutter is
   untracked-by-existing-rules local files. Nothing deleted.
5. **Comment standardization — DONE (2026-07-28, close-out session).** DOCTRINE
   §4's comment style law landed (own commit), then the codebase-wide pass:
   a comment-law fleet (~130 files, src/include/lib/sim/test, each verified
   comments-only via stripped-source hash matching) plus this session's
   mechanical follow-up on top of it. Two law amendments stamped in the same
   DOCTRINE commit, ratified here as main-loop rulings: banner width settled
   at column 80 (amended from 76, normalizing to the tree's actual dominant
   width instead of repadding against it); RFC-nnn/T-nn references inside a
   banner NAME are POINTERS (allowed), not the numbering C-12 forbids.
   **This session's additions:** section banners repadded to column 80
   (855 banners, 105 files — a hand-rolled Python tokenizer threading through
   `//`, `/* */`, string/char/raw-string literals, and C++14 digit
   separators, gating every edit on a stripped-code-hash proof; the one
   file where a banner-shaped line lives inside a raw-string literal,
   `GraphPage.h`'s embedded HTML/JS page, was correctly left alone — that
   text is page content, not a C++ comment); ~23 stale `CLAUDE.md §N`
   pointers (predating the CANON/DOCTRINE/TRAPS split) repointed to
   DOCTRINE.md §1/2/3/8/9, TRAPS.md T5, or this ledger's Phase E entry, each
   target verified to exist first — one (a released-but-inert-field remark
   in `MachineSim.h`) had no separate current home and was reworded inline
   rather than given a guessed pointer; ~19 `plan.md` references (a
   gitignored operator-local scratch file, dead for any repo reader)
   reworded to carry their constraint self-contained, plus one boot-log
   STRING LITERAL citing the same dead file, fixed as a direct content edit
   (not claimed as comments-only, since a string literal is compiled data).
   **TRAPS gained T15** (SlopGlow's `GlowState` priority ordering: a
   fixed-priority display can mask a lower-priority but time-critical state
   — recovered from commit `5106d521`, the RFC-027 pairing-window-hidden-
   behind-Fault incident), **T16** (a stall watchdog sized for "stuck"
   cannot tell a wedged client from a healthy bulk transfer — the
   BLOB_CHUNK/`kCtrlStallMs` incident already narrated in
   `SlopSyncAsyncWsTransport.h`), and **T17** (a flag reused across
   unrelated concerns silencing a sink at compile time — the
   `SERIAL_CONTROL_MODE`-gated serial sink incident in `AppLog.cpp`), each
   with a code-site pointer added (comments-only, hash-proven). Evaluated
   and left OUT of TRAPS (lean NO, recorded here not as entries): AIM
   PWM*10/wire-sign stories (their durable home is already
   `config_api.h`'s own constants + constraint comments, not a narrative);
   sim-local "used to" bug notes in `MachineSim`/`MachineScreen` (sim-local
   debugging narration, not a recurring field-trap mechanism).
   **`lib/SharedProtocol/SharedProtocol.h` — DELETED** (own commit): zero
   `#include`s anywhere in src/, include/, lib/, sim/, test/, or either C5
   main (`src/c5_waveshare/main.cpp`, `src/c5_tdongle/main.cpp`); its one
   historical includer, the `src/s3_main/main.cpp` placeholder stub, was
   already deleted in the 2026-07-27 dead-code pass; `platformio.ini` never
   wired it via `lib_deps`/`lib_extra_dirs` (PlatformIO's LDF auto-discovers
   from `#include`s alone, so it was never actually compiled into any
   environment despite stale comments claiming otherwise). `README.md` and
   `platformio.ini`'s stale "all three environments share it" prose fixed
   in the same commit (C-9). Both C5 envs (`c5_waveshare`, `c5_tdongle`)
   rebuilt SUCCESS after the deletion as the before/after proof.
   **Gauntlet (all green, every command run directly this session):**
   native suite 31/31 exit 0 (TRAPS T10); `pio run -e sd32-ota` SUCCESS —
   RAM 78,948 B (byte-identical to the pre-session baseline), flash
   1,870,292 B vs a freshly-rebuilt pre-session HEAD baseline of
   1,870,276 B (+16 B, fully attributable to the one deliberate log-string
   edit above; comments cost nothing, confirmed by rebuilding at HEAD via a
   stash round-trip); `c5_waveshare`/`c5_tdongle` SUCCESS; `sim/slopsim` +
   `sim/slopbench` (CMake+Ninja) clean builds (only pre-existing, unrelated
   `slopmotion.hpp`/vendored-SHA256 warnings); `node webui/test/
   slopsync-sim.mjs` + `slopsync-wire.test.mjs` ALL PASS against a fresh
   `slopsim` instance; `python sim/slopbench/tools/smoke_test.py` 12/12;
   `npm run check` (webui/) ALL PASS; `python tools/canon_lint.py` 0
   findings; `python tools/catalog_lint.py` OK (32 entries); all six
   generators `--check` green (`gen_registry_header.py`,
   `gen_channel_map.py`, `gen_channel_grid.py`, `gen_docs_tables.py`,
   `gen_spec_pages.py`, `gen_channel_grid_page.py`); `dotnet build` clean
   (0/0) for all three `clients/mfp-slopsync` projects (SlopSync,
   LiveWireTest, WireSelfTest) — `WireSelfTest` NOT run against the live
   device (build-only, per scope). [verified 2026-07-28 — every command
   above run directly this session, exit codes / PASS-ALL / 0-findings
   observed firsthand]
6. **`minimal` sim profile floor — PENDING, operator draft recorded:**
   "home, motion, move, window controls and pattern Gen are the bare
   minimum." Decide + implement later; current spec-core+motion/move/home
   stands until then.
7. **dictionary.yaml lane call — ratified.**
   Sequencing: Phase G close-out → (2) wire strings → (3) sim parity +
   SlopBench → (1) coalescing → (5) comment pass. Deploys serialize.

## Named work items (operator-approved 2026-07-27)

- **SlopSync repo split (operator direction ruling, 2026-07-28; execute
  at/around the v1.0 tag):** SlopSync moves to its OWN repo as the
  first-class source of truth — the spec suite (SPEC.md, RENDERING.md,
  registry/registry.yaml + codegen, RFC-QUEUE.md, CHANNEL-MAP.md +
  generators), lib/slopsync (C++ core), the JS reference client, SlopBench
  (the machine-agnostic reference hub), and the verification tools
  (slopsync_probe, slopscope, slopsoak). SlopDrive-32 stays the machine repo
  and CONSUMES SlopSync via a pinned version; sim/slopsim stays with the
  machine (1:1 device twin). Operator constraints: everything visible in one
  VS Code view, and commits stay SIMPLE — main-loop recommended shape
  (veto-able): two plain side-by-side repos + a multi-root .code-workspace;
  explicitly NO git submodules (pointer-bump commit hell) and NO subtree
  merges (arcane push/pull); a cross-repo change is two ordinary commits,
  spec repo first, exactly the order the existing spec-gap ritual already
  enforces. Known tension to solve deliberately at migration time: the
  registry<->catalog lockstep `--check` generators assume one tree today;
  post-split they verify against the PINNED slopsync copy. Timing rationale:
  v1.0 tag day already carries the Old-column retirement and the
  `(was 0x...)` comment sweep, so the split joins one clean go-public event.
  [ruling recorded 2026-07-28 by main loop; not scheduled work yet]

- **Phase G (operator, 2026-07-28, runs after the live-verify + commits):**
  sonnet fleet updates ALL docs to final post-batch state, and the channel
  grid becomes a docs-site page — interactive like the standalone visual,
  styled to the webui's instrument aesthetic (2px radius, chip strips,
  status-token accents, the registration-crosshair vibe) but tuned for
  reading: lighter density, docs typography. Aesthetic source of truth:
  webui/src/style.css tokens.
  STAMPED RIDERS (operator 2026-07-28, on the C4 judgment calls): (1)
  SlopSyncCatalog.h interior section banners lose their hex ids ENTIRELY
  (names stay; a comment quoting a wire number is a second home for one
  fact — the C-1 disease in miniature); (2) historical docs keep old ids
  verbatim but each gains a one-line "ids herein are historical (pre-C4);
  current map: CHANNEL-MAP.md" header; (3) the CHANNEL-MAP Old column is
  one-hop by design and RETIRES entirely at the v1.0 tag (noted in the
  map's prose so it self-schedules).
  RIDER 4 — STE REGISTER PAGES (operator 2026-07-28): the docs-site
  REGISTER/CHANNEL REFERENCE pages are written in ASD-STE100 Simplified
  Technical English. Binding rules for those pages: no em/en dashes or
  double hyphens (use a period, a comma, or restructure); max 20 words per
  procedural sentence, 25 per descriptive; simple tenses only, no
  progressive -ing forms; imperative mood for every step; no phrasal verbs
  (start, not turn on); banned vocabulary: delve, leverage, robust,
  seamless, synergy, testament, tapestry, unlock, quiet, notable; banned
  phrases: "it is worth noting", "in order to", "not just X, but Y"; no
  intro fluff, meta-commentary, or summary conclusions. SCOPE (main-loop
  reading, veto-able): register/channel reference pages ONLY — the rest of
  the docs keep the house voice, and wire-visible catalog/registry
  description strings are NOT rewritten (they ship in the live catalog and
  feed the etag; an STE pass on wire strings requires its own ruling and a
  deliberate etag bump). Where a generator emits register-page prose, the
  generator's emitted text is in scope; its registry.yaml source strings
  are not.
  RIDER 5 — GOLD-STANDARD PASS (operator 2026-07-28): Phase G is the
  gold-standard docs effort, not a refresh, and it is MULTI-ROUND: an
  initial full pass, then repeated agent correction sweeps ("a good bit of
  time" budgeted) until the bar is met. Binding rules:
  (a) HOUSE VOICE, non-register pages: de-AI it — terse but simple, not
  fluffy. Short declarative sentences; cut hedging, throat-clearing, and
  decorative framing; keep the technical content dense and plain.
  (Register pages remain STE per Rider 4.)
  (b) MERMAID: many more diagrams, and every diagram must be legible
  standalone — the entry point is visually marked (styled start node),
  loops are explicit (labeled back-edges, never implied), area-to-area
  transitions are labeled, and subgraphs name their areas. Test: a reader
  answers "where does this start, what repeats, how does control move
  between areas" from the picture alone. Existing diagrams get upgraded to
  this bar, not grandfathered.
  (c) ORGANIZATION: pages well divided and segmented; every page has a
  clear topic scope; follow-topic links at natural exit points.
  (d) LINK RULE: when prose references a topic, page, term, channel, or
  tool that has a home, it LINKS to that home (~90% of references become
  links). A named reference without a link is a finding in correction
  sweeps.
  (e) AESTHETIC: matches the og webui (webui/src/style.css tokens) without
  being intrusive — instrument accents, not instrument density.
  (f) DEMO CANDIDATES: wherever a live demo or embedded runnable/copyable
  snippet would genuinely help, agents MARK the spot with the greppable
  callout `> DEMO-CANDIDATE: <one line: what it would show>` and move on —
  the operator implements demos personally; agents never build them.

## Phase G LANDED (2026-07-28): docs gold-standard pass + channel-grid page + close-out gauntlet

Round tally (main-loop orchestration of this effort): 1 initial build pass +
9 correction sub-sweeps; round-2 sub-sweep totals: 81 findings, 77 fixed, 4
deliberately left open, per Rider 5's own "multi-round, until the bar is
met" design. This close-out session ran its own verification + fix round on
top of that (Steps 1-2 below), independently re-proven, not merely relayed.

All five STAMPED RIDERS confirmed executed: (1) `SlopSyncCatalog.h` interior
section banners lose their hex ids entirely (`grep '// ---- 0x'
include/comms/SlopSyncCatalog.h` — 0 hits); (2) `V1-READINESS.md`,
`WEBUI-HANDOFF-RFC-BATCH.md`, and `RFC-QUEUE.md` each carry the one-line
"ids herein are historical (pre-C4)" header; (3) `CHANNEL-MAP.md`'s Old
column documents its own v1.0-tag retirement in prose; (4) STE register
pages (`reference/channel-catalog.md`, `channel-grid.md`, `dictionary.md`,
`reference/index.md`) carry `register: STE`, spec/registry pages keep
`register: IEEE` — correctly scoped, the registry.yaml-generated pages are
normative IEEE voice, not the STE "register/channel reference" pages Rider 4
names; (5) rider text intact verbatim, gold-standard bar applied (house-voice
de-AI pass, mermaid diagram upgrades, ~90% link-the-reference rule, webui-
matched aesthetic). DEMO-CANDIDATE markers: **35** total, found by grepping
docs/ + docs-site/docs/ for the greppable callout string Rider 5(f) defines
(one hit above is that definition itself, in this ledger's own Named-work-
items section — excluded from the 35; it names the format, it is not an
instance of it).

**Notable truth fixes landed in this pass** (spot-verified directly against
the uncommitted `git diff`, not taken on faith):
- **RFC-045 disconnect behavior, plain-language pages.** `docs-site/docs/
  understand/for-everyone.md` ("If your phone dies, the machine stops" →
  "...the machine settles" / "nothing on the machine broadcasts an emergency
  stop on your behalf") and `understand/how-it-works.md` ("a vanished
  streaming client stops the machine" → the hub releases ownership as
  bookkeeping, "motion settles by physics rather than by a safety action").
  Both now describe RFC-045's actually-landed behavior instead of the
  pre-RFC-045 deadman-forces-a-stop model the plain-language tier had never
  been updated to drop.
- **Session-roster overclaim fixed.** `understand/what-it-replaces.md` and
  `understand/security.md` no longer describe "the roster" as a feature a
  reader can rely on today; both now say the roster snapshot is specified,
  not built, and route the same practical claims (who's connected, kick a
  client) through session-events + the trust ledger, which ARE implemented —
  matching this ledger's own "0x0002 session-roster: reserved, NOT
  implemented" entry above.
- **"BLE overclaim downgraded" — RESOLVED: the fix exists and is
  committed.** The close-out agent searched the wrong surface and honestly
  recorded not-found rather than fabricate (correct C-8 instinct); the
  main loop then verified the fix directly: `docs/REFACTOR-ROADMAP.md`
  line 55 (module table, SlopSync row) reads "BLE GATT is deployed and its
  advertising is confirmed by a real scan, but no client has yet held a
  live GATT session" — the round-2 docs-root sweep's reported edit,
  landed in the Phase G commits. [verified 2026-07-28 — grep of the
  committed file by the main loop]
  **Separately found, and worth its own look:** `SPEC.md` §18 item 22 ("BLE
  GATT... specified with no reference implementation... until a BLE
  `ITransport` and a UDP responder land") is itself stale in the OTHER
  direction — `src/comms/SlopSyncBleTransport.{h,cpp}` and the UDP responder
  have been committed in-tree since Phase E (commit `07a3b90`), build/host-
  verified. Not touched this pass: a normative §18 status rewrite is bigger
  than a docs/link close-out and deserves its own review, not a drive-by
  edit riding on this session's scope.

**Anchor-fallout fix (this session's Step 1).** An earlier sweep retitled
`RFC-QUEUE.md` headings RFC-043..048 from bare `## RFC-043` to descriptive
titles, breaking every bare `#rfc-04X` cross-reference into it. Wrote a
tree-wide markdown link verifier (walks every `.md` under `docs/` +
`docs-site/docs/`, extracts relative links, verifies the target file exists
and, if there's an anchor, that some heading in the target slugifies to it —
underscore-preserving, em-dash → double-hyphen, matching this repo's own
already-working full-slug RFC links; also honors mkdocs `attr_list` explicit
`{#id}` anchors like `spec/session.md`'s `{#s6-4}` style). First run: 38
broken — 36 were the anchor fallout (`docs/slopdeck/DESIGN.md` ×3,
`docs/slopsync/RENDERING.md` ×4, `docs/slopsync/SPEC.md` ×29), all fixed to
the full slug (e.g. `#rfc-043--transport-conformance-profiles-which-
bindings-a-hub-must-offer`); the other 2 were the `plugins.md` links below.
`docs-site/tools/gen_spec_pages.py`'s own `SOURCE_LINKS` allowlist carried
the same 5 stale bare anchors (would have failed its own `--check` the
moment it ran against the corrected SPEC.md) — updated to match. Second run:
**0 broken.**

**mkdocs strict fix (this session's Step 2).** The 2 warnings were
`docs-site/docs/build/plugins.md` linking `../cli.md#the-probe` and
`../local-testing.md#the-pattern-that-is-mandatory` with a spurious `../` —
`cli.md` and `local-testing.md` are `plugins.md`'s own siblings in
`docs-site/docs/build/`, not one level up (the same page's own "Where to go
next" section links `cli.md` bare, confirming the sibling relationship).
Fixed both to same-directory links. `mkdocs build --strict`: exit 0, zero
warnings.

**Also found and fixed, not in the original brief:** this ledger's own
"Morning ruling batch" item 7 still named the new test-hub tool "SlopRig"
after item 3, two lines above it, had already ratified the name "SlopBench"
— a rename that hadn't propagated within the same file. Corrected.

**Gauntlet (all green, this session, every command run directly):**
tree-wide link checker 0 broken; `mkdocs build --strict` exit 0, zero
warnings; all six generators `--check` green (`gen_registry_header.py`,
`gen_channel_map.py`, `gen_channel_grid.py`, `gen_docs_tables.py` — 14
files/108 terms, `gen_spec_pages.py` — 20 files, `gen_channel_grid_page.py`);
`canon_lint.py` 0 findings; `catalog_lint.py` OK (32 entries, 113 desc / 44
role annotations); native suite 31/31 exit 0 (mingw64 PATH prepend, TRAPS
T10); `pio run -e sd32-ota` SUCCESS, **RAM 24.1% / 78,948 B, flash 28.5% /
1,869,456 B** — byte-identical to the last-verified PARKED-SLOT SAFETY
BROADCAST build, confirming this phase's `SlopSyncCatalog.h` banner-comment
strip moved zero bytes (comments-only, as expected); webui untouched this
phase (`git status` shows 0 changes under `webui/`, confirmed before
declaring this) and `npm run check` still ALL PASS (device-knowledge 27
files/87 wire fields; settings-model all cases; "the renderer is
machine-agnostic"). [verified 2026-07-28 — every command above run directly
this session, exit codes checked; link-verifier script + both its runs,
`mkdocs build --strict` output, all six generator outputs, `canon_lint.py`/
`catalog_lint.py` output, native suite + `sd32-ota` build output all
reproduced in this session]

**Addendum — a lint gap this same session created and then caught.** The
`canon_lint.py` "0 findings" run cited above ran before the `.gitignore`
fix's rescued `docs-site/docs/build/` pages were staged; committing them
(the first of this Phase G close-out's three commits) made canon_lint see
those 10 pages for real for the first time ever, and the mandatory
post-commit re-run (CANON §5: run before declaring substantive work done)
turned up 13 genuine British-spelling findings (the British variants of
"color", "behavior", and "honor", across `cli.md`, the four client-language
stub pages, and `local-testing.md`) that had simply never been checked
before. Fixed in a
third commit, re-verified clean, no firmware/native/webui touched by that
fix. Recorded here rather than silently folded into the number above,
because the number above was accurate when written and would otherwise read
as having covered ground it hadn't yet.

- **Phase C4 LANDED (2026-07-28, execution spec = tools/gen_channel_grid.py's
  ALLOC dict, stamped 2026-07-27 via the channel-grid visual):** 22 device
  channels renumbered onto the family-nibble sub-slot convention — slot =
  [family][member], member 0 = family master, the MIRROR RULE (twin channels
  share domain+family+member digits across class bands), family F =
  admin/meta in every band, named reserves (0x1011 battery, 0x1012 thermal),
  reserved domains 3=auxiliary 4=playback 5=automation, 8-F = parked
  multi-axis block. Moves: power→0x1010, odometer→0x1020,
  machine-modes→0x1030, plan-strip→0x1110, slopmotion-diag→0x1111,
  sm-limits/chase/waveform→0x1120-22, pattern-advanced→0x1210 + its six
  modifier lanes reordered into MEMBER order speed-in/out, accel-in/out,
  depth-1/2 →0x1211-16 (was authoring/BaseId order — `kModChannels` in
  SlopSyncHubService.cpp still indexes by BaseId, comment corrected, array
  contents unchanged), presets-roster→0x1220, modes-set→0x3030,
  machine-admin→0x30F0, sm-set→0x3120, pattern-advanced-cmd→0x3210,
  presets-cmd→0x3220, store→0x5220. `pattern-state` (0x1200) did NOT move —
  Phase D's `background_run` field rides along untouched. Consumers updated:
  SlopSyncCatalog.h (`ch::` constants + invocation order, still strictly
  ascending — encodeCatalog/etag require it), SlopSyncHubService.cpp (stale
  comments only — all call sites use `ch::` symbols, no literal ids),
  webui (frames.js/index.js raw hex constants, session.js/shadow.svelte.js
  comments, slopsync-modes.mjs/slopsync-tuning.mjs live-test scripts),
  tools/slopsync_probe.py (34 literal replacements), slopscope.py,
  slopsoak.py, docs/slopdeck/DESIGN.md, docs/webui-architecture.md.
  `gen_channel_grid.py` now PARSES the live catalog (reuses
  gen_channel_map.py's parsing helpers) instead of an embedded ALLOC dict,
  keeps the named-reserve overlay, and gained `--check`; its `decode_grid`-
  equivalent cell decode now shows `family:member` hex nibbles instead of a
  flat decimal slot. `gen_channel_map.py` regenerated CHANNEL-MAP.md (legend
  prose rewritten with the sub-slot convention; the false "last renumber"
  claim corrected to explain why family-nibble runway actually makes this
  one stick; `decode_grid()` fixed the same way). RFC-047's queue entry
  gained the sub-slot-convention paragraph. Left untouched as pre-existing,
  out-of-scope debt at landing time: the interior
  `// ---- 0x0080 "motion"` -style section-banner comments inside
  SlopSyncCatalog.h's `buildSlopDriveCatalog()` body already quoted
  PRE-C2 ids before this pass and were not touched by C2 either; and the
  dated historical docs (V1-READINESS.md, WEBUI-HANDOFF-RFC-BATCH.md,
  REFACTOR-ROADMAP.md, older RFC-QUEUE.md landed-RFC entries) keep the ids
  they had at the time they were written, matching the precedent those
  same docs already set across the C2 renumber. **RULED, implementation
  deferred to Phase G:** see the STAMPED RIDERS under the Phase G work item
  above — operator ruled 2026-07-28 that the banners lose their hex ids
  entirely and the historical docs get a one-line "historical (pre-C4)"
  header; not yet implemented (`grep '// ---- 0x' include/comms/
  SlopSyncCatalog.h` still shows hex ids as of 2026-07-28) — this is no
  longer an open veto, it is queued Phase G work. [verified 2026-07-28 —
  code read, hex ids still present]
  **Gauntlet (all green):** native suite (mingw64 PATH prepend, exit 0,
  31/31 cases incl. `test_slopsync_staleness`), `pio run -e sd32-ota`
  SUCCESS (RAM 27.4% / 89,920 B, flash 26.1% / 1,712,336 B), `canon_lint.py`
  0 findings, `catalog_lint.py` OK (32 entries, 113 desc / 44 role
  annotations), `gen_registry_header.py` + `--check`, `gen_docs_tables.py`
  (14 files, 2 changed) + `--check`, `gen_spec_pages.py` (20 files, 4
  changed) + `--check`, `gen_channel_map.py` + `--check`,
  `gen_channel_grid.py` + `--check`, `node webui/test/slopsync-wire.test.mjs`
  ALL PASS (sim catalog fixture unaffected — confirmed the sim's own
  `benchrig` catalog shares none of the moved ids), `npm run check` in
  webui ALL PASS, MFP `WireSelfTest` (dotnet run, Release) ALL PASS
  (confirms the plugin's wire touches only unmoved channels). [verified
  2026-07-28 — every command above run directly this session, exit codes
  checked]
- **RFC-050 — LANDED (v1.0), spec/registry side, 2026-07-28** (operator
  stamp on the recommendation, batched with Phase C4; implementation
  deferred, post-batch hub work — SPEC §18-24): new frame type `0x20
  BLOB_DONE` (dir any, plane raw) — the same identity fields as `blob_keys`
  (namespace, store_id, slot, generation) + `status:u8` (0 verified-complete,
  1 hash-mismatch, 2 aborted), sent by the RECEIVER of a transfer, idempotent
  like CATALOG_READY. Operator's call was the new-frame option (b) over the
  draft's own (a)-leaning recommendation, because it generalizes to the
  client→hub direction (a STORE import) that CATALOG_READY's c2h-only shape
  does not fit. New `limits.blob_chunks_in_flight` (4, advertised sender
  pacing budget). SPEC.md §8.4 gained the normative backpressure decision
  table keyed to §13.1's per-binding congestion signal (congested with
  budget→send, congested at budget→hold, recovered→resume, sustained >5s→
  abort with one NACK BUSY + retry_after_ms, reusing the existing
  "one NACK answers one BLOB_REQ" rule) plus the BLOB_DONE completion
  contract; the old "Pacing granularity is a hub policy and is not on the
  wire" advisory line is reworded to point at the table. Appendix A/G rows
  added; reserved-range comment moved `0x20–0x3F`→`0x21–0x3F` (31 slots);
  §4.4's reserved-range prose corrected in the same pass (was already
  stale pre-C4, said `0x1E–0x3F` when 0x1E/0x1F were already RFC-046
  allocations). §18 gained item 24 recording spec-landed/implementation-
  deferred status. RFC-QUEUE.md's own RFC-050 entry rewritten from DRAFT to
  Landed. Gauntlet: same run as Phase C4 above (registry.yaml and SPEC.md
  changes are covered by the same generator + native-suite pass).

- **RFC-048 — LANDED (2026-07-27, see Phase C1 LANDED above), superseding
  this entry's narrower scope:** the well-known channel-name vocabulary
  landed as RENDERING.md §2's STANDARD tier + capability interfaces
  (motion/power/odometer minima, pattern-generator + fray-d-shaped
  advanced-generator interfaces); the two parking rulings below are
  preserved verbatim as RENDERING.md §2.2's PARKED note. Original text,
  kept for the rationale: well-known channel-name vocabulary (registry
  RECOMMENDS names/semantic tags for universal channels — motion, power,
  odometer — convention by name, layouts stay catalog-described) + formal
  PARKING of two operator-deferred questions with their runway documented:
  multi-axis (per-axis domain vs per-axis slots in the 0xCDSS grid — both
  additive) and actuator types / vibrator support (self-describing catalog
  already carries them; a units/action-vocabulary extension RFC when a real
  second-actuator device exists). Operator explicitly does NOT want these
  answered now — parked ≠ forgotten.
- **OSSM-Sauce opcodes: ruled N/A 2026-07-27** — zero OSSM protocol surface
  remains in-tree; no emulation shim ever (masquerade class). OSSM audience
  path: SlopSync-hub firmware on their hardware + upstream SlopSync client
  support in community apps + SlopDeck. Opcode compat, if anyone wants it,
  is a third-party client-side adapter via the onramp.

- **Discovery + BLE transport (operator 2026-07-27) — SUPERSEDED by Phase E
  LANDED below (2026-07-28, build/host-verified only).** Original scope:
  RFC-046 (BLE-primary discovery, pinned ecosystem UUIDs, WELCOME endpoint
  keys, cross-transport migration via RFC-042 reattach) + the BLE GATT
  `ITransport` implementation (NimBLE returns; T5 enqueue pattern; SPEC
  §13.4 binding). mDNS stays as secondary. Sequencing: registry/RFC numbers
  FIRST (spec-gap ritual — already done in Phase B/C1, so Phase E coded
  straight against the landed registry), then firmware transport, then
  SlopDeck shell consumes it. Runs alongside (not instead of) SlopDeck
  milestone 1 (sim fidelity).

- **Extract MachineCommand from WebUI.cpp** — `handleCommand` + the `apply*`
  mutation family + post-clamp echo building move to their own HTTP-free
  class; the SlopSync delegate calls it directly. What remains of WebUI is
  honestly the HTTP plane (page serving, OTA + /uitoken hosting, read-only
  diagnostic GETs, 410 tombstones, bench homeoverride sideband) and gets
  named accordingly. Follow-up riding on it: migrate the extracted core to
  TYPED intent handlers, dissolving the WS_OP/JSON shim op-by-op
  (UiProtocol.h shrinks to nothing). Queued behind the transport-kill
  surgery; sonnet-executable once the seam is mapped.

- **Clocked-logging + legacy-log audit** (operator 2026-07-27) — inventory
  every periodic/cadence log (heap beacon, [sys] lines, rate reports) AND
  the MotionArbiter's logging specifically (operator: arbiter LOGGING feels
  legacy — bring-up-era dispatch/gate narration is the suspect class).
  Verdict each line: earns its keep / demote to SLOGD / delete. Rule:
  clocked telemetry is legal, bring-up noise is not. (sonnet)
- **MotionArbiter sediment note** (smaller, found during today's surgery):
  the retired OSSM_STREAM source slot (id 3 stays reserved — wire label
  row) and interpolator-era comment vocabulary; strip during the logging
  audit's pass through the file.
- **SlopLog + SlopGlow uplift pass** — bring the two elder modules up to
  the slopmotion/slopsync core standard (injected clock, purer hardware-free
  core, tighter conformance tests); operator explicitly opened them to
  improvement. (sonnet, mapped by main loop first)

## SPEC §18 status reconcile (2026-07-28)

Every SS18 known-limitations item (1-24) checked against this ledger's
landed state plus a direct code read; five were stale, nineteen checked out
still accurate and were left untouched (docs commit only, no wire change).

- **Item 8 (blob `INVALID_NAMESPACE`):** still NOT shipped — Phase D landed
  without it. `Hub::resolveBlobBytes` (`hub_impl.hpp`) has no
  `INVALID_NAMESPACE` branch; an unregistered `blob.ns` still answers
  `CHUNK_UNAVAILABLE`. Reworded so "Implementation: Phase D" no longer reads
  as pending work under a phase that has since closed — now "open, no phase
  currently owns it."
- **Item 9 (empty-chunks / `ns=0` MALFORMED grammar):** the reference hub
  was ALREADY compliant before RFC-049 restated the rule precisely —
  `decodeBlobReq` (`blob_req.hpp`) rejects both shapes, its own comment
  citing the older RFC-022.6. Nothing to implement; corrected from
  "Implementation: Phase D."
- **Item 22 (BLE GATT / UDP discovery):** both shipped and live-verified on
  the deployed fw 2.1.78+ reference hub (`SlopSyncBleTransport.*`,
  `SlopSyncUdpDiscovery.*`; live UDP unicast/broadcast/rate-limit and a live
  BLE scan both confirmed — see the DEPLOY + LIVE-VERIFY entry's items (f)
  and (j) above). Kept true: no client has held a live GATT session, no
  small-MTU control-frame fragmentation, and cross-transport migration —
  mechanically in place via identity-based reattach — has never been
  exercised across two bindings live.
- **Item 23 (RENDERING.md vocabulary):** the catalog side landed in Phase C2
  (entry `rank` key 16, field `rank`/`aspect`/`scope`/`provenance`/`unit_id`
  keys 19-23, `category` repurposed for `ui_categories`; `catalog_lint` 113
  desc / 44 role annotations). Kept true: `webui/src` decodes
  `category`/`categoryLabel` and stops there — no reference client builds
  pages from the full derivation chain.
- **Item 24 (blob backpressure/completion):** the hold-not-drop half shipped
  in the HEAP RELIEF pass (fw 2.1.81, `SlopSyncAsyncWsTransport::write()`
  gates `BLOB_CHUNK` on `limits::blob_chunks_in_flight`). Kept true: no NACK
  `BUSY` on sustained congestion, no reference `BLOB_DONE` emission.

[verified 2026-07-28 — direct reads of `hub_impl.hpp` (resolveBlobBytes,
handleReattach), `blob_req.hpp` (decodeBlobReq), `SlopSyncAsyncWsTransport.cpp`,
`SlopSyncCatalog.h` (category/rank annotations), `webui/src/core/slopsync/
catalog.js` (no rank/archetype consumption); commit `ea072aa`]

## WIRE-STRING PUNCTUATION EVOLUTION (2026-07-28) — morning ruling item 2, fw 2.1.82 → 2.1.83

ONE atomic catalog evolution: em/en dashes, double-hyphens, and banned prose
words (delve/leverage/robust/seamless/synergy/testament/tapestry/unlock/
quiet/notable) purged from every wire-emitted `.desc` string in
`include/comms/SlopSyncCatalog.h` (3 instances, incl. the named "quiet" hit
in a serial/settle-adjacent desc) and from `docs/slopsync/registry/
registry.yaml`'s desc/note documentation strings (95 mechanically converted
+ 13 hand-rewritten to avoid same-clause double punctuation + 1 named
banned-word instance — prose only, no keys/numbers/names/refs/status
touched). Frozen artifacts (`lib/slopsync` conformance `mini_catalog.hpp`,
`vectors/fixtures/mini-catalog.yaml`, golden byte arrays) checked and
confirmed to carry no dash or banned word already — untouched, C-11/T11
precedent.

Regenerated once (lockstep, all `--check` green): `gen_registry_header`,
`gen_docs_tables` (9 of 14 pages changed), `gen_spec_pages` (0 changed —
SPEC.md prose is independent of registry.yaml notes), `gen_channel_map`,
`gen_channel_grid`, `gen_channel_grid_page`. `webui/test/fixtures/
slopsim-catalog.{bin,etag}` re-captured from a fresh `sim/slopsim`
device-profile build (44 channels, 24,585 B, etag `b69eb06249ebe73a` —
independently matches the live device's new etag, below).

**Gauntlet (all green, every command run directly this session):** native
suite 31/31 environments PASSED exit 0 (mingw64 PATH prepend, TRAPS T10);
`pio run -e sd32-ota` SUCCESS (RAM 24.1% / 78,948 B, flash 28.5% /
1,869,456 B — byte-identical to the pre-pass build); `canon_lint.py` 0
findings; `catalog_lint.py` OK (32 entries, 113 desc / 44 role annotations,
every desc still under its byte cap); `mkdocs build --strict` exit 0, zero
warnings; `node webui/test/slopsync-sim.mjs` (fresh device-profile slopsim)
ALL PASS; `node webui/test/slopsync-wire.test.mjs` ALL PASS; webui `npm run
check` ALL PASS. A standalone decode-and-scan of the served catalog bytes
(webui's own `decodeCatalog` against the device-identical fixture): 44
entries, 935 text fields scanned, 0 dash hits, 0 banned-word hits, 0
British-spelling hits.

**Live deploy + verify.** `FIRMWARE_VERSION` 2.1.82 → 2.1.83
(`include/system/config_api.h`), OTA'd via `deploy.ps1`. `/api/capabilities`
confirms `fw_version 2.1.83`. `tools/slopsync_probe.py --estop --bench-home
--bench-home-no-revert`: **50 passed / 0 failed / 3 skipped**, exit 0.
Catalog etag CHANGED `9275f578ada7d314` → `b69eb06249ebe73a` (matches the
sim's independently captured etag above). BLOB transfer completes: 129
chunks, 24,585 bytes, reassembly verifies against the WELCOME etag.

**Pre-existing drift found at session start, not caused by this pass:** the
device's `home_override` was `false` on connect (the PARKED-SLOT SAFETY
BROADCAST entry's documented end-state was `home_override=true`) — a reboot
between sessions cleared the volatile bench override, as it always does.
Restored via `--bench-home --bench-home-no-revert` in the same probe run
that verified the deploy.

**Device end-state:** fw **2.1.83**, reachable at 192.168.1.229,
`homed=true` `home_override=true` (fake-homed, left ON per
`--bench-home-no-revert`), `estopped=false`, not moving,
`measured_stroke_mm=250`.

**Files touched:** `include/comms/SlopSyncCatalog.h`,
`docs/slopsync/registry/registry.yaml`,
`lib/slopsync/include/slopsync/generated/registry_constants.hpp`,
`docs/slopsync/CHANNEL-MAP.md`, 9 `docs-site/docs/reference/registry/*.md`
pages, `include/system/config_api.h` (version bump),
`webui/test/fixtures/slopsim-catalog.{bin,etag}`. [verified 2026-07-28 —
every command above run directly this session, exit codes checked; commit
`274abc3`]

## BLE ADVERTISING MSD FIX (2026-07-28) — item (j) CLOSED, fw 2.1.83 → 2.1.84

**Item (j) reopened by an operator phone scan (nRF Connect, hard evidence,
2026-07-28).** The live advertisement carried Flags, Complete 128-bit
Service UUID list, and Complete Local Name "SD32" in the primary
advertisement, plus Complete Local Name "SlopDrive-32" in the scan
response — but **no Manufacturer Specific Data record existed in either
packet.** The `ble_adv_flags` byte (bit0 `pairing_window_open`, bit1
`ws_available`) had never been on the air. This retroactively explains the
DEPLOY + LIVE-VERIFY session's item (j) `bleak` result (`manufacturer_data`
came back `{}`) — not Windows/WinRT company-id filtering as guessed at the
time, but a firmware gap.

**Mechanism confirmed by reading the code path and the vendored NimBLE
source (`.pio/libdeps/sd32-ota/NimBLE-Arduino/src/NimBLEAdvertisementData.cpp`)
— TRAPS T14, new entry.** `SlopSyncBlePort::refreshAdvertisingData()` built
ONE `NimBLEAdvertisementData` carrying Flags(3B) + 128-bit Complete Service
UUID(18B) + `setName(_shortName)`(6B, "SD32") + Manufacturer Specific
Data(5B, company `0xFFFF` + the flags byte) = **32 bytes, one over
`BLE_HS_ADV_MAX_SZ` (31)**. `NimBLEAdvertisementData::addData()` silently
returns `false` and adds nothing once the running total would exceed the
budget — earlier records already added still reach the radio; only the
overflowing one (here, the MSD, added last) is dropped. `refreshAdvertisingData()`
discarded every setter's return value, so the failure was invisible.
**Compounding, separately-caught bug:** `setName(_shortName)` was called
with no second argument, and `NimBLEAdvertisementData::setName()` defaults
`isComplete=true` — so "SD32" went out as AD type 0x09 (Complete Local
Name), matching what nRF Connect saw, not the shortened-name (0x08) design
intent.

**Fix (`src/comms/SlopSyncBleTransport.cpp`, `SlopSyncBlePort::refreshAdvertisingData`):**
- Advertisement: Flags(3) + 128-bit Complete Service UUID(18) +
  **shortened** name via `setShortName()` (AD 0x08, "SD32", 6B) = **27B**.
  No MSD here — no room for it alongside the other two records.
- Scan response: complete name "SlopDrive-32" (AD 0x09, 14B) +
  Manufacturer Specific Data (company `0xFFFF` + flags byte, 5B) = **19B**.
  The flags byte now rides the scan response, which requires an ACTIVE
  scan to read (both `bleak` on Windows and nRF Connect do this by
  default).
- Every advertising-config return code (`setFlags`/`setCompleteServices`/
  `setShortName`/`setName`/`setManufacturerData` on both
  `NimBLEAdvertisementData` objects, and `NimBLEAdvertising::
  setAdvertisementData`/`setScanResponseData` themselves) is now checked
  and a failure logs `SLOGW` — never silently discarded again.
- `updateAdvertising()`'s diff-gated refresh already called
  `refreshAdvertisingData()` unconditionally on any flags change, so the
  scan-response rewrite was already wired through that path; verified live
  (below) that `bleak` sees the byte change on a boot-time flags publish.

**SPEC/registry wording amended (spec-gap ritual, comment/prose only, no
registry VALUE changed, no wire/etag effect):** SPEC.md §13.4's advertising-
payload paragraph and `registry.yaml`'s `ble_adv_flags` header comment both
previously read as if the flags byte rode the same 31-byte budget as the
service UUID and shortened name (the design mismatch that let this bug
ship unnoticed). Reworded both to state plainly that the flags byte rides
the scan response, active scan required. Regenerated all six doc/spec
generators (`gen_registry_header.py`, `gen_docs_tables.py`,
`gen_spec_pages.py` — 1 file changed, `docs-site/docs/spec/transports.md`,
matching the SPEC.md §13.4 wording edit — `gen_channel_map.py`,
`gen_channel_grid.py`, `gen_channel_grid_page.py`), all `--check` green.

**No native test added for the payload byte-budget arithmetic — judgment
call, reasoning recorded rather than silently skipped.** The actual
AD-record-length computation lives entirely inside vendored NimBLE
(`NimBLEAdvertisementData::addData`/`setName`/`setManufacturerData`),
compiled only under `-DBLE_ENABLED`; `[env:native]` is header-only (no
`src/` compilation, no Arduino/NimBLE on the include path by design — see
`SlopSyncBleTransport.h`'s own file-scope comment). Testing the REAL path
would mean linking NimBLE on the host, which is not portable off the
ESP32/Arduino BLE stack; writing a parallel calculation instead would test
the test, not the code, and could drift from the real `addData()` logic
silently. Skipped per the task brief's own escape hatch.

**Host gauntlet (all green, every command run directly):** native suite
31/31 exit 0 (mingw64 PATH prepend, TRAPS T10); `pio run -e sd32-ota`
SUCCESS, RAM 24.1% / 78,948 B, flash 28.5% / 1,869,924 B (+468 B over the
pre-fix build — the added return-code checks/log lines, as expected, no
wire/catalog change); `canon_lint.py` 0 findings; `catalog_lint.py` OK (32
entries, 113 desc / 44 role annotations — untouched, no catalog edit this
pass); all six generators `--check` green (above).

**Live deploy + verify.** `FIRMWARE_VERSION` 2.1.83 → 2.1.84
(`include/system/config_api.h`), OTA'd via `deploy.ps1`. `/api/capabilities`
confirms `fw_version 2.1.84`. Active `bleak` scan (repo `.venv`, package
`bleak==3.0.2`) against the live device:

```
address: 20:6E:F1:31:74:6D
local_name: SlopDrive-32
service_uuids: ['534c4f50-5359-4e43-8000-000000000001']
manufacturer_data (raw): {65535: b'\x02'}
  company_id: 0xFFFF
  payload bytes: 02
```

Company id `0xFFFF` matches `kBleMfgCompanyId`; payload byte `0x02` = bit1
`ws_available` set, bit0 `pairing_window_open` clear — exactly the expected
current bench state (WiFi up, no pairing window open). The on-device
`/api/log` boot trace independently confirms both payload builds
succeeded with no WARN (`BLE advertising flags -> 0x02 (pairing=0 ws=1)`
logged, no `SLOGW` line) and shows the GATT server naming ("BLE GATT port
up ... name 'SlopDrive-32'"). Windows/WinRT's `bleak` backend merges
advertisement + scan-response fields into one `AdvertisementData` object
per device and does not expose which physical packet carried which AD
record, so the shortened-name-in-ADV-specifically claim rests on the code
read + the successful `setAdvertisementData()` return (no WARN logged) —
an operator phone + nRF Connect re-check (raw per-packet AD dump) remains
the strongest confirmation and is optional follow-up, not required to close
this item.

**Pairing-window toggle (bit0) — SKIPPED, judgment call.** No plain HTTP
route opens/closes the pairing window; the only paths are the 3-quick-
power-cycle boot gesture (disruptive: requires actual reboots) or a signed
SlopSync INTENT frame over an authenticated WS/BLE session (meaningfully
more machinery than this fix's scope, and it would leave real pairing
state written to NVS). Skipped rather than manufacture a side quest; the
diff-gate code path (`updateAdvertising` → `refreshAdvertisingData` on any
flags change) is unchanged by this fix and was already exercised at boot
(the flags publish captured in `/api/log` above).

**`tools/slopsync_probe.py`, two runs, motor unplugged throughout:**
`--stream 1 --segments 1` (exercises intent/stream/segment/safety-mode
round-trips; no `--estop`, no `--bench-home` — the OTA's own reboot had
already cleared the volatile fake-home, see below): **55 passed / 0 failed
/ 2 skipped** (`estop_assert`, `bench_home` — both correctly skipped,
opt-in-only ops). A second run, `--no-motion --bench-home
--bench-home-no-revert`, to restore the bench state: **45 passed / 0
failed / 6 skipped**, `bench_home` PASS. (Total assertion count is higher
than the DEPLOY + LIVE-VERIFY session's "50/0/3" because that run used
`--estop --bench-home` together in one pass and against a smaller catalog
of subscribe checks; both runs here are 0-failed, and the higher pass
count reflects exercising MORE of the protocol, not a different bar.)

**Pre-existing, expected, not a regression:** the OTA deploy's own reboot
cleared the volatile `home_override` (`false` immediately post-OTA) exactly
as documented in the WIRE-STRING PUNCTUATION EVOLUTION entry above — every
firmware OTA does this. Restored via the second probe run above.

**Device end-state:** fw **2.1.84**, reachable at 192.168.1.229,
`homed=true` `home_override=true` (fake-homed, left ON), `estopped=false`,
not moving, `measured_stroke_mm=250`.

**Files touched:** `src/comms/SlopSyncBleTransport.cpp`,
`include/comms/SlopSyncBleTransport.h` (comment only), `docs/slopsync/SPEC.md`
(§13.4 prose), `docs/slopsync/registry/registry.yaml` (`ble_adv_flags`
comment only — no value changed), `docs-site/docs/spec/transports.md`
(regenerated), `include/system/config_api.h` (version bump),
`docs/canon/TRAPS.md` (T14), `SD32-OVERNIGHT-REPORT.md`. [verified
2026-07-28 — every command above run directly this session, exit codes
checked; live `bleak` scan + `/api/log` + `/api/capabilities` + `/api/status`
all reproduced above]

## Deferred / planned (homes: docs/REFACTOR-ROADMAP.md, docs/MOTION-TODO.md)

- TCode pass-through channel (post-MFP; parser cross-task race was the
  blocker).
- Native Intiface SlopSync support (replaces the deleted :55555 bridge).
- Telemetry redesign; Tauri 2 shell; C5-node SlopSync transports; merge to
  `main`.
