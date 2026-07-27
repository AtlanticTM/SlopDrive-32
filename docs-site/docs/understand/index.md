---
title: Understand SlopSync
description: >-
  The why tier: the mental model behind SlopSync, what it does and does not replace, how it fits the existing ecosystem, and what its security actually defends.
register: STE
---

# Understand SlopSync

This section explains the ideas. It asks for no code, and it shows no wire
numbers — [Anatomy of a frame](anatomy.md) shows byte *shapes*, and every
actual number on this site lives in the generated
[registry reference](../reference/registry/index.md).

Read [How it works](how-it-works.md) first. Everything else assumes it.

| Page | Answers |
|---|---|
| [How it works](how-it-works.md) | What is a catalog, a channel, a shadow, an intent? |
| [Anatomy of a frame](anatomy.md) | What is in a frame, and why are there two payload encodings? |
| [Capabilities and custom hardware](capabilities.md) | What must my device provide, and what is optional? |
| [What it replaces](what-it-replaces.md) | What does SlopSync take over, and what does it leave alone? |
| [Ecosystem and compatibility](ecosystem.md) | How does this fit alongside the firmwares people already run? |
| [Security model and the audit](security.md) | What is defended, what is not, and what did the fuzzing find? |
| [For everyone](for-everyone.md) | I just own a machine. What does this mean for me? |

## The three sentences

**A machine describes itself.** The hub publishes a catalog: every channel,
type, unit, limit and access level it has. A client reads it and builds its
interface from it.

**The machine is the only authority.** A client sends intent. The hub applies
what it can and echoes what it actually did. A client's shadow updates from the
hub, never from its own request.

**Nothing moves unwatched.** A source that goes silent trips its deadman.
ESTOP is role-exempt, jumps every queue and latches until it is explicitly
cleared.
