---
title: Build with SlopSync
description: >-
  The how tier: connect a client, write an app integration, make your firmware a conforming hub, and test all of it locally.
register: STE
---

# Build with SlopSync

This section is task-oriented. Each page gets you to something running.

| Page | Gets you |
|---|---|
| [Quickstart](quickstart.md) | A connected client in about 20 lines |
| [JavaScript](clients/javascript.md) · [C#](clients/csharp.md) · [C++](clients/cpp.md) · [Python](clients/python.md) | A working client in your language |
| [Plugin guide](plugins.md) | An app integration, with a shipped plugin as the worked example |
| [Hub implementer guide](hub.md) | Your own firmware answering as a conforming hub |
| [CLI guide](cli.md) | The command-line tooling, including motion-versus-planner graphing |
| [Local testing](local-testing.md) | A simulator, a probe, the fuzz harnesses, and the regression patterns that matter |

## Before you start

Read [How it works](../understand/how-it-works.md). Clients that skip the
mental model tend to fight the protocol: they build optimistic local state,
then discover the hub disagrees with them.

Keep [the Dictionary](../reference/dictionary.md) open. Every term on this
site has exactly one meaning, and the guides assume it.

## The one rule that catches everyone

**Your [shadow](../reference/dictionary.md#shadow) updates from the hub,
never from your own request.**

You send an [intent](../reference/dictionary.md#intent). The hub
[clamps](../reference/dictionary.md#clamp) it. The
[echo](../reference/dictionary.md#echo) tells you what was actually
applied. Render that. If you render what you asked for, your interface is
lying, and on a machine that moves that is a safety defect.
