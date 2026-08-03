---
name: drift
description: Run the spec-drift detector and turn divergences into RFC candidates. Use when asked what the implementation has built that the spec does not know yet, before proposing RFCs, after bumping slopsync.pin, or when spec-vs-code disagreement is suspected.
---

# /drift: spec drift detector

Run it from the repo root and show the operator the full grouped output:

```bash
bash tools/spec-drift.sh
```

The script diffs this repo's observed protocol reality against the sibling
SlopSync checkout (pinned by slopsync.pin) and groups findings by class:

- PIN DRIFT: slopsync.pin vs sibling HEAD, and sibling working-tree
  dirtiness. A stale pin makes every other class meaningless; fix first.
- STALE GENERATED: registry.yaml newer than its codegen outputs
  (gen_registry_header.py --check).
- HAND-TRANSCRIPTION (T20 class, `.claude/rules/build-test-deploy.md`): values codegen does not emit and
  this repo hand-copies: udp_discovery + ble_adv_flags in
  include/comms/SlopSyncDiscoveryWire.h, ble_identity in
  include/comms/SlopSyncBleTransport.h. These drift with zero compiler
  symptom; highest-value class.
- INTERNAL VOCAB: SlopDrive-private two-ended constants (C5 bridge byte
  vocabulary in SlopSyncUartTransport.h vs src/c5_probe/main.cpp). Not spec
  drift, same failure shape.
- SYMBOL DRIFT: slopsync:: identifiers this repo uses that the pinned
  generated header does not define (implemented-ahead-of-spec candidates).

## Interpreting results

Each finding is a candidate RFC, not a bug to silently fix: the flow is
dev -> RFC -> ruling -> spec + registry -> pin bump. Never edit the sibling
spec to match this code. For each finding the operator wants: which side is
ahead, what the RFC must argue, and what breaks if it lands. Offer to draft
RFC-QUEUE entries (upstream, the one writable file) or file dev-board issues;
do either only on an explicit yes.
