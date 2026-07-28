# docs/slopsync/ — this machine's channel allocation only

The SlopSync protocol (spec, registry, RFCs, docs site) lives in the
**SlopSync repo** (sibling checkout `../SlopSync`, pinned at the repo root's
`slopsync.pin`). Read it there: `spec/SPEC.md`, `spec/RENDERING.md`,
`spec/RFC-QUEUE.md`, `spec/registry/registry.yaml`, and the generated docs
site (`docs-site/`).

This directory keeps only **CHANNEL-MAP.md** — SlopDrive-32's own
device-channel allocation, mechanically generated from `include/comms/
SlopSyncCatalog.h` by `tools/gen_channel_map.py`. It stays at this path
because ~100s of inbound links point here; it is a machine-specific fact
(C-1: one home), not protocol spec.
