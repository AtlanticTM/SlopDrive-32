---
title: What it replaces, and what it does not
description: >-
  An honest account of what SlopSync takes over and what it deliberately leaves alone, including TCode's continued role as compatibility ingest.
register: STE
---

# What it replaces, and what it does not

Both columns on this page are honest. The second one is longer, and it is the
one that matters more.

## Start here if you use TCode

**Your setup keeps working.** SlopSync does not deprecate TCode, and there is
no flag day.

A hub keeps its TCode edges as **compatibility ingest**. It wraps each active
edge in a [synthetic session](../reference/dictionary.md#synthetic-session):
an internal session object that appears in the roster, owns its arbiter
source, and carries a [deadman](../reference/dictionary.md#deadman) equal to
that edge's existing quiet timeout. A TCode sender receives no SlopSync
frames. The wrapping is entirely hub-side bookkeeping.

That is a safety rule, not a courtesy. There is no unmonitored path to motion.
A legacy transport that could move the machine outside the safety machinery
would be a hole in it.

SlopSync-native motion streaming is the **upgrade path**, not a deadline. A
native stream carries timestamps, a rate
[grant](../reference/dictionary.md#grant),
[source ownership](../reference/dictionary.md#source-ownership) and the
deadman as protocol, rather than as conventions each firmware reimplements.
Move when that is worth something to you.

## What it replaces

Every row here is something projects in this space build again from scratch.

| Replaced | With | What that buys |
|---|---|---|
| A bespoke HTTP or WebSocket API per device | One catalog, one frame grammar | A client stops being written against one firmware |
| Hand-written client code per device | Rendering from the catalog | A control added in firmware appears in existing clients |
| Hardcoded interface layouts | Catalog-driven controls with unit, limit and [`setting_key`](../reference/dictionary.md#setting_key) | The layout cannot drift from the firmware it renders |
| A hand-copied table of command schemas | The catalog the client already decodes | The copy that goes stale silently is gone |
| Polling an endpoint for status | STATE channels with [retained values](../reference/dictionary.md#retained-value) | Connect and adopt, instead of poll and hope |
| Per-feature chunked transfer machinery | One blob verb, with namespaces | Catalogs, presets and ledgers all move the same way |
| A device log endpoint | A log channel | Clients that are not browsers can see the logs |
| A client list and a kick endpoint | Session-events for joins and leaves, plus an admin evict intent — the roster snapshot itself is specified, not yet built | Session administration is protocol, not a side door |
| A capabilities endpoint | [Capability discovery](../reference/dictionary.md#capability-discovery) | The feature list cannot disagree with reality |

The pattern in that table is one idea applied repeatedly. **Every surface that
was a private agreement between one firmware and one client becomes a declared
channel that any client can read.**

<p class="ss-point" markdown>**The point.** None of those replacements is about a nicer encoding. Each one deletes a second source of truth — the hand-written table, the hardcoded layout, the parallel feature list — because the second source is the one that drifts.</p>

## What it does not replace

### TCode

Covered above. It stays, as compatibility ingest, under the same safety
obligations as any native session.

### Motion planning

The machine owns motion. A client sends intent. The hub decides how to execute
it safely, with its own planner, its own limits, and its own knowledge of
where the carriage actually is.

This is a doctrine, not a gap. A client cannot know the machine's live
position, its acceleration headroom, or which limit set applies at this
instant. A protocol that let clients plan motion would export a decision to
the one party that cannot make it correctly.

The consequence is freeing. A client ships what its author meant, and the
machine renders it as well as it can. SlopSync carries intent honestly and
refuses to make the client responsible for feasibility.

### Firmware update

Update transport lives outside the protocol, on its own credential plane.
Update rights are never derivable from any
[access level](../reference/dictionary.md#access-level), `configure` included.

The reasoning is blunt. Flashing firmware replaces the thing that enforces
every rule on this site. That capability must not be reachable by escalating
inside the system it would replace.

### Intiface and WSDM

These are a boundary, not a competition. Where a hub dials out to an
application protocol, that outbound client is an adapter the hub owns, and it
materializes as a synthetic session like any other legacy edge. Exposing
SlopSync to those stacks directly is out of scope.

### Any firmware's own interface, protocol or identity

A hub can speak SlopSync **alongside** whatever it already speaks. Nothing
here asks a project to retire its own control surface, its own app or its own
name. See [Ecosystem and compatibility](ecosystem.md), whose tone rules apply
to this section too.

### The hardware emergency-stop path

The protocol's ESTOP is a software convenience layered above the hardware
path. It is fast, role-exempt and latched, and it is still software on a
network. The hardware path remains the guarantee of last resort.

## What SlopSync deliberately is not

These are non-goals. They were decided, not overlooked.

- **No cloud.** Nothing leaves the site. There is no telemetry, no account
  service, and no remote dependency of any kind.
- **No broker.** The hub is the only authority. There is no message bus to
  deploy and nothing extra to keep running beside the machine.
- **No account system.** Identity is a device, not a person. A client is 8
  bytes of durable id plus a token it earned through a physical ceremony.
- **No peer-to-peer.** Clients never talk to each other. All truth flows
  through the hub, which is what makes one observable state possible at all.
- **No wide-area deployment.** The port is a LAN port. Exposing it to the
  internet is not a supported configuration.

<p class="ss-point" markdown>**The point.** Every one of those non-goals removes a component that would otherwise have to be running, trusted, updated or paid for. A machine on a bench with no internet connection is the design center, not a degraded mode.</p>

## What migration deletes, honestly

On the machine where SlopSync was written, migration did not only move
surfaces. It deleted some. The reasons are worth publishing, because every
project has a version of this list.

- **A transport-mode selector** that chose between competing ingest paths.
  SlopSync replaced the thing it was selecting between.
- **An endpoint that always answered "not cleared"** — a stub whose only real
  behavior was a side effect available elsewhere.
- **A control that posted to a route the firmware never had.** It rendered, it
  did nothing, and nobody noticed until the surfaces were enumerated.
- **A panel wired to a data source that had already been superseded.** It
  showed live-looking gauges fed by a dead producer.

A control that renders but drives nothing is a defect. An interface that lies
about machine state is a safety defect. Those four were found by enumerating
every surface for migration, which is an argument for doing the enumeration
even if you migrate nothing.

## Where to go next

- [Ecosystem and compatibility](ecosystem.md) — how this sits beside the
  firmwares people already run.
- [Capabilities and custom hardware](capabilities.md) — what adopting it
  actually asks of a device.
- [Security model and the audit](security.md) — what "LAN-first" defends, and
  what it does not.
