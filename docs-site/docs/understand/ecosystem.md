---
title: Ecosystem and compatibility
description: >-
  How SlopSync relates to OSSM, ossm-rs, fray-d lite and other open motion
  firmwares: a unifying compatibility layer they could adopt, not a
  replacement for any of them.
register: STE
---

# Ecosystem and compatibility

OSSM, **ossm-rs**, **fray-d lite** and their peers are why this space exists.
People designed them, shipped them, supported them, and taught everyone else
what a good machine controller looks like. Anyone building here today is
building on work those projects did first.

**SlopSync is not a replacement for any of them, and not an upgrade over any
of them.** It is one thing only: a compatibility layer a firmware can adopt,
so that one app works across all of them.

## The problem, stated neutrally

Every firmware in this space answered the same questions separately. How does
an app discover what this machine can do? How does it read live position? How
does it send a command and learn what actually happened?

Each project answered on its own, because **there was no shared answer to
adopt**. That is the normal outcome of solving a problem first. It is not a
mistake by anyone, and it is not a mess that needs cleaning up.

The cost is only visible from outside any one project. An app author who wants
to support three firmwares writes three integrations, tests against three
machines, and maintains all three forever. Most authors write one and stop,
which means owners of the other two machines never get that app.

## What a shared layer buys each side

**For an app author.** One client instead of one per firmware. A machine
describes itself, so a new device works on the day it appears, without an app
update.

**For a firmware author.** Every app that speaks the layer, without writing
app-side code for any of them. Your machine appears in tools you never
integrated with, because they were written against the protocol rather than
against a device.

**For an owner.** The remote, the phone app, the desktop plugin and the web
page all agree about what the machine is doing, because they are all reading
the same declared state.

<p class="ss-point" markdown>**The point.** A compatibility layer is worth adopting only if it is additive. If it asked a project to give up its own interface, its own planner or its own identity, the trade would not be worth making, and nobody should make it.</p>

## What a firmware keeps

Everything that makes it itself.

- **Its own interface.** SlopSync describes what a value **is**. It never says
  how a value should look. There is no widget field in the catalog,
  deliberately.
- **Its own motion planning.** The machine owns motion. The protocol carries
  intent, and the hub decides how to execute it.
- **Its own protocol.** A hub can speak SlopSync alongside whatever it already
  speaks. Adding a binding removes nothing.
- **Its own identity.** Its name, its brand, its community, its release
  cadence. A conforming hub is a conforming hub, whoever built it.
- **Its own opinions.** Feature set, defaults, hardware support and product
  decisions stay entirely with the project.

## What adopting it actually costs

Honest numbers, so a maintainer can estimate the work rather than guess at it.

- The **hub floor** is a checklist, and every item on it is something a
  firmware already does internally. It is on
  [Capabilities and custom hardware](capabilities.md).
- The **client floor** is one parser, no required cryptography, and 24 bytes
  of stored identity.
- The **frame budget** is 242 bytes for every mandatory message, because the
  weakest transport writes the rules.
- **No language is imposed.** The reference implementation is header-only
  C++20, and nothing requires you to use it. The registry and the
  [golden vectors](../reference/dictionary.md#golden-vector) are the contract.
  Implement it in the language your firmware is already written in.
- **No transport is imposed.** A binding is four operations plus an honest
  declaration of what it can do. Implement the ones your hardware has.

If some rule in [the specification](../spec/index.md) assumes something your
firmware cannot do, that is a finding, not a verdict on your design. It
belongs in the [RFC process](../community/rfc-process.md), where it will be
answered on technical merit.

## Working with specific projects

These notes are written **without input from those projects yet**. They are an
invitation to a conversation, not a survey of anyone's code, and a
maintainer's description of their own project outranks anything written here.
Corrections are pull requests, and they will be taken.

**OSSM** and **ossm-rs** — an open machine controller and a Rust
implementation in the same family. The mapping questions are the interesting
part, and they are questions rather than proposals. Which of the machine's
values are already snapshot-shaped? Which commands are already absolute rather
than relative? Where does the firmware's own arbiter already decide who is
driving?

Where those already exist, a binding is mostly declaration. Where they differ,
the difference is worth understanding before anybody writes code. A Rust hub
is a first-class implementation, not a port.

**fray-d lite** — the same conversation, with its own answers. A hub binding
would have to fit how that project already models a session and a running
pattern, rather than the other way around.

**Apps and bridges.** An application that already speaks a device protocol can
add a SlopSync client without dropping anything it supports today. The first
external client written against this protocol was a plugin for an existing
desktop application, and it kept every other integration that application had.

**Anyone else.** If you maintain a firmware, a remote, an app or a bridge in
this space and want the mapping worked out with you, open an issue. "We
already do this differently, here is why" is the most useful reply this page
can get.

## The governance stance

**SlopSync favours no firmware, no vendor and no product. It is provided to
the community as a tool.**

That stance is what makes the rest of this page mean anything, so it is stated
plainly and mechanically enforced where it can be.

- **No firmware is the reference firmware.** SlopDrive-32 is where SlopSync
  was written and where it is proven on hardware. That makes it the first
  implementation, not the privileged one.
- **No vendor gets a reserved number.** The registry allocates in the open, by
  pull request, on technical merit. There is no private range anyone else
  cannot read.
- **The specification is the product.** The library is its reference
  implementation. Where they disagree, the specification wins and the library
  has a bug.

The full statement lives on the [Governance](../community/governance.md) page.

## How this page is reviewed

One test, applied to every sentence before it lands: **would you be
comfortable if the maintainer of the project you just named read it out
loud?**

A sentence that reads as "better than" is a defect on this page. Not a style
preference — a defect, handled the way a wrong error code would be handled. If
you find one, report it, and it will be rewritten.

## Where to go next

- [What it replaces](what-it-replaces.md) — the honest both-columns list,
  including what SlopSync leaves alone.
- [Capabilities and custom hardware](capabilities.md) — the floor a firmware
  would be adopting.
- [Governance](../community/governance.md) — the stance in full, and what it
  forbids.
