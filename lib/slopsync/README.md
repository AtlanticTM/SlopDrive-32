# slopsync

Reference C++ implementation of **SlopSync** (`slopsync/1`) — a hub-and-spoke
device-shadow protocol for machines with many kinds of clients: browser UIs,
hardware remotes, mobile apps, simulators. One hub holds the canonical machine
state; every client negotiates capabilities in a handshake, adopts state, and
stays truthfully in sync. Clients send **intents**; the hub echoes **applied**
(post-clamp) values; every subscriber sees the same reality.

**The protocol spec is the product** — this library is its reference
implementation. Spec: [`docs/slopsync/SPEC.md`](../../docs/slopsync/SPEC.md);
single source of truth for every wire number:
[`registry/registry.yaml`](../../docs/slopsync/registry/registry.yaml).

## Properties

- **Zero dependencies.** C++20 standard headers only. Vendored SHA-256/CRC-32,
  hand-rolled deterministic-CBOR subset (byte-exact golden vectors prove it).
- **Header-only, heap-free steady state, no threads, no exceptions.** You pump
  `Hub::update()` / `Client::update()`; time and randomness are injected
  (`IClock`, `IRandom`) — every behavior is deterministic under test.
- **Hardware-free.** Compiles identically for desktop (tests, simulators) and
  embedded targets (developed against ESP32-S3 / GCC 13+). Transports are
  adapters you implement: `ITransport` is four operations (§13.1).
- **Safety-literal.** ESTOP frames are recognizable in a byte stream without
  deframing, repeat until the latch is observed, and jump every queue. Deadman
  policy, exclusive control ownership with takeover, and priority shedding are
  library behavior, not app conventions.

## Quick start (both roles, one process)

```cpp
#include "slopsync/slopsync.h"
using namespace slopsync;

ManualClock clock;                 // or your platform clock adapter
XorShift32 rng(0xB007CAFE);        // hardware entropy in production
InProcessLink link(clock, rng);    // or your real transport adapter

MyDelegate machine;                // implements HubDelegate::applyIntent etc.
Hub hub(myCatalog, clock, rng, machine);
hub.attachTransport(link.endpointA());

MyUiDelegate ui;                   // implements ClientDelegate
Client client(identity, link.endpointB(), clock, rng, ui);
client.addSubscriptionWish(0x0003 /*safety*/, 0.0f, Priority::critical);
client.connect();

for (;;) { hub.update(clock.nowUs()); client.update(clock.nowUs()); /* pace */ }
```

## Layout

`generated/` (registry codegen — regenerate with
`python tools/gen_registry_header.py`, `--check` in CI) → `core/` (clock/rng/
result) → `util/` → `wire/` (codecs) → `transport/` (contract + in-process
fault-injection binding) → `channel/` → `session/` (safety, pairing,
shedding) → `hub/` + `client/`.

Tests: `test/native/test_slopsync_*` (doctest, `pio test -e native`) — ~150
cases implementing the golden-vector manifest
([`vectors/manifest.yaml`](../../docs/slopsync/vectors/manifest.yaml)).
Demo: [`examples/slopsync_demo/`](../../examples/slopsync_demo/).

## Conformance

`conformance/catalog_check.hpp` mechanically validates any catalog (STATE-fit,
id order, form rules). The frozen fixture `conformance/mini_catalog.hpp` pins
the protocol's reference etag (`21 CB 26 C9 4F B3 88 B5`) — if your
implementation reproduces it byte-for-byte, your catalog codec is correct.

License: MIT. Part of the SlopDrive-32 / OSSM-adjacent ecosystem.
