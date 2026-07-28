---
title: SlopSync
description: >-
  SlopSync is an open device-shadow and capability-negotiation protocol for
  motion machines. One app, many firmwares, one truth about what the machine
  is doing.
register: STE
---

# SlopSync

**An open protocol for talking to a motion machine, so one app can work with
many firmwares and never lie about what the machine is doing.**

A machine running SlopSync describes itself. A client reads that description
and builds its interface from it. Neither side needs to be compiled against
the other.

<div class="ss-doors" markdown>

<div class="ss-door" markdown>
### Understand
Read the mental model first. What a catalog is, what the four channel classes
do, and why the machine — not the app — owns motion.

[Start here](understand/index.md)
</div>

<div class="ss-door" markdown>
### Build with it
Connect a client, write an app integration, or make your own firmware a
conforming hub.

[Start building](build/index.md)
</div>

<div class="ss-door" markdown>
### Specification
The normative clauses, the conformance suite, and every registered wire
number in generated tables.

[Read the spec](spec/index.md)
</div>

</div>

## What it does

<div class="ss-facts" markdown>

| | |
|---|---|
| **Describes itself** | A hub publishes a catalog. A client reads it and knows every channel, type, unit, limit and access level, without a firmware-specific driver. |
| **Keeps one truth** | State channels carry full snapshots. The hub retains the latest one and pushes it the moment a client connects. Page load adopts device state. |
| **Never lies** | An echo reports the value the machine actually applied, after clamping. A client's shadow updates from the hub, never from its own request. |
| **Stops safely** | ESTOP is role-exempt, jumps every queue, and latches. A vanished motion source trips its deadman and the machine stops. |
| **Fits small hardware** | The mandatory client floor is one parser, no crypto and 24 bytes of stored identity. A coin-cell remote is a first-class peer. |

</div>

## What it is not

SlopSync is not a replacement for the firmware you already like. It is a
compatibility layer those firmwares can adopt, so that one app works across all
of them. See [Ecosystem and compatibility](understand/ecosystem.md).

SlopSync is not a motion planner. The machine owns motion processing. A client
sends intent; the hub decides how to execute it safely.

SlopSync is not a security product. The v1 threat model is casual and drive-by
prevention on a trusted local network, and the specification says so plainly
wherever a defense is weak. See [Security model](understand/security.md).

## Governance, briefly

**SlopSync favours no firmware, no vendor and no product. It is provided to
the community as a tool.** The full statement is on its own page:
[Governance](community/governance.md).

## Where the numbers come from

Every frame type, CBOR key, NACK code, channel id and limit on this site is
generated from one registry file. No wire number here is typed by a human, and
a stale table fails the build.

[Registry reference](reference/registry/index.md){ .md-button }
[The Dictionary](reference/dictionary.md){ .md-button }
